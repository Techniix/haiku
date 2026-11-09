/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 */

/*!	POSIX message queues: the libroot side. The queues themselves are kernel
	objects (src/system/kernel/posix/mqueue.cpp); this file converts the
	POSIX calling conventions (variadic mq_open(), absolute timespec
	timeouts, sigevent with SIGEV_THREAD) to the syscalls.

	mq_send(), mq_timedsend(), mq_receive() and mq_timedreceive() are
	cancellation points, as POSIX requires.
*/


#include <mqueue.h>

#include <errno.h>
#include <stdarg.h>
#include <unistd.h>

#include <errno_private.h>
#include <pthread_private.h>
#include <syscall_utils.h>
#include <syscalls.h>
#include <thread_defs.h>
#include <time_private.h>
#include <umask.h>
#include <user_thread.h>


/*!	Converts an absolute CLOCK_REALTIME timeout for the kernel. An invalid
	timespec becomes -1: POSIX wants EINVAL only if the call has to block,
	so the kernel decides when it would wait.
*/
static void
absolute_timeout(const struct timespec* timeout, uint32& _flags,
	bigtime_t& _timeout)
{
	if (timeout == NULL) {
		_flags = 0;
		_timeout = B_INFINITE_TIMEOUT;
		return;
	}

	_flags = B_ABSOLUTE_REAL_TIME_TIMEOUT;
	if (!timespec_to_bigtime(*timeout, _timeout))
		_timeout = -1;
}


static status_t
notify_thread_entry(void* _entry, void* _value)
{
	// Same as the SIGEV_THREAD entry of POSIX timers (timer_support.cpp):
	// the kernel created this thread with signals deferred.
	pthread_thread thread;
	__init_pthread(&thread, NULL, NULL);
	thread.flags |= THREAD_DETACHED;

	get_user_thread()->pthread = &thread;
	undefer_signals();

	union sigval value;
	value.sival_ptr = _value;
	((void (*)(union sigval))_entry)(value);
	return B_OK;
}


mqd_t
mq_open(const char* name, int openFlags, ...)
{
	mode_t mode = 0;
	struct mq_attr* attr = NULL;
	if ((openFlags & O_CREAT) != 0) {
		va_list args;
		va_start(args, openFlags);
		mode = va_arg(args, int) & ~__gUmask;
		attr = va_arg(args, struct mq_attr*);
		va_end(args);
	}

	RETURN_AND_SET_ERRNO(_kern_mqueue_open(name, openFlags, mode, attr));
}


int
mq_close(mqd_t queue)
{
	// The descriptor must be a message queue (EBADF otherwise, as POSIX
	// says), so check before closing an unrelated file.
	status_t status = _kern_mqueue_getsetattr(queue, NULL, NULL);
	if (status != B_OK)
		RETURN_AND_SET_ERRNO(status);

	return close(queue);
}


int
mq_unlink(const char* name)
{
	RETURN_AND_SET_ERRNO(_kern_mqueue_unlink(name));
}


int
mq_send(mqd_t queue, const char* message, size_t length,
	unsigned int priority)
{
	RETURN_AND_SET_ERRNO_TEST_CANCEL(_kern_mqueue_send(queue, message, length,
		priority, 0, B_INFINITE_TIMEOUT));
}


int
mq_timedsend(mqd_t queue, const char* message, size_t length,
	unsigned int priority, const struct timespec* absoluteTimeout)
{
	uint32 flags;
	bigtime_t timeout;
	absolute_timeout(absoluteTimeout, flags, timeout);

	RETURN_AND_SET_ERRNO_TEST_CANCEL(_kern_mqueue_send(queue, message, length,
		priority, flags, timeout));
}


ssize_t
mq_receive(mqd_t queue, char* buffer, size_t length, unsigned int* _priority)
{
	RETURN_AND_SET_ERRNO_TEST_CANCEL(_kern_mqueue_receive(queue, buffer,
		length, _priority, 0, B_INFINITE_TIMEOUT));
}


ssize_t
mq_timedreceive(mqd_t queue, char* buffer, size_t length,
	unsigned int* _priority, const struct timespec* absoluteTimeout)
{
	uint32 flags;
	bigtime_t timeout;
	absolute_timeout(absoluteTimeout, flags, timeout);

	RETURN_AND_SET_ERRNO_TEST_CANCEL(_kern_mqueue_receive(queue, buffer,
		length, _priority, flags, timeout));
}


int
mq_getattr(mqd_t queue, struct mq_attr* attr)
{
	if (attr == NULL)
		RETURN_AND_SET_ERRNO(EFAULT);

	RETURN_AND_SET_ERRNO(_kern_mqueue_getsetattr(queue, NULL, attr));
}


int
mq_setattr(mqd_t queue, const struct mq_attr* newAttr,
	struct mq_attr* oldAttr)
{
	if (newAttr == NULL)
		RETURN_AND_SET_ERRNO(EFAULT);

	RETURN_AND_SET_ERRNO(_kern_mqueue_getsetattr(queue, newAttr, oldAttr));
}


int
mq_notify(mqd_t queue, const struct sigevent* event)
{
	thread_creation_attributes threadAttributes;
	if (event != NULL && event->sigev_notify == SIGEV_THREAD) {
		if (event->sigev_notify_function == NULL)
			RETURN_AND_SET_ERRNO(EINVAL);

		status_t status = __pthread_init_creation_attributes(
			event->sigev_notify_attributes, NULL, &notify_thread_entry,
			(void*)event->sigev_notify_function, event->sigev_value.sival_ptr,
			"mqueue notify", &threadAttributes);
		if (status != B_OK)
			RETURN_AND_SET_ERRNO(status);

		threadAttributes.flags |= THREAD_CREATION_FLAG_DEFER_SIGNALS;
	}

	RETURN_AND_SET_ERRNO(_kern_mqueue_notify(queue, event,
		event != NULL && event->sigev_notify == SIGEV_THREAD
			? &threadAttributes : NULL));
}
