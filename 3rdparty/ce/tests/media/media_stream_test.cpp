/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 */

/*!	Streaming through the FFmpeg plug-in's protocols (roadmap F-31 phase 4,
	src/add-ons/media/plugins/ffmpeg/FFmpegURLIO.h), on the loopback
	interface (the harness gives the guest no network card):

	- HLS playback: the test writes three one-second MPEG-TS segments with
	  MediaWriter (MPEG-4 video, red, green, blue), a playlist naming them
	  relative to itself, and serves them over HTTP/1.0 from a small server
	  thread; MediaReader opens http://127.0.0.1:<port>/index.m3u8 through
	  BMediaFile(BUrl), and every frame must decode in its segment's colour.
	- UDP and RTP output: MediaWriter writes to udp://127.0.0.1:<port>
	  (MPEG-TS) and rtp://127.0.0.1:<port> (RTP with MPEG-TS, payload type
	  33); a socket receives the datagrams: whole TS packets (sync byte 47h
	  every 188 bytes), RTP version 2 with consecutive sequence numbers,
	  and the received TS decodes again to the frames sent.
	- MediaPlayer opens the HLS URL (screenshot mediaplayer_hls).
	- RTMP output: MediaWriter publishes FLV (Sorenson Spark video, AAC
	  audio) to rtmp://127.0.0.1:<port>/live/ce; the server is FFmpeg's own
	  RTMP protocol in listen mode, run by the test through libavformat
	  (the guest has no network card, so no server on the host), which
	  must receive the publish, both streams with their codecs, and the
	  packets sent; what it received is written to an FLV file that must
	  decode to the frames sent.
*/


#include <Application.h>
#include <Messenger.h>
#include <OS.h>
#include <Roster.h>
#include <String.h>

#include <MediaCodec.h>

#include <arpa/inet.h>
#include <errno.h>
#include <math.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <unistd.h>

#include <vector>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
}


#define SUITE "media_stream"

using BPrivate::MediaReader;
using BPrivate::MediaTrackInfo;
using BPrivate::MediaWriter;

static const int32 kWidth = 160;
static const int32 kHeight = 120;
static const float kFrameRate = 15;
static const int32 kSegments = 3;
static const char* kDirectory = "/tmp/media_stream_hls";
static const uint16 kHttpPort = 8093;
static const uint16 kUdpPort = 5004;
static const uint16 kRtpPort = 5006;
static const uint16 kRtmpPort = 19350;

static int sFailures = 0;


static void
report(const char* name, bool pass, const char* format, ...)
{
	char detail[400];
	va_list args;
	va_start(args, format);
	vsnprintf(detail, sizeof(detail), format, args);
	va_end(args);
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


static uint32
segment_color(int32 segment)
{
	static const uint32 kColors[3] = { 0xffd02020, 0xff20d020, 0xff2020d0 };
	return kColors[segment % 3];
}


static bool
color_near(uint32 a, uint32 b, int tolerance)
{
	for (int shift = 0; shift < 24; shift += 8) {
		if (abs((int)((a >> shift) & 0xff) - (int)((b >> shift) & 0xff))
				> tolerance)
			return false;
	}
	return true;
}


//!	Writes \a segments seconds of MPEG-TS (one colour per second) to \a url.
static status_t
write_ts(const char* url, int32 firstSegment, int32 segments,
	BString* description)
{
	MediaWriter writer;
	status_t status = writer.Open(url, "mpegts");
	int32 video = -1;
	if (status == B_OK)
		status = writer.AddVideoTrack("mpeg4", kWidth, kHeight, kFrameRate,
			&video);
	if (status == B_OK)
		status = writer.Start();
	std::vector<uint32> bits(kWidth * kHeight);
	for (int32 s = 0; status == B_OK && s < segments; s++) {
		for (int32 i = 0; i < kWidth * kHeight; i++)
			bits[i] = segment_color(firstSegment + s);
		for (int32 f = 0; status == B_OK && f < (int32)kFrameRate; f++) {
			status = writer.WriteVideoFrame(video, bits.data(), kWidth * 4,
				f == 0);
		}
	}
	status_t finish = writer.Finish();
	if (description != NULL)
		*description = writer.Description();
	return status == B_OK ? finish : status;
}


/*!	Decodes every video frame of \a url; returns the count and how many
	were in the colour of their second (\a firstSegment on).
*/
static int32
decode_colors(const char* url, int32 firstSegment, int32* _right,
	status_t* _status, BString* codec)
{
	MediaReader reader;
	status_t status = reader.Open(url);
	*_status = status;
	*_right = 0;
	if (status != B_OK)
		return 0;
	int32 video = reader.FindTrack(B_MEDIA_RAW_VIDEO);
	if (video < 0) {
		*_status = B_MEDIA_NO_HANDLER;
		return 0;
	}
	MediaTrackInfo info;
	reader.GetTrackInfo(video, &info);
	if (codec != NULL)
		codec->SetToFormat("%s %" B_PRId32 "x%" B_PRId32, info.codec.String(),
			info.width, info.height);
	if (info.width != kWidth || info.height != kHeight) {
		*_status = B_BAD_DATA;
		return 0;
	}
	std::vector<uint32> bits(kWidth * kHeight);
	int32 frames = 0;
	while (frames < 1000) {
		bigtime_t time;
		if (reader.ReadVideoFrame(video, bits.data(), kWidth * 4, &time)
				!= B_OK)
			break;
		int32 second = frames / (int32)kFrameRate;
		if (color_near(bits[kHeight / 2 * kWidth + kWidth / 2],
				segment_color(firstSegment + second), 48))
			(*_right)++;
		frames++;
	}
	return frames;
}


// #pragma mark - HTTP server


static int sServerSocket = -1;
static int32 sRequests = 0;


static status_t
serve(void*)
{
	while (true) {
		int client = accept(sServerSocket, NULL, NULL);
		if (client < 0)
			return B_OK;
		char request[2048];
		ssize_t length = recv(client, request, sizeof(request) - 1, 0);
		if (length <= 0) {
			close(client);
			continue;
		}
		request[length] = '\0';
		atomic_add(&sRequests, 1);
		char path[256] = "";
		sscanf(request, "GET %255s", path);
		char* query = strchr(path, '?');
		if (query != NULL)
			*query = '\0';
		BString file(kDirectory);
		file << (strstr(path, "..") == NULL ? path : "/none");
		FILE* input = fopen(file.String(), "rb");
		if (input == NULL) {
			const char* notFound = "HTTP/1.0 404 Not Found\r\n"
				"Content-Length: 0\r\nConnection: close\r\n\r\n";
			send(client, notFound, strlen(notFound), 0);
			close(client);
			continue;
		}
		fseek(input, 0, SEEK_END);
		long size = ftell(input);
		fseek(input, 0, SEEK_SET);
		const char* type = file.EndsWith(".m3u8")
			? "application/vnd.apple.mpegurl" : "video/mp2t";
		BString header;
		header.SetToFormat("HTTP/1.0 200 OK\r\nContent-Type: %s\r\n"
			"Content-Length: %ld\r\nConnection: close\r\n\r\n", type, size);
		send(client, header.String(), header.Length(), 0);
		char buffer[16384];
		size_t read;
		while ((read = fread(buffer, 1, sizeof(buffer), input)) > 0) {
			if (send(client, buffer, read, 0) < 0)
				break;
		}
		fclose(input);
		close(client);
	}
}


static bool
start_server()
{
	sServerSocket = socket(AF_INET, SOCK_STREAM, 0);
	if (sServerSocket < 0)
		return false;
	int one = 1;
	setsockopt(sServerSocket, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
	sockaddr_in address;
	memset(&address, 0, sizeof(address));
	address.sin_family = AF_INET;
	address.sin_len = sizeof(address);
	address.sin_port = htons(kHttpPort);
	address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	if (bind(sServerSocket, (sockaddr*)&address, sizeof(address)) != 0
		|| listen(sServerSocket, 8) != 0) {
		return false;
	}
	thread_id thread = spawn_thread(serve, "http server", B_NORMAL_PRIORITY,
		NULL);
	return resume_thread(thread) == B_OK;
}


// #pragma mark - datagram receiver


struct Receiver {
	int					socket;
	std::vector<uint8>	data;		// TS payload, in order
	int32				datagrams;
	int32				badSync;
	int32				rtpBad;
	int32				sequenceGaps;
	bool				rtp;
	int32				quit;
};


static status_t
receive(void* cookie)
{
	Receiver* receiver = (Receiver*)cookie;
	uint8 buffer[65536];
	int32 lastSequence = -1;
	while (atomic_get(&receiver->quit) == 0) {
		ssize_t length = recv(receiver->socket, buffer, sizeof(buffer), 0);
		if (length < 0) {
			// timeouts (Haiku reports B_TIMED_OUT) and interruptions:
			// look at the quit flag again
			continue;
		}
		receiver->datagrams++;
		size_t offset = 0;
		if (receiver->rtp) {
			if (length < 12 || (buffer[0] >> 6) != 2
				|| (buffer[1] & 0x7f) != 33) {
				receiver->rtpBad++;
				continue;
			}
			int32 sequence = buffer[2] << 8 | buffer[3];
			if (lastSequence >= 0 && sequence != ((lastSequence + 1) & 0xffff))
				receiver->sequenceGaps++;
			lastSequence = sequence;
			offset = 12 + 4 * (buffer[0] & 0xf);
		}
		size_t payload = length - offset;
		if (payload % 188 != 0)
			receiver->badSync++;
		for (size_t p = offset; p + 188 <= (size_t)length; p += 188) {
			if (buffer[p] != 0x47)
				receiver->badSync++;
		}
		receiver->data.insert(receiver->data.end(), buffer + offset,
			buffer + length);
	}
	return B_OK;
}


/*!	Starts receiving datagrams on \a port (127.0.0.1) in a thread. */
static bool
start_receiver(Receiver& receiver, uint16 port, bool rtp, thread_id* _thread)
{
	receiver.datagrams = receiver.badSync = receiver.rtpBad = 0;
	receiver.sequenceGaps = 0;
	receiver.rtp = rtp;
	receiver.quit = 0;
	receiver.socket = socket(AF_INET, SOCK_DGRAM, 0);
	sockaddr_in address;
	memset(&address, 0, sizeof(address));
	address.sin_family = AF_INET;
	address.sin_len = sizeof(address);
	address.sin_port = htons(port);
	address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	int size = 4 * 1024 * 1024;
	setsockopt(receiver.socket, SOL_SOCKET, SO_RCVBUF, &size, sizeof(size));
	struct timeval timeout = { 0, 200000 };
	setsockopt(receiver.socket, SOL_SOCKET, SO_RCVTIMEO, &timeout,
		sizeof(timeout));
	if (bind(receiver.socket, (sockaddr*)&address, sizeof(address)) != 0) {
		close(receiver.socket);
		return false;
	}
	*_thread = spawn_thread(receive, "receiver", B_NORMAL_PRIORITY,
		&receiver);
	resume_thread(*_thread);
	return true;
}


static void
stop_receiver(Receiver& receiver, thread_id thread)
{
	atomic_set(&receiver.quit, 1);
	status_t result;
	wait_for_thread(thread, &result);
	close(receiver.socket);
}


/*!	Screenshot streams a region of the screen with system audio (the
	capture session's stream output, Screenshot's "Stream to:").
*/
static void
test_capture_stream()
{
	if (!be_roster->IsRunning("application/x-vnd.Be-input_server")) {
		info("no desktop, no capture stream");
		return;
	}
	system("/boot/system/preferences/ScreenRecording --set "
		"application/x-vnd.haiku-screenshot allow >/dev/null 2>&1");
	Receiver receiver;
	thread_id thread;
	const uint16 kPort = 5008;
	if (!start_receiver(receiver, kPort, false, &thread)) {
		report("capture_stream", false, "bind port %u: %s", kPort,
			strerror(errno));
		return;
	}
	BString command;
	command.SetToFormat("/boot/system/apps/Screenshot --record 3 --region "
		"0,0,160,120 --stream 'udp://127.0.0.1:%u?pkt_size=1316' "
		"--system-audio --frame-rate 10 --quit 2>&1", kPort);
	FILE* pipe = popen(command.String(), "r");
	BString output;
	char line[256];
	while (pipe != NULL && fgets(line, sizeof(line), pipe) != NULL)
		output << line;
	if (pipe != NULL)
		pclose(pipe);
	snooze(500000);
	stop_receiver(receiver, thread);
	output.RemoveAll("\n");

	BString file("/tmp/media_stream_capture.ts");
	FILE* stream = fopen(file.String(), "wb");
	if (stream != NULL) {
		fwrite(receiver.data.data(), 1, receiver.data.size(), stream);
		fclose(stream);
	}
	MediaReader reader;
	status_t status = reader.Open(file.String());
	int32 video = reader.FindTrack(B_MEDIA_RAW_VIDEO);
	int32 audio = reader.FindTrack(B_MEDIA_RAW_AUDIO);
	int32 frames = 0;
	if (video >= 0) {
		std::vector<uint32> bits(kWidth * kHeight);
		bigtime_t time;
		while (frames < 1000 && reader.ReadVideoFrame(video, bits.data(),
				kWidth * 4, &time) == B_OK)
			frames++;
	}
	report("capture_stream", receiver.datagrams > 5 && receiver.badSync == 0
		&& status == B_OK && video >= 0 && audio >= 0 && frames >= 15,
		"Screenshot --stream udp: \"%s\"; %" B_PRId32 " datagrams, %"
		B_PRIuSIZE " bytes, %" B_PRId32 " without TS sync; decoded: %s, "
		"video %s (%" B_PRId32 " frames, want 15 or more), audio %s",
		output.String(), receiver.datagrams, receiver.data.size(),
		receiver.badSync, strerror(status), video >= 0 ? "yes" : "no",
		frames, audio >= 0 ? "yes" : "no");
	unlink(file.String());
}


static void
test_output(const char* name, bool rtp, uint16 port)
{
	Receiver receiver;
	receiver.datagrams = receiver.badSync = receiver.rtpBad = 0;
	receiver.sequenceGaps = 0;
	receiver.rtp = rtp;
	receiver.quit = 0;
	receiver.socket = socket(AF_INET, SOCK_DGRAM, 0);
	sockaddr_in address;
	memset(&address, 0, sizeof(address));
	address.sin_family = AF_INET;
	address.sin_len = sizeof(address);
	address.sin_port = htons(port);
	address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	int size = 4 * 1024 * 1024;
	setsockopt(receiver.socket, SOL_SOCKET, SO_RCVBUF, &size, sizeof(size));
	struct timeval timeout = { 0, 200000 };
	setsockopt(receiver.socket, SOL_SOCKET, SO_RCVTIMEO, &timeout,
		sizeof(timeout));
	if (bind(receiver.socket, (sockaddr*)&address, sizeof(address)) != 0) {
		report(BString(name) << "_send", false, "bind port %u: %s", port,
			strerror(errno));
		close(receiver.socket);
		return;
	}
	thread_id thread = spawn_thread(receive, "receiver", B_NORMAL_PRIORITY,
		&receiver);
	resume_thread(thread);

	BString url;
	url.SetToFormat("%s://127.0.0.1:%u%s", rtp ? "rtp" : "udp", port,
		rtp ? "" : "?pkt_size=1316");
	BString description;
	// Paced like a live source: one second at a time.
	status_t status = write_ts(url.String(), 0, kSegments, &description);
	snooze(500000);
	atomic_set(&receiver.quit, 1);
	status_t result;
	wait_for_thread(thread, &result);
	close(receiver.socket);

	report(BString(name) << "_send", status == B_OK
		&& receiver.datagrams > 10 && receiver.badSync == 0
		&& receiver.rtpBad == 0 && receiver.sequenceGaps == 0,
		"%s (%s): %s; %" B_PRId32 " datagrams, %" B_PRIuSIZE " bytes of "
		"MPEG-TS, %" B_PRId32 " without TS sync, %" B_PRId32 " not RTP/33, %"
		B_PRId32 " sequence gaps", url.String(), description.String(),
		strerror(status), receiver.datagrams, receiver.data.size(),
		receiver.badSync, receiver.rtpBad, receiver.sequenceGaps);

	BString file;
	file.SetToFormat("/tmp/media_stream_%s.ts", name);
	FILE* output = fopen(file.String(), "wb");
	if (output != NULL) {
		fwrite(receiver.data.data(), 1, receiver.data.size(), output);
		fclose(output);
	}
	int32 right;
	BString codec;
	int32 frames = decode_colors(file.String(), 0, &right, &status, &codec);
	report(BString(name) << "_received_decodes",
		frames >= kSegments * (int32)kFrameRate - 2 && right >= frames - 1,
		"what arrived: %s, %s, %" B_PRId32 " frames (sent %" B_PRId32 "), %"
		B_PRId32 " in the right colour", strerror(status), codec.String(),
		frames, kSegments * (int32)kFrameRate, right);
	unlink(file.String());
}


// #pragma mark - RTMP


struct RtmpServer {
	BString		url;
	status_t	status;
	BString		error;
	int32		videoPackets;
	int32		audioPackets;
	BString		codecs;
	BString		file;
	int32		ready;
};


/*!	FFmpeg's RTMP protocol as a server: accepts one publisher, reads its
	FLV stream and remuxes it into a file.
*/
static status_t
rtmp_server(void* data)
{
	RtmpServer* server = (RtmpServer*)data;
	AVDictionary* options = NULL;
	av_dict_set(&options, "listen", "1", 0);
	av_dict_set(&options, "timeout", "30", 0);
	AVFormatContext* input = NULL;
	atomic_set(&server->ready, 1);
	int result = avformat_open_input(&input, server->url.String(), NULL,
		&options);
	av_dict_free(&options);
	if (result < 0) {
		char error[AV_ERROR_MAX_STRING_SIZE];
		av_strerror(result, error, sizeof(error));
		server->error = error;
		server->status = B_ERROR;
		return B_ERROR;
	}
	avformat_find_stream_info(input, NULL);

	AVFormatContext* output = NULL;
	avformat_alloc_output_context2(&output, NULL, "flv",
		server->file.String());
	std::vector<int> map(input->nb_streams, -1);
	for (unsigned i = 0; output != NULL && i < input->nb_streams; i++) {
		AVCodecParameters* parameters = input->streams[i]->codecpar;
		server->codecs << (i > 0 ? " + " : "")
			<< avcodec_get_name(parameters->codec_id);
		AVStream* stream = avformat_new_stream(output, NULL);
		if (stream == NULL)
			continue;
		avcodec_parameters_copy(stream->codecpar, parameters);
		stream->codecpar->codec_tag = 0;
		map[i] = stream->index;
	}
	bool writing = output != NULL
		&& avio_open(&output->pb, server->file.String(), AVIO_FLAG_WRITE)
			>= 0
		&& avformat_write_header(output, NULL) >= 0;

	AVPacket* packet = av_packet_alloc();
	while (av_read_frame(input, packet) >= 0) {
		AVMediaType type
			= input->streams[packet->stream_index]->codecpar->codec_type;
		if (type == AVMEDIA_TYPE_VIDEO)
			server->videoPackets++;
		else if (type == AVMEDIA_TYPE_AUDIO)
			server->audioPackets++;
		int index = map[packet->stream_index];
		if (writing && index >= 0) {
			av_packet_rescale_ts(packet,
				input->streams[packet->stream_index]->time_base,
				output->streams[index]->time_base);
			packet->stream_index = index;
			av_interleaved_write_frame(output, packet);
		}
		av_packet_unref(packet);
	}
	av_packet_free(&packet);
	if (writing)
		av_write_trailer(output);
	if (output != NULL) {
		avio_closep(&output->pb);
		avformat_free_context(output);
	}
	avformat_close_input(&input);
	server->status = B_OK;
	return B_OK;
}


static void
test_rtmp()
{
	RtmpServer server;
	server.url.SetToFormat("rtmp://127.0.0.1:%u/live/ce", kRtmpPort);
	server.status = B_ERROR;
	server.videoPackets = server.audioPackets = 0;
	server.file = "/tmp/media_stream_rtmp.flv";
	server.ready = 0;
	unlink(server.file.String());
	thread_id thread = spawn_thread(rtmp_server, "rtmp server",
		B_NORMAL_PRIORITY, &server);
	resume_thread(thread);
	while (atomic_get(&server.ready) == 0)
		snooze(10000);
	snooze(1000000);

	// Publish three seconds of FLV: one colour per second, a tone.
	MediaWriter writer;
	status_t status = writer.Open(server.url.String(), "flv");
	const char* step = "open";
	int32 video = -1, audio = -1;
	if (status == B_OK) {
		step = "video track";
		status = writer.AddVideoTrack("flv1", kWidth, kHeight, kFrameRate,
			&video);
	}
	if (status == B_OK) {
		step = "audio track";
		status = writer.AddAudioTrack("aac", 44100, 2, &audio, true);
	}
	if (status == B_OK) {
		step = "header";
		status = writer.Start();
	}
	std::vector<uint32> bits(kWidth * kHeight);
	std::vector<float> samples(44100 / (int32)kFrameRate * 2);
	int64 sample = 0;
	for (int32 s = 0; status == B_OK && s < kSegments; s++) {
		step = "frames";
		for (int32 i = 0; i < kWidth * kHeight; i++)
			bits[i] = segment_color(s);
		for (int32 f = 0; status == B_OK && f < (int32)kFrameRate; f++) {
			status = writer.WriteVideoFrame(video, bits.data(), kWidth * 4,
				f == 0);
			for (size_t i = 0; i < samples.size() / 2; i++, sample++) {
				float value = 0.25f * sinf(2 * M_PI * 440 * sample / 44100.0);
				samples[i * 2] = samples[i * 2 + 1] = value;
			}
			if (status == B_OK) {
				status = writer.WriteAudio(audio, samples.data(),
					samples.size() / 2);
			}
		}
		// paced like a live source
		snooze(300000);
	}
	status_t finish = writer.Finish();
	if (status == B_OK)
		status = finish;
	BString description = writer.Description();

	bigtime_t deadline = system_time() + 15000000;
	while (system_time() < deadline) {
		thread_info info;
		if (get_thread_info(thread, &info) != B_OK)
			break;
		snooze(100000);
	}
	status_t result;
	if (system_time() >= deadline)
		kill_thread(thread);
	wait_for_thread(thread, &result);

	report("rtmp_publish", status == B_OK && server.status == B_OK
			&& server.videoPackets >= kSegments * (int32)kFrameRate - 2
			&& server.audioPackets > 0,
		"%s (%s): %s at %s; the RTMP server (FFmpeg, listen mode): %s, "
		"streams %s, %" B_PRId32 " video packets (sent %" B_PRId32 "), %"
		B_PRId32 " audio packets", server.url.String(), description.String(),
		strerror(status), step, server.status == B_OK ? "received"
			: server.error.String(), server.codecs.String(),
		server.videoPackets, kSegments * (int32)kFrameRate,
		server.audioPackets);

	int32 right;
	BString codec;
	int32 frames = decode_colors(server.file.String(), 0, &right, &status,
		&codec);
	report("rtmp_received_decodes",
		frames >= kSegments * (int32)kFrameRate - 2 && right >= frames - 1,
		"what the server received: %s, %s, %" B_PRId32 " frames (sent %"
		B_PRId32 "), %" B_PRId32 " in the right colour", strerror(status),
		codec.String(), frames, kSegments * (int32)kFrameRate, right);
	unlink(server.file.String());
}


// #pragma mark -


static void
test_hls()
{
	mkdir(kDirectory, 0755);
	status_t status = B_OK;
	BString playlist("#EXTM3U\n#EXT-X-VERSION:3\n#EXT-X-TARGETDURATION:1\n"
		"#EXT-X-MEDIA-SEQUENCE:0\n");
	for (int32 s = 0; status == B_OK && s < kSegments; s++) {
		BString path;
		path.SetToFormat("%s/segment%" B_PRId32 ".ts", kDirectory, s);
		status = write_ts(path.String(), s, 1, NULL);
		playlist << "#EXTINF:1.0,\nsegment" << s << ".ts\n";
	}
	playlist << "#EXT-X-ENDLIST\n";
	BString playlistPath(kDirectory);
	playlistPath << "/index.m3u8";
	FILE* file = fopen(playlistPath.String(), "w");
	if (file != NULL) {
		fputs(playlist.String(), file);
		fclose(file);
	}
	report("hls_segments", status == B_OK && file != NULL,
		"%" B_PRId32 " MPEG-TS segments and a playlist: %s", kSegments,
		strerror(status));

	bool server = start_server();
	report("http_server", server, "HTTP server on 127.0.0.1:%u", kHttpPort);
	if (!server)
		return;

	BString url;
	url.SetToFormat("http://127.0.0.1:%u/index.m3u8", kHttpPort);
	int32 right;
	BString codec;
	int32 before = atomic_get(&sRequests);
	int32 frames = decode_colors(url.String(), 0, &right, &status, &codec);
	int32 requests = atomic_get(&sRequests) - before;
	report("hls_playback", status == B_OK
		&& frames >= kSegments * (int32)kFrameRate - 2
		&& right >= frames - 1 && requests >= kSegments + 1,
		"%s: %s, %s, %" B_PRId32 " frames (want %" B_PRId32 "), %" B_PRId32
		" in the right colour, %" B_PRId32 " HTTP requests (playlist and "
		"segments)", url.String(), strerror(status), codec.String(), frames,
		kSegments * (int32)kFrameRate, right, requests);
}


static void
test_mediaplayer()
{
	if (!be_roster->IsRunning("application/x-vnd.Be-input_server")) {
		info("no desktop, no MediaPlayer screenshot");
		return;
	}
	BString url;
	url.SetToFormat("http://127.0.0.1:%u/index.m3u8", kHttpPort);
	int32 before = atomic_get(&sRequests);
	pid_t child = fork();
	if (child == 0) {
		execl("/boot/system/apps/MediaPlayer", "MediaPlayer", url.String(),
			(char*)NULL);
		_exit(127);
	}
	snooze(5000000);
	int32 requests = atomic_get(&sRequests) - before;
	BMessenger messenger(NULL, child);
	report("mediaplayer_opens_url", messenger.IsValid() && requests > 0,
		"MediaPlayer %s, %" B_PRId32 " requests to the server",
		messenger.IsValid() ? "running" : "not running", requests);
	printf("CE-HOST screenshot mediaplayer_hls\n");
	fflush(stdout);
	snooze(1500000);
	if (messenger.IsValid())
		messenger.SendMessage(B_QUIT_REQUESTED);
	snooze(1000000);
	kill(child, SIGKILL);
	waitpid(child, NULL, 0);
}


int
main()
{
	signal(SIGPIPE, SIG_IGN);
	BApplication app("application/x-vnd.Renga-media-stream-test");
	printf("CE-SUITE " SUITE " START\n");
	fflush(stdout);

	test_hls();
	test_output("udp", false, kUdpPort);
	test_output("rtp", true, kRtpPort);
	test_rtmp();
	test_capture_stream();
	test_mediaplayer();

	printf("CE-SUITE " SUITE " DONE failures=%d\n", sFailures);
	return sFailures == 0 ? 0 : 1;
}
