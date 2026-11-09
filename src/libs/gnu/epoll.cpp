/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 *
 * Thin wrappers around the epoll syscalls. The kernel takes struct
 * epoll_event in the Linux layout and the EPOLL* values as they are; only
 * the timeout conversion (milliseconds or timespec to microseconds) and
 * the epoll_create() size check happen here.
 */


#include <sys/epoll.h>

#include <errno.h>
#include <stddef.h>

#include <errno_private.h>
#include <syscalls.h>


// The kernel (src/system/kernel/events/epoll.cpp) uses this header's
// definitions; make sure the layout is the Linux one.
#if defined(__x86_64__)
static_assert(sizeof(struct epoll_event) == 12, "epoll_event must be packed");
static_assert(offsetof(struct epoll_event, data) == 4, "epoll_event layout");
#endif
static_assert((EPOLL_CLOEXEC & EPOLL_CLOFORK) == 0, "flag collision");


static int
check(int result)
{
	if (result < 0) {
		__set_errno(result);
		return -1;
	}
	return result;
}


int
epoll_create(int size)
{
	// The size is only a hint (and ignored) since Linux 2.6.8, but must be
	// positive.
	if (size <= 0) {
		__set_errno(EINVAL);
		return -1;
	}
	return check(_kern_epoll_create(0));
}


int
epoll_create1(int flags)
{
	if ((flags & ~(EPOLL_CLOEXEC | EPOLL_CLOFORK)) != 0) {
		__set_errno(EINVAL);
		return -1;
	}
	return check(_kern_epoll_create(flags));
}


int
epoll_ctl(int epfd, int op, int fd, struct epoll_event* event)
{
	return check(_kern_epoll_ctl(epfd, op, fd, event));
}


int
epoll_wait(int epfd, struct epoll_event* events, int maxEvents, int timeout)
{
	return epoll_pwait(epfd, events, maxEvents, timeout, NULL);
}


int
epoll_pwait(int epfd, struct epoll_event* events, int maxEvents, int timeout,
	const sigset_t* sigMask)
{
	bigtime_t micros = timeout < 0 ? -1 : (bigtime_t)timeout * 1000;
	return check(_kern_epoll_wait(epfd, events, maxEvents, micros, sigMask));
}


int
epoll_pwait2(int epfd, struct epoll_event* events, int maxEvents,
	const struct timespec* timeout, const sigset_t* sigMask)
{
	bigtime_t micros = -1;
	if (timeout != NULL) {
		if (timeout->tv_sec < 0 || timeout->tv_nsec < 0
			|| timeout->tv_nsec >= 1000000000L) {
			__set_errno(EINVAL);
			return -1;
		}
		// Round up, so that a timeout never expires early.
		micros = (bigtime_t)timeout->tv_sec * 1000000
			+ (timeout->tv_nsec + 999) / 1000;
	}
	return check(_kern_epoll_wait(epfd, events, maxEvents, micros, sigMask));
}
