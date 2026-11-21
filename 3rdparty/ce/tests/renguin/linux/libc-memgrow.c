/*
 * Copyright 2026, Renguin contributors.
 * Distributed under the terms of the MIT License.
 *
 * Growing a shared memory file while it is mapped (batch 11; static glibc
 * program), as Wayland clients grow their wl_shm pools when a window gets
 * bigger (Linux Chromium died with SIGBUS around a resize on real
 * hardware). Linux semantics: pages inside the file's new size are valid
 * in existing MAP_SHARED mappings. One line per part, then "ok".
 *
 *  grow_mapped   memfd of 1 page grown to 1 MiB while its first page is
 *                mapped, then mapped at 1 MiB and written across (mapping
 *                beyond the end, which Linux allows, is not supported yet:
 *                Haiku refuses it, and allowing it broke the VM's
 *                commitment accounting)
 *  remap         munmap and mmap at the new size see what was written
 *  steps         four more ftruncate+mmap rounds as a pool grows, each
 *                written and read back
 *  mincore       every page of a mapping reported resident; ENOMEM for an
 *                unmapped range (Chromium's memory dumps)
 */

#define _GNU_SOURCE
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>


static void
fail(const char* what)
{
	printf("FAIL %s: %s\n", what, strerror(errno));
	exit(1);
}


static void
bus(int signal, siginfo_t* info, void* context)
{
	char line[96];
	int length = snprintf(line, sizeof(line), "FAIL SIGBUS at %p\n",
		info->si_addr);
	write(1, line, length);
	_exit(1);
}


int
main(void)
{
	setvbuf(stdout, NULL, _IONBF, 0);
	struct sigaction action;
	memset(&action, 0, sizeof(action));
	action.sa_sigaction = bus;
	action.sa_flags = SA_SIGINFO;
	sigaction(SIGBUS, &action, NULL);
	sigaction(SIGSEGV, &action, NULL);

	const size_t page = 4096, big = 1024 * 1024;
	int fd = memfd_create("wl_shm-test", MFD_CLOEXEC);
	if (fd < 0 || ftruncate(fd, page) != 0)
		fail("memfd");
	unsigned char* map = mmap(NULL, page, PROT_READ | PROT_WRITE, MAP_SHARED,
		fd, 0);
	if (map == MAP_FAILED)
		fail("mmap");
	map[0] = 1;
	if (ftruncate(fd, big) != 0)
		fail("ftruncate");
	map[page - 1] = 2;
	munmap(map, page);
	map = mmap(NULL, big, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (map == MAP_FAILED || map[0] != 1)
		fail("mmap grown");
	for (size_t i = 0; i < big; i += page)
		map[i] = (unsigned char)(i / page);
	printf("grow_mapped\n");

	munmap(map, big);
	map = mmap(NULL, big, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (map == MAP_FAILED)
		fail("remap");
	for (size_t i = page; i < big; i += page) {
		if (map[i] != (unsigned char)(i / page)) {
			errno = EIO;
			fail("remap contents");
		}
	}
	munmap(map, big);
	printf("remap\n");

	size_t size = big;
	for (int round = 0; round < 4; round++) {
		size_t next = size * 2;
		if (ftruncate(fd, next) != 0)
			fail("steps ftruncate");
		map = mmap(NULL, next, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
		if (map == MAP_FAILED)
			fail("steps mmap");
		memset(map + size, 0x5a, next - size);
		if (map[next - 1] != 0x5a) {
			errno = EIO;
			fail("steps contents");
		}
		munmap(map, next);
		size = next;
	}
	printf("steps\n");

	map = mmap(NULL, 4 * page, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	unsigned char vector[4] = { 9, 9, 9, 9 };
	if (map == MAP_FAILED || mincore(map, 4 * page, vector) != 0
		|| vector[0] != 1 || vector[3] != 1)
		fail("mincore");
	munmap(map, 4 * page);
	if (mincore(map, page, vector) == 0 || errno != ENOMEM)
		fail("mincore ENOMEM");
	printf("mincore\n");
	close(fd);
	printf("ok\n");
	return 0;
}
