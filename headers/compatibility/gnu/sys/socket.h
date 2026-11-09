/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 *
 * Linux additions to <sys/socket.h>: sendmmsg() and recvmmsg(), batch
 * versions of sendmsg()/recvmsg(). Implemented in libgnu as loops over the
 * single-message calls: the semantics match Linux, the per-call savings
 * do not (yet). They live in libnetwork, next to the other socket calls,
 * so no extra library is needed.
 */
#ifndef _GNU_SYS_SOCKET_H
#define _GNU_SYS_SOCKET_H


#include_next <sys/socket.h>

#include <sys/cdefs.h>
#include <time.h>


struct mmsghdr {
	struct msghdr	msg_hdr;	/* the message */
	unsigned int	msg_len;	/* bytes sent or received */
};

/* recvmmsg(): return after the first message (other flags are passed on) */
#ifndef MSG_WAITFORONE
#	define MSG_WAITFORONE	0x10000
#endif


__BEGIN_DECLS

int		sendmmsg(int socket, struct mmsghdr* messages, unsigned int count,
			int flags);
int		recvmmsg(int socket, struct mmsghdr* messages, unsigned int count,
			int flags, struct timespec* timeout);

__END_DECLS


#endif	/* _GNU_SYS_SOCKET_H */
