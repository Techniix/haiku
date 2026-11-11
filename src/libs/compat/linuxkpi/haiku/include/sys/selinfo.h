/*
 * Copyright 2026, LinuxKPI_HaikuOS contributors.
 * Distributed under the terms of the MIT License.
 *
 * Haiku overlay for FreeBSD <sys/selinfo.h>: an opaque struct selinfo so
 * structures embedding one have a size. select/poll wakeups are not wired
 * up on Haiku (only used by LinuxKPI's file layer, which is not built).
 */
#ifndef _LKPI_HAIKU_SYS_SELINFO_H_
#define _LKPI_HAIKU_SYS_SELINFO_H_

struct sigio;

struct selinfo {
	void	*si_opaque[4];
};

#endif
