/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 */
#ifndef _FOLDER_SIZE_CACHE_H
#define _FOLDER_SIZE_CACHE_H


/*!	Folder sizes in the Size column, computed in the background (roadmap
	F-66, batch 9).

	A folder's size is the sum of the sizes of the files below it (symbolic
	links and other volumes mounted inside are not followed). Computing it
	reads the whole tree, so it happens on a thread of low priority, one
	folder at a time; the Size column shows "-" until the size is known and
	then the size. When a size is ready the cache posts kFolderSizeReady to
	the application, which passes it on to every window; the pose view
	updates that folder's row.

	A known size is shown at once; after kMaxAge it is computed again in
	the background on the next request (the old value stays until then), so
	sizes follow changes deep inside a folder without watching every
	subfolder. Folders on network volumes (shared, or a network file system)
	are never read. At most kMaxEntries entries are read for one folder;
	bigger trees stay "-".

	Turned on and off with "Show folder sizes" (TrackerSettings::
	ShowFolderSizes(), Tracker preferences, Windows page; on by default).
*/


#include <Locker.h>
#include <Node.h>
#include <ObjectList.h>

#include <map>


namespace BPrivate {


const uint32 kFolderSizeReady = 'Tfsz';
	// "device" (int32), "node" (int64): that folder's size is known


class FolderSizeCache {
public:
	static	FolderSizeCache*	Default();

			//!	The folder's size: true with \a _size when it is known
			//	(perhaps old, then it is computed again in the background);
			//	false while it is computed, or when it is never computed (a
			//	network volume, the setting off, too many entries).
			bool				Lookup(const node_ref& folder, off_t* _size);

			void				Clear();

			// for the tests
			int32				CountComputed() const { return fComputed; }
			int32				CountQueued();
	static	bool				IsNetworkVolume(dev_t device);

								FolderSizeCache();
									// use Default()
								~FolderSizeCache();

private:

	static	status_t			_WorkerEntry(void* data);
			void				_Worker();
			bool				_Compute(const node_ref& folder,
									off_t* _size);

private:
			struct Entry {
				off_t		size;
				bigtime_t	computed;
				bool		known;
					// false: too big, or not readable
			};

			BLocker				fLock;
			std::map<node_ref, Entry> fEntries;
			BObjectList<node_ref, true> fQueue;
			sem_id				fQueueSem;
			thread_id			fWorker;
			int32				fComputed;
			std::map<dev_t, bool> fNetworkVolumes;
};


}	// namespace BPrivate

using namespace BPrivate;


#endif	// _FOLDER_SIZE_CACHE_H
