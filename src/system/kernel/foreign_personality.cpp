/*
 * Copyright 2026, Renguin contributors.
 * Distributed under the terms of the MIT License.
 */


/*!	Registry and dispatch for foreign ABI personalities.
	See headers/private/kernel/foreign_personality.h.
*/


#include <foreign_personality.h>

#include <fcntl.h>
#include <string.h>

#include <AutoDeleter.h>

#include <arch/generic/user_memory.h>
#include <kernel.h>
#include <ksignal.h>
#include <lock.h>
#include <syscalls.h>
#include <team.h>
#include <thread.h>
#include <util/AutoLock.h>
#include <util/ThreadAutoLock.h>
#include <vfs.h>


//#define TRACE_FOREIGN_PERSONALITY
#ifdef TRACE_FOREIGN_PERSONALITY
#	define TRACE(x...) dprintf("foreign personality: " x)
#else
#	define TRACE(x...) ;
#endif


static const int32 kMaxPersonalities = 8;
static const bigtime_t kRescanInterval = 10000000LL;
	// Look for newly installed personality modules at most this often.

static mutex sLock = MUTEX_INITIALIZER("foreign personalities");
static const foreign_personality_module_info*
	sPersonalities[kMaxPersonalities];
static int32 sPersonalityCount = 0;
static bigtime_t sLastScan = -1;


static bool
is_personality_loaded_locked(const char* name)
{
	for (int32 i = 0; i < sPersonalityCount; i++) {
		if (strcmp(sPersonalities[i]->info.name, name) == 0)
			return true;
	}
	return false;
}


static void
scan_personality_modules_locked()
{
	sLastScan = system_time();

	void* cookie = open_module_list(FOREIGN_PERSONALITY_MODULE_PREFIX);
	if (cookie == NULL)
		return;

	char name[B_FILE_NAME_LENGTH];
	while (sPersonalityCount < kMaxPersonalities) {
		size_t nameLength = sizeof(name);
		if (read_next_module_name(cookie, name, &nameLength) != B_OK)
			break;

		if (is_personality_loaded_locked(name))
			continue;

		foreign_personality_module_info* module;
		if (get_module(name, (module_info**)&module) != B_OK)
			continue;

		if (module->probe == NULL || module->load == NULL
			|| module->enter_userspace == NULL || module->syscall == NULL) {
			dprintf("foreign personality: %s is incomplete, ignored\n", name);
			put_module(name);
			continue;
		}

		// Personality modules stay loaded: teams may refer to them at any
		// time.
		sPersonalities[sPersonalityCount++] = module;
		dprintf("foreign personality: loaded %s (%s)\n", name,
			module->abi_name != NULL ? module->abi_name : "?");
	}

	close_module_list(cookie);
}


static void
release_team_personality(Team* team, Thread* thread)
{
	foreign_personality_team_released(team);
	if (thread != NULL)
		atomic_and(&thread->flags, ~THREAD_FLAGS_FOREIGN_ABI);
}


status_t
foreign_personality_exec(Team* team, Thread* thread, const char* path,
	char* const* flatArgs, uint32 argCount, uint32 envCount, mode_t umask,
	uint32 flags, const foreign_personality_module_info** _personality,
	addr_t* _entry, addr_t* _stackPointer)
{
	const foreign_personality_module_info* personalities[kMaxPersonalities];
	int32 count;
	{
		MutexLocker locker(sLock);
		if (sLastScan < 0 || system_time() - sLastScan >= kRescanInterval)
			scan_personality_modules_locked();

		count = sPersonalityCount;
		memcpy(personalities, sPersonalities, count * sizeof(personalities[0]));
	}

	if (count == 0) {
		release_team_personality(team, thread);
		return B_NOT_SUPPORTED;
	}

	// The path comes from userland as given to exec()/load_image() and may
	// be relative to the team's working directory (libroot passes "./hello"
	// through unchanged). Resolve it in the team's I/O context; _kern_open()
	// below would otherwise resolve it against the kernel's. Failures are
	// left for the native path to report, which yields the same errors as
	// before.
	char* normalizedPath = (char*)malloc(B_PATH_NAME_LENGTH);
	if (normalizedPath == NULL) {
		release_team_personality(team, thread);
		return B_NOT_SUPPORTED;
	}
	MemoryDeleter normalizedPathDeleter(normalizedPath);
	if (vfs_normalize_path(path, normalizedPath, B_PATH_NAME_LENGTH, true,
			false) != B_OK) {
		release_team_personality(team, thread);
		return B_NOT_SUPPORTED;
	}

	int fd = _kern_open(-1, normalizedPath, O_RDONLY, 0);
	if (fd < 0) {
		release_team_personality(team, thread);
		return B_NOT_SUPPORTED;
	}
	FileDescriptorCloser fdCloser(fd);

	uint8 header[FOREIGN_PERSONALITY_HEADER_SIZE];
	ssize_t headerSize = _kern_read(fd, 0, header, sizeof(header));
	if (headerSize <= 0) {
		release_team_personality(team, thread);
		return B_NOT_SUPPORTED;
	}

	foreign_exec_args args = {};
	args.team = team;
	args.thread = thread;
	args.path = normalizedPath;
	args.flat_args = flatArgs;
	args.arg_count = argCount;
	args.env_count = envCount;
	args.umask = umask;
	args.flags = flags;
	args.fd = fd;
	args.header = header;
	args.header_size = headerSize;

	const foreign_personality_module_info* personality = NULL;
	for (int32 i = 0; i < count; i++) {
		if (personalities[i]->probe(&args) == B_OK) {
			personality = personalities[i];
			break;
		}
	}

	if (personality == NULL) {
		release_team_personality(team, thread);
		return B_NOT_SUPPORTED;
	}

	TRACE("%s claimed \"%s\"\n", personality->abi_name, path);

	// Drop the state of the program being replaced, also when it used the
	// same personality: the new program gets fresh personality data.
	release_team_personality(team, NULL);
	team->personality = personality;
	atomic_or(&thread->flags, THREAD_FLAGS_FOREIGN_ABI);

	status_t status = personality->load(&args);
	if (status != B_OK) {
		dprintf("foreign personality: %s failed to load \"%s\": %s\n",
			personality->abi_name, path, strerror(status));
		// B_NOT_SUPPORTED would make the caller continue natively.
		return status == B_NOT_SUPPORTED ? B_NOT_AN_EXECUTABLE : status;
	}

	*_personality = personality;
	*_entry = args.entry;
	*_stackPointer = args.stack_pointer;
	return B_OK;
}


void
foreign_personality_team_released(Team* team)
{
	const foreign_personality_module_info* personality = team->personality;
	if (personality == NULL)
		return;

	if (personality->team_released != NULL)
		personality->team_released(team);

	team->personality = NULL;
	team->personality_data = NULL;
}


status_t
foreign_personality_fork(Team* parent, Team* child, Thread* childThread)
{
	const foreign_personality_module_info* personality = parent->personality;
	if (personality == NULL)
		return B_OK;

	child->personality = personality;
	child->personality_data = NULL;
	if (personality->team_forked != NULL) {
		status_t status = personality->team_forked(parent, child);
		if (status != B_OK) {
			child->personality = NULL;
			child->personality_data = NULL;
			return status;
		}
	}

#ifdef __x86_64__
	// Foreign ABIs keep the FPU state across fork(), as Linux does. The
	// child loads it on its way to userland (x86_return_to_userland()).
	memcpy(childThread->arch_info.user_fpu_state,
		thread_get_current_thread()->arch_info.user_fpu_state,
		sizeof(childThread->arch_info.user_fpu_state));
#endif

	// The child's thread has not run yet; it enters userland through
	// enter_userspace(), which checks this flag.
	atomic_or(&childThread->flags, THREAD_FLAGS_FOREIGN_ABI);
	return B_OK;
}


void
foreign_personality_thread_entering_userspace(Thread* thread, bool forked)
{
	const foreign_personality_module_info* personality
		= thread->team->personality;
	if (personality != NULL && personality->thread_entering_userspace != NULL)
		personality->thread_entering_userspace(thread, forked);
}


void
foreign_personality_syscall(struct iframe* frame)
{
	Thread* thread = thread_get_current_thread();
	const foreign_personality_module_info* personality
		= thread->team->personality;

	if (personality == NULL) {
		// Should not happen: the flag is only set while the team has a
		// personality. Fail the call rather than the system.
		dprintf("foreign_personality_syscall(): thread %" B_PRId32
			" has no personality\n", thread->id);
		frame->ax = (uint64)(int64)B_BAD_VALUE;
		return;
	}

	personality->syscall(frame);

	// As the native syscall path does after every call: the restarted flag
	// is valid for exactly one call. Haiku's restartable _user_* functions,
	// which personalities may call, read it to pick up saved parameters.
	if ((thread->flags & THREAD_FLAGS_SYSCALL_RESTARTED) != 0)
		atomic_and(&thread->flags, ~THREAD_FLAGS_SYSCALL_RESTARTED);
}


status_t
foreign_personality_setup_signal_frame(Thread* thread,
	struct sigaction* action, struct signal_frame_data* data)
{
	const foreign_personality_module_info* personality
		= thread->team->personality;
	if (personality == NULL || personality->setup_signal_frame == NULL) {
		// Haiku's own frame would need the commpage's handler wrapper,
		// which foreign teams don't have. Don't run the handler.
		return B_NOT_SUPPORTED;
	}
	return personality->setup_signal_frame(thread, action, data);
}


void
foreign_personality_fault_signal(Thread* thread, int number, int32 code,
	int32 error, void* address)
{
	Team* team = thread->team;
	{
		TeamLocker teamLocker(team);
		struct sigaction& action = team->SignalActionFor(number);

		InterruptsSpinLocker locker(team->signal_lock);
		bool blocked
			= (thread->sig_block_mask & SIGNAL_TO_MASK(number)) != 0;
		if (blocked || action.sa_handler == SIG_IGN) {
			action.sa_handler = SIG_DFL;
			thread->sig_block_mask &= ~SIGNAL_TO_MASK(number);
		}
	}

	Signal signal(number, code, error, team->id);
	signal.SetAddress(address);
	send_signal_to_thread(thread, signal, 0);
}


status_t
foreign_personality_user_atomic_test_and_set(int32* value, int32 newValue,
	int32 testAgainst, int32* _oldValue)
{
	// Modules cannot access user memory directly: the instructions that open
	// it up (stac/clac with SMAP) are patched in at boot in the kernel image
	// only. user_access() handles faults as user_memcpy() does.
	if (!IS_USER_ADDRESS(value) || ((addr_t)value & (sizeof(int32) - 1)) != 0)
		return B_BAD_ADDRESS;

	int32 oldValue = 0;
	if (!user_access([=, &oldValue] {
			oldValue = atomic_test_and_set(value, newValue, testAgainst);
		})) {
		return B_BAD_ADDRESS;
	}

	*_oldValue = oldValue;
	return B_OK;
}
