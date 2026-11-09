/*
 * Copyright 2026, Haiku Community Edition contributors.
 * Distributed under the terms of the MIT License.
 *
 * Linux getrandom(). Haiku's random number generator is seeded before any
 * program runs, so getrandom() never blocks and GRND_NONBLOCK, GRND_RANDOM
 * and GRND_INSECURE change nothing. getentropy() (POSIX, in libroot) is
 * declared here too, as glibc does. Link with -lgnu.
 */
#ifndef _GNU_SYS_RANDOM_H
#define _GNU_SYS_RANDOM_H


#include <sys/cdefs.h>
#include <sys/types.h>
#include <unistd.h>


#define GRND_NONBLOCK	0x0001
#define GRND_RANDOM		0x0002
#define GRND_INSECURE	0x0004


__BEGIN_DECLS

ssize_t	getrandom(void* buffer, size_t length, unsigned int flags);

__END_DECLS


#endif	/* _GNU_SYS_RANDOM_H */
