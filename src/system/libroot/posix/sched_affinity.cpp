/*
 * Copyright 2023, Jérôme Duval, jerome.duval@gmail.com. All rights reserved.
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 *
 * sched_getaffinity()/sched_setaffinity() and pthread_getaffinity_np()/
 * pthread_setaffinity_np(), with glibc's interface.
 *
 * These were in libgnu; they live in libroot now, so programs using them
 * need no extra library. Programs and libraries linked against libgnu
 * before keep working: their references are resolved in the global scope,
 * where libroot (loaded by every program) provides the symbols. The
 * declarations stay in the gnu headers (<sched.h>, <pthread.h> from
 * headers/compatibility/gnu), with cpuset_t/cpu_set_t and the CPU_*()
 * macros, like Haiku's other non-POSIX extensions.
 *
 * As on Linux, the sched_*() functions take a thread ID (0 for the calling
 * thread; a process ID names the process's main thread, since Haiku's team
 * ID is its main thread's ID) and return -1 with errno set; the pthread
 * ones return the error number.
 *
 * The gnu headers cannot be included here (libroot's system include path
 * finds the POSIX <sched.h> first), so the mask is passed on as an opaque
 * pointer; the kernel checks the size.
 */

#include <pthread.h>
#include <sched.h>

#include <errno.h>
#include <unistd.h>

#include <errno_private.h>
#include <pthread_private.h>
#include <syscalls.h>


static thread_id
pthread_thread_id(pthread_t thread)
{
	// The main thread's pthread_thread is a static structure without an ID;
	// 0 would make the kernel use the calling thread instead. The main
	// thread's ID is the team ID.
	if (thread->id == 0)
		return getpid();
	return thread->id;
}


// Linux reports a NULL mask as EFAULT; the kernel calls it B_BAD_VALUE.
static int
to_errno(status_t status)
{
	switch (status) {
		case B_OK:
			return 0;
		case B_BAD_THREAD_ID:
			return ESRCH;
		case B_NOT_ALLOWED:
			return EPERM;
		default:
			return status;
				// B_BAD_VALUE is EINVAL, B_BAD_ADDRESS is EFAULT
	}
}


extern "C" int
pthread_setaffinity_np(pthread_t thread, size_t cpusetsize,
	const void* mask)
{
	if (mask == NULL)
		return EFAULT;
	return to_errno(_kern_set_thread_affinity(pthread_thread_id(thread), mask,
		cpusetsize));
}


extern "C" int
pthread_getaffinity_np(pthread_t thread, size_t cpusetsize, void* mask)
{
	if (mask == NULL)
		return EFAULT;
	return to_errno(_kern_get_thread_affinity(pthread_thread_id(thread), mask,
		cpusetsize));
}


extern "C" int
sched_setaffinity(pid_t id, size_t cpusetsize, const void* mask)
{
	if (id < 0 || mask == NULL) {
		__set_errno(id < 0 ? ESRCH : EFAULT);
		return -1;
	}
	int error = to_errno(_kern_set_thread_affinity(id, mask, cpusetsize));
	if (error != 0) {
		__set_errno(error);
		return -1;
	}
	return 0;
}


extern "C" int
sched_getaffinity(pid_t id, size_t cpusetsize, void* mask)
{
	if (id < 0 || mask == NULL) {
		__set_errno(id < 0 ? ESRCH : EFAULT);
		return -1;
	}
	int error = to_errno(_kern_get_thread_affinity(id, mask, cpusetsize));
	if (error != 0) {
		__set_errno(error);
		return -1;
	}
	return 0;
}
