/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 */


#include "BatchRenameWindow.h"

#include <stdio.h>
#include <string.h>

#include <Button.h>
#include <Catalog.h>
#include <CheckBox.h>
#include <ControlLook.h>
#include <Directory.h>
#include <LayoutBuilder.h>
#include <ListView.h>
#include <ScrollView.h>
#include <Spinner.h>
#include <StringItem.h>
#include <StringView.h>
#include <TextControl.h>

#include <set>
#include <string>

#include "FSUndoRedo.h"


#undef B_TRANSLATION_CONTEXT
#define B_TRANSLATION_CONTEXT "BatchRenameWindow"


static const uint32 kChanged = 'BRch';


//!	Splits off a short extension (".txt", ".tar.gz" counts as ".gz").
static void
split_extension(const BString& name, BString& base, BString& extension)
{
	base = name;
	extension = "";
	int32 dot = name.FindLast('.');
	if (dot > 0 && name.Length() - dot <= 9 && name.FindFirst(' ', dot) < 0) {
		name.CopyInto(extension, dot, name.Length() - dot);
		base.Truncate(dot);
	}
}


/*static*/ BString
BatchRenameWindow::NewName(const char* name, const char* find,
	const char* replace, const char* prefix, const char* suffix, bool number,
	int32 value, int32 digits)
{
	BString result(name);
	if (find != NULL && find[0] != '\0')
		result.ReplaceAll(find, replace != NULL ? replace : "");

	BString base;
	BString extension;
	split_extension(result, base, extension);
	if (prefix != NULL)
		base.Prepend(prefix);
	if (suffix != NULL)
		base.Append(suffix);
	if (number) {
		BString numberText;
		numberText.SetToFormat(" %0*" B_PRId32, (int)digits, value);
		base.Append(numberText);
	}
	base.Append(extension);
	return base;
}


BatchRenameWindow::BatchRenameWindow(const BObjectList<entry_ref, true>& refs)
	:
	BWindow(BRect(0, 0, 100, 100), "", B_TITLED_WINDOW,
		B_NOT_ZOOMABLE | B_AUTO_UPDATE_SIZE_LIMITS | B_CLOSE_ON_ESCAPE),
	fRefs(refs.CountItems()),
	fNewNames(refs.CountItems())
{
	for (int32 i = 0; i < refs.CountItems(); i++)
		fRefs.AddItem(new entry_ref(*refs.ItemAt(i)));

	BString title;
	title.SetToFormat(B_TRANSLATE("Rename %d items"),
		(int)fRefs.CountItems());
	SetTitle(title);

	fFind = new BTextControl("find", B_TRANSLATE("Find:"), "",
		NULL);
	fReplace = new BTextControl("replace", B_TRANSLATE("Replace with:"), "",
		NULL);
	fPrefix = new BTextControl("prefix", B_TRANSLATE("Add before:"), "",
		NULL);
	fSuffix = new BTextControl("suffix", B_TRANSLATE("Add after:"), "",
		NULL);
	BTextControl* texts[] = { fFind, fReplace, fPrefix, fSuffix };
	for (int i = 0; i < 4; i++)
		texts[i]->SetModificationMessage(new BMessage(kChanged));
	fNumber = new BCheckBox("number", B_TRANSLATE("Number them, starting "
		"at:"), new BMessage(kChanged));
	fStart = new BSpinner("start", NULL, new BMessage(kChanged));
	fStart->SetRange(0, 999999);
	fStart->SetValue(1);

	fPreview = new BListView("preview");
	BScrollView* scroll = new BScrollView("scroll", fPreview, 0, false, true);
	float width = be_plain_font->StringWidth("M") * 40;
	scroll->SetExplicitMinSize(BSize(width, width * 0.45f));
	fStatus = new BStringView("status", "");
	fRename = new BButton("rename", B_TRANSLATE("Rename"),
		new BMessage(kBatchRenameApply));
	BButton* cancel = new BButton("cancel", B_TRANSLATE("Cancel"),
		new BMessage(B_QUIT_REQUESTED));

	BLayoutBuilder::Group<>(this, B_VERTICAL, B_USE_DEFAULT_SPACING)
		.SetInsets(B_USE_WINDOW_SPACING)
		.AddGrid(B_USE_SMALL_SPACING, B_USE_SMALL_SPACING)
			.AddTextControl(fFind, 0, 0)
			.AddTextControl(fReplace, 0, 1)
			.AddTextControl(fPrefix, 0, 2)
			.AddTextControl(fSuffix, 0, 3)
			.Add(fNumber, 0, 4, 1, 1)
			.Add(fStart, 1, 4)
			.End()
		.Add(scroll)
		.Add(fStatus)
		.AddGroup(B_HORIZONTAL)
			.AddGlue()
			.Add(cancel)
			.Add(fRename)
			.End();
	SetDefaultButton(fRename);
	CenterOnScreen();
	_Update();
}


BatchRenameWindow::~BatchRenameWindow()
{
}


/*!	Builds the new names and the preview; false when one of them cannot be
	used (empty, a "/", the same new name twice in a folder, or a name an
	item outside the selection has).
*/
bool
BatchRenameWindow::_Update()
{
	fNewNames.MakeEmpty();
	while (BListItem* item = fPreview->RemoveItem((int32)0))
		delete item;

	int32 count = fRefs.CountItems();
	int32 last = fStart->Value() + count - 1;
	int32 digits = 1;
	for (int32 n = last; n >= 10; n /= 10)
		digits++;

	bool valid = true;
	BString problem;
	std::set<std::string> taken;
	std::set<std::string> selected;
	for (int32 i = 0; i < count; i++) {
		entry_ref* ref = fRefs.ItemAt(i);
		BString key;
		key.SetToFormat("%" B_PRIdDEV ":%" B_PRIdINO ":%s", ref->device,
			ref->directory, ref->name);
		selected.insert(key.String());
	}
	int32 changed = 0;
	for (int32 i = 0; i < count; i++) {
		entry_ref* ref = fRefs.ItemAt(i);
		BString name = NewName(ref->name, fFind->Text(), fReplace->Text(),
			fPrefix->Text(), fSuffix->Text(),
			fNumber->Value() == B_CONTROL_ON, fStart->Value() + i, digits);
		fNewNames.AddItem(new BString(name));
		if (name != ref->name)
			changed++;

		BString key;
		key.SetToFormat("%" B_PRIdDEV ":%" B_PRIdINO ":%s", ref->device,
			ref->directory, name.String());
		if (name.IsEmpty() || name.FindFirst('/') >= 0
			|| name.Length() >= B_FILE_NAME_LENGTH) {
			valid = false;
			problem.SetToFormat(B_TRANSLATE("\"%s\" is not a valid name."),
				name.String());
		} else if (taken.find(key.String()) != taken.end()) {
			valid = false;
			problem.SetToFormat(B_TRANSLATE("\"%s\" would be used twice."),
				name.String());
		} else if (name != ref->name
			&& selected.find(key.String()) == selected.end()) {
			BDirectory directory;
			node_ref folder;
			folder.device = ref->device;
			folder.node = ref->directory;
			if (directory.SetTo(&folder) == B_OK
				&& directory.Contains(name.String())) {
				valid = false;
				problem.SetToFormat(B_TRANSLATE("\"%s\" exists already."),
					name.String());
			}
		}
		taken.insert(key.String());

		BString line;
		line.SetToFormat("%s  \xE2\x86\x92  %s", ref->name, name.String());
		fPreview->AddItem(new BStringItem(line));
	}

	if (!valid)
		fStatus->SetText(problem);
	else {
		BString status;
		status.SetToFormat(B_TRANSLATE("%d of %d names change."),
			(int)changed, (int)count);
		fStatus->SetText(status);
	}
	fRename->SetEnabled(valid && changed > 0);
	return valid;
}


/*!	Renames in two steps (each item first to a temporary name), so that
	names can be swapped or shifted within the selection. Each item gets its
	own undo step. On a failure the items renamed so far get their old names
	back.
*/
status_t
BatchRenameWindow::_Apply()
{
	if (!_Update())
		return B_BAD_VALUE;

	int32 count = fRefs.CountItems();
	BObjectList<BString, true> temporary(count);
	status_t status = B_OK;
	int32 moved = 0;
	for (; moved < count; moved++) {
		entry_ref* ref = fRefs.ItemAt(moved);
		BEntry entry(ref);
		BString name;
		name.SetToFormat(".renga-rename-%d-%" B_PRId32, (int)find_thread(NULL),
			moved);
		status = entry.Rename(name);
		if (status != B_OK)
			break;
		temporary.AddItem(new BString(name));
	}
	if (status != B_OK) {
		for (int32 i = 0; i < moved; i++) {
			entry_ref* ref = fRefs.ItemAt(i);
			node_ref folder;
			folder.device = ref->device;
			folder.node = ref->directory;
			BDirectory directory(&folder);
			BEntry entry(&directory, temporary.ItemAt(i)->String());
			entry.Rename(ref->name);
		}
		return status;
	}

	for (int32 i = 0; i < count; i++) {
		entry_ref* ref = fRefs.ItemAt(i);
		node_ref folder;
		folder.device = ref->device;
		folder.node = ref->directory;
		BDirectory directory(&folder);
		BEntry entry(&directory, temporary.ItemAt(i)->String());
		status_t result = entry.Rename(fNewNames.ItemAt(i)->String());
		if (result != B_OK) {
			entry.Rename(ref->name);
			status = result;
			continue;
		}
		{
			// the undo step goes from the new name back to the old one
			// (an abstract entry: the old name is gone now)
			BEntry old(&directory, ref->name);
			RenameUndo undo(old, fNewNames.ItemAt(i)->String());
		}
		strlcpy(ref->name, fNewNames.ItemAt(i)->String(), B_FILE_NAME_LENGTH);
	}
	return status;
}


void
BatchRenameWindow::MessageReceived(BMessage* message)
{
	switch (message->what) {
		case kChanged:
			_Update();
			break;

		case kBatchRenameSet:
		{
			const char* text;
			if (message->FindString("find", &text) == B_OK)
				fFind->SetText(text);
			if (message->FindString("replace", &text) == B_OK)
				fReplace->SetText(text);
			if (message->FindString("prefix", &text) == B_OK)
				fPrefix->SetText(text);
			if (message->FindString("suffix", &text) == B_OK)
				fSuffix->SetText(text);
			bool number;
			if (message->FindBool("number", &number) == B_OK)
				fNumber->SetValue(number ? B_CONTROL_ON : B_CONTROL_OFF);
			int32 start;
			if (message->FindInt32("start", &start) == B_OK)
				fStart->SetValue(start);
			BMessage reply(B_REPLY);
			reply.AddBool("valid", _Update());
			for (int32 i = 0; i < fNewNames.CountItems(); i++)
				reply.AddString("name", *fNewNames.ItemAt(i));
			reply.AddString("status", fStatus->Text());
			message->SendReply(&reply);
			break;
		}

		case kBatchRenameApply:
		{
			status_t status = _Apply();
			if (message->IsSourceWaiting()) {
				BMessage reply(B_REPLY);
				reply.AddInt32("status", status);
				message->SendReply(&reply);
			}
			if (status == B_OK)
				PostMessage(B_QUIT_REQUESTED);
			else {
				BString text;
				text.SetToFormat(B_TRANSLATE("Renaming failed: %s"),
					strerror(status));
				fStatus->SetText(text);
			}
			break;
		}

		default:
			BWindow::MessageReceived(message);
	}
}
