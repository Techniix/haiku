/*
 * Copyright 2026, LinuxKPI_HaikuOS contributors.
 * Distributed under the terms of the MIT License.
 *
 * Haiku overlay for FreeBSD <sys/sleepqueue.h>: the subset LinuxKPI uses,
 * with FreeBSD's prototypes. Implemented in haiku/lkpi_thread.cpp on Haiku
 * ConditionVariables. Flags/queue/priority arguments are accepted and
 * ignored; interruptibility comes from the *_sig variants as in FreeBSD.
 */
#ifndef _LKPI_HAIKU_SYS_SLEEPQUEUE_H_
#define _LKPI_HAIKU_SYS_SLEEPQUEUE_H_

#define SLEEPQ_TYPE		0x0ff
#define SLEEPQ_SLEEP		0x00
#define SLEEPQ_CONDVAR		0x01
#define SLEEPQ_PAUSE		0x02
#define SLEEPQ_SX		0x03
#define SLEEPQ_LK		0x04
#define SLEEPQ_INTERRUPTIBLE	0x100
#define SLEEPQ_UNFAIR		0x200
#define SLEEPQ_DROP		0x400

struct lock_object;

#ifdef __cplusplus
extern "C" {
#endif
void	sleepq_lock(const void *wchan);
void	sleepq_release(const void *wchan);
void	sleepq_add(const void *wchan, struct lock_object *lock,
	    const char *wmesg, int flags, int queue);
void	sleepq_set_timeout(const void *wchan, int timo);
void	sleepq_wait(const void *wchan, int pri);
int	sleepq_wait_sig(const void *wchan, int pri);
int	sleepq_timedwait(const void *wchan, int pri);
int	sleepq_timedwait_sig(const void *wchan, int pri);
int	sleepq_signal(const void *wchan, int flags, int pri, int queue);
int	sleepq_broadcast(const void *wchan, int flags, int pri, int queue);
void	lkpi_sleepq_init(void);
#ifdef __cplusplus
}
#endif

#endif
