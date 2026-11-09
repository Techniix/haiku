/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 */

/*!	In-guest conformance test for timerfd (kernel object + libgnu API).

	Checks the behaviour Linux documents in timerfd_create(2), which event
	loops such as libuv, glib, systemd-style daemons and Wine rely on.

	The harness runs QEMU without hardware acceleration, so timing checks use
	wide margins: they look for "fired, not early, roughly on time", never
	for precise latency. Output format: see 3rdparty/ce/README.md.
*/


#include <sys/timerfd.h>

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
#include <time.h>
#include <unistd.h>


#define SUITE "timerfd"


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


static struct itimerspec
make_spec(bigtime_t value, bigtime_t interval)
{
	struct itimerspec spec;
	spec.it_value.tv_sec = value / 1000000;
	spec.it_value.tv_nsec = (value % 1000000) * 1000;
	spec.it_interval.tv_sec = interval / 1000000;
	spec.it_interval.tv_nsec = (interval % 1000000) * 1000;
	return spec;
}


static bigtime_t
to_usecs(const struct timespec& time)
{
	return (bigtime_t)time.tv_sec * 1000000 + time.tv_nsec / 1000;
}


static bigtime_t
clock_usecs(clockid_t clock)
{
	struct timespec now;
	clock_gettime(clock, &now);
	return to_usecs(now);
}


//!	Reads the expiration count; returns -1 (errno set) on failure.
static int64
read_count(int fd)
{
	uint64_t count = 0;
	ssize_t bytes = read(fd, &count, sizeof(count));
	if (bytes != (ssize_t)sizeof(count))
		return -1;
	return (int64)count;
}


static int
set_relative(int fd, bigtime_t value, bigtime_t interval,
	struct itimerspec* old = NULL)
{
	struct itimerspec spec = make_spec(value, interval);
	return timerfd_settime(fd, 0, &spec, old);
}


static void
test_create()
{
	errno = 0;
	int fd = timerfd_create(CLOCK_PROCESS_CPUTIME_ID, 0);
	int clockErrno = errno;
	if (fd >= 0)
		close(fd);
	report("create_bad_clock", fd < 0 && clockErrno == EINVAL,
		"fd %d errno %s", fd, strerror(clockErrno));

	errno = 0;
	fd = timerfd_create(CLOCK_MONOTONIC, 0x40000000);
	int flagsErrno = errno;
	if (fd >= 0)
		close(fd);
	report("create_bad_flags", fd < 0 && flagsErrno == EINVAL,
		"fd %d errno %s", fd, strerror(flagsErrno));
}


static void
test_disarmed(int fd)
{
	struct itimerspec spec;
	memset(&spec, 0xff, sizeof(spec));
	int result = timerfd_gettime(fd, &spec);
	report("gettime_disarmed", result == 0 && to_usecs(spec.it_value) == 0
			&& to_usecs(spec.it_interval) == 0,
		"value %lld interval %lld", (long long)to_usecs(spec.it_value),
		(long long)to_usecs(spec.it_interval));

	errno = 0;
	int64 count = read_count(fd);
	report("read_nonblock_eagain", count < 0 && errno == EAGAIN,
		"result %lld errno %s", (long long)count, strerror(errno));
}


static void
test_oneshot(int fd)
{
	// The descriptor is non-blocking; flip it to blocking for this check.
	fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) & ~O_NONBLOCK);

	bigtime_t start = system_time();
	int result = set_relative(fd, 50000, 0);
	int64 count = read_count(fd);
	bigtime_t elapsed = system_time() - start;
	report("oneshot_relative", result == 0 && count == 1 && elapsed >= 50000
			&& elapsed < 5000000,
		"count %lld after %lld us", (long long)count, (long long)elapsed);

	// A one-shot timer is disarmed once it has fired.
	struct itimerspec spec;
	timerfd_gettime(fd, &spec);
	report("oneshot_disarms", to_usecs(spec.it_value) == 0,
		"value %lld", (long long)to_usecs(spec.it_value));

	fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
}


static void
test_gettime_settime(int fd)
{
	set_relative(fd, 2000000, 500000);
	struct itimerspec spec;
	int result = timerfd_gettime(fd, &spec);
	bigtime_t value = to_usecs(spec.it_value);
	bigtime_t interval = to_usecs(spec.it_interval);
	report("gettime_armed", result == 0 && value > 0 && value <= 2000000
			&& interval == 500000,
		"value %lld interval %lld", (long long)value, (long long)interval);

	// settime reports the previous setting and replaces it.
	struct itimerspec old;
	memset(&old, 0, sizeof(old));
	result = set_relative(fd, 3000000, 0, &old);
	value = to_usecs(old.it_value);
	interval = to_usecs(old.it_interval);
	report("settime_old_value", result == 0 && value > 0 && value <= 2000000
			&& interval == 500000,
		"old value %lld interval %lld", (long long)value, (long long)interval);

	// Zero it_value disarms; nothing fires afterwards.
	result = set_relative(fd, 0, 0, &old);
	snooze(100000);
	errno = 0;
	int64 count = read_count(fd);
	timerfd_gettime(fd, &spec);
	report("disarm", result == 0 && to_usecs(old.it_value) > 0 && count < 0
			&& errno == EAGAIN && to_usecs(spec.it_value) == 0,
		"old value %lld read %lld (%s)", (long long)to_usecs(old.it_value),
		(long long)count, strerror(errno));
}


static void
test_periodic(int fd)
{
	// 20 ms period; sleep about 10 periods without reading. Every period
	// must be counted, even though nobody read in between.
	const bigtime_t kPeriod = 20000;
	bigtime_t before = system_time();
	set_relative(fd, kPeriod, kPeriod);
	bigtime_t after = system_time();
	snooze(10 * kPeriod + kPeriod / 2);
	bigtime_t readStart = system_time();
	int64 count = read_count(fd);
	bigtime_t readEnd = system_time();

	// Lower bound: periods that surely elapsed before the read started.
	// Upper bound: periods that could have elapsed until it returned.
	int64 low = (readStart - after) / kPeriod - 1;
	int64 high = (readEnd - before) / kPeriod + 1;
	report("periodic_count", count >= low && count <= high && count > 0,
		"count %lld expected %lld..%lld", (long long)count, (long long)low,
		(long long)high);

	// Reading resets the count; the timer keeps running.
	errno = 0;
	int64 again = read_count(fd);
	snooze(3 * kPeriod);
	int64 later = read_count(fd);
	report("periodic_reset_continue", (again < 0 || again <= 2) && later >= 1,
		"immediate %lld, after 3 periods %lld", (long long)again,
		(long long)later);

	set_relative(fd, 0, 0);
}


static void
test_absolute(int fd, clockid_t clock, const char* name)
{
	int clockFD = timerfd_create(clock, 0);
	if (clockFD < 0) {
		report(name, false, "create: %s", strerror(errno));
		return;
	}

	bigtime_t target = clock_usecs(clock) + 80000;
	struct itimerspec spec = make_spec(target, 0);
	int result = timerfd_settime(clockFD, TFD_TIMER_ABSTIME, &spec, NULL);
	int64 count = read_count(clockFD);
	bigtime_t now = clock_usecs(clock);
	report(name, result == 0 && count == 1 && now >= target
			&& now - target < 5000000,
		"count %lld, %lld us after target", (long long)count,
		(long long)(now - target));
	close(clockFD);
}


static void
test_absolute_past(int fd)
{
	// An absolute time in the past expires immediately.
	struct itimerspec spec = make_spec(1000000, 0);
	int result = timerfd_settime(fd, TFD_TIMER_ABSTIME, &spec, NULL);
	struct pollfd pfd = { fd, POLLIN, 0 };
	int ready = poll(&pfd, 1, 2000);
	int64 count = read_count(fd);
	report("abstime_past", result == 0 && ready == 1 && count == 1,
		"poll %d count %lld", ready, (long long)count);
}


static void
test_poll_select(int fd)
{
	set_relative(fd, 60000, 0);

	struct pollfd pfd = { fd, POLLIN, 0 };
	int early = poll(&pfd, 1, 0);
	bigtime_t start = system_time();
	pfd.revents = 0;
	int ready = poll(&pfd, 1, 3000);
	bigtime_t elapsed = system_time() - start;
	report("poll", early == 0 && ready == 1 && (pfd.revents & POLLIN) != 0
			&& elapsed < 3000000,
		"early %d ready %d revents 0x%x after %lld us", early, ready,
		pfd.revents, (long long)elapsed);
	read_count(fd);

	set_relative(fd, 60000, 0);
	fd_set readSet;
	FD_ZERO(&readSet);
	FD_SET(fd, &readSet);
	struct timeval timeout = { 3, 0 };
	ready = select(fd + 1, &readSet, NULL, NULL, &timeout);
	report("select", ready == 1 && FD_ISSET(fd, &readSet), "ready %d",
		ready);
	read_count(fd);
}


static void
test_errors(int fd)
{
	uint64_t value = 1;
	errno = 0;
	ssize_t bytes = write(fd, &value, sizeof(value));
	report("write_fails", bytes < 0, "result %zd errno %s", bytes,
		strerror(errno));

	set_relative(fd, 1000, 0);
	snooze(50000);
	uint32_t small;
	errno = 0;
	bytes = read(fd, &small, sizeof(small));
	int smallErrno = errno;
	int64 count = read_count(fd);
	report("short_read_einval", bytes < 0 && smallErrno == EINVAL
			&& count == 1,
		"result %zd errno %s, count kept %lld", bytes, strerror(smallErrno),
		(long long)count);

	struct itimerspec spec = make_spec(1000, 0);
	spec.it_value.tv_nsec = 1000000000;
	errno = 0;
	int result = timerfd_settime(fd, 0, &spec, NULL);
	report("bad_timespec", result < 0 && errno == EINVAL, "errno %s",
		strerror(errno));

	spec = make_spec(1000000, 0);
	errno = 0;
	result = timerfd_settime(fd, TFD_TIMER_ABSTIME | TFD_TIMER_CANCEL_ON_SET,
		&spec, NULL);
	report("cancel_on_set_unsupported", result < 0 && errno == EINVAL,
		"errno %s", strerror(errno));

	int pipeFDs[2];
	pipe(pipeFDs);
	errno = 0;
	result = timerfd_gettime(pipeFDs[0], &spec);
	int pipeErrno = errno;
	close(pipeFDs[0]);
	close(pipeFDs[1]);
	errno = 0;
	int badResult = timerfd_gettime(9999, &spec);
	report("not_a_timerfd", result < 0 && pipeErrno == EINVAL
			&& badResult < 0 && errno == EBADF,
		"pipe: %s, bad fd: %s", strerror(pipeErrno), strerror(errno));
}


static void
test_descriptor(int fd)
{
	int cloexecFD = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC);
	report("cloexec", cloexecFD >= 0
			&& (fcntl(cloexecFD, F_GETFD) & FD_CLOEXEC) != 0
			&& (fcntl(fd, F_GETFD) & FD_CLOEXEC) == 0,
		"fd %d", cloexecFD);
	close(cloexecFD);

	struct stat st;
	memset(&st, 0, sizeof(st));
	int statResult = fstat(fd, &st);
	report("fstat", statResult == 0 && S_ISFIFO(st.st_mode), "mode 0%o",
		(unsigned)st.st_mode);

	// The timer is shared across fork(): the child sees the expiration.
	set_relative(fd, 50000, 0);
	pid_t child = fork();
	if (child == 0) {
		struct pollfd pfd = { fd, POLLIN, 0 };
		poll(&pfd, 1, 3000);
		_exit(read_count(fd) == 1 ? 0 : 1);
	}
	int childStatus = -1;
	waitpid(child, &childStatus, 0);
	errno = 0;
	int64 parentCount = read_count(fd);
	report("fork_shared", child > 0 && WIFEXITED(childStatus)
			&& WEXITSTATUS(childStatus) == 0 && parentCount < 0,
		"child status 0x%x, parent read %lld", childStatus,
		(long long)parentCount);
}


static void
test_stress()
{
	// Closing armed fast periodic timers exercises the teardown path
	// (cancel_timer + DPCQueue::Cancel) while the timer's DPC may be
	// running. That path once panicked (DPCQueue::Cancel used an
	// uninitialized condition variable); the harness hit it about once in
	// 30 boots with 300 iterations, so this runs many more, with a 100 us
	// period and varied delays to land closes inside the DPC.
	const int kIterations = 2000;
	bigtime_t start = system_time();
	int created = 0;
	for (int i = 0; i < kIterations; i++) {
		int fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK);
		if (fd < 0)
			break;
		created++;
		set_relative(fd, 100, 100);
		snooze(50 + (i % 7) * 40);
		close(fd);
	}
	report("stress_close_armed", created == kIterations,
		"%d timers in %lld us", created,
		(long long)(system_time() - start));

	// Re-arming a fast timer over and over must never leave stale
	// expirations from the previous setting behind.
	int fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK);
	for (int i = 0; i < 500; i++)
		set_relative(fd, 200, 200);
	set_relative(fd, 0, 0);
	snooze(20000);
	errno = 0;
	int64 count = read_count(fd);
	report("stress_rearm_then_disarm", count < 0 && errno == EAGAIN,
		"read %lld (%s)", (long long)count, strerror(errno));
	close(fd);
}


int
main()
{
	test_create();

	int fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK);
	report("create", fd >= 0, "fd %d (%s)", fd, fd < 0 ? strerror(errno) : "");
	if (fd < 0) {
		printf("CE-SUITE " SUITE " DONE failures=%d\n", sFailures);
		return 1;
	}

	test_disarmed(fd);
	test_oneshot(fd);
	test_gettime_settime(fd);
	test_periodic(fd);
	test_absolute(fd, CLOCK_MONOTONIC, "abstime_monotonic");
	test_absolute(fd, CLOCK_REALTIME, "abstime_realtime");
	test_absolute_past(fd);
	test_poll_select(fd);
	test_errors(fd);
	test_descriptor(fd);
	test_stress();

	close(fd);

	printf("CE-SUITE " SUITE " DONE failures=%d\n", sFailures);
	fflush(stdout);
	return sFailures == 0 ? 0 : 1;
}
