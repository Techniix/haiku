/*
 * Copyright 2026, Haiku Community Edition contributors.
 * Distributed under the terms of the MIT License.
 */

/*!	In-guest conformance test for eventfd (kernel object + libgnu API).

	Checks the behaviour Linux documents in eventfd(2), which FreeBSD also
	implements, and which ports such as glib, libuv, Chromium and Wine rely
	on. Output format: see 3rdparty/ce/README.md.
*/


#include <sys/eventfd.h>

#include <OS.h>

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>


#define SUITE "eventfd"


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
poll_events(int fd)
{
	struct pollfd pfd = { fd, POLLIN | POLLOUT, 0 };
	if (poll(&pfd, 1, 0) < 0)
		return -1;
	return pfd.revents;
}


struct WriterArgs {
	int			fd;
	bigtime_t	delay;
	eventfd_t	value;
	int			count;
};


static status_t
writer_thread(void* data)
{
	WriterArgs* args = (WriterArgs*)data;
	snooze(args->delay);
	for (int i = 0; i < args->count; i++)
		eventfd_write(args->fd, args->value);
	return B_OK;
}


int
main()
{
	printf("CE-SUITE " SUITE " START\n");
	fflush(stdout);

	// Creation and flag validation
	int fd = eventfd(0, 0);
	report("create", fd >= 0, "%s", fd >= 0 ? "" : strerror(errno));
	if (fd < 0) {
		printf("CE-SUITE " SUITE " DONE failures=%d\n", sFailures);
		return 1;
	}

	errno = 0;
	int bad = eventfd(0, 0x10000000);
	report("reject_bad_flags", bad < 0 && errno == EINVAL, "%s",
		strerror(errno));

	// The counter accumulates writes; a read returns and clears it.
	eventfd_write(fd, 3);
	eventfd_write(fd, 4);
	eventfd_t value = 0;
	int result = eventfd_read(fd, &value);
	report("accumulate", result == 0 && value == 7, "read %llu",
		(unsigned long long)value);

	// Initial value
	int initFD = eventfd(42, EFD_NONBLOCK);
	value = 0;
	result = eventfd_read(initFD, &value);
	report("initial_value", result == 0 && value == 42, "read %llu",
		(unsigned long long)value);
	errno = 0;
	result = eventfd_read(initFD, &value);
	report("nonblock_empty", result < 0 && errno == EAGAIN, "%s",
		strerror(errno));
	close(initFD);

	// Semaphore mode: every read returns 1.
	int semFD = eventfd(3, EFD_SEMAPHORE | EFD_NONBLOCK);
	int reads = 0;
	while (eventfd_read(semFD, &value) == 0 && value == 1 && reads < 10)
		reads++;
	report("semaphore", reads == 3 && errno == EAGAIN, "%d reads of 1", reads);
	close(semFD);

	// Size and value validation
	uint32 small = 0;
	errno = 0;
	ssize_t bytes = read(fd, &small, sizeof(small));
	report("short_read", bytes < 0 && errno == EINVAL, "%s", strerror(errno));
	errno = 0;
	bytes = write(fd, &small, sizeof(small));
	report("short_write", bytes < 0 && errno == EINVAL, "%s",
		strerror(errno));
	errno = 0;
	result = eventfd_write(fd, UINT64_MAX);
	report("reject_max_value", result < 0 && errno == EINVAL, "%s",
		strerror(errno));

	// Overflow: the counter maxes out at 2^64 - 2; more blocks.
	int fullFD = eventfd(0, EFD_NONBLOCK);
	result = eventfd_write(fullFD, UINT64_MAX - 1);
	errno = 0;
	int overflow = eventfd_write(fullFD, 1);
	short revents = poll_events(fullFD);
	report("overflow", result == 0 && overflow < 0 && errno == EAGAIN
			&& (revents & POLLIN) != 0 && (revents & POLLOUT) == 0,
		"write: %s, poll 0x%x", strerror(errno), revents);
	close(fullFD);

	// poll() and select() readiness
	revents = poll_events(fd);
	bool emptyOk = (revents & POLLIN) == 0 && (revents & POLLOUT) != 0;
	eventfd_write(fd, 1);
	revents = poll_events(fd);
	bool fullOk = (revents & POLLIN) != 0;
	report("poll", emptyOk && fullOk, "revents 0x%x", revents);

	fd_set readSet;
	FD_ZERO(&readSet);
	FD_SET(fd, &readSet);
	struct timeval timeout = { 0, 0 };
	int ready = select(fd + 1, &readSet, NULL, NULL, &timeout);
	report("select", ready == 1 && FD_ISSET(fd, &readSet), "ready %d", ready);
	eventfd_read(fd, &value);

	// A blocked read is woken by a write from another thread.
	WriterArgs args = { fd, 100000, 5, 1 };
	thread_id thread = spawn_thread(writer_thread, "eventfd writer",
		B_NORMAL_PRIORITY, &args);
	bigtime_t start = system_time();
	resume_thread(thread);
	value = 0;
	result = eventfd_read(fd, &value);
	bigtime_t waited = system_time() - start;
	status_t threadResult;
	wait_for_thread(thread, &threadResult);
	report("blocking_read", result == 0 && value == 5 && waited >= 80000,
		"read %llu after %lld us", (unsigned long long)value,
		(long long)waited);

	// A blocked poll() is woken as well.
	args.delay = 100000;
	thread = spawn_thread(writer_thread, "eventfd writer",
		B_NORMAL_PRIORITY, &args);
	resume_thread(thread);
	struct pollfd pfd = { fd, POLLIN, 0 };
	start = system_time();
	ready = poll(&pfd, 1, 3000);
	waited = system_time() - start;
	wait_for_thread(thread, &threadResult);
	report("blocking_poll", ready == 1 && (pfd.revents & POLLIN) != 0
			&& waited >= 80000 && waited < 2000000,
		"ready %d after %lld us", ready, (long long)waited);
	eventfd_read(fd, &value);

	// fcntl(): close-on-exec and switching to non-blocking at runtime
	int cloexecFD = eventfd(0, EFD_CLOEXEC);
	report("cloexec", (fcntl(cloexecFD, F_GETFD) & FD_CLOEXEC) != 0
			&& (fcntl(fd, F_GETFD) & FD_CLOEXEC) == 0, "");
	close(cloexecFD);

	int flags = fcntl(fd, F_GETFL);
	int setResult = fcntl(fd, F_SETFL, flags | O_NONBLOCK);
	errno = 0;
	result = eventfd_read(fd, &value);
	report("set_nonblock", setResult == 0 && result < 0 && errno == EAGAIN,
		"fcntl %d, read: %s", setResult, strerror(errno));
	fcntl(fd, F_SETFL, flags);

	// fstat() works and reports a FIFO-like object.
	// (fstat() first: argument evaluation order is unspecified.)
	struct stat st;
	memset(&st, 0, sizeof(st));
	int statResult = fstat(fd, &st);
	report("fstat", statResult == 0 && S_ISFIFO(st.st_mode),
		"mode 0%o", (unsigned)st.st_mode);

	// The counter is shared across fork().
	pid_t child = fork();
	if (child == 0) {
		eventfd_write(fd, 11);
		_exit(0);
	}
	int childStatus = 0;
	waitpid(child, &childStatus, 0);
	value = 0;
	result = eventfd_read(fd, &value);
	report("fork_shared", child > 0 && result == 0 && value == 11,
		"read %llu", (unsigned long long)value);

	// Stress: one writer thread adding 1 many times, the reader summing.
	const int kIterations = 20000;
	WriterArgs stress = { fd, 0, 1, kIterations };
	thread = spawn_thread(writer_thread, "eventfd stress",
		B_NORMAL_PRIORITY, &stress);
	resume_thread(thread);
	uint64 total = 0;
	start = system_time();
	while (total < (uint64)kIterations && system_time() - start < 20000000) {
		if (eventfd_read(fd, &value) == 0)
			total += value;
	}
	wait_for_thread(thread, &threadResult);
	report("stress", total == (uint64)kIterations, "summed %llu of %d",
		(unsigned long long)total, kIterations);

	close(fd);

	printf("CE-SUITE " SUITE " DONE failures=%d\n", sFailures);
	fflush(stdout);
	return sFailures == 0 ? 0 : 1;
}
