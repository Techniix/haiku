/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 *
 * SDL game controller mappings for BGamepad (roadmap F-42). Private: used
 * by libdevice, the Gamepads preferences and the harness.
 */
#ifndef _GAMEPAD_MAPPING_H
#define _GAMEPAD_MAPPING_H


#include <Gamepad.h>

#include <linux/input.h>


namespace BPrivate {


// A pad's evdev capabilities and current state, enumerated the way SDL's
// Linux joystick back end does it, so the database's "b3", "a2" and "h0.4"
// mean the same here as there: buttons are the key codes from BTN_JOYSTICK
// up, then those below it; axes are the absolute axes in code order except
// the hats; hats are ABS_HAT0 to ABS_HAT3.
struct GamepadDevice {
	struct input_id			id;
	char					name[128];
	uint8					keyBits[(KEY_CNT + 7) / 8];
	uint8					absBits[(ABS_CNT + 7) / 8];
	struct input_absinfo	abs[ABS_CNT];

	uint16					buttonCodes[KEY_CNT];
	int32					buttonCount;
	uint16					axisCodes[ABS_CNT];
	int32					axisCount;
	int32					hatCount;
	uint16					hatCodes[4];

	uint8					keyState[(KEY_CNT + 7) / 8];

			void			Enumerate();
			bool			HasKey(uint32 code) const;
			bool			KeyDown(uint32 code) const;
			int32			AbsValue(uint32 code) const;
			int16			NormalizedAxis(int32 index) const;
				// -32768..32767 over the axis' range
			uint8			HatValue(int32 hat) const;
				// SDL bits: 1 up, 2 right, 4 down, 8 left
			void			GUID(char string[33]) const;
};


class GamepadMapping {
public:
							GamepadMapping();

			bool			Parse(const char* line);
			bool			SetDefault(const GamepadDevice& device);
				// a mapping from the Linux gamepad codes, for pads that
				// report them but are not in the database
			void			Apply(const GamepadDevice& device,
								gamepad_state& state) const;
			const char*		Line() const { return fLine.String(); }
			const char*		Name() const { return fName.String(); }

	static	const char*		ButtonName(gamepad_button button);
	static	const char*		AxisName(gamepad_axis axis);

private:
			enum {
				SOURCE_NONE = 0,
				SOURCE_BUTTON,
				SOURCE_AXIS,
				SOURCE_HAT,
				SOURCE_KEY_CODE,	// default mapping: an evdev key code
				SOURCE_ABS_CODE		// default mapping: an evdev axis code
			};
			enum {
				RANGE_FULL = 0,
				RANGE_POSITIVE,
				RANGE_NEGATIVE
			};
			struct Binding {
				uint8	sourceType;
				uint8	sourceRange;
				bool	invert;
				uint16	index;		// button, axis or hat number, or code
				uint8	hatMask;
				uint8	targetRange;
			};

			bool			_ParseBinding(const char* value, Binding& binding);
			int32			_SourceValue(const GamepadDevice& device,
								const Binding& binding, bool forButton) const;

			BString			fLine;
			BString			fName;
			Binding			fButtons[B_GAMEPAD_BUTTON_COUNT];
			Binding			fAxes[B_GAMEPAD_AXIS_COUNT][2];
				// [0] full or positive half, [1] negative half
};


// The database: system file, user file, lines added by the program.
status_t	gamepad_find_mapping(const char* guid, BString& line);
status_t	gamepad_add_mapping(const char* line);
void		gamepad_reload_mappings();

extern const char* kGamepadSystemDatabase;
extern const char* kGamepadUserDatabase;
	// relative to the user settings directory


}	// namespace BPrivate


#endif	// _GAMEPAD_MAPPING_H
