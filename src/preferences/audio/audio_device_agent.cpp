/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 */

/*!	The audio device agent (roadmap F-59): media_server starts it when the
	audio devices changed and settled. It applies the per-device policies
	(AudioDevicePolicy.h), shows a prompt for a new device that asks
	(AudioDeviceAgent.h), and quits when there is nothing left to show.
*/


#include <Application.h>

#include "AudioDeviceAgent.h"
#include "MicrophoneProcessing.h"


class AgentApp : public BApplication {
public:
	AgentApp()
		:
		BApplication("application/x-vnd.Renga-AudioDeviceAgent")
	{
	}

	virtual void ReadyToRun()
	{
		if (handle_audio_devices_changed() == 0 && !_KeysInUse())
			PostMessage(B_QUIT_REQUESTED);
	}

	virtual void MessageReceived(BMessage* message)
	{
		if (message->what == B_SILENT_RELAUNCH) {
			// media_server started the agent again while it was running
			// (the devices changed once more). It had no resources before,
			// and a second instance started; now it is a background app
			// with B_EXCLUSIVE_LAUNCH (AudioDeviceAgent.rdef), and the
			// running one looks again
			if (handle_audio_devices_changed() == 0 && CountWindows() == 0
				&& !_KeysInUse())
				PostMessage(B_QUIT_REQUESTED);
			return;
		}
		if (message->what == kMsgAgentPromptClosed) {
			// the closing prompt is still counted
			if (CountWindows() <= 1 && !_KeysInUse())
				PostMessage(B_QUIT_REQUESTED);
			return;
		}
		if (message->what == kMsgMicrophoneKey) {
			// batch 11: push-to-talk and the mute key, from the audio_keys
			// input_server filter
			bool muted;
			if (message->GetBool("toggle", false))
				muted = !any_input_muted();
			else
				muted = message->GetBool("mute", false);
			set_all_inputs_muted(muted);
			if (!_KeysInUse() && CountWindows() == 0)
				PostMessage(B_QUIT_REQUESTED);
			return;
		}
		BApplication::MessageReceived(message);
	}

private:
	//!	With push-to-talk or a mute key the agent stays, to answer fast.
	bool _KeysInUse()
	{
		audio_keys keys;
		read_audio_keys(keys);
		return keys.pushToTalk != 0 || keys.muteToggle != 0;
	}
};


int
main()
{
	AgentApp app;
	app.Run();
	return 0;
}
