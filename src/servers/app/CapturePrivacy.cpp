/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 */


#include "CapturePrivacy.h"

#include <set>
#include <string>

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include <Autolock.h>
#include <FindDirectory.h>
#include <Locker.h>
#include <OS.h>
#include <Path.h>

#include <streamer_mode.h>

#include "ServerApp.h"
#include "ServerWindow.h"
#include "Window.h"


namespace CapturePrivacy {


static BLocker sLock("capture privacy");
static bool sEnabled = true;
static std::set<std::string> sApps;
	// signatures, lowercase
static std::set<std::string> sWindows;
	// "<signature lowercase>\t<title>"
static std::set<int32> sTokens;
	// windows whose application asked
static bool sStreamerHides = false;
	// streamer mode on with its "hide marked windows" part
static bigtime_t sLastCheck = 0;
static ino_t sStreamerNode = -1;
static time_t sStreamerTime = -1;
static time_t sFileTime = -1;
static off_t sFileSize = -1;
static ino_t sFileNode = -1;


static std::string
lower(const char* text)
{
	std::string result(text != NULL ? text : "");
	for (size_t i = 0; i < result.size(); i++) {
		if (result[i] >= 'A' && result[i] <= 'Z')
			result[i] += 'a' - 'A';
	}
	return result;
}


/*!	Re-reads the settings file if it changed; with sLock held. Lines:
	"enabled true|false", "app <signature>", "window <signature>\t<title>";
	"#" starts a comment.
*/
static void
reload_if_changed()
{
	bigtime_t now = system_time();
	if (sLastCheck != 0 && now - sLastCheck < 1000000)
		return;
	sLastCheck = now;

	// streamer mode: its part keeps marked windows hidden while streaming
	char streamerPath[B_PATH_NAME_LENGTH];
	struct stat streamerStat;
	if (StreamerMode::path(streamerPath, sizeof(streamerPath))) {
		bool exists = stat(streamerPath, &streamerStat) == 0;
		time_t streamerTime = exists ? streamerStat.st_mtime : 0;
		ino_t streamerNode = exists ? streamerStat.st_ino : 0;
			// the writers replace the file: a new node every time
		if (streamerTime != sStreamerTime || streamerNode != sStreamerNode) {
			sStreamerTime = streamerTime;
			sStreamerNode = streamerNode;
			StreamerMode::Settings streamer;
			StreamerMode::load(streamer);
			sStreamerHides = streamer.enabled && streamer.hideMarkedWindows;
		}
	}

	BPath path;
	if (find_directory(B_USER_SETTINGS_DIRECTORY, &path) != B_OK
		|| path.Append("Renga/capture_privacy") != B_OK)
		return;
	struct stat st;
	if (stat(path.Path(), &st) != 0) {
		if (sFileTime != 0) {
			sFileTime = 0;
			sFileSize = 0;
			sEnabled = true;
			sApps.clear();
			sWindows.clear();
		}
		return;
	}
	if (st.st_mtime == sFileTime && st.st_size == sFileSize
		&& st.st_ino == sFileNode)
		return;
	sFileTime = st.st_mtime;
	sFileSize = st.st_size;
	sFileNode = st.st_ino;

	sEnabled = true;
	sApps.clear();
	sWindows.clear();
	FILE* file = fopen(path.Path(), "r");
	if (file == NULL)
		return;
	char line[1024];
	while (fgets(line, sizeof(line), file) != NULL) {
		line[strcspn(line, "\r\n")] = '\0';
		if (line[0] == '#' || line[0] == '\0')
			continue;
		if (strncmp(line, "enabled ", 8) == 0)
			sEnabled = strcmp(line + 8, "false") != 0;
		else if (strncmp(line, "app ", 4) == 0)
			sApps.insert(lower(line + 4));
		else if (strncmp(line, "window ", 7) == 0) {
			char* tab = strchr(line + 7, '\t');
			if (tab == NULL)
				continue;
			*tab = '\0';
			sWindows.insert(lower(line + 7) + "\t" + (tab + 1));
		}
	}
	fclose(file);
}


bool
Enabled()
{
	BAutolock locker(sLock);
	reload_if_changed();
	return sEnabled || sStreamerHides;
}


bool
IsHidden(Window* window)
{
	if (window == NULL || window->ServerWindow() == NULL)
		return false;

	BAutolock locker(sLock);
	reload_if_changed();
	if (!sEnabled && !sStreamerHides)
		return false;
	if (sTokens.find(window->ServerWindow()->ServerToken()) != sTokens.end())
		return true;
	if (sApps.empty() && sWindows.empty())
		return false;

	ServerApp* app = window->ServerWindow()->App();
	std::string signature = lower(app != NULL ? app->Signature() : "");
	if (signature.empty())
		return false;
	if (sApps.find(signature) != sApps.end())
		return true;
	return sWindows.find(signature + "\t" + (window->Title() != NULL
		? window->Title() : "")) != sWindows.end();
}


void
SetWindowHidden(int32 serverToken, bool hidden)
{
	BAutolock locker(sLock);
	if (hidden)
		sTokens.insert(serverToken);
	else
		sTokens.erase(serverToken);
}


}	// namespace CapturePrivacy
