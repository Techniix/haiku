/*
 * Copyright 2026, Renguin contributors.
 * Distributed under the terms of the MIT License.
 *
 * Identity changes, resource usage, scheduling and the small calls around
 * them: set*id, getgroups/setgroups, sysinfo, times, getrusage,
 * get/setpriority, sched_*, personality and the mlock family.
 *
 * FreeBSD counterparts: sys/compat/linux/linux_uid16.c (the 32-bit calls
 * are native there), linux_misc.c (sysinfo, times, getrusage, personality,
 * sched_*). Haiku's user and group calls are libroot wrappers around
 * _kern_setresuid()/_kern_setresgid() (src/system/libroot/posix/unistd/
 * usergroup.cpp); the same mapping is used here.
 */


#include "renguin.h"

#include <algorithm>
#include "linux_defs.h"

#include <string.h>

#include <KernelExport.h>
#include <OS.h>

#include <kernel.h>
#include <smp.h>
#include <team.h>
#include <thread.h>
#include <usergroup.h>


// #pragma mark - users and groups


int64
renguin_sys_setresuid(uint64 ruid, uint64 euid, uint64 suid, bool setAll)
{
	return renguin_encode(_user_setresuid((uid_t)ruid, (uid_t)euid,
		(uid_t)suid, setAll));
}


int64
renguin_sys_setresgid(uint64 rgid, uint64 egid, uint64 sgid, bool setAll)
{
	return renguin_encode(_user_setresgid((gid_t)rgid, (gid_t)egid,
		(gid_t)sgid, setAll));
}


/*!	setfsuid()/setfsgid(): Haiku has no separate file system IDs (they
	always follow the effective IDs, as Linux's do unless changed). The
	call returns the current value and changes nothing; Linux's callers
	check by calling again, and see that the change did not take.
*/
int64
renguin_sys_setfsuid(uint64 uid)
{
	return thread_get_current_thread()->team->effective_uid;
}


int64
renguin_sys_setfsgid(uint64 gid)
{
	return thread_get_current_thread()->team->effective_gid;
}


int64
renguin_sys_getgroups(int64 count, uint64 list)
{
	if ((int32)count < 0)
		return -LX_EINVAL;
	// gid_t is 32 bits on both systems.
	return renguin_encode(_user_getgroups((int32)count,
		(gid_t*)(addr_t)list));
}


int64
renguin_sys_setgroups(int64 count, uint64 list)
{
	if ((int32)count < 0 || count > 65536)	// NGROUPS_MAX
		return -LX_EINVAL;
	return renguin_encode(_user_setgroups((int32)count,
		(const gid_t*)(addr_t)list));
}


// #pragma mark - resource usage


struct linux_sysinfo {
	int64	uptime;
	uint64	loads[3];
	uint64	totalram;
	uint64	freeram;
	uint64	sharedram;
	uint64	bufferram;
	uint64	totalswap;
	uint64	freeswap;
	uint16	procs;
	uint16	pad;
	uint64	totalhigh;
	uint64	freehigh;
	uint32	mem_unit;
	char	reserved[4];
};
static_assert(sizeof(linux_sysinfo) == 112, "Linux ABI");

struct linux_tms {
	int64	tms_utime;
	int64	tms_stime;
	int64	tms_cutime;
	int64	tms_cstime;
};

struct linux_rusage {
	linux_timeval	ru_utime;
	linux_timeval	ru_stime;
	int64			ru_maxrss;
	int64			ru_ixrss;
	int64			ru_idrss;
	int64			ru_isrss;
	int64			ru_minflt;
	int64			ru_majflt;
	int64			ru_nswap;
	int64			ru_inblock;
	int64			ru_oublock;
	int64			ru_msgsnd;
	int64			ru_msgrcv;
	int64			ru_nsignals;
	int64			ru_nvcsw;
	int64			ru_nivcsw;
};
static_assert(sizeof(linux_rusage) == 144, "Linux ABI");

#define LX_RUSAGE_SELF		0
#define LX_RUSAGE_CHILDREN	(-1)
#define LX_RUSAGE_THREAD	1

// USER_HZ: the unit of times() and /proc tick counts.
#define LX_USER_HZ			100


int64
renguin_sys_sysinfo(uint64 buffer)
{
	system_info info;
	get_system_info(&info);

	linux_sysinfo out;
	memset(&out, 0, sizeof(out));
	out.uptime = system_time() / 1000000;
	// Haiku keeps no load averages; 0 is what an idle Linux system shows.
	out.totalram = info.max_pages * B_PAGE_SIZE;
	out.freeram = info.free_memory;
	out.bufferram = info.block_cache_pages * B_PAGE_SIZE;
	out.totalswap = info.max_swap_pages * B_PAGE_SIZE;
	out.freeswap = info.free_swap_pages * B_PAGE_SIZE;
	out.procs = (uint16)min_c(info.used_threads, (uint32)0xffff);
	out.mem_unit = 1;
	if (renguin_copy_out(buffer, &out, sizeof(out)) != B_OK)
		return -LX_EFAULT;
	return 0;
}


static void
timeval_from(bigtime_t time, linux_timeval* value)
{
	value->tv_sec = time / 1000000;
	value->tv_usec = time % 1000000;
}


int64
renguin_sys_times(uint64 buffer)
{
	team_id team = thread_get_current_thread()->team->id;
	team_usage_info self = {}, children = {};
	get_team_usage_info(team, B_TEAM_USAGE_SELF, &self);
	get_team_usage_info(team, B_TEAM_USAGE_CHILDREN, &children);

	if (buffer != 0) {
		linux_tms out;
		out.tms_utime = self.user_time / (1000000 / LX_USER_HZ);
		out.tms_stime = self.kernel_time / (1000000 / LX_USER_HZ);
		out.tms_cutime = children.user_time / (1000000 / LX_USER_HZ);
		out.tms_cstime = children.kernel_time / (1000000 / LX_USER_HZ);
		if (renguin_copy_out(buffer, &out, sizeof(out)) != B_OK)
			return -LX_EFAULT;
	}
	// Ticks since an arbitrary point: boot, as on Linux.
	return system_time() / (1000000 / LX_USER_HZ);
}


int64
renguin_sys_getrusage(int64 who, uint64 buffer)
{
	linux_rusage out;
	memset(&out, 0, sizeof(out));
	Thread* thread = thread_get_current_thread();

	switch ((int32)who) {
		case LX_RUSAGE_SELF:
		case LX_RUSAGE_CHILDREN:
		{
			team_usage_info usage = {};
			status_t status = get_team_usage_info(thread->team->id,
				(int32)who == LX_RUSAGE_SELF
					? B_TEAM_USAGE_SELF : B_TEAM_USAGE_CHILDREN, &usage);
			if (status != B_OK)
				return renguin_encode(status);
			timeval_from(usage.user_time, &out.ru_utime);
			timeval_from(usage.kernel_time, &out.ru_stime);
			if ((int32)who == LX_RUSAGE_SELF) {
				// Peak resident size is not tracked; the current one is
				// the closest.
				uint64 resident = 0;
				area_info area;
				ssize_t cookie = 0;
				while (get_next_area_info(thread->team->id, &cookie, &area)
						== B_OK) {
					resident += area.ram_size;
				}
				out.ru_maxrss = resident / 1024;
			}
			break;
		}
		case LX_RUSAGE_THREAD:
		{
			thread_info info;
			status_t status = get_thread_info(thread->id, &info);
			if (status != B_OK)
				return renguin_encode(status);
			timeval_from(info.user_time, &out.ru_utime);
			timeval_from(info.kernel_time, &out.ru_stime);
			break;
		}
		default:
			return -LX_EINVAL;
	}

	if (renguin_copy_out(buffer, &out, sizeof(out)) != B_OK)
		return -LX_EFAULT;
	return 0;
}


// #pragma mark - scheduling


#define LX_PRIO_PROCESS		0
#define LX_PRIO_USER		2

#define LX_SCHED_OTHER		0
#define LX_SCHED_FIFO		1
#define LX_SCHED_RR			2
#define LX_SCHED_BATCH		3
#define LX_SCHED_IDLE		5


static bool
target_exists(int64 pid)
{
	if ((int32)pid == 0)
		return true;
	thread_info info;
	team_info teamInfo;
	return get_team_info((team_id)pid, &teamInfo) == B_OK
		|| get_thread_info((thread_id)pid, &info) == B_OK;
}


/*!	Nice values on Haiku's thread priorities (batch 10). Linux's nice is
	per thread (PRIO_PROCESS with a thread ID, 0 for the caller; a process
	ID names its main thread), and so are Haiku's priorities. Positive
	nice lowers the priority by one step per two (nice 10 is
	B_LOW_PRIORITY, 5; 19 is 1); negative nice raises it by one per four,
	at most to B_DISPLAY_PRIORITY (15), so a Linux program cannot outrank
	the desktop. Chromium marks its background and utility threads this
	way; build tools run under "nice" stay out of the desktop's way. The
	value set is kept with the thread, so getpriority() returns it exactly.
	PRIO_PGRP and PRIO_USER are accepted and not applied.
*/
static int32
priority_for_nice(int32 nice)
{
	if (nice > 0)
		return std::max((int32)B_LOWEST_ACTIVE_PRIORITY, B_NORMAL_PRIORITY
			- (nice + 1) / 2);
	return std::min((int32)B_DISPLAY_PRIORITY, B_NORMAL_PRIORITY
		+ (-nice + 3) / 4);
}


static int32
nice_for_priority(int32 priority)
{
	if (priority < B_NORMAL_PRIORITY)
		return std::min(19, (B_NORMAL_PRIORITY - priority) * 2);
	return std::max(-20, -(priority - B_NORMAL_PRIORITY) * 4);
}


//! The thread PRIO_PROCESS \a who names in the caller's team, or -1.
static thread_id
nice_target(int64 who)
{
	if ((int32)who == 0)
		return thread_get_current_thread()->id;
	Thread* thread = Thread::Get((thread_id)who);
	if (thread == NULL)
		return -1;
	thread_id id = thread->id;
	thread->ReleaseReference();
	return id;
}


/*!	getpriority(): the raw call returns 20 - nice (the C library converts).
*/
int64
renguin_sys_getpriority(int64 which, int64 who)
{
	if (which < LX_PRIO_PROCESS || which > LX_PRIO_USER)
		return -LX_EINVAL;
	if (which != LX_PRIO_PROCESS)
		return 20;
	thread_id target = nice_target(who);
	if (target < 0)
		return target_exists(who) ? 20 : -LX_ESRCH;
	int32 nice = 0;
	Thread* thread = Thread::Get(target);
	if (thread != NULL) {
		renguin_thread* data = (renguin_thread*)thread->personality_data;
		if (data != NULL && data->nice_set
			&& thread->team == thread_get_current_thread()->team) {
			nice = data->nice;
		} else
			nice = nice_for_priority(thread->priority);
		thread->ReleaseReference();
	}
	return 20 - nice;
}


int64
renguin_sys_setpriority(int64 which, int64 who, int64 nice)
{
	if (which < LX_PRIO_PROCESS || which > LX_PRIO_USER)
		return -LX_EINVAL;
	if (which != LX_PRIO_PROCESS)
		return target_exists(who) || which == LX_PRIO_USER ? 0 : -LX_ESRCH;
	thread_id target = nice_target(who);
	if (target < 0)
		return target_exists(who) ? 0 : -LX_ESRCH;
	int32 value = (int32)std::max((int64)-20, std::min((int64)19, nice));
	status_t status = set_thread_priority(target, priority_for_nice(value));
	if (status < B_OK)
		return renguin_encode(status);
	Thread* thread = Thread::Get(target);
	if (thread != NULL) {
		renguin_thread* data = (renguin_thread*)thread->personality_data;
		if (data != NULL
			&& thread->team == thread_get_current_thread()->team) {
			data->nice = (int8)value;
			data->nice_set = true;
		}
		thread->ReleaseReference();
	}
	return 0;
}


int64
renguin_sys_sched_getscheduler(int64 pid)
{
	if ((int32)pid < 0)
		return -LX_EINVAL;
	if (!target_exists(pid))
		return -LX_ESRCH;
	return LX_SCHED_OTHER;
}


int64
renguin_sys_sched_setscheduler(int64 pid, uint64 policy, uint64 param)
{
	if ((int32)pid < 0 || param == 0)
		return -LX_EINVAL;
	if (!target_exists(pid))
		return -LX_ESRCH;
	int32 priority;
	if (renguin_copy_in(&priority, param, sizeof(priority)) != B_OK)
		return -LX_EFAULT;
	switch (policy & ~(uint64)0x40000000) {		// SCHED_RESET_ON_FORK
		case LX_SCHED_OTHER:
		case LX_SCHED_BATCH:
		case LX_SCHED_IDLE:
			return priority == 0 ? 0 : -LX_EINVAL;
		case LX_SCHED_FIFO:
		case LX_SCHED_RR:
			return -LX_EPERM;		// no real-time policies
		default:
			return -LX_EINVAL;
	}
}


int64
renguin_sys_sched_getparam(int64 pid, uint64 param)
{
	if ((int32)pid < 0 || param == 0)
		return -LX_EINVAL;
	if (!target_exists(pid))
		return -LX_ESRCH;
	int32 priority = 0;
	if (renguin_copy_out(param, &priority, sizeof(priority)) != B_OK)
		return -LX_EFAULT;
	return 0;
}


int64
renguin_sys_sched_setparam(int64 pid, uint64 param)
{
	if ((int32)pid < 0 || param == 0)
		return -LX_EINVAL;
	if (!target_exists(pid))
		return -LX_ESRCH;
	int32 priority;
	if (renguin_copy_in(&priority, param, sizeof(priority)) != B_OK)
		return -LX_EFAULT;
	return priority == 0 ? 0 : -LX_EINVAL;
}


int64
renguin_sys_sched_get_priority(uint64 policy, bool maximum)
{
	switch (policy) {
		case LX_SCHED_FIFO:
		case LX_SCHED_RR:
			return maximum ? 99 : 1;
		case LX_SCHED_OTHER:
		case LX_SCHED_BATCH:
		case LX_SCHED_IDLE:
			return 0;
		default:
			return -LX_EINVAL;
	}
}


int64
renguin_sys_sched_rr_get_interval(int64 pid, uint64 interval)
{
	if ((int32)pid < 0)
		return -LX_EINVAL;
	if (!target_exists(pid))
		return -LX_ESRCH;
	linux_timespec value = { 0, 100000000 };	// Linux's default RR slice
	if (renguin_copy_out(interval, &value, sizeof(value)) != B_OK)
		return -LX_EFAULT;
	return 0;
}


/*!	personality(): only Linux's own (PER_LINUX, 0) exists here. A query
	(0xffffffff) returns it; flags such as ADDR_NO_RANDOMIZE are accepted
	and ignored, as their effects are not available.
*/
int64
renguin_sys_personality(uint64 persona)
{
	return 0;
}
