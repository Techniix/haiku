/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 *
 * waitid() and close_range() (COVERAGE.md's next calls; glibc's
 * posix_spawn and Python's subprocess use both). Prints one word per
 * part, then "ok"; the first failure stops with a message on stderr.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

#define FAIL(what) do { fprintf(stderr, "%s: %s\n", what, strerror(errno)); \
	return 1; } while (0)

int
main(void)
{
	siginfo_t info;
	memset(&info, 0xff, sizeof(info));
	if (waitid(P_ALL, 0, &info, WEXITED | WNOHANG) != -1 || errno != ECHILD)
		FAIL("no children: ECHILD");

	pid_t child = fork();
	if (child == 0)
		_exit(7);
	memset(&info, 0, sizeof(info));
	if (waitid(P_PID, child, &info, WEXITED) != 0)
		FAIL("waitid");
	if (info.si_signo != SIGCHLD || info.si_code != CLD_EXITED
		|| info.si_pid != child || info.si_status != 7) {
		fprintf(stderr, "waitid: signo %d code %d pid %d status %d\n",
			info.si_signo, info.si_code, info.si_pid, info.si_status);
		return 1;
	}
	puts("exited");

	child = fork();
	if (child == 0) {
		pause();
		_exit(0);
	}
	memset(&info, 0xff, sizeof(info));
	if (waitid(P_PID, child, &info, WEXITED | WNOHANG) != 0
		|| info.si_pid != 0)
		FAIL("WNOHANG with a running child");
	kill(child, SIGTERM);
	if (waitid(P_PID, child, &info, WEXITED) != 0
		|| info.si_code != CLD_KILLED || info.si_status != SIGTERM)
		FAIL("killed child");
	puts("killed");

	int a = dup(1), b = dup(1), c = dup(1);
	if (syscall(SYS_close_range, b, ~0U, 4 /* CLOSE_RANGE_CLOEXEC */) != 0
		|| (fcntl(b, F_GETFD) & FD_CLOEXEC) == 0
		|| (fcntl(c, F_GETFD) & FD_CLOEXEC) == 0
		|| (fcntl(a, F_GETFD) & FD_CLOEXEC) != 0)
		FAIL("close_range CLOEXEC");
	if (syscall(SYS_close_range, b, ~0U, 0) != 0
		|| fcntl(b, F_GETFD) != -1 || fcntl(c, F_GETFD) != -1
		|| fcntl(a, F_GETFD) == -1)
		FAIL("close_range");
	if (syscall(SYS_close_range, 5, 4, 0) != -1 || errno != EINVAL)
		FAIL("close_range first > last");
	puts("close_range");

	puts("ok");
	return 0;
}
