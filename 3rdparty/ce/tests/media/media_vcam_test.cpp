/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 */

/*!	The virtual camera (roadmap F-58, OBS Studio's request O-12;
	headers/private/media/VirtualCamera.h): a Media Kit video source that
	one program feeds through a memfd and a local socket, and every other
	program records like a camera.

	Checks print "CE-TEST virtual_camera.<name> PASS|FAIL <detail>":
	- listed: "Virtual Camera" is among the video sources (the query
	  CodyCam's Camera menu makes)
	- no_signal: without a feeder it sends dark grey frames at the rate
	  asked for
	- fed_colours: frames fed at 160x120 arrive scaled to the recorder's
	  320x240, red, then green, in that order
	- status: the node's "renga:virtual_camera" parameter names the feeder
	  and its size
	- second_feeder_busy: a second feeder is refused with B_BUSY
	- feeder_gone: after the feeder disconnects, the frames are "no signal"
	  again
	- codycam: CodyCam previews it (screenshot virtual_camera_codycam)
*/


#include <Application.h>
#include <Autolock.h>
#include <Locker.h>
#include <MediaAddOn.h>
#include <MediaRecorder.h>
#include <MediaRoster.h>
#include <Messenger.h>
#include <OS.h>
#include <ParameterWeb.h>
#include <String.h>

#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include <vector>

#include <VirtualCamera.h>


#define SUITE "virtual_camera"

using BPrivate::VirtualCameraFeeder;

static const char* kCodyCam = "/boot/system/apps/CodyCam";
static int sFailures = 0;


static void
report(const char* name, bool pass, const char* format, ...)
{
	char detail[512];
	va_list args;
	va_start(args, format);
	vsnprintf(detail, sizeof(detail), format, args);
	va_end(args);
	printf("CE-TEST " SUITE ".%s %s %s\n", name, pass ? "PASS" : "FAIL",
		detail);
	fflush(stdout);
	if (!pass)
		sFailures++;
}


// The centre pixel of every frame the recorder got, with the frame size.
struct Frames {
	BLocker					lock;
	std::vector<uint32>		centres;
	std::vector<bigtime_t>	times;
	uint32					width;
	uint32					height;

	Frames() : lock("frames"), width(0), height(0) {}
};


static void
frame_received(void* cookie, bigtime_t timestamp, void* data, size_t size,
	const media_format& format)
{
	Frames* frames = (Frames*)cookie;
	const media_raw_video_format& raw = format.u.raw_video;
	uint32 width = raw.display.line_width;
	uint32 height = raw.display.line_count;
	uint32 bpr = raw.display.bytes_per_row != 0 ? raw.display.bytes_per_row
		: width * 4;
	if (size < (size_t)bpr * height || width == 0)
		return;
	uint32 centre = *(const uint32*)((const uint8*)data
		+ (height / 2) * bpr + (width / 2) * 4);
	BAutolock locker(frames->lock);
	frames->centres.push_back(centre & 0x00ffffff);
	frames->times.push_back(system_time());
	frames->width = width;
	frames->height = height;
}


static bool
is_colour(uint32 pixel, uint32 colour)
{
	for (int shift = 0; shift < 24; shift += 8) {
		int a = (pixel >> shift) & 0xff;
		int b = (colour >> shift) & 0xff;
		if (abs(a - b) > 8)
			return false;
	}
	return true;
}


static BString
status_of(const media_node& node)
{
	BString status;
	BParameterWeb* web = NULL;
	if (BMediaRoster::Roster()->GetParameterWebFor(node, &web) != B_OK)
		return status;
	for (int32 i = 0; i < web->CountParameters(); i++) {
		BParameter* parameter = web->ParameterAt(i);
		if (strcmp(parameter->Kind(), VIRTUAL_CAMERA_STATUS_KIND) != 0)
			continue;
		char text[256];
		size_t size = sizeof(text);
		bigtime_t when;
		if (parameter->GetValue(text, &size, &when) == B_OK)
			status.SetTo(text, strnlen(text, sizeof(text)));
	}
	delete web;
	return status;
}


static void
fill(std::vector<uint32>& pixels, uint32 colour)
{
	for (size_t i = 0; i < pixels.size(); i++)
		pixels[i] = 0xff000000 | colour;
}


//!	Feeds \a colour at 30 fps for \a seconds.
static status_t
feed(VirtualCameraFeeder& feeder, uint32 colour, double seconds)
{
	std::vector<uint32> pixels(160 * 120);
	fill(pixels, colour);
	bigtime_t end = system_time() + (bigtime_t)(seconds * 1000000);
	status_t status = B_OK;
	while (system_time() < end && status == B_OK) {
		status = feeder.WriteFrame(pixels.data(), 160 * 4, system_time());
		snooze(33333);
	}
	return status;
}


struct FeedArgs {
	VirtualCameraFeeder*	feeder;
	uint32					colour;
	double					seconds;
};


static status_t
feed_thread(void* data)
{
	FeedArgs* args = (FeedArgs*)data;
	return feed(*args->feeder, args->colour, args->seconds);
}


int
main()
{
	signal(SIGPIPE, SIG_IGN);
	BApplication app("application/x-vnd.Renga-virtual-camera-test");
	printf("CE-SUITE " SUITE " START\n");
	fflush(stdout);

	BMediaRoster* roster = NULL;
	dormant_node_info info;
	bool listed = false;
	BString names;
	bigtime_t deadline = system_time() + 60000000;
	while (!listed && system_time() < deadline) {
		if (roster == NULL && BMediaRoster::IsRunning())
			roster = BMediaRoster::Roster();
		if (roster != NULL) {
			media_format format;
			format.type = B_MEDIA_RAW_VIDEO;
			format.u.raw_video = media_raw_video_format::wildcard;
			dormant_node_info infos[64];
			int32 count = 64;
			names = "";
			if (roster->GetDormantNodes(infos, &count, NULL, &format, NULL,
					B_BUFFER_PRODUCER, 0) == B_OK) {
				for (int32 i = 0; i < count; i++) {
					names << (names.Length() > 0 ? ", " : "")
						<< infos[i].name;
					if (strcmp(infos[i].name, VIRTUAL_CAMERA_FLAVOR_NAME)
							== 0) {
						info = infos[i];
						listed = true;
					}
				}
			}
		}
		if (!listed)
			snooze(500000);
	}
	report("listed", listed, "video sources: %s", names.String());
	if (!listed) {
		printf("CE-SUITE " SUITE " DONE failures=%d\n", sFailures);
		return 1;
	}

	media_node node;
	status_t status = roster->InstantiateDormantNode(info, &node,
		B_FLAVOR_IS_GLOBAL);
	media_node timeSource;
	if (status == B_OK && roster->GetTimeSource(&timeSource) == B_OK) {
		roster->SetTimeSourceFor(node.node, timeSource.node);
		roster->ReleaseNode(timeSource);
	}

	Frames frames;
	BMediaRecorder recorder("virtual camera test", B_MEDIA_RAW_VIDEO);
	recorder.SetHooks(frame_received, NULL, &frames);
	media_output output;
	int32 count = 0;
	if (status == B_OK)
		status = roster->GetFreeOutputsFor(node, &output, 1, &count,
			B_MEDIA_RAW_VIDEO);
	media_format format;
	format.type = B_MEDIA_RAW_VIDEO;
	format.u.raw_video = media_raw_video_format::wildcard;
	format.u.raw_video.display.format = B_RGB32;
	format.u.raw_video.display.line_width = 320;
	format.u.raw_video.display.line_count = 240;
	format.u.raw_video.field_rate = 15;
	if (status == B_OK && count == 1)
		status = recorder.Connect(node, &output, &format);
	if (status == B_OK)
		status = recorder.Start();

	// No feeder: dark grey frames, about 15 a second.
	snooze(2000000);
	size_t greyCount;
	bool grey;
	{
		BAutolock locker(frames.lock);
		greyCount = frames.centres.size();
		grey = greyCount > 0 && is_colour(frames.centres.back(), 0x303030);
	}
	report("no_signal", status == B_OK && grey && greyCount >= 20
			&& greyCount <= 36,
		"%s; %zu frames in 2 s at 15 fps (want ~30), last centre 0x%06"
		B_PRIx32 " (want 0x303030)", strerror(status), greyCount,
		greyCount > 0 ? frames.centres.back() : 0);

	VirtualCameraFeeder feeder;
	status_t connected = feeder.Connect(160, 120, 30);
	BString during = status_of(node);
	size_t start;
	{
		BAutolock locker(frames.lock);
		start = frames.centres.size();
	}
	status_t fed = feed(feeder, 0xff0000, 1.5);
	if (fed == B_OK)
		fed = feed(feeder, 0x00ff00, 1.5);
	snooze(300000);
	int red = 0, green = 0, other = 0;
	bool ordered = true;
	bool sawGreen = false;
	uint32 width, height;
	{
		BAutolock locker(frames.lock);
		width = frames.width;
		height = frames.height;
		for (size_t i = start; i < frames.centres.size(); i++) {
			uint32 centre = frames.centres[i];
			if (is_colour(centre, 0xff0000)) {
				red++;
				if (sawGreen)
					ordered = false;
			} else if (is_colour(centre, 0x00ff00)) {
				green++;
				sawGreen = true;
			} else
				other++;
		}
	}
	report("fed_colours", connected == B_OK && fed == B_OK && red >= 10
			&& green >= 10 && ordered && width == 320 && height == 240,
		"Connect: %s; %d red, %d green, %d other frames, red before green: "
		"%s; frames %" B_PRIu32 "x%" B_PRIu32 " (fed 160x120)",
		strerror(connected), red, green, other, ordered ? "yes" : "no",
		width, height);

	BString expected;
	expected << getpid() << " 160x120";
	report("status", during.StartsWith(expected.String()),
		"\"%s\" (want \"%s ...\")", during.String(), expected.String());

	VirtualCameraFeeder second;
	status_t busy = second.Connect(64, 64, 30);
	report("second_feeder_busy", busy == B_BUSY,
		"a second feeder: %s (want %s)", strerror(busy), strerror(B_BUSY));

	// CodyCam previews it while it is fed (the node has one output: the
	// recorder lets go of it meanwhile).
	recorder.Stop();
	recorder.Disconnect();
	VirtualCameraFeeder* feederPointer = &feeder;
	FeedArgs args = { feederPointer, 0x2060c0, 9.0 };
	thread_id feeding = spawn_thread(feed_thread, "feed", B_NORMAL_PRIORITY,
		&args);
	resume_thread(feeding);
	pid_t codycam = fork();
	if (codycam == 0) {
		execl(kCodyCam, kCodyCam, "--camera", VIRTUAL_CAMERA_FLAVOR_NAME,
			(char*)NULL);
		_exit(127);
	}
	snooze(6000000);
	printf("CE-HOST screenshot virtual_camera_codycam\n");
	fflush(stdout);
	snooze(1500000);
	BMessenger(NULL, codycam).SendMessage(B_QUIT_REQUESTED);
	int exitStatus = -1;
	// CodyCam stops and disconnects the camera node before it quits; the
	// node lives in media_addon_server, so give it time.
	for (int i = 0; i < 120; i++) {
		if (waitpid(codycam, &exitStatus, WNOHANG) == codycam)
			break;
		snooze(250000);
		if (i == 119) {
			kill(codycam, SIGKILL);
			waitpid(codycam, &exitStatus, 0);
		}
	}
	status_t feedResult;
	wait_for_thread(feeding, &feedResult);
	report("codycam", WIFEXITED(exitStatus) && feedResult == B_OK,
		"CodyCam --camera \"%s\" ran and quit (status 0x%x); feeding %s",
		VIRTUAL_CAMERA_FLAVOR_NAME, exitStatus, strerror(feedResult));

	feeder.Disconnect();
	snooze(1000000);
	BString after = status_of(node);
	Frames later;
	BMediaRecorder again("virtual camera test 2", B_MEDIA_RAW_VIDEO);
	again.SetHooks(frame_received, NULL, &later);
	status = roster->GetFreeOutputsFor(node, &output, 1, &count,
		B_MEDIA_RAW_VIDEO);
	if (status == B_OK && count == 1)
		status = again.Connect(node, &output, &format);
	if (status == B_OK)
		status = again.Start();
	snooze(1500000);
	bool backToGrey;
	{
		BAutolock locker(later.lock);
		backToGrey = !later.centres.empty()
			&& is_colour(later.centres.back(), 0x303030);
	}
	again.Stop();
	again.Disconnect();
	report("feeder_gone", backToGrey && after == "none",
		"after the feeder disconnected: centre %s, status \"%s\"",
		backToGrey ? "grey" : "not grey", after.String());

	roster->ReleaseNode(node);

	printf("CE-SUITE " SUITE " DONE failures=%d\n", sFailures);
	fflush(stdout);
	return sFailures == 0 ? 0 : 1;
}
