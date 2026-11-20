/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 */
#ifndef _SCREEN_OUTPUTS_H
#define _SCREEN_OUTPUTS_H


/*!	The outputs the desktop is spread over (Renga batch 6, several screens
	through <add-ons/graphics/MultiHead.h>). With one screen there is one
	output, the whole screen. Private API of libbe and app_server
	(AS_GET_SCREEN_OUTPUTS, AS_RELOAD_DISPLAYS).
*/


#include <Rect.h>


struct screen_output_info {
	BRect		frame;				// in screen coordinates
	uint32		index;				// the accelerant's output number
	uint32		mirror_of;			// output shown, or 0xffffffff
	bool		enabled;
	bool		primary;
	bool		connected;
	bool		_reserved0;
	uint32		width;				// the output's mode
	uint32		height;
	uint32		refresh_millihz;
	char		name[32];
	uint32		_reserved[8];
};


namespace BPrivate {

//!	At most \a maxCount outputs; \a _count gets how many there are.
status_t	get_screen_outputs(screen_output_info* outputs, int32 maxCount,
				int32* _count);

//!	The frame of the output with \a where (the primary output's if none).
status_t	get_output_frame_at(BPoint where, BRect* _frame);

//!	app_server reads the displays settings again and applies them.
status_t	reload_display_settings();

}	// namespace BPrivate


#endif	// _SCREEN_OUTPUTS_H
