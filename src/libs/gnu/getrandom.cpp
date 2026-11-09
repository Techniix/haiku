/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 *
 * getrandom(): Linux's interface to the kernel random number generator.
 * It uses the same source as libroot's getentropy() (the "random" bus
 * manager's generic syscall), without getentropy()'s 256-byte limit.
 */


#include <sys/random.h>

#include <errno.h>

#include <errno_private.h>
#include <random_defs.h>
#include <syscalls.h>


// Linux returns at most this much per call (INT_MAX >> 6 bytes).
static const size_t kMaxLength = 33554431;


ssize_t
getrandom(void* buffer, size_t length, unsigned int flags)
{
	if ((flags & ~(GRND_NONBLOCK | GRND_RANDOM | GRND_INSECURE)) != 0
		|| (flags & (GRND_RANDOM | GRND_INSECURE))
			== (GRND_RANDOM | GRND_INSECURE)) {
		__set_errno(EINVAL);
		return -1;
	}
	if (length > kMaxLength)
		length = kMaxLength;

	// Haiku's generator is always seeded, so none of the flags can make a
	// difference: there is nothing to wait for.
	size_t done = 0;
	while (done < length) {
		struct random_get_entropy_args args;
		args.buffer = (char*)buffer + done;
		args.length = length - done;

		status_t status = _kern_generic_syscall(RANDOM_SYSCALLS,
			RANDOM_GET_ENTROPY, &args, sizeof(args));
		if (status == B_INTERRUPTED) {
			if (done > 0)
				break;
			__set_errno(EINTR);
			return -1;
		}
		if (status < 0) {
			if (done > 0)
				break;
			__set_errno(status);
			return -1;
		}
		if (args.length == 0)
			break;
		done += args.length;
	}
	return done;
}
