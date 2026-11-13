/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 */
#ifndef AUDIO_DEVICE_AGENT_H
#define AUDIO_DEVICE_AGENT_H


#include <String.h>
#include <StringList.h>
#include <Window.h>


class BButton;
class BCheckBox;


/*!	A new audio device was connected (roadmap F-59): the prompt.

	"<name> was connected." with "Switch to it" (the default output and/or
	input, the directions whose policy is "ask") and "Not now"; "Remember
	my choice" stores "switch automatically" or "do nothing" for them;
	"Settings…" opens the Audio preferences' Devices page on the device.
	Closes itself after kPromptTimeout as "Not now".

	Scripting for tests: kMsgPromptSwitch / kMsgPromptNotNow with an
	optional bool "remember" do what the buttons do.
*/
enum {
	kMsgPromptSwitch	= 'AdSw',
	kMsgPromptNotNow	= 'AdNn',
	kMsgPromptSettings	= 'AdSt',
	kMsgAgentPromptClosed = 'AdQt'
};


class DevicePromptWindow : public BWindow {
public:
								DevicePromptWindow(const char* device,
									bool output, bool input);

	virtual	void				MessageReceived(BMessage* message);
	virtual	bool				QuitRequested();

private:
			void				_Answer(bool switchTo, bool remember);

			BString				fDevice;
			bool				fOutput;
			bool				fInput;
			BCheckBox*			fRemember;
			bool				fAnswered;
};


/*!	What the agent does (/boot/system/servers/audio_device_agent, which
	media_server starts when the audio devices change; a program of its own,
	so that it never shares an instance with the Audio preferences): compares the devices with those of the
	last time, switches the defaults to new devices whose policy says so,
	opens a prompt for those that ask, and switches back for devices that
	went. Returns the number of prompts opened.
*/
int32		handle_audio_devices_changed();


#endif	// AUDIO_DEVICE_AGENT_H
