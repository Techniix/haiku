/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 */

/*!	Tests the UVC payload code of the usb_webcam media add-on
	(addons/uvc/UVCPayload.cpp) with payloads made up here, since QEMU has
	no USB video device: frames of a known pattern split into payloads of
	at most 1024 bytes with 12-byte headers (PTS and SCR), as a camera's
	isochronous packets carry them, and the cases a real stream has: the
	FID toggling every frame, EOF set or not (some cameras only toggle),
	payloads with only a header, a payload with the ERR bit, a lost
	payload, invalid headers, and a stream joined in the middle of a
	frame. Also the YUY2 to B_RGB32 conversion (BT.601 colours) and the
	Huffman tables MJPEG frames leave out.
*/


#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <vector>

#include "UVCPayload.h"


#define SUITE "uvc"

static int sFailures = 0;
static const uint32 kWidth = 64;
static const uint32 kHeight = 48;
static const size_t kFrameSize = kWidth * kHeight * 2;


static void
report(const char* name, bool pass, const char* format, ...)
{
	char detail[256];
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


static std::vector<uint8>
make_frame(int number)
{
	std::vector<uint8> frame(kFrameSize);
	for (size_t i = 0; i < kFrameSize; i++)
		frame[i] = (uint8)(i * 7 + number * 31);
	return frame;
}


enum {
	kEOF = 1 << 0,
	kNoEOF = 1 << 1,		// the camera only toggles FID
	kError = 1 << 2,		// ERR in the third payload
	kLose = 1 << 3,			// the third payload never arrives
	kHeaderOnly = 1 << 4	// a header-only payload after every one
};


//!	The payloads of one frame.
static std::vector<std::vector<uint8> >
packetize(const std::vector<uint8>& frame, int fid, uint32 flags)
{
	std::vector<std::vector<uint8> > payloads;
	const size_t kMax = 1024 - 12;
	for (size_t offset = 0, index = 0; offset < frame.size();
			offset += kMax, index++) {
		size_t length = frame.size() - offset < kMax ? frame.size() - offset
			: kMax;
		bool last = offset + length == frame.size();
		std::vector<uint8> payload(12);
		payload[0] = 12;
		payload[1] = 0x80 | 0x08 | 0x04 | fid;	// EOH, SCR, PTS, FID
		if (last && (flags & kNoEOF) == 0)
			payload[1] |= 0x02;
		if (index == 2 && (flags & kError) != 0)
			payload[1] |= 0x40;
		uint32 pts = 1000 + fid;
		memcpy(&payload[2], &pts, 4);
		payload.insert(payload.end(), frame.begin() + offset,
			frame.begin() + offset + length);
		if (index == 2 && (flags & kLose) != 0)
			continue;
		payloads.push_back(payload);
		if ((flags & kHeaderOnly) != 0) {
			std::vector<uint8> header(payload.begin(), payload.begin() + 12);
			header[1] &= ~0x02;
			payloads.push_back(header);
		}
	}
	return payloads;
}


struct Result {
	std::vector<std::vector<uint8> > frames;
};


static void
feed(UVCPayloadParser& parser, const std::vector<std::vector<uint8> >& payloads,
	Result& result)
{
	for (size_t i = 0; i < payloads.size(); i++) {
		if (parser.Write(payloads[i].data(), payloads[i].size()))
			result.frames.push_back(parser.Frame());
	}
}


static void
test_parser()
{
	// three good frames, FID toggling, EOF set
	UVCPayloadParser parser(kFrameSize * 2);
	parser.SetFrameSize(kFrameSize);
	Result result;
	for (int i = 0; i < 3; i++)
		feed(parser, packetize(make_frame(i), i & 1, kEOF), result);
	bool exact = result.frames.size() == 3;
	for (size_t i = 0; exact && i < 3; i++)
		exact = result.frames[i] == make_frame(i);
	report("frames", exact && parser.PresentationTime() == 1000,
		"%zu of 3 frames, %s", result.frames.size(),
		exact ? "bit-exact" : "wrong data");

	// header-only payloads in between change nothing
	parser.Reset();
	result.frames.clear();
	for (int i = 0; i < 2; i++)
		feed(parser, packetize(make_frame(i), i & 1, kHeaderOnly), result);
	report("header_only_payloads", result.frames.size() == 2
			&& result.frames[1] == make_frame(1),
		"%zu of 2 frames", result.frames.size());

	// no EOF: frames end when the FID toggles
	parser.Reset();
	result.frames.clear();
	for (int i = 0; i < 4; i++)
		feed(parser, packetize(make_frame(i), i & 1, kNoEOF), result);
	report("fid_toggle_without_eof", result.frames.size() == 3
			&& result.frames[2] == make_frame(2),
		"%zu frames from 4 (the last ends with the next)",
		result.frames.size());

	// ERR in one payload: that frame is dropped, the next is fine
	parser.Reset();
	result.frames.clear();
	feed(parser, packetize(make_frame(0), 0, kError), result);
	feed(parser, packetize(make_frame(1), 1, kEOF), result);
	report("error_bit_drops_frame", result.frames.size() == 1
			&& result.frames[0] == make_frame(1) && parser.droppedErrors == 1,
		"%zu frame(s), %u dropped for errors", result.frames.size(),
		parser.droppedErrors);

	// a lost payload: the frame is short and dropped
	parser.Reset();
	result.frames.clear();
	feed(parser, packetize(make_frame(0), 0, kLose), result);
	feed(parser, packetize(make_frame(1), 1, kEOF), result);
	report("short_frame_dropped", result.frames.size() == 1
			&& parser.droppedSize == 1,
		"%zu frame(s), %u dropped for their size", result.frames.size(),
		parser.droppedSize);

	// joined in the middle of a frame: its tail is dropped, then sync
	parser.Reset();
	result.frames.clear();
	std::vector<std::vector<uint8> > first = packetize(make_frame(0), 0, kEOF);
	first.erase(first.begin(), first.begin() + 3);
	feed(parser, first, result);
	feed(parser, packetize(make_frame(1), 1, kEOF), result);
	feed(parser, packetize(make_frame(2), 0, kEOF), result);
	report("joined_mid_frame", result.frames.size() == 2
			&& result.frames[0] == make_frame(1)
			&& result.frames[1] == make_frame(2),
		"%zu frames after joining late (%u size drops)", result.frames.size(),
		parser.droppedSize);

	// invalid headers are ignored
	parser.Reset();
	result.frames.clear();
	uint8 tooLong[4] = { 12, 0x80, 0, 0 };
	uint8 tooShort[4] = { 1, 0x80, 0, 0 };
	parser.Write(tooLong, sizeof(tooLong));
	parser.Write(tooShort, sizeof(tooShort));
	parser.Write(tooShort, 1);
	feed(parser, packetize(make_frame(5), 1, kEOF), result);
	report("invalid_headers", parser.invalidHeaders == 3
			&& result.frames.size() == 1 && result.frames[0] == make_frame(5),
		"%u invalid header(s) ignored, %zu frame(s)", parser.invalidHeaders,
		result.frames.size());

	// MJPEG: variable frame size, delivered as they come
	UVCPayloadParser mjpeg(kFrameSize);
	mjpeg.SetFrameSize(0);
	result.frames.clear();
	std::vector<uint8> small(3000, 0x55);
	feed(mjpeg, packetize(small, 0, kEOF), result);
	report("variable_size_frames", result.frames.size() == 1
			&& result.frames[0].size() == 3000,
		"%zu frame(s) of %zu bytes", result.frames.size(),
		result.frames.empty() ? 0 : result.frames[0].size());
}


static void
test_yuy2()
{
	struct {
		uint8 y, u, v;
		uint8 r, g, b;
		const char* name;
	} colours[] = {
		{ 235, 128, 128, 255, 255, 255, "white" },
		{ 16, 128, 128, 0, 0, 0, "black" },
		{ 81, 90, 240, 255, 0, 0, "red" },
		{ 145, 54, 34, 0, 255, 0, "green" },
		{ 41, 240, 110, 0, 0, 255, "blue" },
		{ 126, 128, 128, 128, 128, 128, "grey" }
	};
	for (size_t i = 0; i < sizeof(colours) / sizeof(colours[0]); i++) {
		uint8 yuy2[8] = { colours[i].y, colours[i].u, colours[i].y,
			colours[i].v, colours[i].y, colours[i].u, colours[i].y,
			colours[i].v };
		uint8 rgb[16];
		uvc_yuy2_to_rgb32(yuy2, rgb, 4, 1, 16);
		bool ok = true;
		for (int p = 0; p < 4; p++) {
			ok &= abs(rgb[p * 4 + 2] - colours[i].r) <= 3
				&& abs(rgb[p * 4 + 1] - colours[i].g) <= 3
				&& abs(rgb[p * 4 + 0] - colours[i].b) <= 3
				&& rgb[p * 4 + 3] == 255;
		}
		char name[48];
		snprintf(name, sizeof(name), "yuy2_%s", colours[i].name);
		report(name, ok, "Y %u U %u V %u -> R %u G %u B %u (want %u %u %u)",
			colours[i].y, colours[i].u, colours[i].v, rgb[2], rgb[1], rgb[0],
			colours[i].r, colours[i].g, colours[i].b);
	}

	// stride: rows land on bytesPerRow, padding untouched
	std::vector<uint8> frame(kWidth * kHeight * 2, 128);
	std::vector<uint8> out((kWidth * 4 + 16) * kHeight, 0xaa);
	uvc_yuy2_to_rgb32(frame.data(), out.data(), kWidth, kHeight,
		kWidth * 4 + 16);
	report("yuy2_stride", out[kWidth * 4] == 0xaa && out[kWidth * 4 + 15]
			== 0xaa && out[kWidth * 4 + 16 + 3] == 255,
		"row padding kept, next row starts at bytesPerRow");
}


static void
test_mjpeg()
{
	// SOI, DQT (67 bytes), SOF0 (17 bytes), SOS (12 bytes), data, EOI
	std::vector<uint8> jpeg = { 0xff, 0xd8 };
	jpeg.insert(jpeg.end(), { 0xff, 0xdb, 0x00, 0x43, 0x00 });
	for (int i = 0; i < 64; i++)
		jpeg.push_back(1);
	jpeg.insert(jpeg.end(), { 0xff, 0xc0, 0x00, 0x11, 0x08, 0x00, 0x30, 0x00,
		0x40, 0x03, 0x01, 0x21, 0x00, 0x02, 0x11, 0x00, 0x03, 0x11, 0x00 });
	size_t scan = jpeg.size();
	jpeg.insert(jpeg.end(), { 0xff, 0xda, 0x00, 0x0c, 0x03, 0x01, 0x00, 0x02,
		0x11, 0x03, 0x11, 0x00, 0x3f, 0x00 });
	jpeg.insert(jpeg.end(), { 0x12, 0x34, 0x56, 0xff, 0xd9 });

	std::vector<uint8> fixed;
	bool ok = uvc_mjpeg_add_huffman_tables(jpeg.data(), jpeg.size(), fixed);
	bool inserted = ok && fixed.size() == jpeg.size() + 420
		&& memcmp(fixed.data(), jpeg.data(), scan) == 0
		&& fixed[scan] == 0xff && fixed[scan + 1] == 0xc4
		&& fixed[scan + 2] == 0x01 && fixed[scan + 3] == 0xa2
		&& memcmp(fixed.data() + scan + 420, jpeg.data() + scan,
			jpeg.size() - scan) == 0
		&& uvc_mjpeg_has_huffman_tables(fixed.data(), fixed.size());
	report("mjpeg_huffman_inserted", inserted, "%zu -> %zu bytes, DHT (418) "
		"before the scan", jpeg.size(), fixed.size());

	std::vector<uint8> again;
	ok = uvc_mjpeg_add_huffman_tables(fixed.data(), fixed.size(), again);
	report("mjpeg_tables_kept", ok && again == fixed, "a frame with tables "
		"stays as it is");

	uint8 garbage[16] = { 0x12, 0x34 };
	report("mjpeg_rejects_garbage", !uvc_mjpeg_add_huffman_tables(garbage,
		sizeof(garbage), again), "not a JPEG");
}


int
main()
{
	printf("CE-SUITE " SUITE " START\n");
	fflush(stdout);
	test_parser();
	test_yuy2();
	test_mjpeg();
	printf("CE-SUITE " SUITE " DONE failures=%d\n", sFailures);
	fflush(stdout);
	return sFailures == 0 ? 0 : 1;
}
