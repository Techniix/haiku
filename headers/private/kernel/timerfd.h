/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 */
#ifndef _KERNEL_TIMERFD_H
#define _KERNEL_TIMERFD_H


#include <OS.h>
#include <time.h>


#ifdef __cplusplus
extern "C" {
#endif


extern int		_user_timerfd_create(clockid_t clock, int openFlags);
extern status_t	_user_timerfd_settime(int fd, bool absolute,
					const struct itimerspec* newValue,
					struct itimerspec* oldValue);
extern status_t	_user_timerfd_gettime(int fd, struct itimerspec* value);


#ifdef __cplusplus
}
#endif


#endif	// _KERNEL_TIMERFD_H
