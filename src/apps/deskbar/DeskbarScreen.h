/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 */
#ifndef DESKBAR_SCREEN_H
#define DESKBAR_SCREEN_H


#include <Screen.h>
#include <Window.h>

#include <ScreenOutputs.h>


/*!	Renga (batch 6, several screens): the frame Deskbar places itself in.
	With one screen it is the screen; with a desktop spread over several
	outputs it is the primary output, so that Deskbar and the switcher stay
	on the display chosen as primary in Screen -> Displays.
*/
static inline BRect
deskbar_screen_frame(BWindow* window = NULL)
{
	BRect frame = window != NULL ? BScreen(window).Frame()
		: BScreen(B_MAIN_SCREEN_ID).Frame();
	screen_output_info outputs[16];
	int32 count = 0;
	if (BPrivate::get_screen_outputs(outputs, 16, &count) != B_OK
		|| count < 2)
		return frame;
	for (int32 i = 0; i < count && i < 16; i++) {
		if (outputs[i].primary && outputs[i].frame.IsValid())
			return outputs[i].frame & frame;
	}
	return frame;
}


#endif	// DESKBAR_SCREEN_H
