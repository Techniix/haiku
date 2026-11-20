/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 */

/*!	The first-boot welcome (batch 5).

	  welcome.status          --status reports what it finds: network
	                          devices, NVIDIA and Quasar, the Renga
	                          repository, Renguin and the Linux root, the
	                          shortcut key
	  welcome.pages           the window shows each of its eight pages
	  welcome.graphics        without an NVIDIA card, "Install Quasar" is
	                          disabled and the page says so
	  welcome.shortcut_key    the setup page marks the shortcut key in use
	  welcome.launch_job      the user launch job is installed and checks
	                          the "done" file and read-only media
	  welcome.screenshots     the graphics and setup pages are saved
	  welcome.quasar_absent   no repository has quasar: --status says so,
	                          and with an NVIDIA card (assumed through
	                          --test-assume-nvidia, QEMU has none)
	                          the page shows the build instructions button,
	                          not "Install Quasar"
	  welcome.quasar_offered  a test repository with a quasar package:
	                          --status finds it and the page shows
	                          "Install Quasar"

	Harness images carry the "done" file, so the welcome does not cover the
	desktop at login; this test opens it itself.
*/


#include <Application.h>
#include <Messenger.h>
#include <OS.h>
#include <Roster.h>
#include <String.h>

#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>


#define SUITE "welcome"

static const char* kApp = "/boot/system/apps/Welcome";
static const char* kSignature = "application/x-vnd.Renga-Welcome";
static const uint32 kMsgShowPage = 'wlPg';
static int sFailures = 0;


static void
report(const char* name, bool pass, const char* format, ...)
{
	char detail[500];
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


static int32
control_value(BMessenger& app, const char* view, const char* property)
{
	BMessage request(B_GET_PROPERTY);
	request.AddSpecifier(property);
	request.AddSpecifier("View", view);
	request.AddSpecifier("Window", (int32)0);
	BMessage reply;
	if (app.SendMessage(&request, &reply, 2000000, 2000000) != B_OK)
		return -1000;
	int32 value;
	bool enabled;
	if (reply.FindInt32("result", &value) == B_OK)
		return value;
	if (reply.FindBool("result", &enabled) == B_OK)
		return enabled ? 1 : 0;
	return -1000;
}


static BString
show_page(BMessenger& app, const char* id)
{
	BMessage request(kMsgShowPage);
	request.AddString("id", id);
	BMessage reply;
	status_t status = app.SendMessage(&request, &reply, 5000000, 5000000);
	printf("CE-INFO welcome: page %s: %s, reply '%.4s'\n", id,
		strerror(status), (const char*)&reply.what);
	fflush(stdout);
	if (status != B_OK)
		return "";
	return reply.GetString("page", "");
}


static const char* kRepoWork = "/tmp/ce-welcome-repo";
static const char* kRepoConfig
	= "/boot/system/settings/package-repositories/CETestQuasar";
static const char* kRepoCache
	= "/boot/system/cache/package-repositories/CETestQuasar";


static void
write_text(const BString& path, const char* text)
{
	FILE* file = fopen(path.String(), "w");
	if (file != NULL) {
		fputs(text, file);
		fclose(file);
	}
}


//!	A local repository with a "quasar" package, configured like pkgman
//!	add-repo does, its index in the cache (no download).
static bool
make_quasar_repository(BString& error)
{
	BString dir(kRepoWork);
	BString content = dir;
	content << "/content";
	int status;
	run((BString("rm -rf ") << dir << " && mkdir -p " << content).String(),
		&status);
	write_text(BString(content) << "/.PackageInfo",
		"name\t\t\tquasar\nversion\t\t\t1.0-1\narchitecture\tany\n"
		"summary\t\t\t\"Test package\"\n"
		"description\t\t\"Only used by welcome_ui_test.\"\n"
		"packager\t\t\"Renga harness <harness@renga.invalid>\"\n"
		"vendor\t\t\t\"Techniix\"\ncopyrights\t\t\"2026 Renga contributors\"\n"
		"licenses\t\t\"MIT\"\nprovides {\n\tquasar = 1.0\n}\nrequires {\n}\n");
	BString output = run((BString("package create -q -C ") << content << " "
		<< dir << "/quasar-1.0-1-any.hpkg 2>&1").String(), &status);
	if (status != 0) {
		error = output;
		return false;
	}
	BString info;
	info << "name CETestQuasar\nvendor \"Techniix\"\nsummary \"test\"\n"
		"priority 3\nbaseurl file://" << dir << "\n"
		"identifier tag:renga.invalid,2026:ce-test/quasar\n"
		"architecture x86_64\n";
	write_text(BString(dir) << "/repo.info", info.String());
	output = run((BString("cd ") << dir << " && package_repo create -q "
		"repo.info quasar-1.0-1-any.hpkg 2>&1 && cp repo " << kRepoCache
		<< " 2>&1").String(), &status);
	if (status != 0) {
		error = output;
		return false;
	}
	BString config;
	config << "cfgversion=2\nbaseurl=file://" << dir << "\n"
		"identifier=tag:renga.invalid,2026:ce-test/quasar\n"
		"url=tag:renga.invalid,2026:ce-test/quasar\npriority=3\n";
	write_text(kRepoConfig, config.String());
	return true;
}


static void
remove_quasar_repository()
{
	run((BString("rm -rf ") << kRepoWork << " " << kRepoConfig << " "
		<< kRepoCache).String());
}


static bool
view_hidden(BMessenger& app, const char* view)
{
	BMessage request(B_GET_PROPERTY);
	request.AddSpecifier("Hidden");
	request.AddSpecifier("View", view);
	request.AddSpecifier("Window", (int32)0);
	BMessage reply;
	bool hidden = true;
	if (app.SendMessage(&request, &reply, 2000000, 2000000) != B_OK
		|| reply.FindBool("result", &hidden) != B_OK)
		return true;
	return hidden;
}


//!	Opens the window as with an NVIDIA card, on the graphics page, waits
//!	for the repository check, and reads which button shows.
static void
graphics_buttons(bool& install, bool& instructions)
{
	// Welcome is single-launch: a launch while the registrar still lists
	// the last (just killed) instance would only send that one the
	// arguments
	for (int i = 0; i < 50 && be_roster->IsRunning(kSignature); i++)
		snooze(100000);
	const char* args[] = { "--page", "graphics", "--test-assume-nvidia" };
	team_id team = -1;
	status_t launched = be_roster->Launch(kSignature, 3, args, &team);
	printf("CE-INFO welcome: launched as with an NVIDIA card: %s\n",
		strerror(launched));
	fflush(stdout);
	BMessenger messenger;
	for (int i = 0; i < 100 && !messenger.IsValid(); i++) {
		snooze(100000);
		messenger = BMessenger(kSignature);
	}
	show_page(messenger, "graphics");
	install = instructions = false;
	// pkgman starts slowly in QEMU without KVM
	bigtime_t start = system_time();
	for (int i = 0; i < 360 && !install && !instructions; i++) {
		snooze(250000);
		install = !view_hidden(messenger, "installQuasar");
		instructions = !view_hidden(messenger, "quasarInstructions");
	}
	printf("CE-INFO welcome: repository check answered after %.1f s\n",
		(system_time() - start) / 1000000.0);
	fflush(stdout);
	messenger.SendMessage(B_QUIT_REQUESTED);
	snooze(500000);
	if (team > 0)
		kill(team, SIGKILL);
}


static void
quasar_checks()
{
	BString status = run("/boot/system/apps/Welcome --status 2>/dev/null");
	bool install, instructions;
	graphics_buttons(install, instructions);
	report("quasar_absent", status.FindFirst("quasar_available no") >= 0
			&& !install && instructions,
		"status says %s; Install Quasar %s, build instructions %s",
		status.FindFirst("quasar_available no") >= 0 ? "not available"
			: "available", install ? "shown" : "hidden",
		instructions ? "shown" : "hidden");

	BString error;
	bool made = make_quasar_repository(error);
	status = run("/boot/system/apps/Welcome --status 2>/dev/null");
	install = instructions = false;
	if (made)
		graphics_buttons(install, instructions);
	report("quasar_offered", made && status.FindFirst("quasar_available yes")
			>= 0 && install && !instructions,
		"%s; Install Quasar %s, build instructions %s",
		made ? (status.FindFirst("quasar_available yes") >= 0
			? "status: available" : "status: not available")
			: error.String(), install ? "shown" : "hidden",
		instructions ? "shown" : "hidden");
	remove_quasar_repository();
}


int
main()
{
	BApplication app("application/x-vnd.Renga-welcome-test");
	printf("CE-SUITE %s START\n", SUITE);
	if (access(kApp, X_OK) != 0) {
		printf("CE-INFO no %s on this image, nothing to test\n", kApp);
		printf("CE-SUITE %s DONE failures=0\n", SUITE);
		return 0;
	}

	int status;
	BString found = run("/boot/system/apps/Welcome --status 2>/dev/null",
		&status);
	BString shown(found);
	shown.ReplaceAll("\n", "; ");
	report("status", status == 0 && found.FindFirst("nvidia no") >= 0
			&& found.FindFirst("interfaces ") >= 0
			&& found.FindFirst("renga_repository ") >= 0
			&& found.FindFirst("linux_root ") >= 0
			&& found.FindFirst("shortcut_key ") >= 0
			&& found.FindFirst("done yes") >= 0,
		"%s", shown.String());

	BString job = run("cat /boot/system/data/user_launch/user_welcome "
		"2>&1");
	report("launch_job", job.FindFirst("not file_exists ~/config/settings/"
			"Renga/welcome") >= 0 && job.FindFirst("not read_only") >= 0,
		"%" B_PRId32 " bytes", job.Length());

	team_id team = -1;
	be_roster->Launch(kSignature, (BMessage*)NULL, &team);
	BMessenger messenger;
	for (int i = 0; i < 100 && !messenger.IsValid(); i++) {
		snooze(100000);
		messenger = BMessenger(kSignature);
	}
	printf("CE-INFO welcome: launched team %" B_PRId32 ", messenger %s\n",
		team, messenger.IsValid() ? "valid" : "invalid");
	fflush(stdout);
	snooze(2000000);

	const char* pages[] = { "welcome", "network", "graphics", "software",
		"linux", "gamepads", "setup", "tips" };
	BString missing;
	for (size_t i = 0; i < 8; i++) {
		if (show_page(messenger, pages[i]) != pages[i])
			missing << pages[i] << " ";
	}
	report("pages", missing.IsEmpty(), "not shown: %s",
		missing.IsEmpty() ? "none" : missing.String());

	show_page(messenger, "graphics");
	snooze(800000);
	bool quasarHidden = view_hidden(messenger, "installQuasar");
	report("graphics", quasarHidden, "Install Quasar %s (no NVIDIA card in "
		"QEMU)", quasarHidden ? "not shown" : "shown");
	printf("CE-HOST screenshot welcome_graphics\n");
	fflush(stdout);
	snooze(1500000);

	show_page(messenger, "setup");
	snooze(800000);
	int32 ctrl = control_value(messenger, "shortcutCtrl", "Value");
	int32 alt = control_value(messenger, "shortcutAlt", "Value");
	bool altInUse = found.FindFirst("shortcut_key alt") >= 0;
	report("shortcut_key", altInUse ? (alt == 1 && ctrl == 0)
			: (ctrl == 1 && alt == 0),
		"Ctrl %" B_PRId32 ", Alt %" B_PRId32 " (in use: %s)", ctrl, alt,
		altInUse ? "Alt" : "Ctrl");
	printf("CE-HOST screenshot welcome_setup\n");
	fflush(stdout);
	snooze(1500000);

	messenger.SendMessage(B_QUIT_REQUESTED);
	snooze(500000);
	if (team >= 0)
		kill(team, SIGKILL);

	quasar_checks();

	printf("CE-SUITE %s DONE failures=%d\n", SUITE, sFailures);
	return sFailures == 0 ? 0 : 1;
}
