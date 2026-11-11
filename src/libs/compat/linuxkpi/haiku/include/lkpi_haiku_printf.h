/*
 * Copyright 2026, LinuxKPI_HaikuOS contributors.
 * Distributed under the terms of the MIT License.
 *
 * Route LinuxKPI's printf-family calls through lkpi_printf.c, which adds the
 * %j/%t length modifiers Haiku's kernel printf lacks (see there). Included
 * last by the preinclude, after the real prototypes have been seen.
 */
#ifndef _LKPI_HAIKU_PRINTF_H_
#define _LKPI_HAIKU_PRINTF_H_

#include <sys/bus.h>
#include <stdarg.h>

#ifdef __cplusplus
extern "C" {
#endif
int	lkpi_printf(const char *fmt, ...) __printflike(1, 2);
int	lkpi_device_printf(device_t dev, const char *fmt, ...)
	    __printflike(2, 3);
int	lkpi_snprintf(char *str, size_t size, const char *fmt, ...)
	    __printflike(3, 4);
int	lkpi_vsnprintf(char *str, size_t size, const char *fmt, va_list ap);
#ifdef __cplusplus
}
#endif

#undef printf
#define	printf		lkpi_printf
#define	device_printf	lkpi_device_printf
#define	snprintf	lkpi_snprintf
#define	vsnprintf	lkpi_vsnprintf

#endif
