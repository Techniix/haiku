/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 *
 * The Linux epoll interface: a file descriptor that watches a set of other
 * file descriptors and reports which of them are ready, edge- or
 * level-triggered. Names, values and the layout of struct epoll_event are
 * those of Linux (struct epoll_event is packed on x86_64, as there), so
 * code written for Linux builds unchanged. Link with -lgnu.
 *
 * Differences from Linux (see 3rdparty/ce/PORTING.md, "epoll"):
 *  - EPOLLEXCLUSIVE is accepted on EPOLL_CTL_ADD and has no effect: every
 *    epoll instance watching the descriptor is woken.
 *  - EPOLLWAKEUP is accepted and ignored (Haiku has no suspend blockers).
 *  - EPOLLRDHUP is reported when the descriptor reports a disconnect
 *    (together with EPOLLHUP), not for a peer's half-close.
 *  - Closing an epoll descriptor while another thread waits on it wakes
 *    that thread with EBADF.
 */
#ifndef _GNU_SYS_EPOLL_H
#define _GNU_SYS_EPOLL_H


#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <sys/cdefs.h>
#include <time.h>


/* flags for epoll_create1() */
#define EPOLL_CLOEXEC	O_CLOEXEC
#define EPOLL_CLOFORK	O_CLOFORK	/* Haiku extension */

/* events */
#define EPOLLIN			0x00000001
#define EPOLLPRI		0x00000002
#define EPOLLOUT		0x00000004
#define EPOLLERR		0x00000008
#define EPOLLHUP		0x00000010
#define EPOLLRDNORM		0x00000040
#define EPOLLRDBAND		0x00000080
#define EPOLLWRNORM		0x00000100
#define EPOLLWRBAND		0x00000200
#define EPOLLMSG		0x00000400	/* accepted, never reported */
#define EPOLLRDHUP		0x00002000

/* input flags */
#define EPOLLEXCLUSIVE	(1u << 28)	/* accepted on ADD, no effect */
#define EPOLLWAKEUP		(1u << 29)	/* accepted, ignored */
#define EPOLLONESHOT	(1u << 30)
#define EPOLLET			(1u << 31)

/* operations for epoll_ctl() */
#define EPOLL_CTL_ADD	1
#define EPOLL_CTL_DEL	2
#define EPOLL_CTL_MOD	3


typedef union epoll_data {
	void*		ptr;
	int			fd;
	uint32_t	u32;
	uint64_t	u64;
} epoll_data_t;

struct epoll_event {
	uint32_t		events;
	epoll_data_t	data;
}
#if defined(__x86_64__)
	__attribute__((__packed__))
#endif
;


__BEGIN_DECLS


int		epoll_create(int size);
int		epoll_create1(int flags);
int		epoll_ctl(int epfd, int op, int fd, struct epoll_event* event);
int		epoll_wait(int epfd, struct epoll_event* events, int maxEvents,
			int timeout);
int		epoll_pwait(int epfd, struct epoll_event* events, int maxEvents,
			int timeout, const sigset_t* sigMask);
int		epoll_pwait2(int epfd, struct epoll_event* events, int maxEvents,
			const struct timespec* timeout, const sigset_t* sigMask);


__END_DECLS


#endif	/* _GNU_SYS_EPOLL_H */
