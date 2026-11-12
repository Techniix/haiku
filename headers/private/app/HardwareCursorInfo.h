/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 */
#ifndef _HARDWARE_CURSOR_INFO_H
#define _HARDWARE_CURSOR_INFO_H


/*!	What app_server does with the hardware cursor (Renga, roadmap F-59),
	for tests and diagnostics: AS_GET_HARDWARE_CURSOR_INFO.

	AS_TEST_HARDWARE_CURSOR replaces the accelerant's cursor hooks with a
	stand-in that behaves like a hardware cursor (it keeps what it is given
	and draws nothing; app_server stops drawing the software cursor). It
	lets the harness test the hardware cursor paths in QEMU, whose display
	drivers have none. app_server accepts it only in harness images (the
	directory /boot/system/non-packaged/data/ce-harness exists); it is
	never turned on by itself.

	Request AS_TEST_HARDWARE_CURSOR:
		int32	enable (0 or 1)
		uint16	largest width and height the stand-in accepts
		bool	whether it exports B_GET_CURSOR_MAX_SIZE
	Reply: status only.

	AS_GET_HARDWARE_CURSOR_DRAG and AS_SET_HARDWARE_CURSOR_DRAG (bool) read
	and change the setting the Input preferences show as "Show dragged
	items with the hardware pointer" (default on; off: the software cursor
	is used while something is dragged, as before F-59). app_server keeps
	it in ~/config/settings/system/app_server/hardware_cursor.

	Request AS_GET_HARDWARE_CURSOR_INFO:
		int32	x, y of a pixel of the hardware cursor image to return
	Reply: status, then a hardware_cursor_info.
*/


#include <GraphicsDefs.h>
#include <SupportDefs.h>


struct hardware_cursor_info {
	bool		hardware;			// the accelerant shows the cursor
	bool		test_hooks;			// the stand-in is in use
	bool		drag_bitmap;		// a drag bitmap is set
	bool		drag_in_hardware;	// ...and is part of the hardware image
	bool		visible;
	uint16		width;				// of the last image the accelerant
	uint16		height;				// accepted
	uint16		hot_x;
	uint16		hot_y;
	uint32		color_space;
	uint32		sample;				// B_RGBA32 pixel (x, y) of that image,
									// as B G R A bytes in memory order
	int32		set_calls;			// set_cursor_bitmap() calls (stand-in)
	int32		refused;			// ...that it refused
	int32		size_queries;		// B_GET_CURSOR_MAX_SIZE calls (stand-in)
	int32		software_fallbacks;	// drag images left to the software
									// cursor because the hardware refused
									// or reported a smaller limit
	uint16		position_x;			// last move_cursor() (stand-in)
	uint16		position_y;
	bool		drag_compositing;	// the setting (AS_SET_HARDWARE_CURSOR_DRAG)
};


#endif	// _HARDWARE_CURSOR_INFO_H
