/*
 * Copyright 2002 David Shipman,
 * Copyright 2003-2007 Marcus Overhagen
 * Copyright 2007 Haiku Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */


#include "MixerAddOn.h"

#include <stdio.h>
#include <string.h>

#include <MediaRoster.h>
#include <MediaNode.h>

#include "AudioMixer.h"
#include "MixerTap.h"


// instantiation function
extern "C" _EXPORT BMediaAddOn* make_media_addon(image_id image) {
	return new AudioMixerAddon(image);
}


AudioMixerAddon::AudioMixerAddon(image_id image)
	:
	BMediaAddOn(image),
	fFormat(new media_format),
	fInfo(new flavor_info),
	fLoopbackInfo(new flavor_info)
{
	fFormat->type = B_MEDIA_RAW_AUDIO;
	fFormat->u.raw_audio = media_raw_audio_format::wildcard;

	fInfo->internal_id = 0;
	fInfo->name = "Audio Mixer";
	fInfo->info = "Audio Mixer media addon";
	fInfo->kinds = B_BUFFER_PRODUCER | B_BUFFER_CONSUMER | B_SYSTEM_MIXER | B_CONTROLLABLE;
	fInfo->flavor_flags = 0;	// 0 = global or local instantiation allowed, no restrictions
	fInfo->possible_count = 0;	// 0 = infinite
	fInfo->in_format_count = 1;
	fInfo->in_formats = fFormat;
	fInfo->out_format_count = 1;
	fInfo->out_formats = fFormat;

	// Renga: what the system mixer plays, as a source recorders list next
	// to the sound cards' inputs (see MixerTap). Instantiated in the
	// media_addon_server, where the system mixer runs.
	fLoopbackInfo->internal_id = 1;
	fLoopbackInfo->name = "What you hear (loopback)";
	fLoopbackInfo->info = "Records what the system mixer plays: all "
		"applications, or one of them";
	fLoopbackInfo->kinds = B_BUFFER_PRODUCER | B_PHYSICAL_INPUT
		| B_CONTROLLABLE;
	fLoopbackInfo->flavor_flags = B_FLAVOR_IS_GLOBAL;
	fLoopbackInfo->possible_count = 0;
	fLoopbackInfo->in_format_count = 0;
	fLoopbackInfo->in_formats = NULL;
	fLoopbackInfo->out_format_count = 1;
	fLoopbackInfo->out_formats = fFormat;
}


AudioMixerAddon::~AudioMixerAddon()
{
	delete fFormat;
	delete fInfo;
	delete fLoopbackInfo;
}


// #pragma mark - BMediaAddOn implementation


status_t
AudioMixerAddon::InitCheck(const char** out_failure_text)
{
	return B_OK;
}


int32
AudioMixerAddon::CountFlavors()
{
	return 2;
}


status_t
AudioMixerAddon::GetFlavorAt(int32 n, const flavor_info** out_info)
{
	if (n == 0)
		*out_info = fInfo;
	else if (n == 1)
		*out_info = fLoopbackInfo;
	else
		return B_ERROR;
	return B_OK;
}


BMediaNode *
AudioMixerAddon::InstantiateNodeFor(const flavor_info* info, BMessage* config,
	status_t* out_error)
{
	if (info != NULL && info->internal_id == 1) {
		return new MixerTap(this, MixerTap::kLoopback,
			"What you hear (loopback)", 1);
	}
	return new AudioMixer(this, false);
}


status_t
AudioMixerAddon::GetConfigurationFor(BMediaNode* your_node,
	BMessage* into_message)
{
	// no config yet
	return B_ERROR;
}


bool
AudioMixerAddon::WantsAutoStart()
{
	return true;
}


status_t
AudioMixerAddon::AutoStart(int in_index, BMediaNode **out_node,
	int32 *out_internal_id, bool *out_has_more)
{
	*out_has_more = false;

	if (in_index != 0)
		return B_ERROR;

	*out_internal_id = 0;
	AudioMixer *mixer = new AudioMixer(this, true);
	*out_node = mixer;

	return B_OK;
}
