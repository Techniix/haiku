/*
 * Copyright 2004, Axel Dörfler, axeld@pinc-software.de. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef GENERIC_TEXT_CONSOLE_H
#define GENERIC_TEXT_CONSOLE_H


#include <boot/vfs.h>
#include <boot/stdio.h>

// Standard 16 color palette.
enum console_color {
	// foreground and background colors
	BLACK,
	BLUE,
	GREEN,
	CYAN,
	RED,
	PURPLE,
	BROWN,
	GRAY,
	// foreground colors only, if blinking is enabled (on bios_ia32)
	DARK_GRAY,
	BRIGHT_BLUE,
	BRIGHT_GREEN,
	BRIGHT_CYAN,
	BRIGHT_RED,
	MAGENTA,
	YELLOW,
	WHITE
};

enum {
	// ASCII
	TEXT_CONSOLE_NO_KEY				= '\0',
	TEXT_CONSOLE_KEY_RETURN			= '\r',
	TEXT_CONSOLE_KEY_BACKSPACE		= '\b',
	TEXT_CONSOLE_KEY_ESCAPE			= 0x1b,
	TEXT_CONSOLE_KEY_SPACE			= ' ',

	// cursor keys
	TEXT_CONSOLE_CURSOR_KEYS_START	= 0x40000000,
	TEXT_CONSOLE_KEY_UP				= TEXT_CONSOLE_CURSOR_KEYS_START,
	TEXT_CONSOLE_KEY_DOWN,
	TEXT_CONSOLE_KEY_LEFT,
	TEXT_CONSOLE_KEY_RIGHT,
	TEXT_CONSOLE_KEY_PAGE_UP,
	TEXT_CONSOLE_KEY_PAGE_DOWN,
	TEXT_CONSOLE_KEY_HOME,
	TEXT_CONSOLE_KEY_END,
	TEXT_CONSOLE_CURSOR_KEYS_END,
};

// Results of console_wait_for_key_timeout() that are not keys.
#define TEXT_CONSOLE_NO_KEY				(-1)
	// no key was pressed before the timeout
#define TEXT_CONSOLE_NO_TIMED_WAIT		(-2)
	// the platform cannot wait for a key with a timeout

#define TEXT_CONSOLE_IS_CURSOR_KEY(key) \
	(key >= TEXT_CONSOLE_CURSOR_KEYS_START \
	 && key < TEXT_CONSOLE_CURSOR_KEYS_END)

#ifdef __cplusplus
extern "C" {
#endif

extern void console_clear_screen(void);
extern int32 console_width(void);
extern int32 console_height(void);
extern void console_set_cursor(int32 x, int32 y);
extern void console_show_cursor(void);
extern void console_hide_cursor(void);
extern void console_set_color(int32 foreground, int32 background);

extern int console_wait_for_key(void);
extern int console_wait_for_key_timeout(bigtime_t timeout);
	// Like console_wait_for_key(), but gives up after about \a timeout
	// microseconds and returns TEXT_CONSOLE_NO_KEY then. Platforms that do
	// not implement it get a default that returns TEXT_CONSOLE_NO_TIMED_WAIT
	// at once (see generic/text_menu.cpp).

#ifdef __cplusplus
}
#endif

#endif	/* GENERIC_TEXT_CONSOLE_H */
