/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 *
 * The gamepad core (kernel module "generic/gamepad/v1", roadmap F-42).
 *
 * Gamepad drivers (usb_hid's HID bridge, xinput, the uinput virtual device)
 * register each pad here and report Linux evdev events for it. The core
 * publishes the pad as /dev/input/event<N> with the Linux evdev interface
 * (<linux/input.h>): event queues per open descriptor with SYN_DROPPED on
 * overflow, the current key and axis state, EVIOCGRAB, memoryless force
 * feedback (rumble effects with replay length and delay, gain), select().
 * It also gives every pad a player number (1-8, lowest free first) and
 * tells the driver so the pad's LEDs can show it, keeps the battery level
 * the driver reports, and, on request, publishes a BJoystick node
 * (/dev/joystick/gamepad/<N>) for pads the old joystick API cannot see
 * otherwise.
 *
 * Everything here is called from thread context (never from an interrupt
 * handler); report() takes a mutex.
 */
#ifndef _GAMEPAD_MODULE_H
#define _GAMEPAD_MODULE_H


#include <OS.h>
#include <module.h>

#include <linux/input.h>


#define GAMEPAD_MODULE_NAME		"generic/gamepad/v1"

#define GAMEPAD_MAX_DEVICES		32
#define GAMEPAD_MAX_PLAYERS		8
#define GAMEPAD_MAX_EFFECTS		16


typedef struct gamepad_device gamepad_device;


enum {
	/* Also publish /dev/joystick/gamepad/<N> for BJoystick. */
	GAMEPAD_FLAG_JOYSTICK_NODE	= 0x01,
	/* A uinput device made by a program, not hardware. */
	GAMEPAD_FLAG_VIRTUAL		= 0x02,
	/* Do not assign a player number (not a game controller for players,
	   e.g. a remapping tool's helper device). */
	GAMEPAD_FLAG_NO_PLAYER		= 0x04
};


typedef struct gamepad_device_info {
	char					name[80];
	char					phys[64];
	char					uniq[64];
	struct input_id			id;
	uint32					flags;

	uint8					ev_bits[(EV_CNT + 7) / 8];
	uint8					key_bits[(KEY_CNT + 7) / 8];
	uint8					abs_bits[(ABS_CNT + 7) / 8];
	uint8					ff_bits[(FF_CNT + 7) / 8];
	uint8					prop_bits[(INPUT_PROP_CNT + 7) / 8];
	struct input_absinfo	abs[ABS_CNT];
} gamepad_device_info;


typedef struct gamepad_device_hooks {
	/* The first descriptor was opened: start sending input. May be NULL
	   for drivers that always run. */
	status_t	(*open)(void* cookie);
	/* The last descriptor was closed. */
	void		(*close)(void* cookie);
	/* Combined rumble strength of all playing effects (0 = stop). */
	void		(*rumble)(void* cookie, uint16 strong, uint16 weak);
	/* The player number changed (0 = none); show it on the LEDs. */
	void		(*set_player)(void* cookie, int32 player);
	/* An event a client wrote that the core does not handle itself
	   (EV_LED, EV_SND, ...), or EV_FF play/stop for virtual devices. */
	void		(*output_event)(void* cookie, uint16 type, uint16 code,
					int32 value);
} gamepad_device_hooks;


typedef struct gamepad_module_info {
	module_info	info;

	status_t	(*register_device)(const gamepad_device_info* info,
					const gamepad_device_hooks* hooks, void* cookie,
					gamepad_device** _device);
		/* Publishes the node. The hooks may be called from then on, until
		   unregister_device() returns. */
	void		(*unregister_device)(gamepad_device* device);
		/* Unpublishes the node; open descriptors then fail with ENODEV.
		   No hook is called once this returns. */

	void		(*report)(gamepad_device* device, uint16 type, uint16 code,
					int32 value);
		/* EV_KEY/EV_ABS/EV_MSC/...; EV_SYN SYN_REPORT completes a packet.
		   Unchanged key and axis values are dropped like on Linux. */
	void		(*set_battery)(gamepad_device* device, int32 level,
					bool charging, bool wired);
	int32		(*player)(gamepad_device* device);
	int32		(*index)(gamepad_device* device);
		/* The N of /dev/input/event<N>. */
} gamepad_module_info;


#ifdef __cplusplus
/* Small helpers for filling a gamepad_device_info. */
static inline void
gamepad_set_bit(uint8* bits, uint32 bit)
{
	bits[bit / 8] |= (uint8)(1 << (bit % 8));
}

static inline bool
gamepad_test_bit(const uint8* bits, uint32 bit)
{
	return (bits[bit / 8] & (1 << (bit % 8))) != 0;
}

static inline void
gamepad_set_abs(gamepad_device_info* info, uint32 code, int32 minimum,
	int32 maximum, int32 fuzz, int32 flat)
{
	gamepad_set_bit(info->ev_bits, EV_ABS);
	gamepad_set_bit(info->abs_bits, code);
	info->abs[code].minimum = minimum;
	info->abs[code].maximum = maximum;
	info->abs[code].fuzz = fuzz;
	info->abs[code].flat = flat;
	info->abs[code].value = 0;
}

static inline void
gamepad_set_key(gamepad_device_info* info, uint32 code)
{
	gamepad_set_bit(info->ev_bits, EV_KEY);
	gamepad_set_bit(info->key_bits, code);
}
#endif


#endif	/* _GAMEPAD_MODULE_H */
