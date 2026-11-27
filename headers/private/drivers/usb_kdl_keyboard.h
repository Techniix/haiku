/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 */
#ifndef _USB_KDL_KEYBOARD_H
#define _USB_KDL_KEYBOARD_H


/*!	The layout of a USB keyboard's input report for the kernel debugger
	(Renga, batch 4: KDL keyboard input with USB keyboards on real hardware).

	usb_hid describes the keyboard it hands to the debugger with this
	(debug variables of "get_usb_keyboard_config"), and the usb_keyboard
	debugger add-on turns each report into the boot-protocol form it has
	always parsed (byte 0 modifiers, bytes 2.. pressed key usages). Before,
	the add-on assumed the boot layout for every keyboard, which is wrong
	for keyboards with report IDs (a byte in front) and for "N-key
	rollover" keyboards (a bitmap of keys), common on gaming and wireless
	keyboards, so the debugger prompt got no or wrong keys.
*/


#include <SupportDefs.h>
#include <string.h>


#define USB_KDL_MAX_KEYS	32


typedef struct usb_kdl_keyboard_layout {
	int32	score;				// how keyboard-like; 0: not usable
	uint8	report_id;			// 0: the device uses no report IDs
	int16	modifier_offset;	// byte of the 8 modifier bits, -1: none
	int16	key_offset;			// array: first key byte; bitmap: first byte
	uint8	key_count;			// array: number of key bytes
	uint8	bitmap;				// 1: keys are a bitmap
	uint16	bitmap_first_usage;	// usage of bit 0 of the bitmap
	uint16	bitmap_bits;		// number of bits
	uint16	report_size;		// bytes after the report ID
} usb_kdl_keyboard_layout;


/*!	Turns one input report (as transferred, with the report ID byte if the
	device uses them) into boot form: \a boot[0] modifiers, \a boot[1] 0,
	\a boot[2...] pressed usages. Returns the boot length (2 + keys), or 0
	if the report is not the keyboard's (another report ID) or too short.
*/
static inline size_t
usb_kdl_keyboard_normalize(const usb_kdl_keyboard_layout* layout,
	const uint8* data, size_t length, uint8* boot, size_t bootSize)
{
	if (bootSize < 2)
		return 0;
	if (layout->report_id != 0) {
		if (length < 1 || data[0] != layout->report_id)
			return 0;
		data++;
		length--;
	}
	memset(boot, 0, bootSize);
	if (layout->modifier_offset >= 0
		&& (size_t)layout->modifier_offset < length)
		boot[0] = data[layout->modifier_offset];

	size_t count = 2;
	if (layout->bitmap) {
		for (uint32 bit = 0; bit < layout->bitmap_bits && count < bootSize;
				bit++) {
			size_t byte = layout->key_offset + bit / 8;
			if (byte >= length)
				break;
			if ((data[byte] & (1 << (bit % 8))) == 0)
				continue;
			uint32 usage = layout->bitmap_first_usage + bit;
			if (usage >= 0xe0 && usage <= 0xe7)
				continue;	// modifiers have their byte
			if (usage <= 0xff)
				boot[count++] = (uint8)usage;
		}
		return count;
	}
	for (uint32 i = 0; i < layout->key_count && count < bootSize; i++) {
		size_t byte = layout->key_offset + i;
		if (byte >= length)
			break;
		boot[count++] = data[byte];
	}
	return count;
}


/*!	Renga (batch 14, maintainer's hardware: an 8BitDo Retro keyboard dongle,
	2dc8:5201, and a second USB keyboard typed nothing in the debugger):
	usb_hid hands the debugger every keyboard it has, not only the most
	keyboard-like one, and per keyboard every keyboard report of its
	interface. The dongle's keyboard interface describes three keyboard
	reports (ID 1 a six-key array, IDs 10 and 12 key bitmaps) and sends the
	bitmap ones; the debugger only accepted the best-scored report (ID 1),
	so every key was dropped, and the other keyboard was never polled
	because only one keyboard was. The add-on finds the table through the
	debug variable "_usbKeyboardTable" (set by "get_usb_keyboard_config").
*/

#define USB_KDL_MAX_KEYBOARDS		4
#define USB_KDL_MAX_LAYOUTS			4
#define USB_KDL_KEYBOARD_TABLE_VERSION	1

typedef struct usb_kdl_keyboard {
	uint64					pipe;			// usb_id of the interrupt IN pipe
	uint16					transfer_size;	// bytes per transfer
	uint8					layout_count;
	const void*				owners[USB_KDL_MAX_LAYOUTS];	// usb_hid handlers
	usb_kdl_keyboard_layout	layouts[USB_KDL_MAX_LAYOUTS];
} usb_kdl_keyboard;

typedef struct usb_kdl_keyboard_table {
	uint32					version;		// USB_KDL_KEYBOARD_TABLE_VERSION
	uint32					count;			// best keyboard first
	usb_kdl_keyboard		keyboards[USB_KDL_MAX_KEYBOARDS];
} usb_kdl_keyboard_table;

// What the debugger keeps per keyboard: the last boot form of each layout.
typedef struct usb_kdl_keyboard_state {
	uint8	boot[USB_KDL_MAX_LAYOUTS][2 + USB_KDL_MAX_KEYS];
} usb_kdl_keyboard_state;


static inline int32
usb_kdl_keyboard_best_score(const usb_kdl_keyboard* keyboard)
{
	int32 best = 0;
	for (uint32 i = 0; i < keyboard->layout_count; i++) {
		if (keyboard->layouts[i].score > best)
			best = keyboard->layouts[i].score;
	}
	return best;
}


//!	Best keyboard first (stable: an earlier keyboard wins a tie).
static inline void
usb_kdl_keyboard_table_sort(usb_kdl_keyboard_table* table)
{
	for (uint32 i = 1; i < table->count; i++) {
		for (uint32 j = i; j > 0 && usb_kdl_keyboard_best_score(
				&table->keyboards[j]) > usb_kdl_keyboard_best_score(
				&table->keyboards[j - 1]); j--) {
			usb_kdl_keyboard swap = table->keyboards[j];
			table->keyboards[j] = table->keyboards[j - 1];
			table->keyboards[j - 1] = swap;
		}
	}
}


/*!	Adds the keyboard report \a layout of \a owner (a usb_hid handler) on
	the interrupt pipe \a pipe. When the table or the keyboard is full, a
	better-scored entry replaces the worst one. Returns whether it was
	added.
*/
static inline bool
usb_kdl_keyboard_table_add(usb_kdl_keyboard_table* table, uint64 pipe,
	uint16 transferSize, const void* owner,
	const usb_kdl_keyboard_layout* layout)
{
	if (layout->score <= 0)
		return false;
	table->version = USB_KDL_KEYBOARD_TABLE_VERSION;

	usb_kdl_keyboard* keyboard = NULL;
	for (uint32 i = 0; i < table->count; i++) {
		if (table->keyboards[i].pipe == pipe) {
			keyboard = &table->keyboards[i];
			break;
		}
	}
	if (keyboard == NULL) {
		if (table->count < USB_KDL_MAX_KEYBOARDS)
			keyboard = &table->keyboards[table->count++];
		else {
			// replace the worst keyboard if this one looks better
			keyboard = &table->keyboards[table->count - 1];
			if (usb_kdl_keyboard_best_score(keyboard) >= layout->score)
				return false;
		}
		memset(keyboard, 0, sizeof(*keyboard));
		keyboard->pipe = pipe;
	}
	if (transferSize > keyboard->transfer_size)
		keyboard->transfer_size = transferSize;

	uint32 slot = keyboard->layout_count;
	if (slot == USB_KDL_MAX_LAYOUTS) {
		slot = 0;
		for (uint32 i = 1; i < USB_KDL_MAX_LAYOUTS; i++) {
			if (keyboard->layouts[i].score < keyboard->layouts[slot].score)
				slot = i;
		}
		if (keyboard->layouts[slot].score >= layout->score)
			return false;
	} else
		keyboard->layout_count++;
	keyboard->owners[slot] = owner;
	keyboard->layouts[slot] = *layout;

	usb_kdl_keyboard_table_sort(table);
	return true;
}


//!	Removes everything \a owner added (its device went away).
static inline void
usb_kdl_keyboard_table_remove(usb_kdl_keyboard_table* table,
	const void* owner)
{
	for (uint32 i = 0; i < table->count; i++) {
		usb_kdl_keyboard* keyboard = &table->keyboards[i];
		for (uint32 j = 0; j < keyboard->layout_count; j++) {
			if (keyboard->owners[j] != owner)
				continue;
			keyboard->layout_count--;
			keyboard->owners[j] = keyboard->owners[keyboard->layout_count];
			keyboard->layouts[j] = keyboard->layouts[keyboard->layout_count];
			j--;
		}
		if (keyboard->layout_count == 0) {
			table->count--;
			for (uint32 k = i; k < table->count; k++)
				table->keyboards[k] = table->keyboards[k + 1];
			i--;
		}
	}
	usb_kdl_keyboard_table_sort(table);
}


/*!	Turns one report of \a keyboard into boot form with all its keyboard
	reports taken together: the layout whose report ID matches updates its
	part of \a state, and \a boot gets the modifiers and keys of all parts
	(a key held in one report stays held while another report changes).
	Returns the boot length, or 0 if no layout takes the report.
*/
static inline size_t
usb_kdl_keyboard_merge(const usb_kdl_keyboard* keyboard,
	usb_kdl_keyboard_state* state, const uint8* data, size_t length,
	uint8* boot, size_t bootSize)
{
	if (bootSize < 2)
		return 0;
	bool taken = false;
	for (uint32 i = 0; i < keyboard->layout_count; i++) {
		if (usb_kdl_keyboard_normalize(&keyboard->layouts[i], data, length,
				state->boot[i], sizeof(state->boot[i])) != 0) {
			taken = true;
			break;
		}
	}
	if (!taken)
		return 0;

	memset(boot, 0, bootSize);
	size_t count = 2;
	for (uint32 i = 0; i < keyboard->layout_count; i++) {
		boot[0] |= state->boot[i][0];
		for (size_t k = 2; k < sizeof(state->boot[i]) && count < bootSize;
				k++) {
			uint8 usage = state->boot[i][k];
			if (usage == 0)
				continue;
			bool seen = false;
			for (size_t j = 2; j < count; j++)
				seen |= boot[j] == usage;
			if (!seen)
				boot[count++] = usage;
		}
	}
	return count;
}


#endif	// _USB_KDL_KEYBOARD_H
