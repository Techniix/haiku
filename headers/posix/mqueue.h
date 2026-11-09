/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 */
#ifndef _MQUEUE_H
#define _MQUEUE_H


#include <fcntl.h>
#include <signal.h>
#include <sys/types.h>
#include <time.h>


/* A message queue descriptor is a file descriptor: it works with select(),
   poll() and wait_for_objects(), is inherited by fork() and closed on
   exec(). */
typedef int mqd_t;

struct mq_attr {
	long	mq_flags;		/* 0 or O_NONBLOCK */
	long	mq_maxmsg;		/* maximum number of messages */
	long	mq_msgsize;		/* maximum message size in bytes */
	long	mq_curmsgs;		/* messages currently queued */
	long	__reserved[4];
};


#ifdef __cplusplus
extern "C" {
#endif

mqd_t	mq_open(const char* name, int openFlags, ...);
int		mq_close(mqd_t queue);
int		mq_unlink(const char* name);

int		mq_send(mqd_t queue, const char* message, size_t length,
			unsigned int priority);
int		mq_timedsend(mqd_t queue, const char* message, size_t length,
			unsigned int priority, const struct timespec* absoluteTimeout);
ssize_t	mq_receive(mqd_t queue, char* buffer, size_t length,
			unsigned int* _priority);
ssize_t	mq_timedreceive(mqd_t queue, char* buffer, size_t length,
			unsigned int* _priority, const struct timespec* absoluteTimeout);

int		mq_getattr(mqd_t queue, struct mq_attr* attr);
int		mq_setattr(mqd_t queue, const struct mq_attr* newAttr,
			struct mq_attr* oldAttr);
int		mq_notify(mqd_t queue, const struct sigevent* event);

#ifdef __cplusplus
}
#endif


#endif	/* _MQUEUE_H */
