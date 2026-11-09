/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 */

/*!	timerfd: a timer that is read through a file descriptor, as on Linux and
	FreeBSD 14+ (timerfd_create(2)). The user API lives in libgnu
	(headers/compatibility/gnu/sys/timerfd.h); this is the kernel object
	behind it.

	Semantics:
	 - read() needs an 8-byte buffer. It returns the number of expirations
	   since the last read (or since the timer was set) and resets it; it
	   blocks while there were none (EAGAIN with O_NONBLOCK).
	 - The descriptor is readable while expirations are pending.
	 - Setting the timer cancels the previous setting and discards pending
	   expirations. A zero it_value disarms it.
	 - Absolute CLOCK_REALTIME timers follow changes of the real-time clock;
	   relative timers are not affected by them.

	How it works:
	 - Expirations are derived from time, not from timer interrupts:
	   CatchUp() compares the clock with nextExpiry and adds every period
	   that has passed. read(), select() and timerfd_gettime() call it
	   first, so their answer is exact even if the wake-up below is late
	   (an early version counted only in the DPC and under-reported when
	   the DPC thread was delayed, which the harness caught on one CPU).
	 - The kernel timer exists only to wake up waiters on time. Its hook
	   runs in interrupt context, where neither the object's mutex nor the
	   select pool may be touched, so it just queues a DPC on the
	   high-priority DPC queue. The DPC takes the mutex, calls CatchUp(),
	   re-arms the kernel timer for the next period and wakes up readers
	   and select()/poll() waiters.
	 - Everything else runs under the object's mutex, and the kernel timer
	   is always (re)started for the current nextExpiry whenever that
	   changes, so a stale hook or DPC can at most cause a spurious,
	   harmless CatchUp().

	Event loops use timerfd for timeouts (libuv, systemd-style daemons,
	PipeWire, Wine), and a Linux compatibility layer needs it as a native
	object.
*/


#include <fcntl.h>
#include <new>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include <condition_variable.h>
#include <DPC.h>
#include <fs/fd.h>
#include <fs/select_sync_pool.h>
#include <kernel.h>
#include <lock.h>
#include <real_time_clock.h>
#include <syscalls.h>
#include <syscall_restart.h>
#include <timer.h>
#include <timerfd.h>
#include <util/AutoLock.h>
#include <vfs.h>
#include <wait_for_objects.h>


using BKernel::DPCCallback;
using BKernel::DPCQueue;


struct TimerFD : DPCCallback {
	// Protected by "lock":
	mutex				lock;
	ConditionVariable	condition;
	select_sync_pool*	selectPool;
	clockid_t			clock;
	bool				armed;
	bool				realTimeBase;	// absolute CLOCK_REALTIME timer
	bigtime_t			nextExpiry;		// in the timer's time base
	bigtime_t			interval;		// 0: one-shot
	uint64				expirations;	// not yet read
	bool				closing;

	::timer				kernelTimer;

	TimerFD(clockid_t clock)
		:
		selectPool(NULL),
		clock(clock),
		armed(false),
		realTimeBase(false),
		nextExpiry(0),
		interval(0),
		expirations(0),
		closing(false)
	{
		mutex_init(&lock, "timerfd");
		condition.Init(this, "timerfd");
		memset(&kernelTimer, 0, sizeof(kernelTimer));
		kernelTimer.user_data = this;
	}

	~TimerFD()
	{
		delete_select_sync_pool(selectPool);
		mutex_destroy(&lock);
	}

	//! The current time in this timer's time base.
	bigtime_t Now() const
	{
		return realTimeBase ? real_time_clock_usecs() : system_time();
	}

	virtual void DoDPC(DPCQueue* queue);

	bool CatchUp();
	status_t Arm();
	void Disarm();
};


static DPCQueue*
timerfd_dpc_queue()
{
	// Timer wake-ups are latency sensitive (event loops sleep on them), so
	// use the high-priority queue rather than the normal one.
	return DPCQueue::DefaultQueue(B_URGENT_DISPLAY_PRIORITY);
}


static int32
timerfd_hook(timer* kernelTimer)
{
	// Interrupt context: just hand over to the DPC. EALREADY (already
	// queued) is fine, the queued DPC will see this expiry as well.
	timerfd_dpc_queue()->Add((TimerFD*)kernelTimer->user_data);
	return B_HANDLED_INTERRUPT;
}


/*!	Adds the expirations that have happened by now and advances
	nextExpiry past the present. A one-shot timer becomes disarmed. Does not
	touch the kernel timer. Called with lock held. Returns whether any
	expirations were added.
*/
bool
TimerFD::CatchUp()
{
	if (!armed)
		return false;

	bigtime_t now = Now();
	if (now < nextExpiry)
		return false;

	if (interval > 0) {
		bigtime_t periods = (now - nextExpiry) / interval + 1;
		expirations += periods;
		nextExpiry += periods * interval;
	} else {
		expirations++;
		armed = false;
	}
	return true;
}


/*!	(Re)starts the kernel timer for nextExpiry, cancelling a pending one.
	Called with lock held.
*/
status_t
TimerFD::Arm()
{
	cancel_timer(&kernelTimer);

	uint32 flags = B_ONE_SHOT_ABSOLUTE_TIMER;
	if (realTimeBase)
		flags |= B_TIMER_REAL_TIME_BASE;

	status_t status = add_timer(&kernelTimer, &timerfd_hook, nextExpiry, flags);
	armed = status == B_OK;
	return status;
}


/*!	Stops the timer. Called with lock held. cancel_timer() waits for a hook
	running on another CPU; a DPC that hook queued finds armed == false and
	does nothing.
*/
void
TimerFD::Disarm()
{
	cancel_timer(&kernelTimer);
	armed = false;
}


void
TimerFD::DoDPC(DPCQueue* queue)
{
	MutexLocker locker(lock);
	if (closing)
		return;

	bool expired = CatchUp();

	if (armed) {
		// Periodic (or a spurious early DPC): wait for the next expiry.
		// One-shot timers that just expired were disarmed by CatchUp().
		Arm();
	} else
		cancel_timer(&kernelTimer);

	if (expired || expirations > 0) {
		condition.NotifyAll();
		notify_select_event_pool(selectPool, B_SELECT_READ);
	}
}


//	#pragma mark - file descriptor ops


static status_t
timerfd_close(file_descriptor* descriptor)
{
	return B_OK;
}


static void
timerfd_free(file_descriptor* descriptor)
{
	TimerFD* timerFD = (TimerFD*)descriptor->cookie;

	MutexLocker locker(timerFD->lock);
	timerFD->closing = true;
	timerFD->Disarm();
	locker.Unlock();

	// Waits for a DPC that is running right now; it sees "closing" and
	// returns without touching anything.
	timerfd_dpc_queue()->Cancel(timerFD);

	delete timerFD;
}


static status_t
timerfd_read(file_descriptor* descriptor, off_t pos, void* buffer,
	size_t* _length)
{
	if (*_length < sizeof(uint64))
		return B_BAD_VALUE;

	TimerFD* timerFD = (TimerFD*)descriptor->cookie;
	MutexLocker locker(timerFD->lock);

	timerFD->CatchUp();
	while (timerFD->expirations == 0) {
		if ((descriptor->open_mode & O_NONBLOCK) != 0)
			return B_WOULD_BLOCK;

		status_t status = timerFD->condition.Wait(&timerFD->lock,
			B_CAN_INTERRUPT);
		if (status != B_OK)
			return status;
		timerFD->CatchUp();
	}

	uint64 value = timerFD->expirations;
	if (user_memcpy(buffer, &value, sizeof(value)) != B_OK)
		return B_BAD_ADDRESS;

	timerFD->expirations = 0;
	*_length = sizeof(uint64);
	return B_OK;
}


static status_t
timerfd_set_flags(file_descriptor* descriptor, int flags)
{
	// O_NONBLOCK is read from descriptor->open_mode.
	return B_OK;
}


static status_t
timerfd_select(file_descriptor* descriptor, uint8 event, selectsync* sync)
{
	TimerFD* timerFD = (TimerFD*)descriptor->cookie;
	MutexLocker locker(timerFD->lock);

	status_t status = add_select_sync_pool_entry(&timerFD->selectPool, sync,
		event);
	if (status != B_OK)
		return status;

	timerFD->CatchUp();
	if (event == B_SELECT_READ && timerFD->expirations > 0)
		return notify_select_event(sync, event);

	return B_OK;
}


static status_t
timerfd_deselect(file_descriptor* descriptor, uint8 event, selectsync* sync)
{
	TimerFD* timerFD = (TimerFD*)descriptor->cookie;
	MutexLocker locker(timerFD->lock);

	remove_select_sync_pool_entry(&timerFD->selectPool, sync, event);
	return B_OK;
}


static status_t
timerfd_read_stat(file_descriptor* descriptor, struct stat* stat)
{
	memset(stat, 0, sizeof(*stat));
	stat->st_mode = S_IFIFO | S_IRUSR | S_IWUSR;
	stat->st_nlink = 1;
	stat->st_blksize = sizeof(uint64);
	return B_OK;
}


static struct fd_ops sTimerFDOps = {
	&timerfd_close,
	&timerfd_free,
	&timerfd_read,
	NULL,	// write (EINVAL, as on Linux)
	NULL,	// readv
	NULL,	// writev
	NULL,	// seek
	NULL,	// ioctl
	&timerfd_set_flags,
	&timerfd_select,
	&timerfd_deselect,
	NULL,	// read_dir
	NULL,	// rewind_dir
	&timerfd_read_stat,
	NULL,	// write_stat
};


//	#pragma mark - helpers


static status_t
get_timerfd(int fd, file_descriptor*& descriptor)
{
	descriptor = get_fd(get_current_io_context(false), fd);
	if (descriptor == NULL)
		return B_FILE_ERROR;

	if (descriptor->ops != &sTimerFDOps) {
		put_fd(descriptor);
		return B_BAD_VALUE;
	}
	return B_OK;
}


//!	Releases the reference get_timerfd() acquired.
struct FileDescriptorPutter {
	FileDescriptorPutter(file_descriptor* descriptor)
		: fDescriptor(descriptor) {}
	~FileDescriptorPutter() { put_fd(fDescriptor); }

private:
	file_descriptor* fDescriptor;
};


//!	Converts a timespec to microseconds, rounding up: never fire early.
static status_t
timespec_to_bigtime(const timespec& time, bigtime_t& _value)
{
	if (time.tv_sec < 0 || time.tv_nsec < 0 || time.tv_nsec >= 1000000000)
		return B_BAD_VALUE;
	if (time.tv_sec > (B_INFINITE_TIMEOUT / 1000000) - 1)
		return B_BAD_VALUE;

	_value = (bigtime_t)time.tv_sec * 1000000 + (time.tv_nsec + 999) / 1000;
	return B_OK;
}


static void
bigtime_to_timespec(bigtime_t value, timespec& _time)
{
	if (value < 0)
		value = 0;
	_time.tv_sec = value / 1000000;
	_time.tv_nsec = (value % 1000000) * 1000;
}


//!	The current setting, as timerfd_gettime() reports it. Lock held.
static void
get_setting(TimerFD* timerFD, itimerspec& setting)
{
	timerFD->CatchUp();

	bigtime_t remaining = 0;
	if (timerFD->armed) {
		remaining = timerFD->nextExpiry - timerFD->Now();
		if (remaining < 1) {
			// CatchUp() just moved nextExpiry past the present, but a
			// real-time clock reading can step: never report 0, which
			// would mean "disarmed".
			remaining = 1;
		}
	}
	bigtime_to_timespec(remaining, setting.it_value);
	bigtime_to_timespec(timerFD->interval, setting.it_interval);
}


//	#pragma mark - syscalls


int
_user_timerfd_create(clockid_t clock, int openFlags)
{
	if (clock != CLOCK_MONOTONIC && clock != CLOCK_REALTIME)
		return B_BAD_VALUE;
	if ((openFlags & ~(O_CLOEXEC | O_CLOFORK | O_NONBLOCK)) != 0)
		return B_BAD_VALUE;

	TimerFD* timerFD = new(std::nothrow) TimerFD(clock);
	if (timerFD == NULL)
		return B_NO_MEMORY;

	file_descriptor* descriptor = alloc_fd();
	if (descriptor == NULL) {
		delete timerFD;
		return B_NO_MEMORY;
	}

	descriptor->ops = &sTimerFDOps;
	descriptor->cookie = timerFD;
	descriptor->open_mode = O_RDWR | (openFlags & O_NONBLOCK);

	io_context* context = get_current_io_context(false);
	rw_lock_write_lock(&context->lock);
	int fd = new_fd(context, descriptor);
	if (fd < 0) {
		rw_lock_write_unlock(&context->lock);
		descriptor->ops = NULL;
		descriptor->cookie = NULL;
		put_fd(descriptor);
		delete timerFD;
		return fd;
	}

	fd_set_close_on_exec(context, fd, (openFlags & O_CLOEXEC) != 0);
	fd_set_close_on_fork(context, fd, (openFlags & O_CLOFORK) != 0);
	rw_lock_write_unlock(&context->lock);

	return fd;
}


status_t
_user_timerfd_settime(int fd, bool absolute, const itimerspec* userNewValue,
	itimerspec* userOldValue)
{
	itimerspec newValue;
	if (userNewValue == NULL || !IS_USER_ADDRESS(userNewValue)
		|| user_memcpy(&newValue, userNewValue, sizeof(newValue)) != B_OK) {
		return B_BAD_ADDRESS;
	}
	if (userOldValue != NULL && !IS_USER_ADDRESS(userOldValue))
		return B_BAD_ADDRESS;

	bigtime_t value, interval;
	status_t status = timespec_to_bigtime(newValue.it_value, value);
	if (status == B_OK)
		status = timespec_to_bigtime(newValue.it_interval, interval);
	if (status != B_OK)
		return status;

	file_descriptor* descriptor;
	status = get_timerfd(fd, descriptor);
	if (status != B_OK)
		return status;
	FileDescriptorPutter putter(descriptor);

	TimerFD* timerFD = (TimerFD*)descriptor->cookie;
	MutexLocker locker(timerFD->lock);

	itimerspec oldValue;
	get_setting(timerFD, oldValue);

	// A new setting replaces the old one and discards pending expirations.
	timerFD->Disarm();
	timerFD->expirations = 0;
	timerFD->interval = 0;

	if (value != 0) {
		timerFD->interval = interval;
		if (absolute) {
			timerFD->realTimeBase = timerFD->clock == CLOCK_REALTIME;
			timerFD->nextExpiry = value;
		} else {
			// Relative timers ignore real-time clock changes.
			timerFD->realTimeBase = false;
			timerFD->nextExpiry = system_time() + value;
		}
		status = timerFD->Arm();
	}
	locker.Unlock();

	if (status == B_OK && userOldValue != NULL
		&& user_memcpy(userOldValue, &oldValue, sizeof(oldValue)) != B_OK) {
		return B_BAD_ADDRESS;
	}
	return status;
}


status_t
_user_timerfd_gettime(int fd, itimerspec* userValue)
{
	if (userValue == NULL || !IS_USER_ADDRESS(userValue))
		return B_BAD_ADDRESS;

	file_descriptor* descriptor;
	status_t status = get_timerfd(fd, descriptor);
	if (status != B_OK)
		return status;
	FileDescriptorPutter putter(descriptor);

	TimerFD* timerFD = (TimerFD*)descriptor->cookie;
	itimerspec value;
	MutexLocker locker(timerFD->lock);
	get_setting(timerFD, value);
	locker.Unlock();

	return user_memcpy(userValue, &value, sizeof(value));
}
