/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 */

/*!	In-guest test: a 1000 Hz mouse while app_server is busy (the pointer
	and packaging fix, 2026-11-13; the maintainer's Darmoshark M3 lagged
	behind the hand, worst after boot).

	The harness images carry ce_vmouse (src/add-ons/kernel/drivers/input/
	ce_vmouse), a virtual mouse this test plugs in and moves with reports
	of its own, at 1000 reports a second or any other rate, through the
	whole real path: the mouse add-on's watcher, input_server's event loop,
	app_server's event loop and a window of this test that records every
	pointer event it gets (B_FULL_POINTER_HISTORY). app_server is made busy
	with AS_TEST_STALL_INPUT (harness images only): it holds the Desktop's
	window lock, as long drawing does, so pointer events pile up.

	The mouse runs with speed 1 and no acceleration for the position checks,
	so a count is a pixel:

	  device                  the virtual mouse appears as "Virtual Mouse 1"
	  backlog.final_position  800 reports of 1 count at 1000 Hz with app_server
	                          busy for 600 ms in the middle: the pointer ends
	                          exactly 800 pixels on
	  backlog.caught_up       and gets there at most 60 ms after the hand
	                          stopped (before: the backlog was replayed, with
	                          a queueing driver for seconds)
	  backlog.no_replay       after the busy time the window gets at most two
	                          moves that are older than the busy time's end
	                          (before: the queued moves were replayed one by
	                          one)
	  clicks.order            presses, a drag, a release, a double click and a
	                          secondary click made while app_server is busy
	                          arrive in order, none lost, none merged
	  clicks.positions        each at exactly the position the motion before
	                          it had reached
	  long_stall.clicks       a click at the end of 1.9 s of motion while
	                          app_server is busy for 2 s still arrives, at the
	                          right place (before: app_server's port, 200
	                          messages, filled up and input_server dropped
	                          what did not fit, the press and release too)
	  accel_rate.adaptive     with the default speed and acceleration, the
	                          same hand speed at 1000 and at 125 reports a
	                          second, each after a second of rest, moves the
	                          pointer the same distance (within 5 %); before,
	                          the watcher read the first ~140 ms after a rest
	                          at 1000 Hz and Classic gave those far less
	  two_interfaces.one_device a mouse with two HID mouse interfaces (both
	                          nodes name the same USB device) is one pointing
	                          device for input_server and Input preferences
	  two_interfaces.first_moves, .second_moves
	                          and moves with either interface
	  two_interfaces.settings_shared
	                          a speed set for the device applies to its
	                          second interface too

	  latency.1000hz          a quick flick (750 counts in 25 ms) on a USB
	                          mouse reporting at 1000 Hz (ce_vmouse's
	                          emulated USB mouse, polled the way usb_hid
	                          polls): the pointer is at the end within one
	                          60 Hz frame (16.7 ms) of the hand stopping, and
	                          stays there (before: usb_hid polled once per
	                          read, every 8 ms, and the mouse's reports came
	                          out after the hand had stopped; reported as an
	                          INFO line for comparison)
	  latency.8000hz          the same at 8000 Hz (stress case)

	The driver is plugged in "queued" mode (like PS/2: every report is kept)
	for the backlog and click checks and in "on demand" mode (like usb_hid:
	motion not read yet is summed) for the others. Without a desktop or
	without the driver the test prints an INFO line and reports nothing.
	Output format: see 3rdparty/ce/README.md.
*/


#include <AppServerLink.h>
#include <Application.h>
#include <Input.h>
#include <InterfaceDefs.h>
#include <List.h>
#include <Locker.h>
#include <Autolock.h>
#include <Roster.h>
#include <Screen.h>
#include <ServerProtocol.h>
#include <String.h>
#include <View.h>
#include <Window.h>

#include <InputServerTypes.h>
#include <input_globals.h>
#include <pointer_acceleration.h>
#include <kb_mouse_settings.h>

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <vector>

#include "ce_vmouse.h"


#define SUITE "pointer_rate"

static const char* kControl = "/dev/" CE_VMOUSE_CONTROL_NAME;
static const char* kDeviceName = "Virtual Mouse 1";

static int sFailures = 0;
static int sControl = -1;


static void
report(const char* name, bool pass, const char* format, ...)
{
	char detail[500];
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


//	#pragma mark - recording window


struct Record {
	uint32		what;
	BPoint		where;		// screen coordinates
	int32		buttons;
	bigtime_t	when;		// the event's time
	bigtime_t	received;	// when the window got it
};


static BLocker sRecordLock("pointer_rate records");
static std::vector<Record> sRecords;


class RecorderView : public BView {
public:
	RecorderView(BRect frame)
		:
		BView(frame, "recorder", B_FOLLOW_ALL, B_WILL_DRAW)
	{
		SetViewColor(216, 216, 216);
	}

	virtual void AttachedToWindow()
	{
		SetEventMask(B_POINTER_EVENTS, B_FULL_POINTER_HISTORY);
	}

	virtual void MouseMoved(BPoint where, uint32 transit,
		const BMessage* dragMessage)
	{
		_Record(B_MOUSE_MOVED, where);
	}

	virtual void MouseDown(BPoint where)
	{
		_Record(B_MOUSE_DOWN, where);
	}

	virtual void MouseUp(BPoint where)
	{
		_Record(B_MOUSE_UP, where);
	}

private:
	void _Record(uint32 what, BPoint where)
	{
		Record record;
		record.what = what;
		record.where = ConvertToScreen(where);
		record.buttons = 0;
		record.when = 0;
		record.received = system_time();
		BMessage* message = Window()->CurrentMessage();
		if (message != NULL) {
			message->FindInt32("buttons", &record.buttons);
			message->FindInt64("when", &record.when);
		}
		BAutolock _(sRecordLock);
		sRecords.push_back(record);
	}
};


static void
clear_records()
{
	BAutolock _(sRecordLock);
	sRecords.clear();
}


static std::vector<Record>
records()
{
	BAutolock _(sRecordLock);
	return sRecords;
}


//	#pragma mark - the virtual mouse


static bool
plug(uint32 nodes, uint32 mode)
{
	ce_vmouse_plug request = { nodes, mode };
	if (ioctl(sControl, CE_VMOUSE_PLUG, &request, sizeof(request)) != 0)
		return false;
	return true;
}


static void
unplug()
{
	ioctl(sControl, CE_VMOUSE_UNPLUG, NULL, 0);
}


static void
send(uint32 node, int32 dx, int32 dy, uint32 buttons, bigtime_t when,
	int32 clicks = 0)
{
	ce_vmouse_report report;
	memset(&report, 0, sizeof(report));
	report.node = node;
	report.movement.xdelta = dx;
	report.movement.ydelta = dy;
	report.movement.buttons = buttons;
	report.movement.clicks = clicks;
	report.movement.timestamp = when;
	write(sControl, &report, sizeof(report));
}


//! The virtual mouse's input_server devices (names), "Virtual" ones only.
static int32
virtual_devices(BString& names)
{
	names = "";
	BList list;
	if (get_input_devices(&list) != B_OK)
		return 0;
	int32 count = 0;
	for (int32 i = 0; i < list.CountItems(); i++) {
		BInputDevice* device = (BInputDevice*)list.ItemAt(i);
		if (device->Type() == B_POINTING_DEVICE
			&& strncmp(device->Name(), "Virtual", 7) == 0) {
			if (count++ > 0)
				names << ", ";
			names << device->Name();
		}
		delete device;
	}
	return count;
}


static bool
wait_for_device(int32 count, BString& names)
{
	for (int i = 0; i < 100; i++) {
		if (virtual_devices(names) == count)
			return true;
		snooze(100000);
	}
	return false;
}


static BPoint
pointer_position()
{
	// the newest position the window has seen
	std::vector<Record> list = records();
	if (list.empty())
		return BPoint(-1, -1);
	return list.back().where;
}


//! Puts the pointer at \a where and waits until the window has seen it.
static bool
place_pointer(BPoint where)
{
	// set_mouse_position() without libgame, which minimum images lack
	BMessage command(IS_SET_MOUSE_POSITION);
	BMessage reply;
	command.AddPoint("where", where);
	_control_input_server_(&command, &reply);
	for (int i = 0; i < 50; i++) {
		snooze(20000);
		if (pointer_position() == where)
			return true;
	}
	return false;
}


//	#pragma mark - busy app_server


struct StallRequest {
	bigtime_t	delay;
	int32		milliseconds;
	bigtime_t	start;
	bigtime_t	end;
	status_t	status;
};


static status_t
stall_thread(void* data)
{
	StallRequest* request = (StallRequest*)data;
	snooze(request->delay);
	request->start = system_time();
	BPrivate::AppServerLink link;
	link.StartMessage(AS_TEST_STALL_INPUT);
	link.Attach<int32>(request->milliseconds);
	status_t status = B_ERROR;
	if (link.FlushWithReply(status) != B_OK)
		status = B_ERROR;
	request->end = system_time();
	request->status = status;
	return B_OK;
}


static thread_id
start_stall(StallRequest& request, bigtime_t delay, int32 milliseconds)
{
	request.delay = delay;
	request.milliseconds = milliseconds;
	request.start = request.end = 0;
	request.status = B_ERROR;
	thread_id thread = spawn_thread(stall_thread, "stall app_server",
		B_NORMAL_PRIORITY, &request);
	resume_thread(thread);
	return thread;
}


static void
wait_stall(thread_id thread)
{
	status_t result;
	wait_for_thread(thread, &result);
}


//	#pragma mark - tests


static void
test_backlog(BRect screen)
{
	BPoint start(screen.left + 60, screen.top + 300);
	int32 reports = 800;
	if (screen.Width() < reports + 120)
		reports = (int32)screen.Width() - 120;
	if (!place_pointer(start)) {
		report("backlog.final_position", false, "the pointer did not go to "
			"(%.0f,%.0f), it is at (%.0f,%.0f)", start.x, start.y,
			pointer_position().x, pointer_position().y);
		return;
	}
	clear_records();

	StallRequest stall;
	thread_id stallThread = start_stall(stall, 200000, 600);

	bigtime_t begin = system_time() + 1000;
	for (int32 i = 0; i < reports; i++) {
		bigtime_t when = begin + i * 1000;
		snooze_until(when, B_SYSTEM_TIMEBASE);
		send(0, 1, 0, 0, when);
	}
	bigtime_t handStopped = system_time();
	wait_stall(stallThread);

	BPoint expected(start.x + reports, start.y);
	bigtime_t reached = 0;
	for (int i = 0; i < 300 && reached == 0; i++) {
		std::vector<Record> list = records();
		for (size_t r = 0; r < list.size(); r++) {
			if (list[r].what == B_MOUSE_MOVED && list[r].where == expected) {
				reached = list[r].received;
				break;
			}
		}
		if (reached == 0)
			snooze(10000);
	}
	snooze(100000);
	std::vector<Record> list = records();
	BPoint final = list.empty() ? BPoint(-1, -1) : list.back().where;

	report("backlog.final_position", final == expected && stall.status == B_OK,
		"%" B_PRId32 " reports at 1000 Hz, app_server busy %.0f ms (%s): "
		"ended at (%.0f,%.0f), want (%.0f,%.0f)", reports,
		(stall.end - stall.start) / 1000.0, strerror(stall.status), final.x,
		final.y, expected.x, expected.y);

	bigtime_t settled = handStopped > stall.end ? handStopped : stall.end;
	report("backlog.caught_up", reached != 0 && reached - settled <= 60000,
		reached != 0 ? "reached the end %.1f ms after the hand stopped and "
			"app_server was free again" : "never reached the end (%.1f)",
		(reached - settled) / 1000.0);

	int32 stale = 0;
	int32 afterStall = 0;
	for (size_t r = 0; r < list.size(); r++) {
		if (list[r].what != B_MOUSE_MOVED || list[r].received < stall.end)
			continue;
		afterStall++;
		if (list[r].when != 0 && list[r].when < stall.end - 30000)
			stale++;
	}
	report("backlog.no_replay", stall.status == B_OK && stale <= 2,
		"%" B_PRId32 " of the %" B_PRId32 " moves received after the busy "
		"time were older than its end", stale, afterStall);
}


struct Step {
	int32	dx;
	int32	dy;
	uint32	buttons;
	int32	clicks;
};


static void
test_clicks(BRect screen)
{
	BPoint start(screen.left + 200, screen.top + 300);
	if (!place_pointer(start)) {
		report("clicks.order", false, "the pointer did not go to the start");
		return;
	}
	clear_records();

	static const Step kSteps[] = {
		{ 10, 0, 0, 0 },					// (210,300)
		{ 0, 0, B_PRIMARY_MOUSE_BUTTON, 1 },	// down (210,300)
		{ 5, 5, B_PRIMARY_MOUSE_BUTTON, 1 },	// drag to (215,295)
		{ 0, 0, 0, 0 },						// up (215,295)
		{ 20, 0, 0, 0 },					// (235,295)
		{ 0, 0, B_PRIMARY_MOUSE_BUTTON, 1 },	// down
		{ 0, 0, 0, 0 },						// up
		{ 0, 0, B_PRIMARY_MOUSE_BUTTON, 2 },	// down (double click)
		{ 0, 0, 0, 0 },						// up
		{ -30, 0, 0, 0 },					// (205,295)
		{ 0, 0, B_SECONDARY_MOUSE_BUTTON, 1 },	// down
		{ 0, 0, 0, 0 },						// up
		{ 3, -3, 0, 0 }						// (208,298)
	};
	struct Expected {
		uint32	what;
		float	x;
		float	y;
		int32	buttons;
	};
	const Expected kExpected[] = {
		{ B_MOUSE_DOWN, 210, 300, B_PRIMARY_MOUSE_BUTTON },
		{ B_MOUSE_UP, 215, 295, 0 },
		{ B_MOUSE_DOWN, 235, 295, B_PRIMARY_MOUSE_BUTTON },
		{ B_MOUSE_UP, 235, 295, 0 },
		{ B_MOUSE_DOWN, 235, 295, B_PRIMARY_MOUSE_BUTTON },
		{ B_MOUSE_UP, 235, 295, 0 },
		{ B_MOUSE_DOWN, 205, 295, B_SECONDARY_MOUSE_BUTTON },
		{ B_MOUSE_UP, 205, 295, 0 }
	};
	const size_t kExpectedCount = sizeof(kExpected) / sizeof(kExpected[0]);

	StallRequest stall;
	thread_id stallThread = start_stall(stall, 0, 500);
	snooze(60000);
	bigtime_t when = system_time();
	for (size_t i = 0; i < sizeof(kSteps) / sizeof(kSteps[0]); i++) {
		when += 1000;
		snooze_until(when, B_SYSTEM_TIMEBASE);
		send(0, kSteps[i].dx, kSteps[i].dy, kSteps[i].buttons, when,
			kSteps[i].clicks);
	}
	wait_stall(stallThread);
	snooze(400000);

	std::vector<Record> list = records();
	std::vector<Record> buttons;
	for (size_t r = 0; r < list.size(); r++) {
		if (list[r].what != B_MOUSE_MOVED)
			buttons.push_back(list[r]);
	}

	bool order = buttons.size() == kExpectedCount;
	bool positions = order;
	BString got;
	for (size_t i = 0; i < buttons.size(); i++) {
		const Record& r = buttons[i];
		got << (r.what == B_MOUSE_DOWN ? "down" : "up") << "("
			<< (int32)r.where.x - (int32)start.x + 200 << ","
			<< (int32)r.where.y - (int32)start.y + 300 << ")";
		if (i + 1 < buttons.size())
			got << " ";
		if (i >= kExpectedCount)
			continue;
		if (r.what != kExpected[i].what
			|| (r.what == B_MOUSE_DOWN && r.buttons != kExpected[i].buttons))
			order = false;
		BPoint want(start.x + kExpected[i].x - 200,
			start.y + kExpected[i].y - 300);
		if (r.where != want)
			positions = false;
	}
	report("clicks.order", order && stall.status == B_OK,
		"%d of %d button events, app_server busy %.0f ms: %s",
		(int)buttons.size(), (int)kExpectedCount,
		(stall.end - stall.start) / 1000.0, got.String());
	report("clicks.positions", positions,
		"positions relative to (200,300) in the list above; want down(210,"
		"300) up(215,295) down(235,295) up down up down(205,295) up");
}


static void
test_long_stall(BRect screen)
{
	BPoint start(screen.left + 200, screen.top + 400);
	if (!place_pointer(start)) {
		report("long_stall.clicks", false, "the pointer did not go to the "
			"start");
		return;
	}
	clear_records();

	StallRequest stall;
	thread_id stallThread = start_stall(stall, 0, 2000);
	snooze(30000);
	bigtime_t when = system_time();
	// 1.9 s of motion, a count every 2 ms, out and back
	for (int32 i = 0; i < 950; i++) {
		when += 2000;
		snooze_until(when, B_SYSTEM_TIMEBASE);
		send(0, i < 475 ? 1 : -1, 0, 0, when);
	}
	send(0, 7, 0, 0, when + 500);
	send(0, 0, 0, B_PRIMARY_MOUSE_BUTTON, when + 1000, 1);
	send(0, 0, 0, 0, when + 1500);
	wait_stall(stallThread);
	snooze(500000);

	BPoint want(start.x + 7, start.y);
	std::vector<Record> list = records();
	int32 downs = 0, ups = 0;
	bool placed = true;
	for (size_t r = 0; r < list.size(); r++) {
		if (list[r].what == B_MOUSE_DOWN)
			downs++;
		else if (list[r].what == B_MOUSE_UP)
			ups++;
		else
			continue;
		if (list[r].where != want)
			placed = false;
	}
	BPoint final = list.empty() ? BPoint(-1, -1) : list.back().where;
	report("long_stall.clicks", downs == 1 && ups == 1 && placed
			&& final == want && stall.status == B_OK,
		"app_server busy %.0f ms, %d moves sent by the watcher meanwhile: "
		"%" B_PRId32 " press(es), %" B_PRId32 " release(s), ended at "
		"(%.0f,%.0f), want one of each at (%.0f,%.0f)",
		(stall.end - stall.start) / 1000.0, 950 / 4, downs, ups,
		final.x, final.y, want.x, want.y);
}


//! The pixels a move of \a counts at \a rate reports a second moves.
static float
accelerated_move(BRect screen, int32 rate, double countsPerSecond,
	bigtime_t duration)
{
	BPoint start(screen.left + 40, screen.top + 500);
	place_pointer(start);
	snooze(1000000);
		// a second of rest, as before a real move
	bigtime_t interval = 1000000 / rate;
	double owed = 0;
	bigtime_t when = system_time();
	for (bigtime_t t = interval; t <= duration; t += interval) {
		when += interval;
		snooze_until(when, B_SYSTEM_TIMEBASE);
		owed += countsPerSecond * interval / 1000000.0;
		int32 counts = (int32)owed;
		owed -= counts;
		if (counts != 0)
			send(0, counts, 0, 0, when);
			// the time of the motion, as a mouse would stamp it: under
			// emulation this loop can fall behind, and stamping reports
			// when they are read would turn that into a slower hand
	}
	snooze(300000);
	return pointer_position().x - start.x;
}


static void
test_acceleration_rate(BRect screen)
{
	set_mouse_speed(kDeviceName, kDefaultMouseSpeed);
	set_mouse_acceleration(kDeviceName, kDefaultAccelerationFactor);
	snooze(100000);

	// slow enough to stay on the screen: 900 counts a second for 250 ms
	float at1000 = accelerated_move(screen, 1000, 900, 250000);
	float at125 = accelerated_move(screen, 125, 900, 250000);
	float low = at1000 < at125 ? at1000 : at125;
	float high = at1000 < at125 ? at125 : at1000;
	report("accel_rate.adaptive", low > 0 && high - low <= high * 0.05,
		"default speed and acceleration, Adaptive, 900 counts/s for 250 ms "
		"after a second of rest: %.0f pixels at 1000 Hz, %.0f at 125 Hz",
		at1000, at125);

	set_mouse_speed(kDeviceName, 65536);
	set_mouse_acceleration(kDeviceName, 0);
	snooze(100000);
}


static void
test_two_interfaces(BRect screen)
{
	unplug();
	BString names;
	wait_for_device(0, names);
	if (!plug(2, CE_VMOUSE_ON_DEMAND)) {
		report("two_interfaces.one_device", false, "plugging failed: %s",
			strerror(errno));
		return;
	}
	snooze(2000000);
		// both nodes found; a second device would show up meanwhile
	int32 count = virtual_devices(names);
	report("two_interfaces.one_device", count == 1 && names == kDeviceName,
		"%" B_PRId32 " pointing device(s): %s", count, names.String());

	BPoint start(screen.left + 300, screen.top + 300);
	place_pointer(start);
	send(0, 25, 0, 0, 0);
	snooze(300000);
	BPoint after = pointer_position();
	report("two_interfaces.first_moves", after == start + BPoint(25, 0),
		"25 counts on interface 0: (%.0f,%.0f), want (%.0f,%.0f)", after.x,
		after.y, start.x + 25, start.y);

	place_pointer(start);
	send(1, 40, 0, 0, 0);
	snooze(300000);
	after = pointer_position();
	report("two_interfaces.second_moves", after == start + BPoint(40, 0),
		"40 counts on interface 2: (%.0f,%.0f), want (%.0f,%.0f)", after.x,
		after.y, start.x + 40, start.y);

	set_mouse_speed(kDeviceName, 131072);
	snooze(200000);
	place_pointer(start);
	send(1, 20, 0, 0, 0);
	snooze(300000);
	after = pointer_position();
	report("two_interfaces.settings_shared", after == start + BPoint(40, 0),
		"speed 2 set for \"%s\", 20 counts on interface 2: (%.0f,%.0f), want "
		"(%.0f,%.0f)", kDeviceName, after.x, after.y, start.x + 40, start.y);
	set_mouse_speed(kDeviceName, 65536);
	snooze(100000);
}


/*!	Pointer latency (2026-11-14): a fast flick on a USB mouse reporting at
	\a rate Hz, through ce_vmouse's emulated USB mouse (CE_VMOUSE_USB: the
	firmware sends one report per poll) and the whole real input path.
	Returns how long after the hand stopped the window saw the pointer at
	the end of the move (-1: never), and the position it ended at.
*/
static bigtime_t
flick_latency(BRect screen, int32 rate, uint32 pollMode, BPoint& final,
	BPoint& expected)
{
	unplug();
	BString names;
	wait_for_device(0, names);
	ce_vmouse_plug request = { 1, CE_VMOUSE_USB,
		(uint32)(1000000 / rate), pollMode };
	int plugged = ioctl(sControl, CE_VMOUSE_PLUG, &request, sizeof(request));
	int plugError = plugged != 0 ? errno : 0;
	if (plugged != 0 || !wait_for_device(1, names)) {
		printf("CE-INFO pointer_rate: USB mouse at %" B_PRId32 " Hz: %s "
			"(%s), devices: %s\n", rate, plugged != 0 ? "plug failed"
				: "never appeared", strerror(plugError), names.String());
		fflush(stdout);
		final = expected = BPoint(-1, -1);
		return -1;
	}
	send(0, 1, 0, 0, 0);
	snooze(200000);

	// 30 counts a millisecond for 25 ms (30 000 counts a second, a quick
	// flick with a 1600 DPI mouse), sent as the sensor makes them: every
	// millisecond, that millisecond's reports.
	const int32 kMilliseconds = 25;
	const double kCountsPerMillisecond = 30;
	int32 distance = (int32)(kCountsPerMillisecond * kMilliseconds);
	if (screen.Width() < distance + 200)
		distance = (int32)screen.Width() - 200;
	BPoint start(screen.left + 100, screen.top + 200);
	expected = BPoint(start.x + distance, start.y);
	place_pointer(start);
	clear_records();

	int32 perMillisecond = rate / 1000 > 0 ? rate / 1000 : 1;
	double owed = 0;
	int32 sent = 0;
	bigtime_t when = system_time() + 1000;
	for (int32 ms = 0; ms < kMilliseconds && sent < distance; ms++) {
		snooze_until(when, B_SYSTEM_TIMEBASE);
		ce_vmouse_report reports[16];
		int32 count = 0;
		for (int32 i = 0; i < perMillisecond && count < 16; i++) {
			owed += kCountsPerMillisecond / perMillisecond;
			int32 counts = (int32)owed;
			if (sent + counts > distance)
				counts = distance - sent;
			owed -= counts;
			sent += counts;
			memset(&reports[count], 0, sizeof(reports[count]));
			reports[count].movement.xdelta = counts;
			reports[count].movement.timestamp
				= when + i * (1000 / perMillisecond);
			count++;
		}
		write(sControl, reports, count * sizeof(ce_vmouse_report));
		when += 1000;
	}
	if (sent < distance)
		send(0, distance - sent, 0, 0, when);
	bigtime_t handStopped = system_time();

	bigtime_t reached = -1;
	for (int i = 0; i < 400 && reached < 0; i++) {
		std::vector<Record> list = records();
		for (size_t r = 0; r < list.size(); r++) {
			if (list[r].what == B_MOUSE_MOVED && list[r].where == expected) {
				reached = list[r].received - handStopped;
				if (reached < 0)
					reached = 0;
				break;
			}
		}
		if (reached < 0)
			snooze(5000);
	}
	snooze(200000);
	final = pointer_position();
	return reached;
}


static void
test_latency(BRect screen)
{
	// A refresh at 60 Hz: the pointer has to be where the hand stopped
	// within one frame. The add-on reads every 8 ms, so up to 8 ms of it
	// are its cadence.
	const bigtime_t kFrame = 16700;
	BPoint before = pointer_position();
	struct { int32 rate; uint32 mode; const char* name; } runs[] = {
		{ 1000, CE_VMOUSE_POLL_ON_DEMAND, NULL },
		{ 1000, CE_VMOUSE_POLL_CONTINUOUS, "latency.1000hz" },
		{ 8000, CE_VMOUSE_POLL_CONTINUOUS, "latency.8000hz" }
	};
	for (size_t i = 0; i < sizeof(runs) / sizeof(runs[0]); i++) {
		BPoint final, expected;
		bigtime_t latency = flick_latency(screen, runs[i].rate, runs[i].mode,
			final, expected);
		if (runs[i].name == NULL) {
			// usb_hid's polling before the fix, for comparison
			printf("CE-INFO pointer_rate: polled once per read (usb_hid "
				"before 2026-11-14), %" B_PRId32 " Hz: at the end %.1f ms "
				"after the hand stopped, ended at (%.0f,%.0f), want "
				"(%.0f,%.0f)\n", runs[i].rate,
				latency >= 0 ? latency / 1000.0 : -1.0, final.x, final.y,
				expected.x, expected.y);
			fflush(stdout);
			continue;
		}
		report(runs[i].name, latency >= 0 && latency <= kFrame
			&& final == expected,
			"a flick of %.0f counts in 25 ms at %" B_PRId32 " Hz: the pointer "
			"was at the end %.1f ms after the hand stopped (one frame: %.1f "
			"ms) and ended at (%.0f,%.0f), want (%.0f,%.0f)",
			expected.x - (screen.left + 100), runs[i].rate,
			latency >= 0 ? latency / 1000.0 : -1.0, kFrame / 1000.0, final.x,
			final.y, expected.x, expected.y);
	}
	// leave the pointer where the earlier checks did, for the suites after
	// this one (pointer_test's first tablet move)
	if (before.x >= 0)
		place_pointer(before);
}


int
main()
{
	printf("CE-SUITE %s START\n", SUITE);
	fflush(stdout);

	if (access("/boot/system/servers/app_server", X_OK) != 0
		|| access("/boot/system/servers/input_server", X_OK) != 0) {
		printf("CE-INFO pointer_rate: no desktop, skipped\n");
		printf("CE-SUITE %s DONE failures=0\n", SUITE);
		return 0;
	}
	sControl = open(kControl, O_RDWR);
	if (sControl < 0) {
		printf("CE-INFO pointer_rate: no %s (%s), skipped\n", kControl,
			strerror(errno));
		printf("CE-SUITE %s DONE failures=0\n", SUITE);
		return 0;
	}

	bool desktop = false;
	for (int i = 0; i < 300 && !desktop; i++) {
		desktop = be_roster->IsRunning("application/x-vnd.Be-input_server");
		if (!desktop) {
			if (i > 0 && i % 30 == 0) {
				printf("CE-INFO pointer_rate: waiting for input_server "
					"(%d s)\n", i);
				fflush(stdout);
			}
			snooze(1000000);
		}
	}
	if (!desktop) {
		report("device", false, "input_server not running after 300 s");
		printf("CE-SUITE %s DONE failures=%d\n", SUITE, sFailures);
		return 1;
	}

	status_t status = B_ERROR;
	BApplication* app = new BApplication(
		"application/x-vnd.Renga-ce-pointer-rate", &status);
	if (status != B_OK) {
		report("device", false, "no BApplication: %s", strerror(status));
		printf("CE-SUITE %s DONE failures=%d\n", SUITE, sFailures);
		return 1;
	}

	BRect screen = BScreen().Frame();
	BWindow* window = new BWindow(screen, "pointer_rate", B_NO_BORDER_WINDOW_LOOK,
		B_FLOATING_ALL_WINDOW_FEEL, B_NOT_MOVABLE | B_NOT_RESIZABLE
			| B_AVOID_FOCUS);
	window->AddChild(new RecorderView(window->Bounds()));
	window->Show();
	snooze(500000);

	BString names;
	bool found = plug(1, CE_VMOUSE_QUEUED) && wait_for_device(1, names);
	report("device", found && names == kDeviceName,
		"virtual mouse plugged in: %s", names.String());
	if (found) {
		set_mouse_speed(kDeviceName, 65536);
		set_mouse_acceleration(kDeviceName, 0);
		snooze(200000);
		// one report so the watcher applies the settings, then back
		send(0, 1, 0, 0, 0);
		snooze(100000);

		test_backlog(screen);
		test_clicks(screen);

		unplug();
		wait_for_device(0, names);
		if (plug(1, CE_VMOUSE_ON_DEMAND) && wait_for_device(1, names)) {
			send(0, 1, 0, 0, 0);
			snooze(100000);
			test_long_stall(screen);
			test_acceleration_rate(screen);
		} else
			report("long_stall.clicks", false, "plugging again failed");

		test_two_interfaces(screen);
		test_latency(screen);
		unplug();
	}

	window->Lock();
	window->Quit();
	delete app;
	close(sControl);

	printf("CE-SUITE %s DONE failures=%d\n", SUITE, sFailures);
	fflush(stdout);
	return sFailures != 0;
}
