/*
 * Copyright 2026, Haiku Community Edition contributors.
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
	 - The kernel timer hook runs in interrupt context, where neither the
	   object's mutex nor the select pool may be touched. It only counts the
	   fire under a spinlock and queues a DPC.
	 - The DPC runs in a kernel thread with the object's mutex held. It
	   turns fires into expirations, adds the periods that passed while it
	   was delayed, re-arms periodic timers, and wakes up readers and
	   select()/poll() waiters.
	 - timerfd_settime() holds the same mutex while it cancels the kernel
	   timer and clears the pending fires, so a DPC can never re-arm a
	   stale timer, and a DPC queued for a stale timer finds nothing to do.

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
	uint64				expirations;
	bool				closing;

	// Protected by "fireLock"; written by the timer hook.
	spinlock			fireLock;
	uint32				pendingFires;

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
		closing(false),
		pendingFires(0)
	{
		mutex_init(&lock, "timerfd");
		condition.Init(this, "timerfd");
		B_INITIALIZE_SPINLOCK(&fireLock);
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

	status_t Arm();
	void Disarm();
};


static int32
timerfd_hook(timer* kernelTimer)
{
	TimerFD* timerFD = (TimerFD*)kernelTimer->user_data;

	acquire_spinlock(&timerFD->fireLock);
	timerFD->pendingFires++;
	release_spinlock(&timerFD->fireLock);

	// EALREADY just means a DPC is queued and will see this fire, too.
	DPCQueue::DefaultQueue(B_NORMAL_PRIORITY)->Add(timerFD);
	return B_HANDLED_INTERRUPT;
}


//!	Starts the kernel timer for nextExpiry. Called with lock held.
status_t
TimerFD::Arm()
{
	uint32 flags = B_ONE_SHOT_ABSOLUTE_TIMER;
	if (realTimeBase)
		flags |= B_TIMER_REAL_TIME_BASE;

	status_t status = add_timer(&kernelTimer, &timerfd_hook, nextExpiry, flags);
	armed = status == B_OK;
	return status;
}


/*!	Stops the kernel timer and forgets fires the DPC has not processed yet.
	Called with lock held; afterwards the hook is not running and will not
	run again until the next Arm().
*/
void
TimerFD::Disarm()
{
	if (armed)
		cancel_timer(&kernelTimer);
	armed = false;

	InterruptsSpinLocker locker(fireLock);
	pendingFires = 0;
}


void
TimerFD::DoDPC(DPCQueue* queue)
{
	MutexLocker locker(lock);
	if (closing)
		return;

	InterruptsSpinLocker fireLocker(fireLock);
	uint32 fires = pendingFires;
	pendingFires = 0;
	fireLocker.Unlock();

	if (fires == 0)
		return;

	uint64 newExpirations = fires;
	if (interval > 0) {
		// Re-arm; count the periods that passed while this DPC was pending.
		bigtime_t next = nextExpiry + interval;
		bigtime_t now = Now();
		if (now >= next) {
			bigtime_t missed = (now - next) / interval + 1;
			newExpirations += missed;
			next += missed * interval;
		}
		nextExpiry = next;
		Arm();
	} else
		armed = false;

	expirations += newExpirations;
	condition.NotifyAll();
	notify_select_event_pool(selectPool, B_SELECT_READ);
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
	DPCQueue::DefaultQueue(B_NORMAL_PRIORITY)->Cancel(timerFD);

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

	while (timerFD->expirations == 0) {
		if ((descriptor->open_mode & O_NONBLOCK) != 0)
			return B_WOULD_BLOCK;

		status_t status = timerFD->condition.Wait(&timerFD->lock,
			B_CAN_INTERRUPT);
		if (status != B_OK)
			return status;
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
	bigtime_t remaining = 0;
	if (timerFD->armed) {
		remaining = timerFD->nextExpiry - timerFD->Now();
		if (remaining < 1) {
			// Fired, the DPC has just not run yet: report the shortest
			// possible time rather than 0, which would mean "disarmed".
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
