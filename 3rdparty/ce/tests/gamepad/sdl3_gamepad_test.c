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

	 - video (in a child process): SDL_Init(SDL_INIT_VIDEO |
	   SDL_INIT_GAMEPAD), then SDL_CreateWindow() from a second pthread, as
	   OpenCE does; Renga's first build crashed there;
	 - foreign quit request: B_QUIT_REQUESTED sent to SDL's BApplication
	   from outside gives SDL_EVENT_QUIT and leaves the application running
	   (before, it ended and windows crashed: OpenCE on hardware);
	 - OpenCE's start-up order, 200 starts (10 processes of 20), all SDL
	   calls on one thread with its own stack, OpenGL window, frames;
	 - idle: with a pad open, an idle frame loop uses under 5 % of a CPU.

	SDL is loaded with dlopen() so the program runs on images without it
	(minimum images): then it reports that SDL3 is not installed.
	CE_SDL3_LIBRARY names another library to load (for comparison runs
	with another SDL3 build).
*/


#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <spawn.h>
#include <sys/inotify.h>
#include <sys/wait.h>
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

// sdl3_quit_helper.cpp
int sdl3_test_post_quit_to_be_app(void);
int sdl3_test_be_app_exists(void);

/* The parts of SDL3's ABI used here (SDL 3.x, stable). */
#define SDL_INIT_VIDEO					0x00000020u
#define SDL_INIT_GAMEPAD				0x00002000u
#define SDL_EVENT_QUIT					0x100
#define SDL_WINDOW_OPENGL				0x0000000000000002ull
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
	void*			(*CreateWindow)(const char*, int, int, uint64_t);
	void			(*DestroyWindow)(void*);
	void			(*PumpEvents)(void);
	bool			(*SetHint)(const char*, const char*);
	void*			(*GL_CreateContext)(void*);
	bool			(*GL_MakeCurrent)(void*, void*);
	bool			(*GL_SwapWindow)(void*);
	bool			(*GL_DestroyContext)(void*);
	const char*		(*GetCurrentVideoDriver)(void);
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
	SYM(CreateWindow, "SDL_CreateWindow")
	SYM(PumpEvents, "SDL_PumpEvents")
	SYM(SetHint, "SDL_SetHint")
	SYM(GL_CreateContext, "SDL_GL_CreateContext")
	SYM(GL_MakeCurrent, "SDL_GL_MakeCurrent")
	SYM(GL_SwapWindow, "SDL_GL_SwapWindow")
	SYM(GL_DestroyContext, "SDL_GL_DestroyContext")
	SYM(DestroyWindow, "SDL_DestroyWindow")
	SYM(GetCurrentVideoDriver, "SDL_GetCurrentVideoDriver")
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


/*	The video phase, in a child process (the program run again with
	"video"): SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD) on the main thread,
	then SDL_CreateWindow() from a second pthread, as OpenCE does. Renga's
	first libsdl3 build (3.4.16-100) crashed there on hardware: BWindow's
	"You need a valid BApplication object" debugger call. Exit status 0 when
	the window was made. */
static void*
create_window_thread(void* data)
{
	void** window = (void**)data;
	*window = sdl.CreateWindow("sdl3_gamepad_test", 320, 240, 0);
	return NULL;
}


/*	OpenCE's start-up (child "opence"), as its Haiku host and the game do
	it (Techniix/OpenCE_HaikuOS, port/haiku/host/host_main.c and
	port/linux/src/sdl_platform.c): the main thread only waits; one thread,
	with a stack of its own (the game's), calls everything: SDL_SetHint(
	SDL_HINT_APP_NAME, "Halo"), SDL_Init(VIDEO | AUDIO | GAMEPAD | EVENTS),
	SDL_Init(VIDEO) again (its game-data offer does), SDL_CreateWindow() with
	SDL_WINDOW_OPENGL, an OpenGL context, then frames: events pumped and
	polled, the gamepads listed, buffers swapped. Twenty starts and quits
	per process. */
static int sOpenCEResult = 0;

static void*
opence_thread(void* data)
{
	(void)data;

	for (int round = 0; round < 20; round++) {
		sdl.SetHint("SDL_APP_NAME", "Halo");
		if (!sdl.Init(SDL_INIT_VIDEO | 0x00000010u | SDL_INIT_GAMEPAD
				| 0x00004000u) || !sdl.Init(SDL_INIT_VIDEO)) {
			printf("CE-INFO sdl3_gamepad: opence round %d: SDL_Init: %s\n",
				round, sdl.GetError());
			sOpenCEResult = 1;
			return NULL;
		}
		void* window = sdl.CreateWindow("Halo", 640, 480, SDL_WINDOW_OPENGL);
		if (window == NULL) {
			printf("CE-INFO sdl3_gamepad: opence round %d: SDL_CreateWindow: "
				"%s\n", round, sdl.GetError());
			sOpenCEResult = 1;
			return NULL;
		}
		void* context = sdl.GL_CreateContext(window);
		if (context != NULL)
			sdl.GL_MakeCurrent(window, context);
		for (int frame = 0; frame < 10; frame++) {
			SDL_Event event;
			sdl.PumpEvents();
			while (sdl.PollEvent(&event))
				;
			int count = 0;
			SDL_JoystickID* ids = sdl.GetGamepads(&count);
			sdl.free(ids);
			if (context != NULL)
				sdl.GL_SwapWindow(window);
			snooze(5000);
		}
		if (context != NULL)
			sdl.GL_DestroyContext(context);
		sdl.DestroyWindow(window);
		sdl.Quit();
	}
	return NULL;
}


static int
opence_child(const char* library)
{
	if (!load_sdl(library))
		return 2;
	pthread_attr_t attributes;
	pthread_attr_init(&attributes);
	size_t stackSize = 8 * 1024 * 1024;
	// (64 KiB to spare after the stack: until 2026-11-14 the kernel ran
	// such threads 16 KiB above the stack they were given, which showed as
	// "Bad address" faults past its end; fixed in the kernel and checked by
	// thread_stack_test. The spare room keeps this check about SDL.)
	void* stack = malloc(stackSize + 65536);
	pthread_attr_setstack(&attributes, stack, stackSize);
	pthread_t thread;
	if (pthread_create(&thread, &attributes, opence_thread, NULL) != 0)
		return 3;
	pthread_join(thread, NULL);
	return sOpenCEResult;
}


/*	A quit request from outside (child "quitrequest"): after SDL_Init(VIDEO)
	someone sends SDL's BApplication B_QUIT_REQUESTED (Deskbar's "Quit", or
	any program). The program must get SDL_EVENT_QUIT, the application must
	stay, and a window must still open. Before, the application ended
	under SDL: be_app NULL, BWindow's "You need a valid BApplication object"
	debugger call, the crash OpenCE showed on hardware (SDLLooper alive, SDL's
	application thread gone). */
static int
quitrequest_child(const char* library)
{
	if (!load_sdl(library))
		return 2;
	if (!sdl.Init(SDL_INIT_VIDEO))
		return 3;
	if (sdl3_test_post_quit_to_be_app() != 0)
		return 4;
	bool quitEvent = false;
	bigtime_t end = system_time() + 2000000;
	while (!quitEvent && system_time() < end) {
		SDL_Event event;
		sdl.PumpEvents();
		while (sdl.PollEvent(&event)) {
			if (event.type == SDL_EVENT_QUIT)
				quitEvent = true;
		}
		snooze(10000);
	}
	snooze(200000);
	bool appStays = sdl3_test_be_app_exists();
	void* window = appStays ? sdl.CreateWindow("after quit request", 320, 240,
		0) : NULL;
	// libsdl3 3.4.16-104 logs who sent it, for traces on hardware
	char wanted[96];
	snprintf(wanted, sizeof(wanted), "SDL: B_QUIT_REQUESTED from team %d ",
		(int)getpid());
	bool logged = false;
	for (int i = 0; i < 20 && !logged; i++) {
		FILE* log = fopen("/boot/system/var/log/syslog", "r");
		char line[512];
		while (log != NULL && fgets(line, sizeof(line), log) != NULL) {
			if (strstr(line, wanted) != NULL)
				logged = true;
		}
		if (log != NULL)
			fclose(log);
		if (!logged)
			snooze(100000);
	}
	printf("CE-INFO sdl3_gamepad: quit request: SDL_EVENT_QUIT %s, "
		"BApplication %s, window %s, sender %s in the syslog\n",
		quitEvent ? "seen" : "missing", appStays ? "still there" : "gone",
		window != NULL ? "made" : "not made", logged ? "named" : "not named");
	fflush(stdout);
	if (window != NULL)
		sdl.DestroyWindow(window);
	sdl.Quit();
	return quitEvent && appStays && window != NULL ? 0 : 1;
}


/*	Idle CPU use (child "idle"): with a pad plugged in, SDL_Init(GAMEPAD)
	and 4 s of SDL_PumpEvents() every 16 ms, as a game's idle frame loop.
	Renga's back ends must not poll or spin (evdev with inotify, HIDAPI
	with inotify on /dev): less than 5 % of one CPU for the team. Busy
	threads are listed. */
static int
idle_child(const char* library)
{
	if (!load_sdl(library))
		return 2;
	struct VirtualPad pad;
	create_pad(&pad, 0x045e, 0x028e, 0x0114, "Microsoft X-Box 360 pad");
	snooze(200000);
	if (!sdl.Init(SDL_INIT_GAMEPAD))
		return 3;
	int count = 0;
	SDL_JoystickID* ids = sdl.GetGamepads(&count);
	void* gamepad = count > 0 ? sdl.OpenGamepad(ids[0]) : NULL;
	sdl.free(ids);
	SDL_Event event;
	while (sdl.PollEvent(&event))
		;

	team_usage_info before, after;
	get_team_usage_info(B_CURRENT_TEAM, B_TEAM_USAGE_SELF, &before);
	bigtime_t start = system_time();
	while (system_time() - start < 4000000) {
		sdl.PumpEvents();
		while (sdl.PollEvent(&event))
			;
		snooze(16000);
	}
	get_team_usage_info(B_CURRENT_TEAM, B_TEAM_USAGE_SELF, &after);
	bigtime_t wall = system_time() - start;
	bigtime_t used = after.user_time + after.kernel_time - before.user_time
		- before.kernel_time;
	double percent = 100.0 * used / wall;
	printf("CE-INFO sdl3_gamepad: idle: %.1f %% of one CPU over %.1f s "
		"(%s, %d gamepad(s) open)\n", percent, wall / 1000000.0, library,
		gamepad != NULL ? 1 : 0);
	thread_info thread;
	int32 cookie = 0;
	while (get_next_thread_info(B_CURRENT_TEAM, &cookie, &thread) == B_OK) {
		if (thread.user_time + thread.kernel_time > 100000) {
			printf("CE-INFO sdl3_gamepad: idle: thread \"%s\" %.0f ms in "
				"total\n", thread.name,
				(thread.user_time + thread.kernel_time) / 1000.0);
		}
	}
	int watch = inotify_init1(IN_NONBLOCK);
	int added = watch >= 0 ? inotify_add_watch(watch, "/dev",
		IN_CREATE | IN_DELETE) : -1;
	printf("CE-INFO sdl3_gamepad: inotify on /dev (HIDAPI hot-plug): %s\n",
		added >= 0 ? "watched" : strerror(errno));
	if (watch >= 0)
		close(watch);
	fflush(stdout);
	if (gamepad != NULL)
		sdl.CloseGamepad(gamepad);
	sdl.Quit();
	close(pad.fd);
	return percent < 5.0 ? 0 : 1;
}


static int
video_child(const char* library)
{
	if (!load_sdl(library))
		return 2;
	if (!sdl.Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) {
		printf("CE-INFO sdl3_gamepad: video: SDL_Init: %s\n", sdl.GetError());
		return 3;
	}
	const char* driver = sdl.GetCurrentVideoDriver();
	void* window = NULL;
	pthread_t thread;
	pthread_create(&thread, NULL, create_window_thread, &window);
	pthread_join(thread, NULL);
	printf("CE-INFO sdl3_gamepad: video: driver %s, window %s%s\n",
		driver != NULL ? driver : "(none)", window != NULL ? "made" : "failed: ",
		window != NULL ? "" : sdl.GetError());
	fflush(stdout);
	if (window != NULL) {
		snooze(300000);
		sdl.DestroyWindow(window);
	}
	sdl.Quit();
	return window != NULL ? 0 : 4;
}


//! Runs the program again with \a mode; the exit status, -1 when it hung.
static int
run_child(const char* self, const char* mode, bigtime_t timeout)
{
	char* argv[] = { (char*)self, (char*)mode, NULL };
	extern char** environ;
	pid_t child;
	if (posix_spawn(&child, self, NULL, NULL, argv, environ) != 0)
		return -2;
	int status = 0;
	bigtime_t end = system_time() + timeout;
	pid_t done = 0;
	while ((done = waitpid(child, &status, WNOHANG)) == 0
		&& system_time() < end)
		snooze(50000);
	if (done == 0) {
		kill(child, SIGKILL);
		waitpid(child, &status, 0);
		return -1;
	}
	return WIFEXITED(status) ? WEXITSTATUS(status) : -3;
}


static void
test_idle(const char* self, const char* library)
{
	int status = run_child(self, "idle", 30000000);
	report("idle_cpu", status == 0, "an idle SDL3 program with a pad open "
		"uses under 5 %% of one CPU (exit %d; the numbers are in the INFO "
		"lines above)", status);
}


static void
test_opence(const char* self, const char* library)
{
	int runs = 10;
	int failed = 0, hung = 0, crashed = 0;
	for (int i = 0; i < runs; i++) {
		int status = run_child(self, "opence", 120000000);
		if (status == -1)
			hung++;
		else if (status == -3)
			crashed++;
		else if (status != 0)
			failed++;
	}
	report("opence_sequence", failed + hung + crashed == 0,
		"%d processes of 20 starts each (200) in OpenCE's order, everything "
		"on one thread with its own stack: SDL_Init(VIDEO|AUDIO|GAMEPAD|"
		"EVENTS), an OpenGL window, frames: %d failed, %d hung, %d crashed "
		"(%s)", runs, failed, hung, crashed, library);
}


static void
test_quit_request(const char* self, const char* library)
{
	int status = run_child(self, "quitrequest", 30000000);
	report("foreign_quit_request", status == 0, "B_QUIT_REQUESTED to SDL's "
		"BApplication from outside: SDL_EVENT_QUIT, the application stays, "
		"a window opens (exit %d; see the INFO line)", status);
}


static void
test_video(const char* self, const char* library)
{
	char* argv[] = { (char*)self, (char*)"video", NULL };
	extern char** environ;
	pid_t child;
	if (posix_spawn(&child, self, NULL, NULL, argv, environ) != 0) {
		report("video_window", false, "could not start %s: %s", self,
			strerror(errno));
		return;
	}
	int status = 0;
	bigtime_t end = system_time() + 30000000;
	pid_t done = 0;
	while ((done = waitpid(child, &status, WNOHANG)) == 0
		&& system_time() < end)
		snooze(100000);
	if (done == 0) {
		kill(child, SIGKILL);
		waitpid(child, &status, 0);
		report("video_window", false, "SDL_Init(VIDEO|GAMEPAD) and "
			"SDL_CreateWindow() from a second thread did not finish in 30 s "
			"(crashed into the debugger?)");
		return;
	}
	bool ok = WIFEXITED(status) && WEXITSTATUS(status) == 0;
	report("video_window", ok, "SDL_Init(VIDEO|GAMEPAD), SDL_CreateWindow() "
		"from a second thread (as OpenCE does): %s (%s, status %d)",
		ok ? "window made" : "failed", library,
		WIFEXITED(status) ? WEXITSTATUS(status) : -1);
}


int
main(int argc, char** argv)
{
	const char* library = getenv("CE_SDL3_LIBRARY");
	if (library == NULL)
		library = "libSDL3.so.0";
	if (argc > 1 && strcmp(argv[1], "video") == 0)
		return video_child(library);
	if (argc > 1 && strcmp(argv[1], "opence") == 0)
		return opence_child(library);
	if (argc > 1 && strcmp(argv[1], "quitrequest") == 0)
		return quitrequest_child(library);
	if (argc > 1 && strcmp(argv[1], "idle") == 0)
		return idle_child(library);

	printf("CE-SUITE " SUITE " START\n");
	fflush(stdout);
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

	test_video(argv[0], library);
	test_quit_request(argv[0], library);
	test_opence(argv[0], library);
	test_idle(argv[0], library);

	printf("CE-SUITE " SUITE " DONE failures=%d\n", sFailures);
	return sFailures == 0 ? 0 : 1;
}
