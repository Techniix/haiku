/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 */
#ifndef _KERNEL_SIGNALFD_H
#define _KERNEL_SIGNALFD_H


#include <OS.h>
#include <signal.h>


#ifdef __cplusplus
extern "C" {
#endif


/* Number of open signalfds; the signal path does nothing while 0. */
extern int32	gSignalFDCount;

extern void		signalfd_signal_queued_slow(uint32 signal);
/* Delivers the notifications signalfd_signal_queued() deferred, now; to be
   called without locks held (thread context). */
extern void		signalfd_flush_notifications(void);

/*!	Called by signal.cpp whenever a signal becomes pending for a thread or
	team, with the team's signal lock held and interrupts disabled. */
static inline void
signalfd_signal_queued(uint32 signal)
{
	if (atomic_get(&gSignalFDCount) > 0)
		signalfd_signal_queued_slow(signal);
}

/* signal.cpp: dequeue one pending signal of \a set for the current thread,
   like sigwaitinfo(); with \a wait false fails with B_WOULD_BLOCK. */
extern status_t	signal_dequeue_for_signalfd(const sigset_t* set,
					siginfo_t* info, bool wait);
/* signal.cpp: whether a signal of \a set is pending for the thread. */
extern bool		signal_pending_for_signalfd(thread_id thread,
					sigset_t set);

extern int		_user_signalfd(int fd, const sigset_t* mask, int openFlags);


#ifdef __cplusplus
}
#endif


#endif	// _KERNEL_SIGNALFD_H
