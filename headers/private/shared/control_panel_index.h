/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 */
#ifndef _CONTROL_PANEL_INDEX_H
#define _CONTROL_PANEL_INDEX_H


/*!	The Control Panel's pages and settings with their synonyms, and the
	search over them (roadmap F-28). Shared, so that every search shows the
	same results: the Control Panel's own search field and its --search
	option, and the Deskbar's menu search (F-41, batch 9), which opens a
	result with "ControlPanel --page <id>".

	Header only: static tables and functions, one copy per program that
	includes it (a few kilobytes). Moved here unchanged from
	src/apps/controlpanel/ControlPanel.cpp.
*/


#include <stdio.h>
#include <string.h>
#include <strings.h>

#include <string>
#include <utility>
#include <vector>


namespace ControlPanelIndex {




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
	// batch 9 (F-41)
	{ "deskbar", "Search in the Deskbar menu", "find launcher start apps "
		"run" },
	{ "deskbar", "Calculator and unit conversion in the search",
		"calculate math convert units" },
	{ "deskbar", "Find files by name in the search", "query files" },
	{ "deskbar", "Restart into in the Shutdown menu", "reboot boot menu "
		"restore point windows" },
	{ "deskbar", "Recent documents in application menus", "jump list "
		"recent files" },
	// batch 9 (F-66)
	{ "tracker", "Show folder sizes", "size column disk usage" },
	{ "tracker", "Keep both when names collide", "replace copy conflict "
		"duplicate" },
	{ "tracker", "Open Terminal here", "shell command line folder" },
	{ "tracker", "Quick Look with the space bar", "preview space" },
	{ "tracker", "Rename several items at once", "batch rename multiple" },
	{ "tracker", "Type to filter", "type ahead filtering search" },
};


static inline const PageInfo*
find_page(const char* id)
{
	for (size_t i = 0; i < sizeof(kPages) / sizeof(kPages[0]); i++) {
		if (strcmp(kPages[i].id, id) == 0)
			return &kPages[i];
	}
	return NULL;
}


static inline bool
contains_word(const char* haystack, const std::string& needle)
{
	return haystack != NULL && strcasestr(haystack, needle.c_str()) != NULL;
}


/*!	The settings matching \a text: every word of it must be found in the
	setting's name, its synonyms, its page's title or the page's synonyms.
	A page matching by itself is listed too, as its first setting.
*/
static inline std::vector<std::pair<const PageInfo*, std::string> >
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


}	// namespace ControlPanelIndex


#endif	// _CONTROL_PANEL_INDEX_H
