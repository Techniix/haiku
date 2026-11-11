/*
 * Copyright 2026, LinuxKPI_HaikuOS contributors.
 * Distributed under the terms of the MIT License.
 *
 * Read-only status interface for userland (LinuxKPI Manager, lkpictl).
 * ABI and transport: headers/private/linuxkpi/lkpi_control.h.
 *
 * Safety rules for this file:
 *  - nothing here changes driver or hardware state; every opcode only reads
 *    counters, constants and tunable values;
 *  - the request is copied in with user_memcpy() and validated (magic,
 *    version, user address, size) before anything else; the reply is built
 *    in a kernel buffer and copied out with user_memcpy();
 *  - replies are bounded by LKPI_CONTROL_MAX_REPLY.
 * Anything that would change state belongs in a separate, explicitly
 * designed opcode (docs/MANAGER.md, "Write operations"), not here.
 */
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/linker_set.h>
#include <sys/sysctl.h>

#include <KernelExport.h>
#include <kernel.h>		/* IS_USER_ADDRESS */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <compat/sys/haiku-module.h>	/* gDriverName, gDriverControlHook */
#include <lkpi_control.h>		/* headers/private/linuxkpi */

#include "lkpi_version.h"
#include "lkpi_control_kern.h"

extern unsigned long jiffies;			/* lkpi_thread.cpp */
void lkpi_dma_get_stats(uint64_t *maps, uint64_t *bounces,
    uint64_t *pool_misses);			/* lkpi_dma.c */
void lkpi_thread_get_stats(int32 *live, int32 *created,
    int32 *freed);				/* lkpi_thread.cpp */

SET_DECLARE(__lkpi_tunable, struct lkpi_tunable);

static bigtime_t lkpi_control_load_time;

struct lkpi_reply {
	char	*buf;
	size_t	size;	/* capacity */
	size_t	len;	/* bytes that would have been written */
};

/* Extra STATS keys from a subsystem (e.g. LinuxKPI 802.11). */
static lkpi_control_stats_fn lkpi_control_stats_provider;

void
lkpi_control_set_stats_provider(lkpi_control_stats_fn fn)
{
	lkpi_control_stats_provider = fn;
}

static void reply_vadd(struct lkpi_reply *r, const char *key,
    const char *fmt, va_list ap);

void
lkpi_control_add(struct lkpi_reply *r, const char *key, const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	reply_vadd(r, key, fmt, ap);
	va_end(ap);
}

static void
reply_add(struct lkpi_reply *r, const char *key, const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	reply_vadd(r, key, fmt, ap);
	va_end(ap);
}

static void
reply_vadd(struct lkpi_reply *r, const char *key, const char *fmt, va_list ap)
{
	char value[256];
	int n;

	vsnprintf(value, sizeof(value), fmt, ap);
	/* keys and values are single-line by construction; enforce it */
	for (char *p = value; *p != '\0'; p++)
		if (*p == '\n' || *p == '\r')
			*p = ' ';
	n = snprintf(r->buf + MIN(r->len, r->size), r->len < r->size ?
	    r->size - r->len : 0, "%s=%s\n", key, value);
	if (n > 0)
		r->len += n;
}

static long long
tunable_value(const struct lkpi_tunable *t)
{
	if (t->is_bool)
		return (*(uint8_t *)t->ptr != 0);
	switch (t->size) {
	case 1: return t->is_signed ? *(int8_t *)t->ptr : *(uint8_t *)t->ptr;
	case 2: return t->is_signed ? *(int16_t *)t->ptr : *(uint16_t *)t->ptr;
	case 4: return t->is_signed ? *(int32_t *)t->ptr : *(uint32_t *)t->ptr;
	case 8: return *(int64_t *)t->ptr;
	}
	return 0;
}

static void
build_info(struct lkpi_reply *r, const struct lkpi_control_request *req)
{
	reply_add(r, "abi", "%u", LKPI_CONTROL_VERSION);
	reply_add(r, "interface", "%s", req->name);
	reply_add(r, "driver", "%s", gDriverName);
	reply_add(r, "linuxkpi.version", "%s", LKPI_HAIKU_VERSION);
	reply_add(r, "linuxkpi.freebsd_import", "%s", LKPI_FREEBSD_IMPORT);
	reply_add(r, "linuxkpi.hz", "%d", 1000);
	reply_add(r, "loaded_for_ms", "%lld",
	    (long long)((system_time() - lkpi_control_load_time) / 1000));
}

static void
build_stats(struct lkpi_reply *r)
{
	uint64_t maps, bounces, misses;
	int32 live, created, freed;

	lkpi_dma_get_stats(&maps, &bounces, &misses);
	lkpi_thread_get_stats(&live, &created, &freed);
	reply_add(r, "jiffies", "%lu", jiffies);
	reply_add(r, "dma.maps", "%llu", (unsigned long long)maps);
	reply_add(r, "dma.bounces", "%llu", (unsigned long long)bounces);
	reply_add(r, "dma.bounce_pool_misses", "%llu",
	    (unsigned long long)misses);
	reply_add(r, "threads.records_live", "%d", (int)live);
	reply_add(r, "threads.records_created", "%d", (int)created);
	reply_add(r, "threads.records_freed", "%d", (int)freed);
	if (lkpi_control_stats_provider != NULL)
		lkpi_control_stats_provider(r);
}

static void
build_tunables(struct lkpi_reply *r)
{
	struct lkpi_tunable **tp;
	char key[128], kk[160];

	for (tp = SET_BEGIN(__lkpi_tunable); tp < SET_LIMIT(__lkpi_tunable);
	    tp++) {
		const struct lkpi_tunable *t = *tp;

		if (t->ptr == NULL || t->name[0] == '\0')
			continue;
		lkpi_tunable_key(t, key, sizeof(key));
		reply_add(r, key, "%lld", tunable_value(t));
		snprintf(kk, sizeof(kk), "%s.flags", key);
		reply_add(r, kk, "%s%s%s",
		    (t->access & CTLFLAG_RD) ? "r" : "",
		    (t->access & CTLFLAG_WR) ? "w" : "",
		    (t->access & CTLFLAG_TUN) ? "t" : "");
	}
}

/* [base, base + size) lies entirely in userland and does not wrap. */
static bool
lkpi_user_range(addr_t base, size_t size)
{
	return (size != 0 && base + size > base && IS_USER_ADDRESS(base) &&
	    IS_USER_ADDRESS(base + size - 1));
}

static status_t
lkpi_control(void *cookie, uint32 op, void *arg, size_t length)
{
	struct lkpi_control_request req;
	struct lkpi_reply r;
	status_t status;

	(void)cookie;
	if (op < LKPI_CTL_FIRST || op > LKPI_CTL_LAST)
		return B_DEV_INVALID_IOCTL;	/* not ours: next handler */

	/* B_BAD_DATA, not B_BAD_VALUE: userland reads B_BAD_VALUE as "driver
	 * without this interface" (wlan_control()'s answer). */
	if (arg == NULL || (length != 0 && length < sizeof(req)) ||
	    !lkpi_user_range((addr_t)arg, sizeof(req)))
		return B_BAD_DATA;
	if (user_memcpy(&req, arg, sizeof(req)) != B_OK)
		return B_BAD_ADDRESS;
	req.name[sizeof(req.name) - 1] = '\0';
	if (req.magic != LKPI_CONTROL_MAGIC)
		return B_BAD_DATA;
	if (req.version != LKPI_CONTROL_VERSION)
		return B_BAD_TYPE;		/* caller must match the ABI */
	if (req.buffer == 0 || req.buffer_size == 0 ||
	    req.buffer != (uint64_t)(addr_t)req.buffer)	/* 32-bit kernel */
		return B_BAD_ADDRESS;

	r.size = MIN(req.buffer_size, LKPI_CONTROL_MAX_REPLY);
	if (!lkpi_user_range((addr_t)req.buffer, r.size))
		return B_BAD_ADDRESS;
	r.len = 0;
	r.buf = malloc(r.size);
	if (r.buf == NULL)
		return B_NO_MEMORY;
	r.buf[0] = '\0';

	switch (op) {
	case LKPI_CTL_GET_INFO:		build_info(&r, &req); break;
	case LKPI_CTL_GET_STATS:	build_stats(&r); break;
	case LKPI_CTL_GET_TUNABLES:	build_tunables(&r); break;
	}

	req.reply_size = (uint32_t)r.len;
	if (r.len >= r.size) {
		status = B_BUFFER_OVERFLOW;	/* reply_size says how much */
	} else {
		status = user_memcpy((void *)(addr_t)req.buffer, r.buf, r.len + 1);
	}
	free(r.buf);
	if (status == B_OK || status == B_BUFFER_OVERFLOW) {
		if (user_memcpy(&((struct lkpi_control_request *)arg)->reply_size,
		    &req.reply_size, sizeof(req.reply_size)) != B_OK)
			return B_BAD_ADDRESS;
	}
	return status;
}

void
lkpi_control_init(void)
{
	lkpi_control_load_time = system_time();
	gDriverControlHook = lkpi_control;
}

void
lkpi_control_uninit(void)
{
	if (gDriverControlHook == lkpi_control)
		gDriverControlHook = NULL;
}
