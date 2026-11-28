/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 */

/*!	The encode/decode API (roadmap F-31 phase 4, headers/private/media/
	MediaCodec.h) and the FFmpeg plug-in's encoders behind it. For each
	container and codec pair, MediaWriter writes two seconds of synthetic
	video (160x120 at 15 fps: red, green, blue and white thirds of the
	time, a frame counter in the top left corner's grey level) and audio
	(440 Hz left, 660 Hz right), and MediaReader decodes the file again:
	the tracks, their codecs and sizes, the frame count, the colours in
	order, the audio length and its tones (Goertzel), and seeking.

	Every video case also checks the frames' times (media_header's
	start_time and BMediaTrack::CurrentTime() after each ReadFrames(): one
	frame duration apart, in microseconds, the last one ending at the
	track's duration), and a Matroska file whose video starts 2 s into the
	stream (libavformat remuxes the WebM clip with the offset) checks that
	the stream's start offset is taken off as it is for audio, in the
	track's Duration() and CountFrames() too. Two clips with B frames
	(libavcodec's MPEG-4 encoder, MP4 and Matroska) check the times with
	pts out of order, and the plug-in's time conversion is checked for a
	1 ns time base.

	Also, when the harness provides them, samples the host's FFmpeg made
	with encoders the plug-in only decodes (AV1, HEVC, H.264) in
	/boot/system/non-packaged/data/ce-harness/media (same picture, 30
	frames).
*/


#include <Application.h>
#include <Directory.h>
#include <Entry.h>
#include <File.h>
#include <Message.h>
#include <OS.h>
#include <Path.h>
#include <String.h>

#include <MediaCodec.h>
#include <MediaFile.h>
#include <experimental/MediaIO.h>
#include <MediaTrack.h>

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/mathematics.h>
}

#include "Utilities.h"
	// the FFmpeg plug-in's time conversion (test_time_conversion())


#define SUITE "media_codec"

using BPrivate::MediaReader;
using BPrivate::MediaTrackInfo;
using BPrivate::MediaWriter;

static const int32 kWidth = 160;
static const int32 kHeight = 120;
static const float kFrameRate = 15;
static const int32 kFrames = 30;
static const float kRate = 48000;
static const double kSeconds = 2.0;
static const char* kSamples = "/boot/system/non-packaged/data/ce-harness/media";

static int sFailures = 0;
static bool sKnownFailure = false;


static void
report(const char* name, bool pass, const char* format, ...)
{
	char detail[400];
	va_list args;
	va_start(args, format);
	vsnprintf(detail, sizeof(detail), format, args);
	va_end(args);
	if (sKnownFailure) {
		// a combination that does not work yet (see the roadmap, F-31)
		printf("CE-INFO %s known limit: %s %s %s\n", SUITE, name,
			pass ? "works" : "fails", detail);
		fflush(stdout);
		return;
	}
	printf("CE-TEST %s.%s %s %s\n", SUITE, name, pass ? "PASS" : "FAIL",
		detail);
	fflush(stdout);
	if (!pass)
		sFailures++;
}


static void
info(const char* format, ...)
{
	char text[400];
	va_list args;
	va_start(args, format);
	vsnprintf(text, sizeof(text), format, args);
	va_end(args);
	printf("CE-INFO " SUITE ": %s\n", text);
	fflush(stdout);
}


//!	The colour of frame \a index: red, green, blue, white in quarters.
static uint32
frame_color(int32 index)
{
	static const uint32 kColors[4] = { 0xffd02020, 0xff20d020, 0xff2020d0,
		0xffe0e0e0 };
	return kColors[(index * 4 / kFrames) % 4];
}


static void
make_frame(int32 index, uint32* bits)
{
	uint32 color = frame_color(index);
	for (int32 i = 0; i < kWidth * kHeight; i++)
		bits[i] = color;
}


static double
goertzel(const std::vector<float>& samples, size_t start, size_t count,
	double hertz)
{
	double coefficient = 2 * cos(2 * M_PI * hertz / kRate);
	double s1 = 0, s2 = 0;
	for (size_t i = start; i < start + count && i < samples.size(); i++) {
		double s0 = samples[i] + coefficient * s1 - s2;
		s2 = s1;
		s1 = s0;
	}
	return s1 * s1 + s2 * s2 - coefficient * s1 * s2;
}


//!	The share of a channel's energy in \a hertz over a window.
static double
purity(const std::vector<float>& samples, double hertz)
{
	size_t window = 4800;
	size_t start = samples.size() > 3 * window ? samples.size() / 2 - window
		: 0;
	double energy = 0;
	for (size_t i = start; i < start + window && i < samples.size(); i++)
		energy += samples[i] * samples[i];
	if (energy <= 0)
		return 0;
	return 2 * goertzel(samples, start, window, hertz) / (window * energy);
}


static bool
color_near(uint32 a, uint32 b, int tolerance)
{
	for (int shift = 0; shift < 24; shift += 8) {
		int ca = (a >> shift) & 0xff;
		int cb = (b >> shift) & 0xff;
		if (abs(ca - cb) > tolerance)
			return false;
	}
	return true;
}


/*!	The times of the video frames of \a path through BMediaTrack: frame i
	starts at i / kFrameRate s (+ half a frame), CurrentTime() after reading
	it is a frame later, and the last frame ends at the track's Duration().
	The FFmpeg plug-in used to report the stream's own time base here (an
	MP4 at 1/15: 0, 1, 2, ...; WebM at 1/1000: 0, 66, 133, ...).
	The duration ends at the last frame's start or at its end (containers
	do either). Also the track's CountFrames() (<name>_frame_count): never
	below the real count, as callers stop there (MediaConverter), at most
	one above. For a Matroska file, whose streams have no duration of their
	own, AVFormatReader falls back to the file's duration: it did not take
	the stream's start offset off (a stream starting at 2 s was 2 s too
	long), and the frame count derived from it was a million times too high
	(microseconds times the frame rate), and then, rounded, one low.
*/
static void
check_video_times(const char* name, const char* path)
{
	entry_ref ref;
	status_t status = get_ref_for_path(path, &ref);
	BMediaFile* file = NULL;
	BMediaTrack* track = NULL;
	if (status == B_OK) {
		file = new BMediaFile(&ref);
		status = file->InitCheck();
	}
	for (int32 i = 0; status == B_OK && i < file->CountTracks(); i++) {
		BMediaTrack* candidate = file->TrackAt(i);
		media_format format;
		if (candidate != NULL && candidate->EncodedFormat(&format) == B_OK
			&& format.type == B_MEDIA_ENCODED_VIDEO) {
			track = candidate;
			break;
		}
		file->ReleaseTrack(candidate);
	}
	media_format format;
	if (status == B_OK && track == NULL)
		status = B_ENTRY_NOT_FOUND;
	if (status == B_OK) {
		format.Clear();
		format.type = B_MEDIA_RAW_VIDEO;
		format.u.raw_video = media_raw_video_format::wildcard;
		format.u.raw_video.display.format = B_RGB32;
		status = track->DecodedFormat(&format);
	}
	const bigtime_t step = bigtime_t(1000000 / kFrameRate);
	int32 frames = 0, badStart = 0, badCurrent = 0;
	bigtime_t last = -1;
	BString times;
	if (status == B_OK) {
		size_t size = (size_t)format.u.raw_video.display.bytes_per_row
			* format.u.raw_video.display.line_count;
		std::vector<uint8> buffer(size > 0 ? size : 1);
		while (frames < kFrames * 2) {
			int64 count = 0;
			media_header header;
			if (track->ReadFrames(buffer.data(), &count, &header) != B_OK
				|| count <= 0) {
				break;
			}
			bigtime_t want = frames * step;
			if (llabs(header.start_time - want) > step / 2)
				badStart++;
			if (llabs(track->CurrentTime() - (want + step)) > step / 2)
				badCurrent++;
			if (frames < 4 || frames == kFrames - 1) {
				times << " " << frames << ":" << header.start_time << "/"
					<< track->CurrentTime();
			}
			last = header.start_time;
			frames++;
		}
	}
	bigtime_t duration = track != NULL ? track->Duration() : 0;
	report(BString(name) << "_video_times", status == B_OK
		&& frames == kFrames && badStart == 0 && badCurrent == 0
		&& duration >= last && duration <= last + step + 1000,
		"%s: %" B_PRId32 " frames, %" B_PRId32 " start times and %" B_PRId32
		" CurrentTime()s off by over half a frame (%" B_PRIdBIGTIME
		" us); last frame at %" B_PRIdBIGTIME " us, duration %" B_PRIdBIGTIME
		" us; frame:start_time/CurrentTime()%s", strerror(status), frames,
		badStart, badCurrent, step, last, duration, times.String());
	int64 frameCount = track != NULL ? track->CountFrames() : 0;
	report(BString(name) << "_frame_count", status == B_OK
		&& frameCount >= kFrames && frameCount <= kFrames + 1,
		"%s: CountFrames() %" B_PRId64 " (want %" B_PRId32 " or one more)",
		strerror(status), frameCount, kFrames);
	if (file != NULL) {
		if (track != NULL)
			file->ReleaseTrack(track);
		delete file;
	}
}


//!	A file as a plain BDataIO: no seeking, no size (a stream, a pipe).
class PlainFileIO : public BDataIO {
public:
	PlainFileIO(const char* path)
		:
		fFile(path, B_READ_ONLY)
	{
	}

	status_t InitCheck() const { return fFile.InitCheck(); }

	virtual ssize_t Read(void* buffer, size_t size)
	{
		return fFile.Read(buffer, size);
	}

	virtual ssize_t Write(const void*, size_t)
	{
		return B_NOT_ALLOWED;
	}

private:
	BFile fFile;
};


/*!	A file as a source whose size is not known yet (B_MEDIA_MUTABLE_SIZE),
	like a download in progress through HTTPMediaIO: seekable, its size
	the part read so far.
*/
class GrowingFileIO : public BMediaIO {
public:
	GrowingFileIO(const char* path)
		:
		fFile(path, B_READ_ONLY)
	{
	}

	status_t InitCheck() const { return fFile.InitCheck(); }

	virtual void GetFlags(int32* flags) const
	{
		*flags = B_MEDIA_SEEKABLE | B_MEDIA_MUTABLE_SIZE;
	}

	virtual ssize_t ReadAt(off_t position, void* buffer, size_t size)
	{
		return fFile.ReadAt(position, buffer, size);
	}

	virtual ssize_t WriteAt(off_t, const void*, size_t)
	{
		return B_NOT_ALLOWED;
	}

	virtual off_t Seek(off_t position, uint32 seekMode)
	{
		return fFile.Seek(position, seekMode);
	}

	virtual off_t Position() const
	{
		return fFile.Position();
	}

	virtual status_t SetSize(off_t)
	{
		return B_NOT_ALLOWED;
	}

	virtual status_t GetSize(off_t* size) const
	{
		return fFile.GetSize(size);
	}

private:
	BFile fFile;
};


/*!	The video track's CountFrames() when \a path is read from a source of
	unknown size (B_MEDIA_MUTABLE_SIZE: HTTPMediaIO without a length, RTSP,
	the adapter for a plain BDataIO): Duration() is 0 there on purpose, but
	the frame count came from Duration() and was 0 as well, so MediaPlayer
	showed a streamed Matroska, WebM or FLV video as a still image. Through
	\a plain, a plain BDataIO (no seeking at all), reported only: the
	adapter for those (PluginManager's DataIOAdapter) cannot open most of
	these files, a limit of its own.
*/
static void
check_unseekable_frame_count(const char* name, const char* path, bool plain)
{
	BDataIO* io;
	status_t status;
	if (plain) {
		PlainFileIO* file = new PlainFileIO(path);
		status = file->InitCheck();
		io = file;
	} else {
		GrowingFileIO* file = new GrowingFileIO(path);
		status = file->InitCheck();
		io = file;
	}
	BMediaFile* file = NULL;
	BMediaTrack* track = NULL;
	if (status == B_OK) {
		file = new BMediaFile(io);
		status = file->InitCheck();
	}
	for (int32 i = 0; status == B_OK && i < file->CountTracks(); i++) {
		BMediaTrack* candidate = file->TrackAt(i);
		media_format format;
		if (candidate != NULL && candidate->EncodedFormat(&format) == B_OK
			&& format.type == B_MEDIA_ENCODED_VIDEO) {
			track = candidate;
			break;
		}
		file->ReleaseTrack(candidate);
	}
	if (status == B_OK && track == NULL)
		status = B_ENTRY_NOT_FOUND;
	int64 frameCount = track != NULL ? track->CountFrames() : 0;
	bigtime_t duration = track != NULL ? track->Duration() : 0;
	if (plain) {
		info("%s: through a plain BDataIO: %s, CountFrames() %" B_PRId64
			", Duration() %" B_PRIdBIGTIME " us", name, strerror(status),
			frameCount, duration);
	} else {
		report(BString(name) << "_unseekable_frame_count", status == B_OK
			&& frameCount >= kFrames && frameCount <= kFrames + 1,
			"%s: from a source of unknown size, CountFrames() %" B_PRId64
			" (want %" B_PRId32 " or one more), Duration() %" B_PRIdBIGTIME
			" us (0: size unknown)", strerror(status), frameCount, kFrames,
			duration);
	}
	if (file != NULL) {
		if (track != NULL)
			file->ReleaseTrack(track);
		delete file;
	}
	delete io;
}


/*!	Copies \a from to the Matroska file \a to with every packet \a offset
	microseconds later: the stream then has a start time of its own.
*/
static status_t
remux_with_offset(const char* from, const char* to, int64 offset)
{
	AVFormatContext* input = NULL;
	if (avformat_open_input(&input, from, NULL, NULL) < 0)
		return B_ERROR;
	status_t status = B_ERROR;
	AVFormatContext* output = NULL;
	if (avformat_find_stream_info(input, NULL) >= 0
		&& avformat_alloc_output_context2(&output, NULL, "matroska", to)
			>= 0) {
		bool ok = true;
		for (unsigned i = 0; i < input->nb_streams && ok; i++) {
			AVStream* stream = avformat_new_stream(output, NULL);
			ok = stream != NULL && avcodec_parameters_copy(stream->codecpar,
				input->streams[i]->codecpar) >= 0;
			if (ok) {
				stream->codecpar->codec_tag = 0;
				stream->time_base = input->streams[i]->time_base;
			}
		}
		ok = ok && avio_open(&output->pb, to, AVIO_FLAG_WRITE) >= 0
			&& avformat_write_header(output, NULL) >= 0;
		AVPacket* packet = av_packet_alloc();
		while (ok && packet != NULL && av_read_frame(input, packet) >= 0) {
			AVRational timeBase
				= input->streams[packet->stream_index]->time_base;
			int64 shift = av_rescale_q(offset, AVRational{ 1, 1000000 },
				timeBase);
			if (packet->pts != AV_NOPTS_VALUE)
				packet->pts += shift;
			if (packet->dts != AV_NOPTS_VALUE)
				packet->dts += shift;
			av_packet_rescale_ts(packet, timeBase,
				output->streams[packet->stream_index]->time_base);
			packet->pos = -1;
			ok = av_interleaved_write_frame(output, packet) >= 0;
			av_packet_unref(packet);
		}
		av_packet_free(&packet);
		if (ok && av_write_trailer(output) >= 0)
			status = B_OK;
		if (output->pb != NULL)
			avio_closep(&output->pb);
		avformat_free_context(output);
	}
	avformat_close_input(&input);
	return status;
}


/*!	Encodes the test picture (kFrames frames at kFrameRate) with
	libavcodec's MPEG-4 encoder and up to two B frames between reference
	frames into \a path, a \a container ("mp4", "matroska") file: the
	packets' pts are then out of order, and in MP4 the first dts is
	negative. The plug-in's encoder is not used, as it adds B frames only
	for MPEG-2, and its defaults stay as they are.
*/
static status_t
encode_with_b_frames(const char* path, const char* container)
{
	const AVCodec* codec = avcodec_find_encoder(AV_CODEC_ID_MPEG4);
	AVFormatContext* output = NULL;
	if (codec == NULL || avformat_alloc_output_context2(&output, NULL,
			container, path) < 0) {
		return B_ERROR;
	}
	AVCodecContext* context = avcodec_alloc_context3(codec);
	AVStream* stream = avformat_new_stream(output, NULL);
	AVFrame* frame = av_frame_alloc();
	AVPacket* packet = av_packet_alloc();
	bool ok = context != NULL && stream != NULL && frame != NULL
		&& packet != NULL;
	if (ok) {
		context->width = kWidth;
		context->height = kHeight;
		context->time_base = AVRational{ 1, (int)kFrameRate };
		context->framerate = AVRational{ (int)kFrameRate, 1 };
		context->pix_fmt = AV_PIX_FMT_YUV420P;
		context->gop_size = 12;
		context->max_b_frames = 2;
		context->bit_rate = 400000;
		if ((output->oformat->flags & AVFMT_GLOBALHEADER) != 0)
			context->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
		ok = avcodec_open2(context, codec, NULL) >= 0
			&& avcodec_parameters_from_context(stream->codecpar, context)
				>= 0;
	}
	bool started = false;
	if (ok) {
		stream->time_base = context->time_base;
		frame->format = context->pix_fmt;
		frame->width = kWidth;
		frame->height = kHeight;
		ok = av_frame_get_buffer(frame, 0) >= 0
			&& avio_open(&output->pb, path, AVIO_FLAG_WRITE) >= 0
			&& avformat_write_header(output, NULL) >= 0;
		started = ok;
	}
	for (int32 i = 0; ok && i <= kFrames; i++) {
		if (i < kFrames) {
			// frame_color() in BT.601 YUV, the whole picture
			ok = av_frame_make_writable(frame) >= 0;
			uint32 color = frame_color(i);
			int r = (color >> 16) & 0xff, g = (color >> 8) & 0xff,
				b = color & 0xff;
			int y = (66 * r + 129 * g + 25 * b + 128) / 256 + 16;
			int u = (-38 * r - 74 * g + 112 * b + 128) / 256 + 128;
			int v = (112 * r - 94 * g - 18 * b + 128) / 256 + 128;
			for (int32 row = 0; ok && row < kHeight; row++) {
				memset(frame->data[0] + row * frame->linesize[0], y, kWidth);
				if (row < kHeight / 2) {
					memset(frame->data[1] + row * frame->linesize[1], u,
						kWidth / 2);
					memset(frame->data[2] + row * frame->linesize[2], v,
						kWidth / 2);
				}
			}
			frame->pts = i;
		}
		// the last round flushes the encoder
		ok = ok && avcodec_send_frame(context, i < kFrames ? frame : NULL)
			>= 0;
		while (ok) {
			int result = avcodec_receive_packet(context, packet);
			if (result == AVERROR(EAGAIN) || result == AVERROR_EOF)
				break;
			if (result < 0) {
				ok = false;
				break;
			}
			av_packet_rescale_ts(packet, context->time_base,
				stream->time_base);
			packet->stream_index = stream->index;
			ok = av_interleaved_write_frame(output, packet) >= 0;
		}
	}
	status_t status = started && ok && av_write_trailer(output) >= 0
		? B_OK : B_ERROR;
	if (output->pb != NULL)
		avio_closep(&output->pb);
	avformat_free_context(output);
	avcodec_free_context(&context);
	av_frame_free(&frame);
	av_packet_free(&packet);
	return status;
}


/*!	Counts the video packets of \a path, as libavformat reads them, whose
	pts is lower than an earlier packet's (\a _reordered), whose pts and
	dts differ (\a _ptsNotDts) and whose dts is negative (\a _negativeDts),
	and the pictures libavcodec decodes from them, drained at the end
	(\a _decoded: what the plug-in should deliver).
*/
static status_t
count_reordered_packets(const char* path, int32* _packets,
	int32* _reordered, int32* _ptsNotDts, int32* _negativeDts,
	int32* _decoded)
{
	*_packets = *_reordered = *_ptsNotDts = *_negativeDts = *_decoded = 0;
	AVFormatContext* input = NULL;
	if (avformat_open_input(&input, path, NULL, NULL) < 0)
		return B_ERROR;
	status_t status = B_ERROR;
	int index = -1;
	if (avformat_find_stream_info(input, NULL) >= 0) {
		index = av_find_best_stream(input, AVMEDIA_TYPE_VIDEO, -1, -1, NULL,
			0);
	}
	AVPacket* packet = av_packet_alloc();
	AVFrame* frame = av_frame_alloc();
	AVCodecContext* decoder = NULL;
	if (index >= 0) {
		const AVCodec* codec = avcodec_find_decoder(
			input->streams[index]->codecpar->codec_id);
		decoder = codec != NULL ? avcodec_alloc_context3(codec) : NULL;
		if (decoder != NULL && (avcodec_parameters_to_context(decoder,
					input->streams[index]->codecpar) < 0
				|| avcodec_open2(decoder, codec, NULL) < 0)) {
			avcodec_free_context(&decoder);
		}
	}
	if (index >= 0 && packet != NULL && frame != NULL) {
		status = B_OK;
		int64 highest = AV_NOPTS_VALUE;
		while (av_read_frame(input, packet) >= 0) {
			if (packet->stream_index == index) {
				(*_packets)++;
				if (decoder != NULL && avcodec_send_packet(decoder, packet)
						>= 0) {
					while (avcodec_receive_frame(decoder, frame) >= 0)
						(*_decoded)++;
				}
				if (packet->pts != AV_NOPTS_VALUE) {
					if (highest != AV_NOPTS_VALUE && packet->pts < highest)
						(*_reordered)++;
					else
						highest = packet->pts;
				}
				if (packet->pts != AV_NOPTS_VALUE
					&& packet->dts != AV_NOPTS_VALUE
					&& packet->pts != packet->dts) {
					(*_ptsNotDts)++;
				}
				if (packet->dts != AV_NOPTS_VALUE && packet->dts < 0)
					(*_negativeDts)++;
			}
			av_packet_unref(packet);
		}
		if (decoder != NULL && avcodec_send_packet(decoder, NULL) >= 0) {
			while (avcodec_receive_frame(decoder, frame) >= 0)
				(*_decoded)++;
		}
	}
	avcodec_free_context(&decoder);
	av_frame_free(&frame);
	av_packet_free(&packet);
	avformat_close_input(&input);
	return status;
}


/*!	B frames (pts out of order; in MP4 pts != dts and a negative first
	dts): the frame times through BMediaTrack in presentation order. The
	other cases' encoders write no B frames, so their pts follow dts.
	\a needDts: the container stores dts (MP4; Matroska stores only pts).
*/
static void
test_b_frames(const char* name, const char* container,
	const char* extension, bool needDts)
{
	BString path("/tmp/media_codec_test.bframes.");
	path << extension;
	unlink(path.String());
	status_t status = encode_with_b_frames(path.String(), container);
	int32 packets = 0, reordered = 0, ptsNotDts = 0, negativeDts = 0,
		decoded = 0;
	if (status == B_OK) {
		status = count_reordered_packets(path.String(), &packets, &reordered,
			&ptsNotDts, &negativeDts, &decoded);
	}
	report(BString(name) << "_reordered", status == B_OK
		&& packets == kFrames && decoded == kFrames && reordered > 0
		&& (!needDts || ptsNotDts > 0),
		"%s: %" B_PRId32 " packets, %" B_PRId32 " with a pts below an "
		"earlier one, %" B_PRId32 " with pts != dts, %" B_PRId32 " with a "
		"negative dts; libavcodec decodes %" B_PRId32 " pictures",
		strerror(status), packets, reordered, ptsNotDts, negativeDts,
		decoded);
	if (status == B_OK)
		check_video_times(name, path.String());
	unlink(path.String());
}


/*!	The plug-in's time conversion (Utilities.h), shared by AVFormatReader
	and AVCodecDecoder: truncating toward zero, the stream start offset
	taken off, and no overflow for a 1 ns time base (3 hours are 1.08e13
	ns; "1000000LL * time * num / den" overflowed after about 2.6 hours).
*/
static void
test_time_conversion()
{
	avpacket_user_data data = {};
	data.time_base_num = 1;
	data.time_base_den = 1000000000;
	data.stream_start_time = 0;
	const int64 threeHours = 3LL * 3600 * 1000000;
	int64 nanoseconds = stream_time_to_microseconds(threeHours * 1000, data);
	int64 truncated = stream_time_to_microseconds(1999, data);
	int64 negative = stream_time_to_microseconds(-1999, data);
	data.time_base_den = 15360;
	data.stream_start_time = 2 * 15360;
	int64 offset = stream_time_to_microseconds(2 * 15360 + 1024, data);
	int64 none = stream_time_to_microseconds(AV_NOPTS_VALUE, data);
	report("stream_time_long_time_base", nanoseconds == threeHours
		&& truncated == 1 && negative == -1 && offset == 66666
		&& none == AV_NOPTS_VALUE,
		"3 h at 1/1000000000: %" B_PRId64 " us (want %" B_PRId64 "), "
		"1999 ns: %" B_PRId64 ", -1999 ns: %" B_PRId64 " (want 1, -1); "
		"1024 at 1/15360 after a 2 s start: %" B_PRId64 " (want 66666); "
		"AV_NOPTS_VALUE %s", nanoseconds, threeHours, truncated, negative,
		offset, none == AV_NOPTS_VALUE ? "kept" : "changed");

	// Back again (seeks): a reported time converts to the stream time it
	// came from, or for a time base finer than a microsecond to the last
	// stream time with that time, never to one before the frame (which
	// sent a seek to a keyframe's time to the keyframe before it).
	int64 coarse = stream_time_to_microseconds(1024, 1, 15360,
		AV_NOPTS_VALUE);
	int64 coarseBack = microseconds_to_stream_time(coarse, 1, 15360,
		AV_NOPTS_VALUE);
	int64 offsetBack = microseconds_to_stream_time(66666, 1, 15360,
		2 * 15360);
	const int64 keyFrame = 1234567891;
	int64 fine = stream_time_to_microseconds(keyFrame, 1, 1000000000,
		AV_NOPTS_VALUE);
	int64 fineBack = microseconds_to_stream_time(fine, 1, 1000000000,
		AV_NOPTS_VALUE);
	int64 fineAgain = stream_time_to_microseconds(fineBack, 1, 1000000000,
		AV_NOPTS_VALUE);
	int64 between = microseconds_to_stream_time(66665, 1, 15360,
		AV_NOPTS_VALUE);
	report("stream_time_round_trip", coarse == 66666 && coarseBack == 1024
		&& offsetBack == 2 * 15360 + 1024 && fine == 1234567
		&& fineBack >= keyFrame && fineAgain == fine && between == 1023,
		"1024 at 1/15360: %" B_PRId64 " us, back %" B_PRId64 " (want 66666, "
		"1024), after a 2 s start %" B_PRId64 " (want %d); %" B_PRId64
		" ns: %" B_PRId64 " us, back %" B_PRId64 " ns (want at least the "
		"original), %" B_PRId64 " us again; 66665 us at 1/15360: %" B_PRId64
		" (want 1023, the frame showing then)", coarse, coarseBack,
		offsetBack, 2 * 15360 + 1024, keyFrame, fine, fineBack, fineAgain,
		between);
}


struct Case {
	const char*	name;
	const char*	container;
	const char*	extension;
	const char*	video;		// NULL: audio only
	const char*	audio;
	bool		seek;		// seeking is checked (see below)
	bool		known;		// a known failure: reported as CE-INFO only
	bool		shortSamples;	// 16-bit input instead of float
};


static void
test_case(const Case& test)
{
	BString path("/tmp/media_codec_test.");
	path << test.extension;
	unlink(path.String());

	// Write
	bigtime_t start = system_time();
	status_t status;
	const char* step = "open";
	int32 videoTrack = -1, audioTrack = -1;
	{
		MediaWriter writer;
		status = writer.Open(path.String(), test.container);
		if (status == B_OK && test.video != NULL) {
			step = "video track";
			status = writer.AddVideoTrack(test.video, kWidth, kHeight,
				kFrameRate, &videoTrack);
		}
		if (status == B_OK) {
			step = "audio track";
			status = writer.AddAudioTrack(test.audio, kRate, 2, &audioTrack,
				!test.shortSamples);
		}
		if (status == B_OK) {
			step = "header";
			status = writer.Start();
		}
		if (status == B_OK)
			step = "frames";

		std::vector<uint32> bits(kWidth * kHeight);
		std::vector<float> audio;
		int32 audioPerFrame = (int32)(kRate / kFrameRate);
		int64 sample = 0;
		for (int32 frame = 0; status == B_OK && frame < kFrames; frame++) {
			if (videoTrack >= 0) {
				make_frame(frame, bits.data());
				status = writer.WriteVideoFrame(videoTrack, bits.data(),
					kWidth * 4);
			}
			audio.resize(audioPerFrame * 2);
			std::vector<int16> shorts(audioPerFrame * 2);
			for (int32 i = 0; i < audioPerFrame; i++, sample++) {
				audio[i * 2] = 0.3f * sin(2 * M_PI * 440 * sample / kRate);
				audio[i * 2 + 1] = 0.3f * sin(2 * M_PI * 660 * sample / kRate);
				shorts[i * 2] = (int16)(audio[i * 2] * 32767);
				shorts[i * 2 + 1] = (int16)(audio[i * 2 + 1] * 32767);
			}
			if (status == B_OK) {
				status = writer.WriteAudio(audioTrack, test.shortSamples
					? (const void*)shorts.data() : (const void*)audio.data(),
					audioPerFrame);
			}
		}
		status_t finish = writer.Finish();
		if (status == B_OK) {
			step = "finish";
			status = finish;
		}
		info("%s: %s, %.1f s to write", test.name, writer.Description(),
			(system_time() - start) / 1000000.0);
	}
	struct stat st;
	off_t size = stat(path.String(), &st) == 0 ? st.st_size : 0;
	report(BString(test.name) << "_write", status == B_OK && size > 0,
		"%s %s%s%s: %s%s%s, %" B_PRIdOFF " bytes", test.container,
		test.video != NULL ? test.video : "", test.video != NULL ? "+" : "",
		test.audio, status == B_OK ? "" : step, status == B_OK ? "" : ": ",
		strerror(status), size);
	if (status != B_OK)
		return;

	// Read
	MediaReader reader;
	status = reader.Open(path.String());
	int32 video = reader.FindTrack(B_MEDIA_RAW_VIDEO);
	int32 audio = reader.FindTrack(B_MEDIA_RAW_AUDIO);
	MediaTrackInfo videoInfo, audioInfo;
	bool tracks = status == B_OK && (test.video == NULL || video >= 0)
		&& audio >= 0;
	if (video >= 0)
		reader.GetTrackInfo(video, &videoInfo);
	if (audio >= 0)
		reader.GetTrackInfo(audio, &audioInfo);
	report(BString(test.name) << "_tracks", tracks
		&& (test.video == NULL || (videoInfo.width == kWidth
			&& videoInfo.height == kHeight))
		&& audioInfo.channels == 2 && audioInfo.frameRate == kRate,
		"%s: %" B_PRId32 " tracks, video %s %" B_PRId32 "x%" B_PRId32
		", audio %s %.0f Hz %" B_PRId32 " ch", strerror(status),
		reader.CountTracks(), video >= 0 ? videoInfo.codec.String() : "-",
		videoInfo.width, videoInfo.height,
		audio >= 0 ? audioInfo.codec.String() : "-", audioInfo.frameRate,
		audioInfo.channels);
	if (!tracks)
		return;

	if (test.video != NULL) {
		std::vector<uint32> bits(kWidth * kHeight);
		int32 frames = 0, rightColor = 0;
		BString wrong;
		while (frames < kFrames * 2) {
			bigtime_t time;
			if (reader.ReadVideoFrame(video, bits.data(), kWidth * 4, &time)
					!= B_OK) {
				break;
			}
			uint32 center = bits[kHeight / 2 * kWidth + kWidth / 2];
			if (color_near(center, frame_color(frames), 48))
				rightColor++;
			else if (wrong.Length() < 60)
				wrong << " " << frames << ":" << BString().SetToFormat("%06"
					B_PRIx32, center & 0xffffff);
			frames++;
		}
		report(BString(test.name) << "_video", frames == kFrames
			&& rightColor >= kFrames - 1,
			"%" B_PRId32 " frames decoded (want %" B_PRId32 "), %" B_PRId32
			" in the right colour%s", frames, kFrames, rightColor,
			wrong.String());
		check_video_times(test.name, path.String());
		if (strcmp(test.container, "mp4") != 0) {
			// MP4 has a duration per stream (and MediaWriter puts the
			// index at the end, out of reach without seeking)
			check_unseekable_frame_count(test.name, path.String(), false);
			check_unseekable_frame_count(test.name, path.String(), true);
		}
		if (strcmp(test.name, "webm_vp9_opus") == 0) {
			// a stream start offset of 2 s
			const char* offsetPath = "/tmp/media_codec_test.offset.mkv";
			unlink(offsetPath);
			status_t remuxed = remux_with_offset(path.String(), offsetPath,
				2000000);
			if (remuxed == B_OK)
				check_video_times("mkv_vp9_start_offset", offsetPath);
			else {
				report("mkv_vp9_start_offset_video_times", false,
					"remuxing with libavformat failed");
			}
			unlink(offsetPath);
		}
	}

	std::vector<float> left, right;
	float buffer[4096 * 2];
	while (left.size() < kRate * kSeconds * 2) {
		int32 frames = 0;
		bigtime_t time;
		if (reader.ReadAudio(audio, buffer, 4096, &frames, &time) != B_OK
			|| frames == 0)
			break;
		for (int32 i = 0; i < frames; i++) {
			left.push_back(buffer[i * 2]);
			right.push_back(buffer[i * 2 + 1]);
		}
	}
	double seconds = left.size() / kRate;
	double leftPurity = purity(left, 440);
	double rightPurity = purity(right, 660);
	report(BString(test.name) << "_audio", fabs(seconds - kSeconds) < 0.1
		&& leftPurity > 0.9 && rightPurity > 0.9,
		"%.3f s decoded (want %.1f), 440 Hz %.3f of the left channel, "
		"660 Hz %.3f of the right (want over 0.9)", seconds, kSeconds,
		leftPurity, rightPurity);
	if (strcmp(test.container, "mkv") == 0
		|| strcmp(test.container, "webm") == 0) {
		// Matroska streams have no duration: the count comes from the
		// file's, and MediaTrackAudioSupplier reads no further than it
		report(BString(test.name) << "_audio_frame_count",
			audioInfo.frames >= (int64)left.size(),
			"CountFrames() %" B_PRId64 ", %zu frames decoded (want at most "
			"the count)", audioInfo.frames, left.size());
	}

	status = reader.Seek(1000000);
	int32 frames = 0;
	bigtime_t time = -1;
	if (status == B_OK)
		status = reader.ReadAudio(audio, buffer, 1024, &frames, &time);
	// Seeking audio in WebM and Ogg files goes back to the start in the
	// FFmpeg plug-in (a known limit, recorded in the roadmap); checked for
	// the other containers.
	bool seeked = status == B_OK && frames > 0 && time >= 500000
		&& time <= 1100000;
	if (test.seek) {
		report(BString(test.name) << "_seek", seeked,
			"seek to 1 s: %s, audio from %.3f s", strerror(status),
			time / 1000000.0);
	} else {
		info("%s: seek to 1 s: %s, audio from %.3f s", test.name,
			strerror(status), time / 1000000.0);
	}
	unlink(path.String());
}


//!	Samples the host's FFmpeg made (codecs the plug-in only decodes).
static void
test_samples()
{
	BDirectory directory(kSamples);
	if (directory.InitCheck() != B_OK) {
		info("no samples in %s", kSamples);
		return;
	}
	BEntry entry;
	while (directory.GetNextEntry(&entry) == B_OK) {
		BPath path(&entry);
		BString name(path.Leaf());
		if (!name.EndsWith(".mkv") && !name.EndsWith(".mp4"))
			continue;
		BString check(name);
		check.RemoveLast(".mkv").RemoveLast(".mp4");
		check.Prepend("decode_");
		MediaReader reader;
		status_t status = reader.Open(path.Path());
		int32 video = reader.FindTrack(B_MEDIA_RAW_VIDEO);
		MediaTrackInfo videoInfo;
		videoInfo.width = videoInfo.height = 0;
		if (video >= 0)
			reader.GetTrackInfo(video, &videoInfo);
		std::vector<uint32> bits(kWidth * kHeight);
		int32 frames = 0, rightColor = 0;
		while (video >= 0 && videoInfo.width == kWidth
			&& videoInfo.height == kHeight && frames < kFrames * 2) {
			bigtime_t time;
			if (reader.ReadVideoFrame(video, bits.data(), kWidth * 4, &time)
					!= B_OK) {
				break;
			}
			if (color_near(bits[kHeight / 2 * kWidth + kWidth / 2],
					frame_color(frames), 48)) {
				rightColor++;
			}
			frames++;
		}
		report(check.String(), status == B_OK && frames == kFrames
			&& rightColor >= kFrames - 1,
			"%s: %s, %s %" B_PRId32 "x%" B_PRId32 ", %" B_PRId32 " frames, %"
			B_PRId32 " in the right colour", name.String(), strerror(status),
			videoInfo.codec.String(), videoInfo.width, videoInfo.height,
			frames, rightColor);
	}
}


int
main()
{
	BApplication app("application/x-vnd.Renga-media-codec-test");
	printf("CE-SUITE " SUITE " START\n");
	fflush(stdout);

	BMessage list;
	status_t status = MediaWriter::ListCodecs("webm", &list);
	BString codecs;
	const char* name;
	for (int32 i = 0; list.FindString("video", i, &name) == B_OK; i++)
		codecs << name << " ";
	codecs << "/ ";
	for (int32 i = 0; list.FindString("audio", i, &name) == B_OK; i++)
		codecs << name << " ";
	bool listed = status == B_OK && codecs.FindFirst("vp9 ") >= 0
		&& codecs.FindFirst("vp8 ") >= 0 && codecs.FindFirst("opus ") >= 0
		&& codecs.FindFirst("vorbis ") >= 0;
	report("list_codecs", listed, "WebM: %s", codecs.String());

	// Since the writer stopped rewriting the caller's sample format (F-31
	// phase 4), float samples reach every encoder, and the encoders of
	// planar formats (AAC, Vorbis, MP3) are created; seeking audio reads
	// forward from where the demuxer could go back to.
	static const Case kCases[] = {
		{ "webm_vp9_opus", "webm", "webm", "vp9", "opus", true, false,
			false },
		{ "webm_vp8_opus", "webm", "webm", "vp8", "opus", true, false,
			false },
		{ "ogg_opus", "ogg", "opus", NULL, "opus", true, false, false },
		{ "mkv_mpeg4_flac", "mkv", "mkv", "mpeg4", "flac", true, false,
			true },
		{ "mkv_mpeg4_flac_float", "mkv", "mkv", "mpeg4", "flac", true, false,
			false },
		{ "ogg_vorbis", "ogg", "ogg", NULL, "vorbis", true, false, false },
		{ "mp4_mpeg4_aac", "mp4", "mp4", "mpeg4", "aac", true, false, false },
		{ "mp3", "mp3", "mp3", NULL, "mp3", true, false, false },
		// FLV, what RTMP carries: Sorenson Spark video and AAC audio
		{ "flv_flv1_aac", "flv", "flv", "flv1", "aac", true, false, false },
	};
	for (size_t i = 0; i < sizeof(kCases) / sizeof(kCases[0]); i++) {
		sKnownFailure = kCases[i].known;
		test_case(kCases[i]);
	}
	sKnownFailure = false;

	test_b_frames("mp4_mpeg4_bframes", "mp4", "mp4", true);
	test_b_frames("mkv_mpeg4_bframes", "matroska", "mkv", false);
	test_time_conversion();

	test_samples();

	printf("CE-SUITE " SUITE " DONE failures=%d\n", sFailures);
	return sFailures == 0 ? 0 : 1;
}
