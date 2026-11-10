/*
 * Copyright 2003-2016 Haiku Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Marcus Overhagen
 * 	Dario Casalinuovo
 */


#include "MixerCore.h"

#include <string.h>

#include <Buffer.h>
#include <BufferGroup.h>
#include <BufferProducer.h>
#include <MediaNode.h>
#include <RealtimeAlloc.h>
#include <StackOrHeapArray.h>
#include <StopWatch.h>
#include <TimeSource.h>

#include "AudioMixer.h"
#include "Interpolate.h"
#include "MixerInput.h"
#include "MixerOutput.h"
#include "MixerTap.h"
#include "MixerUtils.h"
#include "Resampler.h"
#include "RtList.h"


#define DOUBLE_RATE_MIXING 	0

#if DEBUG > 1
#	define ASSERT_LOCKED()	if (fLocker->IsLocked()) {} \
	else debugger("core not locked, meltdown occurred")
#else
#	define ASSERT_LOCKED()	((void)0)
#endif

/*!	Mixer channels are identified by a type number, each type number corresponds
	to the one of the channel masks of enum media_multi_channels.

	The mixer buffer uses either the same frame rate and same count of frames as
	the output buffer, or the double frame rate and frame count.

	All mixer input ring buffers must be an exact multiple of the mixer buffer
	size, so that we do not get any buffer wrap around during reading from the
	input buffers.
	The mixer input is told by constructor (or after a format change by
	SetMixBufferFormat() of the current mixer buffer propertys, and must
	allocate a buffer that is an exact multiple,
*/


struct chan_info {
	const char 	*base;
	uint32		sample_offset;
	float		gain;
	float		gain_delta;		// per frame: gain ramps, see _Mix()
};


MixerCore::MixerCore(AudioMixer *node)
	:
	fLocker(new BLocker("mixer core lock")),
	fInputs(new BList),
	fOutput(0),
	fNextInputID(1),
	fRunning(false),
	fStarted(false),
	fOutputEnabled(true),
	fResampler(0),
	fMixBuffer(0),
	fMixBufferFrameRate(0),
	fMixBufferFrameCount(0),
	fMixBufferChannelCount(0),
	fMixBufferChannelTypes(0),
	fDoubleRateMixing(DOUBLE_RATE_MIXING),
	fDownstreamLatency(1),
	fSettings(new MixerSettings),
	fNode(node),
	fBufferGroup(0),
	fTimeSource(0),
	fMixThread(-1),
	fMixThreadWaitSem(-1),
	fOutputGain(1.0),
	fTaps(new BList),
	fTapBuffer(NULL),
	fAllBuffer(NULL)
{
}


MixerCore::~MixerCore()
{
	delete fSettings;

	delete fLocker;
	delete fInputs;

	ASSERT(fMixThreadWaitSem == -1);
	ASSERT(fMixThread == -1);

	if (fMixBuffer)
		rtm_free(fMixBuffer);
	if (fTapBuffer)
		rtm_free(fTapBuffer);
	if (fAllBuffer)
		rtm_free(fAllBuffer);

	delete fTaps;

	if (fTimeSource)
		fTimeSource->Release();

	if (fResampler) {
		for (int i = 0; i < fMixBufferChannelCount; i++)
			delete fResampler[i];
		delete[] fResampler;
	}

	delete[] fMixBufferChannelTypes;
}


MixerSettings *
MixerCore::Settings()
{
	return fSettings;
}


void
MixerCore::UpdateResamplingAlgorithm()
{
	ASSERT_LOCKED();

	_UpdateResamplers(fOutput->MediaOutput().format.u.raw_audio);

	for (int32 i = fInputs->CountItems() - 1; i >= 0; i--) {
		MixerInput* input
			= reinterpret_cast<MixerInput*>(fInputs->ItemAtFast(i));
		input->UpdateResamplingAlgorithm();
	}
}


void
MixerCore::SetOutputAttenuation(float gain)
{
	ASSERT_LOCKED();
	fOutputGain = gain;
}


MixerInput*
MixerCore::AddInput(const media_input& input)
{
	ASSERT_LOCKED();
	MixerInput* in = new MixerInput(this, input, fMixBufferFrameRate,
		fMixBufferFrameCount);
	fInputs->AddItem(in);
	return in;
}


MixerOutput*
MixerCore::AddOutput(const media_output& output)
{
	ASSERT_LOCKED();
	if (fOutput != NULL) {
		ERROR("MixerCore::AddOutput: already connected\n");
		return fOutput;
	}

	fOutput = new MixerOutput(this, output);
	// the output format might have been adjusted inside MixerOutput
	_ApplyOutputFormat();

	ASSERT(!fRunning);
	if (fStarted && fOutputEnabled)
		_StartMixThread();

	return fOutput;
}


bool
MixerCore::RemoveInput(int32 inputID)
{
	ASSERT_LOCKED();
	MixerInput *input;
	for (int i = 0; (input = Input(i)) != 0; i++) {
		if (input->ID() == inputID) {
			fInputs->RemoveItem(i);
			delete input;
			return true;
		}
	}
	return false;
}


bool
MixerCore::RemoveOutput()
{
	ASSERT_LOCKED();
	if (fOutput == NULL)
		return false;

	if (fRunning)
		_StopMixThread();

	delete fOutput;
	fOutput = NULL;
	fOutputEnabled = true;
	return true;
}


int32
MixerCore::CreateInputID()
{
	ASSERT_LOCKED();
	return fNextInputID++;
}


MixerInput *
MixerCore::Input(int i)
{
	ASSERT_LOCKED();
	return (MixerInput *)fInputs->ItemAt(i);
}


MixerOutput *
MixerCore::Output()
{
	ASSERT_LOCKED();
	return fOutput;
}


void
MixerCore::BufferReceived(BBuffer *buffer, bigtime_t lateness)
{
	ASSERT_LOCKED();
	if (!fRunning)
		return;

	MixerInput *input;
	int32 id = buffer->Header()->destination;
	for (int i = 0; (input = Input(i)) != 0; i++) {
		if (input->ID() == id) {
			input->BufferReceived(buffer);
			return;
		}
	}
	ERROR("MixerCore::BufferReceived: received buffer for unknown id %ld\n",
		id);
}


void
MixerCore::InputFormatChanged(int32 inputID,
	const media_multi_audio_format &format)
{
	ASSERT_LOCKED();
	ERROR("MixerCore::InputFormatChanged not handled\n");
}


void
MixerCore::OutputFormatChanged(const media_multi_audio_format &format)
{
	ASSERT_LOCKED();

	if (fRunning)
		_StopMixThread();

	fOutput->ChangeFormat(format);
	_ApplyOutputFormat();

	if (fStarted)
		_StartMixThread();
}


void
MixerCore::SetOutputBufferGroup(BBufferGroup *group)
{
	ASSERT_LOCKED();
	fBufferGroup = group;
}


void
MixerCore::SetTimingInfo(BTimeSource *ts, bigtime_t downstream_latency)
{
	ASSERT_LOCKED();

	if (fTimeSource != ts) {
		if (fTimeSource != NULL)
			fTimeSource->Release();
		if (ts != NULL)
			fTimeSource = dynamic_cast<BTimeSource *>(ts->Acquire());
		else
			fTimeSource = NULL;
	}

	fDownstreamLatency = downstream_latency;

	TRACE("MixerCore::SetTimingInfo, now = %lld, downstream latency %lld\n",
		ts != NULL ? fTimeSource->Now() : 0, fDownstreamLatency);
}


void
MixerCore::EnableOutput(bool enabled)
{
	ASSERT_LOCKED();
	TRACE("MixerCore::EnableOutput %d\n", enabled);
	fOutputEnabled = enabled;

	if (fRunning && !fOutputEnabled)
		_StopMixThread();

	if (!fRunning && fOutput != NULL && fStarted && fOutputEnabled)
		_StartMixThread();
}


uint32
MixerCore::OutputChannelCount()
{
	return (fOutput) ? fOutput->GetOutputChannelCount() : 0;
}


bool
MixerCore::Start()
{
	ASSERT_LOCKED();
	TRACE("MixerCore::Start\n");
	if (fStarted)
		return false;

	fStarted = true;

	ASSERT(!fRunning);

	// only start the mix thread if we have an output
	if (fOutput != NULL && fOutputEnabled)
		_StartMixThread();

	return true;
}


bool
MixerCore::Stop()
{
	ASSERT_LOCKED();
	TRACE("MixerCore::Stop\n");
	if (!fStarted)
		return false;

	if (fRunning)
		_StopMixThread();

	fStarted = false;
	return true;
}


void
MixerCore::_StartMixThread()
{
	ASSERT(fOutputEnabled);
	ASSERT(!fRunning);
	ASSERT(fOutput != NULL);

	fRunning = true;
	fMixThreadWaitSem = create_sem(0, "mix thread wait");
	fMixThread = spawn_thread(_MixThreadEntry, "Yeah baby, very shagadelic",
		120, this);
	resume_thread(fMixThread);
}


void
MixerCore::_StopMixThread()
{
	ASSERT(fRunning);
	ASSERT(fMixThread > 0);
	ASSERT(fMixThreadWaitSem > 0);

	fRunning = false;
	status_t unused;
	delete_sem(fMixThreadWaitSem);
	wait_for_thread(fMixThread, &unused);

	fMixThread = -1;
	fMixThreadWaitSem = -1;
}


// #pragma mark - private


void
MixerCore::_UpdateResamplers(const media_multi_audio_format& format)
{
	ASSERT_LOCKED();

	if (fResampler != NULL) {
		for (int i = 0; i < fMixBufferChannelCount; i++)
			delete fResampler[i];
		delete[] fResampler;
	}

	fResampler = new Resampler*[fMixBufferChannelCount];
	for (int i = 0; i < fMixBufferChannelCount; i++) {
		switch (Settings()->ResamplingAlgorithm()) {
			case 2:
				fResampler[i] = new Interpolate(
					media_raw_audio_format::B_AUDIO_FLOAT, format.format);
				break;
			default:
				fResampler[i] = new Resampler(
					media_raw_audio_format::B_AUDIO_FLOAT, format.format);
		}
	}
}


void
MixerCore::_ApplyOutputFormat()
{
	ASSERT_LOCKED();

	const media_multi_audio_format& format
		= fOutput->MediaOutput().format.u.raw_audio;

	if (fMixBuffer != NULL)
		rtm_free(fMixBuffer);

	delete[] fMixBufferChannelTypes;

	fMixBufferFrameRate = (int32)(0.5 + format.frame_rate);
	fMixBufferFrameCount = frames_per_buffer(format);
	if (fDoubleRateMixing) {
		fMixBufferFrameRate *= 2;
		fMixBufferFrameCount *= 2;
	}
	fMixBufferChannelCount = format.channel_count;
	ASSERT(fMixBufferChannelCount == fOutput->GetOutputChannelCount());
	fMixBufferChannelTypes = new int32 [format.channel_count];

	for (int i = 0; i < fMixBufferChannelCount; i++) {
		 fMixBufferChannelTypes[i]
		 	= ChannelMaskToChannelType(GetChannelMask(i, format.channel_mask));
	}

	fMixBuffer = (float*)rtm_alloc(NULL, sizeof(float) * fMixBufferFrameCount
		* fMixBufferChannelCount);
	ASSERT(fMixBuffer != NULL);
	if (fTapBuffer != NULL)
		rtm_free(fTapBuffer);
	if (fAllBuffer != NULL)
		rtm_free(fAllBuffer);
	fTapBuffer = (float*)rtm_alloc(NULL, sizeof(float) * fMixBufferFrameCount
		* fMixBufferChannelCount);
	fAllBuffer = (float*)rtm_alloc(NULL, sizeof(float) * fMixBufferFrameCount
		* fMixBufferChannelCount);

	_UpdateResamplers(format);

	TRACE("MixerCore::OutputFormatChanged:\n");
	TRACE("  fMixBufferFrameRate %ld\n", fMixBufferFrameRate);
	TRACE("  fMixBufferFrameCount %ld\n", fMixBufferFrameCount);
	TRACE("  fMixBufferChannelCount %ld\n", fMixBufferChannelCount);
	for (int i = 0; i < fMixBufferChannelCount; i++)
		TRACE("  fMixBufferChannelTypes[%i] %ld\n", i, fMixBufferChannelTypes[i]);

	MixerInput *input;
	for (int i = 0; (input = Input(i)); i++)
		input->SetMixBufferFormat(fMixBufferFrameRate, fMixBufferFrameCount);
}


int32
MixerCore::_MixThreadEntry(void* arg)
{
	static_cast<MixerCore*>(arg)->_MixThread();
	return 0;
}


void
MixerCore::_MixThread()
{
	if (!Lock())
		return;
	{
		// Delay starting the mixer until we have a valid time source.
		bigtime_t performanceTime, realTime;
		float drift;
		while (fTimeSource->GetTime(&performanceTime, &realTime, &drift) != B_OK
				|| performanceTime <= 0 || realTime <= 0
				|| (realTime + 1 * 1000 * 1000) <= system_time()) {
			TRACE("MixerCore: delaying _MixThread start, timesource is at %" B_PRIdBIGTIME "\n",
				performanceTime);
			Unlock();
			snooze(5000);
			while (!LockWithTimeout(10000)) {
				if (!fRunning)
					return;
			}
		}
	}
	Unlock();

	const bigtime_t start = fTimeSource->Now();

	bigtime_t eventLatency = max((bigtime_t)3600, bigtime_t(0.4 * buffer_duration(
		fOutput->MediaOutput().format.u.raw_audio)));

	// TODO: when the format changes while running, everything is wrong!
	bigtime_t bufferRequestTimeout = buffer_duration(
		fOutput->MediaOutput().format.u.raw_audio) / 2;

	TRACE("MixerCore: starting _MixThread at %lld with latency %lld and "
		"downstream latency %lld, bufferRequestTimeout %lld\n", start,
		eventLatency, fDownstreamLatency, bufferRequestTimeout);

	// We must read from the input buffer at a position (pos) that is always
	// a multiple of fMixBufferFrameCount.
	int64 temp = frames_for_duration(fMixBufferFrameRate, start);
	const int64 frameBase = ((temp / fMixBufferFrameCount) + 1)
		* fMixBufferFrameCount;
	const bigtime_t timeBase = duration_for_frames(fMixBufferFrameRate, frameBase);

	TRACE("MixerCore: starting _MixThread, start %lld, timeBase %lld, "
		"frameBase %lld\n", start, timeBase, frameBase);

	ASSERT(fMixBufferFrameCount > 0);

	chan_info_list inputChanInfos[MAX_CHANNEL_TYPES];
	BStackOrHeapArray<chan_info_list, 16> mixChanInfos(fMixBufferChannelCount);
		// TODO: this does not support changing output channel count
	if (!mixChanInfos.IsValid()) {
		ERROR("MixerCore::_MixThread mixChanInfos allocation failed\n");
		return;
	}

	int64 framePos = 0;
	uint64 bufferIndex = 0;
	bigtime_t eventTime = 0, nextRun = B_INFINITE_TIMEOUT;
	while (fRunning) {
		if (nextRun == B_INFINITE_TIMEOUT) {
			eventTime = timeBase + bigtime_t((1000000LL * framePos)
				/ fMixBufferFrameRate);
			nextRun = fTimeSource->RealTimeFor(eventTime, 0)
				- eventLatency - fDownstreamLatency;
		}

		status_t status = acquire_sem_etc(fMixThreadWaitSem, 1,
			B_ABSOLUTE_TIMEOUT, nextRun);
		if (status != B_TIMED_OUT) {
			if (status == B_OK || status == B_INTERRUPTED)
				continue;
			return;
		}
		nextRun = B_INFINITE_TIMEOUT;

		if (!LockWithTimeout(10000)) {
			ERROR("MixerCore: LockWithTimeout failed\n");
			continue;
		}

		// Nothing to mix: send silence, and keep the taps (sinks, loopback
		// recorders) fed with it too.
		bool tapsReady = false;
		for (int32 i = 0; i < fTaps->CountItems(); i++) {
			if (((MixerTap*)fTaps->ItemAt(i))->IsReady()) {
				tapsReady = true;
				break;
			}
		}
		if (fInputs->IsEmpty() || (fOutput->IsMuted() && !tapsReady)) {
			int size = fOutput->MediaOutput().format.u.raw_audio.buffer_size;
			BBuffer* buffer = fBufferGroup->RequestBuffer(size,
				bufferRequestTimeout);
			if (buffer != NULL) {
				int middle = 0;
				if (fOutput->MediaOutput().format.u.raw_audio.format
						== media_raw_audio_format::B_AUDIO_UCHAR)
					middle = 128;
				memset(buffer->Data(), middle, size);
				// fill in the buffer header
				media_header* hdr = buffer->Header();
				hdr->type = B_MEDIA_RAW_AUDIO;
				hdr->size_used = size;
				hdr->time_source = fTimeSource->ID();
				hdr->start_time = eventTime;
				if (fNode->SendBuffer(buffer, fOutput) != B_OK) {
					ERROR("MixerCore: SendBuffer failed for buffer %lld\n",
						bufferIndex);
					buffer->Recycle();
				}
			} else {
				ERROR("MixerCore: RequestBuffer failed for buffer %lld\n",
					bufferIndex);
			}

			if (tapsReady) {
				_FeedTaps(frameBase + framePos, eventTime, inputChanInfos,
					&mixChanInfos[0], true);
			}

			bufferIndex++;
			framePos += fMixBufferFrameCount;

			Unlock();
			continue;
		}

		int64 currentFramePos;
		currentFramePos = frameBase + framePos;

		// mix all data from all inputs into the mix buffer
		ASSERT(currentFramePos % fMixBufferFrameCount == 0);

		PRINT(4, "create new buffer event at %lld, reading input frames at "
			"%lld\n", eventTime, currentFramePos);

		// The default output gets the streams not routed to a sink; the
		// sinks and loopbacks get theirs in _FeedTaps().
		_Mix(fMixBuffer, kMixPrimary, NULL, NULL, currentFramePos, eventTime,
			inputChanInfos, &mixChanInfos[0]);
		_FeedTaps(currentFramePos, eventTime, inputChanInfos,
			&mixChanInfos[0], false);
		_FinishMixCycle(currentFramePos, eventTime);

		// request a buffer
		BBuffer* buffer;
		buffer = fBufferGroup->RequestBuffer(
			fOutput->MediaOutput().format.u.raw_audio.buffer_size,
			bufferRequestTimeout);
		if (buffer != NULL) {
			// copy data from mix buffer into output buffer
			for (int i = 0; i < fMixBufferChannelCount; i++) {
				fResampler[i]->Resample(
					reinterpret_cast<char*>(fMixBuffer) + i * sizeof(float),
					fMixBufferChannelCount * sizeof(float),
					fMixBufferFrameCount,
					reinterpret_cast<char*>(buffer->Data())
						+ (i * bytes_per_sample(
							fOutput->MediaOutput().format.u.raw_audio)),
					bytes_per_frame(fOutput->MediaOutput().format.u.raw_audio),
					frames_per_buffer(
						fOutput->MediaOutput().format.u.raw_audio),
					fOutput->IsMuted() ? 0.0f
						: fOutputGain * fOutput->GetOutputChannelGain(i));
			}
			PRINT(4, "send buffer, inframes %ld, outframes %ld\n",
				fMixBufferFrameCount,
				frames_per_buffer(fOutput->MediaOutput().format.u.raw_audio));

			// fill in the buffer header
			media_header* hdr = buffer->Header();
			hdr->type = B_MEDIA_RAW_AUDIO;
			hdr->size_used
				= fOutput->MediaOutput().format.u.raw_audio.buffer_size;
			hdr->time_source = fTimeSource->ID();
			hdr->start_time = eventTime;

			// swap byte order if necessary
			fOutput->AdjustByteOrder(buffer);

			// send the buffer
			status_t res = fNode->SendBuffer(buffer, fOutput);
			if (res != B_OK) {
				ERROR("MixerCore: SendBuffer failed for buffer %lld\n",
					bufferIndex);
				buffer->Recycle();
			}
		} else {
			ERROR("MixerCore: RequestBuffer failed for buffer %lld\n",
				bufferIndex);
		}

		bufferIndex++;
		framePos += fMixBufferFrameCount;

		Unlock();
	}
}


// #pragma mark - taps and routing


void
MixerCore::AddTap(MixerTap* tap)
{
	ASSERT_LOCKED();
	if (!fTaps->HasItem(tap))
		fTaps->AddItem(tap);
	UpdateRouting();
}


bool
MixerCore::RemoveTap(MixerTap* tap)
{
	ASSERT_LOCKED();
	if (!fTaps->RemoveItem(tap))
		return false;
	for (int32 i = 0; MixerInput* input = Input(i); i++) {
		if (input->Sink() == tap)
			input->SetSink(NULL);
		if (input->PreviousSink() == tap)
			input->SetPreviousSink(NULL);
	}
	return true;
}


int32
MixerCore::CountTaps() const
{
	return fTaps->CountItems();
}


MixerTap*
MixerCore::TapAt(int32 index) const
{
	return (MixerTap*)fTaps->ItemAt(index);
}


void
MixerCore::SetPrimaryDeviceName(const char* name)
{
	ASSERT_LOCKED();
	fPrimaryDeviceName = name != NULL ? name : "";
	UpdateRouting();
}


void
MixerCore::UpdateRouting()
{
	ASSERT_LOCKED();
	for (int32 i = 0; MixerInput* input = Input(i); i++) {
		MixerTap* sink = NULL;
		const char* route = input->Route();
		if (route[0] != '\0' && fPrimaryDeviceName != route) {
			for (int32 j = 0; j < fTaps->CountItems(); j++) {
				MixerTap* tap = (MixerTap*)fTaps->ItemAt(j);
				if (tap->Kind() == MixerTap::kSink && !tap->IsDead()
					&& strcmp(tap->DeviceName(), route) == 0) {
					sink = tap;
					break;
				}
			}
		}
		input->SetSink(sink);
	}
}


/*!	Whether \a input goes into the mix of the given target this cycle, and
	with which fade: a stream that moves between the default output and a
	sink (a route change, or a sink that became ready or went away) fades
	out over one buffer on the old one and in on the new one, so that a
	device switch does not click.
*/
bool
MixerCore::_InputFade(MixerInput* input, int mode, MixerTap* tap,
	const char* signature, float& fadeStart, float& fadeEnd)
{
	fadeStart = fadeEnd = 1.0f;
	switch (mode) {
		case kMixAll:
			return true;
		case kMixApplication:
			return signature != NULL && signature[0] != '\0'
				&& strcmp(input->AppSignature(), signature) == 0;
	}

	MixerTap* now = input->Sink();
	if (now != NULL && !now->IsReady())
		now = NULL;
	MixerTap* before = input->PreviousSink();
	MixerTap* target = mode == kMixPrimary ? NULL : tap;

	bool isNow = now == target;
	bool wasBefore = before == target;
	if (!isNow && !wasBefore)
		return false;
	fadeStart = wasBefore ? 1.0f : 0.0f;
	fadeEnd = isNow ? 1.0f : 0.0f;
	return true;
}


/*!	Mixes the inputs that belong to the given target into \a destination
	(fMixBufferChannelCount interleaved channels, fMixBufferFrameCount
	frames), through the output's channel mapping. Gains ramp linearly over
	the buffer from what each input channel had in the previous buffer (and
	the fade of _InputFade()), so volume changes, mute and device switches
	do not click. Returns whether any input contributed.
*/
bool
MixerCore::_Mix(float* destination, int mode, MixerTap* tap,
	const char* signature, int64 framePos, bigtime_t eventTime,
	chan_info_list* inputChanInfos, chan_info_list* mixChanInfos)
{
	bool any = false;
	float frames = (float)fMixBufferFrameCount;

	for (int i = 0; MixerInput* input = Input(i); i++) {
		float fadeStart, fadeEnd;
		if (!_InputFade(input, mode, tap, signature, fadeStart, fadeEnd))
			continue;

		int count = input->GetMixerChannelCount();
		for (int channel = 0; channel < count; channel++) {
			int type;
			const float* base;
			uint32 sampleOffset;
			float gain;
			if (!input->GetMixerChannelInfo(channel, framePos, eventTime,
					&base, &sampleOffset, &type, &gain, true)) {
				continue;
			}
			if (type < 0 || type >= MAX_CHANNEL_TYPES)
				continue;
			float last = input->LastMixerChannelGain(channel);
			if (last < 0)
				last = gain;
			float start = last * fadeStart;
			float end = gain * fadeEnd;
			if (start == 0 && end == 0)
				continue;
			chan_info* info = inputChanInfos[type].Create();
			info->base = (const char*)base;
			info->sample_offset = sampleOffset;
			info->gain = start;
			info->gain_delta = (end - start) / frames;
			any = true;
		}
	}

	for (int channel = 0; channel < fMixBufferChannelCount; channel++) {
		int sourceCount = fOutput->GetOutputChannelSourceCount(channel);
		for (int i = 0; i < sourceCount; i++) {
			int type;
			float gain;
			fOutput->GetOutputChannelSourceInfoAt(channel, i, &type, &gain);
			if (type < 0 || type >= MAX_CHANNEL_TYPES)
				continue;
			int count = inputChanInfos[type].CountItems();
			for (int j = 0; j < count; j++) {
				chan_info* info = inputChanInfos[type].ItemAt(j);
				chan_info* newInfo = mixChanInfos[channel].Create();
				newInfo->base = info->base;
				newInfo->sample_offset = info->sample_offset;
				newInfo->gain = info->gain * gain;
				newInfo->gain_delta = info->gain_delta * gain;
			}
		}
	}

	memset(destination, 0,
		fMixBufferChannelCount * fMixBufferFrameCount * sizeof(float));
	for (int channel = 0; channel < fMixBufferChannelCount; channel++) {
		int count = mixChanInfos[channel].CountItems();
		for (int i = 0; i < count; i++) {
			chan_info* info = mixChanInfos[channel].ItemAt(i);
			// This looks slightly ugly, but the current GCC will generate
			// the fastest code this way.
			// fMixBufferFrameCount is always > 0.
			uint32 dstSampleOffset = fMixBufferChannelCount * sizeof(float);
			uint32 srcSampleOffset = info->sample_offset;
			char* dst = (char*)&destination[channel];
			char* src = (char*)info->base;
			float gain = info->gain;
			int j = fMixBufferFrameCount;
			if (info->gain_delta == 0) {
				do {
					*(float*)dst += *(const float*)src * gain;
					dst += dstSampleOffset;
					src += srcSampleOffset;
				} while (--j);
			} else {
				float delta = info->gain_delta;
				do {
					*(float*)dst += *(const float*)src * gain;
					gain += delta;
					dst += dstSampleOffset;
					src += srcSampleOffset;
				} while (--j);
			}
		}
	}

	for (int i = 0; i < MAX_CHANNEL_TYPES; i++)
		inputChanInfos[i].MakeEmpty();
	for (int i = 0; i < fMixBufferChannelCount; i++)
		mixChanInfos[i].MakeEmpty();
	return any;
}


/*!	Remembers, per input, the gains and the sink of the cycle just mixed:
	the starting point of the next cycle's ramps and fades.
*/
void
MixerCore::_FinishMixCycle(int64 framePos, bigtime_t eventTime)
{
	for (int i = 0; MixerInput* input = Input(i); i++) {
		int count = input->GetMixerChannelCount();
		for (int channel = 0; channel < count; channel++) {
			int type;
			const float* base;
			uint32 sampleOffset;
			float gain;
			if (input->GetMixerChannelInfo(channel, framePos, eventTime,
					&base, &sampleOffset, &type, &gain, true)) {
				input->SetLastMixerChannelGain(channel, gain);
			}
		}
		MixerTap* now = input->Sink();
		input->SetPreviousSink(now != NULL && now->IsReady() ? now : NULL);
	}
}


/*!	Feeds the sinks (the streams routed to them, at the master volume) and
	the loopbacks (every stream, or one application's, before the master
	volume, so that a recording does not depend on the speaker volume).
*/
void
MixerCore::_FeedTaps(int64 framePos, bigtime_t eventTime,
	chan_info_list* inputChanInfos, chan_info_list* mixChanInfos, bool silent)
{
	if (fTaps->IsEmpty() || fTapBuffer == NULL || fAllBuffer == NULL)
		return;

	size_t bytes = fMixBufferChannelCount * fMixBufferFrameCount
		* sizeof(float);
	float masterGain = 0;
	if (!fOutput->IsMuted()) {
		for (int i = 0; i < fMixBufferChannelCount; i++)
			masterGain += fOutput->GetOutputChannelGain(i);
		masterGain = fOutputGain * masterGain / fMixBufferChannelCount;
	}

	bool allMixed = false;
	for (int32 i = 0; i < fTaps->CountItems(); i++) {
		MixerTap* tap = (MixerTap*)fTaps->ItemAt(i);
		if (!tap->IsReady())
			continue;

		if (silent) {
			memset(fTapBuffer, 0, bytes);
			tap->Push(fTapBuffer, fMixBufferChannelCount,
				fMixBufferFrameCount, fMixBufferFrameRate, eventTime, fTimeSource->ID(), 1.0f);
			continue;
		}

		if (tap->Kind() == MixerTap::kSink) {
			_Mix(fTapBuffer, kMixSink, tap, NULL, framePos, eventTime,
				inputChanInfos, mixChanInfos);
			tap->Push(fTapBuffer, fMixBufferChannelCount,
				fMixBufferFrameCount, fMixBufferFrameRate, eventTime,
				fTimeSource->ID(), masterGain);
			continue;
		}

		char signature[B_MIME_TYPE_LENGTH];
		if (!tap->GetMixSource(signature, sizeof(signature)))
			continue;
		if (signature[0] == '\0') {
			if (!allMixed) {
				_Mix(fAllBuffer, kMixAll, NULL, NULL, framePos, eventTime,
					inputChanInfos, mixChanInfos);
				allMixed = true;
			}
			tap->Push(fAllBuffer, fMixBufferChannelCount,
				fMixBufferFrameCount, fMixBufferFrameRate, eventTime, fTimeSource->ID(), 1.0f);
		} else {
			_Mix(fTapBuffer, kMixApplication, NULL, signature, framePos,
				eventTime, inputChanInfos, mixChanInfos);
			tap->Push(fTapBuffer, fMixBufferChannelCount,
				fMixBufferFrameCount, fMixBufferFrameRate, eventTime, fTimeSource->ID(), 1.0f);
		}
	}
}
