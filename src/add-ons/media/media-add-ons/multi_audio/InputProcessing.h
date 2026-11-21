/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 */
#ifndef INPUT_PROCESSING_H
#define INPUT_PROCESSING_H


#include <MediaDefs.h>
#include <String.h>


/*!	Batch 11 (F-31 routing): what a sound card's recording goes through
	after the recording gain, set by the hidden text parameter
	"renga:input_processing" (AudioStreamControl.h) and kept per device.
	Every part is off by default, so a recording is unchanged until the
	user switches something on in the Audio preferences.
*/
struct input_processing_settings {
	bool	mute;			// silence (push-to-talk releases it too)
	float	gateDB;			// noise gate threshold; 0: off
	bool	agc;			// automatic gain towards -20 dBFS
	bool	highPass;		// 80 Hz high-pass filter

			input_processing_settings();

	bool	IsNeutral() const
				{ return !mute && gateDB == 0 && !agc && !highPass; }
	// "key value" lines; unknown keys are ignored, missing ones kept.
	void	Parse(const char* text);
	BString	Format() const;
};


/*!	The state of the processing of one recorded stream (one node output):
	filter memories, the gate's and the automatic gain's levels. Works on
	the buffer in place, in any raw audio sample format, interleaved.
	Real-time safe: no allocation, no locks.
*/
class InputProcessor {
public:
								InputProcessor();

			void				Reset();
			void				Process(void* data,
									const media_raw_audio_format& format,
									const input_processing_settings& settings);

			// for the tests and the level display: the last gate state and
			// the automatic gain applied, in dB
			bool				GateOpen() const { return fGateOpen; }
			float				AutomaticGainDB() const;

private:
	enum { kMaxChannels = 8 };

			float				fRate;
			// biquad high-pass, direct form I, per channel
			float				fB0, fB1, fB2, fA1, fA2;
			float				fX1[kMaxChannels], fX2[kMaxChannels];
			float				fY1[kMaxChannels], fY2[kMaxChannels];
			// gate
			bool				fGateOpen;
			float				fGateGain;		// now, 0.01 to 1
			float				fGateHold;		// seconds left open
			// automatic gain
			float				fAgcGain;		// now, linear
			// mute fade
			float				fMuteGain;		// now, 0 to 1
};


#endif	// INPUT_PROCESSING_H
