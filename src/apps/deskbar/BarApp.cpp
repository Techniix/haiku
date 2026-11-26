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


#include "BarApp.h"

#include <stdio.h>
#include <unistd.h>

#include <locale.h>
#include <strings.h>

#include <AppFileInfo.h>
#include <Autolock.h>
#include <Bitmap.h>
#include <Catalog.h>
#include <ControlLook.h>
#include <Debug.h>
#include <Directory.h>
#include <Dragger.h>
#include <File.h>
#include <FindDirectory.h>
#include <IconUtils.h>
#include <Locale.h>
#include <Message.h>
#include <Messenger.h>
#include <Mime.h>
#include <Node.h>
#include <NodeInfo.h>
#include <Path.h>
#include <Roster.h>
#include <WindowInfo.h>

#include <map>
#include <vector>

#include <DeskbarPrivate.h>
#include <boot_state_file.h>
#include <RosterPrivate.h>
#include <wayland_clients.h>
#include "tracker_private.h"

#include "BarView.h"
#include "DeskbarMenu.h"
#include "MenuSearch.h"
#include "SearchWindow.h"
#include "WindowMenu.h"
#include "BarWindow.h"
#include "DeskbarUtils.h"
#include "FSUtils.h"
#include "PreferencesWindow.h"
#include "ResourceSet.h"
#include "StatusView.h"
#include "Switcher.h"

#include "icons.h"


BLocker TBarApp::sSubscriberLock;
BList TBarApp::sBarTeamInfoList;


// Renga (batch 12): the Wayland compositor's clients (TBarApp::
// _UpdateWaylandClients()). sWaylandLock is held only briefly and never
// while sending a message, so window threads and the expando monitor
// thread can ask about windows at any time.
static BLocker sWaylandLock("wayland clients");
static team_id sCompositorTeam = -1;
static std::map<int32, BString> sWaylandTokens;
	// a window's client token -> its program's signature (lower case)
static std::multimap<BString, team_id> sWaylandPids;
	// a program's signature (lower case) -> its client processes
static std::map<BString, BString> sWaylandIconPaths;
	// a program's signature (lower case) -> its Deskbar menu entry
BList TBarApp::sWindowIconCache;
BList TBarApp::sSubscribers;


const uint32 kShowDeskbarMenu		= 'BeMn';
const uint32 kShowTeamMenu			= 'TmMn';


int
main()
{
	setlocale(LC_ALL, "");
	TBarApp app;
	app.Run();

	return B_OK;
}


TBarApp::TBarApp()
	:
	BServer(kDeskbarSignature, true, NULL),
	fSettingsFile(NULL),
	fClockSettingsFile(NULL),
	fPreferencesWindow(NULL),
	fSearchWindow(NULL)
{
	InitSettings();
	InitIconPreloader();

	fBarWindow = new TBarWindow();
	fBarView = fBarWindow->BarView();

	be_roster->StartWatching(this);

	gLocalizedNamePreferred
		= BLocaleRoster::Default()->IsFilesystemTranslationPreferred();

	sBarTeamInfoList.MakeEmpty();

	BList teamList;
	int32 numTeams;
	be_roster->GetAppList(&teamList);
	numTeams = teamList.CountItems();
	for (int32 i = 0; i < numTeams; i++) {
		app_info appInfo;
		team_id tID = (addr_t)teamList.ItemAt(i);
		if (be_roster->GetRunningAppInfo(tID, &appInfo) == B_OK) {
			if (strcasecmp(appInfo.signature,
					RENGA_WAYLAND_COMPOSITOR_SIGNATURE) == 0) {
				BAutolock lock(sWaylandLock);
				sCompositorTeam = appInfo.team;
			}
			AddTeam(appInfo.team, appInfo.flags, appInfo.signature,
				&appInfo.ref);
		}
	}
	if (sCompositorTeam >= 0)
		_RequestWaylandClients();

	sWindowIconCache.MakeEmpty();
	for (int32 id = R_WindowShownIcon; id <= R_WindowHiddenSwitchIcon; id++) {
		if (id == R_ResizeIcon)
			continue;

		WindowIconCache* winCache = new WindowIconCache(id);
		_CacheWindowIcon(winCache);
		sWindowIconCache.AddItem(winCache);
	}

	sSubscribers.MakeEmpty();
	fSwitcherMessenger = BMessenger(new TSwitchManager());
	fBarWindow->Show();

	fBarWindow->Lock();
	fBarView->UpdatePlacement();
	fBarWindow->Unlock();

	// this messenger now targets the barview instead of the
	// statusview so that all additions to the tray
	// follow the same path
	fStatusViewMessenger = BMessenger(fBarWindow->FindView("BarView"));
}


TBarApp::~TBarApp()
{
	be_roster->StopWatching(this);

	int32 teamCount = sBarTeamInfoList.CountItems();
	for (int32 i = 0; i < teamCount; i++) {
		BarTeamInfo* barInfo = (BarTeamInfo*)sBarTeamInfoList.ItemAt(i);
		delete barInfo;
	}

	int32 subsCount = sSubscribers.CountItems();
	for (int32 i = 0; i < subsCount; i++) {
		BMessenger* messenger
			= static_cast<BMessenger*>(sSubscribers.ItemAt(i));
		delete messenger;
	}

	SaveSettings();

	delete fSettingsFile;
	delete fClockSettingsFile;
}


bool
TBarApp::QuitRequested()
{
	// don't allow the user to quit
	if (CurrentMessage() && CurrentMessage()->FindBool("shortcut")) {
		// but close the preferences window
		QuitPreferencesWindow();
		return false;
	}

	// system quitting, call inherited to notify all loopers
	fBarWindow->SaveSettings();
	BApplication::QuitRequested();
	return true;
}


void
TBarApp::SaveSettings()
{
	if (fSettingsFile->InitCheck() == B_OK) {
		fSettingsFile->Seek(0, SEEK_SET);
		BMessage prefs;
		prefs.AddBool("vertical", fSettings.vertical);
		prefs.AddBool("left", fSettings.left);
		prefs.AddBool("top", fSettings.top);
		prefs.AddInt32("state", fSettings.state);
		prefs.AddFloat("width", fSettings.width);
		prefs.AddPoint("switcherLoc", fSettings.switcherLoc);
		prefs.AddBool("showClock", fSettings.showClock);
		// applications
		prefs.AddBool("trackerAlwaysFirst", fSettings.trackerAlwaysFirst);
		prefs.AddBool("sortRunningApps", fSettings.sortRunningApps);
		prefs.AddBool("superExpando", fSettings.superExpando);
		prefs.AddBool("expandNewTeams", fSettings.expandNewTeams);
		prefs.AddBool("hideLabels", fSettings.hideLabels);
		prefs.AddInt32("iconSize", fSettings.iconSize);
		// recent items
		prefs.AddBool("recentDocsEnabled", fSettings.recentDocsEnabled);
		prefs.AddBool("recentFoldersEnabled", fSettings.recentFoldersEnabled);
		prefs.AddBool("recentAppsEnabled", fSettings.recentAppsEnabled);
		prefs.AddInt32("recentDocsCount", fSettings.recentDocsCount);
		prefs.AddInt32("recentFoldersCount", fSettings.recentFoldersCount);
		prefs.AddInt32("recentAppsCount", fSettings.recentAppsCount);
		// window
		prefs.AddBool("alwaysOnTop", fSettings.alwaysOnTop);
		prefs.AddBool("autoRaise", fSettings.autoRaise);
		prefs.AddBool("autoHide", fSettings.autoHide);
		// Renga (F-41, batch 9)
		prefs.AddBool("showSearch", fSettings.showSearch);
		prefs.AddBool("searchCalculator", fSettings.searchCalculator);
		prefs.AddBool("searchFiles", fSettings.searchFiles);
		prefs.AddBool("showRestartInto", fSettings.showRestartInto);
		prefs.AddBool("appRecentDocuments", fSettings.appRecentDocuments);

		prefs.Flatten(fSettingsFile);
	}

	if (fClockSettingsFile->InitCheck() == B_OK) {
		fClockSettingsFile->Seek(0, SEEK_SET);
		BMessage prefs;
		prefs.AddBool("showSeconds", fClockSettings.showSeconds);
		prefs.AddBool("showDayOfWeek", fClockSettings.showDayOfWeek);
		prefs.AddBool("showTimeZone", fClockSettings.showTimeZone);

		prefs.Flatten(fClockSettingsFile);
	}
}


void
TBarApp::InitSettings()
{
	// compute metrics
	sIconGap = ceilf(be_control_look->DefaultLabelSpacing() / 3.0f);

	gDragRegionWidth = be_control_look->ComposeSpacing(B_USE_HALF_ITEM_SPACING);
	gDragWidth = ceilf(gDragRegionWidth * 0.6f);

	gMinReplicantHeight = gMinReplicantWidth =
		be_control_look->ComposeIconSize(B_MINI_ICON).IntegerWidth() + 1;

	// 1 pixel for left gutter
	// space for replicant tray (6 items)
	// 6 pixel drag region
	gMinimumTrayWidth = sIconGap + gMinReplicantWidth
		+ (kMinimumReplicantCount * sIconGap)
		+ (kMinimumReplicantCount * gMinReplicantWidth) + kGutter;

	gMinimumWindowWidth = kGutter + gMinimumTrayWidth + gDragRegionWidth;
	gMaximumWindowWidth = gMinimumWindowWidth * 2;

	// defaults
	desk_settings settings;
	settings.vertical = fDefaultSettings.vertical = true;
	settings.left = fDefaultSettings.left = false;
	settings.top = fDefaultSettings.top = true;
	settings.state = fDefaultSettings.state = kExpandoState;
	settings.width = fDefaultSettings.width = gMinimumWindowWidth;
	settings.switcherLoc = fDefaultSettings.switcherLoc = BPoint(5000, 5000);
	settings.showClock = fDefaultSettings.showClock = true;
	// applications
	settings.trackerAlwaysFirst = fDefaultSettings.trackerAlwaysFirst = true;
	settings.sortRunningApps = fDefaultSettings.sortRunningApps = false;
	settings.superExpando = fDefaultSettings.superExpando = false;
	settings.expandNewTeams = fDefaultSettings.expandNewTeams = false;
	settings.hideLabels = fDefaultSettings.hideLabels = false;
	settings.iconSize = fDefaultSettings.iconSize = kMinimumIconSize;
	// recent items
	settings.recentDocsEnabled = fDefaultSettings.recentDocsEnabled = true;
	settings.recentFoldersEnabled
		= fDefaultSettings.recentFoldersEnabled = true;
	settings.recentAppsEnabled = fDefaultSettings.recentAppsEnabled = true;
	settings.recentDocsCount = fDefaultSettings.recentDocsCount = 10;
	settings.recentFoldersCount = fDefaultSettings.recentFoldersCount = 10;
	settings.recentAppsCount = fDefaultSettings.recentAppsCount = 10;
	// window
	settings.alwaysOnTop = fDefaultSettings.alwaysOnTop = false;
	settings.autoRaise = fDefaultSettings.autoRaise = false;
	settings.autoHide = fDefaultSettings.autoHide = false;
	// Renga (F-41, batch 9)
	settings.showSearch = fDefaultSettings.showSearch = true;
	settings.searchCalculator = fDefaultSettings.searchCalculator = true;
	settings.searchFiles = fDefaultSettings.searchFiles = true;
	settings.showRestartInto = fDefaultSettings.showRestartInto = true;
	settings.appRecentDocuments = fDefaultSettings.appRecentDocuments = true;

	clock_settings clock;
	clock.showSeconds = false;
	clock.showDayOfWeek = false;
	clock.showTimeZone = false;

	BPath dirPath;
	const char* settingsFileName = "settings";
	const char* clockSettingsFileName = "clock_settings";

	find_directory(B_USER_DESKBAR_DIRECTORY, &dirPath, true);
		// just make it

	if (GetDeskbarSettingsDirectory(dirPath, true) == B_OK) {
		BPath filePath = dirPath;
		filePath.Append(settingsFileName);
		fSettingsFile = new BFile(filePath.Path(), O_RDWR);
		if (fSettingsFile->InitCheck() != B_OK) {
			BDirectory theDir(dirPath.Path());
			if (theDir.InitCheck() == B_OK)
				theDir.CreateFile(settingsFileName, fSettingsFile);
		}

		BMessage prefs;
		if (fSettingsFile->InitCheck() == B_OK
			&& prefs.Unflatten(fSettingsFile) == B_OK) {
			settings.vertical = prefs.GetBool("vertical",
				fDefaultSettings.vertical);
			settings.left = prefs.GetBool("left",
				fDefaultSettings.left);
			settings.top = prefs.GetBool("top",
				fDefaultSettings.top);
			settings.state = prefs.GetInt32("state",
				fDefaultSettings.state);
			settings.width = prefs.GetFloat("width",
				fDefaultSettings.width);
			settings.switcherLoc = prefs.GetPoint("switcherLoc",
				fDefaultSettings.switcherLoc);
			settings.showClock = prefs.GetBool("showClock",
				fDefaultSettings.showClock);
			// applications
			settings.trackerAlwaysFirst = prefs.GetBool("trackerAlwaysFirst",
				fDefaultSettings.trackerAlwaysFirst);
			settings.sortRunningApps = prefs.GetBool("sortRunningApps",
				fDefaultSettings.sortRunningApps);
			settings.superExpando = prefs.GetBool("superExpando",
				fDefaultSettings.superExpando);
			settings.expandNewTeams = prefs.GetBool("expandNewTeams",
				fDefaultSettings.expandNewTeams);
			settings.hideLabels = prefs.GetBool("hideLabels",
				fDefaultSettings.hideLabels);
			settings.iconSize = prefs.GetInt32("iconSize",
				fDefaultSettings.iconSize);
			// recent items
			settings.recentDocsEnabled = prefs.GetBool("recentDocsEnabled",
				fDefaultSettings.recentDocsEnabled);
			settings.recentFoldersEnabled
				= prefs.GetBool("recentFoldersEnabled",
					fDefaultSettings.recentFoldersEnabled);
			settings.recentAppsEnabled = prefs.GetBool("recentAppsEnabled",
				fDefaultSettings.recentAppsEnabled);
			settings.recentDocsCount = prefs.GetInt32("recentDocsCount",
				fDefaultSettings.recentDocsCount);
			settings.recentFoldersCount = prefs.GetInt32("recentFoldersCount",
				fDefaultSettings.recentFoldersCount);
			settings.recentAppsCount = prefs.GetInt32("recentAppsCount",
				fDefaultSettings.recentAppsCount);
			// window
			settings.alwaysOnTop = prefs.GetBool("alwaysOnTop",
				fDefaultSettings.alwaysOnTop);
			settings.autoRaise = prefs.GetBool("autoRaise",
				fDefaultSettings.autoRaise);
			settings.autoHide = prefs.GetBool("autoHide",
				fDefaultSettings.autoHide);
			// Renga (F-41, batch 9)
			settings.showSearch = prefs.GetBool("showSearch",
				fDefaultSettings.showSearch);
			settings.searchCalculator = prefs.GetBool("searchCalculator",
				fDefaultSettings.searchCalculator);
			settings.searchFiles = prefs.GetBool("searchFiles",
				fDefaultSettings.searchFiles);
			settings.showRestartInto = prefs.GetBool("showRestartInto",
				fDefaultSettings.showRestartInto);
			settings.appRecentDocuments = prefs.GetBool("appRecentDocuments",
				fDefaultSettings.appRecentDocuments);
		}

		// constrain width setting within limits
		if (settings.width < gMinimumWindowWidth)
			settings.width = gMinimumWindowWidth;
		else if (settings.width > gMaximumWindowWidth)
			settings.width = gMaximumWindowWidth;

		filePath = dirPath;
		filePath.Append(clockSettingsFileName);
		fClockSettingsFile = new BFile(filePath.Path(), O_RDWR);
		if (fClockSettingsFile->InitCheck() != B_OK) {
			BDirectory theDir(dirPath.Path());
			if (theDir.InitCheck() == B_OK)
				theDir.CreateFile(clockSettingsFileName, fClockSettingsFile);
		}

		if (fClockSettingsFile->InitCheck() == B_OK
			&& prefs.Unflatten(fClockSettingsFile) == B_OK) {
			clock.showSeconds = prefs.GetBool("showSeconds", false);
			clock.showDayOfWeek = prefs.GetBool("showDayOfWeek", false);
			clock.showTimeZone = prefs.GetBool("showTimeZone", false);
		}
	}

	fSettings = settings;
	fClockSettings = clock;
}


void
TBarApp::MessageReceived(BMessage* message)
{
	switch (message->what) {
		// BDeskbar originating messages we can handle
		case kMsgIsAlwaysOnTop:
		{
			BMessage reply('rply');
			reply.AddBool("always on top", fSettings.alwaysOnTop);
			message->SendReply(&reply);
			break;
		}
		case kMsgIsAutoRaise:
		{
			BMessage reply('rply');
			reply.AddBool("auto raise", fSettings.autoRaise);
			message->SendReply(&reply);
			break;
		}
		case kMsgIsAutoHide:
		{
			BMessage reply('rply');
			reply.AddBool("auto hide", fSettings.autoHide);
			message->SendReply(&reply);
			break;
		}

		// pass rest of BDeskbar originating messages onto the window
		// (except for setters handled below)
		case kMsgLocation:
		case kMsgSetLocation:
		case kMsgIsExpanded:
		case kMsgExpand:
		case kMsgGetItemInfo:
		case kMsgHasItem:
		case kMsgCountItems:
		case kMsgMaxItemSize:
		case kMsgAddView:
		case kMsgRemoveItem:
		case kMsgAddAddOn:
			fBarWindow->PostMessage(message);
			break;

		case kConfigShow:
			ShowPreferencesWindow();
			break;

		case kConfigQuit:
			QuitPreferencesWindow();
			break;

		case kStateChanged:
			if (fPreferencesWindow != NULL)
				fPreferencesWindow->PostMessage(message);
			break;

		case kShowDeskbarMenu:
			if (fBarWindow->Lock()) {
				fBarWindow->ShowDeskbarMenu();
				fBarWindow->Unlock();
			}
			break;

		case kShowTeamMenu:
			if (fBarWindow->Lock()) {
				fBarWindow->ShowTeamMenu();
				fBarWindow->Unlock();
			}
			break;

		case kUpdateRecentCounts:
			int32 count;
			bool enabled;

			if (message->FindInt32("applications", &count) == B_OK)
				fSettings.recentAppsCount = count;
			if (message->FindBool("applicationsEnabled", &enabled) == B_OK)
				fSettings.recentAppsEnabled = enabled && count > 0;

			if (message->FindInt32("folders", &count) == B_OK)
				fSettings.recentFoldersCount = count;
			if (message->FindBool("foldersEnabled", &enabled) == B_OK)
				fSettings.recentFoldersEnabled = enabled && count > 0;

			if (message->FindInt32("documents", &count) == B_OK)
				fSettings.recentDocsCount = count;
			if (message->FindBool("documentsEnabled", &enabled) == B_OK)
				fSettings.recentDocsEnabled = enabled && count > 0;

			if (fPreferencesWindow != NULL)
				fPreferencesWindow->PostMessage(kUpdatePreferences);
			break;

		case B_SOME_APP_LAUNCHED:
		{
			team_id team = -1;
			message->FindInt32("be:team", &team);

			uint32 flags = 0;
			message->FindInt32("be:flags", (int32*)&flags);

			const char* signature = NULL;
			message->FindString("be:signature", &signature);

			entry_ref ref;
			message->FindRef("be:ref", &ref);

			if (signature != NULL && strcasecmp(signature,
					RENGA_WAYLAND_COMPOSITOR_SIGNATURE) == 0) {
				// Renga (batch 12): the compositor (re)started
				{
					BAutolock lock(sWaylandLock);
					sCompositorTeam = team;
				}
				_RequestWaylandClients();
			}

			AddTeam(team, flags, signature, &ref);
			break;
		}

		case B_SOME_APP_QUIT:
		{
			team_id team = -1;
			message->FindInt32("be:team", &team);
			RemoveTeam(team);
			break;
		}

		case kMsgWaylandClients:
			// Renga (batch 12): the compositor's reply or its update
			_UpdateWaylandClients(message);
			break;

		case kTestWaylandClients:
			// harness images only: the Wayland programs' entries, with the
			// windows each lists (as its window menu does)
			if (_IsHarness()) {
				BMessage reply(B_REPLY);
				reply.AddInt32("compositor", CompositorTeam());
				BAutolock autolock(sSubscriberLock);
				for (int32 i = 0; i < sBarTeamInfoList.CountItems(); i++) {
					BarTeamInfo* barInfo
						= (BarTeamInfo*)sBarTeamInfoList.ItemAt(i);
					if (!barInfo->waylandClient)
						continue;
					BMessage entry;
					entry.AddString("name", barInfo->name);
					entry.AddString("sig", barInfo->sig);
					entry.AddString("icon_path", barInfo->iconPath);
					entry.AddBool("icon", barInfo->icon != NULL);
					int32 count = 0;
					int32* tokens = WindowTokensFor(barInfo->teams,
						barInfo->sig, &count);
					for (int32 j = 0; j < count; j++) {
						client_window_info* info = get_window_info(tokens[j]);
						if (info == NULL)
							continue;
						if (TWindowMenu::WindowShouldBeListed(info))
							entry.AddString("window", info->name);
						free(info);
					}
					free(tokens);
					reply.AddMessage("entry", &entry);
				}
				message->SendReply(&reply);
			}
			break;

		case B_ARCHIVED_OBJECT:
			// TODO: what's this???
			message->AddString("special", "Alex Osadzinski");
			fStatusViewMessenger.SendMessage(message);
			break;

		case kToggleDraggers:
			if (BDragger::AreDraggersDrawn())
				BDragger::HideAllDraggers();
			else
				BDragger::ShowAllDraggers();
			break;

		case kMsgAlwaysOnTop: // from BDeskbar
		case kAlwaysTop:
			fSettings.alwaysOnTop = !fSettings.alwaysOnTop;

			if (fPreferencesWindow != NULL)
				fPreferencesWindow->PostMessage(kUpdatePreferences);

			fBarWindow->SetFeel(fSettings.alwaysOnTop
				? B_FLOATING_ALL_WINDOW_FEEL
				: B_NORMAL_WINDOW_FEEL);
			break;

		case kMsgAutoRaise: // from BDeskbar
		case kAutoRaise:
			fSettings.autoRaise = fSettings.alwaysOnTop ? false
				: !fSettings.autoRaise;

			if (fPreferencesWindow != NULL)
				fPreferencesWindow->PostMessage(kUpdatePreferences);
			break;

		case kMsgAutoHide: // from BDeskbar
		case kAutoHide:
			fSettings.autoHide = !fSettings.autoHide;

			if (fPreferencesWindow != NULL)
				fPreferencesWindow->PostMessage(kUpdatePreferences);

			fBarWindow->Lock();
			fBarView->HideDeskbar(fSettings.autoHide);
			fBarWindow->Unlock();
			break;

		case kTrackerFirst:
			fSettings.trackerAlwaysFirst = !fSettings.trackerAlwaysFirst;

			if (fPreferencesWindow != NULL)
				fPreferencesWindow->PostMessage(kUpdatePreferences);

			// if mini mode we don't need to update the view
			if (fBarView->MiniState())
				break;

			fBarWindow->Lock();
			fBarView->PlaceApplicationBar();
			fBarWindow->Unlock();
			break;

		case kSortRunningApps:
			fSettings.sortRunningApps = !fSettings.sortRunningApps;

			if (fPreferencesWindow != NULL)
				fPreferencesWindow->PostMessage(kUpdatePreferences);

			// if mini mode we don't need to update the view
			if (fBarView->MiniState())
				break;

			fBarWindow->Lock();
			fBarView->PlaceApplicationBar();
			fBarWindow->Unlock();
			break;

		case kUnsubscribe:
		{
			BMessenger messenger;
			if (message->FindMessenger("messenger", &messenger) == B_OK)
				Unsubscribe(messenger);
			break;
		}

		case kSuperExpando:
			fSettings.superExpando = !fSettings.superExpando;

			if (fPreferencesWindow != NULL)
				fPreferencesWindow->PostMessage(kUpdatePreferences);

			// if mini mode we don't need to update the view
			if (fBarView->MiniState())
				break;

			fBarWindow->Lock();
			fBarView->PlaceApplicationBar();
			fBarWindow->Unlock();
			break;

		case kExpandNewTeams:
			fSettings.expandNewTeams = !fSettings.expandNewTeams;

			if (fPreferencesWindow != NULL)
				fPreferencesWindow->PostMessage(kUpdatePreferences);

			// if mini mode we don't need to update the view
			if (fBarView->MiniState())
				break;

			fBarWindow->Lock();
			fBarView->PlaceApplicationBar();
			fBarWindow->Unlock();
			break;

		case kHideLabels:
			fSettings.hideLabels = !fSettings.hideLabels;

			if (fPreferencesWindow != NULL)
				fPreferencesWindow->PostMessage(kUpdatePreferences);

			// if mini mode we don't need to update the view
			if (fBarView->MiniState())
				break;

			fBarWindow->Lock();
			fBarView->PlaceApplicationBar();
			fBarWindow->Unlock();
			break;

		case kResizeTeamIcons:
		{
			int32 oldIconSize = fSettings.iconSize;
			int32 value;
			if (message->FindInt32("be:value", &value) != B_OK)
				break;

			int32 iconSize = value * kIconSizeInterval;
			fSettings.iconSize = iconSize;

			// pin icon size between min and max values
			if (fSettings.iconSize < kMinimumIconSize)
				fSettings.iconSize = kMinimumIconSize;
			else if (fSettings.iconSize > kMaximumIconSize)
				fSettings.iconSize = kMaximumIconSize;

			// don't resize if icon size hasn't changed
			if (fSettings.iconSize == oldIconSize)
				break;

			ResizeTeamIcons();

			if (fPreferencesWindow != NULL)
				fPreferencesWindow->PostMessage(kUpdatePreferences);

			// if mini mode we don't need to update the view
			if (fBarView->MiniState())
				break;

			fBarWindow->Lock();
			if (!fBarView->Vertical()) {
				// Must also resize the Deskbar menu and replicant tray in
				// horizontal mode
				fBarView->PlaceDeskbarMenu();
				fBarView->PlaceTray(false, false);
			}
			fBarView->PlaceApplicationBar();
			fBarWindow->Unlock();
			break;
		}

		case 'TASK':
			fSwitcherMessenger.SendMessage(message);
			break;

		case kSuspendSystem:
			// TODO: Call BRoster?
			break;

		case kRebootSystem:
		case kShutdownSystem:
		{
			bool reboot = (message->what == kRebootSystem);
			bool confirm;
			message->FindBool("confirm", &confirm);

			BRoster roster;
			BRoster::Private rosterPrivate(roster);
			status_t error = rosterPrivate.ShutDown(reboot, confirm, false);
			if (error != B_OK)
				fprintf(stderr, "Shutdown failed: %s\n", strerror(error));

			break;
		}

		case kShowSplash:
			run_be_about();
			break;

		// Renga (F-41, batch 9)
		case kShowSearchWindow:
			_ShowSearchWindow(message);
			break;

		case kToggleSearch:
		case kToggleSearchCalculator:
		case kToggleSearchFiles:
		case kToggleRestartInto:
		case kToggleAppRecentDocuments:
		{
			bool* setting = message->what == kToggleSearch
				? &fSettings.showSearch
				: message->what == kToggleSearchCalculator
					? &fSettings.searchCalculator
				: message->what == kToggleSearchFiles
					? &fSettings.searchFiles
				: message->what == kToggleRestartInto
					? &fSettings.showRestartInto
				: &fSettings.appRecentDocuments;
			int32 value;
			if (message->FindInt32("be:value", &value) == B_OK)
				*setting = value != B_CONTROL_OFF;
			else
				*setting = !*setting;
			if (fSearchWindow != NULL && fSearchWindow->Lock()) {
				fSearchWindow->SetCalculatorEnabled(
					fSettings.searchCalculator);
				fSearchWindow->SetFilesEnabled(fSettings.searchFiles);
				if (!fSettings.showSearch && !fSearchWindow->IsHidden())
					fSearchWindow->Hide();
				fSearchWindow->Unlock();
			}
			if (fPreferencesWindow != NULL)
				fPreferencesWindow->PostMessage(kUpdatePreferences);
			SaveSettings();
			break;
		}

		case kRestartInto:
			_RestartInto(message);
			break;

		case kTestDeskbarMenu:
			// harness images only: builds the Deskbar menu as opening it
			// does and replies with the time and the entries
			if (_IsHarness()) {
				BMessage reply(B_REPLY);
				TDeskbarMenu::BuildForTest(fBarView, &reply);
				message->SendReply(&reply);
			}
			break;

		case kTestSearch:
			// harness images only: what the search finds for "query",
			// files included (when switched on), and how long it took
			if (_IsHarness()) {
				BMessage reply(B_REPLY);
				MenuSearch search;
				search.SetCalculatorEnabled(fSettings.searchCalculator);
				SearchResultList results(20);
				bigtime_t start = system_time();
				search.SearchQuick(message->GetString("query", ""), results);
				bigtime_t quick = system_time() - start;
				int32 quickCount = results.CountItems();
				start = system_time();
				if (fSettings.searchFiles) {
					MenuSearch::SearchFiles(message->GetString("query", ""),
						20, results);
				}
				reply.AddInt64("quick_time", quick);
				reply.AddInt64("file_time", system_time() - start);
				reply.AddInt32("quick_count", quickCount);
				reply.AddInt32("applications", search.CountApplications());
				for (int32 i = 0; i < results.CountItems(); i++) {
					SearchResult* result = results.ItemAt(i);
					reply.AddString("title", result->title);
					reply.AddString("detail", result->detail);
					reply.AddInt32("kind", result->kind);
					reply.AddString("value", result->value);
				}
				message->SendReply(&reply);
			}
			break;

		case kTestSearchWindow:
			// harness images only: "show" (with "text"), "results", "open"
			// (the selected result, as Return does) or "hide"
			if (_IsHarness()) {
				BMessage reply(B_REPLY);
				BString action = message->GetString("action", "");
				if (action == "show") {
					BMessage show(kShowSearchWindow);
					show.AddString("text", message->GetString("text", ""));
					show.AddBool("test", true);
					_ShowSearchWindow(&show);
				}
				reply.AddBool("exists", fSearchWindow != NULL);
				if (fSearchWindow != NULL && fSearchWindow->Lock()) {
					if (action == "hide")
						fSearchWindow->Hide();
					else if (action == "open")
						fSearchWindow->PostMessage('DSop');
					fSearchWindow->GetResults(reply);
					reply.AddRect("frame", fSearchWindow->Frame());
					fSearchWindow->Unlock();
				}
				message->SendReply(&reply);
			}
			break;

		case kTestAppRecentDocuments:
			// harness images only: the "Recent documents" submenu that an
			// application's window menu gets, built, for "signature"
			if (_IsHarness()) {
				BMessage reply(B_REPLY);
				BMenu* menu = TWindowMenu::RecentDocumentsMenu(
					message->GetString("signature", ""));
				reply.AddBool("menu", menu != NULL);
				if (TRecentsMenu* recents
						= dynamic_cast<TRecentsMenu*>(menu)) {
					recents->BuildForTest();
					for (int32 i = 0; i < recents->CountItems(); i++)
						reply.AddString("item", recents->ItemAt(i)->Label());
				}
				delete menu;
				message->SendReply(&reply);
			}
			break;

		case B_LOCALE_CHANGED:
		{
			BLocaleRoster::Default()->Refresh();

			bool localize;
			if (message->FindBool("filesys", &localize) == B_OK)
				gLocalizedNamePreferred = localize;
		}
		// fall-through

		case kRealignReplicants:
		case kShowHideTime:
		case kShowSeconds:
		case kShowDayOfWeek:
		case kShowTimeZone:
		case kGetClockSettings:
			fStatusViewMessenger.SendMessage(message);
				// Notify the replicant tray (through BarView) that the time
				// interval has changed and it should update the time view
				// and reflow the tray icons.
			break;

		default:
			BApplication::MessageReceived(message);
			break;
	}
}


void
TBarApp::RefsReceived(BMessage* refs)
{
	entry_ref ref;
	for (int32 i = 0; refs->FindRef("refs", i, &ref) == B_OK; i++) {
		BMessage refsReceived(B_REFS_RECEIVED);
		refsReceived.AddRef("refs", &ref);

		BEntry entry(&ref);
		if (!entry.IsDirectory())
			TrackerLaunch(&refsReceived, false);
	}
}


void
TBarApp::Subscribe(const BMessenger &subscriber, BList* list)
{
	// called when ExpandoMenuBar, TeamMenu or Switcher are built/rebuilt
	list->MakeEmpty();

	BAutolock autolock(sSubscriberLock);
	if (!autolock.IsLocked())
		return;

	int32 numTeams = sBarTeamInfoList.CountItems();
	for (int32 i = 0; i < numTeams; i++) {
		BarTeamInfo* barInfo = (BarTeamInfo*)sBarTeamInfoList.ItemAt(i);
		BarTeamInfo* newBarInfo = new(std::nothrow) BarTeamInfo(*barInfo);
		if (newBarInfo != NULL)
			list->AddItem(newBarInfo);
	}

	int32 subsCount = sSubscribers.CountItems();
	for (int32 i = 0; i < subsCount; i++) {
		BMessenger* messenger = (BMessenger*)sSubscribers.ItemAt(i);
		if (*messenger == subscriber)
			return;
	}

	sSubscribers.AddItem(new BMessenger(subscriber));
}


void
TBarApp::Unsubscribe(const BMessenger &subscriber)
{
	BAutolock autolock(sSubscriberLock);
	if (!autolock.IsLocked())
		return;

	int32 count = sSubscribers.CountItems();
	for (int32 i = 0; i < count; i++) {
		BMessenger* messenger = (BMessenger*)sSubscribers.ItemAt(i);
		if (*messenger == subscriber) {
			sSubscribers.RemoveItem(i);
			delete messenger;
			break;
		}
	}
}


BBitmap*
TBarApp::FetchTeamIcon(team_id team, int32 size)
{
	BAutolock autolock(sSubscriberLock);
	if (!autolock.IsLocked())
		return NULL;

	int32 teamCount = sBarTeamInfoList.CountItems();
	for (int32 i = 0; i < teamCount; i++) {
		BarTeamInfo* barInfo = (BarTeamInfo*)sBarTeamInfoList.ItemAt(i);
		if (barInfo->waylandClient)
			continue;
		if (barInfo->teams->HasItem((void*)(addr_t)team)) {
			_CacheTeamIcon(barInfo, size);
			BBitmap* icon = barInfo->icon;

			// restore icon pointer back to setting
			if (size != fSettings.iconSize) {
				int32 index = (fSettings.iconSize - kMinimumIconSize)
					/ kIconSizeInterval;
				barInfo->icon = barInfo->iconCache[index];
			}

			return icon;
		}
	}

	return NULL;
}


BBitmap*
TBarApp::FetchWindowIcon(bool local, bool minimized)
{
	int32 id = R_WindowShownIcon;
	int32 index = 0;

	if (local) {
		if (!minimized) {
			id = R_WindowShownIcon;
			index = 0;
		}
		else {
			id = R_WindowHiddenIcon;
			index = 1;
		}
	} else {
		if (!minimized) {
			id = R_WindowShownSwitchIcon;
			index = 2;
		}
		else {
			id = R_WindowHiddenSwitchIcon;
			index = 3;
		}
	}

	// create a new cache entry if not found
	WindowIconCache* winCache
		= (WindowIconCache*)sWindowIconCache.ItemAt(index);
	if (winCache == NULL)
		winCache = new WindowIconCache(id);

	_CacheWindowIcon(winCache);

	return winCache->icon;
}


void
TBarApp::AddTeam(team_id team, uint32 flags, const char* sig, entry_ref* ref)
{
	if ((flags & B_BACKGROUND_APP) != 0
		|| strcasecmp(sig, kDeskbarSignature) == 0) {
		// don't add if a background app or Deskbar itself
		return;
	}

	BAutolock autolock(sSubscriberLock);
	if (!autolock.IsLocked())
		return;

	// have we already seen this team, is this another instance of
	// a known app?
	BarTeamInfo* multiLaunchTeam = NULL;
	int32 teamCount = sBarTeamInfoList.CountItems();
	for (int32 i = 0; i < teamCount; i++) {
		BarTeamInfo* barInfo = (BarTeamInfo*)sBarTeamInfoList.ItemAt(i);
		if (barInfo->waylandClient)
			continue;
		if (barInfo->teams->HasItem((void*)(addr_t)team))
			return;
		if (strcasecmp(barInfo->sig, sig) == 0)
			multiLaunchTeam = barInfo;
	}

	if (multiLaunchTeam != NULL) {
		multiLaunchTeam->teams->AddItem((void*)(addr_t)team);

		int32 subsCount = sSubscribers.CountItems();
		if (subsCount > 0) {
			BMessage message(kAddTeam);
			message.AddInt32("team", team);
			message.AddString("sig", multiLaunchTeam->sig);

			for (int32 i = 0; i < subsCount; i++)
				((BMessenger*)sSubscribers.ItemAt(i))->SendMessage(&message);
		}
		return;
	}

	BString name;
	if (!gLocalizedNamePreferred
		|| BLocaleRoster::Default()->GetLocalizedFileName(name, *ref)
			!= B_OK) {
		name = ref->name;
	}

	BarTeamInfo* barInfo = new BarTeamInfo(new BList(), flags, strdup(sig),
		strdup(name.String()));
	_CacheTeamIcon(barInfo);
	barInfo->teams->AddItem((void*)(addr_t)team);
	sBarTeamInfoList.AddItem(barInfo);

	_NotifyTeamAdded(barInfo);
}


//!	Tells the subscribers about a new entry (sSubscriberLock held).
void
TBarApp::_NotifyTeamAdded(BarTeamInfo* barInfo)
{
	int32 subsCount = sSubscribers.CountItems();
	for (int32 i = 0; i < subsCount; i++) {
		BMessenger* messenger = (BMessenger*)sSubscribers.ItemAt(i);
		BMessage message(B_SOME_APP_LAUNCHED);

		BList* tList = new BList(*(barInfo->teams));
		message.AddPointer("teams", tList);

		BBitmap* icon = new BBitmap(barInfo->icon);
		ASSERT(icon);

		message.AddPointer("icon", icon);

		message.AddInt32("flags", static_cast<int32>(barInfo->flags));
		message.AddString("name", barInfo->name);
		message.AddString("sig", barInfo->sig);
		if (barInfo->waylandClient)
			message.AddBool("wayland", true);

		messenger->SendMessage(&message);
	}
}


void
TBarApp::RemoveTeam(team_id team)
{
	BAutolock autolock(sSubscriberLock);
	if (!autolock.IsLocked())
		return;

	if (team >= 0 && team == CompositorTeam()) {
		// Renga (batch 12): the compositor quit, its programs with it
		BMessage none(kMsgWaylandClients);
		none.AddInt32("team", team);
		_UpdateWaylandClients(&none);
		BAutolock lock(sWaylandLock);
		sCompositorTeam = -1;
		return;
	}

	int32 teamCount = sBarTeamInfoList.CountItems();
	for (int32 i = 0; i < teamCount; i++) {
		BarTeamInfo* barInfo = (BarTeamInfo*)sBarTeamInfoList.ItemAt(i);
		if (barInfo->waylandClient)
			continue;
		if (barInfo->teams->HasItem((void*)(addr_t)team)) {
			int32 subsCount = sSubscribers.CountItems();
			if (subsCount > 0) {
				BMessage message((barInfo->teams->CountItems() == 1)
					? B_SOME_APP_QUIT : kRemoveTeam);

				message.AddInt32("team", team);
				for (int32 i = 0; i < subsCount; i++) {
					BMessenger* messenger = (BMessenger*)sSubscribers.ItemAt(i);
					messenger->SendMessage(&message);
				}
			}

			barInfo->teams->RemoveItem((void*)(addr_t)team);
			if (barInfo->teams->CountItems() < 1) {
				delete (BarTeamInfo*)sBarTeamInfoList.RemoveItem(i);
				return;
			}
		}
	}
}


void
TBarApp::ResizeTeamIcons()
{
	BAutolock autolock(sSubscriberLock);
	if (!autolock.IsLocked())
		return;

	for (int32 i = sBarTeamInfoList.CountItems() - 1; i >= 0; i--) {
		BarTeamInfo* barInfo = (BarTeamInfo*)sBarTeamInfoList.ItemAt(i);
		if ((barInfo->flags & B_BACKGROUND_APP) == 0
			&& strcasecmp(barInfo->sig, kDeskbarSignature) != 0) {
			_CacheTeamIcon(barInfo);
		}
	}
}


int32
TBarApp::TeamIconSize()
{
	static int32 iconSize = 0, composed = 0;
	int32 saved = fSettings.iconSize;
	if (iconSize != saved) {
		composed = be_control_look->ComposeIconSize(saved).IntegerWidth() + 1;
		iconSize = saved;
	}

	return composed;
}


void
TBarApp::ShowPreferencesWindow()
{
	if (fPreferencesWindow == NULL) {
		fPreferencesWindow = new PreferencesWindow(BRect(100, 100, 320, 240));
		fPreferencesWindow->Show();
	} else if (fPreferencesWindow->Lock()) {
		if (fPreferencesWindow->IsHidden())
			fPreferencesWindow->Show();
		else
			fPreferencesWindow->Activate();

		fPreferencesWindow->Unlock();
	}
}


bool
TBarApp::_IsHarness() const
{
	return access("/boot/system/non-packaged/data/ce-harness", F_OK) == 0;
}


/*!	The Deskbar menu's "Search..." (F-41, batch 9): the search window next
	to the Deskbar, created on first use. "text" fills the field.
*/
void
TBarApp::_ShowSearchWindow(BMessage* message)
{
	if (!fSettings.showSearch && !message->GetBool("test", false))
		return;
	if (fSearchWindow == NULL)
		fSearchWindow = new TSearchWindow();
	BRect deskbarFrame;
	if (fBarWindow->Lock()) {
		deskbarFrame = fBarWindow->Frame();
		fBarWindow->Unlock();
	}
	if (fSearchWindow->Lock()) {
		fSearchWindow->SetCalculatorEnabled(fSettings.searchCalculator);
		fSearchWindow->SetFilesEnabled(fSettings.searchFiles);
		fSearchWindow->ShowNear(deskbarFrame, fSettings.vertical,
			message->GetString("text", NULL),
			message->GetBool("test", false));
		fSearchWindow->Unlock();
	}
}


/*!	"Restart into" in the Shutdown menu (boot loader A): the boot state's
	one-shot choice for the next start ("menu" shows the boot menu,
	"state:<directory>" a restore point), as the Boot menu preferences set
	it (src/preferences/bootmenu/BootMenu.cpp), then a restart. The boot
	loader clears the choice when it uses it.
*/
void
TBarApp::_RestartInto(BMessage* message)
{
	const char* next = message->GetString("next", "menu");
	char path[B_PATH_NAME_LENGTH];
	BootState::file_path(path, sizeof(path));
	BootState::Record record;
	int slot = BootState::read_file(path, record);
	status_t status = B_ENTRY_NOT_FOUND;
	if (slot != -2) {
		BootState::set_name(record.nextOnce, sizeof(record.nextOnce), next);
		status = BootState::write_file(path, record, slot);
	}
	fprintf(stderr, "Deskbar: restart into \"%s\": %s\n", next,
		strerror(status));

	if (message->GetBool("test", false) && _IsHarness()) {
		BMessage reply(B_REPLY);
		reply.AddInt32("status", status);
		message->SendReply(&reply);
		return;
	}
	if (status != B_OK)
		return;
	BRoster roster;
	BRoster::Private(roster).ShutDown(true, false, false);
}


void
TBarApp::QuitPreferencesWindow()
{
	if (fPreferencesWindow == NULL)
		return;

	if (fPreferencesWindow->Lock()) {
		fPreferencesWindow->Quit();
			// Quit() destroys the window so don't unlock
		fPreferencesWindow = NULL;
	}
}


status_t
TBarApp::_CacheTeamIcon(BarTeamInfo* barInfo)
{
	if (barInfo == NULL)
		return B_BAD_VALUE;

	return _CacheTeamIcon(barInfo, fSettings.iconSize);
}


status_t
TBarApp::_CacheTeamIcon(BarTeamInfo* barInfo, int32 size)
{
	if (barInfo == NULL || size < kMinimumIconSize)
		return B_BAD_VALUE;

	// icon index based on icon size
	const int32 index = (size - kMinimumIconSize) / kIconSizeInterval;

	// first look in the icon cache
	barInfo->icon = barInfo->iconCache[index];
	if (barInfo->icon != NULL)
		return B_OK;

	int32 composed = be_control_look->ComposeIconSize(size).IntegerWidth() + 1;
	BRect iconRect = BRect(0, 0, composed - 1, composed - 1);
	BBitmap* icon = new BBitmap(iconRect, B_RGBA32);

	if (barInfo->waylandClient) {
		// Renga (batch 12): a Wayland program: the icon of its Deskbar
		// menu entry (linux-apps), else of its signature, else the
		// compositor's
		entry_ref ref;
		if (barInfo->iconPath.Length() > 0
			&& get_ref_for_path(barInfo->iconPath.String(), &ref) == B_OK
			&& BNodeInfo::GetTrackerIcon(&ref, icon, (icon_size)size)
				== B_OK) {
			barInfo->iconCache[index] = barInfo->icon = icon;
			return B_OK;
		}
		BMimeType type(barInfo->sig);
		if (type.GetIcon(icon, (icon_size)size) == B_OK) {
			barInfo->iconCache[index] = barInfo->icon = icon;
			return B_OK;
		}
	}

	// icon wasn't in cache, get it from be_roster and cache it
	app_info appInfo;
	if (be_roster->GetAppInfo(barInfo->waylandClient
			? RENGA_WAYLAND_COMPOSITOR_SIGNATURE : barInfo->sig,
			&appInfo) == B_OK) {
		// fetch the app icon
		BFile file(&appInfo.ref, B_READ_ONLY);
		BAppFileInfo appMime(&file);
		if (appMime.GetIcon(icon, (icon_size)size) == B_OK) {
			barInfo->iconCache[index] = barInfo->icon = icon;

			return B_OK;
		}
	}

	// couldn't find the app icon
	// fetch the generic 3 boxes icon and cache it
	BMimeType defaultAppMime;
	defaultAppMime.SetTo(B_APP_MIME_TYPE);
	if (defaultAppMime.GetIcon(icon, (icon_size)size) == B_OK) {
		barInfo->iconCache[index] = barInfo->icon = icon;

		return B_OK;
	}

	// couldn't find generic 3 boxes icon
	// fill with transparent
	uint8* iconBits = (uint8*)icon->Bits();
	if (icon->ColorSpace() == B_RGBA32) {
		size_t i = 0;
		while (i < icon->BitsLength()) {
			iconBits[i++] = B_TRANSPARENT_32_BIT.red;
			iconBits[i++] = B_TRANSPARENT_32_BIT.green;
			iconBits[i++] = B_TRANSPARENT_32_BIT.blue;
			iconBits[i++] = B_TRANSPARENT_32_BIT.alpha;
		}
	} else {
		// Assume B_CMAP8
		for (size_t i = 0; i < icon->BitsLength(); i++)
			iconBits[i] = B_TRANSPARENT_MAGIC_CMAP8;
	}

	barInfo->iconCache[index] = barInfo->icon = icon;

	return B_OK;
}


status_t
TBarApp::_CacheWindowIcon(WindowIconCache* winCache)
{
	if (winCache == NULL)
		return B_BAD_VALUE;

	// clip font size
	int32 fontSize = (int32)floorf(be_plain_font->Size());
	if (fontSize < kMinimumFontSize)
		fontSize = kMinimumFontSize;
	if (fontSize > kMaximumFontSize)
		fontSize = kMaximumFontSize;

	// icon index based on font size
	const int32 index = (fontSize - kMinimumFontSize) / kFontSizeInterval;

	// first look in the icon cache
	winCache->icon = winCache->iconCache[index];
	if (winCache->icon != NULL)
		return B_OK;

	int32 id = winCache->id;
	uint8* data;
	size_t size;

	// icon wasn't in cache, get vector icon from resource and cache it
	data = (uint8*)AppResSet()->FindResource(B_VECTOR_ICON_TYPE, id, &size);
	if (data != NULL && size > 0) {
		// seems valid, scale bitmap according to font size
		BBitmap* icon = new(std::nothrow) BBitmap(
			BRect(B_ORIGIN, be_control_look->ComposeIconSize(B_MINI_ICON)),
			B_RGBA32);
		if (icon != NULL && icon->InitCheck() == B_OK
			&& BIconUtils::GetVectorIcon(const_cast<const uint8*>(data),
				size, icon) == B_OK) {
			winCache->iconCache[index] = winCache->icon = icon;

			return B_OK;
		} else if (icon != NULL) {
			// ran out of memory allocating bitmap, this should never happen
			delete icon;
			delete winCache->iconCache[index];
			winCache->iconCache[index] = winCache->icon = NULL;

			return B_NO_MEMORY;
		}
	}

	return B_ERROR;
}


//	#pragma mark - Wayland programs (Renga, batch 12)


/*!	Every window of a Wayland program, a Linux program under Renguin or a
	native one, belongs to the compositor's team, a background server that
	has no entry. The compositor answers kMsgListWaylandClients, and sends
	an update whenever a program shows its first window or closes its last,
	with one "client" per program: its name, signature, icon (the program's
	Deskbar menu entry) and the client tokens of its windows
	(headers/private/shared/wayland_clients.h). Each program gets an entry
	of its own (BarTeamInfo::waylandClient) whose team is the compositor;
	everything that lists or acts on a team's windows asks
	WindowTokensFor() or WindowBelongsTo(), which keep for such an entry
	only the windows with its tokens. Several clients of one program (one
	signature) share an entry, as several teams of one application do.
	The Switcher does not list them yet.
*/
void
TBarApp::_RequestWaylandClients()
{
	BMessenger compositor(RENGA_WAYLAND_COMPOSITOR_SIGNATURE);
	BMessage request(kMsgListWaylandClients);
	// the reply (kMsgWaylandClients) comes to MessageReceived()
	compositor.SendMessage(&request, this, 500000);
}


/*static*/ team_id
TBarApp::CompositorTeam()
{
	BAutolock lock(sWaylandLock);
	return sCompositorTeam;
}


/*static*/ bool
TBarApp::WindowBelongsTo(const char* signature, const window_info* info)
{
	if (info == NULL)
		return false;
	BAutolock lock(sWaylandLock);
	if (sCompositorTeam < 0 || info->team != sCompositorTeam)
		return true;
	if (signature == NULL)
		return false;
	std::map<int32, BString>::const_iterator found
		= sWaylandTokens.find(info->client_token);
	return found != sWaylandTokens.end()
		&& found->second.ICompare(signature) == 0;
}


/*!	The server tokens of the windows of \a teams (get_token_list() for each)
	that belong to the entry with \a signature: for a Wayland program only
	its own windows among the compositor's. The caller free()s the result.
*/
/*static*/ int32*
TBarApp::WindowTokensFor(const BList* teams, const char* signature,
	int32* _count)
{
	*_count = 0;
	if (teams == NULL)
		return NULL;
	team_id compositor = CompositorTeam();
	std::vector<int32> result;
	for (int32 i = 0; i < teams->CountItems(); i++) {
		team_id team = (addr_t)teams->ItemAt(i);
		int32 count = 0;
		int32* tokens = get_token_list(team, &count);
		for (int32 j = 0; j < count; j++) {
			if (compositor < 0 || team != compositor) {
				result.push_back(tokens[j]);
				continue;
			}
			client_window_info* info = get_window_info(tokens[j]);
			if (info != NULL && WindowBelongsTo(signature, info))
				result.push_back(tokens[j]);
			free(info);
		}
		free(tokens);
	}
	int32* tokens = (int32*)malloc(sizeof(int32) * (result.size() + 1));
	if (tokens == NULL)
		return NULL;
	for (size_t i = 0; i < result.size(); i++)
		tokens[i] = result[i];
	*_count = result.size();
	return tokens;
}


//!	The client processes of the Wayland program with \a signature.
/*static*/ void
TBarApp::WaylandClientPids(const char* signature, BList* pids)
{
	BString key(signature);
	key.ToLower();
	BAutolock lock(sWaylandLock);
	std::pair<std::multimap<BString, team_id>::const_iterator,
		std::multimap<BString, team_id>::const_iterator> range
			= sWaylandPids.equal_range(key);
	for (; range.first != range.second; range.first++)
		pids->AddItem((void*)(addr_t)range.first->second);
}


//!	The Deskbar menu entry of the Wayland program with \a signature.
/*static*/ BString
TBarApp::WaylandIconPath(const char* signature)
{
	BString key(signature);
	key.ToLower();
	BAutolock lock(sWaylandLock);
	std::map<BString, BString>::const_iterator found
		= sWaylandIconPaths.find(key);
	return found != sWaylandIconPaths.end() ? found->second : BString();
}


//!	Removes the Wayland entry at \a index (sSubscriberLock held).
void
TBarApp::_RemoveWaylandEntry(int32 index)
{
	BarTeamInfo* barInfo = (BarTeamInfo*)sBarTeamInfoList.RemoveItem(index);
	if (barInfo == NULL)
		return;
	int32 subsCount = sSubscribers.CountItems();
	for (int32 i = 0; i < subsCount; i++) {
		BMessage message(B_SOME_APP_QUIT);
		message.AddInt32("team", (addr_t)barInfo->teams->ItemAt(0));
		message.AddString("sig", barInfo->sig);
		((BMessenger*)sSubscribers.ItemAt(i))->SendMessage(&message);
	}
	delete barInfo;
}


void
TBarApp::_UpdateWaylandClients(BMessage* message)
{
	team_id team = message->GetInt32("team", -1);
	if (team < 0)
		return;

	struct Program {
		BString				sig;
		BString				name;
		BString				iconPath;
		std::vector<int32>	tokens;
		std::vector<team_id> pids;
	};
	std::vector<Program> programs;
	BMessage client;
	for (int32 i = 0; message->FindMessage("client", i, &client) == B_OK;
			i++) {
		BString sig = client.GetString("signature", "");
		sig.ToLower();
		BString name = client.GetString("name", "");
		if (sig.Length() == 0 || name.Length() == 0)
			continue;
		Program* program = NULL;
		for (Program& other : programs) {
			if (other.sig == sig)
				program = &other;
		}
		if (program == NULL) {
			programs.push_back(Program());
			program = &programs.back();
			program->sig = sig;
			program->name = name;
			program->iconPath = client.GetString("icon_path", "");
		}
		int32 token;
		for (int32 j = 0; client.FindInt32("token", j, &token) == B_OK; j++)
			program->tokens.push_back(token);
		int32 pid = client.GetInt32("pid", -1);
		if (pid > 0)
			program->pids.push_back(pid);
	}

	{
		BAutolock lock(sWaylandLock);
		sCompositorTeam = team;
		sWaylandTokens.clear();
		sWaylandPids.clear();
		sWaylandIconPaths.clear();
		for (const Program& program : programs) {
			sWaylandIconPaths[program.sig] = program.iconPath;
			for (int32 token : program.tokens)
				sWaylandTokens[token] = program.sig;
			for (team_id pid : program.pids) {
				sWaylandPids.insert(
					std::pair<BString, team_id>(program.sig, pid));
			}
		}
	}

	BAutolock autolock(sSubscriberLock);
	if (!autolock.IsLocked())
		return;

	// entries whose program is gone, or whose name or icon changed
	std::vector<bool> present(programs.size(), false);
	for (int32 i = sBarTeamInfoList.CountItems() - 1; i >= 0; i--) {
		BarTeamInfo* barInfo = (BarTeamInfo*)sBarTeamInfoList.ItemAt(i);
		if (!barInfo->waylandClient)
			continue;
		bool keep = false;
		for (size_t j = 0; j < programs.size(); j++) {
			if (programs[j].sig.ICompare(barInfo->sig) != 0)
				continue;
			keep = (team_id)(addr_t)barInfo->teams->ItemAt(0) == team
				&& programs[j].name == barInfo->name
				&& programs[j].iconPath == barInfo->iconPath;
			present[j] = keep;
		}
		if (!keep)
			_RemoveWaylandEntry(i);
	}

	// new programs
	for (size_t j = 0; j < programs.size(); j++) {
		if (present[j])
			continue;
		BarTeamInfo* barInfo = new BarTeamInfo(new BList(), B_MULTIPLE_LAUNCH,
			strdup(programs[j].sig.String()),
			strdup(programs[j].name.String()));
		barInfo->waylandClient = true;
		barInfo->iconPath = programs[j].iconPath;
		barInfo->teams->AddItem((void*)(addr_t)team);
		_CacheTeamIcon(barInfo);
		sBarTeamInfoList.AddItem(barInfo);
		_NotifyTeamAdded(barInfo);
	}
}


//	#pragma mark - BarTeamInfo


BarTeamInfo::BarTeamInfo(BList* teams, uint32 flags, char* sig, char* name,
	BBitmap* icon)
	:
	teams(teams),
	flags(flags),
	sig(sig),
	name(name),
	icon(icon),
	waylandClient(false)
{
	_Init();
}


BarTeamInfo::BarTeamInfo(const BarTeamInfo &info)
	:
	teams(new BList(*info.teams)),
	flags(info.flags),
	sig(strdup(info.sig)),
	name(strdup(info.name)),
	icon(new BBitmap(*info.icon)),
	waylandClient(info.waylandClient),
	iconPath(info.iconPath)
{
	_Init();
}


BarTeamInfo::~BarTeamInfo()
{
	delete teams;
	free(sig);
	free(name);
	for (int32 i = 0; i < kIconCacheCount; i++)
		delete iconCache[i];
}


void
BarTeamInfo::_Init()
{
	for (int32 i = 0; i < kIconCacheCount; i++)
		iconCache[i] = NULL;
}


//	#pragma mark - WindowIconCache


WindowIconCache::WindowIconCache(int32 id, BBitmap* icon)
	:
	id(id),
	icon(icon)
{
	_Init();
}


WindowIconCache::WindowIconCache(const WindowIconCache &cache)
	:
	id(cache.id),
	icon(new BBitmap(*cache.icon))
{
	_Init();
}


WindowIconCache::~WindowIconCache()
{
	for (int32 i = 0; i < kWindowIconCacheCount; i++)
		delete iconCache[i];
}


void
WindowIconCache::_Init()
{
	for (int32 i = 0; i < kWindowIconCacheCount; i++)
		iconCache[i] = NULL;
}
