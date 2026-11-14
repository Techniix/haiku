/*
 * Copyright 2026, Renguin contributors.
 * Distributed under the terms of the MIT License.
 */
#ifndef _RENGUIN_H
#define _RENGUIN_H


#include <SupportDefs.h>

#include <foreign_personality.h>
#include <lock.h>


// Where the Linux userland lives (the "emul path", FreeBSD's /compat/linux).
// Absolute Linux paths are looked up here first and fall back to Haiku's own
// tree, as FreeBSD's linux_emul_convpath() did. Haiku's "/" is an in-memory
// rootfs, so the tree lives on the boot volume.
#define RENGUIN_LINUX_ROOT	"/boot/system/non-packaged/compat/linux"

// What Linux programs are told about the kernel (uname, /proc).
// FreeBSD reports 5.15 by default (compat.linux.osrelease); glibc refuses
// to start on kernels older than the one it was configured for.
#define RENGUIN_LINUX_RELEASE	"6.1.0-renguin"
#define RENGUIN_LINUX_VERSION	"#1 SMP Renguin on Haiku"
#define RENGUIN_LINUX_HOSTNAME	"haiku"

// Linux ELF constants (ELF64, x86-64).
#define RENGUIN_EI_CLASS		4
#define RENGUIN_EI_DATA			5
#define RENGUIN_EI_OSABI		7
#define RENGUIN_ELFCLASS64		2
#define RENGUIN_ELFDATA2LSB		1
#define RENGUIN_ELFOSABI_SYSV	0
#define RENGUIN_ELFOSABI_LINUX	3
#define RENGUIN_ELFOSABI_GNU	3
#define RENGUIN_ET_EXEC			2
#define RENGUIN_ET_DYN			3
#define RENGUIN_EM_X86_64		62

#define RENGUIN_PT_LOAD			1
#define RENGUIN_PT_DYNAMIC		2
#define RENGUIN_PT_INTERP		3
#define RENGUIN_PT_NOTE			4
#define RENGUIN_PT_PHDR			6
#define RENGUIN_PT_TLS			7
#define RENGUIN_PT_GNU_STACK	0x6474e551

#define RENGUIN_PF_X			0x1
#define RENGUIN_PF_W			0x2
#define RENGUIN_PF_R			0x4

#define RENGUIN_NT_GNU_ABI_TAG	1
#define RENGUIN_GNU_ABI_LINUX	0

// Linux auxiliary vector entry types (a_type).
#define RENGUIN_AT_NULL			0
#define RENGUIN_AT_IGNORE		1
#define RENGUIN_AT_PHDR			3
#define RENGUIN_AT_PHENT		4
#define RENGUIN_AT_PHNUM		5
#define RENGUIN_AT_PAGESZ		6
#define RENGUIN_AT_BASE			7
#define RENGUIN_AT_FLAGS		8
#define RENGUIN_AT_ENTRY		9
#define RENGUIN_AT_UID			11
#define RENGUIN_AT_EUID			12
#define RENGUIN_AT_GID			13
#define RENGUIN_AT_EGID			14
#define RENGUIN_AT_PLATFORM		15
#define RENGUIN_AT_HWCAP		16
#define RENGUIN_AT_CLKTCK		17
#define RENGUIN_AT_SECURE		23
#define RENGUIN_AT_RANDOM		25
#define RENGUIN_AT_HWCAP2		26
#define RENGUIN_AT_EXECFN		31

#define RENGUIN_AT_RANDOM_LEN	16

// Linux errno values that handlers return directly. The full native->Linux
// table is in abi/linux_errno.h / abi/linux_errno_table.h.
#define RENGUIN_ENOSYS			38


typedef struct {
	uint8	e_ident[16];
	uint16	e_type;
	uint16	e_machine;
	uint32	e_version;
	uint64	e_entry;
	uint64	e_phoff;
	uint64	e_shoff;
	uint32	e_flags;
	uint16	e_ehsize;
	uint16	e_phentsize;
	uint16	e_phnum;
	uint16	e_shentsize;
	uint16	e_shnum;
	uint16	e_shstrndx;
} renguin_ehdr;

typedef struct {
	uint32	p_type;
	uint32	p_flags;
	uint64	p_offset;
	uint64	p_vaddr;
	uint64	p_paddr;
	uint64	p_filesz;
	uint64	p_memsz;
	uint64	p_align;
} renguin_phdr;


#define RENGUIN_BRK_AREA_LOOKUP	(-2)
	// the heap area exists but its ID is not known yet (fork()ed child)


// A Linux signal action as given to rt_sigaction() (the kernel's struct
// sigaction on x86-64).
struct renguin_sigaction {
	uint64	handler;
	uint64	flags;
	uint64	restorer;
	uint64	mask;
};

#define RENGUIN_NSIG	64		// Linux _NSIG


// Per-team data owned by the personality, hung off Team::personality_data.
struct renguin_team {
	addr_t	executable_base;	// load bias of the main executable
	addr_t	interpreter_base;	// load bias of the ELF interpreter, or 0
	addr_t	entry;				// executable entry point (biased)
	addr_t	interpreter_entry;	// interpreter entry point (biased), or 0
	addr_t	phdr;				// address of the executable's program headers
	uint16	phentsize;
	uint16	phnum;
	addr_t	brk_start;			// base of the program break (heap)
	addr_t	brk_current;		// current program break (exact, unaligned)
	addr_t	brk_mapped_end;		// end of the pages backing the break
	area_id	brk_area;			// anonymous area backing the heap, or -1;
								// RENGUIN_BRK_AREA_LOOKUP after fork()
	addr_t	fork_clear_child_tid;
		// clone(CLONE_CHILD_CLEARTID) in a fork()ed child: handed to its main
		// thread's renguin_thread when it first runs
	addr_t	fork_set_child_tid;	// clone(CLONE_CHILD_SETTID) in a fork()ed
								// child: written when it first runs
	uint64	fork_unmapped_blocked;
		// the forking thread's blocked signals that have no Haiku number,
		// for the child's main thread
	uint64	fork_gs_base;		// parent's GS base, for a fork()ed child
								// (Haiku's fork copies only the FS base)
	image_id app_image;			// registered B_APP_IMAGE, or -1
	bool	have_linux_root;	// RENGUIN_LINUX_ROOT existed at exec time
	uint32	umask;				// file creation mask. Haiku's kernel does not
								// apply one (libroot does, in userland), so
								// Renguin applies it for Linux programs
	char	exe_path[B_PATH_NAME_LENGTH];
		// host path of the executable (for /proc/self/exe)
	addr_t	arg_start, arg_end;	// argument strings on the initial stack,
	addr_t	env_start, env_end;	// then the environment (/proc/<pid>/cmdline
								// and environ, as Linux's mm->arg_start...)

	// Directory FDs standing for /proc directories (procfs.cpp). The FD is
	// open on an empty placeholder directory; getdents64 on it lists the
	// /proc directory instead.
	struct {
		int		fd;				// -1: unused
		ino_t	node;			// the placeholder FD's node, to recognize it
		dev_t	device;
		uint32	position;		// getdents64 position (entry index)
		char	path[48];		// "/proc", "/proc/123/fd", ...
	} proc_dirs[16];

	mutex	lock;				// guards threads, the program break (brk_*)
								// and sigactions
	struct renguin_thread* threads;
		// per-thread data of this team's threads (threads.cpp); freed with
		// the team, whichever way its threads ended

	struct renguin_sigaction sigactions[RENGUIN_NSIG + 1];
		// what each rt_sigaction() asked for, by Linux signal number. Haiku
		// holds the actions themselves (for signals it has a number for);
		// this adds SA_RESTORER and the Linux flags and mask.
};


struct renguin_fork_request;

// Per-thread data owned by the personality, hung off Thread::personality_data
// and listed in renguin_team::threads. Created on demand; only the owning
// thread changes it once that thread runs.
struct renguin_thread {
	struct renguin_thread* next;	// in renguin_team::threads
	thread_id	id;
	addr_t		clear_child_tid;	// set_tid_address()/CLONE_CHILD_CLEARTID:
									// zeroed and futex-woken on thread exit
	addr_t		robust_list;		// set_robust_list() head, or 0
	struct renguin_fork_request* fork_request;
		// what clone() asked of _user_fork(), for renguin_team_forked()
	bool		starting;			// created by clone(CLONE_THREAD), has not
									// entered userland yet
	bool		altstack_autodisarm;	// sigaltstack(SS_AUTODISARM)
	uint8		restart_policy;		// RENGUIN_RESTART_*: how the current system
									// call may be restarted after a signal
	bool		forced_sigsegv;		// force_sigsegv() sent the thread to
	addr_t		forced_sigsegv_ip;	// address 0; where it really was
	uint64		unmapped_blocked;	// blocked Linux signals that have no Haiku
									// number (Haiku keeps the rest)
};


#ifdef __cplusplus
extern "C" {
#endif

// errno.cpp
int		renguin_errno_from_haiku(status_t status);

// Encode a Haiku result for return in rax: a byte count or value stays,
// a negative Haiku status_t becomes a negative Linux errno.
static inline int64
renguin_encode(ssize_t result)
{
	if (result < 0)
		return -(int64)renguin_errno_from_haiku((status_t)result);
	return result;
}

// loader.cpp
status_t	renguin_probe(const struct foreign_exec_args* args);
status_t	renguin_load(struct foreign_exec_args* args);
status_t	renguin_enter_userspace(Thread* thread, addr_t entry,
				addr_t stackPointer);
void		renguin_team_released(Team* team);
bool		renguin_is_linux_team(Team* team);

// exec.cpp
status_t	renguin_team_forked(Team* parent, Team* child);
int64		renguin_sys_clone(uint64 flags, uint64 newStack, uint64 parentTid,
				uint64 childTid, uint64 tls);
int64		renguin_sys_fork();
int64		renguin_sys_vfork();
int64		renguin_sys_wait4(int64 pid, uint64 status, uint64 options,
				uint64 rusage);
int64		renguin_sys_execve(uint64 path, uint64 argv, uint64 envp);

// threads.cpp
status_t	renguin_team_threads_init(struct renguin_team* data);
void		renguin_team_threads_free(struct renguin_team* data);
void		renguin_thread_entering_userspace(Thread* thread, bool forked);
struct renguin_thread* renguin_current_thread_data(bool create);
	// the calling thread's data; with create, allocated if missing (NULL if
	// out of memory)
bool		renguin_is_thread_clone(uint64 flags);
	// clone() flags asking for a thread (shared memory) rather than a process
int64		renguin_clone_thread(uint64 flags, uint64 newStack,
				uint64 parentTid, uint64 childTid, uint64 tls);
void		renguin_thread_exit(status_t status);
	// the calling thread ends on its way back to userland
int64		renguin_sys_set_tid_address(uint64 tidptr);
int64		renguin_sys_set_robust_list(uint64 head, uint64 length);
int64		renguin_sys_get_robust_list(int64 tid, uint64 head, uint64 length);
int64		renguin_sys_clone3(uint64 args, uint64 size);
int64		renguin_sys_sched_getaffinity(int64 pid, uint64 size, uint64 mask);
int64		renguin_sys_sched_setaffinity(int64 pid, uint64 size, uint64 mask);
int64		renguin_sys_getcpu(uint64 cpu, uint64 node, uint64 cache);

// futex.cpp
status_t	renguin_futex_init();
void		renguin_futex_uninit();
int64		renguin_sys_futex(uint64 address, uint64 op, uint64 value,
				uint64 timeout, uint64 address2, uint64 value3);
int32		renguin_futex_wake(team_id team, addr_t address, int32 count);
	// wakes up to count waiters on a futex word of the given team

// signal.cpp
enum {
	RENGUIN_RESTART_SYS = 0,
		// Linux's ERESTARTSYS: restart if no handler runs, or the handler has
		// SA_RESTART (Haiku's own rule)
	RENGUIN_RESTART_NOHAND
		// ERESTARTNOHAND/ERESTART_RESTARTBLOCK: restart only if no handler
		// runs; a handler means EINTR (nanosleep, sigtimedwait)
};
void		renguin_set_restart(int policy, const bigtime_t* deadline);
void		renguin_cancel_restart();
	// a call returning partial progress after an interruption
	// the current call was interrupted and may be restarted; a deadline is
	// kept for the restarted call (renguin_restarted_deadline())
bool		renguin_restarted_deadline(bigtime_t* _deadline);
	// true, with the saved deadline, if this call is a restart
status_t	renguin_setup_signal_frame(Thread* thread, struct sigaction* action,
				struct signal_frame_data* data);
void		renguin_reset_fpu_state(Thread* thread);
sigset_t	renguin_push_signal_mask(uint64 linuxMask);
void		renguin_pop_signal_mask(sigset_t oldMask, bool interrupted);
	// a temporary signal mask for the wait of ppoll/pselect6
	// the initial FPU state, loaded on the way to userland
int64		renguin_sys_rt_sigaction(uint64 signal, uint64 action,
				uint64 oldAction, uint64 sigsetSize);
int64		renguin_sys_rt_sigprocmask(uint64 how, uint64 set, uint64 oldSet,
				uint64 sigsetSize);
int64		renguin_sys_rt_sigreturn();
int64		renguin_sys_sigaltstack(uint64 stack, uint64 oldStack);
int64		renguin_sys_rt_sigpending(uint64 set, uint64 sigsetSize);
int64		renguin_sys_rt_sigsuspend(uint64 mask, uint64 sigsetSize);
int64		renguin_sys_pause();
int64		renguin_sys_rt_sigtimedwait(uint64 set, uint64 info, uint64 timeout,
				uint64 sigsetSize);
int64		renguin_sys_rt_sigqueueinfo(int64 pid, uint64 signal, uint64 info);
int64		renguin_sys_rt_tgsigqueueinfo(int64 tgid, int64 tid, uint64 signal,
				uint64 info);
int64		renguin_sys_setitimer(uint64 which, uint64 newValue,
				uint64 oldValue);
int64		renguin_sys_getitimer(uint64 which, uint64 value);
int64		renguin_sys_alarm(uint64 seconds);

// syscall.cpp
void		renguin_syscall(struct iframe* frame);
struct renguin_team* renguin_current_team();

// usermem.cpp: checked copies to and from the Linux program's memory. Unlike
// a bare user_memcpy(), these refuse kernel addresses. They return B_OK or
// B_BAD_ADDRESS.
status_t	renguin_copy_in(void* to, uint64 userFrom, size_t size);
status_t	renguin_copy_out(uint64 userTo, const void* from, size_t size);
ssize_t		renguin_copy_string_in(char* to, uint64 userFrom, size_t size);
	// returns the length, B_NAME_TOO_LONG, or B_BAD_ADDRESS
status_t	renguin_user_atomic_cmpxchg(uint64 address, uint32 expected,
				uint32 newValue, uint32* _oldValue);
	// atomic compare-and-swap of an aligned user word; *_oldValue is what
	// was there (the swap happened if it equals expected)

// usermem.cpp: scratch space in the calling thread's user stack, below the
// System V red zone, for passing data to Haiku's _user_* calls (which only
// accept user addresses). Valid until the syscall returns.
struct renguin_scratch {
	addr_t	next;
	addr_t	limit;		// lowest usable address; 0: not looked up yet
};
void		renguin_scratch_init(struct renguin_scratch* scratch);
uint64		renguin_scratch_alloc(struct renguin_scratch* scratch, size_t size);
uint64		renguin_scratch_put(struct renguin_scratch* scratch,
				const void* data, size_t size);
	// returns 0 if the user stack is not writable

// path.cpp: Linux root ("emul path") handling.
#define RENGUIN_STATUS_IN_PROC	(B_GENERAL_ERROR_BASE + 0x5250)
	// a path that leads into /proc (through the Linux root's own /proc, or a
	// link into it such as /etc/mtab): the path returned is the Linux
	// /proc path, for procfs.cpp. renguin_encode() makes it EACCES for the
	// calls that don't take /proc paths.
#define RENGUIN_PATH_FOLLOW_LEAF	0x1		// resolve a final symlink
#define RENGUIN_PATH_MAY_CREATE		0x2		// final component may not exist
status_t	renguin_resolve_in_root(const char* linuxPath, uint32 flags,
				char* hostPath, size_t hostPathSize);
	// B_OK if the path exists under the Linux root; hostPath is then the
	// fully resolved host path. B_ENTRY_NOT_FOUND otherwise.
status_t	renguin_get_user_path(uint64 userPath, uint32 flags, char* path,
				size_t pathSize);
	// copies a path from userland and translates absolute paths into the
	// Linux root where they exist there
uint64		renguin_user_path(struct renguin_scratch* scratch, uint64 userPath,
				uint32 flags, status_t* _status);
	// like renguin_get_user_path(), but returns a user-space copy of the
	// translated path for Haiku's _user_* calls (or userPath unchanged)
void		renguin_host_to_linux_path(char* path);
	// strips the Linux root prefix from a host path, in place

// mem.cpp
int64		renguin_sys_brk(uint64 addr);
int64		renguin_sys_mmap(uint64 addr, uint64 length, uint64 prot,
				uint64 flags, int64 fd, int64 offset);
int64		renguin_sys_munmap(uint64 addr, uint64 length);
int64		renguin_sys_mprotect(uint64 addr, uint64 length, uint64 prot);
int64		renguin_sys_mremap(uint64 oldAddr, uint64 oldSize, uint64 newSize,
				uint64 flags, uint64 newAddr);
int64		renguin_sys_msync(uint64 addr, uint64 length, uint64 flags);
int64		renguin_sys_madvise(uint64 addr, uint64 length, uint64 advice);

// file.cpp
uint32		renguin_current_umask();
int64		renguin_sys_read(int fd, uint64 buffer, uint64 count);
int64		renguin_sys_write(int fd, uint64 buffer, uint64 count);
int64		renguin_sys_pread64(int fd, uint64 buffer, uint64 count,
				int64 offset);
int64		renguin_sys_pwrite64(int fd, uint64 buffer, uint64 count,
				int64 offset);
int64		renguin_sys_writev(int fd, uint64 iov, uint64 count);
int64		renguin_sys_readv(int fd, uint64 iov, uint64 count);
int64		renguin_sys_openat(int dirfd, uint64 path, uint64 flags, uint64 mode);
int64		renguin_sys_close(int fd);
int64		renguin_sys_lseek(int fd, int64 offset, int whence);
int64		renguin_sys_fstat(int fd, uint64 statbuf);
int64		renguin_sys_newfstatat(int dirfd, uint64 path, uint64 statbuf,
				uint64 flags);
int64		renguin_sys_readlinkat(int dirfd, uint64 path, uint64 buffer,
				uint64 size);
int64		renguin_sys_faccessat(int dirfd, uint64 path, uint64 mode,
				uint64 flags);
int64		renguin_sys_getdents64(int fd, uint64 buffer, uint64 count);
int64		renguin_sys_dup(int fd);
int64		renguin_sys_dup3(int oldfd, int newfd, uint64 flags, bool isDup2);
int64		renguin_sys_pipe2(uint64 fds, uint64 flags);
int64		renguin_sys_fcntl(int fd, uint64 cmd, uint64 arg);
int64		renguin_sys_getcwd(uint64 buffer, uint64 size);
int64		renguin_sys_chdir(uint64 path);
int64		renguin_sys_fchdir(int fd);
int64		renguin_sys_mkdirat(int dirfd, uint64 path, uint64 mode);
int64		renguin_sys_unlinkat(int dirfd, uint64 path, uint64 flags);
int64		renguin_sys_renameat(int olddirfd, uint64 oldpath, int newdirfd,
				uint64 newpath);
int64		renguin_sys_fsync(int fd);
int64		renguin_sys_ftruncate(int fd, int64 length);

int64		renguin_stat_at(int dirfd, uint64 path, uint64 flags,
				struct stat* st);
	// stat() for newfstatat and statx; 0 or a negative Linux errno

// procfs.cpp: the Linux /proc. Paths are checked raw, before translation.
#define RENGUIN_PROC_PATH_MAX	96
void		renguin_procfs_init();
void		renguin_proc_init_team(struct renguin_team* data);
bool		renguin_proc_path(int dirfd, const char* rawPath, char* procPath,
				size_t procPathSize);
	// true if rawPath (relative to dirfd) names something under /proc;
	// procPath is then the normalized absolute path
int64		renguin_proc_stat(const char* procPath, bool followLeaf,
				struct stat* st);
int64		renguin_proc_readlink(const char* procPath, char* target,
				size_t size);
	// the target's length, or a negative Linux errno
int64		renguin_proc_open(const char* procPath, uint64 linuxFlags);
int64		renguin_proc_access(const char* procPath, int mode);
bool		renguin_proc_getdents(int fd, uint64 buffer, uint64 count,
				int64* _result);
bool		renguin_proc_lseek(int fd, int64 offset, int whence,
				int64* _result);
	// false: fd is not a /proc directory
void		renguin_proc_closed(int fd);
bool		renguin_proc_fstat(int fd, struct stat* st);
void		renguin_proc_dup(int oldfd, int newfd);
	// keeps the /proc directory table in step with dup2/dup3/F_DUPFD
bool		renguin_proc_is_dir_fd(int fd);
bool		renguin_proc_dirfd_refused(int dirfd);
	// (file.cpp) true if dirfd is a /proc directory: the *at() calls that
	// create or change things refuse it with EACCES
int32		renguin_proc_super_magic();
int64		renguin_user_proc_path(int dirfd, uint64 userPath, char* procPath,
				size_t procPathSize);
	// 1 if the user path is a /proc path, 0 if not, or a negative errno

// fsops.cpp
int64		renguin_sys_statx(int dirfd, uint64 path, uint64 flags, uint64 mask,
				uint64 buffer);
int64		renguin_sys_statfs(uint64 path, uint64 buffer);
int64		renguin_sys_fstatfs(int fd, uint64 buffer);
int64		renguin_sys_fchmodat(int dirfd, uint64 path, uint64 mode,
				uint64 flags);
int64		renguin_sys_fchmod(int fd, uint64 mode);
int64		renguin_sys_fchownat(int dirfd, uint64 path, uint64 owner,
				uint64 group, uint64 flags);
int64		renguin_sys_fchown(int fd, uint64 owner, uint64 group);
int64		renguin_sys_utimensat(int dirfd, uint64 path, uint64 times,
				uint64 flags);
int64		renguin_sys_futimesat(int dirfd, uint64 path, uint64 times);
int64		renguin_sys_utime(uint64 path, uint64 times);
int64		renguin_sys_linkat(int oldDirFD, uint64 oldPath, int newDirFD,
				uint64 newPath, uint64 flags);
int64		renguin_sys_symlinkat(uint64 target, int dirfd, uint64 path);
int64		renguin_sys_mknodat(int dirfd, uint64 path, uint64 mode,
				uint64 device);
int64		renguin_sys_truncate(uint64 path, int64 length);
int64		renguin_sys_fadvise64(int fd, int64 offset, int64 length,
				uint64 advice);
int64		renguin_sys_readahead(int fd, int64 offset, uint64 count);
int64		renguin_sys_fallocate(int fd, uint64 mode, int64 offset,
				int64 length);
int64		renguin_sys_sync();
int64		renguin_sys_syncfs(int fd);
int64		renguin_sys_flock(int fd, uint64 operation);
int64		renguin_sys_sendfile(int outFD, int inFD, uint64 offset,
				uint64 count);

// sysinfo.cpp
int64		renguin_sys_setresuid(uint64 ruid, uint64 euid, uint64 suid,
				bool setAll);
int64		renguin_sys_setresgid(uint64 rgid, uint64 egid, uint64 sgid,
				bool setAll);
int64		renguin_sys_setfsuid(uint64 uid);
int64		renguin_sys_setfsgid(uint64 gid);
int64		renguin_sys_getgroups(int64 count, uint64 list);
int64		renguin_sys_setgroups(int64 count, uint64 list);
int64		renguin_sys_sysinfo(uint64 buffer);
int64		renguin_sys_times(uint64 buffer);
int64		renguin_sys_getrusage(int64 who, uint64 buffer);
int64		renguin_sys_getpriority(int64 which, int64 who);
int64		renguin_sys_setpriority(int64 which, int64 who, int64 nice);
int64		renguin_sys_sched_getscheduler(int64 pid);
int64		renguin_sys_sched_setscheduler(int64 pid, uint64 policy,
				uint64 param);
int64		renguin_sys_sched_getparam(int64 pid, uint64 param);
int64		renguin_sys_sched_setparam(int64 pid, uint64 param);
int64		renguin_sys_sched_get_priority(uint64 policy, bool maximum);
int64		renguin_sys_sched_rr_get_interval(int64 pid, uint64 interval);
int64		renguin_sys_personality(uint64 persona);

// poll.cpp
int64		renguin_sys_poll(uint64 fds, uint64 count, int64 timeoutMillis);
int64		renguin_sys_ppoll(uint64 fds, uint64 count, uint64 timeout,
				uint64 mask, uint64 maskSize);
int64		renguin_sys_select(int64 count, uint64 readSet, uint64 writeSet,
				uint64 errorSet, uint64 timeout);
int64		renguin_sys_pselect6(int64 count, uint64 readSet, uint64 writeSet,
				uint64 errorSet, uint64 timeout, uint64 maskData);

// tty.cpp
int64		renguin_sys_ioctl(int fd, uint64 cmd, uint64 arg);

// misc.cpp
int64		renguin_sys_uname(uint64 buffer);
int64		renguin_sys_getrandom(uint64 buffer, uint64 length, uint64 flags);
int64		renguin_sys_clock_gettime(uint64 clockId, uint64 timespec);
int64		renguin_sys_clock_getres(uint64 clockId, uint64 timespec);
int64		renguin_sys_gettimeofday(uint64 timeval, uint64 timezone);
int64		renguin_sys_time(uint64 tloc);
int64		renguin_sys_nanosleep(uint64 request, uint64 remain);
int64		renguin_sys_clock_nanosleep(uint64 clockId, uint64 flags,
				uint64 request, uint64 remain);
int64		renguin_sys_arch_prctl(uint64 code, uint64 addr);
int64		renguin_sys_prctl(uint64 option, uint64 arg2);
int64		renguin_sys_sched_yield();

// process.cpp
int64		renguin_sys_getpid();
int64		renguin_sys_gettid();
int64		renguin_sys_getppid();
int64		renguin_sys_getpgid(int64 pid);
int64		renguin_sys_getsid(int64 pid);
int64		renguin_sys_setpgid(int64 pid, int64 pgid);
int64		renguin_sys_setsid();
int64		renguin_sys_getuid();
int64		renguin_sys_geteuid();
int64		renguin_sys_getgid();
int64		renguin_sys_getegid();
int64		renguin_sys_getresuid(uint64 ruid, uint64 euid, uint64 suid);
int64		renguin_sys_getresgid(uint64 rgid, uint64 egid, uint64 sgid);
int64		renguin_sys_kill(int64 pid, int64 signal);
int64		renguin_sys_tgkill(int64 tgid, int64 tid, int64 signal);
int64		renguin_sys_prlimit64(int64 pid, uint64 resource, uint64 newLimit,
				uint64 oldLimit);
int64		renguin_sys_umask(uint64 mask);
int			renguin_signal_to_haiku(int linuxSignal);
int			renguin_signal_from_haiku(int haikuSignal);

#ifdef __cplusplus
}
#endif


#endif	// _RENGUIN_H
