/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 */

/*!	In-guest test: app_server's copies from the back buffer to the screen
	(batch 7, the default display driver; <FrontCopyInfo.h>).

	The same two drawing loads run in each copy mode (plain memcpy, SIMD
	stores, SIMD with changed pixels only), measured with app_server's
	counters (AS_GET_FRONT_COPY_STATS):
	  "same"     a 640x480 view redrawn 60 times with the same pixels
	  "changing" the same view in a new colour each time
	The numbers are QEMU numbers (CE-INFO lines); they show the bytes
	written to the frame buffer, which is what costs on real hardware.

	  front_copy.default_changed   the default mode is "changed"
	  front_copy.same_skipped      "changed" writes at most a tenth of what
	                               the other modes write for "same"
	  front_copy.changing_written  ...and everything for "changing"
	  front_copy.pixels_<mode>     after each load the back buffer has the
	                               view's last colour (read back); the
	                               host checks the screen itself
	                               (front_copy.screen, QEMU's screendump:
	                               the view's colour after every mode, and
	                               no pointer trace in "changed" mode)
	  front_copy.cursor_no_trail   in "changed" mode the software pointer
	                               moved over the view and away leaves no
	                               trace (the shadow mirrors the pointer's
	                               pixels too)
	  front_copy.mode_switch       AS_SET_FRONT_COPY_MODE changes the mode
*/


#include <stdarg.h>

#include <algorithm>
#include <stdio.h>
#include <string.h>

#include <AppServerLink.h>
#include <Application.h>
#include <Bitmap.h>
#include <Screen.h>
#include <ServerProtocol.h>
#include <View.h>
#include <Window.h>

#include <FrontCopyInfo.h>


#define SUITE "front_copy"

static int sFailures = 0;
static BRect sScreen;


static void
result(const char* name, bool pass, const char* format = "", ...)
{
	char detail[512];
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


static bool
get_stats(front_copy_stats& stats, bool reset)
{
	BPrivate::AppServerLink link;
	link.StartMessage(AS_GET_FRONT_COPY_STATS);
	link.Attach<bool>(reset);
	int32 code;
	if (link.FlushWithReply(code) != B_OK || code != B_OK)
		return false;
	return link.Read(&stats, sizeof(stats)) == B_OK;
}


static bool
set_mode(int32 mode)
{
	BPrivate::AppServerLink link;
	link.StartMessage(AS_SET_FRONT_COPY_MODE);
	link.Attach<int32>(mode);
	int32 code;
	return link.FlushWithReply(code) == B_OK && code == B_OK;
}


static bool
read_pixel(BPoint where, rgb_color& color)
{
	BScreen screen;
	BBitmap* bitmap = NULL;
	BRect frame(where, where);
	if (screen.GetBitmap(&bitmap, false, &frame) != B_OK || bitmap == NULL)
		return false;
	uint8* bits = (uint8*)bitmap->Bits();
	color.blue = bits[0];
	color.green = bits[1];
	color.red = bits[2];
	delete bitmap;
	return true;
}


class FillView : public BView {
public:
	FillView(BRect frame)
		:
		BView(frame, "fill", B_FOLLOW_ALL, B_WILL_DRAW)
	{
		SetViewColor(B_TRANSPARENT_COLOR);
		fColor = make_color(40, 120, 200);
	}

	virtual void Draw(BRect update)
	{
		SetHighColor(fColor);
		FillRect(Bounds());
	}

	rgb_color	fColor;
};


static const char* kModeNames[] = { "plain", "stream", "changed" };


int
main()
{
	printf("CE-SUITE %s START\n", SUITE);
	fflush(stdout);
	BApplication app("application/x-vnd.Renga-front-copy-test");
	sScreen = BScreen().Frame();

	front_copy_stats stats;
	bool got = get_stats(stats, true);
	result("default_changed", got && stats.mode == B_FRONT_COPY_CHANGED,
		"mode %d, simd %d, shadow %u bytes", got ? (int)stats.mode : -1,
		got ? (int)stats.simd : -1, got ? stats.shadow_size : 0);

	BWindow* window = new BWindow(BRect(100, 100, 739, 579), "Front copy",
		B_TITLED_WINDOW, B_NOT_RESIZABLE);
	FillView* view = new FillView(window->Bounds());
	window->AddChild(view);
	window->Show();
	snooze(1000000);

	uint64 written[3][2] = {};
	uint64 requested[3][2] = {};
	for (int32 mode = 0; mode < 3; mode++) {
		set_mode(mode);
		snooze(500000);
		for (int load = 0; load < 2; load++) {
			get_stats(stats, true);
			for (int i = 0; i < 60; i++) {
				if (window->Lock()) {
					if (load == 1) {
						view->fColor = make_color(i * 4, 255 - i * 4,
							(i * 37) & 0xff);
					}
					view->Invalidate();
					window->Unlock();
				}
				// let each frame reach the screen before the next
				if (window->Lock()) {
					view->Sync();
					window->Unlock();
				}
				snooze(15000);
			}
			snooze(300000);
			get_stats(stats, false);
			written[mode][load] = stats.bytes_written;
			requested[mode][load] = stats.bytes_requested;
			printf("CE-INFO front_copy QEMU: mode %s, load %s: %" B_PRIu64
				" rects, %.1f MB requested, %.1f MB written, %.1f ms copying"
				"\n", kModeNames[mode], load == 0 ? "same" : "changing",
				stats.rects, stats.bytes_requested / 1048576.0,
				stats.bytes_written / 1048576.0, stats.copy_time / 1000.0);
			fflush(stdout);

			rgb_color shown;
			rgb_color expected = view->fColor;
			bool read = read_pixel(BPoint(420, 340), shown);
			if (load == 1) {
				char name[32];
				snprintf(name, sizeof(name), "pixels_%s", kModeNames[mode]);
				result(name, read && shown.red == expected.red
					&& shown.green == expected.green
					&& shown.blue == expected.blue,
					"%d,%d,%d, expected %d,%d,%d", shown.red, shown.green,
					shown.blue, expected.red, expected.green, expected.blue);
			}
			if (window->Lock()) {
				view->fColor = make_color(40, 120, 200);
				view->Invalidate();
				window->Unlock();
			}
			snooze(300000);
			if (load == 1) {
				// the screen itself (QEMU's screendump), checked by the host
				printf("CE-HOST screenshot front_copy_%s\n", kModeNames[mode]);
				fflush(stdout);
				snooze(300000);
			}
		}
	}

	uint64 others = std::max(written[0][0], written[1][0]);
	result("same_skipped", others > 0 && written[2][0] * 10 <= others,
		"changed wrote %" B_PRIu64 " bytes, plain %" B_PRIu64 ", stream %"
		B_PRIu64, written[2][0], written[0][0], written[1][0]);
	result("changing_written", requested[2][1] > 0
		&& written[2][1] * 10 >= requested[2][1] * 9,
		"changed wrote %" B_PRIu64 " of %" B_PRIu64 " bytes requested",
		written[2][1], requested[2][1]);

	// the software pointer over the view and away again
	set_mode(B_FRONT_COPY_CHANGED);
	snooze(500000);
	int32 width = sScreen.IntegerWidth() + 1;
	int32 height = sScreen.IntegerHeight() + 1;
	for (int i = 0; i < 6; i++) {
		printf("CE-HOST pointer usb %d %d\n",
			(int)((int64)(300 + i * 20) * 32767 / width),
			(int)((int64)(300 + i * 10) * 32767 / height));
		fflush(stdout);
		snooze(150000);
	}
	printf("CE-HOST pointer usb %d %d\n", (int)((int64)900 * 32767 / width),
		(int)((int64)650 * 32767 / height));
	fflush(stdout);
	snooze(600000);
	printf("CE-HOST screenshot front_copy_cursor\n");
	fflush(stdout);
	snooze(300000);
	int traces = 0;
	for (int i = 0; i < 6; i++) {
		for (int dy = 0; dy < 12; dy += 3) {
			for (int dx = 0; dx < 12; dx += 3) {
				rgb_color shown;
				if (read_pixel(BPoint(300 + i * 20 + dx, 300 + i * 10 + dy),
						shown) && !(shown.red == 40 && shown.green == 120
						&& shown.blue == 200))
					traces++;
			}
		}
	}
	result("cursor_no_trail", traces == 0, "%d pixels differ from the view",
		traces);

	set_mode(B_FRONT_COPY_STREAM);
	get_stats(stats, false);
	bool switched = stats.mode == B_FRONT_COPY_STREAM;
	set_mode(B_FRONT_COPY_CHANGED);
	get_stats(stats, false);
	result("mode_switch", switched && stats.mode == B_FRONT_COPY_CHANGED);

	window->Lock();
	window->Quit();
	printf("CE-SUITE %s DONE failures=%d\n", SUITE, sFailures);
	return sFailures == 0 ? 0 : 1;
}
