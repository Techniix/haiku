/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 *
 * Renguin test (F-27, Steam groundwork): a program built against the Debian
 * root's own glibc (2.41), run in that root through chroot(). It checks
 * what a dynamically linked glibc program needs beyond a static one, and
 * the glibc-specific system calls the design note names:
 *  - the loader and libc are Debian's (gnu_get_libc_version());
 *  - rseq: glibc registers an rseq area for every thread; if the kernel
 *    has none (ENOSYS) glibc must have noticed (__rseq_size 0), else the
 *    area's cpu_id must be a real CPU number;
 *  - clone3: pthread_create() tries clone3 first and falls back to clone;
 *    the threads must run and join either way, with their own TLS;
 *  - dlopen() of a library by soname through the loader's search (the
 *    multiarch directories, or /etc/ld.so.cache once ldconfig made one);
 *  - NSS: getpwuid() and getaddrinfo() through /etc/nsswitch.conf;
 *  - /etc/os-release is the root's (Debian 13).
 * One line per check ("<name> ok" or "<name> FAIL: ..."), then "ok".
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <gnu/libc-version.h>
#include <netdb.h>
#include <pthread.h>
#include <pwd.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/rseq.h>
#include <sys/syscall.h>
#include <unistd.h>

static int sFailures = 0;

static void
check(const char* name, int pass, const char* detail)
{
	if (pass)
		printf("%s ok\n", name);
	else {
		printf("%s FAIL: %s\n", name, detail);
		sFailures++;
	}
}

static __thread int sThreadValue;

static void*
thread_main(void* argument)
{
	sThreadValue = (int)(long)argument;
	sched_yield();
	return (void*)(long)(sThreadValue * 2);
}

int
main(void)
{
	char detail[256];

	const char* version = gnu_get_libc_version();
	snprintf(detail, sizeof(detail), "glibc %s", version);
	check("glibc_2.41", strcmp(version, "2.41") == 0, detail);

	// rseq: registered (a CPU number in the area) or cleanly off
	if (__rseq_size == 0) {
		long result = syscall(SYS_rseq, NULL, 0, 0, 0);
		snprintf(detail, sizeof(detail), "rseq off but syscall gives %ld "
			"errno %d", result, errno);
		check("rseq", result == -1 && (errno == ENOSYS || errno == EINVAL),
			detail);
	} else {
		struct rseq* area = (struct rseq*)((char*)__builtin_thread_pointer()
			+ __rseq_offset);
		snprintf(detail, sizeof(detail), "rseq on, cpu_id %d",
			(int)area->cpu_id);
		check("rseq", (int)area->cpu_id >= 0, detail);
	}

	// threads through clone3 (or its fallback), each with its own TLS
	sThreadValue = 100;
	pthread_t threads[4];
	int created = 0, joined = 0;
	for (int i = 0; i < 4; i++) {
		if (pthread_create(&threads[i], NULL, thread_main, (void*)(long)(i + 1))
				== 0)
			created++;
	}
	for (int i = 0; i < created; i++) {
		void* result;
		if (pthread_join(threads[i], &result) == 0
			&& (long)result == (i + 1) * 2)
			joined++;
	}
	snprintf(detail, sizeof(detail), "%d created, %d joined, main TLS %d",
		created, joined, sThreadValue);
	check("threads", created == 4 && joined == 4 && sThreadValue == 100,
		detail);

	// dlopen by soname
	void* libm = dlopen("libm.so.6", RTLD_NOW);
	double (*cosine)(double) = libm != NULL
		? (double (*)(double))dlsym(libm, "cos") : NULL;
	snprintf(detail, sizeof(detail), "%s", libm == NULL ? dlerror() : "no cos");
	check("dlopen", cosine != NULL && cosine(0.0) == 1.0, detail);
	void* sdl = dlopen("libSDL2-2.0.so.0", RTLD_LAZY);
	snprintf(detail, sizeof(detail), "%s", sdl == NULL ? dlerror() : "");
	check("dlopen_sdl2", sdl != NULL, detail);

	// NSS (files)
	struct passwd* user = getpwuid(0);
	snprintf(detail, sizeof(detail), "getpwuid(0): %s",
		user != NULL ? user->pw_name : "NULL");
	check("nss_passwd", user != NULL && strcmp(user->pw_name, "root") == 0,
		detail);
	struct addrinfo hints, *result = NULL;
	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_INET;
	hints.ai_socktype = SOCK_STREAM;
	int status = getaddrinfo("localhost", "80", &hints, &result);
	snprintf(detail, sizeof(detail), "getaddrinfo: %s", gai_strerror(status));
	check("nss_hosts", status == 0 && result != NULL, detail);
	if (result != NULL)
		freeaddrinfo(result);

	// the root's own files
	FILE* file = fopen("/etc/os-release", "r");
	char line[256];
	int debian = 0;
	while (file != NULL && fgets(line, sizeof(line), file) != NULL) {
		if (strncmp(line, "VERSION_ID=\"13\"", 15) == 0)
			debian = 1;
	}
	if (file != NULL)
		fclose(file);
	check("os_release", debian, "no VERSION_ID=\"13\" in /etc/os-release");

	if (sFailures == 0)
		printf("ok\n");
	return sFailures == 0 ? 0 : 1;
}
