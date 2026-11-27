/*
 * Copyright 2026, Renguin contributors.
 * Distributed under the terms of the MIT License.
 *
 * Linux x86-64 system call dispatch.
 *
 * The kernel's system call entry (the foreign-ABI branch in
 * src/system/kernel/arch/x86/64/interrupts.S) calls renguin_syscall() with the
 * thread's iframe whenever a thread of a Linux team makes a system call. The
 * call number is in orig_rax and the arguments are in the Linux registers
 * (rdi, rsi, rdx, r10, r8, r9). The result, or a negative Linux errno, goes
 * back in rax. This is the job of FreeBSD's linux_sysent table and
 * linux_fetch_syscall_args() / linux_set_syscall_retval()
 * (sys/amd64/linux/linux_sysvec.c).
 *
 * Handlers live in file.cpp, mem.cpp, misc.cpp, process.cpp, tty.cpp,
 * exec.cpp, threads.cpp, futex.cpp, signal.cpp, fsops.cpp, poll.cpp,
 * procfs.cpp and sysinfo.cpp; this
 * file routes to them. Anything not implemented returns -ENOSYS and is logged
 * once per team and call number. The numbers are checked against
 * abi/linux_syscall_names.h (generated from FreeBSD's syscalls.master) by
 * tools/check-syscall-numbers.py.
 */


#include "renguin.h"
#include "linux_defs.h"

#include <string.h>

#include <algorithm>

#include <KernelExport.h>
#include <driver_settings.h>

#include <arch/x86/64/iframe.h>
#include <team.h>
#include <thread.h>
#include <util/atomic.h>

#include <stdio.h>
#include <stdlib.h>

#include "abi/linux_syscall_names.h"


//#define TRACE_RENGUIN_SYSCALL
#ifdef TRACE_RENGUIN_SYSCALL
#	define TRACE(x...) dprintf("renguin syscall: " x)
#else
#	define TRACE(x...) ;
#endif


// Linux x86-64 system call numbers (arch/x86/entry/syscalls/syscall_64.tbl).
enum {
	LINUX_read				= 0,
	LINUX_write				= 1,
	LINUX_open				= 2,
	LINUX_close				= 3,
	LINUX_stat				= 4,
	LINUX_fstat				= 5,
	LINUX_lstat				= 6,
	LINUX_poll				= 7,
	LINUX_lseek				= 8,
	LINUX_mmap				= 9,
	LINUX_mprotect			= 10,
	LINUX_munmap			= 11,
	LINUX_brk				= 12,
	LINUX_rt_sigaction		= 13,
	LINUX_rt_sigprocmask	= 14,
	LINUX_rt_sigreturn		= 15,
	LINUX_ioctl				= 16,
	LINUX_pread64			= 17,
	LINUX_pwrite64			= 18,
	LINUX_readv				= 19,
	LINUX_writev			= 20,
	LINUX_access			= 21,
	LINUX_pipe				= 22,
	LINUX_select			= 23,
	LINUX_sched_yield		= 24,
	LINUX_mremap			= 25,
	LINUX_msync				= 26,
	LINUX_madvise			= 28,
	LINUX_mincore			= 27,
	LINUX_dup				= 32,
	LINUX_dup2				= 33,
	LINUX_pause				= 34,
	LINUX_nanosleep			= 35,
	LINUX_getitimer			= 36,
	LINUX_alarm				= 37,
	LINUX_setitimer			= 38,
	LINUX_getpid			= 39,
	LINUX_sendfile			= 40,
	LINUX_socket			= 41,
	LINUX_connect			= 42,
	LINUX_accept			= 43,
	LINUX_sendto			= 44,
	LINUX_recvfrom			= 45,
	LINUX_sendmsg			= 46,
	LINUX_recvmsg			= 47,
	LINUX_shutdown			= 48,
	LINUX_bind				= 49,
	LINUX_listen			= 50,
	LINUX_getsockname		= 51,
	LINUX_getpeername		= 52,
	LINUX_socketpair		= 53,
	LINUX_setsockopt		= 54,
	LINUX_getsockopt		= 55,
	LINUX_clone				= 56,
	LINUX_fork				= 57,
	LINUX_vfork				= 58,
	LINUX_execve			= 59,
	LINUX_exit				= 60,
	LINUX_wait4				= 61,
	LINUX_kill				= 62,
	LINUX_uname				= 63,
	LINUX_fcntl				= 72,
	LINUX_flock				= 73,
	LINUX_fsync				= 74,
	LINUX_fdatasync			= 75,
	LINUX_truncate			= 76,
	LINUX_ftruncate			= 77,
	LINUX_getcwd			= 79,
	LINUX_chdir				= 80,
	LINUX_fchdir			= 81,
	LINUX_rename			= 82,
	LINUX_mkdir				= 83,
	LINUX_rmdir				= 84,
	LINUX_creat				= 85,
	LINUX_link				= 86,
	LINUX_unlink			= 87,
	LINUX_symlink			= 88,
	LINUX_readlink			= 89,
	LINUX_chmod				= 90,
	LINUX_fchmod			= 91,
	LINUX_chown				= 92,
	LINUX_fchown			= 93,
	LINUX_lchown			= 94,
	LINUX_umask				= 95,
	LINUX_gettimeofday		= 96,
	LINUX_getrlimit			= 97,
	LINUX_getrusage			= 98,
	LINUX_sysinfo			= 99,
	LINUX_times				= 100,
	LINUX_getuid			= 102,
	LINUX_getgid			= 104,
	LINUX_setuid			= 105,
	LINUX_setgid			= 106,
	LINUX_geteuid			= 107,
	LINUX_getegid			= 108,
	LINUX_setpgid			= 109,
	LINUX_getppid			= 110,
	LINUX_getpgrp			= 111,
	LINUX_setsid			= 112,
	LINUX_setreuid			= 113,
	LINUX_setregid			= 114,
	LINUX_getgroups			= 115,
	LINUX_setgroups			= 116,
	LINUX_setresuid			= 117,
	LINUX_getresuid			= 118,
	LINUX_setresgid			= 119,
	LINUX_getresgid			= 120,
	LINUX_getpgid			= 121,
	LINUX_setfsuid			= 122,
	LINUX_setfsgid			= 123,
	LINUX_getsid			= 124,
	LINUX_rt_sigpending		= 127,
	LINUX_rt_sigtimedwait	= 128,
	LINUX_rt_sigqueueinfo	= 129,
	LINUX_rt_sigsuspend		= 130,
	LINUX_sigaltstack		= 131,
	LINUX_utime				= 132,
	LINUX_mknod				= 133,
	LINUX_personality		= 135,
	LINUX_statfs			= 137,
	LINUX_fstatfs			= 138,
	LINUX_getpriority		= 140,
	LINUX_setpriority		= 141,
	LINUX_sched_setparam	= 142,
	LINUX_sched_getparam	= 143,
	LINUX_sched_setscheduler = 144,
	LINUX_sched_getscheduler = 145,
	LINUX_sched_get_priority_max = 146,
	LINUX_sched_get_priority_min = 147,
	LINUX_sched_rr_get_interval = 148,
	LINUX_mlock				= 149,
	LINUX_munlock			= 150,
	LINUX_mlockall			= 151,
	LINUX_munlockall		= 152,
	LINUX_prctl				= 157,
	LINUX_arch_prctl		= 158,
	LINUX_setrlimit			= 160,
	LINUX_chroot			= 161,
	LINUX_sync				= 162,
	LINUX_gettid			= 186,
	LINUX_readahead			= 187,
	LINUX_setxattr			= 188,
	LINUX_lsetxattr			= 189,
	LINUX_fsetxattr			= 190,
	LINUX_getxattr			= 191,
	LINUX_lgetxattr			= 192,
	LINUX_fgetxattr			= 193,
	LINUX_listxattr			= 194,
	LINUX_llistxattr		= 195,
	LINUX_flistxattr		= 196,
	LINUX_removexattr		= 197,
	LINUX_lremovexattr		= 198,
	LINUX_fremovexattr		= 199,
	LINUX_tkill				= 200,
	LINUX_time				= 201,
	LINUX_futex				= 202,
	LINUX_sched_setaffinity	= 203,
	LINUX_sched_getaffinity	= 204,
	LINUX_epoll_create		= 213,
	LINUX_getdents64		= 217,
	LINUX_set_tid_address	= 218,
	LINUX_fadvise64			= 221,
	LINUX_clock_gettime		= 228,
	LINUX_clock_getres		= 229,
	LINUX_clock_nanosleep	= 230,
	LINUX_exit_group		= 231,
	LINUX_epoll_wait		= 232,
	LINUX_epoll_ctl			= 233,
	LINUX_tgkill			= 234,
	LINUX_utimes			= 235,
	LINUX_inotify_init		= 253,
	LINUX_inotify_add_watch	= 254,
	LINUX_inotify_rm_watch	= 255,
	LINUX_openat			= 257,
	LINUX_mkdirat			= 258,
	LINUX_mknodat			= 259,
	LINUX_fchownat			= 260,
	LINUX_futimesat			= 261,
	LINUX_newfstatat		= 262,
	LINUX_unlinkat			= 263,
	LINUX_renameat			= 264,
	LINUX_linkat			= 265,
	LINUX_symlinkat			= 266,
	LINUX_readlinkat		= 267,
	LINUX_fchmodat			= 268,
	LINUX_faccessat			= 269,
	LINUX_pselect6			= 270,
	LINUX_ppoll				= 271,
	LINUX_set_robust_list	= 273,
	LINUX_get_robust_list	= 274,
	LINUX_sync_file_range	= 277,
	LINUX_utimensat			= 280,
	LINUX_epoll_pwait		= 281,
	LINUX_signalfd			= 282,
	LINUX_timerfd_create	= 283,
	LINUX_eventfd			= 284,
	LINUX_fallocate			= 285,
	LINUX_timerfd_settime	= 286,
	LINUX_timerfd_gettime	= 287,
	LINUX_accept4			= 288,
	LINUX_signalfd4			= 289,
	LINUX_eventfd2			= 290,
	LINUX_epoll_create1		= 291,
	LINUX_dup3				= 292,
	LINUX_preadv			= 295,
	LINUX_pwritev			= 296,
	LINUX_pipe2				= 293,
	LINUX_inotify_init1		= 294,
	LINUX_rt_tgsigqueueinfo	= 297,
	LINUX_recvmmsg			= 299,
	LINUX_prlimit64			= 302,
	LINUX_syncfs			= 306,
	LINUX_sendmmsg			= 307,
	LINUX_getcpu			= 309,
	LINUX_renameat2			= 316,
	LINUX_getrandom			= 318,
	LINUX_memfd_create		= 319,
	LINUX_membarrier		= 324,
	LINUX_mlock2			= 325,
	LINUX_preadv2			= 327,
	LINUX_pwritev2			= 328,
	LINUX_copy_file_range	= 326,
	LINUX_statx				= 332,
	LINUX_rseq				= 334,
	LINUX_waitid			= 247,
	LINUX_clone3			= 435,
	LINUX_close_range		= 436,
	LINUX_faccessat2		= 439,
	LINUX_epoll_pwait2		= 441,
	LINUX_fchmodat2			= 452,
	LINUX_shmget			= 29,
	LINUX_shmat				= 30,
	LINUX_shmctl			= 31,
	LINUX_semget			= 64,
	LINUX_semop				= 65,
	LINUX_semctl			= 66,
	LINUX_shmdt				= 67,
	LINUX_msgget			= 68,
	LINUX_msgsnd			= 69,
	LINUX_msgrcv			= 70,
	LINUX_msgctl			= 71,
	LINUX_semtimedop		= 220,
	LINUX_timer_create		= 222,
	LINUX_timer_settime		= 223,
	LINUX_timer_gettime		= 224,
	LINUX_timer_getoverrun	= 225,
	LINUX_timer_delete		= 226,
};


struct renguin_team*
renguin_current_team()
{
	return (renguin_team*)thread_get_current_thread()->team->personality_data;
}


static void
log_unimplemented(uint64 number)
{
	// Log each missing call once per team: a loop calling it would
	// otherwise flood the syslog. A small bitmap is enough for the table.
	static team_id sLastTeam = -1;
	static uint64 sLogged[(LINUX_SYSCALL_COUNT + 63) / 64];

	team_id team = thread_get_current_thread()->team->id;
	if (team != sLastTeam) {
		sLastTeam = team;
		memset(sLogged, 0, sizeof(sLogged));
	}
	if (number < LINUX_SYSCALL_COUNT) {
		uint64 bit = (uint64)1 << (number % 64);
		if ((sLogged[number / 64] & bit) != 0)
			return;
		sLogged[number / 64] |= bit;
	}

	const char* name = number < LINUX_SYSCALL_COUNT
		? kLinuxSyscallNames[number] : NULL;
	dprintf("renguin: team %" B_PRId32 ": unimplemented Linux syscall %"
		B_PRIu64 " (%s)\n", team, number, name != NULL ? name : "?");
}


/*!	Opt-in system call trace, for diagnosing Linux programs. In
	~/config/settings/kernel/drivers/renguin (or the system settings):
	  trace_errors <pattern>	every call that returns an error (EAGAIN and
							EINTR excepted)
	  trace_all <pattern>		every call, also when it starts (to see
							where a thread blocks)
	  trace_process <pattern>	only processes, threads and descriptor
							passing (clone, fork, execve, wait4,
							socketpair, sendmsg, recvmsg, dup2/3, kill),
							in every matching process
	of the Linux programs whose command line (read from the program's own
	argument strings; the team's args stop at 64 characters) contains
	<pattern> (quote it: driver settings split at "="; a leading "^"
	anchors it at the start), with the first
	three arguments and the result, to the syslog. At most 4000 lines per
	boot; trace_all follows only the first matching process and leaves out
	successful file lookups, clocks, getpid/gettid, getrandom, mremap and
	ENOENT path searches. Read again at every exec of a Linux program, so it can
	be changed without restarting. Off by default.

	"trace_buffer true" (with any of the above) keeps the lines in memory,
	with a millisecond time stamp, instead of writing each to the syslog as
	it happens, and writes them out when the traced process ends (up to
	16000 lines). Writing every line at once slows the traced program down
	so much that timing-dependent problems disappear: Linux Chrome's child
	processes worked under trace_all and failed without it.
*/
static char sTracePattern[64];
static bool sTraceAll;
static bool sTraceProcess;
static int32 sTraceGeneration = 1;
static bool sTraceBuffer;

static const int32 kTraceBufferLines = 16000;
static const size_t kTraceLineSize = 176;
static char* sTraceRing;
static int32 sTraceRingCount;
static spinlock sTraceRingLock = B_SPINLOCK_INITIALIZER;


//!	Writes a trace line, or keeps it for later (trace_buffer).
static void
trace_emit(const char* line)
{
	if (!sTraceBuffer) {
		dprintf("%s", line);
		return;
	}
	if (sTraceRing == NULL) {
		char* ring = (char*)malloc(kTraceBufferLines * kTraceLineSize);
		if (ring == NULL || atomic_pointer_test_and_set(&sTraceRing, ring,
				(char*)NULL) != NULL) {
			free(ring);
		}
		if (sTraceRing == NULL) {
			dprintf("%s", line);
			return;
		}
	}
	cpu_status state = disable_interrupts();
	acquire_spinlock(&sTraceRingLock);
	if (sTraceRingCount < kTraceBufferLines) {
		snprintf(sTraceRing + sTraceRingCount * kTraceLineSize,
			kTraceLineSize, "%" B_PRId64 " %s", system_time() / 1000, line);
		sTraceRingCount++;
	}
	release_spinlock(&sTraceRingLock);
	restore_interrupts(state);
}


/*!	A team went away: with trace_buffer, write out what was kept when it
	was a traced one.
*/
void
renguin_trace_team_released(renguin_team* data)
{
	if (data == NULL || data->trace_match != 1 || sTraceRing == NULL)
		return;
	dprintf("renguin trace: buffered lines follow (ms since boot first)\n");
	for (int32 i = 0; i < kTraceBufferLines; i++) {
		cpu_status state = disable_interrupts();
		acquire_spinlock(&sTraceRingLock);
		bool more = i < sTraceRingCount;
		char line[kTraceLineSize];
		if (more)
			memcpy(line, sTraceRing + i * kTraceLineSize, kTraceLineSize);
		release_spinlock(&sTraceRingLock);
		restore_interrupts(state);
		if (!more)
			break;
		line[kTraceLineSize - 1] = '\0';
		dprintf("renguin trace: @%s", line);
	}
	cpu_status state = disable_interrupts();
	acquire_spinlock(&sTraceRingLock);
	sTraceRingCount = 0;
	release_spinlock(&sTraceRingLock);
	restore_interrupts(state);
	dprintf("renguin trace: end of buffered lines\n");
}


void
renguin_reload_trace_settings()
{
	char pattern[sizeof(sTracePattern)] = "";
	bool all = false;
	bool process = false;
	bool buffer = false;
	bool xattrs = true;
	bool hardlinkCopy = true;
	bool linuxUser = true;
	if (void* handle = load_driver_settings("renguin")) {
		// not trace settings, but read as often (at every exec)
		xattrs = get_driver_boolean_parameter(handle, "xattrs", true, true);
		hardlinkCopy = get_driver_boolean_parameter(handle, "hardlink_copy",
			true, true);
		linuxUser = get_driver_boolean_parameter(handle, "linux_user", true,
			true);
		buffer = get_driver_boolean_parameter(handle, "trace_buffer", false,
			true);
		const char* value = get_driver_parameter(handle, "trace_all", NULL,
			NULL);
		process = false;
		if (value != NULL)
			all = true;
		else if ((value = get_driver_parameter(handle, "trace_process", NULL,
				NULL)) != NULL) {
			all = process = true;
		} else
			value = get_driver_parameter(handle, "trace_errors", NULL, NULL);
		if (value != NULL)
			strlcpy(pattern, value, sizeof(pattern));
		unload_driver_settings(handle);
	}
	renguin_xattr_settings(xattrs);
	renguin_hardlink_settings(hardlinkCopy);
	renguin_user_settings(linuxUser);
	if (strcmp(pattern, sTracePattern) != 0 || all != sTraceAll
		|| process != sTraceProcess || buffer != sTraceBuffer) {
		dprintf("renguin: system call trace %s%s%s\n",
			pattern[0] != '\0' ? (all ? "of all calls" : "of errors") : "off",
			pattern[0] != '\0' ? " for " : "", pattern);
		sTraceAll = all;
		sTraceProcess = process;
		sTraceBuffer = buffer;
		strlcpy(sTracePattern, pattern, sizeof(sTracePattern));
		atomic_add(&sTraceGeneration, 1);
	}
}


static void
trace_call(uint64 number, uint64 a1, uint64 a2, uint64 a3, int64 result,
	bool entering = false)
{
	if (sTracePattern[0] == '\0' || (entering && !sTraceAll))
		return;
	if (!sTraceAll && (result >= 0 || result == -LX_EAGAIN
			|| result == -LX_EINTR)) {
		return;
	}
	if (sTraceProcess) {
		// trace_process: processes, threads and descriptor passing only
		switch (number) {
			case LINUX_clone: case LINUX_clone3: case LINUX_fork:
			case LINUX_vfork: case LINUX_execve: case LINUX_wait4:
			case LINUX_waitid: case LINUX_close_range:
			case LINUX_socketpair: case LINUX_sendmsg: case LINUX_recvmsg:
			case LINUX_dup2: case LINUX_dup3: case LINUX_kill:
			case LINUX_tgkill: case LINUX_tkill:
				break;
			default:
				return;
		}
	} else if (sTraceAll && (entering || result >= 0)) {
		// Leave out the calls that succeed in great numbers and say
		// little (memory, file lookups, clocks); their errors still show.
		switch (number) {
			case LINUX_open: case LINUX_openat: case LINUX_stat:
			case LINUX_fstat: case LINUX_lstat: case LINUX_newfstatat:
			case LINUX_access: case LINUX_faccessat: case LINUX_readlink:
			case LINUX_getpid: case LINUX_gettid: case LINUX_clock_gettime:
			case LINUX_getrandom:
				return;
		}
	}
	if (sTraceAll && (number == LINUX_mremap
			|| (!entering && result == -LX_ENOENT))) {
		// path searches, and musl's pthread_getattr_np(), which probes the
		// main thread's stack with one mremap() per page (mem.cpp)
		return;
	}
	Thread* thread = thread_get_current_thread();
	renguin_team* data = (renguin_team*)thread->team->personality_data;
	if (data == NULL)
		return;
	int32 generation = atomic_get(&sTraceGeneration);
	if (data->trace_generation != generation) {
		// The command line from the program's own argument strings (the
		// team's args are cut at 64 characters), once per team and setting.
		char commandLine[1024];
		size_t length = data->arg_end > data->arg_start
			? std::min((size_t)(data->arg_end - data->arg_start),
				sizeof(commandLine) - 1) : 0;
		if (length == 0 || renguin_copy_in(commandLine, data->arg_start,
				length) != B_OK) {
			length = 0;
		}
		for (size_t i = 0; i < length; i++) {
			if (commandLine[i] == '\0')
				commandLine[i] = ' ';
		}
		commandLine[length] = '\0';
		// "trace_all": only the first matching process (a program that
		// starts many alike would use up the record before it is useful)
		static team_id sTracedTeam = -1;
		// a leading '^' anchors the pattern at the start of the command line
		bool match = sTracePattern[0] == '^'
			? strncmp(commandLine, sTracePattern + 1,
				strlen(sTracePattern + 1)) == 0
			: strstr(commandLine, sTracePattern) != NULL;
		if (match && sTraceAll && !sTraceProcess) {
			team_id id = thread->team->id;
			team_id previous = atomic_test_and_set(&sTracedTeam, id, -1);
			if (previous != -1 && previous != id)
				match = false;
		}
		data->trace_match = match ? 1 : -1;
		data->trace_generation = generation;
	}
	if (data->trace_match != 1)
		return;
	static int32 sTraced = 0;
	if (atomic_add(&sTraced, 1) >= (sTraceBuffer ? kTraceBufferLines : 4000))
		return;
	const char* name = number < LINUX_SYSCALL_COUNT
		? kLinuxSyscallNames[number] : NULL;
	// the path argument of the calls that take one, for reading the trace
	char path[96] = "";
	uint64 pathArgument = 0;
	switch (number) {
		case LINUX_open: case LINUX_stat: case LINUX_lstat:
		case LINUX_access: case LINUX_readlink: case LINUX_execve:
			pathArgument = a1;
			break;
		case LINUX_openat: case LINUX_newfstatat: case 267:	// readlinkat
		case LINUX_faccessat:
			pathArgument = a2;
			break;
	}
	if (pathArgument != 0
		&& renguin_copy_string_in(path, pathArgument, sizeof(path)) < 0) {
		path[0] = '\0';
	}
	char line[kTraceLineSize];
	if (entering) {
		// (with trace_all: shows the call a thread blocks in)
		snprintf(line, sizeof(line), "renguin trace: %" B_PRId32 ":%"
			B_PRId32 " > %s(%#" B_PRIx64 ", %#" B_PRIx64 ", %#" B_PRIx64
			")%s%s\n", thread->team->id, thread->id,
			name != NULL ? name : "?", a1, a2, a3,
			path[0] != '\0' ? " " : "", path);
	} else {
		snprintf(line, sizeof(line), "renguin trace: %" B_PRId32 ":%"
			B_PRId32 " %s(%#" B_PRIx64 ", %#" B_PRIx64 ", %#" B_PRIx64
			") = %" B_PRId64 "%s%s\n", thread->team->id, thread->id,
			name != NULL ? name : "?", a1, a2, a3, result,
			path[0] != '\0' ? " " : "", path);
	}
	trace_emit(line);
}

void
renguin_syscall(struct iframe* frame)
{
	uint64 number = frame->orig_rax;
	uint64 a1 = frame->di, a2 = frame->si, a3 = frame->dx;
	uint64 a4 = frame->r10, a5 = frame->r8, a6 = frame->r9;
	int64 result;

	TRACE("team %" B_PRId32 " call %" B_PRIu64 "\n",
		thread_get_current_thread()->team->id, number);

	// Restart state belongs to one call. Haiku never clears "always
	// restart" once a call (sigwait) has set it, and the policy may be left
	// from a call that was restarted without a handler running.
	Thread* thread = thread_get_current_thread();
	if ((thread->flags & THREAD_FLAGS_ALWAYS_RESTART_SYSCALL) != 0)
		atomic_and(&thread->flags, ~THREAD_FLAGS_ALWAYS_RESTART_SYSCALL);
	renguin_thread* threadData = (renguin_thread*)thread->personality_data;
	if (threadData != NULL)
		threadData->restart_policy = RENGUIN_RESTART_SYS;

	trace_call(number, a1, a2, a3, 0, true);

	switch (number) {
		// --- I/O (file.cpp) ---
		case LINUX_read:
			result = renguin_sys_read((int)a1, a2, a3);
			break;
		case LINUX_write:
			result = renguin_sys_write((int)a1, a2, a3);
			break;
		case LINUX_pread64:
			result = renguin_sys_pread64((int)a1, a2, a3, (int64)a4);
			break;
		case LINUX_pwrite64:
			result = renguin_sys_pwrite64((int)a1, a2, a3, (int64)a4);
			break;
		case LINUX_readv:
			result = renguin_sys_readv((int)a1, a2, a3);
			break;
		case LINUX_writev:
			result = renguin_sys_writev((int)a1, a2, a3);
			break;
		// x86-64 passes the offset whole in the fourth argument (the
		// fifth, its high half, is for 32-bit ABIs).
		case LINUX_preadv:
			result = renguin_sys_preadv((int)a1, a2, a3, (int64)a4, 0, false,
				false);
			break;
		case LINUX_pwritev:
			result = renguin_sys_preadv((int)a1, a2, a3, (int64)a4, 0, true,
				false);
			break;
		case LINUX_preadv2:
			result = renguin_sys_preadv((int)a1, a2, a3, (int64)a4, a6, false,
				true);
			break;
		case LINUX_pwritev2:
			result = renguin_sys_preadv((int)a1, a2, a3, (int64)a4, a6, true,
				true);
			break;
		case LINUX_lseek:
			result = renguin_sys_lseek((int)a1, (int64)a2, (int)a3);
			break;
		case LINUX_ioctl:
			result = renguin_sys_ioctl((int)a1, a2, a3);
			break;
		case LINUX_fsync:
		case LINUX_fdatasync:
			result = renguin_sys_fsync((int)a1);
			break;
		case LINUX_ftruncate:
			result = renguin_sys_ftruncate((int)a1, (int64)a2);
			break;

		// --- Opening, stat, paths (file.cpp) ---
		case LINUX_open:
			result = renguin_sys_openat(LX_AT_FDCWD, a1, a2, a3);
			break;
		case LINUX_openat:
			result = renguin_sys_openat((int)a1, a2, a3, a4);
			break;
		case LINUX_close:
			result = renguin_sys_close((int)a1);
			break;
		case LINUX_stat:
			result = renguin_sys_newfstatat(LX_AT_FDCWD, a1, a2, 0);
			break;
		case LINUX_lstat:
			result = renguin_sys_newfstatat(LX_AT_FDCWD, a1, a2,
				LX_AT_SYMLINK_NOFOLLOW);
			break;
		case LINUX_fstat:
			result = renguin_sys_fstat((int)a1, a2);
			break;
		case LINUX_newfstatat:
			result = renguin_sys_newfstatat((int)a1, a2, a3, a4);
			break;
		case LINUX_readlink:
			result = renguin_sys_readlinkat(LX_AT_FDCWD, a1, a2, a3);
			break;
		case LINUX_readlinkat:
			result = renguin_sys_readlinkat((int)a1, a2, a3, a4);
			break;
		case LINUX_access:
			result = renguin_sys_faccessat(LX_AT_FDCWD, a1, a2, 0);
			break;
		case LINUX_faccessat:
			result = renguin_sys_faccessat((int)a1, a2, a3, 0);
			break;
		case LINUX_faccessat2:
			result = renguin_sys_faccessat((int)a1, a2, a3, a4);
			break;
		case LINUX_getdents64:
			result = renguin_sys_getdents64((int)a1, a2, a3);
			break;
		case LINUX_getcwd:
			result = renguin_sys_getcwd(a1, a2);
			break;
		case LINUX_chdir:
			result = renguin_sys_chdir(a1);
			break;
		case LINUX_fchdir:
			result = renguin_sys_fchdir((int)a1);
			break;
		case LINUX_mkdir:
			result = renguin_sys_mkdirat(LX_AT_FDCWD, a1, a2);
			break;
		case LINUX_mkdirat:
			result = renguin_sys_mkdirat((int)a1, a2, a3);
			break;
		case LINUX_unlink:
			result = renguin_sys_unlinkat(LX_AT_FDCWD, a1, 0);
			break;
		case LINUX_rmdir:
			result = renguin_sys_unlinkat(LX_AT_FDCWD, a1, LX_AT_REMOVEDIR);
			break;
		case LINUX_unlinkat:
			result = renguin_sys_unlinkat((int)a1, a2, a3);
			break;
		case LINUX_rename:
			result = renguin_sys_renameat(LX_AT_FDCWD, a1, LX_AT_FDCWD, a2);
			break;
		case LINUX_renameat:
			result = renguin_sys_renameat((int)a1, a2, (int)a3, a4);
			break;
		case LINUX_renameat2:
			// RENAME_NOREPLACE/EXCHANGE/WHITEOUT are not supported yet.
			result = a5 != 0 ? -LX_EINVAL
				: renguin_sys_renameat((int)a1, a2, (int)a3, a4);
			break;
		case LINUX_umask:
			result = renguin_sys_umask(a1);
			break;
		case LINUX_statx:
			result = renguin_sys_statx((int)a1, a2, a3, a4, a5);
			break;

		// --- More file system calls (fsops.cpp) ---
		case LINUX_statfs:
			result = renguin_sys_statfs(a1, a2);
			break;
		case LINUX_fstatfs:
			result = renguin_sys_fstatfs((int)a1, a2);
			break;
		case LINUX_chmod:
			result = renguin_sys_fchmodat(LX_AT_FDCWD, a1, a2, 0);
			break;
		case LINUX_fchmod:
			result = renguin_sys_fchmod((int)a1, a2);
			break;
		case LINUX_fchmodat:
			result = renguin_sys_fchmodat((int)a1, a2, a3, 0);
			break;
		case LINUX_fchmodat2:
			result = renguin_sys_fchmodat((int)a1, a2, a3, a4);
			break;
		case LINUX_chown:
			result = renguin_sys_fchownat(LX_AT_FDCWD, a1, a2, a3, 0);
			break;
		case LINUX_lchown:
			result = renguin_sys_fchownat(LX_AT_FDCWD, a1, a2, a3,
				LX_AT_SYMLINK_NOFOLLOW);
			break;
		case LINUX_fchown:
			result = renguin_sys_fchown((int)a1, a2, a3);
			break;
		case LINUX_fchownat:
			result = renguin_sys_fchownat((int)a1, a2, a3, a4, a5);
			break;
		case LINUX_utimensat:
			result = renguin_sys_utimensat((int)a1, a2, a3, a4);
			break;
		case LINUX_utimes:
			result = renguin_sys_futimesat(LX_AT_FDCWD, a1, a2);
			break;
		case LINUX_futimesat:
			result = renguin_sys_futimesat((int)a1, a2, a3);
			break;
		case LINUX_utime:
			result = renguin_sys_utime(a1, a2);
			break;
		case LINUX_link:
			result = renguin_sys_linkat(LX_AT_FDCWD, a1, LX_AT_FDCWD, a2, 0);
			break;
		case LINUX_linkat:
			result = renguin_sys_linkat((int)a1, a2, (int)a3, a4, a5);
			break;
		case LINUX_symlink:
			result = renguin_sys_symlinkat(a1, LX_AT_FDCWD, a2);
			break;
		case LINUX_symlinkat:
			result = renguin_sys_symlinkat(a1, (int)a2, a3);
			break;
		case LINUX_mknod:
			result = renguin_sys_mknodat(LX_AT_FDCWD, a1, a2, a3);
			break;
		case LINUX_mknodat:
			result = renguin_sys_mknodat((int)a1, a2, a3, a4);
			break;
		case LINUX_creat:
			result = renguin_sys_openat(LX_AT_FDCWD, a1,
				LX_O_CREAT | LX_O_WRONLY | LX_O_TRUNC, a2);
			break;
		case LINUX_truncate:
			result = renguin_sys_truncate(a1, (int64)a2);
			break;
		case LINUX_fadvise64:
			result = renguin_sys_fadvise64((int)a1, (int64)a2, (int64)a3, a4);
			break;
		case LINUX_readahead:
			result = renguin_sys_readahead((int)a1, (int64)a2, a3);
			break;
		case LINUX_sync_file_range:
			result = renguin_sys_fadvise64((int)a1, 0, 0, 0) == -LX_EBADF
				? -LX_EBADF : 0;
			break;
		case LINUX_fallocate:
			result = renguin_sys_fallocate((int)a1, a2, (int64)a3, (int64)a4);
			break;
		case LINUX_sync:
			result = renguin_sys_sync();
			break;
		case LINUX_syncfs:
			result = renguin_sys_syncfs((int)a1);
			break;
		case LINUX_flock:
			result = renguin_sys_flock((int)a1, a2);
			break;
		case LINUX_sendfile:
			result = renguin_sys_sendfile((int)a1, (int)a2, a3, a4);
			break;
		// Extended attributes: the user. namespace is the file's Haiku
		// attributes (xattr.cpp); the others are EOPNOTSUPP, which ls, cp
		// and tar take as "no ACLs/attributes".
		case LINUX_setxattr:
			result = renguin_sys_setxattr(XATTR_PATH, a1, a2, a3, a4, a5);
			break;
		case LINUX_lsetxattr:
			result = renguin_sys_setxattr(XATTR_LINK, a1, a2, a3, a4, a5);
			break;
		case LINUX_fsetxattr:
			result = renguin_sys_setxattr(XATTR_FD, a1, a2, a3, a4, a5);
			break;
		case LINUX_getxattr:
			result = renguin_sys_getxattr(XATTR_PATH, a1, a2, a3, a4);
			break;
		case LINUX_lgetxattr:
			result = renguin_sys_getxattr(XATTR_LINK, a1, a2, a3, a4);
			break;
		case LINUX_fgetxattr:
			result = renguin_sys_getxattr(XATTR_FD, a1, a2, a3, a4);
			break;
		case LINUX_listxattr:
			result = renguin_sys_listxattr(XATTR_PATH, a1, a2, a3);
			break;
		case LINUX_llistxattr:
			result = renguin_sys_listxattr(XATTR_LINK, a1, a2, a3);
			break;
		case LINUX_flistxattr:
			result = renguin_sys_listxattr(XATTR_FD, a1, a2, a3);
			break;
		case LINUX_removexattr:
			result = renguin_sys_removexattr(XATTR_PATH, a1, a2);
			break;
		case LINUX_lremovexattr:
			result = renguin_sys_removexattr(XATTR_LINK, a1, a2);
			break;
		case LINUX_fremovexattr:
			result = renguin_sys_removexattr(XATTR_FD, a1, a2);
			break;
		// cp and others fall back to read/write; not logged.
		case LINUX_copy_file_range:
			result = -LX_ENOSYS;
			break;
		case LINUX_chroot:
			result = renguin_sys_chroot(a1);
			break;

		// --- Sockets (net.cpp) ---
		case LINUX_socket:
			result = renguin_sys_socket((int64)(int32)a1, a2,
				(int64)(int32)a3);
			break;
		case LINUX_socketpair:
			result = renguin_sys_socketpair((int64)(int32)a1, a2,
				(int64)(int32)a3, a4);
			break;
		case LINUX_bind:
			result = renguin_sys_bind((int)a1, a2, (uint32)a3);
			break;
		case LINUX_connect:
			result = renguin_sys_connect((int)a1, a2, (uint32)a3);
			break;
		case LINUX_listen:
			result = renguin_sys_listen((int)a1, (int64)(int32)a2);
			break;
		case LINUX_accept:
			result = renguin_sys_accept4((int)a1, a2, a3, 0);
			break;
		case LINUX_accept4:
			result = renguin_sys_accept4((int)a1, a2, a3, a4);
			break;
		case LINUX_getsockname:
			result = renguin_sys_getsockname((int)a1, a2, a3);
			break;
		case LINUX_getpeername:
			result = renguin_sys_getpeername((int)a1, a2, a3);
			break;
		case LINUX_shutdown:
			result = renguin_sys_shutdown((int)a1, (int64)(int32)a2);
			break;
		case LINUX_sendto:
			result = renguin_sys_sendto((int)a1, a2, a3, a4, a5,
				(uint32)a6);
			break;
		case LINUX_recvfrom:
			result = renguin_sys_recvfrom((int)a1, a2, a3, a4, a5, a6);
			break;
		case LINUX_sendmsg:
			result = renguin_sys_sendmsg((int)a1, a2, a3);
			break;
		case LINUX_recvmsg:
			result = renguin_sys_recvmsg((int)a1, a2, a3);
			break;
		case LINUX_sendmmsg:
			result = renguin_sys_sendmmsg((int)a1, a2, (uint32)a3, a4);
			break;
		case LINUX_recvmmsg:
			result = renguin_sys_recvmmsg((int)a1, a2, (uint32)a3, a4, a5);
			break;
		case LINUX_setsockopt:
			result = renguin_sys_setsockopt((int)a1, (int64)(int32)a2,
				(int64)(int32)a3, a4, (uint32)a5);
			break;
		case LINUX_getsockopt:
			result = renguin_sys_getsockopt((int)a1, (int64)(int32)a2,
				(int64)(int32)a3, a4, a5);
			break;

		// --- Event and memory descriptors on Renga's own (fds.cpp) ---
		case LINUX_epoll_create:
			result = renguin_sys_epoll_create((int64)(int32)a1);
			break;
		case LINUX_epoll_create1:
			result = renguin_sys_epoll_create1(a1);
			break;
		case LINUX_epoll_ctl:
			result = renguin_sys_epoll_ctl((int)a1, (int64)(int32)a2,
				(int)a3, a4);
			break;
		case LINUX_epoll_wait:
			result = renguin_sys_epoll_pwait((int)a1, a2, (int64)(int32)a3,
				(int64)(int32)a4, 0, 0);
			break;
		case LINUX_epoll_pwait:
			result = renguin_sys_epoll_pwait((int)a1, a2, (int64)(int32)a3,
				(int64)(int32)a4, a5, a6);
			break;
		case LINUX_epoll_pwait2:
			result = renguin_sys_epoll_pwait2((int)a1, a2, (int64)(int32)a3,
				a4, a5, a6);
			break;
		case LINUX_eventfd:
			result = renguin_sys_eventfd2(a1, 0);
			break;
		case LINUX_eventfd2:
			result = renguin_sys_eventfd2(a1, a2);
			break;
		case LINUX_timerfd_create:
			result = renguin_sys_timerfd_create((int64)(int32)a1, a2);
			break;
		case LINUX_timerfd_settime:
			result = renguin_sys_timerfd_settime((int)a1, a2, a3, a4);
			break;
		case LINUX_timerfd_gettime:
			result = renguin_sys_timerfd_gettime((int)a1, a2);
			break;
		case LINUX_signalfd:
			result = renguin_sys_signalfd4((int)a1, a2, a3, 0);
			break;
		case LINUX_signalfd4:
			result = renguin_sys_signalfd4((int)a1, a2, a3, a4);
			break;
		case LINUX_inotify_init:
			result = renguin_sys_inotify_init1(0);
			break;
		case LINUX_inotify_init1:
			result = renguin_sys_inotify_init1(a1);
			break;
		case LINUX_inotify_add_watch:
			result = renguin_sys_inotify_add_watch((int)a1, a2, a3);
			break;
		case LINUX_inotify_rm_watch:
			result = renguin_sys_inotify_rm_watch((int)a1, (int64)(int32)a2);
			break;
		case LINUX_memfd_create:
			result = renguin_sys_memfd_create(a1, a2);
			break;

		// --- Waiting for descriptors (poll.cpp) ---
		case LINUX_poll:
			result = renguin_sys_poll(a1, a2, (int64)a3);
			break;
		case LINUX_ppoll:
			result = renguin_sys_ppoll(a1, a2, a3, a4, a5);
			break;
		case LINUX_select:
			result = renguin_sys_select((int64)(int32)a1, a2, a3, a4, a5);
			break;
		case LINUX_pselect6:
			result = renguin_sys_pselect6((int64)(int32)a1, a2, a3, a4, a5,
				a6);
			break;

		// --- Descriptors (file.cpp) ---
		case LINUX_dup:
			result = renguin_sys_dup((int)a1);
			break;
		case LINUX_dup2:
			result = renguin_sys_dup3((int)a1, (int)a2, 0, true);
			break;
		case LINUX_dup3:
			result = renguin_sys_dup3((int)a1, (int)a2, a3, false);
			break;
		case LINUX_pipe:
			result = renguin_sys_pipe2(a1, 0);
			break;
		case LINUX_pipe2:
			result = renguin_sys_pipe2(a1, a2);
			break;
		case LINUX_fcntl:
			result = renguin_sys_fcntl((int)a1, a2, a3);
			break;

		// --- Memory (mem.cpp) ---
		case LINUX_brk:
			result = renguin_sys_brk(a1);
			break;
		case LINUX_mmap:
			result = renguin_sys_mmap(a1, a2, a3, a4, (int64)a5, (int64)a6);
			break;
		case LINUX_munmap:
			result = renguin_sys_munmap(a1, a2);
			break;
		case LINUX_mprotect:
			result = renguin_sys_mprotect(a1, a2, a3);
			break;
		case LINUX_mremap:
			result = renguin_sys_mremap(a1, a2, a3, a4, a5);
			break;
		case LINUX_msync:
			result = renguin_sys_msync(a1, a2, a3);
			break;
		case LINUX_madvise:
			result = renguin_sys_madvise(a1, a2, a3);
			break;
		case LINUX_mincore:
			result = renguin_sys_mincore(a1, a2, a3);
			break;

		// --- Time, TLS, misc (misc.cpp) ---
		case LINUX_arch_prctl:
			result = renguin_sys_arch_prctl(a1, a2);
			break;
		case LINUX_prctl:
			result = renguin_sys_prctl(a1, a2);
			break;
		case LINUX_set_tid_address:
			result = renguin_sys_set_tid_address(a1);
			break;
		case LINUX_uname:
			result = renguin_sys_uname(a1);
			break;
		case LINUX_getrandom:
			result = renguin_sys_getrandom(a1, a2, a3);
			break;
		case LINUX_clock_gettime:
			result = renguin_sys_clock_gettime(a1, a2);
			break;
		case LINUX_clock_getres:
			result = renguin_sys_clock_getres(a1, a2);
			break;
		case LINUX_gettimeofday:
			result = renguin_sys_gettimeofday(a1, a2);
			break;
		case LINUX_time:
			result = renguin_sys_time(a1);
			break;
		case LINUX_nanosleep:
			result = renguin_sys_nanosleep(a1, a2);
			break;
		case LINUX_clock_nanosleep:
			result = renguin_sys_clock_nanosleep(a1, a2, a3, a4);
			break;
		case LINUX_sched_yield:
			result = renguin_sys_sched_yield();
			break;

		// --- Process (process.cpp) ---
		case LINUX_getpid:
			result = renguin_sys_getpid();
			break;
		case LINUX_gettid:
			result = renguin_sys_gettid();
			break;
		case LINUX_getppid:
			result = renguin_sys_getppid();
			break;
		case LINUX_getpgrp:
			result = renguin_sys_getpgid(0);
			break;
		case LINUX_getpgid:
			result = renguin_sys_getpgid((int64)a1);
			break;
		case LINUX_getsid:
			result = renguin_sys_getsid((int64)a1);
			break;
		case LINUX_setpgid:
			result = renguin_sys_setpgid((int64)a1, (int64)a2);
			break;
		case LINUX_setsid:
			result = renguin_sys_setsid();
			break;
		case LINUX_getuid:
			result = renguin_sys_getuid();
			break;
		case LINUX_geteuid:
			result = renguin_sys_geteuid();
			break;
		case LINUX_getgid:
			result = renguin_sys_getgid();
			break;
		case LINUX_getegid:
			result = renguin_sys_getegid();
			break;
		case LINUX_getresuid:
			result = renguin_sys_getresuid(a1, a2, a3);
			break;
		case LINUX_getresgid:
			result = renguin_sys_getresgid(a1, a2, a3);
			break;
		case LINUX_kill:
			result = renguin_sys_kill((int64)a1, (int64)a2);
			break;
		case LINUX_tgkill:
			result = renguin_sys_tgkill((int64)a1, (int64)a2, (int64)a3);
			break;
		case LINUX_tkill:
			result = renguin_sys_tgkill(-1, (int64)a1, (int64)a2);
			break;
		case LINUX_setuid:
			result = renguin_sys_setresuid(a1, (uint32)-1, (uint32)-1, true);
			break;
		case LINUX_setgid:
			result = renguin_sys_setresgid(a1, (uint32)-1, (uint32)-1, true);
			break;
		case LINUX_setreuid:
			result = renguin_sys_setresuid(a1, a2, (uint32)-1, false);
			break;
		case LINUX_setregid:
			result = renguin_sys_setresgid(a1, a2, (uint32)-1, false);
			break;
		case LINUX_setresuid:
			result = renguin_sys_setresuid(a1, a2, a3, false);
			break;
		case LINUX_setresgid:
			result = renguin_sys_setresgid(a1, a2, a3, false);
			break;
		case LINUX_setfsuid:
			result = renguin_sys_setfsuid(a1);
			break;
		case LINUX_setfsgid:
			result = renguin_sys_setfsgid(a1);
			break;
		case LINUX_getgroups:
			result = renguin_sys_getgroups((int64)a1, a2);
			break;
		case LINUX_setgroups:
			result = renguin_sys_setgroups((int64)a1, a2);
			break;
		case LINUX_sysinfo:
			result = renguin_sys_sysinfo(a1);
			break;
		case LINUX_times:
			result = renguin_sys_times(a1);
			break;
		case LINUX_getrusage:
			result = renguin_sys_getrusage((int64)a1, a2);
			break;
		case LINUX_getpriority:
			result = renguin_sys_getpriority((int64)(int32)a1,
				(int64)(int32)a2);
			break;
		case LINUX_setpriority:
			result = renguin_sys_setpriority((int64)(int32)a1,
				(int64)(int32)a2, (int64)(int32)a3);
			break;
		case LINUX_sched_getscheduler:
			result = renguin_sys_sched_getscheduler((int64)a1);
			break;
		case LINUX_sched_setscheduler:
			result = renguin_sys_sched_setscheduler((int64)a1, a2, a3);
			break;
		case LINUX_sched_getparam:
			result = renguin_sys_sched_getparam((int64)a1, a2);
			break;
		case LINUX_sched_setparam:
			result = renguin_sys_sched_setparam((int64)a1, a2);
			break;
		case LINUX_sched_get_priority_max:
			result = renguin_sys_sched_get_priority(a1, true);
			break;
		case LINUX_sched_get_priority_min:
			result = renguin_sys_sched_get_priority(a1, false);
			break;
		case LINUX_sched_rr_get_interval:
			result = renguin_sys_sched_rr_get_interval((int64)a1, a2);
			break;
		case LINUX_personality:
			result = renguin_sys_personality(a1);
			break;
		// Memory locking: Haiku does not page out locked memory on request
		// from userland; accepted as Linux would for small amounts.
		case LINUX_mlock:
		case LINUX_munlock:
		case LINUX_mlock2:
		case LINUX_mlockall:
		case LINUX_munlockall:
			result = 0;
			break;
		case LINUX_prlimit64:
			result = renguin_sys_prlimit64((int64)a1, a2, a3, a4);
			break;
		case LINUX_getrlimit:
			result = renguin_sys_prlimit64(0, a1, 0, a2);
			break;
		case LINUX_setrlimit:
			result = renguin_sys_prlimit64(0, a1, a2, 0);
			break;
		case LINUX_rt_sigaction:
			result = renguin_sys_rt_sigaction(a1, a2, a3, a4);
			break;
		case LINUX_rt_sigprocmask:
			result = renguin_sys_rt_sigprocmask(a1, a2, a3, a4);
			break;
		case LINUX_sigaltstack:
			result = renguin_sys_sigaltstack(a1, a2);
			break;
		case LINUX_rt_sigreturn:
			result = renguin_sys_rt_sigreturn();
			break;
		case LINUX_rt_sigpending:
			result = renguin_sys_rt_sigpending(a1, a2);
			break;
		case LINUX_rt_sigsuspend:
			result = renguin_sys_rt_sigsuspend(a1, a2);
			break;
		case LINUX_pause:
			result = renguin_sys_pause();
			break;
		case LINUX_rt_sigtimedwait:
			result = renguin_sys_rt_sigtimedwait(a1, a2, a3, a4);
			break;
		case LINUX_rt_sigqueueinfo:
			result = renguin_sys_rt_sigqueueinfo((int64)(int32)a1, a2, a3);
			break;
		case LINUX_rt_tgsigqueueinfo:
			result = renguin_sys_rt_tgsigqueueinfo((int64)(int32)a1,
				(int64)(int32)a2, a3, a4);
			break;
		case LINUX_setitimer:
			result = renguin_sys_setitimer(a1, a2, a3);
			break;
		case LINUX_getitimer:
			result = renguin_sys_getitimer(a1, a2);
			break;
		case LINUX_alarm:
			result = renguin_sys_alarm(a1);
			break;

		// --- System V IPC (ipc.cpp) ---
		case LINUX_shmget:
			result = renguin_sys_shmget(a1, a2, a3);
			break;
		case LINUX_shmat:
			result = renguin_sys_shmat(a1, a2, a3);
			break;
		case LINUX_shmdt:
			result = renguin_sys_shmdt(a1);
			break;
		case LINUX_shmctl:
			result = renguin_sys_shmctl(a1, a2, a3);
			break;
		case LINUX_semget:
			result = renguin_sys_semget(a1, a2, a3);
			break;
		case LINUX_semop:
			result = renguin_sys_semtimedop(a1, a2, a3, 0);
			break;
		case LINUX_semtimedop:
			result = renguin_sys_semtimedop(a1, a2, a3, a4);
			break;
		case LINUX_semctl:
			result = renguin_sys_semctl(a1, a2, a3, a4);
			break;
		case LINUX_msgget:
			result = renguin_sys_msgget(a1, a2);
			break;
		case LINUX_msgsnd:
			result = renguin_sys_msgsnd(a1, a2, a3, a4);
			break;
		case LINUX_msgrcv:
			result = renguin_sys_msgrcv(a1, a2, a3, a4, a5);
			break;
		case LINUX_msgctl:
			result = renguin_sys_msgctl(a1, a2, a3);
			break;

		// --- POSIX timers (timers.cpp) ---
		case LINUX_timer_create:
			result = renguin_sys_timer_create(a1, a2, a3);
			break;
		case LINUX_timer_settime:
			result = renguin_sys_timer_settime(a1, a2, a3, a4);
			break;
		case LINUX_timer_gettime:
			result = renguin_sys_timer_gettime(a1, a2);
			break;
		case LINUX_timer_getoverrun:
			result = renguin_sys_timer_getoverrun(a1);
			break;
		case LINUX_timer_delete:
			result = renguin_sys_timer_delete(a1);
			break;

		// --- Process creation (exec.cpp) ---
		case LINUX_clone:
			// x86-64 argument order: flags, newsp, parent_tid, child_tid, tls
			result = renguin_sys_clone(a1, a2, a3, a4, a5);
			break;
		case LINUX_fork:
			result = renguin_sys_fork();
			break;
		case LINUX_vfork:
			result = renguin_sys_vfork();
			break;
		case LINUX_execve:
			result = renguin_sys_execve(a1, a2, a3);
			break;
		case LINUX_wait4:
			result = renguin_sys_wait4((int64)a1, a2, a3, a4);
			break;
		case LINUX_waitid:
			result = renguin_sys_waitid((int)a1, (int64)a2, a3, a4, a5);
			break;
		case LINUX_close_range:
			result = renguin_sys_close_range((uint32)a1, (uint32)a2, a3);
			break;

		// --- Process exit (handled inline) ---
		case LINUX_clone3:
			result = renguin_sys_clone3(a1, a2);
			break;
		case LINUX_exit:
			// Ends the calling thread (threads.cpp). Ending the main thread
			// ends the team with this status.
			renguin_thread_exit((status_t)(a1 & 0xff));
			frame->ax = 0;
			return;
		case LINUX_exit_group:
		{
			// A record of Linux programs that end with an error status
			// (capped), for diagnosing multi-process programs.
			static int32 sExitsLogged = 0;
			if ((a1 & 0xff) != 0 && atomic_add(&sExitsLogged, 1) < 32) {
				dprintf("renguin: team %" B_PRId32 " (%s): exit_group(%"
					B_PRIu64 ")\n", thread->team->id, thread->team->Name(),
					a1 & 0xff);
			}
			_user_exit_team((status_t)(a1 & 0xff));
			frame->ax = 0;
			return;
		}

		// --- Threads and futexes (threads.cpp, futex.cpp) ---
		case LINUX_futex:
			result = renguin_sys_futex(a1, a2, a3, a4, a5, a6);
			break;
		case LINUX_set_robust_list:
			result = renguin_sys_set_robust_list(a1, a2);
			break;
		case LINUX_get_robust_list:
			result = renguin_sys_get_robust_list((int64)(int32)a1, a2, a3);
			break;
		case LINUX_sched_getaffinity:
			result = renguin_sys_sched_getaffinity((int64)(int32)a1, a2, a3);
			break;
		case LINUX_sched_setaffinity:
			result = renguin_sys_sched_setaffinity((int64)(int32)a1, a2, a3);
			break;
		case LINUX_getcpu:
			result = renguin_sys_getcpu(a1, a2, a3);
			break;
		case LINUX_membarrier:
			result = renguin_sys_membarrier((int64)(int32)a1, (uint32)a2,
				(int64)(int32)a3);
			break;

		// No restartable-sequence support; glibc/musl fall back when told so.
		case LINUX_rseq:
			result = -LX_ENOSYS;
			break;

		default:
			log_unimplemented(number);
			result = -LX_ENOSYS;
			break;
	}

	// Calls Linux restarts with ERESTARTSYS when a signal interrupts them
	// before they transfer anything: again after the signal if it had no
	// handler or an SA_RESTART one, EINTR otherwise. Haiku's own versions
	// just fail with EINTR; Haiku's restartable calls (wait4, F_SETLKW)
	// have set the flag already.
	if (result == -LX_EINTR) {
		switch (number) {
			case LINUX_read:
			case LINUX_write:
			case LINUX_readv:
			case LINUX_writev:
			case LINUX_pread64:
			case LINUX_pwrite64:
			case LINUX_ioctl:
			case LINUX_open:
			case LINUX_openat:
			case LINUX_creat:
			case LINUX_flock:
			case LINUX_sendfile:
			case LINUX_sendto:
			case LINUX_recvfrom:
			case LINUX_sendmsg:
			case LINUX_recvmsg:
				renguin_set_restart(RENGUIN_RESTART_SYS, NULL);
				break;
		}
	}

	trace_call(number, a1, a2, a3, result);
	frame->ax = (uint64)result;
}
