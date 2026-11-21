/*
 * Copyright 2026, Renguin contributors.
 * Distributed under the terms of the MIT License.
 *
 * Hard links on BFS under Renguin (batch 11; static glibc program). BFS has
 * no hard links; with the "hardlink_copy" switch (on) link() and linkat()
 * make a copy, as tzdata's aliased zones need ("apk add" ended with "1
 * error"). One line per part that worked, then "ok".
 *
 *  link       link() succeeds, the new name has the same contents and mode
 *  linkat     linkat() with directory descriptors, the same
 *  times      the copy keeps the modification time
 *  exists     linking onto an existing name is EEXIST, a missing source
 *             ENOENT
 *  ln         busybox-style: link then unlink the old name (a rename by
 *             links) leaves the contents under the new name
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>


static void
fail(const char* what)
{
	printf("FAIL %s: %s\n", what, strerror(errno));
	exit(1);
}


static void
same(const char* a, const char* b, const char* what)
{
	char x[64] = "", y[64] = "";
	int fa = open(a, O_RDONLY), fb = open(b, O_RDONLY);
	if (fa < 0 || fb < 0 || read(fa, x, sizeof(x) - 1) <= 0
		|| read(fb, y, sizeof(y) - 1) <= 0 || strcmp(x, y) != 0) {
		errno = EINVAL;
		fail(what);
	}
	close(fa);
	close(fb);
}


int
main(void)
{
	setvbuf(stdout, NULL, _IONBF, 0);
	mkdir("/tmp/ce-link", 0755);
	chdir("/tmp/ce-link");
	unlink("a");
	unlink("b");
	unlink("c");
	unlink("d");
	int fd = open("a", O_WRONLY | O_CREAT | O_TRUNC, 0640);
	if (fd < 0 || write(fd, "zone data\n", 10) != 10)
		fail("create");
	close(fd);
	struct timespec times[2] = { { 1000000000, 0 }, { 1000000000, 0 } };
	utimensat(AT_FDCWD, "a", times, 0);

	if (link("a", "b") != 0)
		fail("link");
	same("a", "b", "link contents");
	struct stat st;
	if (stat("b", &st) != 0 || (st.st_mode & 0777) != 0640) {
		errno = EINVAL;
		fail("link mode");
	}
	printf("link\n");

	int dir = open(".", O_RDONLY | O_DIRECTORY);
	if (linkat(dir, "a", dir, "c", 0) != 0)
		fail("linkat");
	same("a", "c", "linkat contents");
	printf("linkat\n");

	if (st.st_mtime != 1000000000) {
		errno = EINVAL;
		fail("times");
	}
	printf("times\n");

	if (link("a", "b") == 0 || errno != EEXIST)
		fail("exists EEXIST");
	if (link("missing", "x") == 0 || errno != ENOENT)
		fail("exists ENOENT");
	printf("exists\n");

	if (link("c", "d") != 0 || unlink("c") != 0)
		fail("ln");
	same("a", "d", "ln contents");
	printf("ln\n");

	unlink("a");
	unlink("b");
	unlink("d");
	printf("ok\n");
	return 0;
}
