/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 */

/*!	In-guest test: Renga's agents are background applications, so they
	never show in Deskbar (2026-11-13: on the maintainer's PC the Gamepads
	preferences, started with --agent at login, stayed in Deskbar for good
	with "No windows", and System logs --notify showed there for a minute
	after every login).

	  <agent>.background_flag   the agent's file has B_BACKGROUND_APP in its
	                            app flags: gamepad_agent,
	                            crash_report_notifier, audio_device_agent
	  roster.no_foreground_agents
	                            after boot, no running Renga application
	                            in /boot/system/servers, and none with an
	                            agent's signature, is a foreground
	                            application (no B_BACKGROUND_APP)
	  roster.gamepad_agent      the gamepad agent, started as the user's
	                            launch script starts it, registers as a
	                            background app (harness boots have no user
	                            session, so the test starts the agents
	                            that are not running)
	  roster.no_agent_apps      neither Gamepads nor System logs runs for
	                            agent work (their old agent modes)

	Images without the agents (minimum images) report an INFO line and
	nothing else. Output format: see 3rdparty/ce/README.md.
*/


#include <AppFileInfo.h>
#include <Application.h>
#include <File.h>
#include <List.h>
#include <Path.h>
#include <Roster.h>
#include <String.h>

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <vector>

extern char** environ;


#define SUITE "background_agents"

static int sFailures = 0;


struct Agent {
	const char*	name;
	const char*	path;
	const char*	signature;
};

static const Agent kAgents[] = {
	{ "gamepad_agent", "/boot/system/servers/gamepad_agent",
		"application/x-vnd.Renga-GamepadAgent" },
	{ "crash_report_notifier", "/boot/system/servers/crash_report_notifier",
		"application/x-vnd.Renga-CrashReportNotifier" },
	{ "audio_device_agent", "/boot/system/servers/audio_device_agent",
		"application/x-vnd.Renga-AudioDeviceAgent" }
};
static const size_t kAgentCount = sizeof(kAgents) / sizeof(kAgents[0]);


static void
report(const char* name, bool pass, const char* format, ...)
{
	char detail[600];
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


static bool
is_agent_signature(const char* signature)
{
	for (size_t i = 0; i < kAgentCount; i++) {
		if (strcasecmp(signature, kAgents[i].signature) == 0)
			return true;
	}
	return false;
}


int
main()
{
	printf("CE-SUITE %s START\n", SUITE);
	fflush(stdout);

	if (access(kAgents[0].path, X_OK) != 0) {
		printf("CE-INFO background_agents: no Renga agents on this image\n");
		printf("CE-SUITE %s DONE failures=0\n", SUITE);
		return 0;
	}

	for (size_t i = 0; i < kAgentCount; i++) {
		BFile file(kAgents[i].path, B_READ_ONLY);
		BAppFileInfo info(&file);
		uint32 flags = 0;
		status_t status = info.InitCheck() == B_OK
			? info.GetAppFlags(&flags) : info.InitCheck();
		char signature[B_MIME_TYPE_LENGTH] = "";
		if (status == B_OK)
			info.GetSignature(signature);
		BString name(kAgents[i].name);
		name << ".background_flag";
		report(name.String(), status == B_OK
				&& (flags & B_BACKGROUND_APP) != 0
				&& strcasecmp(signature, kAgents[i].signature) == 0,
			"%s: %s, flags 0x%" B_PRIx32 ", signature \"%s\"",
			kAgents[i].path, strerror(status), flags, signature);
	}

	BApplication app("application/x-vnd.Renga-ce-background-agents");

	// Harness boots have no user session (no Tracker, no Deskbar), so the
	// user's launch scripts that start the gamepad agent and the crash
	// report notifier never run. Each agent that is not running is
	// started here the way its launch job or media_server starts it (by
	// its path, no arguments), and the roster is looked at while they run.
	std::vector<team_id> started;
	for (size_t i = 0; i < kAgentCount; i++) {
		if (be_roster->IsRunning(kAgents[i].signature))
			continue;
		const char* arguments[] = { kAgents[i].path, NULL };
		thread_id thread = load_image(1, arguments, (const char**)environ);
		if (thread < 0)
			continue;
		thread_info info;
		if (get_thread_info(thread, &info) == B_OK)
			started.push_back(info.team);
		resume_thread(thread);
	}
	bool gamepadAgent = false;
	for (int i = 0; i < 40 && !gamepadAgent; i++) {
		gamepadAgent = be_roster->IsRunning(kAgents[0].signature);
		if (!gamepadAgent)
			snooze(250000);
	}
	snooze(1000000);

	BList teams;
	be_roster->GetAppList(&teams);
	BString foreground;
	BString agentApps;
	int32 checked = 0;
	bool agentBackground = false;
	for (int32 i = 0; i < teams.CountItems(); i++) {
		team_id team = (team_id)(addr_t)teams.ItemAt(i);
		app_info info;
		if (be_roster->GetRunningAppInfo(team, &info) != B_OK)
			continue;
		BPath path(&info.ref);
		bool server = path.InitCheck() == B_OK
			&& strncmp(path.Path(), "/boot/system/servers/", 21) == 0
			&& strncasecmp(info.signature, "application/x-vnd.Renga-", 24)
				== 0;
			// Renga's own servers; upstream's are upstream's business
		bool agent = is_agent_signature(info.signature);
		if (strcasecmp(info.signature, kAgents[0].signature) == 0)
			agentBackground = (info.flags & B_BACKGROUND_APP) != 0;
		if (strcasecmp(info.signature, "application/x-vnd.Renga-Gamepads")
				== 0
			|| strncasecmp(info.signature,
				"application/x-vnd.Renga-SystemLogs", 34) == 0) {
			if (agentApps.Length() > 0)
				agentApps << ", ";
			agentApps << info.signature;
		}
		if (!server && !agent)
			continue;
		checked++;
		if ((info.flags & B_BACKGROUND_APP) == 0) {
			if (foreground.Length() > 0)
				foreground << ", ";
			foreground << info.signature << " (" << path.Path() << ")";
		}
	}

	report("roster.no_foreground_agents", foreground.Length() == 0,
		"%" B_PRId32 " running servers and agents checked; foreground: %s",
		checked, foreground.Length() > 0 ? foreground.String() : "none");
	report("roster.gamepad_agent", gamepadAgent && agentBackground,
		"gamepad agent %s%s", gamepadAgent ? "running" : "not running",
		gamepadAgent ? (agentBackground ? ", background" : ", FOREGROUND")
			: "");
	for (size_t i = 0; i < started.size(); i++)
		kill_team(started[i]);

	report("roster.no_agent_apps", agentApps.Length() == 0,
		"applications started for agent work: %s",
		agentApps.Length() > 0 ? agentApps.String() : "none");

	printf("CE-SUITE %s DONE failures=%d\n", SUITE, sFailures);
	fflush(stdout);
	return sFailures != 0;
}
