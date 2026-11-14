/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 *
 * The Linux/FreeBSD eventfd interface: a file descriptor carrying a 64-bit
 * counter, used for wakeups between threads and processes. In libroot
 * (Renga; moved from libgnu, which programs no longer need for it).
 */
#ifndef _SYS_EVENTFD_H
#define _SYS_EVENTFD_H


#include <fcntl.h>
#include <stdint.h>
#include <sys/cdefs.h>


typedef uint64_t eventfd_t;

/* flags for eventfd() */
#define EFD_SEMAPHORE	0x00000001	/* reads return 1 and decrement */
#define EFD_CLOEXEC		O_CLOEXEC
#define EFD_NONBLOCK	O_NONBLOCK
#define EFD_CLOFORK		O_CLOFORK	/* Haiku extension */


__BEGIN_DECLS


int		eventfd(unsigned int initialValue, int flags);
int		eventfd_read(int fd, eventfd_t* value);
int		eventfd_write(int fd, eventfd_t value);


__END_DECLS


#endif	/* _SYS_EVENTFD_H */
