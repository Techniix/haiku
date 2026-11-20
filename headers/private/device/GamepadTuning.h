/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 */
#ifndef _GAMEPAD_TUNING_H
#define _GAMEPAD_TUNING_H


/*!	Per-pad tuning (Renga, roadmap F-42 configurator polish): what the
	Gamepads preferences set for one controller and BGamepad applies to the
	state it reports, after the mapping. Stored by the pad's SDL GUID in
	~/config/settings/Gamepads/tuning (a flattened BMessage with one
	"pad" message per GUID). Everything off by default: a pad without an
	entry reports what it sends.

	- dead zone (0-50 %): stick deflections inside a circle of that radius
	  read as centred; beyond it the range is stretched back to full scale,
	  so there is no jump at its edge (radial, not per axis, so diagonals
	  keep their direction);
	- response curve (1.0 linear, up to 3.0): the deflection outside the
	  dead zone is raised to that power, finer control near the centre;
	- trigger threshold (0-50 %): trigger values below it read 0, above it
	  are stretched to the full range;
	- face button swap: A with B and X with Y, for players used to the
	  Nintendo layout (labels by position stay as they are).
*/


#include <SupportDefs.h>


struct gamepad_state;
class BString;


namespace BPrivate {


struct GamepadTuning {
			int32			deadZone;			// percent, 0-50
			float			curve;				// 1.0-3.0
			int32			triggerThreshold;	// percent, 0-50
			bool			swapFaceButtons;

							GamepadTuning() { SetDefaults(); }

			void			SetDefaults();
			bool			IsDefault() const;
			void			Clamp();

			void			Apply(gamepad_state& state) const;

	static	status_t		Load(const char* guid, GamepadTuning& tuning);
	static	status_t		Save(const char* guid,
								const GamepadTuning& tuning);
				// a default tuning removes the pad's entry
};


}	// namespace BPrivate


#endif	// _GAMEPAD_TUNING_H
