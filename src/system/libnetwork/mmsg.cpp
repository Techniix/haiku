/*
 * Copyright 2026, Haiku Community Edition contributors.
 * Distributed under the terms of the MIT License.
 *
 * sendmmsg() and recvmmsg(): Linux batch versions of sendmsg()/recvmsg().
 * Implemented as loops over the single-message calls, which gives the
 * Linux semantics (and lets ports drop their fallbacks) without the
 * per-call savings. They live in libnetwork with the other socket calls;
 * the declarations (and struct mmsghdr) are in the gnu <sys/socket.h>
 * because these are Linux extensions. Error handling follows Linux: an error on the first
 * message is returned; an error after some messages ends the batch and
 * the number of messages done is returned.
 */


#include <sys/socket.h>

#include <errno.h>
#include <time.h>

#include <OS.h>

#include <errno_private.h>


int
sendmmsg(int socket, struct mmsghdr* messages, unsigned int count, int flags)
{
	if (count > 1024)
		count = 1024;	// UIO_MAXIOV, as Linux

	unsigned int done = 0;
	for (; done < count; done++) {
		ssize_t sent = sendmsg(socket, &messages[done].msg_hdr, flags);
		if (sent < 0)
			return done > 0 ? (int)done : -1;
		messages[done].msg_len = sent;
	}
	return done;
}


int
recvmmsg(int socket, struct mmsghdr* messages, unsigned int count, int flags,
	struct timespec* timeout)
{
	if (count > 1024)
		count = 1024;

	bigtime_t deadline = B_INFINITE_TIMEOUT;
	if (timeout != NULL) {
		if (timeout->tv_sec < 0 || timeout->tv_nsec < 0
			|| timeout->tv_nsec >= 1000000000) {
			__set_errno(EINVAL);
			return -1;
		}
		deadline = system_time() + (bigtime_t)timeout->tv_sec * 1000000
			+ timeout->tv_nsec / 1000;
	}

	bool waitForOne = (flags & MSG_WAITFORONE) != 0;
	flags &= ~MSG_WAITFORONE;

	unsigned int done = 0;
	for (; done < count; done++) {
		int messageFlags = flags;
		if (done > 0 && waitForOne)
			messageFlags |= MSG_DONTWAIT;

		ssize_t received = recvmsg(socket, &messages[done].msg_hdr,
			messageFlags);
		if (received < 0)
			return done > 0 ? (int)done : -1;
		messages[done].msg_len = received;

		// Linux checks the timeout only after each received message.
		if (timeout != NULL && system_time() >= deadline) {
			done++;
			break;
		}
	}
	return done;
}
