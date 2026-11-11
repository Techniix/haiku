/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 */
#ifndef _SINC_RESAMPLER_H
#define _SINC_RESAMPLER_H


#include "Resampler.h"


/*!	Band-limited resampling (Renga, roadmap F-31 phase 1): a windowed-sinc
	interpolator (32 taps, Kaiser window, beta 9, 256 phases interpolated
	linearly), the cutoff following the ratio so that downsampling does not
	alias. The phase is continuous: input is queued and consumed at the
	nominal rate ratio (SetRates()), whatever the mixer's per-buffer frame
	counts are (they are rounded to whole frames, and a resampler that
	restarts its phase with each buffer puts a phase step at every buffer
	boundary: that limited linear interpolation to about -34 dB THD+N at
	1 kHz). A small correction keeps the queue at its working level when
	the counts and the ratio disagree over time. A resampled stream is
	delayed by about 20 input samples (0.4 ms); equal rates are a plain copy.

	The mixer's "Resampling algorithm" 3, "Best quality"; the default for
	new settings. Measured in the harness (media_resample): a 1.1 kHz tone
	from 44.1 to 48 kHz (see the roadmap's F-31 for the numbers).
*/
class SincResampler : public Resampler {
public:
								SincResampler(uint32 sourceFormat,
									uint32 destFormat);
								~SincResampler();

			//!	The nominal input and output frame rates.
			void				SetRates(float inputRate, float outputRate);

private:
	static	void				_Resample(Resampler* object, const void* src,
									int32 srcSampleOffset,
									int32 srcSampleCount, void* dest,
									int32 destSampleOffset,
									int32 destSampleCount, float gain);
			bool				_Grow(int32 samples);
			void				_BuildTable(float cutoff);

			uint32				fSourceFormat;
			float*				fBuffer;		// queued input
			int32				fBufferSize;
			int32				fQueued;		// samples in fBuffer
			double				fPosition;		// of the next output point
			double				fStep;			// nominal; 0: unknown
			float*				fTable;			// (phases + 1) x taps
			float				fCutoff;
};


#endif	// _SINC_RESAMPLER_H
