/*
 * Copyright 2026, Renguin contributors.
 * Distributed under the terms of the MIT License.
 *
 * Linux miscellaneous system calls: arch_prctl, prctl, set_tid_address,
 * uname, getrandom, clocks and sleeping, sched_yield.
 * FreeBSD counterparts: sys/amd64/linux/linux_machdep.c (arch_prctl),
 * sys/compat/linux/linux_misc.c, linux_time.c, linux_mib.c (uname).
 */


#include "renguin.h"
#include "linux_defs.h"

#include <string.h>
#include <time.h>

#include <KernelExport.h>
#include <OS.h>

#include <arch/x86/arch_thread.h>
#include <kernel.h>
#include <real_time_clock.h>
#include <team.h>
#include <thread.h>
#include <UserTimer.h>
#include <util/Random.h>


//! arch_prctl: set or read the FS/GS base. ARCH_SET_FS is how glibc and musl
//! install thread-local storage on x86-64. Setting user_local_storage and
//! calling x86_set_tls_context() applies the FS base immediately and keeps it
//! across context switches (see the foreign-ABI case in arch_thread.cpp).
int64
renguin_sys_arch_prctl(uint64 code, uint64 addr)
{
	Thread* thread = thread_get_current_thread();

	switch (code) {
		case LX_ARCH_SET_FS:
			if (addr != 0 && !IS_USER_ADDRESS((addr_t)addr))
				return -LX_EPERM;
			thread->user_local_storage = (addr_t)addr;
			x86_set_tls_context(thread);
			return 0;

		case LX_ARCH_SET_GS:
			if (addr != 0 && !IS_USER_ADDRESS((addr_t)addr))
				return -LX_EPERM;
			thread->arch_info.user_gs_base = (uint64)addr;
			x86_set_tls_context(thread);
			return 0;

		case LX_ARCH_GET_FS:
		{
			uint64 value = thread->user_local_storage;
			if (renguin_copy_out(addr, &value, sizeof(value)) != B_OK)
				return -LX_EFAULT;
			return 0;
		}

		case LX_ARCH_GET_GS:
		{
			uint64 value = thread->arch_info.user_gs_base;
			if (renguin_copy_out(addr, &value, sizeof(value)) != B_OK)
				return -LX_EFAULT;
			return 0;
		}

		default:
			return -LX_EINVAL;
	}
}


int64
renguin_sys_prctl(uint64 option, uint64 arg2)
{
	switch (option) {
		case LX_PR_SET_NAME:
		{
			// Linux names are at most 15 characters plus NUL.
			char name[16];
			if (arg2 == 0 || !IS_USER_ADDRESS((addr_t)arg2)
				|| user_strlcpy(name, (const char*)(addr_t)arg2,
					sizeof(name)) < 0) {
				return -LX_EFAULT;
			}
			name[sizeof(name) - 1] = '\0';
			return renguin_encode(rename_thread(thread_get_current_thread_id(),
				name));
		}
		case LX_PR_GET_NAME:
		{
			char name[16];
			strlcpy(name, thread_get_current_thread()->name, sizeof(name));
			if (renguin_copy_out(arg2, name, sizeof(name)) != B_OK)
				return -LX_EFAULT;
			return 0;
		}

		case 1:		// PR_SET_PDEATHSIG: accepted, not yet delivered
		case 4:		// PR_SET_DUMPABLE
		case 38:	// PR_SET_NO_NEW_PRIVS: Haiku has no setuid escalation
					// paths this could restrict further
			return 0;
		case 3:		// PR_GET_DUMPABLE
			return 1;
		case 39:	// PR_GET_NO_NEW_PRIVS
			return 0;

		default:
			return -LX_EINVAL;
	}
}


int64
renguin_sys_uname(uint64 buffer)
{
	linux_utsname info;
	memset(&info, 0, sizeof(info));
	strlcpy(info.sysname, "Linux", sizeof(info.sysname));
	strlcpy(info.nodename, RENGUIN_LINUX_HOSTNAME, sizeof(info.nodename));
	strlcpy(info.release, RENGUIN_LINUX_RELEASE, sizeof(info.release));
	strlcpy(info.version, RENGUIN_LINUX_VERSION, sizeof(info.version));
	strlcpy(info.machine, "x86_64", sizeof(info.machine));
	strlcpy(info.domainname, "(none)", sizeof(info.domainname));

	if (renguin_copy_out(buffer, &info, sizeof(info)) != B_OK)
		return -LX_EFAULT;
	return 0;
}


int64
renguin_sys_getrandom(uint64 buffer, uint64 length, uint64 flags)
{
	if ((flags & ~(uint64)(LX_GRND_NONBLOCK | LX_GRND_RANDOM
			| LX_GRND_INSECURE)) != 0) {
		return -LX_EINVAL;
	}
	// The kernel RNG never blocks, so GRND_NONBLOCK needs no handling.
	if (length > 32 * 1024 * 1024)
		length = 32 * 1024 * 1024;

	uint8 chunk[256];
	uint64 done = 0;
	while (done < length) {
		size_t size = length - done < sizeof(chunk)
			? (size_t)(length - done) : sizeof(chunk);
		for (size_t i = 0; i < size; i += sizeof(uint32)) {
			uint32 word = secure_random_value();
			memcpy(chunk + i, &word,
				size - i < sizeof(word) ? size - i : sizeof(word));
		}
		if (renguin_copy_out(buffer + done, chunk, size) != B_OK)
			return done > 0 ? (int64)done : -LX_EFAULT;
		done += size;
	}
	return (int64)done;
}


//! Reads a clock in microseconds, Haiku's time unit, through Haiku's own
//! clock code (which takes the right locks for the CPU-time clocks).
static status_t
read_clock(uint64 clockId, bigtime_t* _micros)
{
	clockid_t haikuClock;
	switch (clockId) {
		case LX_CLOCK_REALTIME:
		case LX_CLOCK_REALTIME_COARSE:
			haikuClock = CLOCK_REALTIME;
			break;
		case LX_CLOCK_MONOTONIC:
		case LX_CLOCK_MONOTONIC_RAW:
		case LX_CLOCK_MONOTONIC_COARSE:
		case LX_CLOCK_BOOTTIME:
			haikuClock = CLOCK_MONOTONIC;
			break;
		case LX_CLOCK_THREAD_CPUTIME_ID:
			haikuClock = CLOCK_THREAD_CPUTIME_ID;
			break;
		case LX_CLOCK_PROCESS_CPUTIME_ID:
			haikuClock = CLOCK_PROCESS_CPUTIME_ID;
			break;
		default:
			return B_BAD_VALUE;
	}
	return user_timer_get_clock(haikuClock, *_micros);
}


static linux_timespec
to_timespec(bigtime_t micros)
{
	linux_timespec ts;
	ts.tv_sec = micros / 1000000;
	ts.tv_nsec = (micros % 1000000) * 1000;
	return ts;
}


int64
renguin_sys_clock_gettime(uint64 clockId, uint64 timespecPtr)
{
	bigtime_t micros;
	if (read_clock(clockId, &micros) != B_OK)
		return -LX_EINVAL;

	linux_timespec ts = to_timespec(micros);
	if (renguin_copy_out(timespecPtr, &ts, sizeof(ts)) != B_OK)
		return -LX_EFAULT;
	return 0;
}


int64
renguin_sys_clock_getres(uint64 clockId, uint64 timespecPtr)
{
	bigtime_t micros;
	if (read_clock(clockId, &micros) != B_OK)
		return -LX_EINVAL;
	if (timespecPtr == 0)
		return 0;

	// Haiku's clocks count microseconds.
	linux_timespec ts = { 0, 1000 };
	if (renguin_copy_out(timespecPtr, &ts, sizeof(ts)) != B_OK)
		return -LX_EFAULT;
	return 0;
}


int64
renguin_sys_gettimeofday(uint64 timevalPtr, uint64 timezonePtr)
{
	if (timevalPtr != 0) {
		bigtime_t micros = real_time_clock_usecs();
		linux_timeval tv;
		tv.tv_sec = micros / 1000000;
		tv.tv_usec = micros % 1000000;
		if (renguin_copy_out(timevalPtr, &tv, sizeof(tv)) != B_OK)
			return -LX_EFAULT;
	}
	if (timezonePtr != 0) {
		// struct timezone { int tz_minuteswest; int tz_dsttime; }: UTC.
		int32 zone[2] = { 0, 0 };
		if (renguin_copy_out(timezonePtr, zone, sizeof(zone)) != B_OK)
			return -LX_EFAULT;
	}
	return 0;
}


int64
renguin_sys_time(uint64 tloc)
{
	int64 seconds = real_time_clock_usecs() / 1000000;
	if (tloc != 0 && renguin_copy_out(tloc, &seconds, sizeof(seconds)) != B_OK)
		return -LX_EFAULT;
	return seconds;
}


/*!	Sleeps until the system-time deadline \a until. Returns B_OK, or
	B_INTERRUPTED if a signal arrived.
*/
static status_t
sleep_until(bigtime_t until)
{
	status_t status;
	do {
		status = snooze_etc(until, B_SYSTEM_TIMEBASE,
			B_ABSOLUTE_TIMEOUT | B_CAN_INTERRUPT);
	} while (status != B_OK && status != B_INTERRUPTED
		&& system_time() < until);
	return status == B_INTERRUPTED ? B_INTERRUPTED : B_OK;
}


static status_t
read_timespec(uint64 address, bigtime_t* _micros)
{
	linux_timespec ts;
	if (renguin_copy_in(&ts, address, sizeof(ts)) != B_OK)
		return B_BAD_ADDRESS;
	if (ts.tv_sec < 0 || ts.tv_nsec < 0 || ts.tv_nsec >= 1000000000)
		return B_BAD_VALUE;
	// Round nanoseconds up, so we never sleep too short.
	if (ts.tv_sec > INT64_MAX / 1000000 - 1)
		*_micros = INT64_MAX / 2;
	else
		*_micros = ts.tv_sec * 1000000 + (ts.tv_nsec + 999) / 1000;
	return B_OK;
}


static int64
interrupted_sleep(uint64 remain, bigtime_t until)
{
	if (remain != 0) {
		bigtime_t left = until - system_time();
		linux_timespec ts = to_timespec(left > 0 ? left : 0);
		renguin_copy_out(remain, &ts, sizeof(ts));
	}
	return -LX_EINTR;
}


int64
renguin_sys_nanosleep(uint64 request, uint64 remain)
{
	bigtime_t micros;
	status_t status = read_timespec(request, &micros);
	if (status != B_OK)
		return status == B_BAD_ADDRESS ? -LX_EFAULT : -LX_EINVAL;

	// A restarted sleep (the signal had no handler) continues to the same
	// end, as Linux's restart_block does.
	bigtime_t until;
	if (!renguin_restarted_deadline(&until))
		until = system_time() + micros;
	if (sleep_until(until) == B_INTERRUPTED) {
		renguin_set_restart(RENGUIN_RESTART_NOHAND, &until);
		return interrupted_sleep(remain, until);
	}
	return 0;
}


int64
renguin_sys_clock_nanosleep(uint64 clockId, uint64 flags, uint64 request,
	uint64 remain)
{
	bigtime_t micros;
	status_t status = read_timespec(request, &micros);
	if (status != B_OK)
		return status == B_BAD_ADDRESS ? -LX_EFAULT : -LX_EINVAL;

	bool absolute = (flags & LX_TIMER_ABSTIME) != 0;
	bigtime_t until;
	switch (clockId) {
		case LX_CLOCK_REALTIME:
			until = absolute
				? system_time() + (micros - real_time_clock_usecs())
				: system_time() + micros;
			break;
		case LX_CLOCK_MONOTONIC:
		case LX_CLOCK_BOOTTIME:
			until = absolute ? micros : system_time() + micros;
			break;
		default:
			return -LX_EINVAL;
	}

	renguin_restarted_deadline(&until);
	if (sleep_until(until) == B_INTERRUPTED) {
		renguin_set_restart(RENGUIN_RESTART_NOHAND, &until);
		// Linux only reports the remaining time for relative sleeps.
		return interrupted_sleep(absolute ? 0 : remain, until);
	}
	return 0;
}


int64
renguin_sys_sched_yield()
{
	thread_yield();
	return 0;
}
