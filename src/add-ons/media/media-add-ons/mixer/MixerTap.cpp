/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 */


#include "MixerTap.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <Autolock.h>
#include <Buffer.h>
#include <BufferGroup.h>
#include <MediaAddOn.h>
#include <MediaRoster.h>
#include <ParameterWeb.h>
#include <TimeSource.h>

#include <MediaRosterEx.h>
	// (its debug macros give way to the mixer's)
#undef ERROR
#undef TRACE
#undef CALLED
#undef PRINT
#undef PRINT_FORMAT
#undef PRINT_INPUT
#undef PRINT_OUTPUT

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
	fSourceTeam(0),
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
	fUnwantedSince(0),
	fDeviceClock(NULL),
	fDriftCount(0),
	fDriftNext(0),
	fDriftRestarts(0),
	fDriftRejected(0),
	fDrift(0),
	fMeasuredDrift(0),
	fDriftValid(false),
	fFramesIn(0),
	fFramesOut(0)
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
	if (fDeviceClock != NULL)
		fDeviceClock->Release();
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
loopback_selection::Excludes(const char* candidate) const
{
	if (exclude[0] == '\0' || candidate == NULL || candidate[0] == '\0')
		return false;
	size_t length = strlen(candidate);
	const char* line = exclude;
	while (*line != '\0') {
		const char* end = strchr(line, '\n');
		if (end == NULL)
			end = line + strlen(line);
		if ((size_t)(end - line) == length
			&& strncasecmp(line, candidate, length) == 0) {
			return true;
		}
		line = *end == '\n' ? end + 1 : end;
	}
	return false;
}


BString
MixerTap::Exclusions()
{
	BAutolock locker(fLock);
	return fExclusions;
}


/*!	Renga (F-58, OBS Studio's O-1): leaves the streams of the listed
	applications out of this loopback. \a list holds signatures separated
	by newlines (a carriage return, spaces and tabs around each are
	ignored, so does an empty line); it replaces the previous list. The
	list is kept normalized, each signature followed by a newline, and cut
	at loopback_selection::kMaxExclude bytes on a line boundary, so the mix
	thread can copy it as it is.
*/
void
MixerTap::SetExclusions(const char* list)
{
	BString normalized;
	if (list != NULL) {
		const char* line = list;
		while (*line != '\0') {
			const char* end = strchr(line, '\n');
			if (end == NULL)
				end = line + strlen(line);
			const char* start = line;
			const char* stop = end;
			while (start < stop && (*start == ' ' || *start == '\t'
					|| *start == '\r'))
				start++;
			while (stop > start && (stop[-1] == ' ' || stop[-1] == '\t'
					|| stop[-1] == '\r'))
				stop--;
			if (stop > start && stop - start < B_MIME_TYPE_LENGTH
				&& normalized.Length() + (stop - start) + 2
					<= loopback_selection::kMaxExclude) {
				normalized.Append(start, stop - start);
				normalized.Append('\n', 1);
			}
			line = *end == '\n' ? end + 1 : end;
		}
	}

	BAutolock locker(fLock);
	fExclusions = normalized;
	fLastChange = system_time();
}


team_id
MixerTap::SourceTeam()
{
	BAutolock locker(fLock);
	return fSourceTeam;
}


void
MixerTap::SetSourceTeam(team_id team)
{
	BAutolock locker(fLock);
	fSourceTeam = team > 0 ? team : 0;
	fLastChange = system_time();
}


bool
MixerTap::GetMixSource(loopback_selection& selection)
{
	if (fLock.LockWithTimeout(0) != B_OK)
		return false;
	selection.team = fSourceTeam;
	strlcpy(selection.signature, fSourceSignature.String(),
		sizeof(selection.signature));
	strlcpy(selection.exclude, fExclusions.String(),
		sizeof(selection.exclude));
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
MixerTap::SetDeviceNode(media_node_id node)
{
	BTimeSource* clock = NULL;
	if (node >= 0) {
		// the device node's own published time, whatever time source it
		// is slaved to
		clock = MediaRosterEx(BMediaRoster::Roster())
			->MakeTimeSourceObject(node);
	}
	BAutolock locker(fLock);
	if (fDeviceClock != NULL)
		fDeviceClock->Release();
	fDeviceClock = clock;
	fDeviceNode = node;
	fDriftValid = false;
	fDriftCount = 0;
	fDriftNext = 0;
	fDrift = 0;
}


/*!	Estimates how fast the device's clock runs against the mixer's: both
	clocks' performance times at the same real time, sampled once a second,
	and the slope of a least-squares line through the last 16 samples. A
	published time source is extrapolated between the device's buffer
	completions, which come in bursts on virtual hardware (several ms of
	jitter): a two-point estimate over two seconds was off by up to
	1000 ppm in QEMU; the fit over 16 s follows the actual difference
	(QEMU's virtual cards wander against each other by about that much). Called by
	the mix thread with fLock held; reading a time source does not block.
	The adaptive resampling follows the estimate, so a sink neither runs
	dry nor piles up on hardware whose cards run from separate crystals.
*/
void
MixerTap::_UpdateDrift(BTimeSource* mixerClock)
{
	if (fDeviceClock == NULL)
		return;
	bigtime_t real = BTimeSource::RealTime();
	bigtime_t device = fDeviceClock->PerformanceTimeFor(real);
	bigtime_t mixer = mixerClock->PerformanceTimeFor(real);
	if (fDriftCount > 0) {
		int32 last = (fDriftNext + kDriftSamples - 1) % kDriftSamples;
		bigtime_t mixerSpan = mixer - fDriftMixer[last];
		if (mixerSpan >= 0 && mixerSpan < 1000000)
			return;
		double slope = mixerSpan > 0
			? (double)(device - fDriftDevice[last]) / mixerSpan : 0;
		if (mixerSpan < 0 || fabs(slope - 1.0) > 0.05) {
			// a jump (a clock restarted, a time base changed): start over
			fDriftRestarts++;
			fDriftCount = 0;
			fDriftNext = 0;
		}
	}
	fDriftMixer[fDriftNext] = mixer;
	fDriftDevice[fDriftNext] = device;
	fDriftNext = (fDriftNext + 1) % kDriftSamples;
	if (fDriftCount < kDriftSamples)
		fDriftCount++;
	if (fDriftCount < 4)
		return;

	// least squares, relative to the oldest sample for precision
	int32 first = (fDriftNext + kDriftSamples - fDriftCount) % kDriftSamples;
	double sumX = 0, sumY = 0, sumXX = 0, sumXY = 0;
	for (int32 i = 0; i < fDriftCount; i++) {
		int32 k = (first + i) % kDriftSamples;
		double x = (double)(fDriftMixer[k] - fDriftMixer[first]);
		double y = (double)(fDriftDevice[k] - fDriftDevice[first]) - x;
		sumX += x;
		sumY += y;
		sumXX += x * x;
		sumXY += x * y;
	}
	double n = fDriftCount;
	double denominator = n * sumXX - sumX * sumX;
	if (denominator <= 0)
		return;
	double measured = (n * sumXY - sumX * sumY) / denominator;
	if (fabs(measured) > 0.02) {
		// more than any crystal is off (virtual cards can be off by
		// 0.8 %, see media_drift_test): a clock misbehaving
		fDriftRejected++;
		return;
	}
	fMeasuredDrift = measured;
	fDrift = measured;
	fDriftValid = true;
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
	bigtime_t startTime, media_node_id timeSource, float gain,
	BTimeSource* mixerClock)
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

	// Renga (F-31): a sink's device has a clock of its own; it plays
	// (1 + drift) of its frames in one second of the mixer's clock, so the
	// mix is resampled to that many.
	if (fKind == kSink && mixerClock != NULL)
		_UpdateDrift(mixerClock);

	// Room for what this push produces plus a few buffers.
	double step = rate / (fRate * (1.0 + fDrift));
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
	fFramesIn += frames;
	fFramesOut += written;

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
		// in the mixer's time: the device plays (1 + drift) frames a second
		fFifoTime += (bigtime_t)(fBufferFrames * 1000000.0
			/ (fRate * (1.0 + fDrift)));

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
	if (fKind == kSink) {
		// Renga (F-31): the clock drift correction, for diagnostics and the
		// harness
		BParameterWeb* web = new BParameterWeb();
		BParameterGroup* group = web->MakeGroup("Sink");
		BTextParameter* drift = group->MakeTextParameter(kDriftParameter,
			B_MEDIA_RAW_AUDIO, "Clock drift", "renga:clock_drift", 96);
		drift->SetFlags(B_HIDDEN_PARAMETER);
		SetParameterWeb(web);
		return;
	}
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

	// Renga (F-58, O-1): applications left out, one signature per line.
	BTextParameter* exclude = group->MakeTextParameter(kExcludeParameter,
		B_MEDIA_RAW_AUDIO, "Excluded applications", "renga:loopback_exclude",
		loopback_selection::kMaxExclude);
	exclude->SetFlags(B_HIDDEN_PARAMETER);

	// Renga (F-58, O-3): one team's streams (helper processes of one
	// executable share its signature).
	BTextParameter* team = group->MakeTextParameter(kTeamParameter,
		B_MEDIA_RAW_AUDIO, "Team", "renga:loopback_team", 16);
	team->SetFlags(B_HIDDEN_PARAMETER);

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
	if (id == kDriftParameter) {
		char text[128];
		snprintf(text, sizeof(text), "%.1f %.1f %" B_PRId64 " %" B_PRId64
			" %" B_PRId32 " %" B_PRId32 " %" B_PRId32, fMeasuredDrift * 1e6,
			fDrift * 1e6, fFramesIn, fFramesOut, fDriftCount, fDriftRestarts,
			fDriftRejected);
		size_t length = strlen(text) + 1;
		if (*_size < length)
			return B_BAD_VALUE;
		memcpy(value, text, length);
		*_size = length;
		return B_OK;
	}
	if (id == kTeamParameter && fKind == kLoopback) {
		char text[16];
		snprintf(text, sizeof(text), "%" B_PRId32, fSourceTeam);
		size_t length = strlen(text) + 1;
		if (*_size < length)
			return B_BAD_VALUE;
		memcpy(value, text, length);
		*_size = length;
		return B_OK;
	}
	if (id == kSignatureParameter || (id == kExcludeParameter
			&& fKind == kLoopback)) {
		const BString& text = id == kSignatureParameter
			? fSourceSignature : fExclusions;
		size_t length = text.Length() + 1;
		if (*_size < length)
			return B_BAD_VALUE;
		memcpy(value, text.String(), length);
		*_size = length;
		return B_OK;
	}
	return B_BAD_VALUE;
}


void
MixerTap::SetParameterValue(int32 id, bigtime_t when, const void* value,
	size_t size)
{
	if (id == kTeamParameter && fKind == kLoopback) {
		BString text((const char*)value, strnlen((const char*)value, size));
		text.Trim();
		char* end = NULL;
		long team = strtol(text.String(), &end, 10);
		if (text.Length() > 0 && (end == NULL || *end != '\0'))
			return;
		SetSourceTeam(text.Length() > 0 ? (team_id)team : 0);
		char reply[16];
		snprintf(reply, sizeof(reply), "%" B_PRId32, SourceTeam());
		BroadcastNewParameterValue(when, kTeamParameter, reply,
			strlen(reply) + 1);
		return;
	}
	if (id == kExcludeParameter && fKind == kLoopback) {
		BString list((const char*)value, strnlen((const char*)value, size));
		SetExclusions(list.String());
		BString normalized = Exclusions();
		BroadcastNewParameterValue(when, kExcludeParameter,
			(void*)normalized.String(), normalized.Length() + 1);
		return;
	}

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
