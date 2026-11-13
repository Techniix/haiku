/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 */
#ifndef AUDIO_DEVICE_POLICY_H
#define AUDIO_DEVICE_POLICY_H


#include <Message.h>
#include <String.h>
#include <StringList.h>


/*!	What happens when an audio device is connected (roadmap F-59), per
	device and per direction: ask (a prompt with "Switch to it" and "Not
	now"), switch to it automatically, or do nothing; and whether the
	previous default comes back when the device is disconnected. Only the
	default output and input change: routes of single applications (F-32,
	F-58 O-4) are left as they are.

	Kept in ~/config/settings/Media/audio_device_policies (a flattened
	BMessage), together with the devices present the last time
	(to tell new ones from known ones) and, for each device the default
	was switched to, what the default was before. The Audio preferences'
	Devices page edits the policies; the agent (audio_device_agent,
	started by media_server when the devices change, AudioDeviceAgent.h)
	applies them.
*/

enum {
	AUDIO_DEVICE_ASK		= 0,
	AUDIO_DEVICE_SWITCH		= 1,
	AUDIO_DEVICE_IGNORE		= 2
};


struct AudioDevicePolicy {
	int32		output;		// AUDIO_DEVICE_*
	int32		input;
	bool		switchBack;

	AudioDevicePolicy()
		:
		output(AUDIO_DEVICE_ASK),
		input(AUDIO_DEVICE_ASK),
		switchBack(true)
	{
	}

	bool IsDefault() const
	{
		return output == AUDIO_DEVICE_ASK && input == AUDIO_DEVICE_ASK
			&& switchBack;
	}
};


class AudioDevicePolicies {
public:
								AudioDevicePolicies();

			status_t			Load();
			status_t			Save() const;

			AudioDevicePolicy	Get(const char* device) const;
			void				Set(const char* device,
									const AudioDevicePolicy& policy);
			//!	Every device back to the defaults.
			void				Reset();
			bool				IsDefault() const;

			//!	The devices present the last time the agent looked.
			bool				HasKnown() const { return fHasKnown; }
			const BStringList&	KnownOutputs() const { return fKnownOutputs; }
			const BStringList&	KnownInputs() const { return fKnownInputs; }
			void				SetKnown(const BStringList& outputs,
									const BStringList& inputs);

			//!	What the default output/input was before it was switched
			//	to \a device (empty: it was not).
			BString				SwitchedFrom(const char* device,
									bool output) const;
			void				SetSwitchedFrom(const char* device,
									bool output, const char* previous);
			//!	The default given back when \a device went ("switch
			//	back"); media_server takes the device back as the default
			//	when it returns (its saved default), which the agent then
			//	undoes unless the policy says to switch.
			BString				SwitchedBackTo(const char* device,
									bool output) const;
			void				SetSwitchedBackTo(const char* device,
									bool output, const char* previous);

	static	status_t			Path(BString& path);

private:
			BMessage			fPolicies;	// "name" -> message
			BMessage			fSwitched;	// "output:<name>" -> previous
			BStringList			fKnownOutputs;
			BStringList			fKnownInputs;
			bool				fHasKnown;
};


#endif	// AUDIO_DEVICE_POLICY_H
