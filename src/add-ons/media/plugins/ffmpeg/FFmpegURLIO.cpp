/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 */


#include "FFmpegURLIO.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>


const char* FFmpegURLIO::kMagic = "RENGA-FFMPEG-URL\n";


/*!	The reader gets its source through the Media Kit's wrappers, not as
	this object; so the object reads as a small pseudo file that names the
	URL (kMagic, the URL, a newline, zeros), which AVFormatReader
	recognises in the first bytes it reads.
*/
FFmpegURLIO::FFmpegURLIO(const BUrl& url)
	:
	fURL(url.UrlString()),
	fPosition(0)
{
	memset(fData, 0, sizeof(fData));
	snprintf(fData, sizeof(fData), "%s%s\n", kMagic, fURL.String());
}


/*static*/ BString
FFmpegURLIO::URLFrom(const uint8* data, size_t size)
{
	size_t magic = strlen(kMagic);
	if (size < magic || memcmp(data, kMagic, magic) != 0)
		return BString();
	size_t end = magic;
	while (end < size && data[end] != '\n' && data[end] != '\0')
		end++;
	return BString((const char*)data + magic, end - magic);
}


/*static*/ bool
FFmpegURLIO::Handles(const BUrl& url)
{
	BString protocol = url.Protocol();
	protocol.ToLower();
	// Live protocols no Media Kit streamer speaks.
	if (protocol == "rtmp" || protocol == "rtmps" || protocol == "rtp"
		|| protocol == "udp" || protocol == "srt" || protocol == "tcp") {
		return true;
	}
	// HLS and DASH: the playlist names segments FFmpeg has to fetch itself
	// (relative to the playlist's URL), so a byte stream of the playlist is
	// not enough. Plain HTTP files stay with the HTTP streamer.
	if (protocol == "http" || protocol == "https") {
		BString path = url.Path();
		path.ToLower();
		return path.EndsWith(".m3u8") || path.EndsWith(".m3u")
			|| path.EndsWith(".mpd");
	}
	return false;
}


ssize_t
FFmpegURLIO::ReadAt(off_t position, void* buffer, size_t size)
{
	if (position < 0)
		return B_BAD_VALUE;
	if (position >= (off_t)kSize)
		return 0;
	size = min_c(size, (size_t)(kSize - position));
	memcpy(buffer, fData + position, size);
	return size;
}


ssize_t
FFmpegURLIO::WriteAt(off_t position, const void* buffer, size_t size)
{
	return B_NOT_SUPPORTED;
}


off_t
FFmpegURLIO::Seek(off_t position, uint32 seekMode)
{
	if (seekMode == SEEK_CUR)
		position += fPosition;
	else if (seekMode == SEEK_END)
		position += kSize;
	if (position < 0)
		return B_BAD_VALUE;
	fPosition = position;
	return fPosition;
}


off_t
FFmpegURLIO::Position() const
{
	return fPosition;
}


status_t
FFmpegURLIO::SetSize(off_t size)
{
	return B_NOT_SUPPORTED;
}


status_t
FFmpegURLIO::GetSize(off_t* size) const
{
	*size = kSize;
	return B_OK;
}


void
FFmpegURLIO::GetFlags(int32* flags) const
{
	*flags = B_MEDIA_SEEKABLE;
}


status_t
FFmpegStreamer::Sniff(const BUrl& url, BDataIO** source)
{
	if (!FFmpegURLIO::Handles(url))
		return B_NOT_SUPPORTED;
	fprintf(stderr, "ffmpeg: opening %s with FFmpeg's protocols\n",
		url.UrlString().String());
	FFmpegURLIO* io = new(std::nothrow) FFmpegURLIO(url);
	if (io == NULL)
		return B_NO_MEMORY;
	*source = io;
	return B_OK;
}
