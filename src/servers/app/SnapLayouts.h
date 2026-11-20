/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 */
#ifndef SNAP_LAYOUTS_H
#define SNAP_LAYOUTS_H


#include <Point.h>
#include <Rect.h>
#include <SupportDefs.h>


class Desktop;
class Window;


/*!	Snap layouts (Renga, batch 7; roadmap F-25, Lumina rebuilt in
	app_server): zones on each screen and workspace, chosen while dragging
	a window with the snap modifier, by Command+Control+digit, by the
	window menu on the tab and by per-application rules. The settings are
	<window_layouts.h>'s file, re-read when it changes (checked at most once
	a second), so the Control Panel's switches act without a restart.
*/
namespace SnapLayouts {

enum { kMaxFrames = 36 };

//!	Whether a drag with these modifiers chooses a zone; \a span tells
//!	whether the span key is held as well.
bool			DragActive(int32 modifiers, bool* span);

//!	Zones of the screen containing \a where, for the current workspace.
int32			ZoneFrames(Desktop* desktop, BPoint where, BRect* frames,
					int32 maxFrames);

//!	Shows the grid and the chosen zone; \a chosen invalid shows the grid
//!	only.
void			ShowOverlay(Desktop* desktop, const BRect* frames,
					int32 count, BRect chosen);
void			HideOverlay(Desktop* desktop);

//!	Moves and resizes \a window so its decorated frame fills \a zone;
//!	remembers the frame from before (or \a restoreFrame, the frame where a
//!	drag started), for RestoreWindow().
void			PlaceWindow(Desktop* desktop, Window* window, BRect zone,
					const BRect* restoreFrame = NULL);
bool			RestoreWindow(Desktop* desktop, Window* window);
//!	Zone \a index (0-based) of the window's screen and workspace.
status_t		PlaceWindowInZone(Desktop* desktop, Window* window,
					int32 index);

//!	Command+Control+1..9/0 for the focus window; the overview key.
//!	Called without the window lock. Returns true if the key was used.
bool			HandleKey(Desktop* desktop, int32 key, int32 modifiers);

//!	A window is shown for the first time: per-application rules.
//!	Called with the window lock held for writing.
void			WindowShown(Desktop* desktop, Window* window);
void			WindowRemoved(Window* window);

bool			TabMenuEnabled();
//!	Sends the window menu request to the window's application.
void			ShowWindowMenu(Desktop* desktop, Window* window,
					BPoint where);

//!	Saved arrangements: the frames of the current workspace's windows.
status_t		SaveArrangement(Desktop* desktop, const char* name);
status_t		RestoreArrangement(Desktop* desktop, const char* name,
					int32* _restored);

}	// namespace SnapLayouts


#endif	// SNAP_LAYOUTS_H
