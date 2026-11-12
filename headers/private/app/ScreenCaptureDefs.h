/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 */
#ifndef _SCREEN_CAPTURE_DEFS_H
#define _SCREEN_CAPTURE_DEFS_H


/*!	Definitions shared by app_server's screen capture (ScreenCaptureManager)
	and its clients in libbe (BPrivate::BScreenCapture) and the Screen
	recording preferences. Renga-only; see 3rdparty/ce/PORTING.md, "Screen
	capture".
*/


namespace BPrivate {


// What a capture session records.
enum screen_capture_source {
	SCREEN_CAPTURE_SCREEN	= 0,	// the whole screen
	SCREEN_CAPTURE_WINDOW	= 1,	// one window, by its server token
	SCREEN_CAPTURE_REGION	= 2		// a rectangle of the screen
};

// Session flags.
enum {
	SCREEN_CAPTURE_INCLUDE_CURSOR	= 0x01,
	SCREEN_CAPTURE_WINDOW_BORDER	= 0x02	// window: with tab and border
};

// Masks of the sources, for BScreenCapture::RequestPermission() (F-58).
enum {
	SCREEN_CAPTURE_SCREEN_MASK	= 1 << SCREEN_CAPTURE_SCREEN,
	SCREEN_CAPTURE_WINDOW_MASK	= 1 << SCREEN_CAPTURE_WINDOW,
	SCREEN_CAPTURE_REGION_MASK	= 1 << SCREEN_CAPTURE_REGION,
	SCREEN_CAPTURE_ANY_MASK		= 0x07
};

// The answer to BScreenCapture::RequestPermission() (F-58, OBS Studio's
// O-7), sent to the messenger given there:
//	"status"	int32	B_OK: allowed; B_NOT_ALLOWED: the user said no (or
//						remembered no); B_CANCELED: the prompt was closed
//						without an answer
//	"request"	int32	the prompt's request id, -1 if no prompt was needed
//	"sources"	uint32	the sources asked for
enum {
	SCREEN_CAPTURE_PERMISSION_ANSWER	= 'ScPa'
};

// What NextFrame() says about a window source (F-58, OBS Studio's O-9).
// Without a compositor a window's area of the screen is captured, so a
// recorder pauses on HIDDEN and OTHER_WORKSPACE (the area shows something
// else) and may on FULLY_COVERED. Region and screen sources report 0.
enum {
	SCREEN_CAPTURE_FRAME_HIDDEN				= 0x01,	// hidden or minimized
	SCREEN_CAPTURE_FRAME_OTHER_WORKSPACE	= 0x02,	// not on the shown one
	SCREEN_CAPTURE_FRAME_COVERED			= 0x04,	// partly covered
	SCREEN_CAPTURE_FRAME_FULLY_COVERED		= 0x08,	// nothing of it shows
	SCREEN_CAPTURE_FRAME_NOT_VISIBLE		= SCREEN_CAPTURE_FRAME_HIDDEN
		| SCREEN_CAPTURE_FRAME_OTHER_WORKSPACE
		| SCREEN_CAPTURE_FRAME_FULLY_COVERED
};

// What the user decided for an application (by signature).
enum screen_capture_permission {
	SCREEN_CAPTURE_PERMISSION_UNKNOWN	= 0,	// ask (the default)
	SCREEN_CAPTURE_PERMISSION_ALLOWED	= 1,
	SCREEN_CAPTURE_PERMISSION_DENIED	= 2
};


// The system application that asks the user, shows the indicator and
// holds the settings. app_server starts it with "--prompt <request>" and
// "--indicator"; only teams running system software (an executable under
// /boot/system, not /boot/system/non-packaged) may answer requests or
// change permissions.
#define SCREEN_CAPTURE_AGENT_PATH	"/boot/system/preferences/ScreenRecording"
#define SCREEN_CAPTURE_AGENT_SIGNATURE	"application/x-vnd.Renga-ScreenRecording"


}	// namespace BPrivate


#endif	// _SCREEN_CAPTURE_DEFS_H
