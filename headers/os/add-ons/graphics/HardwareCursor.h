/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 */
#ifndef _HARDWARE_CURSOR_H
#define _HARDWARE_CURSOR_H


/*!	Hardware cursor limits (Renga, roadmap F-59). Optional and add-only:
	an accelerant that does not export the hook keeps working exactly as
	before.

	app_server gives the accelerant's B_SET_CURSOR_BITMAP hook more than
	the pointer itself: while something is dragged, it hands over the
	pointer and the drag bitmap composited into one B_RGBA32 image (the
	pointer over the bitmap, with premultiplied alpha like every cursor
	bitmap app_server hands over: the system cursors are premultiplied by
	app_server's CursorManager), with the hot spot where the pointer's hot
	spot is. That image can be much larger than a
	pointer (a dragged icon is 32x32 to 64x64 plus the pointer, a dragged
	selection of text or an image can be several hundred pixels wide).

	Without this hook app_server simply calls set_cursor_bitmap() and uses
	the software cursor for as long as the accelerant returns an error.
	With it, app_server asks first and does not offer a bitmap larger than
	the limit:

		get_cursor_max_size getMax = (get_cursor_max_size)
			get_accelerant_hook(B_GET_CURSOR_MAX_SIZE, NULL);
		uint16 width, height;
		if (getMax != NULL && getMax(B_RGBA32, &width, &height) == B_OK)
			...

	The hook returns B_OK and the largest width and height that
	set_cursor_bitmap() accepts in that color space for the current
	display mode, or an error if the color space is not supported. An
	accelerant that reports a limit must accept every size up to it (most
	hardware has a few fixed sizes, such as 64x64, 128x128 and 256x256,
	and pads a smaller bitmap with transparent pixels). NVIDIA hardware
	since Kepler takes up to 256x256 ARGB.
*/


#include <GraphicsDefs.h>
#include <SupportDefs.h>


// get_accelerant_hook() feature code ('RGA' range, vendor neutral; see
// also B_GPU_PRESENT_HOOKS in <add-ons/graphics/GpuPresent.h>).
#define B_GET_CURSOR_MAX_SIZE	0x52474101


#ifdef __cplusplus
extern "C" {
#endif

typedef status_t (*get_cursor_max_size)(color_space space, uint16* maxWidth,
	uint16* maxHeight);

#ifdef __cplusplus
}
#endif


#endif	/* _HARDWARE_CURSOR_H */
