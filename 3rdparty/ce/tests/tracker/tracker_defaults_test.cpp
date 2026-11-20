/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 */

/*!	In-guest test: Tracker's defaults for new installs (maintainer,
	2026-11-20): single-window navigation and the navigator (path bar) on.
	On the harness's fresh image there is no saved Tracker setting for
	either, so TrackerSettings (libtracker) gives the defaults:

	  tracker_defaults.single_window   SingleWindowBrowse() is true
	  tracker_defaults.navigator       ShowNavigator() is true

	Saved settings keep their values (TrackerSettings reads the file over
	the defaults); both stay switchable in the Tracker preferences.
	Known-bad: before the change both read false.
*/


#include <stdio.h>

#include <Application.h>

#include "TrackerSettings.h"


int
main()
{
	BApplication app("application/x-vnd.Renga-tracker-defaults-test");
	printf("CE-SUITE tracker_defaults START\n");
	BPrivate::TrackerSettings settings;
	int failures = 0;
	bool single = settings.SingleWindowBrowse();
	bool navigator = settings.ShowNavigator();
	printf("CE-TEST tracker_defaults.single_window %s SingleWindowBrowse %d\n",
		single ? "PASS" : "FAIL", single);
	printf("CE-TEST tracker_defaults.navigator %s ShowNavigator %d\n",
		navigator ? "PASS" : "FAIL", navigator);
	failures = (single ? 0 : 1) + (navigator ? 0 : 1);
	printf("CE-SUITE tracker_defaults DONE failures=%d\n", failures);
	return failures == 0 ? 0 : 1;
}
