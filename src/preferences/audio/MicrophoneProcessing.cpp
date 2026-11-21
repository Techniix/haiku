/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 */


#include "MicrophoneProcessing.h"

#include <stdio.h>
#include <stdlib.h>

#include <Directory.h>
#include <Entry.h>
#include <File.h>
#include <FindDirectory.h>
#include <MediaRoster.h>
#include <ParameterWeb.h>
#include <Path.h>
#include <StringList.h>

#include <AudioStreamControl.h>

#include "AudioDevices.h"


static status_t
processing_path(const char* device, BPath& path)
{
	status_t status = find_directory(B_USER_SETTINGS_DIRECTORY, &path);
	if (status == B_OK)
		status = path.Append("Media/input_processing");
	BString leaf(device);
	leaf.ReplaceAll('/', '-');
	if (status == B_OK)
		status = path.Append(leaf.String());
	return status;
}


/*!	The running node's processing parameter, or NULL; the caller deletes
	\a _web.
*/
static BParameter*
processing_parameter(const char* device, BParameterWeb** _web)
{
	*_web = NULL;
	AudioDeviceList devices;
	if (devices.Update(false) != B_OK)
		return NULL;
	const AudioDevice* entry = devices.FindDevice(device);
	if (entry == NULL || !entry->input || !entry->running)
		return NULL;
	BMediaRoster* roster = BMediaRoster::Roster();
	media_node node;
	if (roster == NULL || roster->GetNodeFor(entry->nodeID, &node) != B_OK)
		return NULL;
	BParameterWeb* web = NULL;
	status_t status = roster->GetParameterWebFor(node, &web);
	roster->ReleaseNode(node);
	if (status != B_OK)
		return NULL;
	for (int32 i = 0; i < web->CountParameters(); i++) {
		BParameter* parameter = web->ParameterAt(i);
		if (strcmp(parameter->Kind(), RENGA_INPUT_PROCESSING_KIND) == 0) {
			*_web = web;
			return parameter;
		}
	}
	delete web;
	return NULL;
}


status_t
get_input_processing(const char* device, BString& text)
{
	text = "";
	BParameterWeb* web;
	BParameter* parameter = processing_parameter(device, &web);
	if (parameter != NULL) {
		char buffer[1025];
		size_t size = sizeof(buffer) - 1;
		bigtime_t when;
		status_t status = parameter->GetValue(buffer, &size, &when);
		delete web;
		if (status != B_OK)
			return status;
		buffer[min_c(size, sizeof(buffer) - 1)] = '\0';
		text = buffer;
		return B_OK;
	}
	BPath path;
	status_t status = processing_path(device, path);
	if (status != B_OK)
		return status;
	BFile file(path.Path(), B_READ_ONLY);
	char buffer[1025];
	ssize_t bytes = file.InitCheck() == B_OK
		? file.Read(buffer, sizeof(buffer) - 1) : 0;
	if (bytes > 0) {
		buffer[bytes] = '\0';
		text = buffer;
	}
	return B_OK;
}


BString
input_processing_value(const BString& text, const char* key,
	const char* fallback)
{
	BStringList lines;
	text.Split("\n", true, lines);
	BString prefix(key);
	prefix << " ";
	for (int32 i = 0; i < lines.CountStrings(); i++) {
		if (lines.StringAt(i).StartsWith(prefix)) {
			BString value;
			lines.StringAt(i).CopyInto(value, prefix.Length(),
				lines.StringAt(i).Length() - prefix.Length());
			return value.Trim();
		}
	}
	return BString(fallback);
}


status_t
set_input_processing(const char* device, const char* text)
{
	BParameterWeb* web;
	BParameter* parameter = processing_parameter(device, &web);
	if (parameter != NULL) {
		status_t status = parameter->SetValue(text, strlen(text) + 1,
			system_time());
		delete web;
		return status;
	}
	// not running: merged into the file its node reads when it starts
	BString merged;
	get_input_processing(device, merged);
	const char* keys[] = { "mute", "gate", "agc", "highpass" };
	BString result;
	for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
		BString value = input_processing_value(text, keys[i],
			input_processing_value(merged, keys[i], "0").String());
		result << keys[i] << " " << value << "\n";
	}
	BPath path;
	status_t status = processing_path(device, path);
	if (status != B_OK)
		return status;
	BPath parent;
	path.GetParent(&parent);
	create_directory(parent.Path(), 0755);
	BFile file(path.Path(), B_WRITE_ONLY | B_CREATE_FILE | B_ERASE_FILE);
	status = file.InitCheck();
	if (status == B_OK)
		file.Write(result.String(), result.Length());
	return status;
}


int32
set_all_inputs_muted(bool muted)
{
	AudioDeviceList devices;
	if (devices.Update(false) != B_OK)
		return 0;
	int32 changed = 0;
	BStringList names = devices.InputNames();
	for (int32 i = 0; i < names.CountStrings(); i++) {
		if (set_input_processing(names.StringAt(i).String(),
				muted ? "mute 1\n" : "mute 0\n") == B_OK) {
			changed++;
		}
	}
	return changed;
}


bool
any_input_muted()
{
	AudioDeviceList devices;
	if (devices.Update(false) != B_OK)
		return false;
	BStringList names = devices.InputNames();
	for (int32 i = 0; i < names.CountStrings(); i++) {
		BString text;
		if (get_input_processing(names.StringAt(i).String(), text) == B_OK
			&& input_processing_value(text, "mute", "0") == "1") {
			return true;
		}
	}
	return false;
}


static status_t
keys_path(BPath& path)
{
	status_t status = find_directory(B_USER_SETTINGS_DIRECTORY, &path);
	if (status == B_OK)
		status = path.Append("Media/audio_keys");
	return status;
}


status_t
read_audio_keys(audio_keys& keys)
{
	keys.pushToTalk = 0;
	keys.muteToggle = 0;
	BPath path;
	status_t status = keys_path(path);
	if (status != B_OK)
		return status;
	BFile file(path.Path(), B_READ_ONLY);
	char buffer[257];
	ssize_t bytes = file.InitCheck() == B_OK
		? file.Read(buffer, sizeof(buffer) - 1) : 0;
	if (bytes <= 0)
		return B_OK;
	buffer[bytes] = '\0';
	BString text(buffer);
	keys.pushToTalk = atoi(input_processing_value(text, "ptt", "0").String());
	keys.muteToggle = atoi(input_processing_value(text, "mute", "0").String());
	return B_OK;
}


status_t
write_audio_keys(const audio_keys& keys)
{
	BPath path;
	status_t status = keys_path(path);
	if (status != B_OK)
		return status;
	if (keys.pushToTalk == 0 && keys.muteToggle == 0) {
		BEntry(path.Path()).Remove();
		return B_OK;
	}
	BPath parent;
	path.GetParent(&parent);
	create_directory(parent.Path(), 0755);
	char text[64];
	snprintf(text, sizeof(text), "ptt %" B_PRId32 "\nmute %" B_PRId32 "\n",
		keys.pushToTalk, keys.muteToggle);
	BFile file(path.Path(), B_WRITE_ONLY | B_CREATE_FILE | B_ERASE_FILE);
	status = file.InitCheck();
	if (status == B_OK)
		file.Write(text, strlen(text));
	return status;
}
