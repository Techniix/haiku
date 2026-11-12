/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 */

/*!	SOCK_SEQPACKET UNIX sockets with descriptor passing (roadmap F-58,
	OBS Studio's O-13): what an IPC layer built on them relies on (OBS's
	browser helper, Chromium's and Mojo's channels, Wayland-style
	protocols). Linux semantics as unix(7) and socket(2) describe them:

	- message boundaries: each send is one message, each receive one
	  message; a receive with a smaller buffer gets the start of the
	  message with MSG_TRUNC set and the rest is discarded;
	- an empty message is a message (it is not end of file);
	- SCM_RIGHTS: descriptors travel with the message they were sent
	  with, several in one message, each a new descriptor in the receiver
	  that refers to the same open file (a memfd written by the receiver
	  is seen by the sender); MSG_CTRUNC when the receiver's control buffer
	  is too small, and the descriptors that did not fit are not leaked;
	- socketpair() and bind/listen/accept/connect on a path;
	- the peer's close: the next receive returns 0.

	Output format: see 3rdparty/ce/README.md.
*/


#include <sys/mman.h>

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <sys/un.h>
#include <unistd.h>


#define SUITE "seqpacket"


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


//!	Sends \a size bytes of \a data with the descriptors \a fds.
static ssize_t
send_with_fds(int socket, const void* data, size_t size, const int* fds,
	int count)
{
	char control[CMSG_SPACE(8 * sizeof(int))];
	memset(control, 0, sizeof(control));
	iovec iov = { (void*)data, size };
	msghdr message = {};
	message.msg_iov = &iov;
	message.msg_iovlen = 1;
	if (count > 0) {
		message.msg_control = control;
		message.msg_controllen = CMSG_SPACE(count * sizeof(int));
		cmsghdr* header = CMSG_FIRSTHDR(&message);
		header->cmsg_level = SOL_SOCKET;
		header->cmsg_type = SCM_RIGHTS;
		header->cmsg_len = CMSG_LEN(count * sizeof(int));
		memcpy(CMSG_DATA(header), fds, count * sizeof(int));
	}
	return sendmsg(socket, &message, 0);
}


/*!	Receives one message into \a buffer (\a size bytes) with room for
	\a room descriptors; \a fds gets those received (\a _count of them),
	\a _flags the message flags.
*/
static ssize_t
receive_with_fds(int socket, void* buffer, size_t size, int room, int* fds,
	int* _count, int* _flags)
{
	char control[CMSG_SPACE(8 * sizeof(int))];
	memset(control, 0, sizeof(control));
	iovec iov = { buffer, size };
	msghdr message = {};
	message.msg_iov = &iov;
	message.msg_iovlen = 1;
	if (room > 0) {
		message.msg_control = control;
		message.msg_controllen = CMSG_SPACE(room * sizeof(int));
	}
	ssize_t received = recvmsg(socket, &message, 0);
	*_count = 0;
	*_flags = message.msg_flags;
	if (received < 0)
		return received;
	for (cmsghdr* header = CMSG_FIRSTHDR(&message); header != NULL;
			header = CMSG_NXTHDR(&message, header)) {
		if (header->cmsg_level != SOL_SOCKET
			|| header->cmsg_type != SCM_RIGHTS)
			continue;
		int count = (header->cmsg_len - CMSG_LEN(0)) / sizeof(int);
		memcpy(fds + *_count, CMSG_DATA(header), count * sizeof(int));
		*_count += count;
	}
	return received;
}


static int
open_descriptors()
{
	int count = 0;
	for (int fd = 0; fd < 1024; fd++) {
		if (fcntl(fd, F_GETFD) >= 0)
			count++;
	}
	return count;
}


static void
test_boundaries()
{
	int pair[2];
	int result = socketpair(AF_UNIX, SOCK_SEQPACKET, 0, pair);
	report("socketpair", result == 0, "socketpair(AF_UNIX, SOCK_SEQPACKET): "
		"%s", result == 0 ? "ok" : strerror(errno));
	if (result != 0)
		return;

	int type = 0;
	socklen_t length = sizeof(type);
	getsockopt(pair[0], SOL_SOCKET, SO_TYPE, &type, &length);
	report("so_type", type == SOCK_SEQPACKET, "SO_TYPE %d (want %d)", type,
		SOCK_SEQPACKET);

	// Three messages: 10, 3000 and 1 bytes, then an empty one.
	char big[3000];
	for (size_t i = 0; i < sizeof(big); i++)
		big[i] = (char)(i * 7);
	ssize_t sent1 = send(pair[0], "0123456789", 10, 0);
	ssize_t sent2 = send(pair[0], big, sizeof(big), 0);
	ssize_t sent3 = send(pair[0], "!", 1, 0);
	ssize_t sent4 = send(pair[0], "", 0, 0);

	char buffer[8192];
	ssize_t got1 = recv(pair[1], buffer, sizeof(buffer), 0);
	bool first = got1 == 10 && memcmp(buffer, "0123456789", 10) == 0;
	ssize_t got2 = recv(pair[1], buffer, sizeof(buffer), 0);
	bool second = got2 == (ssize_t)sizeof(big)
		&& memcmp(buffer, big, sizeof(big)) == 0;
	ssize_t got3 = recv(pair[1], buffer, sizeof(buffer), 0);
	bool third = got3 == 1 && buffer[0] == '!';
	report("boundaries", sent1 == 10 && sent2 == 3000 && sent3 == 1 && first
			&& second && third,
		"sent %zd/%zd/%zd, received %zd/%zd/%zd (each one message)", sent1,
		sent2, sent3, got1, got2, got3);

	// The empty message is one, not the end of the stream.
	errno = 0;
	ssize_t got4 = recv(pair[1], buffer, sizeof(buffer), MSG_DONTWAIT);
	report("empty_message", sent4 == 0 && got4 == 0,
		"sent %zd, received %zd (%s)", sent4, got4,
		got4 < 0 ? strerror(errno) : "a message of 0 bytes");

	// A short buffer: the start, MSG_TRUNC, the rest dropped.
	send(pair[0], "abcdefghij", 10, 0);
	send(pair[0], "next", 4, 0);
	iovec iov = { buffer, 4 };
	msghdr message = {};
	message.msg_iov = &iov;
	message.msg_iovlen = 1;
	ssize_t truncated = recvmsg(pair[1], &message, 0);
	bool start = truncated == 4 && memcmp(buffer, "abcd", 4) == 0;
	bool flagged = (message.msg_flags & MSG_TRUNC) != 0;
	ssize_t next = recv(pair[1], buffer, sizeof(buffer), 0);
	report("truncation", start && flagged && next == 4
			&& memcmp(buffer, "next", 4) == 0,
		"4-byte buffer: %zd bytes \"%.4s\", MSG_TRUNC %s; then %zd bytes "
		"(want the next message)", truncated, start ? "abcd" : "?",
		flagged ? "set" : "missing", next);

	// The peer's close.
	close(pair[0]);
	ssize_t end = recv(pair[1], buffer, sizeof(buffer), 0);
	report("peer_closed", end == 0, "recv after the peer closed: %zd", end);
	close(pair[1]);
}


static void
test_rights()
{
	int pair[2];
	if (socketpair(AF_UNIX, SOCK_SEQPACKET, 0, pair) != 0) {
		report("rights", false, "socketpair: %s", strerror(errno));
		return;
	}

	// A memfd (shared memory, as OBS's browser source passes frames) and
	// both ends of a pipe, with the message they belong to.
	int memory = memfd_create("seqpacket", 0);
	ftruncate(memory, 4096);
	int pipeEnds[2];
	pipe(pipeEnds);
	int fds[3] = { memory, pipeEnds[0], pipeEnds[1] };
	send(pair[0], "before", 6, 0);
	ssize_t sent = send_with_fds(pair[0], "frame", 5, fds, 3);
	send(pair[0], "after", 5, 0);

	char buffer[64];
	int received[8];
	int count, flags;
	ssize_t got1 = receive_with_fds(pair[1], buffer, sizeof(buffer), 8,
		received, &count, &flags);
	bool none = got1 == 6 && count == 0;
	ssize_t got2 = receive_with_fds(pair[1], buffer, sizeof(buffer), 8,
		received, &count, &flags);
	bool withFds = got2 == 5 && memcmp(buffer, "frame", 5) == 0
		&& count == 3;
	int memoryCopy = count == 3 ? received[0] : -1;
	int readCopy = count == 3 ? received[1] : -1;
	int writeCopy = count == 3 ? received[2] : -1;
	int dummy[8];
	int afterCount;
	ssize_t got3 = receive_with_fds(pair[1], buffer, sizeof(buffer), 8,
		dummy, &afterCount, &flags);
	report("rights_with_message", sent == 5 && none && withFds && got3 == 5
			&& afterCount == 0,
		"\"before\": %zd bytes, %s; \"frame\": %zd bytes with %d "
		"descriptors; \"after\": %zd bytes, %d descriptors", got1,
		none ? "none" : "descriptors!", got2, count, got3, afterCount);

	// The new descriptors refer to the same files.
	bool distinct = memoryCopy >= 0 && memoryCopy != memory
		&& readCopy != pipeEnds[0] && writeCopy != pipeEnds[1];
	bool shared = false;
	if (memoryCopy >= 0) {
		char* mine = (char*)mmap(NULL, 4096, PROT_READ | PROT_WRITE,
			MAP_SHARED, memory, 0);
		char* theirs = (char*)mmap(NULL, 4096, PROT_READ | PROT_WRITE,
			MAP_SHARED, memoryCopy, 0);
		if (mine != MAP_FAILED && theirs != MAP_FAILED) {
			strcpy(theirs, "written by the receiver");
			shared = strcmp(mine, "written by the receiver") == 0;
		}
		if (mine != MAP_FAILED)
			munmap(mine, 4096);
		if (theirs != MAP_FAILED)
			munmap(theirs, 4096);
	}
	bool pipeWorks = false;
	if (writeCopy >= 0 && write(writeCopy, "p", 1) == 1) {
		char c = 0;
		pipeWorks = read(pipeEnds[0], &c, 1) == 1 && c == 'p';
	}
	report("rights_same_files", distinct && shared && pipeWorks,
		"new numbers %s; memfd shared %s; pipe through the copy %s",
		distinct ? "yes" : "no", shared ? "yes" : "no",
		pipeWorks ? "works" : "fails");
	for (int i = 0; i < count; i++)
		close(received[i]);

	// Room for one descriptor of three: MSG_CTRUNC; the other two are
	// closed, not leaked into the receiver.
	int before = open_descriptors();
	send_with_fds(pair[0], "x", 1, fds, 3);
	ssize_t got = receive_with_fds(pair[1], buffer, sizeof(buffer), 1,
		received, &count, &flags);
	bool ctrunc = (flags & MSG_CTRUNC) != 0;
	for (int i = 0; i < count; i++)
		close(received[i]);
	int after = open_descriptors();
	report("rights_ctrunc", got == 1 && ctrunc && count <= 1
			&& after == before,
		"room for 1 of 3: %zd byte, MSG_CTRUNC %s, %d received, %d "
		"descriptors open before, %d after", got, ctrunc ? "set" : "missing",
		count, before, after);

	// A message with descriptors and no data.
	ssize_t sentEmpty = send_with_fds(pair[0], "", 0, fds, 1);
	got = receive_with_fds(pair[1], buffer, sizeof(buffer), 8, received,
		&count, &flags);
	report("rights_empty_message", sentEmpty == 0 && got == 0 && count == 1,
		"sent %zd, received %zd bytes with %d descriptor(s)", sentEmpty, got,
		count);
	for (int i = 0; i < count; i++)
		close(received[i]);

	close(memory);
	close(pipeEnds[0]);
	close(pipeEnds[1]);
	close(pair[0]);
	close(pair[1]);
}


static void
test_listen()
{
	char path[64];
	snprintf(path, sizeof(path), "/tmp/seqpacket-%d", (int)getpid());
	unlink(path);
	sockaddr_un address = {};
	address.sun_family = AF_UNIX;
	strlcpy(address.sun_path, path, sizeof(address.sun_path));
	socklen_t addressLength = sizeof(address);

	int listener = socket(AF_UNIX, SOCK_SEQPACKET, 0);
	bool bound = listener >= 0
		&& bind(listener, (sockaddr*)&address, addressLength) == 0
		&& listen(listener, 2) == 0;
	int client = socket(AF_UNIX, SOCK_SEQPACKET, 0);
	bool connected = bound
		&& connect(client, (sockaddr*)&address, addressLength) == 0;
	int server = connected ? accept(listener, NULL, NULL) : -1;
	int memory = memfd_create("seqpacket listen", 0);
	write(memory, "hello", 5);
	ssize_t sent = server >= 0 ? send_with_fds(client, "hi", 2, &memory, 1)
		: -1;
	char buffer[16];
	int received[8];
	int count = 0, flags = 0;
	ssize_t got = server >= 0 ? receive_with_fds(server, buffer,
		sizeof(buffer), 8, received, &count, &flags) : -1;
	char content[8] = {};
	if (count == 1)
		pread(received[0], content, 5, 0);
	report("listen_accept", bound && connected && server >= 0 && sent == 2
			&& got == 2 && count == 1 && strcmp(content, "hello") == 0,
		"bind/listen %s, connect %s, accept %d, message %zd bytes with %d "
		"descriptor(s), the memfd reads \"%s\"", bound ? "ok" : "failed",
		connected ? "ok" : "failed", server, got, count, content);
	for (int i = 0; i < count; i++)
		close(received[i]);
	close(memory);
	close(client);
	if (server >= 0)
		close(server);
	close(listener);
	unlink(path);
}


int
main()
{
	printf("CE-SUITE " SUITE " START\n");
	fflush(stdout);
	test_boundaries();
	test_rights();
	test_listen();
	printf("CE-SUITE " SUITE " DONE failures=%d\n", sFailures);
	fflush(stdout);
	return sFailures == 0 ? 0 : 1;
}
