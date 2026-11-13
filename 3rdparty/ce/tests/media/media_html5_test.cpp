/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 */

/*!	HTML5 audio and video in WebPositive (roadmap F-57 item 4): HaikuWebKit
	plays <video> and <audio> through the Media Kit (BMediaFile on the
	media's URL, BMediaTrack, BSoundPlayer), so the FFmpeg plug-in's codecs
	and protocols are what it can play. The test serves a page and clips
	written with MediaWriter from a local HTTP server (the guest has no
	network card; the MP4 clip gets its index moved to the front, as MP4
	files on the web have it) and opens the page in WebPositive.

	In-tree part (checks):
	- BNetworkAddress("127.0.0.1") resolves at once without a network
	  (it took 56 s and failed: the HTTP streamer could not reach a local
	  server);
	- BMediaFile on each clip's http:// URL (the HTTP streamer): both
	  tracks open and decode;
	- the same URL opened 20 times, each open finding the audio track
	  (url_open_repeated; the streamer's start-up race, F-61);
	- WebKit's audio path without WebKit: the decoded format handed to a
	  BSoundPlayer, one ReadFrames() per callback; the loopback recording
	  must hold both tones cleanly, without gaps.
	In WebPositive (checks):
	- canPlayType() for the types sites ask about (the page puts the answers
	  in its title, which the test reads from WebPositive's window through
	  scripting): WebM VP9/Opus, MP4 MPEG-4/AAC, Ogg Vorbis, FLAC, MP3, HLS;
	- the WebM clip (VP9, Opus: 440 Hz left, 660 Hz right) and the MP4 clip
	  (MPEG-4 video, AAC audio) start playing on their own ("playing"
	  event; autoplay); screenshots show the frame.
	Logged, not checked: what WebPositive's own stream sounds like.

	Known limits (HaikuWebKit 1.10.0, packaged, not in this tree): a video
	without an audio track does not advance (its audio drives the clock;
	the fix is in RengaPorts, haiku-libs/haikuwebkit, not built here); its
	sound has gaps (WebM, about 90 % of 10 ms blocks silent in the harness)
	or is missing (MP4) although the Media Kit's part of the same path
	plays cleanly: its player decodes video in the audio callback.
*/


#include <Application.h>
#include <MediaFile.h>
#include <MediaRecorder.h>
#include <MediaRoster.h>
#include <MediaTrack.h>
#include <Messenger.h>
#include <NetworkAddress.h>
#include <OS.h>
#include <Roster.h>
#include <SoundPlayer.h>
#include <String.h>
#include <Url.h>

#include <AudioStreamControl.h>
#include <MediaCodec.h>

#include <Autolock.h>
#include <Locker.h>

#include <arpa/inet.h>
#include <math.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <vector>

extern "C" {
#include <libavformat/avformat.h>
}


#define SUITE "html5_media"

using BPrivate::AudioStreamControl;
using BPrivate::MediaWriter;

static const char* kDirectory = "/tmp/html5_media";
static const uint16 kPort = 8095;
static const char* kWebPositive = "application/x-vnd.Haiku-WebPositive";
static const int32 kWidth = 160;
static const int32 kHeight = 120;
static const float kFrameRate = 15;
static const float kRate = 48000;

static int sFailures = 0;
static int sServerSocket = -1;
static int32 sAccepted = 0;
static int32 sRequests = 0;


static void
report(const char* name, bool pass, const char* format, ...)
{
	char detail[500];
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
	char text[500];
	va_list args;
	va_start(args, format);
	vsnprintf(text, sizeof(text), format, args);
	va_end(args);
	printf("CE-INFO html5_media: %s\n", text);
	fflush(stdout);
}


// #pragma mark - media and page


static status_t
write_clip(const char* name, const char* container, const char* video,
	const char* audio, float seconds)
{
	BString path(kDirectory);
	path << "/" << name;
	MediaWriter writer;
	status_t status = writer.Open(path.String(), container);
	int32 videoTrack = -1;
	int32 audioTrack = -1;
	if (status == B_OK)
		status = writer.AddVideoTrack(video, kWidth, kHeight, kFrameRate,
			&videoTrack);
	if (status == B_OK && audio != NULL)
		status = writer.AddAudioTrack(audio, kRate, 2, &audioTrack, true);
	if (status == B_OK)
		status = writer.Start();
	std::vector<uint32> bits(kWidth * kHeight);
	int32 perFrame = (int32)(kRate / kFrameRate);
	std::vector<float> samples(perFrame * 2);
	int64 sample = 0;
	int32 frames = (int32)(seconds * kFrameRate);
	for (int32 f = 0; status == B_OK && f < frames; f++) {
		uint32 colour = (f / (int32)kFrameRate) % 2 == 0 ? 0xff2060d0
			: 0xffd0a020;
		for (int32 i = 0; i < kWidth * kHeight; i++)
			bits[i] = colour;
		status = writer.WriteVideoFrame(videoTrack, bits.data(), kWidth * 4,
			f % (int32)kFrameRate == 0);
		if (status != B_OK || audio == NULL)
			continue;
		for (int32 i = 0; i < perFrame; i++, sample++) {
			samples[i * 2] = 0.25f * sinf(2 * M_PI * 440 * sample / kRate);
			samples[i * 2 + 1] = 0.25f * sinf(2 * M_PI * 660 * sample / kRate);
		}
		status = writer.WriteAudio(audioTrack, samples.data(), perFrame);
	}
	status_t finish = writer.Finish();
	return status == B_OK ? finish : status;
}


/*!	Rewrites \a name with its index (the MP4 "moov" box) first, as MP4
	files served on the web are: the Media Kit's writer puts it at the end,
	and the HTTP streamer reads a file front to back.
*/
static status_t
make_faststart(const char* name)
{
	BString path(kDirectory);
	path << "/" << name;
	BString temporary(path);
	temporary << ".faststart.mp4";
	AVFormatContext* input = NULL;
	if (avformat_open_input(&input, path.String(), NULL, NULL) < 0)
		return B_ERROR;
	status_t status = B_ERROR;
	AVFormatContext* output = NULL;
	if (avformat_find_stream_info(input, NULL) >= 0
		&& avformat_alloc_output_context2(&output, NULL, "mp4",
			temporary.String()) >= 0) {
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
		AVDictionary* options = NULL;
		av_dict_set(&options, "movflags", "+faststart", 0);
		ok = ok && avio_open(&output->pb, temporary.String(), AVIO_FLAG_WRITE)
				>= 0
			&& avformat_write_header(output, &options) >= 0;
		av_dict_free(&options);
		AVPacket* packet = av_packet_alloc();
		while (ok && packet != NULL && av_read_frame(input, packet) >= 0) {
			av_packet_rescale_ts(packet,
				input->streams[packet->stream_index]->time_base,
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
	if (status == B_OK && rename(temporary.String(), path.String()) != 0)
		status = B_ERROR;
	return status;
}


static const char* kTypes[] = {
	"video/webm; codecs=\"vp9, opus\"",
	"video/webm; codecs=\"vp8, vorbis\"",
	"video/mp4; codecs=\"mp4v.20.8, mp4a.40.2\"",
	"audio/ogg; codecs=\"vorbis\"",
	"audio/flac",
	"audio/mpeg",
	"application/vnd.apple.mpegurl",
	NULL
};


static status_t
write_page(const char* name, const char* clip)
{
	BString page;
	page << "<!DOCTYPE html><html><head><title>loading</title></head>"
		"<body style='background:#202020;color:#ddd'>"
		"<p>Renga HTML5 media test: " << clip << "</p>"
		"<video id='v' autoplay width='320' height='240' src='" << clip
		<< "'></video><script>\n"
		"var v = document.getElementById('v');\n"
		"var types = [";
	for (int i = 0; kTypes[i] != NULL; i++) {
		BString type(kTypes[i]);
		type.ReplaceAll("\"", "\\\"");
		page << (i > 0 ? "," : "") << "\"" << type << "\"";
	}
	page << "];\n"
		"var answers = [];\n"
		"for (var i = 0; i < types.length; i++)\n"
		"  answers.push(i + '=' + (v.canPlayType(types[i]) || 'no'));\n"
		"document.title = 'types ' + answers.join(' ');\n"
		"v.addEventListener('playing', function() {\n"
		"  document.title += ' playing'; });\n"
		"v.addEventListener('error', function() {\n"
		"  document.title += ' error ' + (v.error ? v.error.code : '?'); });\n"
		"</script></body></html>\n";
	BString path(kDirectory);
	path << "/" << name;
	FILE* file = fopen(path.String(), "w");
	if (file == NULL)
		return B_ERROR;
	fputs(page.String(), file);
	fclose(file);
	return B_OK;
}


// #pragma mark - HTTP server (HTTP/1.0 with byte ranges)


static const char*
content_type(const BString& file)
{
	if (file.EndsWith(".html"))
		return "text/html";
	if (file.EndsWith(".webm"))
		return "video/webm";
	if (file.EndsWith(".mp4"))
		return "video/mp4";
	return "application/octet-stream";
}


/*!	One connection. WebKit opens a second connection (a range request)
	while the first is still being sent, so each has its own thread.
*/
static status_t
serve_client(void* data)
{
	int client = (int)(addr_t)data;
	do {
		char request[4096];
		ssize_t length = recv(client, request, sizeof(request) - 1, 0);
		if (length <= 0) {
			break;
		}
		request[length] = '\0';
		atomic_add(&sRequests, 1);
		char path[256] = "";
		sscanf(request, "GET %255s", path);
		const char* rangeLog = strcasestr(request, "\nRange: ");
		char rangeText[64] = "";
		if (rangeLog != NULL)
			sscanf(rangeLog + 9, "%63s", rangeText);
		info("GET %s %s", path, rangeText);
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
			break;
		}
		fseek(input, 0, SEEK_END);
		long size = ftell(input);
		long first = 0;
		long last = size - 1;
		bool range = false;
		const char* rangeHeader = strcasestr(request, "\nRange: bytes=");
		if (rangeHeader != NULL) {
			long a = -1, b = -1;
			if (sscanf(rangeHeader + 14, "%ld-%ld", &a, &b) >= 1 && a >= 0
				&& a < size) {
				first = a;
				if (b >= a && b < size)
					last = b;
				range = true;
			}
		}
		fseek(input, first, SEEK_SET);
		BString header;
		if (range) {
			header.SetToFormat("HTTP/1.0 206 Partial Content\r\n"
				"Content-Type: %s\r\nAccept-Ranges: bytes\r\n"
				"Content-Range: bytes %ld-%ld/%ld\r\nContent-Length: %ld\r\n"
				"Connection: close\r\n\r\n", content_type(file), first, last,
				size, last - first + 1);
		} else {
			header.SetToFormat("HTTP/1.0 200 OK\r\nContent-Type: %s\r\n"
				"Accept-Ranges: bytes\r\nContent-Length: %ld\r\n"
				"Connection: close\r\n\r\n", content_type(file), size);
		}
		send(client, header.String(), header.Length(), 0);
		char buffer[16384];
		long remaining = last - first + 1;
		while (remaining > 0) {
			size_t read = fread(buffer, 1, min_c((long)sizeof(buffer),
				remaining), input);
			if (read == 0 || send(client, buffer, read, 0) < 0)
				break;
			remaining -= read;
		}
		fclose(input);
	} while (false);
	close(client);
	return B_OK;
}


static status_t
serve(void*)
{
	while (true) {
		int client = accept(sServerSocket, NULL, NULL);
		if (client < 0)
			return B_OK;
		atomic_add(&sAccepted, 1);
		thread_id thread = spawn_thread(serve_client, "html5 client",
			B_NORMAL_PRIORITY, (void*)(addr_t)client);
		if (thread < 0 || resume_thread(thread) != B_OK)
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
	address.sin_port = htons(kPort);
	address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	if (bind(sServerSocket, (sockaddr*)&address, sizeof(address)) != 0
		|| listen(sServerSocket, 16) != 0) {
		return false;
	}
	thread_id thread = spawn_thread(serve, "http server", B_NORMAL_PRIORITY,
		NULL);
	return resume_thread(thread) == B_OK;
}


// #pragma mark - recording


struct Recording {
	BLocker				lock;
	std::vector<float>	left;
	std::vector<float>	right;
	float				rate;

	Recording() : lock("recording"), rate(0) {}
};


static void
record_buffer(void* cookie, bigtime_t timestamp, void* data, size_t size,
	const media_format& format)
{
	Recording* recording = (Recording*)cookie;
	const media_raw_audio_format& raw = format.u.raw_audio;
	size_t sampleSize = raw.format & media_raw_audio_format::B_AUDIO_SIZE_MASK;
	if (sampleSize == 0 || raw.channel_count == 0)
		return;
	BAutolock locker(recording->lock);
	recording->rate = raw.frame_rate;
	size_t frames = size / (sampleSize * raw.channel_count);
	for (size_t i = 0; i < frames; i++) {
		float values[2] = { 0, 0 };
		for (uint32 c = 0; c < 2 && c < raw.channel_count; c++) {
			size_t index = i * raw.channel_count + c;
			switch (raw.format) {
				case media_raw_audio_format::B_AUDIO_FLOAT:
					values[c] = ((float*)data)[index];
					break;
				case media_raw_audio_format::B_AUDIO_SHORT:
					values[c] = ((int16*)data)[index] / 32768.0f;
					break;
				case media_raw_audio_format::B_AUDIO_INT:
					values[c] = ((int32*)data)[index] / 2147483648.0f;
					break;
				default:
					break;
			}
		}
		if (raw.channel_count == 1)
			values[1] = values[0];
		recording->left.push_back(values[0]);
		recording->right.push_back(values[1]);
	}
}


static status_t
record_application(const char* signature, double seconds,
	Recording& recording)
{
	BMediaRoster* roster = BMediaRoster::Roster();
	media_node node;
	status_t status = AudioStreamControl::StartLoopback(signature, &node);
	if (status != B_OK)
		return status;
	media_output output;
	int32 count;
	status = roster->GetFreeOutputsFor(node, &output, 1, &count,
		B_MEDIA_RAW_AUDIO);
	if (status == B_OK && count < 1)
		status = B_BUSY;
	if (status == B_OK) {
		BMediaRecorder recorder("html5 recorder", B_MEDIA_RAW_AUDIO);
		recorder.SetHooks(record_buffer, NULL, &recording);
		media_format format = output.format;
		status = recorder.Connect(node, &output, &format);
		if (status == B_OK && (status = recorder.Start()) == B_OK) {
			snooze((bigtime_t)(seconds * 1000000));
			recorder.Stop();
		}
		recorder.Disconnect();
	}
	roster->ReleaseNode(node);
	return status;
}


//!	The share of \a samples' energy at \a hertz.
static double
purity(const std::vector<float>& samples, double rate, double hertz)
{
	if (samples.size() < 4800 || rate <= 0)
		return 0;
	double coefficient = 2 * cos(2 * M_PI * hertz / rate);
	double s1 = 0, s2 = 0, energy = 0;
	for (size_t i = 0; i < samples.size(); i++) {
		double s0 = samples[i] + coefficient * s1 - s2;
		s2 = s1;
		s1 = s0;
		energy += (double)samples[i] * samples[i];
	}
	double power = s1 * s1 + s2 * s2 - coefficient * s1 * s2;
	return energy > 0 ? 2 * power / (samples.size() * energy) : 0;
}


static double
rms(const std::vector<float>& samples)
{
	if (samples.empty())
		return 0;
	double sum = 0;
	for (size_t i = 0; i < samples.size(); i++)
		sum += (double)samples[i] * samples[i];
	return sqrt(sum / samples.size());
}


//!	The share of 10 ms blocks with (almost) no sound: gaps in playback.
static double
silent_share(const std::vector<float>& samples, double rate)
{
	size_t block = (size_t)(rate / 100);
	if (block == 0 || samples.size() < block)
		return 1;
	size_t blocks = samples.size() / block;
	size_t silent = 0;
	for (size_t b = 0; b < blocks; b++) {
		double peak = 0;
		for (size_t i = b * block; i < (b + 1) * block; i++)
			peak = max_c(peak, fabs((double)samples[i]));
		if (peak < 0.01)
			silent++;
	}
	return (double)silent / blocks;
}


// #pragma mark - WebPositive


/*!	The title of WebPositive's browser window: its windows are numbered in
	creation order and the first may be the Downloads window, so the test
	looks at each and takes the one showing the test page's title.
*/
static BString
window_title(team_id team)
{
	BMessenger app(NULL, team);
	BString fallback;
	for (int32 index = 0; index < 12; index++) {
		BMessage get(B_GET_PROPERTY);
		get.AddSpecifier("Title");
		get.AddSpecifier("Window", index);
		BMessage reply;
		BString title;
		if (app.SendMessage(&get, &reply, 2000000, 2000000) != B_OK
			|| reply.FindString("result", &title) != B_OK)
			continue;
		if (title.FindFirst("types ") >= 0 || title.FindFirst("loading") >= 0)
			return title;
		if (title != "Downloads" && fallback.IsEmpty())
			fallback = title;
	}
	return fallback;
}


static void
quit_team(team_id team)
{
	BMessenger messenger(NULL, team);
	if (messenger.IsValid())
		messenger.SendMessage(B_QUIT_REQUESTED);
	for (int i = 0; i < 20; i++) {
		if (waitpid(team, NULL, WNOHANG) == team)
			return;
		snooze(250000);
	}
	kill(team, SIGKILL);
	waitpid(team, NULL, 0);
}


/*!	Opens \a page in WebPositive and checks the clip plays and is heard.
	The first run also checks the canPlayType() answers.
*/
static void
play_page(const char* name, const char* page, bool checkTypes)
{
	BString url;
	url.SetToFormat("http://127.0.0.1:%u/%s", kPort, page);
	// WebPositive restores the previous run's windows: start afresh
	unlink("/boot/home/config/settings/WebPositive/Session");
	pid_t child = fork();
	if (child == 0) {
		execl("/boot/system/apps/WebPositive", "WebPositive", url.String(),
			(char*)NULL);
		_exit(127);
	}

	BString title;
	bigtime_t deadline = system_time() + 60000000;
	while (system_time() < deadline) {
		snooze(500000);
		title = window_title(child);
		if (title.FindFirst("playing") >= 0 || title.FindFirst("error") >= 0)
			break;
	}
	info("%s: WebPositive's title \"%s\"", name, title.String());

	if (checkTypes) {
		BString answers;
		int answered = 0;
		int supported = 0;
		for (int i = 0; kTypes[i] != NULL; i++) {
			BString key;
			key << " " << i << "=";
			int32 at = title.FindFirst(key);
			BString answer = "missing";
			if (at >= 0) {
				answered++;
				title.CopyInto(answer, at + key.Length(), 8);
				int32 space = answer.FindFirst(' ');
				if (space >= 0)
					answer.Truncate(space);
				if (answer != "no")
					supported++;
			}
			answers << kTypes[i] << ": " << answer << "; ";
		}
		report("can_play_type", answered == 7 && supported == 7,
			"%s", answers.String());
	}

	bool playing = title.FindFirst("playing") >= 0;
	report(BString(name) << "_plays", playing, "\"playing\" %s (title \"%s\")",
		playing ? "fired" : "did not fire", title.String());

	// WebPositive's sound in the mixer
	AudioStreamControl control;
	bool stream = false;
	for (int i = 0; i < 20 && !stream; i++) {
		stream = control.Update() == B_OK
			&& control.FindStream(kWebPositive) != NULL;
		if (!stream)
			snooze(250000);
	}
	Recording recording;
	status_t status = stream ? record_application(kWebPositive, 1.5,
		recording) : B_ENTRY_NOT_FOUND;
	double left = purity(recording.left, recording.rate, 440);
	double right = purity(recording.right, recording.rate, 660);
	double leftOther = purity(recording.left, recording.rate, 660);
	double rightOther = purity(recording.right, recording.rate, 440);
	double leftLevel = rms(recording.left);
	double rightLevel = rms(recording.right);
	double gaps = silent_share(recording.left, recording.rate);
	info("%s: %zu frames recorded; level %.3f left, %.3f right; 10 ms "
		"blocks without sound: %.0f %%", name, recording.left.size(),
		leftLevel, rightLevel, gaps * 100);
	// Not a check: how WebKit's player feeds its BSoundPlayer is the
	// packaged HaikuWebKit's (it decodes video in its audio callback and
	// reads one decoded chunk per callback); the Media Kit's part of that
	// path is checked on its own (<name>_kit_audio). Measured in the
	// harness: WebM audible in about 10 % of 10 ms blocks, MP4 silent.
	info("%s: WebKit's sound (packaged HaikuWebKit, not a check): %s; "
		"left: 440 Hz %.3f, 660 Hz %.3f; right: 660 Hz %.3f, 440 Hz %.3f",
		name, stream ? "in the mixer" : "not in the mixer", left, leftOther,
		right, rightOther);
	(void)status;

	printf("CE-HOST screenshot html5_%s\n", name);
	fflush(stdout);
	snooze(1500000);
	quit_team(child);
	snooze(1000000);
}


/*!	HaikuWebKit's audio path without WebKit: BMediaFile(BUrl), the audio
	track's decoded format given to a BSoundPlayer as is, and one
	ReadFrames() per callback, as MediaPlayerPrivateHaiku does. Tells the
	Media Kit's share of what the browser plays from WebKit's.
*/
struct PlayerState {
	BMediaTrack*	track;
	size_t			frameSize;
	int32			calls;
	int32			shortCalls;		// fewer frames than the buffer holds
	int32			longCalls;		// more: WebKit would overrun
	int64			frames;
	size_t			bufferSize;
};


static void
player_callback(void* cookie, void* buffer, size_t size,
	const media_raw_audio_format&)
{
	PlayerState* state = (PlayerState*)cookie;
	state->calls++;
	state->bufferSize = size;
	// a scratch buffer big enough for any chunk, so that a long one does
	// not overrun the player's (WebKit reads straight into it)
	static char scratch[1 << 20];
	int64 frames = 0;
	media_header header;
	if (state->track->ReadFrames(scratch, &frames, &header) != B_OK)
		frames = 0;
	size_t bytes = frames * state->frameSize;
	if (bytes < size)
		state->shortCalls++;
	else if (bytes > size)
		state->longCalls++;
	memcpy(buffer, scratch, min_c(bytes, size));
	if (bytes < size)
		memset((char*)buffer + bytes, 0, size - bytes);
	state->frames += frames;
}


/*!	Renga (roadmap F-61): the HTTP streamer's start-up race. When the first
	data came before BMediaFile's open had got to waiting for it, the open
	failed with "No handler" (html5_media.webm_kit_audio, once). The local
	server answers at once, so opening the clip again and again makes that
	order likely; every open must find the audio track.
*/
static void
check_repeated_open(const char* clip)
{
	BString url;
	url.SetToFormat("http://127.0.0.1:%u/%s", kPort, clip);
	const int kOpens = 20;
	int good = 0;
	status_t firstError = B_OK;
	for (int i = 0; i < kOpens; i++) {
		BMediaFile file(BUrl(url.String(), false));
		bool audio = false;
		for (int32 t = 0; file.InitCheck() == B_OK && t < file.CountTracks();
				t++) {
			BMediaTrack* track = file.TrackAt(t);
			media_format decoded;
			if (track != NULL && track->DecodedFormat(&decoded) == B_OK
				&& decoded.IsAudio())
				audio = true;
			if (track != NULL)
				file.ReleaseTrack(track);
		}
		if (audio)
			good++;
		else if (firstError == B_OK)
			firstError = file.InitCheck() != B_OK ? file.InitCheck() : B_ERROR;
	}
	report("url_open_repeated", good == kOpens, "%d of %d opens of %s found "
		"the audio track%s%s", good, kOpens, clip,
		firstError != B_OK ? "; first failure: " : "",
		firstError != B_OK ? strerror(firstError) : "");
}


static void
check_webkit_audio_path(const char* name, const char* clip)
{
	BString url;
	url.SetToFormat("http://127.0.0.1:%u/%s", kPort, clip);
	BMediaFile file(BUrl(url.String(), false));
	BMediaTrack* audio = NULL;
	media_format format;
	for (int32 i = 0; file.InitCheck() == B_OK && i < file.CountTracks();
			i++) {
		BMediaTrack* track = file.TrackAt(i);
		media_format decoded;
		if (track != NULL && track->DecodedFormat(&decoded) == B_OK
			&& decoded.IsAudio() && audio == NULL) {
			audio = track;
			format = decoded;
		} else if (track != NULL)
			file.ReleaseTrack(track);
	}
	if (audio == NULL) {
		report(BString(name) << "_kit_audio", false, "no audio track (%s)",
			strerror(file.InitCheck()));
		return;
	}
	const media_raw_audio_format& raw = format.u.raw_audio;
	PlayerState state = { audio, (size_t)raw.channel_count
		* (raw.format & media_raw_audio_format::B_AUDIO_SIZE_MASK), 0, 0, 0,
		0, 0 };
	BSoundPlayer player(&raw, "html5 path", player_callback, NULL, &state);
	player.Start();
	player.SetHasData(true);
	snooze(1000000);
	Recording recording;
	status_t status = record_application(
		"application/x-vnd.Renga-html5-media-test", 1.5, recording);
	player.Stop();
	double left = purity(recording.left, recording.rate, 440);
	double right = purity(recording.right, recording.rate, 660);
	double gaps = silent_share(recording.left, recording.rate);
	info("%s: decoded format %.0f Hz, %" B_PRIu32 " channels, format 0x%"
		B_PRIx32 ", buffer_size %zu; the player's buffers %zu bytes; %"
		B_PRId32 " callbacks, %" B_PRId32 " short, %" B_PRId32 " long, %"
		B_PRId64 " frames", name, raw.frame_rate, raw.channel_count,
		raw.format, raw.buffer_size, state.bufferSize, state.calls,
		state.shortCalls, state.longCalls, state.frames);
	report(BString(name) << "_kit_audio", status == B_OK && left > 0.8
			&& right > 0.8 && gaps < 0.05,
		"the Media Kit part of WebKit's audio path: loopback %s; 440 Hz "
		"%.3f of the left, 660 Hz %.3f of the right, %.0f %% of 10 ms "
		"blocks silent (want over 0.8 and under 5 %%)", strerror(status),
		left, right, gaps * 100);
	file.ReleaseTrack(audio);
}


/*!	A numeric address resolves at once on a machine without a network:
	libbnetapi passed AI_ADDRCONFIG to getaddrinfo(), which then skipped the
	numeric form (no configured IPv4 address besides loopback) and asked
	DNS, so the HTTP streamer took 56 s to fail on http://127.0.0.1/.
*/
static void
check_loopback_address()
{
	bigtime_t start = system_time();
	BNetworkAddress address("127.0.0.1", kPort);
	bigtime_t elapsed = system_time() - start;
	BString text = address.InitCheck() == B_OK ? address.ToString() : "";
	report("loopback_address", address.InitCheck() == B_OK
			&& address.Family() == AF_INET && elapsed < 1000000,
		"BNetworkAddress(\"127.0.0.1\", %u): %s, \"%s\" in %.2f s (want "
		"IPv4 in under 1 s; the guest has no network)", kPort,
		strerror(address.InitCheck()), text.String(), elapsed / 1e6);
}


/*!	The Media Kit part WebKit relies on: HaikuWebKit's player opens the
	element's URL with BMediaFile(BUrl) (the streamer add-ons fetch it over
	HTTP) and decodes the tracks itself. This checks that path without the
	browser: the clip opens over HTTP and both tracks decode.
*/
static void
check_media_url(const char* name, const char* clip)
{
	BString url;
	url.SetToFormat("http://127.0.0.1:%u/%s", kPort, clip);
	int32 requestsBefore = atomic_get(&sRequests);
	bigtime_t start = system_time();
	BMediaFile* file = new BMediaFile(BUrl(url.String(), false));
	status_t status = file->InitCheck();
	bigtime_t opened = system_time() - start;
	int32 tracks = status == B_OK ? file->CountTracks() : 0;
	int64 videoFrames = 0;
	int64 audioFrames = 0;
	for (int32 i = 0; i < tracks; i++) {
		BMediaTrack* track = file->TrackAt(i);
		media_format format;
		if (track == NULL || track->DecodedFormat(&format) != B_OK)
			continue;
		size_t size = format.IsVideo()
			? (size_t)format.Width() * format.Height() * 4
			: (size_t)format.u.raw_audio.buffer_size;
		if (format.IsVideo() && size == 0)
			continue;
		if (!format.IsVideo())
			size = max_c(size, (size_t)65536);
		std::vector<char> buffer(size);
		for (int k = 0; k < 10; k++) {
			int64 frames = 0;
			media_header header;
			if (track->ReadFrames(buffer.data(), &frames, &header) != B_OK)
				break;
			if (format.IsVideo())
				videoFrames += frames;
			else
				audioFrames += frames;
		}
		file->ReleaseTrack(track);
	}
	delete file;
	int32 requests = atomic_get(&sRequests) - requestsBefore;
	if (status != B_OK) {
		info("%s: %" B_PRId32 " connections accepted so far", name,
			atomic_get(&sAccepted));
		system("ls /boot/system/add-ons/media/plugins "
			"/boot/system/non-packaged/add-ons/media/plugins 2>&1 "
			"| sed 's/^/CE-INFO html5_media: plugins: /'");
	}
	report(BString(name) << "_media_url", status == B_OK && tracks == 2
			&& videoFrames > 0 && audioFrames > 0,
		"BMediaFile(%s): %s in %.1f s, %" B_PRId32 " tracks, %" B_PRId64
		" video and %" B_PRId64 " audio frames decoded, %" B_PRId32
		" HTTP requests", url.String(), strerror(status), opened / 1e6, tracks,
		videoFrames, audioFrames, requests);
}


int
main()
{
	signal(SIGPIPE, SIG_IGN);
	BApplication app("application/x-vnd.Renga-html5-media-test");
	printf("CE-SUITE " SUITE " START\n");
	fflush(stdout);

	if (!be_roster->IsRunning("application/x-vnd.Be-input_server")) {
		info("no desktop, nothing to show");
		printf("CE-SUITE " SUITE " DONE failures=0\n");
		return 0;
	}

	mkdir(kDirectory, 0755);
	status_t webm = write_clip("clip.webm", "webm", "vp9", "opus", 12);
	status_t mp4 = write_clip("clip.mp4", "mp4", "mpeg4", "aac", 12);
	if (mp4 == B_OK)
		mp4 = make_faststart("clip.mp4");
	status_t pages = write_page("webm.html", "clip.webm") == B_OK
		&& write_page("mp4.html", "clip.mp4") == B_OK ? B_OK : B_ERROR;
	bool server = start_server();
	report("setup", webm == B_OK && mp4 == B_OK && pages == B_OK && server,
		"clips: WebM %s, MP4 %s; pages %s; HTTP server on 127.0.0.1:%u %s",
		strerror(webm), strerror(mp4), strerror(pages), kPort,
		server ? "up" : "failed");
	if (server) {
		check_loopback_address();
		check_media_url("webm", "clip.webm");
		check_media_url("mp4", "clip.mp4");
		check_repeated_open("clip.webm");
		check_webkit_audio_path("webm", "clip.webm");
		check_webkit_audio_path("mp4", "clip.mp4");
		play_page("webm", "webm.html", true);
		play_page("mp4", "mp4.html", false);
		info("%" B_PRId32 " HTTP requests", atomic_get(&sRequests));
	}
	info("known limits of HaikuWebKit 1.10.0 (packaged, not built here): a "
		"video without an audio track does not advance (fix in RengaPorts); "
		"its sound has gaps or is missing although the Media Kit's part "
		"of its path plays cleanly (see *_kit_audio)");

	printf("CE-SUITE " SUITE " DONE failures=%d\n", sFailures);
	fflush(stdout);
	return sFailures == 0 ? 0 : 1;
}
