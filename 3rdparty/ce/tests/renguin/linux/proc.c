/* Process creation (fork, clone, vfork, execve, wait4) and its errors.
 * Exits 0 and prints "ok", or exits with the number of the first failing
 * check. Re-executes itself, so run it by a path (./proc) from a writable
 * directory. */
#include "sys.h"

struct ts { i64 sec, nsec; };

#define CLONE_CHILD_CLEARTID 0x00200000
#define CLONE_CHILD_SETTID   0x01000000
#define SIGCHLD 17
#define WNOHANG 1

static int exited(int st, int code) { return (st & 0x7f) == 0 && ((st >> 8) & 0xff) == code; }
static int ends_with(const char* s, const char* t)
{
	u64 a = slen(s), b = slen(t);
	return a >= b && seq(s + a - b, t);
}

static i64 spawn_wait(i64 pid)
{
	int st = -1;
	if (pid < 0) return -1000;
	if (sys4(SYS_wait4, pid, &st, 0, 0) != pid) return -1001;
	return st;
}

int check_main(int argc, char** argv)
{
	/* Modes used when this program runs itself. */
	if (argc >= 2 && seq(argv[1], "exec-mode")) {
		put("exec-ok\n");
		return 5;
	}
	if (argc >= 4 && seq(argv[1], "script-arg")) {
		/* argv: interpreter, "script-arg", script path, "x" */
		if (!ends_with(argv[2], "renguin-script.tmp")) return 61;
		if (argc != 4 || !seq(argv[3], "x")) return 62;
		return 6;
	}

	char* const* envp = (char* const*)(argv + argc + 1);
	const char* self = argv[0];

	/* 1. fork + wait4 exit status */
	i64 pid = sys0(SYS_fork);
	if (pid == 0) quit(7);
	if (!exited(spawn_wait(pid), 7)) return 2;

	/* 2. glibc-style clone: the child's tid is written for it */
	static volatile int tid = -1;
	pid = sys6(SYS_clone, CLONE_CHILD_SETTID | CLONE_CHILD_CLEARTID | SIGCHLD,
		0, 0, (i64)&tid, 0, 0);
	if (pid == 0) quit(tid == sys0(SYS_gettid) ? 0 : 1);
	if (!exited(spawn_wait(pid), 0)) return 3;
	if (tid != -1) return 4;	/* the parent's copy is untouched */

	/* 3. vfork */
	pid = sys0(SYS_vfork);
	if (pid == 0) { sys1(SYS_exit, 3); for (;;); }
	if (!exited(spawn_wait(pid), 3)) return 5;

	/* 4. execve of ourselves; output goes to our stdout */
	pid = sys0(SYS_fork);
	if (pid == 0) {
		char* args[] = { (char*)self, "exec-mode", 0 };
		sys3(SYS_execve, self, args, envp);
		quit(99);
	}
	if (!exited(spawn_wait(pid), 5)) return 6;

	/* 5. a "#!" script naming us as interpreter, with one argument */
	static const char script[] = "renguin-script.tmp";
	char line[600] = "#!";
	u64 n = 2;
	for (const char* s = self; *s && n < 500; s++) line[n++] = *s;
	for (const char* s = " script-arg\n"; *s; s++) line[n++] = *s;
	i64 fd = sys4(SYS_openat, AT_FDCWD, script, 0101 | 01000, 0755);
	if (fd < 0) return 7;
	sys3(SYS_write, fd, line, n);
	sys1(SYS_close, fd);
	pid = sys0(SYS_fork);
	if (pid == 0) {
		char path[] = "./renguin-script.tmp";
		char* args[] = { "script", "x", 0 };
		sys3(SYS_execve, path, args, envp);
		quit(98);
	}
	if (!exited(spawn_wait(pid), 6)) return 8;

	/* 6. exec errors come back before the point of no return */
	char* noargs[] = { "x", 0 };
	fd = sys4(SYS_openat, AT_FDCWD, script, 0101 | 01000, 0755);
	sys3(SYS_write, fd, "not a program\n", 14);
	sys1(SYS_close, fd);
	if (sys3(SYS_execve, "./renguin-script.tmp", noargs, envp) != -8)
		return 9;	/* ENOEXEC */
	sys3(SYS_unlinkat, AT_FDCWD, script, 0);
	fd = sys4(SYS_openat, AT_FDCWD, script, 0101 | 01000, 0644);
	sys3(SYS_write, fd, "#!/bin/sh\n", 10);
	sys1(SYS_close, fd);
	if (sys3(SYS_execve, "./renguin-script.tmp", noargs, envp) != -13)
		return 10;	/* EACCES: not executable */
	sys3(SYS_unlinkat, AT_FDCWD, script, 0);
	if (sys3(SYS_execve, "./renguin-no-such-program", noargs, envp) != -2)
		return 11;	/* ENOENT */

	/* 7. no children left */
	int st;
	if (sys4(SYS_wait4, -1, &st, WNOHANG, 0) != -10) return 12;	/* ECHILD */

	/* 8. a child killed by SIGTERM */
	pid = sys0(SYS_fork);
	if (pid == 0) {
		struct ts t = { 30, 0 };
		sys2(SYS_nanosleep, &t, 0);
		quit(97);
	}
	if (sys2(SYS_kill, pid, 15) != 0) return 13;
	st = (int)spawn_wait(pid);
	if ((st & 0x7f) != 15) return 14;	/* WIFSIGNALED, WTERMSIG == 15 */

	put("ok\n");
	return 0;
}
