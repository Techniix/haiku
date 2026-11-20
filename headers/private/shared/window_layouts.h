/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 */
#ifndef _RENGA_WINDOW_LAYOUTS_H
#define _RENGA_WINDOW_LAYOUTS_H


/*!	Snap layouts and workspace names (Renga, batch 7; roadmap F-25, the
	in-tree Lumina rework of 2026-11-12). One settings model shared by
	app_server (src/servers/app/SnapLayouts.cpp), the Control Panel's
	"Windows and workspaces" page, the Workspaces switcher in the Deskbar
	and the tests. Header only, so every user links nothing extra.

	~/config/settings/Renga/window_layouts, "key value" lines, replaced
	(never edited in place) on every change; missing keys keep the
	defaults given here:

	  snap true|false             master switch for snap layouts (on)
	  modifier shift|control|option|command
	                              key that shows the grid while dragging
	                              a window (shift); without it a drag is
	                              exactly the stock drag
	  span true|false             hold Ctrl as well (Shift if the modifier
	                              is Control or Command) to span zones (on)
	  show_grid true|false        draw every zone while choosing (on); off
	                              shows only the chosen zone
	  gap <pixels>                space between zones and screen edges (8)
	  zone_shortcuts true|false   Command+Control+1..9 put the focus
	                              window into zone 1..9, +0 restores its
	                              frame from before snapping (on)
	  tab_menu true|false         right-click on a window's tab opens the
	                              window menu (Move to workspace, Snap to,
	                              Minimize, Close) (on)
	  rules true|false            per-application rules apply (on)
	  switcher_plus true|false    the Deskbar's workspace switcher ends
	                              in a "+" that adds a workspace (on)
	  edge_snap true|false        a window dragged to a screen edge snaps
	                              without the modifier: left or right half,
	                              the whole work area at the top, a quarter
	                              in a corner (on; batch 8)
	  default <spec>              layout of every screen and workspace
	                              without its own (1x2)
	  display <index> <spec>      layout of one screen (0 = first)
	  workspace <index> <spec>    layout of one workspace (wins over the
	                              screen's)
	  layout <name> <l t r b>...  a custom layout from the zone editor:
	                              zones as fractions of the work area
	  rule <signature> <zone>     windows of this application open in this
	                              zone (1-based) of their workspace's layout
	  name <index> <text>         a workspace's name (rest of the line)

	A <spec> is "<rows>x<columns>" (1-6 each) or a custom layout's name.

	Saved sets of workspaces are copies of the workspace lines (count,
	names, layouts) in ~/config/settings/Renga/workspace_sets/<set>.
	Saved window arrangements are written by app_server to
	~/config/settings/Renga/window_arrangements/<name>.
*/


#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include <FindDirectory.h>
#include <InterfaceDefs.h>
#include <Rect.h>


namespace WindowLayouts {


enum {
	kMaxZones = 36,
	kMaxLayouts = 16,
	kMaxDisplays = 8,
	kMaxWorkspaces = 32,
	kMaxRules = 32,
	kSpecLength = 32,
	kNameLength = 48
};


/*!	app_server -> a window's application: open the window menu (the tab
	was right-clicked). Fields: "where" (BPoint, screen), "zones" (int32,
	zones of the window's layout), "workspace" (int32, current),
	"workspaces" (int32, count), "snap" (bool), "name" (strings, one per
	workspace, may be empty). BWindow shows the menu; its items come back
	to the window as kMsgWindowMenuAction with "workspace" (int32, -2 for a
	new one), "zone" (int32, 0-based), or "restore", "minimize", "close"
	(bool).
*/
static const uint32 kMsgWindowMenu = '_WMn';
static const uint32 kMsgWindowMenuAction = '_WMa';


struct Zone {
	float	left, top, right, bottom;
		// fractions of the work area, 0..1
};


struct Layout {
	char	name[kSpecLength];
	int32	count;
	Zone	zones[kMaxZones];
};


struct Rule {
	char	signature[B_MIME_TYPE_LENGTH];
	int32	zone;
		// 1-based
};


struct Settings {
	bool	snap;
	uint32	modifier;
	bool	span;
	bool	showGrid;
	int32	gap;
	bool	zoneShortcuts;
	bool	tabMenu;
	bool	rules;
	bool	switcherPlus;
	bool	edgeSnap;
	char	defaultSpec[kSpecLength];
	char	displaySpec[kMaxDisplays][kSpecLength];
	char	workspaceSpec[kMaxWorkspaces][kSpecLength];
	char	workspaceName[kMaxWorkspaces][kNameLength];
	int32	layoutCount;
	Layout	layouts[kMaxLayouts];
	int32	ruleCount;
	Rule	ruleList[kMaxRules];
};


static inline void
defaults(Settings& settings)
{
	memset(&settings, 0, sizeof(settings));
	settings.snap = true;
	settings.modifier = B_SHIFT_KEY;
	settings.span = true;
	settings.showGrid = true;
	settings.gap = 8;
	settings.zoneShortcuts = true;
	settings.tabMenu = true;
	settings.rules = true;
	settings.switcherPlus = true;
	settings.edgeSnap = true;
	strcpy(settings.defaultSpec, "1x2");
}


static inline bool
directory(char* buffer, size_t size)
{
	char settings[B_PATH_NAME_LENGTH];
	if (find_directory(B_USER_SETTINGS_DIRECTORY, -1, false, settings,
			sizeof(settings)) != B_OK)
		return false;
	snprintf(buffer, size, "%s/Renga", settings);
	return true;
}


static inline bool
path(char* buffer, size_t size)
{
	char dir[B_PATH_NAME_LENGTH];
	if (!directory(dir, sizeof(dir)))
		return false;
	snprintf(buffer, size, "%s/window_layouts", dir);
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


static inline const char*
modifier_name(uint32 modifier)
{
	switch (modifier) {
		case B_CONTROL_KEY:	return "control";
		case B_OPTION_KEY:	return "option";
		case B_COMMAND_KEY:	return "command";
		default:			return "shift";
	}
}


static inline uint32
parse_modifier(const char* value)
{
	if (strncmp(value, "control", 7) == 0)
		return B_CONTROL_KEY;
	if (strncmp(value, "option", 6) == 0)
		return B_OPTION_KEY;
	if (strncmp(value, "command", 7) == 0)
		return B_COMMAND_KEY;
	return B_SHIFT_KEY;
}


/*!	The keys that span zones together with the modifier: Ctrl (Command
	or Control, whichever the keymap makes of the key, since Renga's
	default keymap makes the Ctrl key Command) with Shift or Option, Shift
	otherwise.
*/
static inline uint32
span_modifier(const Settings& settings)
{
	return settings.modifier == B_SHIFT_KEY
			|| settings.modifier == B_OPTION_KEY
		? B_CONTROL_KEY | B_COMMAND_KEY : B_SHIFT_KEY;
}


//!	"<rows>x<columns>" with both 1-6.
static inline bool
parse_grid(const char* spec, int32& rows, int32& columns)
{
	char* end;
	long r = strtol(spec, &end, 10);
	if (end == spec || (*end != 'x' && *end != 'X'))
		return false;
	const char* second = end + 1;
	long c = strtol(second, &end, 10);
	if (end == second || (*end != '\0' && !isspace((unsigned char)*end)))
		return false;
	if (r < 1 || r > 6 || c < 1 || c > 6)
		return false;
	rows = r;
	columns = c;
	return true;
}


static inline void
copy_word(char* target, size_t size, const char* source)
{
	size_t i = 0;
	while (source[i] != '\0' && !isspace((unsigned char)source[i])
		&& i + 1 < size) {
		target[i] = source[i];
		i++;
	}
	target[i] = '\0';
}


static inline bool
valid_zone(const Zone& zone)
{
	return zone.left >= 0 && zone.top >= 0 && zone.right <= 1
		&& zone.bottom <= 1 && zone.right - zone.left >= 0.02f
		&& zone.bottom - zone.top >= 0.02f;
}


/*!	The layout a spec names: a grid, or a custom layout. An unknown spec
	falls back to the default spec, and that to 1x2, so there always is
	a layout.
*/
static inline void
resolve(const Settings& settings, const char* spec, Layout& layout)
{
	for (int pass = 0; pass < 3; pass++) {
		const char* name = pass == 0 ? spec
			: pass == 1 ? settings.defaultSpec : "1x2";
		if (name == NULL || name[0] == '\0')
			continue;
		int32 rows, columns;
		if (parse_grid(name, rows, columns)) {
			strlcpy(layout.name, name, sizeof(layout.name));
			layout.count = rows * columns;
			for (int32 i = 0; i < layout.count; i++) {
				int32 row = i / columns;
				int32 column = i % columns;
				layout.zones[i].left = (float)column / columns;
				layout.zones[i].right = (float)(column + 1) / columns;
				layout.zones[i].top = (float)row / rows;
				layout.zones[i].bottom = (float)(row + 1) / rows;
			}
			return;
		}
		for (int32 i = 0; i < settings.layoutCount; i++) {
			if (strcmp(settings.layouts[i].name, name) == 0
				&& settings.layouts[i].count > 0) {
				layout = settings.layouts[i];
				return;
			}
		}
	}
}


//!	Spec for a workspace on a screen: workspace, then screen, then default.
static inline const char*
spec_for(const Settings& settings, int32 workspace, int32 display)
{
	if (workspace >= 0 && workspace < kMaxWorkspaces
		&& settings.workspaceSpec[workspace][0] != '\0')
		return settings.workspaceSpec[workspace];
	if (display >= 0 && display < kMaxDisplays
		&& settings.displaySpec[display][0] != '\0')
		return settings.displaySpec[display];
	return settings.defaultSpec;
}


/*!	The zones of a layout in screen coordinates inside \a area, with
	\a gap pixels around and between them (half a gap on each side of an
	inner edge, a whole gap at the area's edges). Returns the count.
*/
static inline int32
zone_frames(const Layout& layout, BRect area, int32 gap, BRect* frames,
	int32 maxFrames)
{
	if (gap < 0)
		gap = 0;
	if (gap > 64)
		gap = 64;
	float width = area.Width() + 1;
	float height = area.Height() + 1;
	int32 count = 0;
	for (int32 i = 0; i < layout.count && count < maxFrames; i++) {
		const Zone& zone = layout.zones[i];
		float left = area.left + zone.left * width;
		float top = area.top + zone.top * height;
		float right = area.left + zone.right * width - 1;
		float bottom = area.top + zone.bottom * height - 1;
		left += zone.left <= 0.001f ? gap : gap / 2;
		top += zone.top <= 0.001f ? gap : gap / 2;
		right -= zone.right >= 0.999f ? gap : (gap + 1) / 2;
		bottom -= zone.bottom >= 0.999f ? gap : (gap + 1) / 2;
		BRect frame((int32)left, (int32)top, (int32)right, (int32)bottom);
		if (!frame.IsValid())
			frame = BRect(left, top, left, top);
		frames[count++] = frame;
	}
	return count;
}


/*!	Edge snapping without a key (batch 8): the zone for a drag whose pointer
	is at \a where on the screen \a screen (work area \a area, which leaves
	out an edge Deskbar). Within kEdgeDistance pixels of the left or right
	edge: that half, or that quarter when the pointer is also in the top or
	bottom quarter of the screen's height (a corner); within it of the top
	edge, elsewhere: the whole work area. Returns false away from the edges
	(the bottom edge snaps nothing, so windows can still be dragged down).
*/
static const int32 kEdgeDistance = 3;

static inline bool
edge_zone(BRect screen, BRect area, BPoint where, int32 gap, BRect& frame)
{
	bool left = where.x <= screen.left + kEdgeDistance;
	bool right = where.x >= screen.right - kEdgeDistance;
	bool top = where.y <= screen.top + kEdgeDistance;
	float cornerHeight = (screen.Height() + 1) / 4;
	bool upper = where.y < screen.top + cornerHeight;
	bool lower = where.y > screen.bottom - cornerHeight;
	if (!screen.Contains(where) || !(left || right || top))
		return false;

	Layout layout;
	memset(&layout, 0, sizeof(layout));
	layout.count = 1;
	Zone& zone = layout.zones[0];
	zone.left = 0;
	zone.top = 0;
	zone.right = 1;
	zone.bottom = 1;
	if (left || right) {
		if (left)
			zone.right = 0.5f;
		else
			zone.left = 0.5f;
		if (upper)
			zone.bottom = 0.5f;
		else if (lower)
			zone.top = 0.5f;
	}
	return zone_frames(layout, area, gap, &frame, 1) == 1;
}


//!	Index of the zone containing \a point, or the nearest one; -1 if none.
static inline int32
zone_at(const BRect* frames, int32 count, BPoint point)
{
	int32 best = -1;
	float bestDistance = 0;
	for (int32 i = 0; i < count; i++) {
		if (frames[i].Contains(point))
			return i;
		float dx = point.x < frames[i].left ? frames[i].left - point.x
			: point.x > frames[i].right ? point.x - frames[i].right : 0;
		float dy = point.y < frames[i].top ? frames[i].top - point.y
			: point.y > frames[i].bottom ? point.y - frames[i].bottom : 0;
		float distance = dx * dx + dy * dy;
		if (best < 0 || distance < bestDistance) {
			best = i;
			bestDistance = distance;
		}
	}
	return best;
}


//!	The frame spanning zones \a first to \a last (both included).
static inline BRect
span_frame(const BRect* frames, int32 count, int32 first, int32 last)
{
	if (first < 0 || first >= count)
		return last >= 0 && last < count ? frames[last] : BRect();
	if (last < 0 || last >= count)
		return frames[first];
	return frames[first] | frames[last];
}


static inline void
load_from(FILE* in, Settings& settings)
{
	char line[1024];
	while (fgets(line, sizeof(line), in) != NULL) {
		size_t length = strlen(line);
		while (length > 0 && (line[length - 1] == '\n'
				|| line[length - 1] == '\r'))
			line[--length] = '\0';
		char* value = strchr(line, ' ');
		if (value == NULL)
			continue;
		*value++ = '\0';
		if (strcmp(line, "snap") == 0)
			settings.snap = parse_bool(value, settings.snap);
		else if (strcmp(line, "modifier") == 0)
			settings.modifier = parse_modifier(value);
		else if (strcmp(line, "span") == 0)
			settings.span = parse_bool(value, settings.span);
		else if (strcmp(line, "show_grid") == 0)
			settings.showGrid = parse_bool(value, settings.showGrid);
		else if (strcmp(line, "gap") == 0) {
			int32 gap = atoi(value);
			settings.gap = gap < 0 ? 0 : gap > 64 ? 64 : gap;
		} else if (strcmp(line, "zone_shortcuts") == 0) {
			settings.zoneShortcuts = parse_bool(value,
				settings.zoneShortcuts);
		} else if (strcmp(line, "tab_menu") == 0)
			settings.tabMenu = parse_bool(value, settings.tabMenu);
		else if (strcmp(line, "rules") == 0)
			settings.rules = parse_bool(value, settings.rules);
		else if (strcmp(line, "switcher_plus") == 0)
			settings.switcherPlus = parse_bool(value, settings.switcherPlus);
		else if (strcmp(line, "edge_snap") == 0)
			settings.edgeSnap = parse_bool(value, settings.edgeSnap);
		else if (strcmp(line, "default") == 0) {
			copy_word(settings.defaultSpec, sizeof(settings.defaultSpec),
				value);
		} else if (strcmp(line, "display") == 0
			|| strcmp(line, "workspace") == 0
			|| strcmp(line, "name") == 0) {
			char* end;
			long index = strtol(value, &end, 10);
			if (end == value || *end != ' ')
				continue;
			end++;
			if (line[0] == 'd' && index >= 0 && index < kMaxDisplays) {
				copy_word(settings.displaySpec[index],
					sizeof(settings.displaySpec[index]), end);
			} else if (line[0] == 'w' && index >= 0
				&& index < kMaxWorkspaces) {
				copy_word(settings.workspaceSpec[index],
					sizeof(settings.workspaceSpec[index]), end);
			} else if (line[0] == 'n' && index >= 0
				&& index < kMaxWorkspaces) {
				strlcpy(settings.workspaceName[index], end,
					sizeof(settings.workspaceName[index]));
			}
		} else if (strcmp(line, "layout") == 0) {
			if (settings.layoutCount >= kMaxLayouts)
				continue;
			Layout& layout = settings.layouts[settings.layoutCount];
			copy_word(layout.name, sizeof(layout.name), value);
			int32 rows, columns;
			if (layout.name[0] == '\0' || parse_grid(layout.name, rows,
					columns))
				continue;
			const char* numbers = value + strlen(layout.name);
			layout.count = 0;
			while (layout.count < kMaxZones) {
				Zone zone;
				int used = 0;
				if (sscanf(numbers, " %f %f %f %f%n", &zone.left, &zone.top,
						&zone.right, &zone.bottom, &used) != 4)
					break;
				numbers += used;
				if (valid_zone(zone))
					layout.zones[layout.count++] = zone;
			}
			if (layout.count > 0)
				settings.layoutCount++;
		} else if (strcmp(line, "rule") == 0) {
			if (settings.ruleCount >= kMaxRules)
				continue;
			Rule& rule = settings.ruleList[settings.ruleCount];
			copy_word(rule.signature, sizeof(rule.signature), value);
			const char* zone = value + strlen(rule.signature);
			rule.zone = atoi(zone);
			if (rule.signature[0] != '\0' && rule.zone >= 1
				&& rule.zone <= kMaxZones)
				settings.ruleCount++;
		}
	}
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
	load_from(in, settings);
	fclose(in);
}


static inline void
write_workspaces(FILE* out, const Settings& settings)
{
	for (int32 i = 0; i < kMaxWorkspaces; i++) {
		if (settings.workspaceSpec[i][0] != '\0')
			fprintf(out, "workspace %d %s\n", (int)i, settings.workspaceSpec[i]);
		if (settings.workspaceName[i][0] != '\0')
			fprintf(out, "name %d %s\n", (int)i, settings.workspaceName[i]);
	}
}


static inline bool
write_file(const char* file, const Settings& settings, bool workspacesOnly,
	int32 workspaceCount)
{
	char dir[B_PATH_NAME_LENGTH];
	strlcpy(dir, file, sizeof(dir));
	char* slash = strrchr(dir, '/');
	if (slash != NULL) {
		*slash = '\0';
		mkdir(dir, 0755);
	}
	char temporary[B_PATH_NAME_LENGTH];
	snprintf(temporary, sizeof(temporary), "%s.new", file);
	FILE* out = fopen(temporary, "w");
	if (out == NULL)
		return false;
	if (workspacesOnly)
		fprintf(out, "count %d\n", (int)workspaceCount);
	else {
		fprintf(out, "snap %s\nmodifier %s\nspan %s\nshow_grid %s\ngap %d\n"
			"zone_shortcuts %s\ntab_menu %s\nrules %s\nswitcher_plus %s\n"
			"edge_snap %s\ndefault %s\n",
			settings.snap ? "true" : "false",
			modifier_name(settings.modifier),
			settings.span ? "true" : "false",
			settings.showGrid ? "true" : "false", (int)settings.gap,
			settings.zoneShortcuts ? "true" : "false",
			settings.tabMenu ? "true" : "false",
			settings.rules ? "true" : "false",
			settings.switcherPlus ? "true" : "false",
			settings.edgeSnap ? "true" : "false", settings.defaultSpec);
		for (int32 i = 0; i < kMaxDisplays; i++) {
			if (settings.displaySpec[i][0] != '\0')
				fprintf(out, "display %d %s\n", (int)i, settings.displaySpec[i]);
		}
		for (int32 i = 0; i < settings.layoutCount; i++) {
			const Layout& layout = settings.layouts[i];
			fprintf(out, "layout %s", layout.name);
			for (int32 z = 0; z < layout.count; z++) {
				fprintf(out, " %.4f %.4f %.4f %.4f", layout.zones[z].left,
					layout.zones[z].top, layout.zones[z].right,
					layout.zones[z].bottom);
			}
			fputc('\n', out);
		}
		for (int32 i = 0; i < settings.ruleCount; i++) {
			fprintf(out, "rule %s %d\n", settings.ruleList[i].signature,
				(int)settings.ruleList[i].zone);
		}
	}
	write_workspaces(out, settings);
	bool ok = fflush(out) == 0;
	fclose(out);
	return ok && rename(temporary, file) == 0;
}


//!	Written through a temporary file, never left half written.
static inline bool
save(const Settings& settings)
{
	char file[B_PATH_NAME_LENGTH];
	if (!path(file, sizeof(file)))
		return false;
	return write_file(file, settings, false, 0);
}


//!	The workspace set file of \a name ("" if the name is not usable).
static inline bool
set_path(const char* name, char* buffer, size_t size)
{
	if (name == NULL || name[0] == '\0' || strchr(name, '/') != NULL
		|| name[0] == '.')
		return false;
	char dir[B_PATH_NAME_LENGTH];
	if (!directory(dir, sizeof(dir)))
		return false;
	snprintf(buffer, size, "%s/workspace_sets/%s", dir, name);
	return true;
}


//!	Saves the workspace lines and \a count as the set \a name.
static inline bool
save_set(const char* name, const Settings& settings, int32 count)
{
	char file[B_PATH_NAME_LENGTH];
	if (!set_path(name, file, sizeof(file)))
		return false;
	return write_file(file, settings, true, count);
}


/*!	Replaces the workspace names and layouts of \a settings with those of
	the set \a name; returns its workspace count, or -1.
*/
static inline int32
load_set(const char* name, Settings& settings)
{
	char file[B_PATH_NAME_LENGTH];
	if (!set_path(name, file, sizeof(file)))
		return -1;
	FILE* in = fopen(file, "r");
	if (in == NULL)
		return -1;
	int count = -1;
	if (fscanf(in, "count %d\n", &count) != 1 || count < 1 || count > 32) {
		fclose(in);
		return -1;
	}
	memset(settings.workspaceSpec, 0, sizeof(settings.workspaceSpec));
	memset(settings.workspaceName, 0, sizeof(settings.workspaceName));
	load_from(in, settings);
	fclose(in);
	return count;
}


}	// namespace WindowLayouts


#endif	// _RENGA_WINDOW_LAYOUTS_H
