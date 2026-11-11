/*
 * Copyright 2003-2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT license.
 *
 * Authors:
 *		Jérôme Duval
 *		François Revol
 *		Axel Dörfler, axeld@pinc-software.de.
 *		John Scipione, jscipione@gmail.com
 */


#include "VolumeWindow.h"

#include <Box.h>
#include <Catalog.h>
#include <GroupLayout.h>
#include <SeparatorView.h>
#include <MenuItem.h>
#include <MessageRunner.h>
#include <Screen.h>
#include <SpaceLayoutItem.h>

#include <Button.h>
#include <Roster.h>

#include "ApplicationVolumesView.h"
#include "OutputMenuField.h"
#include "VolumeControl.h"


static const uint32 kMsgOpenAudioSettings = 'opAS';


#undef B_TRANSLATION_CONTEXT
#define B_TRANSLATION_CONTEXT "VolumeWindow"


VolumeWindow::VolumeWindow(BRect frame)
	:
	BWindow(frame, "VolumeWindow", B_BORDERED_WINDOW_LOOK, B_FLOATING_ALL_WINDOW_FEEL,
		B_ASYNCHRONOUS_CONTROLS | B_WILL_ACCEPT_FIRST_CLICK | B_AUTO_UPDATE_SIZE_LIMITS, 0),
	fUpdatedCount(0)
{
	SetLayout(new BGroupLayout(B_HORIZONTAL));

	BGroupLayout* layout = new BGroupLayout(B_VERTICAL, 5);
	layout->SetInsets(5, 5, 5, 5);

	BBox* box = new BBox("sliderbox");
	box->SetLayout(layout);
	box->SetBorder(B_PLAIN_BORDER);
	AddChild(box);

	fVolumeSlider = new VolumeControl();
	fVolumeSlider->SetModificationMessage(new BMessage(kMsgVolumeUpdate));
	box->AddChild(fVolumeSlider);

	fVolumeSlider->SetTarget(this);

	// Renga (F-31/F-32): where sound plays, the same menu as the Audio
	// preferences' Output page; the applications that play, each with its
	// volume, mute and output device, below the master volume; and the way
	// to the Audio preferences.
	fOutput = new OutputMenuField("output", B_TRANSLATE("Output:"));
	box->AddChild(fOutput);
	fApplications = new ApplicationVolumesView("applications", true);
	fSeparator = new BSeparatorView(B_TRANSLATE("Applications"));
	box->AddChild(fSeparator);
	box->AddChild(fApplications);
	BButton* settings = new BButton("audioSettings",
		B_TRANSLATE("Audio settings" B_UTF8_ELLIPSIS),
		new BMessage(kMsgOpenAudioSettings));
	settings->SetTarget(this);
	BGroupView* buttons = new BGroupView(B_HORIZONTAL);
	buttons->GroupLayout()->AddItem(BSpaceLayoutItem::CreateGlue());
	buttons->AddChild(settings);
	box->AddChild(buttons);
	if (fApplications->CountStreams() == 0) {
		fSeparator->Hide();
		fApplications->Hide();
	}
	ResizeToPreferred();

	// Make sure it's not outside the screen.
	const int32 kMargin = 3;
	BRect windowRect = Frame();
	BRect screenFrame(BScreen(B_MAIN_SCREEN_ID).Frame());
	if (screenFrame.right < windowRect.right + kMargin)
		MoveBy(- kMargin - windowRect.right + screenFrame.right, 0);
	if (screenFrame.bottom < windowRect.bottom + kMargin)
		MoveBy(0, - kMargin - windowRect.bottom + screenFrame.bottom);
	if (screenFrame.left > windowRect.left - kMargin)
		MoveBy(kMargin + screenFrame.left - windowRect.left, 0);
	if (screenFrame.top > windowRect.top - kMargin)
		MoveBy(0, kMargin + screenFrame.top - windowRect.top);
}


VolumeWindow::~VolumeWindow()
{
}


void
VolumeWindow::WindowActivated(bool active)
{
	if (!active && fApplications->CountStreams() > 0)
		PostMessage(B_QUIT_REQUESTED);
	BWindow::WindowActivated(active);
}


void
VolumeWindow::MessageReceived(BMessage* message)
{
	switch (message->what) {
		case kMsgVolumeUpdate:
			fUpdatedCount++;
			break;

		case kMsgVolumeChanged:
			// With applications listed the window stays open for them; it
			// closes when it is deactivated (a click elsewhere).
			if (fApplications->CountStreams() > 0)
				break;
			if (fUpdatedCount < 2) {
				// If the slider was set by click only, wait a bit until
				// closing the window to give some feedback that how volume
				// was changed, and that it is.
				BMessage quit(B_QUIT_REQUESTED);
				BMessageRunner::StartSending(this, &quit, 150000, 1);
			} else
				Quit();
			break;

		case kMsgOpenAudioSettings:
			be_roster->Launch("application/x-vnd.Renga-Audio");
			Quit();
			break;

		case B_QUIT_REQUESTED:
			Quit();
			break;

		default:
			BWindow::MessageReceived(message);
			break;
	}
}

