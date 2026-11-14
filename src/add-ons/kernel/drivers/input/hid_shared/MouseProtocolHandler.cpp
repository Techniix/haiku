/*
 * Copyright 2008-2011 Michael Lotz <mmlr@mlotz.ch>
 * Distributed under the terms of the MIT license.
 */


//!	Driver for USB Human Interface Devices.


#include "Driver.h"
#include "MouseProtocolHandler.h"

#include "HIDCollection.h"
#include "HIDDevice.h"
#include "HIDReport.h"
#include "HIDReportItem.h"

#include <new>
#include <string.h>
#include <usb/USB_hid.h>

#include <kernel.h>
#include <keyboard_mouse_driver.h>


MouseProtocolHandler::MouseProtocolHandler(HIDReport &report,
	HIDReportItem &xAxis, HIDReportItem &yAxis)
	:
	ProtocolHandler(report.Device(), "input/mouse/" DEVICE_PATH_SUFFIX "/", 0),
	fReport(report),

	fXAxis(xAxis),
	fYAxis(yAxis),
	fWheel(NULL),
	fHorizontalPan(NULL),

	fLastButtons(0),
	fClickCount(0),
	fLastClickTime(0),
	fClickSpeed(250000),
	fOpenCount(0)
{
	fPoller.Init("usb mouse poller", &_Poll, this);

	uint32 buttonCount = 0;
	for (uint32 i = 0; i < report.CountItems(); i++) {
		HIDReportItem *item = report.ItemAt(i);
		if (!item->HasData())
			continue;

		if (item->UsagePage() == B_HID_USAGE_PAGE_BUTTON
			&& item->UsageID() - 1 < B_MAX_MOUSE_BUTTONS) {
			fButtons[buttonCount++] = item;
		}
	}

	fButtons[buttonCount] = NULL;

	fWheel = report.FindItem(B_HID_USAGE_PAGE_GENERIC_DESKTOP,
		B_HID_UID_GD_WHEEL);
	fHorizontalPan = report.FindItem(B_HID_USAGE_PAGE_CONSUMER,
		B_HID_UID_CON_AC_PAN);

	TRACE("mouse device with %" B_PRIu32 " buttons %sand %swheel\n",
		buttonCount, fHorizontalPan == NULL ? "" : ", horizontal pan ",
		fWheel == NULL ? "no " : "");
	TRACE("report id: %u\n", report.ID());
}


MouseProtocolHandler::~MouseProtocolHandler()
{
	fPoller.Uninit();
}


/*!	Renga (pointer latency fix, 2026-11-14): while the node is open, a
	thread polls the mouse at its own rate (one interrupt transfer queued at
	all times, as Linux's usbhid does) and sums its reports into frames
	(MousePoller.h). Before, a transfer was only scheduled while MS_READ
	waited, and input_server's mouse add-on reads every 8 ms: a 1000 Hz mouse
	was polled 125 times a second, and what it could not send meanwhile came
	out after the hand stopped (the pointer kept moving and caught up).
*/
status_t
MouseProtocolHandler::Open(uint32 flags, uint32 *cookie)
{
	status_t status = ProtocolHandler::Open(flags, cookie);
	if (status != B_OK)
		return status;
	if (atomic_add(&fOpenCount, 1) == 0) {
		status = fPoller.Start();
		if (status != B_OK) {
			atomic_add(&fOpenCount, -1);
			ProtocolHandler::Close(cookie);
			return status;
		}
	}
	return B_OK;
}


status_t
MouseProtocolHandler::Close(uint32 *cookie)
{
	if (atomic_add(&fOpenCount, -1) == 1)
		fPoller.Stop();
	return ProtocolHandler::Close(cookie);
}


/*static*/ status_t
MouseProtocolHandler::_Poll(void *_handler, mouse_movement &movement)
{
	MouseProtocolHandler *handler = (MouseProtocolHandler *)_handler;
	uint32 cookie = 0;
	// returns within 100 ms so that the poller notices Stop()
	status_t status = handler->_ReadReport(&movement, &cookie, 100000);
	if (status == B_BUSY && handler->fReport.Device()->IsRemoved())
		return B_DEV_NOT_READY;
	return status;
}


void
MouseProtocolHandler::AddHandlers(HIDDevice &device, HIDCollection &collection,
	ProtocolHandler *&handlerList)
{
	bool supported = false;
	switch (collection.UsagePage()) {
		case B_HID_USAGE_PAGE_GENERIC_DESKTOP:
		{
			switch (collection.UsageID()) {
				case B_HID_UID_GD_MOUSE:
				case B_HID_UID_GD_POINTER:
					supported = true;
					break;
			}

			break;
		}
	}

	if (!supported) {
		TRACE("collection not a mouse\n");
		return;
	}

	HIDParser &parser = device.Parser();
	uint32 maxReportCount = parser.CountReports(HID_REPORT_TYPE_INPUT);
	if (maxReportCount == 0)
		return;

	uint32 inputReportCount = 0;
	HIDReport *inputReports[maxReportCount];
	collection.BuildReportList(HID_REPORT_TYPE_INPUT, inputReports,
		inputReportCount);

	for (uint32 i = 0; i < inputReportCount; i++) {
		HIDReport *inputReport = inputReports[i];

		// try to find at least an absolute x and y axis
		HIDReportItem *xAxis = inputReport->FindItem(
			B_HID_USAGE_PAGE_GENERIC_DESKTOP, B_HID_UID_GD_X);
		if (xAxis == NULL || !xAxis->Relative())
			continue;

		HIDReportItem *yAxis = inputReport->FindItem(
			B_HID_USAGE_PAGE_GENERIC_DESKTOP, B_HID_UID_GD_Y);
		if (yAxis == NULL || !yAxis->Relative())
			continue;

		ProtocolHandler *newHandler = new(std::nothrow) MouseProtocolHandler(
			*inputReport, *xAxis, *yAxis);
		if (newHandler == NULL) {
			TRACE("failed to allocated mouse protocol handler\n");
			continue;
		}

		newHandler->SetNextHandler(handlerList);
		handlerList = newHandler;
	}
}


status_t
MouseProtocolHandler::Control(uint32 *cookie, uint32 op, void *buffer,
	size_t length)
{
	switch (op) {

		case B_GET_DEVICE_NAME:
		{
			const char name[] = DEVICE_NAME" Mouse";
			return IOGetDeviceName(name,buffer,length);
		}

		case MS_READ:
		{
			if (length < sizeof(mouse_movement))
				return B_BUFFER_OVERFLOW;

			while (true) {
				mouse_movement movement;
				status_t result;
				if (fPoller.IsRunning()) {
					// the frames the poller collected (see Open())
					result = fPoller.Read(movement);
					if (result == B_FILE_ERROR
						&& (*cookie & PROTOCOL_HANDLER_COOKIE_FLAG_CLOSED) == 0
						&& fPoller.IsRunning()) {
						continue;
					}
				} else
					result = _ReadReport(&movement, cookie);
				if (result == B_INTERRUPTED)
					return result;
				if (result == B_BUSY)
					continue;
				if (result != B_OK)
					return result;

				if (!IS_USER_ADDRESS(buffer)
					|| user_memcpy(buffer, &movement, sizeof(movement))
						!= B_OK) {
					return B_BAD_ADDRESS;
				}

				return result;
			}
		}

		case MS_NUM_EVENTS:
		{
			if (fReport.Device()->IsRemoved())
				return B_DEV_NOT_READY;

			// Renga: the frames the poller holds (summed motion, button
			// changes apart), so input_server's watcher merges them
			if (fPoller.IsRunning())
				return fPoller.Count();
			return 0;
		}

		case MS_SET_CLICKSPEED:
			if (!IS_USER_ADDRESS(buffer)
				|| user_memcpy(&fClickSpeed, buffer, sizeof(bigtime_t))
					!= B_OK) {
				return B_BAD_ADDRESS;
			}

			return B_OK;
	}

	return B_ERROR;
}


status_t
MouseProtocolHandler::_ReadReport(void *buffer, uint32 *cookie,
	bigtime_t timeout)
{
	status_t result = fReport.WaitForReport(timeout);
	if (result != B_OK) {
		if (fReport.Device()->IsRemoved()) {
			TRACE("device has been removed\n");
			return B_DEV_NOT_READY;
		}

		if (result == B_INTERRUPTED)
			return result;
		if ((*cookie & PROTOCOL_HANDLER_COOKIE_FLAG_CLOSED) != 0)
			return B_FILE_ERROR;

		if (result != B_BUSY && result != B_TIMED_OUT) {
			// "busy" happens when other reports come in on the same input as ours
			TRACE_ALWAYS("error waiting for report: %s\n", strerror(result));
		}

		// signal that we simply want to try again
		return B_BUSY;
	}

	uint32 axisRelativeData[2];
	axisRelativeData[0] = 0;
	axisRelativeData[1] = 0;

	if (fXAxis.Extract() == B_OK && fXAxis.Valid())
		axisRelativeData[0] = fXAxis.Data();

	if (fYAxis.Extract() == B_OK && fYAxis.Valid())
		axisRelativeData[1] = fYAxis.Data();

	uint32 wheelData[2] = {0};
	if (fWheel != NULL && fWheel->Extract() == B_OK && fWheel->Valid())
		wheelData[0] = fWheel->Data();
	if (fHorizontalPan != NULL && fHorizontalPan->Extract() == B_OK
			&& fHorizontalPan->Valid())
		wheelData[1] = fHorizontalPan->Data();

	uint32 buttons = 0;
	for (uint32 i = 0; i < B_MAX_MOUSE_BUTTONS; i++) {
		HIDReportItem *button = fButtons[i];
		if (button == NULL)
			break;

		if (button->Extract() == B_OK && button->Valid())
			buttons |= (button->Data() & 1) << (button->UsageID() - 1);
	}

	fReport.DoneProcessing();
	TRACE("got mouse report\n");

	int32 clicks = 0;
	bigtime_t timestamp = system_time();
	if (buttons != 0) {
		if (fLastButtons == 0) {
			if (fLastClickTime + fClickSpeed > timestamp)
				fClickCount++;
			else
				fClickCount = 1;
		}

		fLastClickTime = timestamp;
		clicks = fClickCount;
	}

	fLastButtons = buttons;

	mouse_movement *info = (mouse_movement *)buffer;
	memset(info, 0, sizeof(mouse_movement));

	info->buttons = buttons;
	info->xdelta = axisRelativeData[0];
	info->ydelta = -axisRelativeData[1];
	info->clicks = clicks;
	info->timestamp = timestamp;
	info->wheel_ydelta = -wheelData[0];
	info->wheel_xdelta = wheelData[1];

	return B_OK;
}
