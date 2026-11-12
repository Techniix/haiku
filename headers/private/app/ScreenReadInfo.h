/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 */
#ifndef _SCREEN_READ_INFO_H
#define _SCREEN_READ_INFO_H


/*!	What reading the screen costs (Renga, roadmap F-59), for tests and
	measurements: AS_TEST_SCREEN_READ, accepted by app_server only in
	harness images (the directory /boot/system/non-packaged/data/ce-harness
	exists).

	Request:
		int32	legacy reads: 1 for the old front-buffer read of
				BScreen::ReadBitmap() (DrawingEngine::ReadBitmap() under the
				drawing engine's exclusive lock), 0 for the new one
				(src/servers/app/ScreenReader.h), -1 to leave it
		bool	reset the totals after replying
	Reply: status, then a screen_read_stats.

	Reads are BScreen::ReadBitmap() calls and BScreenCapture frames.
	Pointer moves are HWInterface::MoveCursorTo() calls; their wait is the
	time spent getting the drawing engine's lock, which a read holding the
	exclusive lock makes the pointer wait for.
*/


#include <SupportDefs.h>


struct screen_read_stats {
	int32		reads;
	bigtime_t	read_time_total;	// the whole read, locks included
	bigtime_t	read_time_max;
	bigtime_t	lock_held_total;	// with the drawing engine locked
	bigtime_t	lock_held_max;
	int64		back_pixels;		// copied from the back buffer
	int64		front_pixels;		// ...the front buffer
	int64		snapshot_pixels;	// ...saved update regions
	int32		cursor_moves;
	bigtime_t	cursor_wait_total;
	bigtime_t	cursor_wait_max;
	int32		update_snapshots;	// saved right now
	bool		legacy;
};


#endif	// _SCREEN_READ_INFO_H
