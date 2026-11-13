/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 */


#include "AudioDeviceAgent.h"

#include <Application.h>
#include <Button.h>
#include <Catalog.h>
#include <CheckBox.h>
#include <LayoutBuilder.h>
#include <MessageRunner.h>
#include <Roster.h>
#include <Screen.h>
#include <StringView.h>

#include <stdio.h>

#include "AudioDevicePolicy.h"
#include "AudioDevices.h"


#undef B_TRANSLATION_CONTEXT
#define B_TRANSLATION_CONTEXT "AudioDeviceAgent"


static const bigtime_t kPromptTimeout = 60000000;
static const uint32 kMsgTimeout = 'AdTo';
static const char* kAudioSignature = "application/x-vnd.Renga-Audio";


/*!	Makes \a device the default output or input, remembering the previous
	one for "switch back".
*/
static void
switch_to(AudioDeviceList& devices, AudioDevicePolicies& policies,
	const char* device, bool output)
{
	BString previous = output ? devices.DefaultOutputName()
		: devices.DefaultInputName();
	if (previous == device)
		return;
	status_t status = output ? devices.SetDefaultOutput(device)
		: devices.SetDefaultInput(device);
	printf("Audio: %s switched to \"%s\" (was \"%s\"): %s\n",
		output ? "output" : "input", device, previous.String(),
		strerror(status));
	if (status == B_OK)
		policies.SetSwitchedFrom(device, output, previous.String());
}


/*!	What identifies a device's appearance: its name and its dormant flavor
	(add-on and flavor id). A device plugged in again gets a new flavor id
	from its add-on, so it counts as new even if the agent did not run
	while it was away.
*/
static BString
device_key(const AudioDevice* device)
{
	BString key;
	key << device->name << "\t" << device->info.addon << ":"
		<< device->info.flavor_id;
	return key;
}


static BString
key_name(const BString& key)
{
	int32 tab = key.FindFirst('\t');
	if (tab < 0)
		return key;
	BString name;
	key.CopyInto(name, 0, tab);
	return name;
}


int32
handle_audio_devices_changed()
{
	AudioDevicePolicies policies;
	policies.Load();
	AudioDeviceList devices;
	if (devices.Update(false) != B_OK)
		return 0;
	BStringList outputs = devices.OutputNames();
	BStringList inputs = devices.InputNames();
	BStringList outputKeys;
	BStringList inputKeys;
	for (int32 i = 0; i < devices.CountDevices(); i++) {
		const AudioDevice* device = devices.DeviceAt(i);
		if (device->output)
			outputKeys.Add(device_key(device));
		if (device->input)
			inputKeys.Add(device_key(device));
	}
	if (outputs.IsEmpty() && inputs.IsEmpty()) {
		// The media services are (re)starting and nothing is listed yet:
		// nothing to compare with, and nothing that went (the next change
		// brings the agent back).
		printf("Audio: no audio devices listed yet\n");
		return 0;
	}

	if (!policies.HasKnown()) {
		// The first time: what is there now is known, nothing is new.
		policies.SetKnown(outputKeys, inputKeys);
		policies.Save();
		printf("Audio: %d outputs and %d inputs recorded\n",
			(int)outputs.CountStrings(), (int)inputs.CountStrings());
		return 0;
	}

	// Devices that went: switch back where the policy says so.
	for (int32 direction = 0; direction < 2; direction++) {
		bool output = direction == 0;
		const BStringList& known = output ? policies.KnownOutputs()
			: policies.KnownInputs();
		const BStringList& now = output ? outputs : inputs;
		for (int32 i = 0; i < known.CountStrings(); i++) {
			BString name = key_name(known.StringAt(i));
			if (now.HasString(name))
				continue;
			BString previous = policies.SwitchedFrom(name, output);
			if (previous.Length() > 0 && policies.Get(name).switchBack
				&& now.HasString(previous)) {
				status_t status = output
					? devices.SetDefaultOutput(previous)
					: devices.SetDefaultInput(previous);
				printf("Audio: \"%s\" went, %s back to \"%s\": %s\n",
					name.String(), output ? "output" : "input",
					previous.String(), strerror(status));
				if (status == B_OK)
					policies.SetSwitchedBackTo(name, output, previous);
			}
			policies.SetSwitchedFrom(name, output, NULL);
		}
	}

	// New devices: switch, ask or leave them.
	BStringList seen;
	int32 prompts = 0;
	for (int32 direction = 0; direction < 2; direction++) {
		const BStringList& now = direction == 0 ? outputs : inputs;
		for (int32 i = 0; i < now.CountStrings(); i++) {
			BString name = now.StringAt(i);
			if (seen.HasString(name))
				continue;
			seen.Add(name);
			const AudioDevice* device = devices.FindDevice(name);
			BString key = device != NULL ? device_key(device) : name;
			bool newOutput = outputs.HasString(name)
				&& !policies.KnownOutputs().HasString(key);
			bool newInput = inputs.HasString(name)
				&& !policies.KnownInputs().HasString(key);
			if (!newOutput && !newInput)
				continue;
			AudioDevicePolicy policy = policies.Get(name);
			// It was switched back from when it went; media_server takes
			// it back as its saved default now that it is here: undo that
			// unless the policy is to switch to it.
			for (int32 dir = 0; dir < 2; dir++) {
				bool output = dir == 0;
				BString back = policies.SwitchedBackTo(name, output);
				if (back.IsEmpty())
					continue;
				policies.SetSwitchedBackTo(name, output, NULL);
				int32 value = output ? policy.output : policy.input;
				BString current = output ? devices.DefaultOutputName()
					: devices.DefaultInputName();
				const BStringList& list = output ? outputs : inputs;
				if (value != AUDIO_DEVICE_SWITCH && current == name
					&& list.HasString(back)) {
					status_t status = output
						? devices.SetDefaultOutput(back)
						: devices.SetDefaultInput(back);
					printf("Audio: \"%s\" is back, %s stays \"%s\": %s\n",
						name.String(), output ? "output" : "input",
						back.String(), strerror(status));
				}
			}
			if (newOutput && policy.output == AUDIO_DEVICE_SWITCH)
				switch_to(devices, policies, name, true);
			if (newInput && policy.input == AUDIO_DEVICE_SWITCH)
				switch_to(devices, policies, name, false);
			bool askOutput = newOutput && policy.output == AUDIO_DEVICE_ASK;
			bool askInput = newInput && policy.input == AUDIO_DEVICE_ASK;
			printf("Audio: \"%s\" connected (output %s, input %s)\n",
				name.String(), newOutput ? (askOutput ? "ask"
					: policy.output == AUDIO_DEVICE_SWITCH ? "switch"
					: "nothing") : "-", newInput ? (askInput ? "ask"
					: policy.input == AUDIO_DEVICE_SWITCH ? "switch"
					: "nothing") : "-");
			if (askOutput || askInput) {
				(new DevicePromptWindow(name, askOutput, askInput))->Show();
				prompts++;
			}
		}
	}

	policies.SetKnown(outputKeys, inputKeys);
	policies.Save();
	fflush(stdout);
	return prompts;
}


// #pragma mark - DevicePromptWindow


DevicePromptWindow::DevicePromptWindow(const char* device, bool output,
	bool input)
	:
	BWindow(BRect(0, 0, 300, 100), B_TRANSLATE("New audio device"),
		B_FLOATING_WINDOW_LOOK, B_FLOATING_ALL_WINDOW_FEEL,
		B_NOT_RESIZABLE | B_NOT_ZOOMABLE | B_NOT_MINIMIZABLE
			| B_AUTO_UPDATE_SIZE_LIMITS | B_CLOSE_ON_ESCAPE,
		B_ALL_WORKSPACES),
	fDevice(device),
	fOutput(output),
	fInput(input),
	fAnswered(false)
{
	BString text;
	text.SetToFormat(B_TRANSLATE("\"%s\" was connected."), device);
	BStringView* title = new BStringView("title", text.String());
	title->SetFont(be_bold_font);

	const char* question = output && input
		? B_TRANSLATE("Use it for playback and recording?")
		: output ? B_TRANSLATE("Play sound through it?")
		: B_TRANSLATE("Record from it?");
	BStringView* label = new BStringView("question", question);

	fRemember = new BCheckBox("remember",
		B_TRANSLATE("Remember my choice for this device"), NULL);
	BButton* switchButton = new BButton("switch",
		B_TRANSLATE("Switch to it"), new BMessage(kMsgPromptSwitch));
	BButton* notNow = new BButton("notNow", B_TRANSLATE("Not now"),
		new BMessage(kMsgPromptNotNow));
	BButton* settings = new BButton("settings",
		B_TRANSLATE("Settings" B_UTF8_ELLIPSIS),
		new BMessage(kMsgPromptSettings));

	BLayoutBuilder::Group<>(this, B_VERTICAL, B_USE_SMALL_SPACING)
		.SetInsets(B_USE_WINDOW_SPACING)
		.Add(title)
		.Add(label)
		.Add(fRemember)
		.AddGroup(B_HORIZONTAL)
			.Add(settings)
			.AddGlue()
			.Add(notNow)
			.Add(switchButton)
		.End();
	SetDefaultButton(switchButton);

	// Bottom right, where notifications appear.
	BRect screen = BScreen(this).Frame();
	ResizeToPreferred();
	MoveTo(screen.right - Bounds().Width() - 16,
		screen.bottom - Bounds().Height() - 48);

	BMessage timeout(kMsgTimeout);
	BMessageRunner::StartSending(BMessenger(this), &timeout, kPromptTimeout,
		1);
}


void
DevicePromptWindow::MessageReceived(BMessage* message)
{
	switch (message->what) {
		case kMsgPromptSwitch:
		case kMsgPromptNotNow:
		{
			bool remember = fRemember->Value() != 0;
			message->FindBool("remember", &remember);
			_Answer(message->what == kMsgPromptSwitch, remember);
			if (message->IsSourceWaiting()) {
				BMessage reply(B_REPLY);
				message->SendReply(&reply);
			}
			PostMessage(B_QUIT_REQUESTED);
			break;
		}

		case kMsgPromptSettings:
		{
			const char* args[] = { "--page", "devices", "--device",
				fDevice.String(), NULL };
			be_roster->Launch(kAudioSignature, 4, (char**)args);
			break;
		}

		case kMsgTimeout:
			PostMessage(B_QUIT_REQUESTED);
			break;

		default:
			BWindow::MessageReceived(message);
	}
}


bool
DevicePromptWindow::QuitRequested()
{
	if (!fAnswered)
		_Answer(false, false);
	// The agent quits with its last prompt.
	be_app->PostMessage(kMsgAgentPromptClosed);
	return true;
}


void
DevicePromptWindow::_Answer(bool switchTo, bool remember)
{
	fAnswered = true;
	AudioDevicePolicies policies;
	policies.Load();
	if (switchTo) {
		AudioDeviceList devices;
		devices.Update(false);
		if (fOutput)
			switch_to(devices, policies, fDevice, true);
		if (fInput)
			switch_to(devices, policies, fDevice, false);
	}
	if (remember) {
		AudioDevicePolicy policy = policies.Get(fDevice);
		int32 value = switchTo ? AUDIO_DEVICE_SWITCH : AUDIO_DEVICE_IGNORE;
		if (fOutput)
			policy.output = value;
		if (fInput)
			policy.input = value;
		policies.Set(fDevice, policy);
	}
	policies.Save();
	printf("Audio: \"%s\": %s%s\n", fDevice.String(),
		switchTo ? "switched to it" : "not now",
		remember ? ", remembered" : "");
	fflush(stdout);
}
