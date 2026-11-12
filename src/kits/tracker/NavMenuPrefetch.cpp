/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 */


#include "NavMenuPrefetch.h"

#include <Autolock.h>
#include <Directory.h>
#include <NodeMonitor.h>

#include <pthread.h>

#include "Attributes.h"
#include "Model.h"
#include "PoseView.h"
#include "TrackerSettings.h"


namespace {

const int32 kMaxFolders = 64;
const int32 kMaxSubfolders = 32;
const int32 kMaxEntries = 4096;
	// larger folders are not prefetched (the menu builds them as before)

}	// namespace


struct NavMenuPrefetch::Folder {
	entry_ref			ref;
	node_ref			node;
	bool				hideDotFiles;
	BObjectList<Model, true> models;

	Folder()
		:
		models(32)
	{
	}
};


static NavMenuPrefetch* sDefault = NULL;
static pthread_once_t sDefaultOnce = PTHREAD_ONCE_INIT;


static void
create_default()
{
	sDefault = new NavMenuPrefetch();
	sDefault->Run();
}


NavMenuPrefetch*
NavMenuPrefetch::Default()
{
	pthread_once(&sDefaultOnce, &create_default);
	return sDefault;
}


NavMenuPrefetch::NavMenuPrefetch()
	:
	BLooper("folder menu prefetch", B_LOW_PRIORITY),
	fLock("folder menu prefetch"),
	fFolders(16),
	fQueue(16),
	fHits(0),
	fMisses(0),
	fBuilds(0),
	fBuildChanged(false),
	fQuitting(false)
{
	fQueueSem = create_sem(0, "folder menu prefetch queue");
	fWorker = spawn_thread(_WorkerEntry, "folder menu prefetch worker",
		B_LOW_PRIORITY, this);
	resume_thread(fWorker);
}


NavMenuPrefetch::~NavMenuPrefetch()
{
	fQuitting = true;
	delete_sem(fQueueSem);
	status_t result;
	wait_for_thread(fWorker, &result);
	stop_watching(BMessenger(this));
}


void
NavMenuPrefetch::Prefetch(const entry_ref& directory)
{
	if (!TrackerSettings().PrefetchFolderMenus()) {
		if (fFolders.CountItems() > 0)
			Clear();
		return;
	}

	BAutolock locker(fLock);
	if (_Find(directory) != NULL)
		return;
	for (int32 i = 0; i < fQueue.CountItems(); i++) {
		if (*fQueue.ItemAt(i) == directory)
			return;
	}
	fQueue.AddItem(new entry_ref(directory));
	release_sem(fQueueSem);
}


void
NavMenuPrefetch::PrefetchSubfolders(const BObjectList<Model, true>& models)
{
	int32 count = 0;
	for (int32 i = 0; i < models.CountItems() && count < kMaxSubfolders; i++) {
		Model* model = models.ItemAt(i);
		if (model->IsDirectory() && !model->IsQuery()
			&& !model->IsVirtualDirectory() && !model->IsTrash()) {
			Prefetch(*model->EntryRef());
			count++;
		}
	}
}


bool
NavMenuPrefetch::Get(const entry_ref& directory,
	BObjectList<Model, true>& models)
{
	if (!TrackerSettings().PrefetchFolderMenus())
		return false;

	bool hideDotFiles = TrackerSettings().HideDotFiles();
	BAutolock locker(fLock);
	Folder* folder = _Find(directory);
	if (folder == NULL || folder->hideDotFiles != hideDotFiles) {
		atomic_add(&fMisses, 1);
		return false;
	}
	for (int32 i = 0; i < folder->models.CountItems(); i++)
		models.AddItem(new Model(*folder->models.ItemAt(i)));
	// most recently used last
	fFolders.RemoveItem(folder, false);
	fFolders.AddItem(folder);
	atomic_add(&fHits, 1);
	return true;
}


void
NavMenuPrefetch::Clear()
{
	BAutolock locker(fLock);
	while (Folder* folder = fFolders.ItemAt(0))
		_Remove(folder);
	fQueue.MakeEmpty();
}


int32
NavMenuPrefetch::CountFolders()
{
	BAutolock locker(fLock);
	return fFolders.CountItems();
}


void
NavMenuPrefetch::MessageReceived(BMessage* message)
{
	if (message->what != B_NODE_MONITOR) {
		BLooper::MessageReceived(message);
		return;
	}

	// An entry came, went or was renamed in a watched folder: drop it.
	node_ref node;
	if (message->FindInt32("device", &node.device) != B_OK)
		return;
	static const char* kFields[] = { "directory", "from directory",
		"to directory" };
	BAutolock locker(fLock);
	for (size_t i = 0; i < sizeof(kFields) / sizeof(kFields[0]); i++) {
		if (message->FindInt64(kFields[i], &node.node) != B_OK)
			continue;
		if (Folder* folder = _FindNode(node))
			_Remove(folder);
		if (node == fBuildingNode)
			fBuildChanged = true;
	}
}


NavMenuPrefetch::Folder*
NavMenuPrefetch::_Find(const entry_ref& directory) const
{
	for (int32 i = 0; i < fFolders.CountItems(); i++) {
		if (fFolders.ItemAt(i)->ref == directory)
			return fFolders.ItemAt(i);
	}
	return NULL;
}


NavMenuPrefetch::Folder*
NavMenuPrefetch::_FindNode(const node_ref& node) const
{
	for (int32 i = 0; i < fFolders.CountItems(); i++) {
		if (fFolders.ItemAt(i)->node == node)
			return fFolders.ItemAt(i);
	}
	return NULL;
}


//!	fLock held.
void
NavMenuPrefetch::_Remove(Folder* folder)
{
	watch_node(&folder->node, B_STOP_WATCHING, BMessenger(this));
	fFolders.RemoveItem(folder, true);
}


status_t
NavMenuPrefetch::_WorkerEntry(void* data)
{
	((NavMenuPrefetch*)data)->_Worker();
	return B_OK;
}


void
NavMenuPrefetch::_Worker()
{
	while (!fQuitting) {
		if (acquire_sem(fQueueSem) != B_OK)
			break;
		entry_ref ref;
		{
			BAutolock locker(fLock);
			entry_ref* next = fQueue.RemoveItemAt(0);
			if (next == NULL)
				continue;
			ref = *next;
			delete next;
		}
		_Build(ref);
	}
}


/*!	What BNavMenu::AddNextItem() does for each entry, ahead of time: the
	visible entries' Models, with their nodes closed.
*/
void
NavMenuPrefetch::_Build(const entry_ref& ref)
{
	BDirectory directory(&ref);
	node_ref node;
	if (directory.InitCheck() != B_OK || directory.GetNodeRef(&node) != B_OK)
		return;

	// Watch first: a change while reading drops the result.
	{
		BAutolock locker(fLock);
		fBuildingNode = node;
		fBuildChanged = false;
	}
	if (watch_node(&node, B_WATCH_DIRECTORY, BMessenger(this)) != B_OK)
		return;

	Folder* folder = new Folder;
	folder->ref = ref;
	folder->node = node;
	folder->hideDotFiles = TrackerSettings().HideDotFiles();

	BEntry entry;
	int32 count = 0;
	while (!fQuitting && directory.GetNextEntry(&entry) == B_OK) {
		if (++count > kMaxEntries) {
			delete folder;
			watch_node(&node, B_STOP_WATCHING, BMessenger(this));
			return;
		}
		if (folder->hideDotFiles) {
			char name[B_FILE_NAME_LENGTH];
			if (entry.GetName(name) == B_OK && name[0] == '.')
				continue;
		}
		Model* model = new Model(&entry, true);
		if (model->InitCheck() != B_OK || model->IsTrash()) {
			delete model;
			continue;
		}
		ssize_t size = -1;
		PoseInfo poseInfo;
		if (model->Node() != NULL) {
			size = model->Node()->ReadAttr(kAttrPoseInfo, B_RAW_TYPE, 0,
				&poseInfo, sizeof(poseInfo));
		}
		model->CloseNode();
		if (size == sizeof(poseInfo) && !BPoseView::PoseVisible(model,
				&poseInfo)) {
			delete model;
			continue;
		}
		folder->models.AddItem(model);
	}

	// Give the node monitor a moment to report a change made during the
	// read (its message is handled by this looper's thread).
	snooze(20000);
	BAutolock locker(fLock);
	fBuildingNode = node_ref();
	if (fQuitting || fBuildChanged || _FindNode(node) != NULL) {
		if (_FindNode(node) == NULL)
			watch_node(&node, B_STOP_WATCHING, BMessenger(this));
		delete folder;
		return;
	}
	// A message for a change during the read that arrives after this point
	// still finds the folder and drops it; a change before the watch
	// started is in the read.
	fFolders.AddItem(folder);
	atomic_add(&fBuilds, 1);
	while (fFolders.CountItems() > kMaxFolders)
		_Remove(fFolders.ItemAt(0));
}
