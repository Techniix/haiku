/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 */
#ifndef _SCREEN_CAPTURE_H
#define _SCREEN_CAPTURE_H


#include <OS.h>
#include <Rect.h>
#include <Region.h>
#include <String.h>

#include <ScreenCaptureDefs.h>


class BBitmap;
class BMessage;
class BMessenger;


namespace BPrivate {


/*!	Captures the screen, a window or a region through app_server (Renga,
	F-31 phase 3; see 3rdparty/ce/PORTING.md, "Screen capture").

	Start() asks app_server for a session. The first time an application
	captures, the user is asked (the Screen recording preferences show the
	prompt); Start() waits for the answer up to \a permissionTimeout and
	returns B_NOT_ALLOWED if the answer is no, B_TIMED_OUT if none came in
	time (the prompt stays up; a later Start() or RequestPermission() waits
	for the same one). Other threads of the application can use app_server
	meanwhile (F-58, O-6). While a session runs, an indicator with a Stop
	button is on screen.

	RequestPermission() asks without waiting (F-58, OBS Studio's O-7): it
	returns at once, and the answer comes to \a target as a
	SCREEN_CAPTURE_PERMISSION_ANSWER message ("status": B_OK allowed,
	B_NOT_ALLOWED no, B_CANCELED the prompt was closed without an answer;
	see ScreenCaptureDefs.h), right away if the answer is already known.
	\a sources is a mask of SCREEN_CAPTURE_*_MASK; the prompt names the
	first of screen, window, region in it. Permission is per application,
	so a Start...() after an allowed answer starts without asking.

	NextFrame() fills a B_RGB32 bitmap of at least Frame()'s size with the
	source, copying only what changed since that bitmap was last filled; it
	never blocks. *_changed is false if nothing changed since the previous
	frame, *_damage gets what did (frame coordinates), *_time the
	system_time() the frame was taken. Use two or more bitmaps for double
	buffering; each is kept up to date by itself. It returns B_CANCELED
	when the user stopped the session (indicator, preferences) and
	B_ENTRY_NOT_FOUND when the captured window is gone. A window that
	changes size changes Frame(); the bitmap is filled from its top left.

	Not thread safe: use one object from one thread at a time.
*/
class BScreenCapture {
public:
								BScreenCapture();
								~BScreenCapture();

			status_t			StartScreen(uint32 flags = 0,
									bigtime_t permissionTimeout
										= 120000000);
			status_t			StartWindow(int32 windowServerToken,
									uint32 flags = 0,
									bigtime_t permissionTimeout
										= 120000000);
			status_t			StartRegion(BRect region, uint32 flags = 0,
									bigtime_t permissionTimeout
										= 120000000);
			status_t			Stop();

			bool				IsRunning() const { return fSession >= 0; }
			int32				Session() const { return fSession; }
			BRect				Frame() const { return fFrame; }
				// screen coordinates of the source, as of the last frame

			status_t			NextFrame(BBitmap* bitmap, bool* _changed,
									BRegion* _damage = NULL,
									bigtime_t* _time = NULL);
	// F-58 (OBS Studio's O-9): the same, and for window sources whether
	// the window shows (SCREEN_CAPTURE_FRAME_* of ScreenCaptureDefs.h).
			status_t			NextFrame(BBitmap* bitmap, bool* _changed,
									BRegion* _damage, bigtime_t* _time,
									uint32* _flags);

	// F-58 (OBS Studio's O-8): \a semaphore (of this team) is released
	// once when the source changes after a frame was taken (and when the
	// session is stopped); -1 ends that. WaitForFrame() does the same with
	// a semaphore of its own: B_OK when there is something new for
	// NextFrame(), B_TIMED_OUT after \a timeout.
			status_t			SetFrameNotification(sem_id semaphore);
			status_t			WaitForFrame(bigtime_t timeout);

	// For the Screen recording preferences and the indicator; changing
	// permissions and stopping other applications' sessions needs system
	// software (see ScreenCaptureDefs.h).
	static	status_t			GetSessions(BMessage* sessions);
				// "id", "team", "signature", "name", "source" (int32),
				// "frame" (BRect), "started" (int64), "frames" (int64), one
				// entry per session
	static	status_t			StopSession(int32 id);
	static	status_t			GetPermissions(BMessage* permissions);
				// "signature", "name", "state" (int32), one per application
	static	status_t			SetPermission(const char* signature,
									int32 state);
	static	status_t			GetRequest(int32 request, BString& signature,
									BString& name, BString& path,
									int32& source);
	static	status_t			AnswerRequest(int32 request, bool allow,
									bool remember);
	// F-58 (O-7): returns at once; the answer is sent to \a target.
	static	status_t			RequestPermission(BMessenger target,
									uint32 sources = SCREEN_CAPTURE_ANY_MASK);

private:
			status_t			_Start(int32 source, int32 windowToken,
									BRect region, uint32 flags,
									bigtime_t permissionTimeout);

			int32				fSession;
			BRect				fFrame;
};


}	// namespace BPrivate


using BPrivate::BScreenCapture;


#endif	// _SCREEN_CAPTURE_H
