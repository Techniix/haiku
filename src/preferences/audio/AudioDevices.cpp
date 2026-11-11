/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 */


#include "AudioDevices.h"

#include <string.h>

#include <MediaRoster.h>
#include <ParameterWeb.h>

#include <AudioStreamControl.h>
#include <LowLatencyAudio.h>


static bool
same_flavor(const dormant_node_info& a, const dormant_node_info& b)
{
	return a.addon == b.addon && a.flavor_id == b.flavor_id;
}


/*!	The dormant node info of the default audio output (or input) node. */
static bool
default_info(bool output, dormant_node_info* _info)
{
	BMediaRoster* roster = BMediaRoster::CurrentRoster();
	if (roster == NULL)
		return false;
	media_node node;
	status_t status;
	if (output) {
		int32 channel;
		BString channelName;
		status = roster->GetAudioOutput(&node, &channel, &channelName);
	} else
		status = roster->GetAudioInput(&node);
	if (status != B_OK)
		return false;
	status = roster->GetDormantNodeFor(node, _info);
	roster->ReleaseNode(node);
	return status == B_OK;
}


AudioDeviceList::AudioDeviceList()
	:
	fDevices(8)
{
}


AudioDeviceList::~AudioDeviceList()
{
}


status_t
AudioDeviceList::Update(bool details)
{
	// Without details, a device keeps what was read of it last time.
	BObjectList<AudioDevice, true> previous(8);
	if (!details) {
		while (AudioDevice* device = fDevices.RemoveItemAt(0))
			previous.AddItem(device);
	}
	fDevices.MakeEmpty();

	status_t status;
	BMediaRoster* roster = BMediaRoster::Roster(&status);
	if (roster == NULL)
		return status;

	for (int pass = 0; pass < 2; pass++) {
		bool output = pass == 0;
		media_format format;
		format.type = B_MEDIA_RAW_AUDIO;
		dormant_node_info infos[64];
		int32 count = 64;
		status = roster->GetDormantNodes(infos, &count,
			output ? &format : NULL, output ? NULL : &format, NULL,
			output ? B_PHYSICAL_OUTPUT : B_PHYSICAL_INPUT);
		if (status != B_OK)
			return status;
		for (int32 i = 0; i < count; i++) {
			if (!output && strcmp(infos[i].name,
					RENGA_LOOPBACK_FLAVOR_NAME) == 0) {
				continue;
			}
			_Add(infos[i], output);
		}
	}

	dormant_node_info info;
	bool haveOutput = default_info(true, &info);
	for (int32 i = 0; haveOutput && i < CountDevices(); i++) {
		AudioDevice* device = fDevices.ItemAt(i);
		device->defaultOutput = device->output && same_flavor(device->info,
			info);
	}
	bool haveInput = default_info(false, &info);
	for (int32 i = 0; haveInput && i < CountDevices(); i++) {
		AudioDevice* device = fDevices.ItemAt(i);
		device->defaultInput = device->input && same_flavor(device->info,
			info);
	}

	for (int32 i = 0; i < CountDevices(); i++) {
		AudioDevice* device = fDevices.ItemAt(i);
		media_node_id ids[4];
		int32 count = 4;
		if (roster->GetInstancesFor(device->info.addon,
				device->info.flavor_id, ids, &count) == B_OK && count > 0) {
			device->running = true;
			device->nodeID = ids[0];
			if (details)
				_ReadDetails(device);
		}
		for (int32 j = 0; !details && j < previous.CountItems(); j++) {
			const AudioDevice* old = previous.ItemAt(j);
			if (!same_flavor(old->info, device->info)
				|| old->nodeID != device->nodeID) {
				continue;
			}
			device->outputRate = old->outputRate;
			device->inputRate = old->inputRate;
			device->hasBufferSize = old->hasBufferSize;
			device->bufferFrames = old->bufferFrames;
			device->bufferValues = old->bufferValues;
			device->bufferNames = old->bufferNames;
		}
	}
	return B_OK;
}


const AudioDevice*
AudioDeviceList::FindDevice(const char* name) const
{
	for (int32 i = 0; name != NULL && i < CountDevices(); i++) {
		if (DeviceAt(i)->name == name)
			return DeviceAt(i);
	}
	return NULL;
}


BStringList
AudioDeviceList::OutputNames() const
{
	BStringList names;
	for (int32 i = 0; i < CountDevices(); i++) {
		if (DeviceAt(i)->output)
			names.Add(DeviceAt(i)->name);
	}
	return names;
}


BStringList
AudioDeviceList::InputNames() const
{
	BStringList names;
	for (int32 i = 0; i < CountDevices(); i++) {
		if (DeviceAt(i)->input)
			names.Add(DeviceAt(i)->name);
	}
	return names;
}


BString
AudioDeviceList::DefaultOutputName() const
{
	for (int32 i = 0; i < CountDevices(); i++) {
		if (DeviceAt(i)->defaultOutput)
			return DeviceAt(i)->name;
	}
	return BString();
}


BString
AudioDeviceList::DefaultInputName() const
{
	for (int32 i = 0; i < CountDevices(); i++) {
		if (DeviceAt(i)->defaultInput)
			return DeviceAt(i)->name;
	}
	return BString();
}


BString
AudioDeviceList::Key() const
{
	BString key;
	for (int32 i = 0; i < CountDevices(); i++) {
		const AudioDevice* device = DeviceAt(i);
		key << device->name << (device->output ? "o" : "")
			<< (device->input ? "i" : "")
			<< (device->defaultOutput ? "O" : "")
			<< (device->defaultInput ? "I" : "")
			<< (device->running ? "r" : "") << ";";
	}
	return key;
}


status_t
AudioDeviceList::SetDefaultOutput(const char* name)
{
	const AudioDevice* device = FindDevice(name);
	if (device == NULL || !device->output)
		return B_ENTRY_NOT_FOUND;
	return BMediaRoster::Roster()->SetAudioOutput(device->info);
}


status_t
AudioDeviceList::SetDefaultInput(const char* name)
{
	const AudioDevice* device = FindDevice(name);
	if (device == NULL || !device->input)
		return B_ENTRY_NOT_FOUND;
	return BMediaRoster::Roster()->SetAudioInput(device->info);
}


status_t
AudioDeviceList::SetBufferFrames(const char* name, int32 frames)
{
	const AudioDevice* device = FindDevice(name);
	if (device == NULL || !device->running)
		return B_ENTRY_NOT_FOUND;

	BMediaRoster* roster = BMediaRoster::Roster();
	media_node node;
	status_t status = roster->GetNodeFor(device->nodeID, &node);
	if (status != B_OK)
		return status;
	BParameterWeb* web = NULL;
	status = roster->GetParameterWebFor(node, &web);
	if (status == B_OK) {
		status = B_ENTRY_NOT_FOUND;
		for (int32 i = 0; i < web->CountParameters(); i++) {
			BParameter* parameter = web->ParameterAt(i);
			if (strcmp(parameter->Kind(), RENGA_BUFFER_FRAMES_KIND) != 0)
				continue;
			status = parameter->SetValue(&frames, sizeof(frames),
				system_time());
			break;
		}
		delete web;
	}
	roster->ReleaseNode(node);
	// The node applies it a moment later; show the choice meanwhile (the
	// next read of the details confirms it).
	if (status == B_OK)
		const_cast<AudioDevice*>(device)->bufferFrames = frames;
	return status;
}


/*static*/ status_t
AudioDeviceList::LoopbackInfo(dormant_node_info* _info)
{
	return BPrivate::AudioStreamControl::FindLoopbackFlavor(_info);
}


void
AudioDeviceList::_Add(const dormant_node_info& info, bool output)
{
	for (int32 i = 0; i < CountDevices(); i++) {
		AudioDevice* device = fDevices.ItemAt(i);
		if (same_flavor(device->info, info)) {
			if (output)
				device->output = true;
			else
				device->input = true;
			return;
		}
	}

	AudioDevice* device = new AudioDevice;
	device->name = info.name;
	device->info = info;
	device->output = output;
	device->input = !output;
	device->defaultOutput = false;
	device->defaultInput = false;
	device->running = false;
	device->nodeID = -1;
	device->hasBufferSize = false;
	device->bufferFrames = 0;
	fDevices.AddItem(device);
}


void
AudioDeviceList::_ReadDetails(AudioDevice* device)
{
	BMediaRoster* roster = BMediaRoster::Roster();
	media_node node;
	if (roster->GetNodeFor(device->nodeID, &node) != B_OK)
		return;
	BParameterWeb* web = NULL;
	if (roster->GetParameterWebFor(node, &web) == B_OK) {
		for (int32 i = 0; i < web->CountParameters(); i++) {
			BParameter* parameter = web->ParameterAt(i);
			if (parameter->Type() != BParameter::B_DISCRETE_PARAMETER)
				continue;
			BDiscreteParameter* discrete = (BDiscreteParameter*)parameter;
			int32 value = 0;
			size_t size = sizeof(value);
			bigtime_t when;
			if (discrete->GetValue(&value, &size, &when) != B_OK)
				continue;

			if (strcmp(discrete->Kind(), RENGA_BUFFER_FRAMES_KIND) == 0) {
				device->hasBufferSize = true;
				device->bufferFrames = value;
				for (int32 j = 0; j < discrete->CountItems(); j++) {
					device->bufferValues.AddItem(
						(void*)(addr_t)discrete->ItemValueAt(j));
					device->bufferNames.Add(discrete->ItemNameAt(j));
				}
				continue;
			}

			// multi_audio's frequency menus (ids 1: input, 2: output),
			// whose items are named "<rate> Hz"
			if (discrete->ID() != 1 && discrete->ID() != 2)
				continue;
			for (int32 j = 0; j < discrete->CountItems(); j++) {
				if (discrete->ItemValueAt(j) != value)
					continue;
				BString name = discrete->ItemNameAt(j);
				if (!name.EndsWith(" Hz"))
					break;
				if (discrete->ID() == 2)
					device->outputRate = name;
				else
					device->inputRate = name;
				break;
			}
		}
		delete web;
	}
	roster->ReleaseNode(node);
}
