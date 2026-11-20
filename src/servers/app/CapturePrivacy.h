/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 */
#ifndef CAPTURE_PRIVACY_H
#define CAPTURE_PRIVACY_H


#include <SupportDefs.h>


class Window;


/*!	Hide from capture (Renga, batch 5 streaming; roadmap F-31 "Streaming
	features"): windows that stay visible on screen but are blank in every
	screen read, that is every capture, recording and screenshot
	(ScreenReader::Read() fills their visible area).

	A window is hidden when the master switch is on (the default) and
	- its application asked for it (BScreenCapture::SetWindowHidden(),
	  for password managers and the like), or
	- the user marked its application ("app <signature>") or this window
	  ("window <signature> <title>") in the Screen recording preferences.
	The rules are in ~/config/settings/Renga/capture_privacy, re-read when
	the file changes (checked at most once a second).
*/
namespace CapturePrivacy {


bool			IsHidden(Window* window);
void			SetWindowHidden(int32 serverToken, bool hidden);
bool			Enabled();


}	// namespace CapturePrivacy


#endif	// CAPTURE_PRIVACY_H
