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


#endif	// _USB_KDL_KEYBOARD_H
