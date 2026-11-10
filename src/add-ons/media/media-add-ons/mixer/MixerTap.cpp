/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 */


#include "MixerTap.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include <Autolock.h>
#include <Buffer.h>
#include <BufferGroup.h>
#include <MediaAddOn.h>
#include <MediaRoster.h>
#include <ParameterWeb.h>
#include <TimeSource.h>

#include "AudioMixer.h"
#include "MixerDebug.h"


// The format a tap prefers: 16-bit stereo (what SoundRecorder writes into
// WAV files) at the mix rate, about 21 ms per buffer.
static const uint32 kPreferredFrames = 1024;
static const float kDefaultRate = 48000;
static const int32 kBufferCount = 8;


static size_t
sample_size(uint32 format)
{
	return format & media_raw_audio_format::B_AUDIO_SIZE_MASK;
}


MixerTap::MixerTap(BMediaAddOn* addOn, kind_t kind, const char* name,
	int32 flavorID)
	:
	BMediaNode(name),
	BBufferProducer(B_MEDIA_RAW_AUDIO),
	BMediaEventLooper(),
	BControllable(),
	fAddOn(addOn),
	fKind(kind),
	fFlavorID(flavorID),
	fLock("mixer tap"),
	fConnected(false),
	fEnabled(true),
	fRunning(kind == kSink),
	fDead(false),
	fBufferGroup(NULL),
	fOwnBufferGroup(false),
	fDeviceNode(-1),
	fLastChange(0),
	fChannels(2),
	fRate(kDefaultRate),
	fBufferFrames(kPreferredFrames),
	fFifo(NULL),
	fFifoFrames(0),
	fFifoCapacity(0),
	fFifoTime(0),
	fPosition(0),
	fLastRate(0),
	fSendErrors(0),
	fPushTimeSource(-1),
	fActiveAt(0),
	fUnwantedSince(0)
{
	fLast[0] = fLast[1] = 0;
	fOutput.node = media_node::null;
	fOutput.source = media_source::null;
	fOutput.destination = media_destination::null;
	fOutput.format.type = B_MEDIA_RAW_AUDIO;
	fOutput.format.u.raw_audio = media_raw_audio_format::wildcard;
	strlcpy(fOutput.name, kind == kSink ? "Mixer sink" : "Loopback",
		sizeof(fOutput.name));
}


MixerTap::~MixerTap()
{
	// Leave the mixer before anything goes away: the mix thread may be
	// about to push into this tap.
	AudioMixer::TapDeleted(this);
	Quit();
	BAutolock locker(fLock);
	if (fOwnBufferGroup)
		delete fBufferGroup;
	delete[] fFifo;
}


void
MixerTap::SetDeviceName(const char* name)
{
	BAutolock locker(fLock);
	fDeviceName = name;
}


BString
MixerTap::SourceSignature()
{
	BAutolock locker(fLock);
	return fSourceSignature;
}


void
MixerTap::SetSourceSignature(const char* signature)
{
	{
		BAutolock locker(fLock);
		fSourceSignature = signature != NULL ? signature : "";
		fLastChange = system_time();
	}
	_UpdateWeb();
}


void
MixerTap::SetApplications(const BStringList& signatures,
	const BStringList& names)
{
	{
		BAutolock locker(fLock);
		if (fAppSignatures == signatures && fAppNames == names)
			return;
		fAppSignatures = signatures;
		fAppNames = names;
	}
	_UpdateWeb();
}


bool
MixerTap::GetMixSource(char* signature, size_t size)
{
	if (fLock.LockWithTimeout(0) != B_OK)
		return false;
	strlcpy(signature, fSourceSignature.String(), size);
	fLock.Unlock();
	return true;
}


bool
MixerTap::IsConnected()
{
	BAutolock locker(fLock);
	return fConnected && !fDead;
}


bool
MixerTap::GetConnection(media_output& output)
{
	BAutolock locker(fLock);
	if (!fConnected)
		return false;
	output = fOutput;
	return true;
}


bool
MixerTap::IsReady()
{
	if (fLock.LockWithTimeout(0) != B_OK)
		return false;
	bool ready = fConnected && fEnabled && fRunning && !fDead
		&& fBufferGroup != NULL && system_time() >= fActiveAt;
	fLock.Unlock();
	return ready;
}


void
MixerTap::_ResetConversion()
{
	fFifoFrames = 0;
	fPosition = 0;
	fLast[0] = fLast[1] = 0;
	fLastRate = 0;
}


void
MixerTap::Push(const float* data, int32 channels, int32 frames, float rate,
	bigtime_t startTime, media_node_id timeSource, float gain)
{
	fPushTimeSource = timeSource;
	// Never block the mix thread: if the node's own thread holds the lock
	// (a connection change), this buffer is skipped.
	if (fLock.LockWithTimeout(0) != B_OK)
		return;

	if (!fConnected || !fEnabled || !fRunning || fDead || fBufferGroup == NULL
		|| frames <= 0 || channels <= 0 || rate <= 0) {
		fLock.Unlock();
		return;
	}

	if (rate != fLastRate) {
		_ResetConversion();
		fLastRate = rate;
	}

	// Room for what this push produces plus a few buffers.
	double step = rate / fRate;
	uint32 produced = (uint32)(frames / step) + 2;
	uint32 needed = fFifoFrames + produced;
	if (needed > fFifoCapacity) {
		uint32 capacity = max_c(needed, fBufferFrames * 4 + produced);
		float* fifo = new(std::nothrow) float[capacity * fChannels];
		if (fifo == NULL) {
			fLock.Unlock();
			return;
		}
		if (fFifo != NULL)
			memcpy(fifo, fFifo, fFifoFrames * fChannels * sizeof(float));
		delete[] fFifo;
		fFifo = fifo;
		fFifoCapacity = capacity;
	}

	// The FIFO's first frame is this push's first frame when it was empty.
	if (fFifoFrames == 0)
		fFifoTime = startTime;

	// Linear interpolation over the virtual sequence x[0] = last frame of
	// the previous push, x[k] = data frame k - 1; fPosition is where the
	// next output frame lies in it.
	float* out = fFifo + fFifoFrames * fChannels;
	uint32 written = 0;
	while (fPosition < frames && written < produced) {
		int32 index = (int32)fPosition;
		float fraction = fPosition - index;
		float frame[2];
		for (int32 c = 0; c < 2; c++) {
			int32 source = c < channels ? c : channels - 1;
			float a = index == 0 ? fLast[c]
				: data[(index - 1) * channels + source];
			float b = data[index * channels + source];
			frame[c] = (a + (b - a) * fraction) * gain;
		}
		if (fChannels == 1)
			out[0] = (frame[0] + frame[1]) / 2;
		else {
			out[0] = frame[0];
			out[1] = frame[1];
			for (int32 c = 2; c < fChannels; c++)
				out[c] = 0;
		}
		out += fChannels;
		written++;
		fPosition += step;
	}
	fPosition -= frames;
	for (int32 c = 0; c < 2; c++) {
		int32 source = c < channels ? c : channels - 1;
		fLast[c] = data[(frames - 1) * channels + source];
	}
	fFifoFrames += written;

	_SendBuffers();
	fLock.Unlock();
}


//!	Sends every full buffer the FIFO holds. Called with fLock held.
void
MixerTap::_SendBuffers()
{
	const media_raw_audio_format& format = fOutput.format.u.raw_audio;
	size_t sampleSize = sample_size(format.format);
	size_t bufferSize = fBufferFrames * fChannels * sampleSize;

	while (fFifoFrames >= fBufferFrames) {
		BBuffer* buffer = fBufferGroup->RequestBuffer(bufferSize, 0);
		if (buffer != NULL) {
			const float* source = fFifo;
			uint32 samples = fBufferFrames * fChannels;
			switch (format.format) {
				case media_raw_audio_format::B_AUDIO_FLOAT:
					memcpy(buffer->Data(), source, samples * sizeof(float));
					break;
				case media_raw_audio_format::B_AUDIO_INT:
				{
					int32* dest = (int32*)buffer->Data();
					for (uint32 i = 0; i < samples; i++) {
						float v = max_c(-1.0f, min_c(1.0f, source[i]));
						dest[i] = (int32)(v * 2147483520.0f);
					}
					break;
				}
				case media_raw_audio_format::B_AUDIO_SHORT:
				{
					int16* dest = (int16*)buffer->Data();
					for (uint32 i = 0; i < samples; i++) {
						float v = max_c(-1.0f, min_c(1.0f, source[i]));
						dest[i] = (int16)lrintf(v * 32767.0f);
					}
					break;
				}
				case media_raw_audio_format::B_AUDIO_UCHAR:
				{
					uint8* dest = (uint8*)buffer->Data();
					for (uint32 i = 0; i < samples; i++) {
						float v = max_c(-1.0f, min_c(1.0f, source[i]));
						dest[i] = (uint8)(128 + lrintf(v * 127.0f));
					}
					break;
				}
				case media_raw_audio_format::B_AUDIO_CHAR:
				{
					int8* dest = (int8*)buffer->Data();
					for (uint32 i = 0; i < samples; i++) {
						float v = max_c(-1.0f, min_c(1.0f, source[i]));
						dest[i] = (int8)lrintf(v * 127.0f);
					}
					break;
				}
			}

			media_header* header = buffer->Header();
			header->type = B_MEDIA_RAW_AUDIO;
			header->size_used = bufferSize;
			// The times are the mixer's.
			header->time_source = fPushTimeSource;
			header->start_time = fFifoTime;

			status_t status = SendBuffer(buffer, fOutput.source,
				fOutput.destination);
			if (status != B_OK) {
				buffer->Recycle();
				// A consumer that is gone does not come back: let the mixer
				// fall back to its default output.
				if (status == B_BAD_PORT_ID || ++fSendErrors > 50)
					fDead = true;
			} else
				fSendErrors = 0;
		}

		// Without a free buffer the data is dropped: the consumer is behind,
		// and waiting would hold up the mix thread.
		fFifoFrames -= fBufferFrames;
		memmove(fFifo, fFifo + fBufferFrames * fChannels,
			fFifoFrames * fChannels * sizeof(float));
		fFifoTime += (bigtime_t)(fBufferFrames * 1000000.0 / fRate);

		if (fDead)
			break;
	}
}


// #pragma mark - BMediaNode


BMediaAddOn*
MixerTap::AddOn(int32* _internalID) const
{
	if (fAddOn != NULL && _internalID != NULL)
		*_internalID = fFlavorID;
	return fAddOn;
}


void
MixerTap::NodeRegistered()
{
	fOutput.node = Node();
	fOutput.source.port = ControlPort();
	fOutput.source.id = 0;
	_Specialize(&fOutput.format);

	SetPriority(B_REAL_TIME_PRIORITY);
	Run();
	_UpdateWeb();

	// Loopbacks are instantiated by recorders through the add-on; they
	// attach to the system mixer here. Sinks are added by the mixer.
	if (fKind == kLoopback)
		AudioMixer::AttachLoopback(this);
}


status_t
MixerTap::HandleMessage(int32 message, const void* data, size_t size)
{
	if (BControllable::HandleMessage(message, data, size) == B_OK
		|| BBufferProducer::HandleMessage(message, data, size) == B_OK
		|| BMediaNode::HandleMessage(message, data, size) == B_OK) {
		return B_OK;
	}
	HandleBadMessage(message, data, size);
	return B_ERROR;
}


void
MixerTap::Start(bigtime_t performanceTime)
{
	// Taps only forward what the mixer mixes, at its times: there is nothing
	// to schedule, so they run as soon as they are asked to. Recorders start
	// them in their own time source's time, which need not be the mixer's.
	{
		BAutolock locker(fLock);
		fRunning = true;
		_ResetConversion();
	}
	BMediaEventLooper::Start(performanceTime);
}


void
MixerTap::Stop(bigtime_t performanceTime, bool immediate)
{
	if (fKind == kLoopback) {
		BAutolock locker(fLock);
		fRunning = false;
	}
	BMediaEventLooper::Stop(performanceTime, immediate);
}


void
MixerTap::HandleEvent(const media_timed_event* event, bigtime_t lateness,
	bool realTimeEvent)
{
}


// #pragma mark - BBufferProducer


/*!	Fills in the wildcards of \a format with what the tap prefers: 16-bit
	stereo at the mix rate in 1024-frame buffers.
*/
void
MixerTap::_Specialize(media_format* format) const
{
	format->type = B_MEDIA_RAW_AUDIO;
	media_raw_audio_format& raw = format->u.raw_audio;
	const media_raw_audio_format& wild = media_raw_audio_format::wildcard;
	if (raw.frame_rate == wild.frame_rate)
		raw.frame_rate = kDefaultRate;
	if (raw.channel_count == wild.channel_count)
		raw.channel_count = 2;
	if (raw.format == wild.format)
		raw.format = media_raw_audio_format::B_AUDIO_SHORT;
	if (raw.byte_order == wild.byte_order)
		raw.byte_order = B_MEDIA_HOST_ENDIAN;
	if (raw.buffer_size == wild.buffer_size) {
		raw.buffer_size = kPreferredFrames * raw.channel_count
			* sample_size(raw.format);
	}
}


static bool
format_supported(const media_format& format)
{
	if (format.type != B_MEDIA_RAW_AUDIO)
		return false;
	const media_raw_audio_format& raw = format.u.raw_audio;
	switch (raw.format) {
		case media_raw_audio_format::B_AUDIO_FLOAT:
		case media_raw_audio_format::B_AUDIO_INT:
		case media_raw_audio_format::B_AUDIO_SHORT:
		case media_raw_audio_format::B_AUDIO_UCHAR:
		case media_raw_audio_format::B_AUDIO_CHAR:
			break;
		default:
			return false;
	}
	return raw.channel_count >= 1 && raw.channel_count <= 8
		&& raw.frame_rate >= 4000 && raw.frame_rate <= 384000
		&& raw.byte_order == B_MEDIA_HOST_ENDIAN
		&& raw.buffer_size >= raw.channel_count * sample_size(raw.format);
}


status_t
MixerTap::FormatSuggestionRequested(media_type type, int32 quality,
	media_format* format)
{
	if (type != B_MEDIA_RAW_AUDIO && type != B_MEDIA_UNKNOWN_TYPE)
		return B_MEDIA_BAD_FORMAT;
	format->Clear();
	format->type = B_MEDIA_RAW_AUDIO;
	format->u.raw_audio = media_raw_audio_format::wildcard;
	_Specialize(format);
	return B_OK;
}


status_t
MixerTap::FormatProposal(const media_source& output, media_format* format)
{
	if (output != fOutput.source)
		return B_MEDIA_BAD_SOURCE;
	if (format->type == B_MEDIA_UNKNOWN_TYPE)
		format->type = B_MEDIA_RAW_AUDIO;
	if (format->type != B_MEDIA_RAW_AUDIO)
		return B_MEDIA_BAD_FORMAT;
	_Specialize(format);
	if (!format_supported(*format)) {
		format->u.raw_audio = media_raw_audio_format::wildcard;
		_Specialize(format);
		return B_MEDIA_BAD_FORMAT;
	}
	return B_OK;
}


status_t
MixerTap::FormatChangeRequested(const media_source& source,
	const media_destination& destination, media_format* format,
	int32* _deprecated)
{
	return B_ERROR;
}


status_t
MixerTap::GetNextOutput(int32* cookie, media_output* _output)
{
	if (*cookie != 0)
		return B_BAD_INDEX;
	BAutolock locker(fLock);
	*_output = fOutput;
	*cookie = 1;
	return B_OK;
}


status_t
MixerTap::DisposeOutputCookie(int32 cookie)
{
	return B_OK;
}


status_t
MixerTap::SetBufferGroup(const media_source& source, BBufferGroup* group)
{
	if (source != fOutput.source)
		return B_MEDIA_BAD_SOURCE;
	BAutolock locker(fLock);
	if (fOwnBufferGroup)
		delete fBufferGroup;
	fBufferGroup = group;
	fOwnBufferGroup = false;
	if (group == NULL && fConnected) {
		fBufferGroup = new BBufferGroup(fOutput.format.u.raw_audio.buffer_size,
			kBufferCount);
		fOwnBufferGroup = true;
	}
	return B_OK;
}


status_t
MixerTap::GetLatency(bigtime_t* _latency)
{
	// Buffers are sent as soon as the mixer has mixed them; what the tap
	// adds is at most one of its buffers in the FIFO.
	*_latency = EventLatency() + (bigtime_t)(fBufferFrames * 1000000.0 / fRate);
	return B_OK;
}


status_t
MixerTap::PrepareToConnect(const media_source& what,
	const media_destination& where, media_format* format,
	media_source* _source, char* _name)
{
	if (what != fOutput.source)
		return B_MEDIA_BAD_SOURCE;

	BAutolock locker(fLock);
	if (fConnected)
		return B_MEDIA_ALREADY_CONNECTED;

	_Specialize(format);
	if (!format_supported(*format))
		return B_MEDIA_BAD_FORMAT;

	fOutput.destination = where;
	fOutput.format = *format;
	*_source = fOutput.source;
	strlcpy(_name, fOutput.name, B_MEDIA_NAME_LENGTH);
	return B_OK;
}


void
MixerTap::Connect(status_t error, const media_source& source,
	const media_destination& destination, const media_format& format,
	char* ioName)
{
	BAutolock locker(fLock);
	if (error != B_OK) {
		fOutput.destination = media_destination::null;
		return;
	}

	fOutput.destination = destination;
	fOutput.format = format;
	strlcpy(ioName, fOutput.name, B_MEDIA_NAME_LENGTH);

	const media_raw_audio_format& raw = format.u.raw_audio;
	fChannels = raw.channel_count;
	fRate = raw.frame_rate;
	fBufferFrames = raw.buffer_size / (raw.channel_count
		* sample_size(raw.format));
	if (fBufferFrames == 0)
		fBufferFrames = 1;
	_ResetConversion();
	delete[] fFifo;
	fFifo = NULL;
	fFifoCapacity = 0;

	if (fBufferGroup == NULL) {
		fBufferGroup = new BBufferGroup(raw.buffer_size, kBufferCount);
		fOwnBufferGroup = true;
	}
	fConnected = true;
	fDead = false;
	fSendErrors = 0;
	SetBufferDuration((bigtime_t)(fBufferFrames * 1000000.0 / fRate));
}


void
MixerTap::Disconnect(const media_source& what, const media_destination& where)
{
	BAutolock locker(fLock);
	if (what != fOutput.source || where != fOutput.destination)
		return;
	fConnected = false;
	fOutput.destination = media_destination::null;
	fOutput.format.u.raw_audio = media_raw_audio_format::wildcard;
	_Specialize(&fOutput.format);
	if (fOwnBufferGroup)
		delete fBufferGroup;
	fBufferGroup = NULL;
	fOwnBufferGroup = false;
	_ResetConversion();
}


void
MixerTap::LateNoticeReceived(const media_source& what, bigtime_t howMuch,
	bigtime_t performanceTime)
{
}


void
MixerTap::EnableOutput(const media_source& what, bool enabled,
	int32* _deprecated)
{
	if (what != fOutput.source)
		return;
	BAutolock locker(fLock);
	fEnabled = enabled;
}


// #pragma mark - BControllable


void
MixerTap::_UpdateWeb()
{
	if (fKind != kLoopback)
		return;

	BParameterWeb* web = new BParameterWeb();
	BParameterGroup* group = web->MakeGroup("Loopback");
	BDiscreteParameter* source = group->MakeDiscreteParameter(
		kSourceParameter, B_MEDIA_RAW_AUDIO, "Record", B_INPUT_MUX);
	source->AddItem(0, "All applications (what you hear)");

	BAutolock locker(fLock);
	for (int32 i = 0; i < fAppNames.CountStrings(); i++)
		source->AddItem(i + 1, fAppNames.StringAt(i).String());
	// A chosen application without a stream now stays selectable.
	if (fSourceSignature.Length() > 0
		&& !fAppSignatures.HasString(fSourceSignature)) {
		source->AddItem(fAppNames.CountStrings() + 1,
			fSourceSignature.String());
	}
	locker.Unlock();

	BTextParameter* signature = group->MakeTextParameter(kSignatureParameter,
		B_MEDIA_RAW_AUDIO, "renga:loopback_signature", B_GENERIC,
		B_MIME_TYPE_LENGTH);
	signature->SetFlags(B_HIDDEN_PARAMETER);

	SetParameterWeb(web);
}


status_t
MixerTap::GetParameterValue(int32 id, bigtime_t* _lastChange, void* value,
	size_t* _size)
{
	BAutolock locker(fLock);
	*_lastChange = fLastChange;
	if (id == kSourceParameter) {
		if (*_size < sizeof(int32))
			return B_BAD_VALUE;
		int32 index = 0;
		if (fSourceSignature.Length() > 0) {
			index = fAppSignatures.IndexOf(fSourceSignature);
			index = index >= 0 ? index + 1 : fAppNames.CountStrings() + 1;
		}
		*(int32*)value = index;
		*_size = sizeof(int32);
		return B_OK;
	}
	if (id == kSignatureParameter) {
		size_t length = fSourceSignature.Length() + 1;
		if (*_size < length)
			return B_BAD_VALUE;
		memcpy(value, fSourceSignature.String(), length);
		*_size = length;
		return B_OK;
	}
	return B_BAD_VALUE;
}


void
MixerTap::SetParameterValue(int32 id, bigtime_t when, const void* value,
	size_t size)
{
	BString signature;
	if (id == kSourceParameter && size >= sizeof(int32)) {
		int32 index = *(const int32*)value;
		BAutolock locker(fLock);
		if (index == 0)
			signature = "";
		else if (index - 1 < fAppSignatures.CountStrings())
			signature = fAppSignatures.StringAt(index - 1);
		else
			return;
	} else if (id == kSignatureParameter) {
		signature.SetTo((const char*)value, strnlen((const char*)value, size));
	} else
		return;

	SetSourceSignature(signature.String());
	int32 index;
	size_t indexSize = sizeof(index);
	bigtime_t lastChange;
	if (GetParameterValue(kSourceParameter, &lastChange, &index, &indexSize)
			== B_OK) {
		BroadcastNewParameterValue(when, kSourceParameter, &index,
			sizeof(index));
	}
}
