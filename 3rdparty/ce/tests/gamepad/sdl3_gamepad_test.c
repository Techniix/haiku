/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 */

/*	SDL3 gamepads on Renga (roadmap F-42, "SDL3 gamepads"): the system's
	libSDL3 (Renga's build, package libsdl3) must see game controllers
	through SDL's Linux evdev back end, as programs like OpenCE expect
	(SDL_GetGamepads()). HaikuPorts' build only has the BJoystick back end,
	whose GUIDs miss the mapping database, so it lists no gamepads at all.

	Virtual pads made through uinput stand in for real ones:
	 - an Xbox 360 pad (045e:028e, xpad's buttons and axes) created before
	   SDL_Init(): listed by SDL_GetGamepads() exactly once (the BJoystick
	   back end stays off), as an Xbox 360 gamepad with a name, from a
	   /dev/input/event<N> path; a button press arrives as
	   SDL_EVENT_GAMEPAD_BUTTON_DOWN (south); SDL_RumbleGamepad() reaches
	   the device as force feedback;
	 - an 8BitDo Ultimate 2C in X-input mode (2dc8:310a) plugged in while
	   SDL runs: SDL_EVENT_GAMEPAD_ADDED (inotify hot-plug, no udev), then
	   SDL_EVENT_GAMEPAD_REMOVED when its program closes uinput;
	 - HIDAPI (SDL_hid_*) enumerates /dev/hidraw* without udev; QEMU has
	   no raw game controller, so the list is empty, without an error.

	SDL is loaded with dlopen() so the program runs on images without it
	(minimum images): then it reports that SDL3 is not installed.
	CE_SDL3_LIBRARY names another library to load (for comparison runs
	with another SDL3 build).
*/


#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <OS.h>

#include <linux/input.h>
#include <linux/uinput.h>


#define SUITE "sdl3_gamepad"

/* The parts of SDL3's ABI used here (SDL 3.x, stable). */
#define SDL_INIT_GAMEPAD				0x00002000u
#define SDL_EVENT_GAMEPAD_BUTTON_DOWN	0x651
#define SDL_EVENT_GAMEPAD_ADDED			0x653
#define SDL_EVENT_GAMEPAD_REMOVED		0x654
#define SDL_GAMEPAD_TYPE_UNKNOWN		0
#define SDL_GAMEPAD_TYPE_XBOX360		2
#define SDL_GAMEPAD_BUTTON_SOUTH		0

typedef uint32_t SDL_JoystickID;

typedef union {
	uint32_t	type;
	struct {
		uint32_t		type;
		uint32_t		reserved;
		uint64_t		timestamp;
		SDL_JoystickID	which;
	} gdevice;
	struct {
		uint32_t		type;
		uint32_t		reserved;
		uint64_t		timestamp;
		SDL_JoystickID	which;
		uint8_t			button;
		bool			down;
		uint8_t			padding1;
		uint8_t			padding2;
	} gbutton;
	uint8_t		padding[128];
} SDL_Event;

struct hid_device_info;

static struct {
	int				(*GetVersion)(void);
	bool			(*Init)(uint32_t);
	void			(*Quit)(void);
	const char*		(*GetError)(void);
	bool			(*PollEvent)(SDL_Event*);
	SDL_JoystickID*	(*GetGamepads)(int*);
	void			(*free)(void*);
	const char*		(*GetGamepadNameForID)(SDL_JoystickID);
	const char*		(*GetGamepadPathForID)(SDL_JoystickID);
	int				(*GetGamepadTypeForID)(SDL_JoystickID);
	void*			(*OpenGamepad)(SDL_JoystickID);
	void			(*CloseGamepad)(void*);
	bool			(*RumbleGamepad)(void*, uint16_t, uint16_t, uint32_t);
	int				(*hid_init)(void);
	struct hid_device_info* (*hid_enumerate)(unsigned short, unsigned short);
	void			(*hid_free_enumeration)(struct hid_device_info*);
} sdl;

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


static bool
load_sdl(const char* library)
{
	void* handle = dlopen(library, RTLD_NOW | RTLD_LOCAL);
	if (handle == NULL) {
		printf("CE-INFO sdl3_gamepad: dlopen(%s): %s\n", library, dlerror());
		return false;
	}
#define SYM(field, name) \
	*(void**)&sdl.field = dlsym(handle, name); \
	if (sdl.field == NULL) { \
		printf("CE-INFO sdl3_gamepad: %s missing\n", name); \
		return false; \
	}
	SYM(GetVersion, "SDL_GetVersion")
	SYM(Init, "SDL_Init")
	SYM(Quit, "SDL_Quit")
	SYM(GetError, "SDL_GetError")
	SYM(PollEvent, "SDL_PollEvent")
	SYM(GetGamepads, "SDL_GetGamepads")
	SYM(free, "SDL_free")
	SYM(GetGamepadNameForID, "SDL_GetGamepadNameForID")
	SYM(GetGamepadPathForID, "SDL_GetGamepadPathForID")
	SYM(GetGamepadTypeForID, "SDL_GetGamepadTypeForID")
	SYM(OpenGamepad, "SDL_OpenGamepad")
	SYM(CloseGamepad, "SDL_CloseGamepad")
	SYM(RumbleGamepad, "SDL_RumbleGamepad")
	SYM(hid_init, "SDL_hid_init")
	SYM(hid_enumerate, "SDL_hid_enumerate")
	SYM(hid_free_enumeration, "SDL_hid_free_enumeration")
#undef SYM
	return true;
}


struct VirtualPad {
	int		fd;
	int		event;
};


static const uint16_t kButtons[] = { BTN_A, BTN_B, BTN_X, BTN_Y, BTN_TL,
	BTN_TR, BTN_SELECT, BTN_START, BTN_MODE, BTN_THUMBL, BTN_THUMBR };


/* A pad set up the way Linux's xpad (and Renga's xinput driver) sets one
   up: the X-input buttons, sticks, triggers and d-pad hat, and rumble. */
static bool
create_pad(struct VirtualPad* pad, uint16_t vendor, uint16_t product,
	uint16_t version, const char* name)
{
	pad->event = -1;
	pad->fd = open("/dev/input/uinput", O_RDWR | O_NONBLOCK);
	if (pad->fd < 0) {
		printf("CE-INFO sdl3_gamepad: open /dev/input/uinput: %s\n",
			strerror(errno));
		return false;
	}

	ioctl(pad->fd, UI_SET_EVBIT, EV_KEY);
	ioctl(pad->fd, UI_SET_EVBIT, EV_ABS);
	for (size_t i = 0; i < sizeof(kButtons) / sizeof(kButtons[0]); i++)
		ioctl(pad->fd, UI_SET_KEYBIT, kButtons[i]);
	struct { uint16_t code; int32_t min, max, fuzz, flat; } axes[] = {
		{ ABS_X, -32768, 32767, 16, 128 }, { ABS_Y, -32768, 32767, 16, 128 },
		{ ABS_Z, 0, 255, 0, 0 }, { ABS_RX, -32768, 32767, 16, 128 },
		{ ABS_RY, -32768, 32767, 16, 128 }, { ABS_RZ, 0, 255, 0, 0 },
		{ ABS_HAT0X, -1, 1, 0, 0 }, { ABS_HAT0Y, -1, 1, 0, 0 }
	};
	for (size_t i = 0; i < sizeof(axes) / sizeof(axes[0]); i++) {
		struct uinput_abs_setup setup;
		ioctl(pad->fd, UI_SET_ABSBIT, axes[i].code);
		memset(&setup, 0, sizeof(setup));
		setup.code = axes[i].code;
		setup.absinfo.minimum = axes[i].min;
		setup.absinfo.maximum = axes[i].max;
		setup.absinfo.fuzz = axes[i].fuzz;
		setup.absinfo.flat = axes[i].flat;
		ioctl(pad->fd, UI_ABS_SETUP, &setup);
	}
	ioctl(pad->fd, UI_SET_EVBIT, EV_FF);
	ioctl(pad->fd, UI_SET_FFBIT, FF_RUMBLE);
	ioctl(pad->fd, UI_SET_FFBIT, FF_GAIN);

	struct uinput_setup setup;
	memset(&setup, 0, sizeof(setup));
	setup.id.bustype = BUS_USB;
	setup.id.vendor = vendor;
	setup.id.product = product;
	setup.id.version = version;
	strlcpy(setup.name, name, sizeof(setup.name));
	if (ioctl(pad->fd, UI_DEV_SETUP, &setup) < 0
		|| ioctl(pad->fd, UI_DEV_CREATE) < 0) {
		printf("CE-INFO sdl3_gamepad: UI_DEV_SETUP/UI_DEV_CREATE: %s\n",
			strerror(errno));
		close(pad->fd);
		pad->fd = -1;
		return false;
	}

	char sysname[32] = "";
	if (ioctl(pad->fd, UI_GET_SYSNAME(sizeof(sysname)), sysname) >= 0
		&& strncmp(sysname, "input", 5) == 0)
		pad->event = atoi(sysname + 5);
	return true;
}


static void
emit(int fd, uint16_t type, uint16_t code, int32_t value)
{
	struct input_event event;
	memset(&event, 0, sizeof(event));
	event.type = type;
	event.code = code;
	event.value = value;
	write(fd, &event, sizeof(event));
}


/* Pumps SDL's events until one of the given type arrives (for "which",
   when not 0) or the time is up. */
static bool
wait_event(uint32_t type, SDL_JoystickID which, SDL_Event* result,
	bigtime_t timeout)
{
	bigtime_t end = system_time() + timeout;
	SDL_Event event;
	do {
		while (sdl.PollEvent(&event)) {
			if (event.type == type
				&& (which == 0 || event.gdevice.which == which)) {
				if (result != NULL)
					*result = event;
				return true;
			}
		}
		snooze(10000);
	} while (system_time() < end);
	return false;
}


static bool
ends_with(const char* string, const char* suffix)
{
	size_t length = strlen(string);
	size_t suffixLength = strlen(suffix);
	return length >= suffixLength
		&& strcmp(string + length - suffixLength, suffix) == 0;
}


int
main()
{
	printf("CE-SUITE " SUITE " START\n");
	fflush(stdout);

	const char* library = getenv("CE_SDL3_LIBRARY");
	if (library == NULL)
		library = "libSDL3.so.0";
	if (!load_sdl(library)) {
		if (access("/boot/system/lib/libSDL3.so.0", F_OK) != 0
			&& getenv("CE_SDL3_LIBRARY") == NULL) {
			printf("CE-INFO sdl3_gamepad: SDL3 not installed (libsdl3 is "
				"part of the renga-* images); nothing checked\n");
			printf("CE-SUITE " SUITE " DONE failures=0\n");
			return 0;
		}
		report("load", false, "%s could not be loaded", library);
		printf("CE-SUITE " SUITE " DONE failures=%d\n", sFailures);
		return 1;
	}
	int version = sdl.GetVersion();
	printf("CE-INFO sdl3_gamepad: SDL %d.%d.%d (%s)\n", version / 1000000,
		version / 1000 % 1000, version % 1000, library);
	report("version", version >= 3004000, "%d.%d.%d (3.4.0 or later wanted)",
		version / 1000000, version / 1000 % 1000, version % 1000);

	struct VirtualPad xbox;
	bool created = create_pad(&xbox, 0x045e, 0x028e, 0x0114,
		"Microsoft X-Box 360 pad");
	report("uinput_pad", created, "event%d", xbox.event);
	if (!created) {
		printf("CE-SUITE " SUITE " DONE failures=%d\n", sFailures);
		return 1;
	}
	snooze(200000);

	bool init = sdl.Init(SDL_INIT_GAMEPAD);
	report("init", init, "%s", init ? "SDL_INIT_GAMEPAD" : sdl.GetError());
	if (!init) {
		close(xbox.fd);
		printf("CE-SUITE " SUITE " DONE failures=%d\n", sFailures);
		return 1;
	}

	// The pad present at start-up: SDL reports it with an added event.
	wait_event(SDL_EVENT_GAMEPAD_ADDED, 0, NULL, 2000000);
	int count = 0;
	SDL_JoystickID* ids = sdl.GetGamepads(&count);
	SDL_JoystickID xboxID = 0;
	char wanted[64];
	snprintf(wanted, sizeof(wanted), "/dev/input/event%d", xbox.event);
	int matches = 0;
	for (int i = 0; ids != NULL && i < count; i++) {
		const char* path = sdl.GetGamepadPathForID(ids[i]);
		const char* name = sdl.GetGamepadNameForID(ids[i]);
		printf("CE-INFO sdl3_gamepad: gamepad %u: \"%s\" type %d path %s\n",
			ids[i], name != NULL ? name : "", sdl.GetGamepadTypeForID(ids[i]),
			path != NULL ? path : "(none)");
		if (path != NULL && ends_with(path, wanted)) {
			xboxID = ids[i];
			matches++;
		}
	}
	sdl.free(ids);
	report("gamepad_listed", xboxID != 0 && count == 1,
		"%d gamepad(s), the virtual pad %s", count,
		xboxID != 0 ? "among them, from its evdev node" : "missing");
	report("listed_once", matches == 1 && count == 1,
		"listed %d time(s) (the BJoystick back end must stay off)", count);

	if (xboxID != 0) {
		const char* name = sdl.GetGamepadNameForID(xboxID);
		int type = sdl.GetGamepadTypeForID(xboxID);
		report("gamepad_name", name != NULL && strstr(name, "Xbox 360") != NULL,
			"\"%s\"", name != NULL ? name : "");
		report("gamepad_type", type == SDL_GAMEPAD_TYPE_XBOX360,
			"type %d (Xbox 360 is %d)", type, SDL_GAMEPAD_TYPE_XBOX360);

		void* gamepad = sdl.OpenGamepad(xboxID);
		report("open", gamepad != NULL, "%s",
			gamepad != NULL ? "opened" : sdl.GetError());
		if (gamepad != NULL) {
			// Drain what opening produced, then press A.
			SDL_Event event;
			while (sdl.PollEvent(&event))
				;
			emit(xbox.fd, EV_KEY, BTN_A, 1);
			emit(xbox.fd, EV_SYN, SYN_REPORT, 0);
			bool pressed = false;
			bigtime_t end = system_time() + 2000000;
			while (!pressed && system_time() < end) {
				if (!wait_event(SDL_EVENT_GAMEPAD_BUTTON_DOWN, xboxID, &event,
						100000))
					continue;
				pressed = event.gbutton.button == SDL_GAMEPAD_BUTTON_SOUTH;
			}
			report("button_event", pressed, "BTN_A as the south button: %s",
				pressed ? "seen" : "missing");
			emit(xbox.fd, EV_KEY, BTN_A, 0);
			emit(xbox.fd, EV_SYN, SYN_REPORT, 0);

			bool rumbled = sdl.RumbleGamepad(gamepad, 0x8000, 0x4000, 500);
			struct renga_uinput_rumble rumble;
			memset(&rumble, 0, sizeof(rumble));
			end = system_time() + 1000000;
			do {
				// SDL_RumbleGamepad() sends right away; reading uinput lets
				// the play event through.
				struct input_event events[16];
				read(xbox.fd, events, sizeof(events));
				snooze(20000);
				ioctl(xbox.fd, UI_RENGA_GET_RUMBLE, &rumble);
			} while (rumble.strong_magnitude == 0 && system_time() < end);
			report("rumble", rumbled && rumble.strong_magnitude == 0x8000
				&& rumble.weak_magnitude == 0x4000,
				"SDL_RumbleGamepad %s, device strength %#x/%#x",
				rumbled ? "accepted" : sdl.GetError(),
				rumble.strong_magnitude, rumble.weak_magnitude);
			sdl.RumbleGamepad(gamepad, 0, 0, 0);
			sdl.CloseGamepad(gamepad);
		}
	}

	// Hot-plug: a second pad while SDL runs, then gone again.
	struct VirtualPad ultimate;
	SDL_Event added;
	bigtime_t plugged = system_time();
	bool second = create_pad(&ultimate, 0x2dc8, 0x310a, 0x0114,
		"8BitDo Ultimate 2C Wireless Controller");
	bool addedSeen = second
		&& wait_event(SDL_EVENT_GAMEPAD_ADDED, 0, &added, 5000000);
	report("hotplug_added", addedSeen, "SDL_EVENT_GAMEPAD_ADDED %s after %d ms",
		addedSeen ? "seen" : "missing",
		(int)((system_time() - plugged) / 1000));
	if (addedSeen) {
		const char* name = sdl.GetGamepadNameForID(added.gdevice.which);
		int type = sdl.GetGamepadTypeForID(added.gdevice.which);
		printf("CE-INFO sdl3_gamepad: hot-plugged gamepad %u: \"%s\" "
			"type %d\n", added.gdevice.which, name != NULL ? name : "", type);
		report("hotplug_gamepad", name != NULL && name[0] != '\0'
			&& type != SDL_GAMEPAD_TYPE_UNKNOWN, "\"%s\", type %d",
			name != NULL ? name : "", type);
	}
	if (second) {
		bigtime_t unplugged = system_time();
		close(ultimate.fd);
		bool removed = addedSeen && wait_event(SDL_EVENT_GAMEPAD_REMOVED,
			added.gdevice.which, NULL, 5000000);
		report("hotplug_removed", removed,
			"SDL_EVENT_GAMEPAD_REMOVED %s after %d ms",
			removed ? "seen" : "missing",
			(int)((system_time() - unplugged) / 1000));
	}

	// HIDAPI on hidraw, without udev: QEMU has no raw game controller.
	int hidInit = sdl.hid_init();
	struct hid_device_info* devices = sdl.hid_enumerate(0, 0);
	report("hidapi_enumerate", hidInit == 0 && devices == NULL,
		"SDL_hid_init %d, %s", hidInit,
		devices == NULL ? "no raw controllers (none in QEMU)"
			: "devices listed (unexpected in QEMU)");
	if (devices != NULL)
		sdl.hid_free_enumeration(devices);

	sdl.Quit();
	close(xbox.fd);

	printf("CE-SUITE " SUITE " DONE failures=%d\n", sFailures);
	return sFailures == 0 ? 0 : 1;
}
