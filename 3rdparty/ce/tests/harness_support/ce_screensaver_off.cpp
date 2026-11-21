/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 */

/*!	Harness helper (not a test): turns the screen saver's timer off for the
	test run. The guest runner calls it once before the tests.

	Why: the screen saver starts after 15 minutes without input. A full
	renga-raw run has long stretches of media tests without any pointer or
	key input, and the screen went black before renga_doctor_test,
	renguin_manager_test and screen_capture_test (blank screenshots, and
	the capture prompt could not be answered), in batch 5 once and in both
	renga-raw runs of the batch 8 sweep. Tests that need the screen must
	not depend on how long the tests before them took.

	Only the timer flags change (ScreenSaverSettings::SetTimeFlags(0));
	the input_server filter re-reads the file when it changes. The inhibit
	port that programs use (wayland_protocols_test's idle_inhibit) is not
	touched.
*/


#include <stdio.h>

#include <ScreenSaverSettings.h>


int
main()
{
	ScreenSaverSettings settings;
	settings.Load();
	settings.SetTimeFlags(0);
	settings.Save();
	printf("CE-INFO screen saver timer off for the test run\n");
	return 0;
}
