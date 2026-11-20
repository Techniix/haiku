/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 */

/*!	Control Panel (Renga, roadmap F-28; batch 5): one window for the
	system's settings, with categories in a sidebar and a search across
	every page (the maintainer's design, concept 1 "Sidebar & search").

	Pages are of two kinds:
	- built in: the page's own view lives here (Displays and Audio, built
	  from their preferences' sources, which still build the preferences
	  themselves; Streaming and privacy; Boot and recovery);
	- preference pages: a short description, the settings the page has
	  (the same entries the search finds) and a button that opens the
	  preference, which keeps working on its own. They become built-in
	  pages one by one as each preference's window is turned into a view.

	The search matches a setting's name, its page and its synonyms
	("monitor" finds Displays, "dnd" finds streamer mode); choosing a
	result opens its page.

	  ControlPanel                     the window
	  ControlPanel --page <id>         the window, on that page
	  ControlPanel --search <text>     the matching settings, one per line
	                                   ("<page id>\t<page>\t<setting>")
	  ControlPanel --pages             the pages ("<id>\t<category>\t<title>")

	The window also answers kMsgSearch ("text" -> "result" strings) and
	kMsgShowPage ("id"), for the tests.
*/


#include <Application.h>
#include <Box.h>
#include <Button.h>
#include <CardLayout.h>
#include <Catalog.h>
#include <CheckBox.h>
#include <ColumnListView.h>
#include <ColumnTypes.h>
#include <FindDirectory.h>
#include <GroupView.h>
#include <LayoutBuilder.h>
#include <ListView.h>
#include <OutlineListView.h>
#include <Path.h>
#include <Roster.h>
#include <ScrollBar.h>
#include <ScrollView.h>
#include <SeparatorView.h>
#include <StringItem.h>
#include <StringView.h>
#include <TextControl.h>
#include <TextView.h>
#include <Window.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <sys/stat.h>

#include <algorithm>
#include <map>
#include <string>
#include <vector>

#include <AppServerLink.h>
#include <FrontCopyInfo.h>
#include <ServerProtocol.h>
#include <boot_state_file.h>
#include <capture_privacy.h>
#include <streamer_mode.h>

#include "AudioSettingsView.h"
#include "DisplaysView.h"
#include "LinuxKPIManage.h"
#include "RenguinView.h"
#include "WindowsPage.h"


#undef B_TRANSLATION_CONTEXT
#define B_TRANSLATION_CONTEXT "ControlPanel"


static const char* kSignature = "application/x-vnd.Renga-ControlPanel";

static const uint32 kMsgSearchChanged = 'srch';
static const uint32 kMsgPageSelected = 'psel';
static const uint32 kMsgResultInvoked = 'rinv';
static const uint32 kMsgOpenPreference = 'opnp';
static const uint32 kMsgStreamerOption = 'stro';
static const uint32 kMsgRecoveryOption = 'rcvo';
// from other programs (tests)
static const uint32 kMsgSearch = 'cpSr';
static const uint32 kMsgShowPage = 'cpPg';


// #pragma mark - pages and settings


struct PageInfo {
	const char*	id;
	const char*	category;
	const char*	title;
	const char*	description;
	const char*	program;
		// the preference that holds the page's settings, NULL if none
	const char*	synonyms;
};


static const PageInfo kPages[] = {
	{ "displays", "Hardware", "Displays",
		"Arrangement, resolution, refresh rate, scale, rotation and the "
		"graphics card.", "/boot/system/preferences/Screen",
		"screen monitor resolution refresh display gpu graphics" },
	{ "audio", "Hardware", "Audio",
		"Outputs, inputs, per-application volume and routing.",
		"/boot/system/preferences/Audio",
		"sound speakers headphones microphone volume" },
	{ "gamepads", "Hardware", "Gamepads",
		"Controllers, their mapping, tests and player numbers.",
		"/boot/system/preferences/Gamepads",
		"controller joystick xbox dualsense 8bitdo switch pad" },
	{ "devices", "Hardware", "Devices and drivers",
		"Every device and its driver: native, or Linux through LinuxKPI.",
		"/boot/system/apps/LinuxKPIManager",
		"linuxkpi driver wifi wireless hardware device" },
	{ "input", "Hardware", "Mouse, keyboard and touchpad",
		"Pointer speed and acceleration, keyboard repeat.",
		"/boot/system/preferences/Input",
		"mouse pointer touchpad keyboard repeat acceleration" },
	{ "keymap", "Hardware", "Keymap", "The keyboard layout.",
		"/boot/system/preferences/Keymap", "layout language keys" },
	{ "network", "Network", "Network",
		"Wi-Fi, Ethernet, addresses and services.",
		"/boot/system/preferences/Network",
		"wifi wireless ethernet ip dns internet" },
	{ "streaming", "Privacy", "Streaming and privacy",
		"Streamer mode and windows hidden from captures.",
		"/boot/system/preferences/ScreenRecording",
		"stream obs twitch capture record privacy dnd hide" },
	{ "screenrecording", "Privacy", "Screen recording",
		"Which applications may record the screen.",
		"/boot/system/preferences/ScreenRecording",
		"capture permission recording" },
	{ "notifications", "System", "Notifications",
		"Which applications may show notifications, and for how long.",
		"/boot/system/preferences/Notifications", "alerts popup dnd" },
	{ "boot", "System", "Boot and recovery",
		"Restore points, automatic recovery, the boot menu.",
		"/boot/system/preferences/BootMenu",
		"restore rollback recovery startup boot menu windows dual" },
	{ "software", "System", "Software and repositories",
		"Package repositories and updates.",
		"/boot/system/preferences/Repositories",
		"packages update repository haikudepot" },
	{ "renguin", "System", "Linux programs (Renguin)",
		"The Linux compatibility layer, the Linux root and its packages, "
		"diagnostics.", "/boot/system/apps/RenguinManager",
		"linux renguin chrome alpine apk trace" },
	{ "doctor", "System", "Hardware report",
		"Renga Doctor: one report with the hardware, drivers, logs and "
		"crash reports, for a bug report.",
		"/boot/system/apps/RengaDoctor",
		"doctor bug report diagnostics logs crash support" },
	{ "welcome", "System", "Welcome",
		"The first-login tour: network, drivers, software, Linux programs, "
		"gamepads and the shortcut key.", "/boot/system/apps/Welcome",
		"welcome tour setup first start getting started" },
	{ "time", "System", "Date and time", "Time zone, clock and time sync.",
		"/boot/system/preferences/Time", "clock timezone ntp date" },
	{ "locale", "System", "Language and region",
		"Language, formats and translations.",
		"/boot/system/preferences/Locale",
		"language region country format translation" },
	{ "memory", "System", "Virtual memory", "The swap file.",
		"/boot/system/preferences/VirtualMemory", "swap memory" },
	{ "appearance", "Appearance", "Appearance",
		"Colours, fonts, the window style and scroll bars.",
		"/boot/system/preferences/Appearance",
		"theme colour color font decorator" },
	{ "workspaces", "Appearance", "Windows and workspaces",
		"Snap layouts, the window menu, workspaces and their names.",
		NULL,
		"snap tile tiling zone layout grid workspace virtual desktop "
		"overview arrange lumina window menu" },
	{ "backgrounds", "Appearance", "Backgrounds",
		"The desktop background of each workspace.",
		"/boot/system/preferences/Backgrounds", "wallpaper desktop image" },
	{ "deskbar", "Appearance", "Deskbar", "Deskbar's menu and options.",
		"/boot/system/preferences/Deskbar", "taskbar panel menu" },
	{ "tracker", "Appearance", "Tracker", "File manager options.",
		"/boot/system/preferences/Tracker", "files folders desktop" },
	{ "shortcuts", "Appearance", "Shortcuts", "Keyboard shortcuts.",
		"/boot/system/preferences/Shortcuts", "hotkeys keys" },
	{ "screensaver", "Appearance", "Screen saver",
		"The screen saver and its timeout.",
		"/boot/system/preferences/ScreenSaver", "blank lock" },
	{ "sounds", "Appearance", "Sounds", "The system's sound events.",
		"/boot/system/preferences/Sounds", "beep events alerts" },
	{ "printers", "Hardware", "Printers", "Printers and their drivers.",
		"/boot/system/preferences/Printers", "print" },
	{ "bluetooth", "Hardware", "Bluetooth", "Bluetooth devices.",
		"/boot/system/preferences/Bluetooth", "wireless headset pairing" },
};


struct SettingInfo {
	const char*	page;
	const char*	name;
	const char*	synonyms;
};


static const SettingInfo kSettings[] = {
	{ "displays", "Display arrangement", "multi monitor position" },
	{ "displays", "Resolution and refresh rate", "hz mode" },
	{ "displays", "Scale", "hidpi zoom" },
	{ "displays", "Rotation", "portrait" },
	{ "displays", "Variable refresh rate (VRR)", "freesync gsync" },
	{ "displays", "HDR", "high dynamic range" },
	{ "displays", "Wayland programs", "wayland compositor" },
	{ "displays", "Workspaces", "virtual desktops" },
	{ "displays", "Graphics card information", "gpu nvidia quasar driver" },
	{ "audio", "Output device", "speakers headphones hdmi" },
	{ "audio", "Input device", "microphone mic" },
	{ "audio", "Application volumes", "per app mixer" },
	{ "audio", "Also play on", "several outputs mirror" },
	{ "audio", "Low-latency audio", "latency games emulators" },
	{ "gamepads", "Gamepad mapping", "buttons remap" },
	{ "gamepads", "Test a gamepad", "tester" },
	{ "gamepads", "Player numbers", "player lights" },
	{ "devices", "Every device and its driver", "hardware pci list" },
	{ "devices", "Linux drivers", "linuxkpi" },
	{ "devices", "Wi-Fi statistics", "throughput speed" },
	{ "input", "Pointer speed", "mouse acceleration" },
	{ "input", "Key repeat", "keyboard delay" },
	{ "network", "Wi-Fi networks", "ssid wireless" },
	{ "streaming", "Streamer mode", "streaming obs" },
	{ "streaming", "Hide notification content while streaming",
		"notifications private" },
	{ "streaming", "Hold notifications while streaming",
		"do not disturb dnd pause" },
	{ "streaming", "Hide windows from captures",
		"screenshot recording private password" },
	{ "screenrecording", "Recording permissions", "allow deny" },
	{ "screenrecording", "Recording indicator", "red dot" },
	{ "notifications", "Notification duration", "timeout" },
	{ "notifications", "Allowed applications", "block mute" },
	{ "workspaces", "Snap layouts", "snap zones tile drag grid" },
	{ "workspaces", "Zone editor", "custom layout zones" },
	{ "workspaces", "Zone shortcuts", "keyboard snap keys" },
	{ "workspaces", "Window menu on the tab", "move to workspace right click" },
	{ "workspaces", "Per-application window rules", "rule zone open" },
	{ "workspaces", "Saved window arrangements", "restore positions" },
	{ "workspaces", "Workspace names", "label desktop" },
	{ "workspaces", "Workspace switcher in the Deskbar", "plus add" },
	{ "workspaces", "Workspaces overview", "expose all" },
	{ "workspaces", "Saved workspace sets", "profiles" },
	{ "boot", "System restore points", "rollback undo update" },
	{ "boot", "Automatic recovery after failed starts",
		"boot counting safe mode" },
	{ "boot", "Restart into another system", "windows dual boot" },
	{ "boot", "Boot menu timeout", "countdown" },
	{ "displays", "Write only changed pixels to the screen", "vesa "
		"framebuffer speed fast copy" },
	{ "boot", "Native resolution at start", "edid monitor resolution vesa "
		"framebuffer gop" },
	{ "boot", "Default system", "windows dual boot" },
	{ "software", "Repositories", "sources renga haikuports" },
	{ "doctor", "Collect a hardware report", "renga doctor bug" },
	{ "welcome", "Open the welcome again", "tour first login" },
	{ "time", "Time zone", "clock" },
	{ "time", "Synchronize the clock", "ntp internet time" },
	{ "locale", "Language", "translation" },
	{ "appearance", "Colours", "colors theme" },
	{ "appearance", "Fonts", "text size" },
	{ "appearance", "Shortcut key (Ctrl or Alt)", "command control" },
	{ "backgrounds", "Desktop background", "wallpaper" },
	{ "shortcuts", "Keyboard shortcuts", "hotkeys" },
	{ "memory", "Swap file", "virtual memory" },
	{ "renguin", "Linux programs", "linux compatibility" },
	{ "renguin", "Linux root", "alpine download setup" },
	{ "renguin", "Update Linux packages", "apk upgrade" },
	{ "renguin", "Trace a Linux program", "diagnostics strace debug" },
	{ "bluetooth", "Bluetooth devices", "pairing" },
};


static const PageInfo*
find_page(const char* id)
{
	for (size_t i = 0; i < sizeof(kPages) / sizeof(kPages[0]); i++) {
		if (strcmp(kPages[i].id, id) == 0)
			return &kPages[i];
	}
	return NULL;
}


static bool
contains_word(const char* haystack, const std::string& needle)
{
	return haystack != NULL && strcasestr(haystack, needle.c_str()) != NULL;
}


/*!	The settings matching \a text: every word of it must be found in the
	setting's name, its synonyms, its page's title or the page's synonyms.
	A page matching by itself is listed too, as its first setting.
*/
static std::vector<std::pair<const PageInfo*, std::string> >
search(const char* text)
{
	std::vector<std::string> words;
	std::string current;
	for (const char* p = text; ; p++) {
		if (*p == ' ' || *p == '\0') {
			if (!current.empty())
				words.push_back(current);
			current.clear();
			if (*p == '\0')
				break;
		} else
			current += *p;
	}

	std::vector<std::pair<const PageInfo*, std::string> > results;
	if (words.empty())
		return results;
	for (size_t i = 0; i < sizeof(kPages) / sizeof(kPages[0]); i++) {
		const PageInfo& page = kPages[i];
		bool pageMatches = true;
		for (size_t w = 0; w < words.size(); w++) {
			if (!contains_word(page.title, words[w])
				&& !contains_word(page.synonyms, words[w]))
				pageMatches = false;
		}
		bool listed = false;
		for (size_t s = 0; s < sizeof(kSettings) / sizeof(kSettings[0]);
				s++) {
			const SettingInfo& setting = kSettings[s];
			if (strcmp(setting.page, page.id) != 0)
				continue;
			bool matches = true;
			for (size_t w = 0; w < words.size() && matches; w++) {
				matches = contains_word(setting.name, words[w])
					|| contains_word(setting.synonyms, words[w])
					|| contains_word(page.title, words[w])
					|| contains_word(page.synonyms, words[w]);
			}
			if (matches) {
				results.push_back(std::make_pair(&page,
					std::string(setting.name)));
				listed = true;
			}
		}
		if (pageMatches && !listed)
			results.push_back(std::make_pair(&page, std::string(page.title)));
	}
	return results;
}


static int
run(const char* command, std::string* output = NULL)
{
	FILE* pipe = popen(command, "r");
	if (pipe == NULL)
		return -1;
	char buffer[1024];
	size_t length;
	while ((length = fread(buffer, 1, sizeof(buffer), pipe)) > 0) {
		if (output != NULL)
			output->append(buffer, length);
	}
	return pclose(pipe);
}


// #pragma mark - built-in pages


//!	The heading every page starts with.
static BView*
page_heading(const PageInfo* page)
{
	BStringView* title = new BStringView("pageTitle", page->title);
	BFont font(be_bold_font);
	font.SetSize(font.Size() * 1.4f);
	title->SetFont(&font);
	BStringView* description = new BStringView("pageDescription",
		page->description);
	BGroupView* group = new BGroupView(B_VERTICAL, 2);
	BLayoutBuilder::Group<>(group)
		.Add(title)
		.Add(description);
	return group;
}


/*!	Streaming and privacy: streamer mode and its parts, the Hide from
	capture switch; the list of hidden windows stays in Screen recording.
*/
class StreamingPage : public BGroupView {
public:
	StreamingPage()
		:
		BGroupView("streaming", B_VERTICAL)
	{
		fStreamer = new BCheckBox("streamerMode", B_TRANSLATE(
			"Streamer mode"), new BMessage(kMsgStreamerOption));
		fContent = new BCheckBox("streamerContent", B_TRANSLATE(
			"Notifications show only the application's name"),
			new BMessage(kMsgStreamerOption));
		fPause = new BCheckBox("streamerPause", B_TRANSLATE(
			"Hold notifications until streamer mode ends"),
			new BMessage(kMsgStreamerOption));
		fWindows = new BCheckBox("streamerWindows", B_TRANSLATE(
			"Keep windows hidden from captures while streaming"),
			new BMessage(kMsgStreamerOption));
		fPrivacy = new BCheckBox("hideFromCapture", B_TRANSLATE(
			"Hide marked windows from captures, recordings and screenshots"),
			new BMessage(kMsgStreamerOption));
		BMessage* open = new BMessage(kMsgOpenPreference);
		open->AddString("id", "streaming");
		BLayoutBuilder::Group<>(this)
			.SetInsets(B_USE_WINDOW_SPACING)
			.Add(page_heading(find_page("streaming")))
			.Add(new BSeparatorView(B_HORIZONTAL))
			.Add(fStreamer)
			.AddGroup(B_VERTICAL, 0)
				.SetInsets(B_USE_DEFAULT_SPACING, 0, 0, 0)
				.Add(fContent)
				.Add(fPause)
				.Add(fWindows)
			.End()
			.Add(fPrivacy)
			.AddGroup(B_HORIZONTAL)
				.Add(new BButton("chooseWindows", B_TRANSLATE(
					"Choose windows" B_UTF8_ELLIPSIS), open))
				.AddGlue()
			.End()
			.AddGlue();
	}

	virtual void AttachedToWindow()
	{
		BGroupView::AttachedToWindow();
		BView* views[] = { fStreamer, fContent, fPause, fWindows, fPrivacy };
		for (size_t i = 0; i < 5; i++)
			((BControl*)views[i])->SetTarget(this);
		_Show();
	}

	virtual void MessageReceived(BMessage* message)
	{
		if (message->what != kMsgStreamerOption) {
			BGroupView::MessageReceived(message);
			return;
		}
		StreamerMode::Settings streamer;
		streamer.enabled = fStreamer->Value() != 0;
		streamer.hideNotificationContent = fContent->Value() != 0;
		streamer.pauseNotifications = fPause->Value() != 0;
		streamer.hideMarkedWindows = fWindows->Value() != 0;
		StreamerMode::save(streamer);
		CapturePrivacyRules::Rules rules;
		CapturePrivacyRules::load(rules);
		if (rules.enabled != (fPrivacy->Value() != 0)) {
			rules.enabled = fPrivacy->Value() != 0;
			CapturePrivacyRules::save(rules);
		}
		_Show();
	}

private:
	void _Show()
	{
		StreamerMode::Settings streamer;
		StreamerMode::load(streamer);
		fStreamer->SetValue(streamer.enabled);
		fContent->SetValue(streamer.hideNotificationContent);
		fPause->SetValue(streamer.pauseNotifications);
		fWindows->SetValue(streamer.hideMarkedWindows);
		fContent->SetEnabled(streamer.enabled);
		fPause->SetEnabled(streamer.enabled);
		fWindows->SetEnabled(streamer.enabled);
		CapturePrivacyRules::Rules rules;
		CapturePrivacyRules::load(rules);
		fPrivacy->SetValue(rules.enabled);
	}

	BCheckBox*	fStreamer;
	BCheckBox*	fContent;
	BCheckBox*	fPause;
	BCheckBox*	fWindows;
	BCheckBox*	fPrivacy;
};


/*!	Renga (batch 7): "Write only changed pixels to the screen", how
	app_server copies its back buffer to the frame buffer
	(<FrontCopyInfo.h>): on is "changed" (the default), off is "stream".
	Saved in ~/config/settings/Renga/framebuffer and sent to app_server.
*/
class FrontCopySwitch : public BCheckBox {
public:
	FrontCopySwitch()
		:
		BCheckBox("frontCopy", B_TRANSLATE("Write only changed pixels to the "
			"screen (faster with the default display driver)"),
			new BMessage('fcSw'))
	{
	}

	virtual void AttachedToWindow()
	{
		BCheckBox::AttachedToWindow();
		SetTarget(this);
		BPrivate::AppServerLink link;
		link.StartMessage(AS_GET_FRONT_COPY_STATS);
		link.Attach<bool>(false);
		int32 code;
		front_copy_stats stats;
		if (link.FlushWithReply(code) == B_OK && code == B_OK
			&& link.Read(&stats, sizeof(stats)) == B_OK)
			SetValue(stats.mode == B_FRONT_COPY_CHANGED);
	}

	virtual void MessageReceived(BMessage* message)
	{
		if (message->what != 'fcSw') {
			BCheckBox::MessageReceived(message);
			return;
		}
		int32 mode = Value() != 0 ? B_FRONT_COPY_CHANGED : B_FRONT_COPY_STREAM;
		char path[B_PATH_NAME_LENGTH];
		if (find_directory(B_USER_SETTINGS_DIRECTORY, -1, true, path,
				sizeof(path)) == B_OK) {
			strlcat(path, "/Renga", sizeof(path));
			mkdir(path, 0755);
			strlcat(path, "/framebuffer", sizeof(path));
			FILE* out = fopen(path, "w");
			if (out != NULL) {
				fprintf(out, "copy %s\n", mode == B_FRONT_COPY_CHANGED
					? "changed" : "stream");
				fclose(out);
			}
		}
		BPrivate::AppServerLink link;
		link.StartMessage(AS_SET_FRONT_COPY_MODE);
		link.Attach<int32>(mode);
		int32 code;
		link.FlushWithReply(code);
	}
};


/*!	Renga (batch 7): the boot loader's "native_mode" in the vesa driver
	settings (~/config/settings/kernel/drivers/vesa), which the BIOS and
	EFI loaders read: on (the default, no line) the monitor's native mode
	from EDID or the firmware; off the old choice. Other lines (a "mode"
	the vesa driver saved) are kept.
*/
static bool
vesa_settings_path(char* buffer, size_t size)
{
	char settings[B_PATH_NAME_LENGTH];
	if (find_directory(B_USER_SETTINGS_DIRECTORY, -1, false, settings,
			sizeof(settings)) != B_OK)
		return false;
	snprintf(buffer, size, "%s/kernel/drivers/vesa", settings);
	return true;
}


static bool
native_mode_enabled()
{
	char file[B_PATH_NAME_LENGTH];
	if (!vesa_settings_path(file, sizeof(file)))
		return true;
	FILE* in = fopen(file, "r");
	if (in == NULL)
		return true;
	bool enabled = true;
	char line[256];
	while (fgets(line, sizeof(line), in) != NULL) {
		if (strncmp(line, "native_mode", 11) == 0)
			enabled = strstr(line, "false") == NULL
				&& strstr(line, "off") == NULL;
	}
	fclose(in);
	return enabled;
}


static void
set_native_mode(bool enabled)
{
	char file[B_PATH_NAME_LENGTH];
	if (!vesa_settings_path(file, sizeof(file)))
		return;
	std::string kept;
	FILE* in = fopen(file, "r");
	if (in != NULL) {
		char line[256];
		while (fgets(line, sizeof(line), in) != NULL) {
			if (strncmp(line, "native_mode", 11) != 0)
				kept += line;
		}
		fclose(in);
	}
	if (!enabled)
		kept += "native_mode false\n";
	std::string temporary = std::string(file) + ".new";
	FILE* out = fopen(temporary.c_str(), "w");
	if (out == NULL)
		return;
	fputs(kept.c_str(), out);
	fclose(out);
	rename(temporary.c_str(), file);
}


/*!	Boot and recovery: the restore points by date, the automatic recovery
	switch (through the Boot menu preferences' command line, which owns the
	kernel settings file), and the boot menu preferences for the rest.
*/
class BootPage : public BGroupView {
public:
	BootPage()
		:
		BGroupView("boot", B_VERTICAL)
	{
		// batch 6: the switch for every boot loader write
		fBootState = new BCheckBox("bootState", B_TRANSLATE(
			"Count starts and keep restore choices in the boot loader"),
			new BMessage(kMsgRecoveryOption));
		fRecovery = new BCheckBox("autoRecovery", B_TRANSLATE(
			"After two failed starts, open the boot menu with the last "
			"working system selected"), new BMessage(kMsgRecoveryOption));
		fNativeMode = new BCheckBox("nativeMode", B_TRANSLATE(
			"Start in the monitor's native resolution (off: the largest "
			"mode the firmware offers)"), new BMessage(kMsgRecoveryOption));
		fPoints = new BListView("restorePoints");
		BScrollView* scroll = new BScrollView("restorePointsScroll", fPoints,
			0, false, true);
		scroll->SetExplicitMinSize(BSize(300, 90));
		BMessage* open = new BMessage(kMsgOpenPreference);
		open->AddString("id", "boot");
		BLayoutBuilder::Group<>(this)
			.SetInsets(B_USE_WINDOW_SPACING)
			.Add(page_heading(find_page("boot")))
			.Add(new BSeparatorView(B_HORIZONTAL))
			.Add(fBootState)
			.AddGroup(B_VERTICAL)
				.SetInsets(B_USE_DEFAULT_SPACING, 0, 0, 0)
				.Add(fRecovery)
			.End()
			.Add(fNativeMode)
			.Add(new BStringView("pointsLabel", B_TRANSLATE(
				"Restore points (saved before every update; start one from "
				"the boot menu):")))
			.Add(scroll)
			.AddGroup(B_HORIZONTAL)
				.Add(new BButton("bootMenu", B_TRANSLATE(
					"Boot menu and Restart into" B_UTF8_ELLIPSIS), open))
				.AddGlue()
			.End()
			.AddGlue();
	}

	virtual void AttachedToWindow()
	{
		BGroupView::AttachedToWindow();
		fRecovery->SetTarget(this);
		fBootState->SetTarget(this);
		fNativeMode->SetTarget(this);
		_Show();
	}

	virtual void MessageReceived(BMessage* message)
	{
		if (message->what != kMsgRecoveryOption) {
			BGroupView::MessageReceived(message);
			return;
		}
		BControl* source = NULL;
		message->FindPointer("source", (void**)&source);
		if (source == fNativeMode) {
			set_native_mode(fNativeMode->Value() != 0);
		} else if (source == fBootState) {
			run(fBootState->Value() != 0
				? "/boot/system/preferences/BootMenu --set-boot-state on"
				: "/boot/system/preferences/BootMenu --set-boot-state off");
		} else {
			run(fRecovery->Value() != 0
				? "/boot/system/preferences/BootMenu --set-auto-recovery on"
				: "/boot/system/preferences/BootMenu --set-auto-recovery off");
		}
		_Show();
	}

private:
	void _Show()
	{
		std::string shown;
		run("/boot/system/preferences/BootMenu --show 2>/dev/null", &shown);
		fRecovery->SetValue(shown.find("auto_recovery off")
			== std::string::npos);
		bool bootState = shown.find("boot_state off") == std::string::npos;
		fBootState->SetValue(bootState);
		fRecovery->SetEnabled(bootState);
		fNativeMode->SetValue(native_mode_enabled());

		while (BListItem* item = fPoints->RemoveItem((int32)0))
			delete item;
		static char names[64][BootState::kNameLength];
		int count = BootState::list_restore_points(names, 64);
		for (int i = 0; i < count; i++) {
			char label[160];
			BootState::restore_point_label(names[i], label, sizeof(label));
			fPoints->AddItem(new BStringItem(label));
		}
		if (count == 0) {
			fPoints->AddItem(new BStringItem(B_TRANSLATE(
				"No restore points yet")));
		}
	}

	BCheckBox*	fRecovery;
	BCheckBox*	fBootState;
	BCheckBox*	fNativeMode;
	BListView*	fPoints;
};


/*!	Devices and drivers (roadmap F-26/F-28, batch 5): every PCI device with
	its class and IDs, and which driver it has: a Linux driver through
	LinuxKPI (with its state: working, firmware missing, not installed,
	disabled at boot) or a native one. Haiku's device manager does not say
	which native driver bound a device, so those show "Native or none".
	Switching a Linux driver on or off, its settings and logs are in
	LinuxKPI Manager.
*/
using namespace LKPI;


class DriversPage : public BGroupView {
public:
	DriversPage()
		:
		BGroupView("devices", B_VERTICAL)
	{
		fList = new BColumnListView("deviceList", 0, B_FANCY_BORDER, true);
		fList->AddColumn(new BStringColumn(B_TRANSLATE("Device"), 220, 60,
			500, B_TRUNCATE_END), 0);
		fList->AddColumn(new BStringColumn(B_TRANSLATE("ID"), 80, 40, 120,
			B_TRUNCATE_END), 1);
		fList->AddColumn(new BStringColumn(B_TRANSLATE("Driver"), 130, 60,
			300, B_TRUNCATE_END), 2);
		fList->AddColumn(new BStringColumn(B_TRANSLATE("State"), 170, 60,
			400, B_TRUNCATE_END), 3);
		fList->SetExplicitMinSize(BSize(400, 220));
		fSummary = new BStringView("devicesSummary", "");
		BMessage* open = new BMessage(kMsgOpenPreference);
		open->AddString("id", "devices");
		BLayoutBuilder::Group<>(this)
			.SetInsets(B_USE_WINDOW_SPACING)
			.Add(page_heading(find_page("devices")))
			.Add(new BSeparatorView(B_HORIZONTAL))
			.Add(fList, 1.0f)
			.Add(fSummary)
			.AddGroup(B_HORIZONTAL)
				.Add(new BButton("linuxkpiManager", B_TRANSLATE(
					"Linux drivers" B_UTF8_ELLIPSIS), open))
				.AddGlue()
			.End();
		_Fill();
	}

	int32 CountDevices() const { return fList->CountRows(); }

private:
	void _Fill()
	{
		BObjectList<PCIDevice, true> devices;
		ScanPCIDevices(devices);
		DriverCatalog catalog;
		catalog.Load();
		int32 linuxDrivers = 0;
		for (int32 i = 0; i < devices.CountItems(); i++) {
			const PCIDevice* device = devices.ItemAt(i);
			BString id;
			id.SetToFormat("%04x:%04x", device->vendor, device->device);
			const DeviceId* deviceId = NULL;
			Driver* driver = catalog.FindDriverFor(device->vendor,
				device->device, &deviceId);
			BString name(PCIClassName(device->classBase,
				device->classSub));
			BString driverName = B_TRANSLATE("Native or none");
			BString state;
			if (driver != NULL) {
				linuxDrivers++;
				if (deviceId != NULL && !deviceId->name.IsEmpty())
					name = deviceId->name;
				driverName.SetToFormat(B_TRANSLATE("Linux: %s"),
					driver->name.String());
				BStringList missing;
				if (deviceId != NULL)
					driver->MissingFirmware(*deviceId, missing);
				if (!driver->Installed())
					state = B_TRANSLATE("Driver not installed");
				else if (!missing.IsEmpty())
					state = B_TRANSLATE("Firmware missing");
				else if (!driver->interfaces.IsEmpty())
					state = B_TRANSLATE("Working");
				else if (!driver->enabledAtBoot)
					state = B_TRANSLATE("Disabled at boot");
				else
					state = B_TRANSLATE("Not running");
			}
			BRow* row = new BRow();
			row->SetField(new BStringField(name), 0);
			row->SetField(new BStringField(id), 1);
			row->SetField(new BStringField(driverName), 2);
			row->SetField(new BStringField(state), 3);
			fList->AddRow(row);
			printf("ControlPanel: device %s %s: %s %s\n", id.String(),
				name.String(), driverName.String(), state.String());
		}
		BString summary;
		summary.SetToFormat(B_TRANSLATE("%" B_PRId32 " devices, %" B_PRId32
			" with a Linux driver"), devices.CountItems(), linuxDrivers);
		fSummary->SetText(summary);
	}

	BColumnListView*	fList;
	BStringView*		fSummary;
};


/*!	A page whose settings are still in their own preference: what it holds
	and a button that opens it.
*/
class PreferencePage : public BGroupView {
public:
	PreferencePage(const PageInfo* page)
		:
		BGroupView(page->id, B_VERTICAL)
	{
		BLayoutBuilder::Group<> builder(this);
		builder
			.SetInsets(B_USE_WINDOW_SPACING)
			.Add(page_heading(page))
			.Add(new BSeparatorView(B_HORIZONTAL));
		for (size_t s = 0; s < sizeof(kSettings) / sizeof(kSettings[0]);
				s++) {
			if (strcmp(kSettings[s].page, page->id) == 0) {
				BString text("• ");
				text << kSettings[s].name;
				builder.Add(new BStringView(kSettings[s].name, text));
			}
		}
		BMessage* open = new BMessage(kMsgOpenPreference);
		open->AddString("id", page->id);
		BString label;
		BPath program(page->program);
		label.SetToFormat(B_TRANSLATE("Open %s" B_UTF8_ELLIPSIS),
			program.Leaf());
		BButton* button = new BButton("open", label, open);
		button->SetEnabled(page->program != NULL
			&& access(page->program, X_OK) == 0);
		builder
			.AddGroup(B_HORIZONTAL)
				.Add(button)
				.AddGlue()
			.End()
			.AddGlue();
	}
};


// #pragma mark - scrolling pages


/*!	Holds a page at its preferred size inside a BScrollView, so that tall
	pages (Displays, Audio) fit on small screens: the window keeps a
	modest minimum size and the page scrolls.
*/
class PageScroller : public BView {
public:
	PageScroller(BView* page)
		:
		BView(page->Name(), B_WILL_DRAW | B_FRAME_EVENTS),
		fPage(page)
	{
		SetViewUIColor(B_PANEL_BACKGROUND_COLOR);
		AddChild(page);
	}

	virtual void AttachedToWindow()
	{
		BView::AttachedToWindow();
		_Layout();
	}

	virtual void FrameResized(float width, float height)
	{
		BView::FrameResized(width, height);
		_Layout();
	}

	virtual BSize MinSize()
	{
		return BSize(360, 200);
	}

	virtual BSize PreferredSize()
	{
		BSize size = fPage->PreferredSize();
		return BSize(size.width, std::min(size.height, 520.0f));
	}

	virtual BSize MaxSize()
	{
		return BSize(B_SIZE_UNLIMITED, B_SIZE_UNLIMITED);
	}

private:
	void _Layout()
	{
		BSize preferred = fPage->PreferredSize();
		BSize minimum = fPage->MinSize();
		float width = std::max(Bounds().Width(), minimum.width);
		float height = std::max(Bounds().Height(),
			std::max(preferred.height, minimum.height));
		fPage->MoveTo(0, 0);
		fPage->ResizeTo(width, height);
		if (BScrollBar* bar = ScrollBar(B_VERTICAL)) {
			float range = std::max(0.0f, height - Bounds().Height());
			bar->SetRange(0, range);
			bar->SetProportion(range > 0
				? Bounds().Height() / height : 1.0f);
			bar->SetSteps(20, Bounds().Height());
		}
	}

	BView*	fPage;
};


static BView*
scrolling(BView* page)
{
	BScrollView* scroll = new BScrollView(page->Name(),
		new PageScroller(page), 0, false, true, B_NO_BORDER);
	return scroll;
}


// #pragma mark - window


class PageItem : public BStringItem {
public:
	PageItem(const PageInfo* page, uint32 level)
		:
		BStringItem(page != NULL ? page->title : "", level),
		fPage(page)
	{
	}

	PageItem(const char* category)
		:
		BStringItem(category, 0),
		fPage(NULL)
	{
	}

	const PageInfo* Page() const { return fPage; }

private:
	const PageInfo*	fPage;
};


class ControlPanelWindow : public BWindow {
public:
	ControlPanelWindow(const char* pageID)
		:
		BWindow(BRect(80, 60, 900, 620), B_TRANSLATE_SYSTEM_NAME(
			"Control Panel"), B_TITLED_WINDOW, B_QUIT_ON_WINDOW_CLOSE
				| B_AUTO_UPDATE_SIZE_LIMITS)
	{
		fSearch = new BTextControl("search", NULL, "",
			new BMessage(kMsgSearchChanged));
		fSearch->SetModificationMessage(new BMessage(kMsgSearchChanged));
		fSearch->TextView()->SetExplicitMinSize(BSize(180, B_SIZE_UNSET));

		fSidebar = new BOutlineListView("sidebar");
		fSidebar->SetSelectionMessage(new BMessage(kMsgPageSelected));
		std::map<std::string, PageItem*> categories;
		const char* order[] = { "Hardware", "Network", "Privacy", "System",
			"Appearance" };
		for (size_t c = 0; c < 5; c++) {
			PageItem* category = new PageItem(order[c]);
			fSidebar->AddItem(category);
			// AddUnder() puts an item first: add them last to first
			for (size_t i = sizeof(kPages) / sizeof(kPages[0]); i-- > 0;) {
				if (strcmp(kPages[i].category, order[c]) == 0)
					fSidebar->AddUnder(new PageItem(&kPages[i], 1), category);
			}
		}
		BScrollView* sidebarScroll = new BScrollView("sidebarScroll",
			fSidebar, 0, false, true);
		sidebarScroll->SetExplicitMinSize(BSize(190, 300));
		sidebarScroll->SetExplicitMaxSize(BSize(220, B_SIZE_UNLIMITED));

		fResults = new BListView("results");
		fResults->SetInvocationMessage(new BMessage(kMsgResultInvoked));
		fResults->SetSelectionMessage(new BMessage(kMsgResultInvoked));

		fCards = new BCardLayout();
		BView* cards = new BView("pages", 0, fCards);
		cards->SetViewUIColor(B_PANEL_BACKGROUND_COLOR);
		fCards->AddView(new BScrollView("resultsScroll", fResults, 0, false,
			true));
		for (size_t i = 0; i < sizeof(kPages) / sizeof(kPages[0]); i++)
			fCards->AddView(scrolling(_MakePage(&kPages[i])));

		BLayoutBuilder::Group<>(this, B_HORIZONTAL, 0)
			.AddGroup(B_VERTICAL)
				.SetInsets(B_USE_WINDOW_SPACING, B_USE_WINDOW_SPACING, 0,
					B_USE_WINDOW_SPACING)
				.Add(fSearch)
				.Add(sidebarScroll)
			.End()
			.Add(cards, 1.0f);

		_ShowPage(pageID != NULL ? pageID : "displays");
		CenterOnScreen();
	}

	virtual void MessageReceived(BMessage* message)
	{
		switch (message->what) {
			case kMsgSearchChanged:
				_Search(fSearch->Text());
				break;
			case kMsgPageSelected:
			{
				PageItem* item = dynamic_cast<PageItem*>(fSidebar->ItemAt(
					fSidebar->CurrentSelection()));
				if (item != NULL && item->Page() != NULL)
					_ShowPage(item->Page()->id, false);
				break;
			}
			case kMsgResultInvoked:
			{
				int32 index = fResults->CurrentSelection();
				if (index >= 0 && index < (int32)fResultPages.size())
					_ShowPage(fResultPages[index].c_str());
				break;
			}
			case kMsgOpenPreference:
			{
				const PageInfo* page = find_page(message->GetString("id", ""));
				if (page != NULL && page->program != NULL) {
					const char* path = page->program;
					entry_ref ref;
					if (get_ref_for_path(path, &ref) == B_OK)
						be_roster->Launch(&ref);
				}
				break;
			}
			case kMsgSearch:
			{
				fSearch->SetText(message->GetString("text", ""));
				_Search(fSearch->Text());
				BMessage reply(B_REPLY);
				for (int32 i = 0; i < fResults->CountItems(); i++) {
					reply.AddString("result",
						((BStringItem*)fResults->ItemAt(i))->Text());
				}
				message->SendReply(&reply);
				break;
			}
			case kMsgShowPage:
			{
				_ShowPage(message->GetString("id", ""));
				BMessage reply(B_REPLY);
				reply.AddString("page", fCurrent.c_str());
				message->SendReply(&reply);
				break;
			}
			default:
				BWindow::MessageReceived(message);
		}
	}

private:
	BView* _MakePage(const PageInfo* page)
	{
		if (strcmp(page->id, "displays") == 0) {
			BGroupView* group = new BGroupView("displays", B_VERTICAL);
			BLayoutBuilder::Group<>(group)
				.SetInsets(B_USE_WINDOW_SPACING)
				.Add(page_heading(page))
				.Add(new DisplaysView())
				.Add(new FrontCopySwitch());
			return group;
		}
		if (strcmp(page->id, "audio") == 0) {
			BGroupView* group = new BGroupView("audio", B_VERTICAL);
			BLayoutBuilder::Group<>(group)
				.SetInsets(B_USE_WINDOW_SPACING)
				.Add(page_heading(page))
				.Add(new AudioSettingsView());
			return group;
		}
		if (strcmp(page->id, "renguin") == 0) {
			BGroupView* group = new BGroupView("renguin", B_VERTICAL);
			BLayoutBuilder::Group<>(group)
				.SetInsets(B_USE_WINDOW_SPACING)
				.Add(page_heading(page))
				.Add(new RenguinView());
			return group;
		}
		if (strcmp(page->id, "workspaces") == 0)
			return create_windows_page();
		if (strcmp(page->id, "streaming") == 0)
			return new StreamingPage();
		if (strcmp(page->id, "devices") == 0)
			return new DriversPage();
		if (strcmp(page->id, "boot") == 0)
			return new BootPage();
		return new PreferencePage(page);
	}

	void _ShowPage(const char* id, bool selectInSidebar = true)
	{
		for (size_t i = 0; i < sizeof(kPages) / sizeof(kPages[0]); i++) {
			if (strcmp(kPages[i].id, id) != 0)
				continue;
			fCards->SetVisibleItem((int32)i + 1);
			fCurrent = id;
			printf("ControlPanel: page %s\n", id);
			if (selectInSidebar) {
				for (int32 j = 0; j < fSidebar->FullListCountItems(); j++) {
					PageItem* item = dynamic_cast<PageItem*>(
						fSidebar->FullListItemAt(j));
					if (item != NULL && item->Page() == &kPages[i]) {
						fSidebar->Select(fSidebar->IndexOf(item));
						break;
					}
				}
			}
			return;
		}
	}

	void _Search(const char* text)
	{
		while (BListItem* item = fResults->RemoveItem((int32)0))
			delete item;
		fResultPages.clear();
		if (text == NULL || text[0] == '\0') {
			_ShowPage(fCurrent.empty() ? "displays" : fCurrent.c_str());
			return;
		}
		std::vector<std::pair<const PageInfo*, std::string> > results
			= search(text);
		for (size_t i = 0; i < results.size(); i++) {
			BString label;
			label << results[i].second.c_str() << "  —  "
				<< results[i].first->title;
			fResults->AddItem(new BStringItem(label));
			fResultPages.push_back(results[i].first->id);
		}
		if (results.empty())
			fResults->AddItem(new BStringItem(B_TRANSLATE("Nothing found")));
		fCards->SetVisibleItem((int32)0);
	}

	BTextControl*		fSearch;
	BOutlineListView*	fSidebar;
	BListView*			fResults;
	std::vector<std::string> fResultPages;
	BCardLayout*		fCards;
	std::string			fCurrent;
};


class ControlPanelApp : public BApplication {
public:
	ControlPanelApp(const char* page)
		:
		BApplication(kSignature),
		fPage(page)
	{
	}

	virtual void ReadyToRun()
	{
		fWindow = new ControlPanelWindow(fPage);
		fWindow->Show();
	}

	virtual void MessageReceived(BMessage* message)
	{
		switch (message->what) {
			case kMsgSearch:
			case kMsgShowPage:
				fWindow->PostMessage(message);
				break;
			default:
				BApplication::MessageReceived(message);
		}
	}

private:
	const char*			fPage;
	ControlPanelWindow*	fWindow;
};


int
main(int argc, char** argv)
{
	if (argc == 3 && strcmp(argv[1], "--search") == 0) {
		std::vector<std::pair<const PageInfo*, std::string> > results
			= search(argv[2]);
		for (size_t i = 0; i < results.size(); i++) {
			printf("%s\t%s\t%s\n", results[i].first->id,
				results[i].first->title, results[i].second.c_str());
		}
		return results.empty() ? 1 : 0;
	}
	if (argc == 2 && strcmp(argv[1], "--pages") == 0) {
		for (size_t i = 0; i < sizeof(kPages) / sizeof(kPages[0]); i++) {
			printf("%s\t%s\t%s\n", kPages[i].id, kPages[i].category,
				kPages[i].title);
		}
		return 0;
	}
	const char* page = NULL;
	if (argc == 3 && strcmp(argv[1], "--page") == 0)
		page = argv[2];
	else if (argc != 1) {
		fprintf(stderr, "usage: ControlPanel [--page <id> | --search <text> "
			"| --pages]\n");
		return 1;
	}

	ControlPanelApp app(page);
	app.Run();
	return 0;
}
