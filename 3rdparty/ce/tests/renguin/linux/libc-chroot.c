/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 *
 * chroot() (batch 13, the base of the bwrap shim for Flatpak; static glibc
 * program). One line per part, then "ok".
 *
 *  root      after chroot(dir), "/hello" is dir/hello
 *  link      an absolute symbolic link inside the new root resolves under
 *            it
 *  dotdot    ".." at the new root stays at the new root
 *  fork      a fork()ed child has the same root
 *  exec      a program exec()ed from inside the new root (a copy of this
 *            one) keeps it
 *  errors    chroot() of a missing path is ENOENT, of a file ENOTDIR
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
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


int
main(int argc, char** argv)
{
	setvbuf(stdout, NULL, _IONBF, 0);
	char buffer[256];

	if (argc > 1 && strcmp(argv[1], "child") == 0) {
		// exec()ed inside the new root
		if (read_file("/hello", buffer, sizeof(buffer)) != 0
			|| strcmp(buffer, "inside") != 0) {
			printf("FAIL exec: /hello not the new root's\n");
			return 1;
		}
		printf("exec\n");
		return 0;
	}

	char dir[] = "/tmp/renguin-chroot-XXXXXX";
	if (mkdtemp(dir) == NULL)
		fail("mkdtemp");
	char path[512];
	snprintf(path, sizeof(path), "%s/hello", dir);
	write_file(path, "inside");
	snprintf(path, sizeof(path), "%s/etc", dir);
	mkdir(path, 0755);
	snprintf(path, sizeof(path), "%s/etc/target", dir);
	write_file(path, "linked");
	snprintf(path, sizeof(path), "%s/link", dir);
	if (symlink("/etc/target", path) != 0)
		fail("symlink");
	snprintf(path, sizeof(path), "%s/bin", dir);
	mkdir(path, 0755);
	// a copy of this program inside the new root, for the exec part
	snprintf(path, sizeof(path), "%s/bin/self", dir);
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
	snprintf(path, sizeof(path), "%s/hello", dir);
	errno = 0;
	if (chroot("/nonexistent-renguin") != -1 || errno != ENOENT) {
		printf("FAIL errors: missing path: %s\n", strerror(errno));
		return 1;
	}
	errno = 0;
	if (chroot(path) != -1 || errno != ENOTDIR) {
		printf("FAIL errors: a file: %s\n", strerror(errno));
		return 1;
	}

	if (chroot(dir) != 0)
		fail("chroot");
	if (chdir("/") != 0)
		fail("chdir");
	if (read_file("/hello", buffer, sizeof(buffer)) != 0
		|| strcmp(buffer, "inside") != 0) {
		printf("FAIL root: /hello reads \"%s\"\n", buffer);
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

	if (read_file("/../../hello", buffer, sizeof(buffer)) != 0
		|| strcmp(buffer, "inside") != 0) {
		printf("FAIL dotdot: /../../hello reads \"%s\"\n", buffer);
		return 1;
	}
	printf("dotdot\n");

	pid_t child = fork();
	if (child == 0) {
		_exit(read_file("/hello", buffer, sizeof(buffer)) == 0
			&& strcmp(buffer, "inside") == 0 ? 0 : 3);
	}
	int status = 0;
	waitpid(child, &status, 0);
	if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
		printf("FAIL fork: child status %d\n", status);
		return 1;
	}
	printf("fork\n");

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
	printf("errors\nok\n");
	return 0;
}
