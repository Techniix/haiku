/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 */

/*!	The Boot menu preferences (roadmap F-22): they write the boot loader's
	"boot_menu_timeout" and "boot_menu_default" lines into the kernel
	settings file and keep every other line of it.

	- From the command line (--set-timeout, --set-default, --show), the way
	  scripts use it.
	- From the window: the test sends the application the messages the
	  controls send (kMsgSetTimeout, kMsgSetDefault), checks the file, reads
	  the controls back through scripting (the checkbox and the spinner) and
	  has the harness save screen-bootmenu_preferences.png.
	- --list runs (and lists what it finds; a full run has no other
	  systems attached).

	The settings file is put back as it was at the end.
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
#include <sys/wait.h>
#include <unistd.h>


#define SUITE "bootmenu"

static const char* kApp = "/boot/system/preferences/BootMenu";
static const char* kSignature = "application/x-vnd.Renga-BootMenu";
static const char* kSettings
	= "/boot/home/config/settings/kernel/drivers/kernel";
static const uint32 kMsgSetTimeout = 'stmo';
static const uint32 kMsgVolumeSystem = 'vsys';
static const uint32 kMsgDefaults = 'rdfl';
static const uint32 kMsgSetDefault = 'sdfl';

static int sFailures = 0;


static void
report(const char* name, bool pass, const char* format, ...)
{
	char detail[300];
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
read_file(const char* path)
{
	BString text;
	FILE* file = fopen(path, "r");
	if (file == NULL)
		return text;
	char buffer[4096];
	size_t bytes;
	while ((bytes = fread(buffer, 1, sizeof(buffer), file)) > 0)
		text.Append(buffer, bytes);
	fclose(file);
	return text;
}


static void
write_file(const char* path, const BString& text)
{
	FILE* file = fopen(path, "w");
	if (file == NULL)
		return;
	fwrite(text.String(), 1, text.Length(), file);
	fclose(file);
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


//!	Whether every line of \a original that is not a boot menu line is in
//!	\a text, in the same order.
static bool
other_lines_kept(const BString& original, const BString& text)
{
	int32 position = 0;
	int32 start = 0;
	while (start < original.Length()) {
		int32 end = original.FindFirst('\n', start);
		if (end < 0)
			end = original.Length();
		BString line;
		original.CopyInto(line, start, end - start);
		start = end + 1;
		if (line.IsEmpty() || line.StartsWith("boot_menu_"))
			continue;
		int32 found = text.FindFirst(line, position);
		if (found < 0)
			return false;
		position = found + line.Length();
	}
	return true;
}


//!	The value of the control named \a view in the window (scripting).
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


int
main()
{
	BApplication app("application/x-vnd.Renga-bootmenu-test");
	printf("CE-SUITE %s START\n", SUITE);

	if (access(kApp, X_OK) != 0) {
		printf("CE-INFO no %s on this image, nothing to test\n", kApp);
		printf("CE-SUITE %s DONE failures=0\n", SUITE);
		return 0;
	}

	BString original = read_file(kSettings);

	// command line
	int status;
	BString command;
	command << kApp << " --set-timeout 7";
	run(command.String(), &status);
	BString text = read_file(kSettings);
	report("cli_timeout", status == 0 && text.FindFirst("\nboot_menu_timeout 7\n") >= 0
		&& text.FindFirst("boot_menu_default") < 0,
		"exit %d, file %s the line", status,
		text.FindFirst("boot_menu_timeout 7") >= 0 ? "has" : "lacks");
	report("cli_keeps_other_lines", other_lines_kept(original, text)
		&& text.FindFirst("load_symbols true") >= 0,
		"%" B_PRId32 " bytes before, %" B_PRId32 " after", original.Length(),
		text.Length());

	command = "";
	command << kApp << " --set-default 'Windows Boot Manager'";
	run(command.String(), &status);
	command = "";
	command << kApp << " --show";
	BString shown = run(command.String());
	text = read_file(kSettings);
	bool defaultOK = status == 0
		&& text.FindFirst("boot_menu_default \"Windows Boot Manager\"") >= 0
		&& shown.FindFirst("timeout 7\ndefault Windows Boot Manager") >= 0;
	report("cli_default", defaultOK, "--show: %s",
		shown.ReplaceAll("\n", "; ").String());

	command = "";
	command << kApp << " --set-timeout 0";
	run(command.String(), &status);
	text = read_file(kSettings);
	// (the file's own documentation has a commented "#boot_menu_timeout")
	bool off = text.FindFirst("\nboot_menu_") < 0;
	report("cli_off", status == 0 && off && other_lines_kept(original, text),
		"menu off: %s", off ? "no boot menu lines left" : "lines left");

	command = "";
	command << kApp << " --list";
	BString list = run(command.String(), &status);
	list.ReplaceAll("\n", "; ");
	report("cli_list", status == 0, "exit %d, %s", status,
		list.IsEmpty() ? "no other systems" : list.String());

	// the window
	team_id team = -1;
	be_roster->Launch(kSignature, (BMessage*)NULL, &team);
	BMessenger messenger(kSignature);
	for (int i = 0; i < 40 && !messenger.IsValid(); i++) {
		snooze(250000);
		messenger = BMessenger(kSignature);
	}
	int32 initialShow = -1000;
	for (int i = 0; i < 40 && initialShow == -1000; i++) {
		snooze(250000);
		initialShow = control_value(messenger, "show");
	}
	report("window_opens", messenger.IsValid() && initialShow == 0,
		"application %s, checkbox %" B_PRId32 " (want 0: menu off)",
		messenger.IsValid() ? "running" : "not running", initialShow);

	BMessage timeout(kMsgSetTimeout);
	timeout.AddInt32("seconds", 4);
	messenger.SendMessage(&timeout);
	BMessage defaultEntry(kMsgSetDefault);
	defaultEntry.AddString("name", "Windows Boot Manager");
	messenger.SendMessage(&defaultEntry);
	snooze(1000000);

	text = read_file(kSettings);
	report("window_writes", text.FindFirst("\nboot_menu_timeout 4\n") >= 0
		&& text.FindFirst("boot_menu_default \"Windows Boot Manager\"") >= 0
		&& other_lines_kept(original, text),
		"file %s timeout 4 and the default",
		text.FindFirst("boot_menu_timeout 4") >= 0 ? "has" : "lacks");
	int32 show = control_value(messenger, "show");
	int32 seconds = control_value(messenger, "timeout");
	report("window_controls", show == 1 && seconds == 4,
		"checkbox %" B_PRId32 ", spinner %" B_PRId32 " (want 1, 4)", show,
		seconds);

	// F-59: "Show the system on each volume", off and on
	BMessage volumeSystem(kMsgVolumeSystem);
	volumeSystem.AddBool("show", false);
	messenger.SendMessage(&volumeSystem);
	snooze(1000000);
	text = read_file(kSettings);
	int32 volumeBox = control_value(messenger, "volumeSystem");
	report("window_volume_system", volumeBox == 0
			&& text.FindFirst("\nboot_menu_volume_system false\n") >= 0
			&& text.FindFirst("boot_menu_timeout 4") >= 0,
		"checkbox %" B_PRId32 ", file %s \"boot_menu_volume_system false\"",
		volumeBox, text.FindFirst("boot_menu_volume_system false") >= 0
			? "has" : "lacks");

	printf("CE-HOST screenshot bootmenu_preferences\n");
	fflush(stdout);
	snooze(1500000);

	messenger.SendMessage(kMsgDefaults);
	snooze(1000000);
	text = read_file(kSettings);
	report("window_defaults", control_value(messenger, "volumeSystem") == 1
			&& control_value(messenger, "show") == 0
			&& text.FindFirst("\nboot_menu_") < 0
			&& other_lines_kept(original, text),
		"after Restore defaults: %s", text.FindFirst("\nboot_menu_") < 0
			? "no boot menu lines" : "boot menu lines left");

	messenger.SendMessage(B_QUIT_REQUESTED);
	snooze(500000);
	if (team >= 0)
		kill(team, SIGKILL);

	write_file(kSettings, original);
	report("restored", read_file(kSettings) == original,
		"settings file put back");

	printf("CE-SUITE %s DONE failures=%d\n", SUITE, sFailures);
	return sFailures != 0;
}
