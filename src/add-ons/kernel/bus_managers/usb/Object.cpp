/*
 * Copyright 2006-2020, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Michael Lotz <mmlr@mlotz.ch>
 */

#include "usb_private.h"


Object::Object(Stack *stack, BusManager *bus)
	:	fParent(NULL),
		fBusManager(bus),
		fStack(stack),
		fUSBID(fStack->GetUSBID(this))
{
}


Object::Object(Object *parent)
	:	fParent(parent),
		fBusManager(parent->GetBusManager()),
		fStack(parent->GetStack()),
		fUSBID(fStack->GetUSBID(this))
{
}


Object::~Object()
{
	PutUSBID();
}


void
Object::PutUSBID(bool waitForIdle)
{
	if (fUSBID != UINT32_MAX) {
		fStack->PutUSBID(this);
		fUSBID = UINT32_MAX;
	}

	if (waitForIdle)
		WaitForIdle();
}


/*!	Waits until only the caller's reference is left: the users that looked
	the object up before PutUSBID() (a driver's callback queuing the next
	transfer on another CPU, say) let go of it.

	Renga (roadmap F-61, the xhci isochronous pipe panic in usb_audio_test
	with two CPUs): this waited 20 times 100 us, 2 ms in all, and then
	panicked. A user on another CPU can be held up longer than that (an
	interrupt, a higher-priority thread; in QEMU a virtual CPU the host did
	not run for a while). And the old check was off by one: an object that
	became idle exactly at the last retry left the counter at 0 and
	panicked too. Now it waits up to two seconds, in 1 ms steps after the
	first millisecond, and panics only if the object is really still in
	use then.
*/
void
Object::WaitForIdle()
{
	bigtime_t start = system_time();
	while (CountReferences() != 1) {
		bigtime_t waited = system_time() - start;
		if (waited > 2000000)
			break;
		snooze(waited < 1000 ? 100 : 1000);
	}
	if (CountReferences() != 1)
		panic("USB object did not become idle! @! calling -m usb");
}


status_t
Object::SetFeature(uint16 selector)
{
	// to be implemented in subclasses
	TRACE_ERROR("set feature called\n");
	return B_ERROR;
}


status_t
Object::ClearFeature(uint16 selector)
{
	// to be implemented in subclasses
	TRACE_ERROR("clear feature called\n");
	return B_ERROR;
}


status_t
Object::GetStatus(uint16 *status)
{
	// to be implemented in subclasses
	TRACE_ERROR("get status called\n");
	return B_ERROR;
}
