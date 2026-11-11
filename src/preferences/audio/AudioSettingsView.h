/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 */
#ifndef AUDIO_SETTINGS_VIEW_H
#define AUDIO_SETTINGS_VIEW_H


#include <GroupView.h>
#include <ObjectList.h>
#include <String.h>
#include <StringList.h>

#include "AudioDevices.h"


class ApplicationVolumesView;
class BButton;
class BCardLayout;
class BCheckBox;
class BListView;
class BMediaRecorder;
class BMenuField;
class BMessageRunner;
class BSlider;
class BStringView;
class BTextControl;
class BTextView;
class LevelMeter;
class MixerControl;
class OutputMenuField;


/*!	One page of the audio settings. A page has everyday controls, shown
	always, and advanced ones, shown with "Show advanced settings"; and
	words the search matches (each with whether it needs the advanced
	settings).
*/
class AudioPage : public BGroupView {
public:
								AudioPage(const char* name,
									const char* title);

			const char*			Title() const { return fTitle.String(); }

	virtual	void				SetAdvanced(bool advanced) = 0;
	//!	Called once a second while shown, and when devices change.
	virtual	void				Refresh() = 0;
	//!	The page became visible (or was hidden).
	virtual	void				PageShown(bool shown) {}

	//!	Whether \a text (lower case) matches this page; \a _advanced is
	//	set when only advanced settings match.
			bool				Matches(const BString& text,
									bool* _advanced) const;

protected:
			void				AddKeywords(const char* words,
									bool advanced);

			BString				fTitle;
			BStringList			fKeywords;
			BStringList			fAdvancedKeywords;
};


//!	Output: the device that plays, master volume and mute, a test sound;
//	advanced: the device's buffer size.
class OutputPage : public AudioPage {
public:
								OutputPage(AudioDeviceList& devices);
	virtual						~OutputPage();

	virtual	void				AttachedToWindow();
	virtual	void				MessageReceived(BMessage* message);
	virtual	void				SetAdvanced(bool advanced);
	virtual	void				Refresh();

private:
			void				_ConnectVolume();
			void				_PlayTestSound();

			AudioDeviceList&	fDevices;
			OutputMenuField*	fDevice;
			BStringView*		fDeviceInfo;
			BSlider*			fVolume;
			BCheckBox*			fMute;
			BButton*			fTestSound;
			BMenuField*			fBufferSize;
			BStringView*		fBufferNote;
			BView*				fAdvancedBox;
			MixerControl*		fMixer;
			bigtime_t			fVolumeChanged;
			BString				fBufferKey;
};


//!	Input: the device that records, recording gain and a level meter;
//	advanced: recording what applications play (loopback).
class InputPage : public AudioPage {
public:
								InputPage(AudioDeviceList& devices);
	virtual						~InputPage();

	virtual	void				AttachedToWindow();
	virtual	void				MessageReceived(BMessage* message);
	virtual	void				SetAdvanced(bool advanced);
	virtual	void				Refresh();
	virtual	void				PageShown(bool shown);

			LevelMeter*			Meter() const { return fMeter; }

private:
			void				_UpdateGain();
			void				_StartMeter();
			void				_StopMeter();
	static	void				_RecordHook(void* cookie,
									bigtime_t timestamp, void* data,
									size_t size, const media_format& format);

			AudioDeviceList&	fDevices;
			BMenuField*			fDevice;
			BString				fDeviceKey;
			BSlider*			fGain;
			bigtime_t			fGainChanged;
			LevelMeter*			fMeter;
			BMediaRecorder*		fRecorder;
			BString				fMeterInput;
			BView*				fLoopbackBox;
			BMenuField*			fLoopbackSource;
			BString				fLoopbackKey;
};


//!	Applications: volume and mute of every application that plays;
//	advanced: where each one plays and its low-latency switch.
class ApplicationsPage : public AudioPage {
public:
								ApplicationsPage();

	virtual	void				SetAdvanced(bool advanced);
	virtual	void				Refresh();

private:
			ApplicationVolumesView* fApplications;
			BStringView*		fAdvancedNote;
};


//!	Devices: every sound card, with its role and rate, following hot-plug;
//	the selected card's details; advanced: its buffer size, restarting the
//	media services.
class DevicesPage : public AudioPage {
public:
								DevicesPage(AudioDeviceList& devices);

	virtual	void				AttachedToWindow();
	virtual	void				MessageReceived(BMessage* message);
	virtual	void				SetAdvanced(bool advanced);
	virtual	void				Refresh();

			BListView*			List() const { return fList; }

private:
			void				_ShowDetails();
	static	status_t			_RestartMediaServices(void* data);

			AudioDeviceList&	fDevices;
			BListView*			fList;
			BString				fListKey;
			BTextView*			fDetails;
			BMenuField*			fBufferSize;
			BString				fBufferKey;
			BButton*			fMoreSettings;
			BButton*			fRestart;
			BView*				fAdvancedBox;
			bool				fAdvanced;
};


/*!	The whole audio settings: a sidebar with the pages and a search field,
	the page, and "Show advanced settings". A view, not a window, so that
	the Control Panel (F-28) can take it as its Sound page; the Audio
	preferences' window holds one.
*/
class AudioSettingsView : public BGroupView {
public:
								AudioSettingsView();
	virtual						~AudioSettingsView();

	virtual	void				AttachedToWindow();
	virtual	void				DetachedFromWindow();
	virtual	void				MessageReceived(BMessage* message);

			void				SelectPage(const char* name);
			void				SetAdvanced(bool advanced);
			void				Search(const char* text);

			//!	The state for scripting and the tests: page, advanced,
			//	devices, defaults, streams, level meter.
			void				GetState(BMessage* state);

private:
			void				_ShowPage(int32 index);
			void				_RefreshDevices(bool details);
			int32				_PageIndex(const char* name) const;

			AudioDeviceList		fDevices;
			BObjectList<AudioPage> fPages;
			BTextControl*		fSearch;
			BListView*			fSidebar;
			BStringView*		fTitle;
			BStringView*		fSearchNote;
			BCardLayout*		fCards;
			BCheckBox*			fAdvanced;
			BMessageRunner*		fRunner;
			BMessageRunner*		fDevicesRunner;
			int32				fCurrent;
			int32				fRefreshes;
			OutputPage*			fOutputPage;
			InputPage*			fInputPage;
			DevicesPage*		fDevicesPage;
};


#endif	// AUDIO_SETTINGS_VIEW_H
