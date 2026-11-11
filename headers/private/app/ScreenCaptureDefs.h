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
