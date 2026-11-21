/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 *
 * Renga (roadmap F-42): /dev/hidraw<N> for game controllers. Behaves like
 * Linux's hidraw:
 *  - read() returns one input report per call, with the report ID as its
 *    first byte when the device numbers its reports, as it came from the
 *    interrupt pipe; up to 32 reports are queued per device while it is
 *    open (the oldest is dropped when full);
 *  - write() sends an output report: the first byte is the report ID, 0 for
 *    devices without IDs (then it is not sent), through the interrupt OUT
 *    pipe when there is one, otherwise with SET_REPORT;
 *  - HIDIOCGRDESCSIZE, HIDIOCGRDESC, HIDIOCGRAWINFO, HIDIOCGRAWNAME,
 *    HIDIOCGRAWPHYS, HIDIOCGRAWUNIQ, HIDIOCSFEATURE, HIDIOCGFEATURE,
 *    HIDIOCSINPUT/HIDIOCGINPUT and HIDIOCSOUTPUT/HIDIOCGOUTPUT;
 *  - select()/poll() for reading.
 * The interrupt pipe is only kept busy while a descriptor is open.
 */


#include "Driver.h"
#include "HidrawProtocolHandler.h"

#include "HIDCollection.h"
#include "HIDDevice.h"
#include "HIDParser.h"
#include "GamepadEvdevBridge.h"
#include "HidrawPolicy.h"

#include <errno.h>
#include <new>
#include <stdlib.h>
#include <string.h>

#include <driver_settings.h>
#include <kernel.h>
#include <usb/USB_hid.h>
#include <util/AutoLock.h>

#include <linux/hidraw.h>
#include <linux/input.h>


#define COOKIE_NONBLOCKING	0x40000000


HidrawProtocolHandler::HidrawProtocolHandler(HIDDevice& device,
	const uint8* descriptor, size_t descriptorLength, size_t maxReportSize)
	:
	ProtocolHandler(&device, "hidraw", 0),
	fHIDDevice(device),
	fDescriptor(NULL),
	fDescriptorLength(0),
	fSlotSize(maxReportSize + 1),
	fQueue(NULL),
	fQueueLengths(NULL),
	fHead(0),
	fCount(0),
	fOpenCount(0),
	fReadSync(NULL)
{
	mutex_init(&fLock, "hidraw");
	fReadable.Init(this, "hidraw readable");

	if (fSlotSize < 64)
		fSlotSize = 64;
	fDescriptor = (uint8*)malloc(descriptorLength);
	fQueue = (uint8*)malloc(fSlotSize * kQueueLength);
	fQueueLengths = (size_t*)calloc(kQueueLength, sizeof(size_t));
	if (fDescriptor == NULL || fQueue == NULL || fQueueLengths == NULL) {
		fStatus = B_NO_MEMORY;
		return;
	}
	memcpy(fDescriptor, descriptor, descriptorLength);
	fDescriptorLength = descriptorLength;
	// batch 11: read on first use, not while usb_hid attaches the device
	// (string requests can take seconds on devices that do not answer
	// them, and they ran for every interface a hidraw node was made for)
	memset(&fIdentity, 0, sizeof(fIdentity));
	fIdentityRead = false;
	fStatus = B_OK;
}


HidrawProtocolHandler::~HidrawProtocolHandler()
{
	free(fDescriptor);
	free(fQueue);
	free(fQueueLengths);
	mutex_destroy(&fLock);
}


bool
HidrawProtocolHandler::IsAllowed(HIDDevice& device, uint16 vendor)
{
	// the user's switch
	void* settings = load_driver_settings("gamepad");
	if (settings != NULL) {
		bool allowed = get_driver_boolean_parameter(settings, "raw_access",
			true, true);
		unload_driver_settings(settings);
		if (!allowed)
			return false;
	}

	// batch 11: the policy is shared with the harness (HidrawPolicy.h)
	return hidraw_policy_allows(device.Parser(), vendor);
}


status_t
HidrawProtocolHandler::Open(uint32 flags, uint32* cookie)
{
	status_t status = ProtocolHandler::Open(flags, cookie);
	if (status != B_OK)
		return status;
	if ((flags & O_NONBLOCK) != 0)
		*cookie |= COOKIE_NONBLOCKING;

	MutexLocker locker(fLock);
	if (fOpenCount++ == 0) {
		fHead = fCount = 0;
		locker.Unlock();
		fHIDDevice.MaybeScheduleTransfer(NULL);
	}
	return B_OK;
}


status_t
HidrawProtocolHandler::Close(uint32* cookie)
{
	{
		MutexLocker locker(fLock);
		fOpenCount--;
		fReadable.NotifyAll(B_FILE_ERROR);
	}
	return ProtocolHandler::Close(cookie);
}


void
HidrawProtocolHandler::DeviceRemoved()
{
	MutexLocker locker(fLock);
	fReadable.NotifyAll(ENODEV);
	if (fReadSync != NULL)
		notify_select_event(fReadSync, B_SELECT_READ);
}


void
HidrawProtocolHandler::RawReport(status_t status, const uint8* data,
	size_t length)
{
	if (status != B_OK || length == 0)
		return;

	MutexLocker locker(fLock);
	if (fOpenCount == 0)
		return;
	if (length > fSlotSize)
		length = fSlotSize;
	if (fCount == kQueueLength) {
		fHead = (fHead + 1) % kQueueLength;
		fCount--;
	}
	uint32 slot = (fHead + fCount) % kQueueLength;
	memcpy(fQueue + slot * fSlotSize, data, length);
	fQueueLengths[slot] = length;
	fCount++;
	fReadable.NotifyAll();
	if (fReadSync != NULL)
		notify_select_event(fReadSync, B_SELECT_READ);
}


status_t
HidrawProtocolHandler::Read(uint32* cookie, off_t position, void* buffer,
	size_t* numBytes)
{
	size_t length = *numBytes;
	*numBytes = 0;

	if (!fHIDDevice.IsRemoved())
		fHIDDevice.MaybeScheduleTransfer(NULL);

	MutexLocker locker(fLock);
	while (fCount == 0) {
		if (fHIDDevice.IsRemoved())
			return ENODEV;
		if ((*cookie & COOKIE_NONBLOCKING) != 0)
			return B_WOULD_BLOCK;
		if ((*cookie & PROTOCOL_HANDLER_COOKIE_FLAG_CLOSED) != 0)
			return B_CANCELED;

		ConditionVariableEntry entry;
		fReadable.Add(&entry);
		locker.Unlock();
		status_t status = entry.Wait(B_CAN_INTERRUPT);
		locker.Lock();
		if (status == B_INTERRUPTED)
			return status;
		if (status != B_OK && fCount == 0)
			return fHIDDevice.IsRemoved() ? ENODEV : status;
	}

	size_t reportLength = fQueueLengths[fHead];
	if (reportLength > length)
		reportLength = length;
	status_t status = user_memcpy(buffer, fQueue + fHead * fSlotSize,
		reportLength);
	if (status != B_OK)
		return status;
	fHead = (fHead + 1) % kQueueLength;
	fCount--;
	*numBytes = reportLength;
	return B_OK;
}


struct ReportRequest {
	sem_id	done;
	status_t status;
	size_t	actual;
};


static void
report_request_done(void* cookie, status_t status, void* data,
	size_t actualLength)
{
	ReportRequest* request = (ReportRequest*)cookie;
	request->status = status;
	request->actual = actualLength;
	release_sem(request->done);
}


/*!	Renga (batch 5): a HID class request (GET_REPORT/SET_REPORT) on the
	control pipe that gives up after kReportTimeout. send_request() waits
	up to 5 seconds, and SDL's HIDAPI drivers probe several feature reports
	of every hidraw node at SDL_Init(): a pad interface that never answers
	made SDL_Init() take about 35 seconds on the maintainer's machine. An
	answer that does not come in time is B_TIMED_OUT, as Linux's hidraw
	gives an error, and the request is cancelled.
*/
static status_t
report_request(usb_device device, uint8 requestType, uint8 request,
	uint16 value, uint16 index, uint16 length, void* data, size_t* _actual)
{
	static const bigtime_t kReportTimeout = 250000;
	ReportRequest call;
	call.done = create_sem(0, "hidraw report");
	if (call.done < 0)
		return call.done;
	call.status = B_ERROR;
	call.actual = 0;
	status_t status = gUSBModule->queue_request(device, requestType, request,
		value, index, length, data, report_request_done, &call);
	if (status == B_OK) {
		status = acquire_sem_etc(call.done, 1, B_RELATIVE_TIMEOUT,
			kReportTimeout);
		if (status == B_TIMED_OUT) {
			gUSBModule->cancel_queued_requests(device);
			// the callback runs once the request is cancelled
			acquire_sem(call.done);
		} else if (status == B_OK)
			status = call.status;
	}
	delete_sem(call.done);
	if (_actual != NULL)
		*_actual = call.actual;
	return status;
}


status_t
HidrawProtocolHandler::_SetReport(uint8 type, const uint8* data,
	size_t length)
{
	// data[0] is the report ID; for ID 0 it is not part of the report
	uint8 id = data[0];
	const uint8* report = data;
	size_t reportLength = length;
	if (id == 0) {
		report++;
		reportLength--;
	}

	if (type == 2 && fHIDDevice.HasOutputPipe())
		return fHIDDevice.SendRawOutput(report, reportLength);

	size_t actual;
	return report_request(fHIDDevice.USBDevice(),
		USB_REQTYPE_INTERFACE_OUT | USB_REQTYPE_CLASS,
		B_USB_REQUEST_HID_SET_REPORT, (type << 8) | id,
		fHIDDevice.InterfaceIndex(), reportLength, (void*)report, &actual);
}


status_t
HidrawProtocolHandler::_GetReport(uint8 type, uint8* data, size_t length,
	size_t& actual)
{
	uint8 id = data[0];
	uint8* report = id == 0 ? data + 1 : data;
	size_t reportLength = id == 0 ? length - 1 : length;
	status_t status = report_request(fHIDDevice.USBDevice(),
		USB_REQTYPE_INTERFACE_IN | USB_REQTYPE_CLASS,
		B_USB_REQUEST_HID_GET_REPORT, (type << 8) | id,
		fHIDDevice.InterfaceIndex(), reportLength, report, &actual);
	if (status == B_OK && id == 0)
		actual++;
	return status;
}


status_t
HidrawProtocolHandler::Write(uint32* cookie, off_t position,
	const void* buffer, size_t* numBytes)
{
	size_t length = *numBytes;
	*numBytes = 0;
	if (length == 0 || length > 4096)
		return B_BAD_VALUE;
	if (fHIDDevice.IsRemoved())
		return ENODEV;

	uint8* data = (uint8*)malloc(length);
	if (data == NULL)
		return B_NO_MEMORY;
	status_t status = user_memcpy(data, buffer, length);
	if (status == B_OK)
		status = _SetReport(2, data, length);
	free(data);
	if (status == B_OK)
		*numBytes = length;
	return status;
}


static status_t
copy_string(void* buffer, size_t size, const char* string)
{
	size_t length = strlen(string) + 1;
	if (length > size)
		length = size;
	if (length == 0)
		return 0;
	char copy[80];
	strlcpy(copy, string, sizeof(copy));
	if (length > sizeof(copy))
		length = sizeof(copy);
	copy[length - 1] = '\0';
	status_t status = user_memcpy(buffer, copy, length);
	return status == B_OK ? (status_t)length : status;
}


status_t
HidrawProtocolHandler::Control(uint32* cookie, uint32 op, void* buffer,
	size_t length)
{
	switch (op) {
		case B_SET_NONBLOCKING_IO:
			*cookie |= COOKIE_NONBLOCKING;
			return B_OK;
		case B_SET_BLOCKING_IO:
			*cookie &= ~COOKIE_NONBLOCKING;
			return B_OK;
		case B_GET_DEVICE_NAME:
			return IOGetDeviceName("USB raw HID game controller", buffer,
				length);
	}

	if (_LINUX_IOC_TYPE(op) != 'H')
		return B_DEV_INVALID_IOCTL;
	if (fHIDDevice.IsRemoved())
		return ENODEV;

	if (!fIdentityRead) {
		gamepad_hid_get_identity(&fHIDDevice, fIdentity);
		fIdentityRead = true;
	}
	const gamepad_hid_identity& identity = fIdentity;

	uint32 size = _LINUX_IOC_SIZE(op);
	switch (_LINUX_IOC_NR(op)) {
		case 0x01:	// HIDIOCGRDESCSIZE
		{
			int32 descriptorSize = fDescriptorLength;
			return user_memcpy(buffer, &descriptorSize, sizeof(int32));
		}

		case 0x02:	// HIDIOCGRDESC
		{
			uint32 wanted;
			if (user_memcpy(&wanted, buffer, sizeof(wanted)) != B_OK)
				return B_BAD_ADDRESS;
			if (wanted > HID_MAX_DESCRIPTOR_SIZE)
				return B_BAD_VALUE;
			if (wanted > fDescriptorLength)
				wanted = fDescriptorLength;
			return user_memcpy((uint8*)buffer + sizeof(uint32), fDescriptor,
				wanted);
		}

		case 0x03:	// HIDIOCGRAWINFO
		{
			struct hidraw_devinfo info;
			info.bustype = identity.bus;
			info.vendor = (int16)identity.vendor;
			info.product = (int16)identity.product;
			return user_memcpy(buffer, &info, sizeof(info));
		}

		case 0x04:	// HIDIOCGRAWNAME(len)
			return copy_string(buffer, size, identity.name);
		case 0x05:	// HIDIOCGRAWPHYS(len)
			return copy_string(buffer, size, identity.phys);
		case 0x08:	// HIDIOCGRAWUNIQ(len)
			return copy_string(buffer, size, identity.uniq);

		case 0x06:	// HIDIOCSFEATURE(len)
		case 0x09:	// HIDIOCSINPUT(len)
		case 0x0b:	// HIDIOCSOUTPUT(len)
		case 0x07:	// HIDIOCGFEATURE(len)
		case 0x0a:	// HIDIOCGINPUT(len)
		case 0x0c:	// HIDIOCGOUTPUT(len)
		{
			if (size == 0 || size > 4096)
				return B_BAD_VALUE;
			uint8* data = (uint8*)malloc(size);
			if (data == NULL)
				return B_NO_MEMORY;
			status_t status = user_memcpy(data, buffer, size);
			uint32 nr = _LINUX_IOC_NR(op);
			uint8 type = (nr == 0x06 || nr == 0x07) ? 3
				: (nr == 0x09 || nr == 0x0a) ? 1 : 2;
			bool set = nr == 0x06 || nr == 0x09 || nr == 0x0b;
			size_t actual = size;
			if (status == B_OK) {
				status = set ? _SetReport(type, data, size)
					: _GetReport(type, data, size, actual);
			}
			if (status == B_OK && !set)
				status = user_memcpy(buffer, data, actual);
			free(data);
			return status == B_OK ? (status_t)actual : status;
		}

		case 0x0d:	// HIDIOCREVOKE
			return B_NOT_SUPPORTED;
	}
	return B_DEV_INVALID_IOCTL;
}


status_t
HidrawProtocolHandler::Select(uint32* cookie, uint8 event, selectsync* sync)
{
	if (event == B_SELECT_WRITE)
		return notify_select_event(sync, event);
	if (event != B_SELECT_READ)
		return B_OK;
	if (!fHIDDevice.IsRemoved())
		fHIDDevice.MaybeScheduleTransfer(NULL);
	MutexLocker locker(fLock);
	fReadSync = sync;
	if (fCount > 0 || fHIDDevice.IsRemoved())
		notify_select_event(sync, B_SELECT_READ);
	return B_OK;
}


status_t
HidrawProtocolHandler::Deselect(uint32* cookie, uint8 event,
	selectsync* sync)
{
	MutexLocker locker(fLock);
	if (event == B_SELECT_READ && fReadSync == sync)
		fReadSync = NULL;
	return B_OK;
}
