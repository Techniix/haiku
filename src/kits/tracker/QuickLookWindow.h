/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 */
#ifndef _QUICK_LOOK_WINDOW_H
#define _QUICK_LOOK_WINDOW_H


/*!	Quick Look (roadmap F-66, batch 9): the space bar in a Tracker window
	shows the selected item in a small floating window without opening a
	program; Space or Escape closes it again.

	  images   through the Translation Kit, scaled to fit (any format a
	           translator reads)
	  text     the first 64 KiB, read only
	  folders  the number of items and the size (FolderSizeCache)
	  others   the large icon with kind, size and dates

	Audio, video, PDF and fonts show their icon and information for now
	(previews for them are a later step, see the roadmap).

	The space bar still types when a name is being typed (type-ahead
	selection within the double-click time, or type-ahead filtering with
	text), so names with spaces can be found. Turned on and off with "Quick
	Look with the space bar" (TrackerSettings::QuickLook(), Tracker
	preferences, Windows page; on by default).
*/


#include <Entry.h>
#include <Window.h>


namespace BPrivate {


const uint32 kQuickLook = 'TQlk';
	// to a pose view: show or hide Quick Look for the selection


class QuickLookWindow : public BWindow {
public:
								QuickLookWindow(const entry_ref& ref);
	virtual						~QuickLookWindow();

	//!	Shows \a ref, or closes Quick Look when it shows \a ref already.
	static	void				Toggle(const entry_ref& ref);
	static	void				Close();
	//!	"open", "title", "kind" (image, text, folder, info), for the tests.
	static	void				GetState(BMessage* reply);

	virtual	void				MessageReceived(BMessage* message);
	virtual	bool				QuitRequested();

private:
			void				_Build();

private:
			entry_ref			fRef;
			const char*			fKind;
};


}	// namespace BPrivate

using namespace BPrivate;


#endif	// _QUICK_LOOK_WINDOW_H
