/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 */

/*!	Hot-plugging a sound card with the media services running (roadmap
	F-31 phase 2): QEMU's usb-audio device, plugged in and out through the
	harness (CE-HOST usb_audio add|remove). Runs after media_test and
	media_ui_test, with the media services still running.

	Two applications play (copies of media_test in its "player" mode): A
	on the left channel only, routed to the USB device by name, and B on
	the right channel only, on the default output. The phases the harness
	looks for (MEDIA_USB_PHASES in ce-test.py):

		routed     A 1110 Hz on the USB device (usb-2.wav), B 3110 Hz on the
		           default output, the first virtio card (audio.wav)
		default    the USB device made the default output: A 1230 and
		           B 3170 Hz both on it
		fallback   the device pulled out: both fall back to the first
		           virtio card (A 1350, B 3290 Hz in audio.wav)
		back       plugged in again (usb-3.wav): the saved default and A's
		           route return to it by themselves (A 1410, B 3350 Hz)

	The tones are 60 Hz apart (the analysis tells tones apart in 100 ms
	windows) and none of the other suites' tones.

	In the guest it checks that the device's node appears within seconds
	of the plug-in and goes after the unplug without a media services
	restart, that the default output falls back and comes back, and that
	the Media preferences list the device (screenshots of the device's own
	page, "Media --device", and of the list after the unplug).
*/


#include <Application.h>
#include <MediaRoster.h>
#include <Messenger.h>
#include <OS.h>
#include <Roster.h>
#include <String.h>

#include <AudioStreamControl.h>

#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>


using BPrivate::AudioStream;
using BPrivate::AudioStreamControl;


#define SUITE "media_usb"

static const char* kPlayer = "/boot/system/non-packaged/bin/ce-tests-media/"
	"media_test";
static const char* kSignatureA = "application/x-vnd.Renga-usb-test-a";
static const char* kSignatureB = "application/x-vnd.Renga-usb-test-b";
static const char* kDevice = "QEMU USB Audio";

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
host(const char* request)
{
	printf("CE-HOST %s\n", request);
	fflush(stdout);
}


static FILE*
start_player(const char* signature)
{
	BString command;
	command << "'" << kPlayer << "' player " << signature;
	return popen(command.String(), "w");
}


static void
tone(FILE* player, int left, int right)
{
	if (player == NULL)
		return;
	fprintf(player, "tone %d %d\n", left, right);
	fflush(player);
}


static void
stop_player(FILE* player)
{
	if (player == NULL)
		return;
	fprintf(player, "quit\n");
	fflush(player);
	pclose(player);
}


//!	The dormant node of the output device called \a name, if listed.
static bool
find_output(const char* name, dormant_node_info& info)
{
	dormant_node_info infos[32];
	int32 count = 32;
	if (BMediaRoster::Roster()->GetDormantNodes(infos, &count, NULL, NULL,
			NULL, B_BUFFER_CONSUMER | B_PHYSICAL_OUTPUT, 0) != B_OK) {
		return false;
	}
	for (int32 i = 0; i < count; i++) {
		if (strcmp(infos[i].name, name) == 0) {
			info = infos[i];
			return true;
		}
	}
	return false;
}


//!	Whether a live node exists for the output device called \a name.
static bool
live_output(const char* name)
{
	live_node_info infos[32];
	int32 count = 32;
	media_format format;
	format.type = B_MEDIA_RAW_AUDIO;
	if (BMediaRoster::Roster()->GetLiveNodes(infos, &count, &format, NULL,
			NULL, B_BUFFER_CONSUMER | B_PHYSICAL_OUTPUT) != B_OK) {
		return false;
	}
	for (int32 i = 0; i < count; i++) {
		if (strcmp(infos[i].name, name) == 0)
			return true;
	}
	return false;
}


//!	The name of the default output's flavor, empty if there is none.
static BString
default_output()
{
	BMediaRoster* roster = BMediaRoster::Roster();
	media_node node;
	if (roster->GetAudioOutput(&node) != B_OK)
		return "";
	dormant_node_info info;
	BString name;
	if (roster->GetDormantNodeFor(node, &info) == B_OK)
		name = info.name;
	roster->ReleaseNode(node);
	return name;
}


static bool
mixer_connected()
{
	BMediaRoster* roster = BMediaRoster::Roster();
	media_node mixer;
	if (roster->GetAudioMixer(&mixer) != B_OK)
		return false;
	media_output output;
	int32 count = 0;
	bool connected = roster->GetConnectedOutputsFor(mixer, &output, 1, &count)
		== B_OK && count == 1;
	roster->ReleaseNode(mixer);
	return connected;
}


/*!	Waits until \a condition holds, up to \a timeout; returns the time it
	took or -1.
*/
template<typename Condition>
static bigtime_t
wait_until(Condition condition, bigtime_t timeout)
{
	bigtime_t start = system_time();
	while (system_time() - start < timeout) {
		if (condition())
			return system_time() - start;
		snooze(200000);
	}
	return -1;
}


static bool
route_stream(AudioStreamControl& control, const char* signature,
	const char* device)
{
	for (int i = 0; i < 25; i++) {
		if (control.Update() == B_OK) {
			const AudioStream* stream = control.FindStream(signature);
			if (stream != NULL && (device == NULL
					|| control.Outputs().HasString(device))) {
				return control.SetOutputByName(stream->id, device) == B_OK;
			}
		}
		snooze(200000);
	}
	return false;
}


static team_id
start_media(const char* device)
{
	const char* path = "/boot/system/preferences/Media";
	pid_t child = fork();
	if (child == 0) {
		if (device != NULL)
			execl(path, path, "--device", device, (char*)NULL);
		else
			execl(path, path, (char*)NULL);
		_exit(127);
	}
	return child;
}


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


static void
quit_team(team_id team)
{
	BMessenger messenger(NULL, team);
	if (messenger.IsValid())
		messenger.SendMessage(B_QUIT_REQUESTED);
	snooze(500000);
	kill(team, SIGKILL);
	waitpid(team, NULL, 0);
}


static void
screenshot_media(const char* device, const char* name)
{
	team_id media = start_media(device);
	int32 windows = -1;
	for (int i = 0; i < 40 && windows <= 0; i++) {
		snooze(250000);
		windows = window_count(media);
	}
	snooze(2500000);
	BString check("window_");
	check << name;
	report(check.String(), windows > 0, "Media%s%s: %" B_PRId32 " window(s)",
		device != NULL ? " --device " : "", device != NULL ? device : "",
		windows);
	printf("CE-HOST screenshot %s\n", name);
	fflush(stdout);
	snooze(1500000);
	quit_team(media);
}


int
main()
{
	signal(SIGPIPE, SIG_IGN);
	BApplication app("application/x-vnd.Renga-media-usb-test");
	printf("CE-SUITE " SUITE " START\n");
	fflush(stdout);

	bigtime_t took = wait_until(mixer_connected, 60000000);
	report("server_running", took >= 0, "mixer connected after %.1f s",
		took / 1000000.0);
	if (took < 0) {
		printf("CE-SUITE " SUITE " DONE failures=%d\n", sFailures);
		return 1;
	}
	BString originalDefault = default_output();
	dormant_node_info original;
	bool haveOriginal = find_output(originalDefault.String(), original);
	dormant_node_info usb;
	report("none_at_start", !find_output(kDevice, usb), "default output "
		"\"%s\"", originalDefault.String());

	// Plug it in: a flavor, and a node, without a restart.
	host("usb_audio add");
	took = wait_until([&]() { return find_output(kDevice, usb); }, 20000000);
	report("hotplug_listed", took >= 0, "\"%s\" listed after %.1f s", kDevice,
		took / 1000000.0);
	took = wait_until([]() { return live_output(kDevice); }, 10000000);
	report("hotplug_node", took >= 0, "its node %s after %.1f s",
		took >= 0 ? "made" : "not made", took / 1000000.0);
	report("default_unchanged", default_output() == originalDefault,
		"default output still \"%s\"", default_output().String());

	FILE* a = start_player(kSignatureA);
	FILE* b = start_player(kSignatureB);
	tone(a, 1110, 0);
	tone(b, 0, 3110);
	AudioStreamControl control;
	bool routed = route_stream(control, kSignatureA, kDevice);
	report("route_to_usb", routed, "A routed to \"%s\"", kDevice);
	// The mixer's router connects a sink to the device within its
	// two-second round, the device needs a moment to start.
	snooze(5000000);
	tone(a, 1170, 0);
	snooze(500000);
	tone(a, 1110, 0);
	snooze(2500000);

	// The USB device as the default output.
	status_t status = BMediaRoster::Roster()->SetAudioOutput(usb);
	took = wait_until([]() { return default_output() == kDevice
		&& mixer_connected(); }, 15000000);
	report("default_to_usb", status == B_OK && took >= 0, "%s, default "
		"\"%s\" after %.1f s", strerror(status), default_output().String(),
		took / 1000000.0);
	snooze(3000000);
	tone(a, 1230, 0);
	tone(b, 0, 3170);
	snooze(3000000);

	screenshot_media(kDevice, "media_usb_device");

	// Pull it out while both play.
	tone(a, 1290, 0);
	tone(b, 0, 3230);
	snooze(1000000);
	host("usb_audio remove");
	took = wait_until([&]() { return !find_output(kDevice, usb); }, 20000000);
	report("unplug_unlisted", took >= 0, "\"%s\" gone from the list after "
		"%.1f s", kDevice, took / 1000000.0);
	// Alone this takes 2.5-6 s; after the other suites, disconnecting the
	// mixer from the node being released took up to 26 s (an open issue,
	// see the roadmap, F-31).
	took = wait_until([]() { return default_output().Length() > 0
		&& default_output() != kDevice && mixer_connected(); }, 45000000);
	BString fallback = default_output();
	report("unplug_fallback", took >= 0 && fallback == originalDefault,
		"default output \"%s\" after %.1f s, mixer %s", fallback.String(),
		took / 1000000.0, mixer_connected() ? "connected" : "not connected");
	// Releasing the unplugged device's node takes longer (up to 15 s in a
	// full run); the default does not wait for it.
	took = wait_until([]() { return !live_output(kDevice); }, 60000000);
	report("unplug_node_released", took >= 0, "the node %s after %.1f s more",
		took >= 0 ? "was released" : "is still there", took / 1000000.0);
	snooze(3000000);
	tone(a, 1350, 0);
	tone(b, 0, 3290);
	snooze(3000000);

	screenshot_media(NULL, "media_usb_unplugged");

	// Plug it in again: the default and A's route return to it.
	host("usb_audio add");
	took = wait_until([&]() { return find_output(kDevice, usb)
		&& live_output(kDevice); }, 20000000);
	report("replug_listed", took >= 0, "\"%s\" back after %.1f s", kDevice,
		took / 1000000.0);
	took = wait_until([]() { return default_output() == kDevice
		&& mixer_connected(); }, 20000000);
	report("replug_default_back", took >= 0, "default output \"%s\" after "
		"%.1f s", default_output().String(), took / 1000000.0);
	snooze(5000000);
	tone(a, 1410, 0);
	tone(b, 0, 3350);
	snooze(3000000);

	// Leave things as they were: the original default, A's route, no USB
	// device.
	if (haveOriginal)
		BMediaRoster::Roster()->SetAudioOutput(original);
	route_stream(control, kSignatureA, NULL);
	wait_until([&]() { return default_output() == originalDefault
		&& mixer_connected(); }, 15000000);
	stop_player(a);
	stop_player(b);
	host("usb_audio remove");
	took = wait_until([&]() { return !find_output(kDevice, usb); }, 20000000);
	report("cleanup", took >= 0 && default_output() == originalDefault,
		"device gone, default output \"%s\"", default_output().String());

	printf("CE-SUITE " SUITE " DONE failures=%d\n", sFailures);
	fflush(stdout);
	return sFailures == 0 ? 0 : 1;
}
