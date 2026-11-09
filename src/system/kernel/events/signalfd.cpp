/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 */

/*!	signalfd: pending signals read through a file descriptor, as on Linux
	(signalfd(2)). The user API lives in libgnu
	(headers/compatibility/gnu/sys/signalfd.h); this is the kernel object
	behind it.

	Semantics, as on Linux:
	 - read() dequeues pending signals of the descriptor's mask for the
	   *calling* thread (signals sent to it or to its process), like
	   sigwaitinfo(), and returns one 128-byte struct signalfd_siginfo per
	   signal, as many as fit; it blocks while none is pending (EAGAIN with
	   O_NONBLOCK). A buffer smaller than one record fails with EINVAL.
	 - The descriptor is readable while such a signal is pending for the
	   thread that selects it (poll(), select(), or the thread that added
	   or last probed it in an epoll set).
	 - signalfd() on an existing signalfd replaces its mask. SIGKILL and
	   SIGSTOP are silently left out of masks.
	 - After fork(), the child reads its own signals through the inherited
	   descriptor.

	How it works:
	 - Reading uses the kernel's sigwait implementation
	   (signal_dequeue_for_signalfd()), so the signal leaves the pending
	   set exactly as for sigwaitinfo(), and a blocking read is woken by
	   the signal itself.
	 - Waking select()ors: signal.cpp calls signalfd_signal_queued()
	   whenever a signal becomes pending. That runs with the team's signal
	   spinlock held and interrupts disabled, where select pools may not be
	   touched, so it only queues the DPC of every signalfd whose mask
	   contains the signal. The DPC checks, for every registered select
	   entry, whether a signal of the mask is pending for the thread that
	   registered it, and notifies only those; so poll() does not report a
	   signal pending for some other thread or team. This keeps its own list
	   of select entries (with the selecting thread) instead of a
	   select_sync_pool for that reason.
	 - A DPC runs later, though: raise() followed by epoll_wait(..., 0)
	   would miss the signal (the harness caught that with 4 CPUs). So the
	   signal path also marks the signalfd, and _user_send_signal() (kill(),
	   raise(), pthread_kill(), sigqueue()) runs the same notification
	   synchronously before returning, through
	   signalfd_flush_notifications(). The DPC still covers signals sent
	   from kernel code (SIGCHLD, timers), where no such point exists.
*/


#include <fcntl.h>
#include <new>
#include <string.h>
#include <sys/stat.h>

#include <AutoDeleter.h>
#include <AutoDeleterDrivers.h>
#include <DPC.h>
#include <fs/fd.h>
#include <kernel.h>
#include <ksignal.h>
#include <lock.h>
#include <signalfd.h>
#include <syscalls.h>
#include <thread.h>
#include <util/AutoLock.h>
#include <util/DoublyLinkedList.h>
#include <vfs.h>
#include <wait_for_objects.h>

// The user ABI is the Linux one, defined once in the libgnu header; see
// epoll.cpp for why it is included by path.
#include "../../../../headers/compatibility/gnu/sys/signalfd.h"


static_assert(sizeof(struct signalfd_siginfo) == 128,
	"struct signalfd_siginfo must have the Linux size");


using BKernel::DPCCallback;
using BKernel::DPCQueue;


int32 gSignalFDCount = 0;


struct SignalFDSelect : DoublyLinkedListLinkImpl<SignalFDSelect> {
	selectsync*		sync;
	uint8			event;
	thread_id		thread;
};

typedef DoublyLinkedList<SignalFDSelect> SignalFDSelectList;


struct SignalFD : DPCCallback {
	DoublyLinkedListLink<SignalFD> listLink;
		// in sSignalFDs (sSignalFDListLock)
	int32				referenceCount;
		// the descriptor's, plus signalfd_flush_notifications() while
		// it works on it
	int32				flushPending;
		// set by the signal path, cleared by the flush
	mutex				lock;
		// mask writes, select list
	sigset_t			mask;
		// read without the lock by the signal path
	SignalFDSelectList	selects;

	SignalFD(sigset_t mask)
		:
		referenceCount(1),
		flushPending(0),
		mask(mask)
	{
		mutex_init(&lock, "signalfd");
	}

	~SignalFD()
	{
		while (SignalFDSelect* entry = selects.RemoveHead())
			delete entry;
		mutex_destroy(&lock);
	}

	virtual void DoDPC(DPCQueue* queue);

	void ReleaseReference();

	void NotifyReady()
	{
		// lock held
		sigset_t currentMask = mask;
		for (SignalFDSelectList::Iterator it = selects.GetIterator();
				it.HasNext();) {
			SignalFDSelect* entry = it.Next();
			if (entry->event == B_SELECT_READ
				&& signal_pending_for_signalfd(entry->thread, currentMask)) {
				notify_select_event(entry->sync, entry->event);
			}
		}
	}
};

typedef DoublyLinkedList<SignalFD,
	DoublyLinkedListMemberGetLink<SignalFD, &SignalFD::listLink> > SignalFDList;


static spinlock sSignalFDListLock = B_SPINLOCK_INITIALIZER;
static SignalFDList sSignalFDs;
static int32 sFlushNeeded = 0;


static DPCQueue*
signalfd_dpc_queue()
{
	return DPCQueue::DefaultQueue(B_URGENT_DISPLAY_PRIORITY);
}


// Out of line: inlined into signalfd_free(), GCC 13 misreads the
// atomic on the cookie as a write past a zero-sized object.
__attribute__((noinline)) void
SignalFD::ReleaseReference()
{
	if (atomic_add(&referenceCount, -1) == 1)
		delete this;
}


void
SignalFD::DoDPC(DPCQueue* queue)
{
	MutexLocker locker(lock);
	NotifyReady();
}


/*!	A signal became pending somewhere (signal spinlock held, interrupts
	disabled): queue the DPC of every signalfd interested in it.
*/
void
signalfd_signal_queued_slow(uint32 signal)
{
	if (signal == 0 || signal > MAX_SIGNAL_NUMBER)
		return;
	sigset_t bit = SIGNAL_TO_MASK(signal);

	SpinLocker locker(sSignalFDListLock);
	for (SignalFDList::Iterator it = sSignalFDs.GetIterator(); it.HasNext();) {
		SignalFD* signalFD = it.Next();
		if ((atomic_get64((int64*)&signalFD->mask) & bit) != 0
			&& !signalFD->selects.IsEmpty()) {
			// EALREADY (already queued) is fine. The DPC covers signals
			// sent from kernel code; signals sent by a syscall are flushed
			// by signalfd_flush_notifications() before it returns.
			atomic_set(&signalFD->flushPending, 1);
			atomic_set(&sFlushNeeded, 1);
			signalfd_dpc_queue()->Add(signalFD);
		}
	}
}


void
signalfd_flush_notifications()
{
	if (atomic_get(&sFlushNeeded) == 0 || atomic_get_and_set(&sFlushNeeded, 0)
			== 0) {
		return;
	}

	while (true) {
		SignalFD* signalFD = NULL;
		{
			InterruptsSpinLocker locker(sSignalFDListLock);
			for (SignalFDList::Iterator it = sSignalFDs.GetIterator();
					it.HasNext();) {
				SignalFD* candidate = it.Next();
				if (atomic_get_and_set(&candidate->flushPending, 0) != 0) {
					signalFD = candidate;
					atomic_add(&signalFD->referenceCount, 1);
					break;
				}
			}
		}
		if (signalFD == NULL)
			return;

		{
			MutexLocker locker(signalFD->lock);
			signalFD->NotifyReady();
		}
		signalFD->ReleaseReference();
	}
}


static sigset_t
usable_mask(sigset_t mask)
{
	// As Linux: SIGKILL and SIGSTOP cannot be read, and are left out.
	return mask & ~(SIGNAL_TO_MASK(SIGKILL) | SIGNAL_TO_MASK(SIGSTOP))
		& ~(sigset_t)0 >> (64 - MAX_SIGNAL_NUMBER);
}


// #pragma mark - fd hooks


static status_t
signalfd_close(file_descriptor* descriptor)
{
	return B_OK;
}


static void
signalfd_free(file_descriptor* descriptor)
{
	SignalFD* signalFD = (SignalFD*)descriptor->cookie;

	{
		InterruptsSpinLocker locker(sSignalFDListLock);
		sSignalFDs.Remove(signalFD);
	}
	atomic_add(&gSignalFDCount, -1);

	// Waits for a DPC that is running right now; a flush in progress holds
	// its own reference.
	signalfd_dpc_queue()->Cancel(signalFD);
	signalFD->ReleaseReference();
}


static status_t
signalfd_read(file_descriptor* descriptor, off_t pos, void* buffer,
	size_t* _length)
{
	if (*_length < sizeof(struct signalfd_siginfo))
		return B_BAD_VALUE;

	SignalFD* signalFD = (SignalFD*)descriptor->cookie;
	sigset_t mask = atomic_get64((int64*)&signalFD->mask);
	size_t count = *_length / sizeof(struct signalfd_siginfo);
	bool wait = (descriptor->open_mode & O_NONBLOCK) == 0;

	size_t done = 0;
	while (done < count) {
		siginfo_t info;
		status_t status = signal_dequeue_for_signalfd(&mask, &info,
			wait && done == 0);
		if (status != B_OK) {
			if (done > 0)
				break;
			return status;
		}

		struct signalfd_siginfo record;
		memset(&record, 0, sizeof(record));
		record.ssi_signo = info.si_signo;
		record.ssi_errno = info.si_errno;
		record.ssi_code = info.si_code;
		record.ssi_pid = info.si_pid;
		record.ssi_uid = info.si_uid;
		record.ssi_band = info.si_band;
		record.ssi_status = info.si_status;
		record.ssi_int = info.si_value.sival_int;
		record.ssi_ptr = (uint64)(addr_t)info.si_value.sival_ptr;
		record.ssi_addr = (uint64)(addr_t)info.si_addr;

		if (user_memcpy((uint8*)buffer + done * sizeof(record), &record,
				sizeof(record)) != B_OK) {
			// The signal is consumed already; like Linux, report the
			// fault (EFAULT) only if nothing was copied.
			if (done > 0)
				break;
			return B_BAD_ADDRESS;
		}
		done++;
	}

	*_length = done * sizeof(struct signalfd_siginfo);
	return B_OK;
}


static status_t
signalfd_write(file_descriptor* descriptor, off_t pos, const void* buffer,
	size_t* _length)
{
	return B_BAD_VALUE;
}


static status_t
signalfd_set_flags(file_descriptor* descriptor, int flags)
{
	return B_OK;
}


static status_t
signalfd_select(file_descriptor* descriptor, uint8 event, selectsync* sync)
{
	SignalFD* signalFD = (SignalFD*)descriptor->cookie;

	SignalFDSelect* entry = new(std::nothrow) SignalFDSelect;
	if (entry == NULL)
		return B_NO_MEMORY;
	entry->sync = sync;
	entry->event = event;
	entry->thread = thread_get_current_thread_id();

	MutexLocker locker(signalFD->lock);
	signalFD->selects.Add(entry);

	// Readable right away if a signal of the mask is pending for us.
	if (event == B_SELECT_READ
		&& signal_pending_for_signalfd(entry->thread, signalFD->mask)) {
		return notify_select_event(sync, event);
	}
	return B_OK;
}


static status_t
signalfd_deselect(file_descriptor* descriptor, uint8 event, selectsync* sync)
{
	SignalFD* signalFD = (SignalFD*)descriptor->cookie;

	MutexLocker locker(signalFD->lock);
	for (SignalFDSelectList::Iterator it = signalFD->selects.GetIterator();
			it.HasNext();) {
		SignalFDSelect* entry = it.Next();
		if (entry->sync == sync && entry->event == event) {
			it.Remove();
			delete entry;
			break;
		}
	}
	return B_OK;
}


static status_t
signalfd_read_stat(file_descriptor* descriptor, struct stat* stat)
{
	memset(stat, 0, sizeof(*stat));
	stat->st_mode = S_IFIFO | S_IRUSR;
	stat->st_nlink = 1;
	stat->st_blksize = sizeof(struct signalfd_siginfo);
	return B_OK;
}


static struct fd_ops sSignalFDOps = {
	&signalfd_close,
	&signalfd_free,
	&signalfd_read,
	&signalfd_write,
	NULL,	// readv
	NULL,	// writev
	NULL,	// seek
	NULL,	// ioctl
	&signalfd_set_flags,
	&signalfd_select,
	&signalfd_deselect,
	NULL,	// read_dir
	NULL,	// rewind_dir
	&signalfd_read_stat,
	NULL,	// write_stat
};


// #pragma mark - syscall


/*!	Creates a signalfd (\a fd == -1) or changes the mask of an existing one.
	\a openFlags may contain O_CLOEXEC, O_CLOFORK and O_NONBLOCK; they only
	apply to a new descriptor (libgnu translates the SFD_* flags).
*/
int
_user_signalfd(int fd, const sigset_t* userMask, int openFlags)
{
	if ((openFlags & ~(O_CLOEXEC | O_CLOFORK | O_NONBLOCK)) != 0)
		return B_BAD_VALUE;

	sigset_t mask;
	if (userMask == NULL || !IS_USER_ADDRESS(userMask)
		|| user_memcpy(&mask, userMask, sizeof(mask)) != B_OK) {
		return B_BAD_ADDRESS;
	}
	mask = usable_mask(mask);

	if (fd != -1) {
		FileDescriptorPutter descriptor(get_fd(get_current_io_context(false),
			fd));
		if (!descriptor.IsSet())
			return B_FILE_ERROR;
		if (descriptor->ops != &sSignalFDOps)
			return B_BAD_VALUE;

		SignalFD* signalFD = (SignalFD*)descriptor->cookie;
		MutexLocker locker(signalFD->lock);
		atomic_set64((int64*)&signalFD->mask, mask);
		// The new mask may make it readable for a selector.
		signalFD->NotifyReady();
		return fd;
	}

	SignalFD* signalFD = new(std::nothrow) SignalFD(mask);
	if (signalFD == NULL)
		return B_NO_MEMORY;

	file_descriptor* descriptor = alloc_fd();
	if (descriptor == NULL) {
		delete signalFD;
		return B_NO_MEMORY;
	}

	descriptor->ops = &sSignalFDOps;
	descriptor->cookie = signalFD;
	// O_RDWR, so that write() reaches our hook and fails with EINVAL, as
	// on Linux, rather than with EBADF.
	descriptor->open_mode = O_RDWR | (openFlags & O_NONBLOCK);

	io_context* context = get_current_io_context(false);
	rw_lock_write_lock(&context->lock);
	int newFD = new_fd(context, descriptor);
	if (newFD < 0) {
		rw_lock_write_unlock(&context->lock);
		descriptor->ops = NULL;
		descriptor->cookie = NULL;
		put_fd(descriptor);
		delete signalFD;
		return newFD;
	}

	fd_set_close_on_exec(context, newFD, (openFlags & O_CLOEXEC) != 0);
	fd_set_close_on_fork(context, newFD, (openFlags & O_CLOFORK) != 0);
	rw_lock_write_unlock(&context->lock);

	{
		InterruptsSpinLocker locker(sSignalFDListLock);
		sSignalFDs.Add(signalFD);
	}
	atomic_add(&gSignalFDCount, 1);

	return newFD;
}
