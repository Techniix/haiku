/*
 * Copyright 2026, Renguin contributors.
 * Distributed under the terms of the MIT License.
 *
 * IPv4-mapped IPv6 addresses under Renguin (batch 11; static glibc
 * program). Linux AF_INET6 sockets reach IPv4 peers through ::ffff:a.b.c.d;
 * Renguin replaces such a socket by an IPv4 one and reports mapped
 * addresses. One line per part that worked, then "ok"; the first failure
 * prints what went wrong and exits with status 1.
 *
 *  tcp_connect   an AF_INET6 client connects to ::ffff:127.0.0.1 where an
 *                IPv4 server listens; data both ways; getpeername and
 *                getsockname report AF_INET6 ::ffff:127.0.0.1
 *  tcp_listen    an AF_INET6 server bound to ::ffff:127.0.0.1 accepts an
 *                IPv4 client; accept() reports the peer mapped
 *  udp           an AF_INET6 datagram socket sends to ::ffff:127.0.0.1 and
 *                recvfrom() reports the IPv4 sender mapped
 *  same_fd       the descriptor number, O_NONBLOCK and FD_CLOEXEC stay
 *  ipv6_still    a plain AF_INET6 socket still binds ::1 (not replaced)
 */

#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>


static void
fail(const char* what)
{
	printf("FAIL %s: %s\n", what, strerror(errno));
	exit(1);
}


static void
mapped(struct sockaddr_in6* address, int port)
{
	memset(address, 0, sizeof(*address));
	address->sin6_family = AF_INET6;
	address->sin6_port = htons(port);
	inet_pton(AF_INET6, "::ffff:127.0.0.1", &address->sin6_addr);
}


static int
is_mapped_loopback(const struct sockaddr_in6* address, socklen_t length)
{
	struct in6_addr expected;
	inet_pton(AF_INET6, "::ffff:127.0.0.1", &expected);
	return length == sizeof(*address) && address->sin6_family == AF_INET6
		&& memcmp(&address->sin6_addr, &expected, 16) == 0;
}


static int
ipv4_listener(int* port)
{
	int server = socket(AF_INET, SOCK_STREAM, 0);
	struct sockaddr_in address;
	memset(&address, 0, sizeof(address));
	address.sin_family = AF_INET;
	address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	socklen_t length = sizeof(address);
	if (server < 0 || bind(server, (struct sockaddr*)&address, length) != 0
		|| listen(server, 4) != 0
		|| getsockname(server, (struct sockaddr*)&address, &length) != 0)
		fail("ipv4 listener");
	*port = ntohs(address.sin_port);
	return server;
}


int
main(void)
{
	setvbuf(stdout, NULL, _IONBF, 0);

	// tcp_connect
	int port;
	int server = ipv4_listener(&port);
	int client = socket(AF_INET6, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (client < 0)
		fail("socket");
	int number = client;
	struct sockaddr_in6 target;
	mapped(&target, port);
	if (connect(client, (struct sockaddr*)&target, sizeof(target)) != 0)
		fail("tcp_connect connect");
	int peer = accept(server, NULL, NULL);
	if (peer < 0)
		fail("tcp_connect accept");
	char buffer[16];
	if (write(client, "ping", 4) != 4 || read(peer, buffer, 4) != 4
		|| write(peer, "pong", 4) != 4 || read(client, buffer, 4) != 4
		|| memcmp(buffer, "pong", 4) != 0)
		fail("tcp_connect data");
	struct sockaddr_in6 name;
	socklen_t length = sizeof(name);
	if (getpeername(client, (struct sockaddr*)&name, &length) != 0
		|| !is_mapped_loopback(&name, length) || ntohs(name.sin6_port) != port) {
		errno = EINVAL;
		fail("tcp_connect getpeername");
	}
	length = sizeof(name);
	if (getsockname(client, (struct sockaddr*)&name, &length) != 0
		|| !is_mapped_loopback(&name, length)) {
		errno = EINVAL;
		fail("tcp_connect getsockname");
	}
	printf("tcp_connect\n");

	// same_fd: the number and the close-on-exec flag stayed
	int flags = fcntl(client, F_GETFD);
	if (client != number || flags < 0 || (flags & FD_CLOEXEC) == 0) {
		errno = EINVAL;
		fail("same_fd");
	}
	close(peer);
	close(client);
	close(server);

	// tcp_listen
	server = socket(AF_INET6, SOCK_STREAM, 0);
	int on = 1;
	setsockopt(server, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
	struct sockaddr_in6 local;
	mapped(&local, 0);
	length = sizeof(local);
	if (bind(server, (struct sockaddr*)&local, sizeof(local)) != 0
		|| listen(server, 4) != 0
		|| getsockname(server, (struct sockaddr*)&local, &length) != 0
		|| !is_mapped_loopback(&local, length))
		fail("tcp_listen bind");
	client = socket(AF_INET, SOCK_STREAM, 0);
	struct sockaddr_in four;
	memset(&four, 0, sizeof(four));
	four.sin_family = AF_INET;
	four.sin_port = local.sin6_port;
	four.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	if (connect(client, (struct sockaddr*)&four, sizeof(four)) != 0)
		fail("tcp_listen connect");
	length = sizeof(name);
	peer = accept(server, (struct sockaddr*)&name, &length);
	if (peer < 0 || !is_mapped_loopback(&name, length)) {
		if (peer >= 0)
			errno = EINVAL;
		fail("tcp_listen accept");
	}
	printf("tcp_listen\n");
	close(peer);
	close(client);
	close(server);

	// udp
	int receiver = socket(AF_INET, SOCK_DGRAM, 0);
	memset(&four, 0, sizeof(four));
	four.sin_family = AF_INET;
	four.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	length = sizeof(four);
	if (bind(receiver, (struct sockaddr*)&four, sizeof(four)) != 0
		|| getsockname(receiver, (struct sockaddr*)&four, &length) != 0)
		fail("udp receiver");
	int sender = socket(AF_INET6, SOCK_DGRAM | SOCK_NONBLOCK, 0);
	mapped(&target, ntohs(four.sin_port));
	if (sendto(sender, "hello", 5, 0, (struct sockaddr*)&target,
			sizeof(target)) != 5)
		fail("udp sendto");
	struct sockaddr_in from;
	length = sizeof(from);
	if (recvfrom(receiver, buffer, sizeof(buffer), 0, (struct sockaddr*)&from,
			&length) != 5)
		fail("udp recvfrom");
	// and back: the reply reaches the AF_INET6 socket, the sender mapped
	if (sendto(receiver, "back", 4, 0, (struct sockaddr*)&from, length) != 4)
		fail("udp reply");
	usleep(100000);
	length = sizeof(name);
	if (recvfrom(sender, buffer, sizeof(buffer), 0, (struct sockaddr*)&name,
			&length) != 4 || !is_mapped_loopback(&name, length)) {
		fail("udp recvfrom mapped");
	}
	if ((fcntl(sender, F_GETFL) & O_NONBLOCK) == 0) {
		errno = EINVAL;
		fail("same_fd nonblock");
	}
	printf("udp\n");
	printf("same_fd\n");
	close(sender);
	close(receiver);

	// ipv6_still
	int six = socket(AF_INET6, SOCK_STREAM, 0);
	struct sockaddr_in6 loop;
	memset(&loop, 0, sizeof(loop));
	loop.sin6_family = AF_INET6;
	loop.sin6_addr = in6addr_loopback;
	length = sizeof(loop);
	if (bind(six, (struct sockaddr*)&loop, sizeof(loop)) != 0
		|| getsockname(six, (struct sockaddr*)&loop, &length) != 0
		|| loop.sin6_family != AF_INET6
		|| memcmp(&loop.sin6_addr, &in6addr_loopback, 16) != 0)
		fail("ipv6_still");
	printf("ipv6_still\n");
	close(six);

	printf("ok\n");
	return 0;
}
