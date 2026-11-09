/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 */

/*!	In-guest test for sched_getaffinity()/sched_setaffinity() and
	pthread_getaffinity_np()/pthread_setaffinity_np().

	Linked against libroot only: the functions moved there from libgnu.
	Checks glibc's calling conventions (-1/errno for sched_*, error numbers
	for pthread_*), that pid 0 and a team ID address the right threads, and
	that the mask is inherited by new threads and fork()ed children as on
	Linux. Output format: see 3rdparty/ce/README.md.
*/

#define _DEFAULT_SOURCE
	// cpuset_t and the declarations in the gnu headers

#include <sched.h>
#include <pthread.h>

#include <OS.h>

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>


#define SUITE "affinity"


static int sFailures = 0;


static void
report(const char* name, bool pass, const char* format, ...)
{
	char detail[256];
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


static unsigned
mask_bits(const cpuset_t& set)
{
	// The first 32 CPUs are plenty for a test VM.
	return set.bits[0];
}


static int sCPUCount;
static unsigned sAllMask;


struct Worker {
	pthread_t		thread;
	thread_id		id;
	sem_id			ready;
	sem_id			go;
	cpuset_t		inherited;
	int				inheritedResult;
	cpuset_t		mainSeen;
	int				mainSeenResult;
	pthread_t		mainThread;
};


static void*
worker_function(void* data)
{
	Worker* worker = (Worker*)data;
	worker->id = find_thread(NULL);
	CPU_ZERO(&worker->inherited);
	worker->inheritedResult = sched_getaffinity(0, sizeof(cpuset_t),
		&worker->inherited);

	// pthread_getaffinity_np() of the main thread, from another thread.
	CPU_ZERO(&worker->mainSeen);
	worker->mainSeenResult = pthread_getaffinity_np(worker->mainThread,
		sizeof(cpuset_t), &worker->mainSeen);

	release_sem(worker->ready);
	acquire_sem(worker->go);
	return NULL;
}


static void
start_worker(Worker& worker)
{
	worker.ready = create_sem(0, "affinity ready");
	worker.go = create_sem(0, "affinity go");
	worker.mainThread = pthread_self();
	pthread_create(&worker.thread, NULL, worker_function, &worker);
	acquire_sem(worker.ready);
}


static void
stop_worker(Worker& worker)
{
	release_sem(worker.go);
	pthread_join(worker.thread, NULL);
	delete_sem(worker.ready);
	delete_sem(worker.go);
}


int
main()
{
	printf("CE-SUITE " SUITE " START\n");
	fflush(stdout);

	system_info info;
	get_system_info(&info);
	sCPUCount = info.cpu_count;
	sAllMask = sCPUCount >= 32 ? ~0u : (1u << sCPUCount) - 1;

	// Default: every CPU.
	cpuset_t set;
	CPU_ZERO(&set);
	int result = sched_getaffinity(0, sizeof(set), &set);
	report("get_default", result == 0 && mask_bits(set) == sAllMask
			&& CPU_COUNT(&set) == sCPUCount,
		"result %d, mask %#x (want %#x), CPU_COUNT %d", result,
		mask_bits(set), sAllMask, CPU_COUNT(&set));

	CPU_ZERO(&set);
	result = sched_getaffinity(getpid(), sizeof(set), &set);
	report("get_by_pid", result == 0 && mask_bits(set) == sAllMask,
		"result %d, mask %#x", result, mask_bits(set));

	// Errors: -1 and errno.
	errno = 0;
	result = sched_getaffinity(0x7ffffff0, sizeof(set), &set);
	report("get_esrch", result == -1 && errno == ESRCH,
		"result %d, errno %s", result, strerror(errno));

	cpuset_t empty;
	CPU_ZERO(&empty);
	errno = 0;
	result = sched_setaffinity(0, sizeof(empty), &empty);
	report("set_empty_einval", result == -1 && errno == EINVAL,
		"result %d, errno %s", result, strerror(errno));

	errno = 0;
	result = sched_getaffinity(0, 1, &set);
	report("get_short_einval", result == -1 && errno == EINVAL,
		"result %d, errno %s (1-byte mask)", result, strerror(errno));

	errno = 0;
	result = sched_getaffinity(0, sizeof(set), (cpuset_t*)NULL);
	report("get_null_efault", result == -1 && errno == EFAULT,
		"result %d, errno %s", result, strerror(errno));

	errno = 0;
	result = pthread_setaffinity_np(pthread_self(), sizeof(empty), &empty);
	report("pthread_set_empty", result == EINVAL && errno == 0,
		"result %d (want EINVAL, errno untouched: %d)", result, errno);

	// Restrict the main thread to the last CPU.
	int lastCPU = sCPUCount - 1;
	cpuset_t one;
	CPU_ZERO(&one);
	CPU_SET(lastCPU, &one);
	result = sched_setaffinity(0, sizeof(one), &one);
	CPU_ZERO(&set);
	sched_getaffinity(0, sizeof(set), &set);
	report("set_one_cpu", result == 0 && mask_bits(set) == (1u << lastCPU),
		"result %d, mask now %#x (want %#x)", result, mask_bits(set),
		1u << lastCPU);

	// A new thread inherits it; the main thread's pthread_t names the main
	// thread from another thread too.
	Worker worker;
	start_worker(worker);
	report("thread_inherits", worker.inheritedResult == 0
			&& mask_bits(worker.inherited) == (1u << lastCPU),
		"worker mask %#x (want %#x)", mask_bits(worker.inherited),
		1u << lastCPU);
	report("pthread_get_main_from_other", worker.mainSeenResult == 0
			&& mask_bits(worker.mainSeen) == (1u << lastCPU),
		"result %d, main thread mask seen %#x (want %#x)",
		worker.mainSeenResult, mask_bits(worker.mainSeen), 1u << lastCPU);

	// Changing the worker leaves the main thread alone.
	cpuset_t first;
	CPU_ZERO(&first);
	CPU_SET(0, &first);
	result = pthread_setaffinity_np(worker.thread, sizeof(first), &first);
	cpuset_t workerSet, mainSet;
	CPU_ZERO(&workerSet);
	CPU_ZERO(&mainSet);
	int getResult = pthread_getaffinity_np(worker.thread, sizeof(workerSet),
		&workerSet);
	sched_getaffinity(0, sizeof(mainSet), &mainSet);
	report("pthread_set_other", result == 0 && getResult == 0
			&& mask_bits(workerSet) == 1 && mask_bits(mainSet)
				== (1u << lastCPU),
		"set %d get %d: worker %#x (want 0x1), main %#x (want %#x)", result,
		getResult, mask_bits(workerSet), mask_bits(mainSet), 1u << lastCPU);

	CPU_ZERO(&workerSet);
	result = sched_getaffinity(worker.id, sizeof(workerSet), &workerSet);
	report("sched_get_by_tid", result == 0 && mask_bits(workerSet) == 1,
		"result %d, mask %#x", result, mask_bits(workerSet));
	stop_worker(worker);

	// fork() children inherit the calling thread's mask.
	int fds[2];
	pipe(fds);
	pid_t child = fork();
	if (child == 0) {
		cpuset_t childSet;
		CPU_ZERO(&childSet);
		unsigned bits = sched_getaffinity(0, sizeof(childSet), &childSet) == 0
			? mask_bits(childSet) : 0xdead;
		write(fds[1], &bits, sizeof(bits));
		_exit(0);
	}
	unsigned childBits = 0;
	read(fds[0], &childBits, sizeof(childBits));
	waitpid(child, NULL, 0);
	close(fds[0]);
	close(fds[1]);
	report("fork_inherits", childBits == (1u << lastCPU),
		"child mask %#x (want %#x)", childBits, 1u << lastCPU);

	// A mask with all CPUs (and bits beyond them) restores the default.
	cpuset_t all;
	memset(&all, 0xff, sizeof(all));
	result = sched_setaffinity(0, sizeof(all), &all);
	CPU_ZERO(&set);
	sched_getaffinity(0, sizeof(set), &set);
	report("set_all", result == 0 && mask_bits(set) == sAllMask,
		"result %d, mask %#x (want %#x)", result, mask_bits(set), sAllMask);

	// The thread really runs on the CPU it is restricted to: a busy thread
	// pinned to each CPU in turn must not show up on the others.
	if (sCPUCount < 2) {
		report("pinned_runs", true, "INFO one CPU, nothing to compare");
	} else {
		bool pinnedOK = true;
		char detail[128] = "";
		for (int cpu = 0; cpu < sCPUCount && cpu < 4; cpu++) {
			cpuset_t pin;
			CPU_ZERO(&pin);
			CPU_SET(cpu, &pin);
			sched_setaffinity(0, sizeof(pin), &pin);
			// Read the CPU this thread runs on from the kernel's view: the
			// active time of the CPU should grow while we spin.
			cpu_info before[32], after[32];
			get_cpu_info(0, sCPUCount, before);
			bigtime_t end = system_time() + 100000;
			while (system_time() < end)
				;
			get_cpu_info(0, sCPUCount, after);
			bigtime_t mine = after[cpu].active_time - before[cpu].active_time;
			if (mine < 50000) {
				pinnedOK = false;
				snprintf(detail, sizeof(detail),
					"CPU %d busy only %lld us of 100000", cpu, (long long)mine);
			}
		}
		sched_setaffinity(0, sizeof(all), &all);
		report("pinned_runs", pinnedOK, "%s",
			pinnedOK ? "pinned CPU was busy each time" : detail);
	}

	printf("CE-SUITE " SUITE " DONE failures=%d\n", sFailures);
	fflush(stdout);
	return sFailures == 0 ? 0 : 1;
}
