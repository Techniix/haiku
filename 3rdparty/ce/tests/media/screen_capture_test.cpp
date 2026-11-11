/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 */

/*!	In-guest test of the screen capture of F-31 phase 3 and the screen
	recording of F-44 (renga-* images, after the audio Media Kit tests).

	- Permission: the first capture of an application brings up the Screen
	  recording prompt (screenshot "capture_prompt"); the harness presses
	  Return (Allow, "Remember my decision" on); the decision is remembered.
	- The indicator appears while a capture runs ("capture_indicator").
	- Frames match the screen: a region capture equals BScreen::ReadBitmap()
	  of the same rectangle pixel for pixel; an unchanged screen gives an
	  unchanged frame; drawing a rectangle gives damage covering just that
	  rectangle and a frame that matches the screen again; times increase.
	- Window capture: the window's frame, and its pixels.
	- ScreenCaptureNode: a Media Kit video source whose buffers arrive at
	  the frame rate, of the right size, with increasing start times.
	- Revoking the permission stops a running capture and refuses a new
	  one; the settings window lists the application ("capture_settings").
	- Screenshot records a region with system audio ("Screenshot --record"):
	  while it records, the region turns red, green and blue and a 1370 Hz
	  tone plays ("screenshot_recording"). The file is then decoded through
	  the Media Kit: the video has the frames, in the colours shown, in that
	  order; the audio has the tone; both are as long as the recording.
*/


#include <Application.h>
#include <Bitmap.h>
#include <Entry.h>
#include <MediaFile.h>
#include <MediaFormats.h>
#include <MediaRecorder.h>
#include <MediaRoster.h>
#include <MediaTrack.h>
#include <Messenger.h>
#include <Path.h>
#include <Roster.h>
#include <Screen.h>
#include <SoundPlayer.h>
#include <TimeSource.h>
#include <View.h>
#include <Window.h>

#include <math.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <vector>

#include <CaptureSession.h>
#include <ScreenCapture.h>
#include <WindowInfo.h>


#define SUITE "capture"


using BPrivate::BScreenCapture;
using BPrivate::CaptureOptions;
using BPrivate::ScreenCaptureNode;


static const char* kSignature = "application/x-vnd.Renga-screen-capture-test";
static const char* kScreenshotSignature = "application/x-vnd.haiku-screenshot";
static const char* kAgent = SCREEN_CAPTURE_AGENT_PATH;
static int sFailures = 0;
// The recording's test tone. Not a frequency of any harness phase: it is
// also heard on the first sound card (audio.wav), which the virtio_sound
// analysis reads.
static const double kToneHertz = 1370.0;


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


static void
screenshot(const char* name)
{
	printf("CE-HOST screenshot %s\n", name);
	fflush(stdout);
	// The harness takes it when it reads the line; give it the time.
	snooze(1500000);
}


static int
run_agent(const char* arguments)
{
	BString command;
	command << kAgent << " " << arguments;
	int status = system(command.String());
	return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}


// #pragma mark - the window the captures look at


class PatternView : public BView {
public:
	PatternView(BRect frame)
		:
		BView(frame, "pattern", B_FOLLOW_ALL, B_WILL_DRAW),
		fCenter(make_color(255, 0, 0))
	{
	}

	virtual void Draw(BRect)
	{
		BRect bounds = Bounds();
		float w = bounds.Width() / 2;
		float h = bounds.Height() / 2;
		SetHighColor(250, 250, 250);
		FillRect(BRect(0, 0, w, h));
		SetHighColor(20, 20, 20);
		FillRect(BRect(w + 1, 0, bounds.right, h));
		SetHighColor(200, 160, 40);
		FillRect(BRect(0, h + 1, w, bounds.bottom));
		SetHighColor(40, 120, 200);
		FillRect(BRect(w + 1, h + 1, bounds.right, bounds.bottom));
		SetHighColor(fCenter);
		FillRect(CenterRect());
	}

	BRect CenterRect() const
	{
		BRect bounds = Bounds();
		return BRect(bounds.Width() / 4, bounds.Height() / 4,
			bounds.Width() * 3 / 4, bounds.Height() * 3 / 4);
	}

	void SetCenter(rgb_color color)
	{
		fCenter = color;
		Invalidate(CenterRect());
	}

	void FillAll(rgb_color color)
	{
		fCenter = color;
		SetHighColor(color);
		FillRect(Bounds());
	}

private:
	rgb_color	fCenter;
};


static rgb_color
make(uint8 r, uint8 g, uint8 b)
{
	return make_color(r, g, b);
}


class PatternWindow : public BWindow {
public:
	PatternWindow(BRect frame)
		:
		BWindow(frame, "Capture pattern", B_TITLED_WINDOW,
			B_NOT_MOVABLE | B_NOT_RESIZABLE | B_NOT_ZOOMABLE)
	{
		fView = new PatternView(Bounds());
		AddChild(fView);
	}

	PatternView* View() const { return fView; }

private:
	PatternView*	fView;
};


// #pragma mark - helpers


//! Counts the pixels of two B_RGB32 bitmaps of the same size that differ.
static int32
differing_pixels(const BBitmap* a, const BBitmap* b, BRect area)
{
	int32 count = 0;
	for (int32 y = (int32)area.top; y <= (int32)area.bottom; y++) {
		const uint32* rowA = (const uint32*)((const uint8*)a->Bits()
			+ y * a->BytesPerRow());
		const uint32* rowB = (const uint32*)((const uint8*)b->Bits()
			+ y * b->BytesPerRow());
		for (int32 x = (int32)area.left; x <= (int32)area.right; x++) {
			if ((rowA[x] & 0x00ffffff) != (rowB[x] & 0x00ffffff))
				count++;
		}
	}
	return count;
}


static int32
compare_with_screen(const BBitmap* frame, BRect source)
{
	BBitmap screen(source.OffsetToCopy(B_ORIGIN), B_RGB32);
	BRect bounds = source;
	if (BScreen().ReadBitmap(&screen, false, &bounds) != B_OK)
		return -1;
	return differing_pixels(frame, &screen, screen.Bounds());
}


/*!	Whether a Screen recording process shows a window with this title.
	Asked through scripting (B_GET_PROPERTY "Title" of "Window" n), not
	through app_server's window list: polling get_window_info() while the
	prompt's window was being removed left app_server stuck in three of
	five full runs (the window thread spinning in Desktop::RemoveWindow()'s
	write lock, every other Desktop user waiting; not understood yet, see
	the roadmap's open issues), and the test should not depend on it.
*/
static bool
agent_window_exists(const char* title, team_id* _team = NULL)
{
	BList teams;
	be_roster->GetAppList(SCREEN_CAPTURE_AGENT_SIGNATURE, &teams);
	for (int32 i = 0; i < teams.CountItems(); i++) {
		team_id team = (team_id)(addr_t)teams.ItemAt(i);
		BMessenger messenger(NULL, team);
		for (int32 index = 0; index < 4; index++) {
			BMessage request(B_GET_PROPERTY);
			request.AddSpecifier("Title");
			request.AddSpecifier("Window", index);
			BMessage reply;
			if (messenger.SendMessage(&request, &reply, 1000000, 1000000)
					!= B_OK)
				break;
			const char* name;
			if (reply.FindString("result", &name) != B_OK)
				break;
			if (strcmp(name, title) == 0) {
				if (_team != NULL)
					*_team = team;
				return true;
			}
		}
	}
	return false;
}


static bool
wait_for_agent_window(const char* title, bigtime_t timeout,
	team_id* _team = NULL)
{
	bigtime_t deadline = system_time() + timeout;
	while (system_time() < deadline) {
		if (agent_window_exists(title, _team))
			return true;
		snooze(100000);
	}
	return false;
}


static int32
window_token(BWindow* window)
{
	int32 count = 0;
	int32* tokens = get_token_list(getpid(), &count);
	int32 found = -1;
	for (int32 i = 0; tokens != NULL && i < count; i++) {
		client_window_info* info = get_window_info(tokens[i]);
		if (info != NULL && strcmp(info->name, window->Title()) == 0)
			found = info->server_token;
		free(info);
	}
	free(tokens);
	return found;
}


struct StartArgs {
	BScreenCapture*	capture;
	BRect			region;
	status_t		status;
};


static status_t
start_region_thread(void* data)
{
	StartArgs* args = (StartArgs*)data;
	args->status = args->capture->StartRegion(args->region, 0, 30000000);
	return B_OK;
}


// #pragma mark - tone for the recording


static void
play_tone(void* cookie, void* buffer, size_t size,
	const media_raw_audio_format& format)
{
	double* phase = (double*)cookie;
	if (format.format != media_raw_audio_format::B_AUDIO_FLOAT) {
		memset(buffer, 0, size);
		return;
	}
	float* samples = (float*)buffer;
	size_t frames = size / (sizeof(float) * format.channel_count);
	double step = 2 * M_PI * kToneHertz / format.frame_rate;
	for (size_t i = 0; i < frames; i++) {
		float value = 0.3f * sin(*phase);
		*phase += step;
		for (uint32 c = 0; c < format.channel_count; c++)
			samples[i * format.channel_count + c] = value;
	}
	*phase = fmod(*phase, 2 * M_PI);
}


// #pragma mark - the media node


struct NodeRecording {
	int32		buffers;
	size_t		badSize;
	int32		badTimes;
	bigtime_t	lastTime;
	size_t		expectedSize;
};


static void
node_buffer(void* cookie, bigtime_t timestamp, void* data, size_t size,
	const media_format& format)
{
	NodeRecording* recording = (NodeRecording*)cookie;
	if (recording->buffers > 0 && timestamp <= recording->lastTime)
		recording->badTimes++;
	if (size != recording->expectedSize)
		recording->badSize = size;
	recording->lastTime = timestamp;
	recording->buffers++;
}


static void
test_media_node(BRect region)
{
	BMediaRoster* roster = BMediaRoster::Roster();
	CaptureOptions options;
	options.source = BPrivate::SCREEN_CAPTURE_REGION;
	options.region = region;
	options.frameRate = 10;
	ScreenCaptureNode* node = new ScreenCaptureNode("Screen capture test",
		options);
	status_t status = node->InitCheck();
	if (status == B_OK)
		status = roster->RegisterNode(node);

	NodeRecording recording = { 0, 0, 0, 0,
		(size_t)((region.IntegerWidth() + 1) * (region.IntegerHeight() + 1)
			* 4) };
	BMediaRecorder* recorder = NULL;
	if (status == B_OK) {
		recorder = new BMediaRecorder("capture test recorder",
			B_MEDIA_RAW_VIDEO);
		status = recorder->InitCheck();
	}
	media_output output;
	int32 count = 0;
	if (status == B_OK) {
		status = roster->GetFreeOutputsFor(node->Node(), &output, 1, &count,
			B_MEDIA_RAW_VIDEO);
		if (status == B_OK && count < 1)
			status = B_BUSY;
	}
	if (status == B_OK) {
		recorder->SetHooks(node_buffer, NULL, &recording);
		media_format format = output.format;
		status = recorder->Connect(node->Node(), &output, &format);
	}
	if (status == B_OK) {
		BTimeSource* timeSource = roster->MakeTimeSourceFor(node->Node());
		bigtime_t start = timeSource != NULL ? timeSource->Now() + 20000 : 0;
		if (timeSource != NULL)
			timeSource->Release();
		status = roster->StartNode(node->Node(), start);
		if (status == B_OK)
			status = recorder->Start();
		snooze(2000000);
		recorder->Stop();
		roster->StopNode(node->Node(), 0, true);
		recorder->Disconnect();
	}
	report("media_node", status == B_OK && recording.buffers >= 12
			&& recording.buffers <= 26 && recording.badSize == 0
			&& recording.badTimes == 0,
		"%s; %" B_PRId32 " buffers in 2 s at 10 fps (want 12-26), wrong size "
		"%zu (want %zu), %" B_PRId32 " start times not increasing",
		strerror(status), recording.buffers, recording.badSize,
		recording.expectedSize, recording.badTimes);
	delete recorder;
	if (node->InitCheck() == B_OK && node->ID() > 0)
		roster->UnregisterNode(node);
	node->Release();
}


// #pragma mark - recording through Screenshot


struct ScreenshotRun {
	BString		output;
	int			exitStatus;
	BString		command;
};


static status_t
run_screenshot(void* data)
{
	ScreenshotRun* run = (ScreenshotRun*)data;
	FILE* pipe = popen(run->command.String(), "r");
	if (pipe == NULL) {
		run->exitStatus = -1;
		return B_ERROR;
	}
	char line[512];
	while (fgets(line, sizeof(line), pipe) != NULL)
		run->output << line;
	int status = pclose(pipe);
	run->exitStatus = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
	return B_OK;
}


static char
classify(const uint8* pixel)
{
	int b = pixel[0], g = pixel[1], r = pixel[2];
	if (r > 150 && g < 90 && b < 90)
		return 'R';
	if (g > 150 && r < 90 && b < 90)
		return 'G';
	if (b > 150 && r < 90 && g < 90)
		return 'B';
	return '?';
}


static double
goertzel(const int16* samples, size_t count, size_t stride, double rate,
	double hertz)
{
	double coefficient = 2 * cos(2 * M_PI * hertz / rate);
	double s1 = 0, s2 = 0;
	for (size_t i = 0; i < count; i++) {
		double s0 = samples[i * stride] / 32768.0 + coefficient * s1 - s2;
		s2 = s1;
		s1 = s0;
	}
	return s1 * s1 + s2 * s2 - coefficient * s1 * s2;
}


static void
check_recording(const char* path, BRect region, float frameRate,
	double seconds)
{
	entry_ref ref;
	status_t status = get_ref_for_path(path, &ref);
	BMediaFile* file = NULL;
	if (status == B_OK) {
		file = new BMediaFile(&ref);
		status = file->InitCheck();
	}
	BMediaTrack* video = NULL;
	BMediaTrack* audio = NULL;
	int32 tracks = 0;
	if (status == B_OK) {
		tracks = file->CountTracks();
		for (int32 i = 0; i < tracks; i++) {
			BMediaTrack* track = file->TrackAt(i);
			media_format format;
			track->EncodedFormat(&format);
			if (format.IsVideo() && video == NULL)
				video = track;
			else if (format.IsAudio() && audio == NULL)
				audio = track;
			else
				file->ReleaseTrack(track);
		}
	}
	report("recording_file", status == B_OK && video != NULL && audio != NULL,
		"%s: %s, %" B_PRId32 " tracks (video %s, audio %s)", path,
		strerror(status), tracks, video != NULL ? "yes" : "no",
		audio != NULL ? "yes" : "no");
	if (status != B_OK) {
		delete file;
		return;
	}

	// Video: every frame decoded to B_RGB32; the centre's colour in order.
	int32 width = ((region.IntegerWidth() + 1) & ~1);
	int32 height = ((region.IntegerHeight() + 1) & ~1);
	int64 frames = 0;
	BString sequence;
	BString colours;
	int32 wrongSize = 0;
	if (video != NULL) {
		media_format format;
		format.type = B_MEDIA_RAW_VIDEO;
		format.u.raw_video = media_raw_video_format::wildcard;
		format.u.raw_video.display.format = B_RGB32;
		video->DecodedFormat(&format);
		int32 decodedWidth = format.u.raw_video.display.line_width;
		int32 decodedHeight = format.u.raw_video.display.line_count;
		int32 bpr = format.u.raw_video.display.bytes_per_row;
		if (bpr <= 0)
			bpr = decodedWidth * 4;
		if (decodedWidth != width || decodedHeight != height)
			wrongSize++;
		std::vector<uint8> buffer((size_t)bpr * decodedHeight + 4096);
		int64 count = 1;
		media_header header;
		while (video->ReadFrames(buffer.data(), &count, &header) == B_OK
				&& count > 0) {
			frames++;
			const uint8* center = buffer.data()
				+ (decodedHeight / 2) * bpr + (decodedWidth / 2) * 4;
			char c = classify(center);
			colours << c;
			if (c != '?' && (sequence.Length() == 0
					|| sequence[sequence.Length() - 1] != c)) {
				sequence << c;
			}
			count = 1;
		}
		file->ReleaseTrack(video);
	}
	int64 expectedFrames = (int64)(seconds * frameRate);
	report("recording_video_frames", frames >= expectedFrames - 4
			&& frames <= expectedFrames + 6 && wrongSize == 0,
		"%" B_PRId64 " frames (want about %" B_PRId64 " for %.1f s at %.0f "
		"fps), %dx%d", frames, expectedFrames, seconds, frameRate,
		(int)width, (int)height);
	report("recording_video_matches_screen", sequence.FindFirst("RGB") >= 0,
		"centre colours in order: \"%s\" (want R, G, B; frames: %s)",
		sequence.String(), colours.String());

	// Audio: decoded as 16-bit; the tone in the middle, and as long as
	// the video.
	int64 audioFrames = 0;
	double purity = 0;
	double level = 0;
	if (audio != NULL) {
		media_format format;
		format.type = B_MEDIA_RAW_AUDIO;
		format.u.raw_audio = media_raw_audio_format::wildcard;
		format.u.raw_audio.format = media_raw_audio_format::B_AUDIO_SHORT;
		audio->DecodedFormat(&format);
		uint32 channels = max_c(format.u.raw_audio.channel_count, 1);
		double rate = format.u.raw_audio.frame_rate;
		std::vector<int16> all;
		std::vector<int16> buffer(65536 * channels);
		int64 count;
		media_header header;
		while (true) {
			count = 0;
			if (audio->ReadFrames(buffer.data(), &count, &header) != B_OK
				|| count <= 0)
				break;
			all.insert(all.end(), buffer.begin(),
				buffer.begin() + count * channels);
			audioFrames += count;
		}
		file->ReleaseTrack(audio);
		// The middle second: the tone played throughout.
		size_t start = (size_t)(rate * 1.0);
		size_t length = (size_t)rate;
		if (rate > 0 && all.size() / channels >= start + length) {
			const int16* window = all.data() + start * channels;
			double energy = 0;
			for (size_t i = 0; i < length; i++) {
				double v = window[i * channels] / 32768.0;
				energy += v * v;
			}
			level = sqrt(energy / length) * sqrt(2.0);
			purity = energy > 0 ? 2 * goertzel(window, length, channels, rate,
				kToneHertz) / (length * energy) : 0;
		}
		audioFrames = (int64)(audioFrames * 48000.0 / (rate > 0 ? rate : 1));
	}
	report("recording_audio_present", purity > 0.8 && level > 0.15
			&& level < 0.45,
		"%.0f Hz tone in the middle second: purity %.3f, amplitude %.3f "
		"(played at 0.3)", kToneHertz, purity, level);
	double videoSeconds = frames / frameRate;
	double audioSeconds = audioFrames / 48000.0;
	report("recording_av_length", fabs(videoSeconds - audioSeconds) < 0.4
			&& fabs(videoSeconds - seconds) < 0.8,
		"video %.2f s, audio %.2f s, recorded %.1f s", videoSeconds,
		audioSeconds, seconds);
	delete file;
}


static void
test_screenshot_recording(PatternWindow* window, BRect region)
{
	// What the Media Kit can write today (the FFmpeg plug-in).
	{
		BString formats;
		media_file_format format;
		int32 cookie = 0;
		while (get_next_file_format(&cookie, &format) == B_OK)
			formats << format.short_name << " ";
		printf("CE-INFO capture: file formats: %s\n", formats.String());
		CaptureOptions options;
		BString description;
		status_t status = BPrivate::CaptureSession::CheckEncoders(options,
			&description);
		report("encoders", status == B_OK, "%s: %s", strerror(status),
			description.String());
	}

	// Screenshot asks too; allow it beforehand, as the user would once.
	int result = run_agent("--set application/x-vnd.haiku-screenshot allow");
	report("screenshot_permission", result == 0, "ScreenRecording --set ... "
		"allow: exit %d", result);

	double phase = 0;
	BSoundPlayer player("capture test tone", play_tone, NULL, &phase);
	player.Start();
	player.SetHasData(true);
	snooze(500000);

	const double kSeconds = 4.0;
	const float kFrameRate = 10;
	const char* kFile = "/tmp/capture_test.mkv";
	unlink(kFile);
	ScreenshotRun run;
	run.exitStatus = -1;
	run.command.SetToFormat("/boot/system/apps/Screenshot --record %.1f "
		"--region %d,%d,%d,%d --file %s --system-audio --frame-rate %.0f "
		"--quit 2>&1", kSeconds, (int)region.left, (int)region.top,
		(int)region.IntegerWidth() + 1, (int)region.IntegerHeight() + 1, kFile,
		kFrameRate);
	thread_id thread = spawn_thread(run_screenshot, "screenshot", 10, &run);
	resume_thread(thread);

	// Wait for the recording to start (Screenshot's window comes up first),
	// then show red, green and blue, a second and more each.
	bigtime_t deadline = system_time() + 30000000;
	bool recording = false;
	while (!recording && system_time() < deadline) {
		BMessage sessions;
		BScreenCapture::GetSessions(&sessions);
		// Running and writing: it has taken its first frames (the
		// encoders and the audio are set up before the first frame).
		const char* signature;
		for (int32 i = 0; sessions.FindString("signature", i, &signature)
				== B_OK; i++) {
			if (strcasecmp(signature, kScreenshotSignature) == 0
				&& sessions.GetInt64("frames", i, 0) > 0)
				recording = true;
		}
		if (!recording)
			snooze(50000);
	}
	report("screenshot_recording_started", recording,
		"a capture session of %s %s", kScreenshotSignature,
		recording ? "runs" : "never appeared");

	const rgb_color colours[3] = { make(255, 0, 0), make(0, 255, 0),
		make(0, 0, 255) };
	for (int32 i = 0; i < 3; i++) {
		if (window->Lock()) {
			window->View()->FillAll(colours[i]);
			window->View()->Sync();
			window->Unlock();
		}
		if (i == 1) {
			// Screenshot's window shows "Recording ...", the indicator is
			// up.
			printf("CE-HOST screenshot screenshot_recording\n");
			fflush(stdout);
		}
		snooze(1300000);
	}

	status_t result2;
	wait_for_thread(thread, &result2);
	player.SetHasData(false);
	player.Stop();

	BString output = run.output;
	output.RemoveAll("\n");
	// Screenshot's exit status is BApplication::Run()'s thread id (upstream
	// main() returns it), so only its report counts.
	report("screenshot_recorded", run.output.FindFirst("recording saved") >= 0
			&& run.output.FindFirst("(No error)") >= 0,
		"exit %d, \"%s\"", run.exitStatus, output.String());
	check_recording(kFile, region, kFrameRate, kSeconds);
}


// #pragma mark - main checks


static void
run_tests(PatternWindow* window)
{
	BRect region;
	if (window->Lock()) {
		region = window->ConvertToScreen(window->Bounds());
		window->Unlock();
	}

	// Ask again, as on a fresh system.
	int result = run_agent(BString("--set ").Append(kSignature)
		.Append(" ask").String());
	report("agent_cli", result == 0, "ScreenRecording --set %s ask: exit %d",
		kSignature, result);

	// The prompt.
	BScreenCapture capture;
	StartArgs args = { &capture, region, B_ERROR };
	thread_id thread = spawn_thread(start_region_thread, "start", 10, &args);
	resume_thread(thread);
	bool prompted = wait_for_agent_window("Screen recording", 20000000);
	report("prompt_shown", prompted, "the Screen recording prompt %s",
		prompted ? "is up" : "did not appear within 20 s");
	if (prompted) {
		snooze(1000000);
		screenshot("capture_prompt");
		printf("CE-HOST sendkey ret\n");
		fflush(stdout);
	}
	status_t threadResult;
	wait_for_thread(thread, &threadResult);
	report("prompt_allows", args.status == B_OK, "StartRegion after "
		"\"Allow\": %s", strerror(args.status));

	BMessage permissions;
	BScreenCapture::GetPermissions(&permissions);
	int32 state = -1;
	const char* signature;
	for (int32 i = 0; permissions.FindString("signature", i, &signature)
			== B_OK; i++) {
		if (strcasecmp(signature, kSignature) == 0)
			state = permissions.GetInt32("state", i, -1);
	}
	report("permission_remembered",
		state == BPrivate::SCREEN_CAPTURE_PERMISSION_ALLOWED,
		"state %" B_PRId32 " (want %d, allowed)", state,
		BPrivate::SCREEN_CAPTURE_PERMISSION_ALLOWED);
	if (args.status != B_OK)
		return;

	bool indicator = wait_for_agent_window("Recording", 10000000);
	report("indicator_shown", indicator, "the recording indicator %s",
		indicator ? "is up" : "did not appear within 10 s");
	if (indicator)
		screenshot("capture_indicator");

	// Frames.
	BRect frame = capture.Frame();
	report("region_frame", frame == region, "frame %.0f,%.0f-%.0f,%.0f "
		"(want %.0f,%.0f-%.0f,%.0f)", frame.left, frame.top, frame.right,
		frame.bottom, region.left, region.top, region.right, region.bottom);
	BBitmap bitmap(frame.OffsetToCopy(B_ORIGIN), B_RGB32);
	bool changed = false;
	BRegion damage;
	bigtime_t times[4] = { 0, 0, 0, 0 };
	bigtime_t before = system_time();
	status_t status = capture.NextFrame(&bitmap, &changed, &damage, &times[0]);
	int32 differing = compare_with_screen(&bitmap, frame);
	report("frames_match_screen", status == B_OK && changed && differing == 0,
		"%s; changed %d; %" B_PRId32 " of %.0f pixels differ from "
		"BScreen::ReadBitmap()", strerror(status), changed, differing,
		frame.Width() * frame.Height());

	snooze(300000);
	status = capture.NextFrame(&bitmap, &changed, &damage, &times[1]);
	report("static_no_damage", status == B_OK && !changed
			&& damage.CountRects() == 0,
		"%s; changed %d, %" B_PRId32 " damage rects on an unchanged screen",
		strerror(status), changed, damage.CountRects());

	BRect center;
	if (window->Lock()) {
		center = window->View()->CenterRect();
		window->View()->SetCenter(make(0, 200, 0));
		window->View()->Sync();
		window->Unlock();
	}
	snooze(100000);
	status = capture.NextFrame(&bitmap, &changed, &damage, &times[2]);
	BRect damageFrame = damage.Frame();
	BRect grown = center.InsetByCopy(-2, -2);
	bool covers = damage.Contains(center.LeftTop())
		&& damage.Contains(center.RightBottom());
	bool tight = grown.Contains(damageFrame);
	differing = compare_with_screen(&bitmap, frame);
	report("damage_region", status == B_OK && changed && covers && tight,
		"%s; damage %.0f,%.0f-%.0f,%.0f for the redrawn %.0f,%.0f-%.0f,%.0f",
		strerror(status), damageFrame.left, damageFrame.top,
		damageFrame.right, damageFrame.bottom, center.left, center.top,
		center.right, center.bottom);
	report("frame_updated", status == B_OK && differing == 0,
		"%" B_PRId32 " pixels differ from the screen after the change",
		differing);
	status = capture.NextFrame(&bitmap, &changed, &damage, &times[3]);
	bigtime_t after = system_time();
	report("timestamps", times[0] >= before && times[0] < times[1]
			&& times[1] < times[2] && times[2] < times[3] && times[3] <= after,
		"%" B_PRIdBIGTIME ", +%" B_PRIdBIGTIME ", +%" B_PRIdBIGTIME ", +%"
		B_PRIdBIGTIME " us", times[0] - before, times[1] - times[0],
		times[2] - times[1], times[3] - times[2]);

	// The settings window, with this session listed.
	team_id settingsTeam = -1;
	if (be_roster->Launch(SCREEN_CAPTURE_AGENT_SIGNATURE, (BMessage*)NULL,
			&settingsTeam) == B_OK) {
		bool shown = wait_for_agent_window("Screen recording", 10000000);
		report("settings_shown", shown, "the Screen recording preferences %s",
			shown ? "are up" : "did not appear");
		snooze(1500000);
		screenshot("capture_settings");
		BMessenger(NULL, settingsTeam).SendMessage(B_QUIT_REQUESTED);
	} else
		report("settings_shown", false, "could not launch %s", kAgent);
	capture.Stop();

	// One window.
	int32 token = window_token(window);
	BScreenCapture windowCapture;
	status = windowCapture.StartWindow(token, 0, 5000000);
	BRect windowFrame = windowCapture.Frame();
	report("window_frame", status == B_OK && windowFrame == region,
		"%s; token %" B_PRId32 ", frame %.0f,%.0f-%.0f,%.0f (want the "
		"window's %.0f,%.0f-%.0f,%.0f)", strerror(status), token,
		windowFrame.left, windowFrame.top, windowFrame.right,
		windowFrame.bottom, region.left, region.top, region.right,
		region.bottom);
	if (status == B_OK) {
		BBitmap windowBitmap(windowFrame.OffsetToCopy(B_ORIGIN), B_RGB32);
		status = windowCapture.NextFrame(&windowBitmap, &changed);
		differing = compare_with_screen(&windowBitmap, windowFrame);
		report("window_matches", status == B_OK && differing == 0,
			"%s; %" B_PRId32 " pixels differ", strerror(status), differing);
	}

	// Revoked while capturing: the session ends; a new one is refused.
	result = run_agent(BString("--set ").Append(kSignature)
		.Append(" deny").String());
	status = windowCapture.NextFrame(&bitmap, &changed);
	report("revoked_stops", result == 0 && status == B_CANCELED,
		"after --set deny: NextFrame %s (want %s)", strerror(status),
		strerror(B_CANCELED));
	windowCapture.Stop();
	BScreenCapture denied;
	bigtime_t start = system_time();
	status = denied.StartScreen(0, 5000000);
	report("denied", status == B_NOT_ALLOWED
			&& system_time() - start < 1000000,
		"StartScreen when denied: %s after %" B_PRIdBIGTIME " us",
		strerror(status), system_time() - start);
	run_agent(BString("--set ").Append(kSignature).Append(" allow").String());

	// The Media Kit node.
	test_media_node(region);

	// Screenshot.
	test_screenshot_recording(window, region);
}


class TestApplication : public BApplication {
public:
	TestApplication()
		:
		BApplication(kSignature),
		fWindow(NULL)
	{
	}

	virtual void ReadyToRun()
	{
		// Bottom left: away from the Screenshot window and the prompt, which
		// are centred, and from the indicator in the top right corner.
		BRect screen = BScreen().Frame();
		BRect frame(16, screen.bottom - 150, 16 + 159, screen.bottom - 150
			+ 119);
		fWindow = new PatternWindow(frame);
		fWindow->Show();
		thread_id thread = spawn_thread(_Run, "capture checks",
			B_NORMAL_PRIORITY, this);
		resume_thread(thread);
	}

private:
	static status_t _Run(void* data)
	{
		TestApplication* app = (TestApplication*)data;
		snooze(1500000);
		run_tests(app->fWindow);
		app->PostMessage(B_QUIT_REQUESTED);
		return B_OK;
	}

	PatternWindow*	fWindow;
};


int
main()
{
	signal(SIGPIPE, SIG_IGN);
	printf("CE-SUITE " SUITE " START\n");
	fflush(stdout);

	if (access(kAgent, X_OK) != 0) {
		printf("CE-INFO capture: no %s in this image, skipped\n", kAgent);
		printf("CE-SUITE " SUITE " DONE failures=0\n");
		return 0;
	}
	// Windows need app_server and input_server, which can come up late on a
	// full image without KVM.
	for (int i = 0; i < 300; i++) {
		if (be_roster->IsRunning("application/x-vnd.Be-input_server"))
			break;
		if (i > 0 && i % 30 == 0) {
			printf("CE-INFO capture: waiting for input_server (%d s)\n", i);
			fflush(stdout);
		}
		snooze(1000000);
	}

	TestApplication app;
	app.Run();

	printf("CE-SUITE " SUITE " DONE failures=%d\n", sFailures);
	fflush(stdout);
	return sFailures == 0 ? 0 : 1;
}
