/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 */

/*!	In-guest test: drag bitmaps with a hardware cursor (roadmap F-59).

	With a hardware cursor, app_server used to show nothing of a dragged
	icon: AccelerantHWInterface::_DrawCursor() draws nothing while the
	hardware cursor is on, and the accelerant was only ever given the
	pointer itself. Now the accelerant gets the pointer and the drag bitmap
	composited into one B_RGBA32 image through set_cursor_bitmap(), and the
	software cursor is used only if it refuses that image.

	QEMU's display drivers have no hardware cursor, so app_server's stand-in
	is used (AS_TEST_HARDWARE_CURSOR, harness images only;
	headers/private/app/HardwareCursorInfo.h): it accepts images up to a
	given size, keeps what it was given and draws nothing, and
	AS_GET_HARDWARE_CURSOR_INFO reports what it holds. The drags are real:
	the test's window starts them from MouseDown() with
	BView::DragMessage(), the harness presses and releases the USB tablet's
	button ("CE-HOST pointer_button usb down|up") and moves it ("CE-HOST
	pointer usb <x> <y>").

	  hw_cursor.stand_in        the stand-in is accepted and holds the
	                            pointer (hardware cursor on, size, hot spot)
	  hw_cursor.drag_hardware   a 64x64 drag bitmap: still a hardware cursor,
	                            the image is the union of bitmap and pointer
	  hw_cursor.drag_hot_spot   with the pointer's hot spot where the drag
	                            offset puts it
	  hw_cursor.drag_pixels     the bitmap's pixels (opaque red) in the image,
	                            the pointer over it at its hot spot
	  hw_cursor.drag_moves      moving while dragging moves the hardware
	                            cursor, no software fallback
	  hw_cursor.drag_end        after the button is released: the pointer
	                            alone again, in hardware
	  hw_cursor.large_limit     a 300x300 bitmap with a 256x256 limit that the
	                            stand-in reports (B_GET_CURSOR_MAX_SIZE): the
	                            software cursor, without a refused call
	                            (screenshot hw_cursor_software_drag)
	  hw_cursor.large_back      and the hardware cursor again afterwards
	  hw_cursor.large_refused   the same without the size hook: the call is
	                            refused, the software cursor shows the drag
	  hw_cursor.large_refused_back   and the hardware cursor afterwards
	  hw_cursor.setting_off     the setting off (AS_SET_HARDWARE_CURSOR_DRAG,
	                            what the Input preferences send): a 64x64
	                            drag goes to the software cursor
	  hw_cursor.setting_on      and on again: in hardware
	  hw_cursor.prefs_switch    Input preferences: "Show dragged items with
	                            the hardware pointer" turned off from its
	                            window (kMsgDragInHardware) reaches
	                            app_server, which writes its settings file
	                            (screenshot input_drag_setting)
	  hw_cursor.prefs_defaults  Defaults turns it on again and removes the
	                            file
	  hw_cursor.restored        the stand-in removed: the display driver's
	                            own cursor state is back

	A known-bad case of the old behaviour: before F-59 drag_hardware
	failed (the stand-in kept the plain pointer's size) and drag_pixels
	found no bitmap pixels.

	Without a desktop the test prints an INFO line and reports nothing.
	Output format: see 3rdparty/ce/README.md.
*/


#include <Application.h>
#include <Messenger.h>
#include <Roster.h>
#include <Bitmap.h>
#include <Message.h>
#include <Screen.h>
#include <View.h>
#include <Window.h>

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <AppServerLink.h>
#include <HardwareCursorInfo.h>
#include <ServerProtocol.h>


#define SUITE "hw_cursor"

static int sFailures = 0;
static const int32 kAbsMax = 32767;


static void
report(const char* name, bool pass, const char* format, ...)
{
	char detail[256];
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
set_stand_in(bool enable, uint16 maxSize, bool reportMaxSize)
{
	BPrivate::AppServerLink link;
	link.StartMessage(AS_TEST_HARDWARE_CURSOR);
	link.Attach<int32>(enable ? 1 : 0);
	link.Attach<uint16>(maxSize);
	link.Attach<uint16>(maxSize);
	link.Attach<bool>(reportMaxSize);
	status_t status;
	if (link.FlushWithReply(status) != B_OK)
		return B_ERROR;
	return status;
}


static status_t
get_info(hardware_cursor_info& info, int32 x = -1, int32 y = -1)
{
	memset(&info, 0, sizeof(info));
	BPrivate::AppServerLink link;
	link.StartMessage(AS_GET_HARDWARE_CURSOR_INFO);
	link.Attach<int32>(x);
	link.Attach<int32>(y);
	status_t status;
	if (link.FlushWithReply(status) != B_OK)
		return B_ERROR;
	if (status != B_OK)
		return status;
	return link.Read(&info, sizeof(info));
}


static BRect sScreen;


static bool
drag_setting()
{
	BPrivate::AppServerLink link;
	link.StartMessage(AS_GET_HARDWARE_CURSOR_DRAG);
	status_t status;
	bool enabled = false;
	if (link.FlushWithReply(status) == B_OK && status == B_OK)
		link.Read<bool>(&enabled);
	return enabled;
}


static void
set_drag_setting(bool enabled)
{
	BPrivate::AppServerLink link;
	link.StartMessage(AS_SET_HARDWARE_CURSOR_DRAG);
	link.Attach<bool>(enabled);
	link.Flush();
	drag_setting();
		// a round trip: the setting is applied
}


static const char* kSettingsFile
	= "/boot/home/config/settings/system/app_server/hardware_cursor";
static const char* kInputSignature = "application/x-vnd.Haiku-Input";
static const uint32 kMsgDragInHardware = 'PUdh';
static const uint32 kMsgDefaults = 'BTde';


//! Puts the USB tablet's pointer on pixel (x, y), through the host.
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


static void
button(bool down)
{
	printf("CE-HOST pointer_button usb %s\n", down ? "down" : "up");
	fflush(stdout);
}


static bool
wait_pointer(BPoint where, bigtime_t timeout = 5000000)
{
	bigtime_t end = system_time() + timeout;
	while (system_time() < end) {
		BPoint position;
		uint32 buttons;
		if (get_mouse(&position, &buttons) == B_OK && position == where)
			return true;
		snooze(20000);
	}
	return false;
}


//! Waits until the drag bitmap is set (or not).
static bool
wait_drag(bool set, hardware_cursor_info& info, bigtime_t timeout = 5000000)
{
	bigtime_t end = system_time() + timeout;
	while (system_time() < end) {
		if (get_info(info) == B_OK && info.drag_bitmap == set)
			return true;
		snooze(20000);
	}
	return false;
}


// #pragma mark - the window drags start from


class DragView : public BView {
public:
	DragView(BRect frame)
		:
		BView(frame, "drag", B_FOLLOW_ALL, B_WILL_DRAW),
		fSize(64),
		fMouseDowns(0)
	{
		SetViewColor(220, 220, 220);
	}

	virtual void MouseDown(BPoint where)
	{
		atomic_add(&fMouseDowns, 1);
		int32 size = atomic_get(&fSize);
		BBitmap* bitmap = new BBitmap(BRect(0, 0, size - 1, size - 1),
			B_RGBA32);
		uint32* bits = (uint32*)bitmap->Bits();
		int32 count = bitmap->BitsLength() / 4;
		for (int32 i = 0; i < count; i++)
			bits[i] = 0xffff0000;	// opaque red (B G R A in memory)
		BMessage message('drag');
		DragMessage(&message, bitmap, B_OP_ALPHA, BPoint(10, 10));
			// takes ownership of the bitmap
	}

	void SetDragSize(int32 size) { atomic_set(&fSize, size); }
	int32 MouseDowns() { return atomic_get(&fMouseDowns); }

private:
	int32	fSize;
	int32	fMouseDowns;
};


/*!	One drag: presses the button on \a start, waits for the drag bitmap,
	calls \a during, releases and waits until the drag ended.
*/
static bool
drag(DragView* view, BPoint start, hardware_cursor_info& during)
{
	// A real move onto the start point and the window active, before each
	// press. Input delivery is not what this test is about: a press that
	// did not start a drag (seen once in a full renga-raw run after the
	// 300x300 drop) is tried once more, and logged.
	for (int32 attempt = 0; attempt < 2; attempt++) {
		BWindow* window = view->Window();
		if (window->Lock()) {
			window->Activate();
			window->Unlock();
		}
		move_pointer(start + BPoint(3, 3));
		wait_pointer(start + BPoint(3, 3));
		move_pointer(start);
		if (!wait_pointer(start)) {
			printf("CE-INFO hw_cursor: pointer did not reach %g,%g\n",
				start.x, start.y);
			return false;
		}
		snooze(200000);
		int32 downs = view->MouseDowns();
		button(true);
		if (wait_drag(true, during))
			return true;
		printf("CE-INFO hw_cursor: no drag (mouse downs %d -> %d), attempt "
			"%d\n", (int)downs, (int)view->MouseDowns(), (int)attempt + 1);
		button(false);
		snooze(500000);
	}
	return false;
}


static bool
end_drag(hardware_cursor_info& after)
{
	button(false);
	return wait_drag(false, after);
}


int
main(int argc, char** argv)
{
	BApplication app("application/x-vnd.Renga-hw-cursor-test");
	printf("CE-SUITE %s START\n", SUITE);

	if (!BScreen().IsValid() || access("/dev/input/tablet/usb", F_OK) != 0) {
		printf("CE-INFO hw_cursor: no desktop or no USB tablet, nothing to "
			"test\n");
		printf("CE-SUITE %s DONE failures=0\n", SUITE);
		return 0;
	}
	sScreen = BScreen().Frame();

	hardware_cursor_info before;
	get_info(before);

	// 1. The stand-in holds the pointer.
	status_t status = set_stand_in(true, 256, true);
	hardware_cursor_info info;
	get_info(info);
	report("stand_in", status == B_OK && info.test_hooks && info.hardware
			&& info.width > 0 && info.width <= 64 && info.height > 0
			&& info.height <= 64 && !info.drag_bitmap,
		"status 0x%x, stand-in %d, hardware %d, %ux%u, hot spot %u,%u",
		(unsigned)status, info.test_hooks, info.hardware, info.width,
		info.height, info.hot_x, info.hot_y);
	if (status != B_OK) {
		printf("CE-SUITE %s DONE failures=%d\n", SUITE, sFailures);
		return 1;
	}
	uint16 plainWidth = info.width;
	uint16 plainHeight = info.height;
	uint16 plainHotX = info.hot_x;
	uint16 plainHotY = info.hot_y;

	BRect frame(sScreen.Width() / 2 - 200, sScreen.Height() / 2 - 150,
		sScreen.Width() / 2 + 200, sScreen.Height() / 2 + 150);
	BWindow* window = new BWindow(frame, "Hardware cursor test",
		B_TITLED_WINDOW, B_NOT_RESIZABLE | B_NOT_ZOOMABLE
			| B_ASYNCHRONOUS_CONTROLS);
	DragView* view = new DragView(window->Bounds());
	window->AddChild(view);
	window->Show();
	window->Lock();
	window->Activate();
	window->Unlock();
	snooze(500000);

	BPoint start(floorf(frame.left + 100), floorf(frame.top + 100));

	// 2. A 64x64 drag bitmap goes into the hardware image.
	hardware_cursor_info dragging;
	bool started = drag(view, start, dragging);
	get_info(dragging);
	// The image is the union of the bitmap (at -offset from the hot spot)
	// and the pointer: with the offset 10,10 and a pointer whose hot spot
	// is at its top left (the default arrow), 10 + 54 = 64 pixels plus
	// what of the pointer reaches beyond the bitmap.
	int32 left = min_c(-10, -(int32)plainHotX);
	int32 top = min_c(-10, -(int32)plainHotY);
	int32 right = max_c(-10 + 64, (int32)plainWidth - plainHotX);
	int32 bottom = max_c(-10 + 64, (int32)plainHeight - plainHotY);
	int32 wantWidth = right - left;
	int32 wantHeight = bottom - top;
	report("drag_hardware", started && dragging.hardware
			&& dragging.drag_in_hardware && dragging.width == wantWidth
			&& dragging.height == wantHeight
			&& dragging.software_fallbacks == 0,
		"drag started %d, hardware %d, drag in hardware %d, %ux%u (want "
		"%dx%d), fallbacks %d", started, dragging.hardware,
		dragging.drag_in_hardware, dragging.width, dragging.height,
		(int)wantWidth, (int)wantHeight, (int)dragging.software_fallbacks);
	report("drag_hot_spot", started && dragging.hot_x == -left
			&& dragging.hot_y == -top,
		"hot spot %u,%u (want %d,%d)", dragging.hot_x, dragging.hot_y,
		(int)-left, (int)-top);

	// A pixel of the bitmap away from the pointer, and the hot spot.
	hardware_cursor_info sample;
	get_info(sample, dragging.hot_x - 10 + 50, dragging.hot_y - 10 + 50);
	uint32 bitmapPixel = sample.sample;
	// The pointer is drawn over the bitmap: some pixel just below and
	// right of the hot spot (inside the arrow) is not the bitmap's red,
	// and every pixel there is opaque (the bitmap is).
	int32 pointerPixels = 0;
	int32 transparent = 0;
	uint32 pointerPixel = 0;
	for (int32 i = 0; i < 8; i++) {
		get_info(sample, dragging.hot_x + i / 2, dragging.hot_y + i);
		if ((sample.sample >> 24) != 0xff)
			transparent++;
		else if (((sample.sample >> 16) & 0xff) < 0xfd
			|| (sample.sample & 0xffff) != 0) {
			pointerPixels++;
			pointerPixel = sample.sample;
		}
	}
	// Premultiplied like the software cursor's image: 255 * 255 >> 8 is
	// 254 (HWInterface::_AdoptDragBitmap()).
	bool red = (bitmapPixel >> 24) == 0xff
		&& ((bitmapPixel >> 16) & 0xff) >= 0xfd
		&& (bitmapPixel & 0xffff) == 0;
	report("drag_pixels", started && red
			&& pointerPixels > 0 && transparent == 0,
		"bitmap pixel 0x%08x (want opaque red); below the hot spot %d of 8 "
		"pointer pixels (e.g. 0x%08x), %d not opaque", (unsigned)bitmapPixel,
		(int)pointerPixels, (unsigned)pointerPixel, (int)transparent);

	BPoint moved(start.x + 40, start.y + 30);
	move_pointer(moved);
	bool reached = wait_pointer(moved);
	snooze(100000);
	get_info(info);
	report("drag_moves", started && reached && info.hardware
			&& info.drag_in_hardware && info.position_x == moved.x
			&& info.position_y == moved.y && info.software_fallbacks == 0,
		"pointer reached %d, hardware %d, drag in hardware %d, hardware "
		"position %u,%u (want %g,%g)", reached, info.hardware,
		info.drag_in_hardware, info.position_x, info.position_y, moved.x,
		moved.y);

	hardware_cursor_info after;
	bool ended = end_drag(after);
	get_info(after);
	report("drag_end", ended && after.hardware && !after.drag_in_hardware
			&& after.width == plainWidth && after.height == plainHeight
			&& after.hot_x == plainHotX && after.hot_y == plainHotY,
		"ended %d, hardware %d, %ux%u (want %ux%u), hot spot %u,%u",
		ended, after.hardware, after.width, after.height, plainWidth,
		plainHeight, after.hot_x, after.hot_y);

	// 3. Larger than the limit the stand-in reports: software cursor
	// without asking the hardware.
	view->SetDragSize(300);
	set_stand_in(true, 256, true);
	started = drag(view, start, dragging);
	snooze(300000);
	get_info(dragging);
	report("large_limit", started && !dragging.hardware
			&& dragging.software_fallbacks >= 1 && dragging.refused == 0
			&& dragging.size_queries >= 1,
		"drag started %d, hardware %d, fallbacks %d, refused %d, size "
		"queries %d", started, dragging.hardware,
		(int)dragging.software_fallbacks, (int)dragging.refused,
		(int)dragging.size_queries);
	printf("CE-HOST screenshot hw_cursor_software_drag\n");
	fflush(stdout);
	snooze(500000);
	ended = end_drag(after);
	get_info(after);
	report("large_back", ended && after.hardware && after.width == plainWidth
			&& after.height == plainHeight,
		"ended %d, hardware %d, %ux%u", ended, after.hardware, after.width,
		after.height);

	// 4. The same without B_GET_CURSOR_MAX_SIZE: the call is refused.
	set_stand_in(true, 256, false);
	started = drag(view, start, dragging);
	snooze(300000);
	get_info(dragging);
	report("large_refused", started && !dragging.hardware
			&& dragging.software_fallbacks >= 1 && dragging.refused >= 1
			&& dragging.size_queries == 0,
		"drag started %d, hardware %d, fallbacks %d, refused %d, size "
		"queries %d", started, dragging.hardware,
		(int)dragging.software_fallbacks, (int)dragging.refused,
		(int)dragging.size_queries);
	ended = end_drag(after);
	get_info(after);
	report("large_refused_back", ended && after.hardware
			&& after.width == plainWidth,
		"ended %d, hardware %d, %ux%u", ended, after.hardware, after.width,
		after.height);

	// 5. The setting.
	view->SetDragSize(64);
	set_stand_in(true, 256, true);
	set_drag_setting(false);
	started = drag(view, start, dragging);
	snooze(200000);
	get_info(dragging);
	report("setting_off", started && !drag_setting() && !dragging.hardware
			&& !dragging.drag_compositing && dragging.software_fallbacks >= 1,
		"drag started %d, setting %d, hardware %d, fallbacks %d", started,
		dragging.drag_compositing, dragging.hardware,
		(int)dragging.software_fallbacks);
	end_drag(after);
	set_drag_setting(true);
	started = drag(view, start, dragging);
	get_info(dragging);
	report("setting_on", started && dragging.drag_compositing
			&& dragging.hardware && dragging.drag_in_hardware,
		"drag started %d, setting %d, hardware %d, drag in hardware %d",
		started, dragging.drag_compositing, dragging.hardware,
		dragging.drag_in_hardware);
	end_drag(after);

	// The Input preferences' check box.
	window->Lock();
	window->Hide();
	window->Unlock();
	if (access("/boot/system/preferences/Input", X_OK) == 0) {
		team_id team = -1;
		be_roster->Launch(kInputSignature, (BMessage*)NULL, &team);
		// The window, once it answers (a busy desktop takes a while to
		// start an application).
		BMessenger input;
		for (int32 i = 0; i < 150 && !input.IsValid(); i++) {
			BMessenger app(kInputSignature);
			BMessage get(B_GET_PROPERTY);
			get.AddSpecifier("Messenger");
			get.AddSpecifier("Window", (int32)0);
			BMessage reply;
			if (app.IsValid() && app.SendMessage(&get, &reply, 1000000,
					1000000) == B_OK)
				reply.FindMessenger("result", &input);
			if (!input.IsValid())
				snooze(100000);
		}
		snooze(1000000);
		BMessage off(kMsgDragInHardware);
		off.AddInt32("renga:value", 0);
		status_t sent = input.SendMessage(&off);
		for (int32 i = 0; i < 50 && drag_setting(); i++)
			snooze(100000);
		snooze(300000);
		bool fileWritten = access(kSettingsFile, F_OK) == 0;
		bool setting = drag_setting();
		printf("CE-HOST screenshot input_drag_setting\n");
		fflush(stdout);
		snooze(300000);
		report("prefs_switch", sent == B_OK && !setting && fileWritten,
			"sent 0x%x, app_server setting %d, settings file %s",
			(unsigned)sent, setting, fileWritten ? "written" : "missing");

		BMessage defaults(kMsgDefaults);
		sent = input.SendMessage(&defaults);
		for (int32 i = 0; i < 50 && !drag_setting(); i++)
			snooze(100000);
		snooze(300000);
		fileWritten = access(kSettingsFile, F_OK) == 0;
		setting = drag_setting();
		report("prefs_defaults", sent == B_OK && setting && !fileWritten,
			"sent 0x%x, app_server setting %d, settings file %s",
			(unsigned)sent, setting, fileWritten ? "still there" : "removed");
		if (team >= 0)
			BMessenger(NULL, team).SendMessage(B_QUIT_REQUESTED);
		snooze(500000);
	} else
		printf("CE-INFO hw_cursor: no Input preferences in this image\n");
	set_drag_setting(true);

	// 6. The display driver's own state is back.
	window->Lock();
	window->Quit();
	status = set_stand_in(false, 0, false);
	get_info(info);
	report("restored", status == B_OK && !info.test_hooks
			&& info.hardware == before.hardware,
		"status 0x%x, stand-in %d, hardware %d (before %d)",
		(unsigned)status, info.test_hooks, info.hardware, before.hardware);

	printf("CE-SUITE %s DONE failures=%d\n", SUITE, sFailures);
	return sFailures == 0 ? 0 : 1;
}
