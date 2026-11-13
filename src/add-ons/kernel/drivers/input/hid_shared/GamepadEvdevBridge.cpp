/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 *
 * Renga (roadmap F-42): HID game controllers as Linux evdev devices; see
 * GamepadEvdevBridge.h. Built into usb_hid (kernel) and, with USERLAND_HID,
 * into the harness's mapping test, which feeds it report descriptors and
 * reports of real pads.
 */


#ifndef USERLAND_HID
#	include "Driver.h"
#	include <kernel.h>
#	include <util/AutoLock.h>
#else
#	include "UserlandHID.h"
#endif

#include "GamepadEvdevBridge.h"

#include "HIDCollection.h"
#include "HIDDevice.h"
#include "HIDReport.h"
#include "HIDReportItem.h"

#include <new>
#include <stdlib.h>
#include <string.h>

#include <usb/USB_hid.h>


#ifndef USERLAND_HID
static gamepad_module_info* sGamepadModule = NULL;
static int32 sGamepadModuleUsers = 0;
#endif


static const uint16 kUsagePageSimulation = 0x02;
static const uint16 kUsagePageGenericDevice = 0x06;
static const uint16 kUsageBatteryStrength = 0x20;


GamepadEvdevBridge::GamepadEvdevBridge(HIDDevice& device, HIDReport& report)
	:
	fDevice(device),
	fReport(report),
	fMappings(NULL),
	fMappingCount(0),
	fGamepad(NULL),
	fThread(-1),
	fRunning(0),
	fHook(NULL),
	fHookCookie(NULL)
{
	memset(&fInfo, 0, sizeof(fInfo));
#ifndef USERLAND_HID
	fThreadAlive = false;
	mutex_init(&fLock, "hid gamepad bridge");
#endif
}


GamepadEvdevBridge::~GamepadEvdevBridge()
{
	Unregister();

#ifndef USERLAND_HID
	mutex_lock(&fLock);
	fRunning = 0;
	bool alive = fThreadAlive;
	mutex_unlock(&fLock);
	if (alive && fThread >= 0) {
		// only deleted once the device is gone, so the reader's wait has
		// ended or ends right away
		status_t result;
		wait_for_thread(fThread, &result);
	}
	mutex_destroy(&fLock);
	if (atomic_add(&sGamepadModuleUsers, -1) == 1) {
		put_module(GAMEPAD_MODULE_NAME);
		sGamepadModule = NULL;
	}
#endif

	free(fMappings);
}


GamepadEvdevBridge*
GamepadEvdevBridge::Create(HIDDevice& device, HIDReport& report,
	uint16 application)
{
	GamepadEvdevBridge* bridge
		= new(std::nothrow) GamepadEvdevBridge(device, report);
	if (bridge == NULL)
		return NULL;

#ifndef USERLAND_HID
	if (atomic_add(&sGamepadModuleUsers, 1) == 0) {
		if (get_module(GAMEPAD_MODULE_NAME,
				(module_info**)&sGamepadModule) != B_OK) {
			sGamepadModule = NULL;
		}
	}
	if (sGamepadModule == NULL) {
		delete bridge;
		return NULL;
	}
#endif

	gamepad_hid_identity identity;
	memset(&identity, 0, sizeof(identity));
	gamepad_hid_get_identity(&device, identity);
	strlcpy(bridge->fInfo.name, identity.name, sizeof(bridge->fInfo.name));
	strlcpy(bridge->fInfo.phys, identity.phys, sizeof(bridge->fInfo.phys));
	strlcpy(bridge->fInfo.uniq, identity.uniq, sizeof(bridge->fInfo.uniq));
	bridge->fInfo.id.bustype = identity.bus;
	bridge->fInfo.id.vendor = identity.vendor;
	bridge->fInfo.id.product = identity.product;
	bridge->fInfo.id.version = identity.version;

	if (!bridge->_Map(application) || bridge->_Register() != B_OK) {
		delete bridge;
		return NULL;
	}
	return bridge;
}


bool
GamepadEvdevBridge::_Map(uint16 application)
{
	uint32 count = fReport.CountItems();
	fMappings = (Mapping*)calloc(count > 0 ? count : 1, sizeof(Mapping));
	if (fMappings == NULL)
		return false;

	uint32 hats = 0;
	bool haveAxis = false, haveButton = false;

	for (uint32 i = 0; i < count; i++) {
		HIDReportItem* item = fReport.ItemAt(i);
		if (item == NULL || !item->HasData() || item->Relative())
			continue;

		Mapping mapping;
		memset(&mapping, 0, sizeof(mapping));
		mapping.item = item;
		mapping.isSigned = item->Signed();
		mapping.minimum = (int32)item->Minimum();
		mapping.maximum = (int32)item->Maximum();

		uint16 page = item->UsagePage();
		uint16 usage = item->UsageID();

		if (page == B_HID_USAGE_PAGE_BUTTON && !item->Array()) {
			if (usage == 0)
				continue;
			uint32 code = usage - 1;
			if (code <= 0xf) {
				code += application == B_HID_UID_GD_GAMEPAD
					? BTN_GAMEPAD : BTN_JOYSTICK;
			} else
				code = BTN_TRIGGER_HAPPY + code - 0x10;
			if (code >= KEY_CNT || (code > BTN_TRIGGER_HAPPY40
					&& code < BTN_GAMEPAD))
				continue;
			// a second item with the same usage takes the next free code
			while (code < KEY_CNT && gamepad_test_bit(fInfo.key_bits, code))
				code++;
			if (code >= KEY_CNT)
				continue;
			mapping.type = EV_KEY;
			mapping.code = code;
			gamepad_set_key(&fInfo, code);
			haveButton = true;
		} else if (page == B_HID_USAGE_PAGE_GENERIC_DESKTOP) {
			if (usage >= B_HID_UID_GD_X && usage <= B_HID_UID_GD_WHEEL) {
				uint32 code = usage & 0xf;
				while (code < ABS_HAT0X
					&& gamepad_test_bit(fInfo.abs_bits, code))
					code++;
				if (code >= ABS_HAT0X)
					code = ABS_MISC;
				if (gamepad_test_bit(fInfo.abs_bits, code))
					continue;
				mapping.type = EV_ABS;
				mapping.code = code;
				int32 minimum = mapping.minimum, maximum = mapping.maximum;
				if (!mapping.isSigned) {
					minimum = (int32)item->Minimum();
					maximum = (int32)item->Maximum();
				}
				// like hid-input: small fuzz and flat for sticks
				int32 range = maximum - minimum;
				gamepad_set_abs(&fInfo, code, minimum, maximum,
					range > 255 ? range >> 8 : 0,
					range > 255 ? range >> 4 : 0);
				haveAxis = true;
			} else if (usage == B_HID_UID_GD_HAT_SWITCH) {
				if (hats >= 4)
					continue;
				mapping.type = EV_ABS;
				mapping.code = ABS_HAT0X + hats * 2;
				mapping.hat = true;
				gamepad_set_abs(&fInfo, mapping.code, -1, 1, 0, 0);
				gamepad_set_abs(&fInfo, mapping.code + 1, -1, 1, 0, 0);
				hats++;
				haveAxis = true;
			} else if (usage >= 0x90 && usage <= 0x93) {
				static const uint16 kDpad[] = { BTN_DPAD_UP, BTN_DPAD_DOWN,
					BTN_DPAD_RIGHT, BTN_DPAD_LEFT };
				mapping.type = EV_KEY;
				mapping.code = kDpad[usage - 0x90];
				gamepad_set_key(&fInfo, mapping.code);
				haveButton = true;
			} else
				continue;
		} else if (page == kUsagePageSimulation) {
			uint32 code;
			switch (usage) {
				case 0xba: code = ABS_RUDDER; break;
				case 0xbb: code = ABS_THROTTLE; break;
				case 0xc4: code = ABS_GAS; break;
				case 0xc5: code = ABS_BRAKE; break;
				case 0xc8: code = ABS_WHEEL; break;
				default: continue;
			}
			if (gamepad_test_bit(fInfo.abs_bits, code))
				continue;
			mapping.type = EV_ABS;
			mapping.code = code;
			gamepad_set_abs(&fInfo, code, mapping.minimum, mapping.maximum,
				0, 0);
			haveAxis = true;
		} else if (page == kUsagePageGenericDevice
			&& usage == kUsageBatteryStrength) {
			mapping.type = 0;
		} else
			continue;

		fMappings[fMappingCount++] = mapping;
	}

	return haveAxis || haveButton;
}


status_t
GamepadEvdevBridge::_Register()
{
#ifdef USERLAND_HID
	return B_OK;
#else
	static gamepad_device_hooks hooks = {
		_Open,
		_Close,
		NULL,
		NULL,
		NULL
	};
	return sGamepadModule->register_device(&fInfo, &hooks, this, &fGamepad);
#endif
}


void
GamepadEvdevBridge::Unregister()
{
#ifndef USERLAND_HID
	gamepad_device* gamepad = fGamepad;
	fGamepad = NULL;
	if (gamepad != NULL)
		sGamepadModule->unregister_device(gamepad);
#endif
}


void
GamepadEvdevBridge::SetReportHook(void (*hook)(void*, uint16, uint16, int32),
	void* cookie)
{
	fHook = hook;
	fHookCookie = cookie;
}


void
GamepadEvdevBridge::_Report(uint16 type, uint16 code, int32 value)
{
	if (fHook != NULL)
		fHook(fHookCookie, type, code, value);
#ifndef USERLAND_HID
	if (fGamepad != NULL)
		sGamepadModule->report(fGamepad, type, code, value);
#endif
}


void
GamepadEvdevBridge::ProcessReport()
{
	for (uint32 i = 0; i < fMappingCount; i++) {
		Mapping& mapping = fMappings[i];
		if (mapping.item->Extract() != B_OK)
			continue;
		bool valid = mapping.item->Valid();
		int32 data = (int32)mapping.item->Data();

		if (mapping.hat) {
			// out of range is the "null" state: centred
			int32 x = 0, y = 0;
			int32 range = mapping.maximum - mapping.minimum + 1;
			if (valid && range > 0) {
				// 0 = N, then clockwise in eight steps
				static const int8 kHatX[8] = { 0, 1, 1, 1, 0, -1, -1, -1 };
				static const int8 kHatY[8] = { -1, -1, 0, 1, 1, 1, 0, -1 };
				int32 step = (data - mapping.minimum) * 8 / range;
				if (step >= 0 && step < 8) {
					x = kHatX[step];
					y = kHatY[step];
				}
			}
			_Report(EV_ABS, mapping.code, x);
			_Report(EV_ABS, mapping.code + 1, y);
			continue;
		}

		if (!valid)
			continue;

		if (mapping.type == EV_KEY)
			_Report(EV_KEY, mapping.code, data != 0 ? 1 : 0);
		else if (mapping.type == EV_ABS)
			_Report(EV_ABS, mapping.code, data);
		else if (mapping.type == 0) {
			int32 range = mapping.maximum - mapping.minimum;
			int32 level = range > 0
				? (data - mapping.minimum) * 100 / range : -1;
#ifndef USERLAND_HID
			if (fGamepad != NULL)
				sGamepadModule->set_battery(fGamepad, level, false, false);
#endif
			if (fHook != NULL)
				fHook(fHookCookie, 0xffff, 0, level);
		}
	}
	_Report(EV_SYN, SYN_REPORT, 0);
}


#ifndef USERLAND_HID


status_t
GamepadEvdevBridge::_Open(void* cookie)
{
	GamepadEvdevBridge* bridge = (GamepadEvdevBridge*)cookie;
	MutexLocker locker(bridge->fLock);
	bridge->fRunning = 1;
	if (bridge->fThreadAlive)
		return B_OK;
		// the reader of an earlier open is still waiting for a report and
		// simply carries on

	bridge->fThread = spawn_kernel_thread(_ReaderThread, "hid gamepad",
		B_URGENT_DISPLAY_PRIORITY, bridge);
	if (bridge->fThread < 0) {
		bridge->fRunning = 0;
		return bridge->fThread;
	}
	bridge->fThreadAlive = true;
	resume_thread(bridge->fThread);
	return B_OK;
}


void
GamepadEvdevBridge::_Close(void* cookie)
{
	GamepadEvdevBridge* bridge = (GamepadEvdevBridge*)cookie;
	MutexLocker locker(bridge->fLock);
	bridge->fRunning = 0;
	// the reader stops after the report it is waiting for
}


status_t
GamepadEvdevBridge::_ReaderThread(void* cookie)
{
	GamepadEvdevBridge* bridge = (GamepadEvdevBridge*)cookie;
	HIDReport& report = bridge->fReport;

	while (true) {
		{
			MutexLocker locker(bridge->fLock);
			if (bridge->fRunning == 0 || report.Device()->IsRemoved()) {
				bridge->fThreadAlive = false;
				return B_OK;
			}
		}

		status_t result = report.WaitForReport(B_INFINITE_TIMEOUT);
		if (result != B_OK) {
			if (report.Device()->IsRemoved() || result == B_INTERRUPTED) {
				MutexLocker locker(bridge->fLock);
				bridge->fThreadAlive = false;
				return B_OK;
			}
			// busy (another report on the same pipe) or cancelled: retry
			snooze(1000);
			continue;
		}
		if (bridge->fRunning != 0)
			bridge->ProcessReport();
		report.DoneProcessing();
	}
}


#else	// USERLAND_HID


status_t
GamepadEvdevBridge::_Open(void*)
{
	return B_OK;
}


void
GamepadEvdevBridge::_Close(void*)
{
}


status_t
GamepadEvdevBridge::_ReaderThread(void*)
{
	return B_OK;
}


#endif	// USERLAND_HID
