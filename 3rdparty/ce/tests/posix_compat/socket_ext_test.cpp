/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 */

/*!	In-guest test for getpeereid() (libbsd) and for POLLRDHUP/EPOLLRDHUP:
	a peer's shutdown(SHUT_WR) is reported as POLLIN | POLLRDHUP without
	POLLHUP, by poll() and epoll, on TCP and UNIX stream sockets, as on
	Linux; POLLHUP once both directions are shut.
	Output format: see 3rdparty/ce/README.md.
*/

#define _DEFAULT_SOURCE
	// getpeereid() in the bsd <unistd.h>

#include <sys/epoll.h>

#include <OS.h>

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>


#define SUITE "socket_ext"


static int sFailures = 0;


static void
report(const char* name, bool pass, const char* format, ...)
{
	char detail[256];
	va_list args;
	va_start(args, format);
	vsnprintf(detail, sizeof(detail), format, args);
	va_end(args);

	printf("CE-TEST %s.%s %s %s\n", SUITE, name, pass ? "PASS" : "FAIL",
		detail);
	fflush(stdout);
	if (!pass)
		sFailures++;
}


static short
poll_now(int fd, short events, int timeout = 0)
{
	struct pollfd pfd = { fd, events, 0 };
	int result = poll(&pfd, 1, timeout);
	if (result < 0)
		return -1;
	return pfd.revents;
}


static uint32_t
epoll_now(int fd, uint32_t events, int timeout = 0)
{
	int ep = epoll_create1(0);
	struct epoll_event event = {};
	event.events = events;
	event.data.fd = fd;
	epoll_ctl(ep, EPOLL_CTL_ADD, fd, &event);
	struct epoll_event out = {};
	int count = epoll_wait(ep, &out, 1, timeout);
	close(ep);
	return count == 1 ? out.events : 0;
}


// #pragma mark - connections


static void
tcp_pair(int fds[2])
{
	int listener = socket(AF_INET, SOCK_STREAM, 0);
	struct sockaddr_in address = {};
	address.sin_family = AF_INET;
	address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	bind(listener, (struct sockaddr*)&address, sizeof(address));
	socklen_t length = sizeof(address);
	getsockname(listener, (struct sockaddr*)&address, &length);
	listen(listener, 1);
	fds[0] = socket(AF_INET, SOCK_STREAM, 0);
	connect(fds[0], (struct sockaddr*)&address, sizeof(address));
	fds[1] = accept(listener, NULL, NULL);
	close(listener);
}


static void
unix_pair(int fds[2])
{
	socketpair(AF_UNIX, SOCK_STREAM, 0, fds);
}


struct DelayedShutdown {
	int			fd;
	bigtime_t	delay;
};


static void*
shutdown_later(void* data)
{
	DelayedShutdown* args = (DelayedShutdown*)data;
	snooze(args->delay);
	shutdown(args->fd, SHUT_WR);
	return NULL;
}


static void
test_half_close(const char* kind, void (*makePair)(int[2]))
{
	char name[64];
	int fds[2];
	makePair(fds);
	int local = fds[0], peer = fds[1];

	short revents = poll_now(local, POLLIN | POLLRDHUP);
	snprintf(name, sizeof(name), "%s_idle", kind);
	report(name, revents == 0, "revents %#x (want 0)", revents);

	// Data, then the peer's half-close: readable, RDHUP, no HUP, even
	// while the data is still queued.
	write(peer, "abc", 3);
	shutdown(peer, SHUT_WR);
	snooze(50000);
	revents = poll_now(local, POLLIN | POLLRDHUP | POLLOUT);
	snprintf(name, sizeof(name), "%s_rdhup_with_data", kind);
	report(name, revents == (POLLIN | POLLRDHUP | POLLOUT),
		"revents %#x (want POLLIN|POLLRDHUP|POLLOUT %#x, no POLLHUP %#x)",
		revents, POLLIN | POLLRDHUP | POLLOUT, POLLHUP);

	uint32_t events = epoll_now(local, EPOLLIN | EPOLLRDHUP);
	snprintf(name, sizeof(name), "%s_epoll_rdhup", kind);
	report(name, events == (EPOLLIN | EPOLLRDHUP),
		"events %#x (want EPOLLIN|EPOLLRDHUP %#x)", events,
		EPOLLIN | EPOLLRDHUP);

	// Not asked for, not reported.
	revents = poll_now(local, POLLIN);
	snprintf(name, sizeof(name), "%s_rdhup_only_if_requested", kind);
	report(name, revents == POLLIN, "revents %#x (want POLLIN)", revents);

	char buffer[16];
	ssize_t first = read(local, buffer, sizeof(buffer));
	ssize_t second = read(local, buffer, sizeof(buffer));
	revents = poll_now(local, POLLIN | POLLRDHUP);
	snprintf(name, sizeof(name), "%s_eof_still_rdhup", kind);
	report(name, first == 3 && second == 0
			&& revents == (POLLIN | POLLRDHUP),
		"read %zd then %zd, revents %#x", first, second, revents);

	// The other direction still works.
	ssize_t written = write(local, "reply", 5);
	ssize_t received = read(peer, buffer, sizeof(buffer));
	snprintf(name, sizeof(name), "%s_reply_after_half_close", kind);
	report(name, written == 5 && received == 5, "wrote %zd, peer read %zd",
		written, received);

	// Both directions shut: now POLLHUP too.
	shutdown(local, SHUT_WR);
	snooze(50000);
	revents = poll_now(local, POLLIN | POLLRDHUP);
	snprintf(name, sizeof(name), "%s_hup_both_shut", kind);
	report(name, (revents & (POLLHUP | POLLRDHUP | POLLIN))
			== (POLLHUP | POLLRDHUP | POLLIN),
		"revents %#x (want POLLIN|POLLRDHUP|POLLHUP)", revents);
	close(local);
	close(peer);

	// A blocked poll() and epoll_wait() are woken by the half-close.
	makePair(fds);
	local = fds[0];
	peer = fds[1];
	DelayedShutdown args = { peer, 100000 };
	pthread_t thread;
	pthread_create(&thread, NULL, shutdown_later, &args);
	bigtime_t start = system_time();
	revents = poll_now(local, POLLRDHUP, 5000);
	bigtime_t waited = system_time() - start;
	pthread_join(thread, NULL);
	snprintf(name, sizeof(name), "%s_poll_woken", kind);
	report(name, revents == POLLRDHUP && waited >= 80000 && waited < 4000000,
		"revents %#x after %lld us", revents, (long long)waited);
	close(local);
	close(peer);

	makePair(fds);
	local = fds[0];
	peer = fds[1];
	args.fd = peer;
	pthread_create(&thread, NULL, shutdown_later, &args);
	start = system_time();
	events = epoll_now(local, EPOLLRDHUP, 5000);
	waited = system_time() - start;
	pthread_join(thread, NULL);
	snprintf(name, sizeof(name), "%s_epoll_woken", kind);
	report(name, events == EPOLLRDHUP && waited >= 80000
			&& waited < 4000000,
		"events %#x after %lld us", events, (long long)waited);
	close(local);
	close(peer);

	// The peer closing: RDHUP; HUP for UNIX sockets (the connection is gone)
	// and once our side is shut for TCP too, as on Linux.
	makePair(fds);
	local = fds[0];
	peer = fds[1];
	close(peer);
	snooze(50000);
	revents = poll_now(local, POLLIN | POLLRDHUP);
	snprintf(name, sizeof(name), "%s_peer_closed", kind);
	bool isUnix = strcmp(kind, "unix") == 0;
	short want = POLLIN | POLLRDHUP | (isUnix ? POLLHUP : 0);
	report(name, (revents & ~POLLERR) == want,
		"revents %#x (want %#x)", revents, want);
	close(local);
}


// #pragma mark - getpeereid


static void
test_getpeereid()
{
	int fds[2];
	unix_pair(fds);
	uid_t uid = 12345;
	gid_t gid = 12345;
	int result = getpeereid(fds[0], &uid, &gid);
	report("getpeereid_socketpair", result == 0 && uid == geteuid()
			&& gid == getegid(),
		"result %d, uid %d gid %d (want %d %d)", result, (int)uid, (int)gid,
		(int)geteuid(), (int)getegid());
	close(fds[0]);
	close(fds[1]);

	// A listening socket's accepted connection and the connecting side.
	char path[64];
	snprintf(path, sizeof(path), "/tmp/ce-peereid-%d", (int)getpid());
	unlink(path);
	int listener = socket(AF_UNIX, SOCK_STREAM, 0);
	struct sockaddr_un address = {};
	address.sun_family = AF_UNIX;
	strlcpy(address.sun_path, path, sizeof(address.sun_path));
	bind(listener, (struct sockaddr*)&address, sizeof(address));
	listen(listener, 1);
	int client = socket(AF_UNIX, SOCK_STREAM, 0);
	connect(client, (struct sockaddr*)&address, sizeof(address));
	int server = accept(listener, NULL, NULL);
	uid_t serverUID = 1, clientUID = 1;
	gid_t serverGID = 1, clientGID = 1;
	int serverResult = getpeereid(server, &serverUID, &serverGID);
	int clientResult = getpeereid(client, &clientUID, &clientGID);
	report("getpeereid_connected", serverResult == 0 && clientResult == 0
			&& serverUID == geteuid() && clientUID == geteuid()
			&& serverGID == getegid() && clientGID == getegid(),
		"server %d (%d/%d), client %d (%d/%d)", serverResult,
		(int)serverUID, (int)serverGID, clientResult, (int)clientUID,
		(int)clientGID);

	errno = 0;
	result = getpeereid(listener, &uid, &gid);
	report("getpeereid_listener", result == -1 && errno == ENOTCONN,
		"result %d, errno %s (want ENOTCONN)", result, strerror(errno));
	close(server);
	close(client);
	close(listener);
	unlink(path);

	int unconnected = socket(AF_UNIX, SOCK_STREAM, 0);
	errno = 0;
	result = getpeereid(unconnected, &uid, &gid);
	report("getpeereid_unconnected", result == -1 && errno == ENOTCONN,
		"result %d, errno %s (want ENOTCONN)", result, strerror(errno));
	close(unconnected);

	int datagram[2];
	socketpair(AF_UNIX, SOCK_DGRAM, 0, datagram);
	errno = 0;
	result = getpeereid(datagram[0], &uid, &gid);
	report("getpeereid_datagram", result == -1 && errno == EINVAL,
		"result %d, errno %s (want EINVAL)", result, strerror(errno));
	close(datagram[0]);
	close(datagram[1]);

	int tcp[2];
	tcp_pair(tcp);
	errno = 0;
	result = getpeereid(tcp[0], &uid, &gid);
	report("getpeereid_tcp", result == -1 && errno == EINVAL,
		"result %d, errno %s (want EINVAL)", result, strerror(errno));
	close(tcp[0]);
	close(tcp[1]);

	int pipeFDs[2];
	pipe(pipeFDs);
	errno = 0;
	result = getpeereid(pipeFDs[0], &uid, &gid);
	report("getpeereid_not_socket", result == -1 && errno == ENOTSOCK,
		"result %d, errno %s (want ENOTSOCK)", result, strerror(errno));
	close(pipeFDs[0]);
	close(pipeFDs[1]);
}


// #pragma mark - POLLRDHUP on other descriptors


static void
test_other_descriptors()
{
	// Only stream sockets report it: never a pipe, a regular file or a
	// datagram socket, as on Linux.
	int pipeFDs[2];
	pipe(pipeFDs);
	close(pipeFDs[1]);
	short revents = poll_now(pipeFDs[0], POLLIN | POLLRDHUP);
	report("pipe_no_rdhup", (revents & POLLRDHUP) == 0
			&& (revents & POLLHUP) != 0,
		"revents %#x (want POLLHUP, no POLLRDHUP)", revents);
	close(pipeFDs[0]);

	int file = open("/boot/system/lib/libroot.so", O_RDONLY);
	revents = poll_now(file, POLLIN | POLLRDHUP);
	report("file_no_rdhup", revents == POLLIN,
		"revents %#x (want POLLIN)", revents);
	close(file);

	int udp = socket(AF_INET, SOCK_DGRAM, 0);
	revents = poll_now(udp, POLLIN | POLLOUT | POLLRDHUP);
	report("udp_no_rdhup", (revents & POLLRDHUP) == 0,
		"revents %#x", revents);
	close(udp);
}


int
main()
{
	printf("CE-SUITE " SUITE " START\n");
	fflush(stdout);

	report("pollrdhup_value", POLLRDHUP == 0x2000 && EPOLLRDHUP == 0x2000,
		"POLLRDHUP %#x", POLLRDHUP);

	test_getpeereid();
	test_half_close("tcp", tcp_pair);
	test_half_close("unix", unix_pair);
	test_other_descriptors();

	printf("CE-SUITE " SUITE " DONE failures=%d\n", sFailures);
	fflush(stdout);
	return sFailures == 0 ? 0 : 1;
}
