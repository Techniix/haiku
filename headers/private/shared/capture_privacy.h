/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 */
#ifndef _RENGA_CAPTURE_PRIVACY_H
#define _RENGA_CAPTURE_PRIVACY_H


/*!	The user's "Hide from capture" rules (Renga, batch 5), as the Screen
	recording preferences and the tests write them; app_server reads the
	same file (src/servers/app/CapturePrivacy.cpp):

	  enabled true|false             the switch (on by default)
	  app <signature>                every window of the application
	  window <signature>\t<title>    one window, by its title

	~/config/settings/Renga/capture_privacy, replaced (not edited) on every
	change.
*/


#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include <string>
#include <vector>

#include <FindDirectory.h>


namespace CapturePrivacyRules {


struct Rules {
	bool						enabled;
	std::vector<std::string>	apps;
	std::vector<std::string>	windows;
		// "<signature>\t<title>"

	Rules() : enabled(true) {}
};


static inline bool
path(char* buffer, size_t size)
{
	char settings[B_PATH_NAME_LENGTH];
	if (find_directory(B_USER_SETTINGS_DIRECTORY, -1, false, settings,
			sizeof(settings)) != B_OK)
		return false;
	snprintf(buffer, size, "%s/Renga/capture_privacy", settings);
	return true;
}


static inline void
load(Rules& rules)
{
	rules = Rules();
	char file[B_PATH_NAME_LENGTH];
	if (!path(file, sizeof(file)))
		return;
	FILE* in = fopen(file, "r");
	if (in == NULL)
		return;
	char line[1024];
	while (fgets(line, sizeof(line), in) != NULL) {
		line[strcspn(line, "\r\n")] = '\0';
		if (strncmp(line, "enabled ", 8) == 0)
			rules.enabled = strcmp(line + 8, "false") != 0;
		else if (strncmp(line, "app ", 4) == 0)
			rules.apps.push_back(line + 4);
		else if (strncmp(line, "window ", 7) == 0 && strchr(line, '\t'))
			rules.windows.push_back(line + 7);
	}
	fclose(in);
}


static inline bool
save(const Rules& rules)
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
	fprintf(out, "# Hide from capture (Screen recording preferences)\n"
		"enabled %s\n", rules.enabled ? "true" : "false");
	for (size_t i = 0; i < rules.apps.size(); i++)
		fprintf(out, "app %s\n", rules.apps[i].c_str());
	for (size_t i = 0; i < rules.windows.size(); i++)
		fprintf(out, "window %s\n", rules.windows[i].c_str());
	bool ok = fflush(out) == 0;
	fclose(out);
	return ok && rename(temporary, file) == 0;
}


//!	Adds or removes \a entry ("<signature>" or "<signature>\t<title>").
static inline void
set(std::vector<std::string>& list, const std::string& entry, bool present)
{
	for (size_t i = 0; i < list.size(); i++) {
		if (strcasecmp(list[i].c_str(), entry.c_str()) == 0) {
			if (!present)
				list.erase(list.begin() + i);
			return;
		}
	}
	if (present)
		list.push_back(entry);
}


static inline bool
contains(const std::vector<std::string>& list, const std::string& entry)
{
	for (size_t i = 0; i < list.size(); i++) {
		if (strcasecmp(list[i].c_str(), entry.c_str()) == 0)
			return true;
	}
	return false;
}


}	// namespace CapturePrivacyRules


#endif	// _RENGA_CAPTURE_PRIVACY_H
