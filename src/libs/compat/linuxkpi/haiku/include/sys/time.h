/*
 * Copyright 2026, LinuxKPI_HaikuOS contributors.
 * Distributed under the terms of the MIT License.
 *
 * Haiku overlay: extends Haiku's freebsd_network compat/sys/time.h.
 *
 * FreeBSD's microtime() (wall clock) is missing there; LinuxKPI uses it
 * only to time out polling loops (linux/iopoll.h: read_poll_timeout()).
 * It is mapped to getmicrouptime(), which freebsd_network implements
 * with system_time(): monotonic, so a clock adjustment while polling can
 * neither end the wait early nor stretch it.
 */
#ifndef _LKPI_HAIKU_SYS_TIME_H_
#define _LKPI_HAIKU_SYS_TIME_H_

#include_next <sys/time.h>

#ifdef _KERNEL
static inline void
microtime(struct timeval *tvp)
{
	getmicrouptime(tvp);
}
#endif

#endif
