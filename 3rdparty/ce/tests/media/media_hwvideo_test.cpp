/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 */

/*!	Hardware video plumbing (roadmap F-31 phase 5,
	headers/private/media/VideoAcceleration.h) with the "software"
	reference back end, which implements the interface with FFmpeg in
	software (QEMU has no video hardware):

	- the roster loads the back ends, does not choose "software" by itself
	  (it is no hardware), chooses it by name through
	  RENGA_VIDEO_ACCELERATION, and chooses nothing with "off";
	- the back end's capabilities (profiles, entry points, sizes, NV12);
	- encoding through the interface: NV12 surfaces (one colour per
	  second) to VP9 packets, key frames where asked;
	- decoding through the interface: those packets back to NV12
	  surfaces, in order, in their colours, in areas;
	- the FFmpeg plug-in: MediaReader decodes a VP9 WebM file through the
	  back end when it is chosen (the codec name says so, every frame in
	  its colour) and in software otherwise; MediaWriter encodes through
	  it (the plug-in says so on stderr) and the file decodes again.
*/


#include <Application.h>
#include <OS.h>
#include <String.h>

#include <MediaCodec.h>
#include <VideoAcceleration.h>

#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <vector>


#define SUITE "video_accel"

using namespace BPrivate;

static const int32 kWidth = 160;
static const int32 kHeight = 120;
static const float kFrameRate = 15;
static const int32 kSeconds = 2;

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


// one colour per second: red, green (as RGB32 and as BT.601 YCbCr)
static uint32
second_rgb(int32 second)
{
	return second % 2 == 0 ? 0xffd02020 : 0xff20d020;
}


static void
rgb_to_yuv(uint32 rgb, uint8* y, uint8* u, uint8* v)
{
	int r = (rgb >> 16) & 0xff, g = (rgb >> 8) & 0xff, b = rgb & 0xff;
	*y = (uint8)(16 + (65.738 * r + 129.057 * g + 25.064 * b) / 256);
	*u = (uint8)(128 + (-37.945 * r - 74.494 * g + 112.439 * b) / 256);
	*v = (uint8)(128 + (112.439 * r - 94.154 * g - 18.285 * b) / 256);
}


static bool
near(int a, int b, int tolerance)
{
	return abs(a - b) <= tolerance;
}


static bool
rgb_near(uint32 a, uint32 b, int tolerance)
{
	for (int shift = 0; shift < 24; shift += 8) {
		if (!near((a >> shift) & 0xff, (b >> shift) & 0xff, tolerance))
			return false;
	}
	return true;
}


struct Packet {
	std::vector<uint8>	data;
	bigtime_t			time;
	bool				key;
};


static void
test_roster()
{
	unsetenv(RENGA_VIDEO_ACCELERATION_ENVIRONMENT);
	VideoAcceleratorRoster* roster = VideoAcceleratorRoster::Default();
	int32 count = roster->CountAccelerators();
	VideoAccelerator* software = NULL;
	BString names;
	for (int32 i = 0; i < count; i++) {
		VideoAccelerator* accelerator = roster->AcceleratorAt(i);
		names << (i > 0 ? ", " : "") << accelerator->Name() << " ("
			<< accelerator->DeviceName() << ")";
		if (strcmp(accelerator->Name(), "software") == 0)
			software = accelerator;
	}
	report("roster", software != NULL && !software->IsHardware(),
		"%" B_PRId32 " back end(s): %s", count, names.String());

	VideoAccelerator* automatic = roster->Find(B_VIDEO_ACCEL_VP9_PROFILE0,
		B_VIDEO_ACCEL_DECODE, kWidth, kHeight);
	report("auto_skips_software", automatic == NULL
			|| automatic->IsHardware(),
		"choice \"%s\": %s", roster->Choice().String(),
		automatic != NULL ? automatic->Name() : "software decoding in the "
			"plug-in (no video hardware)");

	setenv(RENGA_VIDEO_ACCELERATION_ENVIRONMENT, "software", 1);
	VideoAccelerator* chosen = roster->Find(B_VIDEO_ACCEL_VP9_PROFILE0,
		B_VIDEO_ACCEL_DECODE, kWidth, kHeight);
	setenv(RENGA_VIDEO_ACCELERATION_ENVIRONMENT, "off", 1);
	VideoAccelerator* off = roster->Find(B_VIDEO_ACCEL_VP9_PROFILE0,
		B_VIDEO_ACCEL_DECODE, kWidth, kHeight);
	report("choice_by_name", chosen == software && chosen != NULL
			&& off == NULL,
		"\"software\": %s; \"off\": %s", chosen != NULL ? chosen->Name()
			: "none", off != NULL ? off->Name() : "none");

	BString list;
	bool decode = true;
	bool encode = true;
	if (software != NULL) {
		for (int32 i = 0; i < software->CountCapabilities(); i++) {
			const VideoAccelCapability* capability = software->CapabilityAt(i);
			list << VideoAcceleratorRoster::ProfileName(capability->profile)
				<< ((capability->entrypoints & B_VIDEO_ACCEL_DECODE) != 0
					? " D" : "")
				<< ((capability->entrypoints & B_VIDEO_ACCEL_ENCODE) != 0
					? "E" : "") << "; ";
		}
		decode = software->Supports(B_VIDEO_ACCEL_VP9_PROFILE0,
				B_VIDEO_ACCEL_DECODE, 1920, 1080)
			&& software->Supports(B_VIDEO_ACCEL_H264_HIGH,
				B_VIDEO_ACCEL_DECODE)
			&& !software->Supports(B_VIDEO_ACCEL_VP9_PROFILE0,
				B_VIDEO_ACCEL_DECODE, 8192, 8192);
		encode = software->Supports(B_VIDEO_ACCEL_VP9_PROFILE0,
				B_VIDEO_ACCEL_ENCODE)
			&& software->Supports(B_VIDEO_ACCEL_VP8, B_VIDEO_ACCEL_ENCODE)
			&& !software->Supports(B_VIDEO_ACCEL_H264_HIGH,
				B_VIDEO_ACCEL_ENCODE);
	}
	report("capabilities", software != NULL && decode && encode,
		"%s", list.String());
}


static void
test_interface(std::vector<Packet>& packets)
{
	setenv(RENGA_VIDEO_ACCELERATION_ENVIRONMENT, "software", 1);
	VideoAccelerator* accelerator = VideoAcceleratorRoster::Default()->Find(
		B_VIDEO_ACCEL_VP9_PROFILE0, B_VIDEO_ACCEL_ENCODE, kWidth, kHeight);
	if (accelerator == NULL) {
		report("encode", false, "no back end encodes VP9");
		return;
	}

	VideoAccelEncoderSetup setup;
	setup.profile = B_VIDEO_ACCEL_VP9_PROFILE0;
	setup.width = kWidth;
	setup.height = kHeight;
	setup.frameRate = kFrameRate;
	setup.bitRate = 0;
	setup.quality = 0.8f;
	setup.keyFrameInterval = 0;
	setup.format = B_VIDEO_ACCEL_NV12;
	VideoAccelEncoder* encoder = NULL;
	status_t status = accelerator->CreateEncoder(setup, &encoder);

	int32 frames = kSeconds * (int32)kFrameRate;
	int32 sent = 0;
	int32 areas = 0;
	std::vector<uint8> buffer(1024 * 1024);
	for (int32 f = 0; status == B_OK && f <= frames; f++) {
		if (f < frames) {
			VideoAccelSurface surface;
			status = encoder->AcquireSurface(&surface);
			if (status != B_OK)
				break;
			if (surface.area >= 0)
				areas++;
			uint8 y, u, v;
			rgb_to_yuv(second_rgb(f / (int32)kFrameRate), &y, &u, &v);
			for (int32 row = 0; row < kHeight; row++)
				memset(surface.planes[0] + row * surface.bytesPerRow[0], y,
					kWidth);
			for (int32 row = 0; row < kHeight / 2; row++) {
				uint8* line = surface.planes[1] + row * surface.bytesPerRow[1];
				for (int32 x = 0; x < kWidth / 2; x++) {
					line[x * 2] = u;
					line[x * 2 + 1] = v;
				}
			}
			surface.timestamp = (bigtime_t)(f * 1000000 / kFrameRate);
			status = encoder->SendSurface(&surface,
				f % (int32)kFrameRate == 0);
			sent++;
		} else
			status = encoder->SendSurface(NULL, false);
		while (status == B_OK) {
			size_t size = buffer.size();
			Packet packet;
			status_t received = encoder->ReceivePacket(buffer.data(), &size,
				&packet.time, &packet.key);
			if (received != B_OK)
				break;
			packet.data.assign(buffer.begin(), buffer.begin() + size);
			packets.push_back(packet);
		}
	}
	int32 keys = 0;
	for (size_t i = 0; i < packets.size(); i++)
		keys += packets[i].key ? 1 : 0;
	report("encode", status == B_OK && (int32)packets.size() >= frames - 1
			&& !packets.empty() && packets[0].key && keys >= kSeconds,
		"%s through %s: %s; %" B_PRId32 " surfaces sent (%" B_PRId32 " in "
		"areas), %" B_PRIuSIZE " packets, %" B_PRId32 " key frames (asked "
		"for one a second)", VideoAcceleratorRoster::ProfileName(
			setup.profile), accelerator->Name(), strerror(status), sent,
		areas, packets.size(), keys);
	delete encoder;

	VideoAccelDecoderSetup decodeSetup;
	decodeSetup.profile = B_VIDEO_ACCEL_VP9_PROFILE0;
	decodeSetup.width = kWidth;
	decodeSetup.height = kHeight;
	decodeSetup.extradata = NULL;
	decodeSetup.extradataSize = 0;
	decodeSetup.surfaces = 0;
	VideoAccelDecoder* decoder = NULL;
	status = accelerator->CreateDecoder(decodeSetup, &decoder);
	int32 decoded = 0;
	int32 right = 0;
	bool ordered = true;
	bigtime_t last = -1;
	for (size_t i = 0; status == B_OK && i <= packets.size(); i++) {
		if (i < packets.size()) {
			status = decoder->SendPacket(packets[i].data.data(),
				packets[i].data.size(), packets[i].time, packets[i].key);
		} else
			status = decoder->SendPacket(NULL, 0, 0, false);
		while (status == B_OK) {
			VideoAccelSurface surface;
			if (decoder->ReceiveSurface(&surface) != B_OK)
				break;
			uint8 y, u, v;
			rgb_to_yuv(second_rgb(decoded / (int32)kFrameRate), &y, &u, &v);
			const uint8* uv = surface.planes[1]
				+ kHeight / 4 * surface.bytesPerRow[1] + kWidth / 2;
			uint8 cy = surface.planes[0][kHeight / 2 * surface.bytesPerRow[0]
				+ kWidth / 2];
			if (surface.format == B_VIDEO_ACCEL_NV12 && surface.area >= 0
				&& near(cy, y, 12) && near(uv[0], u, 12)
				&& near(uv[1], v, 12)) {
				right++;
			}
			if (surface.timestamp <= last)
				ordered = false;
			last = surface.timestamp;
			decoder->ReleaseSurface(&surface);
			decoded++;
		}
	}
	report("decode", status == B_OK && decoded == (int32)packets.size()
			&& right == decoded && ordered,
		"%s: %" B_PRId32 " NV12 surfaces from %" B_PRIuSIZE " packets, %"
		B_PRId32 " in their colour, timestamps %s", strerror(status),
		decoded, packets.size(), right, ordered ? "in order" : "out of order");
	delete decoder;
}


/*!	Decodes \a path with MediaReader; returns the frames, how many in their
	second's colour, and the codec name.
*/
static int32
decode_file(const char* path, int32* _right, BString* _codec,
	status_t* _status)
{
	MediaReader reader;
	*_right = 0;
	*_status = reader.Open(path);
	if (*_status != B_OK)
		return 0;
	int32 video = reader.FindTrack(B_MEDIA_RAW_VIDEO);
	MediaTrackInfo info;
	if (video < 0 || reader.GetTrackInfo(video, &info) != B_OK) {
		*_status = B_MEDIA_NO_HANDLER;
		return 0;
	}
	*_codec = info.codec;
	std::vector<uint32> bits(kWidth * kHeight);
	int32 frames = 0;
	bigtime_t time;
	while (frames < 1000 && reader.ReadVideoFrame(video, bits.data(),
			kWidth * 4, &time) == B_OK) {
		if (rgb_near(bits[kHeight / 2 * kWidth + kWidth / 2],
				second_rgb(frames / (int32)kFrameRate), 40))
			(*_right)++;
		frames++;
	}
	return frames;
}


//!	Writes a VP9 WebM file; returns what the plug-in said on stderr.
static status_t
write_file(const char* path, BString* _log)
{
	char logPath[] = "/tmp/video_accel_stderr.txt";
	fflush(stderr);
	int saved = dup(STDERR_FILENO);
	int log = open(logPath, O_CREAT | O_TRUNC | O_WRONLY, 0644);
	dup2(log, STDERR_FILENO);

	MediaWriter writer;
	status_t status = writer.Open(path, "webm");
	int32 video = -1;
	if (status == B_OK)
		status = writer.AddVideoTrack("vp9", kWidth, kHeight, kFrameRate,
			&video);
	if (status == B_OK)
		status = writer.Start();
	std::vector<uint32> bits(kWidth * kHeight);
	for (int32 f = 0; status == B_OK && f < kSeconds * kFrameRate; f++) {
		for (int32 i = 0; i < kWidth * kHeight; i++)
			bits[i] = second_rgb(f / (int32)kFrameRate);
		status = writer.WriteVideoFrame(video, bits.data(), kWidth * 4,
			f % (int32)kFrameRate == 0);
	}
	status_t finish = writer.Finish();

	fflush(stderr);
	dup2(saved, STDERR_FILENO);
	close(saved);
	close(log);
	FILE* file = fopen(logPath, "r");
	char line[256];
	while (file != NULL && fgets(line, sizeof(line), file) != NULL)
		*_log << line;
	if (file != NULL)
		fclose(file);
	unlink(logPath);
	return status == B_OK ? finish : status;
}


static void
test_plugin()
{
	const char* path = "/tmp/video_accel_test.webm";

	// Encoding through the back end, in the plug-in.
	setenv(RENGA_VIDEO_ACCELERATION_ENVIRONMENT, "software", 1);
	BString log;
	status_t status = write_file(path, &log);
	bool through = log.FindFirst("through the software video accelerator")
		>= 0;
	log.ReplaceAll("\n", " | ");
	report("plugin_encode", status == B_OK && through,
		"MediaWriter vp9 with RENGA_VIDEO_ACCELERATION=software: %s; the "
		"plug-in: %s", strerror(status), log.Length() > 0 ? log.String()
			: "(nothing)");

	int32 right;
	BString codec;
	int32 frames = decode_file(path, &right, &codec, &status);
	report("plugin_decode", status == B_OK
			&& frames == kSeconds * (int32)kFrameRate && right == frames
			&& codec.FindFirst("software") >= 0,
		"MediaReader with RENGA_VIDEO_ACCELERATION=software: %s, codec "
		"\"%s\", %" B_PRId32 " frames (want %" B_PRId32 "), %" B_PRId32
		" in their colour", strerror(status), codec.String(), frames,
		kSeconds * (int32)kFrameRate, right);

	setenv(RENGA_VIDEO_ACCELERATION_ENVIRONMENT, "off", 1);
	frames = decode_file(path, &right, &codec, &status);
	report("plugin_decode_off", status == B_OK
			&& frames == kSeconds * (int32)kFrameRate && right == frames
			&& codec.FindFirst("accelerat") < 0,
		"with \"off\": %s, codec \"%s\", %" B_PRId32 " frames, %" B_PRId32
		" in their colour", strerror(status), codec.String(), frames, right);
	unlink(path);
	unsetenv(RENGA_VIDEO_ACCELERATION_ENVIRONMENT);
}


int
main()
{
	BApplication app("application/x-vnd.Renga-video-accel-test");
	printf("CE-SUITE " SUITE " START\n");
	fflush(stdout);

	test_roster();
	std::vector<Packet> packets;
	test_interface(packets);
	test_plugin();

	printf("CE-SUITE " SUITE " DONE failures=%d\n", sFailures);
	fflush(stdout);
	return sFailures == 0 ? 0 : 1;
}
