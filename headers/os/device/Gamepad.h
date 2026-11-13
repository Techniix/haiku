/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 *
 * BGamepad (Renga, roadmap F-42): game controllers with a standard layout.
 *
 * Every pad is presented with the same buttons and axes, named after their
 * position (the "south" face button is B_GAMEPAD_BUTTON_A whatever the pad
 * prints on it), using the SDL game controller mapping format and the
 * community mapping database shipped with the system, plus the mappings
 * the Gamepads preferences save. Pads without a mapping that report the
 * standard Linux gamepad codes are mapped from those.
 *
 * Underneath are the evdev devices /dev/input/event<N>; BJoystick keeps
 * working for the pads it saw before.
 */
#ifndef _GAMEPAD_H
#define _GAMEPAD_H


#include <Messenger.h>
#include <String.h>


enum gamepad_button {
	B_GAMEPAD_BUTTON_A = 0,			// south
	B_GAMEPAD_BUTTON_B,				// east
	B_GAMEPAD_BUTTON_X,				// west
	B_GAMEPAD_BUTTON_Y,				// north
	B_GAMEPAD_BUTTON_BACK,
	B_GAMEPAD_BUTTON_GUIDE,
	B_GAMEPAD_BUTTON_START,
	B_GAMEPAD_BUTTON_LEFT_STICK,
	B_GAMEPAD_BUTTON_RIGHT_STICK,
	B_GAMEPAD_BUTTON_LEFT_SHOULDER,
	B_GAMEPAD_BUTTON_RIGHT_SHOULDER,
	B_GAMEPAD_BUTTON_DPAD_UP,
	B_GAMEPAD_BUTTON_DPAD_DOWN,
	B_GAMEPAD_BUTTON_DPAD_LEFT,
	B_GAMEPAD_BUTTON_DPAD_RIGHT,
	B_GAMEPAD_BUTTON_MISC1,			// share, capture, microphone
	B_GAMEPAD_BUTTON_PADDLE1,		// upper right
	B_GAMEPAD_BUTTON_PADDLE2,		// upper left
	B_GAMEPAD_BUTTON_PADDLE3,		// lower right
	B_GAMEPAD_BUTTON_PADDLE4,		// lower left
	B_GAMEPAD_BUTTON_TOUCHPAD,

	B_GAMEPAD_BUTTON_COUNT
};

enum gamepad_axis {
	B_GAMEPAD_AXIS_LEFT_X = 0,		// -32768 (left) to 32767 (right)
	B_GAMEPAD_AXIS_LEFT_Y,			// -32768 (up) to 32767 (down)
	B_GAMEPAD_AXIS_RIGHT_X,
	B_GAMEPAD_AXIS_RIGHT_Y,
	B_GAMEPAD_AXIS_LEFT_TRIGGER,	// 0 (released) to 32767
	B_GAMEPAD_AXIS_RIGHT_TRIGGER,

	B_GAMEPAD_AXIS_COUNT
};

// Messages sent to the targets of BGamepad::StartWatching(). Each carries
// "path" (B_STRING_TYPE, the device path).
enum {
	B_GAMEPAD_ADDED		= 'gpad',
	B_GAMEPAD_REMOVED	= 'gprm'
};

struct gamepad_state {
	bigtime_t	time;
	uint32		buttons;		// bit (1 << gamepad_button)
	int16		axes[B_GAMEPAD_AXIS_COUNT];
	uint32		_reserved[4];
};


class BGamepad {
public:
								BGamepad();
	virtual						~BGamepad();

			status_t			Open(const char* devicePath);
			status_t			Open(int32 index);
			void				Close();
			status_t			InitCheck() const;

	static	int32				CountGamepads();
	static	status_t			GetGamepadPath(int32 index, BString* path);

	static	status_t			StartWatching(const BMessenger& target);
	static	void				StopWatching(const BMessenger& target);

			const char*			Name() const;
			const char*			Path() const;
			uint16				VendorID() const;
			uint16				ProductID() const;
			const char*			GUID() const;
				// the SDL GUID, 32 hexadecimal digits
			bool				HasMapping() const;
			const char*			Mapping() const;
				// the SDL mapping line in use

			int					FileDescriptor() const;
				// for select()/poll(); readable when Update() has news

			status_t			Update();
				// reads what the pad sent since the last call
			status_t			GetState(gamepad_state* state) const;
			bool				IsPressed(gamepad_button button) const;
			int16				AxisValue(gamepad_axis axis) const;

			bool				CanRumble() const;
			status_t			Rumble(uint16 strongMagnitude,
									uint16 weakMagnitude,
									bigtime_t duration);
			status_t			StopRumble();

			int32				Player() const;
				// 1-8, 0 = none
			status_t			SetPlayer(int32 player);
			status_t			GetBattery(int32* level, bool* charging,
									bool* wired = NULL) const;
				// level 0-100, -1 if the pad does not report one

	// The mapping database: the system's copy of the SDL community
	// database, the user's mappings (~/config/settings/Gamepads/mappings),
	// and lines added here (for this program only).
	static	status_t			AddMapping(const char* sdlMapping);
	static	status_t			FindMapping(const char* guid,
									BString* mapping);

private:
								BGamepad(const BGamepad&);
			BGamepad&			operator=(const BGamepad&);

	virtual	void				_ReservedGamepad1();
	virtual	void				_ReservedGamepad2();
	virtual	void				_ReservedGamepad3();
	virtual	void				_ReservedGamepad4();

			struct Data;
			Data*				fData;
			uint32				_reserved[8];
};


#endif	// _GAMEPAD_H
