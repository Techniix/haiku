/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 */

/*!	Several screens through <add-ons/graphics/MultiHead.h> version 2 (Renga
	batch 6): app_server's desktop across the outputs of one card, the
	private outputs API, window zoom per output, and the Displays page's
	arrangement, primary and mirroring. Runs in every image; with one
	output (the VGA card) it checks that the single-screen path is
	unchanged. With ce-test.py --displays 2 (virtio-gpu, two outputs of
	1024x768):

	  multi.outputs        two outputs, both on, side by side, the screen
	                       is their union (2048x768), output 0 primary
	  multi.second_window  a window placed on output 1 is drawn there
	                       (screenshots multi_head0 and multi_head1; the
	                       harness looks for its colour on head 1 only)
	  multi.zoom           Zoom() of a window on output 1 fills output 1
	  multi.center         CenterOnScreen() of that window centres it on
	                       output 1
	  multi.page_controls  the Displays page lists two displays and enables
	                       primary, mirroring, position and "Use this
	                       display"
	  multi.mirror         the page's mirroring: output 1 shows output 0,
	                       the screen is 1024x768 (screenshots
	                       multi_mirror0 and multi_mirror1; the harness
	                       compares them)
	  multi.deskbar_on_primary  Deskbar is on the primary output
	  multi.primary        the page makes output 1 primary; Deskbar moves
	                       there
	  multi.left_of        output 1 placed left of the primary output
	  multi.restored       without the settings file: back to the default
	                       arrangement
	One output:
	  multi.single_output  one output, the whole screen, primary
	Output format: see 3rdparty/ce/README.md.
*/


#include "DisplaysView.h"

#include <Application.h>
#include <Deskbar.h>
#include <GroupLayout.h>
#include <Screen.h>
#include <View.h>
#include <Window.h>

#include <ScreenOutputs.h>

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>


#define SUITE "multi"

static const char* kSettings
	= "/boot/home/config/settings/system/app_server/displays";

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


static void
host_screenshot(const char* name, int head)
{
	printf("CE-HOST screenshot_head %s %d\n", name, head);
	fflush(stdout);
	snooze(1500000);
}


static int32
outputs(screen_output_info* list)
{
	int32 count = 0;
	if (BPrivate::get_screen_outputs(list, 8, &count) != B_OK)
		return -1;
	return count;
}


static BString
describe(const screen_output_info* list, int32 count)
{
	BString text;
	for (int32 i = 0; i < count && i < 8; i++) {
		const screen_output_info& o = list[i];
		text << o.index << ":" << (o.enabled ? "on" : "off") << " "
			<< (int32)o.frame.left << "," << (int32)o.frame.top << " "
			<< (int32)o.frame.IntegerWidth() + 1 << "x"
			<< (int32)o.frame.IntegerHeight() + 1
			<< (o.primary ? " primary" : "")
			<< (o.mirror_of != 0xffffffff ? " mirror" : "") << "; ";
	}
	return text;
}


class ColorView : public BView {
public:
	ColorView()
		:
		BView("color", B_WILL_DRAW)
	{
		SetViewColor(255, 0, 255);
	}
};


static bool
rect_is(BRect rect, float left, float top, float width, float height)
{
	return rect.left == left && rect.top == top
		&& rect.IntegerWidth() + 1 == width
		&& rect.IntegerHeight() + 1 == height;
}


int
main()
{
	printf("CE-SUITE %s START\n", SUITE);
	fflush(stdout);
	BApplication application("application/x-vnd.Renga-DisplaysMultiTest");
	unlink(kSettings);

	screen_output_info list[8];
	int32 count = outputs(list);
	BRect screenFrame = BScreen().Frame();

	if (count <= 1) {
		report("single_output", count == 1 && list[0].primary
				&& list[0].frame == screenFrame,
			"%d output(s): %s screen %dx%d", (int)count,
			describe(list, count).String(),
			(int)screenFrame.IntegerWidth() + 1,
			(int)screenFrame.IntegerHeight() + 1);
		printf("CE-SUITE %s DONE %d\n", SUITE, sFailures);
		return sFailures == 0 ? 0 : 1;
	}

	report("outputs", count == 2 && list[0].enabled && list[1].enabled
			&& list[0].primary && !list[1].primary
			&& rect_is(list[0].frame, 0, 0, 1024, 768)
			&& rect_is(list[1].frame, 1024, 0, 1024, 768)
			&& rect_is(screenFrame, 0, 0, 2048, 768),
		"%s screen %dx%d", describe(list, count).String(),
		(int)screenFrame.IntegerWidth() + 1,
		(int)screenFrame.IntegerHeight() + 1);

	BRect deskbar = BDeskbar().Frame();
	report("deskbar_on_primary", list[0].frame.Contains(deskbar),
		"Deskbar at %d,%d-%d,%d", (int)deskbar.left, (int)deskbar.top,
		(int)deskbar.right, (int)deskbar.bottom);

	// a magenta window on output 1
	BWindow* window = new BWindow(BRect(1200, 200, 1499, 449), "On output 1",
		B_TITLED_WINDOW, 0);
	window->SetLayout(new BGroupLayout(B_VERTICAL, 0));
	window->AddChild(new ColorView());
	window->Show();
	snooze(1500000);
	host_screenshot("multi_head0", 0);
	host_screenshot("multi_head1", 1);
	report("second_window", true, "window at 1200,200 300x250 (the host "
		"checks the screenshots)");

	BRect zoomed;
	if (window->Lock()) {
		window->Zoom();
		window->Unlock();
	}
	snooze(500000);
	if (window->Lock()) {
		zoomed = window->Frame();
		window->Unlock();
	}
	report("zoom", zoomed.left >= 1024 && zoomed.right <= 2047
			&& zoomed.Width() > 800,
		"zoomed to %d,%d-%d,%d (output 1 is 1024,0-2047,767)",
		(int)zoomed.left, (int)zoomed.top, (int)zoomed.right,
		(int)zoomed.bottom);

	BRect centered;
	if (window->Lock()) {
		window->ResizeTo(299, 249);
		window->MoveTo(1100, 100);
		window->CenterOnScreen();
		centered = window->Frame();
		window->Unlock();
	}
	float centerX = (centered.left + centered.right) / 2;
	report("center", centerX > 1500 && centerX < 1572,
		"centred at x %d (output 1's centre is 1536)", (int)centerX);

	// the Displays page
	DisplaysView* page = new DisplaysView();
	BWindow* pageWindow = new BWindow(BRect(60, 60, 600, 700), "Displays",
		B_TITLED_WINDOW, B_AUTO_UPDATE_SIZE_LIMITS);
	pageWindow->SetLayout(new BGroupLayout(B_VERTICAL, 0));
	pageWindow->AddChild(page);
	pageWindow->Show();
	snooze(1500000);

	if (pageWindow->Lock()) {
		page->SelectDisplay(1);
		report("page_controls", page->CountDisplays() == 2
				&& page->FeatureEnabled("primary")
				&& page->FeatureEnabled("mirror")
				&& page->FeatureEnabled("position")
				&& page->FeatureEnabled("enabled"),
			"%d displays; primary %d, mirror %d, position %d, enabled %d",
			(int)page->CountDisplays(), page->FeatureEnabled("primary"),
			page->FeatureEnabled("mirror"), page->FeatureEnabled("position"),
			page->FeatureEnabled("enabled"));

		status_t status = page->ApplyArrangement(-1, 0, -1);
		pageWindow->Unlock();
		snooze(1000000);
		count = outputs(list);
		screenFrame = BScreen().Frame();
		report("mirror", status == B_OK && count == 2
				&& list[1].mirror_of == list[0].index
				&& rect_is(screenFrame, 0, 0, 1024, 768),
			"%s: %s screen %dx%d", strerror(status),
			describe(list, count).String(),
			(int)screenFrame.IntegerWidth() + 1,
			(int)screenFrame.IntegerHeight() + 1);
		host_screenshot("multi_mirror0", 0);
		host_screenshot("multi_mirror1", 1);
	}

	if (pageWindow->Lock()) {
		page->SelectDisplay(1);
		page->ApplyArrangement(-1, -1, 0);
			// back to its own picture, right of the primary
		status_t status = page->ApplyArrangement(1, -1, -1);
		pageWindow->Unlock();
		snooze(1000000);
		count = outputs(list);
		snooze(1000000);
		BRect deskbar = BDeskbar().Frame();
		report("primary", status == B_OK && count == 2 && list[1].primary
				&& !list[0].primary && list[1].mirror_of == 0xffffffff
				&& list[1].frame.Contains(deskbar),
			"%s: %s Deskbar at %d,%d", strerror(status),
			describe(list, count).String(), (int)deskbar.left,
			(int)deskbar.top);
	}

	if (pageWindow->Lock()) {
		page->SelectDisplay(0);
		status_t status = page->ApplyArrangement(-1, -1, 1);
			// output 0 left of the (new) primary output 1
		pageWindow->Unlock();
		snooze(1000000);
		count = outputs(list);
		report("left_of", status == B_OK && count == 2
				&& list[0].frame.right < list[1].frame.left
				&& list[0].frame.left == 0,
			"%s: %s", strerror(status), describe(list, count).String());
	}

	unlink(kSettings);
	BPrivate::reload_display_settings();
	snooze(1000000);
	count = outputs(list);
	report("restored", count == 2 && list[0].primary
			&& rect_is(list[0].frame, 0, 0, 1024, 768)
			&& rect_is(list[1].frame, 1024, 0, 1024, 768),
		"%s", describe(list, count).String());

	if (pageWindow->Lock())
		pageWindow->Quit();
	if (window->Lock())
		window->Quit();

	printf("CE-SUITE %s DONE %d\n", SUITE, sFailures);
	return sFailures == 0 ? 0 : 1;
}
