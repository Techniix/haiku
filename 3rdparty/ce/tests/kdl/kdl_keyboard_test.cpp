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
	  kdl_keyboard.multi_report  (batch 14) a dongle interface with three
	                          keyboard reports (8BitDo 2dc8:5201: ID 1 six
	                          keys, IDs 12 and 10 key bitmaps): keys sent in
	                          the bitmap reports are read (the old add-on
	                          took only the best-scored report, ID 1, and
	                          dropped them), held keys from two reports add
	                          up, media keys stay out
	  kdl_keyboard.keyboards  (batch 14) every keyboard is in the table,
	                          best first; a removed one leaves it
	Output format: see 3rdparty/ce/README.md.
*/


#include "HIDParser.h"
#include "HIDReport.h"
#include "HIDReportItem.h"
#include "KeyboardDebugLayout.h"
#include "HidrawPolicy.h"

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


// batch 11: a gamepad report (Game Pad: buttons and two axes)
static const uint8 kGamepad[] = {
	0x05, 0x01, 0x09, 0x05, 0xa1, 0x01,
	0x05, 0x09, 0x19, 0x01, 0x29, 0x08, 0x15, 0x00, 0x25, 0x01,
	0x75, 0x01, 0x95, 0x08, 0x81, 0x02,
	0x05, 0x01, 0x09, 0x30, 0x09, 0x31, 0x15, 0x81, 0x25, 0x7f,
	0x75, 0x08, 0x95, 0x02, 0x81, 0x02,
	0xc0
};

// A vendor-defined collection only (a pad before its handshake, or a
// keyboard dongle's configuration interface)
static const uint8 kVendorOnly[] = {
	0x06, 0x00, 0xff, 0x09, 0x01, 0xa1, 0x01,
	0x15, 0x00, 0x26, 0xff, 0x00, 0x75, 0x08, 0x95, 0x20,
	0x09, 0x01, 0x81, 0x02, 0x09, 0x01, 0x91, 0x02,
	0xc0
};

// A dongle's second interface: media keys next to a vendor collection
static const uint8 kMediaAndVendor[] = {
	0x05, 0x0c, 0x09, 0x01, 0xa1, 0x01, 0x85, 0x02,
	0x15, 0x00, 0x26, 0xff, 0x03, 0x19, 0x00, 0x2a, 0xff, 0x03,
	0x75, 0x10, 0x95, 0x01, 0x81, 0x00,
	0xc0,
	0x06, 0x00, 0xff, 0x09, 0x01, 0xa1, 0x01, 0x85, 0x05,
	0x15, 0x00, 0x26, 0xff, 0x00, 0x75, 0x08, 0x95, 0x10,
	0x09, 0x01, 0x81, 0x02,
	0xc0
};


// batch 14: the 8BitDo Retro keyboard dongle's keyboard interface
// (2dc8:5201 interface 2, as usbhid-dump shows it): report 1 a boot-like
// keyboard with LEDs, report 2 consumer keys, reports 12 and 10 "N-key
// rollover" keyboards (modifiers and a 120-bit bitmap each; here 12 has
// usages 0x00-0x77 and 10 usages 0x78-0xef).
static const uint8 kDongleKeyboard[] = {
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
	0xc0,
	0x05, 0x01, 0x09, 0x06, 0xa1, 0x01, 0x85, 0x0c,
	0x05, 0x07, 0x19, 0xe0, 0x29, 0xe7, 0x15, 0x00, 0x25, 0x01,
	0x75, 0x01, 0x95, 0x08, 0x81, 0x02,
	0x05, 0x07, 0x19, 0x00, 0x29, 0x77, 0x15, 0x00, 0x25, 0x01,
	0x75, 0x01, 0x95, 0x78, 0x81, 0x02,
	0xc0,
	0x05, 0x01, 0x09, 0x06, 0xa1, 0x01, 0x85, 0x0a,
	0x05, 0x07, 0x19, 0xe0, 0x29, 0xe7, 0x15, 0x00, 0x25, 0x01,
	0x75, 0x01, 0x95, 0x08, 0x81, 0x02,
	0x05, 0x07, 0x19, 0x78, 0x29, 0xef, 0x15, 0x00, 0x25, 0x01,
	0x75, 0x01, 0x95, 0x78, 0x81, 0x02,
	0xc0
};


/*!	What usb_hid puts into the debugger's table for one interface: a
	layout per input report with keyboard usages (a KeyboardProtocolHandler
	each), the report's LED output report if there is one.
*/
static void
add_interface(usb_kdl_keyboard_table& table, HIDParser& parser,
	uint64 pipe, usb_kdl_keyboard_layout* best = NULL)
{
	if (best != NULL)
		memset(best, 0, sizeof(*best));
	for (uint8 i = 0; i < parser.CountReports(HID_REPORT_TYPE_INPUT); i++) {
		HIDReport* report = parser.ReportAt(HID_REPORT_TYPE_INPUT, i);
		bool keyboard = false;
		for (uint32 j = 0; report != NULL && j < report->CountItems(); j++)
			keyboard |= report->ItemAt(j)->UsagePage()
				== B_HID_USAGE_PAGE_KEYBOARD;
		if (!keyboard)
			continue;
		usb_kdl_keyboard_layout layout;
		compute_keyboard_debug_layout(*report, parser.FindReport(
			HID_REPORT_TYPE_OUTPUT, report->ID()), layout);
		usb_kdl_keyboard_table_add(&table, pipe,
			parser.MaxReportSize(HID_REPORT_TYPE_INPUT), report, &layout);
		if (best != NULL && layout.score > best->score)
			*best = layout;
	}
}


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

	// batch 11: hidraw never takes an 8BitDo (0x2dc8) keyboard's
	// interfaces; pads by usage, vendor-only interfaces by vendor
	{
		Parsed keyboard(kBootKeyboard, sizeof(kBootKeyboard));
		Parsed withMedia(kReportIdKeyboard, sizeof(kReportIdKeyboard));
		Parsed media(kMediaAndVendor, sizeof(kMediaAndVendor));
		Parsed pad(kGamepad, sizeof(kGamepad));
		Parsed vendor(kVendorOnly, sizeof(kVendorOnly));
		bool keyboardRaw = hidraw_policy_allows(keyboard.parser, 0x2dc8);
		bool withMediaRaw = hidraw_policy_allows(withMedia.parser, 0x2dc8);
		bool mediaRaw = hidraw_policy_allows(media.parser, 0x2dc8);
		bool padRaw = hidraw_policy_allows(pad.parser, 0x1234);
		bool vendorRaw = hidraw_policy_allows(vendor.parser, 0x2dc8);
		bool otherVendorRaw = hidraw_policy_allows(vendor.parser, 0x1234);
		report("hidraw_8bitdo_keyboard", !keyboardRaw && !withMediaRaw
				&& !mediaRaw && padRaw && vendorRaw && !otherVendorRaw
				&& keyboard.layout.score > 0,
			"0x2dc8 boot keyboard %d, keyboard with media keys %d, media "
			"and vendor interface %d (all want 0); a game pad %d, a "
			"vendor-only 0x2dc8 interface %d (want 1), another vendor's "
			"%d (want 0); the keyboard stays usable (score %d)",
			keyboardRaw, withMediaRaw, mediaRaw, padRaw, vendorRaw,
			otherVendorRaw, (int)keyboard.layout.score);
	}

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

	// batch 14: all keyboard reports of an interface
	{
		HIDParser parser(NULL);
		status_t status = parser.ParseReportDescriptor(kDongleKeyboard,
			sizeof(kDongleKeyboard));
		usb_kdl_keyboard_table table;
		memset(&table, 0, sizeof(table));
		usb_kdl_keyboard_layout best;
		add_interface(table, parser, 7, &best);
		const usb_kdl_keyboard* keyboard
			= table.count > 0 ? &table.keyboards[0] : NULL;
		usb_kdl_keyboard_state state;
		memset(&state, 0, sizeof(state));

		// report 12: Left Shift + a; then report 10: usage 0x80 (Volume
		// Up on the keyboard page) while a is still held
		uint8 bitmapA[17] = { 0x0c, 0x02 };
		bitmapA[2 + 0x04 / 8] |= 1 << (0x04 % 8);
		uint8 bitmapHigh[17] = { 0x0a, 0x00 };
		bitmapHigh[2 + (0x80 - 0x78) / 8] |= 1 << ((0x80 - 0x78) % 8);
		const uint8 media[3] = { 0x02, 0xe9, 0x00 };

		size_t lengthA = keyboard != NULL ? usb_kdl_keyboard_merge(keyboard,
			&state, bitmapA, sizeof(bitmapA), boot, sizeof(boot)) : 0;
		const uint8 keysA[] = { 0x04 };
		bool readA = lengthA != 0 && keys_are(boot, lengthA, 0x02, keysA, 1);
		size_t lengthBoth = keyboard != NULL ? usb_kdl_keyboard_merge(
			keyboard, &state, bitmapHigh, sizeof(bitmapHigh), boot,
			sizeof(boot)) : 0;
		const uint8 keysBoth[] = { 0x04, 0x80 };
		bool readBoth = lengthBoth != 0
			&& keys_are(boot, lengthBoth, 0x02, keysBoth, 2);
		size_t lengthMedia = keyboard != NULL ? usb_kdl_keyboard_merge(
			keyboard, &state, media, sizeof(media), boot, sizeof(boot)) : 1;

		// the old add-on: the best-scored report only
		size_t oldLength = usb_kdl_keyboard_normalize(&best, bitmapA,
			sizeof(bitmapA), boot, sizeof(boot));

		report("multi_report", status == B_OK && table.count == 1
				&& keyboard->layout_count == 3 && readA && readBoth
				&& lengthMedia == 0,
			"%u keyboard(s), %u keyboard reports (want 1, 3); Shift+a in "
			"report 12 %s, a held + 0x80 in report 10 %s, media report %s; "
			"the old add-on (best report %u only) %s report 12",
			table.count, keyboard != NULL ? keyboard->layout_count : 0,
			readA ? "read" : "misread", readBoth ? "read" : "misread",
			lengthMedia == 0 ? "ignored" : "taken as keys", best.report_id,
			oldLength == 0 ? "dropped" : "read");
	}

	// batch 14: every keyboard, best first
	{
		Parsed mouse(kMouseKeyboard, sizeof(kMouseKeyboard));
		Parsed keyboard(kBootKeyboard, sizeof(kBootKeyboard));
		HIDParser dongle(NULL);
		dongle.ParseReportDescriptor(kDongleKeyboard,
			sizeof(kDongleKeyboard));
		usb_kdl_keyboard_table table;
		memset(&table, 0, sizeof(table));
		usb_kdl_keyboard_table_add(&table, 1, 8, &mouse, &mouse.layout);
		add_interface(table, dongle, 2);
		usb_kdl_keyboard_table_add(&table, 3, 8, &keyboard,
			&keyboard.layout);
		uint32 count = table.count;
		uint64 first = table.keyboards[0].pipe;
		uint64 last = count > 0 ? table.keyboards[count - 1].pipe : 0;
		usb_kdl_keyboard_table_remove(&table, &keyboard);
		uint32 afterRemove = table.count;
		bool keyboardGone = true;
		for (uint32 i = 0; i < table.count; i++)
			keyboardGone &= table.keyboards[i].pipe != 3;
		report("keyboards", count == 3 && first == 3 && last == 1
				&& afterRemove == 2 && keyboardGone,
			"%u keyboards (want 3: mouse, dongle, keyboard), best pipe %u "
			"(want 3, the boot keyboard), last %u (want 1, the mouse); "
			"after unplugging the keyboard %u left", count, (unsigned)first,
			(unsigned)last, afterRemove);
	}

	return sFailures == 0 ? 0 : 1;
}
