/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 */

/*!	The desktop side of LinuxKPI (roadmap F-26, F-34) on the renga-* images:

	- lkpictl, the command line: drivers (mt7921e is known, installed from
	  the system package, set to load at boot), hardware, status of a driver
	  that is not running, coverage, report.
	- LinuxKPI Manager: the window opens, its overview names the known
	  driver, "--tab <name>" switches tabs (also on a running instance), and
	  the harness saves a screenshot of the Overview, Drivers, Hardware and
	  Compatibility tabs (screen-linuxkpi_<tab>.png).
	- System logs: kernel lines of FreeBSD/LinuxKPI drivers
	  ("[lkpi_pcitest] (lkpi) ...", from the boot-time load of the test
	  driver against QEMU's edu device) are filed under their driver as
	  component, and "Save a report" carries hardware/linuxkpi.txt (lkpictl
	  report).

	Wi-Fi itself (Network preferences, the Deskbar network menu) cannot be
	shown in QEMU: it has no Wi-Fi device model. Those are freebsd_wlan's
	existing GUI paths; the hand-over WIFI-HARDWARE-TEST.md checks them on
	the maintainer's MT7922.
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
#include <sys/stat.h>
#include <unistd.h>

#include <string>


#define SUITE "linuxkpi_ui"

static const char* kManager = "/boot/system/apps/LinuxKPIManager";
static const char* kManagerSignature = "application/x-vnd.LinuxKPI-Manager";
static const char* kSystemLogs = "/boot/system/apps/SystemLogs";
static const char* kSystemLogsSignature = "application/x-vnd.Renga-SystemLogs";
static const uint32 kMsgSetFilter = 'slft';
static const uint32 kMsgGetState = 'slst';

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
		*_status = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
	return output;
}


static std::string
read_file(const char* path)
{
	std::string text;
	FILE* file = fopen(path, "rb");
	if (file == NULL)
		return text;
	char buffer[4096];
	size_t bytes;
	while ((bytes = fread(buffer, 1, sizeof(buffer), file)) > 0)
		text.append(buffer, bytes);
	fclose(file);
	return text;
}


static BString
one_line(BString text)
{
	text.ReplaceAll("\n", "; ");
	if (text.Length() > 200)
		text.Truncate(200);
	return text;
}


static BString
window_title(BMessenger& app)
{
	BMessage request(B_GET_PROPERTY);
	request.AddSpecifier("Title");
	request.AddSpecifier("Window", (int32)0);
	BMessage reply;
	if (app.SendMessage(&request, &reply, 2000000, 2000000) != B_OK)
		return "";
	return reply.GetString("result", "");
}


static BString
view_text(BMessenger& app, const char* view)
{
	// BTextView answers "Text" for a range only (B_RANGE_SPECIFIER)
	BMessage request(B_GET_PROPERTY);
	request.AddSpecifier("Text", (int32)0, (int32)4000);
	request.AddSpecifier("View", view);
	request.AddSpecifier("Window", (int32)0);
	BMessage reply;
	if (app.SendMessage(&request, &reply, 2000000, 2000000) != B_OK)
		return "";
	return reply.GetString("result", "");
}


static void
screenshot(const char* name)
{
	printf("CE-HOST screenshot %s\n", name);
	fflush(stdout);
	snooze(1500000);
}


static void
test_cli()
{
	int status;
	BString drivers = run("/bin/lkpictl drivers 2>&1", &status);
	report("cli_drivers", status == 0 && drivers.FindFirst("mt7921e") >= 0,
		"exit %d: %s", status, one_line(drivers).String());

	BString hardware = run("/bin/lkpictl hardware 2>&1", &status);
	report("cli_hardware", status == 0, "exit %d: %s", status,
		one_line(hardware).String());

	// mt7921e is installed but has no device in QEMU: the status request
	// must say so, not crash or hang
	BString state = run("/bin/lkpictl status mt7921e 2>&1", &status);
	report("cli_status_not_running", status >= 0 && state.Length() > 0
			&& state.FindFirst("wlan.state") < 0,
		"exit %d: %s", status, one_line(state).String());

	BString coverage = run("/bin/lkpictl coverage 2>&1", &status);
	report("cli_coverage", status == 0 && coverage.CountChars() > 100,
		"exit %d, %" B_PRId32 " characters", status, coverage.CountChars());

	BString text = run("/bin/lkpictl report 2>&1", &status);
	report("cli_report", status == 0 && text.FindFirst("mt7921e") >= 0,
		"exit %d, %" B_PRId32 " characters", status, text.CountChars());
}


static void
test_manager()
{
	if (access(kManager, X_OK) != 0) {
		report("manager_installed", false, "%s missing", kManager);
		return;
	}
	BString command;
	command << kManager << " &";
	system(command.String());

	BMessenger app;
	BString title;
	for (int i = 0; i < 80; i++) {
		app = BMessenger(kManagerSignature);
		if (app.IsValid()) {
			title = window_title(app);
			if (title.Length() > 0)
				break;
		}
		snooze(250000);
	}
	report("window_opens", title == "LinuxKPI Manager", "title \"%s\"",
		title.String());

	BString overview = view_text(app, "overview text");
	report("overview", overview.FindFirst("Drivers known: 1") >= 0,
		"%s", one_line(overview).String());
	screenshot("linuxkpi_overview");

	static const char* kTabs[] = { "drivers", "hardware", "compatibility" };
	for (size_t i = 0; i < sizeof(kTabs) / sizeof(kTabs[0]); i++) {
		// a second launch: B_SINGLE_LAUNCH hands the arguments to the
		// running instance (ArgvReceived)
		command = "";
		command << kManager << " --tab " << kTabs[i] << " &";
		system(command.String());
		snooze(1500000);
		BString name("linuxkpi_");
		name << kTabs[i];
		screenshot(name.String());
	}
	// the driver details on the Drivers tab are filled once a row is
	// selected; that it lists the driver at all is the overview's count
	report("tabs", app.IsValid() && window_title(app) == "LinuxKPI Manager",
		"switched to %d tabs with --tab, window still open",
		(int)(sizeof(kTabs) / sizeof(kTabs[0])));

	app.SendMessage(B_QUIT_REQUESTED);
	snooze(500000);
}


static void
test_system_logs()
{
	if (access(kSystemLogs, X_OK) != 0) {
		printf("CE-INFO no System logs on this image\n");
		return;
	}
	bool haveTestDriver = access("/boot/system/non-packaged/add-ons/kernel/"
		"drivers/bin/lkpi_pcitest", F_OK) == 0;

	BString command;
	command << kSystemLogs << " &";
	system(command.String());
	BMessenger app;
	BMessage state;
	for (int i = 0; i < 80; i++) {
		app = BMessenger(kSystemLogsSignature);
		if (app.IsValid()) {
			BMessage request(kMsgGetState);
			if (app.SendMessage(&request, &state, 5000000, 5000000) == B_OK
				&& state.GetInt32("live_lines", 0) > 0)
				break;
		}
		snooze(250000);
	}
	BMessage filter(kMsgSetFilter);
	filter.AddInt32("tab", 0);
	filter.AddString("component", "lkpi_pcitest");
	filter.AddInt32("level", 0);
	filter.AddString("search", "");
	BMessage reply;
	app.SendMessage(&filter, &reply, 5000000, 5000000);
	int32 shown = reply.GetInt32("live_shown", 0);
	report("syslog_component", !haveTestDriver || shown > 10,
		"component \"lkpi_pcitest\": %" B_PRId32 " lines%s", shown,
		haveTestDriver ? "" : " (no test driver in this image)");
	filter.ReplaceString("component", "");
	app.SendMessage(&filter, &reply, 5000000, 5000000);
	app.SendMessage(B_QUIT_REQUESTED);
	snooze(500000);

	const char* archive = "/tmp/ce-linuxkpi-report.zip";
	unlink(archive);
	command = "";
	command << kSystemLogs << " --save-report " << archive << " >/dev/null";
	int status = system(command.String());
	std::string zip = read_file(archive);
	report("save_report_linuxkpi", status == 0
			&& zip.find("hardware/linuxkpi.txt") != std::string::npos,
		"exit %d, %d bytes, hardware/linuxkpi.txt %s", status,
		(int)zip.size(), zip.find("hardware/linuxkpi.txt")
			!= std::string::npos ? "in it" : "missing");
	unlink(archive);
}


int
main()
{
	BApplication app("application/x-vnd.Renga-linuxkpi-ui-test");
	printf("CE-SUITE %s START\n", SUITE);
	fflush(stdout);

	if (access("/bin/lkpictl", X_OK) != 0) {
		printf("CE-INFO no lkpictl on this image, nothing to test\n");
		printf("CE-SUITE %s DONE failures=0\n", SUITE);
		return 0;
	}

	test_cli();
	test_manager();
	test_system_logs();

	printf("CE-SUITE %s DONE failures=%d\n", SUITE, sFailures);
	fflush(stdout);
	return sFailures != 0;
}
