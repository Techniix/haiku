/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 */
#ifndef _UVC_MODES_H
#define _UVC_MODES_H


/*!	The modes a UVC camera offers (roadmap F-58, OBS Studio's O-5): frame
	sizes, frame intervals and encodings from its VS_FRAME descriptors, as
	the camera's "renga:video_modes" parameter lists them, and the frame
	interval chosen for a requested frame rate. Kept free of USB and Media
	Kit calls so that 3rdparty/ce/tests/uvc tests it with synthetic
	descriptors.
*/


#include <String.h>
#include <SupportDefs.h>


// Frame intervals are in 100 ns units, as UVC has them.
static const uint32 kUVCMaxIntervals = 32;


/*!	The frame intervals a VS_FRAME_UNCOMPRESSED or VS_FRAME_MJPEG descriptor
	of \a length bytes allows, in \a intervals (at most \a maxCount),
	shortest first. A discrete list as it is (entries past \a length are
	ignored); a continuous range as the common rates inside it (60, 50, 30,
	25, 24, 20, 15, 10, 7.5, 5 fps, each moved to the nearest allowed
	step), plus its ends. Returns the count; 0 for a malformed descriptor.
*/
int32 uvc_frame_intervals(const uint8* descriptor, size_t length,
	uint32* intervals, int32 maxCount);

/*!	The interval of \a intervals (\a count of them) closest to \a fps
	frames per second (by frame rate); the descriptor's default
	\a defaultInterval when \a fps is 0 or no interval is given.
*/
uint32 uvc_choose_interval(const uint32* intervals, int32 count,
	float fps, uint32 defaultInterval);

//!	Frames per second of a frame interval (100 ns units).
float uvc_interval_fps(uint32 interval);

/*!	One line of "renga:video_modes" (no newline):
	"<width>x<height> <encoding> <fps>,<fps>,...", fastest first, each rate
	with up to three decimals and no trailing zeros ("30,15,7.5").
*/
BString uvc_mode_line(uint32 width, uint32 height, const char* encoding,
	const uint32* intervals, int32 count);


#endif	// _UVC_MODES_H
