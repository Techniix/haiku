/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 */
#ifndef _RENGA_STREAMER_MODE_H
#define _RENGA_STREAMER_MODE_H


/*!	Streamer mode (Renga, batch 5; roadmap F-31 "Streaming features"): one
	master switch, off by default, and parts that each have their own
	switch, on by default, which only act while the master switch is on:

	  enabled false                  the master switch
	  hide_notification_content true notifications show only the
	                                 application's name
	  pause_notifications false      Do Not Disturb: notifications are held
	                                 and shown when streamer mode ends
	  hide_marked_windows true       windows marked "Hide from capture"
	                                 are hidden (also outside streamer mode
	                                 when Hide from capture's own switch is
	                                 on; this part keeps them hidden while
	                                 streaming even when it is off)

	The file is ~/config/settings/Renga/streamer_mode, "key value" lines;
	missing keys have the defaults above. Written by the Screen recording
	preferences (Streaming tab) and "ScreenRecording --streamer-mode
	on|off"; read by notification_server and app_server.
*/


#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include <FindDirectory.h>


namespace StreamerMode {


struct Settings {
	bool	enabled;
	bool	hideNotificationContent;
	bool	pauseNotifications;
	bool	hideMarkedWindows;
};


static inline void
defaults(Settings& settings)
{
	settings.enabled = false;
	settings.hideNotificationContent = true;
	settings.pauseNotifications = false;
	settings.hideMarkedWindows = true;
}


static inline bool
path(char* buffer, size_t size)
{
	char settings[B_PATH_NAME_LENGTH];
	if (find_directory(B_USER_SETTINGS_DIRECTORY, -1, false, settings,
			sizeof(settings)) != B_OK)
		return false;
	snprintf(buffer, size, "%s/Renga/streamer_mode", settings);
	return true;
}


static inline bool
parse_bool(const char* value, bool fallback)
{
	if (strncmp(value, "true", 4) == 0 || strncmp(value, "on", 2) == 0)
		return true;
	if (strncmp(value, "false", 5) == 0 || strncmp(value, "off", 3) == 0)
		return false;
	return fallback;
}


static inline void
load(Settings& settings)
{
	defaults(settings);
	char file[B_PATH_NAME_LENGTH];
	if (!path(file, sizeof(file)))
		return;
	FILE* in = fopen(file, "r");
	if (in == NULL)
		return;
	char line[256];
	while (fgets(line, sizeof(line), in) != NULL) {
		char* value = strchr(line, ' ');
		if (value == NULL)
			continue;
		*value++ = '\0';
		if (strcmp(line, "enabled") == 0)
			settings.enabled = parse_bool(value, settings.enabled);
		else if (strcmp(line, "hide_notification_content") == 0) {
			settings.hideNotificationContent = parse_bool(value,
				settings.hideNotificationContent);
		} else if (strcmp(line, "pause_notifications") == 0) {
			settings.pauseNotifications = parse_bool(value,
				settings.pauseNotifications);
		} else if (strcmp(line, "hide_marked_windows") == 0) {
			settings.hideMarkedWindows = parse_bool(value,
				settings.hideMarkedWindows);
		}
	}
	fclose(in);
}


//!	Written through a temporary file, never left half written.
static inline bool
save(const Settings& settings)
{
	char file[B_PATH_NAME_LENGTH];
	if (!path(file, sizeof(file)))
		return false;
	char directory[B_PATH_NAME_LENGTH];
	strlcpy(directory, file, sizeof(directory));
	*strrchr(directory, '/') = '\0';
	mkdir(directory, 0755);

	char temporary[B_PATH_NAME_LENGTH];
	snprintf(temporary, sizeof(temporary), "%s.new", file);
	FILE* out = fopen(temporary, "w");
	if (out == NULL)
		return false;
	fprintf(out, "enabled %s\nhide_notification_content %s\n"
		"pause_notifications %s\nhide_marked_windows %s\n",
		settings.enabled ? "true" : "false",
		settings.hideNotificationContent ? "true" : "false",
		settings.pauseNotifications ? "true" : "false",
		settings.hideMarkedWindows ? "true" : "false");
	bool ok = fflush(out) == 0;
	fclose(out);
	return ok && rename(temporary, file) == 0;
}


}	// namespace StreamerMode


#endif	// _RENGA_STREAMER_MODE_H
