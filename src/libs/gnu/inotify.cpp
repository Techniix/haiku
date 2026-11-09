/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 *
 * Thin wrappers around the inotify syscalls. The kernel takes the IN_*
 * values as they are; events are read() from the descriptor.
 */


#include <sys/inotify.h>

#include <errno.h>
#include <stddef.h>

#include <errno_private.h>
#include <syscalls.h>


static_assert(sizeof(struct inotify_event) == 16, "Linux record layout");
static_assert((IN_CLOEXEC & IN_NONBLOCK) == 0, "flag collision");


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
inotify_init(void)
{
	return check(_kern_inotify_init(0));
}


int
inotify_init1(int flags)
{
	if ((flags & ~(IN_CLOEXEC | IN_NONBLOCK)) != 0) {
		__set_errno(EINVAL);
		return -1;
	}
	return check(_kern_inotify_init(flags));
}


int
inotify_add_watch(int fd, const char* path, uint32_t mask)
{
	return check(_kern_inotify_add_watch(fd, path, mask));
}


int
inotify_rm_watch(int fd, int wd)
{
	return check(_kern_inotify_rm_watch(fd, wd));
}
