/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 */


/*!	MediaWriter and MediaReader (headers/private/media/MediaCodec.h): the
	Media Kit's BMediaFile and BMediaTrack behind a small API, roadmap F-31
	phase 4. The codec lookup by short name is the one CaptureSession used
	(get_next_file_format(), get_next_encoder()); decoding negotiates
	B_RGB32 video and float audio with BMediaTrack::DecodedFormat().
*/


#include <MediaCodec.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <Entry.h>
#include <File.h>
#include <List.h>
#include <MediaFile.h>
#include <MediaFormats.h>
#include <MediaTrack.h>
#include <Message.h>
#include <Url.h>


namespace BPrivate {


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
	// The plug-in lists some encoders for one file family only (its "Raw
	// audio" for WAV) although its writer takes them in others too.
	cookie = 0;
	while (get_next_encoder(&cookie, &info) == B_OK) {
		if (strcmp(info.short_name, shortName) == 0)
			return B_OK;
	}
	return B_ENTRY_NOT_FOUND;
}


static media_format
raw_video_format(int32 width, int32 height, float frameRate)
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
raw_audio_format(float frameRate, int32 channels, bool floatSamples)
{
	media_format format;
	format.type = B_MEDIA_RAW_AUDIO;
	format.u.raw_audio = media_raw_audio_format::wildcard;
	format.u.raw_audio.frame_rate = frameRate;
	format.u.raw_audio.channel_count = channels;
	format.u.raw_audio.format = floatSamples
		? media_raw_audio_format::B_AUDIO_FLOAT
		: media_raw_audio_format::B_AUDIO_SHORT;
	format.u.raw_audio.byte_order = B_MEDIA_HOST_ENDIAN;
	format.u.raw_audio.buffer_size = 4096;
	return format;
}


// #pragma mark - MediaWriter


struct MediaWriter::Track {
	BMediaTrack*	track;
	media_format	format;
	int64			frames;
	int32			width;
	int32			height;
	size_t			frameSize;		// audio: bytes per frame
	uint8*			conversion;		// video: rows packed to width * 4
};


MediaWriter::MediaWriter()
	:
	fFile(NULL),
	fStarted(false),
	fFinished(false)
{
}


MediaWriter::~MediaWriter()
{
	Finish();
	for (int32 i = 0; Track* track = _TrackAt(i); i++) {
		free(track->conversion);
		delete track;
	}
	delete fFile;
}


MediaWriter::Track*
MediaWriter::_TrackAt(int32 index) const
{
	return (Track*)fTracks.ItemAt(index);
}


status_t
MediaWriter::Open(const char* path, const char* container)
{
	if (fFile != NULL)
		return B_BUSY;
	media_file_format fileFormat;
	status_t status = find_file_format(container, fileFormat);
	if (status != B_OK)
		return status;

	if (strstr(path, "://") != NULL && strncmp(path, "file://", 7) != 0) {
		// A stream: rtmp://, rtp://, udp://, srt:// (the FFmpeg plug-in's
		// streamer, F-31 phase 4)
		BUrl url(path, true);
		if (!url.IsValid())
			return B_BAD_VALUE;
		fFile = new(std::nothrow) BMediaFile(url, &fileFormat);
	} else {
		if (strncmp(path, "file://", 7) == 0)
			path += 7;
		status = _CreateFile(path, &fileFormat);
		if (status != B_OK)
			return status;
	}
	if (fFile == NULL)
		return B_NO_MEMORY;
	status = fFile->InitCheck();
	if (status != B_OK) {
		delete fFile;
		fFile = NULL;
		return status;
	}
	fContainer = fileFormat.short_name;
	fDescription = fileFormat.pretty_name;
	return B_OK;
}


status_t
MediaWriter::_CreateFile(const char* path, const media_file_format* format)
{
	status_t status;
	// BMediaFile wants an existing entry for an entry_ref.
	BFile file(path, B_CREATE_FILE | B_ERASE_FILE | B_WRITE_ONLY);
	status = file.InitCheck();
	if (status != B_OK)
		return status;
	file.Unset();
	entry_ref ref;
	status = get_ref_for_path(path, &ref);
	if (status != B_OK)
		return status;

	fFile = new(std::nothrow) BMediaFile(&ref, format);
	return B_OK;
}


status_t
MediaWriter::AddVideoTrack(const char* codec, int32 width, int32 height,
	float frameRate, int32* _track, float quality)
{
	if (fFile == NULL)
		return B_NO_INIT;
	if (fStarted)
		return B_NOT_ALLOWED;
	if (width < 2 || height < 2 || frameRate <= 0)
		return B_BAD_VALUE;
	media_file_format fileFormat;
	status_t status = fFile->GetFileFormatInfo(&fileFormat);
	if (status != B_OK)
		return status;

	media_format format = raw_video_format(width, height, frameRate);
	media_codec_info info;
	status = find_encoder(fileFormat, format, codec, info);
	if (status != B_OK)
		return status;
	BMediaTrack* mediaTrack = fFile->CreateTrack(&format, &info);
	if (mediaTrack == NULL)
		return B_ERROR;
	status = mediaTrack->InitCheck();
	if (status != B_OK)
		return status;
	mediaTrack->SetQuality(quality);

	Track* track = new(std::nothrow) Track;
	if (track == NULL)
		return B_NO_MEMORY;
	track->track = mediaTrack;
	track->format = format;
	track->frames = 0;
	track->width = width;
	track->height = height;
	track->frameSize = 0;
	track->conversion = NULL;
	fTracks.AddItem(track);
	if (_track != NULL)
		*_track = fTracks.CountItems() - 1;
	fDescription << ", " << info.pretty_name;
	return B_OK;
}


status_t
MediaWriter::AddAudioTrack(const char* codec, float frameRate,
	int32 channels, int32* _track, bool floatSamples, float quality)
{
	if (fFile == NULL)
		return B_NO_INIT;
	if (fStarted)
		return B_NOT_ALLOWED;
	if (channels < 1 || channels > 8 || frameRate <= 0)
		return B_BAD_VALUE;
	media_file_format fileFormat;
	status_t status = fFile->GetFileFormatInfo(&fileFormat);
	if (status != B_OK)
		return status;

	media_format format = raw_audio_format(frameRate, channels, floatSamples);
	media_codec_info info;
	status = find_encoder(fileFormat, format, codec, info);
	if (status != B_OK)
		return status;
	BMediaTrack* mediaTrack = fFile->CreateTrack(&format, &info);
	if (mediaTrack == NULL)
		return B_ERROR;
	status = mediaTrack->InitCheck();
	if (status != B_OK)
		return status;
	mediaTrack->SetQuality(quality);

	Track* track = new(std::nothrow) Track;
	if (track == NULL)
		return B_NO_MEMORY;
	track->track = mediaTrack;
	track->format = format;
	track->frames = 0;
	track->width = 0;
	track->height = 0;
	track->frameSize = channels * (floatSamples ? sizeof(float)
		: sizeof(int16));
	track->conversion = NULL;
	fTracks.AddItem(track);
	if (_track != NULL)
		*_track = fTracks.CountItems() - 1;
	fDescription << ", " << info.pretty_name;
	return B_OK;
}


status_t
MediaWriter::Start()
{
	if (fFile == NULL)
		return B_NO_INIT;
	if (fStarted)
		return B_OK;
	status_t status = fFile->CommitHeader();
	if (status == B_OK)
		fStarted = true;
	return status;
}


status_t
MediaWriter::WriteVideoFrame(int32 index, const void* bits, int32 bytesPerRow,
	bool keyFrame)
{
	Track* track = _TrackAt(index);
	if (track == NULL || track->format.type != B_MEDIA_RAW_VIDEO)
		return B_BAD_INDEX;
	if (!fStarted || fFinished)
		return B_NOT_ALLOWED;
	const void* data = bits;
	int32 rowBytes = track->width * 4;
	if (bytesPerRow != rowBytes) {
		if (track->conversion == NULL) {
			track->conversion = (uint8*)malloc((size_t)rowBytes
				* track->height);
			if (track->conversion == NULL)
				return B_NO_MEMORY;
		}
		for (int32 y = 0; y < track->height; y++) {
			memcpy(track->conversion + (size_t)y * rowBytes,
				(const uint8*)bits + (size_t)y * bytesPerRow, rowBytes);
		}
		data = track->conversion;
	}
	status_t status = track->track->WriteFrames(data, 1,
		keyFrame ? B_MEDIA_KEY_FRAME : 0);
	if (status == B_OK)
		track->frames++;
	return status;
}


status_t
MediaWriter::WriteAudio(int32 index, const void* samples, int32 frames)
{
	Track* track = _TrackAt(index);
	if (track == NULL || track->format.type != B_MEDIA_RAW_AUDIO)
		return B_BAD_INDEX;
	if (!fStarted || fFinished)
		return B_NOT_ALLOWED;
	// The encoders take chunks of at most their buffer size; hand them
	// pieces of a second's worth or less.
	int32 piece = max_c((int32)1024, (int32)(4096 / track->frameSize));
	const uint8* data = (const uint8*)samples;
	while (frames > 0) {
		int32 count = min_c(frames, piece);
		status_t status = track->track->WriteFrames(data, count);
		if (status != B_OK)
			return status;
		track->frames += count;
		data += count * track->frameSize;
		frames -= count;
	}
	return B_OK;
}


status_t
MediaWriter::Finish()
{
	if (fFile == NULL || fFinished)
		return B_OK;
	fFinished = true;
	status_t status = B_OK;
	if (fStarted) {
		for (int32 i = 0; Track* track = _TrackAt(i); i++)
			track->track->Flush();
		status = fFile->CloseFile();
	}
	return status;
}


int64
MediaWriter::FramesWritten(int32 index) const
{
	Track* track = _TrackAt(index);
	return track != NULL ? track->frames : 0;
}


/*static*/ status_t
MediaWriter::ListCodecs(const char* container, BMessage* list)
{
	if (list == NULL)
		return B_BAD_VALUE;
	list->MakeEmpty();
	if (container == NULL) {
		int32 cookie = 0;
		media_file_format format;
		while (get_next_file_format(&cookie, &format) == B_OK) {
			if ((format.capabilities & media_file_format::B_WRITABLE) == 0)
				continue;
			list->AddString("container", format.short_name);
			list->AddString("container_name", format.pretty_name);
		}
		return B_OK;
	}

	media_file_format fileFormat;
	status_t status = find_file_format(container, fileFormat);
	if (status != B_OK)
		return status;
	media_format inputs[2] = { raw_video_format(320, 240, 30),
		raw_audio_format(48000, 2, false) };
	const char* kinds[2] = { "video", "audio" };
	for (int k = 0; k < 2; k++) {
		int32 cookie = 0;
		media_format output;
		media_codec_info info;
		BString kind(kinds[k]);
		while (get_next_encoder(&cookie, &fileFormat, &inputs[k], &output,
				&info) == B_OK) {
			list->AddString(kind, info.short_name);
			list->AddString(BString(kind) << "_name", info.pretty_name);
		}
	}
	return B_OK;
}


// #pragma mark - MediaReader


MediaReader::MediaReader()
	:
	fFile(NULL),
	fFormats(NULL),
	fAudioBuffer(NULL),
	fAudioBufferFrames(0),
	fAudioBufferUsed(0),
	fAudioBufferRead(0),
	fAudioBufferTime(0),
	fAudioTrack(-1)
{
}


MediaReader::~MediaReader()
{
	Close();
}


void
MediaReader::Close()
{
	if (fFile != NULL) {
		for (int32 i = 0; i < fTracks.CountItems(); i++)
			fFile->ReleaseTrack((BMediaTrack*)fTracks.ItemAt(i));
	}
	fTracks.MakeEmpty();
	delete fFile;
	fFile = NULL;
	delete[] fFormats;
	fFormats = NULL;
	free(fAudioBuffer);
	fAudioBuffer = NULL;
	fAudioBufferFrames = fAudioBufferUsed = fAudioBufferRead = 0;
	fAudioTrack = -1;
}


status_t
MediaReader::Open(const char* pathOrURL)
{
	Close();
	if (pathOrURL == NULL)
		return B_BAD_VALUE;

	if (strstr(pathOrURL, "://") != NULL
		&& strncmp(pathOrURL, "file://", 7) != 0) {
		BUrl url(pathOrURL, true);
		if (!url.IsValid())
			return B_BAD_VALUE;
		fFile = new(std::nothrow) BMediaFile(url);
	} else {
		const char* path = strncmp(pathOrURL, "file://", 7) == 0
			? pathOrURL + 7 : pathOrURL;
		entry_ref ref;
		status_t status = get_ref_for_path(path, &ref);
		if (status != B_OK)
			return status;
		fFile = new(std::nothrow) BMediaFile(&ref);
	}
	if (fFile == NULL)
		return B_NO_MEMORY;
	status_t status = fFile->InitCheck();
	if (status != B_OK) {
		Close();
		return status;
	}
	status = _Prepare();
	if (status != B_OK)
		Close();
	return status;
}


status_t
MediaReader::_Prepare()
{
	int32 count = fFile->CountTracks();
	if (count <= 0)
		return B_MEDIA_NO_HANDLER;
	fFormats = new(std::nothrow) media_format[count];
	if (fFormats == NULL)
		return B_NO_MEMORY;
	for (int32 i = 0; i < count; i++) {
		BMediaTrack* track = fFile->TrackAt(i);
		fTracks.AddItem(track);
		media_format& format = fFormats[i];
		memset((void*)&format, 0, sizeof(format));
		if (track == NULL)
			continue;
		media_format encoded;
		if (track->EncodedFormat(&encoded) != B_OK)
			continue;
		if (encoded.IsVideo()) {
			format.type = B_MEDIA_RAW_VIDEO;
			format.u.raw_video = media_raw_video_format::wildcard;
			format.u.raw_video.display.format = B_RGB32;
			if (track->DecodedFormat(&format) != B_OK)
				format.type = B_MEDIA_UNKNOWN_TYPE;
		} else if (encoded.IsAudio()) {
			format.type = B_MEDIA_RAW_AUDIO;
			format.u.raw_audio = media_raw_audio_format::wildcard;
			format.u.raw_audio.format = media_raw_audio_format::B_AUDIO_FLOAT;
			format.u.raw_audio.byte_order = B_MEDIA_HOST_ENDIAN;
			if (track->DecodedFormat(&format) != B_OK
				|| format.u.raw_audio.format
					!= media_raw_audio_format::B_AUDIO_FLOAT) {
				format.type = B_MEDIA_UNKNOWN_TYPE;
			}
		}
	}
	return B_OK;
}


int32
MediaReader::CountTracks() const
{
	return fTracks.CountItems();
}


status_t
MediaReader::GetTrackInfo(int32 index, MediaTrackInfo* info) const
{
	BMediaTrack* track = (BMediaTrack*)fTracks.ItemAt(index);
	if (track == NULL || info == NULL)
		return B_BAD_INDEX;
	const media_format& format = fFormats[index];
	info->type = format.type;
	media_codec_info codec;
	if (track->GetCodecInfo(&codec) == B_OK) {
		info->codec = codec.pretty_name;
		info->codecShortName = codec.short_name;
	} else {
		info->codec = "";
		info->codecShortName = "";
	}
	info->duration = track->Duration();
	info->frames = track->CountFrames();
	info->width = info->height = info->channels = 0;
	info->frameRate = 0;
	if (format.type == B_MEDIA_RAW_VIDEO) {
		info->width = format.u.raw_video.display.line_width;
		info->height = format.u.raw_video.display.line_count;
		info->frameRate = format.u.raw_video.field_rate;
	} else if (format.type == B_MEDIA_RAW_AUDIO) {
		info->frameRate = format.u.raw_audio.frame_rate;
		info->channels = format.u.raw_audio.channel_count;
	}
	return B_OK;
}


int32
MediaReader::FindTrack(media_type type) const
{
	for (int32 i = 0; i < fTracks.CountItems(); i++) {
		if (fFormats[i].type == type)
			return i;
	}
	return -1;
}


status_t
MediaReader::ReadVideoFrame(int32 index, void* bits, int32 bytesPerRow,
	bigtime_t* _time)
{
	BMediaTrack* track = (BMediaTrack*)fTracks.ItemAt(index);
	if (track == NULL || fFormats[index].type != B_MEDIA_RAW_VIDEO)
		return B_BAD_INDEX;
	const media_raw_video_format& video = fFormats[index].u.raw_video;
	int32 rowBytes = video.display.bytes_per_row;
	int32 height = video.display.line_count;
	if (rowBytes <= 0 || height <= 0)
		return B_BAD_DATA;

	uint8* target = (uint8*)bits;
	uint8* buffer = NULL;
	if (bytesPerRow != rowBytes) {
		buffer = (uint8*)malloc((size_t)rowBytes * height);
		if (buffer == NULL)
			return B_NO_MEMORY;
		target = buffer;
	}
	int64 frames = 0;
	media_header header;
	status_t status = track->ReadFrames(target, &frames, &header);
	if (status == B_OK && buffer != NULL) {
		int32 copy = min_c(rowBytes, bytesPerRow);
		for (int32 y = 0; y < height; y++) {
			memcpy((uint8*)bits + (size_t)y * bytesPerRow,
				buffer + (size_t)y * rowBytes, copy);
		}
	}
	free(buffer);
	if (status == B_OK && _time != NULL)
		*_time = header.start_time;
	return status;
}


status_t
MediaReader::ReadAudio(int32 index, float* samples, int32 maxFrames,
	int32* _frames, bigtime_t* _time)
{
	BMediaTrack* track = (BMediaTrack*)fTracks.ItemAt(index);
	if (track == NULL || fFormats[index].type != B_MEDIA_RAW_AUDIO
		|| samples == NULL || maxFrames <= 0)
		return B_BAD_INDEX;
	const media_raw_audio_format& audio = fFormats[index].u.raw_audio;
	int32 channels = audio.channel_count;

	if (fAudioTrack != index) {
		fAudioTrack = index;
		fAudioBufferUsed = fAudioBufferRead = 0;
	}
	if (fAudioBufferRead >= fAudioBufferUsed) {
		// ReadFrames() fills a buffer of the decoded format's size.
		size_t size = audio.buffer_size > 0 ? audio.buffer_size
			: 4096 * channels * sizeof(float);
		int64 capacity = size / (channels * sizeof(float));
		if (capacity > fAudioBufferFrames) {
			free(fAudioBuffer);
			// Some decoders hand back more than one buffer's worth.
			fAudioBuffer = (float*)malloc(size * 4);
			if (fAudioBuffer == NULL) {
				fAudioBufferFrames = 0;
				return B_NO_MEMORY;
			}
			fAudioBufferFrames = capacity;
		}
		int64 frames = 0;
		media_header header;
		status_t status = track->ReadFrames(fAudioBuffer, &frames, &header);
		if (status != B_OK)
			return status;
		fAudioBufferUsed = frames;
		fAudioBufferRead = 0;
		fAudioBufferTime = header.start_time;
	}
	int64 count = min_c((int64)maxFrames, fAudioBufferUsed - fAudioBufferRead);
	memcpy(samples, fAudioBuffer + fAudioBufferRead * channels,
		count * channels * sizeof(float));
	if (_time != NULL) {
		*_time = fAudioBufferTime + (bigtime_t)(fAudioBufferRead * 1000000.0
			/ audio.frame_rate);
	}
	fAudioBufferRead += count;
	*_frames = (int32)count;
	return B_OK;
}


status_t
MediaReader::Seek(bigtime_t time)
{
	status_t result = B_OK;
	for (int32 i = 0; i < fTracks.CountItems(); i++) {
		BMediaTrack* track = (BMediaTrack*)fTracks.ItemAt(i);
		if (track == NULL)
			continue;
		bigtime_t when = time;
		status_t status = track->SeekToTime(&when,
			B_MEDIA_SEEK_CLOSEST_BACKWARD);
		if (status != B_OK)
			result = status;
	}
	fAudioBufferUsed = fAudioBufferRead = 0;
	return result;
}


}	// namespace BPrivate
