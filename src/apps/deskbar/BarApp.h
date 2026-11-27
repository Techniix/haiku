/*
Open Tracker License

Terms and Conditions

Copyright (c) 1991-2000, Be Incorporated. All rights reserved.

Permission is hereby granted, free of charge, to any person obtaining a copy of
this software and associated documentation files (the "Software"), to deal in
the Software without restriction, including without limitation the rights to
use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies
of the Software, and to permit persons to whom the Software is furnished to do
so, subject to the following conditions:

The above copyright notice and this permission notice applies to all licensees
and shall be included in all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF TITLE, MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
BE INCORPORATED BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN
AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF, OR IN CONNECTION
WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

Except as contained in this notice, the name of Be Incorporated shall not be
used in advertising or otherwise to promote the sale, use or other dealings in
this Software without prior written authorization from Be Incorporated.

Tracker(TM), Be(R), BeOS(R), and BeIA(TM) are trademarks or registered
trademarks of Be Incorporated in the United States and other countries. Other
brand product names are registered trademarks or trademarks of their respective
holders.
All rights reserved.
*/
#ifndef BAR_APP_H
#define BAR_APP_H


#include <Server.h>
#include <String.h>

#include "BarSettings.h"


/* ------------------------------------ */
// Private app_server defines that I need to use

#define _DESKTOP_W_TYPE_ 1024
#define _FLOATER_W_TYPE_ 4
#define _STD_W_TYPE_ 0


const uint32 kWin95 = 'Bill';
const uint32 kAmiga = 'Ncro';
const uint32 kMac = 'WcOS';
const uint32 kBe = 'Tabs';
const uint32 kAlwaysTop = 'TTop';
const uint32 kToggleDraggers = 'TDra';
const uint32 kUnsubscribe = 'Unsb';
const uint32 kAddTeam = 'AdTm';
const uint32 kRemoveTeam = 'RmTm';
const uint32 kRestart = 'Rtrt';
const uint32 kShutDown = 'ShDn';

// from roster_private.h
const uint32 kShutdownSystem = 301;
const uint32 kRebootSystem = 302;
const uint32 kSuspendSystem = 304;

// icon size constants
const int32 kIconPadding = B_USE_SMALL_SPACING;
const int32 kMinimumIconSize = 16;
const int32 kMaximumIconSize = 96;
const int32 kIconSizeInterval = 8;
const int32 kIconCacheCount = (kMaximumIconSize - kMinimumIconSize)
	/ kIconSizeInterval + 1;

// font size constants
const int32 kMinimumFontSize = 8;
const int32 kMaximumFontSize = 72;
const int32 kFontSizeInterval = 1;
const int32 kWindowIconCacheCount = (kMaximumFontSize - kMinimumFontSize)
	/ kFontSizeInterval + 1;

// update preferences message constant
const uint32 kUpdatePreferences = 'Pref';

// realign replicants message constant
const uint32 kRealignReplicants = 'Algn';

// Renga (F-41, batch 9): settings switches (PreferencesWindow), "Restart
// into" ("next": "menu" or "state:<directory>"; "test" in harness images
// sets the choice and replies instead of restarting) and the harness hooks
// (harness images only, see TBarApp::MessageReceived())
const uint32 kToggleSearch = 'DStS';
const uint32 kToggleSearchCalculator = 'DStC';
const uint32 kToggleSearchFiles = 'DStF';
const uint32 kToggleRestartInto = 'DStR';
const uint32 kToggleAppRecentDocuments = 'DStJ';
const uint32 kRestartInto = 'DSri';
const uint32 kTestDeskbarMenu = 'Tdmn';
const uint32 kTestSearch = 'Tsrq';
const uint32 kTestSearchWindow = 'Tsrw';
const uint32 kTestAppRecentDocuments = 'Tjmp';
const uint32 kTestShowPreferences = 'Tdpf';
const uint32 kTestWaylandClients = 'Twcl';
// Renga (batch 13): the window switcher's Wayland programs
const uint32 kToggleSwitcherLinuxPrograms = 'DStW';
const uint32 kSwitcherSettingChanged = 'SwSc';
const uint32 kTestSwitcher = 'Twsw';
	// harness images only: the switcher's groups (TSwitchManager)

/* --------------------------------------------- */

struct window_info;
class BBitmap;
class BFile;
class BList;
class PreferencesWindow;
class TBarView;
class TSearchWindow;
class TBarWindow;


class BarTeamInfo {
public:
									BarTeamInfo(BList* teams, uint32 flags,
										char* sig, char* name, BBitmap* icon = NULL);
									BarTeamInfo(const BarTeamInfo &info);
									~BarTeamInfo();

private:
			void					_Init();

public:
			BList*					teams;
			uint32					flags;
			char*					sig;
			char*					name;
			BBitmap*				icon;
			BBitmap*				iconCache[kIconCacheCount];

			// Renga (batch 12): an entry for a Wayland program (a client
			// of the compositor, whose team is "teams"); see
			// TBarApp::IsWaylandClient()
			bool					waylandClient;
			BString					iconPath;
};


class WindowIconCache {
public:
									WindowIconCache(int32 id, BBitmap* icon = NULL);
									WindowIconCache(const WindowIconCache &cache);
									~WindowIconCache();

private:
			void					_Init();

public:
			int32					id;
			BBitmap*				icon;
			BBitmap*				iconCache[kWindowIconCacheCount];
};


class TBarApp : public BServer {
public:
									TBarApp();
	virtual							~TBarApp();

	virtual	bool					QuitRequested();
	virtual	void					MessageReceived(BMessage* message);
	virtual	void					RefsReceived(BMessage* refs);

			desk_settings*			Settings() { return &fSettings; }
			desk_settings*			DefaultSettings()
										{ return &fDefaultSettings; }
			clock_settings*			ClockSettings() { return &fClockSettings; }

			TBarView*				BarView() const { return fBarView; }
			TBarWindow*				BarWindow() const { return fBarWindow; }

	static	void					Subscribe(const BMessenger &subscriber,
										BList*);
	static	void					Unsubscribe(const BMessenger &subscriber);

			int32					TeamIconSize();

			BBitmap*				FetchTeamIcon(team_id team, int32 size);
			BBitmap*				FetchWaylandIcon(const char* signature,
										int32 size);
			BBitmap*				FetchWindowIcon(bool local = true,
										bool minimized = false);

	// Renga (batch 12): Wayland programs (Linux programs under Renguin and
	// native ones) as entries of their own. Their windows are windows of
	// the compositor's team; the compositor tells which client token
	// belongs to which program (headers/private/shared/wayland_clients.h).
	static	bool					WindowBelongsTo(const char* signature,
										const window_info* info);
	static	int32*					WindowTokensFor(const BList* teams,
										const char* signature, int32* _count);
	static	void					WaylandClientPids(const char* signature,
										BList* pids);
	static	BString					WaylandIconPath(const char* signature);
	static	team_id					CompositorTeam();

private:
			void					AddTeam(team_id team, uint32 flags,
										const char* sig, entry_ref*);
			void					RemoveTeam(team_id);
			void					_NotifyTeamAdded(BarTeamInfo* barInfo);
			void					_UpdateWaylandClients(BMessage* message);
			void					_RequestWaylandClients();
			void					_RemoveWaylandEntry(int32 index);

			void					InitSettings();
			void					SaveSettings();

			void					ShowPreferencesWindow();
			void					QuitPreferencesWindow();

			void					ResizeTeamIcons();
			void					_ShowSearchWindow(BMessage* message);
			void					_RestartInto(BMessage* message);
			bool					_IsHarness() const;
			status_t				_CacheTeamIcon(BarTeamInfo* barInfo);
			status_t				_CacheTeamIcon(BarTeamInfo* barInfo, int32 size);
			status_t				_CacheWindowIcon(WindowIconCache* winCache);

private:
			TBarWindow*				fBarWindow;
			TBarView*				fBarView;
			BMessenger				fSwitcherMessenger;
			BMessenger				fStatusViewMessenger;
			BFile*					fSettingsFile;
			BFile*					fClockSettingsFile;
			desk_settings			fSettings;
			desk_settings			fDefaultSettings;
			clock_settings			fClockSettings;
			PreferencesWindow*		fPreferencesWindow;
			TSearchWindow*			fSearchWindow;

	static	BLocker					sSubscriberLock;
	static	BList					sBarTeamInfoList;
	static	BList					sWindowIconCache;
	static	BList					sSubscribers;
};


#endif	// BAR_APP_H
