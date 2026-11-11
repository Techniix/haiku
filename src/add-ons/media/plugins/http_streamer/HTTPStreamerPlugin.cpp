/*
 * Copyright 2016, Dario Casalinuovo
 * Distributed under the terms of the MIT License.
 */


#include "HTTPStreamerPlugin.h"

#include "HTTPMediaIO.h"

#include "MediaDebug.h"


HTTPStreamer::HTTPStreamer()
{
	CALLED();
}


HTTPStreamer::~HTTPStreamer()
{
	CALLED();
}


status_t
HTTPStreamer::Sniff(const BUrl& url, BDataIO** source)
{
	CALLED();

	// Renga (F-31 phase 4): only HTTP(S), and not HLS/DASH playlists, which
	// the FFmpeg plug-in's streamer opens with FFmpeg's own protocols (the
	// streamers are tried in turn; this one was taking every URL, also
	// udp:// and rtp://, without being able to read or write them).
	BString protocol = url.Protocol();
	protocol.ToLower();
	if (protocol != "http" && protocol != "https")
		return B_NOT_SUPPORTED;
	BString path = url.Path();
	path.ToLower();
	if (path.EndsWith(".m3u8") || path.EndsWith(".m3u")
		|| path.EndsWith(".mpd"))
		return B_NOT_SUPPORTED;

	HTTPMediaIO* outSource = new HTTPMediaIO(url);

	status_t ret = outSource->Open();
	if (ret == B_OK) {
		*source = outSource;
		return B_OK;
	}
	delete outSource;
	return ret;
}


Streamer*
HTTPStreamerPlugin::NewStreamer()
{
	return new HTTPStreamer();
}


MediaPlugin *instantiate_plugin()
{
	return new HTTPStreamerPlugin();
}
