/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 */


#include <CaptureSession.h>

#include <new>
#include <stdio.h>
#include <string.h>

#include <Autolock.h>
#include <Bitmap.h>
#include <Buffer.h>
#include <BufferGroup.h>
#include <MediaDefs.h>
#include <MediaFile.h>
#include <Url.h>
#include <MediaFormats.h>
#include <MediaRecorder.h>
#include <MediaRoster.h>
#include <MediaTrack.h>
#include <TimeSource.h>

#include <vector>

#include <AudioStreamControl.h>
#include <ScreenCapture.h>


namespace BPrivate {


static const float kAudioRate = 48000.0f;
	// what the session writes and hands out; sources are converted
static const bigtime_t kAudioDelay = 250000;
	// audio due "now" lags this much behind, so that buffers that are
	// still on their way are not replaced by silence
static const bigtime_t kAudioBacklog = 1000000;
	// more than this waiting in a source is dropped (a source ahead)


CaptureOptions::CaptureOptions()
	:
	source(SCREEN_CAPTURE_SCREEN),
	windowToken(-1),
	flags(0),
	frameRate(30.0f),
	systemAudio(false),
	microphone(false),
	fileFormat("mkv"),
	videoCodec("mpeg4"),
	audioCodec("pcm"),
		// the default stays PCM; the writer passes codec private data on
		// since F-31 phase 4 (FLAC, Opus)
	permissionTimeout(120000000)
{
}


CaptureSession::Listener::~Listener()
{
}


void
CaptureSession::Listener::VideoFrame(const BBitmap* frame,
	const BRegion& damage, bigtime_t time)
{
}


void
CaptureSession::Listener::AudioData(const int16* samples, size_t frames,
	bigtime_t time)
{
}


/*!	One audio source: a BMediaRecorder connected to the loopback or the
	default input. The recorder's thread converts what arrives into 48 kHz
	stereo int16 and appends it; the video thread takes what is due.
*/
struct CaptureSession::AudioSource {
	BMediaRecorder*		recorder;
	media_node			node;
	bool				ownsNode;	// the loopback: released at the end
	BLocker				lock;
	std::vector<int16>	samples;	// interleaved stereo
	bigtime_t			startTime;	// session start (system_time)
	bool				started;	// first buffer seen
	double				position;	// resampler: input position
	float				last[2];	// last input frame of the previous buffer
	int64				received;

	AudioSource()
		:
		recorder(NULL),
		ownsNode(false),
		lock("capture audio"),
		startTime(0),
		started(false),
		position(0),
		received(0)
	{
		last[0] = last[1] = 0;
	}
};


CaptureSession::CaptureSession()
	:
	fCapture(NULL),
	fBitmap(NULL),
	fListener(NULL),
	fFile(NULL),
	fVideoTrack(NULL),
	fAudioTrack(NULL),
	fSystem(NULL),
	fMicrophone(NULL),
	fThread(-1),
	fRunning(false),
	fQuit(false),
	fStartTime(0),
	fStopTime(0),
	fFramesWritten(0),
	fAudioFramesWritten(0),
	fError(B_OK)
{
}


CaptureSession::~CaptureSession()
{
	Stop();
}


bigtime_t
CaptureSession::Duration() const
{
	if (fStartTime == 0)
		return 0;
	return (fRunning ? system_time() : fStopTime) - fStartTime;
}


static status_t
find_file_format(const char* shortName, media_file_format& format)
{
	int32 cookie = 0;
	while (get_next_file_format(&cookie, &format) == B_OK) {
		if (strcmp(format.short_name, shortName) == 0)
			return B_OK;
	}
	return B_ENTRY_NOT_FOUND;
}


static status_t
find_encoder(const media_file_format& fileFormat, const media_format& input,
	const char* shortName, media_codec_info& info)
{
	int32 cookie = 0;
	media_format output;
	while (get_next_encoder(&cookie, &fileFormat, &input, &output, &info)
			== B_OK) {
		if (strcmp(info.short_name, shortName) == 0)
			return B_OK;
	}
	// The FFmpeg plug-in lists its "Raw audio" (PCM) encoder for WAV files
	// only, but its writer takes PCM in other containers too (Matroska
	// stores it as A_PCM/INT/LIT); take an encoder of that name whatever
	// file family it is listed for.
	cookie = 0;
	while (get_next_encoder(&cookie, &info) == B_OK) {
		if (strcmp(info.short_name, shortName) == 0)
			return B_OK;
	}
	return B_ENTRY_NOT_FOUND;
}


static media_format
video_format(int32 width, int32 height, float frameRate)
{
	media_format format;
	format.type = B_MEDIA_RAW_VIDEO;
	format.u.raw_video = media_raw_video_format::wildcard;
	format.u.raw_video.field_rate = frameRate;
	format.u.raw_video.interlace = 1;
	format.u.raw_video.first_active = 0;
	format.u.raw_video.last_active = height - 1;
	format.u.raw_video.orientation = B_VIDEO_TOP_LEFT_RIGHT;
	format.u.raw_video.pixel_width_aspect = 1;
	format.u.raw_video.pixel_height_aspect = 1;
	format.u.raw_video.display.format = B_RGB32;
	format.u.raw_video.display.line_width = width;
	format.u.raw_video.display.line_count = height;
	format.u.raw_video.display.bytes_per_row = width * 4;
	return format;
}


static media_format
audio_format()
{
	media_format format;
	format.type = B_MEDIA_RAW_AUDIO;
	format.u.raw_audio = media_raw_audio_format::wildcard;
	format.u.raw_audio.frame_rate = kAudioRate;
	format.u.raw_audio.channel_count = 2;
	format.u.raw_audio.format = media_raw_audio_format::B_AUDIO_SHORT;
	format.u.raw_audio.byte_order = B_MEDIA_HOST_ENDIAN;
	format.u.raw_audio.buffer_size = 4096;
	return format;
}


/*static*/ status_t
CaptureSession::CheckEncoders(const CaptureOptions& options,
	BString* description)
{
	media_file_format fileFormat;
	status_t status = find_file_format(options.fileFormat, fileFormat);
	if (status != B_OK)
		return status;
	media_codec_info video;
	media_format raw = video_format(320, 240, options.frameRate);
	status = find_encoder(fileFormat, raw, options.videoCodec, video);
	if (status != B_OK)
		return status;
	media_codec_info audio;
	media_format rawAudio = audio_format();
	status = find_encoder(fileFormat, rawAudio, options.audioCodec, audio);
	if (description != NULL) {
		description->SetToFormat("%s, %s, %s", fileFormat.pretty_name,
			video.pretty_name, status == B_OK ? audio.pretty_name : "no audio");
	}
	return status;
}


status_t
CaptureSession::Start(const CaptureOptions& options, const entry_ref* file,
	Listener* listener)
{
	if (fRunning)
		return B_BUSY;
	fOptions = options;
	if (fOptions.frameRate < 1)
		fOptions.frameRate = 1;
	if (fOptions.frameRate > 60)
		fOptions.frameRate = 60;
	fListener = listener;
	fError = B_OK;
	fFramesWritten = 0;
	fAudioFramesWritten = 0;
	fQuit = false;

	fCapture = new(std::nothrow) BScreenCapture;
	if (fCapture == NULL)
		return B_NO_MEMORY;
	status_t status;
	switch (fOptions.source) {
		case SCREEN_CAPTURE_WINDOW:
			status = fCapture->StartWindow(fOptions.windowToken,
				fOptions.flags, fOptions.permissionTimeout);
			break;
		case SCREEN_CAPTURE_REGION:
			status = fCapture->StartRegion(fOptions.region, fOptions.flags,
				fOptions.permissionTimeout);
			break;
		default:
			status = fCapture->StartScreen(fOptions.flags,
				fOptions.permissionTimeout);
			break;
	}
	if (status != B_OK) {
		delete fCapture;
		fCapture = NULL;
		return status;
	}

	// Encoders want even sizes: the last odd row or column is left out.
	fFrame = fCapture->Frame();
	int32 width = (fFrame.IntegerWidth() + 1) & ~1;
	int32 height = (fFrame.IntegerHeight() + 1) & ~1;
	if (width < 2 || height < 2) {
		Stop();
		return B_BAD_VALUE;
	}
	fFrame.right = fFrame.left + width - 1;
	fFrame.bottom = fFrame.top + height - 1;
	fBitmap = new(std::nothrow) BBitmap(BRect(0, 0, width - 1, height - 1),
		B_RGB32);
	if (fBitmap == NULL || !fBitmap->IsValid()) {
		Stop();
		return B_NO_MEMORY;
	}
	memset(fBitmap->Bits(), 0, fBitmap->BitsLength());

	if (file != NULL || fOptions.streamURL.Length() > 0) {
		status = _OpenFile(file);
		if (status != B_OK) {
			fprintf(stderr, "CaptureSession: cannot open the file: %s\n",
				strerror(status));
			Stop();
			return status;
		}
	}

	fStartTime = system_time();
	if (fOptions.systemAudio) {
		fSystem = new(std::nothrow) AudioSource;
		if (fSystem != NULL && _StartAudio(fSystem, false) != B_OK) {
			delete fSystem;
			fSystem = NULL;
		}
	}
	if (fOptions.microphone) {
		fMicrophone = new(std::nothrow) AudioSource;
		if (fMicrophone != NULL && _StartAudio(fMicrophone, true) != B_OK) {
			delete fMicrophone;
			fMicrophone = NULL;
		}
	}

	fRunning = true;
	fThread = spawn_thread(_VideoThreadEntry, "capture session",
		B_DISPLAY_PRIORITY, this);
	if (fThread < 0) {
		fRunning = false;
		Stop();
		return fThread;
	}
	resume_thread(fThread);
	return B_OK;
}


status_t
CaptureSession::_OpenFile(const entry_ref* file)
{
	media_file_format fileFormat;
	status_t status = find_file_format(fOptions.fileFormat, fileFormat);
	if (status != B_OK)
		return status;

	if (file == NULL) {
		// Renga (F-31 phase 4): a stream (FFmpeg's protocols, see
		// src/add-ons/media/plugins/ffmpeg/FFmpegURLIO.h)
		BUrl url(fOptions.streamURL.String(), true);
		if (!url.IsValid())
			return B_BAD_VALUE;
		fFile = new(std::nothrow) BMediaFile(url, &fileFormat);
	} else
		fFile = new(std::nothrow) BMediaFile(file, &fileFormat);
	if (fFile == NULL)
		return B_NO_MEMORY;
	status = fFile->InitCheck();
	if (status != B_OK) {
		fprintf(stderr, "CaptureSession: cannot write %s: %s\n",
			fileFormat.short_name, strerror(status));
		return status;
	}

	media_format raw = video_format(fBitmap->Bounds().IntegerWidth() + 1,
		fBitmap->Bounds().IntegerHeight() + 1, fOptions.frameRate);
	media_codec_info videoCodec;
	status = find_encoder(fileFormat, raw, fOptions.videoCodec, videoCodec);
	if (status != B_OK)
		return status;
	fVideoTrack = fFile->CreateTrack(&raw, &videoCodec);
	if (fVideoTrack != NULL && fVideoTrack->InitCheck() != B_OK) {
		fprintf(stderr, "CaptureSession: video track: %s\n",
			strerror(fVideoTrack->InitCheck()));
		return fVideoTrack->InitCheck();
	}
	if (fVideoTrack == NULL) {
		fprintf(stderr, "CaptureSession: no %s video track for %" B_PRIu32
			"x%" B_PRIu32 "\n", videoCodec.short_name,
			raw.u.raw_video.display.line_width,
			raw.u.raw_video.display.line_count);
		return B_ERROR;
	}
	fVideoTrack->SetQuality(0.9f);

	if (fOptions.systemAudio || fOptions.microphone) {
		media_format rawAudio = audio_format();
		media_codec_info audioCodec;
		status = find_encoder(fileFormat, rawAudio, fOptions.audioCodec,
			audioCodec);
		if (status != B_OK)
			return status;
		fAudioTrack = fFile->CreateTrack(&rawAudio, &audioCodec);
		if (fAudioTrack != NULL && fAudioTrack->InitCheck() != B_OK) {
			fprintf(stderr, "CaptureSession: audio track: %s\n",
				strerror(fAudioTrack->InitCheck()));
			return fAudioTrack->InitCheck();
		}
		if (fAudioTrack == NULL) {
			fprintf(stderr, "CaptureSession: no %s audio track\n",
				audioCodec.short_name);
			return B_ERROR;
		}
	}

	status = fFile->CommitHeader();
	if (status != B_OK) {
		fprintf(stderr, "CaptureSession: writing the %s header failed: %s\n",
			fileFormat.short_name, strerror(status));
	}
	return status;
}


status_t
CaptureSession::_StartAudio(AudioSource* source, bool microphone)
{
	BMediaRoster* roster = BMediaRoster::Roster();
	status_t status;
	if (microphone) {
		status = roster->GetAudioInput(&source->node);
	} else {
		status = AudioStreamControl::StartLoopback(
			fOptions.audioApplication.Length() > 0
				? fOptions.audioApplication.String() : NULL, &source->node);
		source->ownsNode = status == B_OK;
	}
	if (status != B_OK)
		return status;

	media_output output;
	int32 count;
	status = roster->GetFreeOutputsFor(source->node, &output, 1, &count,
		B_MEDIA_RAW_AUDIO);
	if (status == B_OK && count < 1)
		status = B_BUSY;
	if (status == B_OK) {
		source->recorder = new(std::nothrow) BMediaRecorder(
			microphone ? "Capture session (microphone)"
				: "Capture session (system audio)", B_MEDIA_RAW_AUDIO);
		if (source->recorder == NULL)
			status = B_NO_MEMORY;
		else
			status = source->recorder->InitCheck();
	}
	if (status == B_OK) {
		source->startTime = fStartTime;
		source->recorder->SetHooks(_RecordHook, NULL, source);
		media_format format = output.format;
		status = source->recorder->Connect(source->node, &output, &format);
		if (status == B_OK)
			status = source->recorder->Start();
	}
	if (status != B_OK)
		_StopAudio(source);
	return status;
}


void
CaptureSession::_StopAudio(AudioSource* source)
{
	if (source == NULL)
		return;
	if (source->recorder != NULL) {
		source->recorder->Stop();
		source->recorder->Disconnect();
		delete source->recorder;
		source->recorder = NULL;
	}
	if (source->ownsNode) {
		BMediaRoster::Roster()->ReleaseNode(source->node);
		source->ownsNode = false;
	}
}


/*!	BMediaRecorder hook: converts a buffer to 48 kHz stereo int16 (linear
	interpolation, as the mixer's taps do) and appends it. The first
	buffer is placed in time: silence for the time between the session's
	start and when that buffer began.
*/
/*static*/ void
CaptureSession::_RecordHook(void* cookie, bigtime_t timestamp, void* data,
	size_t size, const media_format& format)
{
	AudioSource* source = (AudioSource*)cookie;
	const media_raw_audio_format& raw = format.u.raw_audio;
	size_t sampleSize = raw.format & media_raw_audio_format::B_AUDIO_SIZE_MASK;
	if (sampleSize == 0 || raw.channel_count == 0 || raw.frame_rate <= 0)
		return;
	size_t frames = size / (sampleSize * raw.channel_count);
	if (frames == 0)
		return;

	BAutolock locker(source->lock);
	if (!source->started) {
		source->started = true;
		bigtime_t began = system_time()
			- (bigtime_t)(frames * 1000000.0 / raw.frame_rate);
		if (began > source->startTime) {
			size_t silence = (size_t)((began - source->startTime)
				* kAudioRate / 1000000.0);
			source->samples.insert(source->samples.end(), silence * 2, 0);
		}
	}

	double step = raw.frame_rate / kAudioRate;
	while (source->position < frames) {
		int32 index = (int32)source->position;
		float fraction = source->position - index;
		for (int32 c = 0; c < 2; c++) {
			uint32 channel = (uint32)c < raw.channel_count ? c : 0;
			float values[2];
			for (int32 k = 0; k < 2; k++) {
				int32 frame = index - 1 + k;
				if (frame < 0) {
					values[k] = source->last[c];
					continue;
				}
				size_t i = frame * raw.channel_count + channel;
				switch (raw.format) {
					case media_raw_audio_format::B_AUDIO_FLOAT:
						values[k] = ((float*)data)[i];
						break;
					case media_raw_audio_format::B_AUDIO_SHORT:
						values[k] = ((int16*)data)[i] / 32768.0f;
						break;
					case media_raw_audio_format::B_AUDIO_INT:
						values[k] = ((int32*)data)[i] / 2147483648.0f;
						break;
					case media_raw_audio_format::B_AUDIO_UCHAR:
						values[k] = (((uint8*)data)[i] - 128) / 128.0f;
						break;
					default:
						values[k] = 0;
						break;
				}
			}
			float value = values[0] + (values[1] - values[0]) * fraction;
			if (value > 1.0f)
				value = 1.0f;
			if (value < -1.0f)
				value = -1.0f;
			source->samples.push_back((int16)(value * 32767.0f));
		}
		source->position += step;
	}
	source->position -= frames;
	for (int32 c = 0; c < 2; c++) {
		uint32 channel = (uint32)c < raw.channel_count ? c : 0;
		size_t i = (frames - 1) * raw.channel_count + channel;
		switch (raw.format) {
			case media_raw_audio_format::B_AUDIO_FLOAT:
				source->last[c] = ((float*)data)[i];
				break;
			case media_raw_audio_format::B_AUDIO_SHORT:
				source->last[c] = ((int16*)data)[i] / 32768.0f;
				break;
			case media_raw_audio_format::B_AUDIO_INT:
				source->last[c] = ((int32*)data)[i] / 2147483648.0f;
				break;
			default:
				source->last[c] = 0;
				break;
		}
	}
	source->received += frames;

	// A source that runs ahead of the clock must not grow without bound.
	size_t limit = (size_t)(kAudioBacklog * kAudioRate / 1000000.0) * 2;
	if (source->samples.size() > limit * 2) {
		source->samples.erase(source->samples.begin(),
			source->samples.begin() + (source->samples.size() - limit));
	}
}


/*!	Writes the audio due by now (or, at the end, everything up to the stop
	time): system audio and microphone mixed, silence where a source has
	nothing (it started late, or it has a gap). Audio follows the session's
	clock, so it stays in step with the video frames, which do too.
*/
void
CaptureSession::_WriteAudio(bool final)
{
	if (fSystem == NULL && fMicrophone == NULL)
		return;
	bigtime_t until = final ? fStopTime : system_time() - kAudioDelay;
	int64 due = (int64)((until - fStartTime) * kAudioRate / 1000000.0)
		- fAudioFramesWritten;
	if (due <= 0)
		return;

	std::vector<int16> mix(due * 2, 0);
	AudioSource* sources[2] = { fSystem, fMicrophone };
	for (int32 s = 0; s < 2; s++) {
		AudioSource* source = sources[s];
		if (source == NULL)
			continue;
		BAutolock locker(source->lock);
		size_t available = source->samples.size() / 2;
		size_t take = min_c(available, (size_t)due);
		for (size_t i = 0; i < take * 2; i++) {
			int32 value = mix[i] + source->samples[i];
			mix[i] = (int16)max_c(-32768, min_c(32767, value));
		}
		source->samples.erase(source->samples.begin(),
			source->samples.begin() + take * 2);
	}

	bigtime_t time = fStartTime
		+ (bigtime_t)(fAudioFramesWritten * 1000000.0 / kAudioRate);
	if (fListener != NULL)
		fListener->AudioData(mix.data(), due, time);
	if (fAudioTrack != NULL) {
		// In buffers of 4096 frames, as encoders like them.
		for (int64 offset = 0; offset < due; offset += 4096) {
			int64 count = min_c((int64)4096, due - offset);
			status_t status = fAudioTrack->WriteFrames(mix.data() + offset * 2,
				count);
			if (status != B_OK && fError == B_OK) {
				fprintf(stderr, "CaptureSession: writing audio failed: %s\n",
					strerror(status));
				fError = status;
			}
		}
	}
	fAudioFramesWritten += due;
}


/*static*/ status_t
CaptureSession::_VideoThreadEntry(void* data)
{
	((CaptureSession*)data)->_VideoThread();
	return B_OK;
}


/*!	Takes a frame every 1 / frameRate seconds. The file's video has a
	constant frame rate (the encoders count frames), so every period gets a
	frame: an unchanged screen repeats the last one (nothing is copied, the
	encoder makes it cheap), and periods the thread missed (it was late,
	the encoder was slow) repeat the frame taken after them, so that the
	video keeps its place against the audio.
*/
void
CaptureSession::_VideoThread()
{
	bigtime_t period = (bigtime_t)(1000000.0 / fOptions.frameRate);
	int64 index = 0;
	while (!fQuit) {
		bigtime_t due = fStartTime + index * period;
		bigtime_t now = system_time();
		if (due > now) {
			snooze_until(due, B_SYSTEM_TIMEBASE);
			continue;
		}

		bool changed = false;
		BRegion damage;
		bigtime_t time = now;
		status_t status = fCapture->NextFrame(fBitmap, &changed, &damage,
			&time);
		if (status != B_OK) {
			// Stopped by the user, or the window went away: end here.
			if (fError == B_OK)
				fError = status;
			break;
		}
		if (!changed)
			damage.MakeEmpty();

		// All periods up to now get this frame.
		int64 target = (system_time() - fStartTime) / period + 1;
		if (target <= index)
			target = index + 1;
		while (index < target) {
			if (fListener != NULL) {
				fListener->VideoFrame(fBitmap, damage, time);
				damage.MakeEmpty();
			}
			if (fVideoTrack != NULL) {
				status = fVideoTrack->WriteFrames(fBitmap->Bits(), 1,
					index == 0 ? B_MEDIA_KEY_FRAME : 0);
				if (status != B_OK && fError == B_OK) {
					fprintf(stderr, "CaptureSession: writing video frame %"
						B_PRId64 " failed: %s\n", index, strerror(status));
					fError = status;
				}
			}
			index++;
			fFramesWritten = index;
		}
		_WriteAudio(false);
	}
	fStopTime = fStartTime + index * period;
}


status_t
CaptureSession::Stop()
{
	if (fThread >= 0) {
		fQuit = true;
		status_t result;
		wait_for_thread(fThread, &result);
		fThread = -1;
	}
	if (fStopTime == 0 && fStartTime != 0)
		fStopTime = system_time();

	_StopAudio(fSystem);
	_StopAudio(fMicrophone);
	if (fRunning)
		_WriteAudio(true);
	fRunning = false;
	delete fSystem;
	fSystem = NULL;
	delete fMicrophone;
	fMicrophone = NULL;

	status_t status = B_OK;
	if (fFile != NULL) {
		if (fVideoTrack != NULL)
			fFile->ReleaseTrack(fVideoTrack);
		if (fAudioTrack != NULL)
			fFile->ReleaseTrack(fAudioTrack);
		fVideoTrack = NULL;
		fAudioTrack = NULL;
		status = fFile->CloseFile();
		delete fFile;
		fFile = NULL;
	}
	if (fCapture != NULL) {
		fCapture->Stop();
		delete fCapture;
		fCapture = NULL;
	}
	delete fBitmap;
	fBitmap = NULL;
	return fError != B_OK && fError != B_CANCELED ? fError : status;
}


// #pragma mark - ScreenCaptureNode


ScreenCaptureNode::ScreenCaptureNode(const char* name,
	const CaptureOptions& options)
	:
	BMediaNode(name),
	BMediaEventLooper(),
	BBufferProducer(B_MEDIA_RAW_VIDEO),
	fInitStatus(B_NO_INIT),
	fOptions(options),
	fCapture(NULL),
	fBitmap(NULL),
	fBufferGroup(NULL),
	fLock("screen capture node"),
	fThread(-1),
	fQuit(false),
	fConnected(false),
	fEnabled(true),
	fStartPerformanceTime(0),
	fFrame(0)
{
	if (fOptions.frameRate < 1)
		fOptions.frameRate = 1;
	if (fOptions.frameRate > 60)
		fOptions.frameRate = 60;

	fCapture = new(std::nothrow) BScreenCapture;
	if (fCapture == NULL) {
		fInitStatus = B_NO_MEMORY;
		return;
	}
	switch (fOptions.source) {
		case SCREEN_CAPTURE_WINDOW:
			fInitStatus = fCapture->StartWindow(fOptions.windowToken,
				fOptions.flags, fOptions.permissionTimeout);
			break;
		case SCREEN_CAPTURE_REGION:
			fInitStatus = fCapture->StartRegion(fOptions.region,
				fOptions.flags, fOptions.permissionTimeout);
			break;
		default:
			fInitStatus = fCapture->StartScreen(fOptions.flags,
				fOptions.permissionTimeout);
			break;
	}
	if (fInitStatus != B_OK)
		return;

	BRect frame = fCapture->Frame();
	int32 width = frame.IntegerWidth() + 1;
	int32 height = frame.IntegerHeight() + 1;
	fBitmap = new(std::nothrow) BBitmap(BRect(0, 0, width - 1, height - 1),
		B_RGB32);
	if (fBitmap == NULL || !fBitmap->IsValid()) {
		fInitStatus = B_NO_MEMORY;
		return;
	}
	fOutput.format = video_format(width, height, fOptions.frameRate);
}


ScreenCaptureNode::~ScreenCaptureNode()
{
	fQuit = true;
	if (fThread >= 0) {
		status_t result;
		wait_for_thread(fThread, &result);
	}
	Quit();
	delete fBufferGroup;
	delete fBitmap;
	delete fCapture;
}


BMediaAddOn*
ScreenCaptureNode::AddOn(int32* _internalID) const
{
	if (_internalID != NULL)
		*_internalID = 0;
	return NULL;
}


status_t
ScreenCaptureNode::HandleMessage(int32 message, const void* data, size_t size)
{
	return B_ERROR;
}


void
ScreenCaptureNode::NodeRegistered()
{
	fOutput.node = Node();
	fOutput.source.port = ControlPort();
	fOutput.source.id = 0;
	fOutput.destination = media_destination::null;
	strlcpy(fOutput.name, Name(), sizeof(fOutput.name));
	if (fInitStatus != B_OK)
		ReportError(B_NODE_IN_DISTRESS);
	Run();
}


void
ScreenCaptureNode::HandleEvent(const media_timed_event* event,
	bigtime_t lateness, bool realTimeEvent)
{
	switch (event->type) {
		case BTimedEventQueue::B_START:
			if (fThread < 0 && fInitStatus == B_OK) {
				fStartPerformanceTime = event->event_time;
				fFrame = 0;
				fQuit = false;
				fThread = spawn_thread(_ThreadEntry, "screen capture node",
					B_DISPLAY_PRIORITY, this);
				if (fThread >= 0)
					resume_thread(fThread);
			}
			break;
		case BTimedEventQueue::B_STOP:
			if (fThread >= 0) {
				fQuit = true;
				status_t result;
				wait_for_thread(fThread, &result);
				fThread = -1;
			}
			break;
		default:
			break;
	}
}


status_t
ScreenCaptureNode::FormatSuggestionRequested(media_type type, int32 quality,
	media_format* format)
{
	if (type != B_MEDIA_RAW_VIDEO && type != B_MEDIA_UNKNOWN_TYPE)
		return B_MEDIA_BAD_FORMAT;
	*format = fOutput.format;
	return B_OK;
}


status_t
ScreenCaptureNode::FormatProposal(const media_source& output,
	media_format* format)
{
	if (output != fOutput.source)
		return B_MEDIA_BAD_SOURCE;
	bool compatible = format_is_compatible(*format, fOutput.format);
	*format = fOutput.format;
	return compatible ? B_OK : B_MEDIA_BAD_FORMAT;
}


status_t
ScreenCaptureNode::FormatChangeRequested(const media_source& source,
	const media_destination& destination, media_format* format,
	int32* _deprecated)
{
	return B_ERROR;
}


status_t
ScreenCaptureNode::GetNextOutput(int32* cookie, media_output* _output)
{
	if (*cookie != 0)
		return B_BAD_INDEX;
	*_output = fOutput;
	(*cookie)++;
	return B_OK;
}


status_t
ScreenCaptureNode::DisposeOutputCookie(int32 cookie)
{
	return B_OK;
}


status_t
ScreenCaptureNode::SetBufferGroup(const media_source& source,
	BBufferGroup* group)
{
	return B_ERROR;
}


status_t
ScreenCaptureNode::GetLatency(bigtime_t* _latency)
{
	*_latency = EventLatency() + SchedulingLatency();
	return B_OK;
}


status_t
ScreenCaptureNode::PrepareToConnect(const media_source& what,
	const media_destination& where, media_format* format,
	media_source* _source, char* _name)
{
	if (what != fOutput.source)
		return B_MEDIA_BAD_SOURCE;
	if (fConnected)
		return B_MEDIA_ALREADY_CONNECTED;
	if (!format_is_compatible(*format, fOutput.format)) {
		*format = fOutput.format;
		return B_MEDIA_BAD_FORMAT;
	}
	*format = fOutput.format;
	*_source = fOutput.source;
	strlcpy(_name, fOutput.name, B_MEDIA_NAME_LENGTH);
	fOutput.destination = where;
	return B_OK;
}


void
ScreenCaptureNode::Connect(status_t error, const media_source& source,
	const media_destination& destination, const media_format& format,
	char* ioName)
{
	if (error != B_OK || source != fOutput.source) {
		fOutput.destination = media_destination::null;
		return;
	}
	BAutolock locker(fLock);
	fOutput.destination = destination;
	strlcpy(ioName, fOutput.name, B_MEDIA_NAME_LENGTH);

	bigtime_t latency = 0;
	media_node_id timeSource;
	FindLatencyFor(fOutput.destination, &latency, &timeSource);
	SetEventLatency(latency + 2000);

	delete fBufferGroup;
	fBufferGroup = new(std::nothrow) BBufferGroup(fBitmap->BitsLength(), 4);
	fConnected = fBufferGroup != NULL && fBufferGroup->InitCheck() == B_OK;
}


void
ScreenCaptureNode::Disconnect(const media_source& what,
	const media_destination& where)
{
	if (what != fOutput.source || where != fOutput.destination)
		return;
	BAutolock locker(fLock);
	fConnected = false;
	fOutput.destination = media_destination::null;
	delete fBufferGroup;
	fBufferGroup = NULL;
}


void
ScreenCaptureNode::LateNoticeReceived(const media_source& what,
	bigtime_t howMuch, bigtime_t performanceTime)
{
}


void
ScreenCaptureNode::EnableOutput(const media_source& what, bool enabled,
	int32* _deprecated)
{
	if (what == fOutput.source)
		fEnabled = enabled;
}


/*static*/ status_t
ScreenCaptureNode::_ThreadEntry(void* data)
{
	((ScreenCaptureNode*)data)->_Thread();
	return B_OK;
}


/*!	One frame per period of the frame rate, in performance time: the
	buffer's start_time is the performance time of the moment the frame
	was taken (app_server's system_time() of the copy, converted with the
	node's time source).
*/
void
ScreenCaptureNode::_Thread()
{
	// Frames are paced in real time from when the node started: the time
	// source may be one that is not running (a sound card nothing plays
	// on) or jumps, and pacing by it made the thread send frames as fast
	// as it could (2311 buffers in 2 s in one run). Each buffer still
	// carries the performance time it was taken at.
	bigtime_t period = (bigtime_t)(1000000.0 / fOptions.frameRate);
	bigtime_t startReal = system_time();
	while (!fQuit) {
		bigtime_t realDue = startReal + fFrame * period;
		bigtime_t now = system_time();
		if (realDue < now - period) {
			// More than a frame late (the thread was held up): skip the
			// missed frames instead of sending them in a burst.
			fFrame = (now - startReal) / period;
			continue;
		}
		if (realDue > now) {
			snooze_until(min_c(realDue, now + 100000), B_SYSTEM_TIMEBASE);
			continue;
		}
		fFrame++;

		bool changed;
		bigtime_t taken = system_time();
		status_t status = fCapture->NextFrame(fBitmap, &changed, NULL,
			&taken);
		if (status != B_OK) {
			ReportError(B_NODE_IN_DISTRESS);
			break;
		}

		BAutolock locker(fLock);
		if (!fConnected || !fEnabled || fBufferGroup == NULL)
			continue;
		BBuffer* buffer = fBufferGroup->RequestBuffer(fBitmap->BitsLength(),
			0);
		if (buffer == NULL)
			continue;
		memcpy(buffer->Data(), fBitmap->Bits(), fBitmap->BitsLength());
		media_header* header = buffer->Header();
		header->type = B_MEDIA_RAW_VIDEO;
		header->size_used = fBitmap->BitsLength();
		header->time_source = TimeSource()->ID();
		header->start_time = TimeSource()->PerformanceTimeFor(taken);
		header->u.raw_video.field_sequence = fFrame;
		header->u.raw_video.field_number = 0;
		header->u.raw_video.pulldown_number = 0;
		header->u.raw_video.first_active_line = 0;
		header->u.raw_video.line_count
			= fOutput.format.u.raw_video.display.line_count;
		if (SendBuffer(buffer, fOutput.source, fOutput.destination) != B_OK)
			buffer->Recycle();
	}
}


}	// namespace BPrivate
