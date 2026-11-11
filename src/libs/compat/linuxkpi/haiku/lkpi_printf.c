/*
 * Copyright 2026, LinuxKPI_HaikuOS contributors.
 * Distributed under the terms of the MIT License.
 *
 * printf-family wrappers for LinuxKPI code (lkpi_haiku_printf.h renames
 * printf, device_printf, snprintf and vsnprintf to these).
 *
 * Haiku's kernel vsnprintf (src/system/kernel/lib/kernel_vsprintf.cpp)
 * knows the length modifiers h, l, ll/L and z, but not j or t. A "%ju" is
 * printed literally and its argument is not consumed, so every later
 * conversion of the same call reads the wrong argument (a later "%s" would
 * dereference an integer). LinuxKPI, LinuxKPI 802.11 and mt76 use %j/%t in
 * ~40 places. On x86_64 intmax_t is long long and ptrdiff_t is long, so
 * rewrite j -> ll and t -> l before formatting. [V]
 *
 * FreeBSD's "%b" (value, then a bit-name string) is unknown too and would
 * shift later arguments the same way; it becomes "%x%.0s": the value in
 * hex, and the bit-name string consumed without being printed.
 *
 * Built without the LinuxKPI preinclude (calls Haiku/compat functions).
 */
#include <sys/param.h>
#include <sys/bus.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include <KernelExport.h>

extern const char *gDriverName;	/* freebsd_network compat/sys/haiku-module.h */

/* Returns fmt itself, or buf holding a rewritten copy. */
static const char *
lkpi_fmt_fix(const char *fmt, char *buf, size_t size)
{
	const char *s;
	size_t o = 0;
	bool fix = false;

	for (s = fmt; *s != '\0'; s++) {
		if (*s != '%')
			continue;
		s++;
		while (*s != '\0' && strchr("-+ #0123456789.*", *s) != NULL)
			s++;
		if (*s == 'j' || *s == 't' || *s == 'b') {
			fix = true;
			break;
		}
		if (*s == '\0')
			break;
	}
	if (!fix)
		return (fmt);

	for (s = fmt; *s != '\0' && o + 8 < size; s++) {
		buf[o++] = *s;
		if (*s != '%')
			continue;
		s++;
		while (*s != '\0' && strchr("-+ #0123456789.*", *s) != NULL &&
		    o + 8 < size)
			buf[o++] = *s++;
		if (*s == '\0')
			break;
		if (*s == 'j') {
			buf[o++] = 'l';
			buf[o++] = 'l';
		} else if (*s == 't')
			buf[o++] = 'l';
		else if (*s == 'b') {
			/* no '#': it would land after a width ("%5#x") */
			memcpy(buf + o, "x%.0s", 5);
			o += 5;
		} else
			buf[o++] = *s;
	}
	buf[o] = '\0';
	return (buf);
}

int
lkpi_vsnprintf(char *str, size_t size, const char *fmt, va_list ap)
{
	char fbuf[256];

	return (vsnprintf(str, size, lkpi_fmt_fix(fmt, fbuf, sizeof(fbuf)), ap));
}

int
lkpi_snprintf(char *str, size_t size, const char *fmt, ...)
{
	va_list ap;
	int r;

	va_start(ap, fmt);
	r = lkpi_vsnprintf(str, size, fmt, ap);
	va_end(ap);
	return (r);
}

int
lkpi_printf(const char *fmt, ...)
{
	char buf[512];
	va_list ap;
	int r;

	va_start(ap, fmt);
	r = lkpi_vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	dprintf("%s", buf);
	return (r);
}

int
lkpi_device_printf(device_t dev, const char *fmt, ...)
{
	char buf[512];
	va_list ap;
	int r;

	va_start(ap, fmt);
	r = lkpi_vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	/* Linux devices without a FreeBSD device_t (USB interfaces, HCI
	 * devices, ...) reach here via dev_err(dev) -> (dev)->bsddev == NULL;
	 * Haiku's device_printf() dereferences its argument. */
	if (dev == NULL)
		dprintf("%s: %s", gDriverName, buf);
	else
		device_printf(dev, "%s", buf);
	return (r);
}
