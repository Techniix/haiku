/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 */

/*!	Gamepads (roadmap F-42) without gamepad hardware: virtual pads made
	through uinput (/dev/input/uinput) stand in for real ones, set up the
	way the xinput driver sets up an 8BitDo Ultimate 2C in X-input mode
	(2dc8:310a, version 0x0114, xpad's buttons and axes), so the SDL
	database entry for that pad applies.

	Checked: the uinput interface; the evdev device the gamepad core makes
	of it (identity and capability ioctls, events in whole packets,
	unchanged values dropped, select(), the key state, SYN_DROPPED when a
	reader falls behind, EVIOCGRAB); hot-plug as SDL and other Linux code
	see it (inotify on /dev/input) and as Haiku code sees it (node
	monitoring through BGamepad::StartWatching()); force feedback
	(upload, play, the combined strength, replay length, gain, erase);
	player numbers (lowest free, reused); the battery level; the device
	going away while open and when its program closes uinput; the
	BJoystick node; BGamepad with the database mapping, rumble and player
	number; and that no hidraw node exists for QEMU's USB keyboard and
	tablet (raw access is for game controllers only).
*/


#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/inotify.h>
#include <unistd.h>

#include <Application.h>
#include <Directory.h>
#include <Entry.h>
#include <Gamepad.h>
#include <Joystick.h>
#include <Looper.h>
#include <Message.h>
#include <OS.h>
#include <String.h>

#include <linux/input.h>
#include <linux/uinput.h>


#define SUITE "gamepad"

static int sFailures = 0;


static void
report(const char* name, bool pass, const char* format, ...)
{
	char detail[256];
	va_list args;
	va_start(args, format);
	vsnprintf(detail, sizeof(detail), format, args);
	va_end(args);
	printf("CE-TEST %s.%s %s %s\n", SUITE, name, pass ? "PASS" : "FAIL",
		detail);
	fflush(stdout);
	if (!pass)
		sFailures++;
}


// Linux code passes plain integers as the third ioctl() argument; Haiku's
// C++ ioctl() takes a pointer there, so C++ code casts them.
#define ioctl_value(fd, op, value) ioctl((fd), (op), (void*)(addr_t)(value))


static inline bool
test_bit(const uint8* bits, uint32 bit)
{
	return (bits[bit / 8] & (1 << (bit % 8))) != 0;
}


static const uint16 kButtons[] = { BTN_A, BTN_B, BTN_X, BTN_Y, BTN_TL,
	BTN_TR, BTN_SELECT, BTN_START, BTN_MODE, BTN_THUMBL, BTN_THUMBR };


struct VirtualPad {
	int		fd;
	int		event;		// N of /dev/input/event<N>
	char	path[64];
};


static bool
create_pad(VirtualPad& pad, const char* name, bool withFF = true)
{
	pad.fd = open("/dev/input/uinput", O_RDWR | O_NONBLOCK);
	pad.event = -1;
	if (pad.fd < 0) {
		printf("CE-INFO gamepad: open /dev/input/uinput: %s\n",
			strerror(errno));
		return false;
	}

	ioctl_value(pad.fd, UI_SET_EVBIT, EV_KEY);
	ioctl_value(pad.fd, UI_SET_EVBIT, EV_ABS);
	for (size_t i = 0; i < sizeof(kButtons) / sizeof(kButtons[0]); i++)
		ioctl_value(pad.fd, UI_SET_KEYBIT, kButtons[i]);
	struct { uint16 code; int32 min, max, fuzz, flat; } axes[] = {
		{ ABS_X, -32768, 32767, 16, 128 }, { ABS_Y, -32768, 32767, 16, 128 },
		{ ABS_Z, 0, 255, 0, 0 }, { ABS_RX, -32768, 32767, 16, 128 },
		{ ABS_RY, -32768, 32767, 16, 128 }, { ABS_RZ, 0, 255, 0, 0 },
		{ ABS_HAT0X, -1, 1, 0, 0 }, { ABS_HAT0Y, -1, 1, 0, 0 }
	};
	for (size_t i = 0; i < sizeof(axes) / sizeof(axes[0]); i++) {
		ioctl_value(pad.fd, UI_SET_ABSBIT, axes[i].code);
		struct uinput_abs_setup setup;
		memset(&setup, 0, sizeof(setup));
		setup.code = axes[i].code;
		setup.absinfo.minimum = axes[i].min;
		setup.absinfo.maximum = axes[i].max;
		setup.absinfo.fuzz = axes[i].fuzz;
		setup.absinfo.flat = axes[i].flat;
		ioctl(pad.fd, UI_ABS_SETUP, &setup);
	}
	if (withFF) {
		ioctl_value(pad.fd, UI_SET_EVBIT, EV_FF);
		ioctl_value(pad.fd, UI_SET_FFBIT, FF_RUMBLE);
		ioctl_value(pad.fd, UI_SET_FFBIT, FF_GAIN);
	}

	struct uinput_setup setup;
	memset(&setup, 0, sizeof(setup));
	setup.id.bustype = BUS_USB;
	setup.id.vendor = 0x2dc8;
	setup.id.product = 0x310a;
	setup.id.version = 0x0114;
	strlcpy(setup.name, name, sizeof(setup.name));
	if (ioctl(pad.fd, UI_DEV_SETUP, &setup) < 0
		|| ioctl(pad.fd, UI_DEV_CREATE) < 0) {
		printf("CE-INFO gamepad: UI_DEV_SETUP/UI_DEV_CREATE: %s\n",
			strerror(errno));
		close(pad.fd);
		pad.fd = -1;
		return false;
	}

	char sysname[32] = "";
	if (ioctl(pad.fd, UI_GET_SYSNAME(sizeof(sysname)), sysname) < 0
		|| strncmp(sysname, "input", 5) != 0) {
		printf("CE-INFO gamepad: UI_GET_SYSNAME: %s (\"%s\")\n",
			strerror(errno), sysname);
		close(pad.fd);
		pad.fd = -1;
		return false;
	}
	pad.event = atoi(sysname + 5);
	snprintf(pad.path, sizeof(pad.path), "/dev/input/event%d", pad.event);
	return true;
}


static void
emit(int fd, uint16 type, uint16 code, int32 value)
{
	struct input_event event;
	memset(&event, 0, sizeof(event));
	event.type = type;
	event.code = code;
	event.value = value;
	write(fd, &event, sizeof(event));
}


// Reads what is queued (non-blocking descriptor).
static int
read_events(int fd, struct input_event* events, int max)
{
	ssize_t bytes = read(fd, events, max * sizeof(struct input_event));
	return bytes > 0 ? bytes / (ssize_t)sizeof(struct input_event) : 0;
}


static bool
wait_readable(int fd, int milliseconds)
{
	struct pollfd poller = { fd, POLLIN, 0 };
	return poll(&poller, 1, milliseconds) == 1
		&& (poller.revents & POLLIN) != 0;
}


static void
test_uinput_and_evdev()
{
	VirtualPad pad;
	bool created = create_pad(pad, "8BitDo Ultimate 2C Wireless Controller");
	report("uinput_create", created, "event%d", pad.event);
	if (!created)
		return;

	int fd = open(pad.path, O_RDWR | O_NONBLOCK);
	report("evdev_node", fd >= 0, "%s: %s", pad.path,
		fd >= 0 ? "opened" : strerror(errno));
	if (fd < 0) {
		close(pad.fd);
		return;
	}

	int version = 0;
	struct input_id id;
	memset(&id, 0, sizeof(id));
	char name[128] = "";
	bool identity = ioctl(fd, EVIOCGVERSION, &version) >= 0
		&& ioctl(fd, EVIOCGID, &id) >= 0
		&& ioctl(fd, EVIOCGNAME(sizeof(name)), name) > 0;
	report("evdev_identity", identity && version == EV_VERSION
		&& id.bustype == BUS_USB && id.vendor == 0x2dc8
		&& id.product == 0x310a && id.version == 0x0114
		&& strcmp(name, "8BitDo Ultimate 2C Wireless Controller") == 0,
		"version %#x, %04x:%04x:%04x:%04x, \"%s\"", version, id.bustype,
		id.vendor, id.product, id.version, name);

	uint8 evBits[(EV_CNT + 7) / 8] = {};
	uint8 keyBits[(KEY_CNT + 7) / 8] = {};
	uint8 absBits[(ABS_CNT + 7) / 8] = {};
	int keyBytes = ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(keyBits)), keyBits);
	ioctl(fd, EVIOCGBIT(0, sizeof(evBits)), evBits);
	ioctl(fd, EVIOCGBIT(EV_ABS, sizeof(absBits)), absBits);
	bool keysOK = true;
	for (size_t i = 0; i < sizeof(kButtons) / sizeof(kButtons[0]); i++)
		keysOK = keysOK && test_bit(keyBits, kButtons[i]);
	keysOK = keysOK && !test_bit(keyBits, BTN_TL2);
	report("evdev_capabilities", keysOK && keyBytes == (int)sizeof(keyBits)
		&& test_bit(evBits, EV_KEY) && test_bit(evBits, EV_ABS)
		&& test_bit(evBits, EV_FF) && test_bit(absBits, ABS_HAT0Y)
		&& !test_bit(absBits, ABS_THROTTLE),
		"returned %d bytes of key bits", keyBytes);

	struct input_absinfo absinfo;
	memset(&absinfo, 0, sizeof(absinfo));
	ioctl(fd, EVIOCGABS(ABS_Z), &absinfo);
	report("evdev_absinfo", absinfo.minimum == 0 && absinfo.maximum == 255,
		"ABS_Z %d..%d", absinfo.minimum, absinfo.maximum);

	// a packet without SYN_REPORT is not visible yet
	struct input_event events[700];
	emit(pad.fd, EV_KEY, BTN_A, 1);
	emit(pad.fd, EV_ABS, ABS_X, 12000);
	int early = read_events(fd, events, 700);
	emit(pad.fd, EV_SYN, SYN_REPORT, 0);
	bool ready = wait_readable(fd, 1000);
	int count = read_events(fd, events, 700);
	bool packet = count == 3 && events[0].type == EV_KEY
		&& events[0].code == BTN_A && events[0].value == 1
		&& events[1].type == EV_ABS && events[1].code == ABS_X
		&& events[1].value == 12000 && events[2].type == EV_SYN
		&& events[2].code == SYN_REPORT
		&& events[0].time.tv_sec == events[2].time.tv_sec
		&& events[0].time.tv_usec == events[2].time.tv_usec;
	report("evdev_packet", early == 0 && ready && packet,
		"%d events before SYN_REPORT, %d after, poll %s", early, count,
		ready ? "readable" : "not readable");

	// unchanged values are dropped; a packet of only those is no packet
	emit(pad.fd, EV_KEY, BTN_A, 1);
	emit(pad.fd, EV_ABS, ABS_X, 12000);
	emit(pad.fd, EV_SYN, SYN_REPORT, 0);
	emit(pad.fd, EV_ABS, ABS_X, 12004);	// within the fuzz of 16
	emit(pad.fd, EV_SYN, SYN_REPORT, 0);
	snooze(50000);
	count = read_events(fd, events, 700);
	report("evdev_unchanged_dropped", count == 0, "%d events", count);

	uint8 keyState[(KEY_CNT + 7) / 8] = {};
	ioctl(fd, EVIOCGKEY(sizeof(keyState)), keyState);
	memset(&absinfo, 0, sizeof(absinfo));
	ioctl(fd, EVIOCGABS(ABS_X), &absinfo);
	report("evdev_state", test_bit(keyState, BTN_A)
		&& !test_bit(keyState, BTN_B) && absinfo.value == 12000,
		"BTN_A %d, ABS_X %d", test_bit(keyState, BTN_A), absinfo.value);

	// a reader that falls behind gets SYN_DROPPED
	for (int i = 0; i < 400; i++) {
		emit(pad.fd, EV_ABS, ABS_RX, (i % 2) ? 20000 : -20000);
		emit(pad.fd, EV_SYN, SYN_REPORT, 0);
	}
	count = read_events(fd, events, 700);
	bool dropped = false;
	for (int i = 0; i < count; i++) {
		if (events[i].type == EV_SYN && events[i].code == SYN_DROPPED)
			dropped = true;
	}
	report("evdev_syn_dropped", dropped && count <= 512,
		"%d events, SYN_DROPPED %s", count, dropped ? "seen" : "missing");
	while (read_events(fd, events, 700) > 0)
		;

	// EVIOCGRAB: only the grabbing descriptor gets events
	int other = open(pad.path, O_RDONLY | O_NONBLOCK);
	int grab = ioctl_value(fd, EVIOCGRAB, 1);
	int secondGrab = ioctl_value(other, EVIOCGRAB, 1);
	emit(pad.fd, EV_KEY, BTN_B, 1);
	emit(pad.fd, EV_SYN, SYN_REPORT, 0);
	snooze(20000);
	int grabbed = read_events(fd, events, 700);
	int notGrabbed = read_events(other, events, 700);
	ioctl_value(fd, EVIOCGRAB, 0);
	emit(pad.fd, EV_KEY, BTN_B, 0);
	emit(pad.fd, EV_SYN, SYN_REPORT, 0);
	snooze(20000);
	int released = read_events(other, events, 700);
	report("evdev_grab", grab == 0 && secondGrab < 0 && grabbed == 2
		&& notGrabbed == 0 && released == 2,
		"grab %d, second grab %d, events %d/%d, after release %d", grab,
		secondGrab, grabbed, notGrabbed, released);
	close(other);
	while (read_events(fd, events, 700) > 0)
		;

	// force feedback
	int effects = 0;
	ioctl(fd, EVIOCGEFFECTS, &effects);
	struct ff_effect effect;
	memset(&effect, 0, sizeof(effect));
	effect.type = FF_RUMBLE;
	effect.id = -1;
	effect.u.rumble.strong_magnitude = 0x8000;
	effect.u.rumble.weak_magnitude = 0x4000;
	effect.replay.length = 300;
	int upload = ioctl(fd, EVIOCSFF, &effect);
	report("ff_upload", upload == 0 && effect.id >= 0 && effects > 0,
		"result %d, id %d, %d effects", upload, effect.id, effects);

	emit(fd, EV_FF, effect.id, 1);
	snooze(30000);
	struct renga_uinput_rumble rumble;
	memset(&rumble, 0, sizeof(rumble));
	ioctl(pad.fd, UI_RENGA_GET_RUMBLE, &rumble);
	int uinputEvents = read_events(pad.fd, events, 700);
	bool playSeen = false;
	for (int i = 0; i < uinputEvents; i++) {
		if (events[i].type == EV_FF && events[i].code == effect.id
			&& events[i].value == 1)
			playSeen = true;
	}
	report("ff_play", rumble.strong_magnitude == 0x8000
		&& rumble.weak_magnitude == 0x4000 && playSeen,
		"strength %#x/%#x, play event %s", rumble.strong_magnitude,
		rumble.weak_magnitude, playSeen ? "seen" : "missing");

	bigtime_t start = system_time();
	do {
		snooze(20000);
		ioctl(pad.fd, UI_RENGA_GET_RUMBLE, &rumble);
	} while (rumble.strong_magnitude != 0 && system_time() - start < 3000000);
	bigtime_t stoppedAfter = system_time() - start;
	report("ff_replay_length", rumble.strong_magnitude == 0
		&& stoppedAfter > 150000,
		"stopped after %" B_PRIdBIGTIME " ms (effect 300 ms)",
		stoppedAfter / 1000);

	emit(fd, EV_FF, FF_GAIN, 0x8000);
	effect.replay.length = 0;
	ioctl(fd, EVIOCSFF, &effect);
	emit(fd, EV_FF, effect.id, 1);
	snooze(30000);
	ioctl(pad.fd, UI_RENGA_GET_RUMBLE, &rumble);
	uint16 gained = rumble.strong_magnitude;
	emit(fd, EV_FF, effect.id, 0);
	snooze(30000);
	ioctl(pad.fd, UI_RENGA_GET_RUMBLE, &rumble);
	uint16 stopped = rumble.strong_magnitude;
	int erase = ioctl_value(fd, EVIOCRMFF, effect.id);
	int replay = ioctl_value(fd, EVIOCRMFF, effect.id);
	report("ff_gain_stop_erase", gained >= 0x3f00 && gained <= 0x4100
		&& stopped == 0 && erase == 0 && replay < 0,
		"half gain %#x, stopped %#x, erase %d, erase again %d", gained,
		stopped, erase, replay);
	while (read_events(pad.fd, events, 700) > 0)
		;

	// battery and player
	struct renga_input_battery battery = { 42, 1, 0, 0 };
	ioctl(pad.fd, UI_RENGA_SET_BATTERY, &battery);
	memset(&battery, 0, sizeof(battery));
	ioctl(fd, EVIOC_RENGA_GBATTERY, &battery);
	report("battery", battery.level == 42 && battery.charging == 1,
		"level %d, charging %d", battery.level, battery.charging);

	// the device goes away while open
	ioctl(pad.fd, UI_DEV_DESTROY);
	errno = 0;
	ssize_t bytes = read(fd, events, sizeof(struct input_event));
	int readError = errno;
	bool gone = access(pad.path, F_OK) != 0;
	report("unplug_while_open", bytes < 0 && readError == ENODEV && gone,
		"read %zd (%s), node %s", bytes, strerror(readError),
		gone ? "removed" : "still there");
	close(fd);
	close(pad.fd);
}


static void
test_players_and_hotplug()
{
	int notify = inotify_init1(IN_NONBLOCK);
	int watch = notify >= 0
		? inotify_add_watch(notify, "/dev/input", IN_CREATE | IN_DELETE) : -1;

	VirtualPad first, second, third;
	bool ok = create_pad(first, "Player pad one")
		&& create_pad(second, "Player pad two");
	struct renga_uinput_rumble info1, info2, info3;
	memset(&info1, 0, sizeof(info1));
	memset(&info2, 0, sizeof(info2));
	memset(&info3, 0, sizeof(info3));
	ioctl(first.fd, UI_RENGA_GET_RUMBLE, &info1);
	ioctl(second.fd, UI_RENGA_GET_RUMBLE, &info2);
	int player1 = info1.player, player2 = info2.player;

	// closing uinput without UI_DEV_DESTROY removes the pad too
	close(first.fd);
	snooze(20000);
	bool firstGone = access(first.path, F_OK) != 0;
	ok = ok && create_pad(third, "Player pad three");
	ioctl(third.fd, UI_RENGA_GET_RUMBLE, &info3);
	report("player_numbers", ok && player1 >= 1 && player2 == player1 + 1
		&& info3.player == player1 && firstGone,
		"players %d, %d; after the first left, the next got %d", player1,
		player2, info3.player);

	// what inotify saw (SDL's hot-plug path without udev)
	int created = 0, deleted = 0;
	char buffer[4096];
	snooze(50000);
	ssize_t length = notify >= 0 ? read(notify, buffer, sizeof(buffer)) : -1;
	for (ssize_t offset = 0; offset < length;) {
		struct inotify_event* event = (struct inotify_event*)(buffer + offset);
		if (event->len > 0 && strncmp(event->name, "event", 5) == 0) {
			if ((event->mask & IN_CREATE) != 0)
				created++;
			if ((event->mask & IN_DELETE) != 0)
				deleted++;
		}
		offset += sizeof(struct inotify_event) + event->len;
	}
	report("hotplug_inotify", watch >= 0 && created >= 3 && deleted >= 1,
		"%d created, %d deleted", created, deleted);
	if (notify >= 0)
		close(notify);
	close(second.fd);
	close(third.fd);
}


namespace {

class Watcher : public BLooper {
public:
	Watcher()
		:
		BLooper("gamepad test watcher"),
		fAdded(0),
		fRemoved(0)
	{
		fChange = create_sem(0, "gamepad change");
	}

	virtual void MessageReceived(BMessage* message)
	{
		if (message->what == B_GAMEPAD_ADDED)
			atomic_add(&fAdded, 1);
		else if (message->what == B_GAMEPAD_REMOVED)
			atomic_add(&fRemoved, 1);
		else {
			BLooper::MessageReceived(message);
			return;
		}
		release_sem(fChange);
	}

	bool WaitFor(int32* counter, int32 count)
	{
		bigtime_t end = system_time() + 3000000;
		while (atomic_get(counter) < count) {
			if (acquire_sem_etc(fChange, 1, B_ABSOLUTE_TIMEOUT, end) != B_OK)
				return atomic_get(counter) >= count;
		}
		return true;
	}

	int32	fAdded;
	int32	fRemoved;
	sem_id	fChange;
};

}	// namespace


static void
test_bgamepad()
{
	Watcher* watcher = new Watcher;
	watcher->Run();
	status_t watching = BGamepad::StartWatching(BMessenger(watcher));

	int32 before = BGamepad::CountGamepads();
	VirtualPad pad;
	bool created = create_pad(pad, "8BitDo Ultimate 2C Wireless Controller");
	bool added = created && watcher->WaitFor(&watcher->fAdded, 1);
	int32 after = BGamepad::CountGamepads();
	report("bgamepad_hotplug_added", watching == B_OK && added
		&& after == before + 1, "watching %s, added %s, %d -> %d gamepads",
		strerror(watching), added ? "yes" : "no", before, after);
	if (!created) {
		BGamepad::StopWatching(BMessenger(watcher));
		watcher->Lock();
		watcher->Quit();
		return;
	}

	BGamepad gamepad;
	status_t status = gamepad.Open(pad.path);
	const char* mapping = gamepad.Mapping();
	report("bgamepad_database_mapping", status == B_OK
		&& gamepad.HasMapping() && mapping != NULL
		&& strncmp(mapping, "03000000c82d00000a31000014010000,", 33) == 0,
		"GUID %s, mapping \"%.60s\"", gamepad.GUID(),
		mapping != NULL ? mapping : "(none)");

	// south button, left stick right, right trigger half, D-pad up
	emit(pad.fd, EV_KEY, BTN_A, 1);
	emit(pad.fd, EV_KEY, BTN_MODE, 1);
	emit(pad.fd, EV_ABS, ABS_X, 32767);
	emit(pad.fd, EV_ABS, ABS_RZ, 128);
	emit(pad.fd, EV_ABS, ABS_HAT0Y, -1);
	emit(pad.fd, EV_SYN, SYN_REPORT, 0);
	wait_readable(gamepad.FileDescriptor(), 1000);
	gamepad.Update();
	int16 leftX = gamepad.AxisValue(B_GAMEPAD_AXIS_LEFT_X);
	int16 rightTrigger = gamepad.AxisValue(B_GAMEPAD_AXIS_RIGHT_TRIGGER);
	int16 leftTrigger = gamepad.AxisValue(B_GAMEPAD_AXIS_LEFT_TRIGGER);
	report("bgamepad_state", gamepad.IsPressed(B_GAMEPAD_BUTTON_A)
		&& gamepad.IsPressed(B_GAMEPAD_BUTTON_GUIDE)
		&& !gamepad.IsPressed(B_GAMEPAD_BUTTON_B)
		&& gamepad.IsPressed(B_GAMEPAD_BUTTON_DPAD_UP)
		&& !gamepad.IsPressed(B_GAMEPAD_BUTTON_DPAD_DOWN)
		&& leftX == 32767 && leftTrigger == 0
		&& rightTrigger > 15000 && rightTrigger < 17500,
		"A %d, guide %d, up %d, left x %d, triggers %d/%d",
		gamepad.IsPressed(B_GAMEPAD_BUTTON_A),
		gamepad.IsPressed(B_GAMEPAD_BUTTON_GUIDE),
		gamepad.IsPressed(B_GAMEPAD_BUTTON_DPAD_UP), leftX, leftTrigger,
		rightTrigger);

	status_t rumbled = gamepad.Rumble(0xffff, 0x1000, 1000000);
	snooze(30000);
	struct renga_uinput_rumble rumble;
	memset(&rumble, 0, sizeof(rumble));
	ioctl(pad.fd, UI_RENGA_GET_RUMBLE, &rumble);
	gamepad.StopRumble();
	report("bgamepad_rumble", rumbled == B_OK && gamepad.CanRumble()
		&& rumble.strong_magnitude == 0xffff
		&& rumble.weak_magnitude == 0x1000,
		"%s, strength %#x/%#x", strerror(rumbled), rumble.strong_magnitude,
		rumble.weak_magnitude);

	int32 player = gamepad.Player();
	status_t set = gamepad.SetPlayer(4);
	report("bgamepad_player", player >= 1 && set == B_OK
		&& gamepad.Player() == 4, "player %d, then %d", (int)player,
		(int)gamepad.Player());

	// a program's own mapping wins over the database
	BString line;
	line.SetToFormat("%s,Test layout,a:b1,b:b0,platform:Linux,",
		gamepad.GUID());
	BGamepad::AddMapping(line.String());
	BGamepad remapped;
	remapped.Open(pad.path);
	remapped.Update();
	report("bgamepad_added_mapping", remapped.IsPressed(B_GAMEPAD_BUTTON_B)
		&& !remapped.IsPressed(B_GAMEPAD_BUTTON_A),
		"A %d, B %d", remapped.IsPressed(B_GAMEPAD_BUTTON_A),
		remapped.IsPressed(B_GAMEPAD_BUTTON_B));
	remapped.Close();

	// BJoystick sees the pad through the core's joystick node
	BJoystick joystick;
	int32 devices = joystick.CountDevices();
	bool found = false;
	char devName[B_OS_NAME_LENGTH];
	BString expected;
	expected.SetToFormat("gamepad/%d", pad.event);
	for (int32 i = 0; i < devices; i++) {
		if (joystick.GetDeviceName(i, devName) == B_OK
			&& expected == devName)
			found = true;
	}
	bool opened = found && joystick.Open(expected.String(), true) >= 0;
	int16 axes[16] = {};
	uint32 buttons = 0;
	if (opened) {
		joystick.Update();
		joystick.GetAxisValues(axes);
		buttons = joystick.ButtonValues();
	}
	report("bjoystick_node", opened && (buttons & 1) != 0 && axes[0] > 32000,
		"%s %s, buttons %#" B_PRIx32 ", axis 0 %d", expected.String(),
		found ? "listed" : "not listed", buttons, axes[0]);
	joystick.Close();

	gamepad.Close();
	close(pad.fd);
	bool removed = watcher->WaitFor(&watcher->fRemoved, 1);
	report("bgamepad_hotplug_removed", removed
		&& BGamepad::CountGamepads() == before, "removed %s, %d gamepads",
		removed ? "yes" : "no", (int)BGamepad::CountGamepads());

	BGamepad::StopWatching(BMessenger(watcher));
	watcher->Lock();
	watcher->Quit();
}


static void
test_hidraw_access()
{
	// QEMU's USB tablet and keyboard are HID devices; raw access must not
	// be offered for them
	BDirectory dev("/dev");
	BEntry entry;
	int32 hidraw = 0;
	while (dev.GetNextEntry(&entry) == B_OK) {
		char name[B_FILE_NAME_LENGTH];
		entry.GetName(name);
		if (strncmp(name, "hidraw", 6) == 0)
			hidraw++;
	}
	bool tablet = access("/dev/input/tablet/usb", F_OK) == 0
		|| access("/dev/input/mouse/usb", F_OK) == 0;
	report("hidraw_not_for_pointing_devices", hidraw == 0,
		"%d hidraw nodes; USB pointing device %s", (int)hidraw,
		tablet ? "present" : "absent");
}


int
main()
{
	printf("CE-SUITE " SUITE " START\n");
	fflush(stdout);

	BApplication app("application/x-vnd.Renga-gamepad-test");

	// what /dev/input holds (reading it also loads its drivers, as
	// input_server's start does)
	BString listing;
	BDirectory input("/dev/input");
	BEntry entry;
	while (input.GetNextEntry(&entry) == B_OK) {
		char name[B_FILE_NAME_LENGTH];
		entry.GetName(name);
		listing << " " << name;
	}
	printf("CE-INFO gamepad: /dev/input:%s\n", listing.String());

	test_uinput_and_evdev();
	test_players_and_hotplug();
	test_bgamepad();
	test_hidraw_access();

	printf("CE-SUITE " SUITE " DONE failures=%d\n", sFailures);
	return sFailures == 0 ? 0 : 1;
}
