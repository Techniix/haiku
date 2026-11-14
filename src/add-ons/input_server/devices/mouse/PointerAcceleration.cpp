/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 */


/*!	Renga: speed-based pointer acceleration; see
	headers/private/input/pointer_acceleration.h for the design.
*/


#include <pointer_acceleration.h>

#include <Directory.h>
#include <File.h>
#include <FindDirectory.h>
#include <Path.h>

#include <errno.h>
#include <math.h>
#include <string.h>
#include <unistd.h>


namespace BPrivate {


PointerAcceleration::PointerAcceleration()
	:
	fCount(0),
	fNewest(-1),
	fSpeed(65536),
	fAccelFactor(65536),
	fProfile(kDefaultPointerAccelerationProfile),
	fRemainderX(0),
	fRemainderY(0),
	fLastVelocity(0)
{
}


void
PointerAcceleration::SetSettings(int32 speed, int32 accelFactor,
	pointer_acceleration_profile profile)
{
	fSpeed = speed;
	fAccelFactor = accelFactor;
	if (profile != fProfile) {
		fProfile = profile;
		Reset();
	}
}


void
PointerAcceleration::Reset()
{
	fCount = 0;
	fNewest = -1;
	fRemainderX = fRemainderY = 0;
	fLastVelocity = 0;
}


/*!	The speed of the motion ending with the newest sample, in counts per
	microsecond: the distance of the samples from the last kVelocityWindow,
	divided by the time they took. That time starts at the sample before
	them, which is when the first of them began to accumulate; without one
	(the first report after a pause) a report is taken to have covered
	kReferenceInterval, so that report is accelerated as Classic would at
	125 Hz.

	At a steady speed the result is the same for every report rate: at
	1000 Hz the window holds 24 reports of 1 ms, at 125 Hz 3 reports of 8 ms.
	The distance is that of the summed vector, so a quick change of direction
	(the hand going back) slows it down, as it should.
*/
float
PointerAcceleration::_Velocity(bigtime_t now) const
{
	float sumX = 0;
	float sumY = 0;
	bigtime_t start = -1;
	bigtime_t oldestIncluded = now;
	int32 index = fNewest;
	for (int32 i = 0; i < fCount; i++) {
		const Sample& sample = fHistory[index];
		if (now - sample.when >= kVelocityWindow
			|| (i == kHistorySize - 1 && i > 0)) {
			// the first sample outside the window, or (a device reporting
			// so often that the whole history is inside it) the oldest one
			// kept: its time is when the motion summed so far began
			start = sample.when;
			break;
		}
		sumX += sample.dx;
		sumY += sample.dy;
		oldestIncluded = sample.when;
		index = (index + kHistorySize - 1) % kHistorySize;
	}
	if (start < 0)
		start = oldestIncluded - kReferenceInterval;

	bigtime_t span = now - start;
	if (span < 500)
		span = 500;
	return sqrtf(sumX * sumX + sumY * sumY) / span;
}


void
PointerAcceleration::Compute(int32 deltaX, int32 deltaY, bigtime_t when,
	int32& _deltaX, int32& _deltaY)
{
	float x;
	float y;

	if (fProfile == B_POINTER_ACCELERATION_CLASSIC) {
		// Haiku's original computation, unchanged: the factor comes from the
		// size of this report (and the fraction carried over).
		x = (float)deltaX * fSpeed / 65536.0 + fRemainderX;
		y = (float)deltaY * fSpeed / 65536.0 + fRemainderY;

		double acceleration = 1;
		if (fAccelFactor) {
			acceleration = 1 + sqrt(x * x + y * y) * fAccelFactor / 524288.0;
		}
		x *= acceleration;
		y *= acceleration;
		fLastVelocity = 0;
	} else {
		if (fCount > 0 && when - fHistory[fNewest].when > kIdleGap)
			fCount = 0;
		if (fCount > 0 && when < fHistory[fNewest].when)
			when = fHistory[fNewest].when;
		fNewest = (fNewest + 1) % kHistorySize;
		fHistory[fNewest].when = when;
		fHistory[fNewest].dx = deltaX;
		fHistory[fNewest].dy = deltaY;
		if (fCount < kHistorySize)
			fCount++;

		float velocity = _Velocity(when);
		fLastVelocity = velocity * 1000000.0f;

		// Classic's curve, at the distance one 125 Hz report would have had
		// at this speed (after the speed setting, like Classic's).
		double acceleration = 1;
		if (fAccelFactor) {
			double reference = velocity * kReferenceInterval * fSpeed
				/ 65536.0;
			acceleration = 1 + reference * fAccelFactor / 524288.0;
		}
		x = (float)(deltaX * fSpeed / 65536.0 * acceleration) + fRemainderX;
		y = (float)(deltaY * fSpeed / 65536.0 * acceleration) + fRemainderY;
	}

	_deltaX = x >= 0 ? (int32)floorf(x) : (int32)ceilf(x);
	_deltaY = y >= 0 ? (int32)floorf(y) : (int32)ceilf(y);
	fRemainderX = x - _deltaX;
	fRemainderY = y - _deltaY;
}


static status_t
settings_path(BPath& path)
{
	status_t status = find_directory(B_USER_SETTINGS_DIRECTORY, &path);
	if (status != B_OK)
		return status;
	return path.Append(kPointerAccelerationSettingsFile);
}


pointer_acceleration_profile
get_pointer_acceleration_profile()
{
	BPath path;
	if (settings_path(path) != B_OK)
		return kDefaultPointerAccelerationProfile;

	BFile file(path.Path(), B_READ_ONLY);
	char word[32];
	ssize_t length = file.InitCheck() == B_OK
		? file.Read(word, sizeof(word) - 1) : -1;
	if (length <= 0)
		return kDefaultPointerAccelerationProfile;
	word[length] = '\0';
	if (strncmp(word, "classic", 7) == 0)
		return B_POINTER_ACCELERATION_CLASSIC;
	return B_POINTER_ACCELERATION_ADAPTIVE;
}


status_t
set_pointer_acceleration_profile(pointer_acceleration_profile profile)
{
	BPath path;
	status_t status = settings_path(path);
	if (status != B_OK)
		return status;

	if (profile == kDefaultPointerAccelerationProfile) {
		if (unlink(path.Path()) != 0 && errno != ENOENT)
			return errno;
		return B_OK;
	}

	BPath parent;
	path.GetParent(&parent);
	create_directory(parent.Path(), 0755);

	BFile file(path.Path(), B_WRITE_ONLY | B_CREATE_FILE | B_ERASE_FILE);
	status = file.InitCheck();
	if (status != B_OK)
		return status;
	const char* word = profile == B_POINTER_ACCELERATION_CLASSIC
		? "classic\n" : "adaptive\n";
	ssize_t written = file.Write(word, strlen(word));
	return written == (ssize_t)strlen(word) ? B_OK : B_IO_ERROR;
}


}	// namespace BPrivate
