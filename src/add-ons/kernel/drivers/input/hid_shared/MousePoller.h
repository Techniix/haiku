/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 */
#ifndef MOUSE_POLLER_H
#define MOUSE_POLLER_H


/*!	Renga: a relative mouse read the way Linux's evdev reads one (pointer
	latency fix, 2026-11-14).

	A USB mouse sends a report only when the host polls it. Haiku's usb_hid
	scheduled the interrupt transfer only while a reader waited in MS_READ,
	and input_server's mouse add-on reads every 8 ms (125 times a second).
	A mouse reporting at 1000 Hz or faster was then polled 125 times a
	second: what it could not send meanwhile stayed in the mouse (queued
	reports, or motion carried over beyond one report's range), and came
	out after the hand had stopped: the pointer kept moving and "caught up"
	(the maintainer's Darmoshark M3 at 1000 Hz, image
	2026.11.14-chromium-wayland).

	MousePoller keeps polling while the node is open: its thread takes one
	report after another at the device's own rate (for usb_hid: one
	interrupt transfer queued at all times, as Linux's usbhid does) and
	adds each to a MouseFrameQueue; MS_READ returns the oldest frame, and
	MS_NUM_EVENTS says how many wait, so the add-on merges what is there.

	MouseFrameQueue sums reports like evdev frames: motion with the same
	buttons is added to the newest frame; a report that presses or
	releases a button, or turns the wheel, starts a frame of its own and
	nothing is merged into it, so clicks keep their order and land where
	the motion before them ended (the add-on's rule as well).

	Header-only: used by usb_hid (MouseProtocolHandler) and the harness's
	ce_vmouse, which emulates a USB mouse with it.
*/


#include <string.h>

#include <KernelExport.h>
#include <OS.h>
#include <keyboard_mouse_driver.h>

#include <condition_variable.h>
#include <lock.h>
#include <util/AutoLock.h>


class MouseFrameQueue {
public:
	static const uint32 kFrames = 64;

	void Init(const char* name)
	{
		mutex_init(&fLock, name);
		fReadable.Init(this, name);
		Reset();
	}

	void Uninit()
	{
		mutex_destroy(&fLock);
	}

	void Reset()
	{
		fHead = fCount = 0;
		fLastButtons = 0;
		fClosedStatus = B_OK;
	}

	//! Adds one report: summed into the newest frame when it is only motion
	void Add(const mouse_movement& movement)
	{
		MutexLocker locker(fLock);
		if (fCount > 0) {
			uint32 last = (fHead + fCount - 1) % kFrames;
			mouse_movement& frame = fFrames[last];
			uint32 before = fCount > 1
				? fFrames[(fHead + fCount - 2) % kFrames].buttons
				: fLastButtons;
			bool frameIsMotion = frame.buttons == before
				&& frame.wheel_xdelta == 0 && frame.wheel_ydelta == 0;
			bool reportIsMotion = movement.buttons == frame.buttons
				&& movement.wheel_xdelta == 0 && movement.wheel_ydelta == 0;
			if ((frameIsMotion && reportIsMotion) || fCount == kFrames) {
				// (When the queue is full, a reader that has been gone for
				// 64 button changes gets the rest summed into the last
				// frame; its buttons are the newest.)
				frame.xdelta += movement.xdelta;
				frame.ydelta += movement.ydelta;
				frame.wheel_xdelta += movement.wheel_xdelta;
				frame.wheel_ydelta += movement.wheel_ydelta;
				frame.buttons = movement.buttons;
				frame.clicks = movement.clicks;
				frame.timestamp = movement.timestamp;
				fReadable.NotifyAll();
				return;
			}
		}
		fFrames[(fHead + fCount) % kFrames] = movement;
		fCount++;
		fReadable.NotifyAll();
	}

	//! Waits for a frame and takes it
	status_t Read(mouse_movement& movement)
	{
		MutexLocker locker(fLock);
		while (fCount == 0) {
			if (fClosedStatus != B_OK)
				return fClosedStatus;
			ConditionVariableEntry entry;
			fReadable.Add(&entry);
			locker.Unlock();
			status_t status = entry.Wait(B_CAN_INTERRUPT);
			locker.Lock();
			if (status == B_INTERRUPTED)
				return B_INTERRUPTED;
		}
		movement = fFrames[fHead];
		fHead = (fHead + 1) % kFrames;
		fCount--;
		fLastButtons = movement.buttons;
		return B_OK;
	}

	int32 Count()
	{
		MutexLocker locker(fLock);
		return fCount;
	}

	//! Wakes the readers with \a status (device gone, node closed)
	void Close(status_t status)
	{
		MutexLocker locker(fLock);
		fClosedStatus = status;
		fReadable.NotifyAll(status);
	}

private:
	mutex				fLock;
	ConditionVariable	fReadable;
	mouse_movement		fFrames[kFrames];
	uint32				fHead;
	uint32				fCount;
	uint32				fLastButtons;
	status_t			fClosedStatus;
};


/*!	The polling thread. \a poll takes the device's next report: it may block
	until there is one, but should return within about 100 ms (B_TIMED_OUT)
	so that Stop() is noticed; any other error than B_TIMED_OUT, B_BUSY or
	B_INTERRUPTED ends the polling (the device is gone) and is handed to the
	readers.
*/
class MousePoller {
public:
	typedef status_t (*poll_hook)(void* cookie, mouse_movement& movement);

	void Init(const char* name, poll_hook poll, void* cookie)
	{
		fQueue.Init(name);
		fName = name;
		fPoll = poll;
		fCookie = cookie;
		fThread = -1;
		fStop = 0;
	}

	void Uninit()
	{
		Stop();
		fQueue.Uninit();
	}

	status_t Start()
	{
		if (fThread >= 0)
			return B_OK;
		fQueue.Reset();
		atomic_set(&fStop, 0);
		// above input_server's watcher's readers and everything that could
		// hold a poll up; each round does almost nothing
		fThread = spawn_kernel_thread(_Thread, fName,
			B_REAL_TIME_DISPLAY_PRIORITY, this);
		if (fThread < 0)
			return fThread;
		resume_thread(fThread);
		return B_OK;
	}

	void Stop()
	{
		if (fThread < 0)
			return;
		atomic_set(&fStop, 1);
		fQueue.Close(B_FILE_ERROR);
		status_t result;
		wait_for_thread(fThread, &result);
		fThread = -1;
	}

	bool IsRunning() const { return fThread >= 0; }

	status_t Read(mouse_movement& movement) { return fQueue.Read(movement); }
	int32 Count() { return fQueue.Count(); }

private:
	static status_t _Thread(void* data)
	{
		MousePoller* self = (MousePoller*)data;
		while (atomic_get(&self->fStop) == 0) {
			mouse_movement movement;
			memset(&movement, 0, sizeof(movement));
			status_t status = self->fPoll(self->fCookie, movement);
			if (status == B_OK) {
				self->fQueue.Add(movement);
				continue;
			}
			if (status == B_TIMED_OUT || status == B_BUSY
				|| status == B_INTERRUPTED) {
				continue;
			}
			self->fQueue.Close(status);
			break;
		}
		return B_OK;
	}

	MouseFrameQueue		fQueue;
	const char*			fName;
	poll_hook			fPoll;
	void*				fCookie;
	thread_id			fThread;
	int32				fStop;
};


#endif	// MOUSE_POLLER_H
