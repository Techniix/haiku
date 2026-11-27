/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 *
 * madvise() with Linux semantics (batch 13; static glibc program). Renguin
 * accepted every madvise() and ignored it, so MADV_DONTNEED left the old
 * contents in place. V8 and PartitionAlloc (all of Linux Chromium's memory,
 * libxkbcommon's and libbrotli's allocations included, since Chromium's
 * allocator replaces malloc) decommit with MADV_DONTNEED and take the pages
 * to be zero when they use them again: a freed allocator's stale pointers
 * and lengths were read back as fresh state. One line per part, then "ok".
 *
 *  anon_zero     private anonymous pages written, MADV_DONTNEED: they read
 *                back as zero (and can be written again)
 *  partial       only the advised pages are zeroed, the neighbours keep
 *                their contents
 *  file_private  a private file mapping written, MADV_DONTNEED: it reads
 *                back the file's contents, not the private copy
 *  shared        a MAP_SHARED anonymous mapping keeps its contents (Linux
 *                drops only the page table entries)
 *  fork          after fork(), the child's MADV_DONTNEED zeroes its own
 *                copy and leaves the parent's pages alone
 *  free          MADV_FREE is accepted (contents may be dropped)
 *  hints         the hint advices (NORMAL, RANDOM, SEQUENTIAL, WILLNEED,
 *                DONTFORK, DOFORK, HUGEPAGE, NOHUGEPAGE, DONTDUMP, DODUMP)
 *                return 0
 *  errors        an unknown advice is EINVAL, an unaligned address EINVAL,
 *                an unmapped range for MADV_DONTNEED ENOMEM
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

#ifndef MADV_FREE
#define MADV_FREE 8
#endif


static const size_t kPage = 4096;


static void
fail(const char* what)
{
	printf("FAIL %s: %s\n", what, strerror(errno));
	exit(1);
}


static int
all(const unsigned char* bytes, size_t size, unsigned char value)
{
	for (size_t i = 0; i < size; i++) {
		if (bytes[i] != value)
			return 0;
	}
	return 1;
}


int
main(void)
{
	setvbuf(stdout, NULL, _IONBF, 0);

	// anon_zero
	size_t size = 16 * kPage;
	unsigned char* map = mmap(NULL, size, PROT_READ | PROT_WRITE,
		MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (map == MAP_FAILED)
		fail("mmap");
	memset(map, 0xa5, size);
	if (madvise(map, size, MADV_DONTNEED) != 0)
		fail("anon_zero madvise");
	if (!all(map, size, 0)) {
		printf("FAIL anon_zero: byte 0 reads 0x%02x after MADV_DONTNEED\n",
			map[0]);
		return 1;
	}
	memset(map, 0x11, size);
	if (!all(map, size, 0x11))
		fail("anon_zero rewrite");
	printf("anon_zero\n");

	// partial
	memset(map, 0x5a, size);
	if (madvise(map + 4 * kPage, 2 * kPage, MADV_DONTNEED) != 0)
		fail("partial madvise");
	if (!all(map, 4 * kPage, 0x5a) || !all(map + 4 * kPage, 2 * kPage, 0)
		|| !all(map + 6 * kPage, size - 6 * kPage, 0x5a)) {
		printf("FAIL partial: 0x%02x 0x%02x 0x%02x\n", map[0],
			map[4 * kPage], map[6 * kPage]);
		return 1;
	}
	printf("partial\n");

	// file_private
	char path[] = "/tmp/renguin-madvise-XXXXXX";
	int fd = mkstemp(path);
	if (fd < 0)
		fail("mkstemp");
	unlink(path);
	unsigned char content[4 * 4096];
	memset(content, 0x33, sizeof(content));
	if (write(fd, content, sizeof(content)) != (ssize_t)sizeof(content))
		fail("write");
	unsigned char* file = mmap(NULL, sizeof(content), PROT_READ | PROT_WRITE,
		MAP_PRIVATE, fd, 0);
	if (file == MAP_FAILED)
		fail("file mmap");
	memset(file, 0x77, sizeof(content));
	if (madvise(file, sizeof(content), MADV_DONTNEED) != 0)
		fail("file_private madvise");
	if (!all(file, sizeof(content), 0x33)) {
		printf("FAIL file_private: reads 0x%02x, the file has 0x33\n",
			file[0]);
		return 1;
	}
	munmap(file, sizeof(content));
	close(fd);
	printf("file_private\n");

	// shared
	unsigned char* shared = mmap(NULL, 2 * kPage, PROT_READ | PROT_WRITE,
		MAP_SHARED | MAP_ANONYMOUS, -1, 0);
	if (shared == MAP_FAILED)
		fail("shared mmap");
	memset(shared, 0x44, 2 * kPage);
	if (madvise(shared, 2 * kPage, MADV_DONTNEED) != 0)
		fail("shared madvise");
	if (!all(shared, 2 * kPage, 0x44)) {
		printf("FAIL shared: reads 0x%02x\n", shared[0]);
		return 1;
	}
	munmap(shared, 2 * kPage);
	printf("shared\n");

	// fork
	memset(map, 0x66, size);
	pid_t child = fork();
	if (child < 0)
		fail("fork");
	if (child == 0) {
		if (madvise(map, size, MADV_DONTNEED) != 0)
			_exit(2);
		_exit(all(map, size, 0) ? 0 : 3);
	}
	int status = 0;
	waitpid(child, &status, 0);
	if (!WIFEXITED(status) || WEXITSTATUS(status) != 0
		|| !all(map, size, 0x66)) {
		printf("FAIL fork: child status %d, parent reads 0x%02x\n",
			WIFEXITED(status) ? WEXITSTATUS(status) : -1, map[0]);
		return 1;
	}
	printf("fork\n");

	// free
	if (madvise(map, size, MADV_FREE) != 0)
		fail("free");
	map[0] = 1;
	if (map[0] != 1)
		fail("free write");
	printf("free\n");

	// hints
	static const int hints[] = { MADV_NORMAL, MADV_RANDOM, MADV_SEQUENTIAL,
		MADV_WILLNEED, MADV_DONTFORK, MADV_DOFORK, MADV_HUGEPAGE,
		MADV_NOHUGEPAGE, MADV_DONTDUMP, MADV_DODUMP };
	for (size_t i = 0; i < sizeof(hints) / sizeof(hints[0]); i++) {
		if (madvise(map, size, hints[i]) != 0) {
			printf("FAIL hints: advice %d: %s\n", hints[i], strerror(errno));
			return 1;
		}
	}
	printf("hints\n");

	// errors
	errno = 0;
	if (madvise(map, size, 12345) != -1 || errno != EINVAL) {
		printf("FAIL errors: unknown advice: %s\n", strerror(errno));
		return 1;
	}
	errno = 0;
	if (madvise(map + 1, kPage, MADV_DONTNEED) != -1 || errno != EINVAL) {
		printf("FAIL errors: unaligned: %s\n", strerror(errno));
		return 1;
	}
	munmap(map, size);
	errno = 0;
	if (madvise(map, size, MADV_DONTNEED) != -1 || errno != ENOMEM) {
		printf("FAIL errors: unmapped: %s\n", strerror(errno));
		return 1;
	}
	printf("errors\n");

	printf("ok\n");
	return 0;
}
