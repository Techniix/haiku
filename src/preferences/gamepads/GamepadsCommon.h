/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 */
#ifndef GAMEPADS_COMMON_H
#define GAMEPADS_COMMON_H


/*!	What the Gamepads preferences (Gamepads.cpp) and the gamepad agent
	(GamepadAgent.cpp, the gamepad_agent server) share: the settings file
	and its defaults, the timing of the guide-button overlay and the
	battery thresholds, and the message codes. See Gamepads.cpp for what
	the settings do.
*/


#include <algorithm>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <Alert.h>
#include <Application.h>
#include <Autolock.h>
#include <Bitmap.h>
#include <Button.h>
#include <CheckBox.h>
#include <Directory.h>
#include <File.h>
#include <FindDirectory.h>
#include <Font.h>
#include <Gamepad.h>
#include <GroupLayout.h>
#include <LayoutBuilder.h>
#include <ListView.h>
#include <Locker.h>
#include <MenuField.h>
#include <MenuItem.h>
#include <MessageRunner.h>
#include <Notification.h>
#include <ObjectList.h>
#include <Path.h>
#include <PopUpMenu.h>
#include <Screen.h>
#include <ScrollView.h>
#include <SeparatorView.h>
#include <Slider.h>
#include <Box.h>
#include <String.h>
#include <StringView.h>
#include <View.h>
#include <Window.h>

#include <linux/input.h>

#include <IconUtils.h>
#include <Resources.h>
#include <Roster.h>
#include <StringList.h>
#include <WindowInfo.h>
#include <streamer_mode.h>


static const bigtime_t kGuideHold = 500000;
static const int32 kLowBattery = 15;
static const int32 kBatteryRecovered = 20;

enum {
	MSG_SELECT			= 'gsel',
	MSG_TICK			= 'gtck',
	MSG_RUMBLE			= 'grmb',
	MSG_PLAYER			= 'gply',
	MSG_SETTING			= 'gset',
	MSG_DEFAULTS		= 'gdef',
	MSG_SHOW_OVERLAY	= 'govl',
	MSG_HIDE_OVERLAY	= 'ghov',
	MSG_TUNING			= 'gtun',
	MSG_MAP				= 'gmap',
	MSG_MAP_RESET		= 'gmrs',
	MSG_MAP_SKIP		= 'gmsk',
	MSG_MAP_BACK		= 'gmbk',
	MSG_MAP_SAVE		= 'gmsv',
	MSG_MAP_TICK		= 'gmtk',
	MSG_MAP_DONE		= 'gmdn',
	// batch 11 (configurator polish)
	MSG_IDENTIFY		= 'gidf',
	MSG_SILENCE			= 'gsil',
	MSG_OPEN_SETTINGS	= 'gost',
	MSG_MAPPING			= 'gmpg'
};


// #pragma mark - layout families (batch 11)


/*!	The layout family a pad belongs to, for its picture and the labels of
	its face buttons. By what tells the families apart, never a vendor's
	shape: offset sticks (Xbox-style), symmetric sticks (PlayStation-style),
	Nintendo face order, a retro pad without sticks, a generic pad, an
	arcade stick. Pictures: GamepadLayouts.rdef ("layout:<family>").
*/
enum layout_family {
	LAYOUT_OFFSET = 0,
	LAYOUT_SYMMETRIC,
	LAYOUT_NINTENDO,
	LAYOUT_RETRO,
	LAYOUT_GENERIC,
	LAYOUT_ARCADE
};

static const char* const kLayoutNames[] = {
	"offset", "symmetric", "nintendo", "retro", "generic", "arcade"
};

// for people: the window and the notice
static const char* const kLayoutTitles[] = {
	"Offset sticks layout", "Symmetric sticks layout",
	"Nintendo button layout", "Retro layout (no sticks)", "Generic layout",
	"Arcade stick layout"
};

// what each family calls its face buttons: south, east, west, north
static const char* const kFaceLabels[][4] = {
	{ "A", "B", "X", "Y" },
	{ "\xC3\x97", "\xE2\x97\x8B", "\xE2\x96\xA1", "\xE2\x96\xB3" },
		// cross, circle, square, triangle
	{ "B", "A", "Y", "X" },
	{ "B", "A", "Y", "X" },
	{ "1", "2", "3", "4" },
	{ "A", "B", "X", "Y" }
};


static bool
name_has(const char* name, const char* word)
{
	return name != NULL && strcasestr(name, word) != NULL;
}


static inline layout_family
gamepad_layout_family(uint16 vendor, uint16 product, const char* name)
{
	if (name_has(name, "fight") || name_has(name, "arcade")
		|| name_has(name, "hori stick") || name_has(name, "fightstick"))
		return LAYOUT_ARCADE;
	if (name_has(name, "SN30") || name_has(name, "SNES")
		|| name_has(name, "retro") || name_has(name, "NES"))
		return LAYOUT_RETRO;
	switch (vendor) {
		case 0x045e:	// Microsoft
		case 0x0e6f:	// PDP
		case 0x24c6:	// PowerA
		case 0x1532:	// Razer
			return LAYOUT_OFFSET;
		case 0x054c:	// Sony
			return LAYOUT_SYMMETRIC;
		case 0x057e:	// Nintendo
			return LAYOUT_NINTENDO;
		case 0x2dc8:	// 8BitDo: Xbox-style in X-input, Nintendo-style
						// labels otherwise
			return name_has(name, "x-input") || name_has(name, "xinput")
				|| name_has(name, "xbox") ? LAYOUT_OFFSET : LAYOUT_NINTENDO;
	}
	if (name_has(name, "xbox") || name_has(name, "x-box")
		|| name_has(name, "xinput"))
		return LAYOUT_OFFSET;
	if (name_has(name, "playstation") || name_has(name, "dualshock")
		|| name_has(name, "dualsense"))
		return LAYOUT_SYMMETRIC;
	if (name_has(name, "switch") || name_has(name, "joy-con"))
		return LAYOUT_NINTENDO;
	(void)product;
	return LAYOUT_GENERIC;
}


//!	The family's picture at \a size pixels, NULL if missing.
static inline BBitmap*
gamepad_layout_icon(layout_family family, float size)
{
	app_info info;
	if (be_app == NULL || be_app->GetAppInfo(&info) != B_OK)
		return NULL;
	BFile file(&info.ref, B_READ_ONLY);
	BResources resources;
	if (resources.SetTo(&file) != B_OK)
		return NULL;
	BString name("layout:");
	name << kLayoutNames[family];
	size_t length;
	const void* data = resources.LoadResource('VICN', name.String(),
		&length);
	if (data == NULL)
		return NULL;
	BBitmap* bitmap = new BBitmap(BRect(0, 0, size - 1, size - 1),
		B_RGBA32);
	if (BIconUtils::GetVectorIcon((const uint8*)data, length, bitmap)
			!= B_OK) {
		delete bitmap;
		return NULL;
	}
	return bitmap;
}


/*!	Quiet mode for the connect notice (batch 11): while a full-screen
	program is in front (its window covers the screen) or streamer mode is
	on, the notice is small: the name only, no picture, shown briefly.
*/
static inline bool
gamepad_quiet_now(BString* why = NULL)
{
	StreamerMode::Settings streamer;
	StreamerMode::load(streamer);
	if (streamer.enabled) {
		if (why != NULL)
			*why = "streamer mode";
		return true;
	}
	int32* tokens;
	int32 count;
	if (BPrivate::get_window_order(current_workspace(), &tokens, &count)
			!= B_OK)
		return false;
	BRect screen = BScreen().Frame();
	bool quiet = false;
	for (int32 i = 0; i < count; i++) {
		client_window_info* window = get_window_info(tokens[i]);
		if (window == NULL)
			continue;
		bool candidate = !window->is_mini && window->show_hide_level <= 0
			&& window->feel == B_NORMAL_WINDOW_FEEL;
		if (candidate) {
			// the frontmost normal window decides
			quiet = window->window_left <= screen.left
				&& window->window_top <= screen.top
				&& window->window_right >= screen.right
				&& window->window_bottom >= screen.bottom;
			if (quiet && why != NULL)
				*why = window->name;
			free(window);
			break;
		}
		free(window);
	}
	free(tokens);
	return quiet;
}


// #pragma mark - settings


struct Settings {
	bool	overlay;
	bool	lowBattery;
	bool	rawAccess;
	bool	connectNotice;
	// batch 11
	bool	quietMode;		// quiet notices in full screen and streamer mode
	BStringList silenced;	// GUIDs: "Don't show again for this controller"

	Settings() { Defaults(); }

	void Defaults()
	{
		overlay = true;
		lowBattery = true;
		rawAccess = true;
		connectNotice = true;
		quietMode = true;
		silenced.MakeEmpty();
	}

	bool IsDefault() const
	{
		Settings defaults;
		return overlay == defaults.overlay
			&& lowBattery == defaults.lowBattery
			&& rawAccess == defaults.rawAccess
			&& connectNotice == defaults.connectNotice
			&& quietMode == defaults.quietMode && silenced.IsEmpty();
	}

	static status_t Path(BPath& path)
	{
		status_t status = find_directory(B_USER_SETTINGS_DIRECTORY, &path);
		if (status == B_OK)
			status = path.Append("Gamepads/settings");
		return status;
	}

	// raw access lives in the kernel driver settings, read by usb_hid
	static status_t RawPath(BPath& path)
	{
		status_t status = find_directory(B_USER_SETTINGS_DIRECTORY, &path);
		if (status == B_OK)
			status = path.Append("kernel/drivers/gamepad");
		return status;
	}

	void Load()
	{
		Defaults();
		BPath path;
		BMessage archive;
		BFile file;
		if (Path(path) == B_OK
			&& file.SetTo(path.Path(), B_READ_ONLY) == B_OK
			&& archive.Unflatten(&file) == B_OK) {
			archive.FindBool("overlay", &overlay);
			archive.FindBool("low_battery", &lowBattery);
			archive.FindBool("connect_notice", &connectNotice);
			archive.FindBool("quiet_mode", &quietMode);
			const char* guid;
			for (int32 i = 0; archive.FindString("silenced", i, &guid)
					== B_OK; i++) {
				silenced.Add(guid);
			}
		}
		if (RawPath(path) == B_OK
			&& file.SetTo(path.Path(), B_READ_ONLY) == B_OK) {
			char buffer[256];
			ssize_t bytes = file.Read(buffer, sizeof(buffer) - 1);
			if (bytes > 0) {
				buffer[bytes] = '\0';
				if (strstr(buffer, "raw_access false") != NULL)
					rawAccess = false;
			}
		}
	}

	status_t Save() const
	{
		BPath path;
		status_t status = Path(path);
		if (status != B_OK)
			return status;
		BPath parent;
		path.GetParent(&parent);
		create_directory(parent.Path(), 0755);
		BMessage archive;
		archive.AddBool("overlay", overlay);
		archive.AddBool("low_battery", lowBattery);
		archive.AddBool("connect_notice", connectNotice);
		archive.AddBool("quiet_mode", quietMode);
		for (int32 i = 0; i < silenced.CountStrings(); i++)
			archive.AddString("silenced", silenced.StringAt(i));
		BFile file(path.Path(), B_WRITE_ONLY | B_CREATE_FILE | B_ERASE_FILE);
		status = file.InitCheck();
		if (status == B_OK)
			status = archive.Flatten(&file);

		if (RawPath(path) == B_OK) {
			if (rawAccess)
				unlink(path.Path());
			else {
				path.GetParent(&parent);
				create_directory(parent.Path(), 0755);
				BFile raw(path.Path(),
					B_WRITE_ONLY | B_CREATE_FILE | B_ERASE_FILE);
				const char text[] = "# Gamepads preferences: no raw (hidraw) "
					"access to game controllers\nraw_access false\n";
				raw.Write(text, sizeof(text) - 1);
			}
		}
		return status;
	}

	bool* Find(const char* name)
	{
		if (strcmp(name, "overlay") == 0)
			return &overlay;
		if (strcmp(name, "low_battery") == 0)
			return &lowBattery;
		if (strcmp(name, "raw_access") == 0)
			return &rawAccess;
		if (strcmp(name, "connect_notice") == 0)
			return &connectNotice;
		if (strcmp(name, "quiet_mode") == 0)
			return &quietMode;
		return NULL;
	}
};


#endif	// GAMEPADS_COMMON_H
