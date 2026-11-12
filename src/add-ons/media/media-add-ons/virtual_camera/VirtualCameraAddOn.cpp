/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 */


#include <MediaAddOn.h>
#include <MediaFormats.h>

#include <VirtualCamera.h>

#include "VirtualCameraNode.h"


/*!	The virtual camera's add-on (F-58, O-12): one global flavor, so that
	every application shares the node media_addon_server runs.
*/
class VirtualCameraAddOn : public BMediaAddOn {
public:
	VirtualCameraAddOn(image_id image)
		:
		BMediaAddOn(image)
	{
		fFormat.type = B_MEDIA_RAW_VIDEO;
		fFormat.u.raw_video = media_raw_video_format::wildcard;
		fFormat.u.raw_video.interlace = 1;
		fFormat.u.raw_video.display.format = B_RGB32;

		fFlavor.in_format_count = 0;
		fFlavor.in_format_flags = 0;
		fFlavor.in_formats = NULL;
		fFlavor.out_format_flags = 0;
		fFlavor.name = (char*)VIRTUAL_CAMERA_FLAVOR_NAME;
		fFlavor.info = (char*)"A camera fed by an application (OBS Studio's "
			"virtual camera)";
		fFlavor.kinds = B_BUFFER_PRODUCER | B_CONTROLLABLE | B_PHYSICAL_INPUT;
		fFlavor.flavor_flags = B_FLAVOR_IS_GLOBAL;
		fFlavor.internal_id = 0;
		fFlavor.possible_count = 8;
			// the media server makes one as the default video input;
			// every node shows the same feed
		fFlavor.out_format_count = 1;
		fFlavor.out_formats = &fFormat;
	}

	virtual status_t InitCheck(const char** _failureText)
	{
		return B_OK;
	}

	virtual int32 CountFlavors()
	{
		return 1;
	}

	virtual status_t GetFlavorAt(int32 index, const flavor_info** _info)
	{
		if (index != 0)
			return B_BAD_INDEX;
		*_info = &fFlavor;
		return B_OK;
	}

	virtual BMediaNode* InstantiateNodeFor(const flavor_info* info,
		BMessage* config, status_t* _error)
	{
		if (info->internal_id != fFlavor.internal_id) {
			*_error = B_BAD_VALUE;
			return NULL;
		}
		VirtualCameraNode* node = new(std::nothrow) VirtualCameraNode(this,
			fFlavor.name, fFlavor.internal_id);
		if (node == NULL) {
			*_error = B_NO_MEMORY;
			return NULL;
		}
		*_error = B_OK;
		return node;
	}

	virtual bool WantsAutoStart()
	{
		return false;
	}

private:
	flavor_info		fFlavor;
	media_format	fFormat;
};


extern "C" BMediaAddOn*
make_media_addon(image_id image)
{
	return new VirtualCameraAddOn(image);
}
