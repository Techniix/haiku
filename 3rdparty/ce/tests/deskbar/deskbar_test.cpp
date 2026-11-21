/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 */

/*!	In-guest test: the Deskbar's batch 9 features (roadmap F-41) and its
	F-60 review numbers.

	Through the Deskbar's harness hooks (harness images only, BarApp.h):
	kTestDeskbarMenu builds the Deskbar menu exactly as opening it does,
	kTestSearch runs the menu search, kTestSearchWindow shows the search
	window, kTestAppRecentDocuments builds an application's "Recent
	documents" submenu; kRestartInto with "test" sets the boot state's
	choice without restarting.

	  deskbar.menu_search_item     "Search..." heads the Deskbar menu
	  deskbar.menu_time            the menu builds in under 2 s (the time is
	                               printed as INFO, F-60)
	  deskbar.restart_into_menu    Shutdown has "Restart into" with "Boot
	                               menu" and "Other systems..."
	  deskbar.restart_into_sets    choosing "Boot menu" writes "menu" as the
	                               boot state's one-shot choice (reset
	                               afterwards)
	  deskbar.calculator           "2*(3+4)" gives 14, first
	  deskbar.calculator_functions "sqrt(2)^2" gives 2, "10%3" gives 1
	  deskbar.conversion           km to mi, Fahrenheit to Celsius, GiB to
	                               MB, lb to kg
	  deskbar.no_false_expression  "terminal" and "5" give no calculation;
	                               "terminal" finds the Terminal
	  deskbar.settings             "wallpaper" finds the Control Panel's
	                               Backgrounds page
	  deskbar.files                a file is found by part of its name
	  deskbar.recent               a recent document is found by name
	  deskbar.jump_list            StyledEdit's window menu gets a "Recent
	                               documents" submenu with that document
	  deskbar.window               the search window shows "2*21 = 42" first
	                               (screenshot deskbar_search) and opens a
	                               result: the Terminal
	  deskbar.switches             each of the five switches turns its
	                               feature off and on again
	  deskbar.preferences          the Deskbar preferences show the switches
	                               (screenshot deskbar_preferences)

	Output format: see 3rdparty/ce/README.md.
*/


#include <Application.h>
#include <Directory.h>
#include <Entry.h>
#include <File.h>
#include <FindDirectory.h>
#include <Messenger.h>
#include <Path.h>
#include <Roster.h>
#include <String.h>

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <boot_state_file.h>


#define SUITE "deskbar"

static const char* kDeskbarSignature = "application/x-vnd.Be-TSKB";
static const char* kStyledEditSignature = "application/x-vnd.Haiku-StyledEdit";
static const char* kTerminalSignature = "application/x-vnd.Haiku-Terminal";

// src/apps/deskbar/BarApp.h, SearchWindow.h, PreferencesWindow.h
static const uint32 kToggleSearch = 'DStS';
static const uint32 kToggleSearchCalculator = 'DStC';
static const uint32 kToggleSearchFiles = 'DStF';
static const uint32 kToggleRestartInto = 'DStR';
static const uint32 kToggleAppRecentDocuments = 'DStJ';
static const uint32 kRestartInto = 'DSri';
static const uint32 kTestDeskbarMenu = 'Tdmn';
static const uint32 kTestSearch = 'Tsrq';
static const uint32 kTestSearchWindow = 'Tsrw';
static const uint32 kTestAppRecentDocuments = 'Tjmp';
static const uint32 kConfigShow = 'PrSh';

// MenuSearch.h
enum {
	kResultCalculator = 0,
	kResultConversion,
	kResultApplication,
	kResultSetting,
	kResultRecentDocument,
	kResultRecentFolder,
	kResultFile
};

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


static status_t
ask(BMessage& message, BMessage& reply)
{
	status_t status = BMessenger(kDeskbarSignature).SendMessage(&message,
		&reply, 5000000, 30000000);
	if (status == B_OK && reply.what != B_REPLY)
		status = B_ERROR;
	return status;
}


static bool
has_string(const BMessage& message, const char* field, const char* value)
{
	const char* string;
	for (int32 i = 0; message.FindString(field, i, &string) == B_OK; i++) {
		if (strcmp(string, value) == 0)
			return true;
	}
	return false;
}


static int32
count_strings(const BMessage& message, const char* field)
{
	type_code type;
	int32 count = 0;
	if (message.GetInfo(field, &type, &count) != B_OK)
		return 0;
	return count;
}


static BMessage
menu()
{
	BMessage message(kTestDeskbarMenu);
	BMessage reply;
	ask(message, reply);
	return reply;
}


static BMessage
search(const char* query)
{
	BMessage message(kTestSearch);
	message.AddString("query", query);
	BMessage reply;
	ask(message, reply);
	return reply;
}


//! The value of the first result of \a kind, "" if there is none.
static BString
first_value(const BMessage& reply, int32 kind, int32* index = NULL)
{
	int32 resultKind;
	for (int32 i = 0; reply.FindInt32("kind", i, &resultKind) == B_OK; i++) {
		if (resultKind == kind) {
			if (index != NULL)
				*index = i;
			return reply.GetString("value", i, "");
		}
	}
	if (index != NULL)
		*index = -1;
	return "";
}


static bool
has_result(const BMessage& reply, int32 kind, const char* title)
{
	int32 resultKind;
	for (int32 i = 0; reply.FindInt32("kind", i, &resultKind) == B_OK; i++) {
		if (resultKind == kind && (title == NULL
				|| strcmp(reply.GetString("title", i, ""), title) == 0)) {
			return true;
		}
	}
	return false;
}


static void
toggle(uint32 what, bool on)
{
	BMessage message(what);
	message.AddInt32("be:value", on ? 1 : 0);
	BMessenger(kDeskbarSignature).SendMessage(&message);
	snooze(200000);
}


static BString
next_once()
{
	char path[B_PATH_NAME_LENGTH];
	BootState::file_path(path, sizeof(path));
	BootState::Record record;
	if (BootState::read_file(path, record) < 0)
		return "<no file>";
	return record.nextOnce;
}


static void
clear_next_once()
{
	char path[B_PATH_NAME_LENGTH];
	BootState::file_path(path, sizeof(path));
	BootState::Record record;
	int slot = BootState::read_file(path, record);
	if (slot == -2)
		return;
	BootState::set_name(record.nextOnce, sizeof(record.nextOnce), "");
	BootState::write_file(path, record, slot);
}


int
main(int argc, char** argv)
{
	BApplication app("application/x-vnd.Renga-deskbar-test");
	printf("CE-SUITE %s START\n", SUITE);

	for (int32 i = 0; i < 80 && !be_roster->IsRunning(kDeskbarSignature);
			i++) {
		snooze(250000);
	}
	// The renga-* images' first-login phase has no Deskbar: it is started
	// here and quit again at the end, so later suites see the same desktop.
	bool started = false;
	if (!be_roster->IsRunning(kDeskbarSignature)) {
		printf("CE-INFO deskbar: starting the Deskbar\n");
		started = true;
		be_roster->Launch(kDeskbarSignature);
		for (int32 i = 0; i < 120
				&& !be_roster->IsRunning(kDeskbarSignature); i++) {
			snooze(250000);
		}
	}
	snooze(2000000);
	if (!be_roster->IsRunning(kDeskbarSignature)) {
		printf("CE-INFO deskbar: the Deskbar is not running, nothing to "
			"test\n");
		printf("CE-SUITE %s DONE failures=0\n", SUITE);
		return 0;
	}

	// The menu, cold and warm (F-60).
	BMessage first = menu();
	BMessage second = menu();
	report("menu_search_item", has_string(first, "item", "Search\xE2\x80\xA6")
			&& strcmp(first.GetString("item", 0, ""), "Search\xE2\x80\xA6")
				== 0,
		"first item \"%s\", %d items", first.GetString("item", 0, "?"),
		(int)count_strings(first, "item"));
	bigtime_t warm = second.GetInt64("time", -1);
	report("menu_time", warm >= 0 && warm < 2000000,
		"built in %.2f ms (first %.2f ms)", warm / 1000.0,
		first.GetInt64("time", -1) / 1000.0);
	printf("CE-INFO deskbar: menu build %.2f ms cold, %.2f ms warm, %d "
		"top-level items\n", first.GetInt64("time", -1) / 1000.0,
		warm / 1000.0, (int)count_strings(second, "item"));

	// Restart into.
	char statePath[B_PATH_NAME_LENGTH];
	BootState::file_path(statePath, sizeof(statePath));
	bool haveState = access(statePath, W_OK) == 0;
	if (haveState) {
		report("restart_into_menu", has_string(second, "shutdown",
				"Restart into") && has_string(second, "restart", "Boot menu")
				&& has_string(second, "restart", "Other systems\xE2\x80\xA6"),
			"Shutdown has Restart into %d, Boot menu %d, Other systems %d",
			has_string(second, "shutdown", "Restart into"),
			has_string(second, "restart", "Boot menu"),
			has_string(second, "restart", "Other systems\xE2\x80\xA6"));
		BMessage restart(kRestartInto);
		restart.AddString("next", "menu");
		restart.AddBool("test", true);
		BMessage reply;
		status_t sent = ask(restart, reply);
		BString next = next_once();
		report("restart_into_sets", sent == B_OK
				&& reply.GetInt32("status", -1) == B_OK && next == "menu",
			"sent 0x%x, status 0x%x, next start \"%s\" (want \"menu\")",
			(unsigned)sent, (unsigned)reply.GetInt32("status", -1),
			next.String());
		clear_next_once();
		printf("CE-INFO deskbar: next start choice reset to \"%s\"\n",
			next_once().String());
	} else {
		printf("CE-INFO deskbar: no writable boot state file (%s): "
			"\"Restart into\" is not offered, as designed\n", statePath);
		report("restart_into_menu", !has_string(second, "shutdown",
			"Restart into"), "not offered without the boot state file");
	}

	// The calculator and conversions.
	BMessage calc = search("2*(3+4)");
	report("calculator", calc.GetInt32("kind", 0, -1) == kResultCalculator
			&& strcmp(calc.GetString("value", 0, ""), "14") == 0,
		"first kind %d, value \"%s\" (want 14)",
		(int)calc.GetInt32("kind", 0, -1), calc.GetString("value", 0, ""));
	BString root = first_value(search("sqrt(2)^2"), kResultCalculator);
	BString modulo = first_value(search("10%3"), kResultCalculator);
	report("calculator_functions", root == "2" && modulo == "1",
		"sqrt(2)^2 = \"%s\", 10%%3 = \"%s\"", root.String(),
		modulo.String());

	struct {
		const char* query;
		const char* want;
	} conversions[] = {
		{ "5 km to mi", "3.106856 mi" },
		{ "100 f in c", "37.777778 \xC2\xB0" "C" },
		{ "1 GiB in MB", "1073.741824 MB" },
		{ "10lb in kg", "4.535924 kg" },
	};
	bool conversionsPass = true;
	BString conversionDetail;
	for (size_t i = 0; i < sizeof(conversions) / sizeof(conversions[0]);
			i++) {
		BString value = first_value(search(conversions[i].query),
			kResultConversion);
		if (value != conversions[i].want)
			conversionsPass = false;
		conversionDetail << "\"" << conversions[i].query << "\" -> \""
			<< value << "\"; ";
	}
	report("conversion", conversionsPass, "%s", conversionDetail.String());

	BMessage terminal = search("terminal");
	BMessage number = search("5");
	int32 calcIndex;
	first_value(terminal, kResultCalculator, &calcIndex);
	int32 numberCalc;
	first_value(number, kResultCalculator, &numberCalc);
	report("no_false_expression", calcIndex < 0 && numberCalc < 0
			&& has_result(terminal, kResultApplication, "Terminal"),
		"calculation for \"terminal\" %d, for \"5\" %d, Terminal found %d "
		"(%d menu entries read)", calcIndex >= 0, numberCalc >= 0,
		has_result(terminal, kResultApplication, "Terminal"),
		(int)terminal.GetInt32("applications", 0));
	printf("CE-INFO deskbar: search \"terminal\": %.2f ms quick, %.2f ms "
		"files\n", terminal.GetInt64("quick_time", -1) / 1000.0,
		terminal.GetInt64("file_time", -1) / 1000.0);

	BMessage wallpaper = search("wallpaper");
	bool backgrounds = false;
	int32 kind;
	for (int32 i = 0; wallpaper.FindInt32("kind", i, &kind) == B_OK; i++) {
		if (kind == kResultSetting && strstr(wallpaper.GetString("detail",
				i, ""), "Backgrounds") != NULL) {
			backgrounds = true;
		}
	}
	report("settings", backgrounds, "\"wallpaper\": Backgrounds page %s",
		backgrounds ? "found" : "not found");

	// Files and recent documents.
	BPath folder;
	find_directory(B_USER_DIRECTORY, &folder);
	folder.Append("ce-deskbar-search");
	create_directory(folder.Path(), 0755);
	BPath probe(folder.Path(), "RengaSearchProbe notes.txt");
	{
		BFile file(probe.Path(), B_CREATE_FILE | B_WRITE_ONLY);
		file.Write("probe\n", 6);
	}
	snooze(500000);
	BMessage files = search("searchprobe");
	report("files", has_result(files, kResultFile,
			"RengaSearchProbe notes.txt"),
		"\"searchprobe\": %s in %.2f ms",
		has_result(files, kResultFile, "RengaSearchProbe notes.txt")
			? "found" : "not found", files.GetInt64("file_time", -1) / 1000.0);

	entry_ref probeRef;
	get_ref_for_path(probe.Path(), &probeRef);
	be_roster->AddToRecentDocuments(&probeRef, kStyledEditSignature);
	snooze(300000);
	BMessage recent = search("RengaSearchProbe");
	report("recent", has_result(recent, kResultRecentDocument,
			"RengaSearchProbe notes.txt"), "recent document %s",
		has_result(recent, kResultRecentDocument,
			"RengaSearchProbe notes.txt") ? "found" : "not found");

	BMessage jump(kTestAppRecentDocuments);
	jump.AddString("signature", kStyledEditSignature);
	BMessage jumpReply;
	ask(jump, jumpReply);
	report("jump_list", jumpReply.GetBool("menu", false)
			&& has_string(jumpReply, "item", "RengaSearchProbe notes.txt"),
		"submenu %d, %d items, first \"%s\"", jumpReply.GetBool("menu", false),
		(int)count_strings(jumpReply, "item"),
		jumpReply.GetString("item", 0, ""));

	// The window.
	BMessage show(kTestSearchWindow);
	show.AddString("action", "show");
	show.AddString("text", "2*21");
	BMessage shown;
	ask(show, shown);
	snooze(1500000);
	BMessage results(kTestSearchWindow);
	results.AddString("action", "results");
	BMessage listed;
	ask(results, listed);
	printf("CE-HOST screenshot deskbar_search\n");
	fflush(stdout);
	snooze(500000);
	BString windowFirst = listed.GetString("title", 0, "");

	bool terminalRunning = be_roster->IsRunning(kTerminalSignature);
	BMessage typed(kTestSearchWindow);
	typed.AddString("action", "show");
	typed.AddString("text", "Terminal");
	BMessage typedReply;
	ask(typed, typedReply);
	snooze(1000000);
	BMessage open(kTestSearchWindow);
	open.AddString("action", "open");
	BMessage openReply;
	ask(open, openReply);
	bool opened = false;
	for (int32 i = 0; i < 40 && !opened; i++) {
		snooze(250000);
		opened = be_roster->IsRunning(kTerminalSignature);
	}
	report("window", windowFirst == "2*21 = 42"
			&& !listed.GetBool("hidden", true) && (opened || terminalRunning),
		"first result \"%s\" (want \"2*21 = 42\"), shown %d; Terminal "
		"opened %d", windowFirst.String(), !listed.GetBool("hidden", true),
		opened);
	if (opened && !terminalRunning)
		BMessenger(kTerminalSignature).SendMessage(B_QUIT_REQUESTED);
	BMessage hide(kTestSearchWindow);
	hide.AddString("action", "hide");
	BMessage hidden;
	ask(hide, hidden);

	// The switches.
	BString switchDetail;
	bool switchesPass = true;

	toggle(kToggleSearch, false);
	bool searchOff = !has_string(menu(), "item", "Search\xE2\x80\xA6");
	toggle(kToggleSearch, true);
	bool searchOn = has_string(menu(), "item", "Search\xE2\x80\xA6");
	switchDetail << "search " << searchOff << searchOn << "; ";
	switchesPass &= searchOff && searchOn;

	toggle(kToggleSearchCalculator, false);
	bool calcOff = first_value(search("2*21"), kResultCalculator).IsEmpty();
	toggle(kToggleSearchCalculator, true);
	bool calcOn = first_value(search("2*21"), kResultCalculator) == "42";
	switchDetail << "calculator " << calcOff << calcOn << "; ";
	switchesPass &= calcOff && calcOn;

	toggle(kToggleSearchFiles, false);
	bool filesOff = !has_result(search("searchprobe"), kResultFile, NULL);
	toggle(kToggleSearchFiles, true);
	bool filesOn = has_result(search("searchprobe"), kResultFile, NULL);
	switchDetail << "files " << filesOff << filesOn << "; ";
	switchesPass &= filesOff && filesOn;

	if (haveState) {
		toggle(kToggleRestartInto, false);
		bool restartOff = !has_string(menu(), "shutdown", "Restart into");
		toggle(kToggleRestartInto, true);
		bool restartOn = has_string(menu(), "shutdown", "Restart into");
		switchDetail << "restart into " << restartOff << restartOn << "; ";
		switchesPass &= restartOff && restartOn;
	}

	toggle(kToggleAppRecentDocuments, false);
	BMessage jumpOff;
	ask(jump, jumpOff);
	toggle(kToggleAppRecentDocuments, true);
	BMessage jumpOn;
	ask(jump, jumpOn);
	switchDetail << "recent documents " << !jumpOff.GetBool("menu", true)
		<< jumpOn.GetBool("menu", false);
	switchesPass &= !jumpOff.GetBool("menu", true)
		&& jumpOn.GetBool("menu", false);
	report("switches", switchesPass, "off/on: %s", switchDetail.String());

	// The preferences.
	BMessenger(kDeskbarSignature).SendMessage(kConfigShow);
	snooze(2000000);
	BMessage get(B_GET_PROPERTY);
	get.AddSpecifier("Value");
	get.AddSpecifier("View", "search");
	get.AddSpecifier("Window", "Deskbar preferences");
	BMessage value;
	status_t sent = BMessenger(kDeskbarSignature).SendMessage(&get, &value,
		2000000, 2000000);
	int32 checked = -1;
	value.FindInt32("result", &checked);
	printf("CE-HOST screenshot deskbar_preferences\n");
	fflush(stdout);
	snooze(500000);
	report("preferences", sent == B_OK && checked == 1,
		"sent 0x%x, \"Search at the top of the menu\" value %d (want 1)",
		(unsigned)sent, (int)checked);
	BMessage close(B_QUIT_REQUESTED);
	close.AddSpecifier("Window", "Deskbar preferences");
	BMessage closeReply;
	BMessenger(kDeskbarSignature).SendMessage(&close, &closeReply, 1000000,
		1000000);

	BString command;
	command.SetToFormat("rm -rf \"%s\"", folder.Path());
	system(command.String());

	if (started) {
		BMessenger(kDeskbarSignature).SendMessage(B_QUIT_REQUESTED);
		for (int32 i = 0; i < 40 && be_roster->IsRunning(kDeskbarSignature);
				i++) {
			snooze(250000);
		}
		printf("CE-INFO deskbar: Deskbar %s\n",
			be_roster->IsRunning(kDeskbarSignature) ? "still running"
				: "quit again");
	}

	printf("CE-SUITE %s DONE failures=%d\n", SUITE, sFailures);
	return sFailures == 0 ? 0 : 1;
}
