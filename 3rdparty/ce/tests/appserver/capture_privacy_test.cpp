/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 */

/*!	In-guest test: Hide from capture and streamer mode (batch 5,
	streaming; src/servers/app/CapturePrivacy.h, headers/private/shared/
	streamer_mode.h).

	A window of a known colour, then a second one of the same application
	with another title. Every check reads the screen with
	BScreen::ReadBitmap(), which goes through the same ScreenReader::Read()
	as BScreenCapture's frames and recordings.

	  capture_privacy.visible          the window reads as its colour
	  capture_privacy.api              BScreenCapture::SetWindowHidden():
	                                   its area (tab included) reads grey,
	                                   the screen next to it does not change
	  capture_privacy.switch_off       the switch off: shown again
	  capture_privacy.api_off          SetWindowHidden(false): shown
	  capture_privacy.app_rule         "app <signature>": both windows grey
	  capture_privacy.window_rule      "window <signature> <title>": only
	                                   that window grey
	  capture_privacy.streamer_mode    switch off, but streamer mode on with
	                                   "keep marked windows hidden": grey
	  capture_privacy.streamer_off     streamer mode off: shown
	  capture_privacy.notifications_held
	                                   streamer mode with "hold
	                                   notifications": a notification shows
	                                   no window; after streamer mode ends,
	                                   it is shown (when notification_server
	                                   runs)
	  capture_privacy.cli              ScreenRecording's command line
	                                   (renga-* images)

	The settings files are put back as they were.
*/


#include <Application.h>
#include <Bitmap.h>
#include <Notification.h>
#include <Roster.h>
#include <Screen.h>
#include <View.h>
#include <Window.h>

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <string>

#include <ScreenCapture.h>
#include <WindowInfo.h>
#include <capture_privacy.h>
#include <streamer_mode.h>


#define SUITE "capture_privacy"

static const char* kSignature = "application/x-vnd.Renga-capture-privacy-test";
static const rgb_color kGreen = { 40, 160, 90, 255 };
static const rgb_color kBlue = { 0, 0, 200, 255 };
static const uint32 kGrey = 0x303030;
static int sFailures = 0;


static void
report(const char* name, bool pass, const char* format, ...)
{
	char detail[400];
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


static uint32
read_pixel(BPoint where)
{
	BRect frame = BScreen().Frame();
	BBitmap bitmap(frame, B_RGBA32);
	memset(bitmap.Bits(), 0, bitmap.BitsLength());
	BScreen().ReadBitmap(&bitmap, false);
	return *(uint32*)((uint8*)bitmap.Bits()
		+ (int32)where.y * bitmap.BytesPerRow() + (int32)where.x * 4)
		& 0xffffff;
}


static uint32
rgb(rgb_color color)
{
	return (color.red << 16) | (color.green << 8) | color.blue;
}


static std::string
read_file(const char* path)
{
	std::string text;
	FILE* file = fopen(path, "r");
	if (file == NULL)
		return text;
	char buffer[1024];
	size_t length;
	while ((length = fread(buffer, 1, sizeof(buffer), file)) > 0)
		text.append(buffer, length);
	fclose(file);
	return text;
}


static void
restore_file(const char* path, const std::string& text, bool existed)
{
	if (!existed) {
		unlink(path);
		return;
	}
	FILE* file = fopen(path, "w");
	if (file == NULL)
		return;
	fwrite(text.data(), 1, text.size(), file);
	fclose(file);
}


static void
settle()
{
	// app_server checks the files at most once a second
	snooze(1300000);
}


/*!	The height of \a team's tallest shown window: notification_server's
	window grows with every notification it shows.
*/
static int32
team_window_height(team_id team)
{
	int32 count = 0;
	int32* tokens = get_token_list(team, &count);
	int32 height = 0;
	for (int32 i = 0; tokens != NULL && i < count; i++) {
		client_window_info* info = get_window_info(tokens[i]);
		if (info != NULL && info->show_hide_level <= 0) {
			int32 windowHeight = info->window_bottom - info->window_top + 1;
			if (windowHeight > height)
				height = windowHeight;
		}
		free(info);
	}
	free(tokens);
	return height;
}


static void
notification_checks()
{
	app_info info;
	if (be_roster->GetAppInfo("application/x-vnd.Haiku-notification_server",
			&info) != B_OK) {
		printf("CE-INFO %s: no notification_server running, notification "
			"checks skipped\n", SUITE);
		return;
	}
	int32 before = team_window_height(info.team);

	StreamerMode::Settings streamer;
	StreamerMode::defaults(streamer);
	streamer.enabled = true;
	streamer.pauseNotifications = true;
	StreamerMode::save(streamer);

	BNotification notification(B_INFORMATION_NOTIFICATION);
	notification.SetTitle("Secret title");
	notification.SetContent("Secret content");
	notification.Send(8000000);
	snooze(1500000);
	int32 held = team_window_height(info.team);

	streamer.enabled = false;
	StreamerMode::save(streamer);
	int32 after = held;
	for (int i = 0; i < 24 && after <= before; i++) {
		snooze(250000);
		after = team_window_height(info.team);
	}
	report("notifications_held", held <= before && after > before,
		"notification window height %" B_PRId32 " before, %" B_PRId32
		" while held, %" B_PRId32 " after streamer mode", before, held,
		after);
}


static int
run(const char* command, std::string& output)
{
	output.clear();
	FILE* pipe = popen(command, "r");
	if (pipe == NULL)
		return -1;
	char buffer[1024];
	size_t length;
	while ((length = fread(buffer, 1, sizeof(buffer), pipe)) > 0)
		output.append(buffer, length);
	return pclose(pipe);
}


int
main()
{
	BApplication app(kSignature);
	printf("CE-SUITE %s START\n", SUITE);
	if (!BScreen().IsValid()) {
		printf("CE-INFO %s: no screen, nothing to test\n", SUITE);
		printf("CE-SUITE %s DONE failures=0\n", SUITE);
		return 0;
	}

	char privacyPath[B_PATH_NAME_LENGTH];
	char streamerPath[B_PATH_NAME_LENGTH];
	CapturePrivacyRules::path(privacyPath, sizeof(privacyPath));
	StreamerMode::path(streamerPath, sizeof(streamerPath));
	bool privacyExisted = access(privacyPath, F_OK) == 0;
	bool streamerExisted = access(streamerPath, F_OK) == 0;
	std::string privacyText = read_file(privacyPath);
	std::string streamerText = read_file(streamerPath);
	unlink(privacyPath);
	unlink(streamerPath);

	// on the left, clear of the installation medium's first-start prompt
	// in the middle of the screen
	BRect firstFrame(20, 60, 219, 209);
	BRect secondFrame(20, 260, 219, 409);
	BWindow* first = new BWindow(firstFrame, "Private notes",
		B_TITLED_WINDOW, B_NOT_RESIZABLE | B_NOT_ZOOMABLE);
	first->AddChild(new ColorView(first->Bounds(), kGreen));
	first->Show();
	BWindow* second = new BWindow(secondFrame, "Public window",
		B_TITLED_WINDOW, B_NOT_RESIZABLE | B_NOT_ZOOMABLE);
	second->AddChild(new ColorView(second->Bounds(), kBlue));
	second->Show();
	snooze(1500000);
	first->Activate();
	snooze(500000);

	BPoint inFirst(firstFrame.left + 100, firstFrame.top + 70);
	BPoint inSecond(secondFrame.left + 100, secondFrame.top + 70);
	BPoint firstTab(firstFrame.left + 20, firstFrame.top - 10);
	BPoint outside(firstFrame.left + 100, firstFrame.bottom + 40);
	uint32 outsideBefore = read_pixel(outside);
	uint32 tabBefore = read_pixel(firstTab);

	uint32 value = read_pixel(inFirst);
	report("visible", value == rgb(kGreen), "0x%06x (want 0x%06x)",
		(unsigned)value, (unsigned)rgb(kGreen));

	status_t status = BScreenCapture::SetWindowHidden(first, true);
	settle();
	value = read_pixel(inFirst);
	uint32 tab = read_pixel(firstTab);
	uint32 outsideNow = read_pixel(outside);
	report("api", status == B_OK && value == kGrey && tab == kGrey
		&& tabBefore != kGrey && outsideNow == outsideBefore,
		"status %s, content 0x%06x, tab 0x%06x (was 0x%06x), next to it "
		"0x%06x (was 0x%06x)", strerror(status), (unsigned)value,
		(unsigned)tab, (unsigned)tabBefore, (unsigned)outsideNow,
		(unsigned)outsideBefore);

	CapturePrivacyRules::Rules rules;
	rules.enabled = false;
	CapturePrivacyRules::save(rules);
	settle();
	value = read_pixel(inFirst);
	report("switch_off", value == rgb(kGreen), "0x%06x", (unsigned)value);

	rules.enabled = true;
	CapturePrivacyRules::save(rules);
	BScreenCapture::SetWindowHidden(first, false);
	settle();
	value = read_pixel(inFirst);
	report("api_off", value == rgb(kGreen), "0x%06x", (unsigned)value);

	CapturePrivacyRules::set(rules.apps, kSignature, true);
	CapturePrivacyRules::save(rules);
	settle();
	uint32 a = read_pixel(inFirst);
	uint32 b = read_pixel(inSecond);
	report("app_rule", a == kGrey && b == kGrey, "0x%06x, 0x%06x",
		(unsigned)a, (unsigned)b);

	rules.apps.clear();
	CapturePrivacyRules::set(rules.windows,
		std::string(kSignature) + "\tPrivate notes", true);
	CapturePrivacyRules::save(rules);
	settle();
	a = read_pixel(inFirst);
	b = read_pixel(inSecond);
	report("window_rule", a == kGrey && b == rgb(kBlue), "0x%06x, 0x%06x",
		(unsigned)a, (unsigned)b);

	rules.enabled = false;
	CapturePrivacyRules::save(rules);
	StreamerMode::Settings streamer;
	StreamerMode::defaults(streamer);
	streamer.enabled = true;
	StreamerMode::save(streamer);
	settle();
	a = read_pixel(inFirst);
	report("streamer_mode", a == kGrey, "0x%06x", (unsigned)a);

	streamer.enabled = false;
	StreamerMode::save(streamer);
	settle();
	a = read_pixel(inFirst);
	report("streamer_off", a == rgb(kGreen), "0x%06x", (unsigned)a);

	notification_checks();

	if (access("/boot/system/preferences/ScreenRecording", X_OK) == 0) {
		std::string output;
		int s1 = run("/boot/system/preferences/ScreenRecording "
			"--streamer-mode on 2>&1", output);
		int s2 = run("/boot/system/preferences/ScreenRecording --hide-window "
			"application/x-vnd.Test 'A title' on 2>&1", output);
		int s3 = run("/boot/system/preferences/ScreenRecording --privacy 2>&1",
			output);
		bool listed = output.find("streamer_mode on") != std::string::npos
			&& output.find("window application/x-vnd.Test\tA title")
				!= std::string::npos;
		report("cli", s1 == 0 && s2 == 0 && s3 == 0 && listed, "%s",
			output.c_str());
	}

	first->Lock();
	first->Quit();
	second->Lock();
	second->Quit();
	restore_file(privacyPath, privacyText, privacyExisted);
	restore_file(streamerPath, streamerText, streamerExisted);

	printf("CE-SUITE %s DONE failures=%d\n", SUITE, sFailures);
	return sFailures == 0 ? 0 : 1;
}
