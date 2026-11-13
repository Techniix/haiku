/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 */


#include "AudioDevicePolicy.h"

#include <Directory.h>
#include <File.h>
#include <FindDirectory.h>
#include <Path.h>


AudioDevicePolicies::AudioDevicePolicies()
	:
	fHasKnown(false)
{
}


/*static*/ status_t
AudioDevicePolicies::Path(BString& _path)
{
	BPath path;
	status_t status = find_directory(B_USER_SETTINGS_DIRECTORY, &path);
	if (status != B_OK)
		return status;
	path.Append("Media");
	create_directory(path.Path(), 0755);
	path.Append("audio_device_policies");
	_path = path.Path();
	return B_OK;
}


status_t
AudioDevicePolicies::Load()
{
	fPolicies.MakeEmpty();
	fSwitched.MakeEmpty();
	fKnownOutputs.MakeEmpty();
	fKnownInputs.MakeEmpty();
	fHasKnown = false;

	BString path;
	status_t status = Path(path);
	if (status != B_OK)
		return status;
	BFile file(path.String(), B_READ_ONLY);
	BMessage settings;
	status = file.InitCheck();
	if (status == B_OK)
		status = settings.Unflatten(&file);
	if (status != B_OK)
		return status;

	settings.FindMessage("policies", &fPolicies);
	settings.FindMessage("switched", &fSwitched);
	fHasKnown = settings.GetBool("known", false);
	const char* name;
	for (int32 i = 0; settings.FindString("known output", i, &name) == B_OK;
			i++) {
		fKnownOutputs.Add(name);
	}
	for (int32 i = 0; settings.FindString("known input", i, &name) == B_OK;
			i++) {
		fKnownInputs.Add(name);
	}
	return B_OK;
}


status_t
AudioDevicePolicies::Save() const
{
	BString path;
	status_t status = Path(path);
	if (status != B_OK)
		return status;
	BMessage settings;
	settings.AddMessage("policies", &fPolicies);
	settings.AddMessage("switched", &fSwitched);
	settings.AddBool("known", fHasKnown);
	for (int32 i = 0; i < fKnownOutputs.CountStrings(); i++)
		settings.AddString("known output", fKnownOutputs.StringAt(i));
	for (int32 i = 0; i < fKnownInputs.CountStrings(); i++)
		settings.AddString("known input", fKnownInputs.StringAt(i));
	BFile file(path.String(), B_WRITE_ONLY | B_CREATE_FILE | B_ERASE_FILE);
	status = file.InitCheck();
	if (status == B_OK)
		status = settings.Flatten(&file);

	return status;
}


AudioDevicePolicy
AudioDevicePolicies::Get(const char* device) const
{
	AudioDevicePolicy policy;
	BMessage entry;
	if (fPolicies.FindMessage(device, &entry) == B_OK) {
		policy.output = entry.GetInt32("output", policy.output);
		policy.input = entry.GetInt32("input", policy.input);
		policy.switchBack = entry.GetBool("switch back", policy.switchBack);
	}
	return policy;
}


void
AudioDevicePolicies::Set(const char* device, const AudioDevicePolicy& policy)
{
	fPolicies.RemoveName(device);
	if (policy.IsDefault())
		return;
	BMessage entry;
	entry.AddInt32("output", policy.output);
	entry.AddInt32("input", policy.input);
	entry.AddBool("switch back", policy.switchBack);
	fPolicies.AddMessage(device, &entry);
}


void
AudioDevicePolicies::Reset()
{
	fPolicies.MakeEmpty();
}


bool
AudioDevicePolicies::IsDefault() const
{
	return fPolicies.CountNames(B_ANY_TYPE) == 0;
}


void
AudioDevicePolicies::SetKnown(const BStringList& outputs,
	const BStringList& inputs)
{
	fKnownOutputs = outputs;
	fKnownInputs = inputs;
	fHasKnown = true;
}


BString
AudioDevicePolicies::SwitchedFrom(const char* device, bool output) const
{
	BString key;
	key << (output ? "output:" : "input:") << device;
	return BString(fSwitched.GetString(key.String(), ""));
}


void
AudioDevicePolicies::SetSwitchedFrom(const char* device, bool output,
	const char* previous)
{
	BString key;
	key << (output ? "output:" : "input:") << device;
	fSwitched.RemoveName(key.String());
	if (previous != NULL && previous[0] != '\0')
		fSwitched.AddString(key.String(), previous);
}


BString
AudioDevicePolicies::SwitchedBackTo(const char* device, bool output) const
{
	BString key;
	key << (output ? "back-output:" : "back-input:") << device;
	return BString(fSwitched.GetString(key.String(), ""));
}


void
AudioDevicePolicies::SetSwitchedBackTo(const char* device, bool output,
	const char* previous)
{
	BString key;
	key << (output ? "back-output:" : "back-input:") << device;
	fSwitched.RemoveName(key.String());
	if (previous != NULL && previous[0] != '\0')
		fSwitched.AddString(key.String(), previous);
}
