/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 */


#include "Recorder.h"

#include <stdlib.h>
#include <unistd.h>

#include <Message.h>
#include <Path.h>

#include <WindowInfo.h>
#include <WindowPrivate.h>


Recorder::Recorder(BMessenger target)
	:
	fTarget(target),
	fThread(-1),
	fStopSem(-1)
{
}


Recorder::~Recorder()
{
	Stop();
	if (fThread >= 0) {
		status_t result;
		wait_for_thread(fThread, &result);
	}
}


status_t
Recorder::Start(const BPrivate::CaptureOptions& options,
	const entry_ref& file)
{
	if (fThread >= 0)
		return B_BUSY;
	fOptions = options;
	fFile = file;
	fStopSem = create_sem(0, "recorder stop");
	if (fStopSem < 0)
		return fStopSem;
	fThread = spawn_thread(_ThreadEntry, "screen recorder", B_NORMAL_PRIORITY,
		this);
	if (fThread < 0) {
		delete_sem(fStopSem);
		fStopSem = -1;
		return fThread;
	}
	resume_thread(fThread);
	return B_OK;
}


void
Recorder::Stop()
{
	if (fStopSem >= 0)
		release_sem(fStopSem);
}


bool
Recorder::IsRecording() const
{
	return fSession.IsRunning();
}


bigtime_t
Recorder::Duration() const
{
	return fSession.Duration();
}


status_t
Recorder::SessionError() const
{
	return fSession.Error();
}


BRect
Recorder::Frame() const
{
	return fSession.Frame();
}


/*static*/ status_t
Recorder::_ThreadEntry(void* data)
{
	((Recorder*)data)->_Thread();
	return B_OK;
}


void
Recorder::_Thread()
{
	// Asks the user the first time (the Screen recording prompt), so this
	// may take a while.
	status_t status = fSession.Start(fOptions, &fFile);
	BMessage started(kRecorderStarted);
	started.AddInt32("status", status);
	fTarget.SendMessage(&started);

	if (status == B_OK) {
		// Until Stop(), or until the session ends by itself (the user
		// stopped it from the indicator, the window went away).
		while (acquire_sem_etc(fStopSem, 1, B_RELATIVE_TIMEOUT, 100000)
				== B_TIMED_OUT) {
			if (fSession.Error() != B_OK)
				break;
		}
		status = fSession.Stop();
		if (status == B_OK && fSession.Error() == B_CANCELED)
			status = B_OK;
	}

	BPath path(&fFile);
	BMessage stopped(kRecorderStopped);
	stopped.AddInt32("status", status);
	stopped.AddString("path", path.Path());
	stopped.AddInt64("frames", fSession.FramesWritten());
	stopped.AddInt64("audio frames", fSession.AudioFramesWritten());
	stopped.AddInt64("duration", fSession.Duration());

	delete_sem(fStopSem);
	fStopSem = -1;
	fThread = -1;
	fTarget.SendMessage(&stopped);
}


/*static*/ int32
Recorder::FindWindowToken(BRect frame)
{
	int32* tokens = NULL;
	int32 count = 0;
	if (BPrivate::get_window_order(current_workspace(), &tokens, &count)
			!= B_OK || tokens == NULL) {
		return -1;
	}
	team_id self = getpid();
	int32 found = -1;
	int32 first = -1;
	for (int32 i = 0; i < count && found < 0; i++) {
		client_window_info* info = get_window_info(tokens[i]);
		if (info == NULL)
			continue;
		// As the screenshot command finds the active window: no menus, no
		// minimized or hidden windows; and not Screenshot's own.
		if (info->team != self && !info->is_mini
			&& info->show_hide_level <= 0 && info->feel != kMenuWindowFeel) {
			BRect windowFrame(info->window_left, info->window_top,
				info->window_right, info->window_bottom);
			if (first < 0)
				first = info->server_token;
			if (windowFrame == frame)
				found = info->server_token;
		}
		free(info);
	}
	free(tokens);
	return found >= 0 ? found : first;
}
