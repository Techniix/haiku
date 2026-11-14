/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 */
#ifndef _POINTER_ACCELERATION_H
#define _POINTER_ACCELERATION_H


/*!	Renga: pointer acceleration for relative pointing devices.

	Haiku's acceleration ("Classic") is computed from the size of each report:
	the factor grows with the distance a single report moved. A mouse that
	reports eight times as often reports an eighth of the distance each time,
	so at the same hand speed a 1000 Hz mouse got far less acceleration than a
	125 Hz one, and a report that was read late (with more motion in it) got
	more. "Adaptive" computes the factor from the pointer's speed instead,
	measured over the last few reports from their timestamps (the way
	libinput does it), so the result depends on how fast the hand moves, not
	on how often the device reports.

	Adaptive uses the same curve and the same two settings (speed and
	acceleration factor) as Classic, evaluated at the distance the device
	would have reported in 8 ms (125 reports per second, the rate
	input_server's mouse watcher reads at). So a device that reports steadily
	every 8 ms behaves exactly as with Classic, and every other rate feels the
	same as that.

	The profile is the same for every mouse. It is kept in
	~/config/settings/system/input_server/pointer_acceleration as one word,
	"adaptive" or "classic"; no file means the default, Adaptive. The Input
	preferences write it and then tell the devices to reload their settings
	(see set_pointer_acceleration_profile()).
*/


#include <SupportDefs.h>


namespace BPrivate {


enum pointer_acceleration_profile {
	B_POINTER_ACCELERATION_ADAPTIVE	= 0,
	B_POINTER_ACCELERATION_CLASSIC	= 1
};

static const pointer_acceleration_profile kDefaultPointerAccelerationProfile
	= B_POINTER_ACCELERATION_ADAPTIVE;

static const char* const kPointerAccelerationSettingsFile
	= "system/input_server/pointer_acceleration";
	// below B_USER_SETTINGS_DIRECTORY


class PointerAcceleration {
public:
								PointerAcceleration();

			void				SetSettings(int32 speed, int32 accelFactor,
									pointer_acceleration_profile profile);
			pointer_acceleration_profile Profile() const { return fProfile; }

			//! Forgets the motion history and the carried-over fractions.
			void				Reset();

			/*!	Turns one report's device counts into pixels. \a when is the
				report's timestamp (system_time()); it must not go backwards.
				Fractions of a pixel are carried over to the next report.
			*/
			void				Compute(int32 deltaX, int32 deltaY,
									bigtime_t when, int32& _deltaX,
									int32& _deltaY);

			//! The speed the last Compute() measured, counts per second.
			float				LastVelocity() const { return fLastVelocity; }

	static	const bigtime_t		kReferenceInterval = 8000;
									// the 125 Hz report the curve is set for
	static	const bigtime_t		kVelocityWindow = 24000;
									// reports this recent are averaged
	static	const bigtime_t		kIdleGap = 100000;
									// a longer pause starts a new motion

private:
			float				_Velocity(bigtime_t now) const;

private:
	struct Sample {
		bigtime_t	when;
		int32		dx;
		int32		dy;
	};

	static	const int32			kHistorySize = 128;
									// 24 ms at up to 5000 reports per second

			Sample				fHistory[kHistorySize];
			int32				fCount;
			int32				fNewest;

			int32				fSpeed;
			int32				fAccelFactor;
			pointer_acceleration_profile fProfile;

			float				fRemainderX;
			float				fRemainderY;
			float				fLastVelocity;
};


/*!	Reads the profile from its settings file; the default if there is none
	or it cannot be read.
*/
pointer_acceleration_profile get_pointer_acceleration_profile();

/*!	Writes the profile to its settings file (removing the file for the
	default). It does not tell the running devices; the Input preferences do
	that by setting each mouse's acceleration factor again.
*/
status_t set_pointer_acceleration_profile(
	pointer_acceleration_profile profile);


}	// namespace BPrivate


using BPrivate::PointerAcceleration;
using BPrivate::pointer_acceleration_profile;
using BPrivate::B_POINTER_ACCELERATION_ADAPTIVE;
using BPrivate::B_POINTER_ACCELERATION_CLASSIC;
using BPrivate::get_pointer_acceleration_profile;
using BPrivate::set_pointer_acceleration_profile;


#endif	// _POINTER_ACCELERATION_H
