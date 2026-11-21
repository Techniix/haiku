/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 */


#include "ConflictDialog.h"

#include <Button.h>
#include <Catalog.h>
#include <CheckBox.h>
#include <ControlLook.h>
#include <IconUtils.h>
#include <LayoutBuilder.h>
#include <Looper.h>
#include <Screen.h>
#include <StringView.h>
#include <TextView.h>


#undef B_TRANSLATION_CONTEXT
#define B_TRANSLATION_CONTEXT "ConflictDialog"


const char* ConflictDialog::kWindowTitle = "Name conflict";
	// not translated: the tests find the window by it


static BButton*
choice_button(const char* label, conflict_choice choice)
{
	BMessage* message = new BMessage(kConflictChoice);
	message->AddInt32("choice", choice);
	return new BButton(label, message);
}


ConflictDialog::ConflictDialog(const char* name, const char* folderName,
	const char* existingInfo, const char* newInfo, bool isFolder, bool moving,
	bool offerApplyToAll)
	:
	BWindow(BRect(0, 0, 100, 100), kWindowTitle, B_MODAL_WINDOW_LOOK,
		B_MODAL_APP_WINDOW_FEEL, B_NOT_RESIZABLE | B_NOT_ZOOMABLE
			| B_NOT_MINIMIZABLE | B_AUTO_UPDATE_SIZE_LIMITS
			| B_CLOSE_ON_ESCAPE),
	fApplyToAll(NULL),
	fDone(create_sem(0, "conflict dialog")),
	fChoice(kConflictCancel),
	fApplied(false)
{
	BString heading;
	heading.SetToFormat(B_TRANSLATE("\"%s\" already exists in \"%s\"."),
		name, folderName);
	BString text;
	if (isFolder) {
		text = moving
			? B_TRANSLATE("Merge puts the items you are moving into the "
				"existing folder (items with the same names are replaced). "
				"Keep both moves the folder in under a new name.")
			: B_TRANSLATE("Merge puts the items you are copying into the "
				"existing folder (items with the same names are replaced). "
				"Keep both copies the folder in under a new name.");
	} else {
		text.SetToFormat("%s %s\n%s %s", B_TRANSLATE("Existing:"),
			existingInfo, moving ? B_TRANSLATE("Moving:")
				: B_TRANSLATE("Copying:"), newInfo);
	}

	BStringView* headingView = new BStringView("heading", heading);
	BFont bold(be_bold_font);
	headingView->SetFont(&bold);
	headingView->SetExplicitMaxSize(BSize(B_SIZE_UNLIMITED, B_SIZE_UNSET));

	BTextView* textView = new BTextView("text");
	textView->SetText(text);
	textView->MakeEditable(false);
	textView->MakeSelectable(false);
	textView->SetWordWrap(true);
	textView->SetViewUIColor(B_PANEL_BACKGROUND_COLOR);
	textView->SetLowUIColor(B_PANEL_BACKGROUND_COLOR);
	float width = be_plain_font->StringWidth("M") * 34;
	textView->SetExplicitMinSize(BSize(width, B_SIZE_UNSET));

	BButton* replace = choice_button(isFolder ? B_TRANSLATE("Merge")
		: B_TRANSLATE("Replace"), kConflictReplace);
	BButton* keepBoth = choice_button(B_TRANSLATE("Keep both"),
		kConflictKeepBoth);
	BButton* skip = choice_button(B_TRANSLATE("Skip"), kConflictSkip);
	BButton* cancel = choice_button(B_TRANSLATE("Cancel"), kConflictCancel);

	BLayoutBuilder::Group<> builder = BLayoutBuilder::Group<>(this,
		B_VERTICAL, B_USE_DEFAULT_SPACING)
		.SetInsets(B_USE_WINDOW_SPACING)
		.Add(headingView)
		.Add(textView);
	if (offerApplyToAll) {
		fApplyToAll = new BCheckBox("applyToAll", B_TRANSLATE("Apply to all "
			"conflicts"), NULL);
		builder.Add(fApplyToAll);
	}
	builder.AddGroup(B_HORIZONTAL)
			.Add(cancel)
			.AddGlue()
			.Add(skip)
			.Add(keepBoth)
			.Add(replace)
			.End();

	SetDefaultButton(replace);
	CenterOnScreen();
}


ConflictDialog::~ConflictDialog()
{
	delete_sem(fDone);
}


conflict_choice
ConflictDialog::Go(bool* _applyToAll)
{
	Show();

	// As BAlert::Go(): a window's thread keeps its windows drawn while it
	// waits.
	BWindow* window = dynamic_cast<BWindow*>(BLooper::LooperForThread(
		find_thread(NULL)));
	sem_id done = fDone;
	if (window != NULL) {
		while (acquire_sem_etc(done, 1, B_RELATIVE_TIMEOUT, 50000)
				== B_TIMED_OUT) {
			window->UpdateIfNeeded();
		}
	} else {
		while (acquire_sem(done) == B_INTERRUPTED)
			;
	}

	conflict_choice choice = kConflictCancel;
	bool applied = false;
	if (Lock()) {
		choice = fChoice;
		applied = fApplied;
		Quit();
	}
	if (_applyToAll != NULL)
		*_applyToAll = applied;
	return choice;
}


void
ConflictDialog::MessageReceived(BMessage* message)
{
	if (message->what != kConflictChoice) {
		BWindow::MessageReceived(message);
		return;
	}

	bool apply;
	if (fApplyToAll != NULL && message->FindBool("apply_all", &apply) == B_OK)
		fApplyToAll->SetValue(apply ? B_CONTROL_ON : B_CONTROL_OFF);
	fChoice = (conflict_choice)message->GetInt32("choice", kConflictCancel);
	fApplied = fApplyToAll != NULL && fApplyToAll->Value() == B_CONTROL_ON;
	Hide();
	release_sem(fDone);
}


bool
ConflictDialog::QuitRequested()
{
	// Escape or the close key: Cancel
	fChoice = kConflictCancel;
	fApplied = false;
	Hide();
	release_sem(fDone);
	return false;
}
