/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 */

/*!	In-guest test for TCP keepalive: SO_KEEPALIVE probes and the
	TCP_KEEPIDLE / TCP_KEEPINTVL / TCP_KEEPCNT socket options.

	Everything runs over the loopback interface:
	  - option defaults, round trips and range checks;
	  - accepted sockets inherit SO_KEEPALIVE and the TCP_KEEP* values;
	  - a live connection with idle 1 s / interval 1 s / count 2 survives
	    well past the point where it would have been dropped, which shows
	    that answered probes reset the probe count;
	  - a peer that goes silent is detected: the server listens on a
	    temporary alias address on "loop", and removing that alias makes
	    every probe go unanswered (the stack drops segments for an address
	    it no longer owns, without a reset). The keepalive connection then
	    fails with ETIMEDOUT after idle + count * interval, while a
	    connection without SO_KEEPALIVE is left alone. The recv() that is
	    already blocked when the timeout hits must return right away; this
	    also covers the ReadData() fix (before it, a reader blocked while
	    the connection closed under it slept until its own timeout).

	Bringing "loop" down is not enough for the last test: a connection
	keeps its cached route and loopback delivery continues regardless of
	IFF_UP. An alias added with SIOCAIFADDR also gets no routes of its own,
	and loopback only delivers through RTF_LOCAL routes, so the test adds a
	local host route for the alias itself and removes it at the end.

	Output format: see 3rdparty/ce/README.md.
*/


#include <arpa/inet.h>
#include <net/if.h>
#include <net/route.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/sockio.h>
#include <sys/time.h>
#include <unistd.h>

#include <OS.h>

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>


#define SUITE "tcp_keepalive"


static int sFailures = 0;


static void
report(const char* name, bool pass, const char* format, ...)
{
	char detail[300];
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


static int
get_int(int fd, int level, int option)
{
	int value = -1;
	socklen_t length = sizeof(value);
	if (getsockopt(fd, level, option, &value, &length) != 0)
		return -errno;
	return value;
}


static int
set_int(int fd, int level, int option, int value)
{
	return setsockopt(fd, level, option, &value, sizeof(value)) == 0
		? 0 : errno;
}


static void
set_fast_keepalive(int fd)
{
	set_int(fd, SOL_SOCKET, SO_KEEPALIVE, 1);
	set_int(fd, IPPROTO_TCP, TCP_KEEPIDLE, 1);
	set_int(fd, IPPROTO_TCP, TCP_KEEPINTVL, 1);
	set_int(fd, IPPROTO_TCP, TCP_KEEPCNT, 2);
}


static void
set_receive_timeout(int fd, bigtime_t timeout)
{
	struct timeval tv;
	tv.tv_sec = timeout / 1000000;
	tv.tv_usec = timeout % 1000000;
	setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
}


/*!	Creates a connected loopback pair. If \a keepAliveListener is set, the
	listener gets the fast keepalive settings before accept(), so the
	accepted socket inherits them.
*/
static bool
make_pair(int& client, int& server, bool keepAliveListener,
	in_addr_t serverAddress = htonl(INADDR_LOOPBACK))
{
	client = server = -1;
	int listener = socket(AF_INET, SOCK_STREAM, 0);
	if (listener < 0)
		return false;

	if (keepAliveListener)
		set_fast_keepalive(listener);

	struct sockaddr_in address;
	memset(&address, 0, sizeof(address));
	address.sin_len = sizeof(address);
	address.sin_family = AF_INET;
	address.sin_addr.s_addr = serverAddress;
	socklen_t length = sizeof(address);
	if (bind(listener, (sockaddr*)&address, sizeof(address)) != 0
		|| listen(listener, 1) != 0
		|| getsockname(listener, (sockaddr*)&address, &length) != 0) {
		close(listener);
		return false;
	}

	client = socket(AF_INET, SOCK_STREAM, 0);
	if (client < 0
		|| connect(client, (sockaddr*)&address, sizeof(address)) != 0) {
		close(listener);
		return false;
	}
	server = accept(listener, NULL, NULL);
	close(listener);
	return server >= 0;
}


// A loopback alias outside 127.0.0.1 that the test owns for its duration.
static const in_addr_t kAliasAddress = htonl(0x7f00004d);	// 127.0.0.77


static void
fill_inet(sockaddr_storage& storage, in_addr_t address)
{
	memset(&storage, 0, sizeof(storage));
	sockaddr_in& in = (sockaddr_in&)storage;
	in.sin_len = sizeof(sockaddr_in);
	in.sin_family = AF_INET;
	in.sin_addr.s_addr = address;
}


static int
add_loop_alias(in_addr_t address)
{
	int fd = socket(AF_INET, SOCK_DGRAM, 0);
	if (fd < 0)
		return errno;

	struct ifaliasreq request;
	memset(&request, 0, sizeof(request));
	strlcpy(request.ifra_name, "loop", IF_NAMESIZE);
	request.ifra_index = -1;
	fill_inet(request.ifra_addr, address);
	fill_inet(request.ifra_mask, htonl(0xffffffff));
	int error = ioctl(fd, SIOCAIFADDR, &request, sizeof(request)) == 0
		? 0 : errno;
	close(fd);
	return error;
}


static int
remove_loop_alias(in_addr_t address)
{
	int fd = socket(AF_INET, SOCK_DGRAM, 0);
	if (fd < 0)
		return errno;

	struct ifreq request;
	memset(&request, 0, sizeof(request));
	strlcpy(request.ifr_name, "loop", IF_NAMESIZE);
	sockaddr_in& in = (sockaddr_in&)request.ifr_addr;
	in.sin_len = sizeof(sockaddr_in);
	in.sin_family = AF_INET;
	in.sin_addr.s_addr = address;
	int error = ioctl(fd, SIOCDIFADDR, &request, sizeof(request)) == 0
		? 0 : errno;
	close(fd);
	return error;
}


/*!	Adds or removes an RTF_LOCAL host route for \a address on "loop". */
static int
loop_local_route(in_addr_t address, bool add)
{
	int fd = socket(AF_INET, SOCK_DGRAM, 0);
	if (fd < 0)
		return errno;

	sockaddr_in destination;
	memset(&destination, 0, sizeof(destination));
	destination.sin_len = sizeof(destination);
	destination.sin_family = AF_INET;
	destination.sin_addr.s_addr = address;

	struct ifreq request;
	memset(&request, 0, sizeof(request));
	strlcpy(request.ifr_name, "loop", IF_NAMESIZE);
	request.ifr_route.destination = (sockaddr*)&destination;
	request.ifr_route.flags = RTF_LOCAL | RTF_HOST;
	int error = ioctl(fd, add ? SIOCADDRT : SIOCDELRT, &request,
		sizeof(request)) == 0 ? 0 : errno;
	close(fd);
	return error;
}


static void
test_options()
{
	int fd = socket(AF_INET, SOCK_STREAM, 0);

	int idle = get_int(fd, IPPROTO_TCP, TCP_KEEPIDLE);
	int interval = get_int(fd, IPPROTO_TCP, TCP_KEEPINTVL);
	int count = get_int(fd, IPPROTO_TCP, TCP_KEEPCNT);
	report("defaults", idle == 7200 && interval == 75 && count == 9,
		"idle=%d intvl=%d cnt=%d (want 7200/75/9)", idle, interval, count);

	int e1 = set_int(fd, IPPROTO_TCP, TCP_KEEPIDLE, 30);
	int e2 = set_int(fd, IPPROTO_TCP, TCP_KEEPINTVL, 5);
	int e3 = set_int(fd, IPPROTO_TCP, TCP_KEEPCNT, 4);
	idle = get_int(fd, IPPROTO_TCP, TCP_KEEPIDLE);
	interval = get_int(fd, IPPROTO_TCP, TCP_KEEPINTVL);
	count = get_int(fd, IPPROTO_TCP, TCP_KEEPCNT);
	report("set_get", e1 == 0 && e2 == 0 && e3 == 0 && idle == 30
			&& interval == 5 && count == 4,
		"errors %d/%d/%d, read back %d/%d/%d", e1, e2, e3, idle, interval,
		count);

	int r1 = set_int(fd, IPPROTO_TCP, TCP_KEEPIDLE, 0);
	int r2 = set_int(fd, IPPROTO_TCP, TCP_KEEPIDLE, 32768);
	int r3 = set_int(fd, IPPROTO_TCP, TCP_KEEPINTVL, -1);
	int r4 = set_int(fd, IPPROTO_TCP, TCP_KEEPCNT, 0);
	int r5 = set_int(fd, IPPROTO_TCP, TCP_KEEPCNT, 128);
	report("range", r1 == EINVAL && r2 == EINVAL && r3 == EINVAL
			&& r4 == EINVAL && r5 == EINVAL
			&& get_int(fd, IPPROTO_TCP, TCP_KEEPIDLE) == 30,
		"errors %d/%d/%d/%d/%d (want EINVAL=%d), idle unchanged", r1, r2,
		r3, r4, r5, EINVAL);

	int maxIdle = set_int(fd, IPPROTO_TCP, TCP_KEEPIDLE, 32767);
	int maxCount = set_int(fd, IPPROTO_TCP, TCP_KEEPCNT, 127);
	report("limits", maxIdle == 0 && maxCount == 0,
		"idle 32767 -> %d, cnt 127 -> %d", maxIdle, maxCount);

	// Toggling SO_KEEPALIVE on an unconnected socket must be harmless.
	int on = set_int(fd, SOL_SOCKET, SO_KEEPALIVE, 1);
	int value = get_int(fd, SOL_SOCKET, SO_KEEPALIVE);
	int off = set_int(fd, SOL_SOCKET, SO_KEEPALIVE, 0);
	report("so_keepalive_unconnected", on == 0 && off == 0 && value != 0,
		"on=%d value=%d off=%d", on, value, off);

	close(fd);
}


static void
test_live_connection()
{
	int client, server;
	if (!make_pair(client, server, true)) {
		report("live_setup", false, "could not connect: %s",
			strerror(errno));
		return;
	}

	int inherited = get_int(server, SOL_SOCKET, SO_KEEPALIVE);
	int idle = get_int(server, IPPROTO_TCP, TCP_KEEPIDLE);
	int interval = get_int(server, IPPROTO_TCP, TCP_KEEPINTVL);
	int count = get_int(server, IPPROTO_TCP, TCP_KEEPCNT);
	report("accept_inherits", inherited != 0 && idle == 1 && interval == 1
			&& count == 2,
		"SO_KEEPALIVE=%d idle=%d intvl=%d cnt=%d (want on/1/1/2)",
		inherited, idle, interval, count);

	set_fast_keepalive(client);

	// Without answers both sides would give up after 1 + 2 * 1 = 3 s.
	// Six idle seconds means several rounds of probes were answered.
	snooze(6000000);

	int clientError = get_int(client, SOL_SOCKET, SO_ERROR);
	int serverError = get_int(server, SOL_SOCKET, SO_ERROR);
	char buffer[8] = {};
	set_receive_timeout(server, 2000000);
	set_receive_timeout(client, 2000000);
	ssize_t sent = send(client, "ping", 4, 0);
	ssize_t received = recv(server, buffer, sizeof(buffer), 0);
	ssize_t sentBack = send(server, "pong", 4, 0);
	ssize_t receivedBack = recv(client, buffer + 4, 4, 0);
	report("idle_survives", clientError == 0 && serverError == 0
			&& sent == 4 && received == 4 && sentBack == 4
			&& receivedBack == 4 && memcmp(buffer, "pingpong", 8) == 0,
		"after 6 s idle: SO_ERROR %d/%d, transfer %zd/%zd/%zd/%zd",
		clientError, serverError, sent, received, sentBack, receivedBack);

	// Switching keepalive off must stop the timer cleanly.
	int off = set_int(client, SOL_SOCKET, SO_KEEPALIVE, 0);
	report("disable_connected", off == 0
			&& get_int(client, SOL_SOCKET, SO_KEEPALIVE) == 0,
		"setsockopt(SO_KEEPALIVE, 0) -> %d", off);

	close(client);
	close(server);
}


static void
test_dead_peer()
{
	int added = add_loop_alias(kAliasAddress);
	if (added != 0) {
		report("alias_add", false, "SIOCAIFADDR 127.0.0.77 on loop: %s",
			strerror(added));
		return;
	}
	int routed = loop_local_route(kAliasAddress, true);
	if (routed != 0) {
		report("alias_route", false, "SIOCADDRT 127.0.0.77 local: %s",
			strerror(routed));
		remove_loop_alias(kAliasAddress);
		return;
	}

	int client, server, plainClient, plainServer;
	if (!make_pair(client, server, false, kAliasAddress)
		|| !make_pair(plainClient, plainServer, false, kAliasAddress)) {
		report("dead_setup", false, "could not connect to 127.0.0.77: %s",
			strerror(errno));
		loop_local_route(kAliasAddress, false);
		remove_loop_alias(kAliasAddress);
		return;
	}
	set_fast_keepalive(client);

	// From here on nothing answers for 127.0.0.77: segments for it still
	// reach IPv4 through the local route, but are no longer for us (the
	// ICMP host unreachable that results is ignored by TCP, as it should
	// be for a transient condition).
	int removed = remove_loop_alias(kAliasAddress);
	report("alias_remove", removed == 0, "SIOCDIFADDR 127.0.0.77: %s",
		strerror(removed));

	// The keepalive side must notice within idle + count * interval
	// (3 s); allow up to 10 s for a slow emulator.
	set_receive_timeout(client, 10000000);
	char byte;
	bigtime_t start = system_time();
	ssize_t result = recv(client, &byte, 1, 0);
	int error = errno;
	bigtime_t elapsed = system_time() - start;
	int socketError = get_int(client, SOL_SOCKET, SO_ERROR);

	// The side without keepalive must still be quiet.
	set_receive_timeout(plainClient, 500000);
	ssize_t plainResult = recv(plainClient, &byte, 1, 0);
	int plainError = errno;
	int plainSocketError = get_int(plainClient, SOL_SOCKET, SO_ERROR);

	report("dead_peer_timeout", result < 0 && error == ETIMEDOUT
			&& elapsed >= 2000000 && elapsed <= 10000000,
		"recv -> %zd (%s) after %" B_PRId64 " ms (want ETIMEDOUT in 2-10 s)",
		result, result < 0 ? strerror(error) : "data", elapsed / 1000);
	// Whether SO_ERROR still holds the error after recv() reported it
	// differs between systems; it must not be anything else.
	report("dead_peer_so_error", socketError == 0
			|| socketError == ETIMEDOUT,
		"SO_ERROR after recv: %d (%s)", socketError, strerror(socketError));
	report("no_keepalive_untouched", plainResult < 0
			&& (plainError == EAGAIN || plainError == EWOULDBLOCK)
			&& plainSocketError == 0,
		"recv -> %zd (%s), SO_ERROR %d", plainResult,
		plainResult < 0 ? strerror(plainError) : "data", plainSocketError);

	// A dead connection must refuse further writes.
	ssize_t sent = send(client, "x", 1, MSG_NOSIGNAL);
	int sendError = errno;
	report("dead_peer_send_fails", sent < 0,
		"send after timeout -> %zd (%s)", sent,
		sent < 0 ? strerror(sendError) : "accepted");

	close(client);
	close(server);
	close(plainClient);
	close(plainServer);

	int unrouted = loop_local_route(kAliasAddress, false);
	report("alias_cleanup", unrouted == 0, "SIOCDELRT 127.0.0.77: %s",
		strerror(unrouted));
}


int
main()
{
	test_options();
	test_live_connection();
	test_dead_peer();

	printf("CE-SUITE " SUITE " DONE failures=%d\n", sFailures);
	fflush(stdout);
	return sFailures == 0 ? 0 : 1;
}
