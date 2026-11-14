/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 *
 * Linux event and memory descriptors on Renga's own implementations
 * (roadmap F-27: "map onto Renga's own kernel facilities instead of
 * emulating them in the module").
 *
 * Renga's kernel has epoll, eventfd, timerfd, signalfd, inotify and memfd
 * file sealing with Linux semantics and, for epoll_event, inotify_event,
 * signalfd_siginfo and the eventfd/timerfd read formats, Linux's own
 * layouts (src/system/kernel/events, vfs.cpp; F-15, F-43 phase A). So a
 * Linux program's descriptors of these kinds are Renga's: they work with
 * poll(), select(), each other and native programs they are passed to, and
 * nothing is emulated. What is translated here:
 *
 *  - creation flags: Linux's O_CLOEXEC/O_NONBLOCK values (EPOLL_CLOEXEC,
 *    EFD_*, TFD_*, SFD_*, IN_* are those) to Haiku's;
 *  - signal sets (epoll_pwait, signalfd): Linux signal numbers to Haiku's;
 *  - clocks (timerfd_create): Linux clock IDs to Haiku's;
 *  - paths (inotify_add_watch): looked up in the Linux root;
 *  - reads from a signalfd: ssi_signo (and ssi_status for SIGCHLD with a
 *    signal) back to Linux numbers. Such descriptors are remembered (by
 *    reference, as net.cpp remembers netlink sockets) so read() can tell.
 *  - memfd_create(): what libgnu's memfd_create() does for native programs
 *    (an unlinked file on the shared memory ramfs, made sealable through
 *    the private F_RENGA_SET_SEALABLE command), here in the kernel;
 *    F_ADD_SEALS/F_GET_SEALS in fcntl() are passed through (the seal bits
 *    are Linux's).
 */


#include "renguin.h"
#include "linux_defs.h"

#include <stdio.h>
#include <string.h>

#include <KernelExport.h>

#include <AutoDeleter.h>

#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <time.h>
#include <unistd.h>

#include <epoll.h>
#include <eventfd.h>
#include <inotify.h>
#include <signalfd.h>
#include <timerfd.h>

#include <file_seals_defs.h>
#include <fs/fd.h>
#include <lock.h>
#include <util/AutoLock.h>
#include <syscalls.h>
#include <team.h>
#include <thread.h>
#include <vfs.h>


#define LX_EFD_SEMAPHORE		1
#define LX_TFD_TIMER_ABSTIME	1
#define LX_TFD_TIMER_CANCEL_ON_SET 2

#define LX_EPOLL_CTL_ADD		1
#define LX_EPOLL_CTL_DEL		2
#define LX_EPOLL_CTL_MOD		3

#define LX_MFD_CLOEXEC			0x0001U
#define LX_MFD_ALLOW_SEALING	0x0002U
#define LX_MFD_HUGETLB			0x0004U
#define LX_MFD_NOEXEC_SEAL		0x0008U
#define LX_MFD_EXEC				0x0010U

#define LX_CLOCK_REALTIME_		0
#define LX_CLOCK_MONOTONIC_		1
#define LX_CLOCK_BOOTTIME_		7
#define LX_CLOCK_REALTIME_ALARM	8
#define LX_CLOCK_BOOTTIME_ALARM	9

#define LX_SIGCHLD				17


static int
creation_flags(uint64 flags, uint64 allowed)
{
	if ((flags & ~allowed) != 0)
		return -1;
	int result = 0;
	if ((flags & LX_O_CLOEXEC) != 0)
		result |= O_CLOEXEC;
	if ((flags & LX_O_NONBLOCK) != 0)
		result |= O_NONBLOCK;
	return result;
}


static int64
mask_in(uint64 userMask, uint64 size, sigset_t* _mask)
{
	if (size != sizeof(uint64))
		return -LX_EINVAL;
	uint64 linuxMask;
	if (renguin_copy_in(&linuxMask, userMask, sizeof(linuxMask)) != B_OK)
		return -LX_EFAULT;
	sigset_t mask = 0;
	for (int signal = 1; signal <= 64; signal++) {
		if ((linuxMask & ((uint64)1 << (signal - 1))) == 0)
			continue;
		int haikuSignal = renguin_signal_to_haiku(signal);
		if (haikuSignal != 0)
			mask |= SIGNAL_TO_MASK(haikuSignal);
	}
	*_mask = mask;
	return 0;
}


// #pragma mark - remembered descriptors


struct TaggedDescriptor {
	file_descriptor*	descriptor;
	int					tag;
};

enum {
	TAG_SIGNALFD = 1
};

static const int kMaxTagged = 128;
static TaggedDescriptor sTagged[kMaxTagged];
static mutex sTaggedLock = MUTEX_INITIALIZER("renguin fd tags");
static int32 sTaggedCount = 0;


static void
tag_descriptor(int fd, int tag)
{
	file_descriptor* descriptor = get_fd(get_current_io_context(false), fd);
	if (descriptor == NULL)
		return;

	MutexLocker locker(sTaggedLock);
	int freeSlot = -1;
	for (int i = 0; i < kMaxTagged; i++) {
		TaggedDescriptor& entry = sTagged[i];
		if (entry.descriptor != NULL
			&& atomic_get(&entry.descriptor->open_count) <= 0) {
			put_fd(entry.descriptor);
			entry.descriptor = NULL;
			atomic_add(&sTaggedCount, -1);
		}
		if (entry.descriptor == descriptor) {
			put_fd(descriptor);
			return;
		}
		if (entry.descriptor == NULL && freeSlot < 0)
			freeSlot = i;
	}
	if (freeSlot < 0) {
		put_fd(descriptor);
		return;
	}
	sTagged[freeSlot].descriptor = descriptor;
		// keeps get_fd()'s reference
	sTagged[freeSlot].tag = tag;
	atomic_add(&sTaggedCount, 1);
}


static int
descriptor_tag(int fd)
{
	if (atomic_get(&sTaggedCount) == 0)
		return 0;
	file_descriptor* descriptor = get_fd(get_current_io_context(false), fd);
	if (descriptor == NULL)
		return 0;
	int tag = 0;
	mutex_lock(&sTaggedLock);
	for (int i = 0; i < kMaxTagged; i++) {
		if (sTagged[i].descriptor == descriptor) {
			tag = sTagged[i].tag;
			break;
		}
	}
	mutex_unlock(&sTaggedLock);
	put_fd(descriptor);
	return tag;
}


// #pragma mark - epoll


int64
renguin_sys_epoll_create1(uint64 flags)
{
	int haikuFlags = creation_flags(flags, LX_O_CLOEXEC);
	if (haikuFlags < 0)
		return -LX_EINVAL;
	return renguin_encode(_user_epoll_create(haikuFlags));
}


int64
renguin_sys_epoll_create(int64 size)
{
	if (size <= 0)
		return -LX_EINVAL;
	return renguin_encode(_user_epoll_create(0));
}


int64
renguin_sys_epoll_ctl(int epfd, int64 op, int fd, uint64 event)
{
	// The operations and struct epoll_event (packed) are Linux's.
	if (op != LX_EPOLL_CTL_ADD && op != LX_EPOLL_CTL_DEL
		&& op != LX_EPOLL_CTL_MOD) {
		return -LX_EINVAL;
	}
	return renguin_encode(_user_epoll_ctl(epfd, (int)op, fd,
		(const struct epoll_event*)(addr_t)event));
}


static int64
epoll_wait_common(int epfd, uint64 events, int64 maxEvents,
	bigtime_t timeout, uint64 userMask, uint64 maskSize)
{
	sigset_t mask = 0;
	bool haveMask = userMask != 0;
	if (haveMask) {
		int64 result = mask_in(userMask, maskSize, &mask);
		if (result != 0)
			return result;
	}

	// The mask is installed the way ppoll() does it here (signal.cpp), so
	// that Linux-only signals stay blocked and a handler runs with the
	// program's mask.
	uint64 linuxMask = 0;
	if (haveMask && renguin_copy_in(&linuxMask, userMask, sizeof(linuxMask))
			!= B_OK) {
		return -LX_EFAULT;
	}
	sigset_t oldMask = 0;
	if (haveMask)
		oldMask = renguin_push_signal_mask(linuxMask);
	ssize_t result = _user_epoll_wait(epfd,
		(struct epoll_event*)(addr_t)events, (int)maxEvents, timeout, NULL);
	if (haveMask)
		renguin_pop_signal_mask(oldMask, result == B_INTERRUPTED);
	(void)mask;

	if (result == B_INTERRUPTED)
		return -LX_EINTR;
		// Linux never restarts epoll_wait() (it returns EINTR even with
		// SA_RESTART)
	return renguin_encode(result);
}


int64
renguin_sys_epoll_pwait(int epfd, uint64 events, int64 maxEvents,
	int64 timeoutMs, uint64 mask, uint64 maskSize)
{
	if (maxEvents <= 0 || maxEvents > INT_MAX / 12)
		return -LX_EINVAL;
	bigtime_t timeout = (int32)timeoutMs < 0 ? -1
		: (bigtime_t)(int32)timeoutMs * 1000;
	return epoll_wait_common(epfd, events, maxEvents, timeout, mask,
		maskSize);
}


int64
renguin_sys_epoll_pwait2(int epfd, uint64 events, int64 maxEvents,
	uint64 userTimeout, uint64 mask, uint64 maskSize)
{
	if (maxEvents <= 0 || maxEvents > INT_MAX / 12)
		return -LX_EINVAL;
	bigtime_t timeout = -1;
	if (userTimeout != 0) {
		int64 spec[2];
		if (renguin_copy_in(spec, userTimeout, sizeof(spec)) != B_OK)
			return -LX_EFAULT;
		if (spec[0] < 0 || spec[1] < 0 || spec[1] >= 1000000000)
			return -LX_EINVAL;
		timeout = spec[0] * 1000000 + (spec[1] + 999) / 1000;
	}
	return epoll_wait_common(epfd, events, maxEvents, timeout, mask,
		maskSize);
}


// #pragma mark - eventfd, timerfd


int64
renguin_sys_eventfd2(uint64 initialValue, uint64 flags)
{
	int haikuFlags = creation_flags(flags & ~(uint64)LX_EFD_SEMAPHORE,
		LX_O_CLOEXEC | LX_O_NONBLOCK);
	if (haikuFlags < 0)
		return -LX_EINVAL;
	return renguin_encode(_user_eventfd((uint32)initialValue, haikuFlags,
		(flags & LX_EFD_SEMAPHORE) != 0));
}


int64
renguin_sys_timerfd_create(int64 clock, uint64 flags)
{
	int haikuFlags = creation_flags(flags, LX_O_CLOEXEC | LX_O_NONBLOCK);
	if (haikuFlags < 0)
		return -LX_EINVAL;
	clockid_t haikuClock;
	switch (clock) {
		case LX_CLOCK_REALTIME_:
		case LX_CLOCK_REALTIME_ALARM:
			haikuClock = CLOCK_REALTIME;
			break;
		case LX_CLOCK_MONOTONIC_:
		case LX_CLOCK_BOOTTIME_:
		case LX_CLOCK_BOOTTIME_ALARM:
			// Haiku's monotonic clock counts suspended time too.
			haikuClock = CLOCK_MONOTONIC;
			break;
		default:
			return -LX_EINVAL;
	}
	return renguin_encode(_user_timerfd_create(haikuClock, haikuFlags));
}


int64
renguin_sys_timerfd_settime(int fd, uint64 flags, uint64 newValue,
	uint64 oldValue)
{
	if ((flags & ~(uint64)(LX_TFD_TIMER_ABSTIME | LX_TFD_TIMER_CANCEL_ON_SET))
			!= 0) {
		return -LX_EINVAL;
	}
	// struct itimerspec is two struct timespec on both systems. A clock
	// change never cancels the timer (TFD_TIMER_CANCEL_ON_SET is accepted).
	return renguin_encode(_user_timerfd_settime(fd,
		(flags & LX_TFD_TIMER_ABSTIME) != 0,
		(const struct itimerspec*)(addr_t)newValue,
		(struct itimerspec*)(addr_t)oldValue));
}


int64
renguin_sys_timerfd_gettime(int fd, uint64 value)
{
	return renguin_encode(_user_timerfd_gettime(fd,
		(struct itimerspec*)(addr_t)value));
}


// #pragma mark - signalfd


int64
renguin_sys_signalfd4(int fd, uint64 userMask, uint64 maskSize, uint64 flags)
{
	int haikuFlags = creation_flags(flags, LX_O_CLOEXEC | LX_O_NONBLOCK);
	if (haikuFlags < 0)
		return -LX_EINVAL;
	sigset_t mask;
	int64 result = mask_in(userMask, maskSize, &mask);
	if (result != 0)
		return result;

	renguin_scratch scratch;
	renguin_scratch_init(&scratch);
	uint64 user = renguin_scratch_put(&scratch, &mask, sizeof(mask));
	if (user == 0)
		return -LX_EFAULT;
	int newFD = _user_signalfd(fd, (const sigset_t*)(addr_t)user, haikuFlags);
	if (newFD >= 0)
		tag_descriptor(newFD, TAG_SIGNALFD);
	return renguin_encode(newFD);
}


bool
renguin_is_signalfd(int fd)
{
	return descriptor_tag(fd) == TAG_SIGNALFD;
}


/*!	read() from a signalfd: the records are Linux's struct
	signalfd_siginfo (128 bytes) with Haiku signal numbers; those are
	translated in the program's buffer after the read.
*/
int64
renguin_signalfd_read(int fd, uint64 buffer, uint64 count)
{
	ssize_t bytes = _user_read(fd, -1, (void*)(addr_t)buffer, count);
	if (bytes <= 0)
		return renguin_encode(bytes);

	const size_t kRecordSize = 128;
	for (ssize_t offset = 0; offset + (ssize_t)kRecordSize <= bytes;
			offset += kRecordSize) {
		uint32 fields[11];
			// ssi_signo ... ssi_status (offsets 0-43)
		if (renguin_copy_in(fields, buffer + offset, sizeof(fields)) != B_OK)
			return -LX_EFAULT;
		int haikuSignal = fields[0];
		int linuxSignal = renguin_signal_from_haiku(haikuSignal);
		fields[0] = linuxSignal != 0 ? linuxSignal : haikuSignal;
		if (fields[0] == LX_SIGCHLD) {
			// CLD_KILLED/CLD_DUMPED carry the signal in ssi_status.
			int32 code = (int32)fields[2];
			if (code == CLD_KILLED || code == CLD_DUMPED) {
				int statusSignal = renguin_signal_from_haiku(fields[10]);
				if (statusSignal != 0)
					fields[10] = statusSignal;
			}
		}
		// si_code: Haiku numbers each signal's codes from multiples of ten
		// (CLD_EXITED = 60, ...); Linux from 1.
		int32 code = (int32)fields[2];
		if (code >= 10 && code < 80 && code % 10 <= 7)
			fields[2] = code % 10 + 1;
		else if (code == SI_USER)
			fields[2] = 0;
		else if (code == SI_QUEUE)
			fields[2] = (uint32)-1;
		else if (code == SI_TIMER)
			fields[2] = (uint32)-2;
		if (renguin_copy_out(buffer + offset, fields, sizeof(fields)) != B_OK)
			return -LX_EFAULT;
	}
	return bytes;
}


// #pragma mark - inotify


int64
renguin_sys_inotify_init1(uint64 flags)
{
	int haikuFlags = creation_flags(flags, LX_O_CLOEXEC | LX_O_NONBLOCK);
	if (haikuFlags < 0)
		return -LX_EINVAL;
	return renguin_encode(_user_inotify_init(haikuFlags));
}


int64
renguin_sys_inotify_add_watch(int fd, uint64 path, uint64 mask)
{
	renguin_scratch scratch;
	renguin_scratch_init(&scratch);
	status_t status;
	uint64 userPath = renguin_user_path(&scratch, path,
		(mask & 0x02000000 /* IN_DONT_FOLLOW */) != 0
			? 0 : RENGUIN_PATH_FOLLOW_LEAF, &status);
	if (status != B_OK && status != RENGUIN_STATUS_IN_PROC)
		return renguin_encode(status);
	if (status == RENGUIN_STATUS_IN_PROC)
		return -LX_EACCES;
	// The event bits are Linux's.
	return renguin_encode(_user_inotify_add_watch(fd,
		(const char*)(addr_t)userPath, (uint32)mask));
}


int64
renguin_sys_inotify_rm_watch(int fd, int64 watch)
{
	return renguin_encode(_user_inotify_rm_watch(fd, (int)watch));
}


// #pragma mark - memfd


int64
renguin_sys_memfd_create(uint64 userName, uint64 flags)
{
	char name[256];
	ssize_t length = renguin_copy_string_in(name, userName, sizeof(name));
	if (length < 0)
		return length == B_NAME_TOO_LONG ? -LX_EINVAL : -LX_EFAULT;
	if (length > 249)
		return -LX_EINVAL;
	if ((flags & ~(uint64)(LX_MFD_CLOEXEC | LX_MFD_ALLOW_SEALING
			| LX_MFD_NOEXEC_SEAL | LX_MFD_EXEC)) != 0
		|| (flags & (LX_MFD_NOEXEC_SEAL | LX_MFD_EXEC))
			== (LX_MFD_NOEXEC_SEAL | LX_MFD_EXEC)) {
		return -LX_EINVAL;
	}

	// The same file libgnu's memfd_create() makes (src/libs/gnu/memfd.cpp):
	// unique, owner-only, on the shared memory ramfs, unlinked at once.
	char tag[32];
	size_t tagLength = 0;
	for (const char* c = name; *c != '\0' && tagLength < sizeof(tag) - 1;
			c++) {
		bool safe = (*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z')
			|| (*c >= '0' && *c <= '9') || *c == '-' || *c == '_'
			|| *c == '.';
		tag[tagLength++] = safe ? *c : '_';
	}
	tag[tagLength] = '\0';

	static int32 sCounter = 0;
	team_id team = thread_get_current_thread()->team->id;
	int openMode = O_RDWR | O_CREAT | O_EXCL;
	if ((flags & LX_MFD_CLOEXEC) != 0)
		openMode |= O_CLOEXEC;

	for (int attempt = 0; attempt < 16; attempt++) {
		char path[B_PATH_NAME_LENGTH];
		snprintf(path, sizeof(path), "/var/shared_memory/memfd-%" B_PRId32
			"-%" B_PRId32 "-%" B_PRIx64 "-%s", team, atomic_add(&sCounter, 1),
			(uint64)system_time(), tag);

		renguin_scratch scratch;
		renguin_scratch_init(&scratch);
		uint64 userPath = renguin_scratch_put(&scratch, path,
			strlen(path) + 1);
		if (userPath == 0)
			return -LX_EFAULT;
		int fd = _user_open(-1, (const char*)(addr_t)userPath, openMode,
			0600);
		if (fd == B_FILE_EXISTS)
			continue;
		if (fd < 0)
			return fd == B_ENTRY_NOT_FOUND ? -LX_ENOSYS : renguin_encode(fd);
		_user_unlink(-1, (const char*)(addr_t)userPath);

		uint32 seals = 0;
		if ((flags & (LX_MFD_ALLOW_SEALING | LX_MFD_NOEXEC_SEAL)) == 0)
			seals |= F_SEAL_SEAL;
		if ((flags & LX_MFD_NOEXEC_SEAL) != 0)
			seals |= F_SEAL_EXEC;
		status_t status = _user_fcntl(fd, F_RENGA_SET_SEALABLE, seals);
		if (status < 0) {
			_user_close(fd);
			return renguin_encode(status);
		}
		return fd;
	}
	return -LX_EEXIST;
}


int64
renguin_fcntl_seals(int fd, bool add, uint64 seals)
{
	// The seal bits are Linux's (fcntl.h).
	if (add) {
		return renguin_encode(_user_fcntl(fd, F_ADD_SEALS,
			(size_t)(uint32)seals));
	}
	return renguin_encode(_user_fcntl(fd, F_GET_SEALS, 0));
}
