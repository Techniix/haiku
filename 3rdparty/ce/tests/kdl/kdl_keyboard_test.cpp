/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 */

/*!	The kernel debugger's USB keyboard (batch 4), without a debugger: the
	layout usb_hid computes for a keyboard (KeyboardDebugLayout, built here
	with USERLAND_HID) and the add-on's translation of its reports into boot
	form (usb_kdl_keyboard.h), on report descriptors of the kinds found on
	real hardware:
	  kdl_keyboard.boot       a boot-protocol keyboard with LEDs: modifier
	                          byte 0, six keys from byte 2
	  kdl_keyboard.report_id  keys in report 1 next to media keys in report
	                          2: the ID byte is skipped, report 2 ignored
	                          (the old add-on took the ID for modifiers)
	  kdl_keyboard.bitmap     an "N-key rollover" keyboard (a bitmap of 104
	                          keys): pressed bits become key usages (the old
	                          add-on read bitmap bytes as key codes)
	  kdl_keyboard.choice     a gaming mouse's keyboard interface (two keys,
	                          no LEDs) scores below a real keyboard, so the
	                          keyboard is chosen whichever comes first
	  kdl_keyboard.no_keys    a media-key-only interface is not usable
	Output format: see 3rdparty/ce/README.md.
*/


#include "HIDParser.h"
#include "HIDReport.h"
#include "HIDReportItem.h"
#include "KeyboardDebugLayout.h"

#include <usb/USB_hid.h>

#include <stdarg.h>
#include <stdio.h>
#include <string.h>


#define SUITE "kdl_keyboard"

static int sFailures = 0;


static void
report(const char* name, bool pass, const char* format, ...)
{
	char detail[400];
	va_list args;
	va_start(args, format);
	vsnprintf(detail, sizeof(detail), format, args);
	va_end(args);
	printf("CE-TEST %s.%s %s %s\n", SUITE, name, pass ? "PASS" : "FAIL",
		detail);
	fflush(stdout);
	if (!pass)
		sFailures++;
}


// The HID specification's boot keyboard descriptor (appendix B.1).
static const uint8 kBootKeyboard[] = {
	0x05, 0x01, 0x09, 0x06, 0xa1, 0x01,
	0x05, 0x07, 0x19, 0xe0, 0x29, 0xe7, 0x15, 0x00, 0x25, 0x01,
	0x75, 0x01, 0x95, 0x08, 0x81, 0x02,			// modifiers
	0x95, 0x01, 0x75, 0x08, 0x81, 0x01,			// reserved
	0x95, 0x05, 0x75, 0x01, 0x05, 0x08, 0x19, 0x01, 0x29, 0x05,
	0x91, 0x02, 0x95, 0x01, 0x75, 0x03, 0x91, 0x01,	// LEDs
	0x95, 0x06, 0x75, 0x08, 0x15, 0x00, 0x25, 0x65,
	0x05, 0x07, 0x19, 0x00, 0x29, 0x65, 0x81, 0x00,	// keys
	0xc0
};

// The same keys in report 1, media keys in report 2.
static const uint8 kReportIdKeyboard[] = {
	0x05, 0x01, 0x09, 0x06, 0xa1, 0x01, 0x85, 0x01,
	0x05, 0x07, 0x19, 0xe0, 0x29, 0xe7, 0x15, 0x00, 0x25, 0x01,
	0x75, 0x01, 0x95, 0x08, 0x81, 0x02,
	0x95, 0x01, 0x75, 0x08, 0x81, 0x01,
	0x95, 0x05, 0x75, 0x01, 0x05, 0x08, 0x19, 0x01, 0x29, 0x05,
	0x91, 0x02, 0x95, 0x01, 0x75, 0x03, 0x91, 0x01,
	0x95, 0x06, 0x75, 0x08, 0x15, 0x00, 0x25, 0x65,
	0x05, 0x07, 0x19, 0x00, 0x29, 0x65, 0x81, 0x00,
	0xc0,
	0x05, 0x0c, 0x09, 0x01, 0xa1, 0x01, 0x85, 0x02,
	0x15, 0x00, 0x26, 0xff, 0x03, 0x19, 0x00, 0x2a, 0xff, 0x03,
	0x75, 0x10, 0x95, 0x01, 0x81, 0x00,
	0xc0
};

// Modifiers, then a bitmap of usages 0..103 (13 bytes), with LEDs.
static const uint8 kBitmapKeyboard[] = {
	0x05, 0x01, 0x09, 0x06, 0xa1, 0x01,
	0x05, 0x07, 0x19, 0xe0, 0x29, 0xe7, 0x15, 0x00, 0x25, 0x01,
	0x75, 0x01, 0x95, 0x08, 0x81, 0x02,
	0x95, 0x05, 0x75, 0x01, 0x05, 0x08, 0x19, 0x01, 0x29, 0x05,
	0x91, 0x02, 0x95, 0x01, 0x75, 0x03, 0x91, 0x01,
	0x05, 0x07, 0x19, 0x00, 0x29, 0x67, 0x15, 0x00, 0x25, 0x01,
	0x75, 0x01, 0x95, 0x68, 0x81, 0x02,
	0xc0
};

// A gaming mouse's keyboard interface: modifiers and two keys, no LEDs.
static const uint8 kMouseKeyboard[] = {
	0x05, 0x01, 0x09, 0x06, 0xa1, 0x01,
	0x05, 0x07, 0x19, 0xe0, 0x29, 0xe7, 0x15, 0x00, 0x25, 0x01,
	0x75, 0x01, 0x95, 0x08, 0x81, 0x02,
	0x95, 0x02, 0x75, 0x08, 0x15, 0x00, 0x25, 0x65,
	0x05, 0x07, 0x19, 0x00, 0x29, 0x65, 0x81, 0x00,
	0xc0
};

// Media keys only.
static const uint8 kMediaKeys[] = {
	0x05, 0x0c, 0x09, 0x01, 0xa1, 0x01,
	0x15, 0x00, 0x26, 0xff, 0x03, 0x19, 0x00, 0x2a, 0xff, 0x03,
	0x75, 0x10, 0x95, 0x01, 0x81, 0x00,
	0xc0
};


struct Parsed {
	HIDParser parser;
	usb_kdl_keyboard_layout layout;
	status_t status;

	Parsed(const uint8* descriptor, size_t size)
		:
		parser(NULL)
	{
		memset(&layout, 0, sizeof(layout));
		status = parser.ParseReportDescriptor(descriptor, size);
		HIDReport* input = NULL;
		for (uint8 i = 0; i < parser.CountReports(HID_REPORT_TYPE_INPUT);
				i++) {
			HIDReport* report = parser.ReportAt(HID_REPORT_TYPE_INPUT, i);
			// the keyboard's report, as usb_hid's handler gets it
			for (uint32 j = 0; report != NULL && j < report->CountItems();
					j++) {
				if (report->ItemAt(j)->UsagePage()
						== B_HID_USAGE_PAGE_KEYBOARD) {
					input = report;
					break;
				}
			}
			if (input != NULL)
				break;
		}
		if (input == NULL)
			input = parser.ReportAt(HID_REPORT_TYPE_INPUT, 0);
		HIDReport* output = input != NULL ? parser.FindReport(
			HID_REPORT_TYPE_OUTPUT, input->ID()) : NULL;
		if (input != NULL)
			compute_keyboard_debug_layout(*input, output, layout);
	}
};


static bool
keys_are(const uint8* boot, size_t length, uint8 modifiers,
	const uint8* keys, size_t count)
{
	if (boot[0] != modifiers)
		return false;
	size_t found = 0;
	for (size_t i = 2; i < length; i++) {
		if (boot[i] == 0)
			continue;
		bool expected = false;
		for (size_t k = 0; k < count; k++)
			expected |= boot[i] == keys[k];
		if (!expected)
			return false;
		found++;
	}
	return found == count;
}


int
main()
{
	uint8 boot[2 + USB_KDL_MAX_KEYS];

	{
		Parsed keyboard(kBootKeyboard, sizeof(kBootKeyboard));
		const uint8 data[8] = { 0x02, 0, 0x04, 0x05, 0, 0, 0, 0 };
			// Shift + a + b
		size_t length = usb_kdl_keyboard_normalize(&keyboard.layout, data,
			sizeof(data), boot, sizeof(boot));
		const uint8 keys[] = { 0x04, 0x05 };
		report("boot", keyboard.layout.score > 0
				&& keyboard.layout.report_id == 0
				&& keyboard.layout.modifier_offset == 0
				&& keyboard.layout.key_offset == 2
				&& keyboard.layout.key_count == 6
				&& keys_are(boot, length, 0x02, keys, 2),
			"score %d, id %u, modifiers @%d, %u keys @%d",
			(int)keyboard.layout.score, keyboard.layout.report_id,
			keyboard.layout.modifier_offset, keyboard.layout.key_count,
			keyboard.layout.key_offset);
	}

	{
		Parsed keyboard(kReportIdKeyboard, sizeof(kReportIdKeyboard));
		const uint8 keysReport[9] = { 0x01, 0x01, 0, 0x06, 0, 0, 0, 0, 0 };
			// report 1: Ctrl + c
		const uint8 mediaReport[3] = { 0x02, 0xe9, 0x00 };
			// report 2: volume up
		size_t length = usb_kdl_keyboard_normalize(&keyboard.layout,
			keysReport, sizeof(keysReport), boot, sizeof(boot));
		const uint8 keys[] = { 0x06 };
		bool keysOK = keys_are(boot, length, 0x01, keys, 1);
		size_t mediaLength = usb_kdl_keyboard_normalize(&keyboard.layout,
			mediaReport, sizeof(mediaReport), boot, sizeof(boot));
		report("report_id", keyboard.layout.report_id == 1 && keysOK
				&& mediaLength == 0,
			"id %u, Ctrl+c %s, media report %s", keyboard.layout.report_id,
			keysOK ? "read" : "misread",
			mediaLength == 0 ? "ignored" : "taken as keys");
	}

	{
		Parsed keyboard(kBitmapKeyboard, sizeof(kBitmapKeyboard));
		uint8 data[14] = {};
		data[0] = 0x04;					// Left Alt
		data[1 + 0x13 / 8] |= 1 << (0x13 % 8);	// p
		data[1 + 0x28 / 8] |= 1 << (0x28 % 8);	// Return
		size_t length = usb_kdl_keyboard_normalize(&keyboard.layout, data,
			sizeof(data), boot, sizeof(boot));
		const uint8 keys[] = { 0x13, 0x28 };
		report("bitmap", keyboard.layout.bitmap == 1
				&& keyboard.layout.bitmap_bits == 104
				&& keyboard.layout.key_offset == 1
				&& keys_are(boot, length, 0x04, keys, 2),
			"bitmap %u, %u bits @%d from usage %u, Alt+p+Return %s",
			keyboard.layout.bitmap, keyboard.layout.bitmap_bits,
			keyboard.layout.key_offset, keyboard.layout.bitmap_first_usage,
			keys_are(boot, length, 0x04, keys, 2) ? "read" : "misread");
	}

	{
		Parsed mouse(kMouseKeyboard, sizeof(kMouseKeyboard));
		Parsed keyboard(kBootKeyboard, sizeof(kBootKeyboard));
		Parsed bitmap(kBitmapKeyboard, sizeof(kBitmapKeyboard));
		report("choice", mouse.layout.score > 0
				&& keyboard.layout.score > mouse.layout.score
				&& bitmap.layout.score > mouse.layout.score,
			"mouse keyboard interface %d, boot keyboard %d, bitmap keyboard"
			" %d", (int)mouse.layout.score, (int)keyboard.layout.score,
			(int)bitmap.layout.score);
	}

	{
		Parsed media(kMediaKeys, sizeof(kMediaKeys));
		report("no_keys", media.status == B_OK && media.layout.score == 0,
			"score %d", (int)media.layout.score);
	}

	return sFailures == 0 ? 0 : 1;
}
