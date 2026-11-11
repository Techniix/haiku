/*
 * Copyright 2009, Ingo Weinhold, ingo_weinhold@gmx.de.
 * Distributed under the terms of the MIT License.
 */
#include "TimeComputer.h"

#include <OS.h>


TimeComputer::TimeComputer()
	:
	fRealTime(0),
	fPerformanceTime(0),
	fDrift(1),
	fFrameRate(1000000),
	fUsecsPerFrame(1),
	fPerformanceTimeBase(0),
	fFrameBase(0),
	fResetTimeBase(true),
	fFirstEntry(0),
	fLastEntry(0)
{
}


void
TimeComputer::Init(float frameRate, bigtime_t realBaseTime)
{
	fRealTime = realBaseTime;
	fPerformanceTime = 0;
	fDrift = 1;
	fResetTimeBase = true;
	SetFrameRate(frameRate);
}


void
TimeComputer::SetFrameRate(float frameRate)
{
	if (frameRate == fFrameRate)
		return;

	fFrameRate = frameRate;
	fUsecsPerFrame = (double)1000000 / fFrameRate;
	fResetTimeBase = true;
	fFirstEntry = 0;
	fLastEntry = 0;
}


/*!	Renga: the device's buffers were set up again (a new period length),
	so its frame counter starts from zero. The performance time goes on
	from where it is, as after a frame rate change.
*/
static const bigtime_t kMinDriftSpan = 100000;


void
TimeComputer::ResetTimeBase()
{
	fResetTimeBase = true;
	fFirstEntry = 0;
	fLastEntry = 0;
}


void
TimeComputer::AddTimeStamp(bigtime_t realTime, uint64 frames)
{
	bigtime_t estimatedPerformanceTime = fPerformanceTime
		+ bigtime_t((realTime - fRealTime) * fDrift);

	fRealTime = realTime;

	// Renga: the device's frame counter went back (its buffers were set up
	// again, see ResetTimeBase()); the first exchanges after that can still
	// report the old count. Start a new base instead of computing with a
	// negative frame difference.
	if (frames < fFrameBase)
		fResetTimeBase = true;

	if (fResetTimeBase) {
		// use the extrapolated performance time at the given real time
		fPerformanceTime = estimatedPerformanceTime;
		fPerformanceTimeBase = estimatedPerformanceTime;
		fFrameBase = frames;
		fResetTimeBase = false;
		_AddEntry(fRealTime, fPerformanceTime);
		fFirstEntry = fLastEntry;
		return;
	}

	// add entry
	bigtime_t performanceTime = fPerformanceTimeBase
		+ bigtime_t((frames - fFrameBase) * fUsecsPerFrame);
	_AddEntry(realTime, performanceTime);

	// Update performance time and drift. We don't use the given
	// performance time directly, but average it with the estimated
	// performance time.
	fPerformanceTime = (performanceTime + estimatedPerformanceTime) / 2;

	// Renga: the drift only from at least kMinDriftSpan of history. Right
	// after a (re)start a device can hand back its first buffers almost at
	// once; the drift over such a short span came out at many times 1, and
	// the estimate above then moved the performance time by seconds (seen
	// in the harness when the low-latency mode set the buffers up again:
	// steps of 9 to 51 s). A sound card's clock is within a percent of the
	// system's, so anything outside 0.9 to 1.1 is a measurement error too.
	Entry& entry = fEntries[fFirstEntry];
	if (fRealTime - entry.realTime >= kMinDriftSpan) {
		double drift = double(fPerformanceTime - entry.performanceTime)
			/ double(fRealTime - entry.realTime);
		if (drift >= 0.9 && drift <= 1.1)
			fDrift = drift;
	}
}


void
TimeComputer::_AddEntry(bigtime_t realTime, bigtime_t performanceTime)
{
	fLastEntry = (fLastEntry + 1) % kEntryCount;
	Entry& entry = fEntries[fLastEntry];
	entry.realTime = realTime;
	entry.performanceTime = performanceTime;

	if (fLastEntry == fFirstEntry)
		fFirstEntry = (fFirstEntry + 1) % kEntryCount;
}
