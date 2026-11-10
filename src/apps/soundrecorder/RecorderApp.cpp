/*
 * Copyright 2005, Jérôme Duval. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * Inspired by SoundCapture from Be newsletter (Media Kit Basics: Consumers and Producers)
 */

#include "RecorderApp.h"

#include <string.h>
#include "RecorderWindow.h"


RecorderApp::RecorderApp(const char* signature)
	: BApplication(signature), fRecorderWin(NULL)
{
	fRecorderWin = new RecorderWindow();
}


RecorderApp::~RecorderApp()
{
}


status_t
RecorderApp::InitCheck()
{
	if (fRecorderWin)
		return fRecorderWin->InitCheck();
	return B_OK;
}


int
main(int argc, char** argv)
{
	RecorderApp app("application/x-vnd.Haiku-SoundRecorder");
	if (app.InitCheck() != B_OK)
		return 0;

	// Renga: "--input <label>" selects that input (e.g. "What you hear:
	// MediaPlayer") at start, for automation and support instructions.
	for (int i = 1; i + 1 < argc; i++) {
		if (strcmp(argv[i], "--input") == 0) {
			BMessage select(RecorderWindow::SELECT_INPUT);
			select.AddString("label", argv[i + 1]);
			app.Window()->PostMessage(&select);
		}
	}
	app.Run();
	return 0;
}
