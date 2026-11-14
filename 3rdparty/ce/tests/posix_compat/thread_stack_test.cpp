/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 */

/*!	Threads on a stack of the program's own (pthread_attr_setstack()), as
	OpenCE runs its game threads on stacks it maps itself.

	Found 2026-11-14 (sdl3_gamepad's OpenCE start-up check): when such a
	thread ran, the kernel wrote about 0x3e00 bytes past the end of the
	stack ("vm_soft_fault returned error 'Bad address'" when nothing was
	mapped there). Here every thread's stack is followed by a canary region
	filled with a pattern and then a PROT_NONE guard page:

	  stack.canary       1000 threads (doing a little work, a signal handled
	                     on their stack, then pthread_exit or a return) leave
	                     every byte after their stack untouched (the first
	                     changed offset and its bytes are reported)
	  stack.joined       all of them are joined with their return values
	  stack.base         pthread_getattr_np() / get_thread_info() report the
	                     stack the thread was given
*/


#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include <OS.h>


#define SUITE "thread_stack"

static int sFailures = 0;

static const size_t kStackSize = 256 * 1024;
static const size_t kCanarySize = 64 * 1024;
static const uint8_t kPattern = 0xa5;


static void
report(const char* name, bool pass, const char* format, ...)
{
	char detail[300];
	va_list args;
	va_start(args, format);
	vsnprintf(detail, sizeof(detail), format, args);
	va_end(args);
	printf("CE-TEST %s.%s %s %s\n", SUITE, name, pass ? "PASS" : "FAIL",
		detail);
	fflush(stdout);
	if (!pass)
		sFailures++;
}


static volatile int sSignals = 0;

static void
handler(int)
{
	sSignals++;
}


struct ThreadData {
	void*		stack;
	int			index;
	addr_t		reportedBase;
	addr_t		reportedEnd;
};


static void*
thread_body(void* _data)
{
	ThreadData* data = (ThreadData*)_data;
	char buffer[4096];
	memset(buffer, data->index & 0xff, sizeof(buffer));
	thread_info info;
	if (get_thread_info(find_thread(NULL), &info) == B_OK) {
		data->reportedBase = (addr_t)info.stack_base;
		data->reportedEnd = (addr_t)info.stack_end;
	}
	// a signal handled on this stack
	pthread_kill(pthread_self(), SIGUSR1);
	snooze(10);
	if ((data->index & 1) != 0)
		pthread_exit((void*)(addr_t)(data->index + 1));
	return (void*)(addr_t)(data->index + 1 + (buffer[17] & 0));
}


int
main()
{
	printf("CE-SUITE " SUITE " START\n");
	fflush(stdout);

	struct sigaction action;
	memset(&action, 0, sizeof(action));
	action.sa_handler = handler;
	sigaction(SIGUSR1, &action, NULL);

	const size_t pageSize = B_PAGE_SIZE;
	size_t mapSize = kStackSize + kCanarySize + pageSize;
	int runs = 1000;
	int touched = 0, joinFailures = 0, baseMismatch = 0;
	size_t firstOffset = 0;
	uint8_t firstBytes[16];
	memset(firstBytes, 0, sizeof(firstBytes));

	for (int i = 0; i < runs; i++) {
		uint8_t* region = (uint8_t*)mmap(NULL, mapSize, PROT_READ | PROT_WRITE,
			MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
		if (region == MAP_FAILED) {
			report("canary", false, "mmap: %s", strerror(errno));
			break;
		}
		uint8_t* canary = region + kStackSize;
		memset(canary, kPattern, kCanarySize);
		mprotect(region + kStackSize + kCanarySize, pageSize, PROT_NONE);

		pthread_attr_t attributes;
		pthread_attr_init(&attributes);
		pthread_attr_setstack(&attributes, region, kStackSize);
		ThreadData data = { region, i, 0, 0 };
		pthread_t thread;
		void* result = NULL;
		if (pthread_create(&thread, &attributes, thread_body, &data) != 0
			|| pthread_join(thread, &result) != 0
			|| result != (void*)(addr_t)(i + 1)) {
			joinFailures++;
		}
		pthread_attr_destroy(&attributes);

		if (data.reportedBase < (addr_t)region
			|| data.reportedEnd > (addr_t)region + kStackSize) {
			baseMismatch++;
		}

		for (size_t offset = 0; offset < kCanarySize; offset++) {
			if (canary[offset] != kPattern) {
				if (touched == 0) {
					firstOffset = offset;
					memcpy(firstBytes, canary + offset,
						min_c(sizeof(firstBytes), kCanarySize - offset));
				}
				touched++;
				break;
			}
		}
		munmap(region, mapSize);
	}

	report("canary", touched == 0, "%d of %d threads changed memory after "
		"their stack%s (first at stack end + %#zx: %02x %02x %02x %02x %02x "
		"%02x %02x %02x)", touched, runs, touched ? "" : ": none",
		firstOffset, firstBytes[0], firstBytes[1], firstBytes[2],
		firstBytes[3], firstBytes[4], firstBytes[5], firstBytes[6],
		firstBytes[7]);
	report("joined", joinFailures == 0, "%d of %d threads failed to start, "
		"join or return their value (%d signals handled)", joinFailures, runs,
		sSignals);
	report("base", baseMismatch == 0, "%d of %d threads reported a stack "
		"outside the one given", baseMismatch, runs);

	printf("CE-SUITE " SUITE " DONE failures=%d\n", sFailures);
	return sFailures == 0 ? 0 : 1;
}
