/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 */
#ifndef _SHORTCUT_KEY_STYLE_H
#define _SHORTCUT_KEY_STYLE_H


#include <InterfaceDefs.h>


/*!	Which physical key is the shortcut ("Command") key.

	BeOS and Haiku put B_COMMAND_KEY on the Alt keys and B_CONTROL_KEY on the
	Ctrl keys, so Alt+C copies and Ctrl+C sends a control character. Keymap
	preferences has always been able to swap the two ("Windows/Linux mode"):
	then Ctrl+C copies, Alt+Tab switches windows (the switcher is
	Control+Tab) and Alt+letter sends the control character.

	Renga uses the Ctrl style by default; official Haiku builds
	(HAIKU_DISTRO_COMPATIBILITY=official) keep Alt. The style is part of the
	keymap (~/config/settings/Key_map): it is the assignment of the left and
	right command and control keys, nothing else. These helpers recognise
	and set it the way Keymap preferences always swapped it, so maps made by
	either stay compatible:

		Alt (Haiku)   left_command_key 0x5d (left Alt)
		              left_control_key 0x5c (left Ctrl)
		Ctrl          left_command_key 0x5c, left_control_key 0x5d

	The right-hand keys are swapped along with the left ones. Keymaps with
	other assignments (the Mac keymaps put Command on 0x66) are "other" and
	left alone.

	Used by input_server (its built-in default keymap), Keymap preferences,
	FirstBootPrompt and Terminal.
*/


namespace BPrivate {


enum shortcut_key_style {
	SHORTCUT_KEYS_ALT,		// Haiku and BeOS: Alt is the shortcut key
	SHORTCUT_KEYS_CTRL,		// Windows and Linux: Ctrl is the shortcut key
	SHORTCUT_KEYS_OTHER		// neither of the two standard assignments
};


#ifdef HAIKU_DISTRO_COMPATIBILITY_OFFICIAL
static const shortcut_key_style kDefaultShortcutKeyStyle = SHORTCUT_KEYS_ALT;
#else
static const shortcut_key_style kDefaultShortcutKeyStyle = SHORTCUT_KEYS_CTRL;
#endif


static const uint32 kShortcutStyleLeftCtrlKey = 0x5c;
static const uint32 kShortcutStyleLeftAltKey = 0x5d;


static inline shortcut_key_style
get_shortcut_key_style(const key_map& map)
{
	if (map.left_command_key == kShortcutStyleLeftAltKey
		&& map.left_control_key == kShortcutStyleLeftCtrlKey) {
		return SHORTCUT_KEYS_ALT;
	}
	if (map.left_command_key == kShortcutStyleLeftCtrlKey
		&& map.left_control_key == kShortcutStyleLeftAltKey) {
		return SHORTCUT_KEYS_CTRL;
	}
	return SHORTCUT_KEYS_OTHER;
}


/*!	Gives \a map the shortcut key \a style by swapping its command and
	control keys, left and right, like Keymap preferences' switch did.
	Returns whether the map changed. A map in neither standard style, or a
	request for SHORTCUT_KEYS_OTHER, changes nothing.
*/
static inline bool
set_shortcut_key_style(key_map& map, shortcut_key_style style)
{
	shortcut_key_style current = get_shortcut_key_style(map);
	if (style == SHORTCUT_KEYS_OTHER || current == SHORTCUT_KEYS_OTHER
		|| current == style) {
		return false;
	}

	uint32 key = map.left_command_key;
	map.left_command_key = map.left_control_key;
	map.left_control_key = key;

	key = map.right_command_key;
	map.right_command_key = map.right_control_key;
	map.right_control_key = key;
	return true;
}


}	// namespace BPrivate


#endif	// _SHORTCUT_KEY_STYLE_H
