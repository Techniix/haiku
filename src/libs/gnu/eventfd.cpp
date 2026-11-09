/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 */


#include <sys/eventfd.h>

#include <errno.h>
#include <unistd.h>

#include <errno_private.h>
#include <syscalls.h>


static_assert((EFD_SEMAPHORE & (O_CLOEXEC | O_CLOFORK | O_NONBLOCK)) == 0,
	"EFD_SEMAPHORE collides with an open flag");


int
eventfd(unsigned int initialValue, int flags)
{
	if ((flags & ~(EFD_SEMAPHORE | EFD_CLOEXEC | EFD_CLOFORK | EFD_NONBLOCK))
			!= 0) {
		__set_errno(EINVAL);
		return -1;
	}

	int fd = _kern_eventfd(initialValue,
		flags & (EFD_CLOEXEC | EFD_CLOFORK | EFD_NONBLOCK),
		(flags & EFD_SEMAPHORE) != 0);
	if (fd < 0) {
		__set_errno(fd);
		return -1;
	}
	return fd;
}


int
eventfd_read(int fd, eventfd_t* value)
{
	return read(fd, value, sizeof(*value)) == sizeof(*value) ? 0 : -1;
}


int
eventfd_write(int fd, eventfd_t value)
{
	return write(fd, &value, sizeof(value)) == sizeof(value) ? 0 : -1;
}
