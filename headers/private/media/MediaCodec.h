/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 */
#ifndef _MEDIA_CODEC_H
#define _MEDIA_CODEC_H


/*!	Encoding and decoding audio and video with common codecs, without the
	details of BMediaFile, BMediaTrack, media_format negotiation and codec
	lookup (Renga, roadmap F-31 phase 4). See 3rdparty/ce/PORTING.md,
	"Encoding and decoding".

	MediaWriter writes a file: choose the container and codecs by their
	short names (as the FFmpeg plug-in lists them: "webm", "mkv", "mp4",
	"ogg", "flac", "wav"; "vp9", "vp8", "mpeg4", "theora", "mjpeg"; "opus",
	"vorbis", "flac", "aac", "mp3", "ac3", "pcm"), add tracks, then hand
	it B_RGB32 frames and interleaved 16-bit or float audio. Times are
	derived from the frame rate and the sample count.

		BPrivate::MediaWriter writer;
		writer.Open("/boot/home/clip.webm", "webm");
		int32 video, audio;
		writer.AddVideoTrack("vp9", 640, 480, 30, &video);
		writer.AddAudioTrack("opus", 48000, 2, &audio);
		writer.Start();
		writer.WriteVideoFrame(video, bitmap->Bits(), bitmap->BytesPerRow());
		writer.WriteAudio(audio, samples, frames);
		writer.Finish();

	MediaReader reads a file, or a stream by URL (http, https; the Media
	Kit's streamer plug-ins), and decodes to B_RGB32 frames and interleaved
	float audio.

	Both use the Media Kit's plug-ins, so whatever codec the FFmpeg plug-in
	has (and later the hardware back ends of phase 5) is available. They
	are not thread safe; use one object per thread. Private API: the
	classes may change, but only by adding (the symbols live in libmedia).
*/


#include <MediaDefs.h>
#include <String.h>


class BMediaFile;
class BMediaTrack;
class BMessage;


namespace BPrivate {


class MediaWriter {
public:
								MediaWriter();
								~MediaWriter();
									// finishes the file if needed

	/*!	\a container: a file format short name ("webm", "mkv", ...).
		\a path may also be a stream URL (rtmp://, rtp://, udp://, srt://;
		RTMP takes "flv", the others "mpegts").
	*/
			status_t			Open(const char* path, const char* container);

			status_t			AddVideoTrack(const char* codec, int32 width,
									int32 height, float frameRate,
									int32* _track, float quality = 0.8f);
			status_t			AddAudioTrack(const char* codec,
									float frameRate, int32 channels,
									int32* _track, bool floatSamples = false,
									float quality = 0.8f);

	//!	Writes the header; call after adding every track.
			status_t			Start();

	//!	One B_RGB32 frame of the track's size.
			status_t			WriteVideoFrame(int32 track, const void* bits,
									int32 bytesPerRow, bool keyFrame = false);
	//!	Interleaved samples, int16 or float as the track was added.
			status_t			WriteAudio(int32 track, const void* samples,
									int32 frames);

			status_t			Finish();

			int64				FramesWritten(int32 track) const;
			const char*			Description() const
									{ return fDescription.String(); }

	/*!	The codecs the plug-ins offer for \a container: "video" and "audio"
		string arrays of short names, "video_name"/"audio_name" the pretty
		names. With \a container NULL, the containers ("container" and
		"container_name").
	*/
	static	status_t			ListCodecs(const char* container,
									BMessage* list);

private:
	struct Track;

			Track*				_TrackAt(int32 index) const;
			status_t			_CreateFile(const char* path,
									const media_file_format* format);

			BMediaFile*			fFile;
			BList				fTracks;
			bool				fStarted;
			bool				fFinished;
			BString				fContainer;
			BString				fDescription;
};


struct MediaTrackInfo {
	media_type	type;			// B_MEDIA_RAW_VIDEO or B_MEDIA_RAW_AUDIO
	BString		codec;			// the codec's pretty name
	BString		codecShortName;
	bigtime_t	duration;		// 0 if not known (live streams)
	int64		frames;			// video frames or audio frames, 0 if unknown
	int32		width;			// video
	int32		height;
	float		frameRate;		// video: frames per second; audio: Hz
	int32		channels;		// audio
};


class MediaReader {
public:
								MediaReader();
								~MediaReader();

	//!	A file path, or a URL (http://, https://, file://).
			status_t			Open(const char* pathOrURL);
			void				Close();

			int32				CountTracks() const;
			status_t			GetTrackInfo(int32 track,
									MediaTrackInfo* info) const;
	//!	The first track of a type, or -1.
			int32				FindTrack(media_type type) const;

	//!	The next frame as B_RGB32 into \a bits (width * 4 per row at least).
			status_t			ReadVideoFrame(int32 track, void* bits,
									int32 bytesPerRow, bigtime_t* _time);
	//!	Up to \a maxFrames interleaved float frames.
			status_t			ReadAudio(int32 track, float* samples,
									int32 maxFrames, int32* _frames,
									bigtime_t* _time);

			status_t			Seek(bigtime_t time);

private:
			status_t			_Prepare();

			BMediaFile*			fFile;
			BList				fTracks;		// BMediaTrack*
			media_format*		fFormats;		// decoded formats
			float*				fAudioBuffer;
			int64				fAudioBufferFrames;
			int64				fAudioBufferUsed;
			int64				fAudioBufferRead;
			bigtime_t			fAudioBufferTime;
			int32				fAudioTrack;
};


}	// namespace BPrivate


#endif	// _MEDIA_CODEC_H
