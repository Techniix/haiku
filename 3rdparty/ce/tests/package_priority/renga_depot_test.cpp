/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 */

/*!	In-guest test (renga-* images): Renga Depot's first part (batch 12).
	HaikuDepot is shown as "Renga Depot"; its file name, signature,
	settings and server protocol stay HaikuDepot's.

	  renga_depot.window       the main window's title is "Renga Depot"
	                           and so is its first menu
	  renga_depot.update_all   that menu has "Update all…" (a Terminal with
	                           pkgman full-sync), next to "Check for
	                           updates…"
	  renga_depot.source       the package list's repository column is
	                           "Source"; its labels: "Haiku", "HaikuPorts",
	                           "RengaPorts", other repositories by name
	  renga_depot.deskbar      the Deskbar's Applications entry is named
	                           "Renga Depot"
	  renga_depot.screenshots  the window (screen-renga_depot)

	Uses HaikuDepot's harness hook ('Trdp', harness images only).
	Output format: see 3rdparty/ce/README.md.
*/


#include <Application.h>
#include <Messenger.h>
#include <OS.h>
#include <Roster.h>
#include <String.h>

#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>


#define SUITE "renga_depot"

static const char* kSignature = "application/x-vnd.Haiku-HaikuDepot";
static const uint32 kMsgRengaHarnessReport = 'Trdp';
	// src/apps/haikudepot/HaikuDepotConstants.h
static int sFailures = 0;


static void
report(const char* name, bool pass, const char* format, ...)
{
	char detail[600];
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


static BString
joined(const BMessage& message, const char* field)
{
	BString text;
	const char* value;
	for (int32 i = 0; message.FindString(field, i, &value) == B_OK; i++)
		text << (i > 0 ? " | " : "") << value;
	return text;
}


static bool
has(const BMessage& message, const char* field, const char* wanted)
{
	const char* value;
	for (int32 i = 0; message.FindString(field, i, &value) == B_OK; i++) {
		if (strcmp(value, wanted) == 0)
			return true;
	}
	return false;
}


int
main()
{
	BApplication app("application/x-vnd.Renga-renga-depot-test");
	printf("CE-SUITE %s START\n", SUITE);
	fflush(stdout);

	entry_ref ref;
	if (be_roster->FindApp(kSignature, &ref) != B_OK) {
		printf("CE-INFO no HaikuDepot on this image, nothing to test\n");
		printf("CE-SUITE %s DONE failures=0\n", SUITE);
		return 0;
	}

	bool wasRunning = be_roster->IsRunning(kSignature);
	team_id team = -1;
	if (!wasRunning)
		be_roster->Launch(kSignature, (BMessage*)NULL, &team);
	else
		team = be_roster->TeamFor(kSignature);

	// the main window appears once the local repositories are read
	BMessage reply;
	for (int attempt = 0; attempt < 60; attempt++) {
		snooze(500000);
		BMessenger depot(kSignature);
		BMessage request(kMsgRengaHarnessReport);
		reply.MakeEmpty();
		if (depot.SendMessage(&request, &reply, 2000000, 2000000) == B_OK
			&& reply.what == B_REPLY && reply.HasString("title"))
			break;
	}

	const char* title = reply.GetString("title", "");
	report("window", strcmp(title, "Renga Depot") == 0
			&& strcmp(reply.GetString("menu", ""), "Renga Depot") == 0,
		"team %d, title \"%s\", first menu \"%s\"", (int)team, title,
		reply.GetString("menu", ""));

	report("update_all", has(reply, "menu_item", "Update all…")
			&& has(reply, "menu_item", "Check for updates…"),
		"menu: %s", joined(reply, "menu_item").String());

	BString sources = joined(reply, "source");
	report("source", has(reply, "column", "Source")
			&& !has(reply, "column", "Repository")
			&& sources == "Haiku | HaikuPorts | RengaPorts | my-repo",
		"columns: %s; labels for Haiku, haikuports_x86_64, RengaPorts, "
		"my-repo: %s", joined(reply, "column").String(), sources.String());

	bool link = access("/boot/system/data/deskbar/menu/Applications/"
		"Renga Depot", F_OK) == 0;
	bool oldLink = access("/boot/system/data/deskbar/menu/Applications/"
		"HaikuDepot", F_OK) == 0;
	report("deskbar", link && !oldLink, "\"Renga Depot\" entry %s, "
		"\"HaikuDepot\" entry %s", link ? "present" : "missing",
		oldLink ? "still there" : "gone");

	printf("CE-HOST screenshot renga_depot\n");
	fflush(stdout);
	snooze(1500000);
	report("screenshots", reply.HasString("title"), "screen-renga_depot");

	if (!wasRunning && team >= 0) {
		BMessenger(kSignature).SendMessage(B_QUIT_REQUESTED);
		for (int i = 0; i < 40 && be_roster->IsRunning(kSignature); i++)
			snooze(250000);
		if (be_roster->IsRunning(kSignature))
			kill_team(team);
	}

	printf("CE-SUITE %s DONE failures=%d\n", SUITE, sFailures);
	fflush(stdout);
	return sFailures != 0;
}
