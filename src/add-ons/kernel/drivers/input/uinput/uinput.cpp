/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 *
 * uinput (roadmap F-42): virtual input devices made by a program, with the
 * Linux interface of <linux/uinput.h>, published as /dev/input/uinput.
 *
 * A program opens the node, describes a device (UI_SET_EVBIT, UI_SET_KEYBIT,
 * UI_SET_ABSBIT, UI_SET_FFBIT, UI_ABS_SETUP and UI_DEV_SETUP, or the older
 * write() of a struct uinput_user_dev), creates it with UI_DEV_CREATE and
 * then write()s input_event records, which the gamepad core passes to the
 * readers of the new /dev/input/event<N> as if a pad had sent them. The
 * device goes away with UI_DEV_DESTROY or when the program closes the node,
 * so a virtual pad unplugs itself when its program quits or crashes.
 *
 * Uses: the harness's gamepad checks (buttons, axes, hot-plug, rumble and
 * player numbers without hardware), remapping tools, and Linux programs
 * under Renguin that make virtual devices.
 *
 * Differences from Linux: force feedback uploads are answered by the
 * gamepad core itself, so the program never sees UI_FF_UPLOAD requests; it
 * reads EV_FF play and stop events like on Linux, and UI_RENGA_GET_RUMBLE
 * returns the combined strength the core would send to a real pad.
 * UI_GET_SYSNAME gives "input<N>" for /dev/input/event<N>; there is no
 * sysfs. /dev/uinput (the other Linux name) is not published, because
 * Haiku's devfs loads drivers by directory.
 */


#include <errno.h>
#include <new>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <Drivers.h>
#include <KernelExport.h>
#include <Select.h>

#include <condition_variable.h>
#include <lock.h>
#include <util/AutoLock.h>

#include <gamepad_module.h>
#include <linux/uinput.h>


int32 api_version = B_CUR_DRIVER_API_VERSION;

#define UINPUT_DEVICE_NAME "input/uinput"
static const char* kDeviceName = UINPUT_DEVICE_NAME;
static const char* kDeviceNames[] = { UINPUT_DEVICE_NAME, NULL };
	// a literal: legacy drivers get no static constructors, so an array
	// initialised from another variable would stay empty
static const uint32 kQueueSize = 64;

static gamepad_module_info* sGamepad = NULL;


struct uinput_cookie {
	mutex				lock;
	bool				nonBlocking;
	gamepad_device_info	info;
	bool				setup;
	gamepad_device*		device;

	struct input_event	queue[kQueueSize];
	uint32				head;
	uint32				count;
	ConditionVariable	readable;
	selectsync*			readSync;

	uint16				strong;
	uint16				weak;
	uint32				playCount;
};


static status_t
copy_in(void* to, const void* from, size_t size)
{
	if (from == NULL)
		return B_BAD_ADDRESS;
	return user_memcpy(to, from, size);
}


static status_t
copy_out(void* to, const void* from, size_t size)
{
	if (to == NULL)
		return B_BAD_ADDRESS;
	return user_memcpy(to, from, size);
}


// Called with the cookie lock held.
static void
queue_event(uinput_cookie* cookie, uint16 type, uint16 code, int32 value)
{
	if (cookie->count == kQueueSize) {
		cookie->head = (cookie->head + 1) % kQueueSize;
		cookie->count--;
	}
	struct input_event& event
		= cookie->queue[(cookie->head + cookie->count) % kQueueSize];
	bigtime_t now = real_time_clock_usecs();
	event.time.tv_sec = now / 1000000;
	event.time.tv_usec = now % 1000000;
	event.type = type;
	event.code = code;
	event.value = value;
	cookie->count++;
	cookie->readable.NotifyAll();
	if (cookie->readSync != NULL)
		notify_select_event(cookie->readSync, B_SELECT_READ);
}


// #pragma mark - gamepad core hooks


static void
uinput_rumble(void* _cookie, uint16 strong, uint16 weak)
{
	uinput_cookie* cookie = (uinput_cookie*)_cookie;
	MutexLocker locker(cookie->lock);
	cookie->strong = strong;
	cookie->weak = weak;
}


static void
uinput_output_event(void* _cookie, uint16 type, uint16 code, int32 value)
{
	uinput_cookie* cookie = (uinput_cookie*)_cookie;
	MutexLocker locker(cookie->lock);
	if (type == EV_FF && code < FF_GAIN && value > 0)
		cookie->playCount++;
	queue_event(cookie, type, code, value);
	queue_event(cookie, EV_SYN, SYN_REPORT, 0);
}


static gamepad_device_hooks sHooks = {
	NULL,
	NULL,
	uinput_rumble,
	NULL,
	uinput_output_event
};


// #pragma mark - device hooks


static status_t
uinput_open(const char* name, uint32 flags, void** _cookie)
{
	uinput_cookie* cookie = new(std::nothrow) uinput_cookie;
	if (cookie == NULL)
		return B_NO_MEMORY;

	mutex_init(&cookie->lock, "uinput");
	cookie->nonBlocking = (flags & O_NONBLOCK) != 0;
	memset(&cookie->info, 0, sizeof(cookie->info));
	cookie->setup = false;
	cookie->device = NULL;
	cookie->head = cookie->count = 0;
	cookie->readable.Init(cookie, "uinput readable");
	cookie->readSync = NULL;
	cookie->strong = cookie->weak = 0;
	cookie->playCount = 0;
	*_cookie = cookie;
	return B_OK;
}


static void
destroy_device(uinput_cookie* cookie)
{
	gamepad_device* device;
	{
		MutexLocker locker(cookie->lock);
		device = cookie->device;
		cookie->device = NULL;
	}
	if (device != NULL)
		sGamepad->unregister_device(device);
}


static status_t
uinput_close(void* _cookie)
{
	destroy_device((uinput_cookie*)_cookie);
	return B_OK;
}


static status_t
uinput_free(void* _cookie)
{
	uinput_cookie* cookie = (uinput_cookie*)_cookie;
	mutex_destroy(&cookie->lock);
	delete cookie;
	return B_OK;
}


static status_t
uinput_read(void* _cookie, off_t position, void* buffer, size_t* _length)
{
	uinput_cookie* cookie = (uinput_cookie*)_cookie;
	size_t length = *_length;
	*_length = 0;
	if (length < sizeof(struct input_event))
		return B_BAD_VALUE;

	MutexLocker locker(cookie->lock);
	while (cookie->count == 0) {
		if (cookie->nonBlocking)
			return B_WOULD_BLOCK;
		ConditionVariableEntry entry;
		cookie->readable.Add(&entry);
		locker.Unlock();
		status_t status = entry.Wait(B_CAN_INTERRUPT);
		locker.Lock();
		if (status == B_INTERRUPTED)
			return status;
	}

	size_t done = 0;
	while (cookie->count > 0 && length - done >= sizeof(struct input_event)) {
		status_t status = copy_out((uint8*)buffer + done,
			&cookie->queue[cookie->head], sizeof(struct input_event));
		if (status != B_OK)
			return done > 0 ? B_OK : status;
		cookie->head = (cookie->head + 1) % kQueueSize;
		cookie->count--;
		done += sizeof(struct input_event);
	}
	*_length = done;
	return B_OK;
}


static status_t
uinput_write(void* _cookie, off_t position, const void* buffer,
	size_t* _length)
{
	uinput_cookie* cookie = (uinput_cookie*)_cookie;
	size_t length = *_length;
	*_length = 0;

	MutexLocker locker(cookie->lock);
	if (cookie->device == NULL) {
		// the old way of describing a device
		if (length != sizeof(struct uinput_user_dev))
			return B_BAD_VALUE;
		struct uinput_user_dev* user
			= (struct uinput_user_dev*)malloc(sizeof(*user));
		if (user == NULL)
			return B_NO_MEMORY;
		status_t status = copy_in(user, buffer, sizeof(*user));
		if (status == B_OK) {
			strlcpy(cookie->info.name, user->name, sizeof(cookie->info.name));
			cookie->info.id = user->id;
			for (int32 i = 0; i < ABS_CNT; i++) {
				cookie->info.abs[i].minimum = user->absmin[i];
				cookie->info.abs[i].maximum = user->absmax[i];
				cookie->info.abs[i].fuzz = user->absfuzz[i];
				cookie->info.abs[i].flat = user->absflat[i];
			}
			cookie->setup = true;
			*_length = length;
		}
		free(user);
		return status;
	}

	gamepad_device* device = cookie->device;
	locker.Unlock();

	if (length < sizeof(struct input_event))
		return B_BAD_VALUE;
	size_t done = 0;
	while (length - done >= sizeof(struct input_event)) {
		struct input_event event;
		status_t status = copy_in(&event, (const uint8*)buffer + done,
			sizeof(event));
		if (status != B_OK)
			return done > 0 ? B_OK : status;
		sGamepad->report(device, event.type, event.code, event.value);
		done += sizeof(event);
	}
	*_length = done;
	return B_OK;
}


static status_t
set_bit(uinput_cookie* cookie, uint8* bits, size_t size, void* value)
{
	uint32 bit = (uint32)(addr_t)value;
	MutexLocker locker(cookie->lock);
	if (cookie->device != NULL)
		return B_BUSY;
	if (bit >= size * 8)
		return B_BAD_VALUE;
	gamepad_set_bit(bits, bit);
	return B_OK;
}


static status_t
uinput_control(void* _cookie, uint32 op, void* buffer, size_t length)
{
	uinput_cookie* cookie = (uinput_cookie*)_cookie;
	gamepad_device_info& info = cookie->info;

	switch (op) {
		case B_SET_NONBLOCKING_IO:
			cookie->nonBlocking = true;
			return B_OK;
		case B_SET_BLOCKING_IO:
			cookie->nonBlocking = false;
			return B_OK;

		case UI_SET_EVBIT:
			return set_bit(cookie, info.ev_bits, sizeof(info.ev_bits), buffer);
		case UI_SET_KEYBIT:
			return set_bit(cookie, info.key_bits, sizeof(info.key_bits),
				buffer);
		case UI_SET_ABSBIT:
			return set_bit(cookie, info.abs_bits, sizeof(info.abs_bits),
				buffer);
		case UI_SET_FFBIT:
			return set_bit(cookie, info.ff_bits, sizeof(info.ff_bits), buffer);
		case UI_SET_PROPBIT:
			return set_bit(cookie, info.prop_bits, sizeof(info.prop_bits),
				buffer);
		case UI_SET_MSCBIT:
		case UI_SET_RELBIT:
		case UI_SET_LEDBIT:
		case UI_SET_SNDBIT:
		case UI_SET_SWBIT:
			// accepted, but gamepads do not use them
			return B_OK;

		case UI_SET_PHYS:
		{
			char phys[sizeof(info.phys)];
			if (buffer == NULL
				|| user_strlcpy(phys, (const char*)buffer, sizeof(phys)) < 0)
				return B_BAD_ADDRESS;
			MutexLocker locker(cookie->lock);
			strlcpy(info.phys, phys, sizeof(info.phys));
			return B_OK;
		}

		case UI_DEV_SETUP:
		{
			struct uinput_setup setup;
			if (copy_in(&setup, buffer, sizeof(setup)) != B_OK)
				return B_BAD_ADDRESS;
			MutexLocker locker(cookie->lock);
			if (cookie->device != NULL)
				return B_BUSY;
			info.id = setup.id;
			strlcpy(info.name, setup.name, sizeof(info.name));
			cookie->setup = true;
			return B_OK;
		}

		case UI_ABS_SETUP:
		{
			struct uinput_abs_setup setup;
			if (copy_in(&setup, buffer, sizeof(setup)) != B_OK)
				return B_BAD_ADDRESS;
			if (setup.code >= ABS_CNT)
				return B_BAD_VALUE;
			MutexLocker locker(cookie->lock);
			if (cookie->device != NULL)
				return B_BUSY;
			info.abs[setup.code] = setup.absinfo;
			return B_OK;
		}

		case UI_DEV_CREATE:
		{
			MutexLocker locker(cookie->lock);
			if (cookie->device != NULL)
				return B_BUSY;
			if (!cookie->setup || info.name[0] == '\0')
				return B_BAD_VALUE;
			gamepad_device_info copy = info;
			locker.Unlock();

			copy.flags = GAMEPAD_FLAG_VIRTUAL | GAMEPAD_FLAG_JOYSTICK_NODE;
			if (copy.id.bustype == 0)
				copy.id.bustype = BUS_VIRTUAL;
			if (copy.phys[0] == '\0')
				strlcpy(copy.phys, "virtual", sizeof(copy.phys));
			gamepad_device* device;
			status_t status = sGamepad->register_device(&copy, &sHooks,
				cookie, &device);
			if (status != B_OK)
				return status;
			locker.Lock();
			cookie->device = device;
			return B_OK;
		}

		case UI_DEV_DESTROY:
			destroy_device(cookie);
			return B_OK;

		case UI_GET_VERSION:
		{
			uint32 version = UINPUT_VERSION;
			return copy_out(buffer, &version, sizeof(version));
		}

		case UI_RENGA_GET_RUMBLE:
		{
			struct renga_uinput_rumble rumble;
			memset(&rumble, 0, sizeof(rumble));
			MutexLocker locker(cookie->lock);
			rumble.strong_magnitude = cookie->strong;
			rumble.weak_magnitude = cookie->weak;
			rumble.play_count = cookie->playCount;
			rumble.player = cookie->device != NULL
				? sGamepad->player(cookie->device) : 0;
			locker.Unlock();
			return copy_out(buffer, &rumble, sizeof(rumble));
		}

		case UI_RENGA_SET_BATTERY:
		{
			struct renga_input_battery battery;
			if (copy_in(&battery, buffer, sizeof(battery)) != B_OK)
				return B_BAD_ADDRESS;
			MutexLocker locker(cookie->lock);
			if (cookie->device == NULL)
				return B_BAD_VALUE;
			sGamepad->set_battery(cookie->device, battery.level,
				battery.charging != 0, battery.wired != 0);
			return B_OK;
		}
	}

	if (_LINUX_IOC_TYPE(op) == UINPUT_IOCTL_BASE && _LINUX_IOC_NR(op) == 44
		&& _LINUX_IOC_DIR(op) == _LINUX_IOC_READ) {
		// UI_GET_SYSNAME(len)
		MutexLocker locker(cookie->lock);
		if (cookie->device == NULL)
			return B_BAD_VALUE;
		char name[32];
		snprintf(name, sizeof(name), "input%" B_PRId32,
			sGamepad->index(cookie->device));
		locker.Unlock();
		size_t size = min_c(strlen(name) + 1, _LINUX_IOC_SIZE(op));
		if (size == 0)
			return B_BAD_VALUE;
		name[size - 1] = '\0';
		status_t status = copy_out(buffer, name, size);
		return status == B_OK ? (status_t)size : status;
	}

	return B_DEV_INVALID_IOCTL;
}


static status_t
uinput_select(void* _cookie, uint8 event, uint32 ref, selectsync* sync)
{
	uinput_cookie* cookie = (uinput_cookie*)_cookie;
	if (event == B_SELECT_WRITE)
		return notify_select_event(sync, event);
	if (event != B_SELECT_READ)
		return B_OK;
	MutexLocker locker(cookie->lock);
	cookie->readSync = sync;
	if (cookie->count > 0)
		notify_select_event(sync, B_SELECT_READ);
	return B_OK;
}


static status_t
uinput_deselect(void* _cookie, uint8 event, selectsync* sync)
{
	uinput_cookie* cookie = (uinput_cookie*)_cookie;
	MutexLocker locker(cookie->lock);
	if (event == B_SELECT_READ && cookie->readSync == sync)
		cookie->readSync = NULL;
	return B_OK;
}


static device_hooks sDeviceHooks = {
	uinput_open,
	uinput_close,
	uinput_free,
	uinput_control,
	uinput_read,
	uinput_write,
	uinput_select,
	uinput_deselect,
	NULL,
	NULL
};


// #pragma mark - driver


status_t
init_hardware()
{
	return B_OK;
}


status_t
init_driver()
{
	status_t status = get_module(GAMEPAD_MODULE_NAME,
		(module_info**)&sGamepad);
	if (status != B_OK) {
		dprintf("uinput: no gamepad core (%s): %s\n", GAMEPAD_MODULE_NAME,
			strerror(status));
	}
	return status;
}


void
uninit_driver()
{
	put_module(GAMEPAD_MODULE_NAME);
}


const char**
publish_devices()
{
	return kDeviceNames;
}


device_hooks*
find_device(const char* name)
{
	if (strcmp(name, kDeviceName) == 0)
		return &sDeviceHooks;
	return NULL;
}
