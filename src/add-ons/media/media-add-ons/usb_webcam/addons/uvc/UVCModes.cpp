/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 */


#include "UVCModes.h"

#include <math.h>
#include <stdio.h>
#include <string.h>


// Offsets in a VS_FRAME descriptor (UVC 1.1, 3.9.2.2 and 3.9.2.3).
static const size_t kIntervalTypeOffset = 25;
static const size_t kIntervalsOffset = 26;


static uint32
read32(const uint8* data)
{
	return data[0] | (data[1] << 8) | (data[2] << 16)
		| ((uint32)data[3] << 24);
}


static void
add_sorted(uint32* intervals, int32& count, int32 maxCount, uint32 value)
{
	if (value == 0)
		return;
	int32 index = 0;
	while (index < count && intervals[index] < value)
		index++;
	if (index < count && intervals[index] == value)
		return;
	if (count >= maxCount)
		return;
	memmove(intervals + index + 1, intervals + index,
		(count - index) * sizeof(uint32));
	intervals[index] = value;
	count++;
}


int32
uvc_frame_intervals(const uint8* descriptor, size_t length,
	uint32* intervals, int32 maxCount)
{
	if (descriptor == NULL || intervals == NULL || maxCount <= 0
		|| length == 0)
		return 0;
	// bLength bounds what belongs to this descriptor
	if (length > descriptor[0])
		length = descriptor[0];
	if (length <= kIntervalTypeOffset)
		return 0;

	int32 count = 0;
	uint8 type = descriptor[kIntervalTypeOffset];
	if (type > 0) {
		for (uint8 i = 0; i < type; i++) {
			size_t offset = kIntervalsOffset + 4 * i;
			if (offset + 4 > length)
				break;
			add_sorted(intervals, count, maxCount,
				read32(descriptor + offset));
		}
		return count;
	}

	if (kIntervalsOffset + 12 > length)
		return 0;
	uint32 minimum = read32(descriptor + kIntervalsOffset);
	uint32 maximum = read32(descriptor + kIntervalsOffset + 4);
	uint32 step = read32(descriptor + kIntervalsOffset + 8);
	if (minimum == 0 || maximum < minimum)
		return 0;
	if (step == 0)
		step = 1;
	static const float kRates[] = { 60, 50, 30, 25, 24, 20, 15, 10, 7.5f,
		5 };
	add_sorted(intervals, count, maxCount, minimum);
	for (size_t i = 0; i < sizeof(kRates) / sizeof(kRates[0]); i++) {
		double wanted = 10000000.0 / kRates[i];
		if (wanted < minimum || wanted > maximum)
			continue;
		uint32 steps = (uint32)floor((wanted - minimum) / step + 0.5);
		uint64 value = (uint64)minimum + (uint64)steps * step;
		if (value > maximum)
			value = maximum;
		add_sorted(intervals, count, maxCount, (uint32)value);
	}
	// the slowest end, unless a rate above is within one step of it
	if (count == 0 || maximum - intervals[count - 1] >= step)
		add_sorted(intervals, count, maxCount, maximum);
	return count;
}


float
uvc_interval_fps(uint32 interval)
{
	return interval > 0 ? 10000000.0f / interval : 0;
}


uint32
uvc_choose_interval(const uint32* intervals, int32 count, float fps,
	uint32 defaultInterval)
{
	if (count <= 0 || intervals == NULL)
		return defaultInterval;
	if (fps <= 0) {
		for (int32 i = 0; i < count; i++) {
			if (intervals[i] == defaultInterval)
				return defaultInterval;
		}
		return intervals[0];
	}
	uint32 best = intervals[0];
	double bestDistance = -1;
	for (int32 i = 0; i < count; i++) {
		double distance = fabs(uvc_interval_fps(intervals[i]) - fps);
		if (bestDistance < 0 || distance < bestDistance - 1e-6) {
			best = intervals[i];
			bestDistance = distance;
		}
	}
	return best;
}


BString
uvc_mode_line(uint32 width, uint32 height, const char* encoding,
	const uint32* intervals, int32 count)
{
	BString line;
	line << width << "x" << height << " " << encoding << " ";
	for (int32 i = 0; i < count; i++) {
		char rate[32];
		snprintf(rate, sizeof(rate), "%.3f", uvc_interval_fps(intervals[i]));
		// no trailing zeros: 30, 7.5, 29.97
		char* end = rate + strlen(rate) - 1;
		while (end > rate && *end == '0')
			*end-- = '\0';
		if (end > rate && *end == '.')
			*end = '\0';
		if (i > 0)
			line << ",";
		line << rate;
	}
	return line;
}
