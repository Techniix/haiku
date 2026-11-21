/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 */
#ifndef AUDIO_MIXING_PAGES_H
#define AUDIO_MIXING_PAGES_H


#include "AudioSettingsView.h"

#include <AudioStreamControl.h>


class BListView;
class BSlider;
class BStringView;
namespace BPrivate {
	class WrappingNote;
}
class BTextControl;


/*!	Batch 11 (F-31 routing): the microphone page. Per input device: mute,
	noise gate with its threshold, automatic gain, high-pass filter (the
	node's "renga:input_processing"); for all inputs: push-to-talk and a
	mute key (the audio_keys input_server filter).
*/
class MicrophonePage : public AudioPage {
public:
								MicrophonePage(AudioDeviceList& devices);

	virtual	void				AttachedToWindow();
	virtual	void				MessageReceived(BMessage* message);
	virtual	void				SetAdvanced(bool advanced);
	virtual	void				Refresh();
	virtual	void				PageShown(bool shown);

			void				GetState(BMessage* state);

private:
			bool				fWasShown;
			void				_Read();
			void				_Write();
			void				_WriteKeys();
			void				_FillKeyMenu(BMenuField* field,
									uint32 what);
			void				_MarkKey(BMenuField* field, int32 key);

			AudioDeviceList&	fDevices;
			BMenuField*			fDevice;
			BString				fDeviceName;
			BString				fDeviceKey;
			BCheckBox*			fMute;
			BCheckBox*			fGate;
			BSlider*			fGateThreshold;
			BCheckBox*			fAutoGain;
			BCheckBox*			fHighPass;
			BCheckBox*			fPushToTalk;
			BMenuField*			fPushToTalkKey;
			BMenuField*			fMuteKey;
			BView*				fKeysBox;
			bigtime_t			fChanged;
};


/*!	Batch 11 (F-31 routing): virtual devices, several inputs at once
	(combined into a virtual device) and ducking.
*/
class MixingPage : public AudioPage {
public:
								MixingPage(AudioDeviceList& devices);

	virtual	void				AttachedToWindow();
	virtual	void				MessageReceived(BMessage* message);
	virtual	void				SetAdvanced(bool advanced);
	virtual	void				Refresh();
	virtual	void				PageShown(bool shown);

			void				GetState(BMessage* state);

private:
			bool				fWasShown;
			void				_RebuildCombine();
			void				_RebuildTriggers();
			void				_WriteDucking();
			void				_WriteCombined();

			AudioDeviceList&	fDevices;
			BPrivate::AudioStreamControl fControl;
			BListView*			fVirtualList;
			BTextControl*		fVirtualName;
			BButton*			fVirtualRemove;
			BStringList			fVirtual;
			BString				fVirtualKey;
			BMenuField*			fCombineTarget;
			BGroupView*			fCombineGroup;
			BString				fCombineKey;
			BStringList			fCombined;		// "<virtual>\t<device>"
			BCheckBox*			fDucking;
			BSlider*			fDuckAmount;
			BGroupView*			fTriggerGroup;
			BString				fTriggerKey;
			BStringList			fTriggers;
			BPrivate::WrappingNote*	fDuckState;
			bool				fDuckActive;
			bigtime_t			fChanged;
};


/*!	Batch 11 (F-31 routing): presets. A preset holds the ducking settings,
	the default input's microphone processing, the keys and the default
	output and input; built-in ones set only what they are about. Kept in
	~/config/settings/Media/audio_presets/<name> (flattened BMessages).
*/
class PresetsPage : public AudioPage {
public:
								PresetsPage(AudioDeviceList& devices);

	virtual	void				AttachedToWindow();
	virtual	void				MessageReceived(BMessage* message);
	virtual	void				SetAdvanced(bool advanced) {}
	virtual	void				Refresh();

			void				GetState(BMessage* state);

	static	status_t			Apply(const char* name,
									AudioDeviceList& devices);

private:
			void				_Fill();

			AudioDeviceList&	fDevices;
			BListView*			fList;
			BTextControl*		fName;
			BButton*			fApply;
			BButton*			fDelete;
			BPrivate::WrappingNote*	fStatus;
			BString				fLastApplied;
};


#endif	// AUDIO_MIXING_PAGES_H
