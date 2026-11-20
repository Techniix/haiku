/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 */


#include "KeyboardDebugLayout.h"

#include <usb/USB_hid.h>
#include "HIDParser.h"
#include "HIDReport.h"
#include "HIDReportItem.h"


void
compute_keyboard_debug_layout(HIDReport& input, HIDReport* output,
	usb_kdl_keyboard_layout& layout)
{
	memset(&layout, 0, sizeof(layout));
	layout.modifier_offset = -1;
	layout.key_offset = -1;
	layout.report_id = input.Parser()->UsesReportIDs() ? input.ID() : 0;
	layout.report_size = input.ReportSize();

	int32 modifiers = 0;
	int32 modifierByte = -1;
	bool modifiersInOneByte = true;
	int32 arrayKeys = 0;
	int32 arrayOffset = -1;
	int32 bitmapBits = 0;
	int32 bitmapOffset = -1;
	uint32 bitmapFirstUsage = 0;

	for (uint32 i = 0; i < input.CountItems(); i++) {
		HIDReportItem* item = input.ItemAt(i);
		if (!item->HasData() || item->UsagePage() != B_HID_USAGE_PAGE_KEYBOARD)
			continue;
		uint16 usage = item->UsageID();
		if (!item->Array() && item->BitCount() == 1
			&& usage >= B_HID_UID_KB_LEFT_CONTROL
			&& usage <= B_HID_UID_KB_RIGHT_GUI) {
			if (modifierByte < 0)
				modifierByte = item->ByteOffset();
			int32 bit = usage - B_HID_UID_KB_LEFT_CONTROL;
			if ((int32)item->ByteOffset() != modifierByte
				|| item->Shift() != bit) {
				modifiersInOneByte = false;
			}
			modifiers++;
		} else if (item->Array() && item->BitCount() == 8) {
			// key array: consecutive bytes
			if (arrayOffset < 0)
				arrayOffset = item->ByteOffset();
			if ((int32)item->ByteOffset() == arrayOffset + arrayKeys)
				arrayKeys++;
		} else if (!item->Array() && item->BitCount() == 1) {
			// key bitmap ("N-key rollover"): one bit per usage, in order
			int32 bit = (item->ByteOffset() - (bitmapOffset < 0
				? item->ByteOffset() : bitmapOffset)) * 8 + item->Shift();
			if (bitmapOffset < 0) {
				bitmapOffset = item->ByteOffset();
				bitmapFirstUsage = usage - item->Shift();
				bit = item->Shift();
			}
			if ((uint32)usage == bitmapFirstUsage + bit
				&& bit + 1 > bitmapBits)
				bitmapBits = bit + 1;
		}
	}

	if (modifiers == 8 && modifiersInOneByte)
		layout.modifier_offset = modifierByte;

	if (arrayKeys >= bitmapBits / 8 && arrayKeys > 0) {
		layout.key_offset = arrayOffset;
		layout.key_count = arrayKeys > USB_KDL_MAX_KEYS
			? USB_KDL_MAX_KEYS : arrayKeys;
	} else if (bitmapBits > 0) {
		layout.bitmap = 1;
		layout.key_offset = bitmapOffset;
		layout.bitmap_first_usage = bitmapFirstUsage;
		layout.bitmap_bits = bitmapBits;
	} else
		return;		// no keys: buttons, consumer keys

	// a full keyboard scores highest; a mouse's or remote's keyboard
	// interface usually has few keys and no LEDs
	int32 score = 1;
	if (layout.modifier_offset >= 0)
		score += 2;
	if (layout.key_count >= 6 || layout.bitmap_bits >= 100)
		score += 2;
	if (output != NULL)
		score += 2;
	if (layout.report_id == 0)
		score += 1;		// a keyboard of its own, not one report of many
	layout.score = score;
}
