/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 */

/*!	Camera selection and preview (F-31 phase 2, UVC webcams): CodyCam's
	Camera menu and "CodyCam --camera <name>" with the synthetic video
	source every image carries (video_producer_demo, "Demo Video
	Producer"), since QEMU emulates no USB video device.

	Checks that the Media Kit lists the video sources the menu shows (the
	dormant nodes that produce raw video, the same query as the menu), that
	CodyCam opens on the named source (window title "CodyCam: <name>"),
	that its preview node is connected to that source, and that choosing a
	camera in the menu (the message the menu item sends) tears the preview
	down and builds it again in the same window. Screenshots of the preview
	before and after the switch.

	Runs in the harness's second phase, with media_server started, after
	the audio tests (it starts the time source if nothing runs it).
*/


#include <Application.h>
#include <MediaAddOn.h>
#include <MediaRoster.h>
#include <Messenger.h>
#include <OS.h>
#include <String.h>

#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>


#define SUITE "codycam"

static const char* kCodyCam = "/boot/system/apps/CodyCam";
static const char* kSource = "Demo Video Producer";
static const uint32 kMsgCamera = 'camr';
	// CodyCam's msg_camera

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


static int32
video_sources(BMediaRoster* roster, dormant_node_info* infos, int32 max)
{
	media_format format;
	format.type = B_MEDIA_RAW_VIDEO;
	format.u.raw_video = media_raw_video_format::wildcard;
	int32 count = max;
	if (roster->GetDormantNodes(infos, &count, NULL, &format, NULL,
			B_BUFFER_PRODUCER) != B_OK) {
		return -1;
	}
	return count;
}


static BString
window_title(team_id team)
{
	BMessenger messenger(NULL, team);
	BMessage get(B_GET_PROPERTY);
	get.AddSpecifier("Title");
	get.AddSpecifier("Window", (int32)0);
	BMessage reply;
	BString title;
	if (messenger.IsValid()
		&& messenger.SendMessage(&get, &reply, 2000000, 2000000) == B_OK) {
		reply.FindString("result", &title);
	}
	return title;
}


/*!	The name of the node feeding CodyCam's preview node ("CodyCam", a
	buffer consumer), or an empty string.
*/
static BString
preview_source(BMediaRoster* roster, int32* consumers)
{
	live_node_info nodes[8];
	int32 count = 8;
	*consumers = 0;
	if (roster->GetLiveNodes(nodes, &count, NULL, NULL, "CodyCam",
			B_BUFFER_CONSUMER) != B_OK) {
		return BString();
	}
	*consumers = count;
	for (int32 i = 0; i < count; i++) {
		media_input inputs[4];
		int32 inputCount = 0;
		if (roster->GetConnectedInputsFor(nodes[i].node, inputs, 4,
				&inputCount) != B_OK || inputCount < 1) {
			continue;
		}
		// The input names the consumer, so look for the producer whose
		// output goes to it.
		live_node_info producers[32];
		int32 producerCount = 32;
		if (roster->GetLiveNodes(producers, &producerCount, NULL, NULL, NULL,
				B_BUFFER_PRODUCER) != B_OK) {
			continue;
		}
		for (int32 j = 0; j < producerCount; j++) {
			media_output outputs[8];
			int32 outputCount = 0;
			if (roster->GetConnectedOutputsFor(producers[j].node, outputs, 8,
					&outputCount) != B_OK) {
				continue;
			}
			for (int32 k = 0; k < outputCount; k++) {
				if (outputs[k].destination == inputs[0].destination)
					return BString(producers[j].name);
			}
		}
	}
	return BString();
}


static BString
wait_for_preview(BMediaRoster* roster, int32* consumers)
{
	BString source;
	for (int i = 0; i < 40; i++) {
		source = preview_source(roster, consumers);
		if (source.Length() > 0)
			break;
		snooze(250000);
	}
	return source;
}


int
main()
{
	signal(SIGPIPE, SIG_IGN);
	BApplication app("application/x-vnd.Renga-codycam-test");
	printf("CE-SUITE " SUITE " START\n");
	fflush(stdout);

	// With --only, the only media test: media_server was just started
	// (runner.sh) and media_addon_server may still load the add-ons.
	status_t status = B_ERROR;
	BMediaRoster* roster = NULL;
	dormant_node_info infos[64];
	int32 count = -1;
	int32 demo = -1;
	BString names;
	bigtime_t deadline = system_time() + 60000000;
	while (demo < 0 && system_time() < deadline) {
		if (roster == NULL && BMediaRoster::IsRunning())
			roster = BMediaRoster::Roster(&status);
		if (roster != NULL) {
			count = video_sources(roster, infos, 64);
			names = "";
			for (int32 i = 0; i < count; i++) {
				if (names.Length() > 0)
					names << ", ";
				names << infos[i].name;
				if (strcmp(infos[i].name, kSource) == 0)
					demo = i;
			}
		}
		if (demo < 0)
			snooze(500000);
	}
	if (roster == NULL) {
		report("roster", false, "%s", strerror(status));
		printf("CE-SUITE " SUITE " DONE failures=%d\n", sFailures);
		return 1;
	}
	report("video_sources", demo >= 0, "%" B_PRId32 " source(s): %s", count,
		names.Length() > 0 ? names.String() : "none");
	if (demo < 0) {
		printf("CE-SUITE " SUITE " DONE failures=%d\n", sFailures);
		return 1;
	}

	pid_t team = fork();
	if (team == 0) {
		execl(kCodyCam, kCodyCam, "--camera", kSource, (char*)NULL);
		_exit(127);
	}

	BString want("CodyCam: ");
	want << kSource;
	BString title;
	for (int i = 0; i < 40 && title != want; i++) {
		snooze(250000);
		title = window_title(team);
	}
	report("window_title", title == want, "\"%s\" (want \"%s\")",
		title.String(), want.String());

	int32 consumers;
	BString source = wait_for_preview(roster, &consumers);
	report("preview_connected", source == kSource,
		"preview fed by \"%s\" (%" B_PRId32 " preview node(s))",
		source.String(), consumers);
	snooze(2000000);
	screenshot("codycam_preview");

	// What the Camera menu item sends.
	BMessage choose(kMsgCamera);
	choose.AddData("camera", B_RAW_TYPE, &infos[demo],
		sizeof(dormant_node_info));
	BMessenger codycam(NULL, team);
	status = codycam.SendMessage(&choose);
	report("switch_sent", status == B_OK, "%s", strerror(status));
	snooze(1500000);

	source = wait_for_preview(roster, &consumers);
	report("switch_reconnected", source == kSource && consumers == 1,
		"preview fed by \"%s\", %" B_PRId32 " preview node(s) (want 1: the "
		"old one released)", source.String(), consumers);
	title = window_title(team);
	report("switch_window_kept", title == want, "\"%s\" after the switch",
		title.String());
	snooze(1500000);
	screenshot("codycam_switched");

	if (codycam.IsValid())
		codycam.SendMessage(B_QUIT_REQUESTED);
	int exitStatus = -1;
	for (int i = 0; i < 40; i++) {
		if (waitpid(team, &exitStatus, WNOHANG) == team)
			break;
		snooze(250000);
		if (i == 39) {
			kill(team, SIGKILL);
			waitpid(team, &exitStatus, 0);
		}
	}
	report("quit", WIFEXITED(exitStatus), "CodyCam quit (status 0x%x)",
		exitStatus);

	printf("CE-SUITE " SUITE " DONE failures=%d\n", sFailures);
	fflush(stdout);
	return sFailures == 0 ? 0 : 1;
}
