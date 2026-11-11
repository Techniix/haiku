/*
 * Copyright 2026, LinuxKPI_HaikuOS contributors.
 * Distributed under the terms of the MIT License.
 *
 * Haiku overlay for FreeBSD <sys/unistd.h>: the rfork flags LinuxKPI passes
 * to kthread_add() (values as FreeBSD; lkpi_kthread_add() always creates the
 * thread stopped, which is what RFSTOPPED asks for).
 */
#ifndef _LKPI_HAIKU_SYS_UNISTD_H_
#define _LKPI_HAIKU_SYS_UNISTD_H_

#define	RFSTOPPED	(1<<17)
#define	RFHIGHPID	(1<<18)

#endif
