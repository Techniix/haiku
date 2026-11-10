/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 */

/*!	Screenshots of the per-application audio controls (roadmap F-31 phase
	1, F-32) with two applications playing: the Deskbar volume popup, the
	Media preferences' audio settings (everyday and advanced) and
	SoundRecorder recording one application. Runs after media_test, with
	the media services still running, on images with a desktop.

	The applications are copies of media_test (its "player" mode) named
	"Music" and "Browser", which is what the controls show. Each window is
	checked to be open (scripting: its application's window count), then
	the harness saves "CE-HOST screenshot <name>" as screen-<name>.png.
*/


#include <Application.h>
#include <Messenger.h>
#include <MediaRoster.h>
#include <OS.h>
#include <Roster.h>
#include <String.h>

#include <AudioStreamControl.h>

#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>


#define SUITE "media_ui"

static const char* kPlayer = "/boot/system/non-packaged/bin/ce-tests-media/"
	"media_test";
static const char* kMusic = "application/x-vnd.Renga-ui-music";
static const char* kBrowser = "application/x-vnd.Renga-ui-browser";

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


static void
screenshot(const char* name)
{
	printf("CE-HOST screenshot %s\n", name);
	fflush(stdout);
	snooze(1500000);
}


//!	Starts \a path with \a args (NULL-terminated); returns its team.
static team_id
start(const char* path, const char* const* args)
{
	const char* argv[8];
	int count = 0;
	argv[count++] = path;
	while (args != NULL && *args != NULL && count < 7)
		argv[count++] = *args++;
	argv[count] = NULL;
	pid_t child = fork();
	if (child == 0) {
		execv(path, (char* const*)argv);
		_exit(127);
	}
	return child;
}


static FILE*
start_player(const char* name, const char* signature, int left, int right)
{
	BString copy("/tmp/");
	copy << name;
	BString command;
	command << "cp " << kPlayer << " '" << copy << "' && '" << copy
		<< "' player " << signature;
	FILE* pipe = popen(command.String(), "w");
	if (pipe != NULL) {
		fprintf(pipe, "tone %d %d\n", left, right);
		fflush(pipe);
	}
	return pipe;
}


static void
stop_player(FILE* pipe)
{
	if (pipe == NULL)
		return;
	fprintf(pipe, "quit\n");
	fflush(pipe);
	pclose(pipe);
}


//!	How many windows the application of \a team has open (scripting).
static int32
window_count(team_id team)
{
	BMessenger messenger(NULL, team);
	if (!messenger.IsValid())
		return -1;
	BMessage count(B_COUNT_PROPERTIES);
	count.AddSpecifier("Window");
	BMessage reply;
	if (messenger.SendMessage(&count, &reply, 2000000, 2000000) != B_OK)
		return -1;
	int32 result;
	return reply.FindInt32("result", &result) == B_OK ? result : -1;
}


static int32
wait_for_window(team_id team)
{
	int32 count = -1;
	for (int i = 0; i < 40 && count <= 0; i++) {
		snooze(250000);
		count = window_count(team);
	}
	return count;
}


static void
quit(team_id team)
{
	BMessenger messenger(NULL, team);
	if (messenger.IsValid())
		messenger.SendMessage(B_QUIT_REQUESTED);
	snooze(500000);
	kill(team, SIGKILL);
	waitpid(team, NULL, 0);
}


int
main()
{
	signal(SIGPIPE, SIG_IGN);
	BApplication app("application/x-vnd.Renga-media-ui-test");
	printf("CE-SUITE " SUITE " START\n");
	fflush(stdout);

	// Windows need app_server and input_server, which can come up a minute
	// or more after the runner starts on a full image without KVM. The
	// Deskbar and Tracker are not needed (a fresh Renga image shows
	// FirstBootPrompt instead); the controls are shown standalone.
	bool desktop = false;
	for (int i = 0; i < 300 && !desktop; i++) {
		desktop = be_roster->IsRunning("application/x-vnd.Be-input_server");
		if (!desktop) {
			if (i > 0 && i % 30 == 0) {
				printf("CE-INFO media_ui: waiting for input_server (%d s)\n",
					i);
				fflush(stdout);
			}
			snooze(1000000);
		}
	}
	if (!desktop) {
		printf("CE-INFO media_ui: no desktop, nothing to show\n");
		printf("CE-SUITE " SUITE " DONE failures=0\n");
		return 0;
	}
	snooze(3000000);

	FILE* music = start_player("Music", kMusic, 440, 440);
	FILE* browser = start_player("Browser", kBrowser, 660, 660);

	// The streams, one of them moved to the second card and quieter, so
	// that the controls show something to choose.
	BPrivate::AudioStreamControl control;
	const BPrivate::AudioStream* stream = NULL;
	bool found = false;
	for (int i = 0; i < 50 && !found; i++) {
		snooze(200000);
		found = control.Update() == B_OK && control.FindStream(kMusic) != NULL
			&& control.FindStream(kBrowser) != NULL;
	}
	if (found) {
		stream = control.FindStream(kBrowser);
		control.SetGain(stream->id, -9);
		if (control.Outputs().CountStrings() > 2) {
			control.SetOutputByName(stream->id,
				control.Outputs().StringAt(2).String());
		}
		stream = control.FindStream(kMusic);
	}
	report("streams", found, "Music and Browser %s, %" B_PRId32 " output "
		"choices", found ? "playing" : "not found",
		control.Outputs().CountStrings());

	// The Deskbar volume control's popup, as desklink shows it.
	const char* popupArgs[] = { "--volume-control", NULL };
	// desklink shows it without running its application's looper, so it
	// cannot be asked for its windows; it exits at once if the window
	// cannot be shown, so it still running is the check.
	team_id popup = start("/boot/system/bin/desklink", popupArgs);
	snooze(4000000);
	team_info teamInfo;
	bool running = get_team_info(popup, &teamInfo) == B_OK;
	report("volume_popup", running, "desklink --volume-control %s",
		running ? "shows its window" : "exited");
	int32 windows;
	screenshot("volume_popup");
	quit(popup);

	// The Media preferences, everyday and advanced.
	team_id media = start("/boot/system/preferences/Media", NULL);
	windows = wait_for_window(media);
	snooze(2500000);
	report("media_preferences", windows > 0, "%" B_PRId32 " window(s)",
		windows);
	screenshot("media_preferences");
	quit(media);

	const char* advancedArgs[] = { "--advanced-audio", NULL };
	media = start("/boot/system/preferences/Media", advancedArgs);
	windows = wait_for_window(media);
	snooze(2500000);
	report("media_preferences_advanced", windows > 0, "%" B_PRId32
		" window(s)", windows);
	screenshot("media_preferences_advanced");
	quit(media);

	// SoundRecorder with one application as its input.
	const char* recorderArgs[] = { "--input", "What you hear: Music", NULL };
	team_id recorder = start("/boot/system/apps/SoundRecorder",
		recorderArgs);
	windows = wait_for_window(recorder);
	snooze(2500000);
	report("soundrecorder", windows > 0, "%" B_PRId32 " window(s)", windows);
	screenshot("soundrecorder_application");
	quit(recorder);

	// Leave things as they were for other runs.
	if (control.Update() == B_OK) {
		if ((stream = control.FindStream(kBrowser)) != NULL) {
			control.SetGain(stream->id, 0);
			control.SetOutput(stream->id, 0);
		}
	}
	stop_player(music);
	stop_player(browser);

	printf("CE-SUITE " SUITE " DONE failures=%d\n", sFailures);
	fflush(stdout);
	return sFailures == 0 ? 0 : 1;
}
