/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 */

/*!	In-guest test: snap layouts and visible workspaces (batch 7; roadmap
	F-25, Lumina rebuilt in app_server), with the S1-S8 acceptance check
	rewritten for the in-tree design (the 2026-11-12 rework):

	  S1 classic identical    without the snap key, and with snap layouts
	                          off, a drag moves the window exactly as
	                          stock Haiku does (snap.s1_*)
	  S2 window list          "Move to workspace" puts the window on that
	                          workspace and nowhere else (snap.s2_*)
	  S3 snap                 a drag with the snap key held drops the
	                          window into the zone, spanning with the span
	                          key (snap.s3_*)
	  S4 stock x20            twenty stock drags, each exact, no overlay
	                          left on screen (snap.s4_*)
	  S5 crash x3 -> stock    three damaged settings files: defaults,
	                          app_server keeps answering (snap.s5_*)
	  S6 escape               the master switch acts within about a second,
	                          without a restart (snap.s6_*)
	  S7 wrong-hrev engine    not applicable in the tree: there is no
	                          add-on whose version could differ
	  S8 effects              not applicable yet: no effects in the
	                          in-tree design

	Geometry and settings (header only, <window_layouts.h>): grid and
	custom layouts, gaps, nearest zone, spanning, the order workspace >
	screen > default, saving and loading, workspace sets (snap.model_*).

	Live, with the harness's USB tablet ("CE-HOST pointer usb", buttons
	"CE-HOST pointer_button usb down|up|right_down|right_up") and held
	keys ("CE-HOST key_hold shift|ctrl down|up"): drags, zone shortcuts
	(Command+Control+digit, "CE-HOST sendkey ctrl-alt-2"), the window menu
	on the tab, per-application rules, saved arrangements, the overview,
	and the Deskbar switcher (the Control Panel page is checked by
	control_panel_ui_test, in the renga-* images that have it).
	Edge snapping without a key (batch 8): model_edge_zone,
	model_edge_default, edge_snap (the tab dragged to the left edge fills
	the left half; screenshot snap_edge), edge_snap_off;
	model_tab_double_click and tab_double_click (minimize by default, zoom
	or nothing as set).
	Screenshots: snap_grid (the grid while choosing), snap_span,
	snap_window_menu, snap_switcher, snap_overview.

	The settings file is saved first and put back at the end.
*/


#include <algorithm>

#include <math.h>
#include <stdarg.h>
#include <stdio.h>

#include <string>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <AppServerLink.h>
#include <Application.h>
#include <Bitmap.h>
#include <Deskbar.h>
#include <Entry.h>
#include <Roster.h>
#include <Screen.h>
#include <String.h>
#include <ServerProtocol.h>
#include <View.h>
#include <Window.h>

#include <window_layouts.h>


#define SUITE "snap"

static int sFailures = 0;
static BRect sScreen;
static const int32 kAbsMax = 32767;
static const char* kSignature = "application/x-vnd.Renga-snap-layouts-test";


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


static void
host(const char* format, ...)
{
	va_list args;
	va_start(args, format);
	printf("CE-HOST ");
	vprintf(format, args);
	printf("\n");
	va_end(args);
	fflush(stdout);
}


static void
move_pointer(BPoint where)
{
	int32 width = sScreen.IntegerWidth() + 1;
	int32 height = sScreen.IntegerHeight() + 1;
	host("pointer usb %d %d", (int)((int64)where.x * kAbsMax / width),
		(int)((int64)where.y * kAbsMax / height));
	snooze(60000);
}


static bool
same(BRect a, BRect b, float tolerance = 1)
{
	return fabsf(a.left - b.left) <= tolerance
		&& fabsf(a.top - b.top) <= tolerance
		&& fabsf(a.right - b.right) <= tolerance
		&& fabsf(a.bottom - b.bottom) <= tolerance;
}


static const char*
text(BRect r)
{
	static char buffer[4][64];
	static int next = 0;
	char* b = buffer[next++ % 4];
	snprintf(b, 64, "(%d,%d)-(%d,%d)", (int)r.left, (int)r.top,
		(int)r.right, (int)r.bottom);
	return b;
}


// #pragma mark - settings file


static char sSettingsPath[B_PATH_NAME_LENGTH];
static char* sSavedSettings = NULL;
static size_t sSavedSize = 0;
static bool sHadSettings = false;


static void
backup_settings()
{
	WindowLayouts::path(sSettingsPath, sizeof(sSettingsPath));
	FILE* in = fopen(sSettingsPath, "r");
	if (in == NULL)
		return;
	sHadSettings = true;
	sSavedSettings = (char*)malloc(65536);
	sSavedSize = fread(sSavedSettings, 1, 65536, in);
	fclose(in);
}


static void
restore_settings()
{
	if (!sHadSettings) {
		unlink(sSettingsPath);
		return;
	}
	FILE* out = fopen(sSettingsPath, "w");
	if (out != NULL) {
		fwrite(sSavedSettings, 1, sSavedSize, out);
		fclose(out);
	}
}


//!	Writes the settings and waits until app_server has re-read them.
static void
apply(const WindowLayouts::Settings& settings)
{
	WindowLayouts::save(settings);
	snooze(1300000);
}


static int32
server_zones(BPoint where, BRect* frames, int32 max)
{
	BPrivate::AppServerLink link;
	link.StartMessage(AS_GET_SNAP_ZONES);
	link.Attach<BPoint>(where);
	int32 code;
	if (link.FlushWithReply(code) != B_OK || code != B_OK)
		return -1;
	int32 count = 0;
	link.Read<int32>(&count);
	for (int32 i = 0; i < count; i++) {
		BRect frame;
		link.Read<BRect>(&frame);
		if (i < max)
			frames[i] = frame;
	}
	return count;
}


static status_t
arrangement(int32 action, const char* name, int32* restored)
{
	BPrivate::AppServerLink link;
	link.StartMessage(AS_WINDOW_ARRANGEMENT);
	link.Attach<int32>(action);
	link.AttachString(name);
	int32 code;
	if (link.FlushWithReply(code) != B_OK)
		return B_ERROR;
	link.Read<int32>(restored);
	return code;
}


// #pragma mark - model checks (header only)


static void
model_checks()
{
	using namespace WindowLayouts;
	Settings settings;
	defaults(settings);
	result("model_defaults", settings.snap && settings.modifier == B_SHIFT_KEY
		&& settings.gap == 8 && strcmp(settings.defaultSpec, "1x2") == 0
		&& settings.tabMenu && settings.zoneShortcuts,
		"snap %d modifier %s gap %d default %s", settings.snap,
		modifier_name(settings.modifier), (int)settings.gap,
		settings.defaultSpec);

	Layout layout;
	resolve(settings, "1x2", layout);
	BRect frames[kMaxZones];
	BRect area(0, 0, 1023, 767);
	int32 count = zone_frames(layout, area, 8, frames, kMaxZones);
	result("model_halves_gap", count == 2
		&& same(frames[0], BRect(8, 8, 507, 759), 0)
		&& same(frames[1], BRect(516, 8, 1015, 759), 0),
		"%d zones %s %s", (int)count, text(frames[0]), text(frames[1]));

	resolve(settings, "2x2", layout);
	count = zone_frames(layout, area, 0, frames, kMaxZones);
	result("model_quarters", count == 4
		&& same(frames[0], BRect(0, 0, 511, 383), 0)
		&& same(frames[3], BRect(512, 384, 1023, 767), 0),
		"%d zones %s %s", (int)count, text(frames[0]), text(frames[3]));

	BRect span = span_frame(frames, count, 0, 3);
	result("model_span", same(span, area, 0), "%s", text(span));

	// Edge snapping without a key (batch 8): left edge middle -> left
	// half, left edge near the top -> top-left quarter, top edge -> the
	// whole area, bottom edge and the middle -> nothing.
	{
		BRect half, quarter, whole, none;
		bool gotHalf = edge_zone(area, area, BPoint(0, 384), 8, half);
		bool gotQuarter = edge_zone(area, area, BPoint(1, 20), 8, quarter);
		bool gotWhole = edge_zone(area, area, BPoint(500, 0), 8, whole);
		bool gotRight = edge_zone(area, area, BPoint(1023, 700), 8, none);
		BRect rightLower = none;
		bool gotBottom = edge_zone(area, area, BPoint(500, 767), 8, none);
		bool gotMiddle = edge_zone(area, area, BPoint(500, 384), 8, none);
		result("model_edge_zone", gotHalf && same(half, BRect(8, 8, 507, 759), 0)
				&& gotQuarter && same(quarter, BRect(8, 8, 507, 379), 0)
				&& gotWhole && same(whole, BRect(8, 8, 1015, 759), 0)
				&& gotRight && same(rightLower, BRect(516, 388, 1015, 759), 0)
				&& !gotBottom && !gotMiddle,
			"half %s, quarter %s, whole %s, right lower %s, bottom %d, "
			"middle %d", text(half), text(quarter), text(whole),
			text(rightLower), gotBottom, gotMiddle);
		result("model_edge_default", settings.edgeSnap, "edge_snap %d",
			settings.edgeSnap);

		FILE* file = tmpfile();
		fputs("tab_double_click zoom\nedge_snap false\n", file);
		rewind(file);
		Settings parsed;
		defaults(parsed);
		bool defaultMinimize
			= parsed.tabDoubleClick == kTabDoubleClickMinimize;
		load_from(file, parsed);
		fclose(file);
		result("model_tab_double_click", defaultMinimize
				&& parsed.tabDoubleClick == kTabDoubleClickZoom
				&& !parsed.edgeSnap,
			"default minimize %d, parsed %d, edge_snap %d", defaultMinimize,
			(int)parsed.tabDoubleClick, parsed.edgeSnap);
	}
	result("model_nearest", zone_at(frames, count, BPoint(600, 100)) == 1
		&& zone_at(frames, count, BPoint(-50, 900)) == 2,
		"%d %d", (int)zone_at(frames, count, BPoint(600, 100)),
		(int)zone_at(frames, count, BPoint(-50, 900)));

	// a file as the zone editor and the page write it
	FILE* file = tmpfile();
	fputs("snap false\nmodifier control\ngap 99\ndefault 9x9\n"
		"layout coding 0 0 0.6 1 0.6 0 1 0.5 0.6 0.5 1 1 0.5 0.5 0.4 0.4\n"
		"layout 2x2 0 0 1 1\n"
		"workspace 2 coding\ndisplay 1 3x3\nname 2 Mail and chat\n"
		"rule application/x-vnd.Haiku-StyledEdit 2\nrule bad 0\n", file);
	rewind(file);
	defaults(settings);
	load_from(file, settings);
	fclose(file);
	result("model_parse", !settings.snap && settings.modifier == B_CONTROL_KEY
		&& settings.gap == 64 && settings.layoutCount == 1
		&& settings.layouts[0].count == 3 && settings.ruleCount == 1
		&& strcmp(settings.workspaceName[2], "Mail and chat") == 0,
		"snap %d gap %d layouts %d zones %d rules %d name \"%s\"",
		settings.snap, (int)settings.gap, (int)settings.layoutCount,
		(int)settings.layouts[0].count, (int)settings.ruleCount,
		settings.workspaceName[2]);
	resolve(settings, spec_for(settings, 2, 1), layout);
	bool workspaceWins = strcmp(layout.name, "coding") == 0;
	resolve(settings, spec_for(settings, 0, 1), layout);
	bool displayNext = strcmp(layout.name, "3x3") == 0 && layout.count == 9;
	resolve(settings, spec_for(settings, 0, 0), layout);
	bool fallback = strcmp(layout.name, "1x2") == 0;
		// "9x9" is no grid: the default falls back to 1x2
	result("model_precedence", workspaceWins && displayNext && fallback,
		"workspace %d display %d fallback %d (%s)", workspaceWins,
		displayNext, fallback, layout.name);

	// a set of workspaces
	result("model_set_name", !save_set("../evil", settings, 3)
		&& !save_set("", settings, 3));
	Settings other;
	defaults(other);
	bool saved = save_set("snap-test", settings, 5);
	int32 setCount = load_set("snap-test", other);
	result("model_set", saved && setCount == 5
		&& strcmp(other.workspaceName[2], "Mail and chat") == 0
		&& strcmp(other.workspaceSpec[2], "coding") == 0 && other.snap,
		"saved %d count %d name \"%s\"", saved, (int)setCount,
		other.workspaceName[2]);
	char setFile[B_PATH_NAME_LENGTH];
	if (set_path("snap-test", setFile, sizeof(setFile)))
		unlink(setFile);
}


// #pragma mark - live checks


static const uint32 kMsgMenuSeen = 'smSn';


class TestWindow : public BWindow {
public:
	TestWindow(BRect frame, const char* title)
		:
		BWindow(frame, title, B_TITLED_WINDOW, B_ASYNCHRONOUS_CONTROLS),
		fMenuRequests(0),
		fZooms(0),
		fMinimizes(0)
	{
		BView* view = new BView(Bounds(), "content", B_FOLLOW_ALL, 0);
		view->SetViewColor(200, 220, 200);
		AddChild(view);
	}

	virtual void DispatchMessage(BMessage* message, BHandler* target)
	{
		if (message->what == WindowLayouts::kMsgWindowMenu) {
			fMenuRequests++;
			fLastMenu = *message;
		}
		// counted and not acted on, so the window stays as it is
		if (message->what == B_ZOOM) {
			fZooms++;
			return;
		}
		if (message->what == B_MINIMIZE) {
			fMinimizes++;
			return;
		}
		BWindow::DispatchMessage(message, target);
	}

	BRect CurrentFrame()
	{
		BRect frame;
		if (Lock()) {
			frame = Frame();
			Unlock();
		}
		return frame;
	}

	BRect Insets()
	{
		// left, top, right, bottom of the decorator around the content
		BRect insets;
		if (Lock()) {
			BRect frame = Frame();
			BRect decorator = DecoratorFrame();
			insets.Set(frame.left - decorator.left, frame.top - decorator.top,
				decorator.right - frame.right,
				decorator.bottom - frame.bottom);
			Unlock();
		}
		return insets;
	}

	BPoint TabPoint()
	{
		BRect frame = CurrentFrame();
		BRect insets = Insets();
		return BPoint(frame.left + 30, frame.top - insets.top / 2 - 1);
	}

	int32		fMenuRequests;
	BMessage	fLastMenu;
	int32		fZooms;
	int32		fMinimizes;
};


/*!	After the button went up in a drag with a key held: waits (up to 3 s)
	for the drop to move the window away from \a atDrop before the key is
	released. The tablet's button and the keyboard reach app_server by
	different paths, and on a loaded host the key's release could arrive
	first, which ends the zone choice before the drop (the stock drag then,
	by design); s3_snap_drop failed that way in some runs.
*/
static void
wait_for_drop(TestWindow* window, BRect atDrop)
{
	for (int i = 0; i < 30; i++) {
		snooze(100000);
		if (window->CurrentFrame() != atDrop)
			return;
	}
}


//!	A double-click on the window's tab.
static void
double_click_tab(TestWindow* window)
{
	BPoint where = window->TabPoint();
	move_pointer(where);
	snooze(300000);
	for (int i = 0; i < 2; i++) {
		host("pointer_button usb down");
		snooze(60000);
		host("pointer_button usb up");
		snooze(60000);
	}
	snooze(600000);
}


static BRect
content_for(TestWindow* window, BRect zone)
{
	BRect insets = window->Insets();
	return BRect(zone.left + insets.left, zone.top + insets.top,
		zone.right - insets.right, zone.bottom - insets.bottom);
}


//!	A drag from the window's tab: \a keys are held from the first move on.
static void
drag(TestWindow* window, BPoint to, const char* keys = NULL,
	const char* screenshot = NULL, const char* moreKeys = NULL,
	BPoint* via = NULL)
{
	BPoint from = window->TabPoint();
	move_pointer(from);
	snooze(150000);
	host("pointer_button usb down");
	snooze(150000);
	move_pointer(from + BPoint(20, 10));
	if (keys != NULL) {
		host("key_hold %s down", keys);
		snooze(150000);
	}
	if (via != NULL) {
		move_pointer(*via);
		snooze(200000);
	}
	if (moreKeys != NULL) {
		host("key_hold %s down", moreKeys);
		snooze(150000);
	}
	// the step before the target stays on the screen (the tablet refuses
	// negative positions; edge_snap drags to x = 0)
	BPoint before = to - BPoint(5, 5);
	before.x = std::max(before.x, sScreen.left);
	before.y = std::max(before.y, sScreen.top);
	move_pointer(before);
	move_pointer(to);
	snooze(400000);
	if (screenshot != NULL) {
		host("screenshot %s", screenshot);
		snooze(300000);
	}
	BRect atDrop = window->CurrentFrame();
	host("pointer_button usb up");
	snooze(200000);
	if (keys != NULL)
		wait_for_drop(window, atDrop);
	if (moreKeys != NULL)
		host("key_hold %s up", moreKeys);
	if (keys != NULL)
		host("key_hold %s up", keys);
	snooze(500000);
}


static bool
read_pixel(BPoint where, rgb_color& color)
{
	BScreen screen;
	BBitmap* bitmap = NULL;
	if (screen.GetBitmap(&bitmap, false, &sScreen) != B_OK || bitmap == NULL)
		return false;
	uint8* bits = (uint8*)bitmap->Bits() + (int32)where.y
		* bitmap->BytesPerRow() + (int32)where.x * 4;
	color.blue = bits[0];
	color.green = bits[1];
	color.red = bits[2];
	color.alpha = 255;
	delete bitmap;
	return true;
}


static bool
same_color(rgb_color a, rgb_color b)
{
	return abs(a.red - b.red) <= 8 && abs(a.green - b.green) <= 8
		&& abs(a.blue - b.blue) <= 8;
}


static void
live_checks()
{
	using namespace WindowLayouts;
	Settings settings;
	defaults(settings);
	settings.gap = 0;
	apply(settings);

	TestWindow* window = new TestWindow(BRect(200, 200, 499, 399), "Snap test");
	window->Show();
	window->Activate();
	snooze(800000);
	BRect start = window->CurrentFrame();
	BRect insets = window->Insets();
	result("window", start.IsValid() && insets.top > insets.left,
		"frame %s insets %s", text(start), text(insets));

	BRect zones[kMaxZones];
	int32 count = server_zones(BPoint(100, 100), zones, kMaxZones);
	result("server_zones", count == 2 && zones[0].left == sScreen.left
		&& zones[1].right <= sScreen.right
		&& zones[0].right + 1 == zones[1].left,
		"%d zones %s %s (screen %s)", (int)count, text(zones[0]),
		text(zones[1]), text(sScreen));
	if (count != 2) {
		window->PostMessage(B_QUIT_REQUESTED);
		return;
	}

	// S1: no key: exactly the stock drag
	BRect before = window->CurrentFrame();
	drag(window, window->TabPoint() + BPoint(60, 40));
	BRect after = window->CurrentFrame();
	result("s1_stock_drag", same(after, before.OffsetByCopy(60, 40))
		&& after.Width() == before.Width(), "%s -> %s", text(before),
		text(after));

	// S3: Shift held: the grid, the drop puts it into the right zone
	BPoint right((zones[1].left + zones[1].right) / 2,
		(zones[1].top + zones[1].bottom) / 2);
	rgb_color highlight = ui_color(B_KEYBOARD_NAVIGATION_COLOR);
	{
		// look at the overlay while choosing: a custom drag
		BPoint from = window->TabPoint();
		move_pointer(from);
		snooze(150000);
		host("pointer_button usb down");
		snooze(150000);
		move_pointer(from + BPoint(20, 10));
		host("key_hold shift down");
		snooze(150000);
		move_pointer(right);
		snooze(500000);
		rgb_color edge;
		bool read = read_pixel(BPoint(right.x, zones[1].top + 1), edge);
		host("screenshot snap_grid");
		snooze(300000);
		result("s3_overlay_shown", read && same_color(edge, highlight),
			"pixel %d,%d,%d highlight %d,%d,%d", edge.red, edge.green,
			edge.blue, highlight.red, highlight.green, highlight.blue);
		BRect atDrop = window->CurrentFrame();
		host("pointer_button usb up");
		snooze(200000);
		wait_for_drop(window, atDrop);
		host("key_hold shift up");
		snooze(600000);
		after = window->CurrentFrame();
		BRect expected = content_for(window, zones[1]);
		result("s3_snap_drop", same(after, expected), "%s, expected %s",
			text(after), text(expected));
		read = read_pixel(BPoint(right.x, zones[1].top + 1), edge);
		result("s3_overlay_gone", read && !same_color(edge, highlight),
			"pixel %d,%d,%d", edge.red, edge.green, edge.blue);
	}

	// zone shortcuts: Command+Control+1 (Ctrl+Alt in QEMU), +0 restores
	window->Activate();
	snooze(300000);
	BRect snapped = window->CurrentFrame();
	host("sendkey ctrl-alt-1");
	snooze(800000);
	after = window->CurrentFrame();
	result("zone_shortcut", same(after, content_for(window, zones[0])),
		"%s, expected %s", text(after),
		text(content_for(window, zones[0])));
	host("sendkey ctrl-alt-0");
	snooze(800000);
	after = window->CurrentFrame();
	result("zone_restore", same(after, before.OffsetByCopy(60, 40)),
		"%s, expected the frame before snapping %s (after the drop %s)",
		text(after), text(before.OffsetByCopy(60, 40)), text(snapped));

	// spanning: 2x2, Shift at the top-left zone, Control as well, drop at
	// the bottom-right: all four
	strcpy(settings.defaultSpec, "2x2");
	apply(settings);
	count = server_zones(BPoint(100, 100), zones, kMaxZones);
	if (count == 4) {
		BPoint topLeft((zones[0].left + zones[0].right) / 2,
			(zones[0].top + zones[0].bottom) / 2);
		BPoint bottomRight((zones[3].left + zones[3].right) / 2,
			(zones[3].top + zones[3].bottom) / 2);
		drag(window, bottomRight, "shift", "snap_span", "ctrl", &topLeft);
		after = window->CurrentFrame();
		BRect expected = content_for(window, zones[0] | zones[3]);
		result("s3_span", same(after, expected), "%s, expected %s",
			text(after), text(expected));
	} else
		result("s3_span", false, "%d zones for 2x2", (int)count);
	strcpy(settings.defaultSpec, "1x2");
	apply(settings);
	count = server_zones(BPoint(100, 100), zones, kMaxZones);

	// back to a free window for the rest
	host("sendkey ctrl-alt-0");
	snooze(600000);
	if (window->Lock()) {
		window->ResizeTo(299, 199);
		window->MoveTo(200, 200);
		window->Unlock();
	}
	snooze(400000);

	// S4: twenty stock drags
	int32 exact = 0;
	for (int32 i = 0; i < 20; i++) {
		before = window->CurrentFrame();
		BPoint delta(i % 2 == 0 ? 40 : -40, i % 4 < 2 ? 20 : -20);
		drag(window, window->TabPoint() + delta);
		after = window->CurrentFrame();
		if (same(after, before.OffsetByCopy(delta))
			&& after.Width() == before.Width()
			&& after.Height() == before.Height())
			exact++;
	}
	rgb_color edge;
	bool read = read_pixel(BPoint((zones[1].left + zones[1].right) / 2,
		zones[1].top + 1), edge);
	result("s4_stock_x20", exact == 20, "%d of 20 exact", (int)exact);
	result("s4_no_overlay", read && !same_color(edge, highlight),
		"pixel %d,%d,%d", edge.red, edge.green, edge.blue);

	// Edge snapping without a key (batch 8): the tab dragged to the left
	// edge puts the window into the left half (the 1x2 layout's zone 0 is
	// the same frame); with the switch off it is a stock drag.
	{
		BPoint leftEdge(sScreen.left, (sScreen.top + sScreen.bottom) / 2);
		drag(window, leftEdge, NULL, "snap_edge");
		after = window->CurrentFrame();
		BRect expected = content_for(window, zones[0]);
		result("edge_snap", same(after, expected), "%s, expected %s",
			text(after), text(expected));

		host("sendkey ctrl-alt-0");
		snooze(600000);
		if (window->Lock()) {
			window->ResizeTo(299, 199);
			window->MoveTo(200, 200);
			window->Unlock();
		}
		snooze(400000);
		settings.edgeSnap = false;
		apply(settings);
		before = window->CurrentFrame();
		drag(window, leftEdge);
		after = window->CurrentFrame();
		result("edge_snap_off", after.Width() == before.Width()
				&& after.Height() == before.Height()
				&& !same(after, expected),
			"%s -> %s", text(before), text(after));
		settings.edgeSnap = true;
		apply(settings);
		if (window->Lock()) {
			window->MoveTo(200, 200);
			window->Unlock();
		}
		snooze(400000);
	}

	// Double-click on the tab (batch 8): minimize by default, zoom or
	// nothing as set. B_MINIMIZE and B_ZOOM are counted, not acted on.
	{
		int32 minimizes = window->fMinimizes;
		int32 zooms = window->fZooms;
		double_click_tab(window);
		bool minimized = window->fMinimizes == minimizes + 1
			&& window->fZooms == zooms;
		settings.tabDoubleClick = WindowLayouts::kTabDoubleClickZoom;
		apply(settings);
		minimizes = window->fMinimizes;
		double_click_tab(window);
		bool zoomed = window->fZooms == zooms + 1
			&& window->fMinimizes == minimizes;
		settings.tabDoubleClick = WindowLayouts::kTabDoubleClickNone;
		apply(settings);
		zooms = window->fZooms;
		double_click_tab(window);
		bool nothing = window->fZooms == zooms
			&& window->fMinimizes == minimizes;
		settings.tabDoubleClick = WindowLayouts::kTabDoubleClickMinimize;
		apply(settings);
		result("tab_double_click", minimized && zoomed && nothing,
			"minimize %d, zoom %d, nothing %d (B_MINIMIZE %d, B_ZOOM %d)",
			minimized, zoomed, nothing, (int)window->fMinimizes,
			(int)window->fZooms);
	}

	// S6: the master switch off acts without a restart
	settings.snap = false;
	apply(settings);
	before = window->CurrentFrame();
	drag(window, window->TabPoint() + BPoint(-50, 30), "shift");
	after = window->CurrentFrame();
	result("s6_switch_off", same(after, before.OffsetByCopy(-50, 30))
		&& after.Width() == before.Width(), "%s -> %s", text(before),
		text(after));
	settings.snap = true;
	apply(settings);

	// the window menu: a right-click on the tab
	int32 requests = window->fMenuRequests;
	move_pointer(window->TabPoint());
	host("pointer_button usb right_down");
	snooze(100000);
	host("pointer_button usb right_up");
	snooze(800000);
	host("screenshot snap_window_menu");
	snooze(300000);
	result("window_menu", window->fMenuRequests == requests + 1
		&& window->fLastMenu.GetInt32("zones", 0) == 2
		&& window->fLastMenu.GetInt32("workspaces", 0) == count_workspaces(),
		"requests %d, zones %d, workspaces %d", (int)(window->fMenuRequests
			- requests), (int)window->fLastMenu.GetInt32("zones", 0),
		(int)window->fLastMenu.GetInt32("workspaces", 0));
	host("sendkey esc");
	snooze(400000);

	// S2: "Move to workspace" through the menu's message
	int32 workspaces = count_workspaces();
	if (workspaces < 2)
		set_workspace_count(2);
	BMessage move(kMsgWindowMenuAction);
	move.AddInt32("workspace", 1);
	window->PostMessage(&move);
	snooze(600000);
	uint32 on = 0;
	if (window->Lock()) {
		on = window->Workspaces();
		window->Unlock();
	}
	result("s2_move_to_workspace", on == (1UL << 1), "workspaces 0x%" B_PRIx32,
		on);
	if (window->Lock()) {
		window->SetWorkspaces(B_CURRENT_WORKSPACE);
		window->Unlock();
	}
	snooze(300000);

	// "Snap to" through the menu's message
	BMessage zoneMessage(kMsgWindowMenuAction);
	zoneMessage.AddInt32("zone", 1);
	window->PostMessage(&zoneMessage);
	snooze(600000);
	after = window->CurrentFrame();
	result("menu_snap_to", same(after, content_for(window, zones[1])),
		"%s, expected %s", text(after), text(content_for(window, zones[1])));

	// the menu off: a right-click sends the window behind as before
	settings.tabMenu = false;
	apply(settings);
	requests = window->fMenuRequests;
	window->Activate();
	snooze(300000);
	move_pointer(window->TabPoint());
	host("pointer_button usb right_down");
	snooze(100000);
	host("pointer_button usb right_up");
	snooze(800000);
	result("window_menu_off", window->fMenuRequests == requests,
		"requests %d", (int)(window->fMenuRequests - requests));
	settings.tabMenu = true;

	// saved arrangements
	int32 restored = 0;
	status_t status = arrangement(0, "snap-test", &restored);
	before = window->CurrentFrame();
	if (window->Lock()) {
		window->MoveTo(150, 150);
		window->ResizeTo(250, 150);
		window->Unlock();
	}
	snooze(400000);
	status_t restoreStatus = arrangement(1, "snap-test", &restored);
	snooze(400000);
	after = window->CurrentFrame();
	result("arrangement", status == B_OK && restoreStatus == B_OK
		&& restored >= 1 && same(after, before), "save %s restore %s, %d "
		"restored, %s, expected %s", strerror(status),
		strerror(restoreStatus), (int)restored, text(after), text(before));
	result("arrangement_name", arrangement(0, "../evil", &restored)
		== B_BAD_VALUE);
	{
		char dir[B_PATH_NAME_LENGTH];
		char file[B_PATH_NAME_LENGTH + 64];
		if (WindowLayouts::directory(dir, sizeof(dir))) {
			snprintf(file, sizeof(file), "%s/window_arrangements/snap-test",
				dir);
			unlink(file);
		}
	}

	// per-application rules: a new window of this application opens in
	// zone 2
	settings.ruleCount = 1;
	strlcpy(settings.ruleList[0].signature, kSignature,
		sizeof(settings.ruleList[0].signature));
	settings.ruleList[0].zone = 2;
	apply(settings);
	TestWindow* ruled = new TestWindow(BRect(100, 300, 299, 449), "Ruled");
	ruled->Show();
	snooze(800000);
	after = ruled->CurrentFrame();
	result("rule_zone", same(after, content_for(ruled, zones[1])),
		"%s, expected %s", text(after), text(content_for(ruled, zones[1])));
	settings.rules = false;
	apply(settings);
	TestWindow* free = new TestWindow(BRect(100, 300, 299, 449), "Not ruled");
	free->Show();
	snooze(800000);
	after = free->CurrentFrame();
	result("rule_off", same(after, BRect(100, 300, 299, 449)), "%s",
		text(after));
	ruled->PostMessage(B_QUIT_REQUESTED);
	free->PostMessage(B_QUIT_REQUESTED);
	settings.rules = true;
	settings.ruleCount = 0;

	// workspace names reach the window menu
	strlcpy(settings.workspaceName[0], "Main", sizeof(settings.workspaceName[0]));
	apply(settings);
	settings.tabMenu = true;
	apply(settings);
	window->Activate();
	snooze(300000);
	move_pointer(window->TabPoint());
	host("pointer_button usb right_down");
	snooze(100000);
	host("pointer_button usb right_up");
	snooze(800000);
	result("workspace_name", strcmp(window->fLastMenu.GetString("name", 0, ""),
		"Main") == 0, "\"%s\"", window->fLastMenu.GetString("name", 0, ""));
	host("sendkey esc");
	snooze(400000);

	// the overview: opened, then closed when it loses the focus
	const char* args[] = { "--overview", NULL };
	be_roster->Launch("application/x-vnd.Be-WORK", 1, args);
	snooze(1500000);
	bool running = be_roster->IsRunning("application/x-vnd.Be-WORK");
	host("screenshot snap_overview");
	snooze(300000);
	window->Activate();
	bool closed = false;
	for (int i = 0; i < 30 && !closed; i++) {
		snooze(100000);
		closed = !be_roster->IsRunning("application/x-vnd.Be-WORK");
	}
	result("overview", running && closed, "running %d, closed after losing "
		"the focus %d", running, closed);

	// the Deskbar switcher with its "+" (the Deskbar may still be starting
	// on the renga-* images)
	for (int i = 0; i < 300 && !be_roster->IsRunning(
			"application/x-vnd.Be-TSKB"); i++)
		snooze(100000);
	snooze(1000000);
	if (!be_roster->IsRunning("application/x-vnd.Be-TSKB")) {
		// renga-* images show the first-boot prompt in this phase, without
		// a Deskbar; the minimum-* images check the switcher
		printf("CE-INFO snap.deskbar_switcher not checked: no Deskbar "
			"running in this phase\n");
		window->PostMessage(B_QUIT_REQUESTED);
		set_workspace_count(workspaces);
		return;
	}
	BDeskbar deskbar;
	bool had = deskbar.HasItem("workspaces");
	BEntry entry("/boot/system/apps/Workspaces");
	entry_ref ref;
	status_t added = B_OK;
	if (!had && entry.GetRef(&ref) == B_OK)
		added = deskbar.AddItem(&ref);
	bool has = false;
	for (int i = 0; i < 50 && !has; i++) {
		snooze(100000);
		has = deskbar.HasItem("workspaces");
	}
	host("screenshot snap_switcher");
	snooze(300000);
	result("deskbar_switcher", added == B_OK && has, "add %s, has %d",
		strerror(added), has);
	if (!had)
		deskbar.RemoveItem("workspaces");

	window->PostMessage(B_QUIT_REQUESTED);
	set_workspace_count(workspaces);
}


static void
damaged_settings_checks()
{
	// S5: three damaged files: defaults, app_server keeps answering
	int32 good = 0;
	for (int round = 0; round < 3; round++) {
		FILE* out = fopen(sSettingsPath, "w");
		if (out == NULL)
			break;
		for (int i = 0; i < 4096; i++)
			fputc((i * 131 + round * 17) & 0xff, out);
		fputs("\nlayout x 5 5 -1 -1\nrule \ngap -3\nworkspace 99 2x2\n"
			"name -1 x\ndefault 0x0\n", out);
		fclose(out);
		snooze(1300000);
		WindowLayouts::Settings settings;
		WindowLayouts::load(settings);
		BRect zones[WindowLayouts::kMaxZones];
		int32 count = server_zones(BPoint(10, 10), zones,
			WindowLayouts::kMaxZones);
		if (count == 2 && settings.gap == 0 && settings.layoutCount == 0)
			good++;
	}
	result("s5_damaged_x3", good == 3, "%d of 3 rounds with defaults",
		(int)good);
}


/*!	snap.window_menu_catalog (batch 8): the window menu's strings are in
	libbe's catalog under the context "Window menu", so translators get
	them. The English catalog in the image is built from the sources'
	B_TRANSLATE() keys (collectcatkeys), so it holds each string and its
	context. Known-bad: the menu used plain literals and Window.cpp was not
	a catalog source of libbe, so none of them were there.
*/
static void
catalog_checks()
{
	const char* kCatalog = "/boot/system/data/locale/catalogs/"
		"x-vnd.Haiku-libbe/en.catalog";
	const char* kStrings[] = { "Window menu", "Move to workspace",
		"Workspace %number%", "New workspace", "Snap to", "Zone %number%",
		"Restore size", "Minimize", "Zoom", "Close", NULL };
	FILE* file = fopen(kCatalog, "rb");
	std::string data;
	if (file != NULL) {
		char buffer[65536];
		size_t got;
		while ((got = fread(buffer, 1, sizeof(buffer), file)) > 0)
			data.append(buffer, got);
		fclose(file);
	}
	int found = 0;
	BString missing;
	for (int i = 0; kStrings[i] != NULL; i++) {
		std::string key(kStrings[i]);
		key.push_back('\0');
		if (data.find(key) != std::string::npos)
			found++;
		else
			missing << " \"" << kStrings[i] << "\"";
	}
	result("window_menu_catalog", file != NULL && missing.IsEmpty(),
		"%d of 10 strings in %s%s%s", found, kCatalog,
		missing.IsEmpty() ? "" : "; missing:", missing.String());
}


class TestApp : public BApplication {
public:
	TestApp() : BApplication(kSignature) {}

	virtual void ReadyToRun()
	{
		sScreen = BScreen().Frame();
		backup_settings();
		catalog_checks();
		model_checks();
		live_checks();
		damaged_settings_checks();
		restore_settings();
		PostMessage(B_QUIT_REQUESTED);
	}
};


int
main()
{
	printf("CE-SUITE %s START\n", SUITE);
	fflush(stdout);
	TestApp app;
	app.Run();
	printf("CE-SUITE %s DONE failures=%d\n", SUITE, sFailures);
	return sFailures == 0 ? 0 : 1;
}
