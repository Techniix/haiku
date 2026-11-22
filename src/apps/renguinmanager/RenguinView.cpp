/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 */


#include "RenguinView.h"

#include <Alert.h>
#include <Bitmap.h>
#include <Button.h>
#include <Catalog.h>
#include <CheckBox.h>
#include <Directory.h>
#include <FindDirectory.h>
#include <LayoutBuilder.h>
#include <ListItem.h>
#include <ListView.h>
#include <MessageRunner.h>
#include <NodeInfo.h>
#include <OS.h>
#include <Path.h>
#include <Roster.h>
#include <ScrollView.h>
#include <SeparatorView.h>
#include <StringView.h>
#include <TabView.h>
#include <TextControl.h>

#include <driver_settings.h>
#include <signal.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <string>


#undef B_TRANSLATION_CONTEXT
#define B_TRANSLATION_CONTEXT "RenguinView"


static const char* kRenguinModule
	= "/boot/system/add-ons/kernel/personalities/renguin";
static const char* kLinuxRoot = "/boot/system/non-packaged/compat/linux";

static const uint32 kMsgShell = 'rshl';
static const uint32 kMsgSetup = 'rset';
static const uint32 kMsgUpgrade = 'rupg';
static const uint32 kMsgTrace = 'rtrc';
static const uint32 kMsgDeskbar = 'rdsk';
static const uint32 kMsgRemoveRoot = 'rrmv';
static const uint32 kMsgSwitch = 'rswi';
static const uint32 kMsgProgramSelected = 'rpsl';
static const uint32 kMsgProgramApply = 'rpap';
static const uint32 kMsgProcessesTick = 'rptk';
static const uint32 kMsgEndProcess = 'rpen';


/*!	Renguin's switches (batch 10). Kernel switches live in the renguin
	driver settings (read by the module at every exec of a Linux program),
	the others in ~/config/settings/Renguin/settings (read by linux-apps and
	the linux command). All are on by default.
*/
struct renguin_switch {
	const char*	key;
	bool		kernel;
	const char*	label;
};

static const renguin_switch kSwitches[] = {
	{ "vdso", true, B_TRANSLATE_MARK("Fast clocks for Linux programs "
		"(vDSO: time without a system call)") },
	{ "xattrs", true, B_TRANSLATE_MARK("Haiku file attributes are Linux "
		"extended attributes (user.*)") },
	{ "open_with", false, B_TRANSLATE_MARK("List Linux programs in Tracker's "
		"\"Open with\" for the file types they open") },
	{ "badge", false, B_TRANSLATE_MARK("Show the Renguin badge on Linux "
		"programs' icons (Deskbar, Tracker)") },
	{ "appimage", false, B_TRANSLATE_MARK("Open AppImages with a "
		"double-click") },
	{ "theme", false, B_TRANSLATE_MARK("Give Linux programs Renga's colours, "
		"light or dark look and font (GTK 3/4, Qt 5/6)") },
	// batch 11
	{ "hardlink_copy", true, B_TRANSLATE_MARK("Hard links on BFS are made "
		"as copies (Linux packages need them; BFS has none)") },
	{ "linux_user", true, B_TRANSLATE_MARK("Linux programs run as a normal "
		"user, not as root (\"linux --root\" and \"sudo\" for root)") },
	{ "chromium_flags", false, B_TRANSLATE_MARK("Start Linux Chromium with "
		"the flags Renguin needs (no sandbox, zygote or GPU process yet)") },
	{ "hidpi", false, B_TRANSLATE_MARK("Scale Linux programs like Renga "
		"(HiDPI from the font size: GTK and Qt)") },
	// batch 12
	{ "apk_sync", false, B_TRANSLATE_MARK("Update Deskbar's Applications "
		"as soon as apk installs or removes Linux programs") },
	{ "server_frames", false, B_TRANSLATE_MARK("Give Linux windows Renga's "
		"title bar and borders, also when they would draw their own") },
};
static const int32 kSwitchCount = sizeof(kSwitches) / sizeof(kSwitches[0]);
static_assert(kSwitchCount <= 16, "RenguinView::fSwitches holds 16");


static std::string
read_text(const char* path)
{
	std::string text;
	FILE* file = fopen(path, "r");
	if (file == NULL)
		return text;
	char buffer[4096];
	size_t length;
	while ((length = fread(buffer, 1, sizeof(buffer), file)) > 0)
		text.append(buffer, length);
	fclose(file);
	return text;
}


static BPath
trace_settings_path()
{
	BPath path;
	find_directory(B_USER_SETTINGS_DIRECTORY, &path, true);
	path.Append("kernel/drivers/renguin");
	return path;
}


static BPath
user_settings_path(const char* leaf)
{
	BPath path;
	find_directory(B_USER_SETTINGS_DIRECTORY, &path, true);
	path.Append("Renguin");
	create_directory(path.Path(), 0755);
	if (leaf != NULL)
		path.Append(leaf);
	return path;
}


//!	The program's per-program settings (linux-apps reads them).
static BPath
program_settings_path(const char* id)
{
	BPath path = user_settings_path("programs");
	create_directory(path.Path(), 0755);
	path.Append(id);
	return path;
}


static std::string
run_output(const char* command)
{
	std::string output;
	FILE* pipe = popen(command, "r");
	if (pipe == NULL)
		return output;
	char buffer[4096];
	size_t length;
	while ((length = fread(buffer, 1, sizeof(buffer), pipe)) > 0)
		output.append(buffer, length);
	pclose(pipe);
	return output;
}


//! A list row with an icon (a Linux program, from its Deskbar entry).
class IconItem : public BStringItem {
public:
	IconItem(const char* text, const char* key, BBitmap* icon)
		:
		BStringItem(text),
		fKey(key),
		fIcon(icon)
	{
	}

	~IconItem()
	{
		delete fIcon;
	}

	const char* Key() const
	{
		return fKey.String();
	}

	virtual void DrawItem(BView* owner, BRect frame, bool complete)
	{
		rgb_color background = IsSelected()
			? ui_color(B_LIST_SELECTED_BACKGROUND_COLOR)
			: owner->ViewColor();
		owner->SetLowColor(background);
		owner->FillRect(frame, B_SOLID_LOW);
		float iconSize = frame.Height() - 2;
		if (fIcon != NULL) {
			owner->SetDrawingMode(B_OP_ALPHA);
			owner->DrawBitmap(fIcon, fIcon->Bounds(), BRect(frame.left + 4,
				frame.top + 1, frame.left + 4 + iconSize,
				frame.top + 1 + iconSize));
			owner->SetDrawingMode(B_OP_COPY);
		}
		owner->SetHighColor(IsSelected()
			? ui_color(B_LIST_SELECTED_ITEM_TEXT_COLOR)
			: ui_color(B_LIST_ITEM_TEXT_COLOR));
		font_height height;
		owner->GetFontHeight(&height);
		owner->DrawString(Text(), BPoint(frame.left + iconSize + 12,
			frame.top + (frame.Height() + height.ascent - height.descent) / 2));
	}

	virtual void Update(BView* owner, const BFont* font)
	{
		BStringItem::Update(owner, font);
		SetHeight(std::max(Height(), 20.0f) + 4);
	}

private:
	BString		fKey;
	BBitmap*	fIcon;
};


//!	Runs \a command in a Terminal, so its progress and questions are seen.
static void
in_terminal(const char* title, const char* command)
{
	const char* args[] = { "-t", title, "/bin/sh", "-c", command, NULL };
	be_roster->Launch("application/x-vnd.Haiku-Terminal", 5, (char**)args);
}


RenguinView::RenguinView()
	:
	BGroupView("renguin", B_VERTICAL),
	fProcessRunner(NULL),
	fSwitchCount(0)
{
	fTabs = new BTabView("renguinTabs", B_WIDTH_FROM_LABEL);
	BTab* tab = new BTab();
	fTabs->AddTab(_SystemTab(), tab);
	tab->SetLabel(B_TRANSLATE("Linux system"));
	tab = new BTab();
	fTabs->AddTab(_ProgramsTab(), tab);
	tab->SetLabel(B_TRANSLATE("Programs"));
	tab = new BTab();
	fTabs->AddTab(_RunningTab(), tab);
	tab->SetLabel(B_TRANSLATE("Running"));
	tab = new BTab();
	fTabs->AddTab(_SettingsTab(), tab);
	tab->SetLabel(B_TRANSLATE("Settings"));
	tab = new BTab();
	fTabs->AddTab(_DiagnosticsTab(), tab);
	tab->SetLabel(B_TRANSLATE("Diagnostics"));
	fTabs->SetBorder(B_NO_BORDER);

	BLayoutBuilder::Group<>(this)
		.Add(fTabs);
}


RenguinView::~RenguinView()
{
	delete fProcessRunner;
}


BView*
RenguinView::_SystemTab()
{
	fLayer = new BStringView("layer", "");
	fRoot = new BStringView("root", "");
	fRelease = new BStringView("release", "");
	fPackages = new BStringView("packages", "");
	fShell = new BButton("openShell", B_TRANSLATE("Open the Linux shell"),
		new BMessage(kMsgShell));
	fSetup = new BButton("getRoot", B_TRANSLATE("Download a Linux root"),
		new BMessage(kMsgSetup));
	fUpgrade = new BButton("upgrade", B_TRANSLATE("Update Linux packages"),
		new BMessage(kMsgUpgrade));
	fRemoveRoot = new BButton("removeRoot", B_TRANSLATE("Remove the Linux "
		"root" B_UTF8_ELLIPSIS), new BMessage(kMsgRemoveRoot));
	fDeskbar = new BButton("deskbarEntries", B_TRANSLATE("Update Deskbar "
		"entries"), new BMessage(kMsgDeskbar));

	BGroupView* view = new BGroupView(B_TRANSLATE("Linux system"),
		B_VERTICAL);
	BLayoutBuilder::Group<>(view)
		.SetInsets(B_USE_WINDOW_SPACING)
		.Add(fLayer)
		.Add(fRoot)
		.Add(fRelease)
		.Add(fPackages)
		.AddGroup(B_HORIZONTAL)
			.Add(fShell)
			.Add(fSetup)
			.Add(fUpgrade)
			.AddGlue()
		.End()
		.AddGroup(B_HORIZONTAL)
			.Add(new BStringView("deskbarNote", B_TRANSLATE("Linux programs "
				"appear in the Deskbar under Applications, Linux.")))
			.Add(fDeskbar)
			.AddGlue()
		.End()
		.AddGroup(B_HORIZONTAL)
			.Add(new BStringView("rootsNote", B_TRANSLATE("The Linux root is "
				"Alpine Linux. Debian and Arch roots are planned.")))
			.AddGlue()
			.Add(fRemoveRoot)
		.End()
		.AddGlue();
	return view;
}


BView*
RenguinView::_ProgramsTab()
{
	fPrograms = new BListView("programList");
	fPrograms->SetSelectionMessage(new BMessage(kMsgProgramSelected));
	fProgramHidden = new BCheckBox("programHidden", B_TRANSLATE("Leave out "
		"of the Deskbar"), NULL);
	fProgramOptions = new BTextControl("programOptions",
		B_TRANSLATE("Extra options:"), "", NULL);
	fProgramApply = new BButton("programApply", B_TRANSLATE("Apply"),
		new BMessage(kMsgProgramApply));

	BGroupView* view = new BGroupView(B_TRANSLATE("Programs"), B_VERTICAL);
	BLayoutBuilder::Group<>(view)
		.SetInsets(B_USE_WINDOW_SPACING)
		.Add(new BScrollView("programScroll", fPrograms, 0, false, true))
		.Add(new BStringView("programNote", B_TRANSLATE("For the selected "
			"program (used by its Deskbar entry and \"Open with\"):")))
		.Add(fProgramHidden)
		.AddGroup(B_HORIZONTAL)
			.Add(fProgramOptions)
			.Add(fProgramApply)
		.End();
	return view;
}


BView*
RenguinView::_RunningTab()
{
	fProcesses = new BListView("processList");
	fEndProcess = new BButton("endProcess", B_TRANSLATE("End process"),
		new BMessage(kMsgEndProcess));
	BGroupView* view = new BGroupView(B_TRANSLATE("Running"), B_VERTICAL);
	BLayoutBuilder::Group<>(view)
		.SetInsets(B_USE_WINDOW_SPACING)
		.Add(new BStringView("processNote", B_TRANSLATE("Linux processes "
			"(updated every two seconds):")))
		.Add(new BScrollView("processScroll", fProcesses, 0, false, true))
		.AddGroup(B_HORIZONTAL)
			.AddGlue()
			.Add(fEndProcess)
		.End();
	return view;
}


BView*
RenguinView::_SettingsTab()
{
	BGroupView* view = new BGroupView(B_TRANSLATE("Settings"), B_VERTICAL);
	BLayoutBuilder::Group<> builder(view);
	builder.SetInsets(B_USE_WINDOW_SPACING);
	for (int32 i = 0; i < kSwitchCount; i++) {
		BString name("switch_");
		name << kSwitches[i].key;
		BMessage* message = new BMessage(kMsgSwitch);
		fSwitches[i] = new BCheckBox(name.String(),
			B_TRANSLATE_NOCOLLECT(kSwitches[i].label), message);
		builder.Add(fSwitches[i]);
	}
	fSwitchCount = kSwitchCount;
	builder.Add(new BStringView("switchNote", B_TRANSLATE("Changes apply to "
		"Linux programs started from now on.")));
	builder.AddGlue();
	return view;
}


BView*
RenguinView::_DiagnosticsTab()
{
	fTrace = new BCheckBox("trace", B_TRANSLATE("Trace a Linux program's "
		"processes (for bug reports; written to the system log)"),
		new BMessage(kMsgTrace));
	fTracePattern = new BTextControl("tracePattern",
		B_TRANSLATE("Program:"), "", new BMessage(kMsgTrace));
	fTraceBuffer = new BCheckBox("traceBuffer", B_TRANSLATE("Keep the trace "
		"in memory until the program ends (does not slow it down)"),
		new BMessage(kMsgTrace));
	BGroupView* view = new BGroupView(B_TRANSLATE("Diagnostics"), B_VERTICAL);
	BLayoutBuilder::Group<>(view)
		.SetInsets(B_USE_WINDOW_SPACING)
		.Add(fTrace)
		.AddGroup(B_VERTICAL, 0)
			.SetInsets(B_USE_DEFAULT_SPACING, 0, 0, 0)
			.Add(fTracePattern)
			.Add(fTraceBuffer)
		.End()
		.AddGlue();
	return view;
}


void
RenguinView::AttachedToWindow()
{
	BGroupView::AttachedToWindow();
	BControl* controls[] = { fShell, fSetup, fUpgrade, fRemoveRoot, fDeskbar,
		fTrace, fTracePattern, fTraceBuffer, fProgramApply, fEndProcess };
	for (size_t i = 0; i < sizeof(controls) / sizeof(controls[0]); i++)
		controls[i]->SetTarget(this);
	for (int32 i = 0; i < fSwitchCount; i++)
		fSwitches[i]->SetTarget(this);
	fPrograms->SetTarget(this);
	Refresh();
	_RefreshPrograms();
	_RefreshProcesses();
	BMessage tick(kMsgProcessesTick);
	fProcessRunner = new BMessageRunner(BMessenger(this), &tick, 2000000);
}


void
RenguinView::DetachedFromWindow()
{
	delete fProcessRunner;
	fProcessRunner = NULL;
	BGroupView::DetachedFromWindow();
}


void
RenguinView::MessageReceived(BMessage* message)
{
	switch (message->what) {
		case kMsgShell:
			be_roster->Launch("application/x-vnd.Renga-LinuxShell");
			break;
		case kMsgSetup:
			in_terminal("Linux root", "linux --setup; echo; read -p 'Press "
				"Return to close. '");
			break;
		case kMsgUpgrade:
			in_terminal("Linux packages", "linux apk update && linux apk "
				"upgrade; echo; read -p 'Press Return to close. '");
			break;
		case kMsgRemoveRoot:
		{
			BAlert* alert = new BAlert(B_TRANSLATE("Remove the Linux root"),
				B_TRANSLATE("Remove the Linux root and every Linux program "
					"installed in it? Your files in your home folder stay."),
				B_TRANSLATE("Cancel"), B_TRANSLATE("Remove"), NULL,
				B_WIDTH_AS_USUAL, B_WARNING_ALERT);
			alert->SetShortcut(0, B_ESCAPE);
			if (alert->Go() == 1) {
				BString command;
				command.SetToFormat("rm -rf '%s' && /bin/linux-apps sync",
					kLinuxRoot);
				system(command.String());
				Refresh();
				_RefreshPrograms();
			}
			break;
		}
		case kMsgDeskbar:
			system("/bin/linux-apps sync");
			_RefreshPrograms();
			break;
		case kMsgTrace:
		case kMsgSwitch:
			_SaveSwitches();
			Refresh();
			if (message->what == kMsgSwitch) {
				// badges and Open with live in the Deskbar entries; colours
				// in the theme files
				system("/bin/linux-apps sync >/dev/null 2>&1 &");
				system("/bin/linux-apps theme >/dev/null 2>&1 &");
			}
			break;
		case kMsgProgramSelected:
			_ProgramSelected();
			break;
		case kMsgProgramApply:
			_SaveProgram();
			break;
		case kMsgProcessesTick:
			_RefreshProcesses();
			break;
		case kMsgEndProcess:
		{
			BStringItem* item = dynamic_cast<BStringItem*>(
				fProcesses->ItemAt(fProcesses->CurrentSelection()));
			if (item != NULL) {
				team_id team = atoi(item->Text());
				if (team > 0)
					kill(team, SIGTERM);
			}
			_RefreshProcesses();
			break;
		}
		default:
			BGroupView::MessageReceived(message);
	}
}


void
RenguinView::Refresh()
{
	struct stat st;
	bool layer = stat(kRenguinModule, &st) == 0;
	BString shellPath(kLinuxRoot);
	shellPath << "/bin/sh";
	bool root = stat(shellPath.String(), &st) == 0;

	fLayer->SetText(layer
		? B_TRANSLATE("Linux programs: available (Renguin is installed)")
		: B_TRANSLATE("Linux programs: not available on this system"));
	BString text;
	if (root) {
		text.SetToFormat(B_TRANSLATE("Linux root: %s"), kLinuxRoot);
		fRoot->SetText(text);
		BString releasePath(kLinuxRoot);
		releasePath << "/etc/alpine-release";
		std::string release = read_text(releasePath.String());
		while (!release.empty() && (release.back() == '\n'
				|| release.back() == '\r'))
			release.pop_back();
		text.SetToFormat(B_TRANSLATE("Linux system: Alpine Linux %s"),
			release.empty() ? "?" : release.c_str());
		fRelease->SetText(text);

		BString databasePath(kLinuxRoot);
		databasePath << "/lib/apk/db/installed";
		std::string database = read_text(databasePath.String());
		int32 packages = 0;
		for (size_t i = 0; i + 2 < database.size(); i++) {
			if ((i == 0 || database[i - 1] == '\n') && database[i] == 'P'
				&& database[i + 1] == ':')
				packages++;
		}
		text.SetToFormat(B_TRANSLATE("Linux packages installed: %" B_PRId32),
			packages);
		fPackages->SetText(text);
	} else {
		fRoot->SetText(B_TRANSLATE("Linux root: none yet (Linux programs "
			"need one; it is downloaded once)"));
		fRelease->SetText("");
		fPackages->SetText("");
	}
	fShell->SetEnabled(layer && root);
	fSetup->SetEnabled(layer && !root);
	fUpgrade->SetEnabled(layer && root);
	fDeskbar->SetEnabled(layer && root);

	// the trace settings (PORTING.md, "Diagnosing Linux programs")
	std::string settings = read_text(trace_settings_path().Path());
	size_t position = settings.find("trace_process \"");
	bool tracing = position != std::string::npos;
	if (tracing) {
		size_t start = position + 15;
		size_t end = settings.find('"', start);
		if (fTracePattern->Text() == NULL || fTracePattern->Text()[0] == '\0')
			fTracePattern->SetText(settings.substr(start, end - start).c_str());
	}
	fTrace->SetValue(tracing);
	fTraceBuffer->SetValue(settings.find("trace_buffer true")
		!= std::string::npos);
	fTracePattern->SetEnabled(layer);
	fTraceBuffer->SetEnabled(layer && tracing);
	fTrace->SetEnabled(layer);
	fRemoveRoot->SetEnabled(root);

	for (int32 i = 0; i < fSwitchCount; i++) {
		fSwitches[i]->SetValue(SwitchValue(kSwitches[i].key));
		fSwitches[i]->SetEnabled(layer);
	}
}


/*static*/ bool
RenguinView::SwitchValue(const char* key)
{
	bool kernel = false;
	for (int32 i = 0; i < kSwitchCount; i++) {
		if (strcmp(kSwitches[i].key, key) == 0)
			kernel = kSwitches[i].kernel;
	}
	BPath path = kernel ? trace_settings_path() : user_settings_path("settings");
	void* handle = load_driver_settings(path.Path());
	if (handle == NULL)
		return true;
	bool value = get_driver_boolean_parameter(handle, key, true, true);
	unload_driver_settings(handle);
	return value;
}


/*static*/ void
RenguinView::PrintSwitches()
{
	for (int32 i = 0; i < kSwitchCount; i++) {
		printf("switch\t%s\t%s\n", kSwitches[i].key,
			SwitchValue(kSwitches[i].key) ? "on" : "off");
	}
}


/*!	Writes both settings files: the kernel switches and the diagnostics
	trace in ~/config/settings/kernel/drivers/renguin (Renguin reads it at
	every exec), the others in ~/config/settings/Renguin/settings. Switches
	that are on (the defaults) are not written; a file with nothing in it
	is removed.
*/
void
RenguinView::_SaveSwitches()
{
	BString kernel, user;
	for (int32 i = 0; i < fSwitchCount; i++) {
		if (fSwitches[i]->Value() != 0)
			continue;
		BString line;
		line.SetToFormat("%s false\n", kSwitches[i].key);
		(kSwitches[i].kernel ? kernel : user) << line;
	}
	const char* pattern = fTracePattern->Text();
	if (fTrace->Value() != 0 && pattern != NULL && pattern[0] != '\0') {
		BString escaped(pattern);
		escaped.CharacterEscape("\"\\", '\\');
		kernel << "# Renguin Manager: the diagnostics trace\n"
			<< "trace_process \"" << escaped << "\"\n";
		if (fTraceBuffer->Value() != 0)
			kernel << "trace_buffer true\n";
	}

	struct { BPath path; BString* text; } files[] = {
		{ trace_settings_path(), &kernel },
		{ user_settings_path("settings"), &user }
	};
	for (int i = 0; i < 2; i++) {
		if (files[i].text->IsEmpty()) {
			unlink(files[i].path.Path());
			continue;
		}
		BPath parent;
		files[i].path.GetParent(&parent);
		create_directory(parent.Path(), 0755);
		FILE* file = fopen(files[i].path.Path(), "w");
		if (file == NULL)
			continue;
		fputs("# Renguin settings (Renguin Manager)\n", file);
		fputs(files[i].text->String(), file);
		fclose(file);
	}
}


void
RenguinView::_SaveTrace()
{
	_SaveSwitches();
}


//!	The Linux programs (linux-apps list) with their Deskbar entries' icons.
void
RenguinView::_RefreshPrograms()
{
	while (BListItem* item = fPrograms->RemoveItem((int32)0))
		delete item;
	std::string listing = run_output("/bin/linux-apps list 2>/dev/null");
	BPath menu;
	find_directory(B_USER_DESKBAR_DIRECTORY, &menu);
	menu.Append("Applications/Linux");
	size_t start = 0;
	while (start < listing.size()) {
		size_t end = listing.find('\n', start);
		if (end == std::string::npos)
			end = listing.size();
		std::string line = listing.substr(start, end - start);
		start = end + 1;
		size_t tab = line.find('\t');
		size_t tab2 = tab != std::string::npos ? line.find('\t', tab + 1)
			: std::string::npos;
		if (tab2 == std::string::npos)
			continue;
		std::string id = line.substr(0, tab);
		std::string name = line.substr(tab + 1, tab2 - tab - 1);
		BBitmap* icon = new BBitmap(BRect(0, 0, 31, 31), B_RGBA32);
		BPath entry(menu);
		entry.Append(name.c_str());
		entry_ref ref;
		if (get_ref_for_path(entry.Path(), &ref) != B_OK
			|| BNodeInfo::GetTrackerIcon(&ref, icon, B_LARGE_ICON) != B_OK) {
			delete icon;
			icon = NULL;
		}
		fPrograms->AddItem(new IconItem(name.c_str(), id.c_str(), icon));
	}
	_ProgramSelected();
}


void
RenguinView::_ProgramSelected()
{
	IconItem* item = dynamic_cast<IconItem*>(
		fPrograms->ItemAt(fPrograms->CurrentSelection()));
	fSelectedProgram = item != NULL ? item->Key() : "";
	bool hidden = false;
	BString options;
	if (item != NULL) {
		void* handle = load_driver_settings(
			program_settings_path(item->Key()).Path());
		if (handle != NULL) {
			hidden = get_driver_boolean_parameter(handle, "hidden", false,
				true);
			options = get_driver_parameter(handle, "options", "", "");
			unload_driver_settings(handle);
		}
	}
	fProgramHidden->SetValue(hidden);
	fProgramOptions->SetText(options.String());
	fProgramHidden->SetEnabled(item != NULL);
	fProgramOptions->SetEnabled(item != NULL);
	fProgramApply->SetEnabled(item != NULL);
}


void
RenguinView::_SaveProgram()
{
	if (fSelectedProgram.IsEmpty())
		return;
	BPath path = program_settings_path(fSelectedProgram.String());
	BString options(fProgramOptions->Text());
	if (fProgramHidden->Value() == 0 && options.IsEmpty())
		unlink(path.Path());
	else {
		FILE* file = fopen(path.Path(), "w");
		if (file != NULL) {
			options.CharacterEscape("\"\\", '\\');
			fprintf(file, "# Renguin Manager: settings of this Linux program\n"
				"hidden %s\noptions \"%s\"\n",
				fProgramHidden->Value() != 0 ? "true" : "false",
				options.String());
			fclose(file);
		}
	}
	system("/bin/linux-apps sync >/dev/null 2>&1 &");
}


/*!	The Linux processes: teams with an image from the Linux root (their
	program, or the C library's loader for a dynamic program).
*/
void
RenguinView::_RefreshProcesses()
{
	BString selected;
	if (BStringItem* item = dynamic_cast<BStringItem*>(
			fProcesses->ItemAt(fProcesses->CurrentSelection()))) {
		selected = item->Text();
	}
	while (BListItem* item = fProcesses->RemoveItem((int32)0))
		delete item;
	int32 cookie = 0;
	team_info team;
	while (get_next_team_info(&cookie, &team) == B_OK) {
		int32 imageCookie = 0;
		image_info image;
		bool isLinux = false;
		BString program;
		while (get_next_image_info(team.team, &imageCookie, &image) == B_OK) {
			if (strncmp(image.name, kLinuxRoot, strlen(kLinuxRoot)) == 0)
				isLinux = true;
			if (image.type == B_APP_IMAGE)
				program = image.name;
		}
		if (!isLinux)
			continue;
		if (program.StartsWith(kLinuxRoot))
			program.Remove(0, strlen(kLinuxRoot));
		BString text;
		text.SetToFormat("%" B_PRId32 "\t%s", team.team, program.String());
		BStringItem* item = new BStringItem(text.String());
		fProcesses->AddItem(item);
		if (text == selected)
			fProcesses->Select(fProcesses->CountItems() - 1);
	}
	fEndProcess->SetEnabled(fProcesses->CurrentSelection() >= 0);
}
