/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 */


#include "InstallModeWindow.h"

#include <Button.h>
#include <CheckBox.h>
#include <Catalog.h>
#include <LayoutBuilder.h>
#include <RadioButton.h>
#include <SeparatorView.h>
#include <StringView.h>

#include <distro_name.h>


#undef B_TRANSLATION_CONTEXT
#define B_TRANSLATION_CONTEXT "InstallModeWindow"


static const uint32 kMsgContinue = 'cont';
static const uint32 kMsgCancel = 'cncl';


static BStringView*
note(const char* text)
{
	BStringView* view = new BStringView(NULL, text);
	BFont font(be_plain_font);
	font.SetSize(font.Size() * 0.9);
	view->SetFont(&font);
	view->SetHighUIColor(B_PANEL_TEXT_COLOR, B_DARKEN_1_TINT);
	return view;
}


InstallModeWindow::InstallModeWindow(const char* volumeName,
	const InstallationInfo& existing, bool* renameVolume)
	:
	BWindow(BRect(0, 0, 100, 100), B_TRANSLATE("Installation mode"),
		B_MODAL_WINDOW, B_NOT_ZOOMABLE | B_NOT_RESIZABLE
			| B_AUTO_UPDATE_SIZE_LIMITS | B_CLOSE_ON_ESCAPE),
	fDone(create_sem(0, "install mode")),
	fResult(INSTALL_MODE_CANCEL),
	fRename(NULL),
	fRenameVolume(renameVolume)
{
	BString heading;
	if (existing.found) {
		heading.SetToFormat(B_TRANSLATE("\"%s\" already has %s installed."),
			volumeName, existing.Description().String());
	} else {
		heading.SetToFormat(B_TRANSLATE("\"%s\" is not empty."),
			volumeName);
	}
	BStringView* headingView = new BStringView("heading", heading);
	headingView->SetFont(be_bold_font);

	fUpgrade = new BRadioButton("upgrade", B_TRANSLATE(
		"Upgrade to " DISTRO_NAME ", keep my files and applications"), NULL);
	fReplace = new BRadioButton("replace", B_TRANSLATE(
		"Replace the system, keep my files"), NULL);
	fErase = new BRadioButton("erase", B_TRANSLATE(
		"Erase the volume and install"), NULL);

	BString eraseNote;
	eraseNote.SetToFormat(B_TRANSLATE("Everything on \"%s\" is deleted. You "
		"will be asked once more."), volumeName);

	// F-59: a volume named exactly "Haiku" would keep that name
	BString renameLabel;
	renameLabel.SetToFormat(B_TRANSLATE("Rename the volume to \"%s\""),
		DISTRO_NAME);
	fRename = new BCheckBox("rename", renameLabel, NULL);
	fRename->SetValue(renameVolume != NULL && *renameVolume);

	BButton* cancel = new BButton("cancel", B_TRANSLATE("Cancel"),
		new BMessage(kMsgCancel));
	BButton* proceed = new BButton("continue", B_TRANSLATE("Continue"),
		new BMessage(kMsgContinue));

	BLayoutBuilder::Group<>(this, B_VERTICAL)
		.SetInsets(B_USE_WINDOW_SPACING)
		.Add(headingView)
		.AddStrut(B_USE_SMALL_SPACING)
		.Add(fUpgrade)
		.AddGroup(B_VERTICAL, 0)
			.SetInsets(B_USE_BIG_SPACING, 0, 0, 0)
			.Add(note(B_TRANSLATE("The installed system is replaced; your home "
				"folder, settings and the software")))
			.Add(note(B_TRANSLATE("you installed stay. The old system stays "
				"bootable: \"Select boot volume/state\"")))
			.Add(note(B_TRANSLATE("in the boot menu.")))
			.Add(fRename)
		.End()
		.Add(fReplace)
		.AddGroup(B_VERTICAL, 0)
			.SetInsets(B_USE_BIG_SPACING, 0, 0, 0)
			.Add(note(B_TRANSLATE("The installed system and the software you "
				"installed are removed;")))
			.Add(note(B_TRANSLATE("your home folder and settings stay.")))
		.End()
		.Add(fErase)
		.AddGroup(B_VERTICAL, 0)
			.SetInsets(B_USE_BIG_SPACING, 0, 0, 0)
			.Add(note(eraseNote))
		.End()
		.Add(new BSeparatorView(B_HORIZONTAL))
		.AddGroup(B_HORIZONTAL)
			.AddGlue()
			.Add(cancel)
			.Add(proceed)
		.End()
	.End();

	// The safe choice first, and preselected.
	if (existing.found)
		fUpgrade->SetValue(B_CONTROL_ON);
	else {
		fUpgrade->SetEnabled(false);
		fReplace->SetValue(B_CONTROL_ON);
	}
	// Offered for a volume named "Haiku" only: other names are the user's.
	if (renameVolume == NULL || !*renameVolume || !existing.found)
		fRename->Hide();
	proceed->MakeDefault(true);
	CenterOnScreen();
}


InstallModeWindow::~InstallModeWindow()
{
	delete_sem(fDone);
}


void
InstallModeWindow::MessageReceived(BMessage* message)
{
	switch (message->what) {
		case kMsgContinue:
			if (fRenameVolume != NULL)
				*fRenameVolume = fRename->Value() == B_CONTROL_ON;
			if (fUpgrade->Value() == B_CONTROL_ON)
				fResult = INSTALL_MODE_UPGRADE;
			else if (fReplace->Value() == B_CONTROL_ON)
				fResult = INSTALL_MODE_REPLACE;
			else if (fErase->Value() == B_CONTROL_ON)
				fResult = INSTALL_MODE_ERASE;
			release_sem(fDone);
			break;
		case kMsgCancel:
			fResult = INSTALL_MODE_CANCEL;
			release_sem(fDone);
			break;
		default:
			BWindow::MessageReceived(message);
	}
}


bool
InstallModeWindow::QuitRequested()
{
	// Closing the window (or Escape) cancels.
	fResult = INSTALL_MODE_CANCEL;
	release_sem(fDone);
	return false;
}


install_mode
InstallModeWindow::Go()
{
	Show();
	while (acquire_sem(fDone) == B_INTERRUPTED)
		;
	install_mode result = fResult;
	if (Lock())
		Quit();
	return result;
}
