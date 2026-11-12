/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 */

/*!	In-guest test: reading the screen without the front buffer (roadmap
	F-59; src/servers/app/ScreenReader.h).

	BScreen::ReadBitmap() and BScreenCapture used to copy the front buffer
	(slow video memory on real graphics cards) under the drawing engine's
	exclusive lock, which stops the pointer for the whole copy. Now they
	read the back buffer in main memory and composite the cursor
	themselves; only direct windows and BWindowScreen are read from the
	front buffer, and windows in an update session from their last
	completed frame.

	app_server's test hook AS_TEST_SCREEN_READ (harness images only;
	headers/private/app/ScreenReadInfo.h) switches BScreen::ReadBitmap()
	back to the old front-buffer read, for comparison, and returns totals:
	reads, time with the engine locked, pixels from each source, pointer
	moves and how long they waited for the lock.

	  screen_read.matches_front    a read of the whole screen without the
	                               cursor equals the old front-buffer read,
	                               except around the pointer
	  screen_read.no_cursor        without the cursor: the pixel under the
	                               pointer is the window's (the old read had
	                               the software cursor there)
	  screen_read.cursor           with the cursor: around the pointer it
	                               equals the front buffer, where the
	                               software cursor is drawn, within 1 per
	                               channel
	  screen_read.direct_window    a BDirectWindow's content, drawn into the
	                               front buffer only, is read (from there)
	  screen_read.update_session   a window in an update session (its view
	                               cleared, half redrawn, the client
	                               sleeping before the rest) is read as its
	                               last completed frame
	  screen_read.update_done      after EndUpdate() the new frame, and no
	                               saved update region is left
	  screen_read.sources          the totals: pixels from the back buffer,
	                               the front buffer (direct window) and
	                               saved update regions
	  screen_read.cost_old/new     15 full-screen reads while the host moves
	                               the pointer, the old way and the new way:
	                               time per read, time with the engine
	                               locked, the pointer's longest wait
	                               (numbers in the detail; QEMU's frame
	                               buffer is ordinary memory, so they show
	                               the lock, not video memory's slowness)

	Known-bad cases: with the old read, no_cursor and cursor fail, and
	direct_window passes only because everything is read from the front
	buffer; without the front-buffer areas direct_window fails, without the
	saved update regions update_session fails (the cleared view is read).

	Without a desktop the test prints an INFO line and reports nothing.
	Output format: see 3rdparty/ce/README.md.
*/


#include <Application.h>
#include <Autolock.h>
#include <Region.h>
#include <Bitmap.h>
#include <Screen.h>
#include <View.h>
#include <Window.h>

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <AppServerLink.h>
#include <ScreenReadInfo.h>
#include <ServerProtocol.h>


#define SUITE "screen_read"

static int sFailures = 0;
static const int32 kAbsMax = 32767;
static BRect sScreen;

static const rgb_color kGreen = { 40, 160, 90, 255 };
static const rgb_color kBlue = { 0, 0, 200, 255 };
static const rgb_color kYellow = { 200, 200, 0, 255 };
static const rgb_color kWhite = { 255, 255, 255, 255 };
static const uint32 kMagenta = 0xffff00ff;	// B G R A in memory: ff 00 ff ff


static void
report(const char* name, bool pass, const char* format, ...)
{
	char detail[320];
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


static status_t
test_hook(int32 legacy, bool reset, screen_read_stats* stats = NULL)
{
	BPrivate::AppServerLink link;
	link.StartMessage(AS_TEST_SCREEN_READ);
	link.Attach<int32>(legacy);
	link.Attach<bool>(reset);
	status_t status;
	if (link.FlushWithReply(status) != B_OK)
		return B_ERROR;
	if (status != B_OK)
		return status;
	screen_read_stats local;
	return link.Read(stats != NULL ? stats : &local, sizeof(local));
}


static void
move_pointer(BPoint where)
{
	int32 width = sScreen.IntegerWidth() + 1;
	int32 height = sScreen.IntegerHeight() + 1;
	printf("CE-HOST pointer usb %d %d\n",
		(int)((int64)where.x * kAbsMax / width),
		(int)((int64)where.y * kAbsMax / height));
	fflush(stdout);
}


static bool
wait_pointer(BPoint where)
{
	for (int32 i = 0; i < 250; i++) {
		BPoint position;
		uint32 buttons;
		if (get_mouse(&position, &buttons) == B_OK && position == where)
			return true;
		snooze(20000);
	}
	return false;
}


static uint32
pixel(BBitmap* bitmap, BPoint where)
{
	return *(uint32*)((uint8*)bitmap->Bits()
		+ (int32)where.y * bitmap->BytesPerRow() + (int32)where.x * 4);
}


static uint32
color_pixel(rgb_color color)
{
	return 0xff000000 | (color.red << 16) | (color.green << 8) | color.blue;
}


//! RGB only (the alpha byte of a screen read is not meaningful)
static bool
same_rgb(uint32 a, uint32 b, int32 tolerance = 0)
{
	for (int32 shift = 0; shift < 24; shift += 8) {
		int32 difference = (int32)((a >> shift) & 0xff)
			- (int32)((b >> shift) & 0xff);
		if (difference > tolerance || difference < -tolerance)
			return false;
	}
	return true;
}


static BBitmap*
read_screen(bool drawCursor)
{
	BBitmap* bitmap = new BBitmap(sScreen, B_RGBA32);
	memset(bitmap->Bits(), 0, bitmap->BitsLength());
	BScreen().ReadBitmap(bitmap, drawCursor);
	return bitmap;
}


// #pragma mark - windows


class ColorView : public BView {
public:
	ColorView(BRect frame, rgb_color color)
		:
		BView(frame, "color", B_FOLLOW_ALL, B_WILL_DRAW),
		fColor(color)
	{
		SetViewColor(color);
	}

	virtual void Draw(BRect updateRect)
	{
		SetHighColor(fColor);
		FillRect(updateRect);
	}

private:
	rgb_color fColor;
};


/*!	Draws blue; after Redraw(), yellow, in two halves with a pause in
	between, while the server is in the update session.
*/
class UpdateView : public BView {
public:
	UpdateView(BRect frame)
		:
		BView(frame, "update", B_FOLLOW_ALL, B_WILL_DRAW),
		fState(0),
		fDrawn(0)
	{
		SetViewColor(kWhite);
	}

	virtual void Draw(BRect updateRect)
	{
		BRect bounds = Bounds();
		if (atomic_get(&fState) == 0) {
			SetHighColor(kBlue);
			FillRect(bounds);
		} else {
			BRect left = bounds;
			left.right = floorf(bounds.Width() / 2);
			SetHighColor(kYellow);
			FillRect(left);
			Sync();
			snooze(2500000);
			BRect right = bounds;
			right.left = left.right + 1;
			FillRect(right);
		}
		atomic_add(&fDrawn, 1);
	}

	void Redraw()
	{
		atomic_set(&fState, 1);
		Invalidate();
	}

	int32 Drawn() { return atomic_get(&fDrawn); }

private:
	int32	fState;
	int32	fDrawn;
};


// #pragma mark - the cost


struct MoverArgs {
	BPoint	a;
	BPoint	b;
	volatile bool stop;
};


static status_t
mover(void* data)
{
	MoverArgs* args = (MoverArgs*)data;
	for (int32 i = 0; !args->stop && i < 200; i++) {
		move_pointer((i & 1) != 0 ? args->a : args->b);
		snooze(30000);
	}
	return B_OK;
}


static void
measure(const char* name, int32 legacy, BPoint a, BPoint b)
{
	test_hook(legacy, true);
	MoverArgs args = { a, b, false };
	thread_id thread = spawn_thread(mover, "mover", B_NORMAL_PRIORITY, &args);
	resume_thread(thread);
	snooze(100000);

	const int32 kReads = 15;
	BBitmap* bitmap = new BBitmap(sScreen, B_RGBA32);
	bigtime_t start = system_time();
	for (int32 i = 0; i < kReads; i++)
		BScreen().ReadBitmap(bitmap, true);
	bigtime_t elapsed = system_time() - start;
	delete bitmap;

	args.stop = true;
	status_t result;
	wait_for_thread(thread, &result);

	screen_read_stats stats;
	status_t status = test_hook(-1, true, &stats);
	int32 reads = max_c(stats.reads, 1);
	int32 moves = max_c(stats.cursor_moves, 1);
	report(name, status == B_OK && stats.reads == kReads
			&& stats.cursor_moves > 0,
		"%d reads, %.1f ms each in the client, %.1f ms in app_server, engine "
		"locked %.1f ms on average (longest %.1f ms); %d pointer moves, "
		"waited %.2f ms on average, longest %.1f ms",
		(int)stats.reads, elapsed / 1000.0 / kReads,
		stats.read_time_total / 1000.0 / reads,
		stats.lock_held_total / 1000.0 / reads,
		stats.lock_held_max / 1000.0, (int)stats.cursor_moves,
		stats.cursor_wait_total / 1000.0 / moves,
		stats.cursor_wait_max / 1000.0);
}


int
main(int argc, char** argv)
{
	BApplication app("application/x-vnd.Renga-screen-read-test");
	printf("CE-SUITE %s START\n", SUITE);

	if (!BScreen().IsValid() || access("/dev/input/tablet/usb", F_OK) != 0) {
		printf("CE-INFO screen_read: no desktop or no USB tablet, nothing to "
			"test\n");
		printf("CE-SUITE %s DONE failures=0\n", SUITE);
		return 0;
	}
	sScreen = BScreen().Frame();
	if (test_hook(0, true) != B_OK) {
		report("hook", false, "AS_TEST_SCREEN_READ refused");
		printf("CE-SUITE %s DONE failures=%d\n", SUITE, sFailures);
		return 1;
	}

	// Three windows side by side below the top edge.
	BRect plainFrame(40, 80, 239, 279);
	BRect directFrame(280, 80, 479, 279);
	BRect updateFrame(520, 80, 719, 279);

	BWindow* plain = new BWindow(plainFrame, "Plain", B_TITLED_WINDOW,
		B_NOT_RESIZABLE | B_NOT_ZOOMABLE);
	plain->AddChild(new ColorView(plain->Bounds(), kGreen));
	plain->Show();

	// The direct window is a program of its own (libgame; not in the
	// minimum images).
	pid_t direct = -1;
	bool haveDirect = access("/boot/system/lib/libgame.so", F_OK) == 0;
	if (haveDirect) {
		direct = fork();
		if (direct == 0) {
			char left[16], top[16], right[16], bottom[16];
			snprintf(left, sizeof(left), "%g", directFrame.left);
			snprintf(top, sizeof(top), "%g", directFrame.top);
			snprintf(right, sizeof(right), "%g", directFrame.right);
			snprintf(bottom, sizeof(bottom), "%g", directFrame.bottom);
			execl("/boot/system/non-packaged/bin/screen_read_direct",
				"screen_read_direct", left, top, right, bottom, (char*)NULL);
			_exit(127);
		}
	} else {
		printf("CE-INFO screen_read: no libgame in this image, direct "
			"windows are checked on the renga-* images\n");
	}

	BWindow* updating = new BWindow(updateFrame, "Update", B_TITLED_WINDOW,
		B_NOT_RESIZABLE | B_NOT_ZOOMABLE);
	UpdateView* updateView = new UpdateView(updating->Bounds());
	updating->AddChild(updateView);
	updating->Show();
	snooze(1500000);

	// The pointer over the plain window.
	BPoint inPlain(floorf(plainFrame.left + 100), floorf(plainFrame.top + 100));
	move_pointer(inPlain);
	bool reached = wait_pointer(inPlain);
	snooze(300000);

	// 1. The same as the front buffer, except around the pointer.
	test_hook(1, false);
	BBitmap* oldNoCursor = read_screen(false);
	BBitmap* oldCursor = read_screen(true);
	test_hook(0, false);
	BBitmap* newNoCursor = read_screen(false);
	BBitmap* newCursor = read_screen(true);

	BRect around(inPlain.x - 8, inPlain.y - 8, inPlain.x + 40,
		inPlain.y + 40);
	int64 differing = 0;
	int64 compared = 0;
	for (int32 y = 0; y <= sScreen.IntegerHeight(); y++) {
		for (int32 x = 0; x <= sScreen.IntegerWidth(); x++) {
			BPoint where(x, y);
			if (around.Contains(where) || directFrame.Contains(where))
				continue;
			compared++;
			if (!same_rgb(pixel(oldNoCursor, where), pixel(newNoCursor, where)))
				differing++;
		}
	}
	report("matches_front", reached && differing * 2000 <= compared,
		"%lld of %lld pixels differ (at most 0.05 %% allowed: a clock "
		"changing between the reads); pointer placed %d", differing,
		compared, reached);

	uint32 under = pixel(newNoCursor, inPlain);
	uint32 oldUnder = pixel(oldNoCursor, inPlain);
	report("no_cursor", same_rgb(under, color_pixel(kGreen)),
		"under the pointer 0x%06x, the window is 0x%06x (old read: 0x%06x)",
		(unsigned)(under & 0xffffff),
		(unsigned)(color_pixel(kGreen) & 0xffffff),
		(unsigned)(oldUnder & 0xffffff));

	int32 cursorPixels = 0;
	int32 mismatches = 0;
	for (int32 y = (int32)around.top; y <= (int32)around.bottom; y++) {
		for (int32 x = (int32)around.left; x <= (int32)around.right; x++) {
			BPoint where(x, y);
			uint32 now = pixel(newCursor, where);
			if (!same_rgb(now, color_pixel(kGreen)))
				cursorPixels++;
			// the front buffer holds the software cursor as drawn on
			// screen (the old read with the cursor drew it a second time)
			if (!same_rgb(now, pixel(oldNoCursor, where), 1))
				mismatches++;
		}
	}
	report("cursor", cursorPixels > 20 && mismatches == 0,
		"%d pixels of the cursor drawn, %d differ from the screen (the front "
		"buffer) by more than 1", (int)cursorPixels, (int)mismatches);

	// 2. The direct window's content exists only in the front buffer.
	screen_read_stats stats;
	if (haveDirect) {
		test_hook(-1, true);
		BPoint inDirect(floorf(directFrame.left + 100),
			floorf(directFrame.top + 100));
		uint32 directPixel = 0;
		for (int32 i = 0; i < 30; i++) {
			BBitmap* shot = read_screen(false);
			directPixel = pixel(shot, inDirect);
			delete shot;
			if (same_rgb(directPixel, kMagenta))
				break;
			snooze(100000);
		}
		test_hook(-1, false, &stats);
		report("direct_window", same_rgb(directPixel, kMagenta)
				&& stats.front_pixels > 0,
			"pixel 0x%06x (want magenta 0xff00ff), %lld pixels from the "
			"front buffer", (unsigned)(directPixel & 0xffffff),
			stats.front_pixels);
	}

	// 3. A window in an update session.
	BPoint leftHalf(floorf(updateFrame.left + 50), floorf(updateFrame.top + 100));
	BPoint rightHalf(floorf(updateFrame.left + 150),
		floorf(updateFrame.top + 100));
	delete read_screen(false);
		// a capture now: update regions are saved for a few seconds
	test_hook(-1, true);
	int32 drawn = updateView->Drawn();
	updating->Lock();
	updateView->Redraw();
	updating->Unlock();
	snooze(1000000);
	BBitmap* during = read_screen(false);
	test_hook(-1, false, &stats);
	uint32 left = pixel(during, leftHalf);
	uint32 right = pixel(during, rightHalf);
	delete during;
	report("update_session", same_rgb(left, color_pixel(kBlue))
			&& same_rgb(right, color_pixel(kBlue)) && stats.snapshot_pixels > 0
			&& stats.update_snapshots == 1,
		"left 0x%06x, right 0x%06x (want the last frame, blue 0x0000c8; the "
		"view is being redrawn yellow over white), %lld pixels from saved "
		"update regions, %d saved", (unsigned)(left & 0xffffff),
		(unsigned)(right & 0xffffff), stats.snapshot_pixels,
		(int)stats.update_snapshots);

	for (int32 i = 0; i < 50 && updateView->Drawn() == drawn; i++)
		snooze(100000);
	snooze(500000);
	BBitmap* after = read_screen(false);
	left = pixel(after, leftHalf);
	right = pixel(after, rightHalf);
	delete after;
	test_hook(-1, false, &stats);
	report("update_done", same_rgb(left, color_pixel(kYellow))
			&& same_rgb(right, color_pixel(kYellow))
			&& stats.update_snapshots == 0,
		"left 0x%06x, right 0x%06x (want yellow 0xc8c800), %d saved update "
		"regions left", (unsigned)(left & 0xffffff),
		(unsigned)(right & 0xffffff), (int)stats.update_snapshots);

	test_hook(-1, true, &stats);
	report("sources", stats.back_pixels > 0
			&& (stats.front_pixels > 0 || !haveDirect)
			&& stats.snapshot_pixels > 0,
		"%lld pixels from the back buffer, %lld from the front buffer, %lld "
		"from saved update regions", stats.back_pixels, stats.front_pixels,
		stats.snapshot_pixels);

	// 4. The cost, with the pointer moving.
	plain->Lock();
	plain->Quit();
	if (direct > 0) {
		kill(direct, SIGKILL);
		waitpid(direct, NULL, 0);
	}
	updating->Lock();
	updating->Quit();
	snooze(300000);
	BPoint a(sScreen.Width() / 3, sScreen.Height() / 2);
	BPoint b(sScreen.Width() / 2, sScreen.Height() / 3);
	measure("cost_old", 1, a, b);
	measure("cost_new", 0, a, b);
	test_hook(0, true);

	delete oldNoCursor;
	delete oldCursor;
	delete newNoCursor;
	delete newCursor;

	printf("CE-SUITE %s DONE failures=%d\n", SUITE, sFailures);
	return sFailures == 0 ? 0 : 1;
}
