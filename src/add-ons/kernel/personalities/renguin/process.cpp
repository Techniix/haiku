/*
 * Copyright 2026, Renguin contributors.
 * Distributed under the terms of the MIT License.
 *
 * Linux process-level system calls: process and thread IDs, user and group
 * IDs, process groups and sessions, kill/tgkill, resource limits, umask, and
 * the Linux <-> Haiku signal number mapping.
 *
 * IDs map directly (research report section 8): a Linux PID is the Haiku team
 * ID, a TID is the Haiku thread ID, and the main thread's ID equals the team's
 * on both systems.
 *
 * Signal numbers differ above SIGFPE on most entries; see kSignalMap.
 * FreeBSD keeps the same kind of table in sys/compat/linux/linux.c
 * (linux_to_bsd_signal()). Haiku has only 8 real-time signals (33-40):
 * Linux's application real-time signals 34-41 (glibc's SIGRTMIN is 34) map
 * onto those. Linux 32 and 33, which glibc uses internally for thread
 * cancellation and setxid, map onto Haiku's unused SIGRESERVED1/2 (31, 32),
 * whose default action, like Linux's, ends the process. Linux 16
 * (SIGSTKFLT), 30 (SIGPWR) and 42-64 have no Haiku number (research report
 * section 8, "Signals overall"). Handler setup and delivery are in
 * signal.cpp.
 */


#include "renguin.h"
#include "linux_defs.h"

#include <string.h>

#include <KernelExport.h>

#include <ksignal.h>
#include <kernel.h>
#include <signal_defs.h>
#include <syscall_process_info.h>
#include <team.h>
#include <thread.h>

#include <signal.h>
#include <sys/resource.h>


// Linux signal number -> Haiku signal number; 0 means "no equivalent".
static const int kSignalMap[32] = {
	0,				// 0: not a signal (kill(pid, 0) is handled separately)
	SIGHUP,			// 1  SIGHUP
	SIGINT,			// 2  SIGINT
	SIGQUIT,		// 3  SIGQUIT
	SIGILL,			// 4  SIGILL
	SIGTRAP,		// 5  SIGTRAP
	SIGABRT,		// 6  SIGABRT
	SIGBUS,			// 7  SIGBUS
	SIGFPE,			// 8  SIGFPE
	SIGKILL,		// 9  SIGKILL
	SIGUSR1,		// 10 SIGUSR1
	SIGSEGV,		// 11 SIGSEGV
	SIGUSR2,		// 12 SIGUSR2
	SIGPIPE,		// 13 SIGPIPE
	SIGALRM,		// 14 SIGALRM
	SIGTERM,		// 15 SIGTERM
	0,				// 16 SIGSTKFLT
	SIGCHLD,		// 17 SIGCHLD
	SIGCONT,		// 18 SIGCONT
	SIGSTOP,		// 19 SIGSTOP
	SIGTSTP,		// 20 SIGTSTP
	SIGTTIN,		// 21 SIGTTIN
	SIGTTOU,		// 22 SIGTTOU
	SIGURG,			// 23 SIGURG
	SIGXCPU,		// 24 SIGXCPU
	SIGXFSZ,		// 25 SIGXFSZ
	SIGVTALRM,		// 26 SIGVTALRM
	SIGPROF,		// 27 SIGPROF
	SIGWINCH,		// 28 SIGWINCH
	SIGPOLL,		// 29 SIGIO/SIGPOLL
	0,				// 30 SIGPWR
	SIGSYS,			// 31 SIGSYS
};

#define LX_SI_TKILL		(-6)

// glibc reserves Linux 32 and 33; SIGRTMIN for applications is 34.
static const int kLinuxFirstUserRealtime = 34;


int
renguin_signal_to_haiku(int linuxSignal)
{
	if (linuxSignal > 0 && linuxSignal < 32)
		return kSignalMap[linuxSignal];
	if (linuxSignal == LX_SIGRTMIN)
		return SIGRESERVED1;
	if (linuxSignal == LX_SIGRTMIN + 1)
		return SIGRESERVED2;
	if (linuxSignal >= kLinuxFirstUserRealtime && linuxSignal <= LX_NSIG) {
		int haikuSignal = SIGNAL_REALTIME_MIN
			+ (linuxSignal - kLinuxFirstUserRealtime);
		if (haikuSignal <= SIGNAL_REALTIME_MAX)
			return haikuSignal;
	}
	return 0;
}


int
renguin_signal_from_haiku(int haikuSignal)
{
	for (int i = 1; i < 32; i++) {
		if (kSignalMap[i] == haikuSignal)
			return i;
	}
	if (haikuSignal >= SIGNAL_REALTIME_MIN && haikuSignal <= SIGNAL_REALTIME_MAX)
		return kLinuxFirstUserRealtime + (haikuSignal - SIGNAL_REALTIME_MIN);
	if (haikuSignal == SIGRESERVED1)
		return LX_SIGRTMIN;
	if (haikuSignal == SIGRESERVED2)
		return LX_SIGRTMIN + 1;
	return 0;
}


static int64
signal_status(status_t status)
{
	if (status == B_BAD_TEAM_ID || status == B_BAD_THREAD_ID)
		return -LX_ESRCH;
	return renguin_encode(status);
}


// #pragma mark - IDs


int64
renguin_sys_getpid()
{
	return thread_get_current_thread()->team->id;
}


int64
renguin_sys_gettid()
{
	return thread_get_current_thread()->id;
}


int64
renguin_sys_getppid()
{
	return signal_status(_user_process_info(0, PARENT_ID));
}


int64
renguin_sys_getpgid(int64 pid)
{
	return signal_status(_user_process_info((pid_t)pid, GROUP_ID));
}


int64
renguin_sys_getsid(int64 pid)
{
	return signal_status(_user_process_info((pid_t)pid, SESSION_ID));
}


int64
renguin_sys_setpgid(int64 pid, int64 pgid)
{
	return signal_status(_user_setpgid((pid_t)pid, (pid_t)pgid));
}


int64
renguin_sys_setsid()
{
	return renguin_encode(_user_setsid());
}


/*!	Batch 11: Linux programs run as a normal user. Renga's user is uid 0
	in Haiku; Linux programs that see uid 0 act as root (Chromium refuses
	to run without --no-sandbox, others warn or change what they do). With
	the switch "linux_user" (on), a team started without RENGUIN_ROOT=1
	sees ids 0 as 1000 (the "user" entry "linux" writes into the root's
	passwd and group files), for itself and for file owners; chown() to
	1000 means 0 again. Access checks are Haiku's, so nothing is refused
	that was allowed before. "linux --root" (and the root's "sudo")
	start programs with RENGUIN_ROOT=1.
*/
static bool sLinuxUser = true;
static const uint32 kLinuxUserID = 1000;


void
renguin_user_settings(bool asUser)
{
	sLinuxUser = asUser;
}


bool
renguin_user_default()
{
	return sLinuxUser;
}


static bool
as_user()
{
	renguin_team* data = renguin_current_team();
	return data != NULL && data->as_user;
}


uint32
renguin_linux_id(uint32 hostID)
{
	return hostID == 0 && as_user() ? kLinuxUserID : hostID;
}


uint32
renguin_host_id(uint32 linuxID)
{
	return linuxID == kLinuxUserID && as_user() ? 0 : linuxID;
}


int64
renguin_sys_getuid()
{
	return renguin_linux_id(thread_get_current_thread()->team->real_uid);
}


int64
renguin_sys_geteuid()
{
	return renguin_linux_id(
		thread_get_current_thread()->team->effective_uid);
}


int64
renguin_sys_getgid()
{
	return renguin_linux_id(thread_get_current_thread()->team->real_gid);
}


int64
renguin_sys_getegid()
{
	return renguin_linux_id(
		thread_get_current_thread()->team->effective_gid);
}


static int64
copy_out_ids(uint64 first, uint64 second, uint64 third, uint32 a, uint32 b,
	uint32 c)
{
	if (renguin_copy_out(first, &a, sizeof(a)) != B_OK
		|| renguin_copy_out(second, &b, sizeof(b)) != B_OK
		|| renguin_copy_out(third, &c, sizeof(c)) != B_OK) {
		return -LX_EFAULT;
	}
	return 0;
}


int64
renguin_sys_getresuid(uint64 ruid, uint64 euid, uint64 suid)
{
	Team* team = thread_get_current_thread()->team;
	return copy_out_ids(ruid, euid, suid, renguin_linux_id(team->real_uid),
		renguin_linux_id(team->effective_uid),
		renguin_linux_id(team->saved_set_uid));
}


int64
renguin_sys_getresgid(uint64 rgid, uint64 egid, uint64 sgid)
{
	Team* team = thread_get_current_thread()->team;
	return copy_out_ids(rgid, egid, sgid, renguin_linux_id(team->real_gid),
		renguin_linux_id(team->effective_gid),
		renguin_linux_id(team->saved_set_gid));
}


// #pragma mark - Signals


/*!	A record of the signals that end programs (SIGKILL, SIGTERM, SIGABRT,
	SIGSEGV, SIGBUS, SIGILL, SIGTRAP), sent by Linux programs with kill(),
	tkill() or tgkill(), for diagnosing programs that end each other or
	themselves (capped). A signal to the sender's own process comes with
	the sender's stack (abort(), raise() and crash handlers).
*/
static void
log_signal_send(const char* call, int64 target, int64 signal)
{
	if (signal != 9 && signal != 15 && signal != 6 && signal != 11
		&& signal != 7 && signal != 4 && signal != 5) {
		return;
	}
	static int32 sLogged = 0;
	if (atomic_add(&sLogged, 1) >= 48)
		return;
	Thread* thread = thread_get_current_thread();
	dprintf("renguin: team %" B_PRId32 " (%s) thread %" B_PRId32 ": %s(%"
		B_PRId64 ", %" B_PRId64 ")\n", thread->team->id, thread->team->Name(),
		thread->id, call, target, signal);
	bool own = target == thread->team->id;
	if (!own && target > 0) {
		Thread* targetThread = Thread::Get((thread_id)target);
		if (targetThread != NULL) {
			own = targetThread->team == thread->team;
			targetThread->ReleaseReference();
		}
	}
	if (own)
		foreign_personality_log_user_stack("renguin:");
}


int64
renguin_sys_kill(int64 pid, int64 signal)
{
	int haikuSignal = 0;
	if (signal != 0) {
		haikuSignal = renguin_signal_to_haiku((int)signal);
		if (haikuSignal == 0)
			return -LX_EINVAL;
	}

	// As in libroot's kill(): 0 means our own process group, which Haiku's
	// send_signal expresses as a negative group ID.
	if (pid == 0)
		pid = -thread_get_current_thread()->team->group_id;

	log_signal_send("kill", pid, signal);

	return signal_status(_user_send_signal((int32)pid, haikuSignal, NULL, 0));
}


int64
renguin_sys_tgkill(int64 tgid, int64 tid, int64 signal)
{
	if (tid <= 0 || (tgid <= 0 && tgid != -1))
		return -LX_EINVAL;

	int haikuSignal = 0;
	if (signal != 0) {
		haikuSignal = renguin_signal_to_haiku((int)signal);
		if (haikuSignal == 0)
			return -LX_EINVAL;
	}

	log_signal_send(tgid == -1 ? "tkill" : "tgkill", tid, signal);

	// tgkill checks that the thread is in the given thread group; tkill
	// (tgid == -1) does not.
	if (tgid != -1) {
		Thread* thread = Thread::Get((thread_id)tid);
		if (thread == NULL)
			return -LX_ESRCH;
		bool sameGroup = thread->team->id == (team_id)tgid;
		thread->ReleaseReference();
		if (!sameGroup)
			return -LX_ESRCH;
	}

	if (haikuSignal == 0) {
		// existence and permission check only
		return signal_status(_user_send_signal((int32)tid, 0, NULL,
			SIGNAL_FLAG_SEND_TO_THREAD));
	}

	// Linux marks these signals SI_TKILL, and glibc relies on it: its
	// pthread_cancel() and setxid handlers ignore signals that don't carry
	// SI_TKILL and the caller's PID. Haiku's own sending path would say
	// SI_USER, so the signal is built here; signal.cpp passes negative codes
	// through to Linux unchanged.
	Signal haikuSignalInfo(haikuSignal, LX_SI_TKILL, B_OK,
		thread_get_current_thread()->team->id);
	return signal_status(send_signal_to_thread_id((thread_id)tid,
		haikuSignalInfo, B_CHECK_PERMISSION));
}


// #pragma mark - Resource limits


//! Linux resource -> Haiku resource, or -1 if Haiku has no such limit.
static int
resource_to_haiku(uint64 resource)
{
	switch (resource) {
		case LX_RLIMIT_CPU:		return RLIMIT_CPU;
		case LX_RLIMIT_FSIZE:	return RLIMIT_FSIZE;
		case LX_RLIMIT_DATA:	return RLIMIT_DATA;
		case LX_RLIMIT_STACK:	return RLIMIT_STACK;
		case LX_RLIMIT_CORE:	return RLIMIT_CORE;
		case LX_RLIMIT_NOFILE:	return RLIMIT_NOFILE;
		case LX_RLIMIT_AS:		return RLIMIT_AS;
		default:				return -1;
	}
}


/*!	prlimit64 (and getrlimit/setrlimit through it). Haiku's struct rlimit is
	two 64-bit values with RLIM_INFINITY == ~0, like Linux's. Limits Haiku
	does not have read as unlimited and setting them is accepted, which is
	what a Linux kernel without the corresponding enforcement would do.
*/
int64
renguin_sys_prlimit64(int64 pid, uint64 resource, uint64 newLimit,
	uint64 oldLimit)
{
	if (pid != 0 && pid != thread_get_current_thread()->team->id)
		return -LX_EPERM;
	if (resource >= LX_RLIMIT_NLIMITS)
		return -LX_EINVAL;

	int haikuResource = resource_to_haiku(resource);
	renguin_scratch scratch;
	renguin_scratch_init(&scratch);

	if (oldLimit != 0) {
		linux_rlimit old = { LX_RLIM_INFINITY, LX_RLIM_INFINITY };
		if (haikuResource >= 0) {
			uint64 userLimit = renguin_scratch_alloc(&scratch,
				sizeof(struct rlimit));
			if (userLimit == 0)
				return -LX_EFAULT;
			if (_user_getrlimit(haikuResource,
					(struct rlimit*)(addr_t)userLimit) == 0) {
				struct rlimit limit;
				if (user_memcpy(&limit, (void*)(addr_t)userLimit,
						sizeof(limit)) != B_OK) {
					return -LX_EFAULT;
				}
				old.rlim_cur = limit.rlim_cur;
				old.rlim_max = limit.rlim_max;
			}
		}
		if (renguin_copy_out(oldLimit, &old, sizeof(old)) != B_OK)
			return -LX_EFAULT;
	}

	if (newLimit != 0) {
		linux_rlimit limit;
		if (renguin_copy_in(&limit, newLimit, sizeof(limit)) != B_OK)
			return -LX_EFAULT;
		if (limit.rlim_cur > limit.rlim_max)
			return -LX_EINVAL;
		if (haikuResource == RLIMIT_NOFILE) {
			struct rlimit haikuLimit;
			haikuLimit.rlim_cur = limit.rlim_cur;
			haikuLimit.rlim_max = limit.rlim_max;
			uint64 userLimit = renguin_scratch_put(&scratch, &haikuLimit,
				sizeof(haikuLimit));
			if (userLimit == 0)
				return -LX_EFAULT;
			int result = _user_setrlimit(haikuResource,
				(const struct rlimit*)(addr_t)userLimit);
			if (result != 0)
				return renguin_encode(result);
		}
		// Other limits are not enforced by Haiku; accept them.
	}
	return 0;
}


int64
renguin_sys_umask(uint64 mask)
{
	renguin_team* data = renguin_current_team();
	if (data == NULL)
		return 022;
	return (uint32)atomic_get_and_set((int32*)&data->umask,
		(int32)(mask & 0777));
}
