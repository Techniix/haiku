/*
 * Copyright 2003-2010 Haiku Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Marcus Overhagen
 */
#ifndef _MIXER_CORE_H
#define _MIXER_CORE_H


#include "MixerSettings.h"

#include <Buffer.h>
#include <Locker.h>
#include <String.h>
#include <TimeSource.h>


class AudioMixer;
class BBufferGroup;
class MixerInput;
class MixerOutput;
class MixerTap;
struct loopback_selection;


// F-58 (OBS Studio's O-2): the route of a stream that plays on no output
// and is only recorded by loopbacks of its application or team. Not a
// device name any device can have (devices are named after their flavor,
// and no flavor name has the "renga:" prefix).
#define RENGA_LOOPBACK_ONLY_ROUTE	"renga:loopback-only"
class Resampler;
struct chan_info;
template<class value> class RtList;


// The number of "enum media_multi_channels" types from MediaDefs.h
// XXX should be 18, but limited to 12 here
#define MAX_CHANNEL_TYPES	12
// XXX using a dedicated mono channel, this should be channel type 31
//     but for now we redefine type 12
#define B_CHANNEL_MONO		B_CHANNEL_TOP_CENTER


class MixerCore {
public:
								MixerCore(AudioMixer* node);
	virtual						~MixerCore();

			MixerSettings*		Settings();
			void				UpdateResamplingAlgorithm();

	// To avoid calling Settings()->AttenuateOutput() for every outgoing
	// buffer, this setting is cached in fOutputGain and must be set by
	// the audio mixer node using SetOutputAttenuation()
			void				SetOutputAttenuation(float gain);
			MixerInput*			AddInput(const media_input& input);
			MixerOutput*		AddOutput(const media_output& output);

			bool				RemoveInput(int32 inputID);
			bool				RemoveOutput();
			int32				CreateInputID();
			int32				CountInputs() { return fInputs->CountItems(); }

	// index = 0 to count-1, NOT inputID
			MixerInput*			Input(int index);
			MixerOutput*		Output();

			bool				Lock();
			bool				LockWithTimeout(bigtime_t timeout);
			bool				IsLocked() const;
			void				Unlock();

			void				BufferReceived(BBuffer* buffer,
									bigtime_t lateness);

			void				InputFormatChanged(int32 inputID,
									const media_multi_audio_format& format);
			void				OutputFormatChanged(
									const media_multi_audio_format& format);

			void				SetOutputBufferGroup(BBufferGroup* group);
			void				SetTimingInfo(BTimeSource* timeSource,
									bigtime_t downstreamLatency);
			BTimeSource*		TimeSource() const { return fTimeSource; }
			void				EnableOutput(bool enabled);

			bool				Start();
			bool				Stop();

			uint32				OutputChannelCount();

	// Taps (sinks and loopbacks, see MixerTap): the mix thread feeds every
	// registered tap. The core holds no reference: sinks are owned by the
	// AudioMixer's router, loopbacks by whoever instantiated them, and a
	// tap leaves the core before it is deleted.
			void				AddTap(MixerTap* tap);
			bool				RemoveTap(MixerTap* tap);
			int32				CountTaps() const;
			MixerTap*			TapAt(int32 index) const;

	// Routing: the default output device's flavor name, and which sink
	// carries each input (by MixerInput::Route()). Call UpdateRouting()
	// after a route or the set of sinks changed.
			void				SetPrimaryDeviceName(const char* name);
			const char*			PrimaryDeviceName() const
									{ return fPrimaryDeviceName.String(); }
			void				UpdateRouting();

	static	MixerTap* const	kNowhere;

private:
	typedef RtList<chan_info> chan_info_list;

	enum {
		kMixPrimary,		// streams on the default output
		kMixSink,			// streams on one sink
		kMixAll,			// every stream (loopback "what you hear"),
							// less a loopback's exclusions
		kMixApplication		// the streams of one application
	};

			bool				_InputFade(MixerInput* input, int mode,
									MixerTap* tap,
									const loopback_selection* selection,
									float& fadeStart, float& fadeEnd);
			bool				_Mix(float* destination, int mode,
									MixerTap* tap,
									const loopback_selection* selection,
									int64 framePos, bigtime_t eventTime,
									chan_info_list* inputChanInfos,
									chan_info_list* mixChanInfos);
			void				_StartMixCycle(int64 framePos,
									bigtime_t eventTime);
			void				_FinishMixCycle(int64 framePos,
									bigtime_t eventTime);
			void				_FeedTaps(int64 framePos, bigtime_t eventTime,
									chan_info_list* inputChanInfos,
									chan_info_list* mixChanInfos,
									bool silent);

			void				_StartMixThread();
			void				_StopMixThread();

			void				_UpdateResamplers(
									const media_multi_audio_format& format);
			void				_ApplyOutputFormat();
	static	int32				_MixThreadEntry(void* arg);
			void				_MixThread();

private:
			BLocker*			fLocker;
			BList*				fInputs;
			MixerOutput*		fOutput;
			int32				fNextInputID;
			bool				fRunning;
									// true = the mix thread is running

			bool				fStarted;
									// true = mix thread should be
									// started of it is not running

			bool				fOutputEnabled;
									// true = mix thread should be
									// started of it is not running

			Resampler**			fResampler; // array
			float*				fMixBuffer;
			int32				fMixBufferFrameRate;
			int32				fMixBufferFrameCount;
			int32				fMixBufferChannelCount;
			int32*				fMixBufferChannelTypes; //array
			bool				fDoubleRateMixing;
			bigtime_t			fDownstreamLatency;
			MixerSettings*		fSettings;
			AudioMixer*			fNode;
			BBufferGroup*		fBufferGroup;
			BTimeSource*		fTimeSource;
			thread_id			fMixThread;
			sem_id				fMixThreadWaitSem;
			float				fOutputGain;

			BList*				fTaps;
			BString				fPrimaryDeviceName;
			float*				fTapBuffer;		// a sink's or loopback's mix
			float*				fAllBuffer;		// sum of every target
			loopback_selection*	fSelection;		// the mix thread's copy of a
												// loopback's (F-58)

	friend class MixerInput;
		// NOTE: debug only
};


inline bool
MixerCore::Lock()
{
	return fLocker->Lock();
}


inline bool
MixerCore::LockWithTimeout(bigtime_t timeout)
{
	return fLocker->LockWithTimeout(timeout) == B_OK;
}


inline bool
MixerCore::IsLocked() const
{
	return fLocker->IsLocked();
}


inline void
MixerCore::Unlock()
{
	fLocker->Unlock();
}


#endif // _MIXER_CORE_H
