/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 */
#ifndef MAPPING_LEARNER_H
#define MAPPING_LEARNER_H


/*!	The button-mapping wizard's engine (Gamepads preferences, "Map
	buttons…", and "Gamepads --learn <device>" for scripts and the harness):
	asks for each standard control in turn, watches the pad's evdev device
	for the input that answers, and builds an SDL mapping line from them,
	the format of the system's mapping database and of the user's mappings
	file (~/config/settings/Gamepads/mappings), which BGamepad reads.

	Sources are numbered as SDL numbers them on Linux (GamepadDevice::
	Enumerate(): buttons by key code from BTN_JOYSTICK on, then the lower
	codes; axes by code without the hats; hats in pairs), so the line also
	fits SDL's Linux joystick back end. An input counts once it is clearly
	pressed or moved (half the range from where it rested); the next step
	waits until everything is back at rest, so one press never answers two
	steps.
*/


#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <Directory.h>
#include <File.h>
#include <FindDirectory.h>
#include <Path.h>
#include <String.h>

#include <GamepadMapping.h>

#include <linux/input.h>


struct LearnStep {
	const char*	target;		// SDL name: "a", "leftx", ...
	const char*	prompt;
	bool		axis;		// a stick axis: the direction is learned
};


static const LearnStep kLearnSteps[] = {
	{ "a", "Press the bottom face button (A on Xbox-style pads)", false },
	{ "b", "Press the right face button (B)", false },
	{ "x", "Press the left face button (X)", false },
	{ "y", "Press the top face button (Y)", false },
	{ "dpup", "Press up on the D-pad", false },
	{ "dpdown", "Press down on the D-pad", false },
	{ "dpleft", "Press left on the D-pad", false },
	{ "dpright", "Press right on the D-pad", false },
	{ "leftshoulder", "Press the left shoulder button", false },
	{ "rightshoulder", "Press the right shoulder button", false },
	{ "lefttrigger", "Pull the left trigger", false },
	{ "righttrigger", "Pull the right trigger", false },
	{ "back", "Press Back (Select, View, Share, -)", false },
	{ "start", "Press Start (Menu, Options, +)", false },
	{ "guide", "Press the guide (Home) button", false },
	{ "leftstick", "Press the left stick in", false },
	{ "rightstick", "Press the right stick in", false },
	{ "leftx", "Push the left stick to the right", true },
	{ "lefty", "Push the left stick down", true },
	{ "rightx", "Push the right stick to the right", true },
	{ "righty", "Push the right stick down", true },
};
static const int32 kLearnStepCount
	= sizeof(kLearnSteps) / sizeof(kLearnSteps[0]);


class MappingLearner {
public:
	MappingLearner()
		:
		fFD(-1),
		fStep(0),
		fArmed(false)
	{
		memset(&fDevice, 0, sizeof(fDevice));
	}

	~MappingLearner()
	{
		Close();
	}

	status_t Open(const char* path)
	{
		Close();
		fFD = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
		if (fFD < 0)
			return errno;
		memset(&fDevice, 0, sizeof(fDevice));
		if (ioctl(fFD, EVIOCGID, &fDevice.id) < 0
			|| ioctl(fFD, EVIOCGNAME(sizeof(fDevice.name)), fDevice.name) < 0
			|| ioctl(fFD, EVIOCGBIT(EV_KEY, sizeof(fDevice.keyBits)),
				fDevice.keyBits) < 0) {
			Close();
			return B_BAD_TYPE;
		}
		ioctl(fFD, EVIOCGBIT(EV_ABS, sizeof(fDevice.absBits)),
			fDevice.absBits);
		for (uint32 code = 0; code < ABS_CNT; code++) {
			if ((fDevice.absBits[code / 8] & (1 << (code % 8))) != 0)
				ioctl(fFD, EVIOCGABS(code), &fDevice.abs[code]);
		}
		fDevice.Enumerate();
		for (int32 i = 0; i < fDevice.axisCount; i++)
			fRest[i] = fDevice.NormalizedAxis(i);
		fStep = 0;
		fArmed = true;
		for (int32 i = 0; i < kLearnStepCount; i++)
			fBinding[i] = "";
		return B_OK;
	}

	void Close()
	{
		if (fFD >= 0)
			close(fFD);
		fFD = -1;
	}

	int FileDescriptor() const { return fFD; }
	int32 Step() const { return fStep; }
	bool Done() const { return fStep >= kLearnStepCount; }
	const char* Prompt() const
		{ return Done() ? NULL : kLearnSteps[fStep].prompt; }
	const char* Name() const { return fDevice.name; }

	void Skip()
	{
		if (!Done())
			fBinding[fStep++] = "";
	}

	void Back()
	{
		if (fStep > 0)
			fBinding[--fStep] = "";
	}

	/*!	Reads what the pad sent; returns true when that answered the
		current step (the caller shows the next prompt).
	*/
	bool Poll()
	{
		if (fFD < 0 || Done())
			return false;
		struct input_event events[64];
		bool answered = false;
		ssize_t bytes;
		while ((bytes = read(fFD, events, sizeof(events))) > 0) {
			int count = bytes / sizeof(struct input_event);
			for (int i = 0; i < count; i++) {
				const struct input_event& event = events[i];
				if (event.type == EV_KEY && event.code < KEY_CNT) {
					if (event.value != 0) {
						fDevice.keyState[event.code / 8]
							|= 1 << (event.code % 8);
					} else {
						fDevice.keyState[event.code / 8]
							&= ~(1 << (event.code % 8));
					}
				} else if (event.type == EV_ABS && event.code < ABS_CNT)
					fDevice.abs[event.code].value = event.value;
				else if (event.type == EV_SYN && event.code == SYN_REPORT) {
					if (_Evaluate())
						answered = true;
				}
			}
		}
		return answered;
	}

	//!	The SDL mapping line for what was learned.
	BString Line() const
	{
		char guid[33];
		fDevice.GUID(guid);
		BString name(fDevice.name);
		name.ReplaceAll(",", " ");
		BString line;
		line << guid << "," << name << ",";
		for (int32 i = 0; i < kLearnStepCount; i++) {
			if (fBinding[i].IsEmpty())
				continue;
			line << kLearnSteps[i].target << ":" << fBinding[i] << ",";
		}
		line << "platform:Linux,";
		return line;
	}

	/*!	Puts \a line into the user's mappings file in place of any older
		line for the same pad (by GUID).
	*/
	static status_t Save(const BString& line)
	{
		BPath path;
		status_t status = find_directory(B_USER_SETTINGS_DIRECTORY, &path);
		if (status != B_OK || (status = path.Append("Gamepads/mappings"))
				!= B_OK)
			return status;
		BPath parent;
		path.GetParent(&parent);
		create_directory(parent.Path(), 0755);

		BString contents;
		BFile in(path.Path(), B_READ_ONLY);
		off_t size;
		if (in.InitCheck() == B_OK && in.GetSize(&size) == B_OK
			&& size > 0 && size < 1024 * 1024) {
			char* buffer = contents.LockBuffer(size + 1);
			ssize_t bytes = in.Read(buffer, size);
			contents.UnlockBuffer(bytes > 0 ? bytes : 0);
		}
		in.Unset();

		BString guid(line, 32);
		guid.ToLower();
		BString kept;
		int32 start = 0;
		while (start < contents.Length()) {
			int32 end = contents.FindFirst('\n', start);
			if (end < 0)
				end = contents.Length();
			BString current(contents.String() + start, end - start);
			BString currentGUID(current, 32);
			currentGUID.ToLower();
			if (!current.IsEmpty() && currentGUID != guid)
				kept << current << "\n";
			start = end + 1;
		}
		if (kept.IsEmpty())
			kept << "# Gamepads preferences: button mappings (SDL format)\n";
		kept << line << "\n";

		BFile out(path.Path(), B_WRITE_ONLY | B_CREATE_FILE | B_ERASE_FILE);
		status = out.InitCheck();
		if (status != B_OK)
			return status;
		ssize_t written = out.Write(kept.String(), kept.Length());
		return written == kept.Length() ? B_OK : B_IO_ERROR;
	}

private:
	bool _AtRest() const
	{
		for (int32 i = 0; i < fDevice.buttonCount; i++) {
			if (fDevice.KeyDown(fDevice.buttonCodes[i]))
				return false;
		}
		for (int32 i = 0; i < fDevice.axisCount; i++) {
			if (abs((int32)fDevice.NormalizedAxis(i) - fRest[i]) > 16384)
				return false;
		}
		for (int32 i = 0; i < fDevice.hatCount; i++) {
			if (fDevice.HatValue(i) != 0)
				return false;
		}
		return true;
	}

	bool _Evaluate()
	{
		if (!fArmed) {
			fArmed = _AtRest();
			return false;
		}
		BString binding;
		for (int32 i = 0; i < fDevice.buttonCount && binding.IsEmpty(); i++) {
			if (fDevice.KeyDown(fDevice.buttonCodes[i]))
				binding.SetToFormat("b%d", (int)i);
		}
		for (int32 i = 0; i < fDevice.hatCount && binding.IsEmpty(); i++) {
			uint8 value = fDevice.HatValue(i);
			if (value == 1 || value == 2 || value == 4 || value == 8)
				binding.SetToFormat("h%d.%d", (int)i, (int)value);
		}
		for (int32 i = 0; i < fDevice.axisCount && binding.IsEmpty(); i++) {
			int32 value = fDevice.NormalizedAxis(i);
			int32 moved = value - fRest[i];
			if (abs(moved) <= 16384)
				continue;
			const LearnStep& step = kLearnSteps[fStep];
			if (step.axis) {
				// pushed right or down: positive; the other way: inverted
				binding.SetToFormat("a%d%s", (int)i, moved < 0 ? "~" : "");
			} else if (fRest[i] < -16384) {
				// a trigger resting at one end: its whole range
				binding.SetToFormat("a%d", (int)i);
			} else {
				// half of a centred axis (a D-pad or trigger on one axis)
				binding.SetToFormat("%sa%d", moved > 0 ? "+" : "-", (int)i);
			}
		}
		if (binding.IsEmpty())
			return false;
		fBinding[fStep++] = binding;
		fArmed = false;
		return true;
	}

	int							fFD;
	BPrivate::GamepadDevice		fDevice;
	int32						fRest[ABS_CNT];
	int32						fStep;
	bool						fArmed;
	BString						fBinding[kLearnStepCount];
};


#endif	// MAPPING_LEARNER_H
