/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 */


#include "FdTransferServer.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <syslog.h>
#include <unistd.h>

#include <vector>

#include <Autolock.h>

#include <ServerFdTransfer.h>


using namespace BPrivate;


// Clients connected at once (each application keeps one connection).
static const size_t kMaxClients = 256;
// Descriptors kept at once, over all applications.
static const size_t kMaxEntries = 4096;


static FdTransferServer* sDefault = NULL;


FdTransferServer::FdTransferServer()
	:
	fLock("fd transfer"),
	fNextToken(1),
	fListener(-1),
	fThread(-1)
{
	sDefault = this;
}


FdTransferServer::~FdTransferServer()
{
	if (sDefault == this)
		sDefault = NULL;
	// The thread blocks in poll(); app_server only ends with the process.
	BAutolock locker(fLock);
	for (std::map<int32, Entry>::iterator it = fEntries.begin();
			it != fEntries.end(); it++) {
		close(it->second.fd);
	}
	fEntries.clear();
}


/*static*/ FdTransferServer*
FdTransferServer::Default()
{
	return sDefault;
}


status_t
FdTransferServer::Start()
{
	mkdir("/boot/system/var/run", 0755);
	mkdir(APP_SERVER_FD_SOCKET_DIRECTORY, 0755);
	unlink(APP_SERVER_FD_SOCKET);

	fListener = socket(AF_UNIX, SOCK_SEQPACKET, 0);
	if (fListener < 0)
		return errno;
	fcntl(fListener, F_SETFD, FD_CLOEXEC);
	sockaddr_un address = {};
	address.sun_family = AF_UNIX;
	strlcpy(address.sun_path, APP_SERVER_FD_SOCKET,
		sizeof(address.sun_path));
	if (bind(fListener, (sockaddr*)&address, sizeof(address)) != 0
		|| listen(fListener, 16) != 0) {
		status_t status = errno;
		syslog(LOG_ERR, "app_server: cannot listen on %s: %s",
			APP_SERVER_FD_SOCKET, strerror(status));
		close(fListener);
		fListener = -1;
		return status;
	}
	chmod(APP_SERVER_FD_SOCKET, 0666);

	fThread = spawn_thread(_ThreadEntry, "fd transfer", B_NORMAL_PRIORITY,
		this);
	if (fThread < 0)
		return fThread;
	return resume_thread(fThread);
}


int
FdTransferServer::Take(team_id team, int32 token)
{
	BAutolock locker(fLock);
	_Expire();
	std::map<int32, Entry>::iterator it = fEntries.find(token);
	if (it == fEntries.end() || it->second.team != team)
		return -1;
	int fd = it->second.fd;
	fEntries.erase(it);
	return fd;
}


void
FdTransferServer::TeamQuit(team_id team)
{
	BAutolock locker(fLock);
	for (std::map<int32, Entry>::iterator it = fEntries.begin();
			it != fEntries.end();) {
		if (it->second.team == team) {
			close(it->second.fd);
			fEntries.erase(it++);
		} else
			it++;
	}
}


void
FdTransferServer::_Expire()
{
	bigtime_t now = system_time();
	for (std::map<int32, Entry>::iterator it = fEntries.begin();
			it != fEntries.end();) {
		if (now - it->second.received > kFdTransferLifetime) {
			close(it->second.fd);
			fEntries.erase(it++);
		} else
			it++;
	}
}


/*static*/ status_t
FdTransferServer::_ThreadEntry(void* data)
{
	((FdTransferServer*)data)->_Run();
	return B_OK;
}


void
FdTransferServer::_Run()
{
	std::vector<pollfd> fds;
	pollfd listener = { fListener, POLLIN, 0 };
	fds.push_back(listener);

	while (true) {
		int result = poll(fds.data(), fds.size(), -1);
		if (result < 0) {
			if (errno == EINTR)
				continue;
			syslog(LOG_ERR, "app_server: fd transfer poll: %s",
				strerror(errno));
			return;
		}
		for (size_t i = fds.size(); i-- > 1;) {
			if (fds[i].revents == 0)
				continue;
			bool keep = true;
			if ((fds[i].revents & POLLIN) != 0)
				keep = _Receive(fds[i].fd);
			else if ((fds[i].revents & (POLLHUP | POLLERR | POLLNVAL)) != 0)
				keep = false;
			if (!keep) {
				close(fds[i].fd);
				fds.erase(fds.begin() + i);
			}
		}
		if ((fds[0].revents & POLLIN) != 0) {
			int client = accept(fListener, NULL, NULL);
			if (client >= 0) {
				if (fds.size() > kMaxClients) {
					close(client);
				} else {
					fcntl(client, F_SETFD, FD_CLOEXEC);
					pollfd entry = { client, POLLIN, 0 };
					fds.push_back(entry);
				}
			}
		}
	}
}


/*!	One request: a fd_transfer_request with one descriptor. Replies with a
	fd_transfer_reply. Returns false when the connection is to be closed:
	the client closed it (recvmsg() returns 0; the protocol has no empty
	messages) or it failed.
*/
bool
FdTransferServer::_Receive(int client)
{
	fd_transfer_request request;
	char control[CMSG_SPACE(4 * sizeof(int))];
	memset(control, 0, sizeof(control));
	iovec iov = { &request, sizeof(request) };
	msghdr message = {};
	message.msg_iov = &iov;
	message.msg_iovlen = 1;
	message.msg_control = control;
	message.msg_controllen = sizeof(control);
	ssize_t received = recvmsg(client, &message, MSG_DONTWAIT);
	if (received < 0)
		return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR;
	if (received == 0) {
		// end of the connection (descriptors with an empty message are
		// closed with it)
		for (cmsghdr* header = CMSG_FIRSTHDR(&message); header != NULL;
				header = CMSG_NXTHDR(&message, header)) {
			if (header->cmsg_level != SOL_SOCKET
				|| header->cmsg_type != SCM_RIGHTS)
				continue;
			int count = (header->cmsg_len - CMSG_LEN(0)) / sizeof(int);
			for (int i = 0; i < count; i++) {
				int fd;
				memcpy(&fd, CMSG_DATA(header) + i * sizeof(int), sizeof(int));
				close(fd);
			}
		}
		return false;
	}

	std::vector<int> descriptors;
	for (cmsghdr* header = CMSG_FIRSTHDR(&message); header != NULL;
			header = CMSG_NXTHDR(&message, header)) {
		if (header->cmsg_level != SOL_SOCKET
			|| header->cmsg_type != SCM_RIGHTS)
			continue;
		int count = (header->cmsg_len - CMSG_LEN(0)) / sizeof(int);
		for (int i = 0; i < count; i++) {
			int fd;
			memcpy(&fd, CMSG_DATA(header) + i * sizeof(int), sizeof(int));
			// not inherited by the programs app_server starts
			fcntl(fd, F_SETFD, FD_CLOEXEC);
			descriptors.push_back(fd);
		}
	}

	fd_transfer_reply reply = { B_OK, -1 };
	ucred credentials;
	socklen_t length = sizeof(credentials);
	if (received != (ssize_t)sizeof(request)
		|| request.magic != FD_TRANSFER_MAGIC
		|| request.version != FD_TRANSFER_VERSION) {
		reply.status = B_BAD_DATA;
	} else if ((message.msg_flags & MSG_CTRUNC) != 0
		|| descriptors.size() != 1) {
		reply.status = B_BAD_VALUE;
	} else if (getsockopt(client, SOL_SOCKET, SO_PEERCRED, &credentials,
			&length) != 0) {
		reply.status = errno;
	} else {
		BAutolock locker(fLock);
		_Expire();
		if (fEntries.size() >= kMaxEntries)
			reply.status = B_NO_MEMORY;
		else {
			int32 token = fNextToken++;
			if (fNextToken <= 0)
				fNextToken = 1;
			Entry entry = { (team_id)credentials.pid, descriptors[0],
				system_time() };
			fEntries[token] = entry;
			descriptors.clear();
			reply.token = token;
		}
	}
	for (size_t i = 0; i < descriptors.size(); i++)
		close(descriptors[i]);
	send(client, &reply, sizeof(reply), MSG_DONTWAIT);
	return true;
}
