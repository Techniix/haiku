/*
 * Copyright 2026, Renguin contributors.
 * Distributed under the terms of the MIT License.
 */
#ifndef _KERNEL_FOREIGN_PERSONALITY_H
#define _KERNEL_FOREIGN_PERSONALITY_H


/*!	Foreign ABI personalities.

	A personality lets a kernel module run executables of another operating
	system's ABI (for example Linux) directly, without runtime_loader. When a
	team is about to start a new program, the kernel asks every personality
	module whether it claims the executable. The claiming module maps the
	image, builds the initial user stack in its own ABI's format and enters
	userland. From then on, every system call made by a thread of that team
	is passed to the module instead of Haiku's own system call table.

	Native Haiku teams never take any of these paths.

	Personality modules are found under the "personalities" module prefix and
	stay loaded once they have been found.
*/


#include <module.h>
#include <thread_types.h>


#define FOREIGN_PERSONALITY_MODULE_PREFIX	"personalities"

#define FOREIGN_PERSONALITY_HEADER_SIZE		64
	// bytes of the executable passed to probe(); enough for an ELF64 header


struct iframe;
struct sigaction;
struct signal_frame_data;


struct foreign_exec_args {
	Team*			team;
	Thread*			thread;			// the team's main thread (the caller)
	const char*		path;			// absolute, normalized path of the
									// executable (symlinks resolved)
	char* const*	flat_args;		// kernel copy: arguments, NULL,
									// environment, NULL, then the strings
	uint32			arg_count;
	uint32			env_count;
	mode_t			umask;
	uint32			flags;			// TEAM_ARGS_FLAG_*
	int				fd;				// executable, opened read-only by the
									// kernel, closed by the kernel
	const uint8*	header;			// first bytes of the executable
	size_t			header_size;

	// set by load()
	addr_t			entry;
	addr_t			stack_pointer;
};


typedef struct foreign_personality_module_info {
	module_info		info;

	const char*		abi_name;
		// short name used in log messages, e.g. "linux"

	status_t		(*probe)(const struct foreign_exec_args* args);
		// Returns B_OK if the personality claims the executable. Must not
		// change any state.

	status_t		(*load)(struct foreign_exec_args* args);
		// Called after a successful probe() from the new team's main thread.
		// The team's old address space contents are already gone. Maps the
		// image, builds the initial user stack and sets entry and
		// stack_pointer.

	status_t		(*enter_userspace)(Thread* thread, addr_t entry,
						addr_t stackPointer);
		// Enters userland with the ABI's initial register state. Does not
		// return on success.

	void			(*syscall)(struct iframe* frame);
		// Handles a system call of a thread of a team using this
		// personality. The call number and arguments are read from the
		// frame and the result is stored in it.

	void			(*team_released)(Team* team);
		// The team stops using this personality: it executes a native
		// program, or it is being deleted. May be NULL.

	status_t		(*team_forked)(Team* parent, Team* child);
		// Called in the parent during fork(), after the child's address
		// space has been copied and before its thread runs. Sets up
		// child->personality_data. May be NULL: the child then starts
		// without personality data.

	void			(*thread_entering_userspace)(Thread* thread, bool forked);
		// Called in a new thread of a team using this personality (for
		// example a fork()ed child's main thread) right before it enters
		// userland for the first time. Haiku's own TLS setup is skipped for
		// such threads. May be NULL.

	status_t		(*setup_signal_frame)(Thread* thread,
						struct sigaction* action,
						struct signal_frame_data* data);
		// Called instead of arch_setup_signal_frame() when a thread of a
		// team using this personality is to run a signal handler: builds the
		// ABI's signal frame on the user stack and points the thread's user
		// iframe at the handler.  data is what Haiku would have put into
		// its own frame (signal info, the mask to restore, the syscall
		// restart flags that were in effect). The handler's block mask is
		// already set. May be NULL: such teams cannot run signal handlers
		// (they have no commpage for Haiku's handler wrapper).
} foreign_personality_module_info;


#ifdef __cplusplus
extern "C" {
#endif

status_t	foreign_personality_exec(Team* team, Thread* thread,
				const char* path, char* const* flatArgs, uint32 argCount,
				uint32 envCount, mode_t umask, uint32 flags,
				const foreign_personality_module_info** _personality,
				addr_t* _entry, addr_t* _stackPointer);
	// B_OK: a personality claimed and loaded the executable.
	// B_NOT_SUPPORTED: no personality claimed it; continue natively.
	// Other errors: a personality claimed it but loading failed.

void		foreign_personality_team_released(Team* team);

status_t	foreign_personality_fork(Team* parent, Team* child,
				Thread* childThread);
	// fork(): gives the child the parent's personality, if any

void		foreign_personality_thread_entering_userspace(Thread* thread,
				bool forked);
	// called from enter_userspace() for threads with THREAD_FLAGS_FOREIGN_ABI

void		foreign_personality_syscall(struct iframe* frame);
	// called from the architecture's system call entry

status_t	foreign_personality_setup_signal_frame(Thread* thread,
				struct sigaction* action, struct signal_frame_data* data);
	// called from setup_signal_frame() (signal.cpp) for threads with
	// THREAD_FLAGS_FOREIGN_ABI

void		foreign_personality_fault_signal(Thread* thread, int number,
				int32 code, int32 error, void* address);
	// Sends the signal for a fault (a bad memory access, an illegal
	// instruction, ...) to a thread with THREAD_FLAGS_FOREIGN_ABI, the way
	// Linux and FreeBSD do: if the signal is blocked or ignored, it is
	// unblocked and its default action restored, so the program ends rather
	// than faulting again forever, and Haiku's debugger is not called in.

status_t	foreign_personality_user_atomic_test_and_set(int32* value,
				int32 newValue, int32 testAgainst, int32* _oldValue);
	// For personalities: compare-and-swap on a 32-bit word of the current
	// team's user memory (futexes). B_BAD_ADDRESS for kernel, misaligned or
	// unmapped addresses. Modules cannot do this themselves, see
	// foreign_personality.cpp.

#ifdef __cplusplus
}
#endif


#endif	/* _KERNEL_FOREIGN_PERSONALITY_H */
