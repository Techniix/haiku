/*
 * Copyright 2026, Renguin contributors.
 * Distributed under the terms of the MIT License.
 *
 * POSIX per-process timers for Linux programs: timer_create, timer_settime,
 * timer_gettime, timer_getoverrun and timer_delete (x86-64 calls 222-226).
 *
 * They are Haiku's own user timers (UserTimer.cpp, the ones libroot's
 * timer_create() uses): a team timer for SIGEV_SIGNAL and SIGEV_NONE, and a
 * timer of the target thread for Linux's SIGEV_THREAD_ID, which is how glibc
 * and musl implement SIGEV_THREAD (a helper thread waits for the timer
 * signal; SIGEV_THREAD itself never reaches the kernel). Haiku already gives
 * the signal SI_TIMER and the sigevent's value, and counts overruns.
 *
 * Linux names timers with small per-process integers, Haiku with a timer ID
 * that is per team or per thread. renguin_team::timers maps one to the
 * other. As on Linux, a fork()ed child has no timers (Haiku does not copy
 * them, renguin_team_forked() clears the table) and exec removes them (the
 * team data is made anew, Haiku deletes the timers).
 *
 * FreeBSD counterpart: sys/compat/linux/linux_timer.c.
 */


#include "renguin.h"
#include "linux_defs.h"

#include <string.h>

#include <thread.h>

#include <UserTimer.h>
#include <user_timer_defs.h>
#include <util/AutoLock.h>


// Linux sigevent (include/uapi/asm-generic/siginfo.h), 64 bytes
struct linux_sigevent {
	uint64	sigev_value;
	int32	sigev_signo;
	int32	sigev_notify;
	union {
		int32	pad[12];
		int32	tid;			// SIGEV_THREAD_ID
		struct {
			uint64	function;
			uint64	attribute;
		} thread;
	} un;
};

#define LX_SIGEV_SIGNAL		0
#define LX_SIGEV_NONE		1
#define LX_SIGEV_THREAD		2
#define LX_SIGEV_THREAD_ID	4

#define LX_TIMER_ABSTIME	1

struct linux_itimerspec {
	linux_timespec	it_interval;
	linux_timespec	it_value;
};


/*!	The Haiku clock for a Linux one; false for the others (timers on
	Linux's CPU-time clocks of other processes, the encoded negative IDs,
	are not supported). Haiku's clock IDs are 0 and negative numbers
	(CLOCK_REALTIME is -1), so validity is returned apart.
*/
static bool
timer_clock(uint64 clockId, clockid_t* _clock)
{
	switch (clockId) {
		case LX_CLOCK_REALTIME:
		case LX_CLOCK_REALTIME_ALARM:
			*_clock = CLOCK_REALTIME;
			return true;
		case LX_CLOCK_MONOTONIC:
		case LX_CLOCK_BOOTTIME:
		case LX_CLOCK_BOOTTIME_ALARM:
			*_clock = CLOCK_MONOTONIC;
			return true;
		case LX_CLOCK_PROCESS_CPUTIME_ID:
			*_clock = CLOCK_PROCESS_CPUTIME_ID;
			return true;
		case LX_CLOCK_THREAD_CPUTIME_ID:
			*_clock = CLOCK_THREAD_CPUTIME_ID;
			return true;
	}
	return false;
}


//! A Linux timespec as microseconds, rounded up (a timer never fires early);
//! false when it is not a valid timespec.
static bool
micros_from_timespec(const linux_timespec& spec, bigtime_t* _micros)
{
	if (spec.tv_sec < 0 || spec.tv_nsec < 0 || spec.tv_nsec >= 1000000000)
		return false;
	if (spec.tv_sec > (B_INFINITE_TIMEOUT / 1000000) - 1)
		*_micros = B_INFINITE_TIMEOUT - 1;
	else
		*_micros = spec.tv_sec * 1000000 + (spec.tv_nsec + 999) / 1000;
	return true;
}


static void
timespec_from_micros(bigtime_t micros, linux_timespec* _spec)
{
	_spec->tv_sec = micros / 1000000;
	_spec->tv_nsec = (micros % 1000000) * 1000;
}


static void
itimerspec_from_info(const user_timer_info& info, linux_itimerspec* _spec)
{
	timespec_from_micros(info.interval, &_spec->it_interval);
	if (info.remaining_time == B_INFINITE_TIMEOUT)
		timespec_from_micros(0, &_spec->it_value);	// disarmed
	else {
		// an armed timer due now still reads as armed, as on Linux
		timespec_from_micros(info.remaining_time > 0
			? info.remaining_time : 1, &_spec->it_value);
	}
}


static renguin_team*
current_team_data()
{
	return (renguin_team*)thread_get_current_thread()->team->personality_data;
}


//! The table entry of Linux timer \a id, or NULL. Called with the lock held.
static renguin_timer_slot*
lookup_timer(renguin_team* data, uint64 id)
{
	if (id >= RENGUIN_MAX_TIMERS || data->timers[id].haiku_id <= 0)
		return NULL;
	return &data->timers[id];
}


int64
renguin_sys_timer_create(uint64 clockId, uint64 eventPointer,
	uint64 timerIdPointer)
{
	renguin_team* data = current_team_data();
	if (data == NULL)
		return -LX_EINVAL;
	clockid_t clock;
	if (!timer_clock(clockId, &clock))
		return -LX_EINVAL;

	linux_sigevent linuxEvent;
	if (eventPointer != 0) {
		if (renguin_copy_in(&linuxEvent, eventPointer, sizeof(linuxEvent))
				!= B_OK) {
			return -LX_EFAULT;
		}
	} else {
		// NULL: SIGALRM with the timer ID as value (set below), as Linux
		memset(&linuxEvent, 0, sizeof(linuxEvent));
		linuxEvent.sigev_notify = LX_SIGEV_SIGNAL;
		linuxEvent.sigev_signo = LX_SIGALRM;
	}

	struct sigevent event;
	memset(&event, 0, sizeof(event));
	thread_id thread = -1;
	switch (linuxEvent.sigev_notify) {
		case LX_SIGEV_NONE:
			event.sigev_notify = SIGEV_NONE;
			break;
		case LX_SIGEV_SIGNAL:
		case LX_SIGEV_THREAD_ID:
		{
			int signal = linuxEvent.sigev_signo;
			if (signal <= 0 || signal > RENGUIN_NSIG)
				return -LX_EINVAL;
			int haikuSignal = renguin_signal_to_haiku(signal);
			if (haikuSignal <= 0)
				return -LX_EINVAL;
			event.sigev_notify = SIGEV_SIGNAL;
			event.sigev_signo = haikuSignal;
			if (linuxEvent.sigev_notify == LX_SIGEV_THREAD_ID) {
				thread = linuxEvent.un.tid;
				Thread* target = Thread::Get(thread);
				if (target == NULL)
					return -LX_EINVAL;
				bool sameTeam = target->team == thread_get_current_thread()->team;
				target->ReleaseReference();
				if (!sameTeam)
					return -LX_EINVAL;
			}
			break;
		}
		default:
			// SIGEV_THREAD is done by the C library with SIGEV_THREAD_ID
			return -LX_EINVAL;
	}

	// Reserve a slot (haiku_id -1), so the lock is not held while the
	// timer is made (that takes the team's locks).
	MutexLocker locker(data->lock);
	uint32 slot = 0;
	while (slot < RENGUIN_MAX_TIMERS && data->timers[slot].haiku_id != 0)
		slot++;
	if (slot == RENGUIN_MAX_TIMERS)
		return -LX_EAGAIN;
	data->timers[slot].haiku_id = -1;
	locker.Unlock();

	event.sigev_value.sival_ptr = eventPointer != 0
		? (void*)(addr_t)linuxEvent.sigev_value : (void*)(addr_t)slot;

	// _user_create_timer() reads the sigevent from userland
	renguin_scratch scratch;
	renguin_scratch_init(&scratch);
	uint64 userEvent = renguin_scratch_put(&scratch, &event, sizeof(event));
	// SIGEV_THREAD_ID: a timer of the thread that also signals the thread
	int32 id = userEvent == 0 ? B_BAD_ADDRESS : _user_create_timer(clock,
		thread, thread >= 0 ? USER_TIMER_SIGNAL_THREAD : 0,
		(const struct sigevent*)(addr_t)userEvent, NULL);
	int32 linuxId = slot;
	if (id >= 0 && renguin_copy_out(timerIdPointer, &linuxId,
			sizeof(linuxId)) != B_OK) {
		_user_delete_timer(id, thread);
		id = B_BAD_ADDRESS;
	}

	locker.Lock();
	data->timers[slot].haiku_id = id >= 0 ? id : 0;
	data->timers[slot].thread = id >= 0 ? thread : -1;
	locker.Unlock();
	if (id == B_BAD_ADDRESS)
		return -LX_EFAULT;
	if (id < 0)
		return id == B_BAD_THREAD_ID ? -LX_EINVAL : renguin_encode(id);
	return 0;
}


int64
renguin_sys_timer_settime(uint64 timerId, uint64 flags, uint64 newValue,
	uint64 oldValue)
{
	renguin_team* data = current_team_data();
	if (data == NULL)
		return -LX_EINVAL;
	linux_itimerspec value;
	if (renguin_copy_in(&value, newValue, sizeof(value)) != B_OK)
		return -LX_EFAULT;
	bigtime_t start, interval;
	if (!micros_from_timespec(value.it_value, &start)
		|| !micros_from_timespec(value.it_interval, &interval)) {
		return -LX_EINVAL;
	}
	bool disarm = value.it_value.tv_sec == 0 && value.it_value.tv_nsec == 0;
	if (disarm)
		start = B_INFINITE_TIMEOUT;
	else if ((flags & LX_TIMER_ABSTIME) == 0 && start == 0)
		start = 1;

	MutexLocker locker(data->lock);
	renguin_timer_slot* timer = lookup_timer(data, timerId);
	if (timer == NULL)
		return -LX_EINVAL;
	int32 haikuId = timer->haiku_id;
	thread_id thread = timer->thread;
	locker.Unlock();

	renguin_scratch scratch;
	renguin_scratch_init(&scratch);
	uint64 userOld = 0;
	if (oldValue != 0) {
		userOld = renguin_scratch_alloc(&scratch, sizeof(user_timer_info));
		if (userOld == 0)
			return -LX_EFAULT;
	}
	status_t status = _user_set_timer(haikuId, thread, start, interval,
		(flags & LX_TIMER_ABSTIME) != 0 && !disarm
			? B_ABSOLUTE_TIMEOUT : B_RELATIVE_TIMEOUT,
		(user_timer_info*)(addr_t)userOld);
	if (status != B_OK)
		return status == B_BAD_THREAD_ID ? -LX_EINVAL : renguin_encode(status);
	if (oldValue != 0) {
		user_timer_info info;
		if (user_memcpy(&info, (void*)(addr_t)userOld, sizeof(info)) != B_OK)
			return -LX_EFAULT;
		linux_itimerspec old;
		itimerspec_from_info(info, &old);
		if (renguin_copy_out(oldValue, &old, sizeof(old)) != B_OK)
			return -LX_EFAULT;
	}
	return 0;
}


//! timer_gettime() (\a value set) and timer_getoverrun() (\a value 0).
static int64
get_timer(uint64 timerId, uint64 value, bool overrun)
{
	renguin_team* data = current_team_data();
	if (data == NULL)
		return -LX_EINVAL;
	MutexLocker locker(data->lock);
	renguin_timer_slot* timer = lookup_timer(data, timerId);
	if (timer == NULL)
		return -LX_EINVAL;
	int32 haikuId = timer->haiku_id;
	thread_id thread = timer->thread;
	locker.Unlock();

	renguin_scratch scratch;
	renguin_scratch_init(&scratch);
	uint64 userInfo = renguin_scratch_alloc(&scratch, sizeof(user_timer_info));
	if (userInfo == 0)
		return -LX_EFAULT;
	status_t status = _user_get_timer(haikuId, thread,
		(user_timer_info*)(addr_t)userInfo);
	if (status != B_OK)
		return status == B_BAD_THREAD_ID ? -LX_EINVAL : renguin_encode(status);
	user_timer_info info;
	if (user_memcpy(&info, (void*)(addr_t)userInfo, sizeof(info)) != B_OK)
		return -LX_EFAULT;
	if (overrun) {
		// Linux caps it at DELAYTIMER_MAX (INT_MAX)
		return info.overrun_count > 0x7fffffff ? 0x7fffffff
			: (int64)info.overrun_count;
	}
	linux_itimerspec spec;
	itimerspec_from_info(info, &spec);
	if (renguin_copy_out(value, &spec, sizeof(spec)) != B_OK)
		return -LX_EFAULT;
	return 0;
}


int64
renguin_sys_timer_gettime(uint64 timerId, uint64 value)
{
	return get_timer(timerId, value, false);
}


int64
renguin_sys_timer_getoverrun(uint64 timerId)
{
	return get_timer(timerId, 0, true);
}


int64
renguin_sys_timer_delete(uint64 timerId)
{
	renguin_team* data = current_team_data();
	if (data == NULL)
		return -LX_EINVAL;
	MutexLocker locker(data->lock);
	renguin_timer_slot* timer = lookup_timer(data, timerId);
	if (timer == NULL)
		return -LX_EINVAL;
	int32 haikuId = timer->haiku_id;
	thread_id thread = timer->thread;
	timer->haiku_id = 0;
	timer->thread = -1;
	locker.Unlock();

	// a thread timer whose thread has ended went away with it
	status_t status = _user_delete_timer(haikuId, thread);
	if (status != B_OK && status != B_BAD_THREAD_ID && thread < 0)
		return renguin_encode(status);
	return 0;
}


//! A fork()ed child starts without timers (Haiku does not copy them).
void
renguin_timers_init(renguin_team* data)
{
	for (uint32 i = 0; i < RENGUIN_MAX_TIMERS; i++) {
		data->timers[i].haiku_id = 0;
		data->timers[i].thread = -1;
	}
}
