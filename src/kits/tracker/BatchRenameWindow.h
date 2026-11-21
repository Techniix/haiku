/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 */
#ifndef _BATCH_RENAME_WINDOW_H
#define _BATCH_RENAME_WINDOW_H


/*!	Renaming several items at once (roadmap F-66, batch 9): "Rename..."
	(the "Edit name" command with more than one item selected) opens this
	window. The new names are built in this order: every "Find" text is
	replaced with "Replace with"; "Add before" and "Add after" are added
	(after the name, before a short extension); with "Number them", a
	number starting at "Start at" is added after the name, as wide as the
	largest number. A preview lists every old and new name; Rename is
	possible only when every new name is valid and unique in its folder.
	Each renamed item gets its own undo step (Edit > Undo, one at a time).

	Turned on and off with "Rename several items at once" (TrackerSettings::
	BatchRename(), Tracker preferences, Windows page; on by default); off,
	"Edit name" works on one item as before.
*/


#include <Entry.h>
#include <ObjectList.h>
#include <String.h>
#include <Window.h>


class BButton;
class BCheckBox;
class BListView;
class BSpinner;
class BStringView;
class BTextControl;


namespace BPrivate {


// for the tests: kBatchRenameSet ("find", "replace", "prefix", "suffix",
// "number" (bool), "start" (int32)) replies with "name" for each item and
// "valid"; kBatchRenameApply renames and replies with "status"
const uint32 kBatchRenameSet = 'BRst';
const uint32 kBatchRenameApply = 'BRgo';


class BatchRenameWindow : public BWindow {
public:
								BatchRenameWindow(
									const BObjectList<entry_ref, true>& refs);
	virtual						~BatchRenameWindow();

	virtual	void				MessageReceived(BMessage* message);

	static	BString				NewName(const char* name, const char* find,
									const char* replace, const char* prefix,
									const char* suffix, bool number,
									int32 value, int32 digits);

private:
			bool				_Update();
			status_t			_Apply();

private:
			BObjectList<entry_ref, true> fRefs;
			BObjectList<BString, true> fNewNames;
			BTextControl*		fFind;
			BTextControl*		fReplace;
			BTextControl*		fPrefix;
			BTextControl*		fSuffix;
			BCheckBox*			fNumber;
			BSpinner*			fStart;
			BListView*			fPreview;
			BStringView*		fStatus;
			BButton*			fRename;
};


}	// namespace BPrivate

using namespace BPrivate;


#endif	// _BATCH_RENAME_WINDOW_H
