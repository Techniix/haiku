/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 *
 * Thin wrapper around the signalfd syscall: the SFD_* flags are open
 * flags already, the kernel validates the descriptor and the mask.
 */


#include <sys/signalfd.h>

#include <errno.h>
#include <stddef.h>

#include <errno_private.h>
#include <syscalls.h>


static_assert(sizeof(struct signalfd_siginfo) == 128, "Linux record size");
static_assert(offsetof(struct signalfd_siginfo, ssi_ptr) == 48,
	"Linux record layout");
static_assert(offsetof(struct signalfd_siginfo, ssi_arch) == 96,
	"Linux record layout");


int
signalfd(int fd, const sigset_t* mask, int flags)
{
	if ((flags & ~(SFD_CLOEXEC | SFD_NONBLOCK | SFD_CLOFORK)) != 0
		|| fd < -1) {
		__set_errno(EINVAL);
		return -1;
	}

	int result = _kern_signalfd(fd, mask, flags);
	if (result < 0) {
		__set_errno(result);
		return -1;
	}
	return result;
}
