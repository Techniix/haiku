/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 */
#ifndef _KERNEL_EPOLL_H
#define _KERNEL_EPOLL_H


#include <OS.h>
#include <signal.h>


struct epoll_event;
struct file_descriptor;


#ifdef __cplusplus
extern "C" {
#endif


extern void		epoll_descriptor_closed(struct file_descriptor* descriptor);

extern int		_user_epoll_create(int openFlags);
extern status_t	_user_epoll_ctl(int epfd, int op, int fd,
					const struct epoll_event* event);
extern ssize_t	_user_epoll_wait(int epfd, struct epoll_event* events,
					int maxEvents, bigtime_t timeout, const sigset_t* sigMask);


#ifdef __cplusplus
}
#endif


#endif	// _KERNEL_EPOLL_H
