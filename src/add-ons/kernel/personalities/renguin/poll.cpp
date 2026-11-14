/*
 * Copyright 2026, Renguin contributors.
 * Distributed under the terms of the MIT License.
 *
 * poll, ppoll, select and pselect6.
 *
 * FreeBSD counterpart: linux_poll(), linux_ppoll(), linux_select() and
 * linux_pselect6() in sys/compat/linux/linux_misc.c, which translate and call
 * kern_poll()/kern_select(). Here they wrap Haiku's _user_poll() and
 * _user_select() (src/system/kernel/events/wait_for_objects.cpp):
 *
 * - struct pollfd has the same layout, but the event bits differ (Haiku's
 *   match its B_SELECT_* numbers), so the array is translated through user
 *   scratch space.
 * - fd_set: Linux uses 64-bit words and Haiku 32-bit ones, with the same
 *   bit for each descriptor; on a little-endian machine the bytes are the
 *   same, so the program's sets are passed straight through.
 * - Timeouts: Linux writes the time left back for ppoll, select and
 *   pselect6 (not for poll). After a signal without a handler the call is
 *   restarted with the deadline it started with, as Linux's
 *   ERESTARTNOHAND/restart_block do.
 * - ppoll/pselect6 signal masks: the mask is installed for the wait the
 *   way Haiku's sigsuspend() does it, so a handler for a signal the mask
 *   lets through runs before the original mask returns (signal.cpp).
 *   Haiku's own sigMask argument is not used: its old-mask handling does
 *   not go through Renguin's signal frames.
 */


#include "renguin.h"
#include "linux_defs.h"

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include <KernelExport.h>
#include <OS.h>

#include <poll.h>
#include <sys/select.h>

#include <kernel.h>
#include <syscalls.h>
#include <thread.h>
#include <vfs.h>


// Linux poll bits (include/uapi/asm-generic/poll.h)
#define LX_POLLIN		0x0001
#define LX_POLLPRI		0x0002
#define LX_POLLOUT		0x0004
#define LX_POLLERR		0x0008
#define LX_POLLHUP		0x0010
#define LX_POLLNVAL		0x0020
#define LX_POLLRDNORM	0x0040
#define LX_POLLRDBAND	0x0080
#define LX_POLLWRNORM	0x0100
#define LX_POLLWRBAND	0x0200
#define LX_POLLRDHUP	0x2000

static_assert(sizeof(linux_pollfd) == sizeof(struct pollfd), "layout");

static const uint64 kMaxPollFDs = 64 * 1024 / sizeof(struct pollfd);


static short
events_to_haiku(short events)
{
	short result = 0;
	if ((events & (LX_POLLIN | LX_POLLRDNORM)) != 0)
		result |= POLLIN;
	if ((events & (LX_POLLOUT | LX_POLLWRNORM)) != 0)
		result |= POLLOUT;
	if ((events & LX_POLLPRI) != 0)
		result |= POLLPRI;
	if ((events & LX_POLLRDBAND) != 0)
		result |= POLLRDBAND;
	if ((events & LX_POLLWRBAND) != 0)
		result |= POLLWRBAND;
	return result;
}


//! Haiku revents back to Linux, limited to what was asked for (plus the
//! bits Linux always reports).
static short
revents_from_haiku(short revents, short requested)
{
	short result = 0;
	if ((revents & POLLIN) != 0)
		result |= requested & (LX_POLLIN | LX_POLLRDNORM);
	if ((revents & POLLOUT) != 0)
		result |= requested & (LX_POLLOUT | LX_POLLWRNORM);
	if ((revents & POLLPRI) != 0)
		result |= requested & LX_POLLPRI;
	if ((revents & POLLRDBAND) != 0)
		result |= requested & LX_POLLRDBAND;
	if ((revents & POLLWRBAND) != 0)
		result |= requested & LX_POLLWRBAND;
	if ((revents & POLLERR) != 0)
		result |= LX_POLLERR;
	if ((revents & POLLHUP) != 0)
		result |= LX_POLLHUP | (requested & LX_POLLRDHUP);
	if ((revents & POLLNVAL) != 0)
		result |= LX_POLLNVAL;
	return result;
}


// #pragma mark - deadlines


/*!	The absolute deadline of this call: the one it started with if it is a
	restart, else now + \a relative (B_INFINITE_TIMEOUT: none).
*/
static bigtime_t
call_deadline(bigtime_t relative)
{
	bigtime_t deadline;
	if (renguin_restarted_deadline(&deadline))
		return deadline;
	if (relative == B_INFINITE_TIMEOUT)
		return B_INFINITE_TIMEOUT;
	bigtime_t now = system_time();
	if (relative >= B_INFINITE_TIMEOUT - now)
		return B_INFINITE_TIMEOUT;
	return now + relative;
}


//! The relative timeout Haiku's calls take: -1 for none.
static bigtime_t
time_left(bigtime_t deadline)
{
	if (deadline == B_INFINITE_TIMEOUT)
		return -1;
	bigtime_t left = deadline - system_time();
	return left > 0 ? left : 0;
}


static bool
timespec_to_relative(const linux_timespec& time, bigtime_t* _relative)
{
	if (time.tv_sec < 0 || time.tv_nsec < 0 || time.tv_nsec >= 1000000000)
		return false;
	if (time.tv_sec >= B_INFINITE_TIMEOUT / 1000000 - 1)
		*_relative = B_INFINITE_TIMEOUT;
	else
		*_relative = time.tv_sec * 1000000 + (time.tv_nsec + 999) / 1000;
	return true;
}


//! Writes the time left back, as Linux does for ppoll/select/pselect6.
static void
write_time_left(uint64 address, bigtime_t deadline, bool timeval)
{
	if (address == 0 || deadline == B_INFINITE_TIMEOUT)
		return;
	bigtime_t left = deadline - system_time();
	if (left < 0)
		left = 0;
	if (timeval) {
		linux_timeval value = { left / 1000000, left % 1000000 };
		renguin_copy_out(address, &value, sizeof(value));
	} else {
		linux_timespec value = { left / 1000000, (left % 1000000) * 1000 };
		renguin_copy_out(address, &value, sizeof(value));
	}
		// Linux ignores a failure here too, once the call has done its work
}


// #pragma mark - poll


static int64
do_poll(uint64 fds, uint64 count, bigtime_t deadline, bool haveMask,
	uint64 linuxMask)
{
	if (count > kMaxPollFDs)
		return -LX_EINVAL;

	linux_pollfd* linuxFDs = NULL;
	if (count > 0) {
		linuxFDs = (linux_pollfd*)malloc(count * sizeof(linux_pollfd));
		if (linuxFDs == NULL)
			return -LX_ENOMEM;
		if (renguin_copy_in(linuxFDs, fds, count * sizeof(linux_pollfd))
				!= B_OK) {
			free(linuxFDs);
			return -LX_EFAULT;
		}
	}

	renguin_scratch scratch;
	renguin_scratch_init(&scratch);
	uint64 userFDs = 0;
	if (count > 0) {
		struct pollfd* haikuFDs = (struct pollfd*)malloc(
			count * sizeof(struct pollfd));
		if (haikuFDs == NULL) {
			free(linuxFDs);
			return -LX_ENOMEM;
		}
		for (uint64 i = 0; i < count; i++) {
			haikuFDs[i].fd = linuxFDs[i].fd;
			haikuFDs[i].events = events_to_haiku(linuxFDs[i].events);
			haikuFDs[i].revents = 0;
		}
		userFDs = renguin_scratch_put(&scratch, haikuFDs,
			count * sizeof(struct pollfd));
		free(haikuFDs);
		if (userFDs == 0) {
			free(linuxFDs);
			return -LX_ENOMEM;
		}
	}

	sigset_t oldMask = 0;
	if (haveMask)
		oldMask = renguin_push_signal_mask(linuxMask);

	ssize_t result = _user_poll((struct pollfd*)(addr_t)userFDs, (int)count,
		time_left(deadline), NULL);

	if (haveMask)
		renguin_pop_signal_mask(oldMask, result == B_INTERRUPTED);

	if (result >= 0 && count > 0) {
		struct pollfd* haikuFDs = (struct pollfd*)malloc(
			count * sizeof(struct pollfd));
		if (haikuFDs == NULL
			|| user_memcpy(haikuFDs, (void*)(addr_t)userFDs,
				count * sizeof(struct pollfd)) != B_OK) {
			free(haikuFDs);
			free(linuxFDs);
			return haikuFDs == NULL ? -LX_ENOMEM : -LX_EFAULT;
		}
		int64 ready = 0;
		for (uint64 i = 0; i < count; i++) {
			linuxFDs[i].revents = linuxFDs[i].fd < 0 ? 0
				: revents_from_haiku(haikuFDs[i].revents, linuxFDs[i].events);
			if (linuxFDs[i].revents != 0)
				ready++;
		}
		free(haikuFDs);
		// Only revents is written, as Linux does: the program may change
		// fd and events meanwhile (another thread).
		for (uint64 i = 0; i < count; i++) {
			if (renguin_copy_out(fds + i * sizeof(linux_pollfd)
						+ offsetof(linux_pollfd, revents),
					&linuxFDs[i].revents, sizeof(int16)) != B_OK) {
				free(linuxFDs);
				return -LX_EFAULT;
			}
		}
		result = ready;
	}
	free(linuxFDs);

	if (result == B_INTERRUPTED) {
		renguin_set_restart(RENGUIN_RESTART_NOHAND, &deadline);
		return -LX_EINTR;
	}
	return result < 0 ? renguin_encode(result) : result;
}


int64
renguin_sys_poll(uint64 fds, uint64 count, int64 timeoutMillis)
{
	int32 millis = (int32)timeoutMillis;
	bigtime_t relative = millis < 0 ? B_INFINITE_TIMEOUT
		: (bigtime_t)millis * 1000;
	return do_poll(fds, count, call_deadline(relative), false, 0);
}


static int64
read_sigmask(uint64 mask, uint64 size, bool* _haveMask, uint64* _linuxMask)
{
	*_haveMask = false;
	if (mask == 0)
		return 0;
	if (size != sizeof(uint64))
		return -LX_EINVAL;
	if (renguin_copy_in(_linuxMask, mask, sizeof(uint64)) != B_OK)
		return -LX_EFAULT;
	*_haveMask = true;
	return 0;
}


int64
renguin_sys_ppoll(uint64 fds, uint64 count, uint64 timeout, uint64 mask,
	uint64 maskSize)
{
	bigtime_t relative = B_INFINITE_TIMEOUT;
	if (timeout != 0) {
		linux_timespec time;
		if (renguin_copy_in(&time, timeout, sizeof(time)) != B_OK)
			return -LX_EFAULT;
		if (!timespec_to_relative(time, &relative))
			return -LX_EINVAL;
	}
	bool haveMask;
	uint64 linuxMask = 0;
	int64 status = read_sigmask(mask, maskSize, &haveMask, &linuxMask);
	if (status != 0)
		return status;

	bigtime_t deadline = call_deadline(relative);
	int64 result = do_poll(fds, count, deadline, haveMask, linuxMask);
	write_time_left(timeout, deadline, false);
	return result;
}


// #pragma mark - select


static int64
do_select(int64 count, uint64 readSet, uint64 writeSet, uint64 errorSet,
	bigtime_t deadline, bool haveMask, uint64 linuxMask)
{
	if (count < 0 || count > INT32_MAX)
		return -LX_EINVAL;
	// Haiku reads whole 32-bit words; Linux whole 64-bit ones. Check the
	// Linux extent so a set that ends on a page boundary faults as on Linux.
	size_t bytes = (count + 63) / 64 * 8;
	if ((readSet != 0 && !IS_USER_ADDRESS((addr_t)(readSet + bytes - 1)))
		|| (writeSet != 0
			&& !IS_USER_ADDRESS((addr_t)(writeSet + bytes - 1)))
		|| (errorSet != 0
			&& !IS_USER_ADDRESS((addr_t)(errorSet + bytes - 1)))) {
		return -LX_EFAULT;
	}

	sigset_t oldMask = 0;
	if (haveMask)
		oldMask = renguin_push_signal_mask(linuxMask);

	ssize_t result = _user_select((int)count, (fd_set*)(addr_t)readSet,
		(fd_set*)(addr_t)writeSet, (fd_set*)(addr_t)errorSet,
		time_left(deadline), NULL);

	if (haveMask)
		renguin_pop_signal_mask(oldMask, result == B_INTERRUPTED);

	if (result == B_INTERRUPTED) {
		renguin_set_restart(RENGUIN_RESTART_NOHAND, &deadline);
		return -LX_EINTR;
	}
	return result < 0 ? renguin_encode(result) : result;
}


int64
renguin_sys_select(int64 count, uint64 readSet, uint64 writeSet,
	uint64 errorSet, uint64 timeout)
{
	bigtime_t relative = B_INFINITE_TIMEOUT;
	if (timeout != 0) {
		linux_timeval time;
		if (renguin_copy_in(&time, timeout, sizeof(time)) != B_OK)
			return -LX_EFAULT;
		if (time.tv_sec < 0 || time.tv_usec < 0)
			return -LX_EINVAL;
		// Linux normalizes tv_usec >= 1000000 instead of failing.
		if (time.tv_sec >= B_INFINITE_TIMEOUT / 1000000 - 1
			|| time.tv_usec >= B_INFINITE_TIMEOUT / 2)
			relative = B_INFINITE_TIMEOUT;
		else
			relative = time.tv_sec * 1000000 + time.tv_usec;
	}
	bigtime_t deadline = call_deadline(relative);
	int64 result = do_select((int32)count, readSet, writeSet, errorSet,
		deadline, false, 0);
	write_time_left(timeout, deadline, true);
	return result;
}


int64
renguin_sys_pselect6(int64 count, uint64 readSet, uint64 writeSet,
	uint64 errorSet, uint64 timeout, uint64 maskData)
{
	bigtime_t relative = B_INFINITE_TIMEOUT;
	if (timeout != 0) {
		linux_timespec time;
		if (renguin_copy_in(&time, timeout, sizeof(time)) != B_OK)
			return -LX_EFAULT;
		if (!timespec_to_relative(time, &relative))
			return -LX_EINVAL;
	}

	// The sixth argument points to { const sigset_t* ss; size_t ss_len; }.
	bool haveMask = false;
	uint64 linuxMask = 0;
	if (maskData != 0) {
		uint64 data[2];
		if (renguin_copy_in(data, maskData, sizeof(data)) != B_OK)
			return -LX_EFAULT;
		int64 status = read_sigmask(data[0], data[1], &haveMask, &linuxMask);
		if (status != 0)
			return status;
	}

	bigtime_t deadline = call_deadline(relative);
	int64 result = do_select((int32)count, readSet, writeSet, errorSet,
		deadline, haveMask, linuxMask);
	write_time_left(timeout, deadline, false);
	return result;
}
