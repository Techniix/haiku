/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 */

/*!	Programs that stopped playing leave the per-application audio controls
	(batch 5; hardware report: the Audio preferences listed "halo" twice,
	from two earlier runs of a program long gone).

	A child of this program plays a tone with BSoundPlayer under its own
	signature. The system mixer's streams (AudioStreamControl, what the
	Audio and Media preferences and the Deskbar's volume popup show):

	  app_rows.listed              the running program has one stream
	  app_rows.kill9_gone          after kill -9 it is gone within 1 s
	  app_rows.second_run_single   a second run shows one stream, not two
	  app_rows.quit_gone           after a normal quit it is gone within 1 s
	  app_rows.crash_gone          after a crash (SIGSEGV) within 1 s

	On the old mixer the stream of a killed program stayed until
	media_server restarted, and every run added one.
*/


#include <Application.h>
#include <MediaRoster.h>
#include <SoundPlayer.h>

#include <math.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include <AudioStreamControl.h>


#define SUITE "app_rows"

static const char* kSignature = "application/x-vnd.Renga-app-rows-player";
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
play_buffer(void* cookie, void* buffer, size_t size,
	const media_raw_audio_format& format)
{
	float* samples = (float*)buffer;
	float* phase = (float*)cookie;
	for (size_t i = 0; i < size / sizeof(float); i += 2) {
		samples[i] = samples[i + 1] = 0.05f * sinf(*phase);
		*phase += 2 * M_PI * 440 / 48000;
	}
	*phase = fmodf(*phase, 2 * M_PI);
}


//!	The child: plays until "quit" on stdin, or until it is killed.
static int
run_player(bool crash)
{
	BApplication app(kSignature);
	media_raw_audio_format format = media_raw_audio_format::wildcard;
	format.format = media_raw_audio_format::B_AUDIO_FLOAT;
	format.channel_count = 2;
	format.frame_rate = 48000;
	format.byte_order = B_MEDIA_HOST_ENDIAN;
	format.buffer_size = 1024 * 2 * sizeof(float);
	float phase = 0;
	BSoundPlayer player(&format, "app_rows player", play_buffer, NULL,
		&phase);
	if (player.InitCheck() != B_OK)
		return 1;
	player.Start();
	player.SetHasData(true);
	char line[64];
	while (fgets(line, sizeof(line), stdin) != NULL) {
		if (strncmp(line, "crash", 5) == 0) {
			snooze(100000);
			raise(SIGSEGV);
		}
		if (strncmp(line, "quit", 4) == 0)
			break;
	}
	player.Stop();
	return 0;
}


struct Child {
	pid_t	pid;
	int		input;
};


static Child
start_child(const char* self)
{
	Child child = { -1, -1 };
	int fds[2];
	if (pipe(fds) != 0)
		return child;
	child.pid = fork();
	if (child.pid == 0) {
		dup2(fds[0], 0);
		close(fds[1]);
		execl(self, self, "player", (char*)NULL);
		_exit(127);
	}
	close(fds[0]);
	child.input = fds[1];
	return child;
}


static int32
count_streams()
{
	BPrivate::AudioStreamControl control;
	if (control.Update() != B_OK)
		return -1;
	int32 count = 0;
	for (int32 i = 0; i < control.CountStreams(); i++) {
		if (control.StreamAt(i)->signature == kSignature)
			count++;
	}
	return count;
}


//!	Waits until there are \a want streams; returns the time it took, or -1.
static bigtime_t
wait_count(int32 want, bigtime_t timeout, int32* _count)
{
	bigtime_t start = system_time();
	while (true) {
		*_count = count_streams();
		if (*_count == want)
			return system_time() - start;
		if (system_time() - start > timeout)
			return -1;
		snooze(50000);
	}
}


static void
end_child(Child& child, const char* how)
{
	if (strcmp(how, "kill") == 0)
		kill(child.pid, SIGKILL);
	else {
		write(child.input, how, strlen(how));
		write(child.input, "\n", 1);
	}
	int status;
	waitpid(child.pid, &status, 0);
	close(child.input);
}


int
main(int argc, char** argv)
{
	if (argc >= 2 && strcmp(argv[1], "player") == 0)
		return run_player(false);

	BApplication app("application/x-vnd.Renga-app-rows-test");
	printf("CE-SUITE %s START\n", SUITE);
	// the media services may still be starting (the runner starts them
	// right before the first media test)
	bool mixer = false;
	for (int i = 0; i < 360 && !mixer; i++) {
		status_t status;
		BMediaRoster* roster = BMediaRoster::Roster(&status);
		media_node node;
		if (roster != NULL && roster->GetAudioMixer(&node) == B_OK) {
			media_output output;
			int32 count = 0;
			mixer = roster->GetConnectedOutputsFor(node, &output, 1, &count)
				== B_OK && count == 1;
			roster->ReleaseNode(node);
		}
		if (!mixer)
			snooze(250000);
	}
	if (!mixer) {
		report("mixer", false, "no system mixer after 90 s");
		printf("CE-SUITE %s DONE failures=%d\n", SUITE, sFailures);
		return 1;
	}

	int32 count;
	Child child = start_child(argv[0]);
	bigtime_t took = wait_count(1, 15000000, &count);
	report("listed", took >= 0, "%" B_PRId32 " stream(s) after %.2f s",
		count, took / 1000000.0);

	end_child(child, "kill");
	took = wait_count(0, 5000000, &count);
	report("kill9_gone", took >= 0 && took <= 1000000, "%s after %.2f s",
		took >= 0 ? "gone" : "still there", took / 1000000.0);

	child = start_child(argv[0]);
	wait_count(1, 15000000, &count);
	snooze(1500000);
	count = count_streams();
	report("second_run_single", count == 1, "%" B_PRId32 " stream(s)",
		count);

	end_child(child, "quit");
	took = wait_count(0, 5000000, &count);
	report("quit_gone", took >= 0 && took <= 1000000, "%s after %.2f s",
		took >= 0 ? "gone" : "still there", took / 1000000.0);

	child = start_child(argv[0]);
	wait_count(1, 15000000, &count);
	end_child(child, "crash");
	took = wait_count(0, 5000000, &count);
	report("crash_gone", took >= 0 && took <= 1000000, "%s after %.2f s",
		took >= 0 ? "gone" : "still there", took / 1000000.0);

	printf("CE-SUITE %s DONE failures=%d\n", SUITE, sFailures);
	return sFailures == 0 ? 0 : 1;
}
