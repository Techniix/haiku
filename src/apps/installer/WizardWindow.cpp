/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 */


#include "WizardWindow.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

#include <Alert.h>
#include <Application.h>
#include <Button.h>
#include <CardLayout.h>
#include <Catalog.h>
#include <CheckBox.h>
#include <ControlLook.h>
#include <DiskDevice.h>
#include <DiskDeviceRoster.h>
#include <Directory.h>
#include <Entry.h>
#include <File.h>
#include <FindDirectory.h>
#include <LayoutBuilder.h>
#include <MenuBar.h>
#include <MenuField.h>
#include <MenuItem.h>
#include <MessageRunner.h>
#include <Partition.h>
#include <Path.h>
#include <PopUpMenu.h>
#include <RadioButton.h>
#include <Roster.h>
#include <ScrollView.h>
#include <SeparatorView.h>
#include <StatusBar.h>
#include <StringView.h>
#include <TextControl.h>
#include <TextView.h>

#include <distro_name.h>

#include <algorithm>

#include "tracker_private.h"

#include "DiskBarView.h"
#include "InstallerDefs.h"
#include "PartitionMenuItem.h"
#include "WorkerThread.h"


#undef B_TRANSLATION_CONTEXT
#define B_TRANSLATION_CONTEXT "InstallerWizard"


static const uint32 kMsgBack = 'wzBk';
static const uint32 kMsgNext = 'wzNx';
static const uint32 kMsgCancel = 'wzCn';
static const uint32 kMsgChoice = 'wzCh';
static const uint32 kMsgDisk = 'wzDk';
static const uint32 kMsgMode = 'wzMd';
static const uint32 kMsgName = 'wzNm';
static const uint32 kMsgDriveSetup = 'wzDS';
static const uint32 kMsgClassic = 'wzCl';
static const uint32 kMsgApplied = 'wzAp';
static const uint32 kMsgAuto = 'wzAu';
static const uint32 kMsgLean = 'wzLn';

static const char* kDriveSetupSignature = "application/x-vnd.Haiku-DriveSetup";
static const char* kPageNames[] = { "welcome", "destination", "summary",
	"progress", "finish" };
static const bigtime_t kAutoDelay = 4000000;


WizardOptions gWizardOptions;


WizardOptions::WizardOptions()
	:
	wizard(false),
	classic(false),
	autoRun(false),
	lean(false)
{
}


// #pragma mark - the setting


static status_t
settings_path(BPath& path)
{
	status_t status = find_directory(B_USER_SETTINGS_DIRECTORY, &path);
	if (status == B_OK)
		status = path.Append("Renga/installer");
	return status;
}


bool
installer_uses_classic_window()
{
	BPath path;
	if (settings_path(path) != B_OK)
		return false;
	FILE* file = fopen(path.Path(), "r");
	if (file == NULL)
		return false;
	bool classic = false;
	char line[256];
	while (fgets(line, sizeof(line), file) != NULL) {
		char value[64];
		if (sscanf(line, "classic_window %63s", value) == 1)
			classic = strcmp(value, "yes") == 0 || strcmp(value, "true") == 0;
	}
	fclose(file);
	return classic;
}


status_t
set_installer_uses_classic_window(bool classic)
{
	BPath path;
	status_t status = settings_path(path);
	BPath parent;
	if (status == B_OK)
		status = path.GetParent(&parent);
	if (status == B_OK)
		status = create_directory(parent.Path(), 0755);
	if (status != B_OK)
		return status;
	FILE* file = fopen(path.Path(), "w");
	if (file == NULL)
		return B_ERROR;
	fprintf(file, "# The Installer's window: the wizard (no) or the classic "
		"window (yes)\nclassic_window %s\n", classic ? "yes" : "no");
	fclose(file);
	return B_OK;
}


bool
launch_installer_tool(const char* signature, const char* fileName)
{
	if (be_roster->Launch(signature) == B_OK)
		return true;
	// Try really hard to launch it: when running from the installation
	// medium the MIME database can be incomplete.
	BPath path;
	if (find_directory(B_SYSTEM_APPS_DIRECTORY, &path) != B_OK
		|| path.Append(fileName) != B_OK) {
		path.SetTo("/boot/system/apps");
		path.Append(fileName);
	}
	entry_ref ref;
	return BEntry(path.Path()).GetRef(&ref) == B_OK
		&& be_roster->Launch(&ref) == B_OK;
}


// #pragma mark - helpers


//!	The narrowest a note gets: about 42 "m"s of the plain font (as wide as
//!	the volume menu at 20 points, so that the pages stay short enough for
//!	a 768-line screen).
static float
note_width()
{
	return ceilf(be_plain_font->StringWidth("m") * 42);
}


//!	How many lines \a text takes in the plain font at \a width, wrapped at
//!	spaces as BTextView wraps it.
static int
wrapped_lines(const char* text, float width)
{
	int lines = 0;
	BString all(text);
	int32 start = 0;
	while (true) {
		int32 end = all.FindFirst('\n', start);
		bool last = end < 0;
		if (last)
			end = all.Length();
		BString paragraph;
		all.CopyInto(paragraph, start, end - start);
		lines++;
		BString line;
		int32 position = 0;
		while (position < paragraph.Length()) {
			int32 space = paragraph.FindFirst(' ', position);
			if (space < 0)
				space = paragraph.Length();
			BString word;
			paragraph.CopyInto(word, position, space - position);
			position = space + 1;
			BString candidate(line);
			if (candidate.Length() > 0)
				candidate << ' ';
			candidate << word;
			if (line.Length() > 0
				&& be_plain_font->StringWidth(candidate) > width) {
				lines++;
				line = word;
			} else
				line = candidate;
		}
		if (last)
			break;
		start = end + 1;
	}
	return lines;
}


/*!	\a text into the note \a view, whose minimum height becomes what the
	text needs at the note's minimum width (at least \a lines lines, and
	one line to spare): the large-font audit measures it that way, and a
	fixed number of lines clipped the Welcome text at 20 points.
*/
static void
set_note(BTextView* view, const char* text, int lines)
{
	view->SetText(text);
	float width = note_width();
	font_height height;
	be_plain_font->GetHeight(&height);
	float lineHeight = ceilf(height.ascent + height.descent + height.leading);
	int needed = wrapped_lines(text, width - 8) + 1;
	view->SetExplicitMinSize(BSize(width,
		lineHeight * std::max(needed, lines) + 4));
	view->InvalidateLayout();
}


/*!	A read-only text view whose height is its explicit minimum (set_note()):
	a word-wrapping BTextView otherwise gives the layout its height for the
	current width, below the minimum the large-font audit checks.
*/
class NoteView : public BTextView {
public:
	NoteView(const char* name)
		:
		BTextView(name)
	{
	}

	virtual bool HasHeightForWidth()
	{
		return false;
	}
};


static BTextView*
make_note(const char* name, const char* text, int lines)
{
	BTextView* view = new NoteView(name);
	view->MakeEditable(false);
	view->MakeSelectable(false);
	view->SetWordWrap(true);
	view->SetStylable(false);
	view->SetViewUIColor(B_PANEL_BACKGROUND_COLOR);
	view->SetLowUIColor(B_PANEL_BACKGROUND_COLOR);
	rgb_color color = ui_color(B_PANEL_TEXT_COLOR);
	view->SetFontAndColor(be_plain_font, B_FONT_ALL, &color);
	set_note(view, text, lines);
	return view;
}


static BString
boot_partition_device(partition_id* _id = NULL)
{
	BDiskDeviceRoster roster;
	BDiskDevice device;
	BPartition* partition;
	if (roster.FindPartitionByMountPoint("/boot", &device, &partition)
			!= B_OK)
		return "";
	if (_id != NULL)
		*_id = partition->ID();
	BPath path;
	device.GetPath(&path);
	return path.Path();
}


// #pragma mark - WizardWindow


WizardWindow::WizardWindow()
	:
	BWindow(BRect(0, 0, 100, 100), B_TRANSLATE_SYSTEM_NAME("Installer"),
		B_TITLED_WINDOW, B_NOT_ZOOMABLE | B_NOT_RESIZABLE
			| B_AUTO_UPDATE_SIZE_LIMITS | B_ASYNCHRONOUS_CONTROLS),
	fPage(-1),
	fChoiceValid(false),
	fWorkerThread(new WorkerThread(this)),
	fCancelSemaphore(-1),
	fInstalling(false),
	fInstalled(false),
	fDriveSetupRunning(false),
	fApplying(false),
	fTarget(-1),
	fFirmware(running_firmware()),
	fAutoRunner(NULL)
{
	if (!be_roster->IsRunning(kTrackerSignature))
		SetWorkspaces(B_ALL_WORKSPACES);

	BMenuBar* menuBar = new BMenuBar("menu");
	BMenu* options = new BMenu(B_TRANSLATE("Options"));
	fClassicItem = new BMenuItem(
		B_TRANSLATE("Use the classic Installer window"),
		new BMessage(kMsgClassic));
	options->AddItem(fClassicItem);
	menuBar->AddItem(options);

	fTitle = new BStringView("title", "");
	BFont font(be_bold_font);
	font.SetSize(font.Size() * 1.25f);
	fTitle->SetFont(&font);
	fStep = new BStringView("step", "");
	fStep->SetAlignment(B_ALIGN_RIGHT);

	fBackButton = new BButton("back", B_TRANSLATE("Back"),
		new BMessage(kMsgBack));
	fNextButton = new BButton("next", B_TRANSLATE("Next"),
		new BMessage(kMsgNext));
	fCancelButton = new BButton("cancel", B_TRANSLATE("Quit"),
		new BMessage(kMsgCancel));

	BView* cards = new BView("pages", 0);
	fCards = new BCardLayout();
	cards->SetLayout(fCards);
	fCards->AddView(_MakeWelcomePage());
	fCards->AddView(_MakeDestinationPage());
	fCards->AddView(_MakeSummaryPage());
	fCards->AddView(_MakeProgressPage());
	fCards->AddView(_MakeFinishPage());

	// The pages are kept short enough for a 768-line screen at 20 points
	// (the large-font audit): notes as wide as the volume menu, the
	// change list scrolls. (Wrapping the pages in a ScreenFit scroller left
	// the notes below their minimum height inside it.)
	BLayoutBuilder::Group<>(this, B_VERTICAL, 0)
		.Add(menuBar)
		.AddGroup(B_VERTICAL, B_USE_SMALL_SPACING)
			.SetInsets(B_USE_WINDOW_SPACING)
			.AddGroup(B_HORIZONTAL)
				.Add(fTitle)
				.AddGlue()
				.Add(fStep)
			.End()
			.Add(new BSeparatorView(B_HORIZONTAL))
			.Add(cards)
			.Add(new BSeparatorView(B_HORIZONTAL))
			.AddGroup(B_HORIZONTAL)
				.Add(fCancelButton)
				.AddGlue()
				.Add(fBackButton)
				.Add(fNextButton)
			.End()
		.End();

	fNextButton->MakeDefault(true);

	_ScanDisks();
	_ScanVolumes();
	_ApplyOptions();

	int32 first = kWelcomePage;
	for (int32 i = 0; i < kPageCount; i++) {
		if (gWizardOptions.page == kPageNames[i])
			first = i;
	}
	if (first == kSummaryPage) {
		_UpdateDestination();
		if (fChoiceValid)
			_FillSummary();
		else
			first = kDestinationPage;
	}
	if (first == kFinishPage) {
		// a preview of the page (screenshots, the large-font audit)
		fSummary.targetName = gWizardOptions.name.Length() > 0
			? gWizardOptions.name : BString(DISTRO_NAME);
		_Finished(true, NULL);
	} else
		_ShowPage(first);

	// at its layout's size before centering, so that it is on screen
	BSize min = GetLayout()->MinSize();
	ResizeTo(min.width, min.height);
	CenterOnScreen();
	MoveOnScreen(B_MOVE_IF_PARTIALLY_OFFSCREEN);
	Show();

	// as the classic window does: watched once the window runs
	be_roster->StartWatching(this, B_REQUEST_LAUNCHED | B_REQUEST_QUIT);
	if (be_roster->IsRunning(kDriveSetupSignature) && Lock()) {
		fDriveSetupRunning = true;
		_UpdateDestination();
		Unlock();
	}
	_Report("wizard started");
}


WizardWindow::~WizardWindow()
{
	be_roster->StopWatching(this);
	delete fAutoRunner;
	_SetCancelSemaphore(-1);
	if (fWorkerThread->Lock())
		fWorkerThread->Quit();
}


BView*
WizardWindow::_MakeWelcomePage()
{
	BString text;
	text.SetToFormat(B_TRANSLATE("This assistant installs %s onto this "
		"computer.\n\nChoose where: a whole disk, the free space beside other "
		"systems, or a volume with an installation. The Summary page then "
		"lists every change; nothing is written until you press Install.\n\n"
		"Back up your files before installing onto a disk that holds data."),
		DISTRO_NAME);
	BTextView* note = make_note("welcome_text", text.String(), 4);

	BButton* classic = new BButton("classic",
		B_TRANSLATE("Use the classic Installer window"),
		new BMessage(kMsgClassic));

	BView* page = new BView("welcome", 0);
	BLayoutBuilder::Group<>(page, B_VERTICAL)
		.Add(note)
		.AddGlue()
		.AddGroup(B_HORIZONTAL)
			.Add(classic)
			.AddGlue()
		.End();
	return page;
}


BView*
WizardWindow::_MakeDestinationPage()
{
	fEraseRadio = new BRadioButton("erase",
		B_TRANSLATE("Erase a disk and install"), new BMessage(kMsgChoice));
	fAlongsideRadio = new BRadioButton("alongside",
		B_TRANSLATE("Install alongside the other systems, in a disk's free "
			"space"), new BMessage(kMsgChoice));
	fExistingRadio = new BRadioButton("existing",
		B_TRANSLATE("Use a volume: upgrade, replace or erase what is on it"),
		new BMessage(kMsgChoice));
	fManualRadio = new BRadioButton("manual",
		B_TRANSLATE("Set up partitions by hand, then choose the volume"),
		new BMessage(kMsgChoice));

	fDiskMenu = new BPopUpMenu(B_TRANSLATE("Choose a disk"));
	fDiskField = new BMenuField("disk", B_TRANSLATE("Disk:"), fDiskMenu);
	fNameControl = new BTextControl("name", B_TRANSLATE("New volume:"),
		"", new BMessage(kMsgName));
	fNameControl->SetModificationMessage(new BMessage(kMsgName));

	fVolumeMenu = new BPopUpMenu(B_TRANSLATE("Choose a volume"));
	fVolumeField = new BMenuField("volume", B_TRANSLATE("Volume:"),
		fVolumeMenu);
	fModeMenu = new BPopUpMenu("mode");
	struct {
		const char*		label;
		install_mode	mode;
	} modes[] = {
		{ B_TRANSLATE("Upgrade, keep my files and applications"),
			INSTALL_MODE_UPGRADE },
		{ B_TRANSLATE("Replace the system, keep my files"),
			INSTALL_MODE_REPLACE },
		{ B_TRANSLATE("Erase the volume and install"), INSTALL_MODE_ERASE },
	};
	for (size_t i = 0; i < sizeof(modes) / sizeof(modes[0]); i++) {
		BMessage* message = new BMessage(kMsgMode);
		message->AddInt32("mode", modes[i].mode);
		fModeMenu->AddItem(new BMenuItem(modes[i].label, message));
	}
	fModeMenu->ItemAt(0)->SetMarked(true);
	fModeField = new BMenuField("mode", B_TRANSLATE("How:"), fModeMenu);

	fDriveSetupButton = new BButton("drivesetup",
		B_TRANSLATE("Open DriveSetup" B_UTF8_ELLIPSIS),
		new BMessage(kMsgDriveSetup));

	fDestinationNote = make_note("destination_note", "", 2);

	float indent = be_control_look->DefaultLabelSpacing() * 3;
	BView* page = new BView("destination", 0);
	BLayoutBuilder::Group<>(page, B_VERTICAL, B_USE_SMALL_SPACING)
		.Add(fEraseRadio)
		.Add(fAlongsideRadio)
		.AddGrid(B_USE_DEFAULT_SPACING, B_USE_SMALL_SPACING)
			.SetInsets(indent, 0, 0, 0)
			.AddMenuField(fDiskField, 0, 0)
			.AddTextControl(fNameControl, 0, 1)
		.End()
		.Add(fExistingRadio)
		.AddGroup(B_HORIZONTAL)
			.Add(fManualRadio)
			.Add(fDriveSetupButton)
			.AddGlue()
		.End()
		.AddGrid(B_USE_DEFAULT_SPACING, B_USE_SMALL_SPACING)
			.SetInsets(indent, 0, 0, 0)
			.AddMenuField(fVolumeField, 0, 0)
			.AddMenuField(fModeField, 0, 1)
		.End()
		.Add(fDestinationNote)
		.AddGlue();
	fEraseRadio->SetValue(B_CONTROL_ON);
	return page;
}


BView*
WizardWindow::_MakeSummaryPage()
{
	BStringView* intro = new BStringView("summary_intro",
		B_TRANSLATE("Nothing has been written yet. Install makes these "
			"changes:"));
	fChangesView = make_note("changes", "", 4);
	fChangesView->MakeSelectable(true);
	fChangesView->SetViewUIColor(B_DOCUMENT_BACKGROUND_COLOR);
	fChangesView->SetLowUIColor(B_DOCUMENT_BACKGROUND_COLOR);
	rgb_color color = ui_color(B_DOCUMENT_TEXT_COLOR);
	fChangesView->SetFontAndColor(be_plain_font, B_FONT_ALL, &color);
	fChangesView->SetInsets(4, 4, 4, 4);
	BScrollView* scroll = new BScrollView("changes_scroll", fChangesView, 0,
		false, true);

	fBeforeBar = new DiskBarView("before");
	fAfterBar = new DiskBarView("after");
	BStringView* beforeLabel = new BStringView("before_label",
		B_TRANSLATE("Now:"));
	fAfterLabel = new BStringView("after_label", B_TRANSLATE("After:"));
	fSummaryNote = make_note("summary_note", "", 1);
	fLeanCheckBox = new BCheckBox("lean", B_TRANSLATE("Lean install: leave "
		"out the development files (headers, compiler)"),
		new BMessage(kMsgLean));

	BView* page = new BView("summary", 0);
	BLayoutBuilder::Group<>(page, B_VERTICAL)
		.Add(intro)
		.Add(scroll)
		.AddGrid(B_USE_DEFAULT_SPACING, B_USE_SMALL_SPACING)
			.Add(beforeLabel, 0, 0)
			.Add(fBeforeBar, 1, 0)
			.Add(fAfterLabel, 0, 1)
			.Add(fAfterBar, 1, 1)
		.End()
		.Add(fSummaryNote)
		.Add(fLeanCheckBox);
	return page;
}


BView*
WizardWindow::_MakeProgressPage()
{
	fProgressText = make_note("progress_text", "", 3);
	fProgressBar = new BStatusBar("progress", B_TRANSLATE("Install progress:  "));
	fProgressBar->SetMaxValue(100.0);
	BView* page = new BView("progress", 0);
	BLayoutBuilder::Group<>(page, B_VERTICAL)
		.Add(fProgressText)
		.Add(fProgressBar)
		.AddGlue();
	return page;
}


BView*
WizardWindow::_MakeFinishPage()
{
	fFinishText = make_note("finish_text", "", 5);
	BView* page = new BView("finish", 0);
	BLayoutBuilder::Group<>(page, B_VERTICAL)
		.Add(fFinishText)
		.AddGlue();
	return page;
}


void
WizardWindow::_Report(const char* format, ...)
{
	if (!gWizardOptions.Given())
		return;
	va_list args;
	va_start(args, format);
	printf("Installer: ");
	vprintf(format, args);
	printf("\n");
	va_end(args);
	fflush(stdout);
}


void
WizardWindow::_ShowPage(int32 page)
{
	static const char* kTitles[] = {
		B_TRANSLATE_MARK("Welcome"),
		B_TRANSLATE_MARK("Where to install"),
		B_TRANSLATE_MARK("Summary"),
		B_TRANSLATE_MARK("Installing"),
		B_TRANSLATE_MARK("Finished"),
	};
	fPage = page;
	fCards->SetVisibleItem(page);
	fTitle->SetText(B_TRANSLATE_NOCOLLECT(kTitles[page]));
	BString step;
	step.SetToFormat(B_TRANSLATE("Step %d of %d"), (int)page + 1,
		(int)kPageCount);
	fStep->SetText(step);

	fBackButton->SetEnabled(page == kDestinationPage
		|| page == kSummaryPage || (page == kFinishPage && !fInstalled));
	fNextButton->SetLabel(page == kSummaryPage ? B_TRANSLATE("Install")
		: B_TRANSLATE("Next"));
	fNextButton->SetEnabled(page != kProgressPage
		&& (page != kDestinationPage || fChoiceValid)
		&& (page != kSummaryPage || !fDriveSetupRunning));
	fCancelButton->SetLabel(page == kProgressPage ? B_TRANSLATE("Stop")
		: B_TRANSLATE("Quit"));
	fCancelButton->SetEnabled(page != kFinishPage);
	fClassicItem->SetEnabled(!fInstalling);
	if (page == kFinishPage) {
		fNextButton->SetLabel(fInstalled
			&& !be_roster->IsRunning(kDeskbarSignature)
				? B_TRANSLATE("Restart") : B_TRANSLATE("Quit"));
	}
	if (page == kDestinationPage)
		_UpdateDestination();

	_Report("wizard page %s", kPageNames[page]);
	if (gWizardOptions.autoRun && page != kProgressPage)
		_ScheduleAuto(kAutoDelay);
}


void
WizardWindow::_ScheduleAuto(bigtime_t delay)
{
	delete fAutoRunner;
	BMessage message(kMsgAuto);
	message.AddInt32("page", fPage);
	fAutoRunner = new BMessageRunner(BMessenger(this), &message, delay, 1);
}


void
WizardWindow::_ApplyOptions()
{
	const WizardOptions& options = gWizardOptions;
	wizard_destination destination = WIZARD_ERASE_DISK;
	if (options.choice.Length() > 0)
		wizard_destination_for_name(options.choice.String(), destination);
	BRadioButton* radios[] = { fEraseRadio, fAlongsideRadio, fExistingRadio,
		fManualRadio };
	radios[destination]->SetValue(B_CONTROL_ON);

	for (int32 i = 0; options.disk.Length() > 0
			&& i < fDiskMenu->CountItems(); i++) {
		BMenuItem* item = fDiskMenu->ItemAt(i);
		const char* path;
		if (item->Message() != NULL
			&& item->Message()->FindString("path", &path) == B_OK
			&& options.disk == path && item->IsEnabled())
			item->SetMarked(true);
	}
	for (int32 i = 0; options.target.Length() > 0
			&& i < fVolumeMenu->CountItems(); i++) {
		PartitionMenuItem* item
			= dynamic_cast<PartitionMenuItem*>(fVolumeMenu->ItemAt(i));
		if (item != NULL && options.target == item->Name()
			&& item->IsEnabled())
			item->SetMarked(true);
	}
	if (options.mode.Length() > 0) {
		install_mode mode = install_mode_for_name(options.mode.String());
		for (int32 i = 0; i < fModeMenu->CountItems(); i++) {
			BMenuItem* item = fModeMenu->ItemAt(i);
			if (item->Message()->GetInt32("mode", -1) == mode)
				item->SetMarked(true);
		}
	}
	if (options.name.Length() > 0)
		fNameControl->SetText(options.name);
	fLeanCheckBox->SetValue(options.lean ? B_CONTROL_ON : B_CONTROL_OFF);
}


void
WizardWindow::_ScanDisks()
{
	BString marked;
	if (BMenuItem* item = fDiskMenu->FindMarked())
		item->Message()->FindString("path", &marked);
	while (BMenuItem* item = fDiskMenu->RemoveItem((int32)0))
		delete item;

	// the disks may have changed: look at their EFI loaders again
	forget_efi_loader_cache();
	std::vector<RengaDisk::DiskInfo> disks;
	RengaDisk::ListDisks(disks);
	for (const RengaDisk::DiskInfo& disk : disks) {
		if (disk.readOnly)
			continue;
		BString label = describe_disk(disk);
		label << " - " << disk.path;
		if (disk.isBootDisk)
			label << " " << B_TRANSLATE("(the running system's disk)");
		BMessage* message = new BMessage(kMsgDisk);
		message->AddString("path", disk.path);
		BMenuItem* item = new BMenuItem(label, message);
		item->SetEnabled(!disk.isBootDisk);
		item->SetMarked(disk.path == marked);
		fDiskMenu->AddItem(item);
	}
	if (fDiskMenu->CountItems() == 0) {
		BMenuItem* none = new BMenuItem(B_TRANSLATE("No disk found"), NULL);
		none->SetEnabled(false);
		fDiskMenu->AddItem(none);
	}
	if (fNameControl->Text()[0] == '\0')
		fNameControl->SetText(unique_volume_name(DISTRO_NAME));
}


void
WizardWindow::_ScanVolumes()
{
	BString marked;
	if (PartitionMenuItem* item
			= dynamic_cast<PartitionMenuItem*>(fVolumeMenu->FindMarked()))
		marked = item->Name();
	while (BMenuItem* item = fVolumeMenu->RemoveItem((int32)0))
		delete item;

	// the classic window's scan (WorkerThread), so that both offer the
	// same volumes with the same names and installations
	BMenu sources("sources");
	BMenu efi("efi");
	fWorkerThread->ScanDisksPartitions(&sources, fVolumeMenu, &efi);
	partition_id boot = -1;
	boot_partition_device(&boot);
	for (int32 i = 0; i < fVolumeMenu->CountItems(); i++) {
		PartitionMenuItem* item
			= dynamic_cast<PartitionMenuItem*>(fVolumeMenu->ItemAt(i));
		if (item == NULL)
			continue;
		item->SetEnabled(item->IsValidTarget() && item->ID() != boot);
		item->SetMarked(item->IsEnabled() && marked == item->Name());
	}
	if (fVolumeMenu->CountItems() == 0) {
		BMenuItem* none = new BMenuItem(B_TRANSLATE("No volume found"), NULL);
		none->SetEnabled(false);
		fVolumeMenu->AddItem(none);
	}
}


bool
WizardWindow::_MakeChoice(WizardChoice& choice)
{
	choice = WizardChoice();
	if (fEraseRadio->Value() == B_CONTROL_ON)
		choice.destination = WIZARD_ERASE_DISK;
	else if (fAlongsideRadio->Value() == B_CONTROL_ON)
		choice.destination = WIZARD_ALONGSIDE;
	else if (fExistingRadio->Value() == B_CONTROL_ON)
		choice.destination = WIZARD_EXISTING;
	else
		choice.destination = WIZARD_MANUAL;

	if (BMenuItem* item = fDiskMenu->FindMarked())
		choice.diskPath = item->Message()->GetString("path", "");
	choice.volumeName = fNameControl->Text();
	if (PartitionMenuItem* item
			= dynamic_cast<PartitionMenuItem*>(fVolumeMenu->FindMarked()))
		choice.target = item->ID();
	if (BMenuItem* item = fModeMenu->FindMarked()) {
		choice.mode = (install_mode)item->Message()->GetInt32("mode",
			INSTALL_MODE_ASK);
	}
	choice.lean = fLeanCheckBox->Value() == B_CONTROL_ON;
	choice.firmware = fFirmware;
	return true;
}


void
WizardWindow::_UpdateDestination()
{
	_MakeChoice(fChoice);
	bool newVolume = fChoice.destination == WIZARD_ERASE_DISK
		|| fChoice.destination == WIZARD_ALONGSIDE;
	fDiskField->SetEnabled(newVolume && !fInstalling);
	fNameControl->SetEnabled(newVolume && !fInstalling);
	fVolumeField->SetEnabled(!newVolume && !fInstalling);
	fModeField->SetEnabled(!newVolume && !fInstalling);
	fDriveSetupButton->SetEnabled(fChoice.destination == WIZARD_MANUAL
		&& !fDriveSetupRunning && !fInstalling);

	BString why;
	if (fDriveSetupRunning) {
		why = B_TRANSLATE("DriveSetup is running. Close it to continue; the "
			"disks and volumes are read again then.");
		fChoiceValid = false;
	} else if (newVolume && fChoice.volumeName.Length() == 0) {
		why = B_TRANSLATE("Name the new volume.");
		fChoiceValid = false;
	} else if (newVolume && volume_name_exists(fChoice.volumeName)) {
		why.SetToFormat(B_TRANSLATE("A volume named \"%s\" exists already: "
			"choose another name."), fChoice.volumeName.String());
		fChoiceValid = false;
	} else {
		WizardSummary summary;
		fChoiceValid = make_wizard_summary(fChoice, summary, why) == B_OK;
		if (fChoiceValid) {
			if (newVolume) {
				why.SetToFormat(B_TRANSLATE("%s: %s"), summary.disk.path.String(),
					describe_disk(summary.disk).String());
			} else
				why = summary.changes.StringAt(0);
			if (summary.warnings.CountStrings() > 0)
				why << "\n" << summary.warnings.StringAt(0);
		}
	}
	set_note(fDestinationNote, why, 2);
	if (fPage == kDestinationPage)
		fNextButton->SetEnabled(fChoiceValid);
}


void
WizardWindow::_FillSummary()
{
	BString why;
	if (make_wizard_summary(fChoice, fSummary, why) != B_OK) {
		fChoiceValid = false;
		set_note(fDestinationNote, why, 2);
		return;
	}
	BString text;
	for (int32 i = 0; i < fSummary.changes.CountStrings(); i++) {
		BString line = fSummary.changes.StringAt(i);
		text << (line.StartsWith("  ") ? "      " : "\xE2\x80\xA2 ")
			<< line.Trim() << "\n";
		_Report("summary: %s", fSummary.changes.StringAt(i).String());
	}
	for (int32 i = 0; i < fSummary.warnings.CountStrings(); i++) {
		text << "\xE2\x80\xA2 " << fSummary.warnings.StringAt(i) << "\n";
		_Report("summary: %s", fSummary.warnings.StringAt(i).String());
	}
	fChangesView->SetText(text);
	fBeforeBar->SetSegments(fSummary.disk.size, fSummary.before);
	fAfterBar->SetSegments(fSummary.disk.size, fSummary.after);
}


bool
WizardWindow::_ConfirmDiskErase()
{
	const RengaDisk::DiskInfo& disk = fSummary.disk;
	if (gWizardOptions.autoRun) {
		bool confirmed = gWizardOptions.confirmErase == disk.path;
		_Report("erasing the disk %s: %s", disk.path.String(), confirmed
			? "confirmed (--confirm-erase)" : "not confirmed (--confirm-erase "
				"must name the disk)");
		return confirmed;
	}
	BStringList volumes;
	disk_volume_descriptions(disk, volumes);
	BString lost = volumes.Join("; ");
	BString text;
	text.SetToFormat(B_TRANSLATE("Erase the whole disk %s?\n\n%s\n\n"
		"Everything on it is lost%s%s. This cannot be undone."),
		disk.path.String(), describe_disk(disk).String(),
		lost.Length() > 0 ? ": " : "", lost.String());
	BAlert* alert = new BAlert(B_TRANSLATE("Erase disk"), text,
		B_TRANSLATE("Cancel"), B_TRANSLATE("Erase disk"), NULL,
		B_WIDTH_AS_USUAL, B_STOP_ALERT);
	alert->SetShortcut(0, B_ESCAPE);
	return alert->Go() == 1;
}


/*!	Whether the disk is still what the summary showed (review of PR #42:
	another stick in the same port, or DriveSetup started from the
	Deskbar, must not get the old plan). If not, the summary is made again
	from the disk as it is, the page says why, and Install has to be
	pressed again; an automated run stops there.
*/
bool
WizardWindow::_SummaryStillValid()
{
	RengaDisk::DiskInfo now;
	BString why;
	if (RengaDisk::GetDisk(fSummary.disk.path.String(), now) != B_OK)
		why = B_TRANSLATE("the disk is gone");
	else if (same_disk_layout(fSummary.disk, now, why))
		return true;

	BString text;
	text.SetToFormat(B_TRANSLATE("The disk changed after this summary was "
		"made: %s. Nothing was written. The summary below is new: check it, "
		"then press Install again."), why.String());
	_Report("the disk changed after the summary: %s", why.String());
	_FillSummary();
	if (!fChoiceValid) {
		text << "\n" << B_TRANSLATE("The choice no longer works: go back to "
			"the previous page.");
		fNextButton->SetEnabled(false);
	}
	set_note(fSummaryNote, text, 1);
	if (gWizardOptions.autoRun) {
		BString failure;
		failure.SetToFormat("the disk changed after the summary (%s)",
			why.String());
		_Finished(false, failure);
	}
	return false;
}


void
WizardWindow::_StartInstallation()
{
	fInstalling = true;
	fInstalled = false;
	fProgressBar->Reset(B_TRANSLATE("Install progress:  "));
	fProgressBar->SetMaxValue(100.0);
	_ShowPage(kProgressPage);

	if (!fSummary.changesTable) {
		// an existing volume: the classic installation's modes
		_StartCopy(fChoice.target, -1);
		return;
	}

	set_note(fProgressText, B_TRANSLATE("Preparing the disk: saving its "
		"partition table, then partitioning and formatting" B_UTF8_ELLIPSIS),
		3);
	_Report("applying the disk plan for %s", fSummary.disk.path.String());
	// until kMsgApplied comes, neither Stop nor Quit: the disk would be
	// left half written (review of PR #42)
	fApplying = true;
	fCancelButton->SetEnabled(false);

	struct Job {
		RengaDisk::Plan	plan;
		BMessenger		target;
	};
	Job* job = new Job{ fSummary.plan, BMessenger(this) };
	thread_id thread = spawn_thread([](void* data) -> status_t {
		Job* job = (Job*)data;
		BMessage message(kMsgApplied);
		BString log;
		BPath backups;
		status_t status = disk_backup_directory(backups);
		BString backup;
		if (status == B_OK)
			status = job->plan.Apply(backups.Path(), log, &backup);
		message.AddInt32("status", status);
		message.AddString("log", log);
		message.AddString("backup", backup);
		message.AddInt64("offset", job->plan.CreatedOffset());
		job->target.SendMessage(&message);
		delete job;
		return B_OK;
	}, "apply disk plan", B_NORMAL_PRIORITY, job);
	status_t started = thread < 0 ? thread : resume_thread(thread);
	if (started != B_OK) {
		// a thread that could not be resumed never ran: it goes, and so
		// does its job
		if (thread >= 0)
			kill_thread(thread);
		delete job;
		fApplying = false;
		fCancelButton->SetEnabled(true);
		_Finished(false, strerror(started));
	}
}


void
WizardWindow::_PlanApplied(BMessage* message)
{
	fApplying = false;
	fCancelButton->SetEnabled(true);
	status_t status = message->GetInt32("status", B_ERROR);
	BString log = message->GetString("log", "");
	fBackupPath = message->GetString("backup", "");
	_Report("disk plan: %s", status == B_OK ? "applied" : strerror(status));
	if (log.Length() > 0)
		_Report("%s", log.String());
	if (status != B_OK) {
		// the step that failed, and how to put the old table back
		BString step;
		int32 start = 0;
		while (start < log.Length()) {
			int32 end = log.FindFirst('\n', start);
			if (end < 0)
				end = log.Length();
			BString line;
			log.CopyInto(line, start, end - start);
			if (line.StartsWith("step ") || line.StartsWith("refused"))
				step = line;
			start = end + 1;
		}
		BString text;
		text.SetToFormat(B_TRANSLATE("the disk could not be prepared (%s)"),
			strerror(status));
		if (step.Length() > 0)
			text << ".\n\n" << B_TRANSLATE("Failed:") << " " << step;
		if (fBackupPath.Length() > 0) {
			BString journal(fBackupPath);
			journal.RemoveLast(".table");
			journal << ".journal";
			BString restore;
			restore.SetToFormat(B_TRANSLATE("The old partition table is saved "
				"in %s (journal: %s). To put it back, run:\nrengadisk restore "
				"%s %s"), fBackupPath.String(), journal.String(),
				fSummary.disk.path.String(), fBackupPath.String());
			text << "\n\n" << restore;
		}
		_Finished(false, text);
		return;
	}

	// the partition the plan made, on that disk at that offset (another
	// volume of the same name elsewhere is never taken)
	partition_id target = created_partition(fSummary.disk.path.String(),
		message->GetInt64("offset", -1));
	partition_id efi = -1;
	if (fSummary.erasesDisk) {
		RengaDisk::DiskInfo disk;
		if (RengaDisk::GetDisk(fSummary.disk.path.String(), disk) == B_OK) {
			for (const RengaDisk::PartitionInfo& partition : disk.partitions) {
				if (partition.type == "EFI system data")
					efi = partition.id;
			}
		}
	}
	if (target < 0) {
		_Finished(false, B_TRANSLATE("the new volume was not found"));
		return;
	}
	_StartCopy(target, efi);
}


void
WizardWindow::_StartCopy(partition_id target, partition_id newEFIPartition)
{
	partition_id source = -1;
	boot_partition_device(&source);
	if (source < 0) {
		_Finished(false, B_TRANSLATE("the running system's volume was not "
			"found"));
		return;
	}
	_SetCancelSemaphore(create_sem(1, "copy engine cancel"));
	fWorkerThread->SetLock(fCancelSemaphore);
	fWorkerThread->SetPackagesList(new BList());
	fWorkerThread->SetSpaceRequired(0);
	fWorkerThread->SetLeanInstall(fChoice.lean);
	fWorkerThread->SetNewEFIPartition(newEFIPartition);
	if (fSummary.changesTable)
		fWorkerThread->SetInstallMode(INSTALL_MODE_ASK);
	else {
		fWorkerThread->SetInstallMode(fChoice.mode,
			gWizardOptions.autoRun ? gWizardOptions.confirmErase.String()
				: NULL);
	}
	// unattended, nothing asks: what an alert would ask is refused
	fWorkerThread->SetUnattended(gWizardOptions.autoRun);
	set_note(fProgressText, B_TRANSLATE("Copying the system" B_UTF8_ELLIPSIS),
		3);
	fTarget = target;
	_Report("installing onto partition %" B_PRId32, target);
	fWorkerThread->StartInstall(source, target);
}


void
WizardWindow::_Finished(bool success, const char* text)
{
	bool wasInstalling = fInstalling;
	fInstalling = false;
	fInstalled = success;
	_SetCancelSemaphore(-1);
	BString message;
	if (success) {
		message.SetToFormat(B_TRANSLATE("%s is installed on \"%s\".\n\n"
			"Remove the installation medium, then restart the computer to "
			"start %s."), DISTRO_NAME, fSummary.targetName.String(),
			DISTRO_NAME);
		if (fChoice.destination == WIZARD_ALONGSIDE) {
			message << "\n\n" << B_TRANSLATE("The other systems on the disk "
				"are still there.");
		}
		if (fSummary.startNote.Length() > 0)
			message << "\n\n" << fSummary.startNote;
	} else {
		message.SetToFormat(B_TRANSLATE("The installation was not completed: "
			"%s\n\nIf the copy had started, its log is on the destination "
			"(system/var/log/installer.log)."), text);
	}
	set_note(fFinishText, message, 5);
	if (wasInstalling || !success)
		_Report("wizard %s%s", success ? "finished" : "failed: ",
			success ? "" : text);
	_ShowPage(kFinishPage);
}


void
WizardWindow::_SetCancelSemaphore(sem_id semaphore, bool alreadyLocked)
{
	if (fCancelSemaphore >= 0) {
		if (!alreadyLocked)
			acquire_sem(fCancelSemaphore);
		delete_sem(fCancelSemaphore);
	}
	fCancelSemaphore = semaphore;
}


void
WizardWindow::_Stop(bool ask)
{
	if (fCancelSemaphore < 0)
		return;
	// hold the copy engine while asking
	acquire_sem(fCancelSemaphore);
	bool stop = true;
	if (ask) {
		BAlert* alert = new BAlert("stop", B_TRANSLATE("Are you sure you want "
			"to stop the installation?"), B_TRANSLATE("Continue"),
			B_TRANSLATE("Stop"), NULL, B_WIDTH_AS_USUAL, B_STOP_ALERT);
		alert->SetShortcut(0, B_ESCAPE);
		stop = alert->Go() == 1;
	}
	if (stop)
		_SetCancelSemaphore(-1, true);
	else
		release_sem(fCancelSemaphore);
}


void
WizardWindow::_Next()
{
	switch (fPage) {
		case kWelcomePage:
			_ShowPage(kDestinationPage);
			break;
		case kDestinationPage:
			_UpdateDestination();
			if (!fChoiceValid) {
				// an automated run cannot choose otherwise: it stops, saying
				// why (the Destination page's note)
				if (gWizardOptions.autoRun)
					_Finished(false, fDestinationNote->Text());
				break;
			}
			_FillSummary();
			set_note(fSummaryNote, "", 1);
			if (fChoiceValid)
				_ShowPage(kSummaryPage);
			break;
		case kSummaryPage:
		{
			if (fDriveSetupRunning)
				break;
			fChoice.lean = fLeanCheckBox->Value() == B_CONTROL_ON;
			// the plan is only good for the disk its summary showed
			if (!_SummaryStillValid())
				break;
			if (fSummary.erasesDisk && !_ConfirmDiskErase()) {
				if (gWizardOptions.autoRun)
					_Finished(false, B_TRANSLATE("erasing the disk was not "
						"confirmed"));
				break;
			}
			// the confirmation can stay open for any time: once more
			if (!_SummaryStillValid())
				break;
			_StartInstallation();
			break;
		}
		case kFinishPage:
			PostMessage(B_QUIT_REQUESTED);
			break;
	}
}


void
WizardWindow::_Back()
{
	switch (fPage) {
		case kDestinationPage:
			_ShowPage(kWelcomePage);
			break;
		case kSummaryPage:
			_ShowPage(kDestinationPage);
			break;
		case kFinishPage:
			if (!fInstalled) {
				_ScanDisks();
				_ScanVolumes();
				_ShowPage(kDestinationPage);
			}
			break;
	}
}


void
WizardWindow::MessageReceived(BMessage* message)
{
	switch (message->what) {
		case kMsgNext:
			_Next();
			break;
		case kMsgBack:
			_Back();
			break;
		case kMsgCancel:
			if (fApplying)
				_RefuseWhileApplying();
			else if (fPage == kProgressPage)
				_Stop(true);
			else
				PostMessage(B_QUIT_REQUESTED);
			break;
		case kMsgAuto:
			if (message->GetInt32("page", -1) != fPage)
				break;
			if (fPage == kFinishPage) {
				_Report("wizard done");
				PostMessage(B_QUIT_REQUESTED);
			} else
				_Next();
			break;
		case kMsgChoice:
		case kMsgDisk:
		case kMsgMode:
		case kMsgName:
		case TARGET_PARTITION:
			_UpdateDestination();
			break;
		case kMsgLean:
			break;
		case kMsgDriveSetup:
			if (!launch_installer_tool(kDriveSetupSignature, "DriveSetup")) {
				BAlert* alert = new BAlert("error", B_TRANSLATE("DriveSetup, "
					"the application to configure disk partitions, could not "
					"be launched."), B_TRANSLATE("OK"));
				alert->SetFlags(alert->Flags() | B_CLOSE_ON_ESCAPE);
				alert->Go();
			}
			break;
		case kMsgClassic:
		{
			if (fInstalling)
				break;
			BMessage request(kMsgSwitchInstallerWindow);
			request.AddBool("wizard", false);
			be_app->PostMessage(&request);
			break;
		}
		case kMsgApplied:
			_PlanApplied(message);
			break;

		case MSG_STATUS_MESSAGE:
		{
			float progress;
			if (message->FindFloat("progress", &progress) == B_OK) {
				const char* item = message->GetString("item", "");
				BString trailing;
				int32 current, maximum;
				if (message->FindInt32("current", &current) == B_OK
					&& message->FindInt32("maximum", &maximum) == B_OK) {
					trailing.SetToFormat(B_TRANSLATE_COMMENT("%1ld of %2ld",
						"number of files copied"), (long)current,
						(long)maximum);
				}
				fProgressBar->SetTo(progress, item, trailing);
			} else {
				const char* status;
				if (message->FindString("status", &status) == B_OK) {
					set_note(fProgressText, status, 3);
					_Report("%s", status);
				}
			}
			break;
		}
		case MSG_INSTALL_FINISHED:
			_KeepBackupOnTarget();
			_Finished(true, NULL);
			break;
		case MSG_RESET:
		{
			// some refusals carry B_OK as their "error"
			status_t error = message->GetInt32("error", B_CANCELED);
			_Finished(false, error != B_OK ? strerror(error)
				: B_TRANSLATE("refused"));
			break;
		}

		case B_SOME_APP_LAUNCHED:
		case B_SOME_APP_QUIT:
		{
			const char* signature;
			if (message->FindString("be:signature", &signature) != B_OK
				|| strcasecmp(signature, kDriveSetupSignature) != 0)
				break;
			fDriveSetupRunning = message->what == B_SOME_APP_LAUNCHED;
			if (!fDriveSetupRunning) {
				_ScanDisks();
				_ScanVolumes();
			}
			_UpdateDestination();
			if (fPage == kSummaryPage) {
				// no Install while DriveSetup can change the disk; the
				// summary is checked again when Install is pressed
				fNextButton->SetEnabled(!fDriveSetupRunning && fChoiceValid);
				set_note(fSummaryNote, fDriveSetupRunning
					? B_TRANSLATE("DriveSetup is running: Install waits until "
						"it is closed.") : "", 1);
			}
			break;
		}

		default:
			BWindow::MessageReceived(message);
	}
}


bool
WizardWindow::QuitRequested()
{
	if (fApplying) {
		_RefuseWhileApplying();
		return false;
	}
	if (fInstalling && !gWizardOptions.autoRun) {
		BAlert* alert = new BAlert(B_TRANSLATE_SYSTEM_NAME("Installer"),
			B_TRANSLATE("The installation is not complete yet!\n"
				"Are you sure you want to stop it?"),
			B_TRANSLATE("Cancel"), B_TRANSLATE("Stop"), NULL,
			B_WIDTH_AS_USUAL, B_STOP_ALERT);
		alert->SetShortcut(0, B_ESCAPE);
		if (alert->Go() == 0)
			return false;
	}
	if (fDriveSetupRunning && !be_roster->IsRunning(kDeskbarSignature)) {
		BAlert* alert = new BAlert(B_TRANSLATE("Quit DriveSetup"),
			B_TRANSLATE("Please close the DriveSetup window before closing "
				"the Installer window."), B_TRANSLATE("OK"));
		alert->SetFlags(alert->Flags() | B_CLOSE_ON_ESCAPE);
		alert->Go();
		return false;
	}
	_Stop(false);

	BMessage quit(B_QUIT_REQUESTED);
	// never restart the computer from an automated run
	quit.AddBool("install_complete", fInstalled && !gWizardOptions.autoRun);
	be_app->PostMessage(&quit);
	return true;
}


void
WizardWindow::_RefuseWhileApplying()
{
	_Report("not stopped: the disk is being prepared");
	if (gWizardOptions.autoRun)
		return;
	BAlert* alert = new BAlert(B_TRANSLATE_SYSTEM_NAME("Installer"),
		B_TRANSLATE("The disk is being partitioned and formatted. Stopping "
			"now would leave it half written: wait until this step is done "
			"(it takes a few seconds), then you can stop."),
		B_TRANSLATE("OK"));
	alert->SetFlags(alert->Flags() | B_CLOSE_ON_ESCAPE);
	alert->Go(NULL);
}


/*!	After a successful installation on a disk the wizard changed: the table
	backup and its journal also go onto the new volume
	(home/config/settings/Renga/disk-backups), since the installation
	medium may be gone when they are needed.
*/
void
WizardWindow::_KeepBackupOnTarget()
{
	if (fBackupPath.Length() == 0 || fTarget < 0)
		return;
	BDiskDeviceRoster roster;
	BDiskDevice device;
	BPartition* partition;
	BPath root;
	if (roster.GetPartitionWithID(fTarget, &device, &partition) != B_OK
		|| (!partition->IsMounted() && partition->Mount() < B_OK)
		|| partition->GetMountPoint(&root) != B_OK) {
		_Report("the table backup was not copied: the new volume is not "
			"mounted");
		return;
	}
	BPath directory(root.Path(), "home/config/settings/Renga/disk-backups");
	create_directory(directory.Path(), 0700);
	BString journal(fBackupPath);
	journal.RemoveLast(".table");
	journal << ".journal";
	const char* files[] = { fBackupPath.String(), journal.String() };
	for (int i = 0; i < 2; i++) {
		BFile source(files[i], B_READ_ONLY);
		BPath leaf(files[i]);
		BPath target(directory.Path(), leaf.Leaf());
		BFile copy(target.Path(), B_WRITE_ONLY | B_CREATE_FILE
			| B_ERASE_FILE);
		status_t status = source.InitCheck();
		if (status == B_OK)
			status = copy.InitCheck();
		char buffer[64 * 1024];
		ssize_t bytes;
		while (status == B_OK
			&& (bytes = source.Read(buffer, sizeof(buffer))) > 0) {
			if (copy.Write(buffer, bytes) != bytes)
				status = B_IO_ERROR;
		}
		_Report("table backup copied to %s: %s", target.Path(),
			strerror(status));
	}
}
