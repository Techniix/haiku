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
	  Compatibility tabs (screen-linuxkpi_<tab>.png). It has its own vector
	  icon, which draws (manager_icon; Renga icon rule, roadmap). Its
	  settings window for mt7921e ("--settings mt7921e", as Drivers ->
	  Settings... opens it) shows the manifest's tunables: the "Card does
	  the encryption" check box and the Save button are there, inside the
	  window, and the window fits on the screen (settings_*; screen-linuxkpi_settings_mt7921e.png).
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


#include <AppFileInfo.h>
#include <Application.h>
#include <Bitmap.h>
#include <File.h>
#include <IconUtils.h>
#include <Messenger.h>
#include <OS.h>
#include <Roster.h>
#include <Screen.h>
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


//!	The application's vector icon draws, and in colour (not a placeholder).
static void
test_manager_icon()
{
	BFile file(kManager, B_READ_ONLY);
	BAppFileInfo info(&file);
	uint8* data = NULL;
	size_t size = 0;
	status_t status = info.InitCheck() == B_OK
		? info.GetIcon(&data, &size) : info.InitCheck();
	int32 opaque = 0;
	int32 green = 0;
	if (status == B_OK) {
		BBitmap bitmap(BRect(0, 0, 31, 31), B_RGBA32);
		status = BIconUtils::GetVectorIcon(data, size, &bitmap);
		const uint8* bits = (const uint8*)bitmap.Bits();
		for (int32 i = 0; status == B_OK && i < (int32)bitmap.BitsLength();
				i += 4) {
			// B_RGBA32 is B, G, R, A in memory
			if (bits[i + 3] > 0x80) {
				opaque++;
				if (bits[i + 1] > bits[i + 2] + 20 && bits[i + 1] > bits[i] + 20)
					green++;
			}
		}
	}
	delete[] data;
	report("manager_icon", status == B_OK && opaque > 200 && green > 50,
		"vector icon: %s, %" B_PRIuSIZE " bytes; at 32 x 32 %" B_PRId32
		" opaque pixels, %" B_PRId32 " of them green", strerror(status), size,
		opaque, green);
}


/*!	Drivers -> mt7921e -> Settings... (here "--settings mt7921e", the same
	window): the manifest's tunables are shown as controls. On the
	maintainer's PC the window showed only its note and buttons; the check
	asks the window for the hardware encryption check box by name, its
	label, and that it lies inside the window, which lies on the screen.
*/
static void
test_settings_window(BMessenger& app)
{
	BString command;
	command << kManager << " --settings mt7921e &";
	system(command.String());

	int32 index = -1;
	for (int attempt = 0; attempt < 40 && index < 0; attempt++) {
		snooze(250000);
		for (int32 i = 0; i < 8; i++) {
			BMessage request(B_GET_PROPERTY);
			request.AddSpecifier("Title");
			request.AddSpecifier("Window", i);
			BMessage reply;
			if (app.SendMessage(&request, &reply, 2000000, 2000000) != B_OK)
				break;
			if (BString(reply.GetString("result", "")) == "mt7921e settings") {
				index = i;
				break;
			}
		}
	}
	report("settings_window_opens", index >= 0,
		"window \"mt7921e settings\" %s", index >= 0 ? "found" : "missing");
	if (index < 0)
		return;
	snooze(1000000);
	screenshot("linuxkpi_settings_mt7921e");

	static const char* kBox = "compat.linuxkpi.80211.hw_crypto";
	BMessage request(B_GET_PROPERTY);
	request.AddSpecifier("Label");
	request.AddSpecifier("View", kBox);
	request.AddSpecifier("Window", index);
	BMessage reply;
	BString label;
	if (app.SendMessage(&request, &reply, 2000000, 2000000) == B_OK)
		label = reply.GetString("result", "");

	BRect boxFrame;
	request.MakeEmpty();
	request.what = B_GET_PROPERTY;
	request.AddSpecifier("Frame");
	request.AddSpecifier("View", kBox);
	request.AddSpecifier("Window", index);
	reply.MakeEmpty();
	if (app.SendMessage(&request, &reply, 2000000, 2000000) == B_OK)
		reply.FindRect("result", &boxFrame);

	BRect windowFrame;
	request.MakeEmpty();
	request.what = B_GET_PROPERTY;
	request.AddSpecifier("Frame");
	request.AddSpecifier("Window", index);
	reply.MakeEmpty();
	if (app.SendMessage(&request, &reply, 2000000, 2000000) == B_OK)
		reply.FindRect("result", &windowFrame);

	// the Save button below the settings is inside the window too (the
	// window was once too short for its wrapped descriptions)
	BRect saveFrame;
	request.MakeEmpty();
	request.what = B_GET_PROPERTY;
	request.AddSpecifier("Frame");
	request.AddSpecifier("View", "save");
	request.AddSpecifier("Window", index);
	reply.MakeEmpty();
	if (app.SendMessage(&request, &reply, 2000000, 2000000) == B_OK)
		reply.FindRect("result", &saveFrame);

	BRect screen = BScreen().Frame();
	bool saveInside = saveFrame.IsValid() && windowFrame.IsValid()
		&& saveFrame.bottom <= windowFrame.Height() + 1
		&& saveFrame.right <= windowFrame.Width() + 1;
	bool inside = saveInside && boxFrame.IsValid() && boxFrame.Width() > 20
		&& boxFrame.Height() > 5 && windowFrame.IsValid()
		&& boxFrame.right <= windowFrame.Width() + 1
		&& boxFrame.bottom <= windowFrame.Height() + 1;
	report("settings_hw_crypto_box", label == "Card does the encryption"
			&& inside,
		"label \"%s\", box (%.0f,%.0f)-(%.0f,%.0f), Save button bottom "
		"%.0f, in a %.0fx%.0f window", label.String(), boxFrame.left,
		boxFrame.top, boxFrame.right, boxFrame.bottom, saveFrame.bottom,
		windowFrame.Width(), windowFrame.Height());
	report("settings_window_fits", windowFrame.IsValid()
			&& screen.Contains(windowFrame),
		"window (%.0f,%.0f)-(%.0f,%.0f), screen %.0fx%.0f",
		windowFrame.left, windowFrame.top, windowFrame.right,
		windowFrame.bottom, screen.Width() + 1, screen.Height() + 1);
	// the window closes with the application (test_manager())
}


static void
test_manager()
{
	if (access(kManager, X_OK) != 0) {
		report("manager_installed", false, "%s missing", kManager);
		return;
	}
	test_manager_icon();
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

	test_settings_window(app);

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
