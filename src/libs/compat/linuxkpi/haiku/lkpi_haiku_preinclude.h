/*
 * Copyright 2026, LinuxKPI_HaikuOS contributors.
 * Distributed under the terms of the MIT License.
 *
 * Force-included before every LinuxKPI translation unit (-include).
 * Sets up what FreeBSD's kernel build would have defined globally, and
 * resolves the few type clashes between Haiku's POSIX headers and LinuxKPI
 * without editing imported files.
 */
#ifndef _LKPI_HAIKU_PREINCLUDE_H_
#define _LKPI_HAIKU_PREINCLUDE_H_

#ifndef _KERNEL
#define _KERNEL 1
#endif

/* Pull Haiku's FreeBSD-compat base types first (vm_paddr_t lives in vm/vm.h
 * there, not in machine/_types.h as on FreeBSD). */
/* libc-ish headers first: freebsd_network's <sys/malloc.h> turns malloc()
 * and free() into 3-/2-argument macros under FBSD_DRIVER. */
#include <stdlib.h>
#include <ctype.h>
#include <string.h>
#include <sys/param.h>
#include <sys/types.h>
#include <vm/vm.h>

#include <time.h>
#include <OS.h>
#include <lock.h>
#include <machine/atomic.h>	/* our overlay; before compat headers use it */
#include <sys/systm.h>
#include <sys/lock.h>
#include <sys/mutex.h>
#include <sys/sx.h>		/* our overlay: LinuxKPI-flavoured sx */
#include <sys/proc.h>		/* our overlay: struct thread, curthread */

/* Haiku's kernel <lock.h> owns 'struct mutex' / 'mutex' (and mutex_lock() &c).
 * Linux code means its own struct mutex everywhere. Keep Haiku's reachable as
 * haiku_mutex, then rename the token for everything that follows. All Haiku
 * headers using the token are included above, before the rename. */
typedef mutex		haiku_mutex;
#define mutex		lkpi_mutex

#ifdef __cplusplus
extern "C" bool debug_debugger_running(void);
#else
extern bool debug_debugger_running(void);
#endif
#define kdb_active		(debug_debugger_running())
#ifndef SCHEDULER_STOPPED
#define SCHEDULER_STOPPED()	(0)
#endif

/* Haiku's posix <sys/cdefs.h> defines a one-level __CONCAT, which breaks
 * nested use (LinuxKPI's cleanup.h). Use FreeBSD's two-level form. */
#undef __CONCAT
#define __CONCAT1(x, y)	x ## y
#define __CONCAT(x, y)	__CONCAT1(x, y)

#ifndef __diagused
#define __diagused	__unused
#endif
#ifndef __result_use_check
#define __result_use_check	__attribute__((__warn_unused_result__))
#endif

typedef int64_t		quad_t;
/* FreeBSD's double-underscore base types (machine/_types.h), used by
 * imported FreeBSD headers such as sys/tree.h. */
typedef uintptr_t	__uintptr_t;
typedef intptr_t	__intptr_t;
typedef uint64_t	__uint64_t;
typedef int64_t		__int64_t;
typedef uint32_t	__uint32_t;
typedef int32_t		__int32_t;

/* Haiku's <SupportDefs.h> declares atomic_set/add/and/or/get(int32*, ...),
 * which collide with Linux's atomic_t API. Rename the Linux ones. Haiku's
 * functions stay reachable as haiku_atomic_*() from lkpi_haiku.h. */
#define atomic_set	lkpi_atomic_set
#define atomic_add	lkpi_atomic_add
#define atomic_and	lkpi_atomic_and
#define atomic_or	lkpi_atomic_or
#define atomic_get	lkpi_atomic_get

/* Haiku's <KernelExport.h> add_timer() and POSIX timer_delete() vs Linux
 * timer_list API. */
#define add_timer	lkpi_add_timer
#define timer_delete	lkpi_timer_delete

/* Haiku's <time.h> owns 'struct tm'; LinuxKPI's linux/time.h defines its own
 * (time64_to_tm). */
#define tm		lkpi_tm

#ifndef __maybe_unused
#define __maybe_unused	__attribute__((__unused__))
#endif

/* Haiku's compat <sys/systm.h> turns INVARIANTS on; LinuxKPI's INVARIANTS
 * paths need FreeBSD's witness/lock_class machinery. */
#include <sys/mbuf.h>		/* its inlines assume INVARIANTS: parse first */
#include <sys/buf_ring.h>
#undef INVARIANTS
#undef WITNESS

#include <lkpi_haiku_kpi.h>
#include <lkpi_errno.h>	/* positive FreeBSD errno values; see header */
#include <lkpi_haiku_kern.h>
#include <lkpi_haiku_wrap.h>	/* Haiku errno -> FreeBSD errno at the boundary */
#include <lkpi_haiku_printf.h>	/* %j/%t for Haiku's kernel printf */

/* Haiku's <sys/types.h> has 'typedef uint32 umode_t'; LinuxKPI's asm/types.h
 * wants 'unsigned short'. Rename the LinuxKPI one. */
#define umode_t __lkpi_umode_t

#endif
