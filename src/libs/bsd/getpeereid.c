/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 *
 * getpeereid() (FreeBSD, OpenBSD, NetBSD, macOS): the effective user and
 * group ID of the peer of a connected UNIX stream socket, as they were when
 * the connection was made. Built on the unix protocol's SO_PEERCRED, which
 * records the credentials at connect() and listen() time.
 */



#include <errno.h>
#include <sys/socket.h>
#include <unistd.h>

#include <syscalls.h>


int
getpeereid(int fd, uid_t* _uid, gid_t* _gid)
{
	struct ucred credentials;
	socklen_t length = sizeof(credentials);

	// The system call directly: getsockopt() is in libnetwork, which libbsd
	// does not link against.
	status_t status = _kern_getsockopt(fd, SOL_SOCKET, SO_PEERCRED,
		&credentials, &length);
	if (status != B_OK) {
		// As on FreeBSD: EINVAL for sockets that have no such peer
		// (datagram sockets, other address families); EBADF, ENOTSOCK and
		// ENOTCONN pass through.
		if (status == EOPNOTSUPP || status == ENOPROTOOPT)
			status = EINVAL;
		errno = status;
		return -1;
	}

	if (_uid != NULL)
		*_uid = credentials.uid;
	if (_gid != NULL)
		*_gid = credentials.gid;
	return 0;
}
