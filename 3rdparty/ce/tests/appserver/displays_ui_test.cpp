/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 */

/*!	Screen -> Displays (batch 4): the page (DisplaysView, built from the
	Screen preferences' sources) in a window of its own, in QEMU (one
	display, the VESA or virtio driver):
	  displays.found       one display found, shown in the arrangement
	  displays.modes       the driver's mode list fills the resolution menu
	  displays.gpu_info    the graphics card's name from the driver
	  displays.greyed      rotation, VRR, HDR and colour profile are greyed
	                       (no driver support yet); primary and mirroring
	                       are greyed with one display; the Wayland switch
	                       and workspaces are usable
	  displays.workspaces  the workspaces control shows count_workspaces()
	Screenshot "displays_page". Output format: see 3rdparty/ce/README.md.
*/


#include "DisplaysView.h"

#include <Application.h>
#include <GroupLayout.h>
#include <Spinner.h>

#include <stdarg.h>
#include <stdio.h>
#include <string.h>


#define SUITE "displays"


static int sFailures = 0;


static void
report(const char* name, bool pass, const char* format, ...)
{
	char detail[300];
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


int
main()
{
	printf("CE-SUITE %s START\n", SUITE);
	fflush(stdout);
	BApplication application("application/x-vnd.Renga-DisplaysUITest");

	DisplaysView* page = new DisplaysView();
	BWindow* window = new BWindow(BRect(60, 60, 600, 700), "Displays",
		B_TITLED_WINDOW, B_AUTO_UPDATE_SIZE_LIMITS);
	window->SetLayout(new BGroupLayout(B_VERTICAL, 0));
	window->AddChild(page);
	window->Show();
	snooze(1500000);

	if (window->Lock()) {
		report("found", page->CountDisplays() == 1, "%d display(s)",
			(int)page->CountDisplays());
		report("modes", page->CountModes() > 0, "%d modes",
			(int)page->CountModes());
		report("gpu_info", strstr(page->GpuName(), "Name:") != NULL,
			"%s", page->GpuName());
		bool greyed = !page->FeatureEnabled("rotation")
			&& !page->FeatureEnabled("vrr") && !page->FeatureEnabled("hdr")
			&& !page->FeatureEnabled("color")
			&& !page->FeatureEnabled("primary")
			&& !page->FeatureEnabled("mirror");
		bool usable = page->FeatureEnabled("wayland")
			&& page->FeatureEnabled("workspaces")
			&& page->FeatureEnabled("resolution");
		report("greyed", greyed && usable, "not possible greyed %d, usable"
			" %d", greyed, usable);
		BSpinner* spinner = dynamic_cast<BSpinner*>(
			page->FindView("workspaces"));
		report("workspaces", spinner != NULL
				&& spinner->Value() == count_workspaces(),
			"control %d, count_workspaces() %d",
			spinner != NULL ? (int)spinner->Value() : -1,
			(int)count_workspaces());
		window->Unlock();
	}
	printf("CE-HOST screenshot displays_page\n");
	fflush(stdout);
	snooze(1000000);

	window->Lock();
	window->Quit();
	printf("CE-SUITE %s DONE failures=%d\n", SUITE, sFailures);
	return sFailures == 0 ? 0 : 1;
}
