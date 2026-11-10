/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 */

/*!	Media Kit test of the system mixer's per-application streams, routing
	and loopback, and of recording through the Media Kit (roadmap F-31
	phase 1, F-02 through the Media Kit). It runs in the harness's second
	phase, with media_server started by the runner after the driver tests
	(which need the sound cards to themselves).

	The harness attaches two virtio sound cards: "Virtio Sound" records into
	audio.wav, "Virtio Sound 2" (QEMU's SDL backend) writes its output to
	sdl-out.raw and records the tone the harness feeds (700/1300 Hz,
	amplitude 12000, unless a test asks for another one).

	Applications are played by child processes of this program, each a
	BApplication with its own signature and a BSoundPlayer: A plays on the
	left channel only, B on the right only, so that each channel of a
	recording belongs to one application. Their tones change from phase to
	phase, so that the harness can find every phase in the recordings
	without knowing when it happened (see ce-test.py, MEDIA_PHASES).

	In-guest checks record through the loopback ("What you hear") with
	BMediaRecorder, like SoundRecorder does; the harness checks the same
	phases in what the cards played. Every check prints
	"CE-TEST media.<name> PASS|FAIL <detail>".
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
#include <TimeSource.h>

#include <AudioStreamControl.h>

#include <math.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>

#include <vector>


using BPrivate::AudioStream;
using BPrivate::AudioStreamControl;


#define SUITE "media"

static const char* kSignatureA = "application/x-vnd.Renga-media-test-a";
static const char* kSignatureB = "application/x-vnd.Renga-media-test-b";
static const char* kMediaServer = "/boot/system/servers/media_server";
static const float kAmplitude = 0.25f;
// The recording gain parameter of the multi_audio add-on (Renga).
static const int32 kInputGainParameter = 0x52474e01;

static int sFailures = 0;


static void
report(const char* name, bool pass, const char* format, ...)
{
	char detail[400];
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
info(const char* format, ...)
{
	va_list args;
	va_start(args, format);
	printf("CE-INFO media: ");
	vprintf(format, args);
	printf("\n");
	va_end(args);
	fflush(stdout);
}


// #pragma mark - player (child process)


struct PlayerState {
	volatile float	leftHertz;
	volatile float	rightHertz;
	volatile float	target;		// amplitude to move to
	float			amplitude;	// current, ramps to target
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
	// About 10 ms to any new amplitude: the player itself never clicks.
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


/*!	A child: plays "tone <left Hz> <right Hz>" (0: silent channel) read
	from stdin until "quit" or the end of input, then fades out.
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
	BSoundPlayer player(&format, "media_test player", play_buffer, NULL,
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


// #pragma mark - controlling players


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
	int32				buffers;

	Recording()
		:
		lock("recording"),
		rate(0),
		buffers(0)
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
	recording->buffers++;
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
				case media_raw_audio_format::B_AUDIO_INT:
					values[c] = ((int32*)data)[index] / 2147483648.0f;
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


//!	Records \a seconds from the first free output of \a node.
static status_t
record_from(const media_node& node, double seconds, Recording& recording)
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

	BMediaRecorder recorder("media_test recorder", B_MEDIA_RAW_AUDIO);
	if (recorder.InitCheck() != B_OK)
		return recorder.InitCheck();
	recorder.SetHooks(record_buffer, NULL, &recording);
	media_format format = output.format;
	status = recorder.Connect(node, &output, &format);
	if (status != B_OK)
		return status;
	status = recorder.Start();
	if (status == B_OK) {
		snooze((bigtime_t)(seconds * 1000000));
		recorder.Stop();
	}
	recorder.Disconnect();
	return status;
}


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


/*!	Level (sine amplitude, from the RMS) and purity of \a hertz in one
	channel, after \a skip seconds; the worst purity over 100 ms windows.
*/
struct ChannelCheck {
	double	amplitude;
	double	minPurity;
	int32	windows;
	double	maxJump;	// largest step between two samples
};


static ChannelCheck
check_channel(const std::vector<float>& samples, double rate, double skip,
	double hertz)
{
	ChannelCheck result = { 0, 1.0, 0, 0 };
	size_t start = (size_t)(skip * rate);
	if (samples.size() <= start + 1 || rate <= 0) {
		result.minPurity = 0;
		return result;
	}
	double energy = 0;
	for (size_t i = start; i < samples.size(); i++) {
		energy += (double)samples[i] * samples[i];
		if (i > start) {
			result.maxJump = max_c(result.maxJump,
				fabs(samples[i] - samples[i - 1]));
		}
	}
	result.amplitude = sqrt(energy / (samples.size() - start)) * sqrt(2.0);

	size_t window = (size_t)(rate / 10);
	if (hertz > 0) {
		for (size_t w = start; w + window <= samples.size(); w += window) {
			double windowEnergy = 0;
			for (size_t i = w; i < w + window; i++)
				windowEnergy += (double)samples[i] * samples[i];
			double purity = windowEnergy > 0
				? 2 * goertzel(&samples[w], window, rate, hertz)
					/ (window * windowEnergy) : 0;
			result.minPurity = min_c(result.minPurity, purity);
			result.windows++;
		}
	}
	if (result.windows == 0)
		result.minPurity = hertz > 0 ? 0 : 1;
	return result;
}


// #pragma mark - media services


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


static void
start_media_server()
{
	pid_t child = fork();
	if (child == 0) {
		execl(kMediaServer, kMediaServer, (char*)NULL);
		_exit(127);
	}
}


//!	The stream of \a signature, waiting until the mixer has it.
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


struct Device {
	BString				name;
	dormant_node_info	info;
};


static int32
physical_devices(uint64 kinds, Device* devices, int32 maxCount)
{
	dormant_node_info infos[32];
	int32 count = 32;
	if (BMediaRoster::Roster()->GetDormantNodes(infos, &count, NULL, NULL,
			NULL, kinds, 0) != B_OK) {
		return 0;
	}
	int32 found = 0;
	for (int32 i = 0; i < count && found < maxCount; i++) {
		if (strcmp(infos[i].name, RENGA_LOOPBACK_FLAVOR_NAME) == 0)
			continue;
		devices[found].name = infos[i].name;
		devices[found].info = infos[i];
		found++;
	}
	return found;
}


static status_t
node_for(const dormant_node_info& info, media_node& node)
{
	BMediaRoster* roster = BMediaRoster::Roster();
	media_node_id ids[4];
	int32 count = 4;
	if (roster->GetInstancesFor(info.addon, info.flavor_id, ids, &count)
			== B_OK && count > 0) {
		return roster->GetNodeFor(ids[0], &node);
	}
	return roster->InstantiateDormantNode(info, &node, B_FLAVOR_IS_GLOBAL);
}


static status_t
set_float_parameter(const media_node& node, int32 id, float value)
{
	BParameterWeb* web;
	status_t status = BMediaRoster::Roster()->GetParameterWebFor(node, &web);
	if (status != B_OK)
		return status;
	status = B_ENTRY_NOT_FOUND;
	for (int32 i = 0; i < web->CountParameters(); i++) {
		BParameter* parameter = web->ParameterAt(i);
		if (parameter->ID() == id) {
			status = parameter->SetValue(&value, sizeof(value),
				system_time());
			break;
		}
	}
	delete web;
	return status;
}


// #pragma mark - tests


static Device sOutputs[8];
static int32 sOutputCount;
static BString sSecondDevice;	// the SDL card: sdl-out.raw


/*!	Recording through the Media Kit from the card the harness feeds, and
	its recording gain. Also tells which output device is the SDL card:
	the card whose input carries the fed tone.
*/
static void
test_input()
{
	Device inputs[8];
	int32 count = physical_devices(B_BUFFER_PRODUCER | B_PHYSICAL_INPUT,
		inputs, 8);
	int fed = -1;
	ChannelCheck left = {}, right = {};
	media_node fedNode;
	for (int32 i = 0; i < count; i++) {
		media_node node;
		if (node_for(inputs[i].info, node) != B_OK)
			continue;
		Recording recording;
		status_t status = record_from(node, 1.0, recording);
		ChannelCheck l = check_channel(recording.left, recording.rate, 0.3,
			700);
		ChannelCheck r = check_channel(recording.right, recording.rate, 0.3,
			1300);
		info("input \"%s\": %s, %" B_PRId32 " buffers at %.0f Hz, level "
			"%.3f/%.3f", inputs[i].name.String(), strerror(status),
			recording.buffers, recording.rate, l.amplitude, r.amplitude);
		if (fed < 0 && l.amplitude > 0.1) {
			fed = i;
			left = l;
			right = r;
			fedNode = node;
		} else
			BMediaRoster::Roster()->ReleaseNode(node);
	}

	// 12000 / 32768 = 0.366
	report("input_record", fed >= 0 && left.minPurity > 0.9
			&& right.minPurity > 0.9 && fabs(left.amplitude - 0.366) < 0.05
			&& fabs(right.amplitude - 0.366) < 0.05,
		"%" B_PRId32 " inputs; fed: %s; L 700 Hz level %.3f purity >= %.3f, "
		"R 1300 Hz level %.3f purity >= %.3f (want 0.366)", count,
		fed >= 0 ? inputs[fed].name.String() : "none", left.amplitude,
		left.minPurity, right.amplitude, right.minPurity);
	if (fed < 0)
		return;
	sSecondDevice = inputs[fed].name;

	// -6 dB of recording gain halves the level.
	status_t status = set_float_parameter(fedNode, kInputGainParameter,
		-6.0206f);
	Recording recording;
	if (status == B_OK)
		status = record_from(fedNode, 1.0, recording);
	ChannelCheck gained = check_channel(recording.left, recording.rate, 0.3,
		700);
	double ratio = left.amplitude > 0 ? gained.amplitude / left.amplitude : 0;
	report("input_gain", status == B_OK && fabs(ratio - 0.5) < 0.05
			&& gained.minPurity > 0.9,
		"%s; at -6 dB level %.3f, ratio %.3f (want 0.500), purity >= %.3f",
		strerror(status), gained.amplitude, ratio, gained.minPurity);
	set_float_parameter(fedNode, kInputGainParameter, 0);
	BMediaRoster::Roster()->ReleaseNode(fedNode);
}


//!	Records \a seconds from a new loopback of \a signature (NULL: all).
static status_t
record_loopback(const char* signature, double seconds, Recording& recording)
{
	media_node node;
	status_t status = AudioStreamControl::StartLoopback(signature, &node);
	if (status != B_OK)
		return status;
	status = record_from(node, seconds, recording);
	BMediaRoster::Roster()->ReleaseNode(node);
	return status;
}


/*!	The gain the system mixer applies for a gain parameter value. Its
	"dB" are not plain decibels: like upstream Haiku's mixer it bends the
	scale (exponent 1.8 below 0 dB, 1.4 above, over -60..+18 dB), so that
	the slider feels even. -12 on the slider is about -3.3 dB.
*/
static double
mixer_gain(double db)
{
	double maximum = db > 0 ? 18.0 : 60.0;
	double exponent = db > 0 ? 1.4 : 1.8;
	double scaled = pow(fabs(db) * pow(maximum, 1.0 / exponent) / maximum,
		exponent);
	return pow(10.0, (db > 0 ? scaled : -scaled) / 20.0);
}


static bool
near(double value, double expected, double tolerance)
{
	return fabs(value - expected) <= tolerance * expected;
}


// The largest step a sine of this frequency and amplitude makes between two
// samples; anything clearly above it is a click.
static double
max_step(double hertz, double amplitude, double rate)
{
	return 2 * M_PI * hertz / rate * amplitude * 1.15 + 0.002;
}


struct StreamsGuard {
	Player a;
	Player b;
};


static void
test_streams(const char* self)
{
	AudioStreamControl control;
	StreamsGuard players = { { NULL, kSignatureA }, { NULL, kSignatureB } };
	Player& a = players.a;
	Player& b = players.b;

	bool started = start_player(a, self, kSignatureA)
		&& start_player(b, self, kSignatureB);
	tone(a, 1500, 0);
	tone(b, 0, 2500);
	AudioStream streamA, streamB;
	bool found = started && find_stream(control, kSignatureA, streamA)
		&& find_stream(control, kSignatureB, streamB);
	report("streams_listed", found && streamA.id != streamB.id
			&& streamA.name == "media_test" && streamA.hasOutput,
		"A: id %" B_PRId32 " \"%s\", B: id %" B_PRId32 " \"%s\", %" B_PRId32
		" output choices", found ? streamA.id : -1,
		found ? streamA.name.String() : "", found ? streamB.id : -1,
		found ? streamB.name.String() : "", control.Outputs().CountStrings());
	if (!found) {
		stop_player(a);
		stop_player(b);
		return;
	}

	// The default output's time source paces the mixer: it has to run at
	// the speed of real time, or the mixer makes too few (or too many)
	// buffers and the card plays gaps (or drops some).
	{
		media_node output;
		double rate = 0;
		bigtime_t real1 = 0, real2 = 0, performance1 = 0, performance2 = 0;
		float drift = 0;
		if (BMediaRoster::Roster()->GetAudioOutput(&output) == B_OK) {
			BTimeSource* timeSource
				= BMediaRoster::Roster()->MakeTimeSourceFor(output);
			if (timeSource != NULL) {
				real1 = system_time();
				performance1 = timeSource->PerformanceTimeFor(real1);
				snooze(3000000);
				real2 = system_time();
				performance2 = timeSource->PerformanceTimeFor(real2);
				bigtime_t p, r;
				timeSource->GetTime(&p, &r, &drift);
				rate = double(performance2 - performance1)
					/ double(real2 - real1);
				timeSource->Release();
			}
			BMediaRoster::Roster()->ReleaseNode(output);
		}
		report("output_time_source", fabs(rate - 1.0) < 0.01,
			"while playing, performance time runs at %.4f of real time "
			"(published drift %.4f)", rate, drift);
	}

	// Phase "both": loopback of everything and of one application.
	Recording all;
	status_t status = record_loopback(NULL, 1.2, all);
	ChannelCheck allLeft = check_channel(all.left, all.rate, 0.3, 1500);
	ChannelCheck allRight = check_channel(all.right, all.rate, 0.3, 2500);
	report("loopback_all", status == B_OK
			&& near(allLeft.amplitude, kAmplitude, 0.1)
			&& near(allRight.amplitude, kAmplitude, 0.1)
			&& allLeft.minPurity > 0.95 && allRight.minPurity > 0.95,
		"%s; %.0f Hz; L 1500 Hz %.3f purity >= %.3f, R 2500 Hz %.3f purity "
		">= %.3f (want %.3f)", strerror(status), all.rate, allLeft.amplitude,
		allLeft.minPurity, allRight.amplitude, allRight.minPurity, kAmplitude);

	Recording onlyA;
	status = record_loopback(kSignatureA, 1.2, onlyA);
	ChannelCheck aLeft = check_channel(onlyA.left, onlyA.rate, 0.3, 1500);
	ChannelCheck aRight = check_channel(onlyA.right, onlyA.rate, 0.3, 0);
	report("loopback_application", status == B_OK
			&& near(aLeft.amplitude, kAmplitude, 0.1) && aLeft.minPurity > 0.95
			&& aRight.amplitude < 0.005,
		"%s; A only: L 1500 Hz %.3f purity >= %.3f, R (B's channel) %.4f",
		strerror(status), aLeft.amplitude, aLeft.minPurity, aRight.amplitude);

	// Phase "volume": A at -12 dB, recorded across the change (with a tone
	// of its own, so that the harness finds the phase after the change as
	// one level).
	tone(a, 1550, 0);
	snooze(1000000);
	Recording volume;
	{
		media_node node;
		status = AudioStreamControl::StartLoopback(NULL, &node);
		if (status == B_OK) {
			// The change lands in the middle of the recording.
			thread_id changer = spawn_thread([](void* data) -> status_t {
				snooze(600000);
				AudioStreamControl* control = (AudioStreamControl*)data;
				control->Update();
				const AudioStream* stream = control->FindStream(kSignatureA);
				if (stream != NULL)
					control->SetGain(stream->id, -12.0412f);
				return B_OK;
			}, "volume change", B_NORMAL_PRIORITY, &control);
			resume_thread(changer);
			status = record_from(node, 2.0, volume);
			status_t result;
			wait_for_thread(changer, &result);
			BMediaRoster::Roster()->ReleaseNode(node);
		}
	}
	tone(a, 1600, 0);
	snooze(1000000);
	Recording quieter;
	if (status == B_OK)
		status = record_loopback(NULL, 1.0, quieter);
	ChannelCheck qLeft = check_channel(quieter.left, quieter.rate, 0.3, 1600);
	ChannelCheck qRight = check_channel(quieter.right, quieter.rate, 0.3,
		2500);
	double ratio = qLeft.amplitude / kAmplitude;
	double expected = mixer_gain(-12.0412);
	report("app_volume", status == B_OK && fabs(ratio - expected) < 0.03
			&& near(qRight.amplitude, kAmplitude, 0.1)
			&& qLeft.minPurity > 0.95,
		"%s; A at -12 (mixer scale): L 1600 Hz %.3f (ratio %.3f, want %.3f), "
		"purity >= %.3f; B unchanged: R %.3f", strerror(status),
		qLeft.amplitude, ratio, expected, qLeft.minPurity, qRight.amplitude);
	ChannelCheck vLeft = check_channel(volume.left, volume.rate, 0.2, 1550);
	double bound = max_step(1550, kAmplitude, volume.rate);
	report("click_free_volume", volume.left.size() > 0
			&& vLeft.maxJump <= bound,
		"largest step across the change %.4f, a 1550 Hz sine at %.2f makes "
		"at most %.4f", vLeft.maxJump, kAmplitude, bound);
	control.Update();
	if (control.FindStream(kSignatureA) != NULL) {
		report("app_volume_shown", fabs(control.FindStream(kSignatureA)->gain
				+ 12.04) < 0.2,
			"mixer reports %.2f dB", control.FindStream(kSignatureA)->gain);
	}

	// Phase "mute": A muted (it plays 1700 Hz meanwhile), across the change.
	tone(a, 1700, 0);
	tone(b, 0, 2600);
	snooze(800000);
	Recording muting;
	{
		media_node node;
		status = AudioStreamControl::StartLoopback(NULL, &node);
		if (status == B_OK) {
			thread_id changer = spawn_thread([](void* data) -> status_t {
				snooze(600000);
				AudioStreamControl* control = (AudioStreamControl*)data;
				control->Update();
				const AudioStream* stream = control->FindStream(kSignatureA);
				if (stream != NULL)
					control->SetMuted(stream->id, true);
				return B_OK;
			}, "mute", B_NORMAL_PRIORITY, &control);
			resume_thread(changer);
			status = record_from(node, 2.0, muting);
			status_t result;
			wait_for_thread(changer, &result);
			BMediaRoster::Roster()->ReleaseNode(node);
		}
	}
	snooze(1000000);
	Recording muted;
	if (status == B_OK)
		status = record_loopback(NULL, 1.0, muted);
	ChannelCheck mLeft = check_channel(muted.left, muted.rate, 0.3, 0);
	ChannelCheck mRight = check_channel(muted.right, muted.rate, 0.3, 2600);
	report("app_mute", status == B_OK && mLeft.amplitude < 0.002
			&& near(mRight.amplitude, kAmplitude, 0.1),
		"%s; A muted: L %.4f; B: R 2600 Hz %.3f", strerror(status),
		mLeft.amplitude, mRight.amplitude);
	ChannelCheck muteLeft = check_channel(muting.left, muting.rate, 0.2, 0);
	bound = max_step(1700, kAmplitude * mixer_gain(-12.0412), muting.rate);
	report("click_free_mute", muting.left.size() > 0
			&& muteLeft.maxJump <= bound,
		"largest step across the mute %.4f, at most %.4f", muteLeft.maxJump,
		bound);

	// Phase "route": A back to normal on the default output, B on the
	// second card. What you hear still has both.
	control.Update();
	if (const AudioStream* stream = control.FindStream(kSignatureA)) {
		control.SetMuted(stream->id, false);
		control.SetGain(stream->id, 0);
	}
	tone(a, 1800, 0);
	tone(b, 0, 2700);
	status = B_ENTRY_NOT_FOUND;
	if (const AudioStream* stream = control.FindStream(kSignatureB))
		status = control.SetOutputByName(stream->id, sSecondDevice.String());
	snooze(2500000);
	control.Update();
	const AudioStream* routed = control.FindStream(kSignatureB);
	int32 wanted = control.Outputs().IndexOf(sSecondDevice);
	report("route_set", status == B_OK && routed != NULL && wanted > 0
			&& routed->output == wanted,
		"%s; B's output %" B_PRId32 ", \"%s\" is %" B_PRId32, strerror(status),
		routed != NULL ? routed->output : -1, sSecondDevice.String(), wanted);
	Recording everywhere;
	status = record_loopback(NULL, 1.0, everywhere);
	ChannelCheck eLeft = check_channel(everywhere.left, everywhere.rate, 0.3,
		1800);
	ChannelCheck eRight = check_channel(everywhere.right, everywhere.rate,
		0.3, 2700);
	report("route_loopback_all", status == B_OK
			&& near(eLeft.amplitude, kAmplitude, 0.1)
			&& near(eRight.amplitude, kAmplitude, 0.1),
		"%s; what you hear with B on the second card: L 1800 Hz %.3f, R "
		"2700 Hz %.3f", strerror(status), eLeft.amplitude, eRight.amplitude);

	// Back to the default output (B fades out on the second card).
	tone(b, 0, 2750);
	snooze(1500000);
	if (const AudioStream* stream = control.FindStream(kSignatureB))
		control.SetOutput(stream->id, 0);
	snooze(1500000);

	// Phase "remembered": A at -6 dB and B on the second card, then both
	// applications quit and start again.
	control.Update();
	if (const AudioStream* stream = control.FindStream(kSignatureA))
		control.SetGain(stream->id, -6.0206f);
	if (const AudioStream* stream = control.FindStream(kSignatureB))
		control.SetOutputByName(stream->id, sSecondDevice.String());
	stop_player(a);
	stop_player(b);
	snooze(500000);
	start_player(a, self, kSignatureA);
	start_player(b, self, kSignatureB);
	tone(a, 1900, 0);
	tone(b, 0, 2800);
	found = find_stream(control, kSignatureA, streamA)
		&& find_stream(control, kSignatureB, streamB);
	report("remembered_by_application", found
			&& fabs(streamA.gain + 6.02) < 0.2
			&& streamB.output == control.Outputs().IndexOf(sSecondDevice),
		"after restarting the applications: A %.2f dB (want -6.02), B's "
		"output %" B_PRId32 " (want %" B_PRId32 ")", found ? streamA.gain : 0,
		found ? streamB.output : -1,
		(int32)control.Outputs().IndexOf(sSecondDevice));
	snooze(2000000);

	// Phase "restart": the same after restarting the media services. The
	// mixer saves its settings 5 s after the last change.
	snooze(4500000);
	stop_player(a);
	stop_player(b);
	status = shutdown_media_server(10000000);
	info("media services stopped: %s", strerror(status));
	snooze(1000000);
	start_media_server();
	bool back = wait_for_mixer(60000000);
	if (back) {
		start_player(a, self, kSignatureA);
		start_player(b, self, kSignatureB);
		tone(a, 1900, 0);
		tone(b, 0, 2850);
		found = find_stream(control, kSignatureA, streamA)
			&& find_stream(control, kSignatureB, streamB);
	} else
		found = false;
	report("remembered_after_restart", back && found
			&& fabs(streamA.gain + 6.02) < 0.2
			&& streamB.output == control.Outputs().IndexOf(sSecondDevice),
		"media services back: %s; A %.2f dB (want -6.02), B's output %"
		B_PRId32, back ? "yes" : "no", found ? streamA.gain : 0,
		found ? streamB.output : -1);
	snooze(3000000);

	// Phase "default output": the second card becomes the default output;
	// the mixer takes it over from B's sink, so both play there.
	if (back && found) {
		control.SetGain(streamA.id, 0);
		tone(a, 2000, 0);
		tone(b, 0, 2900);
		status = B_ENTRY_NOT_FOUND;
		for (int32 i = 0; i < sOutputCount; i++) {
			if (sOutputs[i].name == sSecondDevice) {
				status = BMediaRoster::Roster()->SetAudioOutput(
					sOutputs[i].info);
			}
		}
		snooze(1000000);
		bool switched = wait_for_mixer(10000000);
		media_node output;
		dormant_node_info outputInfo;
		BString name;
		if (BMediaRoster::Roster()->GetAudioOutput(&output) == B_OK) {
			if (BMediaRoster::Roster()->GetDormantNodeFor(output, &outputInfo)
					== B_OK) {
				name = outputInfo.name;
			}
			BMediaRoster::Roster()->ReleaseNode(output);
		}
		snooze(2500000);
		Recording after;
		status_t recorded = record_loopback(NULL, 1.0, after);
		ChannelCheck afterLeft = check_channel(after.left, after.rate, 0.3,
			2000);
		report("default_output_switch", status == B_OK && switched
				&& name == sSecondDevice
				&& near(afterLeft.amplitude, kAmplitude, 0.1),
			"%s; default output now \"%s\"; mixer connected: %s; A after the "
			"switch: %.3f (%s)", strerror(status), name.String(),
			switched ? "yes" : "no", afterLeft.amplitude, strerror(recorded));

		// And back, so that the harness's other runs see the usual setup.
		for (int32 i = 0; i < sOutputCount; i++) {
			if (sOutputs[i].name != sSecondDevice)
				BMediaRoster::Roster()->SetAudioOutput(sOutputs[i].info);
		}
		wait_for_mixer(10000000);
	}

	stop_player(a);
	stop_player(b);
}


int
main(int argc, char** argv)
{
	if (argc >= 3 && strcmp(argv[1], "player") == 0)
		return run_player(argv[2]);

	signal(SIGPIPE, SIG_IGN);
	BApplication app("application/x-vnd.Renga-media-test");

	printf("CE-SUITE " SUITE " START\n");
	fflush(stdout);

	bigtime_t started = system_time();
	bool running = wait_for_mixer(90000000);
	report("server_running", running, "system mixer connected after %.1f s",
		(system_time() - started) / 1000000.0);
	if (!running) {
		printf("CE-SUITE " SUITE " DONE failures=%d\n", sFailures);
		return 1;
	}

	sOutputCount = physical_devices(B_BUFFER_CONSUMER | B_PHYSICAL_OUTPUT,
		sOutputs, 8);
	BString names;
	bool unique = true;
	for (int32 i = 0; i < sOutputCount; i++) {
		names << (i > 0 ? ", " : "") << "\"" << sOutputs[i].name << "\"";
		for (int32 j = 0; j < i; j++)
			unique = unique && sOutputs[i].name != sOutputs[j].name;
	}
	report("output_devices", sOutputCount == 2 && unique, "%" B_PRId32
		": %s", sOutputCount, names.String());

	dormant_node_info loopback;
	status_t status = AudioStreamControl::FindLoopbackFlavor(&loopback);
	report("loopback_listed", status == B_OK, "\"%s\" among the physical "
		"inputs: %s", RENGA_LOOPBACK_FLAVOR_NAME, strerror(status));

	test_input();
	if (sSecondDevice.Length() > 0)
		test_streams(argv[0]);

	status = shutdown_media_server(10000000);
	info("media services stopped: %s", strerror(status));

	printf("CE-SUITE " SUITE " DONE failures=%d\n", sFailures);
	fflush(stdout);
	return sFailures == 0 ? 0 : 1;
}
