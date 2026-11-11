/*
 * Copyright 2026, LinuxKPI_HaikuOS contributors.
 * Distributed under the terms of the MIT License.
 *
 * Haiku overlay for FreeBSD <sys/signalvar.h>: sigset_t helpers used by
 * LinuxKPI's signal_pending(). Haiku's sigset_t is a 64-bit mask. The
 * per-thread/process pending sets in our struct thread/proc are always
 * empty: LinuxKPI only runs in kernel threads, which Haiku does not signal
 * the way FreeBSD delivers signals to kernel threads.
 */
#ifndef _LKPI_HAIKU_SYS_SIGNALVAR_H_
#define _LKPI_HAIKU_SYS_SIGNALVAR_H_

#include <signal.h>

#define _LKPI_SIGBIT(sig)	((sigset_t)1 << ((sig) - 1))
#define SIGISEMPTY(set)		((set) == 0)
#define SIGISMEMBER(set, sig)	(((set) & _LKPI_SIGBIT(sig)) != 0)
#define SIGADDSET(set, sig)	((set) |= _LKPI_SIGBIT(sig))
#define SIGDELSET(set, sig)	((set) &= ~_LKPI_SIGBIT(sig))
#define SIGEMPTYSET(set)	((set) = 0)
#define SIGFILLSET(set)		((set) = ~(sigset_t)0)
#define SIGSETOR(set1, set2)	((set1) |= (set2))
#define SIGSETAND(set1, set2)	((set1) &= (set2))
#define SIGSETNAND(set1, set2)	((set1) &= ~(set2))

#endif
