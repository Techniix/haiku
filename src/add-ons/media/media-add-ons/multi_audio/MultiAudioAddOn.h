/*
 * Copyright (c) 2002, Jerome Duval (jerome.duval@free.fr)
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 */
#ifndef MULTI_AUDIO_ADDON_H
#define MULTI_AUDIO_ADDON_H


#include <Locker.h>
#include <MediaDefs.h>
#include <MediaAddOn.h>
#include <Message.h>
#include <String.h>

class BEntry;
class BLooper;
class MultiAudioDevice;


class MultiAudioAddOn : public BMediaAddOn {
public:
						MultiAudioAddOn(image_id image);
	virtual				~MultiAudioAddOn();

	virtual	status_t	InitCheck(const char** _failureText);
	virtual	int32		CountFlavors();
	virtual	status_t	GetFlavorAt(int32 i, const flavor_info** _info);
	virtual	BMediaNode*	InstantiateNodeFor(const flavor_info* info,
							BMessage* config, status_t* _error);
	virtual	status_t	GetConfigurationFor(BMediaNode* node,
							BMessage* message);
	virtual	bool		WantsAutoStart();
	virtual	status_t	AutoStart(int count, BMediaNode** _node,
							int32* _internalID, bool* _hasMore);

	// Renga: devices come and go (see DeviceWatcher in the .cpp).
			void		Rescan();
			// A node is gone; its device may be closed now if it was
			// unplugged.
			void		NodeDeleted(MultiAudioDevice* device);
			// A node's device stopped answering: look at once whether
			// it is still there.
			void		DeviceFailed(MultiAudioDevice* device);

private:
			struct DeviceEntry {
				MultiAudioDevice*	device;
				int32				id;		// the flavor's internal_id
				BString				path;
				BString				name;	// distinct, see _AddDevice()
				int32				nodes;	// nodes made for it, still there
			};

			void		_Scan(const char* path, BMessage& found,
							uint32 depth = 0);
			bool		_AddDevice(const char* path);
			DeviceEntry* _FindEntry(int32 id);
			void		_SaveSettings();
			void		_LoadSettings();

private:
	status_t 		fInitStatus;
	BLocker			fLock;
	BList			fDevices;		// of DeviceEntry*, the flavors in order
	BList			fGone;			// unplugged, but a node may still use it
	int32			fNextID;
	BLooper*		fWatcher;

	BMessage		fSettings;
		// loaded from settings directory
};

extern "C" BMediaAddOn* make_media_addon(image_id you);

#endif	// MULTI_AUDIO_ADDON_H
