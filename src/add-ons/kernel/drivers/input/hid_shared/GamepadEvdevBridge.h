/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 *
 * Renga (roadmap F-42): HID game controllers as Linux evdev devices.
 */
#ifndef GAMEPAD_EVDEV_BRIDGE_H
#define GAMEPAD_EVDEV_BRIDGE_H


#include <SupportDefs.h>
#include <OS.h>

#include <gamepad_module.h>

#ifndef USERLAND_HID
#	include <lock.h>
#endif


class HIDDevice;
class HIDReport;
class HIDReportItem;


// What the bus driver knows about the device; usb_hid fills it in.
struct gamepad_hid_identity {
	uint16	bus;		// BUS_USB, ...
	uint16	vendor;
	uint16	product;
	uint16	version;
	char	name[80];
	char	phys[64];
	char	uniq[64];
};

// Implemented by the bus driver.
status_t	gamepad_hid_get_identity(HIDDevice* device,
				gamepad_hid_identity& identity);


// One input report of a joystick or gamepad application collection,
// published through the gamepad core as /dev/input/event<N>. Usages are
// mapped the way Linux's hid-input does it, so the SDL mapping database's
// Linux entries apply: buttons to BTN_GAMEPAD (gamepads) or BTN_JOYSTICK
// (joysticks) and on to BTN_TRIGGER_HAPPY, X/Y/Z/Rx/Ry/Rz/slider/dial/wheel
// to ABS_X ... ABS_WHEEL, hat switches to ABS_HAT<n>X/Y, D-pad usages to
// BTN_DPAD_*, the simulation page's accelerator, brake, rudder, throttle
// and steering to ABS_GAS, ABS_BRAKE, ABS_RUDDER, ABS_THROTTLE and
// ABS_WHEEL. A battery strength usage becomes the pad's battery level.
//
// The report is only read while a descriptor of the evdev device is open.
class GamepadEvdevBridge {
public:
	static	GamepadEvdevBridge*	Create(HIDDevice& device, HIDReport& report,
									uint16 application);
								~GamepadEvdevBridge();

			void				Unregister();
				// the device went away; the evdev node is removed

			// for the harness's userland test of the mapping
			const gamepad_device_info& Info() const { return fInfo; }
			void				ProcessReport();
			void				SetReportHook(void (*hook)(void*, uint16,
									uint16, int32), void* cookie);

private:
								GamepadEvdevBridge(HIDDevice& device,
									HIDReport& report);
			bool				_Map(uint16 application);
			status_t			_Register();

	static	status_t			_Open(void* cookie);
	static	void				_Close(void* cookie);
	static	status_t			_ReaderThread(void* cookie);
			void				_Report(uint16 type, uint16 code,
									int32 value);

			struct Mapping {
				HIDReportItem*	item;
				uint16			type;	// EV_KEY, EV_ABS, 0 = battery
				uint16			code;
				int32			minimum;
				int32			maximum;
				bool			isSigned;
				bool			hat;
			};

			HIDDevice&			fDevice;
			HIDReport&			fReport;
			Mapping*			fMappings;
			uint32				fMappingCount;
			gamepad_device_info	fInfo;
			gamepad_device*		fGamepad;
			thread_id			fThread;
			int32				fRunning;
			void				(*fHook)(void*, uint16, uint16, int32);
			void*				fHookCookie;
#ifndef USERLAND_HID
			mutex				fLock;
			bool				fThreadAlive;
#endif
};


#endif	// GAMEPAD_EVDEV_BRIDGE_H
