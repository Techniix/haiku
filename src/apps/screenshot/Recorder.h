/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 */
#ifndef SCREENSHOT_RECORDER_H
#define SCREENSHOT_RECORDER_H


#include <Entry.h>
#include <Messenger.h>
#include <Rect.h>
#include <String.h>

#include <CaptureSession.h>


// Messages the recorder sends to its target.
enum {
	kRecorderStarted	= 'rcst',	// "status" (int32)
	kRecorderStopped	= 'rcsp'	// "status", "path", "frames", "audio
									// frames" (int64), "duration" (int64)
};


/*!	Screen recording for Screenshot (Renga, F-44): one CaptureSession
	writing a file. Start() and Stop() return at once; the work (asking for
	permission, opening the encoders, writing the end of the file) runs in
	a thread of its own and reports to the target.
*/
class Recorder {
public:
								Recorder(BMessenger target);
								~Recorder();

			status_t			Start(const BPrivate::CaptureOptions& options,
									const entry_ref& file);
			void				Stop();

			bool				IsActive() const { return fThread >= 0; }
			bool				IsRecording() const;
			bigtime_t			Duration() const;
			status_t			SessionError() const;
			BRect				Frame() const;

	// The window to record for "Active window": the frontmost window of
	// another application whose frame is \a frame (or the frontmost one).
	static	int32				FindWindowToken(BRect frame);

private:
	static	status_t			_ThreadEntry(void* data);
			void				_Thread();

			BMessenger			fTarget;
			BPrivate::CaptureSession fSession;
			BPrivate::CaptureOptions fOptions;
			entry_ref			fFile;
			thread_id			fThread;
			sem_id				fStopSem;
};


#endif	// SCREENSHOT_RECORDER_H
