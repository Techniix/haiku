/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 *
 * Renguin test (F-27): the calls the maintainer's hardware tests found
 * wanting (2026-11-14), linked against musl and against glibc. Each line
 * is "<check> ok" or "<check> FAIL <why>"; the last is "ok" when all
 * passed.
 *
 *   connect6_nonblock  a non-blocking connect() to an IPv6 address there is
 *                      no route to fails at once (ENETUNREACH or
 *                      EHOSTUNREACH), or reports the error through poll()
 *                      and SO_ERROR within 5 s (apk hung on dl-cdn's IPv6
 *                      addresses with no IPv6 route)
 *   connect6_failed_poll  after such an immediate failure poll() reports
 *                      POLLOUT within 1 s (libfetch polls the failed socket)
 *   udp6_route         a UDP connect() to that address fails with
 *                      ENETUNREACH, as on Linux (musl's address sorting)
 *   connect6_block     the same with a blocking connect() returns an error
 *                      within 5 s
 *   connect_fallback   then IPv4 to a local listener connects
 *   connect_nonblock_poll  a non-blocking IPv4 connect reports POLLOUT and
 *                      SO_ERROR 0 within 2 s (apk's libfetch way)
 *   sigint_read        SIGINT (default action) ends a child blocked in
 *                      read() at once (Ctrl+C did nothing during apk's hang)
 *   sigint_poll        a SIGINT handler (no SA_RESTART) makes a blocked
 *                      poll() fail with EINTR
 *   memfd_fallocate    weston's buffer file: memfd_create(MFD_CLOEXEC |
 *                      MFD_ALLOW_SEALING), F_ADD_SEALS(F_SEAL_SHRINK),
 *                      posix_fallocate() of 250000 bytes, the size, a
 *                      shared mapping
 *   fallocate_keep     fallocate(FALLOC_FL_KEEP_SIZE) keeps the size, on a
 *                      memfd and on a file in /tmp
 *   memfd_shared       two processes' own MAP_SHARED mappings of one memfd
 *                      see each other's stores (fork, then the child maps
 *                      the inherited descriptor afresh, as a process that
 *                      got it over a socket does; Chromium's shared memory)
 *   pty                posix_openpt(), grantpt(), unlockpt(), ptsname()
 *                      (/dev/pts/N), the slave opens and is a terminal,
 *                      TIOCSWINSZ/TIOCGWINSZ through it, data both ways
 *   forkpty            forkpty() running "sh -c 'tty; stty size'": the
 *                      child's terminal is /dev/pts/N and 24 80
 *   pty_ctrl_c         ^C written to the master ends "sleep 30" in the
 *                      child by SIGINT
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <stdarg.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <pty.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <stdint.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#ifndef FALLOC_FL_KEEP_SIZE
#define FALLOC_FL_KEEP_SIZE 1
#endif

static int sFailed = 0;


static void
result(const char* check, int ok, const char* format, ...)
{
	if (ok) {
		printf("%s ok\n", check);
	} else {
		char detail[256];
		va_list args;
		va_start(args, format);
		vsnprintf(detail, sizeof(detail), format, args);
		va_end(args);
		printf("%s FAIL %s\n", check, detail);
		sFailed++;
	}
	fflush(stdout);
}


static long long
now_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000LL + ts.tv_nsec / 1000000;
}


static void
unreachable6(struct sockaddr_in6* address)
{
	memset(address, 0, sizeof(*address));
	address->sin6_family = AF_INET6;
	address->sin6_port = htons(80);
	// 2001:db8::1 (documentation prefix): the guest has no IPv6 route to it
	inet_pton(AF_INET6, "2001:db8::1", &address->sin6_addr);
}


static void
alarm_handler(int signal)
{
	(void)signal;
}


static void
test_connect(void)
{
	struct sockaddr_in6 address;
	unreachable6(&address);

	int fd = socket(AF_INET6, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
	long long start = now_ms();
	int status = fd >= 0
		? connect(fd, (struct sockaddr*)&address, sizeof(address)) : -1;
	int error = errno;
	if (fd < 0) {
		// a system without IPv6 at all fails here, which is fine too
		result("connect6_nonblock", error == EAFNOSUPPORT, "socket: %s",
			strerror(error));
	} else if (status == 0) {
		result("connect6_nonblock", 0, "connected to 2001:db8::1");
	} else if (error == ENETUNREACH || error == EHOSTUNREACH) {
		result("connect6_nonblock", 1, "");
		// libfetch polls the socket for POLLOUT even after connect() failed
		// at once; Linux reports POLLOUT|POLLHUP straight away
		struct pollfd poller = { fd, POLLOUT, 0 };
		long long pollStart = now_ms();
		int ready = poll(&poller, 1, 3000);
		result("connect6_failed_poll", ready == 1
				&& (poller.revents & POLLOUT) != 0
				&& now_ms() - pollStart < 1000,
			"poll %d (revents %#x) after %lld ms", ready, poller.revents,
			now_ms() - pollStart);
	} else if (error == EINPROGRESS) {
		struct pollfd poller = { fd, POLLOUT, 0 };
		int ready = poll(&poller, 1, 5000);
		int soError = 0;
		socklen_t length = sizeof(soError);
		getsockopt(fd, SOL_SOCKET, SO_ERROR, &soError, &length);
		result("connect6_nonblock", ready == 1 && soError != 0,
			"EINPROGRESS, then poll %d (revents %#x) and SO_ERROR %d after "
			"%lld ms", ready, poller.revents, soError, now_ms() - start);
	} else {
		result("connect6_nonblock", 0, "errno %d (%s)", error,
			strerror(error));
	}
	if (fd >= 0)
		close(fd);

	// musl's getaddrinfo() sorts addresses (RFC 6724) by connect()ing a
	// UDP socket to each: Linux fails that with ENETUNREACH when there is
	// no route, which puts IPv4 first
	fd = socket(AF_INET6, SOCK_DGRAM | SOCK_CLOEXEC, 0);
	error = errno;
	if (fd < 0) {
		result("udp6_route", error == EAFNOSUPPORT, "socket: %s",
			strerror(error));
	} else {
		status = connect(fd, (struct sockaddr*)&address, sizeof(address));
		error = errno;
		result("udp6_route", status != 0
				&& (error == ENETUNREACH || error == EHOSTUNREACH),
			"connect %d, errno %d (%s)", status, error, strerror(error));
		close(fd);
	}

	struct sigaction action;
	memset(&action, 0, sizeof(action));
	action.sa_handler = alarm_handler;
	sigaction(SIGALRM, &action, NULL);
	fd = socket(AF_INET6, SOCK_STREAM | SOCK_CLOEXEC, 0);
	alarm(5);
	start = now_ms();
	status = fd >= 0
		? connect(fd, (struct sockaddr*)&address, sizeof(address)) : -1;
	error = errno;
	alarm(0);
	result("connect6_block", status != 0 && error != EINTR,
		"connect %d, errno %d (%s) after %lld ms", status, error,
		strerror(error), now_ms() - start);
	if (fd >= 0)
		close(fd);

	// IPv4 to a listener of our own
	int listener = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
	struct sockaddr_in local;
	memset(&local, 0, sizeof(local));
	local.sin_family = AF_INET;
	local.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	socklen_t localLength = sizeof(local);
	bind(listener, (struct sockaddr*)&local, sizeof(local));
	listen(listener, 1);
	getsockname(listener, (struct sockaddr*)&local, &localLength);
	fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
	status = connect(fd, (struct sockaddr*)&local, sizeof(local));
	result("connect_fallback", status == 0, "IPv4 connect: %s",
		strerror(errno));
	close(fd);

	// apk's (libfetch's) way: a non-blocking connect, then poll() for
	// POLLOUT and SO_ERROR (apk waited its whole 60 s timeout on hardware)
	fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
	fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
	start = now_ms();
	status = connect(fd, (struct sockaddr*)&local, sizeof(local));
	error = errno;
	struct pollfd writable = { fd, POLLOUT, 0 };
	int ready = status == 0 ? 1 : poll(&writable, 1, 5000);
	int soError = -1;
	socklen_t soLength = sizeof(soError);
	getsockopt(fd, SOL_SOCKET, SO_ERROR, &soError, &soLength);
	result("connect_nonblock_poll", (status == 0 || error == EINPROGRESS)
			&& ready == 1 && (writable.revents & POLLOUT) != 0 && soError == 0
			&& now_ms() - start < 2000,
		"connect %d (%s), poll %d revents %#x, SO_ERROR %d, %lld ms", status,
		status == 0 ? "done" : strerror(error), ready, writable.revents,
		soError, now_ms() - start);
	close(fd);
	close(listener);
}


static void
interrupt_handler(int signal)
{
	(void)signal;
}


static void
test_sigint(void)
{
	int pipes[2];
	if (pipe(pipes) != 0)
		return;
	pid_t child = fork();
	if (child == 0) {
		char byte;
		_exit(read(pipes[0], &byte, 1) == 1 ? 0 : 1);
	}
	usleep(300000);
	long long start = now_ms();
	kill(child, SIGINT);
	int status = 0;
	pid_t done = 0;
	while ((done = waitpid(child, &status, WNOHANG)) == 0
			&& now_ms() - start < 3000)
		usleep(20000);
	if (done == 0) {
		kill(child, SIGKILL);
		waitpid(child, &status, 0);
	}
	result("sigint_read", done == child && WIFSIGNALED(status)
			&& WTERMSIG(status) == SIGINT,
		"child %s (status %#x) after %lld ms", done == child ? "ended"
			: "still blocked", status, now_ms() - start);
	close(pipes[0]);
	close(pipes[1]);

	struct sigaction action;
	memset(&action, 0, sizeof(action));
	action.sa_handler = interrupt_handler;
	sigaction(SIGINT, &action, NULL);
	if (pipe(pipes) != 0)
		return;
	pid_t parent = getpid();
	child = fork();
	if (child == 0) {
		usleep(300000);
		kill(parent, SIGINT);
		_exit(0);
	}
	struct pollfd poller = { pipes[0], POLLIN, 0 };
	start = now_ms();
	int ready = poll(&poller, 1, 5000);
	int error = errno;
	waitpid(child, NULL, 0);
	result("sigint_poll", ready == -1 && error == EINTR,
		"poll %d, errno %d after %lld ms", ready, error, now_ms() - start);
	signal(SIGINT, SIG_DFL);
	close(pipes[0]);
	close(pipes[1]);
}


static void
test_fallocate(void)
{
	int fd = memfd_create("renguin-test-buffer", MFD_CLOEXEC
		| MFD_ALLOW_SEALING);
	int seal = fcntl(fd, F_ADD_SEALS, F_SEAL_SHRINK);
	int status = posix_fallocate(fd, 0, 250000);
	struct stat st;
	fstat(fd, &st);
	void* map = mmap(NULL, 250000, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (map != MAP_FAILED) {
		memset(map, 0x5a, 250000);
		munmap(map, 250000);
	}
	result("memfd_fallocate", fd >= 0 && seal == 0 && status == 0
			&& st.st_size == 250000 && map != MAP_FAILED,
		"memfd %d, F_ADD_SEALS %d, posix_fallocate %d (%s), size %lld, "
		"mmap %s", fd, seal, status, strerror(status),
		(long long)st.st_size, map != MAP_FAILED ? "ok" : strerror(errno));

	int keepMemfd = fallocate(fd, FALLOC_FL_KEEP_SIZE, 0, 300000);
	int keepMemfdErrno = errno;
	fstat(fd, &st);
	off_t memfdSize = st.st_size;
	close(fd);

	unlink("/tmp/renguin-fallocate");
	fd = open("/tmp/renguin-fallocate", O_RDWR | O_CREAT | O_TRUNC, 0600);
	int plain = posix_fallocate(fd, 0, 65536);
	int keep = fallocate(fd, FALLOC_FL_KEEP_SIZE, 0, 131072);
	int keepErrno = errno;
	fstat(fd, &st);
	close(fd);
	unlink("/tmp/renguin-fallocate");
	result("fallocate_keep", keepMemfd == 0 && memfdSize == 250000
			&& plain == 0 && keep == 0 && st.st_size == 65536,
		"memfd: %d (%s), size %lld; file: posix_fallocate %d, keep %d (%s), "
		"size %lld", keepMemfd, strerror(keepMemfdErrno),
		(long long)memfdSize, plain, keep, strerror(keepErrno),
		(long long)st.st_size);
}


static void
test_memfd_shared(void)
{
	int fd = memfd_create("renguin-test-shared", MFD_CLOEXEC);
	int sized = fd >= 0 ? ftruncate(fd, 65536) : -1;
	volatile uint32_t* parent = (volatile uint32_t*)mmap(NULL, 65536,
		PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (fd < 0 || sized != 0 || parent == MAP_FAILED) {
		result("memfd_shared", 0, "memfd %d, ftruncate %d, mmap %s", fd,
			sized, parent == MAP_FAILED ? strerror(errno) : "ok");
		if (fd >= 0)
			close(fd);
		return;
	}
	parent[0] = 0;
	parent[4096] = 0;	/* a second page */
	parent[1] = 0x11111111;
	pid_t child = fork();
	if (child == 0) {
		volatile uint32_t* own = (volatile uint32_t*)mmap(NULL, 65536,
			PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
		if (own == MAP_FAILED)
			_exit(2);
		if (own[1] != 0x11111111)
			_exit(3);		/* the parent's store before fork() */
		own[4096] = 0x600d;
		__atomic_store_n(&own[0], 1, __ATOMIC_SEQ_CST);
		for (int i = 0; i < 500 && __atomic_load_n(&own[0], __ATOMIC_SEQ_CST)
				!= 2; i++) {
			usleep(10000);
		}
		_exit(__atomic_load_n(&own[0], __ATOMIC_SEQ_CST) == 2 ? 0 : 4);
	}
	int seen = 0;
	for (int i = 0; i < 500 && child > 0; i++) {
		if (__atomic_load_n(&parent[0], __ATOMIC_SEQ_CST) == 1) {
			seen = parent[4096] == 0x600d;
			__atomic_store_n(&parent[0], 2, __ATOMIC_SEQ_CST);
			break;
		}
		usleep(10000);
	}
	int status = -1;
	if (child > 0)
		waitpid(child, &status, 0);
	result("memfd_shared", seen && WIFEXITED(status)
			&& WEXITSTATUS(status) == 0,
		"the child's stores %s, child status %d", seen ? "seen" : "not seen",
		WIFEXITED(status) ? WEXITSTATUS(status) : -1);
	munmap((void*)parent, 65536);
	close(fd);
}


static void
test_pty(void)
{
	int master = posix_openpt(O_RDWR | O_NOCTTY);
	if (master < 0) {
		result("pty", 0, "posix_openpt: %s", strerror(errno));
		result("forkpty", 0, "no pty");
		return;
	}
	int granted = grantpt(master);
	int unlocked = unlockpt(master);
	char* name = ptsname(master);
	char slaveName[64] = "";
	if (name != NULL)
		snprintf(slaveName, sizeof(slaveName), "%s", name);
	int slave = name != NULL ? open(slaveName, O_RDWR | O_NOCTTY) : -1;
	int tty = slave >= 0 && isatty(slave);
	struct winsize size = { 30, 100, 0, 0 }, back = { 0, 0, 0, 0 };
	int setSize = slave >= 0 ? ioctl(master, TIOCSWINSZ, &size) : -1;
	int getSize = slave >= 0 ? ioctl(slave, TIOCGWINSZ, &back) : -1;
	char buffer[16] = "";
	int wrote = slave >= 0 ? (int)write(master, "hi\n", 3) : -1;
	int got = slave >= 0 ? (int)read(slave, buffer, sizeof(buffer) - 1) : -1;
	result("pty", granted == 0 && unlocked == 0
			&& strncmp(slaveName, "/dev/pts/", 9) == 0 && tty
			&& setSize == 0 && getSize == 0 && back.ws_row == 30
			&& back.ws_col == 100 && wrote == 3 && got >= 2,
		"grantpt %d, unlockpt %d (%s), ptsname \"%s\", slave %d, isatty %d, "
		"winsize %d/%d %dx%d, wrote %d, read %d", granted, unlocked,
		strerror(errno), slaveName, slave, tty, setSize, getSize,
		back.ws_row, back.ws_col, wrote, got);
	if (slave >= 0)
		close(slave);
	close(master);

	struct winsize childSize = { 24, 80, 0, 0 };
	int pty;
	pid_t child = forkpty(&pty, NULL, NULL, &childSize);
	if (child == 0) {
		// a Linux PATH: the inherited one is Haiku's (its own tty first)
		setenv("PATH", "/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:"
			"/sbin:/bin", 1);
		execl("/bin/sh", "sh", "-c", "tty; stty size", (char*)NULL);
		_exit(127);
	}
	if (child < 0) {
		result("forkpty", 0, "forkpty: %s", strerror(errno));
		return;
	}
	char output[256] = "";
	size_t length = 0;
	long long start = now_ms();
	while (length < sizeof(output) - 1 && now_ms() - start < 5000) {
		struct pollfd poller = { pty, POLLIN, 0 };
		if (poll(&poller, 1, 200) <= 0)
			continue;
		ssize_t bytes = read(pty, output + length, sizeof(output) - 1 - length);
		if (bytes <= 0)
			break;
		length += bytes;
	}
	output[length] = '\0';
	int status = 0;
	waitpid(child, &status, 0);
	close(pty);
	for (char* c = output; *c; c++) {
		if (*c == '\r' || *c == '\n')
			*c = ' ';
	}
	result("forkpty", strstr(output, "/dev/pts/") != NULL
			&& strstr(output, "24 80") != NULL && WIFEXITED(status)
			&& WEXITSTATUS(status) == 0,
		"output \"%s\", status %#x", output, status);

	// Ctrl+C typed into the terminal: the line discipline sends SIGINT to
	// the foreground process group (apk could not be interrupted)
	child = forkpty(&pty, NULL, NULL, &childSize);
	if (child == 0) {
		execl("/bin/sleep", "sleep", "30", (char*)NULL);
		_exit(127);
	}
	if (child < 0) {
		result("pty_ctrl_c", 0, "forkpty: %s", strerror(errno));
		return;
	}
	usleep(500000);
	start = now_ms();
	write(pty, "\003", 1);
	pid_t done = 0;
	while ((done = waitpid(child, &status, WNOHANG)) == 0
			&& now_ms() - start < 3000)
		usleep(20000);
	if (done == 0) {
		kill(child, SIGKILL);
		waitpid(child, &status, 0);
	}
	close(pty);
	result("pty_ctrl_c", done == child && WIFSIGNALED(status)
			&& WTERMSIG(status) == SIGINT,
		"sleep %s (status %#x) %lld ms after ^C", done == child ? "ended"
			: "still running", status, now_ms() - start);
}


int
main(void)
{
	test_connect();
	test_sigint();
	test_fallocate();
	test_memfd_shared();
	test_pty();
	if (sFailed == 0)
		printf("ok\n");
	return sFailed == 0 ? 0 : 1;
}
