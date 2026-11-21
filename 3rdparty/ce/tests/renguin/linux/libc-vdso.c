/*
 * Copyright 2026, Renguin contributors.
 * Distributed under the terms of the MIT License.
 *
 * Renguin's vDSO (static glibc program, which uses it as a dynamic one
 * does). One line per part that worked, then "ok".
 *
 *  auxv        AT_SYSINFO_EHDR names an ELF image that /proc/self/maps
 *              calls [vdso]
 *  monotonic   CLOCK_MONOTONIC from the vDSO never goes back, also mixed
 *              with the system call, over 20000 readings
 *  realtime    CLOCK_REALTIME, gettimeofday() and time() from the vDSO
 *              agree with the system call within 50 ms
 *  getcpu      sched_getcpu() (the vDSO's getcpu) names an online CPU
 *  faster      a vDSO clock read takes less time than the system call
 */

#define _GNU_SOURCE
#include <elf.h>
#include <errno.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/auxv.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>


#define CHECK(condition, ...) \
	do { \
		if (!(condition)) { \
			printf("FAIL %s:%d: ", __func__, __LINE__); \
			printf(__VA_ARGS__); \
			printf(" (errno %d %s)\n", errno, strerror(errno)); \
			exit(1); \
		} \
	} while (0)


static long long
nsecs(const struct timespec* ts)
{
	return ts->tv_sec * 1000000000LL + ts->tv_nsec;
}


static void
test_auxv(void)
{
	unsigned long base = getauxval(AT_SYSINFO_EHDR);
	CHECK(base != 0, "no AT_SYSINFO_EHDR");
	CHECK(memcmp((void*)base, ELFMAG, SELFMAG) == 0, "no ELF image at %#lx",
		base);
	FILE* maps = fopen("/proc/self/maps", "r");
	CHECK(maps != NULL, "/proc/self/maps");
	char line[512];
	int found = 0;
	while (fgets(line, sizeof(line), maps) != NULL) {
		unsigned long start = strtoul(line, NULL, 16);
		if (start == base && strstr(line, "[vdso]") != NULL)
			found = 1;
	}
	fclose(maps);
	CHECK(found, "no [vdso] line at %#lx in /proc/self/maps", base);
	printf("auxv\n");
}


static void
test_monotonic(void)
{
	struct timespec previous, now;
	clock_gettime(CLOCK_MONOTONIC, &previous);
	for (int i = 0; i < 20000; i++) {
		if (i % 100 == 50)
			syscall(SYS_clock_gettime, CLOCK_MONOTONIC, &now);
		else
			clock_gettime(CLOCK_MONOTONIC, &now);
		CHECK(nsecs(&now) >= nsecs(&previous), "went back %lld ns at %d",
			nsecs(&previous) - nsecs(&now), i);
		previous = now;
	}
	printf("monotonic\n");
}


static void
test_realtime(void)
{
	struct timespec fast, slow;
	clock_gettime(CLOCK_REALTIME, &fast);
	syscall(SYS_clock_gettime, CLOCK_REALTIME, &slow);
	long long difference = nsecs(&slow) - nsecs(&fast);
	CHECK(difference > -50000000LL && difference < 50000000LL,
		"vDSO and system call differ by %lld ns", difference);
	struct timeval tv;
	CHECK(gettimeofday(&tv, NULL) == 0, "gettimeofday");
	long long tvDifference = tv.tv_sec * 1000000000LL + tv.tv_usec * 1000LL
		- nsecs(&slow);
	CHECK(tvDifference > -50000000LL && tvDifference < 50000000LL,
		"gettimeofday differs by %lld ns", tvDifference);
	time_t seconds = time(NULL);
	CHECK(seconds >= slow.tv_sec && seconds <= slow.tv_sec + 1,
		"time() %lld, clock %lld", (long long)seconds, (long long)slow.tv_sec);
	printf("realtime\n");
}


static void
test_getcpu(void)
{
	int cpu = sched_getcpu();
	long count = sysconf(_SC_NPROCESSORS_ONLN);
	CHECK(cpu >= 0 && cpu < count, "sched_getcpu %d of %ld", cpu, count);
	printf("getcpu\n");
}


static void
test_faster(void)
{
	struct timespec start, end, value;
	const int kCount = 2000;
	clock_gettime(CLOCK_MONOTONIC, &start);
	for (int i = 0; i < kCount; i++)
		clock_gettime(CLOCK_MONOTONIC, &value);
	clock_gettime(CLOCK_MONOTONIC, &end);
	long long fast = nsecs(&end) - nsecs(&start);
	clock_gettime(CLOCK_MONOTONIC, &start);
	for (int i = 0; i < kCount; i++)
		syscall(SYS_clock_gettime, CLOCK_MONOTONIC, &value);
	clock_gettime(CLOCK_MONOTONIC, &end);
	long long slow = nsecs(&end) - nsecs(&start);
	fprintf(stderr, "vdso %lld ns, syscall %lld ns per call\n",
		fast / kCount, slow / kCount);
	CHECK(fast < slow, "vDSO %lld ns, system call %lld ns for %d reads",
		fast, slow, kCount);
	printf("faster\n");
}


int
main(void)
{
	setvbuf(stdout, NULL, _IONBF, 0);
	test_auxv();
	test_monotonic();
	test_realtime();
	test_getcpu();
	test_faster();
	printf("ok\n");
	return 0;
}
