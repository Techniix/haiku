/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 *
 * Renga (roadmap F-42): raw HID access for game controllers, /dev/hidraw<N>,
 * with the Linux hidraw interface (<linux/hidraw.h>), for SDL's HIDAPI
 * drivers (Switch Pro and Joy-Con with gyro and HD rumble, DualSense with
 * touchpad, lightbar and adaptive triggers, 8BitDo pads in Switch mode).
 */
#ifndef HIDRAW_PROTOCOL_HANDLER_H
#define HIDRAW_PROTOCOL_HANDLER_H


#include <Select.h>

#include <condition_variable.h>
#include <lock.h>

#include "ProtocolHandler.h"


class HIDDevice;


class HidrawProtocolHandler : public ProtocolHandler {
public:
								HidrawProtocolHandler(HIDDevice& device,
									const uint8* descriptor,
									size_t descriptorLength,
									size_t maxReportSize);
	virtual						~HidrawProtocolHandler();

	// Access control: only game controllers get a hidraw node, never
	// keyboards, mice, tablets or other devices whose raw reports could
	// leak typing; and only while raw controller access is on (the
	// Gamepads preferences, kernel driver settings file "gamepad",
	// "raw_access false" to turn it off).
	static	bool				IsAllowed(HIDDevice& device, uint16 vendor);

	virtual	status_t			Open(uint32 flags, uint32* cookie);
	virtual	status_t			Close(uint32* cookie);
	virtual	status_t			Read(uint32* cookie, off_t position,
									void* buffer, size_t* numBytes);
	virtual	status_t			Write(uint32* cookie, off_t position,
									const void* buffer, size_t* numBytes);
	virtual	status_t			Control(uint32* cookie, uint32 op,
									void* buffer, size_t length);
	virtual	status_t			Select(uint32* cookie, uint8 event,
									selectsync* sync);
	virtual	status_t			Deselect(uint32* cookie, uint8 event,
									selectsync* sync);
	virtual	void				DeviceRemoved();

			// called by HIDDevice for every interrupt transfer
			void				RawReport(status_t status, const uint8* data,
									size_t length);
			bool				IsReading() const { return fOpenCount > 0; }

private:
			status_t			_SetReport(uint8 type, const uint8* data,
									size_t length);
			status_t			_GetReport(uint8 type, uint8* data,
									size_t length, size_t& actual);

	static	const uint32		kQueueLength = 32;

			HIDDevice&			fHIDDevice;
			uint8*				fDescriptor;
			size_t				fDescriptorLength;
			size_t				fSlotSize;
			uint8*				fQueue;
			size_t*				fQueueLengths;
			uint32				fHead;
			uint32				fCount;
			int32				fOpenCount;
			mutex				fLock;
			ConditionVariable	fReadable;
			selectsync*			fReadSync;
};


#endif	// HIDRAW_PROTOCOL_HANDLER_H
