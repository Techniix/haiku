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
#include <fcntl.h>
#include <sys/stat.h>
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


// #pragma mark - deep use of a supplied stack, and bad user pointers


static off_t
syslog_size()
{
	struct stat st;
	return stat("/boot/system/var/log/syslog", &st) == 0 ? st.st_size : 0;
}


//!	Lines of the syslog written since \a offset that contain \a text.
static int
syslog_count(off_t offset, const char* text)
{
	FILE* file = fopen("/boot/system/var/log/syslog", "r");
	if (file == NULL)
		return -1;
	fseeko(file, offset, SEEK_SET);
	char line[1024];
	int count = 0;
	while (fgets(line, sizeof(line), file) != NULL) {
		if (strstr(line, text) != NULL)
			count++;
	}
	fclose(file);
	return count;
}


static volatile size_t sDeepest;

/*!	Recurses until the frame is within 12 KiB of the stack's lowest
	address, then read()s a file (BFS) into a buffer on that frame.
*/
static ssize_t
deep_read(uint8_t* bottom, int depth)
{
	uint8_t frame[2048];
	frame[0] = depth;
	if ((size_t)(frame - bottom) > 12 * 1024 && depth < 1000)
		return deep_read(bottom, depth + 1) + (frame[0] - depth);
	sDeepest = frame - bottom;
	int fd = open("/boot/system/lib/libroot.so", O_RDONLY);
	if (fd < 0)
		return -1;
	ssize_t bytes = read(fd, frame, sizeof(frame));
	close(fd);
	return bytes;
}


static void*
deep_thread(void* data)
{
	return (void*)(intptr_t)deep_read((uint8_t*)data, 0);
}


/*!	Batch 3 (a hardware syslog flooded with "write access attempted on
	write-protected area" during a BFS read, suspected to come from the
	supplied-stack fix d8e7fe69): a thread on a stack of its own uses it
	almost to its lowest address and reads a file into a buffer there;
	nothing may fault. Then a read() into a read-only page: EFAULT for the
	program, and the kernel's log of it says which program and area, at
	most 16 times for the team.
*/
static void
check_deep_stack_and_bad_pointer()
{
	off_t before = syslog_size();
	size_t size = 128 * 1024;
	uint8_t* stack = (uint8_t*)mmap(NULL, size, PROT_READ | PROT_WRITE,
		MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	pthread_attr_t attributes;
	pthread_attr_init(&attributes);
	pthread_attr_setstack(&attributes, stack, size);
	pthread_t thread;
	void* result = (void*)-1;
	bool started = pthread_create(&thread, &attributes, deep_thread, stack)
		== 0;
	if (started)
		pthread_join(thread, &result);
	pthread_attr_destroy(&attributes);
	munmap(stack, size);
	snooze(300000);
	int faults = syslog_count(before, "vm_page_fault");
	report("supplied_stack_deep_read", started && (intptr_t)result == 2048
			&& faults == 0,
		"read %d bytes at %zu bytes above the stack's end; kernel faults "
		"logged: %d", (int)(intptr_t)result, (size_t)sDeepest, faults);

	before = syslog_size();
	uint8_t* page = (uint8_t*)mmap(NULL, B_PAGE_SIZE, PROT_READ,
		MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	int fd = open("/boot/system/lib/libroot.so", O_RDONLY);
	int efaults = 0;
	ssize_t lastResult = 0;
	int lastError = 0;
	for (int i = 0; i < 40; i++) {
		lastResult = pread(fd, page, 512, 0);
		lastError = lastResult < 0 ? errno : 0;
		if (lastResult < 0 && lastError == EFAULT)
			efaults++;
	}
	close(fd);
	munmap(page, B_PAGE_SIZE);
	snooze(300000);
	int lines = syslog_count(before, "vm_page_fault: system call wrote to");
	int named = syslog_count(before, "thread_stack_test");
	int raw = syslog_count(before, "write access attempted");
	report("bad_pointer_logged", efaults == 40 && lines >= 1 && lines <= 16
			&& named >= 1 && raw == 0,
		"40 reads into a read-only page: %d EFAULT (last: %d, %s); %d log "
		"lines naming the program (want 1-16; the syslog folds repeats), %d "
		"of the old unattributed ones", efaults, (int)lastResult,
		strerror(lastError), lines, raw);
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

	check_deep_stack_and_bad_pointer();

	printf("CE-SUITE " SUITE " DONE failures=%d\n", sFailures);
	return sFailures == 0 ? 0 : 1;
}
