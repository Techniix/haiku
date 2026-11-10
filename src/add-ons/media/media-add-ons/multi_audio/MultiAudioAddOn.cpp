/*
 * Copyright (c) 2002, Jerome Duval (jerome.duval@free.fr)
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 */


#include "MultiAudioAddOn.h"

#include <limits.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include <Autolock.h>
#include <Directory.h>
#include <Entry.h>
#include <FindDirectory.h>
#include <File.h>
#include <Looper.h>
#include <MessageRunner.h>
#include <Path.h>
#include <PathMonitor.h>

#include "debug.h"
#include "MultiAudioNode.h"
#include "MultiAudioDevice.h"


#define MULTI_SAVE

const char* kSettingsName = "Media/multi_audio_settings";
static const char* kDevicesRoot = "/dev/audio/hmulti/";


/*!	Renga: media_addon_server keeps add-ons exporting this symbol loaded
	even while they have no node, and creates and releases the nodes of
	their physical inputs and outputs whenever they report a change of
	flavors (NotifyFlavorChange()). Without it, an add-on without a sound
	card at startup was unloaded and never saw one plugged in later.
	Additive: servers that do not know the symbol ignore it.
*/
extern "C" _EXPORT const uint32 renga_media_addon_watches_devices = 1;


//! instantiation function
extern "C" BMediaAddOn*
make_media_addon(image_id image)
{
	CALLED();
	return new MultiAudioAddOn(image);
}


//	#pragma mark - DeviceWatcher


static const uint32 kMsgRescan = 'rscn';


/*!	Renga: watches /dev/audio/hmulti (and every directory below it) for
	device nodes drivers publish and unpublish, as USB audio devices, and
	any other hot-pluggable card, do when they come and go. A change
	triggers a rescan shortly after (drivers publish all their nodes at
	once; the rescan waits for the burst to end). A device that does not
	open yet (the driver is still setting it up) is tried again.
*/
class DeviceWatcher : public BLooper {
public:
	DeviceWatcher(MultiAudioAddOn* addOn)
		:
		BLooper("multi_audio device watcher"),
		fAddOn(addOn),
		fRunner(NULL)
	{
	}

	virtual ~DeviceWatcher()
	{
		delete fRunner;
	}

	void Watch()
	{
		status_t status = BPathMonitor::StartWatching(kDevicesRoot,
			B_WATCH_FILES_ONLY | B_WATCH_RECURSIVELY, BMessenger(this));
		if (status != B_OK) {
			fprintf(stderr, "multi_audio: cannot watch %s: %s\n",
				kDevicesRoot, strerror(status));
		}
	}

	void StopWatching()
	{
		BPathMonitor::StopWatching(kDevicesRoot, BMessenger(this));
	}

	void ScheduleRescan(bigtime_t delay)
	{
		delete fRunner;
		BMessage message(kMsgRescan);
		fRunner = new(std::nothrow) BMessageRunner(BMessenger(this),
			&message, delay, 1);
	}

	virtual void MessageReceived(BMessage* message)
	{
		switch (message->what) {
			case B_PATH_MONITOR:
				ScheduleRescan(300000);
				break;
			case kMsgRescan:
				fAddOn->Rescan();
				break;
			default:
				BLooper::MessageReceived(message);
				break;
		}
	}

private:
	MultiAudioAddOn*	fAddOn;
	BMessageRunner*		fRunner;
};


//	#pragma mark -


MultiAudioAddOn::MultiAudioAddOn(image_id image)
	:
	BMediaAddOn(image),
	fLock("multi_audio devices"),
	fNextID(0),
	fWatcher(NULL)
{
	CALLED();

	// The cards there at startup get the flavor IDs 0, 1, ... in the order
	// they are found, as before, so that saved defaults still match.
	BMessage found;
	_Scan(kDevicesRoot, found);
	const char* path;
	for (int32 i = 0; found.FindString("path", i, &path) == B_OK; i++)
		_AddDevice(path);

	_LoadSettings();
	fInitStatus = B_OK;

	DeviceWatcher* watcher = new(std::nothrow) DeviceWatcher(this);
	if (watcher != NULL) {
		watcher->Run();
		watcher->Watch();
		fWatcher = watcher;
	}
}


MultiAudioAddOn::~MultiAudioAddOn()
{
	CALLED();

	if (fWatcher != NULL && fWatcher->Lock()) {
		((DeviceWatcher*)fWatcher)->StopWatching();
		fWatcher->Quit();
	}

	for (int32 i = 0; i < fDevices.CountItems(); i++) {
		DeviceEntry* entry = (DeviceEntry*)fDevices.ItemAt(i);
		delete entry->device;
		delete entry;
	}
	for (int32 i = 0; i < fGone.CountItems(); i++) {
		DeviceEntry* entry = (DeviceEntry*)fGone.ItemAt(i);
		delete entry->device;
		delete entry;
	}

	_SaveSettings();
}


status_t
MultiAudioAddOn::InitCheck(const char** _failureText)
{
	CALLED();
	return fInitStatus;
}


int32
MultiAudioAddOn::CountFlavors()
{
	CALLED();
	BAutolock locker(fLock);
	return fDevices.CountItems();
}


status_t
MultiAudioAddOn::GetFlavorAt(int32 index, const flavor_info** _info)
{
	CALLED();
	BAutolock locker(fLock);

	DeviceEntry* entry = (DeviceEntry*)fDevices.ItemAt(index);
	if (entry == NULL)
		return B_BAD_INDEX;

	flavor_info* info = new (std::nothrow) flavor_info;
	if (info == NULL)
		return B_NO_MEMORY;

	// Renga: the flavor's internal_id stays the device's for as long as it
	// is plugged in, also when other devices come and go (it was the index
	// in the list, which a removed device shifted).
	MultiAudioNode::GetFlavor(info, entry->id);
	info->name = const_cast<char*>(entry->name.String());

	*_info = info;
	return B_OK;
}


BMediaNode*
MultiAudioAddOn::InstantiateNodeFor(const flavor_info* info, BMessage* config,
	status_t* _error)
{
	CALLED();
	BAutolock locker(fLock);

	DeviceEntry* entry = _FindEntry(info->internal_id);
	if (entry == NULL) {
		*_error = B_ERROR;
		return NULL;
	}

	// The node, and with it its saved settings, carry the flavor's name,
	// which tells identical cards apart (see _AddDevice()).
	const char* name = entry->name.String();

#ifdef MULTI_SAVE
	if (fSettings.FindMessage(name, config) == B_OK)
		fSettings.RemoveData(name);
#endif

	MultiAudioNode* node = new (std::nothrow) MultiAudioNode(this,
		name, entry->device, entry->id, config);
	if (node == NULL)
		*_error = B_NO_MEMORY;
	else {
		// counted now, since a node that fails InitCheck() is deleted
		// (NodeDeleted()) like any other
		entry->nodes++;
		*_error = node->InitCheck();
	}

	return node;
}


status_t
MultiAudioAddOn::GetConfigurationFor(BMediaNode* _node, BMessage* message)
{
	CALLED();
	MultiAudioNode* node = dynamic_cast<MultiAudioNode*>(_node);
	if (node == NULL)
		return B_BAD_TYPE;

#ifdef MULTI_SAVE
	if (message == NULL) {
		BMessage settings;
		if (node->GetConfigurationFor(&settings) == B_OK) {
			BAutolock locker(fLock);
			fSettings.RemoveName(node->Name());
			fSettings.AddMessage(node->Name(), &settings);
		}
		return B_OK;
	}
#endif

	// currently never called by the media kit. Seems it is not implemented.

	return node->GetConfigurationFor(message);
}


bool
MultiAudioAddOn::WantsAutoStart()
{
	CALLED();
	return false;
}


status_t
MultiAudioAddOn::AutoStart(int count, BMediaNode** _node, int32* _internalID,
	bool* _hasMore)
{
	CALLED();
	return B_OK;
}


/*!	Compares the devices published now with the known ones: new ones become
	flavors, unplugged ones stop being flavors (their device object lives
	on until their node is gone, NodeDeleted()). Tells media_addon_server
	if anything changed, which then creates and releases the nodes.
*/
void
MultiAudioAddOn::Rescan()
{
	BMessage found;
	_Scan(kDevicesRoot, found);

	bool changed = false;
	bool retry = false;
	{
		BAutolock locker(fLock);
		for (int32 i = fDevices.CountItems() - 1; i >= 0; i--) {
			DeviceEntry* entry = (DeviceEntry*)fDevices.ItemAt(i);
			bool present = false;
			const char* path;
			for (int32 j = 0; found.FindString("path", j, &path) == B_OK;
					j++) {
				present |= entry->path == path;
			}
			if (present)
				continue;
			fprintf(stderr, "multi_audio: %s (%s) is gone\n",
				entry->name.String(), entry->path.String());
			fDevices.RemoveItem(i);
			if (entry->nodes > 0)
				fGone.AddItem(entry);
			else {
				delete entry->device;
				delete entry;
			}
			changed = true;
		}

		const char* path;
		for (int32 j = 0; found.FindString("path", j, &path) == B_OK; j++) {
			bool known = false;
			for (int32 i = 0; i < fDevices.CountItems(); i++)
				known |= ((DeviceEntry*)fDevices.ItemAt(i))->path == path;
			if (known)
				continue;
			if (_AddDevice(path)) {
				fprintf(stderr, "multi_audio: %s is new\n", path);
				changed = true;
			} else
				retry = true;
		}
	}

	if (changed)
		NotifyFlavorChange();
	if (retry && fWatcher != NULL)
		((DeviceWatcher*)fWatcher)->ScheduleRescan(1000000);
}


void
MultiAudioAddOn::NodeDeleted(MultiAudioDevice* device)
{
	BAutolock locker(fLock);
	for (int32 i = 0; i < fDevices.CountItems(); i++) {
		DeviceEntry* entry = (DeviceEntry*)fDevices.ItemAt(i);
		if (entry->device == device && entry->nodes > 0)
			entry->nodes--;
	}
	for (int32 i = 0; i < fGone.CountItems(); i++) {
		DeviceEntry* entry = (DeviceEntry*)fGone.ItemAt(i);
		if (entry->device != device)
			continue;
		if (--entry->nodes > 0)
			break;
		// Closing the device lets its driver free what it kept for it.
		fGone.RemoveItem(i);
		delete entry->device;
		delete entry;
		break;
	}
}


void
MultiAudioAddOn::DeviceFailed(MultiAudioDevice* device)
{
	if (fWatcher != NULL && fWatcher->Lock()) {
		((DeviceWatcher*)fWatcher)->ScheduleRescan(100000);
		fWatcher->Unlock();
	}
}


void
MultiAudioAddOn::_Scan(const char* path, BMessage& found, uint32 depth)
{
	if (depth > 16)
		return;
	BDirectory directory(path);
	BEntry entry;
	while (directory.GetNextEntry(&entry) == B_OK) {
		BPath child;
		if (entry.GetPath(&child) != B_OK)
			continue;
		if (entry.IsDirectory())
			_Scan(child.Path(), found, depth + 1);
		else
			found.AddString("path", child.Path());
	}
}


/*!	Opens the device at \a path and makes it a flavor. Two cards of one
	kind (two virtio sound cards, two identical USB headsets) would
	otherwise be listed under one name, and nobody could tell them apart in
	the Media preferences or choose one as a stream's output: the first
	keeps the plain name, the others get " 2", " 3", ... A device plugged
	in again gets its old name back (the lowest free one), so routes and
	defaults saved by name still find it.
*/
bool
MultiAudioAddOn::_AddDevice(const char* path)
{
	MultiAudioDevice* device = new(std::nothrow) MultiAudioDevice(
		path + strlen(kDevicesRoot), path);
	if (device == NULL)
		return false;
	if (device->InitCheck() != B_OK) {
		delete device;
		return false;
	}

	const char* base = device->Description().friendly_name;
	BString name;
	for (int32 number = 1; ; number++) {
		name = base;
		if (number > 1)
			name << " " << number;
		bool taken = false;
		for (int32 i = 0; i < fDevices.CountItems(); i++)
			taken |= ((DeviceEntry*)fDevices.ItemAt(i))->name == name;
		if (!taken)
			break;
	}

	DeviceEntry* entry = new(std::nothrow) DeviceEntry;
	if (entry == NULL) {
		delete device;
		return false;
	}
	entry->device = device;
	entry->id = fNextID++;
	entry->path = path;
	entry->name = name;
	entry->nodes = 0;
	fDevices.AddItem(entry);
	return true;
}


MultiAudioAddOn::DeviceEntry*
MultiAudioAddOn::_FindEntry(int32 id)
{
	for (int32 i = 0; i < fDevices.CountItems(); i++) {
		DeviceEntry* entry = (DeviceEntry*)fDevices.ItemAt(i);
		if (entry->id == id)
			return entry;
	}
	return NULL;
}


void
MultiAudioAddOn::_SaveSettings()
{
	CALLED();
	BPath path;
	if (find_directory(B_USER_SETTINGS_DIRECTORY, &path) != B_OK)
		return;

	path.Append(kSettingsName);

	BFile file(path.Path(), B_READ_WRITE | B_CREATE_FILE | B_ERASE_FILE);
	if (file.InitCheck() == B_OK)
		fSettings.Flatten(&file);
}


void
MultiAudioAddOn::_LoadSettings()
{
	CALLED();
	fSettings.MakeEmpty();

	BPath path;
	if (find_directory(B_USER_SETTINGS_DIRECTORY, &path) != B_OK)
		return;

	path.Append(kSettingsName);

	BFile file(path.Path(), B_READ_ONLY);
	if (file.InitCheck() == B_OK && fSettings.Unflatten(&file) == B_OK) {
		PRINT_OBJECT(fSettings);
	} else {
		PRINT(("Error unflattening settings file %s\n", path.Path()));
	}
}
