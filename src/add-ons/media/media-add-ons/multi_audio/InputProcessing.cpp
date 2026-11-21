/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 *
 * Microphone processing for sound card inputs (batch 11, roadmap F-31):
 * mute, an 80 Hz high-pass filter, a noise gate and an automatic gain, in
 * this order after the recording gain. Deliberately small and real-time
 * safe; spectral noise suppression and echo cancellation are not done here
 * (they need a reference of what plays, or a trained model).
 */


#include "InputProcessing.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <StringList.h>


static const float kGateClosed = 0.01f;
	// -40 dB: a closed gate leaves a little room noise, which sounds
	// more natural than digital silence and keeps the gate from clicking
static const float kGateHoldSeconds = 0.2f;
static const float kGateAttackSeconds = 0.002f;
static const float kGateReleaseSeconds = 0.08f;
static const float kAgcTarget = 0.1f;
	// -20 dBFS RMS
static const float kAgcMaxGain = 10.0f;
	// +20 dB
static const float kAgcMinGain = 0.316f;
	// -10 dB
static const float kAgcUpDBPerSecond = 3.0f;
static const float kAgcDownDBPerSecond = 20.0f;
static const float kAgcSilence = 0.001f;
	// -60 dBFS: below this the automatic gain holds still (no pumping up
	// the room noise between words)


input_processing_settings::input_processing_settings()
	:
	mute(false),
	gateDB(0),
	agc(false),
	highPass(false)
{
}


void
input_processing_settings::Parse(const char* text)
{
	BStringList lines;
	BString(text).Split("\n", true, lines);
	for (int32 i = 0; i < lines.CountStrings(); i++) {
		BString line = lines.StringAt(i);
		line.ReplaceAll('\r', ' ').Trim();
		int32 space = line.FindFirst(' ');
		if (space <= 0)
			continue;
		BString key, value;
		line.CopyInto(key, 0, space);
		line.CopyInto(value, space + 1, line.Length() - space - 1);
		value.Trim();
		if (key == "mute")
			mute = atoi(value.String()) != 0;
		else if (key == "gate") {
			float db = atof(value.String());
			gateDB = db == 0 ? 0 : fmaxf(-80.0f, fminf(-20.0f, db));
		} else if (key == "agc")
			agc = atoi(value.String()) != 0;
		else if (key == "highpass")
			highPass = atoi(value.String()) != 0;
	}
}


BString
input_processing_settings::Format() const
{
	char text[128];
	snprintf(text, sizeof(text), "mute %d\ngate %g\nagc %d\nhighpass %d\n",
		mute ? 1 : 0, gateDB, agc ? 1 : 0, highPass ? 1 : 0);
	return BString(text);
}


// #pragma mark -


InputProcessor::InputProcessor()
	:
	fRate(0)
{
	Reset();
}


void
InputProcessor::Reset()
{
	memset(fX1, 0, sizeof(fX1));
	memset(fX2, 0, sizeof(fX2));
	memset(fY1, 0, sizeof(fY1));
	memset(fY2, 0, sizeof(fY2));
	fGateOpen = true;
	fGateGain = 1.0f;
	fGateHold = 0;
	fAgcGain = 1.0f;
	fMuteGain = 1.0f;
}


float
InputProcessor::AutomaticGainDB() const
{
	return 20.0f * log10f(fAgcGain);
}


static inline float
read_sample(const void* data, size_t index, uint32 format)
{
	switch (format) {
		case media_raw_audio_format::B_AUDIO_FLOAT:
			return ((const float*)data)[index];
		case media_raw_audio_format::B_AUDIO_INT:
			return ((const int32*)data)[index] / 2147483648.0f;
		case media_raw_audio_format::B_AUDIO_SHORT:
			return ((const int16*)data)[index] / 32768.0f;
		case media_raw_audio_format::B_AUDIO_UCHAR:
			return (((const uint8*)data)[index] - 128) / 128.0f;
		case media_raw_audio_format::B_AUDIO_CHAR:
			return ((const int8*)data)[index] / 128.0f;
	}
	return 0;
}


static inline void
write_sample(void* data, size_t index, uint32 format, float value)
{
	value = fmaxf(-1.0f, fminf(1.0f, value));
	switch (format) {
		case media_raw_audio_format::B_AUDIO_FLOAT:
			((float*)data)[index] = value;
			break;
		case media_raw_audio_format::B_AUDIO_INT:
			((int32*)data)[index] = (int32)fmaxf(-2147483648.0f,
				fminf(2147483520.0f, value * 2147483648.0f));
			break;
		case media_raw_audio_format::B_AUDIO_SHORT:
			((int16*)data)[index] = (int16)fmaxf(-32768.0f,
				fminf(32767.0f, value * 32768.0f));
			break;
		case media_raw_audio_format::B_AUDIO_UCHAR:
			((uint8*)data)[index] = (uint8)(128 + fmaxf(-128.0f,
				fminf(127.0f, value * 128.0f)));
			break;
		case media_raw_audio_format::B_AUDIO_CHAR:
			((int8*)data)[index] = (int8)fmaxf(-128.0f,
				fminf(127.0f, value * 128.0f));
			break;
	}
}


static size_t
sample_size(uint32 format)
{
	return format & media_raw_audio_format::B_AUDIO_SIZE_MASK;
}


void
InputProcessor::Process(void* data, const media_raw_audio_format& format,
	const input_processing_settings& settings)
{
	size_t size = sample_size(format.format);
	uint32 channels = format.channel_count;
	if (size == 0 || channels == 0 || format.frame_rate <= 0)
		return;
	if (settings.IsNeutral() && fMuteGain == 1.0f && fGateGain == 1.0f
		&& fAgcGain == 1.0f) {
		return;
	}
	size_t frames = format.buffer_size / (size * channels);
	if (frames == 0)
		return;
	float rate = format.frame_rate;
	float seconds = frames / rate;

	if (rate != fRate) {
		// RBJ cookbook high-pass, 80 Hz, Q 0.707
		fRate = rate;
		float w = 2 * M_PI * 80.0f / rate;
		float alpha = sinf(w) / (2 * 0.7071f);
		float cosw = cosf(w);
		float a0 = 1 + alpha;
		fB0 = (1 + cosw) / 2 / a0;
		fB1 = -(1 + cosw) / a0;
		fB2 = (1 + cosw) / 2 / a0;
		fA1 = -2 * cosw / a0;
		fA2 = (1 - alpha) / a0;
	}

	// 1. high-pass, in place, and the buffer's level after it
	double energy = 0;
	for (size_t frame = 0; frame < frames; frame++) {
		for (uint32 c = 0; c < channels; c++) {
			size_t index = frame * channels + c;
			float x = read_sample(data, index, format.format);
			if (settings.highPass && c < kMaxChannels) {
				float y = fB0 * x + fB1 * fX1[c] + fB2 * fX2[c]
					- fA1 * fY1[c] - fA2 * fY2[c];
				fX2[c] = fX1[c];
				fX1[c] = x;
				fY2[c] = fY1[c];
				fY1[c] = y;
				x = y;
				write_sample(data, index, format.format, x);
			}
			energy += (double)x * x;
		}
	}
	float rms = sqrtf(energy / (frames * channels));

	// 2. the gate decides per buffer, its gain moves per sample
	float gateTarget = 1.0f;
	if (settings.gateDB != 0) {
		float threshold = powf(10.0f, settings.gateDB / 20.0f);
		if (rms >= threshold) {
			fGateOpen = true;
			fGateHold = kGateHoldSeconds;
		} else if (fGateHold > 0)
			fGateHold -= seconds;
		else
			fGateOpen = false;
		gateTarget = fGateOpen ? 1.0f : kGateClosed;
	} else
		fGateOpen = true;

	// 3. the automatic gain follows speech, not silence
	float agcTarget = 1.0f;
	if (settings.agc) {
		agcTarget = fAgcGain;
		if (rms > kAgcSilence && fGateOpen) {
			float wanted = fmaxf(kAgcMinGain, fminf(kAgcMaxGain,
				kAgcTarget / rms));
			float nowDB = 20.0f * log10f(fAgcGain);
			float wantedDB = 20.0f * log10f(wanted);
			if (wantedDB > nowDB)
				nowDB = fminf(wantedDB, nowDB + kAgcUpDBPerSecond * seconds);
			else
				nowDB = fmaxf(wantedDB, nowDB - kAgcDownDBPerSecond * seconds);
			agcTarget = powf(10.0f, nowDB / 20.0f);
		}
	}

	float muteTarget = settings.mute ? 0.0f : 1.0f;

	// per-sample ramps: the gate with its attack and release times, the
	// automatic gain and mute linearly over the buffer
	float gateCoefficient = gateTarget > fGateGain
		? 1.0f - expf(-1.0f / (kGateAttackSeconds * rate))
		: 1.0f - expf(-1.0f / (kGateReleaseSeconds * rate));
	float agcStep = (agcTarget - fAgcGain) / frames;
	float muteStep = (muteTarget - fMuteGain) / frames;
	float gate = fGateGain;
	float agc = fAgcGain;
	float mute = fMuteGain;
	for (size_t frame = 0; frame < frames; frame++) {
		gate += (gateTarget - gate) * gateCoefficient;
		agc += agcStep;
		mute += muteStep;
		float gain = gate * agc * mute;
		if (gain == 1.0f)
			continue;
		for (uint32 c = 0; c < channels; c++) {
			size_t index = frame * channels + c;
			write_sample(data, index, format.format,
				read_sample(data, index, format.format) * gain);
		}
	}
	fGateGain = settings.gateDB != 0 ? gate : 1.0f;
	if (fabsf(fGateGain - 1.0f) < 1e-4f)
		fGateGain = 1.0f;
	fAgcGain = settings.agc ? agcTarget : 1.0f;
	fMuteGain = muteTarget;
}
