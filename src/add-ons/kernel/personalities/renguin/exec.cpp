/*
 * Copyright 2026, Renguin contributors.
 * Distributed under the terms of the MIT License.
 *
 * Linux process creation: clone (process-style), fork, vfork, execve, wait4.
 *
 * FreeBSD counterparts: sys/compat/linux/linux_fork.c (linux_fork(),
 * linux_vfork(), linux_clone_proc()), linux_misc.c (linux_wait4(),
 * linux_execve() via kern_execve()) and the wait-status conversion in
 * linux_misc.c (linux_to_bsd_waitopts()/bsd_to_linux_wstat()).
 *
 * fork: Haiku's _user_fork() copies the address space and the calling
 * thread's syscall frame; the child resumes in userland right after the
 * syscall with rax = 0. The kernel patch's fork hook gives the child the
 * Linux personality (renguin_team_forked() below) and keeps Haiku from
 * touching the child's thread pointer and signal deferral.
 *
 * Threads (CLONE_THREAD and friends) are created in threads.cpp; clone()
 * hands thread-style requests over to it.
 *
 * execve: Haiku has no kernel-side exec that takes kernel buffers
 * (exec_team() is static in team.cpp), so the arguments are written, in
 * libroot's flat format, to a temporary area in the program's own address
 * space and passed to _user_exec(). Shebang scripts are handled here, as the
 * Linux kernel does in binfmt_script; on Haiku that is libroot's job, and
 * Linux programs don't use libroot.
 */


#include "renguin.h"
#include "linux_defs.h"

#include <string.h>

#include <KernelExport.h>
#include <OS.h>

#include <AutoDeleter.h>

#include <arch/x86/arch_thread.h>
#include <arch/x86/64/iframe.h>
#include <kernel.h>
#include <syscalls.h>
#include <team.h>
#include <thread.h>
#include <user_runtime.h>
#include <util/AutoLock.h>
#include <vfs.h>
#include <vm/vm.h>

#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>


// clone() flags (include/uapi/linux/sched.h).
#define LX_CSIGNAL				0x000000ff
#define LX_CLONE_VM				0x00000100
#define LX_CLONE_FS				0x00000200
#define LX_CLONE_FILES			0x00000400
#define LX_CLONE_SIGHAND		0x00000800
#define LX_CLONE_PIDFD			0x00001000
#define LX_CLONE_PTRACE			0x00002000
#define LX_CLONE_VFORK			0x00004000
#define LX_CLONE_PARENT			0x00008000
#define LX_CLONE_THREAD			0x00010000
#define LX_CLONE_SYSVSEM		0x00040000
#define LX_CLONE_SETTLS			0x00080000
#define LX_CLONE_PARENT_SETTID	0x00100000
#define LX_CLONE_CHILD_CLEARTID	0x00200000
#define LX_CLONE_DETACHED		0x00400000
#define LX_CLONE_UNTRACED		0x00800000
#define LX_CLONE_CHILD_SETTID	0x01000000

// wait4() options.
#define LX_WNOHANG		0x00000001
#define LX_WUNTRACED	0x00000002
#define LX_WCONTINUED	0x00000008
#define LX_WNOTHREAD	0x20000000
#define LX_WALL			0x40000000
#define LX_WCLONE		0x80000000

#define LX_ENOEXEC		8
#define LX_ECHILD		10

static const int kMaxInterpreterDepth = 4;		// as Linux (binfmt_script)
static const size_t kShebangMax = 256;			// BINPRM_BUF_SIZE
static const uint32 kMaxArgs = 128 * 1024;


/*!	What the parent asked clone() for, handed to renguin_team_forked(),
	which runs in the parent's thread during _user_fork(). Passed through the
	calling thread's renguin_thread for the duration of the call.
*/
struct renguin_fork_request {
	uint64	flags;
	uint64	child_tid;
};


// #pragma mark - fork


/*!	Kernel fork hook: called in the parent during fork_team(), after the
	address space has been copied. The child gets its own copy of the team
	data.
*/
status_t
renguin_team_forked(Team* parent, Team* child)
{
	renguin_team* parentData = (renguin_team*)parent->personality_data;
	if (parentData == NULL)
		return B_OK;

	renguin_team* data = (renguin_team*)malloc(sizeof(renguin_team));
	if (data == NULL)
		return B_NO_MEMORY;
	{
		// Another thread may be moving the break.
		MutexLocker locker(parentData->lock);
		memcpy(data, parentData, sizeof(*data));
	}

	// Area and image IDs differ in the child. The heap area is at the same
	// address; renguin_sys_brk() looks it up when it first needs it.
	data->brk_area = data->brk_mapped_end > data->brk_start
		? RENGUIN_BRK_AREA_LOOKUP : -1;
	data->app_image = -1;
	data->fork_clear_child_tid = 0;
	data->fork_set_child_tid = 0;

	// Only the calling thread is copied, as on Linux; the child starts with
	// an empty thread list, and its main thread gets its data when it first
	// runs (renguin_thread_entering_userspace()).
	renguin_team_threads_init(data);

	renguin_thread* threadData = (renguin_thread*)
		thread_get_current_thread()->personality_data;
	renguin_fork_request* request
		= threadData != NULL ? threadData->fork_request : NULL;
	data->fork_unmapped_blocked
		= threadData != NULL ? threadData->unmapped_blocked : 0;
	if (request != NULL) {
		if ((request->flags & LX_CLONE_CHILD_SETTID) != 0)
			data->fork_set_child_tid = request->child_tid;
		if ((request->flags & LX_CLONE_CHILD_CLEARTID) != 0)
			data->fork_clear_child_tid = request->child_tid;
	}

	data->fork_gs_base = thread_get_current_thread()->arch_info.user_gs_base;

	child->personality_data = data;
	return B_OK;
}


int64
renguin_sys_clone(uint64 flags, uint64 newStack, uint64 parentTid,
	uint64 childTid, uint64 tls)
{
	if (renguin_is_thread_clone(flags))
		return renguin_clone_thread(flags, newStack, parentTid, childTid, tls);

	(void)tls;
	uint64 cloneFlags = flags & ~(uint64)LX_CSIGNAL;

	// Process-style clone only. CLONE_VM|CLONE_VFORK (glibc's and musl's
	// posix_spawn(), behind system() and popen()) is run as a fork on the
	// given stack: the child gets a copy of the memory instead of sharing it.
	// That is enough for spawn helpers, which only exec or _exit, and report
	// errors through a pipe (musl) or exit status 127 (glibc).
	bool vforkSpawn = (cloneFlags & (LX_CLONE_VM | LX_CLONE_VFORK))
		== (LX_CLONE_VM | LX_CLONE_VFORK);
	uint64 supported = LX_CLONE_VFORK | LX_CLONE_PARENT_SETTID
		| LX_CLONE_CHILD_SETTID | LX_CLONE_CHILD_CLEARTID | LX_CLONE_DETACHED
		| LX_CLONE_UNTRACED | LX_CLONE_PTRACE;
	if (vforkSpawn)
		supported |= LX_CLONE_VM;
	if ((cloneFlags & ~supported) != 0) {
		dprintf("renguin: team %" B_PRId32 ": clone flags %#" B_PRIx64
			" not supported\n", thread_get_current_thread()->team->id,
			cloneFlags);
		return -LX_ENOSYS;
	}
	if (newStack != 0 && !IS_USER_ADDRESS((addr_t)newStack))
		return -LX_EINVAL;

	iframe* frame = x86_get_user_iframe();
	if (frame == NULL)
		return -LX_EINVAL;

	// The child resumes from a copy of this syscall frame
	// (arch_store_fork_frame()). Give it the new stack, if any, and tell the
	// fork hook what was asked for.
	renguin_fork_request request = { cloneFlags, childTid };
	renguin_thread* threadData = NULL;
	if ((cloneFlags & (LX_CLONE_CHILD_SETTID | LX_CLONE_CHILD_CLEARTID)) != 0) {
		threadData = renguin_current_thread_data(true);
		if (threadData == NULL)
			return -LX_ENOMEM;
		threadData->fork_request = &request;
	}
	uint64 savedStack = frame->user_sp;
	if (newStack != 0)
		frame->user_sp = newStack;

	// The foreign syscall entry points the frame at this thread's FPU save
	// area; the child must not inherit that pointer (a signal handled before
	// it first reaches userland would read another team's thread). Without
	// one, Haiku uses the initial FPU state, as for a native fork().
	void* savedFPU = frame->fpu;
	frame->fpu = NULL;

	thread_id child = _user_fork();

	frame->fpu = savedFPU;
	frame->user_sp = savedStack;
	if (threadData != NULL)
		threadData->fork_request = NULL;

	if (child < 0)
		return renguin_encode(child);

	if ((cloneFlags & LX_CLONE_PARENT_SETTID) != 0) {
		int32 tid = child;
		renguin_copy_out(parentTid, &tid, sizeof(tid));
	}
	return child;
}


int64
renguin_sys_fork()
{
	return renguin_sys_clone(LX_SIGCHLD, 0, 0, 0, 0);
}


int64
renguin_sys_vfork()
{
	// A vfork() may always behave like fork().
	return renguin_sys_clone(LX_CLONE_VFORK | LX_SIGCHLD, 0, 0, 0, 0);
}


// #pragma mark - wait


int64
renguin_sys_wait4(int64 pid, uint64 statusPtr, uint64 options,
	uint64 rusagePtr)
{
	if ((options & ~(uint64)(LX_WNOHANG | LX_WUNTRACED | LX_WCONTINUED
			| LX_WNOTHREAD | LX_WALL | LX_WCLONE)) != 0) {
		return -LX_EINVAL;
	}

	uint32 flags = WEXITED;
	if ((options & LX_WNOHANG) != 0)
		flags |= WNOHANG;
	if ((options & LX_WUNTRACED) != 0)
		flags |= WUNTRACED;
	if ((options & LX_WCONTINUED) != 0)
		flags |= WCONTINUED;

	renguin_scratch scratch;
	renguin_scratch_init(&scratch);
	uint64 userInfo = renguin_scratch_alloc(&scratch, sizeof(siginfo_t));
	uint64 userUsage = renguin_scratch_alloc(&scratch,
		sizeof(team_usage_info));
	if (userInfo == 0 || userUsage == 0)
		return -LX_EFAULT;

	// pid follows waitpid() conventions on both systems: > 0 one child, -1
	// any child, 0 our process group, < -1 that process group.
	pid_t child = _user_wait_for_child((thread_id)pid, flags,
		(siginfo_t*)(addr_t)userInfo, (team_usage_info*)(addr_t)userUsage);
	if (child == B_WOULD_BLOCK && (options & LX_WNOHANG) != 0)
		return 0;
	if (child < 0)
		return renguin_encode(child);

	siginfo_t info;
	team_usage_info usage;
	if (user_memcpy(&info, (void*)(addr_t)userInfo, sizeof(info)) != B_OK
		|| user_memcpy(&usage, (void*)(addr_t)userUsage, sizeof(usage))
			!= B_OK) {
		return -LX_EFAULT;
	}

	// Linux wait status encoding (see FreeBSD's bsd_to_linux_wstat()).
	int32 status;
	int linuxSignal = renguin_signal_from_haiku(info.si_status);
	if (linuxSignal == 0)
		linuxSignal = LX_SIGKILL;	// a Haiku-only signal, e.g. SIGKILLTHR
	switch (info.si_code) {
		case CLD_EXITED:
			status = (info.si_status & 0xff) << 8;
			break;
		case CLD_KILLED:
			status = linuxSignal & 0x7f;
			break;
		case CLD_DUMPED:
			status = (linuxSignal & 0x7f) | 0x80;
			break;
		case CLD_STOPPED:
			status = ((linuxSignal & 0xff) << 8) | 0x7f;
			break;
		case CLD_CONTINUED:
			status = 0xffff;
			break;
		default:
			status = 0;
			break;
	}

	if (statusPtr != 0
		&& renguin_copy_out(statusPtr, &status, sizeof(status)) != B_OK) {
		return -LX_EFAULT;
	}

	if (rusagePtr != 0) {
		// struct rusage: two timevals, then 14 longs.
		int64 rusage[18];
		memset(rusage, 0, sizeof(rusage));
		rusage[0] = usage.user_time / 1000000;
		rusage[1] = usage.user_time % 1000000;
		rusage[2] = usage.kernel_time / 1000000;
		rusage[3] = usage.kernel_time % 1000000;
		if (renguin_copy_out(rusagePtr, rusage, sizeof(rusage)) != B_OK)
			return -LX_EFAULT;
	}

	return child;
}


// #pragma mark - execve


namespace {

//! A kernel-side list of strings (argv or envp) being prepared for exec.
struct StringList {
	StringList()
		:
		strings(NULL), offsets(NULL), count(0), capacity(0), size(0),
		used(0)
	{
	}

	~StringList()
	{
		Clear();
	}

	void Clear()
	{
		free(strings);
		free(offsets);
		strings = NULL;
		offsets = NULL;
		count = capacity = 0;
		size = used = 0;
	}

	void Swap(StringList& other)
	{
		StringList temp;
		memcpy((void*)&temp, (void*)this, sizeof(StringList));
		memcpy((void*)this, (void*)&other, sizeof(StringList));
		memcpy((void*)&other, (void*)&temp, sizeof(StringList));
		memset((void*)&temp, 0, sizeof(StringList));
	}

	status_t Add(const char* string)
	{
		size_t length = strlen(string);
		if (count == capacity) {
			uint32 newCapacity = capacity == 0 ? 32 : capacity * 2;
			if (newCapacity > kMaxArgs)
				return B_TOO_MANY_ARGS;
			size_t* newOffsets = (size_t*)realloc(offsets,
				newCapacity * sizeof(size_t));
			if (newOffsets == NULL)
				return B_NO_MEMORY;
			offsets = newOffsets;
			capacity = newCapacity;
		}
		if (used + length + 1 > size) {
			size_t newSize = size == 0 ? 4096 : size * 2;
			while (used + length + 1 > newSize)
				newSize *= 2;
			if (newSize > MAX_PROCESS_ARGS_SIZE * 2)
				return B_TOO_MANY_ARGS;
			char* newStrings = (char*)realloc(strings, newSize);
			if (newStrings == NULL)
				return B_NO_MEMORY;
			strings = newStrings;
			size = newSize;
		}
		memcpy(strings + used, string, length + 1);
		offsets[count++] = used;
		used += length + 1;
		return B_OK;
	}

	//! Copies a NULL-terminated array of user string pointers.
	status_t AddUserArray(uint64 userArray, char* buffer)
	{
		if (userArray == 0)
			return B_OK;	// Linux accepts NULL argv/envp as empty
		for (uint32 i = 0; ; i++) {
			uint64 pointer;
			if (renguin_copy_in(&pointer, userArray + i * sizeof(uint64),
					sizeof(pointer)) != B_OK) {
				return B_BAD_ADDRESS;
			}
			if (pointer == 0)
				return B_OK;
			if (i >= kMaxArgs)
				return B_TOO_MANY_ARGS;
			ssize_t length = renguin_copy_string_in(buffer, pointer,
				MAX_PROCESS_ARGS_SIZE);
			if (length < 0)
				return length == B_NAME_TOO_LONG ? B_TOO_MANY_ARGS : length;
			status_t status = Add(buffer);
			if (status != B_OK)
				return status;
		}
	}

	const char* At(uint32 index) const
	{
		return strings + offsets[index];
	}

	char*	strings;
	size_t*	offsets;
	uint32	count;
	uint32	capacity;
	size_t	size;
	size_t	used;
};

}	// namespace


/*!	Reads the start of the file at the (absolute) host path. Returns the
	number of bytes read, or an error.
*/
static ssize_t
read_file_start(const char* path, char* buffer, size_t size)
{
	int fd = _kern_open(-1, path, O_RDONLY, 0);
	if (fd < 0)
		return fd;
	ssize_t bytes = _kern_read(fd, 0, buffer, size);
	_kern_close(fd);
	return bytes;
}


/*!	Parses a "#!" line as Linux does (fs/binfmt_script.c): the interpreter is
	the first word, and everything after it, trimmed, is one optional
	argument. \a line must have room for a terminating NUL at \a length.
	Returns false if this is not a usable shebang line.
*/
static bool
parse_shebang(char* line, size_t length, char** _interpreter,
	char** _argument)
{
	if (length < 2 || line[0] != '#' || line[1] != '!')
		return false;
	line[length] = '\0';
	char* end = strchr(line, '\n');
	if (end == NULL && length >= kShebangMax)
		return false;		// line too long: ENOEXEC, as Linux
	if (end != NULL)
		*end = '\0';

	char* p = line + 2;
	while (*p == ' ' || *p == '\t')
		p++;
	if (*p == '\0')
		return false;
	char* interpreter = p;
	while (*p != '\0' && *p != ' ' && *p != '\t')
		p++;
	char* argument = NULL;
	if (*p != '\0') {
		*p++ = '\0';
		while (*p == ' ' || *p == '\t')
			p++;
		if (*p != '\0') {
			argument = p;
			char* last = p + strlen(p) - 1;
			while (last > p
				&& (*last == ' ' || *last == '\t' || *last == '\r')) {
				*last-- = '\0';
			}
		}
	}
	*_interpreter = interpreter;
	*_argument = argument;
	return true;
}


//! Resolves a Linux path (kernel buffer) to an absolute host path.
static status_t
resolve_exec_path(const char* linuxPath, char* hostPath, size_t size)
{
	char* translated = (char*)malloc(B_PATH_NAME_LENGTH);
	if (translated == NULL)
		return B_NO_MEMORY;
	MemoryDeleter translatedDeleter(translated);

	// /proc/self/exe and the like: run what the link points to (busybox
	// re-executes itself this way).
	char procPath[RENGUIN_PROC_PATH_MAX];
	char* procTarget = NULL;
	if (renguin_proc_path(LX_AT_FDCWD, linuxPath, procPath,
			sizeof(procPath))) {
		procTarget = (char*)malloc(B_PATH_NAME_LENGTH);
		if (procTarget == NULL)
			return B_NO_MEMORY;
		int64 length = renguin_proc_readlink(procPath, procTarget,
			B_PATH_NAME_LENGTH - 1);
		if (length < 0 || procTarget[0] != '/') {
			free(procTarget);
			return length == -LX_ENOENT || length >= 0
				? B_ENTRY_NOT_FOUND : B_NOT_ALLOWED;
		}
		procTarget[length] = '\0';
		linuxPath = procTarget;
	}
	MemoryDeleter procTargetDeleter(procTarget);

	const char* path = linuxPath;
	renguin_team* data = renguin_current_team();
	if (linuxPath[0] == '/' && data != NULL && data->have_linux_root
		&& !(strncmp(linuxPath, "/dev", 4) == 0
			&& (linuxPath[4] == '\0' || linuxPath[4] == '/'))
		&& renguin_resolve_in_root(linuxPath, RENGUIN_PATH_FOLLOW_LEAF,
			translated, B_PATH_NAME_LENGTH) == B_OK) {
		path = translated;
	}
	// Relative paths are relative to the program's working directory.
	return vfs_normalize_path(path, hostPath, size, true, false);
}


static int64
exec_error(status_t status)
{
	return status == B_TOO_MANY_ARGS ? -LX_E2BIG : renguin_encode(status);
}


int64
renguin_sys_execve(uint64 userPath, uint64 userArgv, uint64 userEnvp)
{
	// Kernel buffers: the Linux path, the resolved host path, a string
	// buffer for copying arguments, and the start of the file (+1 for a
	// NUL).
	char* buffers = (char*)malloc(B_PATH_NAME_LENGTH * 2
		+ MAX_PROCESS_ARGS_SIZE + kShebangMax + 1);
	if (buffers == NULL)
		return -LX_ENOMEM;
	MemoryDeleter buffersDeleter(buffers);
	char* linuxPath = buffers;
	char* hostPath = buffers + B_PATH_NAME_LENGTH;
	char* stringBuffer = hostPath + B_PATH_NAME_LENGTH;
	char* fileStart = stringBuffer + MAX_PROCESS_ARGS_SIZE;

	ssize_t length = renguin_copy_string_in(linuxPath, userPath,
		B_PATH_NAME_LENGTH);
	if (length < 0)
		return renguin_encode(length);
	if (length == 0)
		return -LX_ENOENT;

	StringList argv;
	StringList envp;
	status_t status = argv.AddUserArray(userArgv, stringBuffer);
	if (status == B_OK)
		status = envp.AddUserArray(userEnvp, stringBuffer);
	if (status != B_OK)
		return exec_error(status);

	// Like Linux since 5.18: an empty argv gets an empty argv[0]. Haiku's
	// exec_team() also needs at least one argument (it stores argv[1...]).
	if (argv.count == 0 && (status = argv.Add("")) != B_OK)
		return exec_error(status);

	// Find the program, following "#!" interpreters as Linux does
	// (binfmt_script): argv becomes interpreter [argument] path argv[1...].
	// Everything that can fail is checked here, before the point of no
	// return in _user_exec().
	for (int depth = 0; ; depth++) {
		status = resolve_exec_path(linuxPath, hostPath, B_PATH_NAME_LENGTH);
		if (status != B_OK)
			return renguin_encode(status);

		struct stat st;
		status = vfs_read_stat(-1, hostPath, true, &st, true);
		if (status != B_OK)
			return renguin_encode(status);
		if (!S_ISREG(st.st_mode) || (st.st_mode & 0111) == 0)
			return -LX_EACCES;

		ssize_t bytes = read_file_start(hostPath, fileStart, kShebangMax);
		if (bytes < 0)
			return renguin_encode(bytes);
		if (bytes >= 4 && memcmp(fileStart, "\177ELF", 4) == 0)
			break;

		char* interpreter;
		char* argument;
		if (!parse_shebang(fileStart, bytes, &interpreter, &argument))
			return -LX_ENOEXEC;		// shells then run it themselves
		if (depth >= kMaxInterpreterDepth)
			return -LX_ELOOP;

		StringList newArgv;
		status = newArgv.Add(interpreter);
		if (status == B_OK && argument != NULL)
			status = newArgv.Add(argument);
		if (status == B_OK)
			status = newArgv.Add(linuxPath);
		for (uint32 i = 1; status == B_OK && i < argv.count; i++)
			status = newArgv.Add(argv.At(i));
		if (status != B_OK)
			return exec_error(status);
		argv.Swap(newArgv);

		if (strlcpy(linuxPath, interpreter, B_PATH_NAME_LENGTH)
				>= B_PATH_NAME_LENGTH) {
			return -LX_ENAMETOOLONG;
		}
	}

	// libroot's flat format (see copy_user_process_args() in team.cpp):
	// argv pointers, NULL, envp pointers, NULL, then the strings. The
	// pointers are user addresses inside the block.
	const uint32 argCount = argv.count;
	const uint32 envCount = envp.count;
	size_t pointerSize = (argCount + envCount + 2) * sizeof(char*);
	size_t flatSize = pointerSize + argv.used + envp.used;
	if (flatSize > MAX_PROCESS_ARGS_SIZE)
		return -LX_E2BIG;
	size_t pathLength = strlen(hostPath) + 1;
	size_t areaSize = PAGE_ALIGN(flatSize + pathLength);

	team_id team = thread_get_current_thread()->team->id;
	void* address = NULL;
	virtual_address_restrictions virtualRestrictions = {};
	virtualRestrictions.address_specification = B_ANY_ADDRESS;
	physical_address_restrictions physicalRestrictions = {};
	area_id area = vm_create_anonymous_area(team, "renguin exec args",
		areaSize, B_NO_LOCK, B_READ_AREA | B_WRITE_AREA, 0, 0,
		&virtualRestrictions, &physicalRestrictions, false, &address);
	if (area < 0)
		return renguin_encode(area);

	addr_t base = (addr_t)address;
	addr_t argStrings = base + pointerSize;
	addr_t envStrings = argStrings + argv.used;
	bool ok = true;
	for (uint32 i = 0; ok && i < argCount + envCount + 2; i++) {
		uint64 pointer = 0;
		if (i < argCount)
			pointer = argStrings + argv.offsets[i];
		else if (i > argCount && i <= argCount + envCount)
			pointer = envStrings + envp.offsets[i - argCount - 1];
		// else: one of the two NULL terminators
		ok = user_memcpy((void*)(base + i * sizeof(char*)), &pointer,
			sizeof(pointer)) == B_OK;
	}
	ok = ok
		&& (argv.used == 0 || user_memcpy((void*)argStrings, argv.strings,
			argv.used) == B_OK)
		&& (envp.used == 0 || user_memcpy((void*)envStrings, envp.strings,
			envp.used) == B_OK)
		&& user_memcpy((void*)(base + flatSize), hostPath, pathLength)
			== B_OK;
	if (!ok) {
		vm_delete_area(team, area, true);
		return -LX_EFAULT;
	}

	renguin_team* data = renguin_current_team();
	mode_t umask = data != NULL ? data->umask : 022;

	// _user_exec() does not return on success, so nothing on this stack
	// is destroyed after it. Free the kernel copies first; everything exec
	// needs is in the user area now.
	argv.Clear();
	envp.Clear();
	free(buffersDeleter.Detach());

	status = _user_exec((const char*)(base + flatSize),
		(const char* const*)base, flatSize, argCount, envCount, umask);

	// Only reached on failure.
	vm_delete_area(team, area, true);
	return renguin_encode(status);
}
