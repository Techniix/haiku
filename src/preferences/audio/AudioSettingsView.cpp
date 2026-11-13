/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 */


#include "AudioSettingsView.h"

#include "AudioDevicePolicy.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include <Box.h>
#include <Button.h>
#include <CardLayout.h>
#include <Catalog.h>
#include <CheckBox.h>
#include <ControlLook.h>
#include <LayoutBuilder.h>
#include <ListView.h>
#include <MediaDefs.h>
#include <MediaRecorder.h>
#include <MediaRoster.h>
#include <MenuField.h>
#include <MenuItem.h>
#include <MessageRunner.h>
#include <ParameterWeb.h>
#include <PopUpMenu.h>
#include <Roster.h>
#include <ScrollView.h>
#include <SeparatorView.h>
#include <Slider.h>
#include <SoundPlayer.h>
#include <StringItem.h>
#include <StringView.h>
#include <TextControl.h>
#include <TextView.h>
#include <Window.h>

#include <AudioStreamControl.h>

#include "ApplicationVolumesView.h"
#include "MixerControl.h"
#include "OutputMenuField.h"


#undef B_TRANSLATION_CONTEXT
#define B_TRANSLATION_CONTEXT "AudioSettings"


enum {
	kMsgPageSelected		= 'auPg',
	kMsgAdvanced			= 'auAd',
	kMsgSearch				= 'auSr',
	kMsgRefresh				= 'auRf',
	kMsgDevicesChanged		= 'auDv',
	kMsgDevicePolicy		= 'auDp',
	kMsgDevicePolicyDefaults = 'auDd',
	kMsgDevicesSettled		= 'auDs',

	kMsgVolume				= 'auVo',
	kMsgMute				= 'auMu',
	kMsgTestSound			= 'auTs',
	kMsgTestSoundDone		= 'auTd',
	kMsgOutputBuffer		= 'auOB',
	kMsgResampling			= 'auRq',

	kMsgInputDevice			= 'auIn',
	kMsgGain				= 'auGn',
	kMsgLevel				= 'auLv',
	kMsgRecordLoopback		= 'auLr',

	kMsgDeviceSelected		= 'auDS',
	kMsgDeviceBuffer		= 'auDB',
	kMsgMoreSettings		= 'auMS',
	kMsgRestart				= 'auRs',
	kMsgRestartDone			= 'auRd',
	kMsgDetailsChanged		= 'auDC'
};

// The recording gain parameter of multi_audio inputs (MediaViews.cpp).
static const int32 kRecordingGainParameter = 0x52474e01;

// A control the user just moved is not overwritten by the refresh for a
// moment, so that it does not jump back while the media services follow.
static const bigtime_t kUserChangeGrace = 1500000;

// Hot-plug: add-ons report devices before media_addon_server made their
// nodes and the defaults followed (MediaWindow does the same).
static const bigtime_t kDevicesSettleDelay = 1200000;

// Not 1000 Hz: the harness's virtio_sound phases use that tone.
static const float kTestSoundHz = 1250.0f;
static const bigtime_t kTestSoundLength = 1200000;


// #pragma mark - helpers


static BString
lower(const char* text)
{
	BString result(text);
	result.ToLower();
	return result;
}


/*!	The recording gain parameter of the default audio input, or NULL. The
	caller deletes \a _web.
*/
static BParameter*
recording_gain_parameter(BParameterWeb** _web)
{
	*_web = NULL;
	BMediaRoster* roster = BMediaRoster::CurrentRoster();
	media_node input;
	if (roster == NULL || roster->GetAudioInput(&input) != B_OK)
		return NULL;
	BParameterWeb* web = NULL;
	status_t status = roster->GetParameterWebFor(input, &web);
	roster->ReleaseNode(input);
	if (status != B_OK)
		return NULL;
	*_web = web;
	for (int32 i = 0; i < web->CountParameters(); i++) {
		if (web->ParameterAt(i)->ID() == kRecordingGainParameter)
			return web->ParameterAt(i);
	}
	return NULL;
}


// The mixer's "Resampling algorithm" (AudioMixer.cpp: PARAM_ETC(70)).
static const int32 kResamplingParameter = (70 << 16) | 0x4;


/*!	Reads (or, with \a set, sets) the mixer's resampling choice; fills
	\a field's menu with its choices when given.
*/
static status_t
mixer_resampling(int32* value, bool set, BMenuField* field, BHandler* target)
{
	BMediaRoster* roster = BMediaRoster::CurrentRoster();
	media_node mixer;
	if (roster == NULL || roster->GetAudioMixer(&mixer) != B_OK)
		return B_NO_INIT;
	BParameterWeb* web = NULL;
	status_t status = roster->GetParameterWebFor(mixer, &web);
	roster->ReleaseNode(mixer);
	if (status != B_OK)
		return status;
	status = B_ENTRY_NOT_FOUND;
	for (int32 i = 0; i < web->CountParameters(); i++) {
		BParameter* parameter = web->ParameterAt(i);
		if (parameter->ID() != kResamplingParameter
			|| parameter->Type() != BParameter::B_DISCRETE_PARAMETER) {
			continue;
		}
		BDiscreteParameter* discrete = (BDiscreteParameter*)parameter;
		if (set) {
			status = discrete->SetValue(value, sizeof(int32), system_time());
			break;
		}
		size_t size = sizeof(int32);
		bigtime_t when;
		status = discrete->GetValue(value, &size, &when);
		if (field != NULL && field->Menu()->CountItems() == 0) {
			for (int32 j = 0; j < discrete->CountItems(); j++) {
				BMessage* message = new BMessage(kMsgResampling);
				message->AddInt32("value", discrete->ItemValueAt(j));
				BMenuItem* item = new BMenuItem(discrete->ItemNameAt(j),
					message);
				item->SetTarget(target);
				field->Menu()->AddItem(item);
			}
		}
		break;
	}
	delete web;
	return status;
}


//!	Fills \a field's menu with the device's buffer size choices.
static void
fill_buffer_menu(BMenuField* field, const AudioDevice* device, uint32 what,
	BHandler* target, BString& key)
{
	BString newKey;
	if (device != NULL && device->hasBufferSize) {
		newKey << device->name << ":";
		for (int32 i = 0; i < device->bufferNames.CountStrings(); i++)
			newKey << device->bufferNames.StringAt(i) << ";";
	}
	BMenu* menu = field->Menu();
	if (newKey != key) {
		key = newKey;
		while (BMenuItem* item = menu->RemoveItem((int32)0))
			delete item;
		for (int32 i = 0; device != NULL
				&& i < device->bufferNames.CountStrings(); i++) {
			BMessage* message = new BMessage(what);
			message->AddString("device", device->name);
			message->AddInt32("frames",
				(int32)(addr_t)device->bufferValues.ItemAt(i));
			BMenuItem* item = new BMenuItem(
				device->bufferNames.StringAt(i), message);
			item->SetTarget(target);
			menu->AddItem(item);
		}
	}
	field->SetEnabled(device != NULL && device->hasBufferSize);
	for (int32 i = 0; device != NULL && i < menu->CountItems(); i++) {
		BMenuItem* item = menu->ItemAt(i);
		int32 frames = item->Message()->GetInt32("frames", -1);
		if (frames == device->bufferFrames && !item->IsMarked())
			item->SetMarked(true);
	}
}


static BString
device_description(const AudioDevice* device)
{
	BString roles;
	if (device->output && device->input)
		roles = B_TRANSLATE("Plays and records");
	else if (device->output)
		roles = B_TRANSLATE("Plays");
	else
		roles = B_TRANSLATE("Records");

	BString text;
	text << device->name << "\n" << roles;
	if (device->defaultOutput && device->defaultInput)
		text << B_TRANSLATE("; the default output and input");
	else if (device->defaultOutput)
		text << B_TRANSLATE("; the default output");
	else if (device->defaultInput)
		text << B_TRANSLATE("; the default input");
	text << "\n";
	BString rate = device->outputRate.Length() > 0 ? device->outputRate
		: device->inputRate;
	if (rate.Length() > 0) {
		text << B_TRANSLATE("Frame rate:") << " " << rate << "\n";
	}
	if (device->hasBufferSize) {
		text << B_TRANSLATE("Buffer size:") << " ";
		for (int32 i = 0; i < device->bufferNames.CountStrings(); i++) {
			if ((int32)(addr_t)device->bufferValues.ItemAt(i)
					== device->bufferFrames) {
				text << device->bufferNames.StringAt(i);
			}
		}
		text << "\n";
	}
	if (!device->running)
		text << B_TRANSLATE("Not running (the media services start it when "
			"it is used)") << "\n";
	return text;
}


//!	Tells the AudioSettingsView holding \a page to read the cards again.
static void
notify_details_changed(BView* page)
{
	for (BView* view = page->Parent(); view != NULL; view = view->Parent()) {
		if (dynamic_cast<AudioSettingsView*>(view) != NULL) {
			BMessenger(view).SendMessage(kMsgDetailsChanged);
			return;
		}
	}
}


// #pragma mark - LevelMeter


/*!	The input level: the peak of the last buffers, as a bar and in dBFS.
	A control so that scripting can read its value (peak x 1000).
*/
class LevelMeter : public BControl {
public:
	LevelMeter()
		:
		BControl("level", NULL, NULL, B_WILL_DRAW),
		fPeak(0)
	{
		SetExplicitMinSize(BSize(200, be_plain_font->Size() * 1.6f));
		SetExplicitMaxSize(BSize(B_SIZE_UNLIMITED,
			be_plain_font->Size() * 1.6f));
	}

	void SetPeak(float peak)
	{
		// fall back slowly, rise at once, like a meter
		fPeak = peak > fPeak ? peak : fPeak * 0.7f + peak * 0.3f;
		SetValueNoUpdate((int32)(fPeak * 1000 + 0.5f));
		Invalidate();
	}

	virtual void Draw(BRect updateRect)
	{
		BRect bounds = Bounds();
		rgb_color base = ui_color(B_PANEL_BACKGROUND_COLOR);
		SetHighColor(tint_color(base, B_DARKEN_2_TINT));
		StrokeRect(bounds);
		bounds.InsetBy(1, 1);
		SetHighColor(tint_color(base, B_DARKEN_1_TINT));
		FillRect(bounds);

		// scale: -60 dBFS .. 0 dBFS
		float db = fPeak > 0.000001f ? 20 * log10f(fPeak) : -120;
		float fraction = (db + 60) / 60;
		if (fraction < 0)
			fraction = 0;
		if (fraction > 1)
			fraction = 1;
		BRect bar = bounds;
		bar.right = bar.left + floorf(bounds.Width() * fraction);
		rgb_color color = db > -3 ? make_color(220, 60, 50)
			: db > -12 ? make_color(230, 190, 40) : make_color(90, 180, 80);
		if (fraction > 0) {
			SetHighColor(color);
			FillRect(bar);
		}

		char text[32];
		if (db <= -60)
			strlcpy(text, B_TRANSLATE("Silence"), sizeof(text));
		else
			snprintf(text, sizeof(text), "%.0f dBFS", db);
		SetHighUIColor(B_PANEL_TEXT_COLOR);
		font_height height;
		GetFontHeight(&height);
		DrawString(text, BPoint(bounds.right - StringWidth(text) - 4,
			bounds.top + (bounds.Height() + height.ascent - height.descent)
				/ 2));
	}

private:
	float fPeak;
};


// #pragma mark - test sound


struct TestSound {
	BSoundPlayer*	player;
	double			phase;
	int64			frame;
	int64			frames;
	float			rate;
};


static void
test_sound_buffer(void* cookie, void* buffer, size_t size,
	const media_raw_audio_format& format)
{
	TestSound* sound = (TestSound*)cookie;
	int32 channels = format.channel_count;
	size_t frames = size / (sizeof(float) * channels);
	float* samples = (float*)buffer;
	int64 fade = (int64)(sound->rate * 0.02);
	for (size_t i = 0; i < frames; i++, sound->frame++) {
		float value = 0;
		if (sound->frame < sound->frames) {
			float envelope = 1.0f;
			if (sound->frame < fade)
				envelope = (float)sound->frame / fade;
			else if (sound->frames - sound->frame < fade)
				envelope = (float)(sound->frames - sound->frame) / fade;
			value = 0.25f * envelope * sinf(sound->phase);
			sound->phase += 2 * M_PI * kTestSoundHz / sound->rate;
			if (sound->phase > 2 * M_PI)
				sound->phase -= 2 * M_PI;
		}
		for (int32 c = 0; c < channels; c++)
			samples[i * channels + c] = value;
	}
}


static TestSound* sTestSound = NULL;


// #pragma mark - AudioPage


AudioPage::AudioPage(const char* name, const char* title)
	:
	BGroupView(name, B_VERTICAL, B_USE_DEFAULT_SPACING),
	fTitle(title)
{
}


void
AudioPage::AddKeywords(const char* words, bool advanced)
{
	BString list(lower(words));
	BStringList split;
	list.Split(",", true, split);
	for (int32 i = 0; i < split.CountStrings(); i++) {
		BString word = split.StringAt(i).Trim();
		(advanced ? fAdvancedKeywords : fKeywords).Add(word);
	}
}


bool
AudioPage::Matches(const BString& text, bool* _advanced) const
{
	*_advanced = false;
	if (lower(fTitle).FindFirst(text) >= 0)
		return true;
	for (int32 i = 0; i < fKeywords.CountStrings(); i++) {
		if (fKeywords.StringAt(i).FindFirst(text) >= 0)
			return true;
	}
	for (int32 i = 0; i < fAdvancedKeywords.CountStrings(); i++) {
		if (fAdvancedKeywords.StringAt(i).FindFirst(text) >= 0) {
			*_advanced = true;
			return true;
		}
	}
	return false;
}


// #pragma mark - OutputPage


OutputPage::OutputPage(AudioDeviceList& devices)
	:
	AudioPage("output", B_TRANSLATE("Output")),
	fDevices(devices),
	fMixer(NULL),
	fVolumeChanged(0)
{
	AddKeywords(B_TRANSLATE_COMMENT("output, speakers, headphones, volume, "
		"master volume, mute, loudness, play, hdmi, displayport, monitor, "
		"test sound, default output, sound card",
		"Search words for the Output page, comma separated"), false);
	AddKeywords(B_TRANSLATE_COMMENT("buffer size, period, latency, "
		"resampling, sample rate conversion, quality",
		"Search words for the Output page's advanced settings"), true);

	fDevice = new OutputMenuField("outputDevice", B_TRANSLATE("Play sound on:"));
	fDeviceInfo = new BStringView("outputInfo", "");
	fDeviceInfo->SetHighUIColor(B_PANEL_TEXT_COLOR, B_LIGHTEN_1_TINT);

	fVolume = new BSlider("masterVolume", B_TRANSLATE("Volume:"),
		new BMessage(kMsgVolume), -600, 180, B_HORIZONTAL);
	fVolume->SetModificationMessage(new BMessage(kMsgVolume));
	fVolume->SetLimitLabels(B_TRANSLATE("Quiet"), B_TRANSLATE("Loud"));
	fVolume->SetToolTip(B_TRANSLATE("The volume of everything that plays"));
	fMute = new BCheckBox("masterMute", B_TRANSLATE("Mute"),
		new BMessage(kMsgMute));
	fTestSound = new BButton("testSound", B_TRANSLATE("Play a test sound"),
		new BMessage(kMsgTestSound));

	fBufferSize = new BMenuField("outputBufferSize", B_TRANSLATE("Buffer size:"),
		new BPopUpMenu(B_TRANSLATE("Automatic")));
	fBufferSize->SetToolTip(B_TRANSLATE("Shorter buffers lower the delay "
		"before sound is heard, but need more processor time. Automatic "
		"uses short buffers only while a low-latency application plays."));
	fResampling = new BMenuField("resampling", B_TRANSLATE("Resampling:"),
		new BPopUpMenu(B_TRANSLATE("None")));
	fResampling->SetToolTip(B_TRANSLATE("How the mixer converts sound "
		"recorded at another frame rate (44.1 kHz music on a 48 kHz card)"));
	fBufferNote = new BStringView("bufferNote",
		B_TRANSLATE("Applications set to \"Low latency\" get short buffers "
			"automatically."));
	fBufferNote->SetHighUIColor(B_PANEL_TEXT_COLOR, B_LIGHTEN_1_TINT);

	BBox* deviceBox = new BBox("deviceBox");
	deviceBox->SetLabel(B_TRANSLATE("Output device"));
	BGroupView* deviceGroup = new BGroupView(B_VERTICAL,
		B_USE_SMALL_SPACING);
	BLayoutBuilder::Group<>(deviceGroup)
		.SetInsets(B_USE_DEFAULT_SPACING, B_USE_SMALL_SPACING,
			B_USE_DEFAULT_SPACING, B_USE_DEFAULT_SPACING)
		.Add(fDevice)
		.Add(fDeviceInfo);
	deviceBox->AddChild(deviceGroup);

	BBox* volumeBox = new BBox("volumeBox");
	volumeBox->SetLabel(B_TRANSLATE("Volume"));
	BGroupView* volumeGroup = new BGroupView(B_VERTICAL, B_USE_SMALL_SPACING);
	BLayoutBuilder::Group<>(volumeGroup)
		.SetInsets(B_USE_DEFAULT_SPACING, B_USE_SMALL_SPACING,
			B_USE_DEFAULT_SPACING, B_USE_DEFAULT_SPACING)
		.Add(fVolume)
		.AddGroup(B_HORIZONTAL)
			.Add(fMute)
			.AddGlue()
			.Add(fTestSound)
		.End();
	volumeBox->AddChild(volumeGroup);

	BBox* advancedBox = new BBox("outputAdvanced");
	advancedBox->SetLabel(B_TRANSLATE("Advanced"));
	BGroupView* advancedGroup = new BGroupView(B_VERTICAL,
		B_USE_SMALL_SPACING);
	BLayoutBuilder::Group<>(advancedGroup)
		.SetInsets(B_USE_DEFAULT_SPACING, B_USE_SMALL_SPACING,
			B_USE_DEFAULT_SPACING, B_USE_DEFAULT_SPACING)
		.Add(fBufferSize)
		.Add(fBufferNote)
		.Add(fResampling);
	advancedBox->AddChild(advancedGroup);
	fAdvancedBox = advancedBox;

	BLayoutBuilder::Group<>(this)
		.Add(deviceBox)
		.Add(volumeBox)
		.Add(advancedBox)
		.AddGlue();
	fAdvancedBox->Hide();
}


OutputPage::~OutputPage()
{
	delete fMixer;
}


void
OutputPage::AttachedToWindow()
{
	AudioPage::AttachedToWindow();
	fVolume->SetTarget(this);
	fMute->SetTarget(this);
	fTestSound->SetTarget(this);
	_ConnectVolume();
	_UpdateResampling();
}


void
OutputPage::_UpdateResampling()
{
	int32 value = -1;
	bool available = mixer_resampling(&value, false, fResampling, this)
		== B_OK;
	fResampling->SetEnabled(available);
	for (int32 i = 0; i < fResampling->Menu()->CountItems(); i++) {
		BMenuItem* item = fResampling->Menu()->ItemAt(i);
		if (item->Message()->GetInt32("value", -2) == value
			&& !item->IsMarked()) {
			item->SetMarked(true);
		}
	}
}


void
OutputPage::_ConnectVolume()
{
	delete fMixer;
	fMixer = new MixerControl();
	float volume = 0;
	bool connected = fMixer->Connect(&volume);
	fVolume->SetEnabled(connected);
	fMute->SetEnabled(connected);
	if (connected) {
		fVolume->SetLimits((int32)(fMixer->Minimum() * 10),
			(int32)(fMixer->Maximum() * 10));
		fVolume->SetValue((int32)floorf(volume * 10 + 0.5f));
		fMute->SetValue(fMixer->IsMuted() ? B_CONTROL_ON : B_CONTROL_OFF);
	}
}


void
OutputPage::MessageReceived(BMessage* message)
{
	switch (message->what) {
		// The value the control had when it sent the message ("be:value"):
		// a refresh from the device queued before this message may have
		// moved the control back in between.
		case kMsgVolume:
			fVolumeChanged = system_time();
			if (fMixer != NULL && fMixer->IsConnected()) {
				fMixer->SetVolume(message->GetInt32("be:value",
					fVolume->Value()) / 10.0f);
			}
			break;

		case kMsgMute:
			fVolumeChanged = system_time();
			if (fMixer != NULL && fMixer->IsConnected()) {
				fMixer->SetMuted(message->GetInt32("be:value",
					fMute->Value()) == B_CONTROL_ON);
			}
			break;

		case kMsgTestSound:
			_PlayTestSound();
			break;

		case kMsgTestSoundDone:
			if (sTestSound != NULL) {
				sTestSound->player->Stop();
				delete sTestSound->player;
				delete sTestSound;
				sTestSound = NULL;
			}
			fTestSound->SetEnabled(true);
			break;

		case kMsgResampling:
		{
			int32 value = message->GetInt32("value", 3);
			mixer_resampling(&value, true, NULL, NULL);
			break;
		}

		case kMsgOutputBuffer:
		{
			const char* device;
			int32 frames;
			if (message->FindString("device", &device) == B_OK
				&& message->FindInt32("frames", &frames) == B_OK) {
				fDevices.SetBufferFrames(device, frames);
				notify_details_changed(this);
			}
			break;
		}

		default:
			AudioPage::MessageReceived(message);
			break;
	}
}


void
OutputPage::_PlayTestSound()
{
	if (sTestSound != NULL)
		return;
	media_raw_audio_format format = media_raw_audio_format::wildcard;
	format.format = media_raw_audio_format::B_AUDIO_FLOAT;
	format.channel_count = 2;
	format.frame_rate = 48000;
	format.byte_order = B_MEDIA_HOST_ENDIAN;

	TestSound* sound = new TestSound;
	sound->phase = 0;
	sound->frame = 0;
	sound->rate = format.frame_rate;
	sound->frames = (int64)(format.frame_rate * kTestSoundLength / 1000000);
	sound->player = new BSoundPlayer(&format, "Test sound", test_sound_buffer,
		NULL, sound);
	if (sound->player->InitCheck() != B_OK) {
		delete sound->player;
		delete sound;
		return;
	}
	sTestSound = sound;
	sound->player->Start();
	sound->player->SetHasData(true);
	fTestSound->SetEnabled(false);

	BMessage done(kMsgTestSoundDone);
	BMessageRunner::StartSending(BMessenger(this), &done,
		kTestSoundLength + 400000, 1);
}


void
OutputPage::SetAdvanced(bool advanced)
{
	if (advanced && fAdvancedBox->IsHidden(fAdvancedBox))
		fAdvancedBox->Show();
	else if (!advanced && !fAdvancedBox->IsHidden(fAdvancedBox))
		fAdvancedBox->Hide();
}


void
OutputPage::Refresh()
{
	fDevice->Refresh(fDevices);

	const AudioDevice* device = fDevices.FindDevice(
		fDevices.DefaultOutputName());
	BString info;
	if (device != NULL) {
		BString rate = device->outputRate.Length() > 0 ? device->outputRate
			: device->inputRate;
		if (rate.Length() > 0)
			info << rate;
	} else if (fDevices.OutputNames().IsEmpty())
		info = B_TRANSLATE("No sound card found.");
	fDeviceInfo->SetText(info);

	fill_buffer_menu(fBufferSize, device, kMsgOutputBuffer, this, fBufferKey);

	if (fMixer == NULL || !fMixer->IsConnected()) {
		_ConnectVolume();
		return;
	}
	if (system_time() - fVolumeChanged < kUserChangeGrace)
		return;
	int32 value = (int32)floorf(fMixer->Volume() * 10 + 0.5f);
	if (value != fVolume->Value())
		fVolume->SetValue(value);
	int32 muted = fMixer->IsMuted() ? B_CONTROL_ON : B_CONTROL_OFF;
	if (muted != fMute->Value())
		fMute->SetValue(muted);
}


// #pragma mark - InputPage


InputPage::InputPage(AudioDeviceList& devices)
	:
	AudioPage("input", B_TRANSLATE("Input")),
	fDevices(devices),
	fGainChanged(0),
	fRecorder(NULL)
{
	AddKeywords(B_TRANSLATE_COMMENT("input, microphone, mic, record, "
		"recording, gain, recording gain, level, line in, default input",
		"Search words for the Input page, comma separated"), false);
	AddKeywords(B_TRANSLATE_COMMENT("loopback, what you hear, record "
		"applications, record an application, capture",
		"Search words for the Input page's advanced settings"), true);

	fDevice = new BMenuField("inputDevice", B_TRANSLATE("Record from:"),
		new BPopUpMenu(B_TRANSLATE("None")));
	fGain = new BSlider("recordingGain", B_TRANSLATE("Recording gain:"),
		new BMessage(kMsgGain), -400, 200, B_HORIZONTAL);
	fGain->SetModificationMessage(new BMessage(kMsgGain));
	fGain->SetLimitLabels(B_TRANSLATE("-40 dB"), B_TRANSLATE("+20 dB"));
	fGain->SetHashMarks(B_HASH_MARKS_BOTTOM);
	fGain->SetHashMarkCount(7);
	fGain->SetToolTip(B_TRANSLATE("How loud the input records"));
	fMeter = new LevelMeter();
	fMeter->SetToolTip(B_TRANSLATE("The level the input records at now"));

	BBox* deviceBox = new BBox("inputBox");
	deviceBox->SetLabel(B_TRANSLATE("Input device"));
	BGroupView* group = new BGroupView(B_VERTICAL, B_USE_SMALL_SPACING);
	BLayoutBuilder::Group<>(group)
		.SetInsets(B_USE_DEFAULT_SPACING, B_USE_SMALL_SPACING,
			B_USE_DEFAULT_SPACING, B_USE_DEFAULT_SPACING)
		.Add(fDevice)
		.Add(fGain)
		.AddGroup(B_HORIZONTAL)
			.Add(new BStringView("levelLabel", B_TRANSLATE("Input level:")))
			.Add(fMeter)
		.End();
	deviceBox->AddChild(group);

	fLoopbackSource = new BMenuField("loopbackSource",
		B_TRANSLATE("Record:"), new BPopUpMenu(B_TRANSLATE("Everything")));
	BStringView* loopbackText = new BStringView("loopbackText",
		B_TRANSLATE("Recording programs can record what you hear: "
			"everything, or one application."));
	loopbackText->SetHighUIColor(B_PANEL_TEXT_COLOR, B_LIGHTEN_1_TINT);
	BButton* record = new BButton("recordLoopback",
		B_TRANSLATE("Record in SoundRecorder" B_UTF8_ELLIPSIS),
		new BMessage(kMsgRecordLoopback));
	record->SetTarget(this);

	BBox* loopbackBox = new BBox("loopbackBox");
	loopbackBox->SetLabel(B_TRANSLATE("What you hear (loopback)"));
	group = new BGroupView(B_VERTICAL, B_USE_SMALL_SPACING);
	BLayoutBuilder::Group<>(group)
		.SetInsets(B_USE_DEFAULT_SPACING, B_USE_SMALL_SPACING,
			B_USE_DEFAULT_SPACING, B_USE_DEFAULT_SPACING)
		.Add(loopbackText)
		.AddGroup(B_HORIZONTAL)
			.Add(fLoopbackSource)
			.Add(record)
		.End();
	loopbackBox->AddChild(group);
	fLoopbackBox = loopbackBox;

	BLayoutBuilder::Group<>(this)
		.Add(deviceBox)
		.Add(loopbackBox)
		.AddGlue();
	fLoopbackBox->Hide();
}


InputPage::~InputPage()
{
	_StopMeter();
}


void
InputPage::AttachedToWindow()
{
	AudioPage::AttachedToWindow();
	fGain->SetTarget(this);
	_UpdateGain();
}


void
InputPage::MessageReceived(BMessage* message)
{
	switch (message->what) {
		case kMsgInputDevice:
		{
			const char* name;
			if (message->FindString("device", &name) == B_OK) {
				fDevices.SetDefaultInput(name);
				_StopMeter();
				fDeviceKey = "";
			}
			break;
		}

		case kMsgGain:
		{
			fGainChanged = system_time();
			BParameterWeb* web;
			BParameter* parameter = recording_gain_parameter(&web);
			if (parameter != NULL) {
				// as sent (see kMsgVolume)
				float gain = message->GetInt32("be:value", fGain->Value())
					/ 10.0f;
				parameter->SetValue(&gain, sizeof(gain), system_time());
			}
			delete web;
			break;
		}

		case kMsgLevel:
			fMeter->SetPeak(message->GetFloat("peak", 0));
			break;

		case kMsgRecordLoopback:
		{
			BMenuItem* item = fLoopbackSource->Menu()->FindMarked();
			BString label = RENGA_LOOPBACK_FLAVOR_NAME;
			if (item != NULL && item->Message() != NULL
				&& item->Message()->HasString("name")) {
				label = B_TRANSLATE("What you hear: %application%");
				label.ReplaceFirst("%application%",
					item->Message()->GetString("name", ""));
			}
			const char* args[] = { "--input", label.String(), NULL };
			be_roster->Launch("application/x-vnd.Haiku-SoundRecorder", 2,
				args);
			break;
		}

		default:
			AudioPage::MessageReceived(message);
			break;
	}
}


void
InputPage::SetAdvanced(bool advanced)
{
	if (advanced && fLoopbackBox->IsHidden(fLoopbackBox))
		fLoopbackBox->Show();
	else if (!advanced && !fLoopbackBox->IsHidden(fLoopbackBox))
		fLoopbackBox->Hide();
}


void
InputPage::_UpdateGain()
{
	if (system_time() - fGainChanged < kUserChangeGrace)
		return;
	BParameterWeb* web;
	BParameter* parameter = recording_gain_parameter(&web);
	float gain = 0;
	bool available = false;
	if (parameter != NULL) {
		size_t size = sizeof(gain);
		bigtime_t when;
		available = parameter->GetValue(&gain, &size, &when) == B_OK;
	}
	delete web;
	fGain->SetEnabled(available);
	int32 value = (int32)(gain * 10 + (gain < 0 ? -0.5 : 0.5));
	if (value != fGain->Value())
		fGain->SetValue(value);
}


void
InputPage::Refresh()
{
	BStringList names = fDevices.InputNames();
	BString current = fDevices.DefaultInputName();
	BString key;
	for (int32 i = 0; i < names.CountStrings(); i++)
		key << names.StringAt(i) << ";";
	key << "|" << current;
	if (key != fDeviceKey) {
		fDeviceKey = key;
		BMenu* menu = fDevice->Menu();
		while (BMenuItem* item = menu->RemoveItem((int32)0))
			delete item;
		for (int32 i = 0; i < names.CountStrings(); i++) {
			BMessage* message = new BMessage(kMsgInputDevice);
			message->AddString("device", names.StringAt(i));
			BMenuItem* item = new BMenuItem(names.StringAt(i), message);
			item->SetTarget(this);
			menu->AddItem(item);
			if (names.StringAt(i) == current)
				item->SetMarked(true);
		}
		fDevice->SetEnabled(!names.IsEmpty());
	}
	_UpdateGain();

	// the meter follows the default input
	if (fRecorder != NULL && fMeterInput != current)
		_StopMeter();
	if (fRecorder == NULL && !IsHidden(this))
		_StartMeter();

	// the loopback's sources: everything, or one application
	BPrivate::AudioStreamControl control;
	BStringList apps;
	BStringList signatures;
	if (control.Update() == B_OK) {
		for (int32 i = 0; i < control.CountStreams(); i++) {
			const BPrivate::AudioStream* stream = control.StreamAt(i);
			if (stream->signature.Length() == 0
				|| signatures.HasString(stream->signature)) {
				continue;
			}
			signatures.Add(stream->signature);
			apps.Add(stream->name);
		}
	}
	BString loopbackKey;
	for (int32 i = 0; i < apps.CountStrings(); i++)
		loopbackKey << apps.StringAt(i) << ";";
	if (loopbackKey != fLoopbackKey || fLoopbackSource->Menu()->CountItems() == 0) {
		fLoopbackKey = loopbackKey;
		BMenu* menu = fLoopbackSource->Menu();
		BString marked;
		if (BMenuItem* item = menu->FindMarked())
			marked = item->Label();
		while (BMenuItem* item = menu->RemoveItem((int32)0))
			delete item;
		menu->AddItem(new BMenuItem(B_TRANSLATE("Everything"),
			new BMessage('none')));
		for (int32 i = 0; i < apps.CountStrings(); i++) {
			BMessage* message = new BMessage('none');
			message->AddString("name", apps.StringAt(i));
			message->AddString("signature", signatures.StringAt(i));
			menu->AddItem(new BMenuItem(apps.StringAt(i), message));
		}
		BMenuItem* item = marked.Length() > 0 ? menu->FindItem(marked) : NULL;
		(item != NULL ? item : menu->ItemAt(0))->SetMarked(true);
		dormant_node_info info;
		fLoopbackSource->SetEnabled(
			AudioDeviceList::LoopbackInfo(&info) == B_OK);
	}
}


void
InputPage::PageShown(bool shown)
{
	if (shown)
		_StartMeter();
	else
		_StopMeter();
}


void
InputPage::_StartMeter()
{
	if (fRecorder != NULL)
		return;
	BMediaRoster* roster = BMediaRoster::CurrentRoster();
	media_node input;
	if (roster == NULL || roster->GetAudioInput(&input) != B_OK)
		return;

	media_output output;
	int32 count = 0;
	if (roster->GetFreeOutputsFor(input, &output, 1, &count,
			B_MEDIA_RAW_AUDIO) != B_OK || count < 1) {
		// busy (another program records from it): no meter for now
		roster->ReleaseNode(input);
		return;
	}

	BMediaRecorder* recorder = new BMediaRecorder("Audio settings level",
		B_MEDIA_RAW_AUDIO);
	media_format format;
	format.type = B_MEDIA_RAW_AUDIO;
	format.u.raw_audio = media_raw_audio_format::wildcard;
	if (recorder->InitCheck() != B_OK) {
		delete recorder;
		roster->ReleaseNode(input);
		return;
	}
	recorder->SetAcceptedFormat(format);
	format.u.raw_audio = output.format.u.raw_audio;
	recorder->SetHooks(_RecordHook, NULL, this);
	if (recorder->Connect(input, &output, &format) != B_OK
		|| recorder->Start() != B_OK) {
		recorder->SetHooks(NULL, NULL, NULL);
		delete recorder;
		roster->ReleaseNode(input);
		return;
	}
	roster->ReleaseNode(input);
	fRecorder = recorder;
	fMeterInput = fDevices.DefaultInputName();
}


void
InputPage::_StopMeter()
{
	if (fRecorder == NULL)
		return;
	fRecorder->Stop(true);
	fRecorder->Disconnect();
	fRecorder->SetHooks(NULL, NULL, NULL);
	delete fRecorder;
	fRecorder = NULL;
	fMeter->SetPeak(0);
	fMeter->SetPeak(0);
}


/*static*/ void
InputPage::_RecordHook(void* cookie, bigtime_t timestamp, void* data,
	size_t size, const media_format& format)
{
	InputPage* page = (InputPage*)cookie;
	float peak = 0;
	switch (format.u.raw_audio.format) {
		case media_raw_audio_format::B_AUDIO_FLOAT:
		{
			const float* samples = (const float*)data;
			for (size_t i = 0; i < size / sizeof(float); i++)
				peak = max_c(peak, fabsf(samples[i]));
			break;
		}
		case media_raw_audio_format::B_AUDIO_SHORT:
		{
			const int16* samples = (const int16*)data;
			for (size_t i = 0; i < size / sizeof(int16); i++)
				peak = max_c(peak, fabsf(samples[i] / 32768.0f));
			break;
		}
		case media_raw_audio_format::B_AUDIO_INT:
		{
			const int32* samples = (const int32*)data;
			for (size_t i = 0; i < size / sizeof(int32); i++)
				peak = max_c(peak, fabsf(samples[i] / 2147483648.0f));
			break;
		}
		default:
			return;
	}
	BMessage level(kMsgLevel);
	level.AddFloat("peak", peak);
	BMessenger(page).SendMessage(&level);
}


// #pragma mark - ApplicationsPage


ApplicationsPage::ApplicationsPage()
	:
	AudioPage("applications", B_TRANSLATE("Applications"))
{
	AddKeywords(B_TRANSLATE_COMMENT("applications, programs, apps, "
		"per-application volume, application volume, mute an application, "
		"streams", "Search words for the Applications page"), false);
	AddKeywords(B_TRANSLATE_COMMENT("low latency, latency, games, emulators, "
		"routing, route, output per application, play on another device",
		"Search words for the Applications page's advanced settings"), true);

	fApplications = new ApplicationVolumesView("applications", false);
	fApplications->SetShowLowLatency(false);
	fAdvancedNote = new BStringView("applicationsNote",
		B_TRANSLATE("Advanced: where each application plays, and \"Low "
			"latency\" for games and emulators."));
	fAdvancedNote->SetHighUIColor(B_PANEL_TEXT_COLOR, B_LIGHTEN_1_TINT);

	BBox* box = new BBox("applicationsBox");
	box->SetLabel(B_TRANSLATE("Applications playing sound"));
	BGroupView* group = new BGroupView(B_VERTICAL, B_USE_SMALL_SPACING);
	BLayoutBuilder::Group<>(group)
		.SetInsets(B_USE_DEFAULT_SPACING, B_USE_SMALL_SPACING,
			B_USE_DEFAULT_SPACING, B_USE_DEFAULT_SPACING)
		.Add(fApplications);
	box->AddChild(group);

	BLayoutBuilder::Group<>(this)
		.Add(box)
		.Add(fAdvancedNote)
		.AddGlue();
}


void
ApplicationsPage::SetAdvanced(bool advanced)
{
	fApplications->SetShowOutputs(advanced);
	fApplications->SetShowLowLatency(advanced);
	if (advanced && !fAdvancedNote->IsHidden(fAdvancedNote))
		fAdvancedNote->Hide();
	else if (!advanced && fAdvancedNote->IsHidden(fAdvancedNote))
		fAdvancedNote->Show();
}


void
ApplicationsPage::Refresh()
{
	// ApplicationVolumesView refreshes itself every second
}


// #pragma mark - DevicesPage


class DeviceItem : public BStringItem {
public:
	DeviceItem(const AudioDevice* device)
		:
		BStringItem(""),
		fName(device->name)
	{
		BString text(device->name);
		BString roles;
		if (device->defaultOutput)
			roles << B_TRANSLATE("default output");
		if (device->defaultInput) {
			if (roles.Length() > 0)
				roles << ", ";
			roles << B_TRANSLATE("default input");
		}
		if (roles.Length() > 0)
			text << "  (" << roles << ")";
		SetText(text);
	}

	const BString& Name() const { return fName; }

private:
	BString fName;
};


DevicesPage::DevicesPage(AudioDeviceList& devices)
	:
	AudioPage("devices", B_TRANSLATE("Devices")),
	fDevices(devices),
	fAdvanced(false)
{
	AddKeywords(B_TRANSLATE_COMMENT("devices, sound cards, usb, headset, "
		"hdmi, displayport, monitor, frame rate, sample rate, hot-plug, "
		"plug in", "Search words for the Devices page"), false);
	AddKeywords(B_TRANSLATE_COMMENT("when connected, new device, "
		"notification, switch automatically, switch back, disconnected, "
		"ask", "Search words for the Devices page's connection settings"),
		false);
	AddKeywords(B_TRANSLATE_COMMENT("buffer size, period, restart media "
		"services, media server, device settings",
		"Search words for the Devices page's advanced settings"), true);

	fList = new BListView("deviceList");
	fList->SetSelectionMessage(new BMessage(kMsgDeviceSelected));
	BScrollView* scroll = new BScrollView("deviceScroll", fList, 0, false,
		true);
	scroll->SetExplicitMinSize(BSize(B_SIZE_UNSET, 90));

	fDetails = new BTextView("deviceDetails");
	fDetails->MakeEditable(false);
	fDetails->MakeSelectable(false);
	fDetails->SetViewUIColor(B_PANEL_BACKGROUND_COLOR);
	fDetails->SetLowUIColor(B_PANEL_BACKGROUND_COLOR);
	float detailsHeight = ceilf(be_plain_font->Size() * 1.3f * 5);
	fDetails->SetExplicitMinSize(BSize(B_SIZE_UNSET, detailsHeight));
	fDetails->SetExplicitMaxSize(BSize(B_SIZE_UNLIMITED, detailsHeight));

	fBufferSize = new BMenuField("deviceBufferSize",
		B_TRANSLATE("Buffer size:"), new BPopUpMenu(B_TRANSLATE("None")));
	fMoreSettings = new BButton("moreSettings",
		B_TRANSLATE("Device controls" B_UTF8_ELLIPSIS),
		new BMessage(kMsgMoreSettings));
	fMoreSettings->SetToolTip(B_TRANSLATE("The card's own controls in the "
		"Media preferences"));
	fRestart = new BButton("restartMedia",
		B_TRANSLATE("Restart media services"), new BMessage(kMsgRestart));

	// F-59: what happens when the device is connected
	const char* policyLabels[] = {
		B_TRANSLATE("Ask"),
		B_TRANSLATE("Switch to it automatically"),
		B_TRANSLATE("Do nothing")
	};
	BPopUpMenu* outputMenu = new BPopUpMenu("output policy");
	BPopUpMenu* inputMenu = new BPopUpMenu("input policy");
	for (int32 i = 0; i < 3; i++) {
		BMessage* message = new BMessage(kMsgDevicePolicy);
		message->AddInt32("output", i);
		outputMenu->AddItem(new BMenuItem(policyLabels[i], message));
		message = new BMessage(kMsgDevicePolicy);
		message->AddInt32("input", i);
		inputMenu->AddItem(new BMenuItem(policyLabels[i], message));
	}
	fOutputPolicy = new BMenuField("devicePolicyOutput",
		B_TRANSLATE("When connected, as output:"), outputMenu);
	fInputPolicy = new BMenuField("devicePolicyInput",
		B_TRANSLATE("When connected, as input:"), inputMenu);
	fSwitchBack = new BCheckBox("deviceSwitchBack",
		B_TRANSLATE("Switch back when it is disconnected"),
		new BMessage(kMsgDevicePolicy));
	fPolicyDefaults = new BButton("devicePolicyDefaults",
		B_TRANSLATE("Restore defaults"),
		new BMessage(kMsgDevicePolicyDefaults));
	fPolicyDefaults->SetToolTip(B_TRANSLATE("Every device: ask when it is "
		"connected, switch back when it is disconnected"));

	BBox* box = new BBox("devicesBox");
	box->SetLabel(B_TRANSLATE("Sound cards"));
	BGroupView* group = new BGroupView(B_VERTICAL, B_USE_SMALL_SPACING);
	BLayoutBuilder::Group<>(group)
		.SetInsets(B_USE_DEFAULT_SPACING, B_USE_SMALL_SPACING,
			B_USE_DEFAULT_SPACING, B_USE_DEFAULT_SPACING)
		.Add(scroll)
		.Add(fDetails)
		.AddGroup(B_HORIZONTAL)
			.AddGlue()
			.Add(fMoreSettings)
		.End();
	box->AddChild(group);

	BBox* advancedBox = new BBox("devicesAdvanced");
	advancedBox->SetLabel(B_TRANSLATE("Advanced"));
	group = new BGroupView(B_VERTICAL, B_USE_SMALL_SPACING);
	BLayoutBuilder::Group<>(group)
		.SetInsets(B_USE_DEFAULT_SPACING, B_USE_SMALL_SPACING,
			B_USE_DEFAULT_SPACING, B_USE_DEFAULT_SPACING)
		.Add(fBufferSize)
		.AddGroup(B_HORIZONTAL)
			.Add(fRestart)
			.AddGlue()
		.End();
	advancedBox->AddChild(group);
	fAdvancedBox = advancedBox;

	BBox* policyBox = new BBox("devicesPolicy");
	policyBox->SetLabel(B_TRANSLATE("When this device is connected"));
	group = new BGroupView(B_VERTICAL, B_USE_SMALL_SPACING);
	BLayoutBuilder::Group<>(group)
		.SetInsets(B_USE_DEFAULT_SPACING, B_USE_SMALL_SPACING,
			B_USE_DEFAULT_SPACING, B_USE_DEFAULT_SPACING)
		.AddGrid(B_USE_SMALL_SPACING, B_USE_SMALL_SPACING)
			.AddMenuField(fOutputPolicy, 0, 0)
			.AddMenuField(fInputPolicy, 0, 1)
		.End()
		.Add(fSwitchBack)
		.AddGroup(B_HORIZONTAL)
			.AddGlue()
			.Add(fPolicyDefaults)
		.End();
	policyBox->AddChild(group);

	BLayoutBuilder::Group<>(this)
		.Add(box)
		.Add(policyBox)
		.Add(advancedBox)
		.AddGlue();
	fAdvancedBox->Hide();
}


void
DevicesPage::AttachedToWindow()
{
	AudioPage::AttachedToWindow();
	fList->SetTarget(this);
	fMoreSettings->SetTarget(this);
	fRestart->SetTarget(this);
	fOutputPolicy->Menu()->SetTargetForItems(this);
	fInputPolicy->Menu()->SetTargetForItems(this);
	fSwitchBack->SetTarget(this);
	fPolicyDefaults->SetTarget(this);
}


void
DevicesPage::MessageReceived(BMessage* message)
{
	switch (message->what) {
		case kMsgDeviceSelected:
			_ShowDetails();
			break;

		case kMsgDevicePolicy:
			_ChangePolicy(message);
			break;

		case kMsgDevicePolicyDefaults:
		{
			AudioDevicePolicies policies;
			policies.Load();
			policies.Reset();
			policies.Save();
			_ShowDetails();
			break;
		}

		case kMsgDeviceBuffer:
		{
			const char* device;
			int32 frames;
			if (message->FindString("device", &device) == B_OK
				&& message->FindInt32("frames", &frames) == B_OK) {
				fDevices.SetBufferFrames(device, frames);
				notify_details_changed(this);
			}
			break;
		}

		case kMsgMoreSettings:
		{
			DeviceItem* item = dynamic_cast<DeviceItem*>(
				fList->ItemAt(fList->CurrentSelection()));
			if (item == NULL)
				break;
			const char* args[] = { "--device", item->Name().String(), NULL };
			be_roster->Launch("application/x-vnd.Haiku-Media", 2, args);
			break;
		}

		case kMsgRestart:
		{
			// As the Media preferences do it, off the window's thread; the
			// page follows through B_MEDIA_SERVER_STARTED.
			fRestart->SetEnabled(false);
			thread_id thread = spawn_thread(&_RestartMediaServices,
				"restart media services", B_NORMAL_PRIORITY,
				new BMessenger(this));
			resume_thread(thread);
			break;
		}

		case kMsgRestartDone:
			fRestart->SetEnabled(true);
			break;

		default:
			AudioPage::MessageReceived(message);
			break;
	}
}


/*static*/ status_t
DevicesPage::_RestartMediaServices(void* data)
{
	BMessenger* messenger = (BMessenger*)data;
	shutdown_media_server();
	launch_media_server();
	messenger->SendMessage(kMsgRestartDone);
	delete messenger;
	return B_OK;
}


void
DevicesPage::SetAdvanced(bool advanced)
{
	fAdvanced = advanced;
	if (advanced && fAdvancedBox->IsHidden(fAdvancedBox))
		fAdvancedBox->Show();
	else if (!advanced && !fAdvancedBox->IsHidden(fAdvancedBox))
		fAdvancedBox->Hide();
}


void
DevicesPage::Refresh()
{
	BString key = fDevices.Key();
	if (key != fListKey) {
		fListKey = key;
		BString selected;
		if (DeviceItem* item = dynamic_cast<DeviceItem*>(
				fList->ItemAt(fList->CurrentSelection()))) {
			selected = item->Name();
		}
		while (BListItem* item = fList->RemoveItem((int32)0))
			delete item;
		int32 select = 0;
		for (int32 i = 0; i < fDevices.CountDevices(); i++) {
			const AudioDevice* device = fDevices.DeviceAt(i);
			fList->AddItem(new DeviceItem(device));
			if (device->name == selected)
				select = i;
		}
		if (fList->CountItems() > 0)
			fList->Select(select);
	}
	_ShowDetails();
}


void
DevicesPage::_ShowDetails()
{
	DeviceItem* item = dynamic_cast<DeviceItem*>(
		fList->ItemAt(fList->CurrentSelection()));
	const AudioDevice* device = item != NULL
		? fDevices.FindDevice(item->Name()) : NULL;
	BString text = device != NULL ? device_description(device)
		: BString(B_TRANSLATE("No sound card found."));
	if (text != fDetails->Text())
		fDetails->SetText(text);
	fMoreSettings->SetEnabled(device != NULL);
	fill_buffer_menu(fBufferSize, device != NULL && device->output ? device
		: NULL, kMsgDeviceBuffer, this, fBufferKey);
	_ShowPolicy(device);
}


void
DevicesPage::SelectDevice(const char* name)
{
	for (int32 i = 0; i < fList->CountItems(); i++) {
		DeviceItem* item = dynamic_cast<DeviceItem*>(fList->ItemAt(i));
		if (item != NULL && item->Name() == name) {
			fList->Select(i);
			fList->ScrollToSelection();
			break;
		}
	}
	_ShowDetails();
}


//!	F-59: the selected device's "when connected" settings.
void
DevicesPage::_ShowPolicy(const AudioDevice* device)
{
	AudioDevicePolicies policies;
	policies.Load();
	AudioDevicePolicy policy;
	if (device != NULL)
		policy = policies.Get(device->name);
	if (BMenuItem* item = fOutputPolicy->Menu()->ItemAt(policy.output))
		item->SetMarked(true);
	if (BMenuItem* item = fInputPolicy->Menu()->ItemAt(policy.input))
		item->SetMarked(true);
	fSwitchBack->SetValue(policy.switchBack);
	fOutputPolicy->SetEnabled(device != NULL && device->output);
	fInputPolicy->SetEnabled(device != NULL && device->input);
	fSwitchBack->SetEnabled(device != NULL);
	fPolicyDefaults->SetEnabled(!policies.IsDefault());
}


void
DevicesPage::_ChangePolicy(BMessage* message)
{
	DeviceItem* item = dynamic_cast<DeviceItem*>(
		fList->ItemAt(fList->CurrentSelection()));
	if (item == NULL)
		return;
	AudioDevicePolicies policies;
	policies.Load();
	AudioDevicePolicy policy = policies.Get(item->Name());
	int32 value;
	if (message->FindInt32("output", &value) == B_OK)
		policy.output = value;
	else if (message->FindInt32("input", &value) == B_OK)
		policy.input = value;
	else
		policy.switchBack = fSwitchBack->Value() != 0;
	policies.Set(item->Name(), policy);
	policies.Save();
	_ShowPolicy(fDevices.FindDevice(item->Name()));
}


// #pragma mark - AudioSettingsView


class PageItem : public BStringItem {
public:
	PageItem(const char* title, int32 index)
		:
		BStringItem(title),
		fIndex(index)
	{
	}

	int32 Index() const { return fIndex; }

private:
	int32 fIndex;
};


AudioSettingsView::AudioSettingsView()
	:
	BGroupView("audioSettings", B_HORIZONTAL, B_USE_DEFAULT_SPACING),
	fPages(4),
	fRunner(NULL),
	fDevicesRunner(NULL),
	fCurrent(-1),
	fRefreshes(0)
{
	fDevices.Update();

	fOutputPage = new OutputPage(fDevices);
	fInputPage = new InputPage(fDevices);
	fDevicesPage = new DevicesPage(fDevices);
	fPages.AddItem(fOutputPage);
	fPages.AddItem(fInputPage);
	fPages.AddItem(new ApplicationsPage());
	fPages.AddItem(fDevicesPage);

	fSearch = new BTextControl("search", B_TRANSLATE("Search:"), "",
		new BMessage(kMsgSearch));
	fSearch->SetModificationMessage(new BMessage(kMsgSearch));
	fSearch->TextView()->SetToolTip(B_TRANSLATE("Search the audio settings"));
	fSidebar = new BListView("sidebar");
	fSidebar->SetSelectionMessage(new BMessage(kMsgPageSelected));
	BScrollView* sidebarScroll = new BScrollView("sidebarScroll", fSidebar,
		0, false, false);
	float width = be_plain_font->StringWidth("Applications") * 1.6f + 20;
	sidebarScroll->SetExplicitMinSize(BSize(width, B_SIZE_UNSET));
	sidebarScroll->SetExplicitMaxSize(BSize(width, B_SIZE_UNLIMITED));
	fSearch->SetExplicitMaxSize(BSize(width, B_SIZE_UNSET));

	fTitle = new BStringView("pageTitle", "");
	BFont font(be_bold_font);
	font.SetSize(font.Size() * 1.3f);
	fTitle->SetFont(&font);
	fSearchNote = new BStringView("searchNote", "");
	fSearchNote->SetHighUIColor(B_PANEL_TEXT_COLOR, B_LIGHTEN_1_TINT);

	BView* cards = new BView("pages", 0);
	fCards = new BCardLayout();
	cards->SetLayout(fCards);
	for (int32 i = 0; i < fPages.CountItems(); i++) {
		fCards->AddView(fPages.ItemAt(i));
		fSidebar->AddItem(new PageItem(fPages.ItemAt(i)->Title(), i));
	}

	fAdvanced = new BCheckBox("advanced",
		B_TRANSLATE("Show advanced settings"), new BMessage(kMsgAdvanced));

	BLayoutBuilder::Group<>(this)
		.AddGroup(B_VERTICAL, B_USE_SMALL_SPACING)
			.Add(fSearch)
			.Add(sidebarScroll)
		.End()
		.AddGroup(B_VERTICAL, B_USE_DEFAULT_SPACING)
			.Add(fTitle)
			.Add(fSearchNote)
			.Add(cards)
			.AddGroup(B_HORIZONTAL)
				.Add(fAdvanced)
				.AddGlue()
			.End()
		.End();
	fSearchNote->Hide();
}


AudioSettingsView::~AudioSettingsView()
{
	delete fRunner;
	delete fDevicesRunner;
}


void
AudioSettingsView::AttachedToWindow()
{
	BGroupView::AttachedToWindow();
	fSearch->SetTarget(this);
	fSidebar->SetTarget(this);
	fAdvanced->SetTarget(this);

	BMessage refresh(kMsgRefresh);
	fRunner = new BMessageRunner(BMessenger(this), &refresh, 1000000);
	BMediaRoster* roster = BMediaRoster::Roster();
	if (roster != NULL) {
		roster->StartWatching(BMessenger(this), B_MEDIA_FLAVORS_CHANGED);
		roster->StartWatching(BMessenger(this), B_MEDIA_DEFAULT_CHANGED);
		roster->StartWatching(BMessenger(this), B_MEDIA_SERVER_STARTED);
	}

	for (int32 i = 0; i < fPages.CountItems(); i++)
		fPages.ItemAt(i)->Refresh();
	if (fCurrent < 0)
		_ShowPage(0);
}


void
AudioSettingsView::DetachedFromWindow()
{
	BMediaRoster* roster = BMediaRoster::CurrentRoster();
	if (roster != NULL) {
		roster->StopWatching(BMessenger(this), B_MEDIA_FLAVORS_CHANGED);
		roster->StopWatching(BMessenger(this), B_MEDIA_DEFAULT_CHANGED);
		roster->StopWatching(BMessenger(this), B_MEDIA_SERVER_STARTED);
	}
	delete fRunner;
	fRunner = NULL;
	if (fCurrent >= 0)
		fPages.ItemAt(fCurrent)->PageShown(false);
	BGroupView::DetachedFromWindow();
}


void
AudioSettingsView::MessageReceived(BMessage* message)
{
	switch (message->what) {
		case kMsgPageSelected:
		{
			PageItem* item = dynamic_cast<PageItem*>(
				fSidebar->ItemAt(fSidebar->CurrentSelection()));
			if (item != NULL)
				_ShowPage(item->Index());
			break;
		}

		case kMsgAdvanced:
			SetAdvanced(fAdvanced->Value() == B_CONTROL_ON);
			break;

		case kMsgSearch:
			Search(fSearch->Text());
			break;

		case kMsgRefresh:
			_RefreshDevices(false);
			break;

		case B_MEDIA_FLAVORS_CHANGED:
		case B_MEDIA_DEFAULT_CHANGED:
		case B_MEDIA_SERVER_STARTED:
		{
			delete fDevicesRunner;
			BMessage settled(kMsgDevicesSettled);
			fDevicesRunner = new BMessageRunner(BMessenger(this), &settled,
				kDevicesSettleDelay, 1);
			_RefreshDevices(true);
			break;
		}

		case kMsgDevicesSettled:
			delete fDevicesRunner;
			fDevicesRunner = NULL;
			_RefreshDevices(true);
			break;

		case kMsgDetailsChanged:
			// a page changed a card's setting: show it (the list has the
			// new value already; the node confirms it with the next read)
			for (int32 i = 0; i < fPages.CountItems(); i++)
				fPages.ItemAt(i)->Refresh();
			break;

		default:
			BGroupView::MessageReceived(message);
			break;
	}
}


/*!	Reading a card's parameters makes its driver ask the hardware (USB
	devices get control requests), so the details (frame rate, buffer size)
	are read only when \a details is set: on media notifications, after
	the user changed something, when the cards changed, and every ten
	seconds; the device list itself once a second.
*/
void
AudioSettingsView::SelectDevice(const char* name)
{
	SelectPage("devices");
	fDevicesPage->SelectDevice(name);
}


void
AudioSettingsView::_RefreshDevices(bool details)
{
	BString key = fDevices.Key();
	if (!details && ++fRefreshes % 10 != 0) {
		fDevices.Update(false);
		details = fDevices.Key() != key;
	}
	if (details || fRefreshes % 10 == 0)
		fDevices.Update(true);
	for (int32 i = 0; i < fPages.CountItems(); i++)
		fPages.ItemAt(i)->Refresh();
}


int32
AudioSettingsView::_PageIndex(const char* name) const
{
	for (int32 i = 0; name != NULL && i < fPages.CountItems(); i++) {
		if (strcasecmp(fPages.ItemAt(i)->Name(), name) == 0
			|| strcasecmp(fPages.ItemAt(i)->Title(), name) == 0) {
			return i;
		}
	}
	return -1;
}


void
AudioSettingsView::SelectPage(const char* name)
{
	int32 index = _PageIndex(name);
	if (index >= 0)
		_ShowPage(index);
}


void
AudioSettingsView::_ShowPage(int32 index)
{
	if (index < 0 || index >= fPages.CountItems())
		return;
	if (index != fCurrent) {
		if (fCurrent >= 0)
			fPages.ItemAt(fCurrent)->PageShown(false);
		fCurrent = index;
		fCards->SetVisibleItem(index);
		fTitle->SetText(fPages.ItemAt(index)->Title());
		fPages.ItemAt(index)->PageShown(true);
	}
	for (int32 i = 0; i < fSidebar->CountItems(); i++) {
		PageItem* item = dynamic_cast<PageItem*>(fSidebar->ItemAt(i));
		if (item != NULL && item->Index() == index) {
			if (fSidebar->CurrentSelection() != i)
				fSidebar->Select(i);
			break;
		}
	}
}


void
AudioSettingsView::SetAdvanced(bool advanced)
{
	int32 value = advanced ? B_CONTROL_ON : B_CONTROL_OFF;
	if (fAdvanced->Value() != value)
		fAdvanced->SetValue(value);
	for (int32 i = 0; i < fPages.CountItems(); i++)
		fPages.ItemAt(i)->SetAdvanced(advanced);
}


/*!	The search narrows the sidebar to the pages whose settings match and
	shows the first; a match among advanced settings shows those (as the
	Control Panel's global search will, F-28).
*/
void
AudioSettingsView::Search(const char* text)
{
	if (strcmp(fSearch->Text(), text) != 0)
		fSearch->SetText(text);
	BString query = lower(text).Trim();

	while (BListItem* item = fSidebar->RemoveItem((int32)0))
		delete item;
	int32 first = -1;
	bool firstAdvanced = false;
	for (int32 i = 0; i < fPages.CountItems(); i++) {
		bool advanced = false;
		if (query.Length() > 0 && !fPages.ItemAt(i)->Matches(query,
				&advanced)) {
			continue;
		}
		fSidebar->AddItem(new PageItem(fPages.ItemAt(i)->Title(), i));
		if (first < 0) {
			first = i;
			firstAdvanced = advanced;
		}
	}

	if (query.Length() == 0) {
		if (!fSearchNote->IsHidden(fSearchNote))
			fSearchNote->Hide();
		_ShowPage(fCurrent);
		return;
	}
	if (first < 0) {
		fSearchNote->SetText(B_TRANSLATE("No setting matches the search."));
	} else {
		BString note(B_TRANSLATE("%count% page(s) match the search."));
		BString count;
		count << fSidebar->CountItems();
		note.ReplaceFirst("%count%", count);
		if (firstAdvanced) {
			SetAdvanced(true);
			note << " " << B_TRANSLATE("Advanced settings are shown.");
		}
		fSearchNote->SetText(note);
		_ShowPage(first);
	}
	if (fSearchNote->IsHidden(fSearchNote))
		fSearchNote->Show();
}


void
AudioSettingsView::GetState(BMessage* state)
{
	if (fCurrent >= 0)
		state->AddString("page", fPages.ItemAt(fCurrent)->Name());
	state->AddBool("advanced", fAdvanced->Value() == B_CONTROL_ON);
	for (int32 i = 0; i < fSidebar->CountItems(); i++) {
		PageItem* item = dynamic_cast<PageItem*>(fSidebar->ItemAt(i));
		if (item != NULL)
			state->AddString("sidebar", fPages.ItemAt(item->Index())->Name());
	}
	for (int32 i = 0; i < fDevices.CountDevices(); i++) {
		const AudioDevice* device = fDevices.DeviceAt(i);
		state->AddString("device", device->name);
		state->AddBool("device_output", device->output);
		state->AddBool("device_input", device->input);
		state->AddInt32("device_buffer_frames", device->bufferFrames);
	}
	for (int32 i = 0; i < fDevicesPage->List()->CountItems(); i++) {
		DeviceItem* item = dynamic_cast<DeviceItem*>(
			fDevicesPage->List()->ItemAt(i));
		if (item != NULL)
			state->AddString("device_listed", item->Name());
	}
	const AudioDevice* output = fDevices.FindDevice(
		fDevices.DefaultOutputName());
	for (int32 i = 0; output != NULL
			&& i < output->bufferNames.CountStrings(); i++) {
		state->AddString("output_buffer_choice",
			output->bufferNames.StringAt(i));
	}
	state->AddString("default_output", fDevices.DefaultOutputName());
	state->AddString("default_input", fDevices.DefaultInputName());
	state->AddInt32("input_level", fInputPage->Meter()->Value());
}
