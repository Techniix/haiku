/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 */


/*!	Snap layouts in app_server (Renga, batch 7; roadmap F-25). See
	SnapLayouts.h and <window_layouts.h>.

	Locking: the settings live behind sLock, which is never held while
	calling into the Desktop. Functions called from a window behaviour run
	with the Desktop's window lock held for writing (mouse events); the
	Desktop methods used here lock it again, which MultiLocker allows.
*/


#include "SnapLayouts.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include <math.h>

#include <algorithm>
#include <map>
#include <set>
#include <string>
#include <vector>

#include <Autolock.h>
#include <FindDirectory.h>
#include <Locker.h>
#include <Message.h>
#include <Region.h>

#include <window_layouts.h>

#include "Desktop.h"
#include "DesktopSettings.h"
#include "ServerApp.h"
#include "ServerWindow.h"
#include "Window.h"
#include "decorator/Decorator.h"


namespace SnapLayouts {


using WindowLayouts::Settings;


static BLocker sLock("snap layouts");
static Settings sSettings;
static bool sLoaded = false;
static bigtime_t sLastCheck = 0;
static ino_t sFileNode = -1;
static time_t sFileTime = -1;
static off_t sFileSize = -1;
static std::map<int32, BRect> sRestoreFrames;
	// server token -> content frame from before the window was snapped
static std::map<int32, bool> sRulesApplied;

static const char* kDeskbarSignature = "application/x-vnd.be-tskb";


//!	With sLock held.
static void
reload_if_changed()
{
	bigtime_t now = system_time();
	if (sLoaded && now - sLastCheck < 1000000)
		return;
	sLastCheck = now;

	char file[B_PATH_NAME_LENGTH];
	struct stat st;
	bool exists = WindowLayouts::path(file, sizeof(file))
		&& stat(file, &st) == 0;
	ino_t node = exists ? st.st_ino : 0;
	time_t time = exists ? st.st_mtime : 0;
	off_t size = exists ? st.st_size : 0;
	if (sLoaded && node == sFileNode && time == sFileTime
		&& size == sFileSize)
		return;
	sFileNode = node;
	sFileTime = time;
	sFileSize = size;
	WindowLayouts::load(sSettings);
	sLoaded = true;
}


static bool
same_signature(const char* a, const char* b)
{
	return a != NULL && b != NULL && strcasecmp(a, b) == 0;
}


/*!	The screen's area that windows may fill: the Deskbar's window is
	left out when it lies along an edge (a horizontal Deskbar takes a band
	at the top or bottom, a vertical one a band at its side).
*/
static BRect
work_area(Desktop* desktop, BRect screen)
{
	BRect area = screen;
	for (Window* window = desktop->CurrentWindows().FirstWindow();
			window != NULL;
			window = window->NextWindow(desktop->CurrentWorkspace())) {
		if (window->IsHidden() || window->ServerWindow() == NULL
			|| window->ServerWindow()->App() == NULL
			|| !same_signature(window->ServerWindow()->App()->Signature(),
				kDeskbarSignature)
			|| strcmp(window->Title(), "Deskbar") != 0)
			continue;
		BRect frame = window->Frame();
		if (window->Decorator() != NULL)
			frame = frame | window->Decorator()->GetFootprint().Frame();
		if (!frame.Intersects(screen))
			continue;
		// only a Deskbar along most of an edge takes a band; one in a
		// corner (the default, expanded at the top right) floats over the
		// windows, as with Zoom
		if (frame.Width() > screen.Width() / 2) {
			if (frame.top <= screen.top + 2)
				area.top = std::max(area.top, frame.bottom + 1);
			else if (frame.bottom >= screen.bottom - 2)
				area.bottom = std::min(area.bottom, frame.top - 1);
		} else if (frame.Height() > screen.Height() / 2) {
			if (frame.left <= screen.left + 2)
				area.left = std::max(area.left, frame.right + 1);
			else if (frame.right >= screen.right - 2)
				area.right = std::min(area.right, frame.left - 1);
		}
	}
	if (!area.IsValid() || area.Width() < screen.Width() / 2
		|| area.Height() < screen.Height() / 2)
		return screen;
	return area;
}


static int32
display_for(Desktop* desktop, BPoint where, BRect& frame)
{
	const ::VirtualScreen& screens = desktop->VirtualScreen();
	int32 count = screens.CountScreens();
	for (int32 i = 0; i < count; i++) {
		BRect screen = screens.ScreenFrameAt(i);
		if (screen.Contains(where)) {
			frame = screen;
			return i;
		}
	}
	frame = count > 0 ? screens.ScreenFrameAt(0) : screens.Frame();
	return 0;
}


bool
DragActive(int32 modifiers, bool* span)
{
	BAutolock _(sLock);
	reload_if_changed();
	if (!sSettings.snap || (modifiers & sSettings.modifier) == 0)
		return false;
	if (span != NULL) {
		*span = sSettings.span
			&& (modifiers & WindowLayouts::span_modifier(sSettings)) != 0;
	}
	return true;
}


bool
EdgeZone(Desktop* desktop, BPoint where, BRect* zone)
{
	int32 gap;
	{
		BAutolock _(sLock);
		reload_if_changed();
		if (!sSettings.snap || !sSettings.edgeSnap)
			return false;
		gap = sSettings.gap;
	}
	BRect screen;
	display_for(desktop, where, screen);
	return WindowLayouts::edge_zone(screen, work_area(desktop, screen), where,
		gap, *zone);
}


int32
ZoneFrames(Desktop* desktop, BPoint where, BRect* frames, int32 maxFrames)
{
	BRect screen;
	int32 display = display_for(desktop, where, screen);
	BRect area = work_area(desktop, screen);

	WindowLayouts::Layout layout;
	int32 gap;
	{
		BAutolock _(sLock);
		reload_if_changed();
		WindowLayouts::resolve(sSettings, WindowLayouts::spec_for(sSettings,
			desktop->CurrentWorkspace(), display), layout);
		gap = sSettings.gap;
	}
	return WindowLayouts::zone_frames(layout, area, gap, frames, maxFrames);
}


static void
add_outline(BRegion& region, BRect frame, int32 width)
{
	if (!frame.IsValid())
		return;
	region.Include(BRect(frame.left, frame.top, frame.right,
		frame.top + width - 1));
	region.Include(BRect(frame.left, frame.bottom - width + 1, frame.right,
		frame.bottom));
	region.Include(BRect(frame.left, frame.top, frame.left + width - 1,
		frame.bottom));
	region.Include(BRect(frame.right - width + 1, frame.top, frame.right,
		frame.bottom));
}


void
ShowOverlay(Desktop* desktop, const BRect* frames, int32 count, BRect chosen)
{
	bool showGrid;
	{
		BAutolock _(sLock);
		showGrid = sSettings.showGrid;
	}
	BRegion grid;
	if (showGrid) {
		for (int32 i = 0; i < count; i++)
			add_outline(grid, frames[i], 1);
	}
	BRegion highlight;
	add_outline(highlight, chosen, 4);
	desktop->SetSnapOverlay(grid, highlight);
}


void
HideOverlay(Desktop* desktop)
{
	desktop->ClearSnapOverlay();
}


void
PlaceWindow(Desktop* desktop, Window* window, BRect zone,
	const BRect* restoreFrame)
{
	if (window == NULL || !zone.IsValid())
		return;

	BRect frame = window->Frame();
	BRect outer = frame;
	if (window->Decorator() != NULL)
		outer = outer | window->Decorator()->GetFootprint().Frame();

	int32 token = window->ServerWindow()->ServerToken();
	{
		BAutolock _(sLock);
		if (sRestoreFrames.find(token) == sRestoreFrames.end()) {
			sRestoreFrames[token] = restoreFrame != NULL && restoreFrame->IsValid()
				? *restoreFrame : frame;
		}
	}

	BRect target(zone.left + (frame.left - outer.left),
		zone.top + (frame.top - outer.top),
		zone.right - (outer.right - frame.right),
		zone.bottom - (outer.bottom - frame.bottom));
	if (!target.IsValid())
		return;

	if ((window->Flags() & B_NOT_RESIZABLE) == 0) {
		float width = (window->Flags() & B_NOT_H_RESIZABLE) != 0
			? frame.Width() : target.Width();
		float height = (window->Flags() & B_NOT_V_RESIZABLE) != 0
			? frame.Height() : target.Height();
		desktop->ResizeWindowBy(window, width - frame.Width(),
			height - frame.Height());
	}
	// a window that cannot take the zone's size is centred in it
	frame = window->Frame();
	float left = target.left;
	float top = target.top;
	if (frame.Width() < target.Width() - 1)
		left += floorf((target.Width() - frame.Width()) / 2);
	if (frame.Height() < target.Height() - 1)
		top += floorf((target.Height() - frame.Height()) / 2);
	desktop->MoveWindowBy(window, left - frame.left, top - frame.top);
}


bool
RestoreWindow(Desktop* desktop, Window* window)
{
	BRect frame;
	{
		BAutolock _(sLock);
		std::map<int32, BRect>::iterator found
			= sRestoreFrames.find(window->ServerWindow()->ServerToken());
		if (found == sRestoreFrames.end())
			return false;
		frame = found->second;
		sRestoreFrames.erase(found);
	}
	BRect current = window->Frame();
	desktop->ResizeWindowBy(window, frame.Width() - current.Width(),
		frame.Height() - current.Height());
	current = window->Frame();
	desktop->MoveWindowBy(window, frame.left - current.left,
		frame.top - current.top);
	return true;
}


status_t
PlaceWindowInZone(Desktop* desktop, Window* window, int32 index)
{
	if (window == NULL || !window->IsNormal()
		|| (window->Flags() & B_NOT_MOVABLE) != 0)
		return B_NOT_ALLOWED;
	BRect frames[kMaxFrames];
	BRect frame = window->Frame();
	BPoint center((frame.left + frame.right) / 2,
		(frame.top + frame.bottom) / 2);
	int32 count = ZoneFrames(desktop, center, frames, kMaxFrames);
	if (index < 0 || index >= count)
		return B_BAD_INDEX;
	PlaceWindow(desktop, window, frames[index]);
	return B_OK;
}


bool
HandleKey(Desktop* desktop, int32 key, int32 modifiers)
{
	if ((modifiers & (B_COMMAND_KEY | B_CONTROL_KEY | B_OPTION_KEY
			| B_SHIFT_KEY)) != (B_COMMAND_KEY | B_CONTROL_KEY))
		return false;
	// keys 1..9 are 0x12..0x1a, 0 is 0x1b; the up arrow is 0x57
	bool overview = key == 0x57;
	if (!overview && (key < 0x12 || key > 0x1b))
		return false;
	{
		BAutolock _(sLock);
		reload_if_changed();
		if (!sSettings.zoneShortcuts)
			return false;
	}
	if (overview) {
		// the Workspaces overview: every workspace, large, to choose from
		system("/boot/system/apps/Workspaces --overview &");
		return true;
	}

	if (!desktop->LockAllWindows())
		return false;
	Window* window = desktop->FocusWindow();
	bool used = false;
	if (window != NULL && window->IsNormal()) {
		if (key == 0x1b)
			used = RestoreWindow(desktop, window);
		else
			used = PlaceWindowInZone(desktop, window, key - 0x12) == B_OK;
	}
	desktop->UnlockAllWindows();
	return used;
}


void
WindowShown(Desktop* desktop, Window* window)
{
	if (window == NULL || !window->IsNormal() || window->ServerWindow() == NULL
		|| window->ServerWindow()->App() == NULL)
		return;
	int32 token = window->ServerWindow()->ServerToken();
	int32 zone = -1;
	{
		BAutolock _(sLock);
		if (sRulesApplied.find(token) != sRulesApplied.end())
			return;
		sRulesApplied[token] = true;
		reload_if_changed();
		if (!sSettings.rules)
			return;
		const char* signature = window->ServerWindow()->App()->Signature();
		for (int32 i = 0; i < sSettings.ruleCount; i++) {
			if (same_signature(sSettings.ruleList[i].signature, signature)) {
				zone = sSettings.ruleList[i].zone - 1;
				break;
			}
		}
	}
	if (zone >= 0)
		PlaceWindowInZone(desktop, window, zone);
}


void
WindowRemoved(Window* window)
{
	if (window == NULL || window->ServerWindow() == NULL)
		return;
	BAutolock _(sLock);
	int32 token = window->ServerWindow()->ServerToken();
	sRestoreFrames.erase(token);
	sRulesApplied.erase(token);
}


bool
TabMenuEnabled()
{
	BAutolock _(sLock);
	reload_if_changed();
	return sSettings.tabMenu;
}


int32
TabDoubleClickAction()
{
	BAutolock _(sLock);
	reload_if_changed();
	return sSettings.tabDoubleClick;
}


void
ShowWindowMenu(Desktop* desktop, Window* window, BPoint where)
{
	BRect frames[kMaxFrames];
	int32 zones = ZoneFrames(desktop, where, frames, kMaxFrames);
	BMessage message(WindowLayouts::kMsgWindowMenu);
	message.AddPoint("where", where);
	message.AddInt32("zones", zones);
	message.AddInt32("workspace", desktop->CurrentWorkspace());
	DesktopSettings desktopSettings(desktop);
	message.AddInt32("workspaces", desktopSettings.WorkspacesCount());
	{
		BAutolock _(sLock);
		bool snap = sSettings.snap;
		message.AddBool("snap", snap);
		for (int32 i = 0; i < WindowLayouts::kMaxWorkspaces; i++)
			message.AddString("name", sSettings.workspaceName[i]);
	}
	window->ServerWindow()->SendMessageToClient(&message);
}


static bool
arrangement_path(const char* name, char* buffer, size_t size)
{
	if (name == NULL || name[0] == '\0' || name[0] == '.'
		|| strchr(name, '/') != NULL)
		return false;
	char dir[B_PATH_NAME_LENGTH];
	if (!WindowLayouts::directory(dir, sizeof(dir)))
		return false;
	mkdir(dir, 0755);
	snprintf(buffer, size, "%s/window_arrangements", dir);
	mkdir(buffer, 0755);
	snprintf(buffer, size, "%s/window_arrangements/%s", dir, name);
	return true;
}


/*!	One line per normal window of the current workspace:
	"<left> <top> <right> <bottom> <signature>\t<title>".
*/
status_t
SaveArrangement(Desktop* desktop, const char* name)
{
	char file[B_PATH_NAME_LENGTH];
	if (!arrangement_path(name, file, sizeof(file)))
		return B_BAD_VALUE;
	char temporary[B_PATH_NAME_LENGTH];
	snprintf(temporary, sizeof(temporary), "%s.new", file);
	FILE* out = fopen(temporary, "w");
	if (out == NULL)
		return B_ERROR;
	if (desktop->LockAllWindows()) {
		for (Window* window = desktop->CurrentWindows().FirstWindow();
				window != NULL;
				window = window->NextWindow(desktop->CurrentWorkspace())) {
			if (window->IsHidden() || !window->IsNormal()
				|| window->ServerWindow()->App() == NULL)
				continue;
			BRect frame = window->Frame();
			fprintf(out, "%d %d %d %d %s\t%s\n", (int)frame.left,
				(int)frame.top, (int)frame.right, (int)frame.bottom,
				window->ServerWindow()->App()->Signature(), window->Title());
		}
		desktop->UnlockAllWindows();
	}
	bool ok = fflush(out) == 0;
	fclose(out);
	if (!ok || rename(temporary, file) != 0)
		return B_ERROR;
	return B_OK;
}


/*!	Windows are matched by signature and title first, then by signature
	alone (each saved line is used once).
*/
status_t
RestoreArrangement(Desktop* desktop, const char* name, int32* _restored)
{
	char file[B_PATH_NAME_LENGTH];
	if (!arrangement_path(name, file, sizeof(file)))
		return B_BAD_VALUE;
	FILE* in = fopen(file, "r");
	if (in == NULL)
		return B_ENTRY_NOT_FOUND;

	struct Entry {
		BRect		frame;
		std::string	signature;
		std::string	title;
		bool		used;
	};
	std::vector<Entry> entries;
	char line[1024];
	while (fgets(line, sizeof(line), in) != NULL) {
		int left, top, right, bottom, used = 0;
		if (sscanf(line, "%d %d %d %d %n", &left, &top, &right, &bottom,
				&used) != 4)
			continue;
		char* rest = line + used;
		rest[strcspn(rest, "\n")] = '\0';
		char* tab = strchr(rest, '\t');
		if (tab == NULL)
			continue;
		*tab = '\0';
		Entry entry = { BRect(left, top, right, bottom), rest, tab + 1,
			false };
		entries.push_back(entry);
	}
	fclose(in);

	int32 restored = 0;
	std::set<Window*> placed;
	if (desktop->LockAllWindows()) {
		for (int pass = 0; pass < 2; pass++) {
			for (Window* window = desktop->CurrentWindows().FirstWindow();
					window != NULL;
					window = window->NextWindow(desktop->CurrentWorkspace())) {
				if (window->IsHidden() || !window->IsNormal()
					|| window->ServerWindow()->App() == NULL
					|| placed.find(window) != placed.end())
					continue;
				const char* signature
					= window->ServerWindow()->App()->Signature();
				for (size_t i = 0; i < entries.size(); i++) {
					Entry& entry = entries[i];
					if (entry.used || !same_signature(entry.signature.c_str(),
							signature)
						|| (pass == 0 && entry.title != window->Title()))
						continue;
					entry.used = true;
					placed.insert(window);
					BRect current = window->Frame();
					desktop->ResizeWindowBy(window,
						entry.frame.Width() - current.Width(),
						entry.frame.Height() - current.Height());
					current = window->Frame();
					desktop->MoveWindowBy(window,
						entry.frame.left - current.left,
						entry.frame.top - current.top);
					restored++;
					break;
				}
			}
		}
		desktop->UnlockAllWindows();
	}
	if (_restored != NULL)
		*_restored = restored;
	return B_OK;
}


}	// namespace SnapLayouts
