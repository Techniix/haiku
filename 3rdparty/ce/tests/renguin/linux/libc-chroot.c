/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 *
 * chroot() (batch 14, the base of the bwrap shim for Flatpak; static glibc
 * program). One line per part, then "ok". The layout, under a fresh
 * directory D:
 *   D/secret          "outside": must never be reached by an absolute path
 *   D/root            the new root
 *   D/root/hello      "inside"
 *   D/root/etc/target "linked"
 *
 *  errors    before chroot(): a missing path is ENOENT, a file ENOTDIR, a
 *            bad pointer EFAULT
 *  root      after chroot(D/root), "/hello" is D/root/hello; getcwd() after
 *            chdir("/") is "/"
 *  link      an absolute symbolic link inside the new root resolves under it
 *  dotdot    ".." in an absolute path stops at the new root: "/../../hello"
 *            is the root's hello, "/../secret" does not exist
 *  symlinks  links cannot lead out: absolute targets "/../secret" and
 *            "/etc/../../secret", relative "../secret" at the top and
 *            "../../secret" one level down, all ENOENT
 *  fchdir    a directory opened before chroot() stays usable: fchdir() to
 *            it works and "secret" opens relative to it (as on Linux, where
 *            this is the documented way out of a chroot)
 *  fork      a fork()ed child and its own child have the same root
 *  exec      a program exec()ed from inside the new root (a copy of this
 *            one) keeps it, and so does the program it execs in turn; a
 *            failed execve() leaves the caller in its root
 *  native    200 fork()ed children exec Haiku's native /boot/system/bin/true
 *            (a host path, outside the root) and exit 0; afterwards no
 *            chroot() root waits for an exec any more: the system-wide
 *            count Renguin adds to /proc/self/status ("RengaExecRoots")
 *            is back where it was (batch 14 review: each such exec used
 *            to leave a 1 KB entry behind until reboot)
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>


static void
fail(const char* what)
{
	printf("FAIL %s: %s\n", what, strerror(errno));
	exit(1);
}


static int
read_file(const char* path, char* buffer, size_t size)
{
	buffer[0] = '\0';
	int fd = open(path, O_RDONLY);
	if (fd < 0)
		return -1;
	ssize_t length = read(fd, buffer, size - 1);
	close(fd);
	if (length < 0)
		return -1;
	buffer[length] = '\0';
	return 0;
}


static void
write_file(const char* path, const char* text)
{
	int fd = open(path, O_CREAT | O_TRUNC | O_WRONLY, 0644);
	if (fd < 0 || write(fd, text, strlen(text)) != (ssize_t)strlen(text))
		fail(path);
	close(fd);
}


//! Whether \a path reads "inside" (the new root's /hello).
static int
is_inside(const char* path)
{
	char buffer[64];
	return read_file(path, buffer, sizeof(buffer)) == 0
		&& strcmp(buffer, "inside") == 0;
}


//! \a path must not open: ENOENT. Exits with a FAIL line otherwise.
static void
must_not_exist(const char* part, const char* path)
{
	char buffer[64];
	errno = 0;
	if (read_file(path, buffer, sizeof(buffer)) == 0) {
		printf("FAIL %s: %s escapes the root (reads \"%s\")\n", part, path,
			buffer);
		exit(1);
	}
	if (errno != ENOENT) {
		printf("FAIL %s: %s: %s, expected ENOENT\n", part, path,
			strerror(errno));
		exit(1);
	}
}


//! Renguin's count of chroot() roots waiting for an exec, or -1.
static long
exec_root_count(void)
{
	char status[4096];
	if (read_file("/proc/self/status", status, sizeof(status)) != 0)
		return -1;
	const char* line = strstr(status, "\nRengaExecRoots:");
	if (line == NULL)
		return -1;
	return strtol(line + strlen("\nRengaExecRoots:"), NULL, 10);
}


int
main(int argc, char** argv)
{
	setvbuf(stdout, NULL, _IONBF, 0);
	char buffer[256];

	if (argc > 1 && strcmp(argv[1], "child") == 0) {
		// exec()ed inside the new root; exec once more
		if (!is_inside("/hello")) {
			printf("FAIL exec: /hello not the new root's\n");
			return 1;
		}
		must_not_exist("exec", "/../secret");
		char* args[] = { (char*)"/bin/self", (char*)"grandchild", NULL };
		execv("/bin/self", args);
		printf("FAIL exec: second exec: %s\n", strerror(errno));
		return 1;
	}
	if (argc > 1 && strcmp(argv[1], "grandchild") == 0) {
		if (!is_inside("/hello")) {
			printf("FAIL exec: /hello not the new root's after two execs\n");
			return 1;
		}
		return 0;
	}

	char dir[] = "/tmp/renguin-chroot-XXXXXX";
	if (mkdtemp(dir) == NULL)
		fail("mkdtemp");
	char root[256];
	char path[512];
	snprintf(root, sizeof(root), "%s/root", dir);
	if (mkdir(root, 0755) != 0)
		fail("mkdir root");
	snprintf(path, sizeof(path), "%s/secret", dir);
	write_file(path, "outside");
	snprintf(path, sizeof(path), "%s/hello", root);
	write_file(path, "inside");
	snprintf(path, sizeof(path), "%s/etc", root);
	mkdir(path, 0755);
	snprintf(path, sizeof(path), "%s/etc/target", root);
	write_file(path, "linked");
	snprintf(path, sizeof(path), "%s/sub", root);
	mkdir(path, 0755);

	static const struct { const char* name; const char* target; } kLinks[] = {
		{ "link", "/etc/target" },
		{ "abs-dotdot", "/../secret" },
		{ "abs-etc-dotdot", "/etc/../../secret" },
		{ "rel-top", "../secret" },
		{ "sub/rel-down", "../../secret" },
	};
	for (size_t i = 0; i < sizeof(kLinks) / sizeof(kLinks[0]); i++) {
		snprintf(path, sizeof(path), "%s/%s", root, kLinks[i].name);
		if (symlink(kLinks[i].target, path) != 0)
			fail("symlink");
	}
	snprintf(path, sizeof(path), "%s/bin", root);
	mkdir(path, 0755);
	// a copy of this program inside the new root, for the exec part
	snprintf(path, sizeof(path), "%s/bin/self", root);
	{
		int in = open("/proc/self/exe", O_RDONLY);
		int out = open(path, O_CREAT | O_TRUNC | O_WRONLY, 0755);
		if (in < 0 || out < 0)
			fail("copy self");
		char chunk[65536];
		ssize_t n;
		while ((n = read(in, chunk, sizeof(chunk))) > 0) {
			if (write(out, chunk, n) != n)
				fail("copy self write");
		}
		close(in);
		close(out);
	}

	// errors (and, on a kernel without chroot(), the first failure: EPERM)
	errno = 0;
	if (chroot("/nonexistent-renguin") != -1 || errno != ENOENT) {
		printf("FAIL errors: missing path: %s\n", strerror(errno));
		return 1;
	}
	snprintf(path, sizeof(path), "%s/hello", root);
	errno = 0;
	if (chroot(path) != -1 || errno != ENOTDIR) {
		printf("FAIL errors: a file: %s\n", strerror(errno));
		return 1;
	}
	errno = 0;
	if (syscall(SYS_chroot, (const char*)8) != -1 || errno != EFAULT) {
		printf("FAIL errors: a bad pointer: %s\n", strerror(errno));
		return 1;
	}
	printf("errors\n");

	int outside = open(dir, O_RDONLY | O_DIRECTORY);
	if (outside < 0)
		fail("open outside");

	if (chroot(root) != 0)
		fail("chroot");
	if (chdir("/") != 0)
		fail("chdir");
	if (!is_inside("/hello")) {
		printf("FAIL root: /hello not the new root's\n");
		return 1;
	}
	if (getcwd(buffer, sizeof(buffer)) == NULL || strcmp(buffer, "/") != 0) {
		printf("FAIL root: getcwd() is \"%s\"\n", buffer);
		return 1;
	}
	printf("root\n");

	if (read_file("/link", buffer, sizeof(buffer)) != 0
		|| strcmp(buffer, "linked") != 0) {
		printf("FAIL link: /link reads \"%s\" (%s)\n", buffer,
			strerror(errno));
		return 1;
	}
	printf("link\n");

	if (!is_inside("/../../hello") || !is_inside("/etc/../../hello")) {
		printf("FAIL dotdot: /../../hello not the new root's\n");
		return 1;
	}
	must_not_exist("dotdot", "/../secret");
	must_not_exist("dotdot", "/../../../secret");
	printf("dotdot\n");

	must_not_exist("symlinks", "/abs-dotdot");
	must_not_exist("symlinks", "/abs-etc-dotdot");
	must_not_exist("symlinks", "/rel-top");
	must_not_exist("symlinks", "/sub/rel-down");
	printf("symlinks\n");

	if (fchdir(outside) != 0)
		fail("fchdir");
	if (read_file("secret", buffer, sizeof(buffer)) != 0
		|| strcmp(buffer, "outside") != 0) {
		printf("FAIL fchdir: secret relative to the old directory reads "
			"\"%s\" (%s)\n", buffer, strerror(errno));
		return 1;
	}
	// absolute paths are still in the root
	if (!is_inside("/hello"))
		fail("fchdir: /hello");
	if (chdir("/") != 0)
		fail("chdir back");
	close(outside);
	printf("fchdir\n");

	pid_t child = fork();
	if (child == 0) {
		pid_t grandchild = fork();
		if (grandchild == 0)
			_exit(is_inside("/hello") ? 0 : 3);
		int grandStatus = 0;
		waitpid(grandchild, &grandStatus, 0);
		_exit(is_inside("/hello") && WIFEXITED(grandStatus)
			&& WEXITSTATUS(grandStatus) == 0 ? 0 : 3);
	}
	int status = 0;
	waitpid(child, &status, 0);
	if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
		printf("FAIL fork: child status %d\n", status);
		return 1;
	}
	printf("fork\n");

	// a failed execve() keeps the caller's root
	char* missing[] = { (char*)"/bin/missing", NULL };
	execv("/bin/missing", missing);
	if (!is_inside("/hello")) {
		printf("FAIL exec: a failed execve() lost the root\n");
		return 1;
	}
	child = fork();
	if (child == 0) {
		char* args[] = { (char*)"/bin/self", (char*)"child", NULL };
		execv("/bin/self", args);
		printf("FAIL exec: %s\n", strerror(errno));
		_exit(4);
	}
	waitpid(child, &status, 0);
	if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
		printf("FAIL exec: child status %d\n", status);
		return 1;
	}
	printf("exec\n");

	// native: execs of a Haiku program from inside the root
	long before = exec_root_count();
	if (before < 0) {
		printf("FAIL native: no RengaExecRoots line in /proc/self/status\n");
		return 1;
	}
	for (int i = 0; i < 200; i++) {
		child = fork();
		if (child == 0) {
			char* args[] = { (char*)"true", NULL };
			execv("/boot/system/bin/true", args);
			_exit(5);
		}
		if (child < 0)
			fail("native: fork");
		waitpid(child, &status, 0);
		if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
			printf("FAIL native: exec %d of /boot/system/bin/true: status "
				"%d\n", i, status);
			return 1;
		}
	}
	long after = exec_root_count();
	// (another team may be in an exec of its own at either moment)
	if (after < 0 || after > before + 2) {
		printf("FAIL native: %ld exec roots before 200 native execs, %ld "
			"after\n", before, after);
		return 1;
	}
	if (!is_inside("/hello")) {
		printf("FAIL native: the caller lost its root\n");
		return 1;
	}
	printf("native\nok\n");
	return 0;
}
