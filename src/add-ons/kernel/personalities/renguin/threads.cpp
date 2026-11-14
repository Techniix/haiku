/*
 * Copyright 2026, Renguin contributors.
 * Distributed under the terms of the MIT License.
 *
 * Linux threads: clone(CLONE_THREAD), clone3(), set_tid_address(),
 * set_robust_list()/get_robust_list(), thread exit, and CPU affinity.
 *
 * FreeBSD counterparts: sys/compat/linux/linux_fork.c (linux_clone_thread(),
 * linux_clone3(), linux_exit()), linux_futex.c (linux_set_robust_list(),
 * linux_get_robust_list(), release_futexes()) and linux_emul.c
 * (linux_thread_detach(), which clears and wakes child_clear_tid).
 *
 * A Linux thread is a Haiku thread of the same team. It is created the way
 * fork_team() creates a fork()ed child's main thread: thread_create_thread()
 * with a copy of the caller's syscall frame (arch_fork_arg), so the new thread
 * returns from the clone() system call with rax = 0, on the stack and with the
 * thread pointer the caller asked for. libc (glibc's clone()/clone3()
 * wrappers, musl's __clone()) keeps the start function and its argument in
 * registers or on the new stack, both of which the frame copy preserves.
 *
 * The thread's stack belongs to the Linux program (libc mmap()s it), so the
 * Thread is given that stack's bounds and no stack area of its own: Haiku
 * neither allocates nor deletes one.
 */


#include "renguin.h"
#include "linux_defs.h"

#include <stdlib.h>
#include <string.h>

#include <KernelExport.h>
#include <OS.h>

#include <AutoDeleter.h>
#include <Referenceable.h>

#include <arch/x86/arch_thread.h>
#include <arch/x86/arch_thread_types.h>
#include <arch/x86/64/iframe.h>
#include <cpu.h>
#include <kernel.h>
#include <smp.h>
#include <team.h>
#include <thread.h>
#include <util/AutoLock.h>


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
#define LX_CLONE_CLEAR_SIGHAND	0x100000000ULL	// clone3() only
#define LX_CLONE_INTO_CGROUP	0x200000000ULL	// clone3() only

// clone3() argument block (include/uapi/linux/sched.h). Versions are told
// apart by size: 64 bytes (VER0), 80 (VER1, set_tid), 88 (VER2, cgroup).
struct linux_clone_args {
	uint64	flags;
	uint64	pidfd;
	uint64	child_tid;
	uint64	parent_tid;
	uint64	exit_signal;
	uint64	stack;
	uint64	stack_size;
	uint64	tls;
	uint64	set_tid;
	uint64	set_tid_size;
	uint64	cgroup;
};
#define LX_CLONE_ARGS_SIZE_VER0	64

// Robust futex list (include/uapi/linux/futex.h).
struct linux_robust_list_head {
	uint64	list;				// struct robust_list* next
	int64	futex_offset;
	uint64	list_op_pending;
};
#define LX_FUTEX_WAITERS		0x80000000
#define LX_FUTEX_OWNER_DIED		0x40000000
#define LX_FUTEX_TID_MASK		0x3fffffff
#define LX_ROBUST_LIST_LIMIT	2048		// ROBUST_LIST_LIMIT

// The flags of a thread-style clone: what glibc's and musl's pthread_create()
// pass, plus the CLONE_DETACHED and tracing flags Linux ignores. Sharing the
// file system information and descriptors is not optional on Haiku, where all
// threads of a team share them.
static const uint64 kThreadCloneRequired = LX_CLONE_VM | LX_CLONE_FS
	| LX_CLONE_FILES | LX_CLONE_SIGHAND | LX_CLONE_THREAD;
static const uint64 kThreadCloneOptional = LX_CLONE_SYSVSEM | LX_CLONE_SETTLS
	| LX_CLONE_PARENT_SETTID | LX_CLONE_CHILD_CLEARTID | LX_CLONE_CHILD_SETTID
	| LX_CLONE_DETACHED | LX_CLONE_UNTRACED | LX_CLONE_PTRACE;


// #pragma mark - per-thread data


status_t
renguin_team_threads_init(renguin_team* data)
{
	mutex_init(&data->lock, "renguin team");
	data->threads = NULL;
	return B_OK;
}


/*!	Frees the data of all of the team's threads. Called when the team stops
	using the personality: it is being deleted (no threads are left) or its
	main thread is executing a new program (the other threads have been
	killed). In the latter case the caller is that main thread, whose pointer
	to its data must not survive.
*/
void
renguin_team_threads_free(renguin_team* data)
{
	Thread* current = thread_get_current_thread();
	if (current != NULL && current->team != NULL
		&& current->team->personality_data == data) {
		current->personality_data = NULL;
	}

	mutex_lock(&data->lock);
	renguin_thread* thread = data->threads;
	data->threads = NULL;
	mutex_destroy(&data->lock);

	while (thread != NULL) {
		renguin_thread* next = thread->next;
		free(thread);
		thread = next;
	}
}


static renguin_thread*
allocate_thread_data(renguin_team* teamData, thread_id id)
{
	renguin_thread* data = (renguin_thread*)calloc(1, sizeof(renguin_thread));
	if (data == NULL)
		return NULL;
	data->id = id;

	MutexLocker locker(teamData->lock);
	data->next = teamData->threads;
	teamData->threads = data;
	return data;
}


static void
free_thread_data(renguin_team* teamData, renguin_thread* data)
{
	{
		MutexLocker locker(teamData->lock);
		renguin_thread** link = &teamData->threads;
		while (*link != NULL && *link != data)
			link = &(*link)->next;
		if (*link == NULL) {
			// not ours: the list has already been freed
			return;
		}
		*link = data->next;
	}
	free(data);
}


renguin_thread*
renguin_current_thread_data(bool create)
{
	Thread* thread = thread_get_current_thread();
	renguin_thread* data = (renguin_thread*)thread->personality_data;
	if (data != NULL || !create)
		return data;

	renguin_team* teamData = (renguin_team*)thread->team->personality_data;
	if (teamData == NULL)
		return NULL;

	data = allocate_thread_data(teamData, thread->id);
	thread->personality_data = data;
	return data;
}


// #pragma mark - thread creation


/*!	clone() of a thread. Mirrors fork_team()'s creation of a child's main
	thread, in the caller's own team. The new Thread gets:
	- a copy of the caller's syscall frame with rax = 0 and the new stack
	  pointer (Linux keeps the caller's if newStack is 0);
	- the thread pointer (FS base) from CLONE_SETTLS, else the caller's, and
	  the caller's GS base, as Linux's copy_thread() does;
	- the caller's signal mask (Linux: inherited);
	- THREAD_FLAGS_FOREIGN_ABI before it first runs, so its system calls come
	  here and Haiku leaves its TLS alone (thread.cpp, enter_userspace()).
	The thread is created suspended; the parent writes the TIDs into the
	shared memory before letting it run, as Linux does before the child can
	be scheduled.
*/
static int64
renguin_create_thread(uint64 flags, uint64 newStack, uint64 parentTid,
	uint64 childTid, uint64 tls)
{
	Thread* parent = thread_get_current_thread();
	Team* team = parent->team;
	renguin_team* teamData = (renguin_team*)team->personality_data;
	if (teamData == NULL)
		return -LX_EINVAL;

	if (newStack != 0 && !IS_USER_ADDRESS((addr_t)newStack))
		return -LX_EINVAL;
	if ((flags & LX_CLONE_SETTLS) != 0 && tls != 0
		&& !IS_USER_ADDRESS((addr_t)tls)) {
		return -LX_EPERM;
	}

	iframe* frame = x86_get_user_iframe();
	if (frame == NULL)
		return -LX_EINVAL;

	// The per-thread data, listed in the team right away so that it is
	// freed with the team even if the thread is killed before it ever runs.
	renguin_thread* data = allocate_thread_data(teamData, -1);
	if (data == NULL)
		return -LX_ENOMEM;
	if ((flags & LX_CLONE_CHILD_CLEARTID) != 0)
		data->clear_child_tid = (addr_t)childTid;
	data->starting = true;
	// The blocked signals Haiku has no number for are inherited like the
	// rest of the mask (which Haiku copies, below).
	renguin_thread* parentData = (renguin_thread*)parent->personality_data;
	if (parentData != NULL)
		data->unmapped_blocked = parentData->unmapped_blocked;

	// The register state the thread starts with. Freed by the new thread in
	// enter_userspace(), or below if it is never started.
	arch_fork_arg* forkArgs = (arch_fork_arg*)malloc(sizeof(arch_fork_arg));
	if (forkArgs == NULL) {
		free_thread_data(teamData, data);
		return -LX_ENOMEM;
	}
	forkArgs->iframe = *frame;
	forkArgs->iframe.ax = 0;
	if (newStack != 0)
		forkArgs->iframe.user_sp = newStack;

	Thread* thread;
	status_t status = Thread::Create(parent->name, thread);
	if (status != B_OK) {
		free(forkArgs);
		free_thread_data(teamData, data);
		return renguin_encode(status);
	}
	BReference<Thread> threadReference(thread, true);

	thread->personality_data = data;
	atomic_or(&thread->flags, THREAD_FLAGS_FOREIGN_ABI);
	thread->user_local_storage = (flags & LX_CLONE_SETTLS) != 0
		? (addr_t)tls : parent->user_local_storage;
	thread->arch_info.user_gs_base = parent->arch_info.user_gs_base;

	// The floating-point state, as Linux's fpu_clone() copies it: the
	// caller's, saved on entry to this system call. The kernel patch loads
	// it on the thread's way to userland (x86_return_to_userland()), so the
	// thread inherits it, including the control state (MXCSR, x87 control
	// word) that C11 says a new thread inherits. The frame must point at the
	// new thread's own buffer, not the caller's: a signal handled before the
	// thread first reaches userland reads it.
	memcpy(thread->arch_info.user_fpu_state, parent->arch_info.user_fpu_state,
		sizeof(thread->arch_info.user_fpu_state));
	forkArgs->iframe.fpu = thread->arch_info.user_fpu_state;

	// Describe the stack libc gave the thread (informational: get_thread_info()
	// and core dumps). A non-zero user_stack_base also keeps
	// thread_create_thread() from allocating a stack area of its own.
	addr_t stackPointer = forkArgs->iframe.user_sp;
	area_info stackArea;
	area_id stackAreaID = area_for((void*)(stackPointer - 1));
	if (stackAreaID >= 0 && get_area_info(stackAreaID, &stackArea) == B_OK) {
		thread->user_stack_base = (addr_t)stackArea.address;
		thread->user_stack_size = stackPointer - (addr_t)stackArea.address;
	} else {
		thread->user_stack_base = ROUNDDOWN(stackPointer - 1, B_PAGE_SIZE);
		thread->user_stack_size = stackPointer - thread->user_stack_base;
	}
	thread->user_stack_area = -1;
	if (thread->user_stack_base == 0)
		thread->user_stack_base = B_PAGE_SIZE;	// never 0, see above

	ThreadCreationAttributes attributes(NULL, parent->name, parent->priority,
		NULL, team->id, thread);
	attributes.forkArgs = forkArgs;
	attributes.signal_mask = parent->sig_block_mask;

	thread_id id = thread_create_thread(attributes, false);
	if (id < 0) {
		thread->personality_data = NULL;
		free(forkArgs);
		free_thread_data(teamData, data);
		return renguin_encode(id);
	}
	data->id = id;

	// Both TIDs are in memory shared with the new thread.
	int32 tid = id;
	if ((flags & LX_CLONE_PARENT_SETTID) != 0)
		renguin_copy_out(parentTid, &tid, sizeof(tid));
	if ((flags & LX_CLONE_CHILD_SETTID) != 0)
		renguin_copy_out(childTid, &tid, sizeof(tid));

	resume_thread(id);
	return id;
}


/*!	Validates the flags of a thread-style clone(). Returns 0 if they describe
	a thread Renguin can create, a negative Linux errno otherwise.
*/
static int64
check_thread_clone_flags(uint64 flags)
{
	// Linux's own consistency checks (copy_process()).
	if ((flags & LX_CLONE_THREAD) != 0 && (flags & LX_CLONE_SIGHAND) == 0)
		return -LX_EINVAL;
	if ((flags & LX_CLONE_SIGHAND) != 0 && (flags & LX_CLONE_VM) == 0)
		return -LX_EINVAL;

	if ((flags & kThreadCloneRequired) != kThreadCloneRequired
		|| (flags & ~(kThreadCloneRequired | kThreadCloneOptional)) != 0) {
		dprintf("renguin: team %" B_PRId32 ": thread clone flags %#" B_PRIx64
			" not supported\n", thread_get_current_thread()->team->id, flags);
		return -LX_ENOSYS;
	}
	return 0;
}


bool
renguin_is_thread_clone(uint64 flags)
{
	return (flags & (LX_CLONE_THREAD | LX_CLONE_SIGHAND)) != 0
		|| ((flags & LX_CLONE_VM) != 0 && (flags & LX_CLONE_VFORK) == 0);
}


int64
renguin_clone_thread(uint64 flags, uint64 newStack, uint64 parentTid,
	uint64 childTid, uint64 tls)
{
	// The exit signal of a thread is ignored (Linux: CLONE_THREAD children
	// never signal their parent).
	flags &= ~(uint64)LX_CSIGNAL;

	int64 result = check_thread_clone_flags(flags);
	if (result != 0)
		return result;

	return renguin_create_thread(flags, newStack, parentTid, childTid, tls);
}


/*!	clone3(): the clone() arguments in a structure. glibc's pthread_create()
	tries it first and falls back to clone() on ENOSYS.
*/
int64
renguin_sys_clone3(uint64 userArgs, uint64 size)
{
	if (size > B_PAGE_SIZE)
		return -LX_E2BIG;
	if (size < LX_CLONE_ARGS_SIZE_VER0)
		return -LX_EINVAL;

	linux_clone_args args;
	memset(&args, 0, sizeof(args));
	if (renguin_copy_in(&args, userArgs, min_c(size, sizeof(args))) != B_OK)
		return -LX_EFAULT;

	// A larger structure from a newer libc is fine as long as the fields we
	// don't know are zero (Linux: copy_struct_from_user()).
	if (size > sizeof(args)) {
		uint8 extra[64];
		for (uint64 offset = sizeof(args); offset < size;
				offset += sizeof(extra)) {
			size_t chunk = min_c(sizeof(extra), size - offset);
			if (renguin_copy_in(extra, userArgs + offset, chunk) != B_OK)
				return -LX_EFAULT;
			for (size_t i = 0; i < chunk; i++) {
				if (extra[i] != 0)
					return -LX_E2BIG;
			}
		}
	}

	// Linux's clone3_args_valid().
	if ((args.flags & (LX_CLONE_DETACHED | LX_CSIGNAL)) != 0
		|| args.exit_signal > 64
		|| ((args.flags & (LX_CLONE_THREAD | LX_CLONE_PARENT)) != 0
			&& args.exit_signal != 0)
		|| (args.stack == 0) != (args.stack_size == 0)) {
		return -LX_EINVAL;
	}
	if ((args.flags & (LX_CLONE_PIDFD | LX_CLONE_CLEAR_SIGHAND
			| LX_CLONE_INTO_CGROUP)) != 0
		|| args.set_tid_size != 0) {
		dprintf("renguin: team %" B_PRId32 ": clone3 flags %#" B_PRIx64
			" not supported\n", thread_get_current_thread()->team->id,
			args.flags);
		return -LX_ENOSYS;
	}

	// The stack is given as base and size; the thread starts at its top.
	uint64 stackTop = 0;
	if (args.stack != 0) {
		stackTop = args.stack + args.stack_size;
		if (stackTop < args.stack)
			return -LX_EINVAL;
	}

	if (renguin_is_thread_clone(args.flags)) {
		return renguin_clone_thread(args.flags, stackTop, args.parent_tid,
			args.child_tid, args.tls);
	}

	return renguin_sys_clone(args.flags | (args.exit_signal & LX_CSIGNAL),
		stackTop, args.parent_tid, args.child_tid, args.tls);
}


/*!	Kernel hook: a new thread of a Linux team is about to enter userland for
	the first time (thread.cpp, enter_userspace()). That is either a thread
	created by renguin_create_thread() (set up completely already) or the
	main thread of a fork()ed child, which needs its GS base and the TIDs
	clone() asked for.
*/
void
renguin_thread_entering_userspace(Thread* thread, bool forked)
{
	renguin_thread* data = (renguin_thread*)thread->personality_data;
	if (data != NULL && data->starting) {
		data->starting = false;
		return;
	}

	renguin_team* teamData = (renguin_team*)thread->team->personality_data;
	if (teamData == NULL || !forked)
		return;

	// The FS base was copied by fork_team(); the GS base is ours to copy.
	// It is loaded on the way to userland (x86_set_tls_context()).
	thread->arch_info.user_gs_base = teamData->fork_gs_base;

	// glibc passes &THREAD_SELF->tid for CLONE_CHILD_SETTID and relies on it
	// (raise(), pthread_self()->tid).
	if (teamData->fork_set_child_tid != 0) {
		int32 tid = thread->id;
		renguin_copy_out(teamData->fork_set_child_tid, &tid, sizeof(tid));
		teamData->fork_set_child_tid = 0;
	}

	if (teamData->fork_clear_child_tid != 0
		|| teamData->fork_unmapped_blocked != 0) {
		data = renguin_current_thread_data(true);
		if (data != NULL) {
			data->clear_child_tid = teamData->fork_clear_child_tid;
			data->unmapped_blocked = teamData->fork_unmapped_blocked;
		}
		teamData->fork_clear_child_tid = 0;
		teamData->fork_unmapped_blocked = 0;
	}
}


// #pragma mark - thread exit


/*!	Walks the robust futex list of the exiting thread, as Linux's
	exit_robust_list() does: every lock the thread still owns is marked
	FUTEX_OWNER_DIED and one waiter is woken, so that the next locker gets
	EOWNERDEAD.
*/
static void
release_robust_futex(addr_t entry, int64 offset, int32 tid, team_id team,
	bool pending)
{
	addr_t word = entry + offset;
	if ((word & 3) != 0 || !IS_USER_ADDRESS(word))
		return;

	for (int attempt = 0; attempt < 32; attempt++) {
		uint32 value;
		if (renguin_copy_in(&value, word, sizeof(value)) != B_OK)
			return;

		// The thread died between releasing the lock in userland and waking
		// a waiter: do the wakeup for it (Linux's handle_futex_death()).
		if (pending && value == 0) {
			renguin_futex_wake(team, word, 1);
			return;
		}

		if ((int32)(value & LX_FUTEX_TID_MASK) != tid)
			return;

		uint32 newValue = (value & LX_FUTEX_WAITERS) | LX_FUTEX_OWNER_DIED;
		uint32 oldValue;
		if (renguin_user_atomic_cmpxchg(word, value, newValue, &oldValue)
				!= B_OK) {
			return;
		}
		if (oldValue != value)
			continue;	// changed under us (a waiter set FUTEX_WAITERS)

		if ((value & LX_FUTEX_WAITERS) != 0)
			renguin_futex_wake(team, word, 1);
		return;
	}
}


static void
exit_robust_list(addr_t head, int32 tid, team_id team)
{
	linux_robust_list_head list;
	if (renguin_copy_in(&list, head, sizeof(list)) != B_OK)
		return;

	// The entry being locked or unlocked when the thread died is handled
	// last, and only if it is not also on the list.
	addr_t pending = list.list_op_pending & ~(uint64)1;

	addr_t entry = list.list & ~(uint64)1;
	for (int count = 0; entry != head && count < LX_ROBUST_LIST_LIMIT;
			count++) {
		uint64 next;
		if (renguin_copy_in(&next, entry, sizeof(next)) != B_OK)
			return;
		if (entry != pending)
			release_robust_futex(entry, list.futex_offset, tid, team, false);
		entry = next & ~(uint64)1;
	}

	if (pending != 0)
		release_robust_futex(pending, list.futex_offset, tid, team, true);
}


/*!	exit(): ends the calling thread. As in Linux's mm_release(), the thread's
	robust futexes are released and, for CLONE_CHILD_CLEARTID or
	set_tid_address(), the TID word is cleared and one waiter woken: that is
	how pthread_join() learns that the thread is gone.

	Haiku ends the whole team when its main thread exits; Linux would keep
	the process alive while other threads run. Programs end with exit_group()
	(libc's exit()), so this only matters for a main thread calling
	pthread_exit().
*/
void
renguin_thread_exit(status_t status)
{
	Thread* thread = thread_get_current_thread();
	renguin_team* teamData = (renguin_team*)thread->team->personality_data;
	renguin_thread* data = (renguin_thread*)thread->personality_data;

	if (data != NULL) {
		if (data->robust_list != 0)
			exit_robust_list(data->robust_list, thread->id, thread->team->id);

		if (data->clear_child_tid != 0) {
			int32 zero = 0;
			if (renguin_copy_out(data->clear_child_tid, &zero, sizeof(zero))
					== B_OK) {
				renguin_futex_wake(thread->team->id, data->clear_child_tid, 1);
			}
		}

		thread->personality_data = NULL;
		if (teamData != NULL)
			free_thread_data(teamData, data);
	}

	_user_exit_thread(status);
}


// #pragma mark - system calls


//! set_tid_address: where to clear the TID and wake a futex on thread exit
//! (glibc and musl point it at the thread descriptor's tid field).
int64
renguin_sys_set_tid_address(uint64 tidptr)
{
	renguin_thread* data = renguin_current_thread_data(true);
	if (data != NULL)
		data->clear_child_tid = (addr_t)tidptr;
	return thread_get_current_thread()->id;
}


int64
renguin_sys_set_robust_list(uint64 head, uint64 length)
{
	if (length != sizeof(linux_robust_list_head))
		return -LX_EINVAL;
	if (head != 0 && !IS_USER_ADDRESS((addr_t)head))
		return -LX_EFAULT;

	renguin_thread* data = renguin_current_thread_data(true);
	if (data == NULL)
		return -LX_ENOMEM;
	data->robust_list = (addr_t)head;
	return 0;
}


int64
renguin_sys_get_robust_list(int64 tid, uint64 headPtr, uint64 lengthPtr)
{
	Thread* current = thread_get_current_thread();
	uint64 head = 0;

	if (tid == 0 || tid == current->id) {
		renguin_thread* data = (renguin_thread*)current->personality_data;
		if (data != NULL)
			head = data->robust_list;
	} else {
		// Only threads of our own team (Linux would allow any process the
		// caller may ptrace).
		renguin_team* teamData = renguin_current_team();
		if (teamData == NULL)
			return -LX_ESRCH;

		bool found = false;
		{
			MutexLocker locker(teamData->lock);
			for (renguin_thread* data = teamData->threads; data != NULL;
					data = data->next) {
				if (data->id == (thread_id)tid) {
					head = data->robust_list;
					found = true;
					break;
				}
			}
		}
		if (!found) {
			Thread* thread = Thread::Get((thread_id)tid);
			if (thread == NULL)
				return -LX_ESRCH;
			bool sameTeam = thread->team == current->team;
			thread->ReleaseReference();
			if (!sameTeam)
				return -LX_EPERM;
		}
	}

	uint64 length = sizeof(linux_robust_list_head);
	if (renguin_copy_out(headPtr, &head, sizeof(head)) != B_OK
		|| renguin_copy_out(lengthPtr, &length, sizeof(length)) != B_OK) {
		return -LX_EFAULT;
	}
	return 0;
}


// #pragma mark - CPU affinity


/*!	sched_getaffinity: Haiku has no per-thread CPU affinity, so every thread
	may run on every enabled CPU. glibc's and musl's sysconf(_SC_NPROCESSORS_
	ONLN) and get_nprocs() fall back to this, so the CPU count matters. The
	kernel returns the size of the mask it wrote.
*/
int64
renguin_sys_sched_getaffinity(int64 pid, uint64 size, uint64 maskPtr)
{
	if (pid < 0)
		return -LX_ESRCH;
	if (pid != 0) {
		Thread* thread = Thread::Get((thread_id)pid);
		if (thread == NULL)
			return -LX_ESRCH;
		thread->ReleaseReference();
	}

	int32 cpuCount = smp_get_num_cpus();
	// The mask is a whole number of longs (Linux: nr_cpu_ids rounded up).
	size_t needed = ((cpuCount + 63) / 64) * sizeof(uint64);
	if (size < needed || (size & (sizeof(uint64) - 1)) != 0)
		return -LX_EINVAL;

	uint64 mask[4];
	size_t maskSize = min_c(needed, sizeof(mask));
	memset(mask, 0, sizeof(mask));
	for (int32 i = 0; i < cpuCount && i < (int32)(sizeof(mask) * 8); i++) {
		if (gCPU[i].disabled)
			continue;
		mask[i / 64] |= (uint64)1 << (i % 64);
	}

	if (renguin_copy_out(maskPtr, mask, maskSize) != B_OK)
		return -LX_EFAULT;
	return maskSize;
}


//! sched_setaffinity: accepted if the mask allows at least one CPU; Haiku
//! threads cannot be pinned, so it has no effect.
int64
renguin_sys_sched_setaffinity(int64 pid, uint64 size, uint64 maskPtr)
{
	if (pid < 0)
		return -LX_ESRCH;
	if (pid != 0) {
		Thread* thread = Thread::Get((thread_id)pid);
		if (thread == NULL)
			return -LX_ESRCH;
		thread->ReleaseReference();
	}

	uint64 mask[4];
	memset(mask, 0, sizeof(mask));
	size_t copySize = min_c(size, sizeof(mask));
	if (renguin_copy_in(mask, maskPtr, copySize) != B_OK)
		return -LX_EFAULT;

	int32 cpuCount = min_c(smp_get_num_cpus(), (int32)(sizeof(mask) * 8));
	for (int32 i = 0; i < cpuCount; i++) {
		if ((mask[i / 64] & ((uint64)1 << (i % 64))) != 0)
			return 0;
	}
	return -LX_EINVAL;
}


int64
renguin_sys_getcpu(uint64 cpuPtr, uint64 nodePtr, uint64 cache)
{
	(void)cache;
	uint32 cpu = smp_get_current_cpu();
	uint32 node = 0;
	if (cpuPtr != 0 && renguin_copy_out(cpuPtr, &cpu, sizeof(cpu)) != B_OK)
		return -LX_EFAULT;
	if (nodePtr != 0 && renguin_copy_out(nodePtr, &node, sizeof(node)) != B_OK)
		return -LX_EFAULT;
	return 0;
}
