/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 */

/*!	The low-latency audio mode (roadmap F-31, headers/private/media/
	LowLatencyAudio.h): BSoundPlayer with small, stable buffers for games
	and emulators, the system mixer's per-application "Low latency" switch,
	and the default output running with short periods while such a stream
	plays. Runs last of the Media Kit tests (renga-* images).

	A child process ("game", its own signature) plays through BSoundPlayer
	with the buffer size left open, as SDL and most ports do. It writes the
	length and time of every callback and, five times, a 2 kHz burst at the
	start of a callback, noting system_time() as the callback starts. The
	test records the whole mix through the loopback ("What you hear"),
	whose buffers carry the performance time the mix is played at, finds
	each burst's first sample and converts its performance time to real
	time with the mixer's time source: the difference is the latency from
	the callback to the sound card's playout as the Media Kit schedules it
	(the sound card's own clock, not an acoustic measurement).

	Phases: normal mode; the switch turned on as the audio settings do it;
	low-latency mode (callback length, period of the default output and of
	the mixer, latency, a 6 s tone at 3100/3300 Hz without gaps, which the
	harness also looks for in what the card played); the switch off (the
	card returns to its default period); the RENGA_AUDIO_LOW_LATENCY
	environment variable; the device's "Buffer size" parameter; and the
	Media preferences with the switch and the device page (screenshots
	media_low_latency and media_buffer_size).
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
#include <Roster.h>
#include <SoundPlayer.h>
#include <String.h>
#include <TimeSource.h>

#include <AudioStreamControl.h>
#include <LowLatencyAudio.h>

#include <algorithm>
#include <math.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>


#define SUITE "media_latency"

using BPrivate::AudioStream;
using BPrivate::AudioStreamControl;

static const char* kGame = "application/x-vnd.Renga-latency-game";
static const char* kEnvGame = "application/x-vnd.Renga-latency-env";
static const int kBursts = 5;
static const float kBurstHertz = 2000;
static const float kToneLeft = 3100;
static const float kToneRight = 3300;
static const bigtime_t kSettle = 1500000;
static const bigtime_t kBurstSpacing = 400000;

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
info(const char* format, ...)
{
	char text[300];
	va_list args;
	va_start(args, format);
	vsnprintf(text, sizeof(text), format, args);
	va_end(args);
	printf("CE-INFO " SUITE ": %s\n", text);
	fflush(stdout);
}


// #pragma mark - the game (child process)


static const int32 kMaxCallbacks = 20000;

struct GameState {
	float		rate;
	int32		callbacks;
	bigtime_t	times[kMaxCallbacks];
	int32		frames[kMaxCallbacks];
	int32		burstRequest;
	int32		bursts;
	bigtime_t	burstTimes[16];
	int32		burstLeft;
	double		phase;
	int32		tone;
	double		leftPhase;
	double		rightPhase;
};


static void
game_buffer(void* cookie, void* buffer, size_t size,
	const media_raw_audio_format& format)
{
	bigtime_t now = system_time();
	GameState* state = (GameState*)cookie;
	float* data = (float*)buffer;
	int32 channels = format.channel_count;
	size_t frames = size / (sizeof(float) * channels);
	if (state->callbacks < kMaxCallbacks) {
		state->times[state->callbacks] = now;
		state->frames[state->callbacks] = frames;
	}
	state->callbacks++;

	if (atomic_get_and_set(&state->burstRequest, 0) != 0
		&& state->bursts < 16) {
		state->burstTimes[state->bursts++] = now;
		state->burstLeft = (int32)(state->rate / 20);
		state->phase = 0;
	}
	bool tone = atomic_get(&state->tone) != 0;
	for (size_t i = 0; i < frames; i++) {
		float left = 0, right = 0;
		if (state->burstLeft > 0) {
			left = right = 0.5f * sin(state->phase);
			state->phase += 2 * M_PI * kBurstHertz / state->rate;
			state->burstLeft--;
		} else if (tone) {
			left = 0.1f * sin(state->leftPhase);
			right = 0.1f * sin(state->rightPhase);
			state->leftPhase += 2 * M_PI * kToneLeft / state->rate;
			state->rightPhase += 2 * M_PI * kToneRight / state->rate;
		}
		data[i * channels] = left;
		if (channels > 1)
			data[i * channels + 1] = right;
	}
	state->leftPhase = fmod(state->leftPhase, 2 * M_PI);
	state->rightPhase = fmod(state->rightPhase, 2 * M_PI);
}


/*!	"game <signature> <stats file> <tone seconds>": plays silence, five
	bursts, a tone, then silence until "quit" on stdin; writes its
	statistics to the file.
*/
static int
run_game(const char* signature, const char* path, double toneSeconds)
{
	BApplication app(signature);

	// Frame rate and format chosen, the buffer size left open (SDL and
	// most ports choose the buffer size themselves; see the buffer_size
	// check below for that case).
	media_raw_audio_format format = media_raw_audio_format::wildcard;
	format.format = media_raw_audio_format::B_AUDIO_FLOAT;
	format.channel_count = 2;
	format.frame_rate = 48000;
	format.byte_order = B_MEDIA_HOST_ENDIAN;

	GameState* state = new GameState;
	memset(state, 0, sizeof(*state));
	state->rate = 48000;
	BSoundPlayer player(&format, "latency game", game_buffer, NULL, state);
	if (player.InitCheck() != B_OK) {
		fprintf(stderr, "game: %s\n", strerror(player.InitCheck()));
		return 1;
	}
	state->rate = player.Format().frame_rate;
	player.Start();
	player.SetHasData(true);

	snooze(kSettle);
	int32 startCallback = state->callbacks;
	bigtime_t performanceStart = player.PerformanceTime();
	for (int i = 0; i < kBursts; i++) {
		atomic_set(&state->burstRequest, 1);
		snooze(kBurstSpacing);
	}
	atomic_set(&state->tone, 1);
	snooze((bigtime_t)(toneSeconds * 1000000));
	atomic_set(&state->tone, 0);
	int32 endCallback = state->callbacks;
	bigtime_t performanceEnd = player.PerformanceTime();
	bigtime_t latency = player.Latency();

	FILE* file = fopen(path, "w");
	if (file != NULL) {
		int32 last = min_c(endCallback, kMaxCallbacks);
		int32 minFrames = INT32_MAX, maxFrames = 0;
		bigtime_t maxInterval = 0;
		int32 late = 0;
		double period = 0;
		for (int32 i = startCallback; i < last; i++) {
			minFrames = min_c(minFrames, state->frames[i]);
			maxFrames = max_c(maxFrames, state->frames[i]);
		}
		period = maxFrames * 1000000.0 / state->rate;
		for (int32 i = startCallback + 1; i < last; i++) {
			bigtime_t interval = state->times[i] - state->times[i - 1];
			maxInterval = max_c(maxInterval, interval);
			if (interval > 3 * period)
				late++;
		}
		bigtime_t elapsed = last > startCallback + 1
			? state->times[last - 1] - state->times[startCallback] : 0;
		fprintf(file, "rate %g\nbuffer_bytes %" B_PRIuSIZE "\n",
			state->rate, player.Format().buffer_size);
		fprintf(file, "callbacks %" B_PRId32 "\nmin_frames %" B_PRId32
			"\nmax_frames %" B_PRId32 "\nmax_interval %" B_PRIdBIGTIME
			"\nlate %" B_PRId32 "\nelapsed %" B_PRIdBIGTIME
			"\nlatency %" B_PRIdBIGTIME "\n", last - startCallback,
			minFrames, maxFrames, maxInterval, late, elapsed, latency);
		fprintf(file, "performance %" B_PRIdBIGTIME "\n",
			performanceEnd - performanceStart);
		for (int32 i = 0; i < state->bursts; i++)
			fprintf(file, "burst %" B_PRIdBIGTIME "\n", state->burstTimes[i]);
		fclose(file);
	}

	char line[64];
	while (fgets(line, sizeof(line), stdin) != NULL) {
		if (strncmp(line, "quit", 4) == 0)
			break;
	}
	player.Stop();
	return 0;
}


// #pragma mark - statistics of a game run


struct GameStats {
	double		rate;
	size_t		bufferBytes;
	int32		callbacks;
	int32		minFrames;
	int32		maxFrames;
	bigtime_t	maxInterval;
	int32		late;
	bigtime_t	elapsed;
	bigtime_t	latency;
	bigtime_t	performance;	// the card's clock over the same span
	std::vector<bigtime_t> bursts;
	bool		valid;

	GameStats()
		:
		rate(0), bufferBytes(0), callbacks(0), minFrames(0), maxFrames(0),
		maxInterval(0), late(0), elapsed(0), latency(0), performance(0),
		valid(false)
	{
	}
};


static GameStats
read_stats(const char* path)
{
	GameStats stats;
	FILE* file = fopen(path, "r");
	if (file == NULL)
		return stats;
	char key[32];
	long long value;
	double real;
	while (fscanf(file, "%31s", key) == 1) {
		if (strcmp(key, "rate") == 0 && fscanf(file, "%lf", &real) == 1) {
			stats.rate = real;
			continue;
		}
		if (fscanf(file, "%lld", &value) != 1)
			break;
		if (strcmp(key, "buffer_bytes") == 0)
			stats.bufferBytes = value;
		else if (strcmp(key, "callbacks") == 0)
			stats.callbacks = value;
		else if (strcmp(key, "min_frames") == 0)
			stats.minFrames = value;
		else if (strcmp(key, "max_frames") == 0)
			stats.maxFrames = value;
		else if (strcmp(key, "max_interval") == 0)
			stats.maxInterval = value;
		else if (strcmp(key, "late") == 0)
			stats.late = value;
		else if (strcmp(key, "elapsed") == 0)
			stats.elapsed = value;
		else if (strcmp(key, "latency") == 0)
			stats.latency = value;
		else if (strcmp(key, "performance") == 0)
			stats.performance = value;
		else if (strcmp(key, "burst") == 0)
			stats.bursts.push_back(value);
	}
	fclose(file);
	stats.valid = stats.callbacks > 0;
	return stats;
}


// #pragma mark - recording the mix


struct Recording {
	BLocker				lock;
	std::vector<float>	left;
	std::vector<bigtime_t> times;	// real time each sample is played at
	float				rate;
	BTimeSource*		timeSource;		// the mixer's

	Recording()
		:
		lock("recording"),
		rate(0),
		timeSource(NULL)
	{
	}
};


static void
record_buffer(void* cookie, bigtime_t timestamp, void* data, size_t size,
	const media_format& format)
{
	Recording* recording = (Recording*)cookie;
	const media_raw_audio_format& raw = format.u.raw_audio;
	if (raw.format != media_raw_audio_format::B_AUDIO_FLOAT
		|| raw.channel_count == 0) {
		return;
	}
	size_t frames = size / (sizeof(float) * raw.channel_count);
	BAutolock locker(recording->lock);
	if (recording->timeSource == NULL)
		return;
	// The buffer's performance time, in real time now: the mapping changes
	// when the card's buffers are set up again, so it is not converted
	// later.
	bigtime_t real = recording->timeSource->RealTimeFor(timestamp, 0);
	recording->rate = raw.frame_rate;
	for (size_t i = 0; i < frames; i++) {
		recording->left.push_back(((float*)data)[i * raw.channel_count]);
		recording->times.push_back(real
			+ (bigtime_t)(i * 1000000.0 / raw.frame_rate));
	}
}


class Recorder {
public:
	Recorder()
		:
		fRecorder("media_latency recorder", B_MEDIA_RAW_AUDIO),
		fStarted(false)
	{
	}

	status_t Start()
	{
		BMediaRoster* roster = BMediaRoster::Roster();
		media_node mixer;
		if (roster->GetAudioMixer(&mixer) == B_OK) {
			fRecording.timeSource = roster->MakeTimeSourceFor(mixer);
			roster->ReleaseNode(mixer);
		}
		status_t status = AudioStreamControl::StartLoopback(NULL, &fNode);
		if (status != B_OK)
			return status;
		media_output output;
		int32 count;
		status = roster->GetFreeOutputsFor(fNode, &output, 1, &count,
			B_MEDIA_RAW_AUDIO);
		if (status == B_OK && count < 1)
			status = B_BUSY;
		if (status == B_OK)
			status = fRecorder.InitCheck();
		if (status == B_OK) {
			fRecorder.SetHooks(record_buffer, NULL, &fRecording);
			media_format format = output.format;
			format.u.raw_audio.format = media_raw_audio_format::B_AUDIO_FLOAT;
			status = fRecorder.Connect(fNode, &output, &format);
		}
		if (status == B_OK)
			status = fRecorder.Start();
		fStarted = status == B_OK;
		return status;
	}

	void Stop()
	{
		if (fStarted) {
			fRecorder.Stop();
			fRecorder.Disconnect();
			BMediaRoster::Roster()->ReleaseNode(fNode);
			fStarted = false;
		}
		BAutolock locker(fRecording.lock);
		if (fRecording.timeSource != NULL) {
			fRecording.timeSource->Release();
			fRecording.timeSource = NULL;
		}
	}

	~Recorder()
	{
		Stop();
	}

	Recording& Data() { return fRecording; }

private:
	BMediaRecorder		fRecorder;
	media_node			fNode;
	Recording			fRecording;
	bool				fStarted;
};


/*!	The level of the bursts in the recording: the 99.5th percentile of the
	magnitudes (the bursts are the loudest part and about 3 % of the
	samples). The mixer's gain in the loopback does not matter then.
*/
static float
burst_level(Recording& recording)
{
	std::vector<float> magnitudes;
	for (size_t i = 0; i < recording.left.size(); i += 4)
		magnitudes.push_back(fabsf(recording.left[i]));
	if (magnitudes.empty())
		return 1;
	size_t index = magnitudes.size() * 995 / 1000;
	std::nth_element(magnitudes.begin(), magnitudes.begin() + index,
		magnitudes.end());
	return max_c(magnitudes[index], 1e-3f);
}


/*!	The real times the bursts' first samples are played at: the first
	sound after at least 20 ms of silence that reaches half the bursts'
	level (the tone is a fifth of it) within a millisecond.
*/
static std::vector<bigtime_t>
burst_onsets(Recording& recording)
{
	std::vector<bigtime_t> onsets;
	BAutolock locker(recording.lock);
	size_t quiet = 0;
	size_t needed = (size_t)(recording.rate / 50);
	float level = burst_level(recording);
	size_t count = recording.left.size();
	for (size_t i = 0; i < count; i++) {
		float x = fabsf(recording.left[i]);
		if (x >= 0.02f * level) {
			// The first sound after a silence: a burst if it gets loud
			// within a millisecond (a gap in the tone does not).
			if (quiet >= needed) {
				size_t end = min_c(count, i + (size_t)(recording.rate / 1000));
				for (size_t j = i; j < end; j++) {
					if (fabsf(recording.left[j]) > 0.5f * level) {
						onsets.push_back(recording.times[i]);
						break;
					}
				}
			}
			quiet = 0;
		} else
			quiet++;
	}
	return onsets;
}


/*!	The longest run of near-zero samples inside the tone (from its first
	to its last loud sample), in samples.
*/
static size_t
tone_gap(Recording& recording, size_t* _toneSamples, int32* _gaps)
{
	BAutolock locker(recording.lock);
	// The tone follows the last burst: skip past the bursts by finding the
	// last run of 30 ms of near-silence before the tone's long loud part.
	size_t count = recording.left.size();
	size_t last = 0;
	float loud = 0.1f * burst_level(recording);
	for (size_t i = count; i > 0; i--) {
		if (fabsf(recording.left[i - 1]) > loud) {
			last = i - 1;
			break;
		}
	}
	size_t first = last;
	size_t quiet = 0;
	size_t window = (size_t)(recording.rate * 0.03);
	for (size_t i = last; i > 0; i--) {
		if (fabsf(recording.left[i - 1]) < 1e-4f) {
			if (++quiet >= window) {
				first = i - 1 + quiet;
				break;
			}
		} else {
			quiet = 0;
			first = i - 1;
		}
	}
	size_t longest = 0, run = 0;
	int32 gaps = 0;
	for (size_t i = first; i <= last && i < count; i++) {
		if (fabsf(recording.left[i]) < 1e-4f) {
			run++;
			if (run == 17)
				gaps++;
			longest = max_c(longest, run);
		} else
			run = 0;
	}
	*_gaps = gaps;
	*_toneSamples = last > first ? last - first : 0;
	return longest;
}


// #pragma mark - helpers


static bool
wait_for_mixer(bigtime_t timeout)
{
	bigtime_t deadline = system_time() + timeout;
	while (system_time() < deadline) {
		BMediaRoster* roster = BMediaRoster::Roster();
		media_node mixer;
		if (roster != NULL && roster->GetAudioMixer(&mixer) == B_OK) {
			media_output output;
			int32 count = 0;
			bool connected = roster->GetConnectedOutputsFor(mixer, &output,
				1, &count) == B_OK && count == 1;
			roster->ReleaseNode(mixer);
			if (connected)
				return true;
		}
		snooze(250000);
	}
	return false;
}


//!	The mixer output's buffer length in frames (0 if not connected).
static int32
mixer_frames()
{
	BMediaRoster* roster = BMediaRoster::Roster();
	media_node mixer;
	if (roster->GetAudioMixer(&mixer) != B_OK)
		return 0;
	media_output output;
	int32 count = 0;
	int32 frames = 0;
	if (roster->GetConnectedOutputsFor(mixer, &output, 1, &count) == B_OK
		&& count == 1) {
		const media_raw_audio_format& raw = output.format.u.raw_audio;
		size_t frameSize = raw.channel_count
			* (raw.format & media_raw_audio_format::B_AUDIO_SIZE_MASK);
		if (frameSize > 0)
			frames = raw.buffer_size / frameSize;
	}
	roster->ReleaseNode(mixer);
	return frames;
}


//!	Reads or sets an int32 parameter of the default output by its kind.
static status_t
output_parameter(const char* kind, int32* value, bool set)
{
	BMediaRoster* roster = BMediaRoster::Roster();
	media_node node;
	status_t status = roster->GetAudioOutput(&node);
	if (status != B_OK)
		return status;
	BParameterWeb* web = NULL;
	status = roster->GetParameterWebFor(node, &web);
	if (status == B_OK) {
		status = B_ENTRY_NOT_FOUND;
		for (int32 i = 0; i < web->CountParameters(); i++) {
			BParameter* parameter = web->ParameterAt(i);
			if (strcmp(parameter->Kind(), kind) != 0)
				continue;
			if (set)
				status = parameter->SetValue(value, sizeof(int32), 0);
			else {
				size_t size = sizeof(int32);
				bigtime_t when;
				status = parameter->GetValue(value, &size, &when);
			}
			break;
		}
		delete web;
	}
	roster->ReleaseNode(node);
	return status;
}


static BString
output_name()
{
	BMediaRoster* roster = BMediaRoster::Roster();
	media_node node;
	BString name;
	if (roster->GetAudioOutput(&node) == B_OK) {
		dormant_node_info info;
		if (roster->GetDormantNodeFor(node, &info) == B_OK)
			name = info.name;
		roster->ReleaseNode(node);
	}
	return name;
}


//!	Waits until mixer_frames() is \a frames; returns the last value.
static int32
wait_mixer_frames(int32 frames, bigtime_t timeout)
{
	bigtime_t deadline = system_time() + timeout;
	int32 value = mixer_frames();
	while (value != frames && system_time() < deadline) {
		snooze(100000);
		value = mixer_frames();
	}
	return value;
}


static int32
wait_period_request(int32 frames, bigtime_t timeout, bigtime_t* _took)
{
	bigtime_t start = system_time();
	bigtime_t deadline = start + timeout;
	int32 value = -1;
	while (true) {
		if (output_parameter(RENGA_PERIOD_REQUEST_KIND, &value, false)
				!= B_OK) {
			value = -1;
		}
		if (value == frames || system_time() >= deadline)
			break;
		snooze(100000);
	}
	if (_took != NULL)
		*_took = system_time() - start;
	return value;
}


static bool
find_stream(AudioStreamControl& control, const char* signature,
	AudioStream& stream, bigtime_t timeout = 10000000)
{
	bigtime_t deadline = system_time() + timeout;
	while (system_time() < deadline) {
		if (control.Update() == B_OK) {
			const AudioStream* found = control.FindStream(signature);
			if (found != NULL) {
				stream = *found;
				return true;
			}
		}
		snooze(200000);
	}
	return false;
}


struct Game {
	FILE*		pipe;
	BString		stats;
};


static bool
start_game(Game& game, const char* self, const char* signature,
	double toneSeconds, bool environment)
{
	game.stats = "/tmp/media_latency_";
	game.stats << (system_time() % 100000) << ".txt";
	unlink(game.stats.String());
	BString command;
	if (environment)
		command << RENGA_LOW_LATENCY_ENVIRONMENT << "=1 ";
	command << "'" << self << "' game " << signature << " '" << game.stats
		<< "' " << toneSeconds;
	game.pipe = popen(command.String(), "w");
	return game.pipe != NULL;
}


//!	How long a game runs before it waits for "quit".
static bigtime_t
game_duration(double toneSeconds)
{
	return kSettle + kBursts * kBurstSpacing
		+ (bigtime_t)(toneSeconds * 1000000) + 500000;
}


static GameStats
stop_game(Game& game)
{
	if (game.pipe != NULL) {
		fprintf(game.pipe, "quit\n");
		fflush(game.pipe);
		pclose(game.pipe);
		game.pipe = NULL;
	}
	GameStats stats = read_stats(game.stats.String());
	unlink(game.stats.String());
	return stats;
}


/*!	The latency of each burst: the real time its first sample is played at
	(the recording's performance time, through the mixer's time source
	when the buffer arrived) minus the time its callback started. Returns
	the median, or -1.
*/
static bigtime_t
measure_latency(Recording& recording, const GameStats& stats,
	bigtime_t* _min, bigtime_t* _max, int32* _found)
{
	std::vector<bigtime_t> onsets = burst_onsets(recording);
	*_found = onsets.size();
	if (onsets.size() != stats.bursts.size() || onsets.empty())
		return -1;

	std::vector<bigtime_t> latencies;
	for (size_t i = 0; i < onsets.size(); i++)
		latencies.push_back(onsets[i] - stats.bursts[i]);
	std::sort(latencies.begin(), latencies.end());
	*_min = latencies.front();
	*_max = latencies.back();
	return latencies[latencies.size() / 2];
}


/*!	Watches the mixer's time source (the default output's clock): every
	20 ms, how far performance time moved against real time. A clock that
	jumps when the card's buffers are set up again shows as a large step.
*/
struct ClockWatch {
	BTimeSource*	timeSource;
	int32			quit;
	bigtime_t		maxStep;
	thread_id		thread;

	static status_t Run(void* data)
	{
		ClockWatch* watch = (ClockWatch*)data;
		bigtime_t lastReal = system_time();
		bigtime_t lastPerformance
			= watch->timeSource->PerformanceTimeFor(lastReal);
		while (atomic_get(&watch->quit) == 0) {
			snooze(20000);
			bigtime_t real = system_time();
			bigtime_t performance = watch->timeSource->PerformanceTimeFor(real);
			bigtime_t step = (performance - lastPerformance)
				- (real - lastReal);
			if (step < 0)
				step = -step;
			watch->maxStep = max_c(watch->maxStep, step);
			lastReal = real;
			lastPerformance = performance;
		}
		return B_OK;
	}

	void Start()
	{
		quit = 0;
		maxStep = 0;
		thread = -1;
		BMediaRoster* roster = BMediaRoster::Roster();
		media_node mixer;
		timeSource = NULL;
		if (roster->GetAudioMixer(&mixer) == B_OK) {
			timeSource = roster->MakeTimeSourceFor(mixer);
			roster->ReleaseNode(mixer);
		}
		if (timeSource == NULL)
			return;
		thread = spawn_thread(Run, "clock watch", B_NORMAL_PRIORITY, this);
		resume_thread(thread);
	}

	bigtime_t Stop()
	{
		if (thread >= 0) {
			atomic_set(&quit, 1);
			status_t result;
			wait_for_thread(thread, &result);
		}
		if (timeSource != NULL)
			timeSource->Release();
		return thread >= 0 ? maxStep : -1;
	}
};


struct PhaseResult {
	GameStats	stats;
	bigtime_t	latency;
	bigtime_t	minLatency;
	bigtime_t	maxLatency;
	int32		onsets;
	size_t		gap;
	int32		gaps;
	size_t		toneSamples;
	bigtime_t	clockStep;
};


/*!	Runs a game while recording the mix; \a during runs once the game is
	in its tone (with the game's signature as argument).
*/
static PhaseResult
run_phase(const char* self, const char* signature, double toneSeconds,
	bool environment, void (*during)(const char*))
{
	PhaseResult result;
	result.latency = -1;
	result.minLatency = result.maxLatency = -1;
	result.onsets = 0;
	result.gap = 0;
	result.gaps = 0;
	result.toneSamples = 0;

	Recorder recorder;
	status_t status = recorder.Start();
	if (status != B_OK)
		info("loopback recording: %s", strerror(status));

	Game game;
	if (!start_game(game, self, signature, toneSeconds, environment)) {
		recorder.Stop();
		result.stats.valid = false;
		result.clockStep = -1;
		return result;
	}
	// The clock is watched while the game plays (the card's clock starts
	// again from zero whenever the card is started, upstream behaviour),
	// across the switch \a during makes 1.4 s before the game's end.
	snooze(kSettle - 300000);
	ClockWatch watch;
	watch.Start();
	snooze(game_duration(toneSeconds) - kSettle + 300000 - 1400000);
	if (during != NULL)
		during(signature);
	snooze(1400000);
	result.clockStep = watch.Stop();
	result.stats = stop_game(game);
	snooze(300000);
	recorder.Stop();

	if (result.stats.valid) {
		result.latency = measure_latency(recorder.Data(), result.stats,
			&result.minLatency, &result.maxLatency, &result.onsets);
		result.gap = tone_gap(recorder.Data(), &result.toneSamples,
			&result.gaps);
	}
	info("%s%s: %" B_PRId32 " callbacks of %" B_PRId32 "-%" B_PRId32
		" frames, longest interval %" B_PRIdBIGTIME " us, %" B_PRId32
		" late, latency %" B_PRIdBIGTIME " us (min %" B_PRIdBIGTIME ", max %"
		B_PRIdBIGTIME ", %" B_PRId32 " bursts found), Latency() %"
		B_PRIdBIGTIME " us, %" B_PRId32 " tone gaps, longest %" B_PRIuSIZE
		" samples of %" B_PRIuSIZE,
		signature, environment ? " (environment)" : "",
		result.stats.callbacks, result.stats.minFrames, result.stats.maxFrames,
		result.stats.maxInterval, result.stats.late, result.latency,
		result.minLatency, result.maxLatency, result.onsets,
		result.stats.latency, result.gaps, result.gap, result.toneSamples);
	info("largest step of the mixer's clock against real time: %"
		B_PRIdBIGTIME " us", result.clockStep);
	return result;
}


static void
switch_on(const char* signature)
{
	AudioStreamControl control;
	AudioStream stream;
	if (find_stream(control, signature, stream, 3000000))
		control.SetLowLatency(stream.id, true);
}


static int32 sSwitchRequest = -1;
static int32 sSwitchMixerFrames = -1;
static bigtime_t sSwitchTook = 0;

//!	Switches on, then sees the default output and the mixer follow.
static void
switch_on_and_watch(const char* signature)
{
	switch_on(signature);
	sSwitchRequest = wait_period_request(BPrivate::kLowLatencyPeriodFrames,
		1000000, &sSwitchTook);
	sSwitchMixerFrames = wait_mixer_frames(BPrivate::kLowLatencyPeriodFrames,
		300000);
}


static void
switch_off(const char* signature)
{
	AudioStreamControl control;
	AudioStream stream;
	if (find_stream(control, signature, stream, 3000000))
		control.SetLowLatency(stream.id, false);
}


static int32 sEnvironmentStreamOn = -1;

static void
check_environment_stream(const char* signature)
{
	AudioStreamControl control;
	AudioStream stream;
	if (find_stream(control, signature, stream, 3000000)) {
		sEnvironmentStreamOn = stream.lowLatency ? 1 : 0;
		control.SetLowLatency(stream.id, false);
	}
}


// #pragma mark - desktop


static void
screenshot(const char* name)
{
	printf("CE-HOST screenshot %s\n", name);
	fflush(stdout);
	snooze(1500000);
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


static team_id
start(const char* path, const char* argument1, const char* argument2)
{
	pid_t child = fork();
	if (child == 0) {
		execl(path, path, argument1, argument2, (char*)NULL);
		_exit(127);
	}
	return child;
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


static void
test_desktop(const char* self)
{
	if (!be_roster->IsRunning("application/x-vnd.Be-input_server")) {
		info("no desktop, no screenshots");
		return;
	}
	static const char* kMedia = "/boot/system/preferences/Media";

	// A game in low-latency mode playing, so that the audio settings list
	// it with its switch on.
	Game game;
	// No tone: starting the preferences loads the guest heavily, which is
	// not what the steadiness checks measure.
	start_game(game, self, kGame, 0, false);
	snooze(kSettle);
	AudioStreamControl control;
	AudioStream stream;
	bool found = find_stream(control, kGame, stream);
	if (found && !stream.lowLatency) {
		control.SetLowLatency(stream.id, true);
		snooze(1000000);
		found = find_stream(control, kGame, stream);
	}
	report("gui_stream", found && stream.lowLatency, "the game's stream %s",
		!found ? "not found" : stream.lowLatency ? "in low-latency mode"
			: "not in low-latency mode");

	team_id media = start(kMedia, NULL, NULL);
	int32 windows = wait_for_window(media);
	report("gui_audio_settings", windows > 0, "Media preferences: %" B_PRId32
		" window(s)", windows);
	snooze(2500000);
	screenshot("media_low_latency");
	quit(media);

	BString device = output_name();
	media = start(kMedia, "--device", device.String());
	windows = wait_for_window(media);
	report("gui_device_page", windows > 0 && device.Length() > 0,
		"Media --device \"%s\": %" B_PRId32 " window(s)", device.String(),
		windows);
	snooze(2500000);
	screenshot("media_buffer_size");
	quit(media);

	if (found)
		control.SetLowLatency(stream.id, false);
	stop_game(game);
}


// #pragma mark -


int
main(int argc, char** argv)
{
	if (argc >= 5 && strcmp(argv[1], "game") == 0)
		return run_game(argv[2], argv[3], atof(argv[4]));

	signal(SIGPIPE, SIG_IGN);
	BApplication app("application/x-vnd.Renga-media-latency-test");
	printf("CE-SUITE " SUITE " START\n");
	fflush(stdout);

	bool mixer = wait_for_mixer(30000000);
	report("mixer", mixer, "system mixer %s", mixer ? "connected"
		: "not connected within 30 s");
	if (!mixer) {
		printf("CE-SUITE " SUITE " DONE failures=%d\n", sFailures);
		return 1;
	}
	// Let the media services settle (the mixer and the cards are set up
	// and started once more right after media_server starts).
	snooze(3000000);
	// The default output may still hold a request of an earlier test.
	bigtime_t took;
	wait_period_request(0, 6000000, &took);
	int32 defaultFrames = mixer_frames();
	BString device = output_name();
	info("default output \"%s\", mixer buffers of %" B_PRId32 " frames",
		device.String(), defaultFrames);

	int32 value = -1;
	bool hasRequest = output_parameter(RENGA_PERIOD_REQUEST_KIND, &value,
		false) == B_OK;
	bool hasBufferFrames = output_parameter(RENGA_BUFFER_FRAMES_KIND, &value,
		false) == B_OK && value == 0;
	report("device_parameters", hasRequest && hasBufferFrames,
		"default output: period request %s, Buffer size %s",
		hasRequest ? "present" : "missing",
		hasBufferFrames ? "automatic" : "missing or not automatic");

	// 1. Normal mode: BSoundPlayer's own buffer length (10 ms).
	PhaseResult normal = run_phase(argv[0], kGame, 2.5, false,
		switch_on_and_watch);
	int32 normalFrames = (int32)(normal.stats.rate / 100);
	report("normal_buffers", normal.stats.valid
		&& normal.stats.minFrames == normal.stats.maxFrames
		&& normal.stats.maxFrames == normalFrames,
		"callbacks of %" B_PRId32 "-%" B_PRId32 " frames (want %" B_PRId32
		", BSoundPlayer's default of 10 ms)", normal.stats.minFrames,
		normal.stats.maxFrames, normalFrames);
	report("clock_continuous_on", normal.clockStep >= 0
		&& normal.clockStep < 20000, "the output's clock moved by at most %"
		B_PRIdBIGTIME " us against real time per 20 ms, across the switch to "
		"short periods (want under 20 ms)", normal.clockStep);
	report("normal_latency", normal.latency > 0, "%" B_PRIdBIGTIME
		" us from callback to playout (median of %" B_PRId32 " bursts, %"
		B_PRIdBIGTIME "-%" B_PRIdBIGTIME "), mixer buffers %" B_PRId32
		" frames", normal.latency, normal.onsets, normal.minLatency,
		normal.maxLatency, defaultFrames);

	// 2. The switch (set during the normal phase, as the audio settings
	// do it) asks the default output for short periods.
	report("switch_requests_period",
		sSwitchRequest == BPrivate::kLowLatencyPeriodFrames,
		"the default output was asked for %" B_PRId32 " frames (want %"
		B_PRId32 ") %.2f s after the switch", sSwitchRequest,
		BPrivate::kLowLatencyPeriodFrames, sSwitchTook / 1000000.0);
	report("mixer_follows_period",
		sSwitchMixerFrames == BPrivate::kLowLatencyPeriodFrames,
		"mixer buffers of %" B_PRId32 " frames (want %" B_PRId32 ")",
		sSwitchMixerFrames, BPrivate::kLowLatencyPeriodFrames);

	// 3. Low-latency mode: the game starts again with the switch on.
	PhaseResult low = run_phase(argv[0], kGame, 6.0, false, switch_off);
	int32 lowFrames = (int32)(low.stats.rate * BPrivate::kLowLatencyPeriodFrames
		/ BPrivate::kLowLatencyPeriodRate);
	report("low_latency_buffers", low.stats.valid
		&& low.stats.minFrames == low.stats.maxFrames
		&& low.stats.maxFrames == lowFrames,
		"callbacks of %" B_PRId32 "-%" B_PRId32 " frames (want %" B_PRId32
		" every time)", low.stats.minFrames, low.stats.maxFrames, lowFrames);
	double period = lowFrames * 1000000.0 / max_c(low.stats.rate, 1.0);
	double expected = low.stats.callbacks * period;
	// QEMU without KVM (the harness) runs the guest at a fraction of real
	// speed and its audio timer ticks every 10 ms, so a callback can come a
	// period late now and then; what must not happen is the stream falling
	// behind (the count of callbacks against the time) or repeated drops.
	// The callbacks are compared with the card's clock (the player's
	// performance time): without KVM QEMU's audio can run slower than the
	// host's clock for a while, and the stream follows the card.
	report("low_latency_callbacks_steady", low.stats.valid
		&& low.stats.late * 100 <= low.stats.callbacks
		&& fabs(low.stats.performance - expected) < 0.06 * expected,
		"%" B_PRId32 " callbacks (%.3f s of buffers) in %.3f s of the card's "
		"clock (%.3f s real), longest interval %.1f ms, %" B_PRId32
		" over 3 periods (at most 1 %%)", low.stats.callbacks,
		expected / 1000000.0, low.stats.performance / 1000000.0,
		low.stats.elapsed / 1000000.0, low.stats.maxInterval / 1000.0,
		low.stats.late);
	report("low_latency_latency", low.latency > 0 && normal.latency > 0
		&& low.latency * 2 <= normal.latency && low.latency <= 30000,
		"%" B_PRIdBIGTIME " us from callback to playout (median, %"
		B_PRIdBIGTIME "-%" B_PRIdBIGTIME "; normal mode %" B_PRIdBIGTIME
		" us; want at most half and 30 ms); BSoundPlayer::Latency() %"
		B_PRIdBIGTIME " us (normal mode %" B_PRIdBIGTIME " us)", low.latency,
		low.minLatency, low.maxLatency, normal.latency, low.stats.latency,
		normal.stats.latency);
	report("low_latency_mix_continuous", low.toneSamples
			> 5 * low.stats.rate && low.gaps <= 1
			&& low.gap <= 4 * (size_t)lowFrames,
		"6 s tone in the mix: %.2f s, %" B_PRId32 " gap(s), the longest %"
		B_PRIuSIZE " samples (at most one, of 4 periods)",
		low.toneSamples / max_c(low.stats.rate, 1.0), low.gaps, low.gap);
	report("clock_steady", low.clockStep >= 0 && low.clockStep < 20000,
		"in low-latency mode the output's clock moved by at most %"
		B_PRIdBIGTIME " us against real time per 20 ms (want under 20 ms)",
		low.clockStep);

	// 4. Switched off (during the tone): after the game, the card goes
	// back to its default period.
	int32 request = wait_period_request(0, 8000000, &took);
	int32 frames = wait_mixer_frames(defaultFrames, 3000000);
	report("switch_off_releases", request == 0 && frames == defaultFrames,
		"period request %" B_PRId32 " %.1f s after the game, mixer buffers "
		"%" B_PRId32 " frames (want %" B_PRId32 ")", request,
		took / 1000000.0, frames, defaultFrames);

	// 5. Asked for by the environment.
	PhaseResult environment = run_phase(argv[0], kEnvGame, 2.0, true,
		check_environment_stream);
	report("environment", environment.stats.valid
		&& environment.stats.maxFrames == lowFrames
		&& sEnvironmentStreamOn == 1,
		"RENGA_AUDIO_LOW_LATENCY=1: callbacks of %" B_PRId32 " frames, "
		"mixer stream switch %s", environment.stats.maxFrames,
		sEnvironmentStreamOn == 1 ? "on" : sEnvironmentStreamOn == 0
			? "off" : "not found");
	wait_period_request(0, 8000000, &took);

	// 6. The user fixes the period ("Buffer size" on the device's page).
	int32 fixed = 512;
	status_t status = output_parameter(RENGA_BUFFER_FRAMES_KIND, &fixed,
		true);
	frames = wait_mixer_frames(512, 4000000);
	int32 automatic = 0;
	output_parameter(RENGA_BUFFER_FRAMES_KIND, &automatic, true);
	int32 back = wait_mixer_frames(defaultFrames, 4000000);
	report("buffer_size_setting", status == B_OK && frames == 512
		&& back == defaultFrames,
		"Buffer size 512: mixer buffers of %" B_PRId32 " frames; Automatic "
		"again: %" B_PRId32 " (want %" B_PRId32 ")", frames, back,
		defaultFrames);

	// 7. The controls.
	test_desktop(argv[0]);
	wait_period_request(0, 8000000, &took);

	printf("CE-SUITE " SUITE " DONE failures=%d\n", sFailures);
	return sFailures == 0 ? 0 : 1;
}
