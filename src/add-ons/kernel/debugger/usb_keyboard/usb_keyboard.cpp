/*
 * Copyright 2009-2011, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Michael Lotz <mmlr@mlotz.ch>
 */

#include <debug.h>
#include <debugger_keymaps.h>

#include <usb_kdl_keyboard.h>

// Renga (batch 4): the transfer lands in a raw buffer and is turned into
// boot form with usb_hid's layout of the keyboard (report ID, modifier
// byte, key array or bitmap), see usb_kdl_keyboard.h.
// Renga (batch 14): every keyboard usb_hid knows, each with all keyboard
// reports of its interface, polled in turn; before only the best-scored
// report of the best keyboard was read (an 8BitDo dongle sending its
// bitmap reports and a second keyboard typed nothing).
static const size_t kBootLength = 2 + USB_KDL_MAX_KEYS;
static const size_t kMaxRawLength = 64;

struct debug_keyboard {
	void*					pipe;
	size_t					raw_length;
	usb_kdl_keyboard		keyboard;
	usb_kdl_keyboard_state	state;
	uint8					raw[kMaxRawLength];
	uint8					last[kBootLength];
};

static debug_keyboard sKeyboards[USB_KDL_MAX_KEYBOARDS];
static int32 sKeyboardCount = 0;
static int32 sNextKeyboard = 0;
static uint8 sUSBTransferData[kBootLength];
static size_t sUSBTransferLength = kBootLength;

// simple ring buffer
static int sBufferedChars[32];
static uint8 sBufferSize = sizeof(sBufferedChars) / sizeof(sBufferedChars[0]);
static uint8 sBufferedCharCount = 0;
static uint8 sBufferWriteIndex = 0;
static uint8 sBufferReadIndex = 0;

#define MODIFIER_CONTROL	0x01
#define MODIFIER_SHIFT		0x02
#define MODIFIER_ALT		0x04

static uint32 sModifierTable[] = {
	MODIFIER_CONTROL,
	MODIFIER_SHIFT,
	MODIFIER_ALT,
	0,
	MODIFIER_CONTROL,
	MODIFIER_SHIFT,
	MODIFIER_ALT,
	0
};

static uint8 sKeyTable[] = {
	0,	// ERROR
	0,	// ERROR
	0,	// ERROR
	0,	// ERROR
	30,	// A
	48,	// B
	46,	// C
	32,	// D
	18,	// E
	33,	// F
	34,	// G
	35,	// H
	23,	// I
	36,	// J
	37,	// K
	38,	// L
	50,	// M
	49,	// N
	24,	// O
	25,	// P
	16,	// Q
	19,	// R
	31,	// S
	20,	// T
	22,	// U
	47,	// V
	17,	// W
	45,	// X
	21,	// Y
	44,	// Z
	2,	// 1
	3,	// 2
	4,	// 3
	5,	// 4
	6,	// 5
	7,	// 6
	8,	// 7
	9,	// 8
	10,	// 9
	11,	// 0
	28,	// enter
	1,	// Esc
	14,	// Backspace
	15,	// Tab
	57,	// Space
	12,	// -
	13,	// =
	26,	// [
	27,	// ]
	43,	// backslash
	80,	// backslash
	39,	// ;
	40,	// '
	41,	// `
	51,	// ,
	52,	// .
	53,	// /
	0,	// Caps
	0,	// F1
	0,	// F2
	0,	// F3
	0,	// F4
	0,	// F5
	0,	// F6
	0,	// F7
	0,	// F8
	0,	// F9
	0,	// F10
	0,	// F11
	0,	// F12
	0,	// PrintScreen
	0,	// Scroll Lock
	0,	// Pause (0x7f with Ctrl)
	0,	// Insert
	0x80 | 'H',	// Home
	0x80 | '5',	// Page up
	0x80 | '3',	// Delete
	0x80 | 'F',	// End
	0x80 | '6',	// Page down
	0x80 | 'C',	// Right arrow
	0x80 | 'D',	// Left arrow
	0x80 | 'B',	// Down arrow
	0x80 | 'A',	// Up arrow
	0,	// Num Lock
	53,	// Pad /
	55,	// Pad *
	12,	// Pad -
	54,	// Pad +
	28,	// Pad Enter
	2,	// Pad 1
	3,	// Pad 2
	4,	// Pad 3
	5,	// Pad 4
	6,	// Pad 5
	7,	// Pad 6
	8,	// Pad 7
	9,	// Pad 8
	10,	// Pad 9
	11,	// Pad 0
	52,	// Pad .
	86,	// <
	0,	// Menu
	0,	// Power
	13	// Pad =
};

static size_t sKeyTableSize = sizeof(sKeyTable) / sizeof(sKeyTable[0]);


//!	Finds usb_hid's pipe object for \a keyboard; false if it has none.
static bool
add_keyboard(const usb_kdl_keyboard& keyboard)
{
	if (sKeyboardCount >= USB_KDL_MAX_KEYBOARDS || keyboard.pipe == 0
		|| keyboard.layout_count == 0 || keyboard.transfer_size == 0
		|| keyboard.transfer_size > kMaxRawLength) {
		return false;
	}

	set_debug_variable("_usbPipeID", keyboard.pipe);
	unset_debug_variable("_usbPipe");
	evaluate_debug_command("get_usb_pipe_for_id");
	void* pipe = (void*)get_debug_variable("_usbPipe", 0);
	if (pipe == NULL)
		return false;

	debug_keyboard& entry = sKeyboards[sKeyboardCount++];
	memset(&entry, 0, sizeof(entry));
	entry.pipe = pipe;
	entry.raw_length = keyboard.transfer_size;
	entry.keyboard = keyboard;
	return true;
}


static void
enter_debugger(void)
{
	sKeyboardCount = 0;
	sNextKeyboard = 0;
	if (!has_debugger_command("get_usb_keyboard_config")
		|| !has_debugger_command("get_usb_pipe_for_id")
		|| !has_debugger_command("usb_process_transfer")) {
		return;
	}

	unset_debug_variable("_usbKeyboardTable");
	unset_debug_variable("_usbReportSize");
	evaluate_debug_command("get_usb_keyboard_config");

	const usb_kdl_keyboard_table* table = (const usb_kdl_keyboard_table*)
		get_debug_variable("_usbKeyboardTable", 0);
	if (table != NULL && table->version == USB_KDL_KEYBOARD_TABLE_VERSION) {
		for (uint32 i = 0; i < table->count && i < USB_KDL_MAX_KEYBOARDS; i++)
			add_keyboard(table->keyboards[i]);
		return;
	}

	// a usb_hid without the table: its one keyboard, one layout (without
	// one at all, the boot layout)
	usb_kdl_keyboard keyboard;
	memset(&keyboard, 0, sizeof(keyboard));
	size_t reportSize = get_debug_variable("_usbReportSize", 0);
	if (reportSize == 0 || reportSize > kMaxRawLength)
		return;
	keyboard.pipe = get_debug_variable("_usbPipeID", 0);
	keyboard.transfer_size = reportSize;
	keyboard.layout_count = 1;
	usb_kdl_keyboard_layout& layout = keyboard.layouts[0];
	layout.score = 1;
	layout.report_id = get_debug_variable("_usbReportID", 0);
	layout.modifier_offset = (int16)(int64)get_debug_variable(
		"_usbModifierOffset", 0);
	layout.key_offset = (int16)(int64)get_debug_variable("_usbKeyOffset", 2);
	layout.key_count = get_debug_variable("_usbKeyCount",
		reportSize > 2 ? reportSize - 2 : 0);
	layout.bitmap = get_debug_variable("_usbKeyBitmap", 0);
	layout.bitmap_first_usage = get_debug_variable("_usbBitmapFirstUsage", 0);
	layout.bitmap_bits = get_debug_variable("_usbBitmapBits", 0);
	if (layout.key_offset < 0)
		return;
	add_keyboard(keyboard);
}


static void
exit_debugger(void)
{
	// make sure pending transfers are canceled
	for (int32 i = 0; i < sKeyboardCount; i++) {
		set_debug_variable("_usbPipe", (uint64)sKeyboards[i].pipe);
		evaluate_debug_command("usb_process_transfer cancel");
	}
	sKeyboardCount = 0;
}


static void
write_key(int key)
{
	sBufferedChars[sBufferWriteIndex++] = key;
	sBufferWriteIndex %= sBufferSize;
	sBufferedCharCount++;
}


static int
debugger_getchar(void)
{
	if (sKeyboardCount == 0)
		return -1;

	if (sBufferedCharCount == 0) {
		// one keyboard per call, in turn; each keeps its transfer pending
		debug_keyboard& keyboard = sKeyboards[sNextKeyboard];
		sNextKeyboard = (sNextKeyboard + 1) % sKeyboardCount;

		set_debug_variable("_usbPipe", (uint64)keyboard.pipe);
		set_debug_variable("_usbTransferData", (uint64)keyboard.raw);
		set_debug_variable("_usbTransferLength",
			(uint64)keyboard.raw_length);

		status_t status = evaluate_debug_command("usb_process_transfer");
		if (status == B_DEV_PENDING)
			return -1;

		if (status != B_OK) {
			// try clearing a possibly set halt due to toggle mismatches
			evaluate_debug_command("usb_clear_stall");
			return -1;
		}

		// another report of the same interface (media keys): not ours
		size_t keyEnd = usb_kdl_keyboard_merge(&keyboard.keyboard,
			&keyboard.state, keyboard.raw, keyboard.raw_length,
			sUSBTransferData, sizeof(sUSBTransferData));
		if (keyEnd == 0)
			return -1;

		bool phantomState = keyEnd > 2;
		for (size_t i = 2; i < keyEnd; i++) {
			if (sUSBTransferData[i] != 0x01) {
				phantomState = false;
				break;
			}
		}

		if (phantomState)
			return -1;

		uint8 modifiers = 0;
		for (uint32 i = 0; i < 8; i++) {
			if (sUSBTransferData[0] & (1 << i))
				modifiers |= sModifierTable[i];
		}

		uint8 *current = sUSBTransferData;
		uint8 *compare = keyboard.last;

		// Renga (F-59): Alt+SysRq+B in the debugger (SysRq is the
		// Print Screen usage, 0x46); the debugger's closing lines name
		// Alt+SysRq+B to restart.
		bool sysReqPressed = false;
		for (uint32 i = 2; i < sUSBTransferLength; i++) {
			if (current[i] == 0x46)
				sysReqPressed = true;
		}
		for (uint32 i = 2; i < sUSBTransferLength; i++) {
			if (current[i] == 0x00 || current[i] == 0x01)
				continue;

			bool found = false;
			for (uint32 j = 2; j < sUSBTransferLength; j++) {
				if (compare[j] == current[i]) {
					found = true;
					break;
				}
			}

			if (found)
				continue;

			if (current[i] >= sKeyTableSize)
				continue;

			int result = -1;
			uint8 key = sKeyTable[current[i]];
			if ((modifiers & MODIFIER_ALT) != 0 && sysReqPressed
				&& (key & 0x80) == 0 && kUnshiftedKeymap[key] == 'b'
				&& debug_emergency_key_pressed('b')) {
				continue;
			}
			if (key & 0x80) {
				write_key(27);
				write_key('[');

				key &= ~0x80;
				write_key(key);

				if (key == '5' || key == '6' || key == '3')
					write_key('~');

				continue;
			} else if (modifiers & MODIFIER_CONTROL) {
				char c = kShiftedKeymap[key];
				if (c >= 'A' && c <= 'Z')
					result = 0x1f & c;
			} else if (modifiers & MODIFIER_ALT)
				result = kAltedKeymap[key];
			else if (modifiers & MODIFIER_SHIFT)
				result = kShiftedKeymap[key];
			else
				result = kUnshiftedKeymap[key];

			if (result < 0)
				continue;

			write_key(result);
		}

		for (uint32 i = 0; i < sUSBTransferLength; i++)
			keyboard.last[i] = sUSBTransferData[i];
	}

	if (sBufferedCharCount == 0)
		return -1;

	int result = sBufferedChars[sBufferReadIndex++];
	sBufferReadIndex %= sBufferSize;
	sBufferedCharCount--;
	return result;
}


static status_t
std_ops(int32 op, ...)
{
	if (op == B_MODULE_INIT || op == B_MODULE_UNINIT)
		return B_OK;

	return B_BAD_VALUE;
}


static struct debugger_module_info sModuleInfo = {
	{
		"debugger/usb_keyboard/v1",
		0,
		&std_ops
	},

	&enter_debugger,
	&exit_debugger,
	NULL,
	&debugger_getchar
};

module_info *modules[] = {
	(module_info *)&sModuleInfo,
	NULL
};

