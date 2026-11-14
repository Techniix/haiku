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
#include <String.h>
#include <StringView.h>
#include <View.h>
#include <Window.h>

#include <linux/input.h>


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
	MSG_HIDE_OVERLAY	= 'ghov'
};


// #pragma mark - settings


struct Settings {
	bool	overlay;
	bool	lowBattery;
	bool	rawAccess;

	Settings() { Defaults(); }

	void Defaults()
	{
		overlay = true;
		lowBattery = true;
		rawAccess = true;
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
		return NULL;
	}
};


#endif	// GAMEPADS_COMMON_H
