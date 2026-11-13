/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 */

/*!	The Installer's window for installing over an existing installation
	(roadmap F-23), on images with a desktop.

	The harness attaches a 64 MiB BFS disk "CETarget" with what looks like
	an installation: a system package file named
	haiku-r1~beta5_hrev57937-1-x86_64.hpkg and a user file. Then:
	- "Installer --list-targets" names it;
	- "Installer --target CETarget --begin" opens the window with that
	  destination chosen and presses Begin: the installation mode window
	  comes up and waits (screenshot installer_modes: the heading names
	  the installation, upgrade is chosen; F-59: the volume is named
	  "Haiku" for this, so "Rename the volume to \"Renga\"" is offered,
	  checked);
	- the Installer is then killed: the volume is unchanged.
	The disk is a snapshot; nothing it gets is kept.
*/


#include <Application.h>
#include <Messenger.h>
#include <OS.h>
#include <Roster.h>
#include <String.h>
#include <Volume.h>
#include <fs_info.h>

#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>


#define SUITE "installer_ui"

static const char* kInstaller = "/boot/system/apps/Installer";
static const char* kSignature = "application/x-vnd.Haiku-Installer";

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






static void
screenshot(const char* name)
{
	printf("CE-HOST screenshot %s\n", name);
	fflush(stdout);
	snooze(1500000);
}


int
main()
{
	BApplication app("application/x-vnd.Renga-installer-ui-test");
	printf("CE-SUITE %s START\n", SUITE);

	// The harness attaches "CETarget", a small BFS disk with what looks like
	// an installation (a system package file and a user file).
	int status;
	BString command;
	BString output;
	run("mountvolume CETarget 2>&1", &status);
	struct stat st;
	bool ready = stat("/CETarget/home/Documents/letter.txt", &st) == 0
		&& stat("/CETarget/system/packages/"
			"haiku-r1~beta5_hrev57937-1-x86_64.hpkg", &st) == 0;
	report("target_ready", ready, "CETarget %s",
		ready ? "mounted, with an installation" : "missing (the harness "
			"attaches it on renga-* runs)");
	if (!ready) {
		printf("CE-SUITE %s DONE failures=%d\n", SUITE, sFailures);
		return 1;
	}

	command = "";
	command << kInstaller << " --list-targets";
	output = run(command, &status);
	report("list_targets", status == 0 && output.FindFirst("CETarget\t") >= 0
		&& output.FindFirst("Haiku r1~beta5_hrev57937") >= 0,
		"CETarget listed with its installation: %s",
		output.FindFirst("Haiku r1~beta5_hrev57937") >= 0 ? "yes" : "no");

	// The window: --target chooses CETarget, --begin skips the licence
	// window and presses Begin after the disk scan, so the installation
	// mode window comes up over it. (Talking to the Installer through
	// messengers and scripting hung this test in three attempts; the cause
	// was not found, so it does without.)
	// F-59: the volume is named "Haiku" for this, so the window also
	// offers to rename it (an upgraded volume keeps its name otherwise);
	// the mount point follows the name.
	BVolume volume;
	volume.SetTo(dev_for_path("/CETarget"));
	status_t renamed = volume.SetName("Haiku");
	printf("CE-INFO CETarget named \"Haiku\": %s\n", strerror(renamed));
	printf("CE-INFO starting %s --target Haiku --begin\n", kInstaller);
	fflush(stdout);
	team_id team = fork();
	if (team == 0) {
		execl(kInstaller, kInstaller, "--target", "Haiku", "--begin",
			(char*)NULL);
		_exit(127);
	}
	snooze(10000000);
	bool running = team > 0 && kill(team, 0) == 0;
	report("mode_window_waits", running,
		"the Installer %s after Begin (it waits for the mode window)",
		running ? "is still running" : "has exited");
	screenshot("installer_modes");

	if (team > 0) {
		kill(team, SIGKILL);
		waitpid(team, NULL, 0);
	}
	volume.SetName("CETarget");
	bool kept = stat("/CETarget/home/Documents/letter.txt", &st) == 0
		&& stat("/CETarget/system/packages/"
			"haiku-r1~beta5_hrev57937-1-x86_64.hpkg", &st) == 0;
	report("nothing_changed", kept, "without an answer the volume is %s",
		kept ? "unchanged" : "changed");

	run("unmount /CETarget 2>&1");

	printf("CE-SUITE %s DONE failures=%d\n", SUITE, sFailures);
	return sFailures != 0;
}
