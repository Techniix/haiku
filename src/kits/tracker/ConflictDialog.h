/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 */
#ifndef _CONFLICT_DIALOG_H
#define _CONFLICT_DIALOG_H


/*!	The question Tracker asks when an item is copied or moved onto a name
	that exists already (roadmap F-66, batch 9): Replace (Merge for
	folders), Keep both, Skip or Cancel, with "Apply to all" when more items
	collide. Shown by CheckName() in FSUtils.cpp when "Ask with \"Keep
	both\" when names collide" is on (TrackerSettings::
	KeepBothOnConflict()); off, Tracker asks its classic questions.

	Go() blocks the calling thread (the copy thread) like BAlert::Go().
*/


#include <String.h>
#include <Window.h>


class BCheckBox;


namespace BPrivate {


enum conflict_choice {
	kConflictCancel = 0,
	kConflictSkip,
	kConflictKeepBoth,
	kConflictReplace
};


// the buttons' messages, also for the tests: "apply_all" (bool,
// optional) sets the check box first
const uint32 kConflictChoice = 'Cfch';
	// "choice" (int32, conflict_choice)


class ConflictDialog : public BWindow {
public:
								ConflictDialog(const char* name,
									const char* folderName,
									const char* existingInfo,
									const char* newInfo, bool isFolder,
									bool moving, bool offerApplyToAll);
	virtual						~ConflictDialog();

	//!	Shows the dialog and waits; the dialog is gone afterwards.
			conflict_choice		Go(bool* _applyToAll);

	virtual	void				MessageReceived(BMessage* message);
	virtual	bool				QuitRequested();

	static	const char*			kWindowTitle;

private:
			BCheckBox*			fApplyToAll;
			sem_id				fDone;
			conflict_choice		fChoice;
			bool				fApplied;
};


}	// namespace BPrivate

using namespace BPrivate;


#endif	// _CONFLICT_DIALOG_H
