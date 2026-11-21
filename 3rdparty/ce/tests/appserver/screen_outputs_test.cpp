/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 */

/*!	In-guest test: asking app_server for the screen's outputs while the
	screen mode changes (batch 9 regression test).

	The batch 9 sweep crashed app_server in AS_GET_SCREEN_OUTPUTS: the
	Deskbar asked for its output while pointer_test changed the screen mode,
	and a mode change resets the virtual screen, whose HWInterface is NULL
	in between. The request now reads under the screen lock.

	Here one thread asks get_screen_outputs() as fast as it can while the
	main thread switches workspaces 40 times (each switch sets the
	workspace's screen configuration again) and then switches between the
	current mode and 800 x 600 ten times.

	  screen_outputs.during_mode_changes  every request answered, at least
	                                      one output each time, app_server
	                                      still the same team afterwards

	Output format: see 3rdparty/ce/README.md.
*/


#include <Application.h>
#include <Roster.h>
#include <Screen.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <ScreenOutputs.h>


#define SUITE "screen_outputs"

static volatile bool sStop = false;
static int32 sRequests = 0;
static int32 sFailed = 0;
static int32 sEmpty = 0;


static status_t
ask_outputs(void*)
{
	while (!sStop) {
		screen_output_info outputs[16];
		int32 count = 0;
		status_t status = BPrivate::get_screen_outputs(outputs, 16, &count);
		atomic_add(&sRequests, 1);
		if (status != B_OK)
			atomic_add(&sFailed, 1);
		else if (count < 1)
			atomic_add(&sEmpty, 1);
	}
	return B_OK;
}


static team_id
app_server_team()
{
	int32 cookie = 0;
	team_info info;
	while (get_next_team_info(&cookie, &info) == B_OK) {
		if (strstr(info.args, "app_server") != NULL)
			return info.team;
	}
	return -1;
}


int
main()
{
	printf("CE-SUITE %s START\n", SUITE);
	fflush(stdout);
	if (access("/boot/system/servers/app_server", X_OK) != 0) {
		printf("CE-SUITE %s DONE failures=0\n", SUITE);
		return 0;
	}
	BApplication app("application/x-vnd.Renga-screen-outputs-test");

	BScreen screen;
	display_mode original;
	display_mode* modes;
	uint32 modeCount;
	display_mode small;
	bool found = false;
	if (screen.GetMode(&original) == B_OK
		&& screen.GetModeList(&modes, &modeCount) == B_OK) {
		for (uint32 i = 0; i < modeCount && !found; i++) {
			if (modes[i].virtual_width == 800
				&& modes[i].virtual_height == 600
				&& modes[i].space == original.space) {
				small = modes[i];
				found = true;
			}
		}
		free(modes);
	}
	if (!found || (original.virtual_width == 800
			&& original.virtual_height == 600)) {
		printf("CE-INFO screen_outputs: no other mode to switch to, not "
			"tested\n");
		printf("CE-SUITE %s DONE failures=0\n", SUITE);
		return 0;
	}

	team_id before = app_server_team();
	thread_id asker = spawn_thread(ask_outputs, "ask outputs",
		B_NORMAL_PRIORITY, NULL);
	resume_thread(asker);

	// workspace switches reset the virtual screen
	// (Desktop::_SetCurrentWorkspaceConfiguration())
	int32 workspace = current_workspace();
	int32 changes = 0;
	for (int32 i = 0; i < 40; i++) {
		activate_workspace(i % 2 == 0 ? (workspace + 1) % count_workspaces()
			: workspace);
		changes++;
		snooze(50000);
	}
	activate_workspace(workspace);
	for (int32 i = 0; i < 10; i++) {
		if (screen.SetMode(&small) == B_OK)
			changes++;
		snooze(150000);
		if (screen.SetMode(&original) == B_OK)
			changes++;
		snooze(150000);
	}
	sStop = true;
	status_t result;
	wait_for_thread(asker, &result);
	snooze(500000);
	team_id after = app_server_team();

	bool pass = sRequests > 0 && sFailed == 0 && sEmpty == 0
		&& before >= 0 && before == after;
	printf("CE-TEST %s.during_mode_changes %s %d workspace and mode changes, %d requests, "
		"%d failed, %d without an output, app_server team %d -> %d\n", SUITE,
		pass ? "PASS" : "FAIL", (int)changes, (int)sRequests, (int)sFailed,
		(int)sEmpty, (int)before, (int)after);
	printf("CE-SUITE %s DONE failures=%d\n", SUITE, pass ? 0 : 1);
	return pass ? 0 : 1;
}
