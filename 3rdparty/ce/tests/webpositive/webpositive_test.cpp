/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 */

/*!	In-guest test: WebPositive's batch 8 quick fixes and speed pass (roadmap
	F-57).

	Suite webpositive_history (every image; WebPositive's BrowsingHistory.cpp
	is built into this program, so no WebKit is needed):
	  webpositive_history.add_fast       a visit costs the same with 8000
	                                     items in the history as with none
	                                     (200 visits each; at most 3 times
	                                     as long plus 50 ms), and under 5 ms.
	                                     Known-bad: every visit rewrote the
	                                     whole history file on the browser
	                                     window's thread (UpdateGlobalHistory)
	                                     and compared the new item with every
	                                     item, so its cost grew with the
	                                     history's length.
	  webpositive_history.saved_later    the file holds all 2000 within 6 s
	                                     (the saver thread writes 2 s after
	                                     the first unwritten change).
	  webpositive_history.load_fast      a file of 8000 items (plus 100
	                                     duplicates) loads in under 1 s.
	                                     Known-bad: loading compared each item
	                                     with every one before it (quadratic,
	                                     on the application thread at start).
	  webpositive_history.load_order     the loaded list keeps the file's order
	                                     and drops the duplicates.
	  webpositive_history.saved_at_quit  a change still waiting for the saver
	                                     is written by the destructor (run
	                                     in a child process that exits at once).

	Suite webpositive_dark (images with WebPositive and a desktop; no checks
	elsewhere): a local page puts matchMedia("(prefers-color-scheme: dark)")
	in its title, which the test reads from WebPositive's window.
	  webpositive_dark.light_start        light appearance: "light".
	  webpositive_dark.follows_appearance Appearance switched to dark colours
	                                     while the page is open: "dark".
	                                     Known-bad: WebPositive never called
	                                     BWebView::SetDarkMode(), pages stayed
	                                     light.
	  webpositive_dark.dark_start         started with dark colours: "dark".
	  webpositive_dark.switch_off         "follow dark appearance" off in the
	                                     settings: "light" with dark colours.
	The system colours and WebPositive's settings are restored afterwards.
*/


#include <Application.h>
#include <File.h>
#include <FindDirectory.h>
#include <InterfaceDefs.h>
#include <Message.h>
#include <Messenger.h>
#include <OS.h>
#include <Path.h>
#include <Roster.h>
#include <String.h>

#include <image.h>

#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "BrowsingHistory.h"


// BrowsingHistory.cpp names its settings folder after WebPositive's.
const char* kApplicationName = "WebPositive";

static int sFailures = 0;
// Restored at the very end, after the history's last write (see main()).
static BString sHistoryBackup;
static BString sHistoryPath;


static void
check(const char* name, bool ok, const char* format, ...)
{
	char detail[512];
	va_list args;
	va_start(args, format);
	vsnprintf(detail, sizeof(detail), format, args);
	va_end(args);
	printf("CE-TEST %s %s %s\n", name, ok ? "PASS" : "FAIL", detail);
	fflush(stdout);
	if (!ok)
		sFailures++;
}


static void
info(const char* format, ...)
{
	va_list args;
	va_start(args, format);
	printf("CE-INFO webpositive: ");
	vprintf(format, args);
	printf("\n");
	va_end(args);
	fflush(stdout);
}


static BString
settings_path(const char* leaf)
{
	BPath path;
	find_directory(B_USER_SETTINGS_DIRECTORY, &path);
	path.Append("WebPositive");
	mkdir(path.Path(), 0755);
	path.Append(leaf);
	return path.Path();
}


static int32
items_in_file(const char* path)
{
	BFile file(path, B_READ_ONLY);
	BMessage archive;
	if (file.InitCheck() != B_OK || archive.Unflatten(&file) != B_OK)
		return -1;
	type_code type;
	int32 count = 0;
	if (archive.GetInfo("history item", &type, &count) != B_OK)
		return 0;
	return count;
}


static void
copy_file(const char* from, const char* to)
{
	BString command;
	command.SetToFormat("cp -p '%s' '%s' 2>/dev/null", from, to);
	system(command.String());
}


// #pragma mark - history


static void
history_suite()
{
	printf("CE-SUITE webpositive_history START\n");
	fflush(stdout);
	int failuresBefore = sFailures;

	BString historyPath = settings_path("BrowsingHistory");
	BString backup = historyPath;
	backup << ".ce-backup";
	bool hadHistory = access(historyPath.String(), F_OK) == 0;
	if (hadHistory)
		copy_file(historyPath.String(), backup.String());

	// A file of 8000 items, newest last, with 100 duplicates of earlier
	// URLs mixed in, written before the first DefaultInstance() loads it.
	const int32 kLoadItems = 8000;
	const int32 kDuplicates = 100;
	{
		BMessage archive;
		archive.AddInt32("max history item age", 7);
		for (int32 i = 0; i < kLoadItems + kDuplicates; i++) {
			int32 index = i < kLoadItems ? i : (i - kLoadItems) * 7;
			BString url;
			url.SetToFormat("http://load.test/%" B_PRId32, index);
			BrowsingHistoryItem item(url);
			BMessage itemArchive;
			item.Archive(&itemArchive);
			archive.AddMessage("history item", &itemArchive);
		}
		BFile file(historyPath.String(),
			B_CREATE_FILE | B_ERASE_FILE | B_WRITE_ONLY);
		archive.Flatten(&file);
	}

	bigtime_t start = system_time();
	BrowsingHistory* history = BrowsingHistory::DefaultInstance();
	bigtime_t loadTime = system_time() - start;
	int32 loaded = history->CountItems();
	check("webpositive_history.load_fast", loadTime < 1000000,
		"%" B_PRId32 " items in %" B_PRId64 " ms (limit 1000)", loaded,
		loadTime / 1000);

	bool ordered = loaded == kLoadItems;
	for (int32 i = 0; ordered && i < loaded; i++) {
		BString url;
		url.SetToFormat("http://load.test/%" B_PRId32, i);
		if (history->HistoryItemAt(i).URL() != url) {
			info("item %" B_PRId32 " is %s, expected %s", i,
				history->HistoryItemAt(i).URL().String(), url.String());
			ordered = false;
		}
	}
	check("webpositive_history.load_order", ordered,
		"%" B_PRId32 " of %" B_PRId32 " items (%" B_PRId32 " duplicates "
		"dropped), in the file's order", loaded, kLoadItems, kDuplicates);

	// 200 new pages visited with the 8000 items loaded, then 200 with an
	// empty history: the cost of a visit must not depend on the length.
	const int32 kTimed = 200;
	start = system_time();
	for (int32 i = 0; i < kTimed; i++) {
		BString url;
		url.SetToFormat("http://full.test/page/%" B_PRId32, i);
		history->AddItem(BrowsingHistoryItem(url));
	}
	bigtime_t fullTime = system_time() - start;

	history->Clear();
	const int32 kVisits = 2000;
	bigtime_t emptyTime = 0;
	start = system_time();
	for (int32 i = 0; i < kVisits; i++) {
		if (i == kTimed)
			emptyTime = system_time() - start;
		BString url;
		url.SetToFormat("http://visit.test/page/%" B_PRId32, i);
		history->AddItem(BrowsingHistoryItem(url));
	}
	bigtime_t allTime = system_time() - start;
	check("webpositive_history.add_fast",
		fullTime < 3 * emptyTime + 50000 && fullTime < kTimed * 5000,
		"%" B_PRId32 " visits: %" B_PRId64 " ms with %" B_PRId32 " items, %"
		B_PRId64 " ms with none (%" B_PRId32 " visits in %" B_PRId64 " ms)",
		kTimed, fullTime / 1000, loaded, emptyTime / 1000, kVisits,
		allTime / 1000);

	int32 saved = -1;
	bigtime_t deadline = system_time() + 6000000;
	while (system_time() < deadline) {
		saved = items_in_file(historyPath.String());
		if (saved == kVisits)
			break;
		snooze(100000);
	}
	check("webpositive_history.saved_later", saved == kVisits,
		"%" B_PRId32 " of %" B_PRId32 " items in the file", saved, kVisits);

	// A visit followed at once by quitting: this program started again
	// with --history-child, whose static history's destructor writes it.
	image_info image;
	int32 cookie = 0;
	BString self;
	while (get_next_image_info(B_CURRENT_TEAM, &cookie, &image) == B_OK) {
		if (image.type == B_APP_IMAGE) {
			self = image.name;
			break;
		}
	}
	pid_t child = fork();
	if (child == 0) {
		execl(self.String(), self.String(), "--history-child", (char*)NULL);
		_exit(127);
	}
	int status = 0;
	waitpid(child, &status, 0);
	saved = items_in_file(historyPath.String());
	check("webpositive_history.saved_at_quit", saved == kVisits + 1,
		"%" B_PRId32 " items in the file after the child quit (expected %"
		B_PRId32 ")", saved, kVisits + 1);

	history->Clear();
	sHistoryBackup = hadHistory ? backup : BString();
	sHistoryPath = historyPath;

	printf("CE-SUITE webpositive_history DONE failures=%d\n",
		sFailures - failuresBefore);
	fflush(stdout);
}


// #pragma mark - dark appearance


static const char* kPage =
	"<html><head><title>scheme pending</title><script>\n"
	"var q = matchMedia('(prefers-color-scheme: dark)');\n"
	"function show() { document.title = 'scheme ' + (q.matches ? 'dark'"
	" : 'light'); }\n"
	"if (q.addEventListener) q.addEventListener('change', show);\n"
	"else q.addListener(show);\n"
	"show();\n"
	"setInterval(show, 500);\n"
	"</script></head><body>prefers-color-scheme</body></html>\n";


static BString
window_title(team_id team)
{
	BMessenger app(NULL, team);
	for (int32 index = 0; index < 12; index++) {
		BMessage get(B_GET_PROPERTY);
		get.AddSpecifier("Title");
		get.AddSpecifier("Window", index);
		BMessage reply;
		BString title;
		if (app.SendMessage(&get, &reply, 2000000, 2000000) != B_OK
			|| reply.FindString("result", &title) != B_OK)
			continue;
		if (title.FindFirst("scheme ") >= 0)
			return title;
	}
	return "";
}


static void
quit_team(team_id team)
{
	BMessenger messenger(NULL, team);
	if (messenger.IsValid())
		messenger.SendMessage(B_QUIT_REQUESTED);
	for (int i = 0; i < 40; i++) {
		if (waitpid(team, NULL, WNOHANG) == team)
			return;
		snooze(250000);
	}
	kill(team, SIGKILL);
	waitpid(team, NULL, 0);
}


static pid_t
start_webpositive(const char* url)
{
	// WebPositive restores the previous run's windows: start afresh.
	unlink(settings_path("Session").String());
	pid_t child = fork();
	if (child == 0) {
		execl("/boot/system/apps/WebPositive", "WebPositive", url,
			(char*)NULL);
		_exit(127);
	}
	return child;
}


/*!	Waits until the page's title says \a want ("dark" or "light"); returns
	the last title seen.
*/
static BString
wait_scheme(pid_t team, const char* want, bigtime_t timeout)
{
	BString title;
	BString expected("scheme ");
	expected << want;
	bigtime_t deadline = system_time() + timeout;
	while (system_time() < deadline) {
		title = window_title(team);
		if (title.FindFirst(expected) >= 0)
			break;
		snooze(500000);
	}
	return title;
}


static void
set_document_colors(bool dark, const BMessage& original)
{
	if (!dark) {
		set_ui_colors(&original);
		return;
	}
	BMessage colors;
	rgb_color background = make_color(0x28, 0x28, 0x28);
	rgb_color text = make_color(0xe8, 0xe8, 0xe8);
	colors.AddColor(ui_color_name(B_DOCUMENT_BACKGROUND_COLOR), background);
	colors.AddColor(ui_color_name(B_DOCUMENT_TEXT_COLOR), text);
	colors.AddColor(ui_color_name(B_PANEL_BACKGROUND_COLOR), background);
	colors.AddColor(ui_color_name(B_PANEL_TEXT_COLOR), text);
	set_ui_colors(&colors);
}


static void
set_follow_setting(const char* path, int value)
{
	// value: 1 on, 0 off, -1 removed (the default)
	BMessage settings;
	{
		BFile file(path, B_READ_ONLY);
		if (file.InitCheck() == B_OK)
			settings.Unflatten(&file);
	}
	settings.RemoveName("follow dark appearance");
	if (value >= 0)
		settings.AddBool("follow dark appearance", value != 0);
	BFile file(path, B_CREATE_FILE | B_ERASE_FILE | B_WRITE_ONLY);
	settings.Flatten(&file);
}


static void
dark_suite()
{
	printf("CE-SUITE webpositive_dark START\n");
	fflush(stdout);
	int failuresBefore = sFailures;

	if (access("/boot/system/apps/WebPositive", X_OK) != 0
		|| !be_roster->IsRunning("application/x-vnd.Be-input_server")) {
		info("no WebPositive or no desktop on this image, dark checks not "
			"run");
		printf("CE-SUITE webpositive_dark DONE failures=0\n");
		return;
	}

	const char* kDirectory = "/boot/home/webpositive_test";
	mkdir(kDirectory, 0755);
	BString pagePath(kDirectory);
	pagePath << "/scheme.html";
	{
		BFile file(pagePath.String(),
			B_CREATE_FILE | B_ERASE_FILE | B_WRITE_ONLY);
		file.Write(kPage, strlen(kPage));
	}
	BString url("file://");
	url << pagePath;

	BMessage original;
	color_which which[] = { B_DOCUMENT_BACKGROUND_COLOR, B_DOCUMENT_TEXT_COLOR,
		B_PANEL_BACKGROUND_COLOR, B_PANEL_TEXT_COLOR };
	for (size_t i = 0; i < sizeof(which) / sizeof(which[0]); i++)
		original.AddColor(ui_color_name(which[i]), ui_color(which[i]));
	bool startedDark = ui_color(B_DOCUMENT_BACKGROUND_COLOR).IsDark();
	if (startedDark) {
		// The checks start from a light appearance.
		BMessage light;
		light.AddColor(ui_color_name(B_DOCUMENT_BACKGROUND_COLOR),
			make_color(255, 255, 255));
		light.AddColor(ui_color_name(B_DOCUMENT_TEXT_COLOR),
			make_color(0, 0, 0));
		set_ui_colors(&light);
	}

	BString appSettings = settings_path("Application");
	BString backup = appSettings;
	backup << ".ce-backup";
	bool hadSettings = access(appSettings.String(), F_OK) == 0;
	if (hadSettings)
		copy_file(appSettings.String(), backup.String());
	set_follow_setting(appSettings.String(), -1);

	pid_t team = start_webpositive(url.String());
	BString title = wait_scheme(team, "light", 60000000);
	check("webpositive_dark.light_start", title.FindFirst("scheme light") >= 0,
		"title \"%s\"", title.String());

	set_document_colors(true, original);
	title = wait_scheme(team, "dark", 15000000);
	check("webpositive_dark.follows_appearance",
		title.FindFirst("scheme dark") >= 0, "title \"%s\" after switching "
		"to dark colours", title.String());
	quit_team(team);

	team = start_webpositive(url.String());
	title = wait_scheme(team, "dark", 60000000);
	check("webpositive_dark.dark_start", title.FindFirst("scheme dark") >= 0,
		"title \"%s\"", title.String());
	quit_team(team);

	set_follow_setting(appSettings.String(), 0);
	team = start_webpositive(url.String());
	// The page settles on "light" at once when the setting is off; give it
	// the same time a dark switch would need to show.
	title = wait_scheme(team, "dark", 20000000);
	if (title.FindFirst("scheme dark") < 0)
		title = wait_scheme(team, "light", 5000000);
	check("webpositive_dark.switch_off", title.FindFirst("scheme light") >= 0,
		"title \"%s\" with dark colours and the setting off", title.String());
	quit_team(team);

	set_ui_colors(&original);
	if (hadSettings) {
		copy_file(backup.String(), appSettings.String());
		unlink(backup.String());
	} else
		set_follow_setting(appSettings.String(), -1);
	if (startedDark)
		info("system colours restored to the dark set");

	printf("CE-SUITE webpositive_dark DONE failures=%d\n",
		sFailures - failuresBefore);
	fflush(stdout);
}


int
main(int argc, char** argv)
{
	if (argc > 1 && strcmp(argv[1], "--history-child") == 0) {
		// Loads the 2000 visits, adds one and quits before the saver's
		// delay is over: the static instance's destructor must write it.
		BrowsingHistory::DefaultInstance()->AddItem(
			BrowsingHistoryItem("http://visit.test/last"));
		return 0;
	}

	BApplication app("application/x-vnd.Renga-webpositive-test");
	history_suite();
	dark_suite();

	// The static history writes itself once more when the program ends;
	// the user's own history is put back after that by ending here.
	BrowsingHistory::DefaultInstance()->Clear();
	if (!sHistoryBackup.IsEmpty()) {
		copy_file(sHistoryBackup.String(), sHistoryPath.String());
		unlink(sHistoryBackup.String());
	} else if (!sHistoryPath.IsEmpty())
		unlink(sHistoryPath.String());
	fflush(stdout);
	_exit(sFailures == 0 ? 0 : 1);
}
