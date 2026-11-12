/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 */

/*!	In-guest test: Tracker's folder menus built ahead of time (roadmap
	F-59; src/kits/tracker/NavMenuPrefetch.h).

	The folder submenus of Tracker's context menus (the Desktop entry of
	the desktop's menu, "Move to", "Copy to") were built in the menu's
	thread when the pointer rested on them, reading the folder and a Model
	per entry: a stall on every hover. Now a background thread reads them
	ahead of time and the menu takes the prebuilt entries.

	Tracker's test hook kTestBuildNavMenu ('Tnav', harness images only)
	builds a folder's menu exactly as hovering it does
	(BNavMenu::BuildForTest()) and replies with the time it took, the
	items, the cache's counters and the setting; "prefetch" changes the
	setting first.

	  tracker_menu.setting_default   the setting is on by default
	  tracker_menu.items             a folder of 400 files and 3
	                                 subfolders: 403 items, with and without
	                                 prefetching
	  tracker_menu.prefetched        after the first build the folder is
	                                 read in the background; the next build
	                                 takes the prebuilt entries (a cache
	                                 hit)
	  (the build times with and without are printed as INFO: with the
	  file system cache warm, as in QEMU, the folder read is a small
	  part of a build, so the times are not judged)
	  tracker_menu.subfolder         a subfolder shown in the menu is read
	                                 ahead too: its first build is a hit
	  tracker_menu.fresh             a file added to the folder shows in the
	                                 next build (the cached list is dropped)
	  tracker_menu.desktop           the Desktop folder (the desktop menu's
	                                 Desktop entry) is a hit after the
	                                 desktop's folder was prefetched
	  tracker_menu.prefs_switch      Tracker preferences, Desktop page:
	                                 "Prefetch folder menus in the
	                                 background" off from the window turns
	                                 the setting off and empties the cache
	                                 (screenshot tracker_prefetch_setting)
	  tracker_menu.off_no_cache      with it off, builds are misses and
	                                 nothing is cached
	  tracker_menu.prefs_defaults    the window's Defaults turns it on again

	Output format: see 3rdparty/ce/README.md.
*/


#include <Application.h>
#include <Directory.h>
#include <Entry.h>
#include <File.h>
#include <FindDirectory.h>
#include <Messenger.h>
#include <Path.h>
#include <Roster.h>
#include <String.h>

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>


#define SUITE "tracker_menu"

static const char* kTrackerSignature = "application/x-vnd.Be-TRAK";
static const uint32 kTestBuildNavMenu = 'Tnav';
static const uint32 kShowSettingsWindow = 'Tstw';
static const uint32 kPrefetchFolderMenusChanged = 'PFMc';
static const uint32 kDefaultsButtonPressed = 'Apbp';

static int sFailures = 0;


static void
report(const char* name, bool pass, const char* format, ...)
{
	char detail[320];
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


struct Build {
	status_t	status;
	bigtime_t	time;
	int32		items;
	int32		hits;
	int32		misses;
	int32		builds;
	int32		folders;
	bool		setting;
};


static Build
build(const char* path, int prefetch = -1)
{
	Build result;
	memset(&result, 0, sizeof(result));
	BMessage message(kTestBuildNavMenu);
	entry_ref ref;
	if (path != NULL && get_ref_for_path(path, &ref) == B_OK)
		message.AddRef("refs", &ref);
	if (prefetch >= 0)
		message.AddBool("prefetch", prefetch != 0);
	BMessage reply;
	result.status = BMessenger(kTrackerSignature).SendMessage(&message,
		&reply, 5000000, 30000000);
	if (result.status == B_OK && reply.what != B_REPLY)
		result.status = B_ERROR;
	result.time = reply.GetInt64("time", -1);
	result.items = reply.GetInt32("items", -1);
	result.hits = reply.GetInt32("hits", -1);
	result.misses = reply.GetInt32("misses", -1);
	result.builds = reply.GetInt32("builds", -1);
	result.folders = reply.GetInt32("folders", -1);
	result.setting = reply.GetBool("setting", false);
	return result;
}


//! Waits until the cache built \a more folders than \a builds.
static bool
wait_builds(int32 builds, int32 more = 1)
{
	for (int32 i = 0; i < 100; i++) {
		if (build(NULL).builds >= builds + more)
			return true;
		snooze(100000);
	}
	return false;
}


static void
touch(const char* path)
{
	BFile file(path, B_CREATE_FILE | B_WRITE_ONLY);
}


int
main(int argc, char** argv)
{
	BApplication app("application/x-vnd.Renga-tracker-menu-test");
	printf("CE-SUITE %s START\n", SUITE);

	// Tracker may still be starting (this runs early in the boot), or not
	// be the desktop at all (the renga-* images' desktop shell): then it
	// is started here.
	for (int32 i = 0; i < 80 && !be_roster->IsRunning(kTrackerSignature);
			i++) {
		snooze(250000);
	}
	if (!be_roster->IsRunning(kTrackerSignature)) {
		printf("CE-INFO tracker_menu: starting Tracker\n");
		be_roster->Launch(kTrackerSignature);
		for (int32 i = 0; i < 120
				&& !be_roster->IsRunning(kTrackerSignature); i++) {
			snooze(250000);
		}
	}
	snooze(3000000);
	if (!be_roster->IsRunning(kTrackerSignature)) {
		printf("CE-INFO tracker_menu: Tracker is not running, nothing to "
			"test\n");
		printf("CE-SUITE %s DONE failures=0\n", SUITE);
		return 0;
	}

	Build state = build(NULL);
	report("setting_default", state.status == B_OK && state.setting,
		"status 0x%x, setting %d", (unsigned)state.status, state.setting);

	// A folder with 400 files and 3 subfolders, outside the Desktop (the
	// desktop shows its own entries).
	BPath folder;
	find_directory(B_USER_DIRECTORY, &folder);
	folder.Append("ce-prefetch");
	create_directory(folder.Path(), 0755);
	for (int32 i = 0; i < 400; i++) {
		BString name;
		name.SetToFormat("%s/file %03d.txt", folder.Path(), (int)i);
		touch(name.String());
	}
	BString subfolder;
	for (int32 i = 1; i <= 3; i++) {
		BString name;
		name.SetToFormat("%s/sub%d", folder.Path(), (int)i);
		create_directory(name.String(), 0755);
		subfolder = name;
		for (int32 j = 0; j < 20; j++) {
			BString file;
			file.SetToFormat("%s/inner %d", name.String(), (int)j);
			touch(file.String());
		}
	}

	// Without prefetching: once for the file system cache, then measured.
	build(folder.Path(), 0);
	bigtime_t offTime = 0;
	Build off;
	for (int32 i = 0; i < 3; i++) {
		off = build(folder.Path(), 0);
		offTime += off.time;
	}
	offTime /= 3;

	// With: the first build misses and starts the background read.
	Build first = build(folder.Path(), 1);
	bool built = wait_builds(first.builds);
	bigtime_t onTime = 0;
	Build on;
	int32 hitsBefore = build(NULL).hits;
	for (int32 i = 0; i < 3; i++) {
		on = build(folder.Path());
		onTime += on.time;
	}
	onTime /= 3;
	report("items", off.items == 403 && on.items == 403,
		"%d items without prefetching, %d with (want 403)", (int)off.items,
		(int)on.items);
	report("prefetched", built && on.hits - hitsBefore == 3,
		"background read %s, %d of 3 builds took the prebuilt entries",
		built ? "done" : "not done", (int)(on.hits - hitsBefore));
	// The time is reported, not judged: with the file system cache warm
	// (QEMU), reading the folder is a small part of a build; the stall it
	// causes on a real disk is what prefetching removes.
	printf("CE-INFO tracker_menu: %.2f ms per build with prefetching, %.2f "
		"ms without\n", onTime / 1000.0, offTime / 1000.0);
	fflush(stdout);

	// The subfolders were read when the folder's menu was built.
	snooze(1500000);
	Build sub = build(subfolder.String());
	Build afterSub = build(NULL);
	report("subfolder", sub.items == 20 && afterSub.hits == sub.hits
			&& sub.hits > on.hits,
		"%d items (want 20), hits %d -> %d", (int)sub.items, (int)on.hits,
		(int)sub.hits);

	// A new file shows at once.
	BString added;
	added.SetToFormat("%s/added later.txt", folder.Path());
	touch(added.String());
	snooze(300000);
	Build fresh = build(folder.Path());
	report("fresh", fresh.items == 404,
		"%d items after adding a file (want 404)", (int)fresh.items);

	// The Desktop.
	BPath desktop;
	find_directory(B_DESKTOP_DIRECTORY, &desktop);
	Build desk = build(desktop.Path());
	wait_builds(desk.builds, 0);
	snooze(1000000);
	Build desk2 = build(desktop.Path());
	report("desktop", desk2.status == B_OK && desk2.hits > desk.hits,
		"Desktop menu: %d items, hits %d -> %d", (int)desk2.items,
		(int)desk.hits, (int)desk2.hits);

	// Tracker preferences.
	BMessenger tracker(kTrackerSignature);
	tracker.SendMessage(kShowSettingsWindow);
	snooze(2000000);
	printf("CE-HOST screenshot tracker_prefetch_setting\n");
	fflush(stdout);
	snooze(300000);
	BMessage turnOff(kPrefetchFolderMenusChanged);
	turnOff.AddBool("renga:value", false);
	turnOff.AddSpecifier("View", "DesktopSettingsView");
	turnOff.AddSpecifier("Window", "Tracker preferences");
	BMessage reply;
	status_t sent = tracker.SendMessage(&turnOff, &reply, 2000000, 2000000);
	snooze(500000);
	Build afterOff = build(NULL);
	report("prefs_switch", sent == B_OK && !afterOff.setting
			&& afterOff.folders == 0,
		"sent 0x%x, setting %d, %d folders cached", (unsigned)sent,
		afterOff.setting, (int)afterOff.folders);

	Build offBuild = build(folder.Path());
	snooze(1000000);
	Build offAfter = build(NULL);
	report("off_no_cache", offBuild.items == 404
			&& offAfter.hits == afterOff.hits && offAfter.folders == 0,
		"%d items, hits %d -> %d, %d folders cached", (int)offBuild.items,
		(int)afterOff.hits, (int)offAfter.hits, (int)offAfter.folders);

	BMessage defaults(kDefaultsButtonPressed);
	defaults.AddSpecifier("Window", "Tracker preferences");
	sent = tracker.SendMessage(&defaults, &reply, 2000000, 2000000);
	snooze(500000);
	Build afterDefaults = build(NULL);
	report("prefs_defaults", sent == B_OK && afterDefaults.setting,
		"sent 0x%x, setting %d", (unsigned)sent, afterDefaults.setting);

	BMessage close(B_QUIT_REQUESTED);
	close.AddSpecifier("Window", "Tracker preferences");
	tracker.SendMessage(&close, &reply, 1000000, 1000000);

	// Clean up.
	BString command;
	command.SetToFormat("rm -rf \"%s\"", folder.Path());
	system(command.String());

	printf("CE-SUITE %s DONE failures=%d\n", SUITE, sFailures);
	return sFailures == 0 ? 0 : 1;
}
