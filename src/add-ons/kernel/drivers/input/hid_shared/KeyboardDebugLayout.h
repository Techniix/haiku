/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 */
#ifndef KEYBOARD_DEBUG_LAYOUT_H
#define KEYBOARD_DEBUG_LAYOUT_H


#include <usb_kdl_keyboard.h>


class HIDReport;


/*!	Describes \a input (a keyboard's input report, \a output its LED report
	or NULL) for the kernel debugger, with a score of how much it looks like
	a full keyboard: keyboard key usages (not consumer or button pages),
	eight modifiers, at least six keys or a key bitmap, LEDs. 0 means the
	debugger cannot use it. Renga, batch 4.
*/
void	compute_keyboard_debug_layout(HIDReport& input, HIDReport* output,
			usb_kdl_keyboard_layout& layout);


#endif	// KEYBOARD_DEBUG_LAYOUT_H
