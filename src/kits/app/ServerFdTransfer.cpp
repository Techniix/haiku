/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 */


#include <ServerFdTransfer.h>

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <syscalls.h>


// libbe does not link libnetwork: the socket calls are made through the
// kernel's syscalls directly (they return the error instead of setting
// errno).


namespace BPrivate {


static pthread_mutex_t sLock = PTHREAD_MUTEX_INITIALIZER;
static int sSocket = -1;
static pid_t sSocketOwner = -1;
	// the process that connected: a child after fork() shares the socket,
	// but app_server sees the parent as its peer, so it connects anew


static status_t
connect_locked()
{
	if (sSocket >= 0 && sSocketOwner == getpid())
		return B_OK;
	if (sSocket >= 0) {
		close(sSocket);
		sSocket = -1;
	}
	int fd = _kern_socket(AF_UNIX, SOCK_SEQPACKET, 0);
	if (fd < 0)
		return fd;
	fcntl(fd, F_SETFD, FD_CLOEXEC);
	sockaddr_un address = {};
	address.sun_family = AF_UNIX;
	strlcpy(address.sun_path, APP_SERVER_FD_SOCKET,
		sizeof(address.sun_path));
	status_t status = _kern_connect(fd, (sockaddr*)&address,
		sizeof(address));
	if (status != B_OK) {
		close(fd);
		return status == ENOENT || status == ECONNREFUSED
			|| status == B_ENTRY_NOT_FOUND ? B_NO_INIT : status;
	}
	sSocket = fd;
	sSocketOwner = getpid();
	return B_OK;
}


static status_t
send_locked(int fd, int32* _token)
{
	fd_transfer_request request = { FD_TRANSFER_MAGIC, FD_TRANSFER_VERSION };
	char control[CMSG_SPACE(sizeof(int))];
	memset(control, 0, sizeof(control));
	iovec iov = { &request, sizeof(request) };
	msghdr message = {};
	message.msg_iov = &iov;
	message.msg_iovlen = 1;
	message.msg_control = control;
	message.msg_controllen = sizeof(control);
	cmsghdr* header = CMSG_FIRSTHDR(&message);
	header->cmsg_level = SOL_SOCKET;
	header->cmsg_type = SCM_RIGHTS;
	header->cmsg_len = CMSG_LEN(sizeof(int));
	memcpy(CMSG_DATA(header), &fd, sizeof(int));
	ssize_t sent;
	do {
		sent = _kern_sendmsg(sSocket, &message, 0);
	} while (sent == B_INTERRUPTED);
	if (sent != (ssize_t)sizeof(request))
		return sent < 0 ? sent : B_ERROR;

	fd_transfer_reply reply;
	ssize_t received;
	do {
		received = _kern_recv(sSocket, &reply, sizeof(reply), 0);
	} while (received == B_INTERRUPTED);
	if (received != (ssize_t)sizeof(reply))
		return received < 0 ? received : B_ERROR;
	if (reply.status != B_OK)
		return reply.status;
	*_token = reply.token;
	return B_OK;
}


status_t
app_server_send_fd(int fd, int32* _token)
{
	if (fd < 0 || _token == NULL)
		return B_BAD_VALUE;
	pthread_mutex_lock(&sLock);
	status_t status = connect_locked();
	if (status == B_OK) {
		status = send_locked(fd, _token);
		if (status == EPIPE || status == ECONNRESET || status == B_ERROR) {
			// app_server was restarted: once more on a new connection
			close(sSocket);
			sSocket = -1;
			status = connect_locked();
			if (status == B_OK)
				status = send_locked(fd, _token);
		}
	}
	pthread_mutex_unlock(&sLock);
	return status;
}


}	// namespace BPrivate
