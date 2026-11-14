/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 */

/*!	The Gamepads preferences and agent (roadmap F-42) on Renga images: the
	application's own vector icon (Renga icon rule), the window opening,
	each setting's switch taking effect (raw controller access lands in the
	kernel driver settings usb_hid reads), the defaults, and the agent's
	guide-button overlay: a short press of the guide button on a virtual pad
	shows nothing, holding it shows the overlay, and with the setting off
	holding it shows nothing.

	Images without the preferences (minimum) only report that.
*/


#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <AppFileInfo.h>
#include <Application.h>
#include <Bitmap.h>
#include <File.h>
#include <FindDirectory.h>
#include <IconUtils.h>
#include <Messenger.h>
#include <OS.h>
#include <Path.h>
#include <Roster.h>
#include <String.h>

#include <linux/uinput.h>


#define SUITE "gamepad_ui"

static const char* kApp = "/boot/system/preferences/Gamepads";
static const char* kAppSignature = "application/x-vnd.Renga-Gamepads";
static int sFailures = 0;


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


static BString
run(const char* arguments)
{
	BString command;
	command.SetToFormat("%s %s 2>&1", kApp, arguments);
	BString output;
	FILE* pipe = popen(command.String(), "r");
	if (pipe == NULL)
		return output;
	char buffer[256];
	while (fgets(buffer, sizeof(buffer), pipe) != NULL)
		output << buffer;
	pclose(pipe);
	output.Trim();
	return output;
}


static void
test_icon()
{
	BFile file(kApp, B_READ_ONLY);
	BAppFileInfo info(&file);
	uint8* data = NULL;
	size_t size = 0;
	status_t status = info.InitCheck() == B_OK
		? info.GetIcon(&data, &size) : info.InitCheck();
	int32 opaque = 0, green = 0, orange = 0;
	if (status == B_OK) {
		BBitmap bitmap(BRect(0, 0, 31, 31), B_RGBA32);
		status = BIconUtils::GetVectorIcon(data, size, &bitmap);
		const uint8* bits = (const uint8*)bitmap.Bits();
		for (int32 i = 0; status == B_OK && i < (int32)bitmap.BitsLength();
				i += 4) {
			if (bits[i + 3] > 0x80) {
				opaque++;
				// B, G, R, A
				if (bits[i + 1] > bits[i + 2] + 20 && bits[i + 1] > bits[i] + 20)
					green++;
				else if (bits[i + 2] > 180 && bits[i] < 100)
					orange++;
			}
		}
	}
	delete[] data;
	report("icon", status == B_OK && opaque > 200 && green > 100
		&& orange > 3, "vector icon: %s, %" B_PRIuSIZE " bytes; at 32 x 32 %"
		B_PRId32 " opaque, %" B_PRId32 " green, %" B_PRId32 " orange or gold "
		"pixels", strerror(status), size, opaque, green, orange);
}


static void
test_settings()
{
	BPath raw;
	find_directory(B_USER_SETTINGS_DIRECTORY, &raw);
	raw.Append("kernel/drivers/gamepad");

	run("--set raw_access off");
	BString off = run("--get raw_access");
	char content[128] = "";
	int fd = open(raw.Path(), O_RDONLY);
	if (fd >= 0) {
		ssize_t bytes = read(fd, content, sizeof(content) - 1);
		content[bytes > 0 ? bytes : 0] = '\0';
		close(fd);
	}
	bool written = strstr(content, "raw_access false") != NULL;
	run("--set raw_access on");
	bool removed = access(raw.Path(), F_OK) != 0;
	report("raw_access_switch", off == "off" && written && removed
		&& run("--get raw_access") == "on",
		"off: \"%s\", driver settings %s; on: file %s", off.String(),
		written ? "say raw_access false" : "unchanged",
		removed ? "removed" : "still there");

	run("--set low_battery off");
	BString lowOff = run("--get low_battery");
	run("--set low_battery on");
	report("low_battery_switch", lowOff == "off"
		&& run("--get low_battery") == "on", "off then on");

	// defaults: overlay and low battery on, raw access on
	BString overlay = run("--get overlay");
	report("defaults", overlay == "on" && run("--get raw_access") == "on",
		"overlay %s", overlay.String());
}


static void
test_window()
{
	team_id team = -1;
	status_t status = be_roster->Launch(kAppSignature, (BMessage*)NULL,
		&team);
	BMessenger app(NULL, team);
	BString title;
	for (int i = 0; i < 40 && title.IsEmpty(); i++) {
		snooze(250000);
		BMessage request(B_GET_PROPERTY);
		request.AddSpecifier("Title");
		request.AddSpecifier("Window", (int32)0);
		BMessage reply;
		if (app.SendMessage(&request, &reply, 1000000, 1000000) == B_OK)
			reply.FindString("result", &title);
	}
	report("window", status == B_OK && title == "Gamepads",
		"launch %s, window \"%s\"", strerror(status), title.String());
	app.SendMessage(B_QUIT_REQUESTED);
}


static int
create_pad()
{
	int fd = open("/dev/input/uinput", O_RDWR | O_NONBLOCK);
	if (fd < 0)
		return -1;
	ioctl(fd, UI_SET_EVBIT, (void*)(addr_t)EV_KEY);
	static const int kKeys[] = { BTN_A, BTN_B, BTN_X, BTN_Y, BTN_MODE,
		BTN_START, BTN_SELECT };
	for (size_t i = 0; i < sizeof(kKeys) / sizeof(kKeys[0]); i++)
		ioctl(fd, UI_SET_KEYBIT, (void*)(addr_t)kKeys[i]);
	struct uinput_setup setup;
	memset(&setup, 0, sizeof(setup));
	setup.id.bustype = BUS_USB;
	setup.id.vendor = 0x2dc8;
	setup.id.product = 0x310a;
	strlcpy(setup.name, "Overlay test pad", sizeof(setup.name));
	if (ioctl(fd, UI_DEV_SETUP, &setup) < 0 || ioctl(fd, UI_DEV_CREATE) < 0) {
		close(fd);
		return -1;
	}
	return fd;
}


static void
emit(int fd, uint16 type, uint16 code, int32 value)
{
	struct input_event event;
	memset(&event, 0, sizeof(event));
	event.type = type;
	event.code = code;
	event.value = value;
	write(fd, &event, sizeof(event));
}


static void
guide(int fd, bigtime_t hold)
{
	emit(fd, EV_KEY, BTN_MODE, 1);
	emit(fd, EV_SYN, SYN_REPORT, 0);
	snooze(hold);
	emit(fd, EV_KEY, BTN_MODE, 0);
	emit(fd, EV_SYN, SYN_REPORT, 0);
}


// 1 shown, 0 hidden, -1 no answer
static const char* kAgentSignature = "application/x-vnd.Renga-GamepadAgent";
static const char* kAgentPath = "/boot/system/servers/gamepad_agent";


static int
overlay_shown(BMessenger& agent)
{
	BMessage request(B_GET_PROPERTY);
	request.AddSpecifier("Hidden");
	request.AddSpecifier("Window", (int32)0);
	BMessage reply;
	bool hidden;
	if (agent.SendMessage(&request, &reply, 1000000, 1000000) != B_OK
		|| reply.FindBool("result", &hidden) != B_OK)
		return -1;
	return hidden ? 0 : 1;
}


static void
test_overlay()
{
	// the gamepad agent (gamepad_agent, a background server the user's
	// launch script starts, once per session). Harness boots have no user
	// session, so it is started here directly (load_image, no roster
	// round trip, which blocked this test) unless it already runs; it
	// notices the new pad through hot-plug.
	int pad = create_pad();
	status_t status = B_OK;
	team_id team = -1;
	if (!be_roster->IsRunning(kAgentSignature)) {
		const char* arguments[] = { kAgentPath, NULL };
		thread_id thread = load_image(1, arguments, (const char**)environ);
		status = thread >= 0 ? resume_thread(thread) : thread;
		thread_info info;
		if (thread >= 0 && get_thread_info(thread, &info) == B_OK)
			team = info.team;
	}
	BMessenger agent;
	for (int i = 0; i < 40; i++) {
		snooze(250000);
		agent = team >= 0 ? BMessenger(NULL, team)
			: BMessenger(kAgentSignature);
		if (agent.IsValid() && overlay_shown(agent) >= 0)
			break;
	}
	snooze(1500000);

	guide(pad, 100000);
	snooze(700000);
	int afterShort = overlay_shown(agent);
	guide(pad, 900000);
	snooze(300000);
	int afterLong = overlay_shown(agent);
	report("overlay_long_press", pad >= 0 && status == B_OK
		&& afterShort == 0 && afterLong == 1,
		"pad %s, agent %s; short press: %d, long press: %d (1 shown)",
		pad >= 0 ? "made" : "missing", strerror(status), afterShort,
		afterLong);

	// switched off: holding shows nothing
	snooze(3500000);
	run("--set overlay off");
	snooze(500000);
	guide(pad, 900000);
	snooze(300000);
	int whenOff = overlay_shown(agent);
	run("--set overlay on");
	report("overlay_switch", whenOff == 0, "held with the overlay off: %d",
		whenOff);

	if (team >= 0)
		kill_team(team);
	if (pad >= 0)
		close(pad);
}


int
main()
{
	printf("CE-SUITE " SUITE " START\n");
	fflush(stdout);
	BApplication app("application/x-vnd.Renga-gamepad-ui-test");

	if (access(kApp, X_OK) != 0) {
		printf("CE-INFO gamepad_ui: no Gamepads preferences in this image\n");
		printf("CE-SUITE " SUITE " DONE failures=0\n");
		return 0;
	}

	test_icon();
	test_settings();
	test_window();
	test_overlay();

	printf("CE-SUITE " SUITE " DONE failures=%d\n", sFailures);
	return sFailures == 0 ? 0 : 1;
}
