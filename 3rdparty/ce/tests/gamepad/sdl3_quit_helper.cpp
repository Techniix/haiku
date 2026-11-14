/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 */

//!	sdl3_gamepad_test: asks the running BApplication to quit, as Deskbar's
//!	"Quit" or any other program could.


#include <Application.h>


extern "C" int
sdl3_test_post_quit_to_be_app(void)
{
	if (be_app == NULL)
		return -1;
	return be_app_messenger.SendMessage(B_QUIT_REQUESTED) == B_OK ? 0 : -1;
}


extern "C" int
sdl3_test_be_app_exists(void)
{
	return be_app != NULL;
}
