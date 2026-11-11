/*
 * Copyright 2010 Wim van der Meer <WPJvanderMeer@gmail.com>
 * All rights reserved. Distributed under the terms of the MIT license.
 *
 * Authors:
 *		Wim van der Meer
 */


#include "ScreenshotApp.h"

#include <stdio.h>

#include <stdlib.h>

#include <Bitmap.h>
#include <Catalog.h>
#include <Locale.h>
#include <Roster.h>
#include <Screen.h>
#include <WindowPrivate.h>

#include "ScreenshotWindow.h"
#include "SelectAreaView.h"
#include "Utility.h"


ScreenshotApp::ScreenshotApp()
	:
	BApplication("application/x-vnd.haiku-screenshot"),
	fScreenshotWindow(NULL),
	fUtility(new Utility),
	fSilent(false),
	fClipboard(false),
	fLaunchWithAreaSelect(false)
{
}


ScreenshotApp::~ScreenshotApp()
{
	delete fUtility;
}


void
ScreenshotApp::MessageReceived(BMessage* message)
{
	status_t status = B_OK;
	switch (message->what) {
		case SS_UTILITY_DATA:
		{
			BMessage bitmap;
			status = message->FindMessage("wholeScreen", &bitmap);
			if (status != B_OK)
				break;

			fUtility->wholeScreen = new BBitmap(&bitmap);

			status = message->FindMessage("cursorBitmap", &bitmap);
			if (status != B_OK)
				break;

			fUtility->cursorBitmap = new BBitmap(&bitmap);

			status = message->FindMessage("cursorAreaBitmap", &bitmap);
			if (status != B_OK)
				break;

			fUtility->cursorAreaBitmap = new BBitmap(&bitmap);

			status = message->FindPoint("cursorPosition",
				&fUtility->cursorPosition);
			if (status != B_OK)
				break;

			status = message->FindRect("activeWindowFrame",
				&fUtility->activeWindowFrame);
			if (status != B_OK)
				break;

			status = message->FindRect("tabFrame", &fUtility->tabFrame);
			if (status != B_OK)
				break;

			status = message->FindFloat("borderSize", &fUtility->borderSize);
			if (status != B_OK)
				break;

			status = message->FindBool("selectArea", &fLaunchWithAreaSelect);
			if (status != B_OK)
				break;

			break;
		}

		case SS_SELECT_AREA_FRAME:
		{
			BRect frame;
			status_t status = message->FindRect("selectAreaFrame", &frame);
			if (status != B_OK)
				break;

			if (!frame.IsValid() && fLaunchWithAreaSelect)
				be_app->PostMessage(B_QUIT_REQUESTED);
			else if (fScreenshotWindow == NULL) {
				fScreenshotWindow = new ScreenshotWindow(*fUtility, fSilent, fClipboard);
				fLaunchWithAreaSelect = false;
			}

			fScreenshotWindow->SetSelectedArea(frame);
			break;
		}

		case SS_LAUNCH_AREA_SELECTOR:
		{
			BWindow* selectAreaWindow = new BWindow(BScreen().Frame(),
				"Area Window", kWindowScreenWindow,
				B_ASYNCHRONOUS_CONTROLS|B_NOT_RESIZABLE|B_NOT_CLOSABLE|B_NOT_ZOOMABLE);

			selectAreaWindow->AddChild(new SelectAreaView(fUtility->wholeScreen));
			selectAreaWindow->Show();
			break;
		}

		default:
			BApplication::MessageReceived(message);
			break;
	}

	if (status != B_OK)
		be_app->PostMessage(B_QUIT_REQUESTED);
}


void
ScreenshotApp::ArgvReceived(int32 argc, char** argv)
{
	for (int32 i = 0; i < argc; i++) {
		if (strcmp(argv[i], "-s") == 0
			|| strcmp(argv[i], "--silent") == 0)
			fSilent = true;
		else if (strcmp(argv[i], "-c") == 0
			|| strcmp(argv[i], "--clipboard") == 0)
			fClipboard = true;
		// Renga: screen recording from the command line (F-44):
		//   Screenshot --record <seconds> [--region <x>,<y>,<w>,<h>]
		//     [--file <path>] [--system-audio] [--microphone]
		//     [--frame-rate <n>] [--format mkv|webm] [--quit]
		// records the screen (or the region) for that long into the file
		// (default: the Screenshot folder) and prints what it wrote.
		else if (strcmp(argv[i], "--record") == 0 && i + 1 < argc) {
			fRecordOptions.what = 'reco';
			fRecordOptions.SetFloat("seconds", atof(argv[++i]));
		} else if (strcmp(argv[i], "--region") == 0 && i + 1 < argc) {
			float x, y, w, h;
			if (sscanf(argv[++i], "%f,%f,%f,%f", &x, &y, &w, &h) == 4)
				fRecordOptions.SetRect("region", BRect(x, y, x + w - 1, y + h - 1));
		} else if (strcmp(argv[i], "--file") == 0 && i + 1 < argc)
			fRecordOptions.SetString("file", argv[++i]);
		else if (strcmp(argv[i], "--system-audio") == 0)
			fRecordOptions.SetBool("system audio", true);
		else if (strcmp(argv[i], "--microphone") == 0)
			fRecordOptions.SetBool("microphone", true);
		else if (strcmp(argv[i], "--frame-rate") == 0 && i + 1 < argc)
			fRecordOptions.SetFloat("frame rate", atof(argv[++i]));
		else if (strcmp(argv[i], "--format") == 0 && i + 1 < argc)
			fRecordOptions.SetString("format", argv[++i]);
		else if (strcmp(argv[i], "--quit") == 0)
			fRecordOptions.SetBool("quit", true);
	}
}


void
ScreenshotApp::ReadyToRun()
{
	// Started directly (not by the screenshot command, which sends the
	// screen first): take the screen here, for the preview.
	if (fUtility->wholeScreen == NULL)
		BScreen().GetBitmap(&fUtility->wholeScreen, false);

	if (fLaunchWithAreaSelect)
		be_app->PostMessage(new BMessage(SS_LAUNCH_AREA_SELECTOR));
	else
		fScreenshotWindow = new ScreenshotWindow(*fUtility, fSilent, fClipboard);

	if (fRecordOptions.what == 'reco' && fScreenshotWindow != NULL
		&& fScreenshotWindow->Lock()) {
		fScreenshotWindow->RecordFromCommandLine(fRecordOptions);
		fScreenshotWindow->Unlock();
	}
}


int
main()
{
	ScreenshotApp app;
	return app.Run();
}
