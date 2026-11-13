/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 */

/*!	Media Kit test of the mixer's routing and loopback additions requested
	by Renga's partner projects (roadmap F-58; OBS Studio's requests O-1 to
	O-4 in Techniix/OBS-Studio_HaikuOS, docs/haiku/RENGA-REQUESTS.md).

	Applications are child processes of this program, each a BApplication
	with its own signature and a BSoundPlayer, as in media_test: A plays on
	the left channel only, B on the right only. Their tones (5100 Hz and
	up) are none the harness looks for in the cards' recordings, so the
	host's phase analyses of the other Media Kit tests are not disturbed.
	Everything is checked in the guest, through loopback recordings.

	Checks print "CE-TEST media_routing.<name> PASS|FAIL <detail>".
*/


#include <Application.h>
#include <Autolock.h>
#include <Locker.h>
#include <MediaAddOn.h>
#include <MediaRecorder.h>
#include <MediaRoster.h>
#include <Messenger.h>
#include <Roster.h>
#include <OS.h>
#include <ParameterWeb.h>
#include <SoundPlayer.h>
#include <String.h>
#include <StringList.h>

#include <AudioStreamControl.h>

#include <math.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <vector>


using BPrivate::AudioStream;
using BPrivate::AudioStreamControl;


#define SUITE "media_routing"

static const char* kSignatureA = "application/x-vnd.Renga-routing-test-a";
static const char* kSignatureB = "application/x-vnd.Renga-routing-test-b";
// Two processes of one application (OBS's browser helpers): O-3.
static const char* kSignatureC = "application/x-vnd.Renga-routing-test-c";
// A browser helper that OBS makes loopback-only by team for now (F-58
// batch 2, O-17 and O-18).
static const char* kSignatureD = "application/x-vnd.Renga-routing-test-d";
static const float kAmplitude = 0.25f;
// A level below this is silence (a stream left out of a recording).
static const double kSilence = 0.005;

static int sFailures = 0;


static void
report(const char* name, bool pass, const char* format, ...)
{
	char detail[1024];
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


// #pragma mark - player (child process)


struct PlayerState {
	volatile float	leftHertz;
	volatile float	rightHertz;
	volatile float	target;
	float			amplitude;
	double			leftPhase;
	double			rightPhase;
	float			rate;
};


static void
play_buffer(void* cookie, void* buffer, size_t size,
	const media_raw_audio_format& format)
{
	PlayerState* state = (PlayerState*)cookie;
	float* data = (float*)buffer;
	size_t frames = size / (sizeof(float) * format.channel_count);
	float step = 1.0f / (state->rate / 100);
	for (size_t i = 0; i < frames; i++) {
		if (state->amplitude < state->target)
			state->amplitude = min_c(state->target, state->amplitude + step);
		else if (state->amplitude > state->target)
			state->amplitude = max_c(state->target, state->amplitude - step);
		data[i * format.channel_count] = state->leftHertz > 0
			? state->amplitude * sin(state->leftPhase) : 0;
		if (format.channel_count > 1) {
			data[i * format.channel_count + 1] = state->rightHertz > 0
				? state->amplitude * sin(state->rightPhase) : 0;
		}
		state->leftPhase += 2 * M_PI * state->leftHertz / state->rate;
		state->rightPhase += 2 * M_PI * state->rightHertz / state->rate;
	}
	state->leftPhase = fmod(state->leftPhase, 2 * M_PI);
	state->rightPhase = fmod(state->rightPhase, 2 * M_PI);
}


/*!	A child: plays "tone <left Hz> <right Hz>" read from stdin until "quit"
	or the end of input. "team" prints its team id on stdout. With \a wait
	(F-58 batch 2) it connects to the mixer only after a line "start", so
	that the parent can set things up for its team first.
*/
static int
run_player(const char* signature, bool wait = false)
{
	BApplication app(signature);

	char line[128];
	if (wait) {
		while (fgets(line, sizeof(line), stdin) != NULL
			&& strncmp(line, "start", 5) != 0) {
			if (strncmp(line, "quit", 4) == 0)
				return 0;
		}
	}

	media_raw_audio_format format = media_raw_audio_format::wildcard;
	format.format = media_raw_audio_format::B_AUDIO_FLOAT;
	format.channel_count = 2;
	format.frame_rate = 48000;
	format.byte_order = B_MEDIA_HOST_ENDIAN;
	format.buffer_size = 1024 * 2 * sizeof(float);

	PlayerState state = { 0, 0, 0, 0, 0, 0, 48000 };
	BSoundPlayer player(&format, "media_routing player", play_buffer, NULL,
		&state);
	if (player.InitCheck() != B_OK) {
		fprintf(stderr, "player: %s\n", strerror(player.InitCheck()));
		return 1;
	}
	state.rate = player.Format().frame_rate;
	player.Start();
	player.SetHasData(true);

	while (fgets(line, sizeof(line), stdin) != NULL) {
		float left, right;
		if (sscanf(line, "tone %f %f", &left, &right) == 2) {
			state.leftHertz = left;
			state.rightHertz = right;
			state.target = kAmplitude;
		} else if (strncmp(line, "quit", 4) == 0)
			break;
	}

	state.target = 0;
	snooze(150000);
	player.Stop();
	return 0;
}


struct Player {
	FILE*		pipe;
	const char*	signature;
};


static bool
start_player(Player& player, const char* self, const char* signature,
	bool wait = false)
{
	BString command;
	command << "'" << self << "' " << (wait ? "player-wait" : "player")
		<< " " << signature;
	player.pipe = popen(command.String(), "w");
	player.signature = signature;
	return player.pipe != NULL;
}


static void
tone(Player& player, int left, int right)
{
	if (player.pipe == NULL)
		return;
	fprintf(player.pipe, "tone %d %d\n", left, right);
	fflush(player.pipe);
}


static void
stop_player(Player& player)
{
	if (player.pipe == NULL)
		return;
	fprintf(player.pipe, "quit\n");
	fflush(player.pipe);
	pclose(player.pipe);
	player.pipe = NULL;
}


// #pragma mark - recording


struct Recording {
	BLocker				lock;
	std::vector<float>	left;
	std::vector<float>	right;
	float				rate;

	Recording()
		:
		lock("recording"),
		rate(0)
	{
	}
};


static void
record_buffer(void* cookie, bigtime_t timestamp, void* data, size_t size,
	const media_format& format)
{
	Recording* recording = (Recording*)cookie;
	const media_raw_audio_format& raw = format.u.raw_audio;
	size_t sampleSize = raw.format & media_raw_audio_format::B_AUDIO_SIZE_MASK;
	if (sampleSize == 0 || raw.channel_count == 0)
		return;
	size_t frames = size / (sampleSize * raw.channel_count);

	BAutolock locker(recording->lock);
	recording->rate = raw.frame_rate;
	for (size_t i = 0; i < frames; i++) {
		float values[2] = { 0, 0 };
		for (uint32 c = 0; c < 2 && c < raw.channel_count; c++) {
			size_t index = i * raw.channel_count + c;
			switch (raw.format) {
				case media_raw_audio_format::B_AUDIO_FLOAT:
					values[c] = ((float*)data)[index];
					break;
				case media_raw_audio_format::B_AUDIO_SHORT:
					values[c] = ((int16*)data)[index] / 32768.0f;
					break;
				default:
					break;
			}
		}
		if (raw.channel_count == 1)
			values[1] = values[0];
		recording->left.push_back(values[0]);
		recording->right.push_back(values[1]);
	}
}


//!	A loopback and a recorder connected to it; several can run at once.
struct LoopbackRecorder {
	media_node			node;
	bool				haveNode;
	BMediaRecorder*		recorder;
	Recording			recording;

	LoopbackRecorder()
		:
		haveNode(false),
		recorder(NULL)
	{
	}

	~LoopbackRecorder()
	{
		Stop();
	}

	//!	A new loopback of \a signature (NULL: every application).
	status_t Create(const char* signature)
	{
		status_t status = AudioStreamControl::StartLoopback(signature, &node);
		haveNode = status == B_OK;
		return status;
	}

	status_t Start()
	{
		BMediaRoster* roster = BMediaRoster::Roster();
		media_output output;
		int32 count;
		status_t status = roster->GetFreeOutputsFor(node, &output, 1, &count,
			B_MEDIA_RAW_AUDIO);
		if (status != B_OK)
			return status;
		if (count < 1)
			return B_BUSY;
		recorder = new BMediaRecorder("media_routing recorder",
			B_MEDIA_RAW_AUDIO);
		if (recorder->InitCheck() != B_OK)
			return recorder->InitCheck();
		recorder->SetHooks(record_buffer, NULL, &recording);
		media_format format = output.format;
		status = recorder->Connect(node, &output, &format);
		if (status != B_OK)
			return status;
		return recorder->Start();
	}

	void Stop()
	{
		if (recorder != NULL) {
			recorder->Stop();
			recorder->Disconnect();
			delete recorder;
			recorder = NULL;
		}
		if (haveNode) {
			BMediaRoster::Roster()->ReleaseNode(node);
			haveNode = false;
		}
	}
};


static double
goertzel(const float* samples, size_t count, double rate, double hertz)
{
	double coefficient = 2 * cos(2 * M_PI * hertz / rate);
	double s1 = 0, s2 = 0;
	for (size_t i = 0; i < count; i++) {
		double s0 = samples[i] + coefficient * s1 - s2;
		s2 = s1;
		s1 = s0;
	}
	return s1 * s1 + s2 * s2 - coefficient * s1 * s2;
}


struct ChannelCheck {
	double	amplitude;
	double	minPurity;
};


//!	Level (sine amplitude from the RMS) and worst 100 ms purity of \a hertz
//!	after \a skip seconds.
static ChannelCheck
check_channel(Recording& recording, bool right, double skip, double hertz)
{
	BAutolock locker(recording.lock);
	const std::vector<float>& samples = right ? recording.right
		: recording.left;
	double rate = recording.rate;
	ChannelCheck result = { 0, 0 };
	size_t start = (size_t)(skip * rate);
	if (rate <= 0 || samples.size() <= start + (size_t)(rate / 10))
		return result;
	double energy = 0;
	for (size_t i = start; i < samples.size(); i++)
		energy += (double)samples[i] * samples[i];
	result.amplitude = sqrt(energy / (samples.size() - start)) * sqrt(2.0);

	result.minPurity = 1.0;
	size_t window = (size_t)(rate / 10);
	if (hertz <= 0)
		return result;
	for (size_t w = start; w + window <= samples.size(); w += window) {
		double windowEnergy = 0;
		for (size_t i = w; i < w + window; i++)
			windowEnergy += (double)samples[i] * samples[i];
		double purity = windowEnergy > 0
			? 2 * goertzel(&samples[w], window, rate, hertz)
				/ (window * windowEnergy) : 0;
		result.minPurity = min_c(result.minPurity, purity);
	}
	return result;
}


static bool
present(const ChannelCheck& check)
{
	return fabs(check.amplitude - kAmplitude) <= 0.1 * kAmplitude
		&& check.minPurity > 0.95;
}


static bool
absent(const ChannelCheck& check)
{
	return check.amplitude < kSilence;
}


/*!	Records \a seconds from a new loopback of \a signature (NULL: all) with
	the exclusion list \a exclude (NULL: none set) and checks the left
	(A's) and right (B's) channels.
*/
static status_t
record(const char* signature, const char* exclude, double seconds,
	double leftHertz, double rightHertz, ChannelCheck& left,
	ChannelCheck& right)
{
	LoopbackRecorder loopback;
	status_t status = loopback.Create(signature);
	if (status == B_OK && exclude != NULL) {
		status = AudioStreamControl::SetLoopbackExclusions(loopback.node,
			exclude);
	}
	if (status == B_OK)
		status = loopback.Start();
	if (status == B_OK)
		snooze((bigtime_t)(seconds * 1000000));
	loopback.Stop();
	left = check_channel(loopback.recording, false, 0.3, leftHertz);
	right = check_channel(loopback.recording, true, 0.3, rightHertz);
	return status;
}


static bool
wait_for_mixer(bigtime_t timeout)
{
	bigtime_t deadline = system_time() + timeout;
	while (system_time() < deadline) {
		status_t status;
		BMediaRoster* roster = BMediaRoster::Roster(&status);
		if (roster != NULL) {
			media_node mixer;
			if (roster->GetAudioMixer(&mixer) == B_OK) {
				media_output output;
				int32 count = 0;
				bool connected = roster->GetConnectedOutputsFor(mixer,
					&output, 1, &count) == B_OK && count == 1;
				roster->ReleaseNode(mixer);
				if (connected)
					return true;
			}
		}
		snooze(250000);
	}
	return false;
}


static bool
find_stream(AudioStreamControl& control, const char* signature,
	AudioStream& stream, bigtime_t timeout = 10000000)
{
	bigtime_t deadline = system_time() + timeout;
	do {
		if (control.Update() == B_OK) {
			const AudioStream* found = control.FindStream(signature);
			if (found != NULL) {
				stream = *found;
				return true;
			}
		}
		snooze(200000);
	} while (system_time() < deadline);
	return false;
}


// #pragma mark - O-1: exclusions


static void
test_exclusions()
{
	ChannelCheck left, right;

	// OBS's case: "What you hear" without the recorder's own monitoring.
	status_t status = record(NULL, kSignatureA, 1.2, 5100, 5300, left,
		right);
	report("exclude_one", status == B_OK && absent(left) && present(right),
		"%s; all less A: L (A) %.4f, R (B) 5300 Hz %.3f purity >= %.3f",
		strerror(status), left.amplitude, right.amplitude, right.minPurity);

	// The parameter by kind, read back normalized: blanks, carriage
	// returns and empty lines dropped, case kept; matching ignores case.
	{
		LoopbackRecorder loopback;
		status = loopback.Create(NULL);
		BString list;
		list << "  " << BString(kSignatureA).ToUpper() << " \r\n\n"
			<< "application/x-vnd.Renga-not-running\n";
		if (status == B_OK) {
			status = AudioStreamControl::SetLoopbackExclusions(loopback.node,
				list.String());
		}
		BString readBack;
		if (status == B_OK) {
			status = AudioStreamControl::GetLoopbackText(loopback.node,
				RENGA_LOOPBACK_EXCLUDE_KIND, readBack);
		}
		BString expected;
		expected << BString(kSignatureA).ToUpper() << "\n"
			<< "application/x-vnd.Renga-not-running\n";
		if (status == B_OK)
			status = loopback.Start();
		if (status == B_OK)
			snooze(1200000);
		loopback.Stop();
		left = check_channel(loopback.recording, false, 0.3, 5100);
		right = check_channel(loopback.recording, true, 0.3, 5300);
		report("exclude_normalized", status == B_OK && readBack == expected
				&& absent(left) && present(right),
			"%s; read back \"%s\" (want \"%s\"); L (A) %.4f, R (B) %.3f",
			strerror(status), BString(readBack).ReplaceAll("\n", "\\n")
				.String(),
			BString(expected).ReplaceAll("\n", "\\n").String(),
			left.amplitude, right.amplitude);
	}

	// Two applications left out.
	BString both;
	both << kSignatureA << "\n" << kSignatureB;
	status = record(NULL, both.String(), 1.0, 5100, 5300, left, right);
	report("exclude_two", status == B_OK && absent(left) && absent(right),
		"%s; all less A and B: L %.4f, R %.4f", strerror(status),
		left.amplitude, right.amplitude);

	// A loopback of one application: the exclusions apply there too.
	status = record(kSignatureA, kSignatureB, 1.0, 5100, 5300, left, right);
	ChannelCheck selfLeft, selfRight;
	status_t selfStatus = record(kSignatureA, kSignatureA, 1.0, 5100, 5300,
		selfLeft, selfRight);
	report("exclude_with_application", status == B_OK && present(left)
			&& absent(right) && selfStatus == B_OK && absent(selfLeft)
			&& absent(selfRight),
		"%s; A's loopback less B: L %.3f, R %.4f; A's loopback less A (%s): "
		"L %.4f, R %.4f", strerror(status), left.amplitude, right.amplitude,
		strerror(selfStatus), selfLeft.amplitude, selfRight.amplitude);

	// A loopback with exclusions next to a plain one: the mix of every
	// stream is shared between plain loopbacks only.
	{
		LoopbackRecorder plain, excluding;
		status = plain.Create(NULL);
		if (status == B_OK)
			status = excluding.Create(NULL);
		if (status == B_OK) {
			status = AudioStreamControl::SetLoopbackExclusions(
				excluding.node, kSignatureB);
		}
		if (status == B_OK)
			status = plain.Start();
		if (status == B_OK)
			status = excluding.Start();
		if (status == B_OK)
			snooze(1200000);
		plain.Stop();
		excluding.Stop();
		ChannelCheck plainLeft = check_channel(plain.recording, false, 0.3,
			5100);
		ChannelCheck plainRight = check_channel(plain.recording, true, 0.3,
			5300);
		left = check_channel(excluding.recording, false, 0.3, 5100);
		right = check_channel(excluding.recording, true, 0.3, 5300);
		report("exclude_beside_plain", status == B_OK && present(plainLeft)
				&& present(plainRight) && present(left) && absent(right),
			"%s; plain: L %.3f, R %.3f; less B: L %.3f, R %.4f",
			strerror(status), plainLeft.amplitude, plainRight.amplitude,
			left.amplitude, right.amplitude);
	}

	// An empty list records everything again, on the same loopback.
	{
		LoopbackRecorder loopback;
		status = loopback.Create(NULL);
		if (status == B_OK) {
			status = AudioStreamControl::SetLoopbackExclusions(loopback.node,
				kSignatureA);
		}
		if (status == B_OK) {
			status = AudioStreamControl::SetLoopbackExclusions(loopback.node,
				"");
		}
		if (status == B_OK)
			status = loopback.Start();
		if (status == B_OK)
			snooze(1200000);
		loopback.Stop();
		left = check_channel(loopback.recording, false, 0.3, 5100);
		right = check_channel(loopback.recording, true, 0.3, 5300);
		report("exclude_cleared", status == B_OK && present(left)
				&& present(right),
			"%s; L %.3f, R %.3f", strerror(status), left.amplitude,
			right.amplitude);
	}
}


// #pragma mark - O-4: routes by device name


static const char* kUsbDevice = "QEMU USB Audio";


static void
host(const char* request)
{
	printf("CE-HOST %s\n", request);
	fflush(stdout);
}


/*!	Waits until \a test holds for A's stream (re-read every 200 ms). */
template<typename Test>
static bool
wait_stream(AudioStreamControl& control, AudioStream& stream,
	bigtime_t timeout, Test test)
{
	bigtime_t deadline = system_time() + timeout;
	do {
		if (control.Update() == B_OK) {
			const AudioStream* found = control.FindStream(kSignatureA);
			if (found != NULL) {
				stream = *found;
				if (test(control, stream))
					return true;
			}
		}
		snooze(200000);
	} while (system_time() < deadline);
	return false;
}


/*!	A stream routed by name to a device that is not there yet keeps the
	route, plays on the default output, moves to the device when it is
	plugged in (QEMU's usb-audio; the harness finds A's 5150 Hz in that
	device's usb-<n>.wav, media_routing_audio.route_to_usb), and keeps the
	route when it is pulled out again.
*/
static void
test_routes(Player& a)
{
	AudioStreamControl control;
	AudioStream stream;
	if (!wait_stream(control, stream, 5000000,
			[](AudioStreamControl&, AudioStream&) { return true; })) {
		report("route_absent_kept", false, "A's stream not listed");
		return;
	}
	bool absent = !control.Outputs().HasString(kUsbDevice);
	status_t status = control.SetRoute(stream.id, kUsbDevice);
	bool kept = wait_stream(control, stream, 5000000,
		[](AudioStreamControl& control, AudioStream& stream) {
			return stream.route == kUsbDevice
				&& stream.output == control.Outputs().CountStrings();
		});
	report("route_absent_kept", absent && status == B_OK && kept
			&& stream.hasRoute,
		"%s absent: %s; SetRoute: %s; route \"%s\", Output index %" B_PRId32
		" of %" B_PRId32 " choices (the \"not available\" item)", kUsbDevice,
		absent ? "yes" : "no", strerror(status), stream.route.String(),
		stream.output, control.Outputs().CountStrings());

	// Still heard meanwhile (on the default output; the loopback records
	// it either way).
	ChannelCheck left, right;
	status = record(kSignatureA, NULL, 1.0, 5100, 0, left, right);
	report("route_absent_plays", status == B_OK && present(left),
		"%s; A while its device is absent: L %.3f", strerror(status),
		left.amplitude);

	tone(a, 5150, 0);
	host("usb_audio add");
	bool applied = wait_stream(control, stream, 30000000,
		[](AudioStreamControl& control, AudioStream& stream) {
			int32 index = control.Outputs().IndexOf(kUsbDevice);
			return index > 0 && stream.output == index;
		});
	report("route_applied", applied,
		"after the plug-in: route \"%s\", Output index %" B_PRId32 " (%s)",
		stream.route.String(), stream.output,
		stream.output >= 0 && stream.output < control.Outputs().CountStrings()
			? control.Outputs().StringAt(stream.output).String() : "?");
	// Let the device play A's tone for the harness.
	snooze(3000000);
	tone(a, 5100, 0);
	snooze(300000);

	host("usb_audio remove");
	bool gone = wait_stream(control, stream, 30000000,
		[](AudioStreamControl& control, AudioStream&) {
			return !control.Outputs().HasString(kUsbDevice);
		});
	report("route_kept_while_absent", gone && stream.route == kUsbDevice
			&& stream.output == control.Outputs().CountStrings(),
		"device %s; route \"%s\", Output index %" B_PRId32 " of %" B_PRId32,
		gone ? "gone" : "still listed", stream.route.String(), stream.output,
		control.Outputs().CountStrings());

	status = control.SetRoute(stream.id, "");
	bool cleared = wait_stream(control, stream, 5000000,
		[](AudioStreamControl&, AudioStream& stream) {
			return stream.route == "" && stream.output == 0;
		});
	report("route_cleared", status == B_OK && cleared,
		"%s; route \"%s\", Output index %" B_PRId32, strerror(status),
		stream.route.String(), stream.output);
}


// #pragma mark - O-2: loopback only; O-3: by team


static void
screenshot(const char* name)
{
	printf("CE-HOST screenshot %s\n", name);
	fflush(stdout);
	snooze(1500000);
}


/*!	Starts a preferences application and quits it after a screenshot. It
	must be gone afterwards: the Audio preferences' level meter and the
	Media preferences keep reading the mixer while they are open, which
	disturbs the timing checks of the tests after this one.
*/
static void
show_and_shoot(const char* command, const char* signature, const char* name)
{
	BString line(command);
	line << " &";
	system(line.String());
	snooze(5000000);
	screenshot(name);
	if (strcmp(signature, "application/x-vnd.Renga-Audio") == 0) {
		// The Audio preferences remember "advanced"; the sound_settings
		// suite after this one starts from the plain view.
		BMessage get(B_GET_PROPERTY);
		get.AddSpecifier("Messenger");
		get.AddSpecifier("Window", (int32)0);
		BMessage reply;
		BMessenger window;
		if (BMessenger(signature).SendMessage(&get, &reply, 2000000,
				2000000) == B_OK
			&& reply.FindMessenger("result", &window) == B_OK) {
			BMessage plain('AuAv');
			plain.AddBool("advanced", false);
			window.SendMessage(&plain);
			snooze(500000);
		}
	}
	BMessenger(signature).SendMessage(B_QUIT_REQUESTED);
	for (int i = 0; i < 30 && be_roster->IsRunning(signature); i++)
		snooze(100000);
	if (be_roster->IsRunning(signature)) {
		team_id team = be_roster->TeamFor(signature);
		if (team >= 0)
			kill_team(team);
		snooze(500000);
	}
}


/*!	"None (loopback only)": A plays on no output (the harness checks that
	its 5250 Hz is on neither card while B's 5350 Hz is,
	media_routing_audio.loopback_only_silent), is left out of the
	loopback of every stream and recorded by A's own loopback.
*/
static void
test_loopback_only(Player& a, Player& b)
{
	AudioStreamControl control;
	AudioStream stream;
	if (!wait_stream(control, stream, 5000000,
			[](AudioStreamControl&, AudioStream&) { return true; })) {
		report("loopback_only_menu", false, "A's stream not listed");
		return;
	}
	int32 none = control.Outputs().IndexOf("None (loopback only)");
	status_t status = control.SetLoopbackOnly(stream.id, true);
	bool set = wait_stream(control, stream, 5000000,
		[](AudioStreamControl& control, AudioStream& stream) {
			return stream.loopbackOnly && stream.output
				== control.Outputs().IndexOf("None (loopback only)");
		});
	report("loopback_only_menu", none > 0 && status == B_OK && set,
		"\"None (loopback only)\" is choice %" B_PRId32 " of %" B_PRId32
		"; SetLoopbackOnly: %s; route \"%s\", Output index %" B_PRId32,
		none, control.Outputs().CountStrings(), strerror(status),
		stream.route.String(), stream.output);

	tone(a, 5250, 0);
	tone(b, 0, 5350);
	snooze(800000);
	ChannelCheck left, right;
	status = record(NULL, NULL, 1.5, 5250, 5350, left, right);
	report("loopback_only_not_in_all", status == B_OK && absent(left)
			&& present(right),
		"%s; every stream: L (A) %.4f, R (B) %.3f", strerror(status),
		left.amplitude, right.amplitude);
	status = record(kSignatureA, NULL, 1.2, 5250, 0, left, right);
	report("loopback_only_in_own", status == B_OK && present(left),
		"%s; A's loopback: L %.3f purity >= %.3f", strerror(status),
		left.amplitude, left.minPurity);

	// The Output menus show it: the Audio preferences (advanced
	// Applications page) and the Media preferences.
	show_and_shoot("/boot/system/preferences/Audio --page applications "
		"--advanced", "application/x-vnd.Renga-Audio",
		"routing_loopback_only");
	show_and_shoot("/boot/system/preferences/Media --advanced-audio",
		"application/x-vnd.Haiku-Media", "routing_loopback_only_media");

	tone(a, 5100, 0);
	tone(b, 0, 5300);
	status = control.SetLoopbackOnly(stream.id, false);
	bool back = wait_stream(control, stream, 5000000,
		[](AudioStreamControl&, AudioStream& stream) {
			return !stream.loopbackOnly && stream.output == 0;
		});
	snooze(500000);
	status_t recorded = record(NULL, NULL, 1.0, 5100, 5300, left, right);
	report("loopback_only_cleared", status == B_OK && back
			&& recorded == B_OK && present(left) && present(right),
		"%s; back on the default output: %s; every stream: L %.3f, R %.3f",
		strerror(status), back ? "yes" : "no", left.amplitude,
		right.amplitude);
}


/*!	Two processes of one application, one on each channel: a loopback of
	either's team records only that one; team 0 records the application
	(both).
*/
static void
test_teams(const char* self)
{
	Player c1 = { NULL, kSignatureC };
	Player c2 = { NULL, kSignatureC };
	bool started = start_player(c1, self, kSignatureC);
	tone(c1, 5400, 0);
	snooze(1500000);
	BList before;
	be_roster->GetAppList(kSignatureC, &before);
	started = started && start_player(c2, self, kSignatureC);
	tone(c2, 0, 5450);
	team_id team1 = before.CountItems() == 1
		? (team_id)(addr_t)before.ItemAt(0) : -1;
	team_id team2 = -1;
	for (int i = 0; i < 50 && team2 < 0; i++) {
		BList teams;
		be_roster->GetAppList(kSignatureC, &teams);
		for (int32 j = 0; j < teams.CountItems(); j++) {
			team_id team = (team_id)(addr_t)teams.ItemAt(j);
			if (team != team1)
				team2 = team;
		}
		if (team2 < 0)
			snooze(200000);
	}
	snooze(2000000);
		// both streams in the mixer

	ChannelCheck left1, right1, left2, right2;
	status_t status1 = B_ERROR, status2 = B_ERROR;
	BString readBack;
	for (int which = 0; which < 2; which++) {
		team_id team = which == 0 ? team1 : team2;
		LoopbackRecorder loopback;
		status_t status = loopback.Create(kSignatureC);
		if (status == B_OK)
			status = AudioStreamControl::SetLoopbackTeam(loopback.node, team);
		if (status == B_OK && which == 0) {
			AudioStreamControl::GetLoopbackText(loopback.node,
				RENGA_LOOPBACK_TEAM_KIND, readBack);
		}
		if (status == B_OK)
			status = loopback.Start();
		if (status == B_OK)
			snooze(1200000);
		loopback.Stop();
		(which == 0 ? left1 : left2) = check_channel(loopback.recording,
			false, 0.3, 5400);
		(which == 0 ? right1 : right2) = check_channel(loopback.recording,
			true, 0.3, 5450);
		(which == 0 ? status1 : status2) = status;
	}
	BString expected;
	expected << team1;
	report("team_selects_process", started && team1 >= 0 && team2 >= 0
			&& status1 == B_OK && status2 == B_OK && present(left1)
			&& absent(right1) && absent(left2) && present(right2)
			&& readBack == expected,
		"teams %" B_PRId32 "/%" B_PRId32 " of one signature; first's "
		"loopback: L %.3f R %.4f; second's: L %.4f R %.3f; parameter read "
		"back \"%s\"", team1, team2, left1.amplitude, right1.amplitude,
		left2.amplitude, right2.amplitude, readBack.String());

	LoopbackRecorder loopback;
	status_t status = loopback.Create(kSignatureC);
	if (status == B_OK)
		status = AudioStreamControl::SetLoopbackTeam(loopback.node, team1);
	if (status == B_OK)
		status = AudioStreamControl::SetLoopbackTeam(loopback.node, 0);
	if (status == B_OK)
		status = loopback.Start();
	if (status == B_OK)
		snooze(1200000);
	loopback.Stop();
	ChannelCheck left = check_channel(loopback.recording, false, 0.3, 5400);
	ChannelCheck right = check_channel(loopback.recording, true, 0.3, 5450);
	report("team_zero_by_signature", status == B_OK && present(left)
			&& present(right),
		"%s; team 0: the application's streams, L %.3f R %.3f",
		strerror(status), left.amplitude, right.amplitude);

	stop_player(c1);
	stop_player(c2);
}


//!	The team of the one running \a signature, or -1 (waits up to 10 s).
static team_id
team_of(const char* signature)
{
	for (int i = 0; i < 50; i++) {
		BList teams;
		be_roster->GetAppList(signature, &teams);
		if (teams.CountItems() == 1)
			return (team_id)(addr_t)teams.ItemAt(0);
		snooze(200000);
	}
	return -1;
}


/*!	Whether \a teams (one id per line, as GetLoopbackOnlyTeams() gives
	them) lists \a team.
*/
static bool
lists_team(const BString& teams, team_id team)
{
	BString number;
	number << team;
	BStringList list;
	teams.Split("\n", true, list);
	return list.HasString(number);
}


/*!	F-58 batch 2 (OBS Studio's O-17 and O-18): each stream names its team;
	a team made loopback-only for now before its stream starts is on no
	output from the first buffer (the harness checks that its 5700 Hz is
	on no card, media_routing_audio.transient_silent), is recorded by its
	team's loopback and by no loopback of every stream, is not
	remembered (the application's next process plays normally) and is
	forgotten when the team quits. Streams already playing switch both
	ways.
*/
static void
test_transient(const char* self, Player& a, Player& b)
{
	tone(a, 0, 0);
	tone(b, 0, 5300);

	Player d = { NULL, kSignatureD };
	bool started = start_player(d, self, kSignatureD, true);
	team_id teamD = started ? team_of(kSignatureD) : -1;
	AudioStreamControl control;
	status_t updated = control.Update();
	status_t status = teamD >= 0 ? control.SetTeamLoopbackOnly(teamD, true)
		: B_ERROR;
	BString teams;
	status_t read = control.GetLoopbackOnlyTeams(teams);
	report("transient_set", updated == B_OK && status == B_OK
			&& read == B_OK && lists_team(teams, teamD),
		"team %" B_PRId32 " (not connected yet): SetTeamLoopbackOnly %s; "
		"read back \"%s\" (%s)", teamD, strerror(status),
		BString(teams).ReplaceAll("\n", ",").String(), strerror(read));

	// Now it connects and plays.
	if (d.pipe != NULL) {
		fprintf(d.pipe, "start\n");
		fflush(d.pipe);
	}
	tone(d, 5700, 0);
	AudioStream streamD;
	bool found = find_stream(control, kSignatureD, streamD);
	AudioStream streamA;
	team_id teamA = team_of(kSignatureA);
	bool foundA = find_stream(control, kSignatureA, streamA);
	const AudioStream* byTeam = found ? control.FindStream(teamD) : NULL;
	report("app_team", found && foundA && streamD.team == teamD
			&& streamA.team == teamA && teamA >= 0 && byTeam != NULL
			&& byTeam->id == streamD.id,
		"streams' teams: D %" B_PRId32 " (want %" B_PRId32 "), A %" B_PRId32
		" (want %" B_PRId32 "); FindStream(team) %s", found ? streamD.team
			: -2, teamD, foundA ? streamA.team : -2, teamA,
		byTeam != NULL ? "found it" : "found nothing");
	report("transient_listed", found && streamD.loopbackOnly
			&& streamD.transientLoopbackOnly
			&& streamD.route != RENGA_LOOPBACK_ONLY_ROUTE,
		"D's stream: loopbackOnly %d, transient %d, remembered route \"%s\" "
		"(want not the loopback-only route), Output index %" B_PRId32 " of %"
		B_PRId32, found && streamD.loopbackOnly,
		found && streamD.transientLoopbackOnly, streamD.route.String(),
		streamD.output, control.Outputs().CountStrings());

	snooze(800000);
	ChannelCheck left, right;
	status = record(NULL, NULL, 1.5, 5700, 5300, left, right);
	report("transient_not_in_all", status == B_OK && absent(left)
			&& present(right),
		"%s; every stream: L (D) %.4f, R (B) %.3f", strerror(status),
		left.amplitude, right.amplitude);

	LoopbackRecorder own;
	status = own.Create(kSignatureD);
	if (status == B_OK)
		status = AudioStreamControl::SetLoopbackTeam(own.node, teamD);
	if (status == B_OK)
		status = own.Start();
	if (status == B_OK)
		snooze(1200000);
	own.Stop();
	left = check_channel(own.recording, false, 0.3, 5700);
	report("transient_in_own_team", status == B_OK && present(left),
		"%s; D's team's loopback: L %.3f purity >= %.3f", strerror(status),
		left.amplitude, left.minPurity);

	// The team quits: forgotten. The next process of the application plays
	// as its settings say (nothing was remembered).
	stop_player(d);
	snooze(500000);
	control.Update();
	read = control.GetLoopbackOnlyTeams(teams);
	bool forgotten = read == B_OK && !lists_team(teams, teamD);
	Player d2 = { NULL, kSignatureD };
	started = start_player(d2, self, kSignatureD);
	tone(d2, 5550, 0);
	AudioStream stream2;
	found = find_stream(control, kSignatureD, stream2);
	snooze(800000);
	status = record(NULL, NULL, 1.2, 5550, 5300, left, right);
	report("transient_not_remembered", forgotten && started && found
			&& !stream2.loopbackOnly && stream2.route == ""
			&& status == B_OK && present(left),
		"after the team quit: listed %s; next process: loopbackOnly %d, "
		"route \"%s\"; every stream: L %.3f", forgotten ? "no" : "yes",
		found && stream2.loopbackOnly, stream2.route.String(),
		left.amplitude);

	// A stream already playing switches, and back.
	team_id team2 = team_of(kSignatureD);
	status = team2 >= 0 ? control.SetTeamLoopbackOnly(team2, true) : B_ERROR;
	snooze(600000);
	ChannelCheck onLeft, onRight;
	status_t recordedOn = record(NULL, NULL, 1.2, 5550, 5300, onLeft,
		onRight);
	status_t cleared = control.SetTeamLoopbackOnly(team2, false);
	snooze(600000);
	status_t recordedOff = record(NULL, NULL, 1.2, 5550, 5300, left, right);
	report("transient_live_switch", status == B_OK && cleared == B_OK
			&& recordedOn == B_OK && recordedOff == B_OK && absent(onLeft)
			&& present(left),
		"on: %s, every stream L %.4f; off: %s, L %.3f", strerror(status),
		onLeft.amplitude, strerror(cleared), left.amplitude);
	stop_player(d2);

	status = control.SetTeamLoopbackOnly(0x7ffffff0, true);
	report("transient_bad_team", status == B_BAD_VALUE,
		"a team that does not exist: %s (want %s)", strerror(status),
		strerror(B_BAD_VALUE));
}


int
main(int argc, char** argv)
{
	if (argc >= 3 && strcmp(argv[1], "player") == 0)
		return run_player(argv[2]);
	if (argc >= 3 && strcmp(argv[1], "player-wait") == 0)
		return run_player(argv[2], true);

	signal(SIGPIPE, SIG_IGN);
	BApplication app("application/x-vnd.Renga-media-routing-test");

	printf("CE-SUITE " SUITE " START\n");
	fflush(stdout);

	bigtime_t started = system_time();
	bool running = wait_for_mixer(90000000);
	if (!running) {
		report("server_running", false, "no system mixer after %.1f s",
			(system_time() - started) / 1000000.0);
		printf("CE-SUITE " SUITE " DONE failures=%d\n", sFailures);
		return 1;
	}

	Player a = { NULL, kSignatureA };
	Player b = { NULL, kSignatureB };
	bool started_ = start_player(a, argv[0], kSignatureA)
		&& start_player(b, argv[0], kSignatureB);
	tone(a, 5100, 0);
	tone(b, 0, 5300);
	AudioStreamControl control;
	AudioStream streamA, streamB;
	bool found = started_ && find_stream(control, kSignatureA, streamA)
		&& find_stream(control, kSignatureB, streamB);
	if (!found) {
		report("players", false, "the players' streams are not listed");
	} else {
		snooze(500000);
		test_exclusions();
		test_routes(a);
		test_loopback_only(a, b);
		test_teams(argv[0]);
		test_transient(argv[0], a, b);
	}

	stop_player(a);
	stop_player(b);

	printf("CE-SUITE " SUITE " DONE failures=%d\n", sFailures);
	fflush(stdout);
	return sFailures == 0 ? 0 : 1;
}
