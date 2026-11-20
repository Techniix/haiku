/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 */
#ifndef WINDOWS_PAGE_H
#define WINDOWS_PAGE_H


#include <GroupView.h>


/*!	Control Panel page "Windows and workspaces" (Renga, batch 7; roadmap
	F-25): snap layouts (switch, modifier, spanning, grid, gap, default,
	per-screen and per-workspace layouts, the zone editor, zone shortcuts,
	the window menu, per-application rules, saved arrangements) and
	visible workspaces (count, names, wallpaper, the Deskbar switcher and
	its "+", the overview, saved sets). Every setting is in
	<window_layouts.h>'s file; app_server picks changes up within a
	second.
*/
BView*	create_windows_page();


#endif	// WINDOWS_PAGE_H
