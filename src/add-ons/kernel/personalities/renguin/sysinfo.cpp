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


/*!	getpriority(): Haiku's priorities don't map onto nice values; every
	process reports nice 0. The raw call returns 20 - nice.
*/
int64
renguin_sys_getpriority(int64 which, int64 who)
{
	if (which < LX_PRIO_PROCESS || which > LX_PRIO_USER)
		return -LX_EINVAL;
	if (which == LX_PRIO_PROCESS && !target_exists(who))
		return -LX_ESRCH;
	return 20;
}


int64
renguin_sys_setpriority(int64 which, int64 who, int64 nice)
{
	if (which < LX_PRIO_PROCESS || which > LX_PRIO_USER)
		return -LX_EINVAL;
	if (which == LX_PRIO_PROCESS && !target_exists(who))
		return -LX_ESRCH;
	// Accepted and not applied (nice, renice, make -l keep working).
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
