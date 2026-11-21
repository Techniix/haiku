/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 */
#ifndef HIDRAW_POLICY_H
#define HIDRAW_POLICY_H


/*!	Which HID interfaces get a hidraw node (roadmap F-42), shared by usb_hid
	and the harness's userland test (kdl_keyboard_test).

	Batch 11 (maintainer's report: an 8BitDo Retro Mechanical Keyboard on
	its 2.4 GHz dongle stopped typing after the gamepad work; 8BitDo uses
	vendor 0x2DC8 for keyboards and pads alike): game controllers by their
	usage (Joystick, Game Pad, Multi-axis Controller); interfaces that
	describe a keyboard, keypad, mouse, pointer, system control, consumer
	control or digitizer never; the vendor list only for interfaces whose
	application collections are all vendor-defined (pads in a vendor mode
	before their handshake), never for a vendor's keyboard dongle.
*/


#include "HIDCollection.h"
#include "HIDParser.h"

#include <usb/USB_hid.h>
#include <usb/USB_hid_page_generic_desktop.h>


static inline bool
hidraw_policy_allows(HIDParser& parser, uint16 vendor)
{
	HIDCollection* root = parser.RootCollection();
	if (root == NULL)
		return false;

	bool gameController = false;
	bool onlyVendor = true;
	uint32 count = root->CountChildrenFlat(COLLECTION_APPLICATION);
	if (count == 0)
		return false;
	for (uint32 i = 0; i < count; i++) {
		HIDCollection* collection
			= root->ChildAtFlat(COLLECTION_APPLICATION, i);
		if (collection == NULL)
			continue;
		uint16 page = collection->UsagePage();
		uint16 usage = collection->UsageID();
		if (page < 0xff00)
			onlyVendor = false;
		if (page == B_HID_USAGE_PAGE_GENERIC_DESKTOP) {
			switch (usage) {
				case B_HID_UID_GD_JOYSTICK:
				case B_HID_UID_GD_GAMEPAD:
				case B_HID_UID_GD_MULTIAXIS:
					gameController = true;
					break;
				case B_HID_UID_GD_KEYBOARD:
				case B_HID_UID_GD_KEYPAD:
				case B_HID_UID_GD_MOUSE:
				case B_HID_UID_GD_POINTER:
				case 0x80:	// System Control (power, sleep)
					// never: raw reports would show typing and pointing,
					// and a keyboard's interface stays the keyboard's
					return false;
			}
		} else if (page == B_HID_USAGE_PAGE_DIGITIZER
			|| page == B_HID_USAGE_PAGE_CONSUMER) {
			return false;
		}
	}

	if (gameController)
		return true;
	if (!onlyVendor)
		return false;

	// Pads that only describe vendor collections (some Nintendo and Sony
	// modes, 8BitDo in Switch mode before its handshake): by vendor.
	switch (vendor) {
		case 0x054c:	// Sony
		case 0x057e:	// Nintendo
		case 0x0f0d:	// Hori
		case 0x20d6:	// PowerA
		case 0x28de:	// Valve
		case 0x2dc8:	// 8BitDo
			return true;
	}
	return false;
}


#endif	// HIDRAW_POLICY_H
