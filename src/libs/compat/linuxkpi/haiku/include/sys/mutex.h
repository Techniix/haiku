/* Haiku overlay: extends compat/sys/mutex.h with FreeBSD mtx_init() option
 * bits LinuxKPI passes (values as FreeBSD sys/mutex.h). Haiku's mtx_init()
 * only tests MTX_RECURSE/MTX_SPIN and ignores the rest (mutex.c:19).
 * Copyright 2026, LinuxKPI_HaikuOS contributors. MIT. */
#ifndef _LKPI_HAIKU_SYS_MUTEX_H_
#define _LKPI_HAIKU_SYS_MUTEX_H_
#include_next <sys/mutex.h>
#ifndef MTX_NOWITNESS
#define MTX_NOWITNESS	0x00000008
#endif
#ifndef MTX_NOPROFILE
#define MTX_NOPROFILE	0x00000020
#endif
#ifndef MTX_NEW
#define MTX_NEW		0x00000040
#endif
#endif
