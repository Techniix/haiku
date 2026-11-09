/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 */

/*!	In-guest conformance test for signalfd (kernel object + libgnu API).

	The expected results are Linux's (signalfd(2)), which event loops use to
	handle SIGCHLD, SIGTERM and friends in the loop instead of in handlers
	(systemd-style daemons, libuv's signal watchers in some builds, glib's
	unix signal sources, Wine's server). Output format: see
	3rdparty/ce/README.md.
*/


#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/signalfd.h>

#include <OS.h>

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>


#define SUITE "signalfd"


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


static const char*
errno_name(int error)
{
	switch (error) {
		case 0: return "0";
		case EBADF: return "EBADF";
		case EINVAL: return "EINVAL";
		case EAGAIN: return "EAGAIN";
		case EFAULT: return "EFAULT";
		case EINTR: return "EINTR";
	}
	return strerror(error);
}


#define EXPECT_ERRNO(name, expected, call) \
	do { \
		errno = 0; \
		int _result = (call); \
		report(name, _result == -1 && errno == (expected), \
			"result %d, errno %s (want %s)", _result, errno_name(errno), \
			errno_name(expected)); \
	} while (false)


static sigset_t
mask_of(int a, int b = 0, int c = 0)
{
	sigset_t set;
	sigemptyset(&set);
	if (a > 0)
		sigaddset(&set, a);
	if (b > 0)
		sigaddset(&set, b);
	if (c > 0)
		sigaddset(&set, c);
	return set;
}


static bool
readable(int fd)
{
	struct pollfd pfd = { fd, POLLIN, 0 };
	return poll(&pfd, 1, 0) == 1 && (pfd.revents & POLLIN) != 0;
}


static bool
is_pending(int signal)
{
	sigset_t pending;
	sigpending(&pending);
	return sigismember(&pending, signal);
}


// #pragma mark -


static void
test_basics()
{
	sigset_t usr1 = mask_of(SIGUSR1);
	int fd = signalfd(-1, &usr1, SFD_NONBLOCK);
	report("create", fd >= 0, "%s", fd >= 0 ? "" : strerror(errno));

	EXPECT_ERRNO("bad_flags", EINVAL, signalfd(-1, &usr1, 0x10));
	EXPECT_ERRNO("bad_fd", EBADF, signalfd(9999, &usr1, 0));
	int pipes[2];
	pipe(pipes);
	EXPECT_ERRNO("not_signalfd", EINVAL, signalfd(pipes[0], &usr1, 0));
	close(pipes[0]);
	close(pipes[1]);
	EXPECT_ERRNO("null_mask", EFAULT, signalfd(-1, NULL, 0));

	int cloexec = signalfd(-1, &usr1, SFD_CLOEXEC);
	report("cloexec", (fcntl(cloexec, F_GETFD) & FD_CLOEXEC) != 0
		&& (fcntl(fd, F_GETFD) & FD_CLOEXEC) == 0, "");
	close(cloexec);

	struct signalfd_siginfo info;
	EXPECT_ERRNO("nonblock_empty", EAGAIN, (int)read(fd, &info,
		sizeof(info)));
	EXPECT_ERRNO("short_buffer", EINVAL, (int)read(fd, &info, 64));
	EXPECT_ERRNO("write_fails", EINVAL, (int)write(fd, &info, sizeof(info)));
	struct stat st;
	report("fstat", fstat(fd, &st) == 0, "mode 0%o", (unsigned)st.st_mode);
	report("idle_not_readable", !readable(fd), "");

	// A blocked, thread-directed signal is read instead of delivered.
	raise(SIGUSR1);
	bool pollReady = readable(fd);
	memset(&info, 0, sizeof(info));
	ssize_t bytes = read(fd, &info, sizeof(info));
	report("raise_read", pollReady && bytes == (ssize_t)sizeof(info)
			&& info.ssi_signo == (uint32_t)SIGUSR1
			&& info.ssi_code == SI_USER
			&& info.ssi_pid == (uint32_t)getpid()
			&& !is_pending(SIGUSR1),
		"poll %d, %zd bytes, signo %u code %d pid %u, still pending %d",
		pollReady, bytes, info.ssi_signo, info.ssi_code, info.ssi_pid,
		is_pending(SIGUSR1));
	report("consumed", !readable(fd), "");

	// Process-directed.
	kill(getpid(), SIGUSR1);
	bytes = read(fd, &info, sizeof(info));
	report("kill_read", bytes == (ssize_t)sizeof(info)
		&& info.ssi_signo == (uint32_t)SIGUSR1, "%zd bytes signo %u", bytes,
		info.ssi_signo);

	// sigqueue() value.
	union sigval value;
	value.sival_int = 4242;
	sigqueue(getpid(), SIGUSR1, value);
	bytes = read(fd, &info, sizeof(info));
	report("sigqueue_value", bytes == (ssize_t)sizeof(info)
			&& info.ssi_code == SI_QUEUE && info.ssi_int == 4242,
		"%zd bytes code %d int %d", bytes, info.ssi_code, info.ssi_int);

	// Signals outside the mask stay pending; changing the mask on the same
	// descriptor makes them readable.
	raise(SIGUSR2);
	errno = 0;
	bytes = read(fd, &info, sizeof(info));
	int outsideErrno = errno;
	sigset_t both = mask_of(SIGUSR1, SIGUSR2);
	int same = signalfd(fd, &both, 0);
	bool nowReadable = readable(fd);
	ssize_t second = read(fd, &info, sizeof(info));
	report("mask_change", bytes == -1 && outsideErrno == EAGAIN && same == fd
			&& nowReadable && second == (ssize_t)sizeof(info)
			&& info.ssi_signo == (uint32_t)SIGUSR2,
		"before %s, signalfd() %d, readable %d, read signo %u",
		errno_name(outsideErrno), same, nowReadable, info.ssi_signo);

	// Several records in one read.
	raise(SIGUSR1);
	raise(SIGUSR2);
	struct signalfd_siginfo records[4];
	bytes = read(fd, records, sizeof(records));
	report("multiple_records", bytes == 2 * (ssize_t)sizeof(info)
			&& records[0].ssi_signo != records[1].ssi_signo,
		"%zd bytes", bytes);

	// SIGKILL and SIGSTOP in the mask are silently ignored.
	sigset_t withKill = mask_of(SIGUSR1, SIGKILL, SIGSTOP);
	int killFD = signalfd(-1, &withKill, SFD_NONBLOCK);
	report("kill_in_mask_ignored", killFD >= 0, "%s",
		killFD >= 0 ? "" : strerror(errno));
	close(killFD);

	close(fd);
}


struct Sender {
	pid_t		pid;
	pthread_t	thread;
	int			signal;
	bigtime_t	delay;
	bool		toThread;
};


static void*
send_later(void* data)
{
	Sender* sender = (Sender*)data;
	usleep(sender->delay);
	if (sender->toThread)
		pthread_kill(sender->thread, sender->signal);
	else
		kill(sender->pid, sender->signal);
	return NULL;
}


struct ThreadReader {
	int			fd;
	int			signo;
	volatile bool ready;
};


static void*
read_in_thread(void* data)
{
	ThreadReader* reader = (ThreadReader*)data;
	reader->ready = true;
	struct pollfd pfd = { reader->fd, POLLIN, 0 };
	if (poll(&pfd, 1, 3000) == 1) {
		struct signalfd_siginfo info;
		if (read(reader->fd, &info, sizeof(info)) == sizeof(info))
			reader->signo = info.ssi_signo;
	}
	return NULL;
}


static void
test_waiting()
{
	sigset_t usr1 = mask_of(SIGUSR1);
	int fd = signalfd(-1, &usr1, 0);

	// Blocking read woken by a signal from another thread.
	Sender sender = { getpid(), pthread_self(), SIGUSR1, 100000, false };
	pthread_t thread;
	pthread_create(&thread, NULL, send_later, &sender);
	struct signalfd_siginfo info;
	bigtime_t start = system_time();
	ssize_t bytes = read(fd, &info, sizeof(info));
	bigtime_t waited = system_time() - start;
	pthread_join(thread, NULL);
	report("blocking_read", bytes == (ssize_t)sizeof(info)
			&& info.ssi_signo == (uint32_t)SIGUSR1 && waited >= 80000,
		"%zd bytes after %lld us", bytes, (long long)waited);

	// Blocking poll() woken.
	pthread_create(&thread, NULL, send_later, &sender);
	struct pollfd pfd = { fd, POLLIN, 0 };
	start = system_time();
	int ready = poll(&pfd, 1, 3000);
	waited = system_time() - start;
	pthread_join(thread, NULL);
	read(fd, &info, sizeof(info));
	report("blocking_poll", ready == 1 && waited >= 80000
		&& waited < 2500000, "%d after %lld us", ready, (long long)waited);

	// select()
	raise(SIGUSR1);
	fd_set readSet;
	FD_ZERO(&readSet);
	FD_SET(fd, &readSet);
	struct timeval timeout = { 0, 0 };
	int selected = select(fd + 1, &readSet, NULL, NULL, &timeout);
	read(fd, &info, sizeof(info));
	report("select", selected == 1, "%d", selected);

	// epoll, level-triggered, woken while blocked.
	int ep = epoll_create1(0);
	struct epoll_event event = {};
	event.events = EPOLLIN;
	event.data.u64 = 5;
	epoll_ctl(ep, EPOLL_CTL_ADD, fd, &event);
	struct epoll_event got = {};
	int idle = epoll_wait(ep, &got, 1, 0);
	pthread_create(&thread, NULL, send_later, &sender);
	start = system_time();
	int count = epoll_wait(ep, &got, 1, 3000);
	waited = system_time() - start;
	pthread_join(thread, NULL);
	int stillReady = epoll_wait(ep, &got, 1, 0);
	read(fd, &info, sizeof(info));
	int afterRead = epoll_wait(ep, &got, 1, 0);
	report("epoll", idle == 0 && count == 1 && got.data.u64 == 5
			&& waited >= 80000 && stillReady == 1 && afterRead == 0,
		"idle %d, woken %d after %lld us, level %d, after read %d", idle,
		count, (long long)waited, stillReady, afterRead);

	// Together with an eventfd in one epoll set.
	int efd = eventfd(0, EFD_NONBLOCK);
	event.data.u64 = 6;
	epoll_ctl(ep, EPOLL_CTL_ADD, efd, &event);
	eventfd_write(efd, 1);
	raise(SIGUSR1);
	struct epoll_event both[4];
	count = epoll_wait(ep, both, 4, 0);
	read(fd, &info, sizeof(info));
	report("epoll_with_eventfd", count == 2, "%d events", count);
	close(efd);
	close(ep);

	// A signal sent to another thread is readable for that thread only.
	ThreadReader reader = { fd, 0, false };
	pthread_create(&thread, NULL, read_in_thread, &reader);
	while (!reader.ready)
		usleep(1000);
	usleep(50000);
	pthread_kill(thread, SIGUSR1);
	usleep(50000);
	bool mainReadable = readable(fd);
	pthread_join(thread, NULL);
	report("thread_directed_other_thread", reader.signo == SIGUSR1
		&& !mainReadable, "reader got %d, main readable %d", reader.signo,
		mainReadable);

	close(fd);
}


static void
test_sigchld_and_fork()
{
	sigset_t chld = mask_of(SIGCHLD);
	int fd = signalfd(-1, &chld, 0);

	pid_t child = fork();
	if (child == 0)
		_exit(7);
	// Haiku interrupts blocking calls on SIGCHLD even while it is blocked
	// (inherited upstream behaviour, see PORTING.md): retry on EINTR, as
	// event loops do.
	struct pollfd pfd = { fd, POLLIN, 0 };
	int ready;
	int interruptions = 0;
	while ((ready = poll(&pfd, 1, 3000)) < 0 && errno == EINTR
		&& interruptions < 10) {
		interruptions++;
	}
	struct signalfd_siginfo info;
	memset(&info, 0, sizeof(info));
	ssize_t bytes = read(fd, &info, sizeof(info));
	int status = 0;
	waitpid(child, &status, 0);
	report("sigchld", ready == 1 && bytes == (ssize_t)sizeof(info)
			&& info.ssi_signo == (uint32_t)SIGCHLD
			&& info.ssi_pid == (uint32_t)child && info.ssi_status == 7
			&& info.ssi_code == CLD_EXITED,
		"poll %d (%d EINTR), signo %u pid %u (child %d) status %d code %d",
		ready, interruptions, info.ssi_signo, info.ssi_pid, (int)child, info.ssi_status,
		info.ssi_code);
	close(fd);

	// The child reads its own signals through the inherited descriptor.
	sigset_t usr1 = mask_of(SIGUSR1);
	fd = signalfd(-1, &usr1, SFD_NONBLOCK);
	child = fork();
	if (child == 0) {
		raise(SIGUSR1);
		struct signalfd_siginfo childInfo;
		ssize_t childBytes = read(fd, &childInfo, sizeof(childInfo));
		_exit(childBytes == (ssize_t)sizeof(childInfo)
			&& childInfo.ssi_signo == (uint32_t)SIGUSR1 ? 0 : 1);
	}
	waitpid(child, &status, 0);
	errno = 0;
	bytes = read(fd, &info, sizeof(info));
	report("fork_child_reads_own", WIFEXITED(status)
			&& WEXITSTATUS(status) == 0 && bytes == -1 && errno == EAGAIN,
		"child exit %d, parent read %zd (%s)", WEXITSTATUS(status), bytes,
		errno_name(errno));
	close(fd);
}


struct Queuer {
	pid_t	pid;
	int		count;
	int		retries;
};


static void*
queue_signals(void* data)
{
	Queuer* queuer = (Queuer*)data;
	for (int i = 1; i <= queuer->count; i++) {
		union sigval value;
		value.sival_int = i;
		while (sigqueue(queuer->pid, SIGRTMIN, value) != 0) {
			queuer->retries++;
			usleep(1000);
		}
	}
	return NULL;
}


static void
test_stress()
{
	// Realtime signals queue: every one must be read exactly once.
	sigset_t rt = mask_of(SIGRTMIN);
	sigset_t old;
	pthread_sigmask(SIG_BLOCK, &rt, &old);
	int fd = signalfd(-1, &rt, SFD_NONBLOCK);
	int ep = epoll_create1(0);
	struct epoll_event event = {};
	event.events = EPOLLIN;
	epoll_ctl(ep, EPOLL_CTL_ADD, fd, &event);

	const int kCount = 2000;
	Queuer queuer = { getpid(), kCount, 0 };
	pthread_t thread;
	pthread_create(&thread, NULL, queue_signals, &queuer);

	int received = 0;
	int64 sum = 0;
	bool inOrder = true;
	int last = 0;
	bigtime_t start = system_time();
	while (received < kCount && system_time() - start < 30000000) {
		struct epoll_event got;
		if (epoll_wait(ep, &got, 1, 1000) != 1)
			continue;
		struct signalfd_siginfo records[16];
		ssize_t bytes = read(fd, records, sizeof(records));
		for (ssize_t i = 0; i < bytes / (ssize_t)sizeof(records[0]); i++) {
			received++;
			sum += records[i].ssi_int;
			if (records[i].ssi_int != last + 1)
				inOrder = false;
			last = records[i].ssi_int;
		}
	}
	pthread_join(thread, NULL);
	int64 expected = (int64)kCount * (kCount + 1) / 2;
	report("stress_rt_queue", received == kCount && sum == expected
			&& inOrder,
		"%d of %d, sum %lld (want %lld), in order %d, %d sender retries, "
		"%lld ms", received, kCount, (long long)sum, (long long)expected,
		inOrder, queuer.retries, (long long)(system_time() - start) / 1000);

	close(ep);
	close(fd);
	pthread_sigmask(SIG_SETMASK, &old, NULL);

	// Creating and closing many descriptors leaves nothing behind.
	int created = 0;
	for (int i = 0; i < 500; i++) {
		int many = signalfd(-1, &rt, 0);
		if (many >= 0) {
			created++;
			close(many);
		}
	}
	report("create_close_many", created == 500, "%d of 500", created);
}


int
main()
{
	printf("CE-SUITE " SUITE " START\n");
	fflush(stdout);

	// signalfd wants its signals blocked (inherited by the threads below).
	sigset_t blocked;
	sigemptyset(&blocked);
	sigaddset(&blocked, SIGUSR1);
	sigaddset(&blocked, SIGUSR2);
	sigaddset(&blocked, SIGCHLD);
	pthread_sigmask(SIG_BLOCK, &blocked, NULL);

	test_basics();
	test_waiting();
	test_sigchld_and_fork();
	test_stress();

	printf("CE-SUITE " SUITE " DONE failures=%d\n", sFailures);
	fflush(stdout);
	return sFailures == 0 ? 0 : 1;
}
