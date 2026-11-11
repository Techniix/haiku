/*
 * Copyright 2026, LinuxKPI_HaikuOS contributors.
 * Distributed under the terms of the MIT License.
 *
 * Haiku overlay for FreeBSD <sys/sysctl.h>, replacing Haiku's freebsd_network
 * compat/sys/sysctl.h inside LinuxKPI translation units.
 *
 * Haiku has no sysctl tree: its compat header is a no-op shell with empty
 * structs. LinuxKPI's sysfs emulation (linux/sysfs.h) walks oids and reads
 * FreeBSD struct fields, so we provide the FreeBSD layouts (subset) and keep
 * registration a no-op: static SYSCTL_* declarations vanish, dynamic
 * SYSCTL_ADD_* return a private dummy oid. Net effect is the same as Haiku's
 * stub (no sysctls, no sysfs) but LinuxKPI compiles unmodified.
 * Only pointers to these types are shared with net80211 (iv_sysctl, iv_oid).
 */
#ifndef _LKPI_HAIKU_SYS_SYSCTL_H_
#define _LKPI_HAIKU_SYS_SYSCTL_H_
#define _FBSD_COMPAT_SYS_SYSCTL_H_	/* suppress Haiku's version */

#include <sys/types.h>
#include <sys/queue.h>

struct sysctl_oid;
SLIST_HEAD(sysctl_oid_list, sysctl_oid);

struct sysctl_req {
	struct thread	*td;
	int		lock;
	void		*oldptr;
	size_t		oldlen;
	size_t		oldidx;
	int		(*oldfunc)(struct sysctl_req *, const void *, size_t);
	const void	*newptr;
	size_t		newlen;
	size_t		newidx;
	int		(*newfunc)(struct sysctl_req *, void *, size_t);
	size_t		validlen;
	int		flags;
};

struct sysctl_oid {
	struct sysctl_oid_list	oid_children;
	struct sysctl_oid_list	*oid_parent;
	SLIST_ENTRY(sysctl_oid)	oid_link;
	int		oid_number;
	u_int		oid_kind;
	void		*oid_arg1;
	intmax_t	oid_arg2;
	const char	*oid_name;
	int		(*oid_handler)(struct sysctl_oid *oidp, void *arg1,
			    intmax_t arg2, struct sysctl_req *req);
	const char	*oid_fmt;
	int		oid_refcnt;
	u_int		oid_running;
	const char	*oid_descr;
	const char	*oid_label;
};

struct sysctl_ctx_entry {
	struct sysctl_oid	*entry;
	TAILQ_ENTRY(sysctl_ctx_entry) link;
};
TAILQ_HEAD(sysctl_ctx_list, sysctl_ctx_entry);

#define SYSCTL_HANDLER_ARGS	struct sysctl_oid *oidp, void *arg1,	\
	intmax_t arg2, struct sysctl_req *req

#define OID_AUTO	(-1)
#define CTL_AUTO_START	0x100

#define CTLTYPE		0xf
#define CTLTYPE_NODE	1
#define CTLTYPE_INT	2
#define CTLTYPE_STRING	3
#define CTLTYPE_S64	4
#define CTLTYPE_OPAQUE	5
#define CTLTYPE_STRUCT	CTLTYPE_OPAQUE
#define CTLTYPE_UINT	6
#define CTLTYPE_LONG	7
#define CTLTYPE_ULONG	8
#define CTLTYPE_U64	9
#define CTLTYPE_U8	0xa
#define CTLTYPE_U16	0xb
#define CTLTYPE_S8	0xc
#define CTLTYPE_S16	0xd
#define CTLTYPE_S32	0xe
#define CTLTYPE_U32	0xf

#define CTLFLAG_RD	0x80000000
#define CTLFLAG_WR	0x40000000
#define CTLFLAG_RW	(CTLFLAG_RD | CTLFLAG_WR)
#define CTLFLAG_DORMANT	0x20000000
#define CTLFLAG_ANYBODY	0x10000000
#define CTLFLAG_SECURE	0x08000000
#define CTLFLAG_PRISON	0x04000000
#define CTLFLAG_DYN	0x02000000
#define CTLFLAG_SKIP	0x01000000
#define CTLFLAG_TUN	0x00080000
#define CTLFLAG_RDTUN	(CTLFLAG_RD | CTLFLAG_TUN)
#define CTLFLAG_RWTUN	(CTLFLAG_RW | CTLFLAG_TUN)
#define CTLFLAG_MPSAFE	0x00040000
#define CTLFLAG_VNET	0x00020000
#define CTLFLAG_DYING	0x00010000
#define CTLFLAG_CAPRD	0x00008000
#define CTLFLAG_CAPWR	0x00004000
#define CTLFLAG_STATS	0x00002000
#define CTLFLAG_NOFETCH	0x00001000
#define CTLFLAG_CAPRW	(CTLFLAG_CAPRD | CTLFLAG_CAPWR)
#define CTLFLAG_NEEDGIANT 0x00000800

#define SYSCTL_CHILDREN(oid)	(&(oid)->oid_children)
#define SYSCTL_FOREACH(oidp, list)	SLIST_FOREACH((oidp), (list), oid_link)

/* Static declarations: nothing is registered on Haiku. */
#define SYSCTL_DECL(name)	extern struct sysctl_oid_list sysctl_##name##_children
/* expands to a harmless file-scope declaration; arguments are dropped */
#define __lkpi_sysctl_unused(name)	_Static_assert(1, "sysctl")
/* A node is an (unregistered) oid so SYSCTL_CHILDREN(&sysctl__x_y) works. */
#define SYSCTL_NODE(parent, nbr, name, access, handler, descr)	\
	static struct sysctl_oid sysctl__##parent##_##name __attribute__((unused))
/*
 * Tunables. FreeBSD reads CTLFLAG_TUN sysctls from the loader environment;
 * Haiku has none, so each scalar SYSCTL_* records {name, address, size} in
 * the __lkpi_tunable linker set and lkpi_tunables_fetch() (lkpi_sysinit.c)
 * fills the CTLFLAG_TUN ones from the driver's settings file
 * (~/config/settings/kernel/drivers/<driver>) before any SYSINIT runs.
 * Keys use FreeBSD's dotted names: "compat.linuxkpi.80211.hw_crypto 1".
 * The indirection through __LKPI_TUNABLE expands parent/name macros first
 * (module_param passes LINUXKPI_PARAM_PARENT / LINUXKPI_PARAM_NAME(x)).
 */
#include <sys/linker_set.h>

struct lkpi_tunable {
	const char	*parent;	/* "_compat_linuxkpi_80211" */
	const char	*name;		/* "hw_crypto" */
	void		*ptr;		/* NULL: constant, not tunable */
	unsigned int	access;
	unsigned char	size;		/* bytes */
	unsigned char	is_signed;
	unsigned char	is_bool;	/* C bool: only 0 or 1 may be stored */
};

/* FreeBSD dotted name of a tunable, e.g. "compat.linuxkpi.80211.hw_crypto"
 * (lkpi_sysinit.c). Node names containing '_' cannot be told apart from the
 * separators; none of the LinuxKPI tunables have one. */
void	lkpi_tunable_key(const struct lkpi_tunable *t, char *buf, size_t len);

#define __LKPI_TUNABLE2(parent, name, access, ptr, sz, sgn, isbool)	\
	static struct lkpi_tunable lkpi_tunable_##parent##_##name = {	\
		#parent, #name, (void *)(ptr), (access), (sz), (sgn),	\
		(isbool) };						\
	DATA_SET(__lkpi_tunable, lkpi_tunable_##parent##_##name)
#define __LKPI_TUNABLE(parent, name, access, ptr, sz, sgn, isbool)	\
	__LKPI_TUNABLE2(parent, name, access, ptr, sz, sgn, isbool)

#define SYSCTL_INT(parent, nbr, name, access, ptr, val, descr)	\
	__LKPI_TUNABLE(parent, name, access, ptr, sizeof(int), 1, 0)
#define SYSCTL_UINT(parent, nbr, name, access, ptr, val, descr)	\
	__LKPI_TUNABLE(parent, name, access, ptr, sizeof(unsigned int), 0, 0)
#define SYSCTL_LONG(parent, nbr, name, access, ptr, val, descr)	\
	__LKPI_TUNABLE(parent, name, access, ptr, sizeof(long), 1, 0)
#define SYSCTL_ULONG(parent, nbr, name, access, ptr, val, descr) \
	__LKPI_TUNABLE(parent, name, access, ptr, sizeof(unsigned long), 0, 0)
#define SYSCTL_U8(parent, nbr, name, access, ptr, val, descr)	\
	__LKPI_TUNABLE(parent, name, access, ptr, 1, 0, 0)
#define SYSCTL_U16(parent, nbr, name, access, ptr, val, descr)	\
	__LKPI_TUNABLE(parent, name, access, ptr, 2, 0, 0)
#define SYSCTL_S16(parent, nbr, name, access, ptr, val, descr)	\
	__LKPI_TUNABLE(parent, name, access, ptr, 2, 1, 0)
#define SYSCTL_BOOL(parent, nbr, name, access, ptr, val, descr)	\
	__LKPI_TUNABLE(parent, name, access, ptr, sizeof(bool), 0, 1)
#define SYSCTL_STRING(parent, nbr, name, access, arg, len, descr) __lkpi_sysctl_unused(parent##_##name)
#define SYSCTL_COUNTER_U64(parent, nbr, name, access, ptr, descr) __lkpi_sysctl_unused(parent##_##name)
#define SYSCTL_PROC(parent, nbr, name, access, ptr, arg, handler, fmt, descr) __lkpi_sysctl_unused(parent##_##name)
#define SYSCTL_OPAQUE(parent, nbr, name, access, ptr, len, fmt, descr) __lkpi_sysctl_unused(parent##_##name)
#define SYSCTL_STATIC_CHILDREN(oid_name)	((struct sysctl_oid_list *)NULL)

#ifdef __cplusplus
extern "C" {
#endif
struct sbuf;
struct sbuf	*sbuf_new_for_sysctl(struct sbuf *s, char *buf, int length,
		    struct sysctl_req *req);
struct sysctl_oid *lkpi_sysctl_dummy_oid(struct sysctl_ctx_list *ctx,
	    struct sysctl_oid_list *parent, const char *name, void *arg1,
	    intmax_t arg2);
int	sysctl_ctx_init(struct sysctl_ctx_list *clist);
int	sysctl_ctx_free(struct sysctl_ctx_list *clist);
int	sysctl_remove_oid(struct sysctl_oid *oidp, int del, int recurse);
int	sysctl_remove_name(struct sysctl_oid *parent, const char *name, int del,
	    int recurse);
int	sysctl_handle_int(SYSCTL_HANDLER_ARGS);
int	sysctl_handle_long(SYSCTL_HANDLER_ARGS);
int	sysctl_handle_string(SYSCTL_HANDLER_ARGS);
int	sysctl_handle_opaque(SYSCTL_HANDLER_ARGS);
#ifdef __cplusplus
}
#endif

#define SYSCTL_ADD_OID(ctx, parent, nbr, name, kind, a1, a2, handler, fmt, descr) \
	lkpi_sysctl_dummy_oid(ctx, parent, name, a1, a2)
#define SYSCTL_ADD_NODE(ctx, parent, nbr, name, access, handler, descr) \
	lkpi_sysctl_dummy_oid(ctx, parent, name, NULL, 0)
#define SYSCTL_ADD_ROOT_NODE(ctx, nbr, name, access, handler, descr) \
	lkpi_sysctl_dummy_oid(ctx, NULL, name, NULL, 0)
#define SYSCTL_ADD_PROC(ctx, parent, nbr, name, access, ptr, arg, handler, fmt, descr) \
	lkpi_sysctl_dummy_oid(ctx, parent, name, ptr, arg)
#define SYSCTL_ADD_INT(ctx, parent, nbr, name, access, ptr, val, descr) \
	lkpi_sysctl_dummy_oid(ctx, parent, name, ptr, val)
#define SYSCTL_ADD_UINT(ctx, parent, nbr, name, access, ptr, val, descr) \
	lkpi_sysctl_dummy_oid(ctx, parent, name, ptr, val)
#define SYSCTL_ADD_U64(ctx, parent, nbr, name, access, ptr, val, descr) \
	lkpi_sysctl_dummy_oid(ctx, parent, name, ptr, val)
#define SYSCTL_ADD_STRING(ctx, parent, nbr, name, access, arg, len, descr) \
	lkpi_sysctl_dummy_oid(ctx, parent, name, arg, len)

#define SYSCTL_OUT(r, p, l)	(0)
#define SYSCTL_IN(r, p, l)	(0)

#endif
