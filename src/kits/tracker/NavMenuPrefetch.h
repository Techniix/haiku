/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 */
#ifndef _NAV_MENU_PREFETCH_H
#define _NAV_MENU_PREFETCH_H


#include <Entry.h>
#include <Locker.h>
#include <Looper.h>
#include <Node.h>
#include <ObjectList.h>


namespace BPrivate {

class Model;


/*!	Folder menus built ahead of time (Renga, roadmap F-59).

	A BNavMenu (the folder submenus of Tracker's context menus, "Move to",
	"Copy to", the Desktop entry of the desktop's menu) is built in the
	menu's own thread when the pointer first rests on it: reading the
	folder, a Model per entry (stat, MIME type, the pose info attribute
	for hidden entries). On a folder with many entries, or a slow disk,
	the menu stalls for that long, on every hover, since the menus are
	built fresh each time.

	This cache does that work in a thread of its own, ahead of time: the
	desktop's folder when the desktop's context menu opens, and the
	subfolders of every folder menu that is shown. A BNavMenu then takes
	copies of the prebuilt Models (StartBuildingItemList()), which costs
	no file system access. Each cached folder is watched (node monitor,
	B_WATCH_DIRECTORY) and dropped when an entry is added, removed or
	renamed in it, so a menu never shows a stale list; at most kMaxFolders
	folders are kept (least recently used first out).

	Turned on and off with Tracker's "Prefetch folder menus" setting
	(TrackerSettings::PrefetchFolderMenus(), Tracker preferences, Windows
	page); off, menus are built as before and the cache is emptied.
*/
class NavMenuPrefetch : public BLooper {
public:
	static	NavMenuPrefetch*	Default();

			//!	Builds \a directory's entries in the background, unless
			//	cached or the setting is off.
			void				Prefetch(const entry_ref& directory);
			//!	Prefetch() for the subfolders among \a models (at most 32).
			void				PrefetchSubfolders(
									const BObjectList<Model, true>& models);

			//!	Copies of the cached Models of \a directory's visible
			//	entries; false if it is not cached (or being built).
			bool				Get(const entry_ref& directory,
									BObjectList<Model, true>& models);

			void				Clear();

			// statistics, for tests
			int32				Hits() const { return fHits; }
			int32				Misses() const { return fMisses; }
			int32				Builds() const { return fBuilds; }
			int32				CountFolders();

	virtual	void				MessageReceived(BMessage* message);

								NavMenuPrefetch();
									// use Default()
private:
	virtual						~NavMenuPrefetch();

			struct Folder;

			Folder*				_Find(const entry_ref& directory) const;
			Folder*				_FindNode(const node_ref& node) const;
			void				_Remove(Folder* folder);
	static	status_t			_WorkerEntry(void* data);
			void				_Worker();
			void				_Build(const entry_ref& directory);

private:
			BLocker				fLock;
			BObjectList<Folder, true> fFolders;	// most recently used last
			BObjectList<entry_ref, true> fQueue;
			sem_id				fQueueSem;
			thread_id			fWorker;
			int32				fHits;
			int32				fMisses;
			int32				fBuilds;
			node_ref			fBuildingNode;
			bool				fBuildChanged;
	volatile bool				fQuitting;
};

}	// namespace BPrivate

using namespace BPrivate;


#endif	// _NAV_MENU_PREFETCH_H
