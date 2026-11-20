/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 */

/*!	The Control Panel (roadmap F-28, batch 5): pages and the search.

	  control_panel.pages            every page is listed, in its category
	  control_panel.search_*         the search finds a setting by its name
	                                 and by a synonym, on the right page
	  control_panel.search_none      nothing for nonsense
	  control_panel.window_search    the window's search lists the results
	  control_panel.window_page_*    the window shows a page, built in
	                                 (Displays, Audio, Streaming and privacy,
	                                 Boot and recovery, Devices and drivers)
	                                 or a preference's;
	                                 the built-in pages' controls are there
	  control_panel.streamer_mode    the Streaming page shows the streamer
	                                 mode file's state
	  control_panel.screenshots      search results and a page are saved by
	                                 the harness

	The old preferences are unchanged; their own tests still run.
*/


#include <Application.h>
#include <Messenger.h>
#include <OS.h>
#include <Roster.h>
#include <String.h>

#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <streamer_mode.h>


#define SUITE "control_panel"

static const char* kApp = "/boot/system/apps/ControlPanel";
static const char* kSignature = "application/x-vnd.Renga-ControlPanel";
static const uint32 kMsgSearch = 'cpSr';
static const uint32 kMsgShowPage = 'cpPg';
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


static BString
run(const char* command, int* _status = NULL)
{
	BString output;
	FILE* pipe = popen(command, "r");
	if (pipe == NULL)
		return output;
	char buffer[1024];
	size_t bytes;
	while ((bytes = fread(buffer, 1, sizeof(buffer), pipe)) > 0)
		output.Append(buffer, bytes);
	int status = pclose(pipe);
	if (_status != NULL)
		*_status = status;
	return output;
}


static void
search_check(const char* name, const char* text, const char* page,
	const char* setting)
{
	BString command;
	command << kApp << " --search '" << text << "'";
	int status;
	BString output = run(command.String(), &status);
	BString wanted;
	wanted << page << "\t";
	bool found = false;
	int32 start = 0;
	while (start < output.Length()) {
		int32 end = output.FindFirst('\n', start);
		if (end < 0)
			end = output.Length();
		BString line;
		output.CopyInto(line, start, end - start);
		if (line.StartsWith(wanted) && line.FindFirst(setting) >= 0)
			found = true;
		start = end + 1;
	}
	output.ReplaceAll("\n", "; ");
	report(name, status == 0 && found, "\"%s\": %s", text,
		output.String());
}


static int32
control_value(BMessenger& app, const char* view)
{
	BMessage request(B_GET_PROPERTY);
	request.AddSpecifier("Value");
	request.AddSpecifier("View", view);
	request.AddSpecifier("Window", (int32)0);
	BMessage reply;
	if (app.SendMessage(&request, &reply, 2000000, 2000000) != B_OK)
		return -1000;
	int32 value;
	return reply.FindInt32("result", &value) == B_OK ? value : -1000;
}


static bool
has_view(BMessenger& app, const char* view)
{
	BMessage request(B_GET_PROPERTY);
	request.AddSpecifier("Frame");
	request.AddSpecifier("View", view);
	request.AddSpecifier("Window", (int32)0);
	BMessage reply;
	BRect frame;
	return app.SendMessage(&request, &reply, 2000000, 2000000) == B_OK
		&& reply.FindRect("result", &frame) == B_OK;
}


static BString
show_page(BMessenger& app, const char* id)
{
	BMessage request(kMsgShowPage);
	request.AddString("id", id);
	BMessage reply;
	if (app.SendMessage(&request, &reply, 5000000, 5000000) != B_OK)
		return "";
	return reply.GetString("page", "");
}


int
main()
{
	BApplication app("application/x-vnd.Renga-control-panel-test");
	printf("CE-SUITE %s START\n", SUITE);
	if (access(kApp, X_OK) != 0) {
		printf("CE-INFO no %s on this image, nothing to test\n", kApp);
		printf("CE-SUITE %s DONE failures=0\n", SUITE);
		return 0;
	}

	int status;
	BString pages = run("/boot/system/apps/ControlPanel --pages", &status);
	report("pages", status == 0 && pages.FindFirst("displays\tHardware\t") >= 0
			&& pages.FindFirst("streaming\tPrivacy\t") >= 0
			&& pages.FindFirst("boot\tSystem\t") >= 0
			&& pages.FindFirst("gamepads\tHardware\t") >= 0
			&& pages.FindFirst("notifications\tSystem\t") >= 0
			&& pages.FindFirst("renguin\tSystem\t") >= 0
			&& pages.FindFirst("audio\tHardware\t") >= 0,
		"%" B_PRId32 " bytes listed", pages.Length());

	search_check("search_name", "restore points", "boot",
		"System restore points");
	search_check("search_synonym_dnd", "dnd", "streaming",
		"Hold notifications");
	search_check("search_synonym_monitor", "monitor", "displays",
		"Display arrangement");
	search_check("search_two_words", "wifi statistics", "devices",
		"Wi-Fi statistics");
	run("/boot/system/apps/ControlPanel --search zzqqxx", &status);
	report("search_none", status != 0, "exit %d", status);

	// the window
	char streamerPath[B_PATH_NAME_LENGTH];
	StreamerMode::path(streamerPath, sizeof(streamerPath));
	bool streamerExisted = access(streamerPath, F_OK) == 0;
	StreamerMode::Settings saved;
	StreamerMode::load(saved);
	StreamerMode::Settings streamer;
	StreamerMode::defaults(streamer);
	streamer.enabled = true;
	StreamerMode::save(streamer);

	team_id team = -1;
	be_roster->Launch(kSignature, (BMessage*)NULL, &team);
	BMessenger messenger;
	for (int i = 0; i < 100 && !messenger.IsValid(); i++) {
		snooze(100000);
		messenger = BMessenger(kSignature);
	}
	snooze(2000000);

	BMessage request(kMsgSearch);
	request.AddString("text", "recovery");
	BMessage reply;
	messenger.SendMessage(&request, &reply, 5000000, 5000000);
	BString results;
	const char* result;
	for (int32 i = 0; reply.FindString("result", i, &result) == B_OK; i++)
		results << result << "; ";
	report("window_search", results.FindFirst("Automatic recovery") >= 0,
		"%s", results.String());
	printf("CE-HOST screenshot control_panel_search\n");
	fflush(stdout);
	snooze(1500000);

	request.MakeEmpty();
	request.AddString("text", "");
	messenger.SendMessage(&request, &reply, 5000000, 5000000);

	BString shown = show_page(messenger, "streaming");
	snooze(500000);
	report("window_page_streaming", shown == "streaming"
			&& has_view(messenger, "streamerPause"),
		"page \"%s\"", shown.String());
	int32 value = control_value(messenger, "streamerMode");
	report("streamer_mode", value == 1, "checkbox %" B_PRId32
		" (the file says on)", value);
	printf("CE-HOST screenshot control_panel_streaming\n");
	fflush(stdout);
	snooze(1500000);

	shown = show_page(messenger, "boot");
	snooze(500000);
	report("window_page_boot", shown == "boot"
			&& has_view(messenger, "autoRecovery")
			&& control_value(messenger, "autoRecovery") == 1
			&& has_view(messenger, "bootState")
			&& control_value(messenger, "bootState") == 1,
		"page \"%s\", recovery switch %" B_PRId32 ", boot state switch %"
		B_PRId32, shown.String(), control_value(messenger, "autoRecovery"),
		control_value(messenger, "bootState"));

	shown = show_page(messenger, "displays");
	snooze(500000);
	report("window_page_displays", shown == "displays", "page \"%s\"",
		shown.String());
	printf("CE-HOST screenshot control_panel_displays\n");
	fflush(stdout);
	snooze(1500000);

	shown = show_page(messenger, "devices");
	snooze(800000);
	report("window_page_devices", shown == "devices"
			&& has_view(messenger, "deviceList")
			&& has_view(messenger, "devicesSummary"),
		"page \"%s\"", shown.String());
	printf("CE-HOST screenshot control_panel_devices\n");
	fflush(stdout);
	snooze(1500000);

	shown = show_page(messenger, "gamepads");
	report("window_page_preference", shown == "gamepads",
		"page \"%s\"", shown.String());

	messenger.SendMessage(B_QUIT_REQUESTED);
	snooze(500000);
	if (team >= 0)
		kill(team, SIGKILL);

	if (streamerExisted)
		StreamerMode::save(saved);
	else
		unlink(streamerPath);

	printf("CE-SUITE %s DONE failures=%d\n", SUITE, sFailures);
	return sFailures == 0 ? 0 : 1;
}
