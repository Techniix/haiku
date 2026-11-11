/*
 * Copyright 2026, LinuxKPI_HaikuOS contributors.
 * Distributed under the terms of the MIT License.
 *
 * Haiku overlay for FreeBSD <sys/stdarg.h>.
 */
/* Haiku overlay: FreeBSD's <sys/stdarg.h> (LinuxKPI's <stdarg.h> includes it). */
#ifndef _LKPI_HAIKU_SYS_STDARG_H_
#define _LKPI_HAIKU_SYS_STDARG_H_
#ifndef _VA_LIST_DECLARED
#define _VA_LIST_DECLARED
#endif
typedef __builtin_va_list __lkpi_va_list;
typedef __builtin_va_list __va_list;
#ifndef __GNUC_VA_LIST
#define __GNUC_VA_LIST
typedef __builtin_va_list __gnuc_va_list;
#endif
#ifndef _VA_LIST
#define _VA_LIST
typedef __builtin_va_list va_list;
#endif
#define va_start(ap, last)	__builtin_va_start(ap, last)
#define va_end(ap)		__builtin_va_end(ap)
#define va_arg(ap, type)	__builtin_va_arg(ap, type)
#define va_copy(d, s)		__builtin_va_copy(d, s)
#endif
