/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 */
#ifndef KERNEL_POSIX_MQUEUE_H
#define KERNEL_POSIX_MQUEUE_H


#include <sys/cdefs.h>
#include <sys/types.h>

#include <OS.h>


struct mq_attr;
struct sigevent;
struct thread_creation_attributes;


__BEGIN_DECLS

void		mqueue_init();

int			_user_mqueue_open(const char* name, int openFlags, mode_t mode,
				const struct mq_attr* attr);
status_t	_user_mqueue_unlink(const char* name);
status_t	_user_mqueue_send(int fd, const void* buffer, size_t length,
				uint32 priority, uint32 flags, bigtime_t timeout);
ssize_t		_user_mqueue_receive(int fd, void* buffer, size_t length,
				uint32* priority, uint32 flags, bigtime_t timeout);
status_t	_user_mqueue_getsetattr(int fd, const struct mq_attr* newAttr,
				struct mq_attr* oldAttr);
status_t	_user_mqueue_notify(int fd, const struct sigevent* event,
				const struct thread_creation_attributes* threadAttributes);

__END_DECLS


#endif	// KERNEL_POSIX_MQUEUE_H
