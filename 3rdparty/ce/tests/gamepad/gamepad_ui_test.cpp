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
#include <sys/stat.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <AppFileInfo.h>
#include <Application.h>
#include <Autolock.h>
#include <Bitmap.h>
#include <File.h>
#include <FindDirectory.h>
#include <IconUtils.h>
#include <Messenger.h>
#include <OS.h>
#include <Path.h>
#include <Resources.h>
#include <Roster.h>
#include <Screen.h>
#include <Window.h>
#include <String.h>

#include <Gamepad.h>
#include <GamepadMapping.h>

#include <linux/uinput.h>

#include <string>


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


static off_t
syslog_size()
{
	struct stat st;
	return stat("/boot/system/var/log/syslog", &st) == 0 ? st.st_size : 0;
}


static std::string
syslog_since(off_t offset)
{
	std::string text;
	FILE* file = fopen("/boot/system/var/log/syslog", "r");
	if (file == NULL)
		return text;
	fseeko(file, offset, SEEK_SET);
	char buffer[4096];
	size_t bytes;
	while ((bytes = fread(buffer, 1, sizeof(buffer), file)) > 0)
		text.append(buffer, bytes);
	fclose(file);
	return text;
}


static int
create_pad(const char* name = "Overlay test pad")
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
	strlcpy(setup.name, name, sizeof(setup.name));
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

	// connect notification: a second pad while the agent watches
	off_t before = syslog_size();
	int second = create_pad("Notice test pad");
	snooze(2500000);
	bool noticed = syslog_since(before).find(
		"notification \"Notice test pad connected\"") != std::string::npos;
	if (second >= 0)
		close(second);
	snooze(1500000);
	run("--set connect_notice off");
	before = syslog_size();
	second = create_pad("Quiet test pad");
	snooze(2500000);
	bool quiet = syslog_since(before).find("Quiet test pad connected")
		== std::string::npos;
	if (second >= 0)
		close(second);
	run("--set connect_notice on");
	report("connect_notice", second >= 0 && noticed && quiet,
		"notification on plug-in: %s; with the switch off: %s",
		noticed ? "shown" : "missing", quiet ? "none" : "shown anyway");

	if (team >= 0)
		kill_team(team);
	if (pad >= 0)
		close(pad);
}


// #pragma mark - mapping wizard and tuning


static int
create_full_pad(uint16 product)
{
	int fd = open("/dev/input/uinput", O_RDWR | O_NONBLOCK);
	if (fd < 0)
		return -1;
	ioctl(fd, UI_SET_EVBIT, (void*)(addr_t)EV_KEY);
	ioctl(fd, UI_SET_EVBIT, (void*)(addr_t)EV_ABS);
	static const int kKeys[] = { BTN_A, BTN_B, BTN_X, BTN_Y, BTN_TL, BTN_TR,
		BTN_SELECT, BTN_START, BTN_MODE, BTN_THUMBL, BTN_THUMBR };
	for (size_t i = 0; i < sizeof(kKeys) / sizeof(kKeys[0]); i++)
		ioctl(fd, UI_SET_KEYBIT, (void*)(addr_t)kKeys[i]);
	struct { int code, min, max; } kAxes[] = {
		{ ABS_X, -32768, 32767 }, { ABS_Y, -32768, 32767 },
		{ ABS_Z, 0, 255 }, { ABS_RX, -32768, 32767 },
		{ ABS_RY, -32768, 32767 }, { ABS_RZ, 0, 255 },
		{ ABS_HAT0X, -1, 1 }, { ABS_HAT0Y, -1, 1 } };
	for (size_t i = 0; i < sizeof(kAxes) / sizeof(kAxes[0]); i++) {
		ioctl(fd, UI_SET_ABSBIT, (void*)(addr_t)kAxes[i].code);
		struct uinput_abs_setup abs;
		memset(&abs, 0, sizeof(abs));
		abs.code = kAxes[i].code;
		abs.absinfo.minimum = kAxes[i].min;
		abs.absinfo.maximum = kAxes[i].max;
		ioctl(fd, UI_ABS_SETUP, &abs);
	}
	struct uinput_setup setup;
	memset(&setup, 0, sizeof(setup));
	setup.id.bustype = BUS_USB;
	setup.id.vendor = 0x1209;
	setup.id.product = product;
		// an ID no database line knows
	strlcpy(setup.name, "Mapping test pad", sizeof(setup.name));
	if (ioctl(fd, UI_DEV_SETUP, &setup) < 0 || ioctl(fd, UI_DEV_CREATE) < 0) {
		close(fd);
		return -1;
	}
	return fd;
}


static BString
find_pad_path(const char* name)
{
	for (int attempt = 0; attempt < 40; attempt++) {
		int32 count = BGamepad::CountGamepads();
		for (int32 i = 0; i < count; i++) {
			BString path;
			BGamepad pad;
			if (BGamepad::GetGamepadPath(i, &path) == B_OK
				&& pad.Open(path.String()) == B_OK
				&& strcmp(pad.Name(), name) == 0)
				return path;
		}
		snooze(100000);
	}
	return "";
}


static void
press(int fd, uint16 type, uint16 code, int32 value, int32 rest)
{
	emit(fd, type, code, value);
	emit(fd, EV_SYN, SYN_REPORT, 0);
	snooze(60000);
	emit(fd, type, code, rest);
	emit(fd, EV_SYN, SYN_REPORT, 0);
	snooze(60000);
}


static void
test_learn()
{
	int fd = create_full_pad(0x0b03);
	BString path = find_pad_path("Mapping test pad");
	if (fd < 0 || path.IsEmpty()) {
		report("learn_mapping", false, "virtual pad %s, device %s",
			fd >= 0 ? "made" : "missing", path.String());
		if (fd >= 0)
			close(fd);
		return;
	}
	// the answer to each step, in MappingLearner's order
	struct { uint16 type, code; int32 value, rest; } kAnswers[] = {
		{ EV_KEY, BTN_A, 1, 0 }, { EV_KEY, BTN_B, 1, 0 },
		{ EV_KEY, BTN_X, 1, 0 }, { EV_KEY, BTN_Y, 1, 0 },
		{ EV_ABS, ABS_HAT0Y, -1, 0 }, { EV_ABS, ABS_HAT0Y, 1, 0 },
		{ EV_ABS, ABS_HAT0X, -1, 0 }, { EV_ABS, ABS_HAT0X, 1, 0 },
		{ EV_KEY, BTN_TL, 1, 0 }, { EV_KEY, BTN_TR, 1, 0 },
		{ EV_ABS, ABS_Z, 255, 0 }, { EV_ABS, ABS_RZ, 255, 0 },
		{ EV_KEY, BTN_SELECT, 1, 0 }, { EV_KEY, BTN_START, 1, 0 },
		{ EV_KEY, BTN_MODE, 1, 0 }, { EV_KEY, BTN_THUMBL, 1, 0 },
		{ EV_KEY, BTN_THUMBR, 1, 0 },
		{ EV_ABS, ABS_X, 32767, 0 }, { EV_ABS, ABS_Y, 32767, 0 },
		{ EV_ABS, ABS_RX, -32768, 0 },	// a stick wired the other way
		{ EV_ABS, ABS_RY, 32767, 0 } };
	const int kAnswerCount = sizeof(kAnswers) / sizeof(kAnswers[0]);

	BString command;
	command.SetToFormat("%s --learn %s 2>&1", kApp, path.String());
	FILE* pipe = popen(command.String(), "r");
	BString line;
	int steps = 0;
	bool saved = false;
	char buffer[512];
	while (pipe != NULL && fgets(buffer, sizeof(buffer), pipe) != NULL) {
		int step;
		if (sscanf(buffer, "step %d:", &step) == 1) {
			steps++;
			snooze(150000);
			if (step >= 1 && step <= kAnswerCount) {
				press(fd, kAnswers[step - 1].type, kAnswers[step - 1].code,
					kAnswers[step - 1].value, kAnswers[step - 1].rest);
			}
		} else if (strncmp(buffer, "line: ", 6) == 0) {
			line = buffer + 6;
			line.Trim();
		} else if (strncmp(buffer, "saved", 5) == 0)
			saved = true;
	}
	if (pipe != NULL)
		pclose(pipe);

	static const char* kExpected[] = { "a:b0,", "b:b1,", "x:b2,", "y:b3,",
		"dpup:h0.1,", "dpdown:h0.4,", "dpleft:h0.8,", "dpright:h0.2,",
		"leftshoulder:b4,", "rightshoulder:b5,", "lefttrigger:a2,",
		"righttrigger:a5,", "back:b6,", "start:b7,", "guide:b8,",
		"leftstick:b9,", "rightstick:b10,", "leftx:a0,", "lefty:a1,",
		"rightx:a3~,", "righty:a4,", "platform:Linux," };
	BString missing;
	for (size_t i = 0; i < sizeof(kExpected) / sizeof(kExpected[0]); i++) {
		if (line.FindFirst(kExpected[i]) < 0)
			missing << kExpected[i];
	}

	BPrivate::gamepad_reload_mappings();
	BGamepad pad;
	bool used = pad.Open(path.String()) == B_OK && pad.Mapping() != NULL
		&& line == pad.Mapping();
	report("learn_mapping", steps == kAnswerCount && saved
			&& missing.IsEmpty() && used,
		"%d steps, %s; missing [%s]; BGamepad %s the line", steps,
		saved ? "saved" : "not saved", missing.String(),
		used ? "uses" : "does not use");

	// the face button swap and the dead zone with the learned mapping
	BString tune;
	tune.SetToFormat("--tune %s 20 1.0 10 1", path.String());
	run(tune.String());
	pad.Close();
	pad.Open(path.String());
	emit(fd, EV_ABS, ABS_X, 32767 * 15 / 100);
	emit(fd, EV_ABS, ABS_Z, 255 * 5 / 100);
	emit(fd, EV_KEY, BTN_A, 1);
	emit(fd, EV_SYN, SYN_REPORT, 0);
	snooze(150000);
	pad.Update();
	int16 smallX = pad.AxisValue(B_GAMEPAD_AXIS_LEFT_X);
	int16 lightTrigger = pad.AxisValue(B_GAMEPAD_AXIS_LEFT_TRIGGER);
	bool aAsB = pad.IsPressed(B_GAMEPAD_BUTTON_B)
		&& !pad.IsPressed(B_GAMEPAD_BUTTON_A);
	emit(fd, EV_ABS, ABS_X, 32767);
	emit(fd, EV_ABS, ABS_Z, 255);
	emit(fd, EV_SYN, SYN_REPORT, 0);
	snooze(150000);
	pad.Update();
	int16 fullX = pad.AxisValue(B_GAMEPAD_AXIS_LEFT_X);
	int16 fullTrigger = pad.AxisValue(B_GAMEPAD_AXIS_LEFT_TRIGGER);
	report("tuning_applied", smallX == 0 && lightTrigger == 0 && aAsB
			&& fullX > 32000 && fullTrigger > 32000,
		"dead zone 20 %%: stick at 15 %% reads %d, at 100 %% %d; threshold "
		"10 %%: trigger at 5 %% reads %d, full %d; A pressed reads as %s",
		smallX, fullX, lightTrigger, fullTrigger, aAsB ? "B" : "A");

	tune.SetToFormat("--tune %s 0 1.0 0 0", path.String());
	run(tune.String());
	pad.Close();
	pad.Open(path.String());
	emit(fd, EV_ABS, ABS_X, 32767 * 15 / 100);
	emit(fd, EV_SYN, SYN_REPORT, 0);
	snooze(150000);
	pad.Update();
	int16 untuned = pad.AxisValue(B_GAMEPAD_AXIS_LEFT_X);
	bool aIsA = pad.IsPressed(B_GAMEPAD_BUTTON_A);
	BString got;
	got.SetToFormat("--get-tuning %s", path.String());
	BString values = run(got.String());
	report("tuning_off", untuned > 4000 && aIsA && values == "0 1.0 0 0",
		"without tuning: stick at 15 %% reads %d, A reads as %s; settings "
		"[%s]", untuned, aIsA ? "A" : "B", values.String());

	pad.Close();
	close(fd);
}


// #pragma mark - configurator polish (batch 11)


static void
screenshot(const char* name)
{
	printf("CE-HOST screenshot %s\n", name);
	fflush(stdout);
	snooze(1500000);
}


static int
create_named_pad(const char* name, uint16 vendor, uint16 product)
{
	int fd = open("/dev/input/uinput", O_RDWR | O_NONBLOCK);
	if (fd < 0)
		return -1;
	ioctl(fd, UI_SET_EVBIT, (void*)(addr_t)EV_KEY);
	ioctl(fd, UI_SET_EVBIT, (void*)(addr_t)EV_FF);
	ioctl(fd, UI_SET_FFBIT, (void*)(addr_t)FF_RUMBLE);
	static const int kKeys[] = { BTN_A, BTN_B, BTN_X, BTN_Y, BTN_MODE,
		BTN_START, BTN_SELECT };
	for (size_t i = 0; i < sizeof(kKeys) / sizeof(kKeys[0]); i++)
		ioctl(fd, UI_SET_KEYBIT, (void*)(addr_t)kKeys[i]);
	struct uinput_setup setup;
	memset(&setup, 0, sizeof(setup));
	setup.id.bustype = BUS_USB;
	setup.id.vendor = vendor;
	setup.id.product = product;
	setup.ff_effects_max = 4;
	strlcpy(setup.name, name, sizeof(setup.name));
	if (ioctl(fd, UI_DEV_SETUP, &setup) < 0 || ioctl(fd, UI_DEV_CREATE) < 0) {
		close(fd);
		return -1;
	}
	return fd;
}


/*!	A window covering the whole screen, in front: what a full-screen game
	looks like to the quiet mode.
*/
class FullScreenWindow : public BWindow {
public:
	FullScreenWindow()
		:
		BWindow(BScreen().Frame(), "Full-screen test", B_NO_BORDER_WINDOW_LOOK,
			B_NORMAL_WINDOW_FEEL, 0)
	{
	}
};


static void
test_polish()
{
	// layout families: by vendor and name, with the family's face labels
	struct {
		const char*	name;
		uint16		vendor;
		uint16		product;
		const char*	expected;
	} kPads[] = {
		{ "Polish X pad", 0x045e, 0x0b12, "offset A B X Y" },
		{ "Polish P pad", 0x054c, 0x0ce6, "symmetric" },
		{ "Polish N pad", 0x057e, 0x2009, "nintendo B A Y X" },
		{ "Polish SN30 pad", 0x1209, 0x7001, "retro" },
		{ "Polish arcade fight stick", 0x1209, 0x7002, "arcade" },
		{ "Polish plain pad", 0x1209, 0x7003, "generic 1 2 3 4" },
	};
	BString families;
	int correct = 0;
	const int padCount = sizeof(kPads) / sizeof(kPads[0]);
	for (int i = 0; i < padCount; i++) {
		int fd = create_named_pad(kPads[i].name, kPads[i].vendor,
			kPads[i].product);
		BString path = find_pad_path(kPads[i].name);
		BString family = path.IsEmpty() ? BString("(no device)")
			: run((BString("--family ") << path).String());
		if (family.StartsWith(kPads[i].expected))
			correct++;
		families << (i > 0 ? "; " : "") << kPads[i].name << ": " << family;
		if (fd >= 0)
			close(fd);
		snooze(300000);
	}
	report("layout_families", correct == padCount, "%d of %d right: %s",
		correct, padCount, families.String());

	// the picture of every family is in the application (and the agent)
	BFile file(kApp, B_READ_ONLY);
	BResources resources(&file);
	int pictures = 0;
	static const char* kFamilies[] = { "offset", "symmetric", "nintendo",
		"retro", "generic", "arcade" };
	for (int i = 0; i < 6; i++) {
		size_t size = 0;
		BString name("layout:");
		name << kFamilies[i];
		const void* data = resources.LoadResource('VICN', name.String(),
			&size);
		BBitmap bitmap(BRect(0, 0, 47, 47), B_RGBA32);
		if (data != NULL && BIconUtils::GetVectorIcon((const uint8*)data,
				size, &bitmap) == B_OK) {
			pictures++;
		}
	}
	report("layout_pictures", pictures == 6, "%d of 6 family pictures "
		"render", pictures);

	// the agent: notices with the family, quiet in full screen,
	// "Don't show again"
	team_id team = -1;
	if (!be_roster->IsRunning(kAgentSignature)) {
		const char* arguments[] = { kAgentPath, NULL };
		thread_id thread = load_image(1, arguments, (const char**)environ);
		if (thread >= 0)
			resume_thread(thread);
		thread_info info;
		if (thread >= 0 && get_thread_info(thread, &info) == B_OK)
			team = info.team;
		snooze(2000000);
	}

	off_t before = syslog_size();
	int pad = create_named_pad("Polish notice pad", 0x054c, 0x0ce6);
	snooze(2500000);
	std::string log = syslog_since(before);
	report("notice_family", log.find("notification \"Polish notice pad "
			"connected\"") != std::string::npos
			&& log.find("symmetric layout") != std::string::npos,
		"the notice names the layout: %s", log.find("symmetric layout")
			!= std::string::npos ? "yes" : "no");

	// Identify: rumbles and runs through the player numbers, then shows
	// the pad's own number again
	BString path = find_pad_path("Polish notice pad");
	BGamepad gamepad;
	int32 playerBefore = gamepad.Open(path.String()) == B_OK
		? gamepad.Player() : -1;
	bigtime_t start = system_time();
	BString identified = run((BString("--identify ") << path).String());
	bigtime_t took = system_time() - start;
	gamepad.Close();
	int32 playerAfter = gamepad.Open(path.String()) == B_OK
		? gamepad.Player() : -1;
	report("identify", identified.IsEmpty() && took > 800000
			&& playerAfter == (playerBefore > 0 ? playerBefore : 1),
		"output \"%s\", %.1f s, player %d before, %d after",
		identified.String(), took / 1e6, (int)playerBefore,
		(int)playerAfter);
	if (pad >= 0)
		close(pad);
	snooze(1000000);

	// quiet: a full-screen window in front
	FullScreenWindow* window = new FullScreenWindow;
	window->Show();
	snooze(800000);
	before = syslog_size();
	pad = create_named_pad("Polish quiet pad", 0x045e, 0x0b12);
	snooze(2500000);
	log = syslog_since(before);
	bool quiet = log.find("\"Polish quiet pad connected\": quiet (")
		!= std::string::npos;
	if (pad >= 0)
		close(pad);
	snooze(800000);
	run("--set quiet_mode off");
	before = syslog_size();
	pad = create_named_pad("Polish loud pad", 0x045e, 0x0b12);
	snooze(2500000);
	log = syslog_since(before);
	bool loud = log.find("\"Polish loud pad connected\": Player")
		!= std::string::npos || log.find("\"Polish loud pad connected\": No")
		!= std::string::npos;
	if (pad >= 0)
		close(pad);
	run("--set quiet_mode on");
	window->Lock();
	window->Quit();
	report("quiet_mode", quiet && loud, "full screen: %s; with the switch "
		"off: %s", quiet ? "quiet" : "full notice",
		loud ? "full notice" : "quiet anyway");

	// "Don't show again for this controller": the connected window's
	// button, then no notice for that pad
	pad = create_named_pad("Polish silenced pad", 0x057e, 0x2009);
	path = find_pad_path("Polish silenced pad");
	BString command;
	command << kApp << " --connected " << path << " &";
	system(command.String());
	BMessenger connected;
	BString title;
	for (int i = 0; i < 40 && title != "Polish silenced pad"; i++) {
		snooze(250000);
		BList teams;
		be_roster->GetAppList(kAppSignature, &teams);
		for (int32 t = 0; t < teams.CountItems(); t++) {
			BMessenger app(NULL, (team_id)(addr_t)teams.ItemAt(t));
			BMessage request(B_GET_PROPERTY);
			request.AddSpecifier("Title");
			request.AddSpecifier("Window", (int32)0);
			BMessage reply;
			if (app.SendMessage(&request, &reply, 1000000, 1000000) == B_OK
				&& reply.FindString("result", &title) == B_OK
				&& title == "Polish silenced pad") {
				BMessage get(B_GET_PROPERTY);
				get.AddSpecifier("Messenger");
				get.AddSpecifier("Window", (int32)0);
				BMessage answer;
				if (app.SendMessage(&get, &answer, 1000000, 1000000) == B_OK)
					answer.FindMessenger("result", &connected);
				break;
			}
		}
	}
	screenshot("gamepad_connected");
	status_t sent = connected.SendMessage('gsil');
	snooze(1000000);
	BString silenced = run((BString("--silenced ") << path).String());
	if (pad >= 0)
		close(pad);
	snooze(1000000);
	before = syslog_size();
	pad = create_named_pad("Polish silenced pad", 0x057e, 0x2009);
	snooze(2500000);
	log = syslog_since(before);
	bool none = log.find("\"Polish silenced pad connected\": silenced")
		!= std::string::npos;
	path = find_pad_path("Polish silenced pad");
	run((BString("--unsilence ") << path).String());
	if (pad >= 0)
		close(pad);
	report("dont_show_again", title == "Polish silenced pad"
			&& sent == B_OK && silenced == "yes" && none,
		"window \"%s\", button %s, silenced %s, next plug-in %s",
		title.String(), strerror(sent), silenced.String(),
		none ? "without a notice" : "noticed");

	if (team >= 0)
		kill_team(team);
}


// #pragma mark - Mapping tab (batch 11)


/*!	A window that records the keys it gets: the active window, as a game
	would be.
*/
class KeyWindow : public BWindow {
public:
	KeyWindow()
		:
		BWindow(BRect(100, 100, 400, 300), "Mapping test",
			B_TITLED_WINDOW, 0),
		fView(new KeyView)
	{
		AddChild(fView);
		fView->MakeFocus(true);
	}

	BString Keys()
	{
		BAutolock locker(this);
		return fView->keys;
	}

	BPoint Mouse()
	{
		BAutolock locker(this);
		BPoint where;
		uint32 buttons;
		fView->GetMouse(&where, &buttons, false);
		fView->ConvertToScreen(&where);
		return where;
	}

private:
	class KeyView : public BView {
	public:
		KeyView()
			:
			BView(BRect(0, 0, 300, 200), "keys", B_FOLLOW_ALL, B_WILL_DRAW)
		{
		}

		virtual void KeyDown(const char* bytes, int32 count)
		{
			int32 key = Window()->CurrentMessage()->GetInt32("key", 0);
			keys << (keys.IsEmpty() ? "" : " ") << "0x";
			char hex[8];
			snprintf(hex, sizeof(hex), "%02" B_PRIx32, key);
			keys << hex;
		}

		BString keys;
	};

	KeyView* fView;
};


static void
test_mapping()
{
	BString off = run("--mapping get");
	mkdir("/boot/home/config/settings/Gamepads", 0755);
	FILE* file = fopen("/boot/home/config/settings/Gamepads/key_mapping",
		"w");
	if (file != NULL) {
		fprintf(file, "enabled 1\nbutton a 0x5e\nbutton b 0x47\n"
			"dpad arrows\nleft_stick none\nright_stick mouse\nspeed 20\n");
		fclose(file);
	}
	KeyWindow* window = new KeyWindow;
	window->Show();
	window->Lock();
	window->Activate();
	window->Unlock();
	int fd = create_full_pad(0x7010);
	// the add-on reads the settings twice a second and looks for pads
	// every two seconds
	snooze(3000000);

	press(fd, EV_KEY, BTN_A, 1, 0);
	press(fd, EV_ABS, ABS_HAT0X, 1, 0);
	press(fd, EV_KEY, BTN_B, 1, 0);
	snooze(500000);
	BString keys = window->Keys();
	report("mapping_keys", off == "off" && keys == "0x5e 0x63 0x47",
		"off by default: %s; A, D-pad right, B gave [%s] (want 0x5e 0x63 "
		"0x47: Space, Right, Enter)", off.String(), keys.String());

	BPoint before = window->Mouse();
	emit(fd, EV_ABS, ABS_RX, 32767);
	emit(fd, EV_SYN, SYN_REPORT, 0);
	snooze(500000);
	emit(fd, EV_ABS, ABS_RX, 0);
	emit(fd, EV_SYN, SYN_REPORT, 0);
	snooze(200000);
	BPoint after = window->Mouse();
	report("mapping_pointer", after.x > before.x + 20,
		"right stick right for 0.5 s: the pointer went from %.0f,%.0f to "
		"%.0f,%.0f", before.x, before.y, after.x, after.y);

	// the escape: guide and start held for 3 seconds
	emit(fd, EV_KEY, BTN_MODE, 1);
	emit(fd, EV_KEY, BTN_START, 1);
	emit(fd, EV_SYN, SYN_REPORT, 0);
	snooze(3500000);
	emit(fd, EV_KEY, BTN_MODE, 0);
	emit(fd, EV_KEY, BTN_START, 0);
	emit(fd, EV_SYN, SYN_REPORT, 0);
	snooze(800000);
	BString state = run("--mapping get");
	BString keysBefore = window->Keys();
	press(fd, EV_KEY, BTN_A, 1, 0);
	snooze(500000);
	BString keysAfter = window->Keys();
	report("mapping_escape", state == "off" && keysAfter == keysBefore,
		"after guide+start held 3 s: mapping %s, A then gave [%s]",
		state.String(), keysAfter == keysBefore ? "nothing" : "a key");
	screenshot("gamepad_mapping");

	if (fd >= 0)
		close(fd);
	unlink("/boot/home/config/settings/Gamepads/key_mapping");
	window->Lock();
	window->Quit();
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
	test_learn();
	test_polish();
	test_mapping();

	printf("CE-SUITE " SUITE " DONE failures=%d\n", sFailures);
	return sFailures == 0 ? 0 : 1;
}
