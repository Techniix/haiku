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


class AgentApp : public BApplication {
public:
	AgentApp()
		:
		BApplication("application/x-vnd.Renga-AudioDeviceAgent")
	{
	}

	virtual void ReadyToRun()
	{
		if (handle_audio_devices_changed() == 0)
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
			if (handle_audio_devices_changed() == 0 && CountWindows() == 0)
				PostMessage(B_QUIT_REQUESTED);
			return;
		}
		if (message->what == kMsgAgentPromptClosed) {
			// the closing prompt is still counted
			if (CountWindows() <= 1)
				PostMessage(B_QUIT_REQUESTED);
			return;
		}
		BApplication::MessageReceived(message);
	}
};


int
main()
{
	AgentApp app;
	app.Run();
	return 0;
}
