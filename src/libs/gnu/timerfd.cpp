/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 *
 * Thin wrappers around the timerfd syscalls. Flag validation that is purely
 * an API concern (the TFD_* namespace) happens here; the kernel validates
 * the clock, the open flags and the itimerspec values.
 */


#include <sys/timerfd.h>

#include <errno.h>

#include <errno_private.h>
#include <syscalls.h>


int
timerfd_create(clockid_t clockID, int flags)
{
	if ((flags & ~(TFD_CLOEXEC | TFD_CLOFORK | TFD_NONBLOCK)) != 0) {
		__set_errno(EINVAL);
		return -1;
	}

	int fd = _kern_timerfd_create(clockID, flags);
	if (fd < 0) {
		__set_errno(fd);
		return -1;
	}
	return fd;
}


int
timerfd_settime(int fd, int flags, const struct itimerspec* newValue,
	struct itimerspec* oldValue)
{
	// TFD_TIMER_CANCEL_ON_SET would need a notification from the kernel's
	// set_real_time_clock() path; reject it instead of silently ignoring it,
	// so callers fall back to their own discontinuity handling.
	if ((flags & ~TFD_TIMER_ABSTIME) != 0) {
		__set_errno(EINVAL);
		return -1;
	}
	if (newValue == NULL) {
		__set_errno(EFAULT);
		return -1;
	}

	status_t status = _kern_timerfd_settime(fd,
		(flags & TFD_TIMER_ABSTIME) != 0, newValue, oldValue);
	if (status != B_OK) {
		__set_errno(status);
		return -1;
	}
	return 0;
}


int
timerfd_gettime(int fd, struct itimerspec* currentValue)
{
	if (currentValue == NULL) {
		__set_errno(EFAULT);
		return -1;
	}

	status_t status = _kern_timerfd_gettime(fd, currentValue);
	if (status != B_OK) {
		__set_errno(status);
		return -1;
	}
	return 0;
}
