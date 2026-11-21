/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 *
 * Gamepads as keyboard and mouse (batch 11, roadmap F-42, the Gamepads
 * preferences' Mapping tab; JoyToKey-style). Off by default.
 *
 * Settings (written by the Gamepads preferences), plain text:
 *   ~/config/settings/Gamepads/key_mapping       all applications
 *   ~/config/settings/Gamepads/profiles/<sig>    one application, used
 *                                                while it is active ('/'
 *                                                in the signature as '_')
 * Lines:
 *   enabled 0|1                   the master switch (key_mapping only)
 *   button <name> <raw key code>  a, b, x, y, lb, rb, lt, rt, back, start,
 *                                 l3, r3 (guide is reserved); 0 none
 *   dpad arrows|wasd|none
 *   left_stick none|arrows|wasd|mouse|scroll
 *   right_stick none|mouse|scroll
 *   speed <1-20>                  pointer speed for sticks as a mouse
 *   mouse_buttons 0|1             a = left button, b = right button when
 *                                 a stick is the mouse (instead of keys)
 *
 * Escape: guide and start held together for 3 seconds switch mapping off
 * (enabled 0 is written), whatever the profile says.
 *
 * The pads are read, not grabbed: games keep seeing them. Discovery is a
 * look at /dev/input/event* every two seconds while mapping is on;
 * nothing is opened while it is off.
 */


#include <dirent.h>
#include <errno.h>
#include <math.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <new>

#include <Autolock.h>
#include <FindDirectory.h>
#include <InputServerDevice.h>
#include <InterfaceDefs.h>
#include <View.h>
#include <Keymap.h>
#include <Message.h>
#include <OS.h>
#include <Path.h>
#include <Roster.h>
#include <String.h>

#include <linux/input.h>


static const int kMaxPads = 8;
static const bigtime_t kEscapeHold = 3000000;

enum {
	kButtonA = 0, kButtonB, kButtonX, kButtonY, kButtonLB, kButtonRB,
	kButtonLT, kButtonRT, kButtonBack, kButtonStart, kButtonL3, kButtonR3,
	kButtonCount
};

static const char* const kButtonNames[kButtonCount] = {
	"a", "b", "x", "y", "lb", "rb", "lt", "rt", "back", "start", "l3", "r3"
};

static const int kButtonCodes[kButtonCount] = {
	BTN_A, BTN_B, BTN_X, BTN_Y, BTN_TL, BTN_TR, -1, -1, BTN_SELECT,
	BTN_START, BTN_THUMBL, BTN_THUMBR
};

enum stick_use { kNone = 0, kArrows, kWasd, kMouse, kScroll };

// raw key codes
static const int32 kKeyUp = 0x57, kKeyDown = 0x62, kKeyLeft = 0x61,
	kKeyRight = 0x63;


struct Profile {
	int32		keys[kButtonCount];
	stick_use	dpad;
	stick_use	leftStick;
	stick_use	rightStick;
	int32		speed;
	bool		mouseButtons;

	void Defaults()
	{
		for (int i = 0; i < kButtonCount; i++)
			keys[i] = 0;
		dpad = kArrows;
		leftStick = kNone;
		rightStick = kNone;
		speed = 8;
		mouseButtons = false;
	}
};


struct Pad {
	int			fd;
	BString		path;
	int32		axisMin[ABS_CNT];
	int32		axisMax[ABS_CNT];
	int32		axis[ABS_CNT];
	bool		button[kButtonCount];
	bool		direction[2][4];	// dpad, left stick: up down left right
	bool		guide;
	bool		start;
	bigtime_t	escapeSince;
	float		scrollRest;
};


class GamepadKeysDevice : public BInputServerDevice {
public:
								GamepadKeysDevice();
	virtual						~GamepadKeysDevice();

	virtual	status_t			InitCheck();
	virtual	status_t			Start(const char* name, void* cookie);
	virtual	status_t			Stop(const char* name, void* cookie);
	virtual	status_t			Control(const char* name, void* cookie,
									uint32 command, BMessage* message);

private:
	static	status_t			_ThreadEntry(void* data);
			void				_Run();
			void				_LoadSettings();
			bool				_ReadProfile(const char* path,
									Profile& profile, bool* enabled);
			void				_Scan();
			void				_Close(int index);
			void				_Handle(Pad& pad, const input_event& event);
			void				_Button(Pad& pad, int button, bool down);
			void				_Directions(Pad& pad, int which, bool up,
									bool down, bool left, bool right);
			void				_Key(int32 key, bool down);
			void				_MouseButton(int32 button, bool down);
			void				_Pointer();
			float				_Axis(const Pad& pad, int axis) const;
			void				_ReleaseAll();
			void				_SwitchOff();

			thread_id			fThread;
			volatile bool		fQuit;
			bool				fEnabled;
			Profile				fDefault;
			Profile				fProfile;		// in use now
			BString				fActiveApp;
			time_t				fSettingsTime;
			bigtime_t			fChecked;
			bigtime_t			fScanned;
			Pad					fPads[kMaxPads];
			int					fPadCount;
			BKeymap				fKeymap;
			uint8				fKeyStates[16];
			int32				fMouseButtons;
			bigtime_t			fLastPointer;
};


extern "C" BInputServerDevice*
instantiate_input_device()
{
	return new(std::nothrow) GamepadKeysDevice();
}


GamepadKeysDevice::GamepadKeysDevice()
	:
	fThread(-1),
	fQuit(false),
	fEnabled(false),
	fSettingsTime(0),
	fChecked(0),
	fScanned(0),
	fPadCount(0),
	fMouseButtons(0),
	fLastPointer(0)
{
	fDefault.Defaults();
	fProfile.Defaults();
	memset(fKeyStates, 0, sizeof(fKeyStates));

	static input_device_ref device = { (char*)"Gamepad mapping",
		B_KEYBOARD_DEVICE, NULL };
	static input_device_ref* devices[] = { &device, NULL };
	RegisterDevices(devices);
}


GamepadKeysDevice::~GamepadKeysDevice()
{
	Stop(NULL, NULL);
}


status_t
GamepadKeysDevice::InitCheck()
{
	return B_OK;
}


status_t
GamepadKeysDevice::Start(const char* name, void* cookie)
{
	if (fThread >= 0)
		return B_OK;
	fKeymap.SetToCurrent();
	fQuit = false;
	fThread = spawn_thread(_ThreadEntry, "gamepad mapping",
		B_URGENT_DISPLAY_PRIORITY, this);
	if (fThread < 0)
		return fThread;
	return resume_thread(fThread);
}


status_t
GamepadKeysDevice::Stop(const char* name, void* cookie)
{
	if (fThread < 0)
		return B_OK;
	fQuit = true;
	status_t result;
	wait_for_thread(fThread, &result);
	fThread = -1;
	return B_OK;
}


status_t
GamepadKeysDevice::Control(const char* name, void* cookie, uint32 command,
	BMessage* message)
{
	if (command == B_KEY_MAP_CHANGED)
		fKeymap.SetToCurrent();
	return B_OK;
}


status_t
GamepadKeysDevice::_ThreadEntry(void* data)
{
	((GamepadKeysDevice*)data)->_Run();
	return B_OK;
}


static stick_use
parse_use(const char* text)
{
	if (strcmp(text, "arrows") == 0)
		return kArrows;
	if (strcmp(text, "wasd") == 0)
		return kWasd;
	if (strcmp(text, "mouse") == 0)
		return kMouse;
	if (strcmp(text, "scroll") == 0)
		return kScroll;
	return kNone;
}


bool
GamepadKeysDevice::_ReadProfile(const char* path, Profile& profile,
	bool* enabled)
{
	FILE* file = fopen(path, "r");
	if (file == NULL)
		return false;
	profile.Defaults();
	char line[128];
	while (fgets(line, sizeof(line), file) != NULL) {
		char word[32], value[64];
		int32 number;
		if (sscanf(line, "enabled %" B_SCNd32, &number) == 1) {
			if (enabled != NULL)
				*enabled = number != 0;
		} else if (sscanf(line, "button %31s %" B_SCNi32, word, &number)
				== 2) {
			for (int i = 0; i < kButtonCount; i++) {
				if (strcmp(word, kButtonNames[i]) == 0)
					profile.keys[i] = number;
			}
		} else if (sscanf(line, "dpad %63s", value) == 1)
			profile.dpad = parse_use(value);
		else if (sscanf(line, "left_stick %63s", value) == 1)
			profile.leftStick = parse_use(value);
		else if (sscanf(line, "right_stick %63s", value) == 1)
			profile.rightStick = parse_use(value);
		else if (sscanf(line, "speed %" B_SCNd32, &number) == 1)
			profile.speed = number < 1 ? 1 : number > 20 ? 20 : number;
		else if (sscanf(line, "mouse_buttons %" B_SCNd32, &number) == 1)
			profile.mouseButtons = number != 0;
	}
	fclose(file);
	return true;
}


void
GamepadKeysDevice::_LoadSettings()
{
	fChecked = system_time();
	BPath path;
	if (find_directory(B_USER_SETTINGS_DIRECTORY, &path) != B_OK)
		return;
	BPath main(path);
	main.Append("Gamepads/key_mapping");
	struct stat st;
	bool wasEnabled = fEnabled;
	if (stat(main.Path(), &st) != 0) {
		fEnabled = false;
		fSettingsTime = 0;
	} else if (st.st_mtime != fSettingsTime) {
		fSettingsTime = st.st_mtime;
		bool enabled = false;
		_ReadProfile(main.Path(), fDefault, &enabled);
		fEnabled = enabled;
	}

	// the active application's own profile, if it has one
	app_info info;
	BString active;
	if (be_roster != NULL && be_roster->GetActiveAppInfo(&info) == B_OK)
		active = info.signature;
	Profile profile = fDefault;
	if (!active.IsEmpty()) {
		BString leaf(active);
		leaf.ReplaceAll('/', '_');
		BPath own(path);
		own.Append("Gamepads/profiles");
		own.Append(leaf.String());
		Profile appProfile;
		if (_ReadProfile(own.Path(), appProfile, NULL))
			profile = appProfile;
	}
	if (memcmp(&profile, &fProfile, sizeof(profile)) != 0) {
		_ReleaseAll();
		fProfile = profile;
	}
	fActiveApp = active;
	if (wasEnabled && !fEnabled)
		_ReleaseAll();
}


void
GamepadKeysDevice::_Scan()
{
	fScanned = system_time();
	DIR* directory = opendir("/dev/input");
	if (directory == NULL)
		return;
	while (struct dirent* entry = readdir(directory)) {
		if (strncmp(entry->d_name, "event", 5) != 0 || fPadCount >= kMaxPads)
			continue;
		BString path("/dev/input/");
		path << entry->d_name;
		bool known = false;
		for (int i = 0; i < fPadCount; i++) {
			if (fPads[i].path == path)
				known = true;
		}
		if (known)
			continue;
		int fd = open(path.String(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
		if (fd < 0)
			continue;
		// game controllers only: a gamepad button
		uint8 keys[KEY_MAX / 8 + 1];
		memset(keys, 0, sizeof(keys));
		if (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(keys)), keys) < 0
			|| (keys[BTN_GAMEPAD / 8] & (1 << (BTN_GAMEPAD % 8))) == 0) {
			close(fd);
			continue;
		}
		Pad& pad = fPads[fPadCount++];
		memset(pad.axis, 0, sizeof(pad.axis));
		memset(pad.button, 0, sizeof(pad.button));
		memset(pad.direction, 0, sizeof(pad.direction));
		pad.fd = fd;
		pad.path = path;
		pad.guide = pad.start = false;
		pad.escapeSince = 0;
		pad.scrollRest = 0;
		for (int axis = 0; axis < ABS_CNT; axis++) {
			struct input_absinfo info;
			if (ioctl(fd, EVIOCGABS(axis), &info) == 0
				&& info.maximum > info.minimum) {
				pad.axisMin[axis] = info.minimum;
				pad.axisMax[axis] = info.maximum;
				pad.axis[axis] = info.value;
			} else {
				pad.axisMin[axis] = -32768;
				pad.axisMax[axis] = 32767;
			}
		}
	}
	closedir(directory);
}


void
GamepadKeysDevice::_Close(int index)
{
	close(fPads[index].fd);
	for (int i = index; i < fPadCount - 1; i++)
		fPads[i] = fPads[i + 1];
	fPadCount--;
}


float
GamepadKeysDevice::_Axis(const Pad& pad, int axis) const
{
	// -1 to 1 (0 to 1 for triggers, whose minimum is 0)
	int32 min = pad.axisMin[axis], max = pad.axisMax[axis];
	if (min >= 0)
		return (float)(pad.axis[axis] - min) / (max - min);
	float center = (min + max) / 2.0f;
	return (pad.axis[axis] - center) / ((max - min) / 2.0f);
}


void
GamepadKeysDevice::_Key(int32 key, bool down)
{
	if (key <= 0 || key >= 128)
		return;
	bool isDown = (fKeyStates[key >> 3] & (1 << (7 - (key & 7)))) != 0;
	if (isDown == down)
		return;
	if (down)
		fKeyStates[key >> 3] |= 1 << (7 - (key & 7));
	else
		fKeyStates[key >> 3] &= ~(1 << (7 - (key & 7)));

	char* string = NULL;
	int32 numBytes = 0;
	fKeymap.GetChars(key, 0, 0, &string, &numBytes);
	BMessage* message = new(std::nothrow) BMessage(numBytes > 0
		? (down ? B_KEY_DOWN : B_KEY_UP)
		: (down ? B_UNMAPPED_KEY_DOWN : B_UNMAPPED_KEY_UP));
	if (message == NULL) {
		delete[] string;
		return;
	}
	message->AddInt64("when", system_time());
	message->AddInt32("key", key);
	message->AddInt32("modifiers", modifiers());
	message->AddData("states", B_UINT8_TYPE, fKeyStates, 16);
	if (numBytes > 0) {
		message->AddString("bytes", BString(string, numBytes));
		for (int32 i = 0; i < numBytes; i++)
			message->AddInt8("byte", string[i]);
		message->AddInt32("raw_char", (uint8)string[0] & 0x7f);
	}
	message->AddBool("be:gamepad_mapping", true);
	delete[] string;
	if (EnqueueMessage(message) != B_OK)
		delete message;
}


void
GamepadKeysDevice::_MouseButton(int32 button, bool down)
{
	int32 buttons = down ? fMouseButtons | button : fMouseButtons & ~button;
	if (buttons == fMouseButtons)
		return;
	fMouseButtons = buttons;
	BMessage* message = new(std::nothrow) BMessage(down ? B_MOUSE_DOWN
		: B_MOUSE_UP);
	if (message == NULL)
		return;
	message->AddInt64("when", system_time());
	message->AddInt32("buttons", fMouseButtons);
	message->AddInt32("x", 0);
	message->AddInt32("y", 0);
	if (down)
		message->AddInt32("clicks", 1);
	if (EnqueueMessage(message) != B_OK)
		delete message;
}


void
GamepadKeysDevice::_Directions(Pad& pad, int which, bool up, bool down,
	bool left, bool right)
{
	stick_use use = which == 0 ? fProfile.dpad : fProfile.leftStick;
	bool now[4] = { up, down, left, right };
	static const int32 kArrowKeys[4] = { kKeyUp, kKeyDown, kKeyLeft,
		kKeyRight };
	// W, S, A, D on the US layout's raw key codes
	static const int32 kWasdKeys[4] = { 0x28, 0x3d, 0x3c, 0x3e };
	for (int i = 0; i < 4; i++) {
		if (now[i] == pad.direction[which][i])
			continue;
		pad.direction[which][i] = now[i];
		if (use == kArrows)
			_Key(kArrowKeys[i], now[i]);
		else if (use == kWasd)
			_Key(kWasdKeys[i], now[i]);
	}
}


void
GamepadKeysDevice::_Button(Pad& pad, int button, bool down)
{
	if (pad.button[button] == down)
		return;
	pad.button[button] = down;
	bool mouse = fProfile.mouseButtons && (fProfile.leftStick == kMouse
		|| fProfile.rightStick == kMouse);
	if (mouse && button == kButtonA) {
		_MouseButton(B_PRIMARY_MOUSE_BUTTON, down);
		return;
	}
	if (mouse && button == kButtonB) {
		_MouseButton(B_SECONDARY_MOUSE_BUTTON, down);
		return;
	}
	_Key(fProfile.keys[button], down);
}


void
GamepadKeysDevice::_Handle(Pad& pad, const input_event& event)
{
	if (event.type == EV_KEY) {
		bool down = event.value != 0;
		if (event.code == BTN_MODE)
			pad.guide = down;
		else if (event.code == BTN_START)
			pad.start = down;
		if (pad.guide && pad.start) {
			if (pad.escapeSince == 0)
				pad.escapeSince = system_time();
		} else
			pad.escapeSince = 0;
		for (int i = 0; i < kButtonCount; i++) {
			if (kButtonCodes[i] == event.code)
				_Button(pad, i, down);
		}
		if (event.code >= BTN_DPAD_UP && event.code <= BTN_DPAD_RIGHT) {
			bool* d = pad.direction[0];
			bool state[4] = { d[0], d[1], d[2], d[3] };
			static const int kIndex[4] = { 0, 1, 2, 3 };
				// BTN_DPAD_UP, DOWN, LEFT, RIGHT in this order
			state[kIndex[event.code - BTN_DPAD_UP]] = down;
			_Directions(pad, 0, state[0], state[1], state[2], state[3]);
		}
	} else if (event.type == EV_ABS && event.code < ABS_CNT) {
		pad.axis[event.code] = event.value;
		switch (event.code) {
			case ABS_HAT0X:
			case ABS_HAT0Y:
				_Directions(pad, 0, pad.axis[ABS_HAT0Y] < 0,
					pad.axis[ABS_HAT0Y] > 0, pad.axis[ABS_HAT0X] < 0,
					pad.axis[ABS_HAT0X] > 0);
				break;
			case ABS_X:
			case ABS_Y:
			{
				float x = _Axis(pad, ABS_X), y = _Axis(pad, ABS_Y);
				_Directions(pad, 1, y < -0.5f, y > 0.5f, x < -0.5f,
					x > 0.5f);
				break;
			}
			case ABS_Z:
				_Button(pad, kButtonLT, _Axis(pad, ABS_Z) > 0.5f);
				break;
			case ABS_RZ:
				_Button(pad, kButtonRT, _Axis(pad, ABS_RZ) > 0.5f);
				break;
		}
	}
}


//!	Sticks as a mouse or a scroll wheel, every 10 ms while deflected.
void
GamepadKeysDevice::_Pointer()
{
	bigtime_t now = system_time();
	if (now - fLastPointer < 10000)
		return;
	fLastPointer = now;
	float dx = 0, dy = 0, scroll = 0;
	for (int i = 0; i < fPadCount; i++) {
		Pad& pad = fPads[i];
		struct { stick_use use; int x, y; } sticks[2] = {
			{ fProfile.leftStick, ABS_X, ABS_Y },
			{ fProfile.rightStick, ABS_RX, ABS_RY } };
		for (int s = 0; s < 2; s++) {
			float x = _Axis(pad, sticks[s].x), y = _Axis(pad, sticks[s].y);
			// a dead zone of 15 %, then a curve for precision
			float length = sqrtf(x * x + y * y);
			if (length < 0.15f)
				continue;
			float scale = (length - 0.15f) / 0.85f;
			scale = scale * scale / length;
			if (sticks[s].use == kMouse) {
				dx += x * scale * fProfile.speed * 2;
				dy += y * scale * fProfile.speed * 2;
			} else if (sticks[s].use == kScroll)
				scroll += y * scale * 0.25f;
		}
		if (scroll != 0) {
			pad.scrollRest += scroll;
			scroll = 0;
			if (fabsf(pad.scrollRest) >= 1) {
				BMessage* message = new(std::nothrow)
					BMessage(B_MOUSE_WHEEL_CHANGED);
				if (message != NULL) {
					float steps = truncf(pad.scrollRest);
					pad.scrollRest -= steps;
					message->AddInt64("when", now);
					message->AddFloat("be:wheel_delta_x", 0);
					message->AddFloat("be:wheel_delta_y", steps);
					if (EnqueueMessage(message) != B_OK)
						delete message;
				}
			}
		}
	}
	if ((int32)dx != 0 || (int32)dy != 0) {
		BMessage* message = new(std::nothrow) BMessage(B_MOUSE_MOVED);
		if (message == NULL)
			return;
		message->AddInt64("when", now);
		message->AddInt32("buttons", fMouseButtons);
		message->AddInt32("x", (int32)dx);
		message->AddInt32("y", (int32)-dy);
			// input_server's relative y grows upwards, as a mouse's
		if (EnqueueMessage(message) != B_OK)
			delete message;
	}
}


void
GamepadKeysDevice::_ReleaseAll()
{
	for (int32 key = 1; key < 128; key++)
		_Key(key, false);
	_MouseButton(B_PRIMARY_MOUSE_BUTTON | B_SECONDARY_MOUSE_BUTTON, false);
	for (int i = 0; i < fPadCount; i++) {
		memset(fPads[i].button, 0, sizeof(fPads[i].button));
		memset(fPads[i].direction, 0, sizeof(fPads[i].direction));
	}
}


//!	The escape combination: mapping off, in the settings too.
void
GamepadKeysDevice::_SwitchOff()
{
	_ReleaseAll();
	fEnabled = false;
	BPath path;
	if (find_directory(B_USER_SETTINGS_DIRECTORY, &path) != B_OK
		|| path.Append("Gamepads/key_mapping") != B_OK) {
		return;
	}
	BString text;
	FILE* file = fopen(path.Path(), "r");
	if (file != NULL) {
		char line[128];
		while (fgets(line, sizeof(line), file) != NULL) {
			if (strncmp(line, "enabled ", 8) != 0)
				text << line;
		}
		fclose(file);
	}
	text.Prepend("enabled 0\n");
	file = fopen(path.Path(), "w");
	if (file != NULL) {
		fputs(text.String(), file);
		fclose(file);
	}
	struct stat st;
	if (stat(path.Path(), &st) == 0)
		fSettingsTime = st.st_mtime;
	debug_printf("gamepad_keys: mapping switched off (guide and start "
		"held)\n");
}


void
GamepadKeysDevice::_Run()
{
	while (!fQuit) {
		bigtime_t now = system_time();
		if (now - fChecked > 500000)
			_LoadSettings();
		if (!fEnabled) {
			while (fPadCount > 0)
				_Close(fPadCount - 1);
			snooze(500000);
			continue;
		}
		if (now - fScanned > 2000000)
			_Scan();

		struct pollfd fds[kMaxPads];
		for (int i = 0; i < fPadCount; i++) {
			fds[i].fd = fPads[i].fd;
			fds[i].events = POLLIN;
			fds[i].revents = 0;
		}
		bool pointer = fProfile.leftStick >= kMouse
			|| fProfile.rightStick >= kMouse;
		int result = poll(fds, fPadCount, pointer ? 10 : 100);
		if (result < 0 && errno != EINTR) {
			snooze(100000);
			continue;
		}
		for (int i = fPadCount - 1; i >= 0; i--) {
			if ((fds[i].revents & (POLLHUP | POLLERR | POLLNVAL)) != 0) {
				_Close(i);
				continue;
			}
			if ((fds[i].revents & POLLIN) == 0)
				continue;
			input_event events[32];
			ssize_t bytes = read(fPads[i].fd, events, sizeof(events));
			if (bytes < 0 && errno != EAGAIN && errno != EINTR) {
				_Close(i);
				continue;
			}
			for (ssize_t e = 0; e < bytes / (ssize_t)sizeof(input_event); e++)
				_Handle(fPads[i], events[e]);
		}
		for (int i = 0; i < fPadCount; i++) {
			if (fPads[i].escapeSince != 0
				&& system_time() - fPads[i].escapeSince >= kEscapeHold) {
				fPads[i].escapeSince = 0;
				_SwitchOff();
				break;
			}
		}
		if (pointer && fEnabled)
			_Pointer();
	}
	_ReleaseAll();
	while (fPadCount > 0)
		_Close(fPadCount - 1);
}
