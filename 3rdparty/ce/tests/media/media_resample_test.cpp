/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 */

/*!	Resampling quality of the system mixer (roadmap F-31 phase 1): a
	stream at 44.1 kHz is mixed at the card's 48 kHz, and what the mixer
	made of it is recorded through the loopback of this program's own
	streams. For each of the mixer's "Resampling algorithm" choices (0
	drop/repeat, 2 linear interpolation, 3 windowed sinc) the test plays a
	1.1 kHz tone (not 1 kHz: the host looks for virtio_sound_test's 1 kHz
	phase in the first card's recording) and a 15 kHz tone (whose image, which interpolation leaves,
	lands at 18.9 kHz) and measures THD+N: the residual after the best-
	fitting sine at the tone's frequency (and DC), in dB below the tone, the
	median over 100 ms windows (so that a buffer QEMU delivers late does not
	decide). The choice in effect before is put back afterwards.
*/


#include <Application.h>
#include <Autolock.h>
#include <Locker.h>
#include <MediaRecorder.h>
#include <MediaRoster.h>
#include <OS.h>
#include <ParameterWeb.h>
#include <SoundPlayer.h>
#include <String.h>

#include <AudioStreamControl.h>

#include <algorithm>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include <vector>


#define SUITE "media_resample"

static const char* kSignature = "application/x-vnd.Renga-media-resample-test";
static const float kPlayRate = 44100;
static const float kAmplitude = 0.25f;
// the mixer's "Resampling algorithm" parameter (AudioMixer.cpp, PARAM_ETC)
static const int32 kAlgorithmParameter = 70;

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


// #pragma mark - player


struct Tone {
	double	phase;
	double	hertz;
};


static void
play_buffer(void* cookie, void* buffer, size_t size,
	const media_raw_audio_format& format)
{
	Tone* tone = (Tone*)cookie;
	float* samples = (float*)buffer;
	size_t frames = size / (sizeof(float) * format.channel_count);
	double step = 2 * M_PI * tone->hertz / format.frame_rate;
	for (size_t i = 0; i < frames; i++) {
		float value = kAmplitude * (float)sin(tone->phase);
		tone->phase += step;
		if (tone->phase > 2 * M_PI)
			tone->phase -= 2 * M_PI;
		for (uint32 c = 0; c < format.channel_count; c++)
			samples[i * format.channel_count + c] = value;
	}
}


// #pragma mark - recording


struct Recording {
	BLocker				lock;
	std::vector<float>	left;
	float				rate;

	Recording() : lock("recording"), rate(0) {}
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
	BAutolock locker(recording->lock);
	recording->rate = raw.frame_rate;
	size_t frames = size / (sampleSize * raw.channel_count);
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
	}
}


static status_t
record_loopback(double seconds, Recording& recording)
{
	BMediaRoster* roster = BMediaRoster::Roster();
	media_node node;
	status_t status = BPrivate::AudioStreamControl::StartLoopback(kSignature,
		&node);
	if (status != B_OK)
		return status;
	media_output output;
	int32 count;
	status = roster->GetFreeOutputsFor(node, &output, 1, &count,
		B_MEDIA_RAW_AUDIO);
	if (status == B_OK && count < 1)
		status = B_BUSY;
	if (status == B_OK) {
		BMediaRecorder recorder("resample recorder", B_MEDIA_RAW_AUDIO);
		recorder.SetHooks(record_buffer, NULL, &recording);
		media_format format = output.format;
		status = recorder.Connect(node, &output, &format);
		if (status == B_OK && (status = recorder.Start()) == B_OK) {
			snooze((bigtime_t)(seconds * 1000000));
			recorder.Stop();
		}
		recorder.Disconnect();
	}
	roster->ReleaseNode(node);
	return status;
}


/*!	THD+N in dB of a tone at \a hertz in \a samples (\a rate): per window,
	the least-squares sine (and DC) at that frequency is taken out and the
	residual's power compared with the sine's; the median over the windows.
*/
static double
thd_n(const std::vector<float>& samples, double rate, double hertz,
	double skip, int32* _windows)
{
	size_t window = (size_t)(rate / 10);
	std::vector<double> results;
	for (size_t start = (size_t)(skip * rate);
			start + window <= samples.size(); start += window) {
		// normal equations for a*sin + b*cos + c
		double s[3][3] = {{0}};
		double r[3] = {0};
		for (size_t i = 0; i < window; i++) {
			double t = 2 * M_PI * hertz * (start + i) / rate;
			double basis[3] = { sin(t), cos(t), 1.0 };
			for (int a = 0; a < 3; a++) {
				r[a] += basis[a] * samples[start + i];
				for (int b = 0; b < 3; b++)
					s[a][b] += basis[a] * basis[b];
			}
		}
		// Gaussian elimination
		for (int col = 0; col < 3; col++) {
			for (int row = col + 1; row < 3; row++) {
				double factor = s[row][col] / s[col][col];
				for (int k = col; k < 3; k++)
					s[row][k] -= factor * s[col][k];
				r[row] -= factor * r[col];
			}
		}
		double x[3];
		for (int row = 2; row >= 0; row--) {
			double sum = r[row];
			for (int k = row + 1; k < 3; k++)
				sum -= s[row][k] * x[k];
			x[row] = sum / s[row][row];
		}
		double signal = 0;
		double residual = 0;
		for (size_t i = 0; i < window; i++) {
			double t = 2 * M_PI * hertz * (start + i) / rate;
			double fit = x[0] * sin(t) + x[1] * cos(t);
			signal += fit * fit;
			double error = samples[start + i] - fit - x[2];
			residual += error * error;
		}
		if (signal > 0)
			results.push_back(10 * log10(residual / signal + 1e-15));
	}
	*_windows = results.size();
	if (results.empty())
		return 0;
	std::sort(results.begin(), results.end());
	return results[results.size() / 2];
}


// #pragma mark - mixer setting


static status_t
algorithm_parameter(int32* value, bool set)
{
	BMediaRoster* roster = BMediaRoster::Roster();
	media_node mixer;
	status_t status = roster->GetAudioMixer(&mixer);
	if (status != B_OK)
		return status;
	BParameterWeb* web;
	status = roster->GetParameterWebFor(mixer, &web);
	if (status == B_OK) {
		status = B_ENTRY_NOT_FOUND;
		for (int32 i = 0; i < web->CountParameters(); i++) {
			BParameter* parameter = web->ParameterAt(i);
			// PARAM_ETC(70) = (70 << 16) | 4 (AudioMixer.cpp)
			if (parameter->ID() != ((kAlgorithmParameter << 16) | 0x4)
				|| parameter->Type() != BParameter::B_DISCRETE_PARAMETER) {
				continue;
			}
			if (set) {
				status = parameter->SetValue(value, sizeof(int32),
					system_time());
			} else {
				size_t size = sizeof(int32);
				bigtime_t when;
				status = parameter->GetValue(value, &size, &when);
			}
			break;
		}
		delete web;
	}
	roster->ReleaseNode(mixer);
	return status;
}


static double
measure(int32 algorithm, double hertz, int32* _windows, status_t* _status)
{
	int32 value = algorithm;
	*_status = algorithm_parameter(&value, true);
	if (*_status != B_OK)
		return 0;

	media_raw_audio_format format = media_raw_audio_format::wildcard;
	format.format = media_raw_audio_format::B_AUDIO_FLOAT;
	format.channel_count = 2;
	format.frame_rate = kPlayRate;
	format.byte_order = B_MEDIA_HOST_ENDIAN;
	Tone tone = { 0, hertz };
	BSoundPlayer player(&format, "resample tone", play_buffer, NULL, &tone);
	*_status = player.InitCheck();
	if (*_status != B_OK)
		return 0;
	player.Start();
	player.SetHasData(true);
	snooze(700000);
	Recording recording;
	*_status = record_loopback(2.0, recording);
	player.Stop();
	if (*_status != B_OK || recording.rate <= 0)
		return 0;
	return thd_n(recording.left, recording.rate, hertz, 0.4, _windows);
}


int
main()
{
	BApplication app(kSignature);
	printf("CE-SUITE " SUITE " START\n");
	fflush(stdout);

	int32 original = -1;
	status_t status = algorithm_parameter(&original, false);
	report("setting", status == B_OK, "the mixer's resampling algorithm: %"
		B_PRId32 " (%s)", original, strerror(status));

	static const struct {
		int32		algorithm;
		const char*	name;
	} kAlgorithms[] = {
		{ 0, "drop_repeat" },
		{ 2, "linear" },
		{ 3, "sinc" },
	};
	double results[3][2];
	for (int a = 0; a < 3; a++) {
		for (int t = 0; t < 2; t++) {
			double hertz = t == 0 ? 1100 : 15000;
			int32 windows = 0;
			results[a][t] = measure(kAlgorithms[a].algorithm, hertz,
				&windows, &status);
			printf("CE-INFO media_resample: %s, %.0f Hz from 44.1 kHz: THD+N "
				"%.1f dB (%" B_PRId32 " windows, %s)\n", kAlgorithms[a].name,
				hertz, results[a][t], windows, strerror(status));
			fflush(stdout);
			if (status != B_OK || windows < 5)
				results[a][t] = 0;
		}
	}

	report("sinc_1khz", results[2][0] < -70 && results[2][0] < results[1][0]
			- 20,
		"windowed sinc: THD+N %.1f dB at 1.1 kHz (want below -70 dB and 20 dB "
		"better than linear interpolation's %.1f dB; drop/repeat %.1f dB)",
		results[2][0], results[1][0], results[0][0]);
	report("sinc_15khz", results[2][1] < -50 && results[2][1] < results[1][1]
			- 20,
		"windowed sinc: THD+N %.1f dB at 15 kHz, the image at 18.9 kHz "
		"included (want below -50 dB and 20 dB better than linear "
		"interpolation's %.1f dB; drop/repeat %.1f dB)", results[2][1],
		results[1][1], results[0][1]);

	int32 now = -1;
	algorithm_parameter(&now, false);
	report("default_sinc", original == 3, "new settings use the windowed sinc "
		"(the setting was %" B_PRId32 " when the test started)", original);
	if (original >= 0)
		algorithm_parameter(&original, true);

	printf("CE-SUITE " SUITE " DONE failures=%d\n", sFailures);
	fflush(stdout);
	return sFailures == 0 ? 0 : 1;
}
