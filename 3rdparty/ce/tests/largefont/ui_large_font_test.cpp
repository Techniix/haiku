/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 */

/*!	Every Renga-added window at a large system font (batch 12).

	The maintainer runs Renga with a larger system font than QEMU's default
	12 points; labels were cut off and windows did not fit. This suite sets
	the plain, bold and menu fonts to 20 points and the screen to 1024x768,
	starts each Renga program with the layout audit loaded
	(liblayout_audit.so through LD_PRELOAD, see layout_audit.cpp), and lets
	the audit check every shown window, page and tab: nothing clipped (each
	label fits its frame, each menu field shows its whole item, each list
	item fits its list, each window fits the screen). A screenshot of every
	state is saved by the harness ("large_font_<name>"). The fonts and the
	screen mode are put back at the end.

	  large_font.setup              fonts at 20 points, screen at 1024x768
	  large_font.<program>[_<page>] the audit found nothing clipped; the
	                                detail lists the first problems (all are
	                                in the serial log as "CE-INFO large_font"
	                                lines)
	  large_font.restore            fonts and screen mode put back

	Programs: Control Panel (every page), Welcome (every page), Renguin
	Manager, Audio (every page), Gamepads, LinuxKPI Manager, Boot menu, Renga
	Doctor, the session bus prompt, the Installer's wizard (every page; the
	Summary page for upgrading "CETarget" when installer_ui_test's disk is
	attached, otherwise it says so), Deskbar preferences and Tracker
	preferences (Deskbar and Tracker are restarted with the audit loaded,
	then restarted normally).
*/


#include <Application.h>
#include <Font.h>
#include <Menu.h>
#include <Messenger.h>
#include <OS.h>
#include <Roster.h>
#include <Screen.h>
#include <String.h>

#include <errno.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <vector>


#define SUITE "large_font"

extern void _set_system_font_(const char* which, font_family family,
	font_style style, float size);

static const char* kAuditLibrary
	= "/boot/system/non-packaged/lib/liblayout_audit.so";
static const float kLargeSize = 20.0f;
static int sFailures = 0;
static int sAuditNumber = 0;


static void
report(const char* name, bool pass, const char* format, ...)
{
	char detail[700];
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
run(const char* command)
{
	BString output;
	FILE* pipe = popen(command, "r");
	if (pipe == NULL)
		return output;
	char buffer[1024];
	size_t bytes;
	while ((bytes = fread(buffer, 1, sizeof(buffer), pipe)) > 0)
		output.Append(buffer, bytes);
	pclose(pipe);
	return output;
}


static bool
running(const char* signature)
{
	return be_roster->IsRunning(signature);
}


static bool
wait_running(const char* signature, bool wanted, int tenths)
{
	for (int i = 0; i < tenths; i++) {
		if (running(signature) == wanted)
			return true;
		snooze(100000);
	}
	return running(signature) == wanted;
}


/*!	Starts \a path with the audit loaded; \a directory is the audit's. */
static pid_t
start_audited(const char* directory, const char* path,
	const std::vector<const char*>& arguments)
{
	mkdir(directory, 0755);
	pid_t pid = fork();
	if (pid == 0) {
		setenv("CE_LAYOUT_AUDIT", directory, 1);
		setenv("LD_PRELOAD", kAuditLibrary, 1);
		std::vector<const char*> argv;
		argv.push_back(path);
		for (size_t i = 0; i < arguments.size(); i++)
			argv.push_back(arguments[i]);
		argv.push_back(NULL);
		execv(path, (char* const*)&argv[0]);
		_exit(127);
	}
	return pid;
}


/*!	Asks the audit in \a directory for a report; returns its text. */
static BString
request_audit(const char* directory, const char* tag)
{
	BString file;
	file << directory << "/" << tag << ".txt";
	unlink(file.String());
	BString request;
	request << directory << "/request";
	FILE* out = fopen(request.String(), "w");
	if (out == NULL)
		return "";
	fprintf(out, "%s\n", tag);
	fclose(out);
	for (int i = 0; i < 150; i++) {
		snooze(100000);
		FILE* in = fopen(file.String(), "r");
		if (in == NULL)
			continue;
		BString text;
		char line[1024];
		while (fgets(line, sizeof(line), in) != NULL)
			text << line;
		fclose(in);
		return text;
	}
	return "";
}


/*!	One audit of the program's windows as they are now: a check, a
	screenshot, every problem in the log.
*/
static void
audit(const char* directory, const char* check,
	const char* onlyWindow = NULL)
{
	BString tag;
	tag << "audit" << sAuditNumber++;
	snooze(700000);
	BString text = request_audit(directory, tag.String());
	printf("CE-HOST screenshot large_font_%s\n", check);
	fflush(stdout);
	snooze(1200000);

	if (text.FindFirst("END windows") < 0) {
		report(check, false, "no audit report (program not running, or the "
			"audit library not loaded)");
		return;
	}
	int32 windows = 0;
	int32 problems = 0;
	BString first;
	int32 start = 0;
	while (start < text.Length()) {
		int32 end = text.FindFirst('\n', start);
		if (end < 0)
			end = text.Length();
		BString line;
		text.CopyInto(line, start, end - start);
		start = end + 1;
		if (onlyWindow != NULL && (line.StartsWith("PROBLEM ")
				|| line.StartsWith("WINDOW "))
			&& line.IFindFirst(onlyWindow) < 0) {
			// another window of the program (Deskbar's own bar, Tracker's
			// desktop): not what this check is about
			continue;
		}
		if (line.StartsWith("PROBLEM ")) {
			problems++;
			printf("CE-INFO large_font %s: %s\n", check, line.String() + 8);
			if (first.Length() < 420) {
				BString shown = line.String() + 8;
				shown.ReplaceAll('\t', ' ');
				first << (first.IsEmpty() ? "" : "; ") << shown;
			}
		} else if (line.StartsWith("WINDOW ")) {
			windows++;
			printf("CE-INFO large_font %s: %s\n", check, line.String());
		} else if (line.StartsWith("SHORTENED ") || line.StartsWith("WIDE ")
			|| line.StartsWith("TALL ") || line.StartsWith("CHAIN "))
			printf("CE-INFO large_font %s: %s\n", check, line.String());
	}
	fflush(stdout);
	if (windows == 0) {
		report(check, false, "no window shown");
		return;
	}
	report(check, problems == 0, "%" B_PRId32 " window(s), %" B_PRId32
		" clipped%s%s", windows, problems, problems > 0 ? ": " : "",
		first.String());
}


static void
quit(const char* signature, pid_t pid)
{
	BMessenger messenger(signature);
	if (messenger.IsValid())
		messenger.SendMessage(B_QUIT_REQUESTED);
	for (int i = 0; i < 30 && running(signature); i++)
		snooze(100000);
	if (pid > 0) {
		if (running(signature))
			kill(pid, SIGKILL);
		waitpid(pid, NULL, WNOHANG);
	}
	wait_running(signature, false, 30);
}


static BMessenger
wait_messenger(const char* signature)
{
	BMessenger messenger;
	for (int i = 0; i < 150 && !messenger.IsValid(); i++) {
		snooze(100000);
		messenger = BMessenger(signature);
	}
	return messenger;
}


/*!	A program with one window (or several shown at once). */
static void
audit_program(const char* check, const char* path, const char* signature,
	const std::vector<const char*>& arguments = std::vector<const char*>())
{
	BString directory;
	directory << "/tmp/ce-large-font-" << check;
	pid_t pid = start_audited(directory.String(), path, arguments);
	BMessenger messenger = wait_messenger(signature);
	if (!messenger.IsValid()) {
		report(check, false, "%s did not start", path);
		if (pid > 0)
			kill(pid, SIGKILL);
		return;
	}
	snooze(2500000);
	audit(directory.String(), check);
	quit(signature, pid);
}


/*!	A program with pages chosen by a message with an "id" field. */
static void
audit_pages(const char* name, const char* path, const char* signature,
	uint32 what, const std::vector<BString>& pages)
{
	BString directory;
	directory << "/tmp/ce-large-font-" << name;
	pid_t pid = start_audited(directory.String(), path,
		std::vector<const char*>());
	BMessenger messenger = wait_messenger(signature);
	if (!messenger.IsValid()) {
		report(name, false, "%s did not start", path);
		if (pid > 0)
			kill(pid, SIGKILL);
		return;
	}
	snooze(2500000);
	for (size_t i = 0; i < pages.size(); i++) {
		BMessage show(what);
		show.AddString("id", pages[i].String());
		BMessage reply;
		messenger.SendMessage(&show, &reply, 5000000, 5000000);
		BString check;
		check << name << "_" << pages[i];
		audit(directory.String(), check.String());
	}
	quit(signature, pid);
}


/*!	Deskbar or Tracker: stopped as a launch_daemon service, started with
	the audit, asked to show its preferences, then restarted normally.
*/
static void
audit_service_preferences(const char* check, const char* job,
	const char* path, const char* signature, uint32 showWhat)
{
	BString directory;
	directory << "/tmp/ce-large-font-" << check;
	BString command;
	command << "launch_roster stop " << job << " 2>&1";
	run(command.String());
	if (!wait_running(signature, false, 50)) {
		BMessenger(signature).SendMessage(B_QUIT_REQUESTED);
		wait_running(signature, false, 50);
	}
	pid_t pid = start_audited(directory.String(), path,
		std::vector<const char*>());
	BMessenger messenger = wait_messenger(signature);
	if (messenger.IsValid()) {
		snooze(3000000);
		messenger.SendMessage(showWhat);
		snooze(1500000);
		audit(directory.String(), check, "preferences");
	} else
		report(check, false, "%s did not start with the audit", path);

	// back to the normal program, started by launch_daemon
	messenger.SendMessage(B_QUIT_REQUESTED);
	if (!wait_running(signature, false, 50) && pid > 0)
		kill(pid, SIGKILL);
	waitpid(pid, NULL, WNOHANG);
	wait_running(signature, false, 30);
	command = "";
	command << "launch_roster start " << job << " 2>&1";
	run(command.String());
	if (!wait_running(signature, true, 100)) {
		// last resort: start it by hand, so that later suites have it
		be_roster->Launch(signature);
		wait_running(signature, true, 100);
	}
	printf("CE-INFO large_font: %s running again: %s\n", signature,
		running(signature) ? "yes" : "no");
	fflush(stdout);
}


static std::vector<BString>
lines_first_field(const BString& text)
{
	std::vector<BString> result;
	int32 start = 0;
	while (start < text.Length()) {
		int32 end = text.FindFirst('\n', start);
		if (end < 0)
			end = text.Length();
		BString line;
		text.CopyInto(line, start, end - start);
		start = end + 1;
		int32 tab = line.FindFirst('\t');
		if (tab > 0)
			line.Truncate(tab);
		if (line.Length() > 0)
			result.push_back(line);
	}
	return result;
}


struct SavedFont {
	font_family	family;
	font_style	style;
	float		size;

	void Save(const BFont* font)
	{
		font->GetFamilyAndStyle(&family, &style);
		size = font->Size();
	}
};


int
main()
{
	BApplication app("application/x-vnd.Renga-LargeFontTest");

	// the fonts as they are now (this program read them at start)
	SavedFont plain, bold;
	plain.Save(be_plain_font);
	bold.Save(be_bold_font);
	menu_info menuInfo;
	get_menu_info(&menuInfo);
	menu_info savedMenu = menuInfo;

	BScreen screen;
	display_mode savedMode;
	bool haveMode = screen.GetMode(&savedMode) == B_OK;
	bool modeSet = false;
	display_mode* modes = NULL;
	uint32 count = 0;
	if (haveMode && screen.GetModeList(&modes, &count) == B_OK) {
		for (uint32 i = 0; i < count; i++) {
			if (modes[i].virtual_width == 1024
				&& modes[i].virtual_height == 768
				&& modes[i].space == savedMode.space) {
				modeSet = screen.SetMode(&modes[i]) == B_OK;
				break;
			}
		}
		free(modes);
	}
	_set_system_font_("plain", plain.family, plain.style, kLargeSize);
	_set_system_font_("bold", bold.family, bold.style, kLargeSize);
	menuInfo.font_size = kLargeSize;
	set_menu_info(&menuInfo);
	snooze(1500000);
	BRect frame = screen.Frame();
	report("setup", modeSet && frame.Width() + 1 == 1024
			&& frame.Height() + 1 == 768,
		"screen %.0fx%.0f (was %ux%u), plain and bold %.0f points "
		"(were %.1f, %.1f), menu %.0f points", frame.Width() + 1,
		frame.Height() + 1, savedMode.virtual_width, savedMode.virtual_height,
		kLargeSize, plain.size, bold.size, kLargeSize);

	// Control Panel: every page
	std::vector<BString> pages = lines_first_field(
		run("/boot/system/apps/ControlPanel --pages"));
	audit_pages("control_panel", "/boot/system/apps/ControlPanel",
		"application/x-vnd.Renga-ControlPanel", 'cpPg', pages);

	// Welcome: every page
	const char* welcomePages[] = { "welcome", "network", "graphics",
		"software", "linux", "gamepads", "setup", "tips" };
	pages.clear();
	for (size_t i = 0; i < sizeof(welcomePages) / sizeof(welcomePages[0]); i++)
		pages.push_back(welcomePages[i]);
	audit_pages("welcome", "/boot/system/apps/Welcome",
		"application/x-vnd.Renga-Welcome", 'wlPg', pages);

	audit_program("renguin_manager", "/boot/system/apps/RenguinManager",
		"application/x-vnd.Renga-RenguinManager");

	// Audio: every page, through "--page" to the running program
	{
		const char* audioPages[] = { "output", "input", "devices",
			"applications", "microphone", "mixing", "presets" };
		const char* directory = "/tmp/ce-large-font-audio";
		const char* path = "/boot/system/preferences/Audio";
		const char* signature = "application/x-vnd.Renga-Audio";
		std::vector<const char*> arguments;
		pid_t pid = start_audited(directory, path, arguments);
		BMessenger messenger = wait_messenger(signature);
		if (!messenger.IsValid())
			report("audio", false, "Audio did not start");
		else {
			snooze(3000000);
			for (size_t i = 0; i < sizeof(audioPages) / sizeof(audioPages[0]);
					i++) {
				BString command;
				command << path << " --page " << audioPages[i];
				run(command.String());
				BString check;
				check << "audio_" << audioPages[i];
				audit(directory, check.String());
			}
		}
		quit(signature, pid);
	}

	audit_program("gamepads", "/boot/system/preferences/Gamepads",
		"application/x-vnd.Renga-Gamepads");
	audit_program("linuxkpi_manager", "/boot/system/apps/LinuxKPIManager",
		"application/x-vnd.LinuxKPI-Manager");
	audit_program("boot_menu", "/boot/system/preferences/BootMenu",
		"application/x-vnd.Renga-BootMenu");
	audit_program("renga_doctor", "/boot/system/apps/RengaDoctor",
		"application/x-vnd.Renga-Doctor");
	{
		std::vector<const char*> arguments;
		arguments.push_back("/boot/home/config/non-packaged/bin/"
			"a-program-with-a-long-name-for-the-prompt");
		audit_program("session_bus_prompt",
			"/boot/system/servers/session_bus_prompt",
			"application/x-vnd.Renga-SessionBusPrompt", arguments);
	}

	// the Installer's wizard: every page, each through "--page" (the
	// Installer is not asked through messengers, see installer_ui_test)
	{
		bool target = run("/boot/system/apps/Installer --list-targets")
			.FindFirst("CETarget\t") >= 0;
		const char* wizardPages[] = { "welcome", "destination", "summary",
			"progress", "finish" };
		for (size_t i = 0; i < sizeof(wizardPages) / sizeof(wizardPages[0]);
				i++) {
			BString check;
			check << "installer_wizard_" << wizardPages[i];
			if (strcmp(wizardPages[i], "summary") == 0 && !target) {
				printf("CE-INFO large_font: %s not audited: no CETarget "
					"disk (run with installer_ui_test)\n", check.String());
				continue;
			}
			std::vector<const char*> arguments;
			arguments.push_back("--wizard");
			arguments.push_back("--page");
			arguments.push_back(wizardPages[i]);
			if (target) {
				arguments.push_back("--choice");
				arguments.push_back("existing");
				arguments.push_back("--target");
				arguments.push_back("CETarget");
				arguments.push_back("--mode");
				arguments.push_back("upgrade");
			}
			// no messenger to the Installer (looking it up hung the
			// guest, as in installer_ui_test): a fixed wait, then kill
			BString directory;
			directory << "/tmp/ce-large-font-" << check;
			pid_t pid = start_audited(directory.String(),
				"/boot/system/apps/Installer", arguments);
			snooze(6000000);
			audit(directory.String(), check.String());
			if (pid > 0) {
				kill(pid, SIGKILL);
				waitpid(pid, NULL, 0);
			}
		}
	}

	audit_service_preferences("deskbar_preferences", "x-vnd.Be-TSKB",
		"/boot/system/Deskbar", "application/x-vnd.Be-TSKB", 'PrSh');
	audit_service_preferences("tracker_preferences", "x-vnd.Be-TRAK",
		"/boot/system/Tracker", "application/x-vnd.Be-TRAK", 'Tstw');

	// put everything back
	_set_system_font_("plain", plain.family, plain.style, plain.size);
	_set_system_font_("bold", bold.family, bold.style, bold.size);
	set_menu_info(&savedMenu);
	bool modeBack = !modeSet || screen.SetMode(&savedMode) == B_OK;
	snooze(1000000);
	frame = screen.Frame();
	report("restore", modeBack && frame.Width() + 1 == savedMode.virtual_width,
		"screen %.0fx%.0f, plain %.1f points", frame.Width() + 1,
		frame.Height() + 1, plain.size);

	return sFailures == 0 ? 0 : 1;
}
