/*
 * Copyright 2026, Haiku Community Edition contributors.
 * Distributed under the terms of the MIT License.
 *
 * The Linux timerfd interface: a file descriptor that becomes readable when
 * a timer expires. read() returns an 8-byte expiration count (the number of
 * expirations since the last read) and resets it to zero; select(), poll()
 * and wait_for_objects() report readability while the count is non-zero.
 *
 * Supported clocks: CLOCK_MONOTONIC and CLOCK_REALTIME. Absolute
 * CLOCK_REALTIME timers follow changes of the system time.
 * TFD_TIMER_CANCEL_ON_SET is not supported; timerfd_settime() rejects it
 * with EINVAL. Link with -lgnu.
 */
#ifndef _GNU_SYS_TIMERFD_H
#define _GNU_SYS_TIMERFD_H


#include <fcntl.h>
#include <sys/cdefs.h>
#include <time.h>


/* flags for timerfd_create() */
#define TFD_CLOEXEC				O_CLOEXEC
#define TFD_NONBLOCK			O_NONBLOCK
#define TFD_CLOFORK				O_CLOFORK	/* Haiku extension */

/* flags for timerfd_settime() */
#define TFD_TIMER_ABSTIME		0x00000001
#define TFD_TIMER_CANCEL_ON_SET	0x00000002	/* not supported: EINVAL */


__BEGIN_DECLS


int		timerfd_create(clockid_t clockID, int flags);
int		timerfd_settime(int fd, int flags, const struct itimerspec* newValue,
			struct itimerspec* oldValue);
int		timerfd_gettime(int fd, struct itimerspec* currentValue);


__END_DECLS


#endif	/* _GNU_SYS_TIMERFD_H */
