/*
 * Copyright 2011, Gabriel Hartmann, gabriel.hartmann@gmail.com.
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 */


#include "UVCDeframer.h"

#include "CamDebug.h"
#include "CamDevice.h"

#include <Autolock.h>


#define MAXFRAMEBUF CAMDEFRAMER_MAX_QUEUED_FRAMES

// 1920 x 1080 YUY2; MJPEG frames are smaller
static const size_t kMaxFrameSize = 1920 * 1080 * 2;


UVCDeframer::UVCDeframer(CamDevice* device)
	: CamDeframer(device),
	fParser(kMaxFrameSize)
{
}


UVCDeframer::~UVCDeframer()
{
}


void
UVCDeframer::SetFrameSize(size_t size)
{
	BAutolock l(fLocker);
	fParser.SetFrameSize(size);
	fParser.Reset();
}


ssize_t
UVCDeframer::Write(const void* buffer, size_t size)
{
	BAutolock l(fLocker);
	if (!fParser.Write((const uint8*)buffer, size))
		return size;

	if (fFrames.CountItems() >= MAXFRAMEBUF) {
		// the producer is behind: the newest frame waits for room
		return size;
	}
	CamFrame* frame = AllocFrame();
	if (frame == NULL)
		return size;
	const std::vector<uint8>& data = fParser.Frame();
	frame->Write(data.data(), data.size());
	fFrames.AddItem(frame);
	release_sem(fFrameSem);
	return size;
}
