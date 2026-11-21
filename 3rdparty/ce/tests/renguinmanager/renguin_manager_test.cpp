/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 */

/*!	Renguin Manager (batch 5).

	  renguin_manager.status        --status: the layer is there, the Linux
	                                root (the harness's), its Alpine release
	                                and how many packages it has
	  renguin_manager.window        the window opens with its controls
	  renguin_manager.control_panel the Control Panel's Renguin page is the
	                                same view (its "Open the Linux shell"
	                                button is there)
	  renguin_manager.screenshots   the window
	  renguin_manager.display_scale (batch 12) the display scale for Linux
	                                programs: the Settings tab's menu and the
	                                Programs tab's are there; --display-scale
	                                sets the file the compositor reads (scale
	                                and a program's own) and removes it at
	                                100% with no program scale left
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
#include <unistd.h>


#define SUITE "renguin_manager"

static const char* kApp = "/boot/system/apps/RenguinManager";
static const char* kSignature = "application/x-vnd.Renga-RenguinManager";
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


static BMessenger
launch(const char* signature, team_id* _team)
{
	be_roster->Launch(signature, (BMessage*)NULL, _team);
	BMessenger messenger;
	for (int i = 0; i < 100 && !messenger.IsValid(); i++) {
		snooze(100000);
		messenger = BMessenger(signature);
	}
	snooze(2000000);
	return messenger;
}


int
main()
{
	BApplication app("application/x-vnd.Renga-renguin-manager-test");
	printf("CE-SUITE %s START\n", SUITE);
	if (access(kApp, X_OK) != 0) {
		printf("CE-INFO no %s on this image, nothing to test\n", kApp);
		printf("CE-SUITE %s DONE failures=0\n", SUITE);
		return 0;
	}

	int status;
	BString output = run("/boot/system/apps/RenguinManager --status "
		"2>/dev/null", &status);
	bool rootThere = access("/boot/system/non-packaged/compat/linux/bin/sh",
		F_OK) == 0;
	BString shown(output);
	shown.ReplaceAll("\n", "; ");
	report("status", status == 0
			&& output.FindFirst("layer\tLinux programs: available") >= 0
			&& (!rootThere || (output.FindFirst("release\tLinux system: "
					"Alpine Linux ") >= 0
				&& output.FindFirst("packages\tLinux packages installed: 0")
					< 0)),
		"%s", shown.String());

	// batch 10: the switches, on by default, read back from the settings
	// files (a kernel one and a user one)
	report("switches", output.FindFirst("switch\tvdso\ton") >= 0
			&& output.FindFirst("switch\txattrs\ton") >= 0
			&& output.FindFirst("switch\tbadge\ton") >= 0
			&& output.FindFirst("switch\topen_with\ton") >= 0
			&& output.FindFirst("switch\tappimage\ton") >= 0
			&& output.FindFirst("switch\ttheme\ton") >= 0,
		"%s", shown.String());
	BString kernelSettings = run("cat ~/config/settings/kernel/drivers/renguin "
		"2>/dev/null", NULL);
	BString userSettings = run("cat ~/config/settings/Renguin/settings "
		"2>/dev/null", NULL);
	run("mkdir -p ~/config/settings/kernel/drivers ~/config/settings/Renguin "
		"&& echo 'vdso false' > ~/config/settings/kernel/drivers/renguin "
		"&& echo 'badge false' > ~/config/settings/Renguin/settings", NULL);
	BString changed = run("/boot/system/apps/RenguinManager --status "
		"2>/dev/null", NULL);
	report("switches_read", changed.FindFirst("switch\tvdso\toff") >= 0
			&& changed.FindFirst("switch\tbadge\toff") >= 0
			&& changed.FindFirst("switch\txattrs\ton") >= 0,
		"after writing vdso false and badge false");
	if (kernelSettings.IsEmpty())
		run("rm -f ~/config/settings/kernel/drivers/renguin", NULL);
	if (userSettings.IsEmpty())
		run("rm -f ~/config/settings/Renguin/settings", NULL);

	team_id team = -1;
	BMessenger messenger = launch(kSignature, &team);
	report("window", messenger.IsValid() && has_view(messenger, "openShell")
			&& has_view(messenger, "renguinTabs"),
		"team %" B_PRId32, team);
	{
		bool menus = has_view(messenger, "displayScale")
			&& has_view(messenger, "programScale");
		const char* file = "~/config/settings/Renga/renguin_display_scale";
		run("/boot/system/apps/RenguinManager --display-scale 1.5", NULL);
		BString set = run(BString("cat ") << file << " 2>/dev/null", NULL);
		BString printed = run("/boot/system/apps/RenguinManager "
			"--display-scale", NULL);
		run("/boot/system/apps/RenguinManager --display-scale 2 foot", NULL);
		BString program = run(BString("cat ") << file << " 2>/dev/null",
			NULL);
		run("/boot/system/apps/RenguinManager --display-scale 0 foot", NULL);
		run("/boot/system/apps/RenguinManager --display-scale 1", NULL);
		int status = 0;
		run(BString("test -e ") << file, &status);
		report("display_scale", menus && set.FindFirst("scale 1.500") >= 0
				&& printed.FindFirst("scale\t1.500") >= 0
				&& program.FindFirst("program foot 2.000") >= 0
				&& status != 0,
			"menus %d, set: %s, printed: %s, program line %d, removed at "
			"100%% %d", menus, set.FindFirst("scale 1.500") >= 0 ? "yes" : "no",
			printed.Trim().String(),
			program.FindFirst("program foot 2.000") >= 0, status != 0);
	}
	printf("CE-HOST screenshot renguin_manager\n");
	fflush(stdout);
	snooze(1500000);
	messenger.SendMessage(B_QUIT_REQUESTED);
	snooze(500000);
	if (team >= 0)
		kill(team, SIGKILL);

	if (access("/boot/system/apps/ControlPanel", X_OK) == 0) {
		team_id panel = -1;
		BMessenger controlPanel = launch(
			"application/x-vnd.Renga-ControlPanel", &panel);
		BMessage request('cpPg');
		request.AddString("id", "renguin");
		BMessage reply;
		controlPanel.SendMessage(&request, &reply, 5000000, 5000000);
		snooze(500000);
		report("control_panel", BString(reply.GetString("page", ""))
				== "renguin" && has_view(controlPanel, "openShell"),
			"page \"%s\"", reply.GetString("page", ""));
		controlPanel.SendMessage(B_QUIT_REQUESTED);
		snooze(500000);
		if (panel >= 0)
			kill(panel, SIGKILL);
	}

	printf("CE-SUITE %s DONE failures=%d\n", SUITE, sFailures);
	return sFailures == 0 ? 0 : 1;
}
