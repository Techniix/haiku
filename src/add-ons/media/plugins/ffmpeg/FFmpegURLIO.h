/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 */
#ifndef FFMPEG_URL_IO_H
#define FFMPEG_URL_IO_H


#include <MediaIO.h>
#include <String.h>
#include <Url.h>

#include "StreamerPlugin.h"


/*!	Renga (roadmap F-31 phase 4, streaming): a URL that FFmpeg opens with
	its own protocols (HLS and DASH playlists over HTTP(S), RTMP, RTP, UDP,
	SRT) instead of reading it through a Media Kit streamer's BDataIO. The
	FFmpeg streamer hands BMediaFile this object; AVFormatReader and
	AVFormatWriter recognise it (same add-on) and call avformat_open_input()
	or avio_open2() with the URL. It has no data of its own.
*/
class FFmpegURLIO : public BMediaIO {
public:
								FFmpegURLIO(const BUrl& url);

			const char*			URL() const { return fURL.String(); }
	//!	Whether FFmpeg should open \a url itself (see FFmpegStreamer).
	static	bool				Handles(const BUrl& url);

	virtual	ssize_t				ReadAt(off_t position, void* buffer,
									size_t size);
	virtual	ssize_t				WriteAt(off_t position, const void* buffer,
									size_t size);
	virtual	off_t				Seek(off_t position, uint32 seekMode);
	virtual	off_t				Position() const;
	virtual	status_t			SetSize(off_t size);
	virtual	status_t			GetSize(off_t* size) const;
	virtual	void				GetFlags(int32* flags) const;

	//!	The pseudo file's first bytes; the URL follows up to a newline.
	static	const char*			kMagic;
	static	const size_t		kSize = 1024;
	//!	The URL a pseudo file of this kind names, or an empty string.
	static	BString				URLFrom(const uint8* data, size_t size);

private:
			BString				fURL;
			char				fData[kSize];
			off_t				fPosition;
};


class FFmpegStreamer : public BPrivate::media::Streamer {
public:
	virtual	status_t			Sniff(const BUrl& url, BDataIO** source);
};


#endif	// FFMPEG_URL_IO_H
