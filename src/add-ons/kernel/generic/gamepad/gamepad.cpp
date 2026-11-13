/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 *
 * The gamepad core (roadmap F-42): Linux evdev devices for gamepads.
 *
 * Drivers register pads through gamepad_module_info (<gamepad_module.h>);
 * this module publishes each one as /dev/input/event<N> and implements the
 * evdev interface on it the way Linux's input core and evdev handler do, so
 * SDL's Linux joystick back end, Chromium's gamepad code, libevdev and Linux
 * programs under Renguin can use it as they are:
 *
 *  - Each open descriptor (a "client") has its own queue of input_event
 *    records. A driver's events are collected into a packet and handed to
 *    the clients when the driver sends SYN_REPORT, so a reader never sees
 *    half a packet. A client that falls behind loses its queue and gets a
 *    SYN_DROPPED instead, after which it re-reads the state with EVIOCGKEY
 *    and EVIOCGABS (Linux behaviour).
 *  - Key and axis values that did not change are not passed on (Linux's
 *    input_handle_event() does the same), so drivers can report a whole
 *    pad state every time.
 *  - EVIOCGRAB gives one client all events.
 *  - Force feedback is "memoryless" like Linux's ff-memless, which xpad
 *    uses: clients upload FF_RUMBLE effects (and FF_PERIODIC/FF_CONSTANT,
 *    which are played as rumble of the same strength), start and stop them
 *    by writing EV_FF events, and the core combines all playing effects into
 *    one strong/weak strength for the driver, honouring each effect's replay
 *    delay and length and FF_GAIN. One kernel thread handles effect timing;
 *    it only wakes up while effects with a length are playing.
 *
 * Renga additions: a player number per pad (1-8, the lowest free one when it
 * connects), handed to the driver for the pad's LEDs; the battery level the
 * driver reports; both readable with Renga-only ioctls. Pads whose driver
 * asks for it also get a BJoystick node, /dev/joystick/gamepad/<N>, using
 * the variable_joystick layout libdevice's BJoystick reads.
 *
 * Locking: sLock protects the device table; each device has its own mutex
 * for its state, clients and effects. Hooks into the driver are called
 * without the device lock held, under the device's hook lock, which
 * unregister_device() takes as well, so no hook runs after it returned.
 */


#include <gamepad_module.h>

#include <errno.h>
#include <new>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <Drivers.h>
#include <KernelExport.h>
#include <Select.h>

#include <condition_variable.h>
#include <fs/devfs.h>
#include <kernel.h>
#include <lock.h>
#include <util/AutoLock.h>
#include <util/DoublyLinkedList.h>

#include <joystick_driver.h>


//#define TRACE_GAMEPAD
#ifdef TRACE_GAMEPAD
#	define TRACE(x...) dprintf("gamepad: " x)
#else
#	define TRACE(x...)
#endif
#define ERROR(x...) dprintf("gamepad: " x)


static const uint32 kQueueSize = 512;
	// events per client; Linux's evdev uses at least 64 and sizes it by
	// the device's event rate
static const uint32 kPacketSize = 128;
static const int32 kEvdevVersion = EV_VERSION;


struct gamepad_client;


struct ff_slot {
	bool			used;
	gamepad_client*	owner;
	struct ff_effect effect;
	bool			playing;
	bigtime_t		start;		// when it starts being felt (after the delay)
	bigtime_t		end;		// 0 = until stopped
	int32			repeats;	// plays left after this one
};


struct gamepad_client : DoublyLinkedListLinkImpl<gamepad_client> {
	gamepad_device*		device;
	bool				joystick;		// opened through the BJoystick node
	bool				nonBlocking;
	struct input_event	queue[kQueueSize];
	uint32				head;
	uint32				count;
	ConditionVariable	readable;
	selectsync*			readSync;
	bool				dropped;

	// BJoystick node only
	variable_joystick	joystickValues;
	joystick_module_info joystickInfo;
};

typedef DoublyLinkedList<gamepad_client> ClientList;


struct gamepad_device {
	int32				index;
	int32				refCount;	// table entry + one per client
	bool				removed;
	char				path[64];
	char				joystickPath[64];

	gamepad_device_info	info;
	gamepad_device_hooks hooks;
	void*				cookie;

	mutex				lock;
	mutex				hookLock;
	ClientList			clients;
	int32				openCount;
	gamepad_client*		grab;

	uint8				keyState[(KEY_CNT + 7) / 8];
	struct input_event	packet[kPacketSize];
	uint32				packetCount;

	ff_slot				effects[GAMEPAD_MAX_EFFECTS];
	uint16				gain;
	uint16				rumbleStrong;
	uint16				rumbleWeak;

	int32				player;
	int32				batteryLevel;
	bool				batteryCharging;
	bool				batteryWired;
};


static mutex sLock = MUTEX_INITIALIZER("gamepad devices");
static gamepad_device* sDevices[GAMEPAD_MAX_DEVICES];

static thread_id sEffectThread = -1;
static ConditionVariable sEffectCondition;
static bool sEffectThreadQuit = false;
static bigtime_t sNextEffectEvent = B_INFINITE_TIMEOUT;

extern device_hooks gEventHooks;
extern device_hooks gJoystickHooks;


// A pad that was unplugged answers ENODEV, as on Linux (SDL and libevdev
// test for it to notice a removed device).


// #pragma mark - helpers


static void
put_device(gamepad_device* device)
{
	if (atomic_add(&device->refCount, -1) != 1)
		return;

	TRACE("freeing device %" B_PRId32 "\n", device->index);
	mutex_destroy(&device->lock);
	mutex_destroy(&device->hookLock);
	delete device;
}


static gamepad_device*
find_device(const char* name, bool& joystick)
{
	MutexLocker locker(sLock);
	for (int32 i = 0; i < GAMEPAD_MAX_DEVICES; i++) {
		gamepad_device* device = sDevices[i];
		if (device == NULL)
			continue;
		if (strcmp(device->path, name) == 0)
			joystick = false;
		else if (device->joystickPath[0] != '\0'
			&& strcmp(device->joystickPath, name) == 0)
			joystick = true;
		else
			continue;
		atomic_add(&device->refCount, 1);
		return device;
	}
	return NULL;
}


// Arguments come from userland ioctl()s, read() and write(); user_memcpy()
// also copes with bad pointers (a fault returns an error instead of
// crashing), which a plain memcpy() would not.
static status_t
copy_out(void* to, const void* from, size_t size)
{
	if (to == NULL)
		return B_BAD_ADDRESS;
	return user_memcpy(to, from, size);
}


static status_t
copy_in(void* to, const void* from, size_t size)
{
	if (from == NULL)
		return B_BAD_ADDRESS;
	return user_memcpy(to, from, size);
}


static inline bool
test_bit(const uint8* bits, uint32 bit)
{
	return (bits[bit / 8] & (1 << (bit % 8))) != 0;
}


static inline void
change_bit(uint8* bits, uint32 bit, bool set)
{
	if (set)
		bits[bit / 8] |= (uint8)(1 << (bit % 8));
	else
		bits[bit / 8] &= (uint8)~(1 << (bit % 8));
}


// Called with the device lock held.
static void
client_push(gamepad_client* client, const struct input_event& event)
{
	if (client->count == kQueueSize) {
		// Linux: throw away what is queued and tell the reader
		struct input_event dropped = event;
		dropped.type = EV_SYN;
		dropped.code = SYN_DROPPED;
		dropped.value = 0;
		client->head = 0;
		client->count = 1;
		client->queue[0] = dropped;
		client->dropped = true;
		return;
	}

	client->queue[(client->head + client->count) % kQueueSize] = event;
	client->count++;
}


// Called with the device lock held.
static void
client_wake(gamepad_client* client)
{
	client->readable.NotifyAll();
	if (client->readSync != NULL)
		notify_select_event(client->readSync, B_SELECT_READ);
}


// #pragma mark - BJoystick node


static void
joystick_layout(gamepad_device* device, uint32& axes, uint32& hats,
	uint32& buttons)
{
	axes = hats = buttons = 0;
	for (uint32 code = 0; code < ABS_CNT; code++) {
		if (!test_bit(device->info.abs_bits, code))
			continue;
		if (code >= ABS_HAT0X && code <= ABS_HAT3Y) {
			if ((code - ABS_HAT0X) % 2 == 0)
				hats++;
		} else
			axes++;
	}
	for (uint32 code = BTN_MISC; code < KEY_CNT; code++) {
		if (test_bit(device->info.key_bits, code))
			buttons++;
	}
}


// Called with the device lock held. Fills the client's variable_joystick
// from the current state.
static void
joystick_update(gamepad_device* device, gamepad_client* client)
{
	variable_joystick& values = client->joystickValues;
	if (values.data == NULL)
		return;

	memset(values.data, 0, values.data_size);
	*values.timestamp = system_time();

	uint32 axis = 0, hat = 0;
	for (uint32 code = 0; code < ABS_CNT; code++) {
		if (!test_bit(device->info.abs_bits, code))
			continue;
		const struct input_absinfo& abs = device->info.abs[code];
		if (code >= ABS_HAT0X && code <= ABS_HAT3Y) {
			if ((code - ABS_HAT0X) % 2 != 0)
				continue;
			if (hat >= values.hat_count)
				continue;
			int32 x = device->info.abs[code].value;
			int32 y = code + 1 < ABS_CNT ? device->info.abs[code + 1].value
				: 0;
			// 0 centred, 1 = N, 2 = NE, ... 8 = NW
			static const uint8 kHat[3][3] = {
				{ 8, 1, 2 },	// y < 0: up
				{ 7, 0, 3 },
				{ 6, 5, 4 }		// y > 0: down
			};
			values.hats[hat++] = kHat[(y > 0) - (y < 0) + 1]
				[(x > 0) - (x < 0) + 1];
			continue;
		}
		if (axis >= values.axis_count)
			continue;
		int64 range = (int64)abs.maximum - abs.minimum;
		int32 value = 0;
		if (range > 0) {
			value = (int32)(((int64)abs.value - abs.minimum) * 65535 / range
				- 32768);
		}
		values.axes[axis++] = (int16)value;
	}

	uint32 button = 0;
	for (uint32 code = BTN_MISC; code < KEY_CNT
			&& button < values.button_blocks * 32; code++) {
		if (!test_bit(device->info.key_bits, code))
			continue;
		if (test_bit(device->keyState, code))
			values.buttons[button / 32] |= 1 << (button % 32);
		button++;
	}
}


// #pragma mark - force feedback


static uint16
scale(uint32 value, uint16 gain)
{
	return (uint16)(value * gain / 0xffff);
}


// Called with the device lock held. Works out the combined strength and
// the next time something changes; returns whether the strength changed.
static bool
ff_compute(gamepad_device* device, bigtime_t now, bigtime_t& next)
{
	uint32 strong = 0, weak = 0;
	next = B_INFINITE_TIMEOUT;

	for (int32 i = 0; i < GAMEPAD_MAX_EFFECTS; i++) {
		ff_slot& slot = device->effects[i];
		if (!slot.used || !slot.playing)
			continue;

		if (slot.end != 0 && now >= slot.end) {
			if (slot.repeats > 0) {
				slot.repeats--;
				slot.start = now + slot.effect.replay.delay * 1000LL;
				slot.end = slot.start + slot.effect.replay.length * 1000LL;
			} else {
				slot.playing = false;
				continue;
			}
		}
		if (now < slot.start) {
			if (slot.start < next)
				next = slot.start;
			continue;
		}
		if (slot.end != 0 && slot.end < next)
			next = slot.end;

		switch (slot.effect.type) {
			case FF_RUMBLE:
				strong += slot.effect.u.rumble.strong_magnitude;
				weak += slot.effect.u.rumble.weak_magnitude;
				break;
			case FF_PERIODIC:
			{
				// like ff-memless: the magnitude drives both motors
				int32 magnitude = slot.effect.u.periodic.magnitude;
				if (magnitude < 0)
					magnitude = -magnitude;
				strong += magnitude * 2;
				weak += magnitude * 2;
				break;
			}
			case FF_CONSTANT:
			{
				int32 level = slot.effect.u.constant.level;
				if (level < 0)
					level = -level;
				strong += level * 2;
				weak += level * 2;
				break;
			}
		}
	}

	if (strong > 0xffff)
		strong = 0xffff;
	if (weak > 0xffff)
		weak = 0xffff;
	uint16 newStrong = scale(strong, device->gain);
	uint16 newWeak = scale(weak, device->gain);
	bool changed = newStrong != device->rumbleStrong
		|| newWeak != device->rumbleWeak;
	device->rumbleStrong = newStrong;
	device->rumbleWeak = newWeak;
	return changed;
}


static void
ff_schedule(bigtime_t when)
{
	if (when == B_INFINITE_TIMEOUT)
		return;
	MutexLocker locker(sLock);
	if (when < sNextEffectEvent) {
		sNextEffectEvent = when;
		sEffectCondition.NotifyAll();
	}
}


// Recomputes the strength and tells the driver. Called without locks.
static void
ff_update(gamepad_device* device)
{
	bigtime_t next;
	uint16 strong, weak;
	bool changed;
	{
		MutexLocker locker(device->lock);
		changed = ff_compute(device, system_time(), next);
		strong = device->rumbleStrong;
		weak = device->rumbleWeak;
	}
	ff_schedule(next);

	if (!changed)
		return;

	MutexLocker hookLocker(device->hookLock);
	if (!device->removed && device->hooks.rumble != NULL)
		device->hooks.rumble(device->cookie, strong, weak);
}


static status_t
ff_effect_thread(void*)
{
	while (true) {
		{
			MutexLocker locker(sLock);
			if (sEffectThreadQuit)
				break;
			bigtime_t now = system_time();
			if (sNextEffectEvent > now) {
				ConditionVariableEntry entry;
				sEffectCondition.Add(&entry);
				bigtime_t when = sNextEffectEvent;
				locker.Unlock();
				if (when == B_INFINITE_TIMEOUT)
					entry.Wait();
				else
					entry.Wait(B_ABSOLUTE_TIMEOUT, when);
				continue;
			}
			sNextEffectEvent = B_INFINITE_TIMEOUT;
		}

		// run every device's effects
		gamepad_device* devices[GAMEPAD_MAX_DEVICES];
		int32 count = 0;
		{
			MutexLocker locker(sLock);
			for (int32 i = 0; i < GAMEPAD_MAX_DEVICES; i++) {
				if (sDevices[i] == NULL)
					continue;
				atomic_add(&sDevices[i]->refCount, 1);
				devices[count++] = sDevices[i];
			}
		}
		for (int32 i = 0; i < count; i++) {
			ff_update(devices[i]);
			put_device(devices[i]);
		}
	}
	return B_OK;
}


// Called with the device lock held.
static void
ff_erase_owned(gamepad_device* device, gamepad_client* owner)
{
	for (int32 i = 0; i < GAMEPAD_MAX_EFFECTS; i++) {
		ff_slot& slot = device->effects[i];
		if (slot.used && slot.owner == owner)
			memset(&slot, 0, sizeof(slot));
	}
}


// #pragma mark - evdev hooks


static status_t
event_open(const char* name, uint32 flags, void** _cookie)
{
	bool joystick = false;
	gamepad_device* device = find_device(name, joystick);
	if (device == NULL)
		return B_ENTRY_NOT_FOUND;

	gamepad_client* client = new(std::nothrow) gamepad_client;
	if (client == NULL) {
		put_device(device);
		return B_NO_MEMORY;
	}
	memset((void*)client->queue, 0, sizeof(client->queue));
	client->device = device;
	client->joystick = joystick;
	client->nonBlocking = (flags & O_NONBLOCK) != 0;
	client->head = client->count = 0;
	client->readSync = NULL;
	client->dropped = false;
	client->readable.Init(client, "evdev client");
	memset(&client->joystickValues, 0, sizeof(client->joystickValues));
	memset(&client->joystickInfo, 0, sizeof(client->joystickInfo));

	if (joystick) {
		uint32 axes, hats, buttons;
		joystick_layout(device, axes, hats, buttons);
		client->joystickValues.initialize(axes, hats, buttons);
		strlcpy(client->joystickInfo.device_name, device->info.name,
			sizeof(client->joystickInfo.device_name));
		strlcpy(client->joystickInfo.module_name, "gamepad",
			sizeof(client->joystickInfo.module_name));
		client->joystickInfo.num_axes = axes;
		client->joystickInfo.num_hats = hats;
		client->joystickInfo.num_buttons = buttons;
		client->joystickInfo.num_sticks = 1;
		client->joystickInfo.flags = js_flag_variable_size_reads;
	}

	bool first;
	{
		MutexLocker locker(device->lock);
		if (device->removed) {
			locker.Unlock();
			free(client->joystickValues.data);
			delete client;
			put_device(device);
			return ENODEV;
		}
		device->clients.Add(client);
		first = device->openCount++ == 0;
	}

	if (first) {
		MutexLocker hookLocker(device->hookLock);
		if (!device->removed && device->hooks.open != NULL) {
			status_t status = device->hooks.open(device->cookie);
			if (status != B_OK) {
				hookLocker.Unlock();
				MutexLocker locker(device->lock);
				device->clients.Remove(client);
				device->openCount--;
				locker.Unlock();
				free(client->joystickValues.data);
				delete client;
				put_device(device);
				return status;
			}
		}
	}

	*_cookie = client;
	return B_OK;
}


static status_t
event_close(void* cookie)
{
	gamepad_client* client = (gamepad_client*)cookie;
	gamepad_device* device = client->device;

	bool last, effectsChanged = false;
	{
		MutexLocker locker(device->lock);
		if (device->grab == client)
			device->grab = NULL;
		for (int32 i = 0; i < GAMEPAD_MAX_EFFECTS; i++) {
			if (device->effects[i].used && device->effects[i].owner == client)
				effectsChanged = true;
		}
		ff_erase_owned(device, client);
		last = --device->openCount == 0;
		client->readable.NotifyAll(B_FILE_ERROR);
	}
	if (effectsChanged)
		ff_update(device);

	if (last) {
		MutexLocker hookLocker(device->hookLock);
		if (!device->removed && device->hooks.close != NULL)
			device->hooks.close(device->cookie);
	}
	return B_OK;
}


static status_t
event_free(void* cookie)
{
	gamepad_client* client = (gamepad_client*)cookie;
	gamepad_device* device = client->device;
	{
		MutexLocker locker(device->lock);
		device->clients.Remove(client);
	}
	free(client->joystickValues.data);
	delete client;
	put_device(device);
	return B_OK;
}


static status_t
event_read(void* cookie, off_t position, void* buffer, size_t* _length)
{
	gamepad_client* client = (gamepad_client*)cookie;
	gamepad_device* device = client->device;
	size_t length = *_length;
	*_length = 0;

	if (client->joystick) {
		MutexLocker locker(device->lock);
		if (device->removed)
			return ENODEV;
		variable_joystick& values = client->joystickValues;
		if (values.data == NULL || length < values.data_size)
			return B_BUFFER_OVERFLOW;
		joystick_update(device, client);
		status_t status = copy_out(buffer, values.data, values.data_size);
		if (status != B_OK)
			return status;
		*_length = values.data_size;
		return B_OK;
	}

	if (length < sizeof(struct input_event))
		return B_BAD_VALUE;

	MutexLocker locker(device->lock);
	while (client->count == 0) {
		if (device->removed)
			return ENODEV;
		if (client->nonBlocking)
			return B_WOULD_BLOCK;

		ConditionVariableEntry entry;
		client->readable.Add(&entry);
		locker.Unlock();
		status_t status = entry.Wait(B_CAN_INTERRUPT);
		locker.Lock();
		if (status == B_INTERRUPTED)
			return B_INTERRUPTED;
		if (status != B_OK && client->count == 0)
			return device->removed ? ENODEV : status;
	}

	size_t done = 0;
	while (client->count > 0 && length - done >= sizeof(struct input_event)) {
		struct input_event event = client->queue[client->head];
		// copying to user memory with a mutex held is fine: it may fault,
		// but faults never take this lock
		status_t status = copy_out((uint8*)buffer + done, &event,
			sizeof(event));
		if (status != B_OK)
			return done > 0 ? B_OK : status;
		client->head = (client->head + 1) % kQueueSize;
		client->count--;
		done += sizeof(event);
		*_length = done;
	}
	return B_OK;
}


static status_t
handle_ff_play(gamepad_device* device, uint16 id, int32 value)
{
	{
		MutexLocker locker(device->lock);
		if (id >= GAMEPAD_MAX_EFFECTS || !device->effects[id].used)
			return B_BAD_VALUE;
		ff_slot& slot = device->effects[id];
		if (value > 0) {
			bigtime_t now = system_time();
			slot.playing = true;
			slot.repeats = value - 1;
			slot.start = now + slot.effect.replay.delay * 1000LL;
			slot.end = slot.effect.replay.length != 0
				? slot.start + slot.effect.replay.length * 1000LL : 0;
		} else
			slot.playing = false;
	}

	if ((device->info.flags & GAMEPAD_FLAG_VIRTUAL) != 0) {
		MutexLocker hookLocker(device->hookLock);
		if (!device->removed && device->hooks.output_event != NULL)
			device->hooks.output_event(device->cookie, EV_FF, id, value);
	}
	ff_update(device);
	return B_OK;
}


static status_t
event_write(void* cookie, off_t position, const void* buffer,
	size_t* _length)
{
	gamepad_client* client = (gamepad_client*)cookie;
	gamepad_device* device = client->device;
	size_t length = *_length;
	*_length = 0;

	if (client->joystick)
		return B_NOT_SUPPORTED;
	if (device->removed)
		return ENODEV;
	if (length < sizeof(struct input_event))
		return B_BAD_VALUE;

	size_t done = 0;
	while (length - done >= sizeof(struct input_event)) {
		struct input_event event;
		status_t status = copy_in(&event, (const uint8*)buffer + done,
			sizeof(event));
		if (status != B_OK)
			return done > 0 ? B_OK : status;
		done += sizeof(event);

		if (event.type == EV_FF) {
			if (event.code == FF_GAIN) {
				MutexLocker locker(device->lock);
				device->gain = (uint16)event.value;
				locker.Unlock();
				ff_update(device);
			} else if (event.code < FF_MAX_EFFECTS)
				handle_ff_play(device, event.code, event.value);
		} else if (event.type != EV_SYN) {
			MutexLocker hookLocker(device->hookLock);
			if (!device->removed && device->hooks.output_event != NULL) {
				device->hooks.output_event(device->cookie, event.type,
					event.code, event.value);
			}
		}
		*_length = done;
	}
	return B_OK;
}


static status_t
copy_bits(void* buffer, size_t length, const uint8* bits, size_t size)
{
	if (length > size)
		length = size;
	status_t status = copy_out(buffer, bits, length);
	return status == B_OK ? (status_t)length : status;
}


static status_t
copy_string(void* buffer, size_t length, const char* string)
{
	size_t size = strlen(string) + 1;
	if (length < size)
		size = length;
	if (size == 0)
		return 0;
	char copy[96];
	strlcpy(copy, string, sizeof(copy));
	if (size > sizeof(copy))
		size = sizeof(copy);
	copy[size - 1] = '\0';
	status_t status = copy_out(buffer, copy, size);
	return status == B_OK ? (status_t)size : status;
}


static status_t
joystick_control(gamepad_client* client, uint32 op, void* buffer,
	size_t length)
{
	switch (op) {
		case B_GET_DEVICE_NAME:
			return copy_string(buffer, length, client->device->info.name) >= 0
				? B_OK : B_BAD_ADDRESS;

		case B_JOYSTICK_SET_DEVICE_MODULE:
		{
			joystick_module_info info;
			if (length < sizeof(info))
				return B_BAD_VALUE;
			if (copy_in(&info, buffer, sizeof(info)) != B_OK)
				return B_BAD_ADDRESS;
			MutexLocker locker(client->device->lock);
			if ((info.flags & js_flag_variable_size_reads) == 0) {
				// the BeOS layout, see JoystickProtocolHandler
				free(client->joystickValues.data);
				client->joystickValues.data = NULL;
				client->joystickValues.initialize_to_extended_joystick();
				client->joystickInfo.flags &= ~js_flag_variable_size_reads;
				if (client->joystickInfo.num_axes > MAX_AXES)
					client->joystickInfo.num_axes = MAX_AXES;
				if (client->joystickInfo.num_hats > MAX_HATS)
					client->joystickInfo.num_hats = MAX_HATS;
				if (client->joystickInfo.num_buttons > MAX_BUTTONS)
					client->joystickInfo.num_buttons = MAX_BUTTONS;
			}
			return B_OK;
		}

		case B_JOYSTICK_GET_DEVICE_MODULE:
			if (length < sizeof(joystick_module_info))
				return B_BAD_VALUE;
			return copy_out(buffer, &client->joystickInfo,
				sizeof(joystick_module_info));
	}
	return B_DEV_INVALID_IOCTL;
}


static status_t
event_control(void* cookie, uint32 op, void* buffer, size_t length)
{
	gamepad_client* client = (gamepad_client*)cookie;
	gamepad_device* device = client->device;

	switch (op) {
		case B_SET_NONBLOCKING_IO:
			client->nonBlocking = true;
			return B_OK;
		case B_SET_BLOCKING_IO:
			client->nonBlocking = false;
			return B_OK;
	}

	if (client->joystick)
		return joystick_control(client, op, buffer, length);

	if (device->removed)
		return ENODEV;

	// Linux encodes the argument size in the number; Haiku's ioctl() with
	// three arguments passes a length of 0.
	uint32 size = _LINUX_IOC_SIZE(op);
	uint32 type = _LINUX_IOC_TYPE(op);
	uint32 nr = _LINUX_IOC_NR(op);
	uint32 dir = _LINUX_IOC_DIR(op);
	if (type != 'E')
		return B_DEV_INVALID_IOCTL;

	if (op == EVIOCGVERSION)
		return copy_out(buffer, &kEvdevVersion, sizeof(int32));
	if (op == EVIOCGID)
		return copy_out(buffer, &device->info.id, sizeof(struct input_id));
	if (op == EVIOCGEFFECTS) {
		int32 count = test_bit(device->info.ev_bits, EV_FF)
			? GAMEPAD_MAX_EFFECTS : 0;
		return copy_out(buffer, &count, sizeof(count));
	}
	if (op == EVIOCGRAB) {
		// the argument is the value itself, as on Linux
		bool grab = buffer != NULL;
		MutexLocker locker(device->lock);
		if (grab) {
			if (device->grab != NULL)
				return B_BUSY;
			device->grab = client;
		} else {
			if (device->grab != client)
				return B_BAD_VALUE;
			device->grab = NULL;
		}
		return B_OK;
	}
	if (op == EVIOCREVOKE)
		return B_NOT_SUPPORTED;
	if (op == EVIOCSCLOCKID)
		return B_OK;
	if (op == EVIOC_RENGA_GPLAYER)
		return copy_out(buffer, &device->player, sizeof(int32));
	if (op == EVIOC_RENGA_SPLAYER) {
		int32 player;
		if (copy_in(&player, buffer, sizeof(player)) != B_OK)
			return B_BAD_ADDRESS;
		if (player < 0 || player > GAMEPAD_MAX_PLAYERS)
			return B_BAD_VALUE;
		device->player = player;
		MutexLocker hookLocker(device->hookLock);
		if (!device->removed && device->hooks.set_player != NULL)
			device->hooks.set_player(device->cookie, player);
		return B_OK;
	}
	if (op == EVIOC_RENGA_GBATTERY) {
		struct renga_input_battery battery;
		memset(&battery, 0, sizeof(battery));
		battery.level = device->batteryLevel;
		battery.charging = device->batteryCharging;
		battery.wired = device->batteryWired;
		return copy_out(buffer, &battery, sizeof(battery));
	}
	if (op == EVIOCRMFF) {
		int32 id = (int32)(addr_t)buffer;
		{
			MutexLocker locker(device->lock);
			if (id < 0 || id >= GAMEPAD_MAX_EFFECTS
				|| !device->effects[id].used)
				return B_BAD_VALUE;
			memset(&device->effects[id], 0, sizeof(ff_slot));
		}
		ff_update(device);
		return B_OK;
	}
	if (op == EVIOCSFF) {
		if (!test_bit(device->info.ev_bits, EV_FF))
			return B_NOT_SUPPORTED;
		struct ff_effect effect;
		if (copy_in(&effect, buffer, sizeof(effect)) != B_OK)
			return B_BAD_ADDRESS;
		if (effect.type < FF_EFFECT_MIN || effect.type > FF_EFFECT_MAX
			|| !test_bit(device->info.ff_bits, effect.type))
			return B_BAD_VALUE;
		bool playing = false;
		{
			MutexLocker locker(device->lock);
			int32 id = effect.id;
			if (id == -1) {
				for (id = 0; id < GAMEPAD_MAX_EFFECTS; id++) {
					if (!device->effects[id].used)
						break;
				}
				if (id == GAMEPAD_MAX_EFFECTS)
					return B_NO_MEMORY;
			} else if (id < 0 || id >= GAMEPAD_MAX_EFFECTS
				|| !device->effects[id].used
				|| device->effects[id].owner != client)
				return B_BAD_VALUE;
			effect.id = id;
			ff_slot& slot = device->effects[id];
			playing = slot.used && slot.playing;
			slot.used = true;
			slot.owner = client;
			slot.effect = effect;
		}
		if (copy_out(buffer, &effect, sizeof(effect)) != B_OK)
			return B_BAD_ADDRESS;
		if (playing)
			ff_update(device);
		return B_OK;
	}

	if (dir != _LINUX_IOC_READ)
		return B_DEV_INVALID_IOCTL;

	switch (nr) {
		case 0x06:
			return copy_string(buffer, size, device->info.name);
		case 0x07:
			return copy_string(buffer, size, device->info.phys);
		case 0x08:
			return copy_string(buffer, size, device->info.uniq);
		case 0x09:
			return copy_bits(buffer, size, device->info.prop_bits,
				sizeof(device->info.prop_bits));
		case 0x18:
		{
			MutexLocker locker(device->lock);
			uint8 state[sizeof(device->keyState)];
			memcpy(state, device->keyState, sizeof(state));
			locker.Unlock();
			return copy_bits(buffer, size, state, sizeof(state));
		}
		case 0x19:
		case 0x1a:
		case 0x1b:
		{
			uint8 none[8] = {};
			return copy_bits(buffer, size, none, sizeof(none));
		}
	}

	if (nr >= 0x20 && nr < 0x20 + EV_CNT) {
		uint8 none[(KEY_CNT + 7) / 8] = {};
		switch (nr - 0x20) {
			case 0:
				return copy_bits(buffer, size, device->info.ev_bits,
					sizeof(device->info.ev_bits));
			case EV_KEY:
				return copy_bits(buffer, size, device->info.key_bits,
					sizeof(device->info.key_bits));
			case EV_ABS:
				return copy_bits(buffer, size, device->info.abs_bits,
					sizeof(device->info.abs_bits));
			case EV_FF:
				return copy_bits(buffer, size, device->info.ff_bits,
					sizeof(device->info.ff_bits));
			default:
				return copy_bits(buffer, size, none, sizeof(none));
		}
	}

	if (nr >= 0x40 && nr < 0x40 + ABS_CNT
		&& size >= sizeof(struct input_absinfo)) {
		uint32 code = nr - 0x40;
		MutexLocker locker(device->lock);
		struct input_absinfo abs = device->info.abs[code];
		locker.Unlock();
		return copy_out(buffer, &abs, sizeof(abs));
	}

	return B_DEV_INVALID_IOCTL;
}


static status_t
event_select(void* cookie, uint8 event, uint32 ref, selectsync* sync)
{
	gamepad_client* client = (gamepad_client*)cookie;
	gamepad_device* device = client->device;

	if (event != B_SELECT_READ) {
		if (event == B_SELECT_WRITE)
			return notify_select_event(sync, event);
		return B_OK;
	}

	MutexLocker locker(device->lock);
	client->readSync = sync;
	if (client->count > 0 || device->removed || client->joystick)
		notify_select_event(sync, B_SELECT_READ);
	return B_OK;
}


static status_t
event_deselect(void* cookie, uint8 event, selectsync* sync)
{
	gamepad_client* client = (gamepad_client*)cookie;
	MutexLocker locker(client->device->lock);
	if (event == B_SELECT_READ && client->readSync == sync)
		client->readSync = NULL;
	return B_OK;
}


device_hooks gEventHooks = {
	event_open,
	event_close,
	event_free,
	event_control,
	event_read,
	event_write,
	event_select,
	event_deselect,
	NULL,
	NULL
};


// #pragma mark - module API


static void
assign_player(gamepad_device* device)
{
	// called with sLock held
	if ((device->info.flags & GAMEPAD_FLAG_NO_PLAYER) != 0) {
		device->player = 0;
		return;
	}
	for (int32 player = 1; player <= GAMEPAD_MAX_PLAYERS; player++) {
		bool used = false;
		for (int32 i = 0; i < GAMEPAD_MAX_DEVICES; i++) {
			if (sDevices[i] != NULL && sDevices[i] != device
				&& sDevices[i]->player == player)
				used = true;
		}
		if (!used) {
			device->player = player;
			return;
		}
	}
	device->player = 0;
}


static status_t
gamepad_register_device(const gamepad_device_info* info,
	const gamepad_device_hooks* hooks, void* cookie,
	gamepad_device** _device)
{
	gamepad_device* device = new(std::nothrow) gamepad_device;
	if (device == NULL)
		return B_NO_MEMORY;

	memset((void*)device, 0, sizeof(*device));
	new(&device->clients) ClientList;
	device->info = *info;
	device->info.name[sizeof(device->info.name) - 1] = '\0';
	device->info.phys[sizeof(device->info.phys) - 1] = '\0';
	device->info.uniq[sizeof(device->info.uniq) - 1] = '\0';
	gamepad_set_bit(device->info.ev_bits, EV_SYN);
	if (hooks != NULL)
		device->hooks = *hooks;
	device->cookie = cookie;
	device->refCount = 1;
	device->gain = 0xffff;
	device->batteryLevel = -1;
	mutex_init(&device->lock, "gamepad device");
	mutex_init(&device->hookLock, "gamepad device hooks");

	MutexLocker locker(sLock);
	int32 index = -1;
	for (int32 i = 0; i < GAMEPAD_MAX_DEVICES; i++) {
		if (sDevices[i] == NULL) {
			index = i;
			break;
		}
	}
	if (index < 0) {
		locker.Unlock();
		put_device(device);
		return B_BUSY;
	}

	device->index = index;
	snprintf(device->path, sizeof(device->path), "input/event%" B_PRId32,
		index);
	if ((info->flags & GAMEPAD_FLAG_JOYSTICK_NODE) != 0) {
		snprintf(device->joystickPath, sizeof(device->joystickPath),
			"joystick/gamepad/%" B_PRId32, index);
	}
	assign_player(device);
	sDevices[index] = device;
	locker.Unlock();

	status_t status = devfs_publish_device(device->path, &gEventHooks);
	if (status == B_OK && device->joystickPath[0] != '\0') {
		if (devfs_publish_device(device->joystickPath, &gEventHooks)
				!= B_OK)
			device->joystickPath[0] = '\0';
	}
	if (status != B_OK) {
		ERROR("could not publish %s: %s\n", device->path, strerror(status));
		locker.Lock();
		sDevices[index] = NULL;
		locker.Unlock();
		put_device(device);
		return status;
	}

	dprintf("gamepad: %s is \"%s\" (%04x:%04x), player %" B_PRId32 "\n",
		device->path, device->info.name, device->info.id.vendor,
		device->info.id.product, device->player);

	if (device->hooks.set_player != NULL) {
		MutexLocker hookLocker(device->hookLock);
		device->hooks.set_player(device->cookie, device->player);
	}

	*_device = device;
	return B_OK;
}


static void
gamepad_unregister_device(gamepad_device* device)
{
	if (device == NULL)
		return;

	{
		MutexLocker locker(sLock);
		if (sDevices[device->index] == device)
			sDevices[device->index] = NULL;
	}

	{
		MutexLocker hookLocker(device->hookLock);
		MutexLocker locker(device->lock);
		device->removed = true;
		ClientList::Iterator iterator = device->clients.GetIterator();
		while (gamepad_client* client = iterator.Next()) {
			client->readable.NotifyAll(ENODEV);
			if (client->readSync != NULL) {
				notify_select_event(client->readSync, B_SELECT_READ);
				notify_select_event(client->readSync, B_SELECT_ERROR);
			}
		}
	}

	devfs_unpublish_device(device->path, false);
	if (device->joystickPath[0] != '\0')
		devfs_unpublish_device(device->joystickPath, false);

	dprintf("gamepad: %s removed\n", device->path);
	put_device(device);
}


static void
gamepad_report(gamepad_device* device, uint16 type, uint16 code,
	int32 value)
{
	MutexLocker locker(device->lock);
	if (device->removed)
		return;

	switch (type) {
		case EV_KEY:
			if (code >= KEY_CNT || !test_bit(device->info.key_bits, code))
				return;
			value = value != 0 ? 1 : 0;
			if (test_bit(device->keyState, code) == (value != 0))
				return;
			change_bit(device->keyState, code, value != 0);
			break;

		case EV_ABS:
		{
			if (code >= ABS_CNT || !test_bit(device->info.abs_bits, code))
				return;
			struct input_absinfo& abs = device->info.abs[code];
			if (abs.value == value)
				return;
			if (abs.fuzz > 0) {
				// Linux's input_defuzz_abs_event(), first step
				int32 delta = value - abs.value;
				if (delta < 0)
					delta = -delta;
				if (delta < abs.fuzz / 2)
					return;
			}
			abs.value = value;
			break;
		}

		case EV_SYN:
			if (code != SYN_REPORT)
				return;
			break;

		case EV_MSC:
			break;

		default:
			return;
	}

	struct input_event event;
	bigtime_t now = real_time_clock_usecs();
	event.time.tv_sec = now / 1000000;
	event.time.tv_usec = now % 1000000;
	event.type = type;
	event.code = code;
	event.value = value;

	if (type != EV_SYN) {
		if (device->packetCount < kPacketSize)
			device->packet[device->packetCount++] = event;
		return;
	}

	if (device->packetCount == 0)
		return;

	// hand the packet over
	ClientList::Iterator iterator = device->clients.GetIterator();
	while (gamepad_client* client = iterator.Next()) {
		if (client->joystick)
			continue;
		if (device->grab != NULL && device->grab != client)
			continue;
		for (uint32 i = 0; i < device->packetCount; i++) {
			device->packet[i].time = event.time;
			client_push(client, device->packet[i]);
		}
		client_push(client, event);
		client_wake(client);
	}
	device->packetCount = 0;
}


static void
gamepad_set_battery(gamepad_device* device, int32 level, bool charging,
	bool wired)
{
	MutexLocker locker(device->lock);
	device->batteryLevel = level;
	device->batteryCharging = charging;
	device->batteryWired = wired;
}


static int32
gamepad_player(gamepad_device* device)
{
	return device->player;
}


static int32
gamepad_index(gamepad_device* device)
{
	return device->index;
}


static status_t
gamepad_std_ops(int32 op, ...)
{
	switch (op) {
		case B_MODULE_INIT:
			memset(sDevices, 0, sizeof(sDevices));
			sEffectCondition.Init(&sEffectCondition, "gamepad effects");
			sEffectThreadQuit = false;
			sNextEffectEvent = B_INFINITE_TIMEOUT;
			sEffectThread = spawn_kernel_thread(ff_effect_thread,
				"gamepad effects", B_NORMAL_PRIORITY, NULL);
			if (sEffectThread < 0)
				return sEffectThread;
			resume_thread(sEffectThread);
			return B_OK;

		case B_MODULE_UNINIT:
		{
			{
				MutexLocker locker(sLock);
				sEffectThreadQuit = true;
				sEffectCondition.NotifyAll();
			}
			status_t result;
			wait_for_thread(sEffectThread, &result);
			sEffectThread = -1;
			return B_OK;
		}
	}
	return B_ERROR;
}


static gamepad_module_info sGamepadModule = {
	{
		GAMEPAD_MODULE_NAME,
		0,
		gamepad_std_ops
	},

	gamepad_register_device,
	gamepad_unregister_device,
	gamepad_report,
	gamepad_set_battery,
	gamepad_player,
	gamepad_index
};


module_info* modules[] = {
	(module_info*)&sGamepadModule,
	NULL
};
