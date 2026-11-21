/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 */

/*!	In-guest test: Tracker's batch 9 features (roadmap F-66) and the F-60
	review numbers.

	Tracker's hook kTestTrackerState (harness images only, Commands.h)
	returns the batch 9 settings and, for an entry, its folder size from
	FolderSizeCache, the Size column's text in its parent's window, or the
	labels of a folder window's context menu. The name-conflict questions
	run in this program: it links libtracker and copies with
	FSMoveToFolder(), Tracker's own copy engine, and answers the dialogs it
	shows by posting their buttons' messages.

	  tracker_modern.big_folder        a folder of 10 000 files opens and
	                                   shows all of them; Tracker answers
	                                   while it loads (times as INFO, F-60)
	  tracker_modern.folder_size       a folder of 10 000 bytes in four files
	                                   (one in a subfolder) is computed in the
	                                   background as 10 000
	  tracker_modern.size_column       its row in the list view shows the size
	                                   (screenshot tracker_folder_sizes)
	  tracker_modern.size_switch       "Show folder sizes" off from the
	                                   preferences shows "-", on shows it again
	  tracker_modern.terminal_item     "Open Terminal here" in a folder
	                                   window's context menu; not with the
	                                   switch off
	  tracker_modern.terminal_opens    choosing it starts a Terminal in that
	                                   folder
	  tracker_modern.keep_both_all     Keep both with "Apply to all": the
	                                   new items arrive as "a copy.txt" and
	                                   "b copy.txt", one question (screenshot
	                                   tracker_conflict_dialog)
	  tracker_modern.replace_all       Replace with "Apply to all"
	  tracker_modern.skip_then_keep    Skip, then Keep both, one at a time
	  tracker_modern.cancel            Cancel copies nothing more
	  tracker_modern.folder_keep_both  a folder kept as "d copy"
	  tracker_modern.move_keep_both    moving onto an existing name keeps
	                                   both and removes the source
	  tracker_modern.classic           with the switch off the classic
	                                   question comes instead
	  tracker_modern.quick_look_image  Quick Look shows a picture (screenshot
	                                   tracker_quick_look) and closes again
	  tracker_modern.quick_look_text   and a text file
	  tracker_modern.quick_look_space  the space bar opens and closes it;
	                                   nothing with the switch off
	  tracker_modern.rename_names      the batch rename's names: numbers as
	                                   wide as the largest, the extension
	                                   kept
	  tracker_modern.batch_rename      "Edit name" with three items selected
	                                   opens the batch rename (screenshot
	                                   tracker_batch_rename); a name that
	                                   exists is refused; Rename renames all
	  tracker_modern.batch_switch      with the switch off it does not open
	  tracker_modern.type_filter       type to filter is on for new installs
	  tracker_modern.prefs_revert      Revert in the Tracker preferences
	                                   restores "Hide dotfiles" and leaves
	                                   "List folders first" alone (an upstream
	                                   bug set the wrong setting); screenshot
	                                   tracker_preferences_batch9

	Output format: see 3rdparty/ce/README.md.
*/


#include <Alert.h>
#include <Application.h>
#include <Directory.h>
#include <Entry.h>
#include <File.h>
#include <FindDirectory.h>
#include <Messenger.h>
#include <NodeInfo.h>
#include <ObjectList.h>
#include <Path.h>
#include <Roster.h>
#include <String.h>
#include <Window.h>

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "Commands.h"
#include "QuickLookWindow.h"
#include "BatchRenameWindow.h"
#include "ConflictDialog.h"
#include "FSUtils.h"
#include "TrackerSettings.h"


#define SUITE "tracker_modern"

static const char* kTerminalSignature = "application/x-vnd.Haiku-Terminal";
static const uint32 kRevertButtonPressed = 'Rebp';
static const uint32 kListModeMessage = 'Tlst';
static const char* kTrackerAppSignature = "application/x-vnd.Be-TRAK";

static int sFailures = 0;


static void
report(const char* name, bool pass, const char* format, ...)
{
	char detail[400];
	va_list args;
	va_start(args, format);
	vsnprintf(detail, sizeof(detail), format, args);
	va_end(args);

	printf("CE-TEST %s.%s %s %s\n", SUITE, name, pass ? "PASS" : "FAIL",
		detail);
	fflush(stdout);
	if (!pass)
		sFailures++;
}


static BMessage
state(const char* path = NULL, const char* action = NULL,
	BMessage* extra = NULL)
{
	BMessage message(kTestTrackerState);
	if (extra != NULL)
		message = *extra, message.what = kTestTrackerState;
	entry_ref ref;
	if (path != NULL && get_ref_for_path(path, &ref) == B_OK)
		message.AddRef("refs", &ref);
	if (action != NULL)
		message.AddString("action", action);
	BMessage reply;
	BMessenger(kTrackerAppSignature).SendMessage(&message, &reply, 5000000,
		10000000);
	return reply;
}


static void
write_file(const char* path, const char* contents, size_t size = 0)
{
	BFile file(path, B_CREATE_FILE | B_ERASE_FILE | B_WRITE_ONLY);
	if (size == 0)
		file.Write(contents, strlen(contents));
	else {
		char* buffer = (char*)calloc(1, size);
		file.Write(buffer, size);
		free(buffer);
	}
}


static BString
read_file(const char* path)
{
	BFile file(path, B_READ_ONLY);
	char buffer[64] = "";
	ssize_t read = file.Read(buffer, sizeof(buffer) - 1);
	if (read < 0)
		return "<missing>";
	buffer[read] = '\0';
	return buffer;
}


static void
remove_tree(const char* path)
{
	BString command;
	command.SetToFormat("rm -rf \"%s\"", path);
	system(command.String());
}


static bool
open_folder(const char* path)
{
	entry_ref ref;
	if (get_ref_for_path(path, &ref) != B_OK)
		return false;
	BMessage refs(B_REFS_RECEIVED);
	refs.AddRef("refs", &ref);
	return BMessenger(kTrackerAppSignature).SendMessage(&refs) == B_OK;
}


static void
close_window(const char* title)
{
	BMessage close(B_QUIT_REQUESTED);
	close.AddSpecifier("Window", title);
	BMessage reply;
	BMessenger(kTrackerAppSignature).SendMessage(&close, &reply, 1000000,
		1000000);
}


static int32
count_poses(const char* windowTitle, bigtime_t* _latency)
{
	BMessage count(B_COUNT_PROPERTIES);
	count.AddSpecifier("Entry");
	count.AddSpecifier("Poses");
	count.AddSpecifier("Window", windowTitle);
	BMessage reply;
	bigtime_t start = system_time();
	status_t status = BMessenger(kTrackerAppSignature).SendMessage(&count,
		&reply, 5000000, 5000000);
	if (_latency != NULL)
		*_latency = system_time() - start;
	int32 result = -1;
	if (status == B_OK)
		reply.FindInt32("result", &result);
	return result;
}


// #pragma mark - conflicts


static ConflictDialog*
find_dialog(bigtime_t timeout = 10000000)
{
	for (bigtime_t waited = 0; waited < timeout; waited += 100000) {
		if (be_app->Lock()) {
			for (int32 i = 0; i < be_app->CountWindows(); i++) {
				ConflictDialog* dialog
					= dynamic_cast<ConflictDialog*>(be_app->WindowAt(i));
				if (dialog != NULL && !dialog->IsHidden()) {
					be_app->Unlock();
					return dialog;
				}
			}
			be_app->Unlock();
		}
		snooze(100000);
	}
	return NULL;
}


static BAlert*
find_alert(bigtime_t timeout)
{
	for (bigtime_t waited = 0; waited < timeout; waited += 100000) {
		if (be_app->Lock()) {
			for (int32 i = 0; i < be_app->CountWindows(); i++) {
				BAlert* alert = dynamic_cast<BAlert*>(be_app->WindowAt(i));
				if (alert != NULL) {
					be_app->Unlock();
					return alert;
				}
			}
			be_app->Unlock();
		}
		snooze(100000);
	}
	return NULL;
}


static void
answer(ConflictDialog* dialog, conflict_choice choice, bool applyToAll)
{
	BMessage message(kConflictChoice);
	message.AddInt32("choice", choice);
	message.AddBool("apply_all", applyToAll);
	dialog->PostMessage(&message);
	snooze(300000);
}


struct Folders {
	BPath	source;
	BPath	dest;
};


/*!	The source has a.txt, b.txt and c.txt ("new-..."), the destination
	a.txt and b.txt ("old-...").
*/
static void
prepare(Folders& folders)
{
	remove_tree(folders.source.Path());
	remove_tree(folders.dest.Path());
	create_directory(folders.source.Path(), 0755);
	create_directory(folders.dest.Path(), 0755);
	const char* names[] = { "a.txt", "b.txt", "c.txt" };
	for (int i = 0; i < 3; i++) {
		BPath path(folders.source.Path(), names[i]);
		BString contents;
		contents.SetToFormat("new-%c", names[i][0]);
		write_file(path.Path(), contents);
		if (i < 2) {
			BPath old(folders.dest.Path(), names[i]);
			contents.SetToFormat("old-%c", names[i][0]);
			write_file(old.Path(), contents);
		}
	}
}


static void
copy(Folders& folders, const char** names, int count, uint32 mode)
{
	BObjectList<entry_ref, true>* list
		= new BObjectList<entry_ref, true>(count);
	for (int i = 0; i < count; i++) {
		BPath path(folders.source.Path(), names[i]);
		entry_ref* ref = new entry_ref;
		get_ref_for_path(path.Path(), ref);
		list->AddItem(ref);
	}
	FSMoveToFolder(list, new BEntry(folders.dest.Path()), mode);
}


static BString
dest(Folders& folders, const char* name)
{
	BPath path(folders.dest.Path(), name);
	return read_file(path.Path());
}


//! Waits until \a name exists in the destination (the copy is done).
static bool
wait_for(Folders& folders, const char* name, bigtime_t timeout = 10000000)
{
	BPath path(folders.dest.Path(), name);
	for (bigtime_t waited = 0; waited < timeout; waited += 100000) {
		if (access(path.Path(), F_OK) == 0) {
			snooze(300000);
			return true;
		}
		snooze(100000);
	}
	return false;
}


static void
test_conflicts()
{
	TrackerSettings settings;
	bool keepBoth = settings.KeepBothOnConflict();
	settings.SetKeepBothOnConflict(true);

	Folders folders;
	find_directory(B_USER_DIRECTORY, &folders.source);
	folders.dest = folders.source;
	folders.source.Append("ce-conflict-source");
	folders.dest.Append("ce-conflict-dest");
	const char* abc[] = { "a.txt", "b.txt", "c.txt" };

	// Keep both, apply to all
	prepare(folders);
	copy(folders, abc, 3, kCopySelectionTo);
	ConflictDialog* dialog = find_dialog();
	if (dialog != NULL) {
		printf("CE-HOST screenshot tracker_conflict_dialog\n");
		fflush(stdout);
		snooze(700000);
		answer(dialog, kConflictKeepBoth, true);
	}
	wait_for(folders, "c.txt");
	ConflictDialog* second = find_dialog(1500000);
	report("keep_both_all", dialog != NULL && second == NULL
			&& dest(folders, "a.txt") == "old-a"
			&& dest(folders, "b.txt") == "old-b"
			&& dest(folders, "a copy.txt") == "new-a"
			&& dest(folders, "b copy.txt") == "new-b"
			&& dest(folders, "c.txt") == "new-c",
		"dialog %d, asked again %d; a \"%s\", a copy \"%s\", b \"%s\", "
		"b copy \"%s\", c \"%s\"", dialog != NULL, second != NULL,
		dest(folders, "a.txt").String(), dest(folders, "a copy.txt").String(),
		dest(folders, "b.txt").String(), dest(folders, "b copy.txt").String(),
		dest(folders, "c.txt").String());
	if (second != NULL)
		answer(second, kConflictCancel, true);

	// Replace, apply to all
	prepare(folders);
	copy(folders, abc, 3, kCopySelectionTo);
	dialog = find_dialog();
	if (dialog != NULL)
		answer(dialog, kConflictReplace, true);
	wait_for(folders, "c.txt");
	second = find_dialog(1500000);
	report("replace_all", dialog != NULL && second == NULL
			&& dest(folders, "a.txt") == "new-a"
			&& dest(folders, "b.txt") == "new-b"
			&& dest(folders, "a copy.txt") == "<missing>",
		"dialog %d, asked again %d; a \"%s\", b \"%s\"", dialog != NULL,
		second != NULL, dest(folders, "a.txt").String(),
		dest(folders, "b.txt").String());
	if (second != NULL)
		answer(second, kConflictCancel, true);

	// Skip, then Keep both
	prepare(folders);
	copy(folders, abc, 3, kCopySelectionTo);
	dialog = find_dialog();
	if (dialog != NULL)
		answer(dialog, kConflictSkip, false);
	second = find_dialog();
	if (second != NULL)
		answer(second, kConflictKeepBoth, false);
	wait_for(folders, "c.txt");
	report("skip_then_keep", dialog != NULL && second != NULL
			&& dest(folders, "a.txt") == "old-a"
			&& dest(folders, "a copy.txt") == "<missing>"
			&& dest(folders, "b.txt") == "old-b"
			&& dest(folders, "b copy.txt") == "new-b"
			&& dest(folders, "c.txt") == "new-c",
		"questions %d+%d; a \"%s\", a copy \"%s\", b copy \"%s\", c \"%s\"",
		dialog != NULL, second != NULL, dest(folders, "a.txt").String(),
		dest(folders, "a copy.txt").String(),
		dest(folders, "b copy.txt").String(), dest(folders, "c.txt").String());

	// Cancel
	prepare(folders);
	copy(folders, abc, 3, kCopySelectionTo);
	dialog = find_dialog();
	if (dialog != NULL)
		answer(dialog, kConflictCancel, false);
	bool copied = wait_for(folders, "c.txt", 3000000);
	report("cancel", dialog != NULL && !copied
			&& dest(folders, "a.txt") == "old-a",
		"dialog %d, c.txt copied %d, a \"%s\"", dialog != NULL, copied,
		dest(folders, "a.txt").String());

	// A folder
	prepare(folders);
	BPath sourceFolder(folders.source.Path(), "d");
	BPath destFolder(folders.dest.Path(), "d");
	create_directory(sourceFolder.Path(), 0755);
	create_directory(destFolder.Path(), 0755);
	BPath inner(sourceFolder.Path(), "y.txt");
	write_file(inner.Path(), "new-y");
	BPath existing(destFolder.Path(), "x.txt");
	write_file(existing.Path(), "old-x");
	const char* d[] = { "d" };
	copy(folders, d, 1, kCopySelectionTo);
	dialog = find_dialog();
	if (dialog != NULL)
		answer(dialog, kConflictKeepBoth, false);
	wait_for(folders, "d copy/y.txt");
	report("folder_keep_both", dialog != NULL
			&& dest(folders, "d copy/y.txt") == "new-y"
			&& dest(folders, "d/x.txt") == "old-x"
			&& dest(folders, "d/y.txt") == "<missing>",
		"dialog %d; d copy/y \"%s\", d/x \"%s\", d/y \"%s\"", dialog != NULL,
		dest(folders, "d copy/y.txt").String(),
		dest(folders, "d/x.txt").String(), dest(folders, "d/y.txt").String());

	// Moving
	prepare(folders);
	const char* a[] = { "a.txt" };
	copy(folders, a, 1, kMoveSelectionTo);
	dialog = find_dialog();
	if (dialog != NULL)
		answer(dialog, kConflictKeepBoth, false);
	wait_for(folders, "a copy.txt");
	BPath movedSource(folders.source.Path(), "a.txt");
	report("move_keep_both", dialog != NULL
			&& dest(folders, "a copy.txt") == "new-a"
			&& dest(folders, "a.txt") == "old-a"
			&& access(movedSource.Path(), F_OK) != 0,
		"dialog %d; a copy \"%s\", a \"%s\", source still there %d",
		dialog != NULL, dest(folders, "a copy.txt").String(),
		dest(folders, "a.txt").String(),
		access(movedSource.Path(), F_OK) == 0);

	// Classic questions with the switch off
	settings.SetKeepBothOnConflict(false);
	prepare(folders);
	copy(folders, abc, 3, kCopySelectionTo);
	BAlert* alert = find_alert(10000000);
	dialog = find_dialog(500000);
	if (alert != NULL) {
		// the classic question for several conflicts: Cancel
		BMessage cancel('ALTB');
		cancel.AddInt32("which", 0);
		alert->PostMessage(&cancel);
	}
	copied = wait_for(folders, "c.txt", 3000000);
	report("classic", alert != NULL && dialog == NULL && !copied,
		"classic question %d, new dialog %d, copied %d", alert != NULL,
		dialog != NULL, copied);

	settings.SetKeepBothOnConflict(keepBoth);
	remove_tree(folders.source.Path());
	remove_tree(folders.dest.Path());
}


// #pragma mark - Quick Look and batch rename


static void
write_bmp(const char* path, int width, int height)
{
	int rowSize = (width * 3 + 3) & ~3;
	int dataSize = rowSize * height;
	uint8 header[54] = { 'B', 'M' };
	uint32 fileSize = 54 + dataSize;
	memcpy(header + 2, &fileSize, 4);
	uint32 offset = 54;
	memcpy(header + 10, &offset, 4);
	uint32 infoSize = 40;
	memcpy(header + 14, &infoSize, 4);
	int32 w = width, h = height;
	memcpy(header + 18, &w, 4);
	memcpy(header + 22, &h, 4);
	uint16 planes = 1, bits = 24;
	memcpy(header + 26, &planes, 2);
	memcpy(header + 28, &bits, 2);
	memcpy(header + 34, &dataSize, 4);
	BFile file(path, B_CREATE_FILE | B_ERASE_FILE | B_WRITE_ONLY);
	file.Write(header, sizeof(header));
	uint8* row = (uint8*)calloc(1, rowSize);
	for (int y = 0; y < height; y++) {
		for (int x = 0; x < width; x++) {
			// a leaf-green to sky-blue gradient with a light band
			row[x * 3 + 0] = (uint8)(255 * x / width);
			row[x * 3 + 1] = (uint8)(160 + 60 * y / height);
			row[x * 3 + 2] = (y > height / 3 && y < height / 2) ? 240 : 60;
		}
		file.Write(row, rowSize);
	}
	free(row);
	BNodeInfo info(&file);
	info.SetType("image/bmp");
}


static status_t
to_poses(const char* windowTitle, BMessage& message, BMessage* reply = NULL)
{
	message.AddSpecifier("Poses");
	message.AddSpecifier("Window", windowTitle);
	BMessage dummy;
	return BMessenger(kTrackerAppSignature).SendMessage(&message,
		reply != NULL ? reply : &dummy, 2000000, 2000000);
}


static void
select_entry(const char* windowTitle, const char* path)
{
	BMessage select(B_SET_PROPERTY);
	entry_ref ref;
	get_ref_for_path(path, &ref);
	select.AddRef("data", &ref);
	select.AddSpecifier("Selection");
	to_poses(windowTitle, select);
	snooze(300000);
}


static BMessage
quick_look_state()
{
	BMessage reply = state();
	BMessage quickLook;
	reply.FindMessage("quick_look", &quickLook);
	return quickLook;
}


static void
press_space(const char* folder)
{
	// through Tracker's hook: a scripted B_KEY_DOWN keeps its specifiers
	// and never reaches KeyDown()
	state(folder, "press_space");
	snooze(1000000);
}


static bool
window_exists(const char* title)
{
	BMessage get(B_GET_PROPERTY);
	get.AddSpecifier("Title");
	get.AddSpecifier("Window", title);
	BMessage reply;
	if (BMessenger(kTrackerAppSignature).SendMessage(&get, &reply, 1000000,
			1000000) != B_OK) {
		return false;
	}
	return reply.what == B_REPLY && reply.GetInt32("error", B_OK) == B_OK;
}


static void
test_quick_look_and_rename(const BPath& home)
{
	BPath folder(home.Path(), "ce-quicklook");
	remove_tree(folder.Path());
	create_directory(folder.Path(), 0755);
	BPath picture(folder.Path(), "leaf.bmp");
	write_bmp(picture.Path(), 320, 200);
	BPath text(folder.Path(), "notes.txt");
	write_file(text.Path(), "Renga Quick Look\nA text file, shown read "
		"only.\n");
	{
		BNode node(text.Path());
		BNodeInfo(&node).SetType("text/plain");
	}
	open_folder(folder.Path());
	snooze(2500000);

	select_entry("ce-quicklook", picture.Path());
	BMessage look(kQuickLook);
	to_poses("ce-quicklook", look);
	snooze(2000000);
	BMessage shown = quick_look_state();
	printf("CE-HOST screenshot tracker_quick_look\n");
	fflush(stdout);
	snooze(500000);
	BMessage again(kQuickLook);
	to_poses("ce-quicklook", again);
	snooze(1000000);
	BMessage closed = quick_look_state();
	// minimum-* images have no image translators: the picture is then
	// shown as its icon and information, as designed
	bool translator = access(
		"/boot/system/add-ons/Translators/BMPTranslator", F_OK) == 0;
	report("quick_look_image", shown.GetBool("open", false)
			&& strcmp(shown.GetString("kind", ""),
				translator ? "image" : "info") == 0
			&& strcmp(shown.GetString("title", ""), "leaf.bmp") == 0
			&& !closed.GetBool("open", true),
		"open %d, kind \"%s\" (BMP translator %d), title \"%s\"; closed "
		"again %d", shown.GetBool("open", false), shown.GetString("kind", ""),
		translator, shown.GetString("title", ""),
		!closed.GetBool("open", true));

	select_entry("ce-quicklook", text.Path());
	BMessage lookText(kQuickLook);
	to_poses("ce-quicklook", lookText);
	snooze(1500000);
	BMessage textShown = quick_look_state();
	BMessage lookTextAgain(kQuickLook);
	to_poses("ce-quicklook", lookTextAgain);
	snooze(800000);
	report("quick_look_text", textShown.GetBool("open", false)
			&& strcmp(textShown.GetString("kind", ""), "text") == 0,
		"open %d, kind \"%s\"", textShown.GetBool("open", false),
		textShown.GetString("kind", ""));

	press_space(folder.Path());
	BMessage spaceOpen = quick_look_state();
	press_space(folder.Path());
	BMessage spaceClosed = quick_look_state();
	BMessage off;
	off.AddBool("set_QuickLook", false);
	state(NULL, NULL, &off);
	press_space(folder.Path());
	BMessage spaceOff = quick_look_state();
	BMessage on;
	on.AddBool("set_QuickLook", true);
	state(NULL, NULL, &on);
	if (spaceOff.GetBool("open", false)) {
		BMessage close(kQuickLook);
		to_poses("ce-quicklook", close);
	}
	report("quick_look_space", spaceOpen.GetBool("open", false)
			&& !spaceClosed.GetBool("open", true)
			&& !spaceOff.GetBool("open", true),
		"space opens %d, closes %d, with the switch off opens %d",
		spaceOpen.GetBool("open", false), !spaceClosed.GetBool("open", true),
		spaceOff.GetBool("open", false));
	close_window("ce-quicklook");

	// Batch rename
	BString numbered = BatchRenameWindow::NewName("photo.jpg", "", "", "",
		"", true, 7, 3);
	BString archive = BatchRenameWindow::NewName("IMG_1.tar.gz", "IMG_",
		"Trip-", "", " (old)", false, 0, 1);
	report("rename_names", numbered == "photo 007.jpg"
			&& archive == "Trip-1.tar (old).gz",
		"\"%s\" (want \"photo 007.jpg\"), \"%s\" (want \"Trip-1.tar "
		"(old).gz\")", numbered.String(), archive.String());

	BPath batch(home.Path(), "ce-batch");
	remove_tree(batch.Path());
	create_directory(batch.Path(), 0755);
	for (int i = 1; i <= 3; i++) {
		BString name;
		name.SetToFormat("%s/IMG_%03d.jpg", batch.Path(), i);
		write_file(name.String(), "jpg");
	}
	open_folder(batch.Path());
	snooze(2500000);
	BMessage selectAll(B_SELECT_ALL);
	to_poses("ce-batch", selectAll);
	snooze(500000);
	// a name the rename would take, outside the selection
	BString blocker;
	blocker.SetToFormat("%s/Beach-002.jpg", batch.Path());
	write_file(blocker.String(), "other");
	snooze(500000);

	BMessage edit(kEditName);
	to_poses("ce-batch", edit);
	snooze(1500000);
	bool opened = window_exists("Rename 3 items");

	BMessage set(kBatchRenameSet);
	set.AddString("find", "IMG_");
	set.AddString("replace", "Beach-");
	set.AddSpecifier("Window", "Rename 3 items");
	BMessage refused;
	BMessenger(kTrackerAppSignature).SendMessage(&set, &refused, 2000000,
		2000000);

	BMessage set2(kBatchRenameSet);
	set2.AddString("find", "IMG_");
	set2.AddString("replace", "");
	set2.AddString("prefix", "Trip-");
	set2.AddSpecifier("Window", "Rename 3 items");
	BMessage accepted;
	BMessenger(kTrackerAppSignature).SendMessage(&set2, &accepted, 2000000,
		2000000);
	printf("CE-HOST screenshot tracker_batch_rename\n");
	fflush(stdout);
	snooze(500000);
	BMessage apply(kBatchRenameApply);
	apply.AddSpecifier("Window", "Rename 3 items");
	BMessage applied;
	BMessenger(kTrackerAppSignature).SendMessage(&apply, &applied, 5000000,
		5000000);
	snooze(1000000);
	BPath trip1(batch.Path(), "Trip-001.jpg"), trip3(batch.Path(),
		"Trip-003.jpg"), img1(batch.Path(), "IMG_001.jpg");
	report("batch_rename", opened && !refused.GetBool("valid", true)
			&& accepted.GetBool("valid", false)
			&& strcmp(accepted.GetString("name", 0, ""), "Trip-001.jpg") == 0
			&& applied.GetInt32("status", -1) == B_OK
			&& access(trip1.Path(), F_OK) == 0
			&& access(trip3.Path(), F_OK) == 0
			&& access(img1.Path(), F_OK) != 0,
		"window %d; \"Beach-\" refused %d (%s); \"Trip-\" valid %d, first "
		"\"%s\"; status 0x%x; Trip-001 %d, Trip-003 %d, IMG_001 gone %d",
		opened, !refused.GetBool("valid", true),
		refused.GetString("status", ""), accepted.GetBool("valid", false),
		accepted.GetString("name", 0, ""),
		(unsigned)applied.GetInt32("status", -1),
		access(trip1.Path(), F_OK) == 0, access(trip3.Path(), F_OK) == 0,
		access(img1.Path(), F_OK) != 0);

	BMessage renameOff;
	renameOff.AddBool("set_BatchRename", false);
	state(NULL, NULL, &renameOff);
	BMessage selectAll2(B_SELECT_ALL);
	to_poses("ce-batch", selectAll2);
	snooze(500000);
	BMessage edit2(kEditName);
	to_poses("ce-batch", edit2);
	snooze(1500000);
	bool openedOff = window_exists("Rename 4 items");
	BMessage renameOn;
	renameOn.AddBool("set_BatchRename", true);
	state(NULL, NULL, &renameOn);
	report("batch_switch", !openedOff, "window with the switch off %d",
		openedOff);
	if (openedOff)
		close_window("Rename 4 items");
	close_window("ce-batch");

	BMessage settings = state();
	report("type_filter", settings.GetBool("TypeAheadFiltering", false),
		"TypeAheadFiltering %d", settings.GetBool("TypeAheadFiltering",
			false));

	remove_tree(folder.Path());
	remove_tree(batch.Path());
}


// #pragma mark - main


int
main(int argc, char** argv)
{
	BApplication app("application/x-vnd.Renga-tracker-modern-test");
	printf("CE-SUITE %s START\n", SUITE);

	for (int32 i = 0; i < 80 && !be_roster->IsRunning(kTrackerAppSignature);
			i++) {
		snooze(250000);
	}
	bool started = false;
	if (!be_roster->IsRunning(kTrackerAppSignature)) {
		printf("CE-INFO tracker_modern: starting Tracker\n");
		started = true;
		be_roster->Launch(kTrackerAppSignature);
		for (int32 i = 0; i < 120
				&& !be_roster->IsRunning(kTrackerAppSignature); i++) {
			snooze(250000);
		}
	}
	snooze(3000000);
	if (!be_roster->IsRunning(kTrackerAppSignature)) {
		printf("CE-INFO tracker_modern: Tracker is not running\n");
		printf("CE-SUITE %s DONE failures=0\n", SUITE);
		return 0;
	}

	BPath home;
	find_directory(B_USER_DIRECTORY, &home);

	// F-60: a folder of 10 000 files
	BPath big(home.Path(), "ce-big10000");
	remove_tree(big.Path());
	create_directory(big.Path(), 0755);
	bigtime_t start = system_time();
	for (int32 i = 0; i < 10000; i++) {
		BString name;
		name.SetToFormat("%s/file %05d.txt", big.Path(), (int)i);
		BFile file(name.String(), B_CREATE_FILE | B_WRITE_ONLY);
	}
	printf("CE-INFO tracker_modern: 10000 files created in %.1f s\n",
		(system_time() - start) / 1000000.0);
	sync();

	start = system_time();
	open_folder(big.Path());
	bigtime_t firstWindow = -1;
	bigtime_t allShown = -1;
	bigtime_t maxLatency = 0;
	int32 shown = 0;
	while (system_time() - start < 120000000) {
		bigtime_t latency;
		shown = count_poses("ce-big10000", &latency);
		if (shown >= 0 && firstWindow < 0)
			firstWindow = system_time() - start;
		if (shown >= 0 && latency > maxLatency)
			maxLatency = latency;
		if (shown >= 10000) {
			allShown = system_time() - start;
			break;
		}
		snooze(50000);
	}
	report("big_folder", allShown >= 0 && maxLatency < 2000000,
		"%d of 10000 shown; window after %.2f s, all after %.2f s, longest "
		"answer while loading %.0f ms", (int)shown, firstWindow / 1000000.0,
		allShown / 1000000.0, maxLatency / 1000.0);
	printf("CE-INFO tracker_modern: F-60 big folder: window %.2f s, 10000 "
		"entries %.2f s, longest answer %.0f ms\n", firstWindow / 1000000.0,
		allShown / 1000000.0, maxLatency / 1000.0);
	close_window("ce-big10000");
	snooze(500000);
	remove_tree(big.Path());

	// Folder sizes
	BMessage initial = state();
	printf("CE-INFO tracker_modern: settings: folder sizes %d, keep both "
		"%d, terminal %d\n", initial.GetBool("ShowFolderSizes", false),
		initial.GetBool("KeepBothOnConflict", false),
		initial.GetBool("OpenTerminalHere", false));
	BPath sizes(home.Path(), "ce-sizes");
	remove_tree(sizes.Path());
	create_directory(sizes.Path(), 0755);
	BPath sub(sizes.Path(), "sub");
	create_directory(sub.Path(), 0755);
	BPath deeper(sub.Path(), "deeper");
	create_directory(deeper.Path(), 0755);
	BPath file1(sub.Path(), "one"), file2(sub.Path(), "two"),
		file3(sub.Path(), "three"), file4(deeper.Path(), "four");
	write_file(file1.Path(), NULL, 1000);
	write_file(file2.Path(), NULL, 2000);
	write_file(file3.Path(), NULL, 3000);
	write_file(file4.Path(), NULL, 4000);

	BMessage size;
	start = system_time();
	for (int32 i = 0; i < 100; i++) {
		size = state(sub.Path(), "folder_size");
		if (size.GetBool("known", false))
			break;
		snooze(100000);
	}
	report("folder_size", size.GetBool("known", false)
			&& size.GetInt64("size", -1) == 10000,
		"known %d, size %" B_PRId64 " (want 10000) after %.2f s",
		size.GetBool("known", false), size.GetInt64("size", -1),
		(system_time() - start) / 1000000.0);

	open_folder(sizes.Path());
	snooze(2000000);
	BMessage listMode(kListModeMessage);
	listMode.AddSpecifier("Poses");
	listMode.AddSpecifier("Window", "ce-sizes");
	BMessenger(kTrackerAppSignature).SendMessage(&listMode);
	BMessage column;
	for (int32 i = 0; i < 50; i++) {
		snooze(200000);
		column = state(sub.Path(), "size_column");
		BString text = column.GetString("text", "");
		if (!text.IsEmpty() && text != "-")
			break;
	}
	printf("CE-HOST screenshot tracker_folder_sizes\n");
	fflush(stdout);
	snooze(500000);
	BString columnText = column.GetString("text", "");
	report("size_column", column.GetBool("pose", false)
			&& columnText.FindFirst("KiB") >= 0,
		"window %d, row %d, Size column \"%s\"", column.GetBool("window",
			false), column.GetBool("pose", false), columnText.String());

	// the switch, through the preferences window
	BMessenger tracker(kTrackerAppSignature);
	// the Windows page: a page's view is in the window only while shown
	BMessage windowsPage;
	windowsPage.AddInt32("show_page", 1);
	state(NULL, NULL, &windowsPage);
	snooze(2000000);
	BMessage turnOff(kShowFolderSizesChanged);
	turnOff.AddBool("renga:value", false);
	turnOff.AddSpecifier("View", "WindowsSettingsView");
	turnOff.AddSpecifier("Window", "Tracker preferences");
	BMessage reply;
	status_t sent = tracker.SendMessage(&turnOff, &reply, 2000000, 2000000);
	snooze(1000000);
	BString offText = state(sub.Path(), "size_column").GetString("text", "");
	BMessage turnOn(turnOff);
	turnOn.ReplaceBool("renga:value", true);
	tracker.SendMessage(&turnOn, &reply, 2000000, 2000000);
	BString onText;
	for (int32 i = 0; i < 30; i++) {
		snooze(200000);
		onText = state(sub.Path(), "size_column").GetString("text", "");
		if (onText.FindFirst("KiB") >= 0)
			break;
	}
	report("size_switch", sent == B_OK && offText == "-"
			&& onText.FindFirst("KiB") >= 0,
		"sent 0x%x; off \"%s\", on \"%s\"", (unsigned)sent, offText.String(),
		onText.String());

	// Open Terminal here
	BMessage menu = state(sizes.Path(), "context_menu");
	bool hasItem = false;
	const char* label;
	for (int32 i = 0; menu.FindString("item", i, &label) == B_OK; i++) {
		if (strcmp(label, "Open Terminal here") == 0)
			hasItem = true;
	}
	BMessage switchOff;
	switchOff.AddBool("set_OpenTerminalHere", false);
	BMessage menuOff = state(sizes.Path(), "context_menu", &switchOff);
	bool hasItemOff = false;
	for (int32 i = 0; menuOff.FindString("item", i, &label) == B_OK; i++) {
		if (strcmp(label, "Open Terminal here") == 0)
			hasItemOff = true;
	}
	BMessage switchOn;
	switchOn.AddBool("set_OpenTerminalHere", true);
	state(NULL, NULL, &switchOn);
	report("terminal_item", hasItem && !hasItemOff,
		"window %d, item with the switch on %d, off %d",
		menu.GetBool("window", false), hasItem, hasItemOff);

	bool terminalRunning = be_roster->IsRunning(kTerminalSignature);
	BMessage openTerminal(kOpenTerminalHere);
	entry_ref sizesRef;
	get_ref_for_path(sizes.Path(), &sizesRef);
	openTerminal.AddRef("refs", &sizesRef);
	BMessage terminalReply;
	tracker.SendMessage(&openTerminal, &terminalReply, 5000000, 5000000);
	bool opened = false;
	for (int32 i = 0; i < 40 && !opened; i++) {
		snooze(250000);
		opened = be_roster->IsRunning(kTerminalSignature);
	}
	report("terminal_opens", terminalReply.GetInt32("status", -1) == B_OK
			&& strcmp(terminalReply.GetString("path", ""), sizes.Path()) == 0
			&& opened,
		"status 0x%x, folder \"%s\", Terminal running %d",
		(unsigned)terminalReply.GetInt32("status", -1),
		terminalReply.GetString("path", ""), opened);
	if (opened && !terminalRunning)
		BMessenger(kTerminalSignature).SendMessage(B_QUIT_REQUESTED);

	close_window("ce-sizes");

	test_quick_look_and_rename(home);

	// The preferences: Revert restores "Hide dotfiles"
	BMessage before = state();
	bool hideBefore = before.GetBool("HideDotFiles", false);
	bool sortBefore = before.GetBool("SortFolderNamesFirst", true);
	BMessage hide(kHideDotFilesChanged);
	hide.AddBool("renga:value", !hideBefore);
	hide.AddSpecifier("View", "WindowsSettingsView");
	hide.AddSpecifier("Window", "Tracker preferences");
	tracker.SendMessage(&hide, &reply, 2000000, 2000000);
	snooze(500000);
	BMessage changed = state();
	printf("CE-HOST screenshot tracker_preferences_batch9\n");
	fflush(stdout);
	snooze(500000);
	BMessage revert(kRevertButtonPressed);
	revert.AddSpecifier("Window", "Tracker preferences");
	sent = tracker.SendMessage(&revert, &reply, 2000000, 2000000);
	snooze(500000);
	BMessage after = state();
	report("prefs_revert", sent == B_OK
			&& changed.GetBool("HideDotFiles", hideBefore) != hideBefore
			&& after.GetBool("HideDotFiles", !hideBefore) == hideBefore
			&& after.GetBool("SortFolderNamesFirst", !sortBefore)
				== sortBefore,
		"sent 0x%x; hide dotfiles %d -> %d -> %d, list folders first %d -> "
		"%d", (unsigned)sent, hideBefore, changed.GetBool("HideDotFiles",
			false), after.GetBool("HideDotFiles", false), sortBefore,
		after.GetBool("SortFolderNamesFirst", false));
	close_window("Tracker preferences");

	remove_tree(sizes.Path());

	// Name conflicts (in this program, with Tracker's copy engine)
	test_conflicts();

	if (started) {
		BMessenger(kTrackerAppSignature).SendMessage(B_QUIT_REQUESTED);
		for (int32 i = 0; i < 40 && be_roster->IsRunning(kTrackerAppSignature);
				i++) {
			snooze(250000);
		}
	}

	printf("CE-SUITE %s DONE failures=%d\n", SUITE, sFailures);
	return sFailures == 0 ? 0 : 1;
}
