/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 */

/*!	System logs (Renga, roadmap F-51).

	One window with three tabs:

	- "This boot": the system log (/boot/system/var/log/syslog) from the
	  start of the current boot on, updated live, with filters by component
	  (the program, or the kernel driver or module that wrote the line), by
	  level and a search.
	- "Previous boot": the same for the boot before. Boots are told apart by
	  the kernel's "Welcome to syslog debug output!" line, which starts the
	  log of every boot; syslog.1 (syslog_daemon's rotated copy) is read
	  before syslog. When the previous boot is no longer in them, the
	  kernel's own log of the previous session (/var/log/previous_syslog,
	  kept in memory across the reboot by the boot loader) is shown.
	- "Crash reports": the kernel crash reports crash_collector saved and
	  the application crash reports debug_server wrote, in
	  /boot/system/var/log/crash, newest first, with a viewer.

	"Save a report" packs the logs, the crash reports and hardware
	information (listdev, listusb, sysinfo) into one ZIP archive on the
	Desktop, for a bug report.

	syslog_daemon does not write a message's priority into the file, so the
	level filter goes by the words in a line (error, fail, panic, ... for
	errors; warn for warnings); the menu says so.

	  SystemLogs                        the window
	  SystemLogs <crash report>         the window, showing that report
	  SystemLogs --notify               at login (data/launch/
	                                    user_crash_reports): a notification
	                                    for each new kernel crash report
	  SystemLogs --save-report [<path>] write the archive without a window

	The application accepts the messages below from other programs; the GUI
	test uses them.
*/


#include <Alert.h>
#include <Application.h>
#include <Button.h>
#include <Catalog.h>
#include <Directory.h>
#include <Entry.h>
#include <File.h>
#include <FindDirectory.h>
#include <LayoutBuilder.h>
#include <ListView.h>
#include <MenuField.h>
#include <MenuItem.h>
#include <MessageRunner.h>
#include <Node.h>
#include <Notification.h>
#include <Path.h>
#include <PopUpMenu.h>
#include <Roster.h>
#include <Screen.h>
#include <ScrollBar.h>
#include <ScrollView.h>
#include <SplitView.h>
#include <String.h>
#include <StringView.h>
#include <TabView.h>
#include <TextControl.h>
#include <TextView.h>
#include <Window.h>
#include <fs_attr.h>

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <map>
#include <string>
#include <vector>

#include <crash_report_format.h>

#include "ReportArchive.h"


#undef B_TRANSLATION_CONTEXT
#define B_TRANSLATION_CONTEXT "SystemLogs"


static const char* kSignature = "application/x-vnd.Renga-SystemLogs";
static const char* kHelperSignature = "application/x-vnd.Renga-SystemLogs-cli";
static const char* kBootMarker = "Welcome to syslog debug output!";

enum {
	// from the controls (and other programs)
	kMsgSetFilter		= 'slft',
		// "component" (string, "" for all), "level" (int32 0-2),
		// "search" (string); missing fields stay as they are
	kMsgSelectTab		= 'sltb',	// "tab": int32 0-2
	kMsgSelectCrash		= 'slsc',	// "path": string
	kMsgSaveReport		= 'slsr',	// "path": string, optional; replies
	kMsgGetState		= 'slst',	// replies with the state, see _State()

	// internal
	kMsgPoll			= 'slpl',
	kMsgComponent		= 'slcm',
	kMsgLevel			= 'sllv',
	kMsgSearch			= 'slse',
	kMsgCrashSelected	= 'slcs',
};

enum {
	kLevelAll = 0,
	kLevelWarnings = 1,		// warnings and errors
	kLevelErrors = 2
};

static const int32 kMaxShownLines = 20000;


// #pragma mark - log lines


struct LogLine {
	std::string	text;
	std::string	component;
	int			level;		// 0 info, 1 warning, 2 error
};


static bool
contains_word(const std::string& lower, const char* word)
{
	return lower.find(word) != std::string::npos;
}


static int
guess_level(const std::string& text)
{
	std::string lower(text);
	for (size_t i = 0; i < lower.size(); i++)
		lower[i] = tolower((unsigned char)lower[i]);
	if (contains_word(lower, "panic") || contains_word(lower, "error")
		|| contains_word(lower, "fail") || contains_word(lower, "fatal")
		|| contains_word(lower, "crash") || contains_word(lower, "cannot")
		|| contains_word(lower, "can't") || contains_word(lower, "unable to")
		|| contains_word(lower, "fault")) {
		return 2;
	}
	if (contains_word(lower, "warn"))
		return 1;
	return 0;
}


/*!	syslog_daemon writes "<FACILITY>[ '<ident>'][[<pid>]]: <message>". The
	component is the ident; for kernel lines ("KERN: ") the driver or module
	name most kernel messages start with ("usb_hid: ..."), or "kernel".
*/
static std::string
component_of(const std::string& line)
{
	size_t colon = line.find(": ");
	if (colon == std::string::npos || colon > 80)
		return "other";
	std::string header = line.substr(0, colon);
	size_t quote = header.find('\'');
	if (quote != std::string::npos) {
		size_t end = header.find('\'', quote + 1);
		if (end != std::string::npos)
			return header.substr(quote + 1, end - quote - 1);
	}
	if (header == "KERN") {
		std::string rest = line.substr(colon + 2);
		size_t inner = rest.find(':');
		if (inner != std::string::npos && inner > 0 && inner <= 32) {
			std::string word = rest.substr(0, inner);
			bool plain = true;
			for (size_t i = 0; i < word.size(); i++) {
				char c = word[i];
				if (!isalnum((unsigned char)c) && c != '_' && c != '-'
					&& c != '.' && c != '/') {
					plain = false;
					break;
				}
			}
			if (plain)
				return word;
		}
		return "kernel";
	}
	for (size_t i = 0; i < header.size(); i++)
		header[i] = tolower((unsigned char)header[i]);
	return header;
}


static void
parse_lines(const std::string& text, std::vector<LogLine>& lines)
{
	size_t start = 0;
	while (start < text.size()) {
		size_t end = text.find('\n', start);
		if (end == std::string::npos)
			end = text.size();
		LogLine line;
		// without the colour escapes (ESC [ ... m) some drivers print
		line.text.reserve(end - start);
		for (size_t i = start; i < end; i++) {
			if (text[i] == '\x1b' && i + 1 < end && text[i + 1] == '[') {
				size_t j = i + 2;
				while (j < end && (isdigit((unsigned char)text[j])
						|| text[j] == ';'))
					j++;
				if (j < end && text[j] == 'm') {
					i = j;
					continue;
				}
			}
			line.text += text[i];
		}
		if (!line.text.empty() && line.text[line.text.size() - 1] == '\r')
			line.text.erase(line.text.size() - 1);
		line.component = component_of(line.text);
		line.level = guess_level(line.text);
		lines.push_back(line);
		start = end + 1;
	}
}


static bool
read_file(const char* path, std::string& contents, off_t from = 0)
{
	FILE* file = fopen(path, "rb");
	if (file == NULL)
		return false;
	if (from > 0)
		fseeko(file, from, SEEK_SET);
	char buffer[65536];
	size_t bytesRead;
	while ((bytesRead = fread(buffer, 1, sizeof(buffer), file)) > 0)
		contents.append(buffer, bytesRead);
	fclose(file);
	return true;
}


static BString
log_path(const char* name)
{
	BPath path;
	if (find_directory(B_SYSTEM_LOG_DIRECTORY, &path) != B_OK)
		path.SetTo("/boot/system/var/log");
	path.Append(name);
	return BString(path.Path());
}


/*!	Splits syslog.1 + syslog at the boot markers. \a _current gets the
	current boot (from the last marker), \a _previous the boot before it, if
	its start is still there. Returns the length of the syslog file itself,
	for the live updates.
*/
static off_t
read_boots(std::string& _current, std::string& _previous,
	bool& _previousFound)
{
	std::string text;
	read_file(log_path("syslog.1").String(), text);
	std::string current;
	read_file(log_path("syslog").String(), current);
	off_t syslogSize = current.size();
	text += current;

	std::vector<size_t> starts;
	size_t position = 0;
	while ((position = text.find(kBootMarker, position)) != std::string::npos) {
		size_t lineStart = text.rfind('\n', position);
		starts.push_back(lineStart == std::string::npos ? 0 : lineStart + 1);
		position++;
	}

	_previousFound = false;
	if (starts.empty()) {
		_current = text;
		_previous.clear();
	} else {
		_current = text.substr(starts.back());
		if (starts.size() >= 2) {
			_previous = text.substr(starts[starts.size() - 2],
				starts.back() - starts[starts.size() - 2]);
			_previousFound = true;
		} else
			_previous.clear();
	}
	return syslogSize;
}


// #pragma mark - LogView


class LogView : public BView {
public:
	LogView(const char* name)
		:
		BView(name, 0),
		fComponent(""),
		fLevel(kLevelAll),
		fShown(0)
	{
		fComponentMenu = new BPopUpMenu(B_TRANSLATE("All components"));
		fComponentField = new BMenuField("component",
			B_TRANSLATE("Component:"), fComponentMenu);

		BPopUpMenu* levelMenu = new BPopUpMenu("level");
		const char* levels[] = {
			B_TRANSLATE("All messages"),
			B_TRANSLATE("Warnings and errors"),
			B_TRANSLATE("Errors"),
			NULL
		};
		for (int32 i = 0; levels[i] != NULL; i++) {
			BMessage* message = new BMessage(kMsgLevel);
			message->AddInt32("level", i);
			BMenuItem* item = new BMenuItem(levels[i], message);
			levelMenu->AddItem(item);
			if (i == 0)
				item->SetMarked(true);
		}
		fLevelField = new BMenuField("level", B_TRANSLATE("Level:"),
			levelMenu);
		fLevelField->SetToolTip(B_TRANSLATE("The log does not record a "
			"level: it is guessed from the words in each line."));

		fSearch = new BTextControl("search", B_TRANSLATE("Search:"), "",
			new BMessage(kMsgSearch));
		fSearch->SetModificationMessage(new BMessage(kMsgSearch));

		fText = new BTextView("text");
		fText->MakeEditable(false);
		fText->SetStylable(false);
		fText->SetWordWrap(false);
		BFont font(be_fixed_font);
		fText->SetFontAndColor(&font);
		fScroll = new BScrollView("scroll", fText, 0, true, true);
		// the text's own width must not set the window's
		fScroll->SetExplicitMinSize(BSize(200, 100));
		fScroll->SetExplicitMaxSize(BSize(B_SIZE_UNLIMITED,
			B_SIZE_UNLIMITED));
		fComponentField->SetExplicitMaxSize(BSize(260, B_SIZE_UNSET));

		fStatus = new BStringView("status", "");
		fStatus->SetExplicitMinSize(BSize(100, B_SIZE_UNSET));

		BLayoutBuilder::Group<>(this, B_VERTICAL, B_USE_HALF_ITEM_SPACING)
			.SetInsets(B_USE_HALF_ITEM_INSETS)
			.AddGroup(B_HORIZONTAL)
				.Add(fComponentField)
				.Add(fLevelField)
				.Add(fSearch)
			.End()
			.Add(fScroll)
			.Add(fStatus);
	}

	virtual void AttachedToWindow()
	{
		BView::AttachedToWindow();
		fLevelField->Menu()->SetTargetForItems(this);
		fSearch->SetTarget(this);
		_UpdateComponentMenu();
	}

	virtual void MessageReceived(BMessage* message)
	{
		switch (message->what) {
			case kMsgComponent:
				fComponent = message->GetString("component", "");
				_Refilter();
				break;
			case kMsgLevel:
				fLevel = message->GetInt32("level", kLevelAll);
				_Refilter();
				break;
			case kMsgSearch:
				fSearchText = fSearch->Text();
				_Refilter();
				break;
			default:
				BView::MessageReceived(message);
		}
	}

	void SetText(const std::string& text, const char* note = NULL)
	{
		fLines.clear();
		parse_lines(text, fLines);
		fNote = note != NULL ? note : "";
		_UpdateComponentMenu();
		_Refilter();
	}

	void AppendText(const std::string& text)
	{
		size_t first = fLines.size();
		parse_lines(text, fLines);
		bool newComponent = false;
		std::string shown;
		for (size_t i = first; i < fLines.size(); i++) {
			if (fComponents.find(fLines[i].component) == fComponents.end())
				newComponent = true;
			fComponents[fLines[i].component]++;
			if (_Matches(fLines[i])) {
				shown += fLines[i].text + "\n";
				fShown++;
			}
		}
		if (newComponent)
			_UpdateComponentMenu();
		if (!shown.empty()) {
			BScrollBar* bar = fScroll->ScrollBar(B_VERTICAL);
			float minimum = 0;
			float maximum = 0;
			if (bar != NULL)
				bar->GetRange(&minimum, &maximum);
			bool atEnd = bar == NULL || bar->Value() >= maximum - 1;
			fText->Insert(fText->TextLength(), shown.c_str(), shown.size());
			if (atEnd)
				fText->ScrollToOffset(fText->TextLength());
		}
		_UpdateStatus();
	}

	void SetFilter(const BMessage* message)
	{
		BString component;
		if (message->FindString("component", &component) == B_OK) {
			fComponent = component.String();
			_MarkComponent();
		}
		int32 level;
		if (message->FindInt32("level", &level) == B_OK) {
			fLevel = std::max((int32)0, std::min(level, (int32)2));
			if (BMenuItem* item = fLevelField->Menu()->ItemAt(fLevel))
				item->SetMarked(true);
		}
		BString search;
		if (message->FindString("search", &search) == B_OK) {
			fSearchText = search;
			fSearch->SetModificationMessage(NULL);
			fSearch->SetText(search);
			fSearch->SetModificationMessage(new BMessage(kMsgSearch));
		}
		_Refilter();
	}

	int32 CountLines() const { return fLines.size(); }
	int32 CountShown() const { return fShown; }
	int32 CountComponents() const { return fComponents.size(); }
	int32 TextLength() const { return fText->TextLength(); }

private:
	bool _Matches(const LogLine& line) const
	{
		if (!fComponent.empty() && line.component != fComponent)
			return false;
		if (fLevel == kLevelWarnings && line.level < 1)
			return false;
		if (fLevel == kLevelErrors && line.level < 2)
			return false;
		if (fSearchText.Length() > 0
			&& BString(line.text.c_str()).IFindFirst(fSearchText) < 0) {
			return false;
		}
		return true;
	}

	void _Refilter()
	{
		std::vector<size_t> matching;
		for (size_t i = 0; i < fLines.size(); i++) {
			if (_Matches(fLines[i]))
				matching.push_back(i);
		}
		fShown = matching.size();
		size_t first = matching.size() > (size_t)kMaxShownLines
			? matching.size() - kMaxShownLines : 0;
		std::string text;
		for (size_t i = first; i < matching.size(); i++)
			text += fLines[matching[i]].text + "\n";
		fText->SetText(text.c_str(), text.size());
		fText->ScrollToOffset(fText->TextLength());
		_UpdateStatus();
	}

	void _UpdateComponentMenu()
	{
		fComponents.clear();
		for (size_t i = 0; i < fLines.size(); i++)
			fComponents[fLines[i].component]++;

		while (BMenuItem* item = fComponentMenu->RemoveItem((int32)0))
			delete item;
		BMessage* all = new BMessage(kMsgComponent);
		all->AddString("component", "");
		fComponentMenu->AddItem(new BMenuItem(B_TRANSLATE("All components"),
			all));
		fComponentMenu->AddSeparatorItem();
		for (std::map<std::string, int32>::iterator it = fComponents.begin();
				it != fComponents.end(); it++) {
			BMessage* message = new BMessage(kMsgComponent);
			message->AddString("component", it->first.c_str());
			BString label;
			label.SetToFormat("%s (%" B_PRId32 ")", it->first.c_str(),
				it->second);
			fComponentMenu->AddItem(new BMenuItem(label, message));
		}
		if (Window() != NULL)
			fComponentMenu->SetTargetForItems(this);
		_MarkComponent();
	}

	void _MarkComponent()
	{
		for (int32 i = 0; BMenuItem* item = fComponentMenu->ItemAt(i); i++) {
			if (item->Message() == NULL)
				continue;
			if (fComponent == item->Message()->GetString("component", "")) {
				item->SetMarked(true);
				return;
			}
		}
	}

	void _UpdateStatus()
	{
		BString status;
		if (fShown > kMaxShownLines) {
			status.SetToFormat(B_TRANSLATE("Showing the last %" B_PRId32
				" of %" B_PRId32 " matching lines (%" B_PRId32 " in all)"),
				kMaxShownLines, fShown, (int32)fLines.size());
		} else {
			status.SetToFormat(B_TRANSLATE("%" B_PRId32 " of %" B_PRId32
				" lines"), fShown, (int32)fLines.size());
		}
		if (!fNote.empty())
			status << "  -  " << fNote.c_str();
		fStatus->SetText(status);
	}

	BPopUpMenu*		fComponentMenu;
	BMenuField*		fComponentField;
	BMenuField*		fLevelField;
	BTextControl*	fSearch;
	BTextView*		fText;
	BScrollView*	fScroll;
	BStringView*	fStatus;

	std::vector<LogLine>		fLines;
	std::map<std::string, int32> fComponents;
	std::string		fComponent;
	int32			fLevel;
	BString			fSearchText;
	int32			fShown;
	std::string		fNote;
};


// #pragma mark - crash reports


class CrashItem : public BStringItem {
public:
	CrashItem(const char* label, const char* path, time_t when)
		:
		BStringItem(label),
		fPath(path),
		fWhen(when)
	{
	}

	const BString& Path() const { return fPath; }
	time_t When() const { return fWhen; }

private:
	BString	fPath;
	time_t	fWhen;
};


static std::string
report_field(const std::string& text, const char* name)
{
	std::string key = std::string("\n") + name + ": ";
	size_t start = text.find(key);
	if (start == std::string::npos)
		return "";
	start += key.size();
	size_t end = text.find('\n', start);
	return text.substr(start, end == std::string::npos ? std::string::npos
		: end - start);
}


static BString
crash_label(const char* leaf, const std::string& text)
{
	BString label;
	if (strncmp(leaf, "kernel-", 7) == 0) {
		std::string when = report_field(text, "crash time");
		std::string reason = report_field(text, "reason");
		std::string message = report_field(text, "message");
		label.SetToFormat(B_TRANSLATE("Kernel crash, %s: %s - %s"),
			when.c_str(), reason.c_str(), message.c_str());
	} else if (strncmp(leaf, "app-", 4) == 0) {
		std::string when = report_field(text, "time");
		std::string program = report_field(text, "program");
		std::string reason = report_field(text, "reason");
		BPath path(program.c_str());
		label.SetToFormat(B_TRANSLATE("Application crash, %s: %s - %s"),
			when.c_str(), path.Leaf() != NULL ? path.Leaf() : program.c_str(),
			reason.c_str());
	} else
		label = leaf;
	return label;
}


static bool
compare_crash_items(const CrashItem* a, const CrashItem* b)
{
	return a->When() > b->When();
}


// #pragma mark - window


class SystemLogsWindow : public BWindow {
public:
	SystemLogsWindow()
		:
		BWindow(BRect(80, 80, 980, 680), B_TRANSLATE_SYSTEM_NAME("System logs"),
			B_TITLED_WINDOW, B_QUIT_ON_WINDOW_CLOSE),
		fLiveOffset(0),
		fCrashDirectoryTime(-1),
		fPollRunner(NULL)
	{
		fTabView = new BTabView("tabs", B_WIDTH_FROM_LABEL);

		fLive = new LogView(B_TRANSLATE("This boot"));
		fPrevious = new LogView(B_TRANSLATE("Previous boot"));

		fCrashList = new BListView("crash list");
		fCrashList->SetSelectionMessage(new BMessage(kMsgCrashSelected));
		fCrashText = new BTextView("crash text");
		fCrashText->MakeEditable(false);
		fCrashText->SetWordWrap(false);
		BFont font(be_fixed_font);
		fCrashText->SetFontAndColor(&font);
		fCrashEmpty = new BStringView("crash status", "");

		BScrollView* listScroll = new BScrollView("crash list scroll",
			fCrashList, 0, false, true);
		listScroll->SetExplicitMinSize(BSize(200, 60));
		BScrollView* textScroll = new BScrollView("crash text scroll",
			fCrashText, 0, true, true);
		textScroll->SetExplicitMinSize(BSize(200, 100));
		textScroll->SetExplicitMaxSize(BSize(B_SIZE_UNLIMITED,
			B_SIZE_UNLIMITED));
		fCrashEmpty->SetExplicitMinSize(BSize(100, B_SIZE_UNSET));

		BView* crashes = new BView(B_TRANSLATE("Crash reports"), 0);
		BSplitView* split;
		BLayoutBuilder::Group<>(crashes, B_VERTICAL, B_USE_HALF_ITEM_SPACING)
			.SetInsets(B_USE_HALF_ITEM_INSETS)
			.AddSplit(B_VERTICAL, B_USE_HALF_ITEM_SPACING, 1.0f)
				.GetSplitView(&split)
				.Add(listScroll, 0.25f)
				.Add(textScroll, 0.75f)
			.End()
			.Add(fCrashEmpty);
		split->SetCollapsible(false);

		fTabView->AddTab(fLive);
		fTabView->AddTab(fPrevious);
		fTabView->AddTab(crashes);

		fSaveButton = new BButton("save report",
			B_TRANSLATE("Save a report" B_UTF8_ELLIPSIS),
			new BMessage(kMsgSaveReport));
		fSaveButton->SetToolTip(B_TRANSLATE("Packs the logs, the crash "
			"reports and hardware information into one archive on the "
			"Desktop, to attach to a bug report."));
		fSaveStatus = new BStringView("save status", "");

		BLayoutBuilder::Group<>(this, B_VERTICAL, 0)
			.Add(fTabView)
			.AddGroup(B_HORIZONTAL)
				.SetInsets(B_USE_WINDOW_INSETS, B_USE_HALF_ITEM_INSETS,
					B_USE_WINDOW_INSETS, B_USE_WINDOW_INSETS)
				.Add(fSaveStatus)
				.AddGlue()
				.Add(fSaveButton)
			.End();

		fSaveStatus->SetExplicitMinSize(BSize(100, B_SIZE_UNSET));

		_LoadLogs();
		_LoadCrashes();

		// Fits the screen: at most 900 x 600, never bigger than the screen.
		BRect screen = BScreen(this).Frame();
		ResizeTo(std::min(900.0f, screen.Width() - 120),
			std::min(600.0f, screen.Height() - 120));
		CenterOnScreen();

		BMessage poll(kMsgPoll);
		fPollRunner = new BMessageRunner(BMessenger(this), &poll, 1000000);
	}

	virtual ~SystemLogsWindow()
	{
		delete fPollRunner;
	}

	virtual void MessageReceived(BMessage* message)
	{
		switch (message->what) {
			case kMsgPoll:
				_Poll();
				break;

			case kMsgSetFilter:
			{
				int32 tab = message->GetInt32("tab", fTabView->Selection());
				LogView* view = tab == 1 ? fPrevious : fLive;
				view->SetFilter(message);
				_Reply(message);
				break;
			}

			case kMsgSelectTab:
				fTabView->Select(message->GetInt32("tab", 0));
				_Reply(message);
				break;

			case kMsgSelectCrash:
				SelectCrash(message->GetString("path", ""));
				_Reply(message);
				break;

			case kMsgCrashSelected:
				_ShowSelectedCrash();
				break;

			case kMsgSaveReport:
			{
				BString path = message->GetString("path",
					default_report_path().String());
				BString summary;
				status_t status = save_report_archive(path.String(), &summary);
				BString text;
				if (status == B_OK) {
					BPath saved(path.String());
					text.SetToFormat(B_TRANSLATE("Saved %s on the Desktop."),
						saved.Leaf());
				} else {
					text.SetToFormat(B_TRANSLATE("Could not save the report: "
						"%s"), strerror(status));
				}
				fSaveStatus->SetText(text);
				if (message->IsSourceWaiting()) {
					BMessage reply(B_REPLY);
					reply.AddInt32("status", status);
					reply.AddString("path", path);
					message->SendReply(&reply);
				}
				break;
			}

			case kMsgGetState:
			{
				BMessage reply(B_REPLY);
				_State(reply);
				message->SendReply(&reply);
				break;
			}

			default:
				BWindow::MessageReceived(message);
		}
	}

	void SelectCrash(const char* path)
	{
		_LoadCrashes();
		fTabView->Select(2);
		for (int32 i = 0; i < fCrashList->CountItems(); i++) {
			CrashItem* item = (CrashItem*)fCrashList->ItemAt(i);
			if (item->Path() == path) {
				fCrashList->Select(i);
				fCrashList->ScrollToSelection();
				_ShowSelectedCrash();
				return;
			}
		}
	}

private:
	void _Reply(BMessage* message)
	{
		if (message->IsSourceWaiting()) {
			BMessage reply(B_REPLY);
			_State(reply);
			message->SendReply(&reply);
		}
	}

	void _State(BMessage& reply)
	{
		reply.AddInt32("tab", fTabView->Selection());
		reply.AddInt32("live_lines", fLive->CountLines());
		reply.AddInt32("live_shown", fLive->CountShown());
		reply.AddInt32("live_components", fLive->CountComponents());
		reply.AddInt32("previous_lines", fPrevious->CountLines());
		reply.AddBool("previous_found", fPreviousFound);
		reply.AddInt32("crash_reports", fCrashList->CountItems());
		CrashItem* item = (CrashItem*)fCrashList->ItemAt(
			fCrashList->CurrentSelection());
		reply.AddString("selected_crash", item != NULL ? item->Path() : "");
		reply.AddString("selected_crash_label",
			item != NULL ? item->Text() : "");
		reply.AddInt32("crash_text_length", fCrashText->TextLength());
	}

	void _LoadLogs()
	{
		std::string current;
		std::string previous;
		fLiveOffset = read_boots(current, previous, fPreviousFound);
		fLive->SetText(current);
		if (fPreviousFound) {
			fPrevious->SetText(previous, B_TRANSLATE("from the system log"));
		} else {
			// Rotated out of the syslog: the kernel's own log of the
			// previous session, which the boot loader kept across the
			// reboot.
			std::string kernel;
			if (read_file(log_path("previous_syslog").String(), kernel)) {
				fPrevious->SetText(kernel, B_TRANSLATE("only the kernel's log "
					"(previous_syslog): the system log no longer has that "
					"boot"));
			} else {
				fPrevious->SetText("", B_TRANSLATE("not available"));
			}
		}
	}

	void _Poll()
	{
		struct stat st;
		BString path = log_path("syslog");
		if (stat(path.String(), &st) == 0) {
			if (st.st_size < fLiveOffset) {
				// syslog_daemon rotated the file
				_LoadLogs();
			} else if (st.st_size > fLiveOffset) {
				std::string text;
				read_file(path.String(), text, fLiveOffset);
				// only whole lines
				size_t end = text.rfind('\n');
				if (end != std::string::npos) {
					text.resize(end + 1);
					fLiveOffset += text.size();
					if (text.find(kBootMarker) != std::string::npos)
						_LoadLogs();
					else
						fLive->AppendText(text.substr(0, text.size() - 1));
				}
			}
		}

		if (stat(CRASH_REPORT_DIRECTORY, &st) == 0
			&& st.st_mtime != fCrashDirectoryTime) {
			BString selected;
			if (CrashItem* item = (CrashItem*)fCrashList->ItemAt(
					fCrashList->CurrentSelection())) {
				selected = item->Path();
			}
			_LoadCrashes();
			if (selected.Length() > 0) {
				for (int32 i = 0; i < fCrashList->CountItems(); i++) {
					if (((CrashItem*)fCrashList->ItemAt(i))->Path()
							== selected) {
						fCrashList->Select(i);
						break;
					}
				}
			}
		}
	}

	void _LoadCrashes()
	{
		struct stat st;
		fCrashDirectoryTime = stat(CRASH_REPORT_DIRECTORY, &st) == 0
			? st.st_mtime : -1;

		std::vector<CrashItem*> items;
		BDirectory directory(CRASH_REPORT_DIRECTORY);
		BEntry entry;
		while (directory.GetNextEntry(&entry) == B_OK) {
			BPath path;
			if (!entry.IsFile() || entry.GetPath(&path) != B_OK)
				continue;
			std::string text;
			read_file(path.Path(), text);
			time_t when = 0;
			entry.GetModificationTime(&when);
			items.push_back(new CrashItem(crash_label(path.Leaf(),
				std::string("\n") + text), path.Path(), when));
		}
		std::sort(items.begin(), items.end(), compare_crash_items);

		fCrashList->SetSelectionMessage(NULL);
		while (BListItem* item = fCrashList->RemoveItem((int32)0))
			delete item;
		for (size_t i = 0; i < items.size(); i++)
			fCrashList->AddItem(items[i]);
		fCrashList->SetSelectionMessage(new BMessage(kMsgCrashSelected));
		fCrashText->SetText("");

		BString status;
		if (items.empty()) {
			status = B_TRANSLATE("No crash reports. Kernel crash reports "
				"appear here after a reboot from the kernel debugger; "
				"application crash reports when a program crashes.");
		} else {
			status.SetToFormat(B_TRANSLATE("%" B_PRId32 " crash reports in "
				"%s"), (int32)items.size(), CRASH_REPORT_DIRECTORY);
		}
		fCrashEmpty->SetText(status);
	}

	void _ShowSelectedCrash()
	{
		CrashItem* item = (CrashItem*)fCrashList->ItemAt(
			fCrashList->CurrentSelection());
		if (item == NULL) {
			fCrashText->SetText("");
			return;
		}
		std::string text;
		read_file(item->Path().String(), text);
		fCrashText->SetText(text.c_str(), text.size());
		fCrashText->ScrollToOffset(0);
		// it has been seen now
		BNode node(item->Path().String());
		node.RemoveAttr(CRASH_REPORT_NEW_ATTRIBUTE);
	}

	BTabView*		fTabView;
	LogView*		fLive;
	LogView*		fPrevious;
	BListView*		fCrashList;
	BTextView*		fCrashText;
	BStringView*	fCrashEmpty;
	BButton*		fSaveButton;
	BStringView*	fSaveStatus;
	off_t			fLiveOffset;
	bool			fPreviousFound;
	time_t			fCrashDirectoryTime;
	BMessageRunner*	fPollRunner;
};


// #pragma mark - application


class SystemLogsApp : public BApplication {
public:
	SystemLogsApp()
		:
		BApplication(kSignature),
		fWindow(NULL)
	{
	}

	virtual void ReadyToRun()
	{
		_Window();
	}

	virtual void ArgvReceived(int32 argc, char** argv)
	{
		for (int32 i = 1; i < argc; i++) {
			if (argv[i][0] == '-')
				continue;
			BEntry entry(argv[i], true);
			BPath path;
			if (entry.GetPath(&path) == B_OK)
				_ShowCrash(path.Path());
		}
	}

	virtual void RefsReceived(BMessage* message)
	{
		entry_ref ref;
		for (int32 i = 0; message->FindRef("refs", i, &ref) == B_OK; i++) {
			BPath path(&ref);
			if (path.InitCheck() == B_OK)
				_ShowCrash(path.Path());
		}
	}

	virtual void MessageReceived(BMessage* message)
	{
		switch (message->what) {
			case kMsgSetFilter:
			case kMsgSelectTab:
			case kMsgSelectCrash:
			case kMsgSaveReport:
			case kMsgGetState:
				// from other programs: the window answers
				_Window()->PostMessage(DetachCurrentMessage());
				break;
			default:
				BApplication::MessageReceived(message);
		}
	}

private:
	SystemLogsWindow* _Window()
	{
		if (fWindow == NULL) {
			fWindow = new SystemLogsWindow();
			fWindow->Show();
		}
		return fWindow;
	}

	void _ShowCrash(const char* path)
	{
		BMessage message(kMsgSelectCrash);
		message.AddString("path", path);
		_Window()->PostMessage(&message);
	}

	SystemLogsWindow*	fWindow;
};


// #pragma mark - command line


static int
notify_new_crashes()
{
	BApplication app(kHelperSignature);

	// notification_server is a system service; at login it may still be
	// starting.
	for (int32 i = 0; i < 60
			&& !be_roster->IsRunning("application/x-vnd.Haiku-notification_server");
			i++) {
		snooze(500000);
	}

	int32 shown = 0;
	BDirectory directory(CRASH_REPORT_DIRECTORY);
	BEntry entry;
	while (directory.GetNextEntry(&entry) == B_OK) {
		BPath path;
		if (entry.GetPath(&path) != B_OK
			|| strncmp(path.Leaf(), "kernel-", 7) != 0) {
			continue;
		}
		BNode node(&entry);
		int32 isNew = 0;
		if (node.ReadAttr(CRASH_REPORT_NEW_ATTRIBUTE, B_INT32_TYPE, 0, &isNew,
				sizeof(isNew)) != sizeof(isNew) || isNew == 0) {
			continue;
		}

		std::string text;
		read_file(path.Path(), text);
		text = "\n" + text;
		std::string when = report_field(text, "crash time");
		std::string message = report_field(text, "message");

		BNotification notification(B_IMPORTANT_NOTIFICATION);
		notification.SetGroup(B_TRANSLATE_SYSTEM_NAME("System logs"));
		notification.SetTitle(B_TRANSLATE("The system crashed"));
		BString content;
		content.SetToFormat(B_TRANSLATE("The kernel stopped on %s: %s\n"
			"Click to view the crash report."), when.c_str(),
			message.c_str());
		notification.SetContent(content);
		notification.SetMessageID(path.Path());
		notification.SetOnClickApp(kSignature);
		entry_ref ref;
		if (entry.GetRef(&ref) == B_OK)
			notification.SetOnClickFile(&ref);

		status_t status = B_ERROR;
		for (int32 attempt = 0; attempt < 20; attempt++) {
			status = notification.Send(60 * 1000000LL);
			if (status == B_OK)
				break;
			snooze(500000);
		}
		if (status != B_OK) {
			syslog(LOG_ERR, "SystemLogs: could not show the notification for "
				"%s: %s", path.Path(), strerror(status));
			continue;
		}
		node.RemoveAttr(CRASH_REPORT_NEW_ATTRIBUTE);
		syslog(LOG_NOTICE, "SystemLogs: crash report notification shown for "
			"%s", path.Path());
		printf("SystemLogs: notification shown for %s\n", path.Path());
		shown++;
	}
	return 0;
}


static int
save_report_command(const char* path)
{
	BApplication app(kHelperSignature);
	BString target = path != NULL ? BString(path) : default_report_path();
	BString summary;
	status_t status = save_report_archive(target.String(), &summary);
	if (status != B_OK) {
		fprintf(stderr, "SystemLogs: could not save %s: %s\n",
			target.String(), strerror(status));
		return 1;
	}
	printf("%s\n%s", target.String(), summary.String());
	return 0;
}


int
main(int argc, char** argv)
{
	openlog("SystemLogs", 0, LOG_USER);

	if (argc >= 2 && strcmp(argv[1], "--notify") == 0)
		return notify_new_crashes();
	if (argc >= 2 && strcmp(argv[1], "--save-report") == 0)
		return save_report_command(argc >= 3 ? argv[2] : NULL);
	if (argc >= 2 && (strcmp(argv[1], "--help") == 0
			|| strcmp(argv[1], "-h") == 0)) {
		printf("usage: %s [<crash report>]\n"
			"       %s --notify\n"
			"       %s --save-report [<archive>]\n", argv[0], argv[0],
			argv[0]);
		return 0;
	}

	SystemLogsApp app;
	app.Run();
	return 0;
}
