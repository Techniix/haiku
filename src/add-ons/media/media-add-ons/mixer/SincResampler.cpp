/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 */


#include "SincResampler.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include <MediaDefs.h>

#include "MixerDebug.h"


static const int32 kTaps = 32;
static const int32 kHalf = kTaps / 2;
static const int32 kPhases = 256;
static const double kBeta = 9.0;


//!	The zeroth-order modified Bessel function of the first kind.
static double
bessel_i0(double x)
{
	double sum = 1.0;
	double term = 1.0;
	double half = x / 2;
	for (int k = 1; k < 32; k++) {
		term *= (half / k) * (half / k);
		sum += term;
		if (term < sum * 1e-12)
			break;
	}
	return sum;
}


SincResampler::SincResampler(uint32 sourceFormat, uint32 destFormat)
	:
	Resampler(),
	fSourceFormat(sourceFormat),
	fBuffer(NULL),
	fBufferSize(0),
	fQueued(kTaps + 4),
	fPosition(kHalf),
	fStep(0),
	fTable(NULL),
	fCutoff(0)
{
	if (destFormat != media_raw_audio_format::B_AUDIO_FLOAT) {
		ERROR("SincResampler: the destination format must be float\n");
		return;
	}
	switch (sourceFormat) {
		case media_raw_audio_format::B_AUDIO_FLOAT:
		case media_raw_audio_format::B_AUDIO_INT:
		case media_raw_audio_format::B_AUDIO_SHORT:
		case media_raw_audio_format::B_AUDIO_CHAR:
		case media_raw_audio_format::B_AUDIO_UCHAR:
			break;
		default:
			ERROR("SincResampler: unknown source format 0x%x\n",
				(int)sourceFormat);
			return;
	}
	fTable = (float*)malloc((kPhases + 1) * kTaps * sizeof(float));
	if (fTable == NULL || !_Grow(4096))
		return;
	_BuildTable(0.92f);
	fFunc = &_Resample;
}


SincResampler::~SincResampler()
{
	free(fBuffer);
	free(fTable);
}


void
SincResampler::SetRates(float inputRate, float outputRate)
{
	fStep = inputRate > 0 && outputRate > 0 ? (double)inputRate / outputRate
		: 0;
}


bool
SincResampler::_Grow(int32 samples)
{
	int32 size = fQueued + samples + kTaps;
	if (size <= fBufferSize)
		return true;
	float* buffer = (float*)realloc(fBuffer, size * sizeof(float));
	if (buffer == NULL)
		return false;
	if (fBuffer == NULL)
		memset(buffer, 0, fQueued * sizeof(float));
	fBuffer = buffer;
	fBufferSize = size;
	return true;
}


/*!	The filter for each of kPhases + 1 fractional positions: tap k of phase
	p weighs the input sample (k - kHalf + 1) samples after the integer
	position, for an output point p / kPhases samples after it.
	\a cutoff is a fraction of the input's Nyquist frequency.
*/
void
SincResampler::_BuildTable(float cutoff)
{
	fCutoff = cutoff;
	double norm = bessel_i0(kBeta);
	for (int32 p = 0; p <= kPhases; p++) {
		double fraction = (double)p / kPhases;
		double sum = 0;
		float* row = fTable + p * kTaps;
		for (int32 k = 0; k < kTaps; k++) {
			double x = (k - kHalf + 1) - fraction;
			double sinc = fabs(x) < 1e-9 ? 1.0
				: sin(M_PI * cutoff * x) / (M_PI * cutoff * x);
			double position = x / kHalf;
			double window = fabs(position) >= 1.0 ? 0.0
				: bessel_i0(kBeta * sqrt(1.0 - position * position)) / norm;
			row[k] = (float)(sinc * window);
			sum += row[k];
		}
		// unity gain at DC for every phase
		for (int32 k = 0; k < kTaps; k++)
			row[k] = (float)(row[k] / sum);
	}
}


template<typename inType>
static inline void
read_samples(float* destination, const char* source, int32 stride,
	int32 count, float scale, float middle)
{
	for (int32 i = 0; i < count; i++) {
		destination[i] = ((float)*(const inType*)source - middle) * scale;
		source += stride;
	}
}


/*static*/ void
SincResampler::_Resample(Resampler* object, const void* src,
	int32 srcSampleOffset, int32 srcSampleCount, void* _dest,
	int32 destSampleOffset, int32 destSampleCount, float gain)
{
	SincResampler* self = (SincResampler*)object;
	if (destSampleCount <= 0)
		return;
	if (srcSampleCount < 0)
		srcSampleCount = 0;
	char* dest = (char*)_dest;
	if (!self->_Grow(srcSampleCount)) {
		// out of memory: silence rather than garbage
		for (int32 i = 0; i < destSampleCount; i++) {
			*(float*)dest = 0;
			dest += destSampleOffset;
		}
		return;
	}

	// queue this call's input
	float* input = self->fBuffer + self->fQueued;
	const char* source = (const char*)src;
	switch (self->fSourceFormat) {
		case media_raw_audio_format::B_AUDIO_FLOAT:
			read_samples<float>(input, source, srcSampleOffset,
				srcSampleCount, 1.0f, 0);
			break;
		case media_raw_audio_format::B_AUDIO_INT:
			read_samples<int32>(input, source, srcSampleOffset,
				srcSampleCount, 1.0f / 2147483647.0f, 0);
			break;
		case media_raw_audio_format::B_AUDIO_SHORT:
			read_samples<int16>(input, source, srcSampleOffset,
				srcSampleCount, 1.0f / 32767.0f, 0);
			break;
		case media_raw_audio_format::B_AUDIO_CHAR:
			read_samples<int8>(input, source, srcSampleOffset,
				srcSampleCount, 1.0f / 127.0f, 0);
			break;
		case media_raw_audio_format::B_AUDIO_UCHAR:
			read_samples<uint8>(input, source, srcSampleOffset,
				srcSampleCount, 2.0f / 255.0f, 128);
			break;
	}
	self->fQueued += srcSampleCount;

	// The step: the nominal ratio, nudged (at most 0.2 %) towards keeping
	// about kHalf + 4 samples queued beyond the next output point; without
	// a nominal ratio, this call's counts.
	double step = (double)srcSampleCount / destSampleCount;
	bool sameRate = false;
	if (self->fStep > 0) {
		double level = self->fQueued - self->fPosition;
		double error = level - (kHalf + 4 + srcSampleCount);
		double nudge = error * 2e-5;
		if (nudge > 0.002)
			nudge = 0.002;
		else if (nudge < -0.002)
			nudge = -0.002;
		sameRate = self->fStep == 1.0;
		step = sameRate ? 1.0 : self->fStep * (1.0 + nudge);
	}

	if (sameRate && srcSampleCount == destSampleCount) {
		// Equal rates: a plain copy, without the filter's delay, so that
		// each buffer lands exactly where the mixer puts it (as with the
		// other resamplers). The queue keeps the last samples for a later
		// rate change.
		const float* in = self->fBuffer + self->fQueued - srcSampleCount;
		for (int32 j = 0; j < destSampleCount; j++) {
			*(float*)dest = in[j] * gain;
			dest += destSampleOffset;
		}
		int32 keep = kTaps + 4;
		if (self->fQueued > keep) {
			memmove(self->fBuffer, self->fBuffer + self->fQueued - keep,
				keep * sizeof(float));
			self->fQueued = keep;
		}
		self->fPosition = kHalf;
		return;
	}

	float cutoff = 0.92f * (step > 1.0 ? (float)(1.0 / step) : 1.0f);
	if (!sameRate && fabsf(cutoff - self->fCutoff) > 0.01f * self->fCutoff)
		self->_BuildTable(cutoff);
	const float* buffer = self->fBuffer;
	const float* table = self->fTable;
	double position = self->fPosition;
	double limit = self->fQueued - kHalf - 1;
	for (int32 j = 0; j < destSampleCount; j++) {
		if (position > limit)
			position = limit;	// starved: hold (should not happen)
		int32 index = (int32)position;
		double fraction = position - index;
		float value;
		if (sameRate && fraction == 0)
			value = buffer[index];
		else {
			double phase = fraction * kPhases;
			int32 p = (int32)phase;
			float blend = (float)(phase - p);
			const float* a = table + p * kTaps;
			const float* b = a + kTaps;
			const float* x = buffer + index - kHalf + 1;
			float sumA = 0;
			float sumB = 0;
			for (int32 k = 0; k < kTaps; k++) {
				sumA += x[k] * a[k];
				sumB += x[k] * b[k];
			}
			value = sumA + (sumB - sumA) * blend;
		}
		*(float*)dest = value * gain;
		dest += destSampleOffset;
		position += step;
	}

	// drop what no later output point needs
	int32 drop = (int32)position - kHalf + 1;
	if (drop > self->fQueued)
		drop = self->fQueued;
	if (drop > 0) {
		memmove(self->fBuffer, self->fBuffer + drop,
			(self->fQueued - drop) * sizeof(float));
		self->fQueued -= drop;
		position -= drop;
	}
	self->fPosition = position;
}
