/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 */

//!	Per-pad tuning applied by BGamepad; see <GamepadTuning.h>.


#include <GamepadTuning.h>

#include <math.h>
#include <string.h>

#include <Directory.h>
#include <File.h>
#include <FindDirectory.h>
#include <Gamepad.h>
#include <Message.h>
#include <Path.h>
#include <String.h>


namespace BPrivate {


static const char* kTuningFile = "Gamepads/tuning";


void
GamepadTuning::SetDefaults()
{
	deadZone = 0;
	curve = 1.0f;
	triggerThreshold = 0;
	swapFaceButtons = false;
}


bool
GamepadTuning::IsDefault() const
{
	return deadZone == 0 && curve == 1.0f && triggerThreshold == 0
		&& !swapFaceButtons;
}


void
GamepadTuning::Clamp()
{
	if (deadZone < 0)
		deadZone = 0;
	if (deadZone > 50)
		deadZone = 50;
	if (!(curve >= 1.0f))
		curve = 1.0f;
	if (curve > 3.0f)
		curve = 3.0f;
	if (triggerThreshold < 0)
		triggerThreshold = 0;
	if (triggerThreshold > 50)
		triggerThreshold = 50;
}


static inline int16
to_axis(float value)
{
	float scaled = value * 32767.0f;
	if (scaled > 32767.0f)
		scaled = 32767.0f;
	if (scaled < -32768.0f)
		scaled = -32768.0f;
	return (int16)lroundf(scaled);
}


static void
apply_stick(int16& x, int16& y, float deadZone, float curve)
{
	float fx = x / 32767.0f;
	float fy = y / 32767.0f;
	float magnitude = sqrtf(fx * fx + fy * fy);
	if (magnitude <= deadZone || magnitude == 0.0f) {
		x = y = 0;
		return;
	}
	float reach = magnitude > 1.0f ? 1.0f : magnitude;
	float shaped = (reach - deadZone) / (1.0f - deadZone);
	if (curve != 1.0f)
		shaped = powf(shaped, curve);
	float factor = shaped / magnitude;
	x = to_axis(fx * factor);
	y = to_axis(fy * factor);
}


static int16
apply_trigger(int16 value, float threshold)
{
	float v = value / 32767.0f;
	if (v <= threshold)
		return 0;
	return to_axis((v - threshold) / (1.0f - threshold));
}


static inline void
swap_bits(uint32& buttons, int a, int b)
{
	bool first = (buttons & (1UL << a)) != 0;
	bool second = (buttons & (1UL << b)) != 0;
	buttons &= ~((1UL << a) | (1UL << b));
	if (first)
		buttons |= 1UL << b;
	if (second)
		buttons |= 1UL << a;
}


void
GamepadTuning::Apply(gamepad_state& state) const
{
	if (IsDefault())
		return;
	float zone = deadZone / 100.0f;
	if (zone > 0.0f || curve != 1.0f) {
		apply_stick(state.axes[B_GAMEPAD_AXIS_LEFT_X],
			state.axes[B_GAMEPAD_AXIS_LEFT_Y], zone, curve);
		apply_stick(state.axes[B_GAMEPAD_AXIS_RIGHT_X],
			state.axes[B_GAMEPAD_AXIS_RIGHT_Y], zone, curve);
	}
	if (triggerThreshold > 0) {
		float threshold = triggerThreshold / 100.0f;
		state.axes[B_GAMEPAD_AXIS_LEFT_TRIGGER] = apply_trigger(
			state.axes[B_GAMEPAD_AXIS_LEFT_TRIGGER], threshold);
		state.axes[B_GAMEPAD_AXIS_RIGHT_TRIGGER] = apply_trigger(
			state.axes[B_GAMEPAD_AXIS_RIGHT_TRIGGER], threshold);
	}
	if (swapFaceButtons) {
		swap_bits(state.buttons, B_GAMEPAD_BUTTON_A, B_GAMEPAD_BUTTON_B);
		swap_bits(state.buttons, B_GAMEPAD_BUTTON_X, B_GAMEPAD_BUTTON_Y);
	}
}


static status_t
tuning_path(BPath& path)
{
	status_t status = find_directory(B_USER_SETTINGS_DIRECTORY, &path);
	if (status == B_OK)
		status = path.Append(kTuningFile);
	return status;
}


static void
load_all(BMessage& all)
{
	BPath path;
	BFile file;
	if (tuning_path(path) == B_OK
		&& file.SetTo(path.Path(), B_READ_ONLY) == B_OK)
		all.Unflatten(&file);
}


/*static*/ status_t
GamepadTuning::Load(const char* guid, GamepadTuning& tuning)
{
	tuning.SetDefaults();
	if (guid == NULL)
		return B_BAD_VALUE;
	BMessage all;
	load_all(all);
	BMessage pad;
	for (int32 i = 0; all.FindMessage("pad", i, &pad) == B_OK; i++) {
		const char* padGUID;
		if (pad.FindString("guid", &padGUID) != B_OK
			|| strcmp(padGUID, guid) != 0)
			continue;
		pad.FindInt32("dead_zone", &tuning.deadZone);
		pad.FindFloat("curve", &tuning.curve);
		pad.FindInt32("trigger_threshold", &tuning.triggerThreshold);
		pad.FindBool("swap_face_buttons", &tuning.swapFaceButtons);
		tuning.Clamp();
		return B_OK;
	}
	return B_ENTRY_NOT_FOUND;
}


/*static*/ status_t
GamepadTuning::Save(const char* guid, const GamepadTuning& tuning)
{
	if (guid == NULL)
		return B_BAD_VALUE;
	BMessage all;
	load_all(all);

	BMessage updated;
	BMessage pad;
	for (int32 i = 0; all.FindMessage("pad", i, &pad) == B_OK; i++) {
		const char* padGUID;
		if (pad.FindString("guid", &padGUID) == B_OK
			&& strcmp(padGUID, guid) == 0)
			continue;
		updated.AddMessage("pad", &pad);
	}
	if (!tuning.IsDefault()) {
		BMessage entry;
		entry.AddString("guid", guid);
		entry.AddInt32("dead_zone", tuning.deadZone);
		entry.AddFloat("curve", tuning.curve);
		entry.AddInt32("trigger_threshold", tuning.triggerThreshold);
		entry.AddBool("swap_face_buttons", tuning.swapFaceButtons);
		updated.AddMessage("pad", &entry);
	}

	BPath path;
	status_t status = tuning_path(path);
	if (status != B_OK)
		return status;
	BPath parent;
	path.GetParent(&parent);
	create_directory(parent.Path(), 0755);
	BFile file(path.Path(), B_WRITE_ONLY | B_CREATE_FILE | B_ERASE_FILE);
	status = file.InitCheck();
	if (status == B_OK)
		status = updated.Flatten(&file);
	return status;
}


}	// namespace BPrivate
