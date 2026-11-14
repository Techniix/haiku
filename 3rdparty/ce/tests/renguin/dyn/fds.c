/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 *
 * Renguin test (F-27): Linux event and memory descriptors, which Renguin
 * passes to Renga's own epoll, eventfd, timerfd, signalfd, inotify and
 * memfd implementations. Linked against musl and against glibc. Each line
 * is "<check> ok" or "<check> FAIL <why>"; the last is "ok".
 *
 *   epoll     epoll_create1(EPOLL_CLOEXEC), EPOLLIN on a pipe with the
 *             caller's data (packed struct epoll_event), EPOLLOUT, EPOLLET
 *             edge, EPOLLONESHOT re-arm with EPOLL_CTL_MOD, EPOLL_CTL_DEL,
 *             a timeout, epoll_pwait() with a signal mask
 *   eventfd   counter and EFD_SEMAPHORE reads, EFD_NONBLOCK EAGAIN, epoll
 *   timerfd   CLOCK_MONOTONIC one-shot and interval expirations, gettime,
 *             TFD_TIMER_ABSTIME, poll()
 *   signalfd  SIGUSR1 (Linux number 10) blocked and read back as
 *             ssi_signo 10 with the sender's pid and SI_USER; SIGCHLD from
 *             a child that exits with 3 (ssi_status 3, CLD_EXITED)
 *   inotify   IN_CREATE, IN_MODIFY and IN_DELETE in a directory of /tmp,
 *             with the file's name; inotify_rm_watch gives IN_IGNORED
 *   memfd     memfd_create(MFD_CLOEXEC | MFD_ALLOW_SEALING), ftruncate,
 *             a shared mapping seen through read(), F_ADD_SEALS write and
 *             grow seals (write and ftruncate then fail with EPERM),
 *             F_GET_SEALS, a memfd without MFD_ALLOW_SEALING reports
 *             F_SEAL_SEAL
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/inotify.h>
#include <sys/mman.h>
#include <sys/signalfd.h>
#include <sys/stat.h>
#include <sys/timerfd.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

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
epoll_test(void)
{
	const char* name = "epoll";
	int ep = epoll_create1(EPOLL_CLOEXEC);
	CHECK(name, ep >= 0 && (fcntl(ep, F_GETFD) & FD_CLOEXEC) != 0,
		"epoll_create1");
	int fds[2];
	CHECK(name, pipe(fds) == 0, "pipe");

	struct epoll_event event;
	memset(&event, 0, sizeof(event));
	event.events = EPOLLIN;
	event.data.u64 = 0x1122334455667788ULL;
	CHECK(name, epoll_ctl(ep, EPOLL_CTL_ADD, fds[0], &event) == 0, "add");
	event.events = EPOLLOUT;
	event.data.fd = fds[1];
	CHECK(name, epoll_ctl(ep, EPOLL_CTL_ADD, fds[1], &event) == 0, "add out");

	struct epoll_event events[4];
	int count = epoll_wait(ep, events, 4, 0);
	CHECK(name, count == 1 && events[0].events == EPOLLOUT
		&& events[0].data.fd == fds[1], "only EPOLLOUT: %d", count);
	CHECK(name, epoll_ctl(ep, EPOLL_CTL_DEL, fds[1], NULL) == 0, "del");

	CHECK(name, write(fds[1], "x", 1) == 1, "write");
	count = epoll_wait(ep, events, 4, 1000);
	CHECK(name, count == 1 && (events[0].events & EPOLLIN) != 0
		&& events[0].data.u64 == 0x1122334455667788ULL,
		"EPOLLIN data %d %#llx", count,
		(unsigned long long)events[0].data.u64);

	// edge-triggered: one report per new data
	event.events = EPOLLIN | EPOLLET;
	event.data.u32 = 5;
	CHECK(name, epoll_ctl(ep, EPOLL_CTL_MOD, fds[0], &event) == 0, "mod ET");
	CHECK(name, epoll_wait(ep, events, 4, 0) == 1, "ET first");
	CHECK(name, epoll_wait(ep, events, 4, 0) == 0, "ET again");
	CHECK(name, write(fds[1], "y", 1) == 1
		&& epoll_wait(ep, events, 4, 0) == 1, "ET new data");

	// one-shot
	event.events = EPOLLIN | EPOLLONESHOT;
	CHECK(name, epoll_ctl(ep, EPOLL_CTL_MOD, fds[0], &event) == 0,
		"mod oneshot");
	CHECK(name, epoll_wait(ep, events, 4, 0) == 1, "oneshot first");
	CHECK(name, epoll_wait(ep, events, 4, 0) == 0, "oneshot disarmed");
	CHECK(name, epoll_ctl(ep, EPOLL_CTL_MOD, fds[0], &event) == 0
		&& epoll_wait(ep, events, 4, 0) == 1, "oneshot re-armed");

	char buffer[4];
	read(fds[0], buffer, sizeof(buffer));
	struct timespec start, end;
	clock_gettime(CLOCK_MONOTONIC, &start);
	sigset_t mask;
	sigemptyset(&mask);
	sigaddset(&mask, SIGUSR2);
	count = epoll_pwait(ep, events, 4, 50, &mask);
	clock_gettime(CLOCK_MONOTONIC, &end);
	long elapsed = (end.tv_sec - start.tv_sec) * 1000
		+ (end.tv_nsec - start.tv_nsec) / 1000000;
	CHECK(name, count == 0 && elapsed >= 40, "timeout %d after %ld ms",
		count, elapsed);
	close(fds[0]);
	close(fds[1]);
	close(ep);
	printf("%s ok\n", name);
}


static void
eventfd_test(void)
{
	const char* name = "eventfd";
	int fd = eventfd(3, EFD_NONBLOCK | EFD_CLOEXEC);
	CHECK(name, fd >= 0, "eventfd");
	uint64_t value = 4;
	CHECK(name, write(fd, &value, 8) == 8, "write");
	CHECK(name, read(fd, &value, 8) == 8 && value == 7, "read %llu",
		(unsigned long long)value);
	errno = 0;
	CHECK(name, read(fd, &value, 8) == -1 && errno == EAGAIN, "EAGAIN");
	close(fd);

	fd = eventfd(2, EFD_SEMAPHORE | EFD_NONBLOCK);
	CHECK(name, fd >= 0, "semaphore");
	CHECK(name, read(fd, &value, 8) == 8 && value == 1
		&& read(fd, &value, 8) == 8 && value == 1
		&& read(fd, &value, 8) == -1, "semaphore reads");
	int ep = epoll_create1(0);
	struct epoll_event event = { EPOLLIN, { 0 } };
	epoll_ctl(ep, EPOLL_CTL_ADD, fd, &event);
	value = 1;
	write(fd, &value, 8);
	CHECK(name, epoll_wait(ep, &event, 1, 0) == 1, "epoll");
	close(ep);
	close(fd);
	printf("%s ok\n", name);
}


static void
timerfd_test(void)
{
	const char* name = "timerfd";
	int fd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC);
	CHECK(name, fd >= 0, "create");
	struct itimerspec spec;
	memset(&spec, 0, sizeof(spec));
	spec.it_value.tv_nsec = 20 * 1000000;
	CHECK(name, timerfd_settime(fd, 0, &spec, NULL) == 0, "settime");
	struct itimerspec current;
	CHECK(name, timerfd_gettime(fd, &current) == 0
		&& current.it_value.tv_sec == 0 && current.it_value.tv_nsec > 0,
		"gettime");
	struct pollfd pollFD = { fd, POLLIN, 0 };
	CHECK(name, poll(&pollFD, 1, 1000) == 1, "poll");
	uint64_t expirations = 0;
	CHECK(name, read(fd, &expirations, 8) == 8 && expirations == 1,
		"one-shot %llu", (unsigned long long)expirations);

	spec.it_interval.tv_nsec = 10 * 1000000;
	spec.it_value.tv_nsec = 10 * 1000000;
	CHECK(name, timerfd_settime(fd, 0, &spec, NULL) == 0, "interval");
	usleep(65000);
	CHECK(name, read(fd, &expirations, 8) == 8 && expirations >= 3,
		"interval %llu", (unsigned long long)expirations);

	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	memset(&spec, 0, sizeof(spec));
	spec.it_value = now;
	spec.it_value.tv_nsec += 10 * 1000000;
	if (spec.it_value.tv_nsec >= 1000000000) {
		spec.it_value.tv_sec++;
		spec.it_value.tv_nsec -= 1000000000;
	}
	CHECK(name, timerfd_settime(fd, TFD_TIMER_ABSTIME, &spec, NULL) == 0,
		"absolute");
	CHECK(name, read(fd, &expirations, 8) == 8 && expirations == 1,
		"absolute expiration");
	close(fd);
	printf("%s ok\n", name);
}


static void
signalfd_test(void)
{
	const char* name = "signalfd";
	sigset_t mask;
	sigemptyset(&mask);
	sigaddset(&mask, SIGUSR1);
	sigaddset(&mask, SIGCHLD);
	CHECK(name, sigprocmask(SIG_BLOCK, &mask, NULL) == 0, "block");
	int fd = signalfd(-1, &mask, SFD_CLOEXEC);
	CHECK(name, fd >= 0, "signalfd");

	CHECK(name, kill(getpid(), SIGUSR1) == 0, "kill");
	struct signalfd_siginfo info;
	memset(&info, 0, sizeof(info));
	CHECK(name, read(fd, &info, sizeof(info)) == sizeof(info), "read");
	CHECK(name, info.ssi_signo == SIGUSR1 && info.ssi_signo == 10
		&& info.ssi_pid == (uint32_t)getpid() && info.ssi_code == SI_USER,
		"signo %u pid %u code %d", info.ssi_signo, info.ssi_pid,
		info.ssi_code);

	pid_t child = fork();
	if (child == 0)
		_exit(3);
	memset(&info, 0, sizeof(info));
	CHECK(name, read(fd, &info, sizeof(info)) == sizeof(info), "read child");
	CHECK(name, info.ssi_signo == SIGCHLD && info.ssi_signo == 17
		&& info.ssi_pid == (uint32_t)child && info.ssi_code == CLD_EXITED
		&& info.ssi_status == 3, "SIGCHLD signo %u pid %u code %d status %d",
		info.ssi_signo, info.ssi_pid, info.ssi_code, info.ssi_status);
	waitpid(child, NULL, 0);
	close(fd);
	sigprocmask(SIG_UNBLOCK, &mask, NULL);
	printf("%s ok\n", name);
}


static void
inotify_test(void)
{
	const char* name = "inotify";
	char directory[64];
	snprintf(directory, sizeof(directory), "/tmp/renguin-inotify-%d",
		(int)getpid());
	mkdir(directory, 0755);
	int fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
	CHECK(name, fd >= 0, "init");
	int watch = inotify_add_watch(fd, directory,
		IN_CREATE | IN_MODIFY | IN_DELETE);
	CHECK(name, watch >= 0, "add_watch %s", directory);

	char path[96];
	snprintf(path, sizeof(path), "%s/file", directory);
	int file = open(path, O_CREAT | O_WRONLY, 0644);
	write(file, "x", 1);
	close(file);
	unlink(path);
	usleep(100000);

	char buffer[1024] __attribute__((aligned(8)));
	ssize_t bytes = read(fd, buffer, sizeof(buffer));
	CHECK(name, bytes > 0, "read");
	int sawCreate = 0, sawModify = 0, sawDelete = 0;
	for (char* p = buffer; p < buffer + bytes;) {
		struct inotify_event* event = (struct inotify_event*)p;
		if (event->wd == watch && event->len > 0
			&& strcmp(event->name, "file") == 0) {
			if ((event->mask & IN_CREATE) != 0)
				sawCreate = 1;
			if ((event->mask & IN_MODIFY) != 0)
				sawModify = 1;
			if ((event->mask & IN_DELETE) != 0)
				sawDelete = 1;
		}
		p += sizeof(*event) + event->len;
	}
	CHECK(name, sawCreate && sawModify && sawDelete,
		"create %d modify %d delete %d", sawCreate, sawModify, sawDelete);

	CHECK(name, inotify_rm_watch(fd, watch) == 0, "rm_watch");
	usleep(50000);
	bytes = read(fd, buffer, sizeof(buffer));
	CHECK(name, bytes >= (ssize_t)sizeof(struct inotify_event)
		&& (((struct inotify_event*)buffer)->mask & IN_IGNORED) != 0,
		"IN_IGNORED %d", (int)bytes);
	close(fd);
	rmdir(directory);
	printf("%s ok\n", name);
}


static void
memfd_test(void)
{
	const char* name = "memfd";
	int fd = memfd_create("renguin-test", MFD_CLOEXEC | MFD_ALLOW_SEALING);
	CHECK(name, fd >= 0, "memfd_create");
	CHECK(name, (fcntl(fd, F_GETFD) & FD_CLOEXEC) != 0, "MFD_CLOEXEC");
	CHECK(name, ftruncate(fd, 8192) == 0, "ftruncate");
	char* map = mmap(NULL, 8192, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	CHECK(name, map != MAP_FAILED, "mmap");
	strcpy(map + 4096, "shared");
	char buffer[8];
	CHECK(name, pread(fd, buffer, 7, 4096) == 7
		&& strcmp(buffer, "shared") == 0, "read through the mapping");
	munmap(map, 8192);

	CHECK(name, fcntl(fd, F_GET_SEALS) == 0, "no seals yet");
	CHECK(name, fcntl(fd, F_ADD_SEALS, F_SEAL_WRITE | F_SEAL_GROW) == 0,
		"F_ADD_SEALS");
	CHECK(name, fcntl(fd, F_GET_SEALS) == (F_SEAL_WRITE | F_SEAL_GROW),
		"F_GET_SEALS %d", fcntl(fd, F_GET_SEALS));
	errno = 0;
	CHECK(name, pwrite(fd, "x", 1, 0) == -1 && errno == EPERM,
		"write sealed");
	errno = 0;
	CHECK(name, ftruncate(fd, 16384) == -1 && errno == EPERM, "grow sealed");
	close(fd);

	fd = memfd_create("unsealable", 0);
	CHECK(name, fd >= 0 && fcntl(fd, F_GET_SEALS) == F_SEAL_SEAL,
		"F_SEAL_SEAL without MFD_ALLOW_SEALING");
	errno = 0;
	CHECK(name, fcntl(fd, F_ADD_SEALS, F_SEAL_WRITE) == -1 && errno == EPERM,
		"sealed against seals");
	close(fd);
	printf("%s ok\n", name);
}


int
main(void)
{
	setvbuf(stdout, NULL, _IONBF, 0);
	epoll_test();
	eventfd_test();
	timerfd_test();
	signalfd_test();
	inotify_test();
	memfd_test();
	if (sFailures == 0)
		printf("ok\n");
	return sFailures != 0;
}
