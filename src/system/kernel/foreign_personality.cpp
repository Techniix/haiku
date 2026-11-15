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
#include <kimage.h>
#include <ksignal.h>
#include <lock.h>
#include <syscalls.h>
#include <team.h>
#include <thread.h>
#include <user_debugger.h>
#include <util/AutoLock.h>
#include <util/ThreadAutoLock.h>
#include <vfs.h>
#include <vm/vm.h>
#include <vm/VMArea.h>
#include <vm/VMCache.h>

#include "cache/vnode_store.h"
#include "vm/VMAddressSpaceLocking.h"

#ifdef __x86_64__
#	include <arch/x86/arch_thread.h>
#endif


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


/*!	Where a foreign-ABI fault happened, for the fault log: the image (path
	and offset from its load base) that contains \a address, or "?".
*/
struct fault_image_lookup {
	addr_t	address;
	char	path[96];
	addr_t	offset;
	bool	found;
};


static bool
find_fault_image(struct image* image, void* cookie)
{
	fault_image_lookup* lookup = (fault_image_lookup*)cookie;
	const image_info& info = image->info.basic_info;
	addr_t start = (addr_t)info.text;
	if (lookup->address < start
		|| lookup->address >= start + (addr_t)info.text_size) {
		return false;
	}
	const char* name = strrchr(info.name, '/');
	strlcpy(lookup->path, name != NULL ? name + 1 : info.name,
		sizeof(lookup->path));
	lookup->offset = lookup->address - (addr_t)image->info.text_delta;
	lookup->found = true;
	return true;
}


static void
describe_fault_address(team_id team, addr_t address, char* buffer,
	size_t size)
{
	fault_image_lookup lookup = {};
	lookup.address = address;
	image_iterate_through_team_images(team, &find_fault_image, &lookup);
	if (lookup.found) {
		snprintf(buffer, size, "%#lx (%s+%#lx)", address, lookup.path,
			lookup.offset);
		return;
	}

	// Not a registered image (shared libraries that the Linux program's own
	// dynamic loader mapped): the area's mapped file and the file offset.
	// The cache walk is the one the core dump writer uses.
	dev_t device = -1;
	ino_t node = -1;
	addr_t fileOffset = 0;
	{
		AddressSpaceReadLocker locker(team);
		VMArea* area = locker.IsLocked()
			? locker.AddressSpace()->LookupArea(address) : NULL;
		if (area != NULL) {
			fileOffset = address - area->Base() + area->cache_offset;
			VMCache* cache = vm_area_get_locked_cache(area);
			while (VMCache* source = cache->source) {
				source->Lock();
				source->AcquireRefLocked();
				cache->ReleaseRefAndUnlock();
				cache = source;
			}
			if (cache->type == CACHE_TYPE_VNODE) {
				VMVnodeCache* vnodeCache = (VMVnodeCache*)cache;
				device = vnodeCache->DeviceId();
				node = vnodeCache->InodeId();
			}
			cache->ReleaseRefAndUnlock();
		}
	}
	char name[B_FILE_NAME_LENGTH];
	struct vnode* vnode;
	if (node >= 0 && vfs_get_vnode(device, node, true, &vnode) == B_OK) {
		if (vfs_get_vnode_name(vnode, name, sizeof(name)) != B_OK)
			strlcpy(name, "?", sizeof(name));
		vfs_put_vnode(vnode);
		snprintf(buffer, size, "%#lx (file %s offset %#lx)", address, name,
			fileOffset);
	} else
		snprintf(buffer, size, "%#lx (?)", address);
}


void
foreign_personality_log_user_stack(const char* prefix)
{
#ifdef __x86_64__
	Thread* thread = thread_get_current_thread();
	iframe* frame = x86_get_user_iframe();
	if (frame == NULL)
		return;
	char where[128];
	describe_fault_address(thread->team->id, frame->ip, where, sizeof(where));
	dprintf("%s thread %" B_PRId32 " at %s\n", prefix, thread->id, where);
	addr_t bp = frame->bp;
	for (int i = 0; i < 8 && bp != 0 && IS_USER_ADDRESS(bp)
			&& (bp & 7) == 0; i++) {
		addr_t pair[2];
		if (user_memcpy(pair, (void*)bp, sizeof(pair)) != B_OK
			|| pair[1] == 0) {
			break;
		}
		describe_fault_address(thread->team->id, pair[1], where,
			sizeof(where));
		dprintf("%s   called from %s\n", prefix, where);
		if (pair[0] <= bp)
			break;
		bp = pair[0];
	}
#else
	(void)prefix;
#endif
}


void
foreign_personality_fault(Thread* thread, int number, int32 code,
	int32 error, void* address, debug_exception_type exception)
{
	// A record for diagnosing foreign programs: their crashes do not start
	// the debug server. Capped, since some programs (JITs, garbage
	// collectors) fault on purpose and handle the signal themselves.
	static int32 sFaultsLogged = 0;
	if (atomic_add(&sFaultsLogged, 1) < 64) {
		dprintf("foreign ABI fault: thread %" B_PRId32 " (team %" B_PRId32
			", %s): signal %d code %" B_PRId32 ", address %p\n", thread->id,
			thread->team->id, thread->team->Name(), number, code, address);
		foreign_personality_log_user_stack("foreign ABI fault:");
#ifdef __x86_64__
		// The instruction pointer, and the word at the stack pointer: after
		// a call through a null function pointer the instruction pointer
		// is 0 and that word is the caller's return address, which names
		// the call site (image and offset, for objdump on the host).
		iframe* frame = x86_get_user_iframe();
		if (frame != NULL) {
			addr_t returnAddress = 0;
			if (!IS_USER_ADDRESS(frame->user_sp)
				|| user_memcpy(&returnAddress, (void*)frame->user_sp,
					sizeof(returnAddress)) != B_OK) {
				returnAddress = 0;
			}
			char ip[128];
			char caller[128];
			describe_fault_address(thread->team->id, frame->ip, ip,
				sizeof(ip));
			describe_fault_address(thread->team->id, returnAddress, caller,
				sizeof(caller));
			dprintf("foreign ABI fault: thread %" B_PRId32 " ip %s, sp %#lx,"
				" [sp] %s\n", thread->id, ip, (addr_t)frame->user_sp, caller);
		}
#endif
	}

	// An installed debugger hears of the fault as for a native thread,
	// and may ignore it (the debugger then handles it, e.g. by changing
	// the thread's state). No debugger is started for a foreign program.
	if ((atomic_get(&thread->team->debug_info.flags)
			& B_TEAM_DEBUG_DEBUGGER_INSTALLED) != 0
		&& !user_debug_exception_occurred(exception, number)) {
		return;
	}

	foreign_personality_fault_signal(thread, number, code, error, address);
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


int
foreign_personality_native_proc_open(const char* path, int openMode)
{
	// Renga: native programs reach the personality's /proc generator here
	// (Chromium R-1, R-2, R-6). Personality modules stay loaded once
	// scanned, so the pointers copied under the lock remain valid.
	const foreign_personality_module_info* personalities[kMaxPersonalities];
	int32 count;
	{
		MutexLocker locker(sLock);
		if (sLastScan < 0)
			scan_personality_modules_locked();
		count = sPersonalityCount;
		memcpy(personalities, sPersonalities, count * sizeof(personalities[0]));
	}

	for (int32 i = 0; i < count; i++) {
		if (personalities[i]->native_proc_open == NULL)
			continue;
		int result = personalities[i]->native_proc_open(path, openMode);
		if (result != B_ENTRY_NOT_FOUND)
			return result;
	}
	return B_ENTRY_NOT_FOUND;
}
