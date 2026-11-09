/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 */

/*!	In-guest conformance test for epoll (kernel object + libgnu API).

	The expected results are Linux's (epoll(7), epoll_ctl(2),
	epoll_wait(2)), which event loops such as libuv (Node.js, Electron),
	libevent, glib's GPollFD backend in Chromium and Wine depend on. The
	documented deviations (PORTING.md, "epoll") are tested as such.
	Output format: see 3rdparty/ce/README.md.
*/


#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/timerfd.h>

#include <OS.h>

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>


#define SUITE "epoll"


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


static int
add(int ep, int fd, uint32_t events, uint64_t data)
{
	struct epoll_event event = {};
	event.events = events;
	event.data.u64 = data;
	return epoll_ctl(ep, EPOLL_CTL_ADD, fd, &event);
}


static int
modify(int ep, int fd, uint32_t events, uint64_t data)
{
	struct epoll_event event = {};
	event.events = events;
	event.data.u64 = data;
	return epoll_ctl(ep, EPOLL_CTL_MOD, fd, &event);
}


//! Waits with timeout 0; returns the count, and the first event.
static int
peek(int ep, struct epoll_event* first = NULL, int timeout = 0)
{
	struct epoll_event events[16];
	int count = epoll_wait(ep, events, 16, timeout);
	if (count > 0 && first != NULL)
		*first = events[0];
	return count;
}


static void
drain(int fd)
{
	char buffer[256];
	while (read(fd, buffer, sizeof(buffer)) > 0)
		;
}


static void
set_nonblock(int fd)
{
	fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
}


static const char*
errno_name(int error)
{
	switch (error) {
		case 0: return "0";
		case EBADF: return "EBADF";
		case EINVAL: return "EINVAL";
		case EEXIST: return "EEXIST";
		case ENOENT: return "ENOENT";
		case EPERM: return "EPERM";
		case ELOOP: return "ELOOP";
		case EFAULT: return "EFAULT";
		case EINTR: return "EINTR";
		case EAGAIN: return "EAGAIN";
	}
	return strerror(error);
}


//! Runs \a call and reports whether it failed with \a expected.
#define EXPECT_ERRNO(name, expected, call) \
	do { \
		errno = 0; \
		int _result = (call); \
		report(name, _result == -1 && errno == (expected), \
			"result %d, errno %s (want %s)", _result, errno_name(errno), \
			errno_name(expected)); \
	} while (false)


// #pragma mark - argument checks


static void
test_arguments()
{
	int ep = epoll_create(1);
	report("create", ep >= 0, "%s", ep >= 0 ? "" : strerror(errno));

	EXPECT_ERRNO("create_size_zero", EINVAL, epoll_create(0));
	EXPECT_ERRNO("create1_bad_flags", EINVAL, epoll_create1(0x10));

	int cloexec = epoll_create1(EPOLL_CLOEXEC);
	report("create1_cloexec", cloexec >= 0
			&& (fcntl(cloexec, F_GETFD) & FD_CLOEXEC) != 0
			&& (fcntl(ep, F_GETFD) & FD_CLOEXEC) == 0, "");
	close(cloexec);

	int efd = eventfd(0, EFD_NONBLOCK);
	int pipes[2];
	pipe(pipes);
	struct epoll_event event = {};
	event.events = EPOLLIN;

	EXPECT_ERRNO("ctl_bad_epfd", EBADF, epoll_ctl(9999, EPOLL_CTL_ADD, efd,
		&event));
	EXPECT_ERRNO("ctl_epfd_not_epoll", EINVAL, epoll_ctl(pipes[0],
		EPOLL_CTL_ADD, efd, &event));
	EXPECT_ERRNO("ctl_bad_fd", EBADF, epoll_ctl(ep, EPOLL_CTL_ADD, 9999,
		&event));
	EXPECT_ERRNO("ctl_self", EINVAL, epoll_ctl(ep, EPOLL_CTL_ADD, ep,
		&event));
	EXPECT_ERRNO("ctl_bad_op", EINVAL, epoll_ctl(ep, 42, efd, &event));
	EXPECT_ERRNO("ctl_null_event", EFAULT, epoll_ctl(ep, EPOLL_CTL_ADD, efd,
		NULL));
	EXPECT_ERRNO("ctl_mod_missing", ENOENT, modify(ep, efd, EPOLLIN, 0));
	EXPECT_ERRNO("ctl_del_missing", ENOENT, epoll_ctl(ep, EPOLL_CTL_DEL, efd,
		NULL));

	int result = add(ep, efd, EPOLLIN, 1);
	report("ctl_add", result == 0, "%s", errno_name(result == 0 ? 0 : errno));
	EXPECT_ERRNO("ctl_add_twice", EEXIST, add(ep, efd, EPOLLIN, 1));

	// The same open file description under a second fd number is a
	// separate registration on Linux.
	int dupFD = dup(efd);
	result = add(ep, dupFD, EPOLLIN, 2);
	report("ctl_add_dup_fd", result == 0, "%s",
		errno_name(result == 0 ? 0 : errno));

	result = epoll_ctl(ep, EPOLL_CTL_DEL, efd, NULL);
	report("ctl_del", result == 0, "%s", errno_name(result == 0 ? 0 : errno));
	EXPECT_ERRNO("ctl_del_twice", ENOENT, epoll_ctl(ep, EPOLL_CTL_DEL, efd,
		NULL));
	close(dupFD);

	// Regular files and directories are always ready; Linux refuses them.
	int file = open("/boot/system/non-packaged/bin/ce-tests/epoll_test",
		O_RDONLY);
	if (file < 0)
		file = open("/boot/system/lib/libroot.so", O_RDONLY);
	EXPECT_ERRNO("ctl_regular_file_eperm", EPERM, add(ep, file, EPOLLIN, 0));
	close(file);
	int dir = open("/boot/system", O_RDONLY);
	EXPECT_ERRNO("ctl_directory_eperm", EPERM, add(ep, dir, EPOLLIN, 0));
	close(dir);

	// EPOLLEXCLUSIVE: accepted on ADD (as a no-op), refused on MOD and in
	// combination with EPOLLONESHOT.
	result = add(ep, pipes[0], EPOLLIN | EPOLLEXCLUSIVE, 3);
	report("exclusive_add", result == 0, "%s",
		errno_name(result == 0 ? 0 : errno));
	EXPECT_ERRNO("exclusive_mod", EINVAL, modify(ep, pipes[0],
		EPOLLIN | EPOLLEXCLUSIVE, 3));
	EXPECT_ERRNO("exclusive_mod_registered", EINVAL, modify(ep, pipes[0],
		EPOLLIN, 3));
	EXPECT_ERRNO("exclusive_oneshot", EINVAL, add(ep, pipes[1],
		EPOLLOUT | EPOLLEXCLUSIVE | EPOLLONESHOT, 4));
	// Unknown event bits are ignored, as on Linux.
	result = add(ep, pipes[1], EPOLLOUT | 0x08000000, 4);
	report("unknown_event_bits", result == 0, "%s",
		errno_name(result == 0 ? 0 : errno));

	struct epoll_event events[4];
	EXPECT_ERRNO("wait_maxevents_zero", EINVAL, epoll_wait(ep, events, 0,
		0));
	EXPECT_ERRNO("wait_not_epoll", EINVAL, epoll_wait(efd, events, 4, 0));
	EXPECT_ERRNO("wait_bad_fd", EBADF, epoll_wait(9999, events, 4, 0));
	EXPECT_ERRNO("wait_bad_buffer", EFAULT, epoll_wait(ep, NULL, 4, 0));

	struct stat st;
	report("fstat", fstat(ep, &st) == 0, "mode 0%o", (unsigned)st.st_mode);

	close(efd);
	close(pipes[0]);
	close(pipes[1]);
	close(ep);
}


// #pragma mark - triggering


static void
test_level_edge_oneshot()
{
	int ep = epoll_create1(0);
	int pipes[2];
	pipe(pipes);
	set_nonblock(pipes[0]);

	// Level-triggered: reported as long as the data is there.
	add(ep, pipes[0], EPOLLIN, 0x1122334455667788ULL);
	report("lt_empty", peek(ep) == 0, "");
	write(pipes[1], "x", 1);
	struct epoll_event event = {};
	int first = peek(ep, &event);
	int second = peek(ep);
	report("lt_ready", first == 1 && event.events == EPOLLIN
			&& event.data.u64 == 0x1122334455667788ULL && second == 1,
		"first %d events %#x data %#llx, second %d", first, event.events,
		(unsigned long long)event.data.u64, second);
	drain(pipes[0]);
	report("lt_drained", peek(ep) == 0, "");

	// data.ptr round trip
	int marker;
	struct epoll_event pointerEvent = {};
	pointerEvent.events = EPOLLIN;
	pointerEvent.data.ptr = &marker;
	epoll_ctl(ep, EPOLL_CTL_MOD, pipes[0], &pointerEvent);
	write(pipes[1], "x", 1);
	peek(ep, &event);
	report("data_ptr", event.data.ptr == &marker, "%p", event.data.ptr);
	drain(pipes[0]);

	// Edge-triggered: once per new arrival.
	modify(ep, pipes[0], EPOLLIN | EPOLLET, 7);
	write(pipes[1], "a", 1);
	first = peek(ep, &event);
	second = peek(ep);
	write(pipes[1], "b", 1);
	int third = peek(ep);
	report("et", first == 1 && event.data.u64 == 7 && second == 0
			&& third == 1, "first %d, again %d, after new data %d", first,
		second, third);
	drain(pipes[0]);

	// ET: data that was there before is reported once on MOD/ADD.
	write(pipes[1], "c", 1);
	modify(ep, pipes[0], EPOLLIN | EPOLLET, 8);
	first = peek(ep);
	second = peek(ep);
	report("et_existing_data", first == 1 && second == 0, "%d then %d", first,
		second);
	drain(pipes[0]);

	// One-shot: one report, then disarmed until MOD.
	modify(ep, pipes[0], EPOLLIN | EPOLLONESHOT, 9);
	write(pipes[1], "d", 1);
	first = peek(ep);
	second = peek(ep);
	write(pipes[1], "e", 1);
	third = peek(ep);
	modify(ep, pipes[0], EPOLLIN | EPOLLONESHOT, 10);
	int rearmed = peek(ep, &event);
	report("oneshot", first == 1 && second == 0 && third == 0
			&& rearmed == 1 && event.data.u64 == 10,
		"%d, %d, %d, re-armed %d", first, second, third, rearmed);
	EXPECT_ERRNO("oneshot_still_registered", EEXIST, add(ep, pipes[0],
		EPOLLIN, 0));
	drain(pipes[0]);

	// Edge-triggered one-shot (the libuv/Go pattern) re-armed many times.
	int rounds = 0;
	for (int i = 0; i < 200; i++) {
		modify(ep, pipes[0], EPOLLIN | EPOLLET | EPOLLONESHOT, i);
		write(pipes[1], "f", 1);
		if (peek(ep, &event) == 1 && event.data.u64 == (uint64_t)i
			&& peek(ep) == 0) {
			rounds++;
		}
		drain(pipes[0]);
	}
	report("oneshot_rearm_loop", rounds == 200, "%d of 200", rounds);

	// EPOLLOUT on the write end, LT; full pipe is not writable.
	add(ep, pipes[1], EPOLLOUT, 11);
	epoll_ctl(ep, EPOLL_CTL_DEL, pipes[0], NULL);
	first = peek(ep, &event);
	report("out_writable", first == 1 && event.events == EPOLLOUT
		&& event.data.u64 == 11, "%d events %#x", first, event.events);
	set_nonblock(pipes[1]);
	char block[4096];
	memset(block, 'z', sizeof(block));
	while (write(pipes[1], block, sizeof(block)) > 0)
		;
	report("out_full", peek(ep) == 0, "");
	drain(pipes[0]);
	first = peek(ep);
	report("out_again", first == 1, "%d", first);

	// Hang-up: the reader sees EPOLLHUP after the last writer closed.
	epoll_ctl(ep, EPOLL_CTL_DEL, pipes[1], NULL);
	add(ep, pipes[0], EPOLLIN, 12);
	write(pipes[1], "g", 1);
	close(pipes[1]);
	first = peek(ep, &event);
	report("pipe_hup", first == 1 && (event.events & EPOLLHUP) != 0
			&& (event.events & EPOLLIN) != 0,
		"%d events %#x", first, event.events);

	close(pipes[0]);
	close(ep);
}


// #pragma mark - timeouts and wakeups


struct DelayedWrite {
	int			fd;
	bigtime_t	delay;
	int			count;
};


static void*
delayed_write(void* data)
{
	DelayedWrite* args = (DelayedWrite*)data;
	usleep(args->delay);
	for (int i = 0; i < args->count; i++)
		eventfd_write(args->fd, 1);
	return NULL;
}


static void
sigusr1_handler(int)
{
}


struct BlockedWait {
	int			ep;
	int			result;
	int			error;
	bigtime_t	waited;
};


static void*
blocked_wait(void* data)
{
	BlockedWait* wait = (BlockedWait*)data;
	struct epoll_event event;
	bigtime_t start = system_time();
	wait->result = epoll_wait(wait->ep, &event, 1, -1);
	wait->error = errno;
	wait->waited = system_time() - start;
	return NULL;
}


static void
test_timeouts()
{
	int ep = epoll_create1(0);
	int efd = eventfd(0, EFD_NONBLOCK);
	add(ep, efd, EPOLLIN, 1);

	struct epoll_event event;
	bigtime_t start = system_time();
	int result = epoll_wait(ep, &event, 1, 0);
	bigtime_t waited = system_time() - start;
	report("timeout_zero", result == 0 && waited < 50000, "%d after %lld us",
		result, (long long)waited);

	start = system_time();
	result = epoll_wait(ep, &event, 1, 150);
	waited = system_time() - start;
	report("timeout_positive", result == 0 && waited >= 145000
		&& waited < 2000000, "%d after %lld us", result, (long long)waited);

	struct timespec timeout = { 0, 120000000 };
	start = system_time();
	result = epoll_pwait2(ep, &event, 1, &timeout, NULL);
	waited = system_time() - start;
	report("pwait2_timeout", result == 0 && waited >= 115000
		&& waited < 2000000, "%d after %lld us", result, (long long)waited);

	// Infinite wait, woken by another thread.
	DelayedWrite args = { efd, 100000, 1 };
	pthread_t thread;
	pthread_create(&thread, NULL, delayed_write, &args);
	start = system_time();
	result = epoll_wait(ep, &event, 1, -1);
	waited = system_time() - start;
	pthread_join(thread, NULL);
	report("timeout_infinite_woken", result == 1 && event.data.u64 == 1
		&& waited >= 80000 && waited < 3000000, "%d after %lld us", result,
		(long long)waited);
	eventfd_t value;
	eventfd_read(efd, &value);

	// A positive timeout cut short by an event.
	args.delay = 50000;
	pthread_create(&thread, NULL, delayed_write, &args);
	start = system_time();
	result = epoll_wait(ep, &event, 1, 5000);
	waited = system_time() - start;
	pthread_join(thread, NULL);
	report("timeout_woken_early", result == 1 && waited < 3000000,
		"%d after %lld us", result, (long long)waited);
	eventfd_read(efd, &value);

	// EINTR: a signal handler interrupts the wait, even with SA_RESTART
	// (epoll_wait() is never restarted on Linux).
	struct sigaction action = {};
	action.sa_handler = sigusr1_handler;
	action.sa_flags = SA_RESTART;
	sigaction(SIGUSR1, &action, NULL);
	BlockedWait wait = { ep, 0, 0, 0 };
	pthread_create(&thread, NULL, blocked_wait, &wait);
	usleep(100000);
	pthread_kill(thread, SIGUSR1);
	pthread_join(thread, NULL);
	report("eintr", wait.result == -1 && wait.error == EINTR,
		"result %d errno %s after %lld us", wait.result,
		errno_name(wait.error), (long long)wait.waited);

	// epoll_pwait(): a blocked, pending signal is let through by the mask.
	sigset_t block, empty, old;
	sigemptyset(&block);
	sigaddset(&block, SIGUSR1);
	sigemptyset(&empty);
	pthread_sigmask(SIG_BLOCK, &block, &old);
	raise(SIGUSR1);
	errno = 0;
	result = epoll_wait(ep, &event, 1, 50);
	int plainErrno = errno;
	errno = 0;
	start = system_time();
	int pwait = epoll_pwait(ep, &event, 1, 2000, &empty);
	int pwaitErrno = errno;
	waited = system_time() - start;
	sigset_t current;
	pthread_sigmask(SIG_SETMASK, NULL, &current);
	report("pwait_sigmask", result == 0 && plainErrno == 0 && pwait == -1
			&& pwaitErrno == EINTR && waited < 1000000
			&& sigismember(&current, SIGUSR1),
		"blocked wait %d (%s), pwait %d (%s) after %lld us, mask restored %d",
		result, errno_name(plainErrno), pwait, errno_name(pwaitErrno),
		(long long)waited, sigismember(&current, SIGUSR1));

	// The same for ppoll(), which shares the signal-mask handling (an
	// upstream bug left the temporary mask installed after the handler).
	raise(SIGUSR1);
	struct timespec pollTimeout = { 2, 0 };
	errno = 0;
	int polled = ppoll(NULL, 0, &pollTimeout, &empty);
	int pollErrno = errno;
	pthread_sigmask(SIG_SETMASK, NULL, &current);
	report("ppoll_sigmask_restored", polled == -1 && pollErrno == EINTR
			&& sigismember(&current, SIGUSR1),
		"ppoll %d (%s), mask restored %d", polled, errno_name(pollErrno),
		sigismember(&current, SIGUSR1));
	pthread_sigmask(SIG_SETMASK, &old, NULL);
	signal(SIGUSR1, SIG_DFL);

	// Closing the epoll descriptor wakes a waiter (Renga: with EBADF).
	wait.result = 0;
	pthread_create(&thread, NULL, blocked_wait, &wait);
	usleep(100000);
	close(ep);
	pthread_join(thread, NULL);
	report("close_wakes_waiter", wait.result == -1 && wait.error == EBADF,
		"result %d errno %s", wait.result, errno_name(wait.error));

	close(efd);
}


// #pragma mark - descriptor types


static void
test_descriptor_types()
{
	int ep = epoll_create1(0);
	struct epoll_event event;

	// eventfd: readable with a count, writable unless full.
	int efd = eventfd(0, EFD_NONBLOCK);
	add(ep, efd, EPOLLIN | EPOLLOUT, 1);
	int count = peek(ep, &event);
	bool writable = count == 1 && event.events == EPOLLOUT;
	eventfd_write(efd, 3);
	count = peek(ep, &event);
	report("eventfd", writable && count == 1
			&& event.events == (EPOLLIN | EPOLLOUT),
		"%d events %#x", count, event.events);
	close(efd);

	// timerfd: readable when it expires.
	int tfd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK);
	add(ep, tfd, EPOLLIN, 2);
	struct itimerspec spec = {};
	spec.it_value.tv_nsec = 50000000;
	timerfd_settime(tfd, 0, &spec, NULL);
	bigtime_t start = system_time();
	count = epoll_wait(ep, &event, 1, 3000);
	bigtime_t waited = system_time() - start;
	uint64_t expirations = 0;
	read(tfd, &expirations, sizeof(expirations));
	report("timerfd", count == 1 && event.data.u64 == 2 && expirations == 1
		&& waited >= 45000, "%d after %lld us, %llu expirations", count,
		(long long)waited, (unsigned long long)expirations);
	close(tfd);
	report("timerfd_closed_gone", peek(ep) == 0, "");

	// UNIX socket pair.
	int pair[2];
	socketpair(AF_UNIX, SOCK_STREAM, 0, pair);
	add(ep, pair[0], EPOLLIN | EPOLLRDHUP | EPOLLET, 3);
	write(pair[1], "hello", 5);
	count = peek(ep, &event);
	report("unix_socket", count == 1 && event.events == EPOLLIN, "%d events "
		"%#x", count, event.events);
	char buffer[16];
	read(pair[0], buffer, sizeof(buffer));
	close(pair[1]);
	count = peek(ep, &event);
	report("unix_socket_peer_closed", count == 1
			&& (event.events & (EPOLLIN | EPOLLHUP)) != 0,
		"%d events %#x", count, event.events);
	close(pair[0]);

	// TCP over loopback: listening socket readable on a pending
	// connection, connected socket readable on data.
	int listener = socket(AF_INET, SOCK_STREAM, 0);
	struct sockaddr_in address = {};
	address.sin_family = AF_INET;
	address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	address.sin_port = 0;
	bind(listener, (struct sockaddr*)&address, sizeof(address));
	socklen_t length = sizeof(address);
	getsockname(listener, (struct sockaddr*)&address, &length);
	listen(listener, 4);
	add(ep, listener, EPOLLIN, 4);
	int client = socket(AF_INET, SOCK_STREAM, 0);
	connect(client, (struct sockaddr*)&address, sizeof(address));
	count = epoll_wait(ep, &event, 1, 3000);
	report("tcp_listen", count == 1 && event.data.u64 == 4
		&& (event.events & EPOLLIN) != 0, "%d events %#x", count,
		event.events);
	int server = accept(listener, NULL, NULL);
	epoll_ctl(ep, EPOLL_CTL_DEL, listener, NULL);
	add(ep, server, EPOLLIN | EPOLLRDHUP, 5);
	report("tcp_idle", peek(ep) == 0, "");
	write(client, "data", 4);
	count = epoll_wait(ep, &event, 1, 3000);
	report("tcp_data", count == 1 && event.data.u64 == 5
		&& event.events == EPOLLIN, "%d events %#x", count, event.events);
	read(server, buffer, sizeof(buffer));
	close(client);
	count = epoll_wait(ep, &event, 1, 3000);
	ssize_t bytes = read(server, buffer, sizeof(buffer));
	report("tcp_peer_closed", count == 1 && (event.events & EPOLLIN) != 0
		&& bytes == 0, "%d events %#x, read %zd", count, event.events, bytes);
	close(server);
	close(listener);

	close(ep);
}


// #pragma mark - nesting, poll of the epoll descriptor


static void
test_nesting()
{
	int outer = epoll_create1(0);
	int inner = epoll_create1(0);
	int efd = eventfd(0, EFD_NONBLOCK);
	add(inner, efd, EPOLLIN, 1);
	int result = add(outer, inner, EPOLLIN, 2);
	report("nested_add", result == 0, "%s", errno_name(result == 0 ? 0
		: errno));

	struct pollfd pfd = { inner, POLLIN, 0 };
	int polled = poll(&pfd, 1, 0);
	report("poll_epoll_idle", polled == 0 && peek(outer) == 0, "%d", polled);

	eventfd_write(efd, 1);
	struct epoll_event event;
	int count = peek(outer, &event);
	polled = poll(&pfd, 1, 0);
	report("nested_ready", count == 1 && event.data.u64 == 2
			&& event.events == EPOLLIN && polled == 1
			&& pfd.revents == POLLIN,
		"outer %d events %#x, poll %d revents %#x", count, event.events,
		polled, pfd.revents);

	fd_set readSet;
	FD_ZERO(&readSet);
	FD_SET(inner, &readSet);
	struct timeval timeout = { 0, 0 };
	int selected = select(inner + 1, &readSet, NULL, NULL, &timeout);
	report("select_epoll", selected == 1, "%d", selected);

	// Once the inner item is no longer ready, neither is the inner epoll
	// (level-triggered state is checked, not remembered).
	eventfd_t value;
	eventfd_read(efd, &value);
	count = peek(outer);
	polled = poll(&pfd, 1, 0);
	report("nested_cleared", count == 0 && polled == 0, "outer %d, poll %d",
		count, polled);

	// A blocked wait on the outer instance is woken through the inner one.
	DelayedWrite args = { efd, 100000, 1 };
	pthread_t thread;
	pthread_create(&thread, NULL, delayed_write, &args);
	count = epoll_wait(outer, &event, 1, 3000);
	pthread_join(thread, NULL);
	report("nested_blocking", count == 1 && event.data.u64 == 2, "%d", count);
	eventfd_read(efd, &value);

	// Loops are refused.
	EXPECT_ERRNO("nested_loop", ELOOP, add(inner, outer, EPOLLIN, 3));
	int third = epoll_create1(0);
	add(third, outer, EPOLLIN, 4);
	EXPECT_ERRNO("nested_loop_indirect", ELOOP, add(inner, third, EPOLLIN,
		5));

	// Chains deeper than Linux's limit are refused.
	int chain[7];
	for (int i = 0; i < 7; i++)
		chain[i] = epoll_create1(0);
	int depth = 0;
	for (int i = 1; i < 7; i++) {
		if (add(chain[i], chain[i - 1], EPOLLIN, i) != 0)
			break;
		depth++;
	}
	int depthErrno = errno;
	report("nested_depth_limit", depth >= 4 && depth < 6
		&& depthErrno == ELOOP, "%d levels, then %s", depth,
		errno_name(depthErrno));
	for (int i = 0; i < 7; i++)
		close(chain[i]);

	// Closing the inner epoll removes it from the outer one.
	close(inner);
	eventfd_write(efd, 1);
	int afterClose = peek(outer);
	report("nested_closed_gone", afterClose == 0, "%d events", afterClose);
	close(third);
	close(outer);
	close(efd);
}


// #pragma mark - close and fork


static void
test_close_and_fork()
{
	int ep = epoll_create1(0);
	int efd = eventfd(0, EFD_NONBLOCK);
	add(ep, efd, EPOLLIN, 1);

	// Registration belongs to the open file description: it survives
	// close() while a dup() keeps the description open...
	int dupFD = dup(efd);
	close(efd);
	eventfd_write(dupFD, 1);
	struct epoll_event event;
	int count = peek(ep, &event);
	report("close_dup_still_registered", count == 1 && event.data.u64 == 1,
		"%d", count);
	EXPECT_ERRNO("close_dup_del_other_number", ENOENT, epoll_ctl(ep,
		EPOLL_CTL_DEL, dupFD, NULL));

	// ...and goes with the last reference.
	close(dupFD);
	count = peek(ep);
	int reused = eventfd(5, EFD_NONBLOCK);
	errno = 0;
	int del = epoll_ctl(ep, EPOLL_CTL_DEL, reused, NULL);
	report("close_last_removes", count == 0 && del == -1 && errno == ENOENT,
		"%d events, DEL on fd %d: %s", count, reused, errno_name(errno));
	close(reused);

	// Closing many registered descriptors (some ready) leaves nothing.
	int ready = 0;
	for (int i = 0; i < 300; i++) {
		int fd = eventfd(i & 1, EFD_NONBLOCK);
		if (add(ep, fd, EPOLLIN | ((i & 2) ? EPOLLET : 0), 100 + i) == 0)
			ready++;
		close(fd);
	}
	count = peek(ep);
	report("close_many_registered", ready == 300 && count == 0,
		"%d added, %d events after close", ready, count);

	// fork(): the child shares the instance and its registrations.
	efd = eventfd(0, EFD_NONBLOCK);
	add(ep, efd, EPOLLIN | EPOLLET, 42);
	pid_t child = fork();
	if (child == 0) {
		struct epoll_event childEvent;
		int childCount = epoll_wait(ep, &childEvent, 1, 3000);
		int code = childCount == 1 && childEvent.data.u64 == 42 ? 0 : 1;
		eventfd_t value;
		eventfd_read(efd, &value);
		// Shared instance: this removes the parent's registration too.
		if (epoll_ctl(ep, EPOLL_CTL_DEL, efd, NULL) != 0)
			code |= 2;
		_exit(code);
	}
	usleep(100000);
	eventfd_write(efd, 1);
	int status = 0;
	waitpid(child, &status, 0);
	int readd = add(ep, efd, EPOLLIN, 43);
	report("fork_child_waits", child > 0 && WIFEXITED(status)
			&& WEXITSTATUS(status) == 0 && readd == 0,
		"child exit %d, parent re-add %d", WEXITSTATUS(status), readd);

	// The child's copy of a descriptor keeps the registration alive after
	// the parent closes its own; the child's exit then removes it.
	int pipes[2];
	pipe(pipes);
	add(ep, pipes[0], EPOLLIN, 44);
	child = fork();
	if (child == 0) {
		usleep(200000);
		write(pipes[1], "x", 1);
		usleep(200000);
		_exit(0);
	}
	close(pipes[0]);
	close(pipes[1]);
	count = epoll_wait(ep, &event, 1, 3000);
	bool reported = count == 1 && event.data.u64 == 44;
	waitpid(child, &status, 0);
	// the parent's eventfd item (43) is not ready
	count = peek(ep);
	report("fork_child_keeps_description", reported && count == 0,
		"reported %d, after child exit %d", reported, count);

	close(efd);
	close(ep);
}


// #pragma mark - many descriptors, threads, stress


static void
test_many()
{
	struct rlimit limit;
	getrlimit(RLIMIT_NOFILE, &limit);
	limit.rlim_cur = 4096;
	setrlimit(RLIMIT_NOFILE, &limit);

	const int kCount = 1000;
	int ep = epoll_create1(0);
	int fds[kCount];
	int created = 0;
	for (int i = 0; i < kCount; i++) {
		fds[i] = eventfd(0, EFD_NONBLOCK);
		if (fds[i] < 0 || add(ep, fds[i], EPOLLIN, i) != 0)
			break;
		created++;
	}
	report("many_add", created == kCount, "%d of %d", created, kCount);

	for (int i = 0; i < created; i += 2)
		eventfd_write(fds[i], 1);

	struct epoll_event* events = new struct epoll_event[kCount];
	int count = epoll_wait(ep, events, kCount, 0);
	bool exact = count == created / 2;
	for (int i = 0; exact && i < count; i++) {
		if (events[i].data.u64 % 2 != 0 || events[i].events != EPOLLIN)
			exact = false;
	}
	report("many_ready", exact, "%d events (want %d)", count, created / 2);

	// Level-triggered round robin: small batches cycle through all ready
	// descriptors.
	bool* seen = new bool[kCount]();
	int distinct = 0;
	for (int round = 0; round < 8; round++) {
		count = epoll_wait(ep, events, 64, 0);
		for (int i = 0; i < count; i++) {
			uint64_t index = events[i].data.u64;
			if (index < (uint64_t)kCount && !seen[index]) {
				seen[index] = true;
				distinct++;
			}
		}
	}
	report("many_round_robin", distinct == created / 2,
		"%d distinct in 8 batches of 64", distinct);

	bigtime_t start = system_time();
	for (int i = 0; i < 100; i++)
		epoll_wait(ep, events, kCount, 0);
	bigtime_t elapsed = system_time() - start;
	printf("CE-INFO epoll: 100 waits over %d ready of %d: %lld us\n",
		created / 2, created, (long long)elapsed);

	for (int i = 0; i < created; i++)
		close(fds[i]);
	count = epoll_wait(ep, events, kCount, 0);
	report("many_closed", count == 0, "%d", count);

	delete[] seen;
	delete[] events;
	close(ep);
}


struct Consumer {
	int				ep;
	int				quit;
	volatile uint64	total;
	int				wakeups;
};


static void*
consumer_thread(void* data)
{
	Consumer* consumer = (Consumer*)data;
	while (true) {
		struct epoll_event events[8];
		int count = epoll_wait(consumer->ep, events, 8, 5000);
		if (count <= 0)
			return NULL;
		consumer->wakeups++;
		for (int i = 0; i < count; i++) {
			int fd = (int)events[i].data.u64;
			if (fd == consumer->quit)
				return NULL;
			eventfd_t value;
			if (eventfd_read(fd, &value) == 0)
				atomic_add64((int64*)&consumer->total, value);
		}
	}
}


static void
test_threads()
{
	// Several threads wait on one instance; every increment is consumed
	// exactly once.
	for (int mode = 0; mode < 2; mode++) {
		uint32_t trigger = mode == 0 ? 0 : EPOLLET;
		int ep = epoll_create1(0);
		const int kFDs = 8;
		int fds[kFDs];
		for (int i = 0; i < kFDs; i++) {
			fds[i] = eventfd(0, EFD_NONBLOCK);
			add(ep, fds[i], EPOLLIN | trigger, fds[i]);
		}
		Consumer consumer = { ep, eventfd(0, EFD_NONBLOCK), 0, 0 };

		const int kThreads = 4;
		pthread_t threads[kThreads];
		for (int i = 0; i < kThreads; i++)
			pthread_create(&threads[i], NULL, consumer_thread, &consumer);

		const int kWrites = 4000;
		for (int i = 0; i < kWrites; i++) {
			eventfd_write(fds[i % kFDs], 1);
			if (i % 100 == 0)
				usleep(1000);
		}
		bigtime_t start = system_time();
		while (atomic_get64((int64*)&consumer.total) < kWrites
			&& system_time() - start < 10000000) {
			usleep(5000);
		}
		uint64 total = atomic_get64((int64*)&consumer.total);

		// The quit eventfd is level-triggered and never read: it wakes
		// every consumer.
		add(ep, consumer.quit, EPOLLIN, consumer.quit);
		eventfd_write(consumer.quit, 1);
		for (int i = 0; i < kThreads; i++)
			pthread_join(threads[i], NULL);

		report(mode == 0 ? "threads_lt" : "threads_et", total == kWrites,
			"consumed %llu of %d in %d wakeups", (unsigned long long)total,
			kWrites, consumer.wakeups);

		for (int i = 0; i < kFDs; i++)
			close(fds[i]);
		close(consumer.quit);
		close(ep);
	}
}


struct Churn {
	int				ep;
	volatile bool	stop;
	int				operations;
};


static void*
churn_thread(void* data)
{
	// Adds, modifies, deletes and closes registered descriptors while
	// another thread waits on the same instance.
	Churn* churn = (Churn*)data;
	while (!churn->stop) {
		int fds[16];
		for (int i = 0; i < 16; i++) {
			fds[i] = eventfd(i & 1, EFD_NONBLOCK);
			add(churn->ep, fds[i], EPOLLIN | (i & 2 ? EPOLLET : 0)
				| (i & 4 ? EPOLLONESHOT : 0), 1000000 + i);
		}
		for (int i = 0; i < 16; i += 3)
			modify(churn->ep, fds[i], EPOLLIN | EPOLLOUT, 2000000 + i);
		for (int i = 0; i < 16; i += 2)
			epoll_ctl(churn->ep, EPOLL_CTL_DEL, fds[i], NULL);
		for (int i = 0; i < 16; i++)
			close(fds[i]);
		churn->operations += 16;
	}
	return NULL;
}


static void
test_stress()
{
	int ep = epoll_create1(0);
	Churn churn = { ep, false, 0 };
	pthread_t threads[2];
	pthread_create(&threads[0], NULL, churn_thread, &churn);
	pthread_create(&threads[1], NULL, churn_thread, &churn);

	// A producer/consumer pair runs on the same instance meanwhile.
	int efd = eventfd(0, EFD_NONBLOCK);
	add(ep, efd, EPOLLIN | EPOLLET, 7);
	DelayedWrite producer = { efd, 0, 20000 };
	pthread_t producerThread;
	pthread_create(&producerThread, NULL, delayed_write, &producer);

	uint64_t total = 0;
	int waits = 0;
	bigtime_t start = system_time();
	while (total < 20000 && system_time() - start < 20000000) {
		struct epoll_event events[32];
		int count = epoll_wait(ep, events, 32, 100);
		waits++;
		for (int i = 0; i < count; i++) {
			if (events[i].data.u64 != 7)
				continue;
			eventfd_t value;
			while (eventfd_read(efd, &value) == 0)
				total += value;
		}
	}
	pthread_join(producerThread, NULL);
	churn.stop = true;
	pthread_join(threads[0], NULL);
	pthread_join(threads[1], NULL);

	report("stress", total == 20000, "summed %llu of 20000 in %d waits, "
		"%d churned registrations, %lld ms", (unsigned long long)total, waits,
		churn.operations, (long long)(system_time() - start) / 1000);

	// Everything the churn threads registered was closed again.
	epoll_ctl(ep, EPOLL_CTL_DEL, efd, NULL);
	struct epoll_event events[32];
	int left = epoll_wait(ep, events, 32, 0);
	report("stress_clean", left == 0, "%d events left", left);

	close(efd);
	close(ep);
}


int
main()
{
	printf("CE-SUITE " SUITE " START\n");
	fflush(stdout);

	test_arguments();
	test_level_edge_oneshot();
	test_timeouts();
	test_descriptor_types();
	test_nesting();
	test_close_and_fork();
	test_many();
	test_threads();
	test_stress();

	printf("CE-SUITE " SUITE " DONE failures=%d\n", sFailures);
	fflush(stdout);
	return sFailures == 0 ? 0 : 1;
}
