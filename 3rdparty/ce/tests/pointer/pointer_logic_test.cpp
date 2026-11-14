/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 */

/*!	In-guest test, no desktop needed: the pointer code that has no timing of
	its own, run with made-up reports (the pointer and packaging fix,
	2026-11-13). The parts that need input_server and app_server are in
	pointer_rate_test.

	Acceleration (PointerAcceleration, headers/private/input/
	pointer_acceleration.h), the default speed and acceleration factor:

	  accel.adaptive_same_<speed>   the same hand speed at 125, 500, 1000
	                                and 8000 reports per second moves the
	                                pointer the same distance (within 3 %),
	                                for a slow, a medium and a fast speed
	  accel.adaptive_is_classic_at_125
	                                a mouse reporting steadily every 8 ms
	                                moves exactly as with Classic
	  accel.classic_unchanged       Classic gives the numbers of Haiku's
	                                original computation (copied here)
	  accel.classic_rate_dependent  and Classic does depend on the rate (a
	                                1000 Hz mouse got about half the motion
	                                of a 125 Hz one): the reason for
	                                Adaptive
	  accel.adaptive_pause          after a pause the history starts again:
	                                a slow move is not sped up by the fast
	                                one before it
	  accel.fractions_kept          slow motion of under a pixel per report
	                                still adds up (fractions carried over)
	  accel.no_acceleration         an acceleration factor of 0 gives the
	                                counts times the speed, either profile

	Physical devices (PhysicalDeviceGroups, the mouse add-on's grouping of
	nodes that are interfaces of one mouse):

	  groups.join                   the second interface joins the first
	  groups.no_key                 nodes without a key (PS/2) never join
	  groups.other_device           another device's node stays separate
	  groups.promote                when the first goes, the next takes its
	                                place, and a third joins that one
	  groups.members                members are listed for their primary

	Output format: see 3rdparty/ce/README.md.
*/


#include <pointer_acceleration.h>
#include <kb_mouse_settings.h>

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

#include "PhysicalDeviceGroups.h"


#define SUITE "pointer_logic"

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


/*!	Moves at \a countsPerSecond (x only) for \a duration with \a rate
	reports a second and returns the pixels the pointer moved. The counts of
	each report are what a real mouse would report: the motion since the
	last report, rounded, with the rest kept for the next one.
*/
static int64
simulate(PointerAcceleration& acceleration, double countsPerSecond,
	int32 rate, bigtime_t duration, bigtime_t start = 1000000)
{
	bigtime_t interval = 1000000 / rate;
	double owed = 0;
	int64 total = 0;
	for (bigtime_t t = interval; t <= duration; t += interval) {
		owed += countsPerSecond * interval / 1000000.0;
		int32 counts = (int32)floor(owed);
		owed -= counts;
		if (counts == 0)
			continue;
		int32 x, y;
		acceleration.Compute(counts, 0, start + t, x, y);
		total += x;
	}
	return total;
}


//! Haiku's original computation, from MouseInputDevice.cpp before Renga.
static void
original_classic(int32 xdelta, int32 ydelta, int32 speed, int32 accelFactor,
	int32& _deltaX, int32& _deltaY, float& historyDeltaX,
	float& historyDeltaY)
{
	float deltaX = (float)xdelta * speed / 65536.0 + historyDeltaX;
	float deltaY = (float)ydelta * speed / 65536.0 + historyDeltaY;

	double acceleration = 1;
	if (accelFactor) {
		acceleration = 1 + sqrt(deltaX * deltaX + deltaY * deltaY)
			* accelFactor / 524288.0;
	}

	deltaX *= acceleration;
	deltaY *= acceleration;

	if (deltaX >= 0)
		_deltaX = (int32)floorf(deltaX);
	else
		_deltaX = (int32)ceilf(deltaX);

	if (deltaY >= 0)
		_deltaY = (int32)floorf(deltaY);
	else
		_deltaY = (int32)ceilf(deltaY);

	historyDeltaX = deltaX - _deltaX;
	historyDeltaY = deltaY - _deltaY;
}


static void
test_acceleration()
{
	struct Speed {
		const char*	name;
		double		countsPerSecond;
	};
	static const Speed kSpeeds[] = {
		{ "slow", 400 },		// a careful move, under a pixel per 1 ms
		{ "medium", 2500 },
		{ "fast", 12000 }		// a flick across the screen
	};
	static const int32 kRates[] = { 125, 500, 1000, 8000 };

	for (size_t s = 0; s < sizeof(kSpeeds) / sizeof(kSpeeds[0]); s++) {
		int64 results[4];
		int64 low = INT64_MAX, high = 0;
		for (size_t r = 0; r < 4; r++) {
			PointerAcceleration acceleration;
			acceleration.SetSettings(kDefaultMouseSpeed,
				kDefaultAccelerationFactor, B_POINTER_ACCELERATION_ADAPTIVE);
			results[r] = simulate(acceleration, kSpeeds[s].countsPerSecond,
				kRates[r], 400000);
			if (results[r] < low)
				low = results[r];
			if (results[r] > high)
				high = results[r];
		}
		BString name("adaptive_same_");
		name << kSpeeds[s].name;
		report(name.String(), low > 0 && (high - low) <= high * 0.03,
			"%.0f counts/s for 400 ms: %" B_PRId64 ", %" B_PRId64 ", %"
			B_PRId64 ", %" B_PRId64 " pixels at 125, 500, 1000, 8000 Hz",
			kSpeeds[s].countsPerSecond, results[0], results[1], results[2],
			results[3]);
	}

	// steady 125 Hz: Adaptive is Classic
	{
		PointerAcceleration adaptive;
		adaptive.SetSettings(kDefaultMouseSpeed, kDefaultAccelerationFactor,
			B_POINTER_ACCELERATION_ADAPTIVE);
		PointerAcceleration classic;
		classic.SetSettings(kDefaultMouseSpeed, kDefaultAccelerationFactor,
			B_POINTER_ACCELERATION_CLASSIC);
		int64 a = simulate(adaptive, 2500, 125, 400000);
		int64 c = simulate(classic, 2500, 125, 400000);
		report("adaptive_is_classic_at_125", llabs(a - c) <= 1,
			"2500 counts/s at 125 Hz: Adaptive %" B_PRId64 ", Classic %"
			B_PRId64 " pixels", a, c);
	}

	// Classic is the original computation, report by report
	{
		PointerAcceleration classic;
		classic.SetSettings(98304, 131072, B_POINTER_ACCELERATION_CLASSIC);
		float hx = 0, hy = 0;
		bool same = true;
		int32 first = -1;
		srand(42);
		for (int32 i = 0; i < 2000; i++) {
			int32 dx = rand() % 41 - 20;
			int32 dy = rand() % 41 - 20;
			int32 x, y, ox, oy;
			classic.Compute(dx, dy, 1000000 + i * 8000, x, y);
			original_classic(dx, dy, 98304, 131072, ox, oy, hx, hy);
			if ((x != ox || y != oy) && first < 0) {
				same = false;
				first = i;
			}
		}
		report("classic_unchanged", same, same
			? "2000 random reports give the original numbers"
			: "report %" B_PRId32 " differs", first);
	}

	// Classic depends on the rate
	{
		PointerAcceleration slow;
		slow.SetSettings(kDefaultMouseSpeed, kDefaultAccelerationFactor,
			B_POINTER_ACCELERATION_CLASSIC);
		PointerAcceleration fast;
		fast.SetSettings(kDefaultMouseSpeed, kDefaultAccelerationFactor,
			B_POINTER_ACCELERATION_CLASSIC);
		int64 at125 = simulate(slow, 2500, 125, 400000);
		int64 at1000 = simulate(fast, 2500, 1000, 400000);
		report("classic_rate_dependent", at1000 < at125 * 0.8,
			"Classic, 2500 counts/s: %" B_PRId64 " pixels at 125 Hz, %"
			B_PRId64 " at 1000 Hz", at125, at1000);
	}

	// a pause starts a new motion
	{
		PointerAcceleration acceleration;
		acceleration.SetSettings(kDefaultMouseSpeed,
			kDefaultAccelerationFactor, B_POINTER_ACCELERATION_ADAPTIVE);
		simulate(acceleration, 12000, 1000, 200000, 1000000);
		int64 afterFast = simulate(acceleration, 400, 1000, 200000, 1500000);
		PointerAcceleration fresh;
		fresh.SetSettings(kDefaultMouseSpeed, kDefaultAccelerationFactor,
			B_POINTER_ACCELERATION_ADAPTIVE);
		int64 alone = simulate(fresh, 400, 1000, 200000, 1500000);
		report("adaptive_pause", llabs(afterFast - alone) <= 1,
			"a slow move 300 ms after a fast one: %" B_PRId64 " pixels, on "
			"its own %" B_PRId64, afterFast, alone);
	}

	// fractions
	{
		PointerAcceleration acceleration;
		acceleration.SetSettings(32768, 0, B_POINTER_ACCELERATION_ADAPTIVE);
		int64 total = 0;
		for (int32 i = 0; i < 100; i++) {
			int32 x, y;
			acceleration.Compute(1, 0, 1000000 + i * 1000, x, y);
			total += x;
		}
		report("fractions_kept", total == 50,
			"100 reports of 1 count at half speed: %" B_PRId64 " pixels "
			"(want 50)", total);
	}

	// no acceleration
	{
		bool same = true;
		for (int profile = 0; profile < 2; profile++) {
			PointerAcceleration acceleration;
			acceleration.SetSettings(65536, 0,
				(pointer_acceleration_profile)profile);
			int64 total = simulate(acceleration, 12000, 1000, 100000);
			if (total != 1200)
				same = false;
		}
		report("no_acceleration", same,
			"factor 0: 1200 counts give 1200 pixels with both profiles");
	}
}


struct FakeDevice {
	int	id;
};


static void
test_groups()
{
	FakeDevice a = { 1 }, b = { 2 }, c = { 3 }, ps2 = { 4 }, ps2b = { 5 },
		other = { 6 };
	PhysicalDeviceGroups<FakeDevice> groups;

	FakeDevice* joinedA = groups.Add(&a, "usb:5");
	FakeDevice* joinedB = groups.Add(&b, "usb:5");
	report("join", joinedA == NULL && joinedB == &a
			&& groups.PrimaryOf(&b) == &a && groups.PrimaryOf(&a) == NULL,
		"first node primary, second joins it");

	FakeDevice* joinedPs2 = groups.Add(&ps2, "");
	FakeDevice* joinedPs2b = groups.Add(&ps2b, NULL);
	report("no_key", joinedPs2 == NULL && joinedPs2b == NULL,
		"nodes without a key stay separate");

	FakeDevice* joinedOther = groups.Add(&other, "usb:9");
	report("other_device", joinedOther == NULL,
		"another device's node is a primary of its own");

	report("members", groups.CountMembers(&a) == 1
			&& groups.MemberAt(&a, 0) == &b && groups.MemberAt(&a, 1) == NULL
			&& groups.CountMembers(&ps2) == 0,
		"%d member(s) for the first device", (int)groups.CountMembers(&a));

	FakeDevice* promoted = groups.Remove(&a);
	FakeDevice* joinedC = groups.Add(&c, "usb:5");
	report("promote", promoted == &b && groups.PrimaryOf(&b) == NULL
			&& joinedC == &b && groups.Remove(&c) == NULL
			&& groups.Remove(&b) == NULL,
		"after the first goes, the second is primary and a third joins it");
}


int
main()
{
	printf("CE-SUITE %s START\n", SUITE);
	fflush(stdout);

	test_acceleration();
	test_groups();

	printf("CE-SUITE %s DONE failures=%d\n", SUITE, sFailures);
	fflush(stdout);
	return sFailures != 0;
}
