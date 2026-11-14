/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 *
 * Renguin test (F-27): Linux networking calls, linked dynamically against
 * musl and against glibc, which take different paths for some of them
 * (glibc's getifaddrs() and if_nameindex() read netlink dumps with
 * recvmsg(), musl's with recv(MSG_DONTWAIT); getaddrinfo() reads
 * /etc/hosts in both). Each line is "<check> ok" or "<check> FAIL <why>";
 * the last line is "ok" when everything passed.
 *
 *   tcp4       socket, setsockopt(SO_REUSEADDR), bind to 127.0.0.1:0,
 *              getsockname, listen, connect, accept4(SOCK_CLOEXEC), send,
 *              recv, getpeername, shutdown (EOF seen), SO_ERROR, SO_TYPE,
 *              SO_DOMAIN, TCP_NODELAY round trip
 *   tcp6       the same over ::1, with IPV6_V6ONLY
 *   udp4       sendto/recvfrom with the sender's address, MSG_PEEK,
 *              sendmsg/recvmsg with an iovec pair, MSG_TRUNC length
 *   unix       a named AF_UNIX stream socket in /tmp: bind, listen,
 *              connect, accept, data both ways, getsockname's sun_path
 *   socketpair AF_UNIX SOCK_STREAM and SOCK_DGRAM pairs (boundaries kept)
 *   fdpass     SCM_RIGHTS: a pipe's read end sent over a socketpair, read
 *              through the received descriptor; MSG_CMSG_CLOEXEC sets
 *              FD_CLOEXEC on it
 *   timeouts   SO_RCVTIMEO: recv() gives EAGAIN after the timeout
 *   nonblock   SOCK_NONBLOCK: accept4() and recv() give EAGAIN
 *   gai        getaddrinfo("localhost") gives 127.0.0.1 (from /etc/hosts),
 *              getaddrinfo of a numeric IPv6 address, getnameinfo
 *   ifaddrs    getifaddrs() (netlink RTM_GETLINK + RTM_GETADDR) lists the
 *              loopback interface with 127.0.0.1 and IFF_UP|IFF_LOOPBACK
 *   ifindex    if_nametoindex("loop") or ("lo") > 0, if_nameindex() lists it
 *   route      a raw NETLINK_ROUTE RTM_GETROUTE dump ends with NLMSG_DONE
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>

/* Netlink, written out: musl's development files carry no <linux/...>
   headers (they are Alpine's linux-headers package). */
#define NETLINK_ROUTE_	0
#define RTM_NEWROUTE_	24
#define RTM_GETROUTE_	26
#define NLMSG_DONE_		3
#define NLM_F_REQUEST_	0x1
#define NLM_F_DUMP_		0x300
#ifndef AF_NETLINK
#	define AF_NETLINK	16
#endif

struct nl_header {
	uint32_t	len;
	uint16_t	type;
	uint16_t	flags;
	uint32_t	seq;
	uint32_t	pid;
};

struct nl_address {
	uint16_t	family;
	uint16_t	pad;
	uint32_t	pid;
	uint32_t	groups;
};

struct rt_message {
	uint8_t		family, dst_len, src_len, tos, table, protocol, scope, type;
	uint32_t	flags;
};

static int sFailures;

#define CHECK(name, cond, ...) do { \
		if (!(cond)) { \
			printf("%s FAIL ", name); \
			printf(__VA_ARGS__); \
			printf(" (errno %d)\n", errno); \
			sFailures++; \
			return; \
		} \
	} while (0)


static void
stream_pair_test(const char* name, int family)
{
	int server = socket(family, SOCK_STREAM, 0);
	CHECK(name, server >= 0, "socket");
	int one = 1;
	CHECK(name, setsockopt(server, SOL_SOCKET, SO_REUSEADDR, &one,
		sizeof(one)) == 0, "SO_REUSEADDR");
	struct sockaddr_storage address;
	memset(&address, 0, sizeof(address));
	socklen_t length;
	if (family == AF_INET) {
		struct sockaddr_in* in = (struct sockaddr_in*)&address;
		in->sin_family = AF_INET;
		in->sin_addr.s_addr = htonl(INADDR_LOOPBACK);
		length = sizeof(*in);
	} else {
		struct sockaddr_in6* in6 = (struct sockaddr_in6*)&address;
		in6->sin6_family = AF_INET6;
		in6->sin6_addr = in6addr_loopback;
		length = sizeof(*in6);
		CHECK(name, setsockopt(server, IPPROTO_IPV6, IPV6_V6ONLY, &one,
			sizeof(one)) == 0, "IPV6_V6ONLY");
	}
	CHECK(name, bind(server, (struct sockaddr*)&address, length) == 0,
		"bind");
	socklen_t gotLength = sizeof(address);
	CHECK(name, getsockname(server, (struct sockaddr*)&address, &gotLength)
		== 0 && gotLength == length && address.ss_family == family,
		"getsockname length %d family %d", (int)gotLength,
		(int)address.ss_family);
	int port = family == AF_INET
		? ntohs(((struct sockaddr_in*)&address)->sin_port)
		: ntohs(((struct sockaddr_in6*)&address)->sin6_port);
	CHECK(name, port != 0, "port 0");
	CHECK(name, listen(server, 4) == 0, "listen");

	int value = 0;
	socklen_t valueLength = sizeof(value);
	CHECK(name, getsockopt(server, SOL_SOCKET, SO_ACCEPTCONN, &value,
		&valueLength) == 0 && value == 1, "SO_ACCEPTCONN %d", value);

	int client = socket(family, SOCK_STREAM | SOCK_CLOEXEC, 0);
	CHECK(name, client >= 0 && (fcntl(client, F_GETFD) & FD_CLOEXEC) != 0,
		"SOCK_CLOEXEC");
	CHECK(name, connect(client, (struct sockaddr*)&address, length) == 0,
		"connect");
	struct sockaddr_storage peer;
	socklen_t peerLength = sizeof(peer);
	int accepted = accept4(server, (struct sockaddr*)&peer, &peerLength,
		SOCK_CLOEXEC);
	CHECK(name, accepted >= 0 && peer.ss_family == family
		&& peerLength == length, "accept4 family %d length %d",
		(int)peer.ss_family, (int)peerLength);
	CHECK(name, (fcntl(accepted, F_GETFD) & FD_CLOEXEC) != 0,
		"accept4 SOCK_CLOEXEC");

	CHECK(name, send(client, "ping", 4, 0) == 4, "send");
	char buffer[16];
	CHECK(name, recv(accepted, buffer, sizeof(buffer), 0) == 4
		&& memcmp(buffer, "ping", 4) == 0, "recv");
	CHECK(name, write(accepted, "pong!", 5) == 5, "write");
	CHECK(name, read(client, buffer, sizeof(buffer)) == 5
		&& memcmp(buffer, "pong!", 5) == 0, "read");

	peerLength = sizeof(peer);
	CHECK(name, getpeername(client, (struct sockaddr*)&peer, &peerLength)
		== 0 && peer.ss_family == family, "getpeername");

	value = 0;
	valueLength = sizeof(value);
	CHECK(name, getsockopt(client, SOL_SOCKET, SO_TYPE, &value,
		&valueLength) == 0 && value == SOCK_STREAM, "SO_TYPE %d", value);
	value = -1;
	CHECK(name, getsockopt(client, SOL_SOCKET, SO_ERROR, &value,
		&valueLength) == 0 && value == 0, "SO_ERROR %d", value);
#ifdef SO_DOMAIN
	value = 0;
	CHECK(name, getsockopt(client, SOL_SOCKET, SO_DOMAIN, &value,
		&valueLength) == 0 && value == family, "SO_DOMAIN %d", value);
#endif
	CHECK(name, setsockopt(client, IPPROTO_TCP, TCP_NODELAY, &one,
		sizeof(one)) == 0, "TCP_NODELAY set");
	value = 0;
	CHECK(name, getsockopt(client, IPPROTO_TCP, TCP_NODELAY, &value,
		&valueLength) == 0 && value != 0, "TCP_NODELAY get %d", value);
	value = 0;
	CHECK(name, getsockopt(client, SOL_SOCKET, SO_RCVBUF, &value,
		&valueLength) == 0 && value > 0, "SO_RCVBUF %d", value);

	CHECK(name, shutdown(client, SHUT_WR) == 0, "shutdown");
	CHECK(name, recv(accepted, buffer, sizeof(buffer), 0) == 0, "EOF");

	close(client);
	close(accepted);
	close(server);
	printf("%s ok\n", name);
}


static void
udp_test(void)
{
	const char* name = "udp4";
	int a = socket(AF_INET, SOCK_DGRAM, 0);
	int b = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	CHECK(name, a >= 0 && b >= 0, "socket");
	struct sockaddr_in address;
	memset(&address, 0, sizeof(address));
	address.sin_family = AF_INET;
	address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	CHECK(name, bind(a, (struct sockaddr*)&address, sizeof(address)) == 0,
		"bind a");
	CHECK(name, bind(b, (struct sockaddr*)&address, sizeof(address)) == 0,
		"bind b");
	struct sockaddr_in aAddress, bAddress;
	socklen_t length = sizeof(aAddress);
	getsockname(a, (struct sockaddr*)&aAddress, &length);
	length = sizeof(bAddress);
	getsockname(b, (struct sockaddr*)&bAddress, &length);

	CHECK(name, sendto(b, "datagram", 8, 0, (struct sockaddr*)&aAddress,
		sizeof(aAddress)) == 8, "sendto");
	char buffer[32];
	CHECK(name, recv(a, buffer, sizeof(buffer), MSG_PEEK) == 8, "MSG_PEEK");
	struct sockaddr_in from;
	socklen_t fromLength = sizeof(from);
	CHECK(name, recvfrom(a, buffer, sizeof(buffer), 0,
		(struct sockaddr*)&from, &fromLength) == 8
		&& fromLength == sizeof(from) && from.sin_family == AF_INET
		&& from.sin_port == bAddress.sin_port, "recvfrom length %d family %d",
		(int)fromLength, (int)from.sin_family);

	struct iovec out[2] = { { (void*)"ab", 2 }, { (void*)"cdef", 4 } };
	struct msghdr message;
	memset(&message, 0, sizeof(message));
	message.msg_name = &aAddress;
	message.msg_namelen = sizeof(aAddress);
	message.msg_iov = out;
	message.msg_iovlen = 2;
	CHECK(name, sendmsg(b, &message, 0) == 6, "sendmsg");

	char first[3], second[2];
	struct iovec in[2] = { { first, 3 }, { second, 2 } };
	memset(&message, 0, sizeof(message));
	message.msg_name = &from;
	message.msg_namelen = sizeof(from);
	message.msg_iov = in;
	message.msg_iovlen = 2;
	ssize_t got = recvmsg(a, &message, 0);
	CHECK(name, got == 5 && memcmp(first, "abc", 3) == 0
		&& memcmp(second, "de", 2) == 0
		&& (message.msg_flags & MSG_TRUNC) != 0
		&& message.msg_namelen == sizeof(from), "recvmsg %d flags %#x",
		(int)got, message.msg_flags);

	close(a);
	close(b);
	printf("%s ok\n", name);
}


static void
unix_test(void)
{
	const char* name = "unix";
	struct sockaddr_un address;
	memset(&address, 0, sizeof(address));
	address.sun_family = AF_UNIX;
	snprintf(address.sun_path, sizeof(address.sun_path),
		"/tmp/renguin-net-%d.sock", (int)getpid());
	unlink(address.sun_path);

	int server = socket(AF_UNIX, SOCK_STREAM, 0);
	CHECK(name, server >= 0, "socket");
	CHECK(name, bind(server, (struct sockaddr*)&address, sizeof(address))
		== 0, "bind %s", address.sun_path);
	CHECK(name, access(address.sun_path, F_OK) == 0, "no socket file");
	CHECK(name, listen(server, 2) == 0, "listen");
	struct sockaddr_un got;
	socklen_t length = sizeof(got);
	CHECK(name, getsockname(server, (struct sockaddr*)&got, &length) == 0
		&& got.sun_family == AF_UNIX
		&& strcmp(got.sun_path, address.sun_path) == 0,
		"getsockname [%s]", got.sun_path);

	int client = socket(AF_UNIX, SOCK_STREAM, 0);
	CHECK(name, connect(client, (struct sockaddr*)&address,
		offsetof(struct sockaddr_un, sun_path) + strlen(address.sun_path)
			+ 1) == 0, "connect");
	int accepted = accept(server, NULL, NULL);
	CHECK(name, accepted >= 0, "accept");
	char buffer[8];
	CHECK(name, write(client, "abc", 3) == 3
		&& read(accepted, buffer, sizeof(buffer)) == 3, "client->server");
	CHECK(name, write(accepted, "xy", 2) == 2
		&& read(client, buffer, sizeof(buffer)) == 2, "server->client");
	struct ucred credentials;
	length = sizeof(credentials);
	CHECK(name, getsockopt(accepted, SOL_SOCKET, SO_PEERCRED, &credentials,
		&length) == 0 && credentials.pid == getpid(), "SO_PEERCRED pid %d",
		(int)credentials.pid);
	close(client);
	close(accepted);
	close(server);
	unlink(address.sun_path);
	printf("%s ok\n", name);
}


static void
socketpair_test(void)
{
	const char* name = "socketpair";
	int fds[2];
	CHECK(name, socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0, "stream");
	char buffer[8];
	CHECK(name, write(fds[0], "1234", 4) == 4
		&& read(fds[1], buffer, 8) == 4, "stream data");
	close(fds[0]);
	close(fds[1]);
	CHECK(name, socketpair(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0, fds) == 0,
		"dgram");
	CHECK(name, send(fds[0], "a", 1, 0) == 1 && send(fds[0], "bc", 2, 0) == 2,
		"dgram send");
	CHECK(name, recv(fds[1], buffer, 8, 0) == 1
		&& recv(fds[1], buffer, 8, 0) == 2, "dgram boundaries");
	close(fds[0]);
	close(fds[1]);
	printf("%s ok\n", name);
}


static void
fdpass_test(void)
{
	const char* name = "fdpass";
	int pair[2], pipeFds[2];
	CHECK(name, socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0, "socketpair");
	CHECK(name, pipe(pipeFds) == 0, "pipe");

	char control[CMSG_SPACE(sizeof(int))];
	memset(control, 0, sizeof(control));
	struct iovec iov = { (void*)"F", 1 };
	struct msghdr message;
	memset(&message, 0, sizeof(message));
	message.msg_iov = &iov;
	message.msg_iovlen = 1;
	message.msg_control = control;
	message.msg_controllen = sizeof(control);
	struct cmsghdr* header = CMSG_FIRSTHDR(&message);
	header->cmsg_level = SOL_SOCKET;
	header->cmsg_type = SCM_RIGHTS;
	header->cmsg_len = CMSG_LEN(sizeof(int));
	memcpy(CMSG_DATA(header), &pipeFds[0], sizeof(int));
	CHECK(name, sendmsg(pair[0], &message, 0) == 1, "sendmsg");
	close(pipeFds[0]);

	char byte;
	char received[CMSG_SPACE(sizeof(int)) + 16];
	memset(received, 0, sizeof(received));
	iov.iov_base = &byte;
	iov.iov_len = 1;
	memset(&message, 0, sizeof(message));
	message.msg_iov = &iov;
	message.msg_iovlen = 1;
	message.msg_control = received;
	message.msg_controllen = sizeof(received);
	CHECK(name, recvmsg(pair[1], &message, MSG_CMSG_CLOEXEC) == 1
		&& byte == 'F', "recvmsg");
	header = CMSG_FIRSTHDR(&message);
	CHECK(name, header != NULL && header->cmsg_level == SOL_SOCKET
		&& header->cmsg_type == SCM_RIGHTS
		&& header->cmsg_len == CMSG_LEN(sizeof(int)),
		"cmsg level %d type %d len %d", header ? header->cmsg_level : -1,
		header ? header->cmsg_type : -1, header ? (int)header->cmsg_len : -1);
	int fd;
	memcpy(&fd, CMSG_DATA(header), sizeof(int));
	CHECK(name, (fcntl(fd, F_GETFD) & FD_CLOEXEC) != 0, "MSG_CMSG_CLOEXEC");
	CHECK(name, write(pipeFds[1], "via", 3) == 3, "pipe write");
	char buffer[4];
	CHECK(name, read(fd, buffer, sizeof(buffer)) == 3
		&& memcmp(buffer, "via", 3) == 0, "read through passed fd");
	close(fd);

	// msg_controllen = CMSG_LEN(n), without the padding at the end (what
	// libwayland sends), is valid on Linux.
	int second[2];
	CHECK(name, pipe(second) == 0, "pipe 2");
	memset(control, 0, sizeof(control));
	iov.iov_base = (void*)"G";
	iov.iov_len = 1;
	memset(&message, 0, sizeof(message));
	message.msg_iov = &iov;
	message.msg_iovlen = 1;
	message.msg_control = control;
	message.msg_controllen = CMSG_LEN(sizeof(int));
	header = CMSG_FIRSTHDR(&message);
	header->cmsg_level = SOL_SOCKET;
	header->cmsg_type = SCM_RIGHTS;
	header->cmsg_len = CMSG_LEN(sizeof(int));
	memcpy(CMSG_DATA(header), &second[0], sizeof(int));
	CHECK(name, sendmsg(pair[0], &message, 0) == 1, "sendmsg unpadded");
	memset(received, 0, sizeof(received));
	iov.iov_base = &byte;
	memset(&message, 0, sizeof(message));
	message.msg_iov = &iov;
	message.msg_iovlen = 1;
	message.msg_control = received;
	message.msg_controllen = sizeof(received);
	CHECK(name, recvmsg(pair[1], &message, 0) == 1 && byte == 'G'
		&& CMSG_FIRSTHDR(&message) != NULL, "recvmsg unpadded");
	memcpy(&fd, CMSG_DATA(CMSG_FIRSTHDR(&message)), sizeof(int));
	close(fd);
	close(second[0]);
	close(second[1]);

	close(pipeFds[1]);
	close(pair[0]);
	close(pair[1]);
	printf("%s ok\n", name);
}


static void
timeout_test(void)
{
	const char* name = "timeouts";
	int fds[2];
	CHECK(name, socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0, "socketpair");
	struct timeval timeout = { 0, 100000 };
	CHECK(name, setsockopt(fds[0], SOL_SOCKET, SO_RCVTIMEO, &timeout,
		sizeof(timeout)) == 0, "SO_RCVTIMEO");
	struct timeval got;
	socklen_t length = sizeof(got);
	CHECK(name, getsockopt(fds[0], SOL_SOCKET, SO_RCVTIMEO, &got, &length)
		== 0 && got.tv_sec == 0 && got.tv_usec >= 90000
		&& got.tv_usec <= 110000, "get %ld.%06ld", (long)got.tv_sec,
		(long)got.tv_usec);
	char byte;
	errno = 0;
	CHECK(name, recv(fds[0], &byte, 1, 0) == -1
		&& (errno == EAGAIN || errno == EWOULDBLOCK), "recv after timeout");
	close(fds[0]);
	close(fds[1]);
	printf("%s ok\n", name);
}


static void
nonblock_test(void)
{
	const char* name = "nonblock";
	int server = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
	CHECK(name, server >= 0 && (fcntl(server, F_GETFL) & O_NONBLOCK) != 0,
		"SOCK_NONBLOCK");
	struct sockaddr_in address;
	memset(&address, 0, sizeof(address));
	address.sin_family = AF_INET;
	address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	CHECK(name, bind(server, (struct sockaddr*)&address, sizeof(address)) == 0
		&& listen(server, 1) == 0, "bind/listen");
	errno = 0;
	CHECK(name, accept4(server, NULL, NULL, 0) == -1 && errno == EAGAIN,
		"accept4");
	int fds[2];
	CHECK(name, socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0, "socketpair");
	char byte;
	errno = 0;
	CHECK(name, recv(fds[0], &byte, 1, MSG_DONTWAIT) == -1
		&& errno == EAGAIN, "MSG_DONTWAIT");
	close(fds[0]);
	close(fds[1]);
	close(server);
	printf("%s ok\n", name);
}


static void
gai_test(void)
{
	const char* name = "gai";
	struct addrinfo hints, *result = NULL;
	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_INET;
	hints.ai_socktype = SOCK_STREAM;
	int error = getaddrinfo("localhost", "80", &hints, &result);
	CHECK(name, error == 0 && result != NULL, "localhost: %s",
		gai_strerror(error));
	struct sockaddr_in* in = (struct sockaddr_in*)result->ai_addr;
	CHECK(name, in->sin_family == AF_INET
		&& in->sin_addr.s_addr == htonl(INADDR_LOOPBACK)
		&& ntohs(in->sin_port) == 80, "localhost address");
	freeaddrinfo(result);

	hints.ai_family = AF_UNSPEC;
	hints.ai_flags = AI_NUMERICHOST;
	result = NULL;
	error = getaddrinfo("::1", NULL, &hints, &result);
	CHECK(name, error == 0 && result != NULL
		&& result->ai_family == AF_INET6, "::1: %s", gai_strerror(error));
	char host[64];
	error = getnameinfo(result->ai_addr, result->ai_addrlen, host,
		sizeof(host), NULL, 0, NI_NUMERICHOST);
	CHECK(name, error == 0 && strcmp(host, "::1") == 0, "getnameinfo [%s]",
		host);
	freeaddrinfo(result);
	printf("%s ok\n", name);
}


static void
ifaddrs_test(void)
{
	const char* name = "ifaddrs";
	struct ifaddrs* list = NULL;
	CHECK(name, getifaddrs(&list) == 0, "getifaddrs");
	int loopback = 0, entries = 0;
	for (struct ifaddrs* entry = list; entry != NULL;
			entry = entry->ifa_next) {
		entries++;
		if (entry->ifa_addr != NULL && entry->ifa_addr->sa_family == AF_INET
			&& ((struct sockaddr_in*)entry->ifa_addr)->sin_addr.s_addr
				== htonl(INADDR_LOOPBACK)
			&& (entry->ifa_flags & (IFF_UP | IFF_LOOPBACK))
				== (IFF_UP | IFF_LOOPBACK)) {
			loopback++;
		}
	}
	freeifaddrs(list);
	CHECK(name, loopback == 1, "%d entries, %d loopback", entries, loopback);
	printf("%s ok\n", name);
}


static void
ifindex_test(void)
{
	const char* name = "ifindex";
	unsigned index = if_nametoindex("loop");
	const char* loopName = "loop";
	if (index == 0) {
		index = if_nametoindex("lo");
		loopName = "lo";
	}
	CHECK(name, index > 0, "if_nametoindex");
	struct if_nameindex* list = if_nameindex();
	CHECK(name, list != NULL, "if_nameindex");
	int found = 0;
	for (struct if_nameindex* entry = list; entry->if_index != 0; entry++) {
		if (entry->if_index == index && strcmp(entry->if_name, loopName) == 0)
			found = 1;
	}
	if_freenameindex(list);
	CHECK(name, found, "%s (%u) not listed", loopName, index);
	char buffer[IF_NAMESIZE];
	CHECK(name, if_indextoname(index, buffer) != NULL
		&& strcmp(buffer, loopName) == 0, "if_indextoname");
	printf("%s ok\n", name);
}


static void
route_test(void)
{
	const char* name = "route";
	int fd = socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, NETLINK_ROUTE_);
	CHECK(name, fd >= 0, "socket");
	struct nl_address local;
	memset(&local, 0, sizeof(local));
	local.family = AF_NETLINK;
	CHECK(name, bind(fd, (struct sockaddr*)&local, sizeof(local)) == 0,
		"bind");
	socklen_t length = sizeof(local);
	CHECK(name, getsockname(fd, (struct sockaddr*)&local, &length) == 0
		&& local.family == AF_NETLINK && local.pid != 0,
		"getsockname pid %u", local.pid);

	struct {
		struct nl_header header;
		struct rt_message message;
	} request;
	memset(&request, 0, sizeof(request));
	request.header.len = sizeof(request);
	request.header.type = RTM_GETROUTE_;
	request.header.flags = NLM_F_REQUEST_ | NLM_F_DUMP_;
	request.header.seq = 77;
	request.message.family = AF_INET;
	struct nl_address kernel;
	memset(&kernel, 0, sizeof(kernel));
	kernel.family = AF_NETLINK;
	CHECK(name, sendto(fd, &request, sizeof(request), 0,
		(struct sockaddr*)&kernel, sizeof(kernel)) == sizeof(request),
		"sendto");

	int routes = 0, done = 0;
	char buffer[8192] __attribute__((aligned(8)));
	while (!done) {
		struct nl_address from;
		socklen_t fromLength = sizeof(from);
		ssize_t bytes = recvfrom(fd, buffer, sizeof(buffer), 0,
			(struct sockaddr*)&from, &fromLength);
		CHECK(name, bytes > 0 && from.family == AF_NETLINK
			&& from.pid == 0, "recvfrom %d", (int)bytes);
		size_t offset = 0;
		while (offset + sizeof(struct nl_header) <= (size_t)bytes) {
			struct nl_header* header = (struct nl_header*)(buffer + offset);
			CHECK(name, header->len >= sizeof(*header)
				&& offset + header->len <= (size_t)bytes, "length %u",
				header->len);
			CHECK(name, header->seq == 77, "seq %u", header->seq);
			if (header->type == NLMSG_DONE_) {
				done = 1;
				break;
			}
			CHECK(name, header->type == RTM_NEWROUTE_, "type %d",
				header->type);
			routes++;
			offset += (header->len + 3) & ~3u;
		}
	}
	CHECK(name, routes > 0, "no routes");
	close(fd);
	printf("%s ok\n", name);
}


int
main(void)
{
	setvbuf(stdout, NULL, _IONBF, 0);
	stream_pair_test("tcp4", AF_INET);
	stream_pair_test("tcp6", AF_INET6);
	udp_test();
	unix_test();
	socketpair_test();
	fdpass_test();
	timeout_test();
	nonblock_test();
	gai_test();
	ifaddrs_test();
	ifindex_test();
	route_test();
	if (sFailures == 0)
		printf("ok\n");
	return sFailures != 0;
}
