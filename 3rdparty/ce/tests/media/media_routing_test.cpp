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
#include <OS.h>
#include <ParameterWeb.h>
#include <SoundPlayer.h>
#include <String.h>

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
	or the end of input. "team" prints its team id on stdout.
*/
static int
run_player(const char* signature)
{
	BApplication app(signature);

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

	char line[128];
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
start_player(Player& player, const char* self, const char* signature)
{
	BString command;
	command << "'" << self << "' player " << signature;
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


int
main(int argc, char** argv)
{
	if (argc >= 3 && strcmp(argv[1], "player") == 0)
		return run_player(argv[2]);

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
	}

	stop_player(a);
	stop_player(b);

	printf("CE-SUITE " SUITE " DONE failures=%d\n", sFailures);
	fflush(stdout);
	return sFailures == 0 ? 0 : 1;
}
