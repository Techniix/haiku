/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 */


#include "FolderSizeCache.h"

#include <dirent.h>
#include <fs_info.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include <Application.h>
#include <Autolock.h>
#include <Directory.h>
#include <Entry.h>
#include <Message.h>

#include <vector>

#include "TrackerSettings.h"


static const bigtime_t kMaxAge = 30000000;
	// a known size older than this is computed again on the next request
static const int32 kMaxEntries = 500000;
static const size_t kMaxCachedFolders = 4096;


static FolderSizeCache* sDefault = NULL;
static pthread_once_t sDefaultOnce = PTHREAD_ONCE_INIT;


static void
create_default()
{
	sDefault = new FolderSizeCache();
}


FolderSizeCache*
FolderSizeCache::Default()
{
	pthread_once(&sDefaultOnce, &create_default);
	return sDefault;
}


FolderSizeCache::FolderSizeCache()
	:
	fLock("folder sizes"),
	fQueue(16),
	fQueueSem(create_sem(0, "folder sizes queue")),
	fWorker(-1),
	fComputed(0)
{
}


FolderSizeCache::~FolderSizeCache()
{
	delete_sem(fQueueSem);
	if (fWorker >= 0) {
		status_t result;
		wait_for_thread(fWorker, &result);
	}
}


/*static*/ bool
FolderSizeCache::IsNetworkVolume(dev_t device)
{
	fs_info info;
	if (fs_stat_dev(device, &info) != 0)
		return true;
	if ((info.flags & B_FS_IS_SHARED) != 0)
		return true;
	static const char* kNetworkFileSystems[] = {
		"nfs", "nfs4", "netfs", "smbfs", "cifs", "ftpfs", "httpfs",
		"webdav", "sshfs", "userlandfs"
	};
	for (size_t i = 0;
			i < sizeof(kNetworkFileSystems) / sizeof(kNetworkFileSystems[0]);
			i++) {
		if (strcmp(info.fsh_name, kNetworkFileSystems[i]) == 0)
			return true;
	}
	return false;
}


bool
FolderSizeCache::Lookup(const node_ref& folder, off_t* _size)
{
	if (!TrackerSettings().ShowFolderSizes())
		return false;

	BAutolock _(fLock);

	std::map<dev_t, bool>::iterator volume
		= fNetworkVolumes.find(folder.device);
	bool network;
	if (volume == fNetworkVolumes.end()) {
		network = IsNetworkVolume(folder.device);
		fNetworkVolumes[folder.device] = network;
	} else
		network = volume->second;
	if (network)
		return false;

	bigtime_t now = system_time();
	std::map<node_ref, Entry>::iterator found = fEntries.find(folder);
	bool known = false;
	bool queue = true;
	if (found != fEntries.end()) {
		Entry& entry = found->second;
		if (entry.known) {
			*_size = entry.size;
			known = true;
		}
		queue = entry.computed >= 0 && now - entry.computed > kMaxAge;
		if (queue)
			entry.computed = -1;
				// queued: not again until it is done
	} else {
		if (fEntries.size() >= kMaxCachedFolders)
			fEntries.clear();
		Entry entry = { 0, -1, false };
		fEntries[folder] = entry;
	}

	if (queue) {
		fQueue.AddItem(new node_ref(folder));
		release_sem_etc(fQueueSem, 1, B_DO_NOT_RESCHEDULE);
		if (fWorker < 0) {
			fWorker = spawn_thread(&_WorkerEntry, "folder sizes",
				B_LOW_PRIORITY, this);
			if (fWorker >= 0)
				resume_thread(fWorker);
		}
	}
	return known;
}


void
FolderSizeCache::Clear()
{
	BAutolock _(fLock);
	fEntries.clear();
	fQueue.MakeEmpty();
	fNetworkVolumes.clear();
}


int32
FolderSizeCache::CountQueued()
{
	BAutolock _(fLock);
	return fQueue.CountItems();
}


/*static*/ status_t
FolderSizeCache::_WorkerEntry(void* data)
{
	((FolderSizeCache*)data)->_Worker();
	return B_OK;
}


void
FolderSizeCache::_Worker()
{
	while (acquire_sem(fQueueSem) == B_OK) {
		node_ref folder;
		{
			BAutolock _(fLock);
			node_ref* next = fQueue.RemoveItemAt(0);
			if (next == NULL)
				continue;
			folder = *next;
			delete next;
		}
		if (!TrackerSettings().ShowFolderSizes())
			continue;

		off_t size = 0;
		bool known = _Compute(folder, &size);

		{
			BAutolock _(fLock);
			Entry entry = { size, system_time(), known };
			fEntries[folder] = entry;
			fComputed++;
		}

		BMessage ready(kFolderSizeReady);
		ready.AddInt32("device", folder.device);
		ready.AddInt64("node", folder.node);
		if (be_app != NULL)
			be_app->PostMessage(&ready);
	}
}


/*!	Adds up the sizes of the files below \a folder, without following
	symbolic links or going into other volumes. False when the folder cannot
	be read or has more than kMaxEntries entries.
*/
bool
FolderSizeCache::_Compute(const node_ref& folder, off_t* _size)
{
	BDirectory root(&folder);
	if (root.InitCheck() != B_OK)
		return false;

	std::vector<BDirectory*> stack;
	stack.push_back(new BDirectory(root));
	off_t size = 0;
	int32 entries = 0;
	bool complete = true;

	while (!stack.empty()) {
		BDirectory* directory = stack.back();
		stack.pop_back();

		char buffer[4096];
		dirent* dirents = (dirent*)buffer;
		int32 count;
		while ((count = directory->GetNextDirents(dirents, sizeof(buffer)))
				> 0 && complete) {
			dirent* current = dirents;
			for (int32 i = 0; i < count; i++) {
				if (strcmp(current->d_name, ".") != 0
					&& strcmp(current->d_name, "..") != 0) {
					if (++entries > kMaxEntries) {
						complete = false;
						break;
					}
					struct stat st;
					if (directory->GetStatFor(current->d_name, &st) == B_OK) {
						if (S_ISDIR(st.st_mode)) {
							if (st.st_dev == folder.device) {
								BDirectory* sub = new BDirectory(directory,
									current->d_name);
								if (sub->InitCheck() == B_OK)
									stack.push_back(sub);
								else
									delete sub;
							}
						} else if (S_ISREG(st.st_mode))
							size += st.st_size;
					}
				}
				current = (dirent*)((uint8*)current + current->d_reclen);
			}
		}
		delete directory;
		if (!complete)
			break;
	}

	for (size_t i = 0; i < stack.size(); i++)
		delete stack[i];

	*_size = size;
	return complete;
}
