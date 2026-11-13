/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 *
 * SDL game controller mappings (roadmap F-42); see GamepadMapping.h.
 *
 * The format is SDL's ("GUID,name,a:b0,leftx:a0,dpup:h0.1,...", with "+"
 * and "-" for half axes and "~" for inverted ones), so the community
 * database (SDL_GameControllerDB, zlib license) can be shipped as it is and
 * the mappings the Gamepads preferences save also work in SDL programs.
 */


#include <GamepadMapping.h>

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <Autolock.h>
#include <File.h>
#include <FindDirectory.h>
#include <Locker.h>
#include <ObjectList.h>
#include <Path.h>


namespace BPrivate {


const char* kGamepadSystemDatabase
	= "/boot/system/data/gamepad/gamecontrollerdb.txt";
const char* kGamepadUserDatabase = "Gamepads/mappings";


static const char* kButtonNames[B_GAMEPAD_BUTTON_COUNT] = {
	"a", "b", "x", "y", "back", "guide", "start", "leftstick", "rightstick",
	"leftshoulder", "rightshoulder", "dpup", "dpdown", "dpleft", "dpright",
	"misc1", "paddle1", "paddle2", "paddle3", "paddle4", "touchpad"
};

static const char* kAxisNames[B_GAMEPAD_AXIS_COUNT] = {
	"leftx", "lefty", "rightx", "righty", "lefttrigger", "righttrigger"
};


static inline bool
test_bit(const uint8* bits, uint32 bit)
{
	return (bits[bit / 8] & (1 << (bit % 8))) != 0;
}


// #pragma mark - GamepadDevice


void
GamepadDevice::Enumerate()
{
	buttonCount = 0;
	for (uint32 code = BTN_JOYSTICK; code < KEY_MAX; code++) {
		if (test_bit(keyBits, code))
			buttonCodes[buttonCount++] = code;
	}
	for (uint32 code = 0; code < BTN_JOYSTICK; code++) {
		if (test_bit(keyBits, code))
			buttonCodes[buttonCount++] = code;
	}

	axisCount = 0;
	for (uint32 code = 0; code < ABS_MAX; code++) {
		if (code == ABS_HAT0X) {
			code = ABS_HAT3Y;
			continue;
		}
		if (test_bit(absBits, code))
			axisCodes[axisCount++] = code;
	}

	hatCount = 0;
	for (uint32 hat = 0; hat < 4; hat++) {
		uint32 code = ABS_HAT0X + hat * 2;
		if (test_bit(absBits, code) || test_bit(absBits, code + 1))
			hatCodes[hatCount++] = code;
	}
}


bool
GamepadDevice::HasKey(uint32 code) const
{
	return code < KEY_CNT && test_bit(keyBits, code);
}


bool
GamepadDevice::KeyDown(uint32 code) const
{
	return code < KEY_CNT && test_bit(keyState, code);
}


int32
GamepadDevice::AbsValue(uint32 code) const
{
	return code < ABS_CNT ? abs[code].value : 0;
}


static int16
normalize(const struct input_absinfo& info)
{
	int64 range = (int64)info.maximum - info.minimum;
	if (range <= 0)
		return 0;
	int64 value = ((int64)info.value - info.minimum) * 65535 / range - 32768;
	if (value < -32768)
		value = -32768;
	if (value > 32767)
		value = 32767;
	return (int16)value;
}


int16
GamepadDevice::NormalizedAxis(int32 index) const
{
	if (index < 0 || index >= axisCount)
		return 0;
	return normalize(abs[axisCodes[index]]);
}


uint8
GamepadDevice::HatValue(int32 hat) const
{
	if (hat < 0 || hat >= hatCount)
		return 0;
	int32 x = abs[hatCodes[hat]].value;
	int32 y = abs[hatCodes[hat] + 1].value;
	uint8 value = 0;
	if (y < 0)
		value |= 1;
	if (x > 0)
		value |= 2;
	if (y > 0)
		value |= 4;
	if (x < 0)
		value |= 8;
	return value;
}


void
GamepadDevice::GUID(char string[33]) const
{
	uint8 guid[16];
	memset(guid, 0, sizeof(guid));
	guid[0] = id.bustype & 0xff;
	guid[1] = id.bustype >> 8;
	if (id.vendor != 0 && id.product != 0) {
		guid[4] = id.vendor & 0xff;
		guid[5] = id.vendor >> 8;
		guid[8] = id.product & 0xff;
		guid[9] = id.product >> 8;
		guid[12] = id.version & 0xff;
		guid[13] = id.version >> 8;
	} else
		memcpy(guid + 4, name, strnlen(name, 12));

	for (int i = 0; i < 16; i++)
		sprintf(string + i * 2, "%02x", guid[i]);
	string[32] = '\0';
}


// #pragma mark - GamepadMapping


GamepadMapping::GamepadMapping()
{
	memset(fButtons, 0, sizeof(fButtons));
	memset(fAxes, 0, sizeof(fAxes));
}


const char*
GamepadMapping::ButtonName(gamepad_button button)
{
	return button < B_GAMEPAD_BUTTON_COUNT ? kButtonNames[button] : NULL;
}


const char*
GamepadMapping::AxisName(gamepad_axis axis)
{
	return axis < B_GAMEPAD_AXIS_COUNT ? kAxisNames[axis] : NULL;
}


bool
GamepadMapping::_ParseBinding(const char* value, Binding& binding)
{
	memset(&binding, 0, sizeof(binding));
	if (*value == '+') {
		binding.sourceRange = RANGE_POSITIVE;
		value++;
	} else if (*value == '-') {
		binding.sourceRange = RANGE_NEGATIVE;
		value++;
	}

	char* end;
	switch (*value) {
		case 'b':
			binding.sourceType = SOURCE_BUTTON;
			binding.index = strtoul(value + 1, &end, 10);
			return end != value + 1;
		case 'a':
			binding.sourceType = SOURCE_AXIS;
			binding.index = strtoul(value + 1, &end, 10);
			if (end == value + 1)
				return false;
			binding.invert = *end == '~';
			return true;
		case 'h':
		{
			binding.sourceType = SOURCE_HAT;
			binding.index = strtoul(value + 1, &end, 10);
			if (*end != '.')
				return false;
			binding.hatMask = strtoul(end + 1, NULL, 10);
			return true;
		}
	}
	return false;
}


bool
GamepadMapping::Parse(const char* line)
{
	memset(fButtons, 0, sizeof(fButtons));
	memset(fAxes, 0, sizeof(fAxes));
	fLine = line;
	fLine.RemoveAll("\r");
	fLine.RemoveAll("\n");

	const char* comma = strchr(line, ',');
	if (comma == NULL || comma - line != 32)
		return false;
	const char* nameEnd = strchr(comma + 1, ',');
	if (nameEnd == NULL)
		return false;
	fName.SetTo(comma + 1, nameEnd - comma - 1);

	bool any = false;
	const char* item = nameEnd + 1;
	while (*item != '\0' && *item != '\n') {
		const char* next = strchr(item, ',');
		size_t length = next != NULL ? (size_t)(next - item) : strlen(item);
		char entry[64];
		if (length > 0 && length < sizeof(entry)) {
			memcpy(entry, item, length);
			entry[length] = '\0';
			char* colon = strchr(entry, ':');
			if (colon != NULL) {
				*colon = '\0';
				char* key = entry;
				uint8 targetRange = RANGE_FULL;
				if (*key == '+') {
					targetRange = RANGE_POSITIVE;
					key++;
				} else if (*key == '-') {
					targetRange = RANGE_NEGATIVE;
					key++;
				}
				Binding binding;
				if (_ParseBinding(colon + 1, binding)) {
					binding.targetRange = targetRange;
					for (int i = 0; i < B_GAMEPAD_BUTTON_COUNT; i++) {
						if (strcmp(key, kButtonNames[i]) == 0) {
							fButtons[i] = binding;
							any = true;
						}
					}
					for (int i = 0; i < B_GAMEPAD_AXIS_COUNT; i++) {
						if (strcmp(key, kAxisNames[i]) == 0) {
							fAxes[i][targetRange == RANGE_NEGATIVE ? 1 : 0]
								= binding;
							any = true;
						}
					}
				}
			}
		}
		if (next == NULL)
			break;
		item = next + 1;
	}
	return any;
}


bool
GamepadMapping::SetDefault(const GamepadDevice& device)
{
	if (!device.HasKey(BTN_SOUTH))
		return false;

	memset(fButtons, 0, sizeof(fButtons));
	memset(fAxes, 0, sizeof(fAxes));

	struct {
		gamepad_button	button;
		uint16			code;
	} keys[] = {
		{ B_GAMEPAD_BUTTON_A, BTN_SOUTH },
		{ B_GAMEPAD_BUTTON_B, BTN_EAST },
		{ B_GAMEPAD_BUTTON_X, BTN_X },
		{ B_GAMEPAD_BUTTON_Y, BTN_Y },
		{ B_GAMEPAD_BUTTON_BACK, BTN_SELECT },
		{ B_GAMEPAD_BUTTON_GUIDE, BTN_MODE },
		{ B_GAMEPAD_BUTTON_START, BTN_START },
		{ B_GAMEPAD_BUTTON_LEFT_STICK, BTN_THUMBL },
		{ B_GAMEPAD_BUTTON_RIGHT_STICK, BTN_THUMBR },
		{ B_GAMEPAD_BUTTON_LEFT_SHOULDER, BTN_TL },
		{ B_GAMEPAD_BUTTON_RIGHT_SHOULDER, BTN_TR },
		{ B_GAMEPAD_BUTTON_DPAD_UP, BTN_DPAD_UP },
		{ B_GAMEPAD_BUTTON_DPAD_DOWN, BTN_DPAD_DOWN },
		{ B_GAMEPAD_BUTTON_DPAD_LEFT, BTN_DPAD_LEFT },
		{ B_GAMEPAD_BUTTON_DPAD_RIGHT, BTN_DPAD_RIGHT },
		{ B_GAMEPAD_BUTTON_MISC1, KEY_RECORD },
		{ B_GAMEPAD_BUTTON_PADDLE1, BTN_GRIPR },
		{ B_GAMEPAD_BUTTON_PADDLE2, BTN_GRIPL },
		{ B_GAMEPAD_BUTTON_PADDLE3, BTN_GRIPR2 },
		{ B_GAMEPAD_BUTTON_PADDLE4, BTN_GRIPL2 },
	};
	for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
		if (!device.HasKey(keys[i].code))
			continue;
		fButtons[keys[i].button].sourceType = SOURCE_KEY_CODE;
		fButtons[keys[i].button].index = keys[i].code;
	}

	// the D-pad as a hat, if it is not made of buttons
	if (!device.HasKey(BTN_DPAD_UP)
		&& test_bit(device.absBits, ABS_HAT0X)) {
		static const uint8 kMasks[] = { 1, 4, 8, 2 };
		for (int i = 0; i < 4; i++) {
			Binding& binding = fButtons[B_GAMEPAD_BUTTON_DPAD_UP + i];
			binding.sourceType = SOURCE_HAT;
			binding.index = 0;
			binding.hatMask = kMasks[i];
		}
	}

	struct {
		gamepad_axis	axis;
		uint16			code;
	} axes[] = {
		{ B_GAMEPAD_AXIS_LEFT_X, ABS_X },
		{ B_GAMEPAD_AXIS_LEFT_Y, ABS_Y },
		{ B_GAMEPAD_AXIS_RIGHT_X, ABS_RX },
		{ B_GAMEPAD_AXIS_RIGHT_Y, ABS_RY },
		{ B_GAMEPAD_AXIS_LEFT_TRIGGER, ABS_Z },
		{ B_GAMEPAD_AXIS_RIGHT_TRIGGER, ABS_RZ },
	};
	for (size_t i = 0; i < sizeof(axes) / sizeof(axes[0]); i++) {
		if (!test_bit(device.absBits, axes[i].code))
			continue;
		fAxes[axes[i].axis][0].sourceType = SOURCE_ABS_CODE;
		fAxes[axes[i].axis][0].index = axes[i].code;
	}
	if (device.HasKey(BTN_TL2) && fAxes[B_GAMEPAD_AXIS_LEFT_TRIGGER][0]
			.sourceType == SOURCE_NONE) {
		fAxes[B_GAMEPAD_AXIS_LEFT_TRIGGER][0].sourceType = SOURCE_KEY_CODE;
		fAxes[B_GAMEPAD_AXIS_LEFT_TRIGGER][0].index = BTN_TL2;
	}
	if (device.HasKey(BTN_TR2) && fAxes[B_GAMEPAD_AXIS_RIGHT_TRIGGER][0]
			.sourceType == SOURCE_NONE) {
		fAxes[B_GAMEPAD_AXIS_RIGHT_TRIGGER][0].sourceType = SOURCE_KEY_CODE;
		fAxes[B_GAMEPAD_AXIS_RIGHT_TRIGGER][0].index = BTN_TR2;
	}

	char guid[33];
	device.GUID(guid);
	fName = device.name;
	fLine.SetToFormat("%s,%s,default", guid, device.name);
	return true;
}


// The value of a binding's source: for buttons 0 or 32767, for axes
// -32768..32767 (or 0..32767 for a half), for hats 0 or 32767.
int32
GamepadMapping::_SourceValue(const GamepadDevice& device,
	const Binding& binding, bool forButton) const
{
	int32 value = 0;
	switch (binding.sourceType) {
		case SOURCE_BUTTON:
			if (binding.index < device.buttonCount)
				value = device.KeyDown(device.buttonCodes[binding.index])
					? 32767 : 0;
			return value;
		case SOURCE_KEY_CODE:
			return device.KeyDown(binding.index) ? 32767 : 0;
		case SOURCE_HAT:
			return (device.HatValue(binding.index) & binding.hatMask) != 0
				? 32767 : 0;
		case SOURCE_AXIS:
			value = device.NormalizedAxis(binding.index);
			break;
		case SOURCE_ABS_CODE:
			if (binding.index < ABS_CNT)
				value = normalize(device.abs[binding.index]);
			break;
		default:
			return 0;
	}

	if (binding.invert)
		value = -value - 1;
	if (binding.sourceRange == RANGE_POSITIVE)
		value = value > 0 ? value : 0;
	else if (binding.sourceRange == RANGE_NEGATIVE)
		value = value < 0 ? -(value + 1) : 0;
	else if (forButton)
		value = (value + 32768) / 2;
			// a full axis as a button: pressed past the middle
	return value;
}


void
GamepadMapping::Apply(const GamepadDevice& device, gamepad_state& state) const
{
	state.buttons = 0;
	for (int i = 0; i < B_GAMEPAD_BUTTON_COUNT; i++) {
		const Binding& binding = fButtons[i];
		if (binding.sourceType == SOURCE_NONE)
			continue;
		int32 value = _SourceValue(device, binding, true);
		if (value > 16384)
			state.buttons |= 1UL << i;
	}

	for (int i = 0; i < B_GAMEPAD_AXIS_COUNT; i++) {
		bool trigger = i == B_GAMEPAD_AXIS_LEFT_TRIGGER
			|| i == B_GAMEPAD_AXIS_RIGHT_TRIGGER;
		int32 value = 0;
		const Binding& positive = fAxes[i][0];
		const Binding& negative = fAxes[i][1];

		if (positive.sourceType != SOURCE_NONE) {
			bool isAxis = positive.sourceType == SOURCE_AXIS
				|| positive.sourceType == SOURCE_ABS_CODE;
			int32 source = _SourceValue(device, positive, false);
			if (positive.targetRange == RANGE_POSITIVE) {
				if (isAxis && positive.sourceRange == RANGE_FULL)
					source = (source + 32768) / 2;
				value += source > 0 ? source : 0;
			} else if (trigger && isAxis
				&& positive.sourceRange == RANGE_FULL) {
				// a full axis on a trigger: 0..32767 over its range
				value = (source + 32768) / 2;
			} else
				value = source;
		}
		if (negative.sourceType != SOURCE_NONE) {
			bool isAxis = negative.sourceType == SOURCE_AXIS
				|| negative.sourceType == SOURCE_ABS_CODE;
			int32 source = _SourceValue(device, negative, false);
			if (isAxis && negative.sourceRange == RANGE_FULL)
				source = (source + 32768) / 2;
			if (source < 0)
				source = 0;
			value -= source;
			if (value < -32768)
				value = -32768;
		}
		if (trigger && value < 0)
			value = 0;
		if (value > 32767)
			value = 32767;
		state.axes[i] = (int16)value;
	}
}


// #pragma mark - the database


struct MappingEntry {
	BString	guid;
	BString	line;
};

static BLocker sDatabaseLock("gamepad mappings");
static bool sDatabaseLoaded = false;
static BObjectList<MappingEntry, true> sAdded(16);
static BObjectList<MappingEntry, true> sUser(16);
static BObjectList<MappingEntry, true> sSystem(512);


static bool
platform_matches(const char* line)
{
	const char* platform = strstr(line, "platform:");
	if (platform == NULL)
		return true;
	// Renga's pads are evdev devices with Linux codes: Linux mappings fit.
	// "Haiku" lines, should the database get them, fit too.
	platform += strlen("platform:");
	return strncmp(platform, "Linux", 5) == 0
		|| strncmp(platform, "Haiku", 5) == 0;
}


static void
add_line(BObjectList<MappingEntry, true>& list, const char* line)
{
	if (line[0] == '#' || strlen(line) < 34 || line[32] != ','
		|| !platform_matches(line))
		return;
	MappingEntry* entry = new(std::nothrow) MappingEntry;
	if (entry == NULL)
		return;
	entry->guid.SetTo(line, 32);
	entry->guid.ToLower();
	entry->line = line;
	// a later line for the same pad wins, as in SDL
	for (int32 i = 0; i < list.CountItems(); i++) {
		if (list.ItemAt(i)->guid == entry->guid) {
			delete list.RemoveItemAt(i);
			break;
		}
	}
	list.AddItem(entry);
}


static void
load_file(BObjectList<MappingEntry, true>& list, const char* path)
{
	list.MakeEmpty();
	BFile file(path, B_READ_ONLY);
	off_t size;
	if (file.InitCheck() != B_OK || file.GetSize(&size) != B_OK
		|| size <= 0 || size > 4 * 1024 * 1024)
		return;
	char* buffer = (char*)malloc(size + 1);
	if (buffer == NULL)
		return;
	ssize_t bytes = file.Read(buffer, size);
	if (bytes > 0) {
		buffer[bytes] = '\0';
		char* line = buffer;
		while (line != NULL && *line != '\0') {
			char* next = strchr(line, '\n');
			if (next != NULL)
				*next++ = '\0';
			char* cr = strchr(line, '\r');
			if (cr != NULL)
				*cr = '\0';
			add_line(list, line);
			line = next;
		}
	}
	free(buffer);
}


static void
load_database()
{
	if (sDatabaseLoaded)
		return;
	load_file(sSystem, kGamepadSystemDatabase);
	BPath path;
	if (find_directory(B_USER_SETTINGS_DIRECTORY, &path) == B_OK
		&& path.Append(kGamepadUserDatabase) == B_OK)
		load_file(sUser, path.Path());
	sDatabaseLoaded = true;
}


static const MappingEntry*
find_in(BObjectList<MappingEntry, true>& list, const BString& guid)
{
	for (int32 i = list.CountItems() - 1; i >= 0; i--) {
		if (list.ItemAt(i)->guid == guid)
			return list.ItemAt(i);
	}
	return NULL;
}


status_t
gamepad_find_mapping(const char* guidString, BString& line)
{
	BAutolock locker(sDatabaseLock);
	load_database();

	BString guid(guidString);
	guid.ToLower();
	if (guid.Length() != 32)
		return B_BAD_VALUE;

	// like SDL: the exact GUID, then without the name checksum, then
	// without the version as well
	BString candidates[3];
	candidates[0] = guid;
	candidates[1] = guid;
	candidates[1].Remove(4, 4).Insert("0000", 4);
	candidates[2] = candidates[1];
	candidates[2].Remove(24, 4).Insert("0000", 24);

	for (int i = 0; i < 3; i++) {
		const MappingEntry* entry = find_in(sAdded, candidates[i]);
		if (entry == NULL)
			entry = find_in(sUser, candidates[i]);
		if (entry == NULL)
			entry = find_in(sSystem, candidates[i]);
		if (entry == NULL && i == 2) {
			// the database's entry may carry a version where the pad
			// reports none, or another one: compare without it
			BObjectList<MappingEntry, true>* lists[]
				= { &sAdded, &sUser, &sSystem };
			for (int l = 0; l < 3 && entry == NULL; l++) {
				for (int32 j = lists[l]->CountItems() - 1; j >= 0; j--) {
					BString other = lists[l]->ItemAt(j)->guid;
					other.Remove(24, 4).Insert("0000", 24);
					other.Remove(4, 4).Insert("0000", 4);
					if (other == candidates[2]) {
						entry = lists[l]->ItemAt(j);
						break;
					}
				}
			}
		}
		if (entry != NULL) {
			line = entry->line;
			return B_OK;
		}
	}
	return B_NAME_NOT_FOUND;
}


status_t
gamepad_add_mapping(const char* line)
{
	if (line == NULL || strlen(line) < 34 || line[32] != ',')
		return B_BAD_VALUE;
	BAutolock locker(sDatabaseLock);
	int32 count = sAdded.CountItems();
	add_line(sAdded, line);
	return sAdded.CountItems() >= count ? B_OK : B_BAD_VALUE;
}


void
gamepad_reload_mappings()
{
	BAutolock locker(sDatabaseLock);
	sDatabaseLoaded = false;
	load_database();
}


}	// namespace BPrivate
