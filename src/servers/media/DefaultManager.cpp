/*
 * Copyright 2002, 2003 Marcus Overhagen, Jérôme Duval. All rights reserved.
 * Distributed under the terms of the MIT License.
 */


#include "DefaultManager.h"

#include <Application.h>
#include <Directory.h>
#include <File.h>
#include <FindDirectory.h>
#include <MediaNode.h>
#include <OS.h>
#include <Path.h>
#include <TimeSource.h>
#include <string.h>

#include "DataExchange.h"
#include "MediaDebug.h"
#include "MediaRosterEx.h"
#include "DormantNodeManager.h"
#include "Notifications.h"
#include "media_server.h"
#include "NodeManager.h"

#include <vector>


/* no locking used in this file, we assume that the caller (NodeManager) does it.
 */


#define MAX_NODE_INFOS 10
#define MAX_INPUT_INFOS 10

const uint32 kMsgHeader = 'sepx';
const uint32 kMsgTypeVideoIn = 0xffffffef;
const uint32 kMsgTypeVideoOut = 0xffffffee;
const uint32 kMsgTypeAudioIn = 0xfffffffe;
const uint32 kMsgTypeAudioOut = 0xffffffff;

const char *kDefaultManagerType 			= "be:_default";
const char *kDefaultManagerAddon 			= "be:_addon_id";
const char *kDefaultManagerFlavorId 		= "be:_internal_id";
const char *kDefaultManagerFlavorName 		= "be:_flavor_name";
const char *kDefaultManagerPath 			= "be:_path";
const char *kDefaultManagerInput 			= "be:_input_id";

const char *kDefaultManagerSettingsDirectory			= "Media";
const char *kDefaultManagerSettingsFile				= "MDefaultManager";


DefaultManager::DefaultManager()
	:
	fMixerConnected(false),
 	fPhysicalVideoOut(-1),
	fPhysicalVideoIn(-1),
	fPhysicalAudioOut(-1),
	fPhysicalAudioIn(-1),
	fSystemTimeSource(-1),
	fTimeSource(-1),
	fAudioMixer(-1),
	fPhysicalAudioOutInputID(0),
	fRescanThread(-1),
	fRescanRequested(0),
	fRescanLock("rescan default manager"),
	fRoster(NULL),
	fAudioOutFallback(false),
	fLostTimeSource(-1)
{
	strcpy(fPhysicalAudioOutInputName, "default");
	fBeginHeader[0] = 0xab00150b;
	fBeginHeader[1] = 0x18723462;
	fBeginHeader[2] = 0x00000002;
	fEndHeader[0] = 0x7465726d;
	fEndHeader[1] = 0x6d666c67;
	fEndHeader[2] = 0x00000002;

	fRoster = BMediaRoster::Roster();
	if (fRoster == NULL)
		TRACE("DefaultManager: The roster is NULL\n");
}


DefaultManager::~DefaultManager()
{
}


// this is called by the media_server *before* any add-ons have been loaded
status_t
DefaultManager::LoadState()
{
	CALLED();
	status_t err = B_OK;
	BPath path;
	if ((err = find_directory(B_USER_SETTINGS_DIRECTORY, &path)) != B_OK)
		return err;

	path.Append(kDefaultManagerSettingsDirectory);
	path.Append(kDefaultManagerSettingsFile);

	BFile file(path.Path(), B_READ_ONLY);

	uint32 categoryCount;
	ssize_t size = sizeof(uint32) * 3;
	if (file.Read(fBeginHeader, size) < size)
		return B_ERROR;
	TRACE("0x%08lx %ld\n", fBeginHeader[0], fBeginHeader[0]);
	TRACE("0x%08lx %ld\n", fBeginHeader[1], fBeginHeader[1]);
	TRACE("0x%08lx %ld\n", fBeginHeader[2], fBeginHeader[2]);
	size = sizeof(uint32);
	if (file.Read(&categoryCount, size) < size) {
		fprintf(stderr,
			"DefaultManager::LoadState() failed to read categoryCount\n");
		return B_ERROR;
	}
	TRACE("DefaultManager::LoadState() categoryCount %ld\n", categoryCount);
	while (categoryCount--) {
		BMessage settings;
		uint32 msg_header;
		uint32 default_type;
		if (file.Read(&msg_header, size) < size) {
			fprintf(stderr,
				"DefaultManager::LoadState() failed to read msg_header\n");
			return B_ERROR;
		}
		if (file.Read(&default_type, size) < size) {
			fprintf(stderr,
				"DefaultManager::LoadState() failed to read default_type\n");
			return B_ERROR;
		}
		if (settings.Unflatten(&file) == B_OK)
			fMsgList.AddItem(new BMessage(settings));
		else
			fprintf(stderr, "DefaultManager::LoadState() failed to unflatten\n");
	}
	size = sizeof(uint32) * 3;
	if (file.Read(fEndHeader,size) < size) {
		fprintf(stderr,
			"DefaultManager::LoadState() failed to read fEndHeader\n");
		return B_ERROR;
	}

	TRACE("LoadState returns B_OK\n");
	return B_OK;
}


status_t
DefaultManager::SaveState(NodeManager *node_manager)
{
	CALLED();
	status_t err = B_OK;
	BPath path;
	BList list;
	if ((err = find_directory(B_USER_SETTINGS_DIRECTORY, &path, true)) != B_OK)
		return err;
	path.Append(kDefaultManagerSettingsDirectory);
	if ((err = create_directory(path.Path(), 0755)) != B_OK)
		return err;
	path.Append(kDefaultManagerSettingsFile);

	uint32 default_types[] = {kMsgTypeVideoIn, kMsgTypeVideoOut,
		kMsgTypeAudioIn, kMsgTypeAudioOut};
	media_node_id media_node_ids[] = {fPhysicalVideoIn, fPhysicalVideoOut,
		fPhysicalAudioIn, fPhysicalAudioOut};
	for (uint32 i = 0; i < sizeof(default_types) / sizeof(default_types[0]);
		i++) {

		// we call the node manager to have more infos about nodes
		dormant_node_info info;
		media_node node;
		entry_ref ref;
		if (node_manager->GetCloneForID(media_node_ids[i], be_app->Team(),
				&node) != B_OK
			|| node_manager->GetDormantNodeInfo(node, &info) != B_OK
			|| node_manager->ReleaseNodeReference(media_node_ids[i],
				be_app->Team()) != B_OK
			|| node_manager->GetAddOnRef(info.addon, &ref) != B_OK) {
			if (media_node_ids[i] != -1) {
				// failed to get node info thus just return
				return B_ERROR;
			}
			continue;
		}

		if (default_types[i] == kMsgTypeAudioOut && fAudioOutFallback
			&& _SavedDefault(kMsgTypeAudioOut) != NULL) {
			// Renga: the device in use stands in for the user's choice,
			// which is not plugged in; that one stays the saved default.
			list.AddItem(new BMessage(*_SavedDefault(kMsgTypeAudioOut)));
			continue;
		}

		BMessage *settings = new BMessage();
		settings->AddInt32(kDefaultManagerType, default_types[i]);
		BPath path(&ref);
		settings->AddInt32(kDefaultManagerAddon, info.addon);
		settings->AddInt32(kDefaultManagerFlavorId, info.flavor_id);
		settings->AddInt32(kDefaultManagerInput,
			default_types[i] == kMsgTypeAudioOut ? fPhysicalAudioOutInputID : 0);
		settings->AddString(kDefaultManagerFlavorName, info.name);
		settings->AddString(kDefaultManagerPath, path.Path());

		list.AddItem(settings);
		TRACE("message %s added\n", info.name);
	}

	BFile file(path.Path(), B_WRITE_ONLY | B_CREATE_FILE | B_ERASE_FILE);

	if (file.Write(fBeginHeader, sizeof(uint32)*3) < (int32)sizeof(uint32)*3)
		return B_ERROR;
	int32 categoryCount = list.CountItems();
	if (file.Write(&categoryCount, sizeof(uint32)) < (int32)sizeof(uint32))
		return B_ERROR;

	for (int32 i = 0; i < categoryCount; i++) {
		BMessage *settings = (BMessage *)list.ItemAt(i);
		uint32 default_type;
		if (settings->FindInt32(kDefaultManagerType,
			(int32*)&default_type) < B_OK)
			return B_ERROR;
		if (file.Write(&kMsgHeader, sizeof(uint32)) < (int32)sizeof(uint32))
			return B_ERROR;
		if (file.Write(&default_type, sizeof(uint32)) < (int32)sizeof(uint32))
			return B_ERROR;
		if (settings->Flatten(&file) < B_OK)
			return B_ERROR;
	}
	// Renga: what is saved is what the defaults go back to (it was only
	// read at startup, so a default chosen later was forgotten).
	for (int32 i = 0; i < fMsgList.CountItems(); i++)
		delete (BMessage*)fMsgList.ItemAt(i);
	fMsgList.MakeEmpty();
	fMsgList.AddList(&list);
	if (file.Write(fEndHeader, sizeof(uint32)*3) < (int32)sizeof(uint32)*3)
		return B_ERROR;

	return B_OK;
}


status_t
DefaultManager::Set(media_node_id node_id, const char *input_name,
	int32 input_id, node_type type)
{
	CALLED();
	TRACE("DefaultManager::Set type : %i, node : %li, input : %li\n", type,
		node_id, input_id);
	switch (type) {
		case VIDEO_INPUT:
			fPhysicalVideoIn = node_id;
			return B_OK;
		case AUDIO_INPUT:
			fPhysicalAudioIn = node_id;
			return B_OK;
		case VIDEO_OUTPUT:
			fPhysicalVideoOut = node_id;
			return B_OK;
		case AUDIO_MIXER:
			return B_ERROR;
		case AUDIO_OUTPUT:
			if (fPhysicalAudioOut == node_id && fPhysicalAudioOutInputID == input_id)
				return B_OK;

			fPhysicalAudioOut = node_id;
			fPhysicalAudioOutInputID = input_id;
			strcpy(fPhysicalAudioOutInputName,
				input_name ? input_name : "<null>");
			// the user's choice now (SaveState() remembers it)
			fAudioOutFallback = false;

			fMixerConnected = false;
			Rescan();
			return B_OK;
		case TIME_SOURCE:
			return B_ERROR;

		// called by the media_server's ServerApp::StartSystemTimeSource()
		case SYSTEM_TIME_SOURCE:
		{
			ASSERT(fSystemTimeSource == -1);
			fSystemTimeSource = node_id;
			return B_OK;
		}

		default:
		{
			ERROR("DefaultManager::Set Error: called with unknown type %d\n",
				type);
			return B_ERROR;
		}
	}
}


status_t
DefaultManager::Get(media_node_id *nodeid, char *input_name, int32 *inputid,
	node_type type)
{
	CALLED();
	switch (type) {
		case VIDEO_INPUT: 		// output: nodeid
			if (fPhysicalVideoIn == -1)
				return B_NAME_NOT_FOUND;
			*nodeid = fPhysicalVideoIn;
			return B_OK;

		case AUDIO_INPUT: 		// output: nodeid
			if (fPhysicalAudioIn == -1)
				return B_NAME_NOT_FOUND;
			*nodeid = fPhysicalAudioIn;
			return B_OK;

		case VIDEO_OUTPUT: 		// output: nodeid
			if (fPhysicalVideoOut == -1)
				return B_NAME_NOT_FOUND;
			*nodeid = fPhysicalVideoOut;
			return B_OK;

		case AUDIO_OUTPUT:		// output: nodeid
			if (fPhysicalAudioOut == -1)
				return B_NAME_NOT_FOUND;
			*nodeid = fPhysicalAudioOut;
			return B_OK;

		case AUDIO_OUTPUT_EX:	// output: nodeid, input_name, input_id
			if (fPhysicalAudioOut == -1)
				return B_NAME_NOT_FOUND;
			*nodeid = fPhysicalAudioOut;
			*inputid = fPhysicalAudioOutInputID;
			strcpy(input_name, fPhysicalAudioOutInputName);
			return B_OK;

		case AUDIO_MIXER:		// output: nodeid
			if (fAudioMixer == -1)
				return B_NAME_NOT_FOUND;
			*nodeid = fAudioMixer;
			return B_OK;

		case TIME_SOURCE:
			if (fTimeSource != -1)
				*nodeid = fTimeSource;
			else
				*nodeid = fSystemTimeSource;
			return B_OK;

		case SYSTEM_TIME_SOURCE:
			*nodeid = fSystemTimeSource;
			return B_OK;

		default:
		{
			ERROR("DefaultManager::Get Error: called with unknown type %d\n",
				type);
			return B_ERROR;
		}
	}
}


// this is called by the media_server *after* the initial add-on loading
// has been done
status_t
DefaultManager::Rescan()
{
	BAutolock locker(fRescanLock);
	atomic_add(&fRescanRequested, 1);
	if (fRescanThread < 0) {
		fRescanThread = spawn_thread(rescan_thread, "rescan defaults",
			B_NORMAL_PRIORITY - 2, this);
		resume_thread(fRescanThread);
	}

	return B_OK;
}


int32
DefaultManager::rescan_thread(void *arg)
{
	reinterpret_cast<DefaultManager *>(arg)->_RescanThread();
	return 0;
}


void
DefaultManager::_RescanThread()
{
	TRACE("DefaultManager::_RescanThread() enter\n");

	BAutolock locker(fRescanLock);

	while (atomic_and(&fRescanRequested, 0) != 0) {
		locker.Unlock();

		// We do not search for the system time source,
		// it should already exist
		ASSERT(fSystemTimeSource != -1);

		// Renga: a default that was unplugged is replaced, one that is back
		// is taken again.
		_CheckDevices();

		if (fPhysicalVideoOut == -1) {
			_FindPhysical(&fPhysicalVideoOut, kMsgTypeVideoOut, false,
				B_MEDIA_RAW_VIDEO);
			_FindPhysical(&fPhysicalVideoOut, kMsgTypeVideoOut, false,
				B_MEDIA_ENCODED_VIDEO);
		}
		if (fPhysicalVideoIn == -1) {
			_FindPhysical(&fPhysicalVideoIn, kMsgTypeVideoIn, true,
				B_MEDIA_RAW_VIDEO);
			_FindPhysical(&fPhysicalVideoIn, kMsgTypeVideoIn, true,
				B_MEDIA_ENCODED_VIDEO);
		}
		if (fPhysicalAudioOut == -1)
			_FindPhysical(&fPhysicalAudioOut, kMsgTypeAudioOut, false,
				B_MEDIA_RAW_AUDIO);
		if (fPhysicalAudioIn == -1)
			_FindPhysical(&fPhysicalAudioIn, kMsgTypeAudioIn, true,
				B_MEDIA_RAW_AUDIO);
		if (fAudioMixer == -1)
			_FindAudioMixer();

		// The normal time source is searched for after the
		// Physical Audio Out has been created.
		if (fTimeSource == -1)
			_FindTimeSource();

		// Connect the mixer and physical audio out (soundcard)
		if (!fMixerConnected && fAudioMixer != -1 && fPhysicalAudioOut != -1) {
			fMixerConnected = _ConnectMixerToOutput() == B_OK;
			if (!fMixerConnected)
				TRACE("DefaultManager: failed to connect mixer and "
					"soundcard\n");
			if (fMixerConnected && fLostTimeSource != -1 && fTimeSource != -1) {
				_MoveSlaves(fLostTimeSource, fTimeSource);
				fLostTimeSource = -1;
			}
		} else {
			TRACE("DefaultManager: Did not try to connect mixer and "
				"soundcard\n");
		}

		if (fMixerConnected) {
			add_on_server_rescan_finished_notify_command cmd;
			SendToAddOnServer(ADD_ON_SERVER_RESCAN_FINISHED_NOTIFY, &cmd,
				sizeof(cmd));
		}

		locker.Lock();
	}

	fRescanThread = -1;

	BMessage msg(MEDIA_SERVER_RESCAN_COMPLETED);
	be_app->PostMessage(&msg);

	TRACE("DefaultManager::_RescanThread() leave\n");
}


void
DefaultManager::_FindPhysical(volatile media_node_id *id, uint32 default_type,
	bool isInput, media_type type)
{
	live_node_info info[MAX_NODE_INFOS];
	media_format format;
	int32 count;
	status_t rv;
	BMessage *msg = NULL;
	BPath msgPath;
	dormant_node_info msgDninfo;
	int32 input_id;
	bool isAudio = (type == B_MEDIA_RAW_AUDIO)
		|| (type == B_MEDIA_ENCODED_AUDIO);

	for (int32 i = 0; i < fMsgList.CountItems(); i++) {
		msg = (BMessage *)fMsgList.ItemAt(i);
		int32 msgType;
		if (msg->FindInt32(kDefaultManagerType, &msgType) == B_OK
			&& ((uint32)msgType == default_type)) {
			const char *name = NULL;
			const char *path = NULL;
			msg->FindInt32(kDefaultManagerAddon, &msgDninfo.addon);
			msg->FindInt32(kDefaultManagerFlavorId, &msgDninfo.flavor_id);
			msg->FindInt32(kDefaultManagerInput, &input_id);
			msg->FindString(kDefaultManagerFlavorName, &name);
			msg->FindString(kDefaultManagerPath, &path);
			if (name)
				strcpy(msgDninfo.name, name);
			if (path)
				msgPath = BPath(path);
			break;
		}
	}

	// Renga: the loop above leaves msg at the last entry when none is of
	// this type; it then stood for a default of another type.
	msg = _SavedDefault(default_type);

	format.type = type;
	count = MAX_NODE_INFOS;
	rv = fRoster->GetLiveNodes(&info[0], &count,
		isInput ? NULL : &format, isInput ? &format : NULL, NULL,
		isInput ? B_BUFFER_PRODUCER | B_PHYSICAL_INPUT
			: B_BUFFER_CONSUMER | B_PHYSICAL_OUTPUT);
	if (rv != B_OK || count < 1) {
		TRACE("Couldn't find physical %s %s node\n",
			isAudio ? "audio" : "video", isInput ? "input" : "output");
		return;
	}
	for (int i = 0; i < count; i++)
		TRACE("info[%d].name %s\n", i, info[i].name);

	// Renga: two passes. First the saved default, matched by its flavor
	// name and add-on (flavor IDs of devices plugged in while the system
	// runs differ from boot to boot; names are distinct per add-on). If it
	// is not there (a USB device that is not plugged in), any other device
	// is better than none, which upstream left the user with: the first
	// one, remembered as a fallback so that the saved one is taken again
	// when it appears (_CheckDevices()).
	for (int pass = msg != NULL ? 0 : 1; pass < 2; pass++) {
		for (int i = 0; i < count; i++) {
			if (isAudio) {
				if (isInput) {
					if (0 == strcmp(info[i].name, "None In")) {
						// we keep the Null audio driver if none else matchs
						*id = info[i].node.node;
						continue;
					}
					// skip the Firewire audio driver
					if (0 == strcmp(info[i].name, "DV Input"))
						continue;
				} else {
					if (0 == strcmp(info[i].name, "None Out")) {
						// we keep the Null audio driver if none else matchs
						*id = info[i].node.node;
						if (msg)
							fPhysicalAudioOutInputID = input_id;
						continue;
					}
					// skip the Firewire audio driver
					if (0 == strcmp(info[i].name, "DV Output"))
						continue;
				}
			}
			if (pass == 0 && !_MatchesSaved(info[i], msg))
				continue;
			// a node whose device was unplugged, still being released
			if (!_FlavorListed(info[i].node.node))
				continue;
			TRACE("Default physical %s %s \"%s\" created!\n",
				isAudio ? "audio" : "video", isInput ? "input" : "output",
				info[i].name);
			*id = info[i].node.node;
			if (isAudio && !isInput) {
				if (pass == 0)
					fPhysicalAudioOutInputID = input_id;
				fAudioOutFallback = pass == 1 && msg != NULL;
				if (fAudioOutFallback) {
					printf("DefaultManager: the default output is not "
						"there; using \"%s\" meanwhile\n", info[i].name);
				}
			}
			return;
		}
	}
}


/*!	Whether live node \a info is the device the saved default \a saved
	names: the same flavor name and add-on.
*/
bool
DefaultManager::_MatchesSaved(const live_node_info& info, BMessage* saved)
{
	if (saved == NULL)
		return false;
	const char* name = NULL;
	const char* path = NULL;
	saved->FindString(kDefaultManagerFlavorName, &name);
	saved->FindString(kDefaultManagerPath, &path);
	dormant_node_info dninfo;
	if (name == NULL || fRoster->GetDormantNodeFor(info.node, &dninfo) != B_OK
		|| strcmp(dninfo.name, name) != 0) {
		return false;
	}
	BPath addOnPath;
	return path == NULL
		|| (gDormantNodeManager->FindAddOnPath(&addOnPath, dninfo.addon)
				== B_OK && addOnPath == BPath(path));
}


BMessage*
DefaultManager::_SavedDefault(uint32 defaultType)
{
	for (int32 i = 0; i < fMsgList.CountItems(); i++) {
		BMessage* msg = (BMessage*)fMsgList.ItemAt(i);
		int32 type;
		if (msg->FindInt32(kDefaultManagerType, &type) == B_OK
			&& (uint32)type == defaultType) {
			return msg;
		}
	}
	return NULL;
}


bool
DefaultManager::_NodeExists(media_node_id id)
{
	media_node node;
	if (fRoster->GetNodeFor(id, &node) != B_OK)
		return false;
	fRoster->ReleaseNode(node);
	return true;
}


/*!	Whether the flavor \a node was made from is still offered. Nodes not
	made by an add-on (or whose flavor cannot be told) count as offered.
*/
bool
DefaultManager::_FlavorListed(media_node_id id)
{
	media_node node;
	if (fRoster->GetNodeFor(id, &node) != B_OK)
		return false;
	dormant_node_info info;
	status_t status = fRoster->GetDormantNodeFor(node, &info);
	fRoster->ReleaseNode(node);
	if (status != B_OK || info.addon <= 0)
		return true;
	dormant_node_info infos[64];
	int32 count = 64;
	if (fRoster->GetDormantNodes(infos, &count, NULL, NULL, NULL, 0, 0)
			!= B_OK) {
		return true;
	}
	for (int32 i = 0; i < count; i++) {
		if (infos[i].addon == info.addon
			&& infos[i].flavor_id == info.flavor_id) {
			return true;
		}
	}
	return false;
}


//!	The saved default output, if it is live now; -1 otherwise.
media_node_id
DefaultManager::_FindSavedAudioOut()
{
	BMessage* saved = _SavedDefault(kMsgTypeAudioOut);
	if (saved == NULL)
		return -1;
	live_node_info info[MAX_NODE_INFOS];
	media_format format;
	format.type = B_MEDIA_RAW_AUDIO;
	int32 count = MAX_NODE_INFOS;
	if (fRoster->GetLiveNodes(&info[0], &count, &format, NULL, NULL,
			B_BUFFER_CONSUMER | B_PHYSICAL_OUTPUT) != B_OK) {
		return -1;
	}
	for (int32 i = 0; i < count; i++) {
		if (_MatchesSaved(info[i], saved))
			return info[i].node.node;
	}
	return -1;
}


/*!	Renga: hot-plugged devices. A default input or output whose node is
	gone (media_addon_server releases the nodes of unplugged devices) is
	forgotten, so that the rescan picks another one, and the mixer is
	connected to it; if the default time source went with it, its slaves
	are moved to the new one once it is known. A default output used as a
	fallback gives way to the saved one as soon as that is back.
*/
void
DefaultManager::_CheckDevices()
{
	// Gone means: its node is gone, or its flavor is (the add-on dropped
	// the unplugged device; releasing the node can take a while, and the
	// default should not wait for it).
	if (fPhysicalAudioOut != -1 && (!_NodeExists(fPhysicalAudioOut)
			|| !_FlavorListed(fPhysicalAudioOut))) {
		printf("DefaultManager: the default output (node %" B_PRId32 ") is "
			"gone\n", fPhysicalAudioOut);
		if (_NodeExists(fPhysicalAudioOut)
			&& fPhysicalAudioOutFromMixer.node != media_node::null) {
			// Only the mixer's side (roadmap F-61, the fallback delay):
			// BMediaRoster::Disconnect() also asks the consumer, and the
			// node of an unplugged device is being released by
			// media_addon_server; its control thread no longer answers, so
			// each such query waited for the full 15 s port timeout
			// (19-56 s to the fallback after a USB device was pulled out).
			// The add-on server disconnects the node's inputs itself when
			// it releases it.
			bigtime_t start = system_time();
			status_t status = _DisconnectProducer(fMixerToPhysicalAudioOut,
				fPhysicalAudioOutFromMixer);
			printf("DefaultManager: the mixer is disconnected from the gone "
				"output in %" B_PRIdBIGTIME " ms: %s\n",
				(system_time() - start) / 1000, strerror(status));
		}
		if (fTimeSource == fPhysicalAudioOut
			|| (fTimeSource != -1 && !_NodeExists(fTimeSource))) {
			fLostTimeSource = fTimeSource;
			fTimeSource = -1;
		}
		fPhysicalAudioOut = -1;
		fMixerConnected = false;
		fMixerToPhysicalAudioOut = media_output();
		fPhysicalAudioOutFromMixer = media_input();
		// the saved one stays the one to go back to
		if (_SavedDefault(kMsgTypeAudioOut) != NULL)
			fAudioOutFallback = true;
	}
	if (fPhysicalAudioIn != -1 && (!_NodeExists(fPhysicalAudioIn)
			|| !_FlavorListed(fPhysicalAudioIn))) {
		fPhysicalAudioIn = -1;
	}

	if (fAudioOutFallback && fPhysicalAudioOut != -1) {
		media_node_id saved = _FindSavedAudioOut();
		if (saved >= 0 && saved != fPhysicalAudioOut) {
			printf("DefaultManager: the default output is back (node %"
				B_PRId32 ")\n", saved);
			BMessage* msg = _SavedDefault(kMsgTypeAudioOut);
			int32 inputID = 0;
			if (msg != NULL)
				msg->FindInt32(kDefaultManagerInput, &inputID);
			fPhysicalAudioOut = saved;
			fPhysicalAudioOutInputID = inputID;
			fMixerConnected = false;
			fAudioOutFallback = false;
		}
	}
}


/*!	Renga (F-61): disconnects \a output from \a input on the producer's
	side only, as BMediaRoster::Disconnect() does it, but without asking the
	consumer, whose node is gone or going (an unplugged device). The
	producer's outputs are published again and the connection's end is
	announced, like BMediaRoster::Disconnect() does.
*/
status_t
DefaultManager::_DisconnectProducer(const media_output& output,
	const media_input& input)
{
	producer_disconnect_request request;
	producer_disconnect_reply reply;
	request.source = output.source;
	request.destination = input.destination;
	status_t status = QueryPort(output.source.port, PRODUCER_DISCONNECT,
		&request, sizeof(request), &reply, sizeof(reply));

	media_node producer;
	if (fRoster->GetNodeFor(output.node.node, &producer) == B_OK) {
		List<media_output> outputs;
		if (MediaRosterEx(fRoster)->GetAllOutputs(producer, &outputs) == B_OK)
			MediaRosterEx(fRoster)->PublishOutputs(producer, &outputs);
		fRoster->ReleaseNode(producer);
	}
	BPrivate::media::notifications::ConnectionBroken(output.source,
		input.destination);
	return status;
}


//!	Moves the nodes slaved to the time source \a from to \a to.
void
DefaultManager::_MoveSlaves(media_node_id from, media_node_id to)
{
	std::vector<media_node_id> nodes;
	gNodeManager->GetNodesWithTimeSource(from, nodes);
	for (size_t i = 0; i < nodes.size(); i++) {
		status_t status = fRoster->SetTimeSourceFor(nodes[i], to);
		printf("DefaultManager: node %" B_PRId32 " moves from the time source "
			"%" B_PRId32 " to %" B_PRId32 ": %s\n", nodes[i], from, to,
			strerror(status));
	}
}


void
DefaultManager::_FindTimeSource()
{
	live_node_info info[MAX_NODE_INFOS];
	media_format input; /* a physical audio output has a logical data input (DAC)*/
	int32 count;
	status_t rv;

	/* First try to use the current default physical audio out
	 */
	if (fPhysicalAudioOut != -1) {
		media_node clone;
		if (fRoster->GetNodeFor(fPhysicalAudioOut,
				&clone) == B_OK) {
			if (clone.kind & B_TIME_SOURCE) {
				fTimeSource = clone.node;
				fRoster->StartTimeSource(clone,
					system_time() + 1000);
				fRoster->ReleaseNode(clone);
				TRACE("Default DAC timesource created!\n");
				return;
			}
			fRoster->ReleaseNode(clone);
		} else {
			TRACE("Default DAC is not a timesource!\n");
		}
	} else {
		TRACE("Default DAC node does not exist!\n");
	}

	/* Now try to find another physical audio out node
	 */
	input.type = B_MEDIA_RAW_AUDIO;
	count = MAX_NODE_INFOS;
	rv = fRoster->GetLiveNodes(&info[0], &count, &input, NULL, NULL,
		B_TIME_SOURCE | B_PHYSICAL_OUTPUT);
	if (rv == B_OK && count >= 1) {
		for (int i = 0; i < count; i++)
			printf("info[%d].name %s\n", i, info[i].name);

		for (int i = 0; i < count; i++) {
			// The BeOS R5 None Out node pretend to be a physical time source,
			// that is pretty dumb
			// skip the Null audio driver
			if (0 == strcmp(info[i].name, "None Out"))
				continue;
			// skip the Firewire audio driver
			if (0 != strstr(info[i].name, "DV Output"))
				continue;
			TRACE("Default DAC timesource \"%s\" created!\n", info[i].name);
			fTimeSource = info[i].node.node;
			fRoster->StartTimeSource(info[i].node,
				system_time() + 1000);
			return;
		}
	} else {
		TRACE("Couldn't find DAC timesource node\n");
	}

	/* XXX we might use other audio or video clock timesources
	 */
}


void
DefaultManager::_FindAudioMixer()
{
	live_node_info info;
	int32 count;
	status_t rv;

	if (fRoster == NULL)
		fRoster = BMediaRoster::Roster();

	count = 1;
	rv = fRoster->GetLiveNodes(&info, &count, NULL, NULL, NULL,
		B_BUFFER_PRODUCER | B_BUFFER_CONSUMER | B_SYSTEM_MIXER);
	if (rv != B_OK || count != 1) {
		TRACE("Couldn't find audio mixer node\n");
		return;
	}
	fAudioMixer = info.node.node;
	TRACE("Default audio mixer node created\n");
}


status_t
DefaultManager::_ConnectMixerToOutput()
{
	if (fPhysicalAudioOutFromMixer.node != media_node::null) {
		// Disconnect the old output. (The mixer should be in auto-stop mode,
		// and stop the output on its own.)
		status_t status = fRoster->Disconnect(fMixerToPhysicalAudioOut,
			fPhysicalAudioOutFromMixer);
		if (status != B_OK)
			return status;

		fMixerToPhysicalAudioOut.node = media_node::null;
		fPhysicalAudioOutFromMixer.node = media_node::null;

		// Find and start the new timesource.
		_FindTimeSource();
	}

	media_node timesource, mixer, soundcard;
	media_input inputs[MAX_INPUT_INFOS];
	media_input input;
	media_output output;
	media_input newinput;
	media_output newoutput;
	media_format format;
	int32 count;

	if (fRoster == NULL)
		fRoster = BMediaRoster::Roster();

	status_t rv = fRoster->GetNodeFor(fPhysicalAudioOut, &soundcard);
	if (rv != B_OK) {
		TRACE("DefaultManager: failed to find soundcard (physical audio "
			"output)\n");
		return B_ERROR;
	}

	rv = fRoster->GetNodeFor(fAudioMixer, &mixer);
	if (rv != B_OK) {
		fRoster->ReleaseNode(soundcard);
		TRACE("DefaultManager: failed to find mixer\n");
		return B_ERROR;
	}

	// we now have the mixer and soundcard nodes,
	// find a free input/output and connect them

	fRoster->GetTimeSource(&timesource);
	fRoster->SetTimeSourceFor(soundcard.node, timesource.node);

	rv = fRoster->GetFreeOutputsFor(mixer, &output, 1, &count,
		B_MEDIA_RAW_AUDIO);
	if (rv != B_OK || count != 1) {
		TRACE("DefaultManager: can't find free mixer output\n");
		rv = B_ERROR;
		goto finish;
	}

	rv = fRoster->GetFreeInputsFor(soundcard, inputs, MAX_INPUT_INFOS, &count,
		B_MEDIA_RAW_AUDIO);
	if (rv == B_OK && count < 1 && _ReleaseMixerSinks(soundcard) > 0) {
		rv = fRoster->GetFreeInputsFor(soundcard, inputs, MAX_INPUT_INFOS,
			&count, B_MEDIA_RAW_AUDIO);
	}
	if (rv != B_OK || count < 1) {
		TRACE("DefaultManager: can't find free soundcard inputs\n");
		rv = B_ERROR;
		goto finish;
	}

	for (int32 i = 0; i < count; i++) {
		input = inputs[i];
		if (input.destination.id == fPhysicalAudioOutInputID)
			break;
	}

	for (int i = 0; i < 6; i++) {
		switch (i) {
			case 0:
				TRACE("DefaultManager: Trying connect in native format (1)\n");
				if (fRoster->GetFormatFor(input, &format) != B_OK) {
					ERROR("DefaultManager: GetFormatFor failed\n");
					continue;
				}
				// XXX BeOS R5 multiaudio node bug workaround
				if (format.u.raw_audio.channel_count == 1) {
					TRACE("##### WARNING! DefaultManager: ignored mono format\n");
					continue;
				}
				break;

			case 1:
				TRACE("DefaultManager: Trying connect in format 1\n");
				format.Clear();
				format.type = B_MEDIA_RAW_AUDIO;
				format.u.raw_audio.frame_rate = 44100;
				format.u.raw_audio.channel_count = 2;
				format.u.raw_audio.format = 0x2;
				break;

			case 2:
				TRACE("DefaultManager: Trying connect in format 2\n");
				format.Clear();
				format.type = B_MEDIA_RAW_AUDIO;
				format.u.raw_audio.frame_rate = 48000;
				format.u.raw_audio.channel_count = 2;
				format.u.raw_audio.format = 0x2;
				break;

			case 3:
				TRACE("DefaultManager: Trying connect in format 3\n");
				format.Clear();
				format.type = B_MEDIA_RAW_AUDIO;
				break;

			case 4:
				// BeOS R5 multiaudio node bug workaround
				TRACE("DefaultManager: Trying connect in native format (2)\n");
				if (fRoster->GetFormatFor(input, &format) != B_OK) {
					ERROR("DefaultManager: GetFormatFor failed\n");
					continue;
				}
				break;

			case 5:
				TRACE("DefaultManager: Trying connect in format 4\n");
				format.Clear();
				break;

		}
		rv = fRoster->Connect(output.source, input.destination, &format,
			&newoutput, &newinput);
		if (rv == B_OK)
			break;
	}
	if (rv != B_OK) {
		ERROR("DefaultManager: connect failed\n");
		goto finish;
	}

	fRoster->SetRunModeNode(mixer, BMediaNode::B_INCREASE_LATENCY);
	fRoster->SetRunModeNode(soundcard, BMediaNode::B_RECORDING);

	fRoster->PrerollNode(mixer);
	fRoster->PrerollNode(soundcard);

	{
		BTimeSource* ts = fRoster->MakeTimeSourceFor(mixer);
		bigtime_t startAt;
		if (ts == NULL) {
			rv = B_ERROR;
			goto finish;
		}

		startAt = ts->Now() + 50000;
		fRoster->StartNode(mixer, startAt);

		// The mixer will be in "auto-stop" mode, so it'll start the output on its own.
		ts->Release();
	}

	fMixerToPhysicalAudioOut = newoutput;
	fPhysicalAudioOutFromMixer = newinput;

finish:
	fRoster->ReleaseNode(mixer);
	fRoster->ReleaseNode(soundcard);
	fRoster->ReleaseNode(timesource);
	return rv;
}


/*!	Renga: the system mixer plays streams routed to another device than the
	default output through "Mixer sink" producers it connects to that
	device (see the mixer add-on's MixerTap). When the user makes such a
	device the default output, the mixer itself takes its input over:
	disconnect the sinks from it. The mixer then plays those streams on the
	default output, which is that device. Returns how many were removed.
*/
int32
DefaultManager::_ReleaseMixerSinks(const media_node& soundcard)
{
	media_input inputs[MAX_INPUT_INFOS];
	int32 count;
	if (fRoster->GetConnectedInputsFor(soundcard, inputs, MAX_INPUT_INFOS,
			&count) != B_OK) {
		return 0;
	}

	int32 released = 0;
	for (int32 i = 0; i < count; i++) {
		media_node_id producerID = fRoster->NodeIDFor(inputs[i].source.port);
		media_node producer;
		if (producerID < 0 || fRoster->GetNodeFor(producerID, &producer)
				!= B_OK) {
			continue;
		}
		live_node_info info;
		if (fRoster->GetLiveNodeInfo(producer, &info) == B_OK
			&& strncmp(info.name, "Mixer sink", 10) == 0
			&& fRoster->Disconnect(producerID, inputs[i].source,
				soundcard.node, inputs[i].destination) == B_OK) {
			released++;
		}
		fRoster->ReleaseNode(producer);
	}
	return released;
}


void
DefaultManager::Dump()
{
}


void
DefaultManager::CleanupTeam(team_id team)
{
}
