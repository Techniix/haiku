/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 */
#ifndef _FRONT_COPY_INFO_H
#define _FRONT_COPY_INFO_H


#include <SupportDefs.h>


/*!	Renga (batch 7): how app_server copies its back buffer to the screen
	with the default display drivers (vesa, the UEFI frame buffer, and
	every other driver while app_server double-buffers).

	  B_FRONT_COPY_PLAIN    memcpy() of every changed rectangle (upstream)
	  B_FRONT_COPY_STREAM   SIMD non-temporal stores where available
	                        (SSE2 on x86), memcpy() elsewhere: write-
	                        combined video memory takes them in full lines
	  B_FRONT_COPY_CHANGED  the default: as STREAM, but only the pixels that
	                        differ from what is on the screen are written
	                        (compared against a copy in main memory, in
	                        blocks of 64 pixels)

	~/config/settings/Renga/framebuffer, "copy plain|stream|changed",
	read by app_server at start; AS_SET_FRONT_COPY_MODE changes it at once
	(the Control Panel's Displays page sends both). AS_GET_FRONT_COPY_STATS
	replies with a front_copy_stats (and resets it if the request's bool
	says so), for the tests and measurements.
*/


enum {
	B_FRONT_COPY_PLAIN = 0,
	B_FRONT_COPY_STREAM = 1,
	B_FRONT_COPY_CHANGED = 2
};


struct front_copy_stats {
	int32		mode;
	int32		simd;			// 1 if the SIMD path is compiled in
	uint64		rects;			// rectangles copied
	uint64		bytes_requested;
	uint64		bytes_written;	// to the frame buffer
	bigtime_t	copy_time;		// microseconds in the copies
	uint32		shadow_size;	// bytes of the main-memory copy, 0 if none
	uint32		reserved[3];
};


#endif	// _FRONT_COPY_INFO_H
