/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 */


/*!	The roster of video acceleration back ends (roadmap F-31 phase 5);
	see headers/private/media/VideoAcceleration.h.
*/


#include <VideoAcceleration.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <Directory.h>
#include <Entry.h>
#include <File.h>
#include <FindDirectory.h>
#include <Path.h>
#include <StringList.h>
#include <image.h>

#include <pthread.h>


namespace BPrivate {


static VideoAcceleratorRoster* sDefaultRoster = NULL;
static pthread_once_t sDefaultRosterOnce = PTHREAD_ONCE_INIT;


static void
create_default_roster()
{
	sDefaultRoster = new(std::nothrow) VideoAcceleratorRoster;
}


/*!	"auto" (hardware back ends only), "off", or a back end's name: the
	environment variable first, then the setting file.
*/
static BString
read_choice()
{
	const char* environment = getenv(RENGA_VIDEO_ACCELERATION_ENVIRONMENT);
	if (environment != NULL && environment[0] != '\0')
		return BString(environment);

	BPath path;
	if (find_directory(B_USER_SETTINGS_DIRECTORY, &path) == B_OK
		&& path.Append("system/video_acceleration") == B_OK) {
		FILE* file = fopen(path.Path(), "r");
		if (file != NULL) {
			char line[256];
			BString choice;
			while (fgets(line, sizeof(line), file) != NULL) {
				char name[128];
				if (sscanf(line, " backend %127s", name) == 1)
					choice = name;
			}
			fclose(file);
			if (choice.Length() > 0)
				return choice;
		}
	}
	return BString("auto");
}


VideoAcceleratorRoster::VideoAcceleratorRoster()
	:
	fAccelerators(4),
	fLoaded(false)
{
}


/*static*/ VideoAcceleratorRoster*
VideoAcceleratorRoster::Default()
{
	pthread_once(&sDefaultRosterOnce, &create_default_roster);
	return sDefaultRoster;
}


void
VideoAcceleratorRoster::_Load()
{
	if (fLoaded)
		return;
	fLoaded = true;

	const directory_which kDirectories[] = {
		B_SYSTEM_ADDONS_DIRECTORY,
		B_USER_ADDONS_DIRECTORY,
		B_SYSTEM_NONPACKAGED_ADDONS_DIRECTORY
	};
	BStringList loaded;
	for (size_t i = 0; i < B_COUNT_OF(kDirectories); i++) {
		BPath path;
		if (find_directory(kDirectories[i], &path) != B_OK
			|| path.Append("media/video_accelerators") != B_OK) {
			continue;
		}
		BDirectory directory(path.Path());
		BEntry entry;
		while (directory.GetNextEntry(&entry, true) == B_OK) {
			BPath addOn;
			if (entry.GetPath(&addOn) != B_OK
				|| loaded.HasString(addOn.Leaf())) {
				continue;
			}
			image_id image = load_add_on(addOn.Path());
			if (image < 0)
				continue;
			instantiate_video_accelerator_func instantiate;
			if (get_image_symbol(image, "instantiate_video_accelerator",
					B_SYMBOL_TYPE_TEXT, (void**)&instantiate) != B_OK) {
				unload_add_on(image);
				continue;
			}
			VideoAccelerator* accelerator
				= instantiate(B_VIDEO_ACCELERATION_VERSION);
			if (accelerator == NULL) {
				// no device for it
				unload_add_on(image);
				continue;
			}
			loaded.Add(addOn.Leaf());
			fAccelerators.AddItem(accelerator);
		}
	}
}


int32
VideoAcceleratorRoster::CountAccelerators()
{
	_Load();
	return fAccelerators.CountItems();
}


VideoAccelerator*
VideoAcceleratorRoster::AcceleratorAt(int32 index)
{
	_Load();
	return fAccelerators.ItemAt(index);
}


VideoAccelerator*
VideoAcceleratorRoster::Find(VideoAccelProfile profile,
	VideoAccelEntrypoint entrypoint, int32 width, int32 height)
{
	BString choice = read_choice();
	if (choice == "off")
		return NULL;
	_Load();
	for (int32 i = 0; i < fAccelerators.CountItems(); i++) {
		VideoAccelerator* accelerator = fAccelerators.ItemAt(i);
		if (choice == "auto") {
			// only real hardware, unless chosen by name
			if (!accelerator->IsHardware())
				continue;
		} else if (choice != accelerator->Name())
			continue;
		if (accelerator->Supports(profile, entrypoint, width, height))
			return accelerator;
	}
	return NULL;
}


BString
VideoAcceleratorRoster::Choice() const
{
	return read_choice();
}


/*static*/ const char*
VideoAcceleratorRoster::ProfileName(VideoAccelProfile profile)
{
	switch (profile) {
		case B_VIDEO_ACCEL_MPEG2_MAIN:
			return "MPEG-2 Main";
		case B_VIDEO_ACCEL_MPEG4_SIMPLE:
			return "MPEG-4 Simple";
		case B_VIDEO_ACCEL_MPEG4_ADVANCED_SIMPLE:
			return "MPEG-4 Advanced Simple";
		case B_VIDEO_ACCEL_H264_CONSTRAINED_BASELINE:
			return "H.264 Constrained Baseline";
		case B_VIDEO_ACCEL_H264_MAIN:
			return "H.264 Main";
		case B_VIDEO_ACCEL_H264_HIGH:
			return "H.264 High";
		case B_VIDEO_ACCEL_HEVC_MAIN:
			return "HEVC Main";
		case B_VIDEO_ACCEL_HEVC_MAIN10:
			return "HEVC Main 10";
		case B_VIDEO_ACCEL_VP8:
			return "VP8";
		case B_VIDEO_ACCEL_VP9_PROFILE0:
			return "VP9 Profile 0";
		case B_VIDEO_ACCEL_VP9_PROFILE2:
			return "VP9 Profile 2";
		case B_VIDEO_ACCEL_AV1_MAIN:
			return "AV1 Main";
		default:
			return "none";
	}
}


}	// namespace BPrivate
