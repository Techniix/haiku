/*
 * Copyright 2026, Renguin contributors.
 * Distributed under the terms of the MIT License.
 *
 * Linux abstract AF_UNIX names under Renguin (static glibc program), as
 * D-Bus (unix:abstract=), X11 (@/tmp/.X11-unix/X0) and many daemons use
 * them. One line per part that worked, then "ok".
 *
 *  stream      bind/listen on "\0renguin-<pid>", getsockname() returns the
 *              same bytes and length, a client connects and both directions
 *              carry data, getpeername() on the client names the server
 *  inuse       a second bind() of the name is EADDRINUSE; after close() the
 *              name is free again (no file is left behind)
 *  missing     connect() to a name nobody bound is ECONNREFUSED
 *  nulls       names that differ only after an embedded null byte, or by
 *              length, are different names
 *  autobind    bind() with only the family picks "\0" + 5 hex digits
 *              (address length 8), as Linux
 *  dgram       SOCK_DGRAM to an abstract name, and recvfrom() reports the
 *              sender's abstract name
 */

#define _GNU_SOURCE
#include <errno.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>


#define CHECK(condition, ...) \
	do { \
		if (!(condition)) { \
			printf("FAIL %s:%d: ", __func__, __LINE__); \
			printf(__VA_ARGS__); \
			printf(" (errno %d %s)\n", errno, strerror(errno)); \
			exit(1); \
		} \
	} while (0)


//! An abstract address for \a name of \a length bytes (after the null byte).
static socklen_t
make_address(struct sockaddr_un* address, const char* name, size_t length)
{
	memset(address, 0, sizeof(*address));
	address->sun_family = AF_UNIX;
	address->sun_path[0] = '\0';
	memcpy(address->sun_path + 1, name, length);
	return offsetof(struct sockaddr_un, sun_path) + 1 + length;
}


static void
test_stream(const char* name)
{
	struct sockaddr_un address;
	socklen_t length = make_address(&address, name, strlen(name));
	int server = socket(AF_UNIX, SOCK_STREAM, 0);
	CHECK(server >= 0, "socket");
	CHECK(bind(server, (struct sockaddr*)&address, length) == 0, "bind");
	CHECK(listen(server, 4) == 0, "listen");

	struct sockaddr_un got;
	socklen_t gotLength = sizeof(got);
	CHECK(getsockname(server, (struct sockaddr*)&got, &gotLength) == 0,
		"getsockname");
	CHECK(gotLength == length && memcmp(&got, &address, length) == 0,
		"getsockname length %u (want %u)", (unsigned)gotLength,
		(unsigned)length);

	int client = socket(AF_UNIX, SOCK_STREAM, 0);
	CHECK(client >= 0, "socket");
	CHECK(connect(client, (struct sockaddr*)&address, length) == 0,
		"connect");
	int accepted = accept(server, NULL, NULL);
	CHECK(accepted >= 0, "accept");
	CHECK(write(client, "ping", 4) == 4, "write");
	char buffer[8];
	CHECK(read(accepted, buffer, sizeof(buffer)) == 4
		&& memcmp(buffer, "ping", 4) == 0, "read");
	CHECK(write(accepted, "pong", 4) == 4, "write back");
	CHECK(read(client, buffer, sizeof(buffer)) == 4
		&& memcmp(buffer, "pong", 4) == 0, "read back");

	gotLength = sizeof(got);
	CHECK(getpeername(client, (struct sockaddr*)&got, &gotLength) == 0
		&& gotLength == length && memcmp(&got, &address, length) == 0,
		"getpeername length %u", (unsigned)gotLength);
	close(accepted);
	close(client);

	// inuse: the name is taken while the server is open, free after
	int second = socket(AF_UNIX, SOCK_STREAM, 0);
	errno = 0;
	CHECK(bind(second, (struct sockaddr*)&address, length) == -1
		&& errno == EADDRINUSE, "second bind");
	close(server);
	CHECK(bind(second, (struct sockaddr*)&address, length) == 0,
		"bind after close");
	close(second);
	printf("stream\ninuse\n");
}


static void
test_missing(const char* name)
{
	struct sockaddr_un address;
	char missing[64];
	snprintf(missing, sizeof(missing), "%s-missing", name);
	socklen_t length = make_address(&address, missing, strlen(missing));
	int client = socket(AF_UNIX, SOCK_STREAM, 0);
	errno = 0;
	CHECK(connect(client, (struct sockaddr*)&address, length) == -1
		&& errno == ECONNREFUSED, "connect to a missing name");
	close(client);
	printf("missing\n");
}


static void
test_nulls(void)
{
	// "a\0b" and "a\0c" and "a" are three different names
	struct sockaddr_un first, second, third;
	socklen_t firstLength = make_address(&first, "rg\0b", 4);
	socklen_t secondLength = make_address(&second, "rg\0c", 4);
	socklen_t thirdLength = make_address(&third, "rg", 2);
	int a = socket(AF_UNIX, SOCK_STREAM, 0);
	int b = socket(AF_UNIX, SOCK_STREAM, 0);
	int c = socket(AF_UNIX, SOCK_STREAM, 0);
	CHECK(bind(a, (struct sockaddr*)&first, firstLength) == 0, "bind a");
	CHECK(bind(b, (struct sockaddr*)&second, secondLength) == 0, "bind b");
	CHECK(bind(c, (struct sockaddr*)&third, thirdLength) == 0, "bind c");
	CHECK(listen(b, 1) == 0, "listen");
	int client = socket(AF_UNIX, SOCK_STREAM, 0);
	CHECK(connect(client, (struct sockaddr*)&second, secondLength) == 0,
		"connect b");
	close(client);
	close(a);
	close(b);
	close(c);
	printf("nulls\n");
}


static void
test_autobind(void)
{
	int fd = socket(AF_UNIX, SOCK_DGRAM, 0);
	struct sockaddr_un address;
	memset(&address, 0, sizeof(address));
	address.sun_family = AF_UNIX;
	CHECK(bind(fd, (struct sockaddr*)&address, sizeof(sa_family_t)) == 0,
		"autobind");
	socklen_t length = sizeof(address);
	CHECK(getsockname(fd, (struct sockaddr*)&address, &length) == 0,
		"getsockname");
	CHECK(length == offsetof(struct sockaddr_un, sun_path) + 6
		&& address.sun_path[0] == '\0', "autobound length %u",
		(unsigned)length);
	for (int i = 1; i < 6; i++) {
		char c = address.sun_path[i];
		CHECK((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'),
			"autobound name byte %d is %#x", i, (unsigned char)c);
	}
	close(fd);
	printf("autobind\n");
}


static void
test_dgram(const char* name)
{
	char serverName[64], clientName[64];
	snprintf(serverName, sizeof(serverName), "%s-dgram-server", name);
	snprintf(clientName, sizeof(clientName), "%s-dgram-client", name);
	struct sockaddr_un serverAddress, clientAddress;
	socklen_t serverLength = make_address(&serverAddress, serverName,
		strlen(serverName));
	socklen_t clientLength = make_address(&clientAddress, clientName,
		strlen(clientName));
	int server = socket(AF_UNIX, SOCK_DGRAM, 0);
	int client = socket(AF_UNIX, SOCK_DGRAM, 0);
	CHECK(bind(server, (struct sockaddr*)&serverAddress, serverLength) == 0,
		"bind server");
	CHECK(bind(client, (struct sockaddr*)&clientAddress, clientLength) == 0,
		"bind client");
	CHECK(sendto(client, "hello", 5, 0, (struct sockaddr*)&serverAddress,
		serverLength) == 5, "sendto");
	char buffer[16];
	struct sockaddr_un from;
	socklen_t fromLength = sizeof(from);
	CHECK(recvfrom(server, buffer, sizeof(buffer), 0, (struct sockaddr*)&from,
		&fromLength) == 5 && memcmp(buffer, "hello", 5) == 0, "recvfrom");
	CHECK(fromLength == clientLength
		&& memcmp(&from, &clientAddress, clientLength) == 0,
		"sender address length %u (want %u)", (unsigned)fromLength,
		(unsigned)clientLength);
	close(server);
	close(client);
	printf("dgram\n");
}


int
main(void)
{
	setvbuf(stdout, NULL, _IONBF, 0);
	char name[48];
	snprintf(name, sizeof(name), "renguin-test-%d", (int)getpid());
	test_stream(name);
	test_missing(name);
	test_nulls();
	test_autobind();
	test_dgram(name);
	printf("ok\n");
	return 0;
}
