/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 *
 * The rest of audio routing (batch 11, roadmap F-31/F-32): the Microphone,
 * Mixing and Presets pages of the Audio preferences. Every feature here is
 * off until switched on, and each has its own switch.
 */


#include "AudioMixingPages.h"

#include <stdio.h>
#include <stdlib.h>

#include <Box.h>
#include <Button.h>
#include <Catalog.h>
#include <CheckBox.h>
#include <Directory.h>
#include <Entry.h>
#include <File.h>
#include <FindDirectory.h>
#include <LayoutBuilder.h>
#include <ListView.h>
#include <MenuField.h>
#include <MenuItem.h>
#include <Path.h>
#include <PopUpMenu.h>
#include <Roster.h>
#include <ScrollView.h>
#include <Slider.h>
#include <StringItem.h>
#include <StringView.h>
#include <TextControl.h>

#include "AudioDevices.h"
#include "MicrophoneProcessing.h"


#undef B_TRANSLATION_CONTEXT
#define B_TRANSLATION_CONTEXT "AudioMixingPages"


enum {
	kMsgMicDevice		= 'amDv',
	kMsgMicChanged		= 'amCh',
	kMsgMicKeys			= 'amKy',
	kMsgVirtualAdd		= 'amVa',
	kMsgVirtualRemove	= 'amVr',
	kMsgVirtualSelect	= 'amVs',
	kMsgCombineTarget	= 'amCt',
	kMsgCombineDevice	= 'amCd',
	kMsgDucking			= 'amDk',
	kMsgDuckTrigger		= 'amDt',
	kMsgPresetSelect	= 'amPs',
	kMsgPresetApply		= 'amPa',
	kMsgPresetSave		= 'amPv',
	kMsgPresetDelete	= 'amPd'
};

static const bigtime_t kChangeGrace = 1500000;

struct key_choice {
	int32		code;
	const char*	name;
};

// Keys that make sense held down or pressed alone, as raw key codes.
static const key_choice kKeyChoices[] = {
	{ 0x02, "F1" }, { 0x03, "F2" }, { 0x04, "F3" }, { 0x05, "F4" },
	{ 0x06, "F5" }, { 0x07, "F6" }, { 0x08, "F7" }, { 0x09, "F8" },
	{ 0x0a, "F9" }, { 0x0b, "F10" }, { 0x0c, "F11" }, { 0x0d, "F12" },
	{ 0x0f, "Scroll Lock" }, { 0x10, "Pause" }, { 0x60, "Right Control" },
	{ 0x3b, "Caps Lock" }
};

static const char* kBuiltInPresets[] = {
	"Default", "Voice call", "Streaming", "Music"
};


static BBox*
make_box(const char* name, const char* label, BView* content)
{
	BBox* box = new BBox(name);
	box->SetLabel(label);
	box->AddChild(content);
	return box;
}


static BStringView*
make_note(const char* name, const char* text)
{
	BStringView* note = new BStringView(name, text);
	note->SetHighUIColor(B_PANEL_TEXT_COLOR, B_LIGHTEN_1_TINT);
	return note;
}


// #pragma mark - MicrophonePage


MicrophonePage::MicrophonePage(AudioDeviceList& devices)
	:
	AudioPage("microphone", B_TRANSLATE("Microphone")),
	fDevices(devices),
	fChanged(0)
{
	AddKeywords(B_TRANSLATE_COMMENT("microphone, mic, mute, noise, noise "
		"gate, gate, automatic gain, agc, high-pass, hum, rumble, "
		"push-to-talk, push to talk, ptt, mute key",
		"Search words for the Microphone page, comma separated"), false);

	fDevice = new BMenuField("micDevice", B_TRANSLATE("Device:"),
		new BPopUpMenu(B_TRANSLATE("None")));
	fMute = new BCheckBox("micMute", B_TRANSLATE("Mute"),
		new BMessage(kMsgMicChanged));
	fMute->SetToolTip(B_TRANSLATE("Records silence until switched off"));
	fGate = new BCheckBox("noiseGate", B_TRANSLATE("Noise gate"),
		new BMessage(kMsgMicChanged));
	fGate->SetToolTip(B_TRANSLATE("Quiets the microphone between words: "
		"sound below the threshold is turned down by 40 dB"));
	fGateThreshold = new BSlider("gateThreshold",
		B_TRANSLATE("Gate threshold:"), new BMessage(kMsgMicChanged), -80,
		-20, B_HORIZONTAL);
	fGateThreshold->SetLimitLabels(B_TRANSLATE("-80 dB"),
		B_TRANSLATE("-20 dB"));
	fGateThreshold->SetValue(-50);
	fAutoGain = new BCheckBox("autoGain", B_TRANSLATE("Automatic gain"),
		new BMessage(kMsgMicChanged));
	fAutoGain->SetToolTip(B_TRANSLATE("Keeps your voice at an even level "
		"(up to +20 dB, slowly; it holds still while you are quiet)"));
	fHighPass = new BCheckBox("highPass",
		B_TRANSLATE("Filter rumble and hum (below 80 Hz)"),
		new BMessage(kMsgMicChanged));
	BStringView* note = make_note("micNote", B_TRANSLATE("Echo cancellation "
		"and spectral noise suppression are not available yet."));

	BGroupView* group = new BGroupView(B_VERTICAL, B_USE_SMALL_SPACING);
	BLayoutBuilder::Group<>(group)
		.SetInsets(B_USE_DEFAULT_SPACING, B_USE_SMALL_SPACING,
			B_USE_DEFAULT_SPACING, B_USE_DEFAULT_SPACING)
		.Add(fDevice)
		.Add(fMute)
		.Add(fGate)
		.Add(fGateThreshold)
		.Add(fAutoGain)
		.Add(fHighPass)
		.Add(note);
	BBox* processing = make_box("micBox", B_TRANSLATE("Processing"), group);

	fPushToTalk = new BCheckBox("pushToTalk",
		B_TRANSLATE("Push-to-talk: the microphones record only while the "
			"key is held"), new BMessage(kMsgMicKeys));
	fPushToTalkKey = new BMenuField("pushToTalkKey", B_TRANSLATE("Key:"),
		new BPopUpMenu(B_TRANSLATE("F9")));
	fMuteKey = new BMenuField("muteKey",
		B_TRANSLATE("Mute or unmute all microphones with:"),
		new BPopUpMenu(B_TRANSLATE("No key")));
	_FillKeyMenu(fPushToTalkKey, kMsgMicKeys);
	_FillKeyMenu(fMuteKey, kMsgMicKeys);
	BMessage* none = new BMessage(kMsgMicKeys);
	none->AddInt32("key", 0);
	fMuteKey->Menu()->AddItem(new BMenuItem(B_TRANSLATE("No key"), none), 0);
	group = new BGroupView(B_VERTICAL, B_USE_SMALL_SPACING);
	BLayoutBuilder::Group<>(group)
		.SetInsets(B_USE_DEFAULT_SPACING, B_USE_SMALL_SPACING,
			B_USE_DEFAULT_SPACING, B_USE_DEFAULT_SPACING)
		.Add(fPushToTalk)
		.Add(fPushToTalkKey)
		.Add(fMuteKey);
	fKeysBox = make_box("micKeysBox", B_TRANSLATE("Keys"), group);

	BLayoutBuilder::Group<>(this)
		.Add(processing)
		.Add(fKeysBox)
		.AddGlue();
}


void
MicrophonePage::_FillKeyMenu(BMenuField* field, uint32 what)
{
	for (size_t i = 0; i < sizeof(kKeyChoices) / sizeof(kKeyChoices[0]);
			i++) {
		BMessage* message = new BMessage(what);
		message->AddInt32("key", kKeyChoices[i].code);
		field->Menu()->AddItem(new BMenuItem(kKeyChoices[i].name, message));
	}
}


void
MicrophonePage::_MarkKey(BMenuField* field, int32 key)
{
	for (int32 i = 0; BMenuItem* item = field->Menu()->ItemAt(i); i++) {
		if (item->Message() != NULL
			&& item->Message()->GetInt32("key", -1) == key) {
			item->SetMarked(true);
			return;
		}
	}
}


void
MicrophonePage::AttachedToWindow()
{
	AudioPage::AttachedToWindow();
	fMute->SetTarget(this);
	fGate->SetTarget(this);
	fGateThreshold->SetTarget(this);
	fAutoGain->SetTarget(this);
	fHighPass->SetTarget(this);
	fPushToTalk->SetTarget(this);
	fPushToTalkKey->Menu()->SetTargetForItems(this);
	fMuteKey->Menu()->SetTargetForItems(this);

	audio_keys keys;
	read_audio_keys(keys);
	fPushToTalk->SetValue(keys.pushToTalk != 0);
	_MarkKey(fPushToTalkKey, keys.pushToTalk != 0 ? keys.pushToTalk : 0x0a);
	_MarkKey(fMuteKey, keys.muteToggle);
	fPushToTalkKey->SetEnabled(keys.pushToTalk != 0);
	Refresh();
}


void
MicrophonePage::MessageReceived(BMessage* message)
{
	switch (message->what) {
		case kMsgMicDevice:
			fDeviceName = message->GetString("device", "");
			fChanged = 0;
			_Read();
			break;

		case kMsgMicChanged:
			fChanged = system_time();
			fGateThreshold->SetEnabled(fGate->Value() == B_CONTROL_ON);
			_Write();
			break;

		case kMsgMicKeys:
			_WriteKeys();
			break;

		default:
			AudioPage::MessageReceived(message);
			break;
	}
}


void
MicrophonePage::SetAdvanced(bool advanced)
{
}


void
MicrophonePage::Refresh()
{
	BStringList names = fDevices.InputNames();
	BString key;
	for (int32 i = 0; i < names.CountStrings(); i++)
		key << names.StringAt(i) << ";";
	if (key != fDeviceKey) {
		fDeviceKey = key;
		BMenu* menu = fDevice->Menu();
		while (BMenuItem* item = menu->RemoveItem((int32)0))
			delete item;
		for (int32 i = 0; i < names.CountStrings(); i++) {
			BMessage* message = new BMessage(kMsgMicDevice);
			message->AddString("device", names.StringAt(i));
			BMenuItem* item = new BMenuItem(names.StringAt(i), message);
			item->SetTarget(this);
			menu->AddItem(item);
		}
		if (!names.HasString(fDeviceName))
			fDeviceName = fDevices.DefaultInputName();
		if (fDeviceName.IsEmpty() && !names.IsEmpty())
			fDeviceName = names.StringAt(0);
		if (BMenuItem* item = menu->FindItem(fDeviceName))
			item->SetMarked(true);
		bool any = !names.IsEmpty();
		fDevice->SetEnabled(any);
		fMute->SetEnabled(any);
		fGate->SetEnabled(any);
		fAutoGain->SetEnabled(any);
		fHighPass->SetEnabled(any);
	}
	_Read();
}


void
MicrophonePage::_Read()
{
	if (fDeviceName.IsEmpty()
		|| system_time() - fChanged < kChangeGrace) {
		fGateThreshold->SetEnabled(fGate->Value() == B_CONTROL_ON);
		return;
	}
	BString text;
	get_input_processing(fDeviceName, text);
	fMute->SetValue(input_processing_value(text, "mute", "0") == "1");
	float gate = atof(input_processing_value(text, "gate", "0").String());
	fGate->SetValue(gate != 0);
	if (gate != 0 && (int32)gate != fGateThreshold->Value())
		fGateThreshold->SetValue((int32)gate);
	fGateThreshold->SetEnabled(gate != 0);
	fAutoGain->SetValue(input_processing_value(text, "agc", "0") == "1");
	fHighPass->SetValue(
		input_processing_value(text, "highpass", "0") == "1");
}


void
MicrophonePage::_Write()
{
	if (fDeviceName.IsEmpty())
		return;
	BString text;
	text << "mute " << (fMute->Value() == B_CONTROL_ON ? 1 : 0) << "\n"
		<< "gate " << (fGate->Value() == B_CONTROL_ON
			? fGateThreshold->Value() : 0) << "\n"
		<< "agc " << (fAutoGain->Value() == B_CONTROL_ON ? 1 : 0) << "\n"
		<< "highpass " << (fHighPass->Value() == B_CONTROL_ON ? 1 : 0)
		<< "\n";
	set_input_processing(fDeviceName, text);
}


void
MicrophonePage::_WriteKeys()
{
	audio_keys keys;
	read_audio_keys(keys);
	bool wasPushToTalk = keys.pushToTalk != 0;
	BMenuItem* item = fPushToTalkKey->Menu()->FindMarked();
	int32 pttKey = item != NULL && item->Message() != NULL
		? item->Message()->GetInt32("key", 0x0a) : 0x0a;
	keys.pushToTalk = fPushToTalk->Value() == B_CONTROL_ON ? pttKey : 0;
	item = fMuteKey->Menu()->FindMarked();
	keys.muteToggle = item != NULL && item->Message() != NULL
		? item->Message()->GetInt32("key", 0) : 0;
	write_audio_keys(keys);
	fPushToTalkKey->SetEnabled(keys.pushToTalk != 0);
	// with push-to-talk the microphones start muted; without it they are
	// heard again
	if (wasPushToTalk != (keys.pushToTalk != 0)) {
		set_all_inputs_muted(keys.pushToTalk != 0);
		fChanged = 0;
		_Read();
	}
	// the agent applies the keys (the input_server filter sends them to it)
	if (keys.pushToTalk != 0 || keys.muteToggle != 0) {
		be_roster->Launch("application/x-vnd.Renga-AudioDeviceAgent");
	}
}


void
MicrophonePage::GetState(BMessage* state)
{
	state->AddString("mic_device", fDeviceName);
	state->AddBool("mic_mute", fMute->Value() == B_CONTROL_ON);
	state->AddInt32("mic_gate", fGate->Value() == B_CONTROL_ON
		? fGateThreshold->Value() : 0);
	state->AddBool("mic_agc", fAutoGain->Value() == B_CONTROL_ON);
	state->AddBool("mic_highpass", fHighPass->Value() == B_CONTROL_ON);
	audio_keys keys;
	read_audio_keys(keys);
	state->AddInt32("ptt_key", keys.pushToTalk);
	state->AddInt32("mute_key", keys.muteToggle);
}


// #pragma mark - MixingPage


MixingPage::MixingPage(AudioDeviceList& devices)
	:
	AudioPage("mixing", B_TRANSLATE("Mixing")),
	fDevices(devices),
	fDuckActive(false),
	fChanged(0)
{
	AddKeywords(B_TRANSLATE_COMMENT("virtual device, virtual output, "
		"virtual microphone, null sink, combine, several inputs, two "
		"microphones, ducking, duck, lower music, voice chat",
		"Search words for the Mixing page, comma separated"), false);

	// virtual devices
	fVirtualList = new BListView("virtualList");
	fVirtualList->SetSelectionMessage(new BMessage(kMsgVirtualSelect));
	BScrollView* scroll = new BScrollView("virtualScroll", fVirtualList, 0,
		false, true);
	scroll->SetExplicitMinSize(BSize(B_SIZE_UNSET, 60));
	fVirtualName = new BTextControl("virtualName", B_TRANSLATE("Name:"),
		"", new BMessage(kMsgVirtualAdd));
	BButton* add = new BButton("virtualAdd", B_TRANSLATE("Add"),
		new BMessage(kMsgVirtualAdd));
	fVirtualRemove = new BButton("virtualRemove", B_TRANSLATE("Remove"),
		new BMessage(kMsgVirtualRemove));
	fVirtualRemove->SetEnabled(false);
	BGroupView* group = new BGroupView(B_VERTICAL, B_USE_SMALL_SPACING);
	BLayoutBuilder::Group<>(group)
		.SetInsets(B_USE_DEFAULT_SPACING, B_USE_SMALL_SPACING,
			B_USE_DEFAULT_SPACING, B_USE_DEFAULT_SPACING)
		.Add(make_note("virtualNote", B_TRANSLATE("Applications can play "
			"into a virtual device (choose it as their output); recording "
			"programs record it like a microphone.")))
		.Add(scroll)
		.AddGroup(B_HORIZONTAL)
			.Add(fVirtualName)
			.Add(add)
			.Add(fVirtualRemove)
		.End();
	BBox* virtualBox = make_box("virtualBox", B_TRANSLATE("Virtual devices"),
		group);

	// several inputs at once
	fCombineTarget = new BMenuField("combineTarget",
		B_TRANSLATE("Record into:"), new BPopUpMenu(B_TRANSLATE("None")));
	fCombineGroup = new BGroupView(B_VERTICAL, 0);
	group = new BGroupView(B_VERTICAL, B_USE_SMALL_SPACING);
	BLayoutBuilder::Group<>(group)
		.SetInsets(B_USE_DEFAULT_SPACING, B_USE_SMALL_SPACING,
			B_USE_DEFAULT_SPACING, B_USE_DEFAULT_SPACING)
		.Add(make_note("combineNote", B_TRANSLATE("Record several inputs "
			"at once: the inputs checked here are mixed into the virtual "
			"device.")))
		.Add(fCombineTarget)
		.Add(fCombineGroup);
	BBox* combineBox = make_box("combineBox",
		B_TRANSLATE("Several inputs at once"), group);

	// ducking
	fDucking = new BCheckBox("ducking", B_TRANSLATE("Lower other sounds "
		"while these applications play (ducking)"),
		new BMessage(kMsgDucking));
	fDuckAmount = new BSlider("duckAmount", B_TRANSLATE("Lower by:"),
		new BMessage(kMsgDucking), -40, 0, B_HORIZONTAL);
	fDuckAmount->SetLimitLabels(B_TRANSLATE("-40 dB"), B_TRANSLATE("0 dB"));
	fDuckAmount->SetValue(-12);
	fTriggerGroup = new BGroupView(B_VERTICAL, 0);
	fDuckState = make_note("duckState", "");
	group = new BGroupView(B_VERTICAL, B_USE_SMALL_SPACING);
	BLayoutBuilder::Group<>(group)
		.SetInsets(B_USE_DEFAULT_SPACING, B_USE_SMALL_SPACING,
			B_USE_DEFAULT_SPACING, B_USE_DEFAULT_SPACING)
		.Add(fDucking)
		.Add(fDuckAmount)
		.Add(fTriggerGroup)
		.Add(fDuckState);
	BBox* duckBox = make_box("duckBox", B_TRANSLATE("Ducking"), group);

	BLayoutBuilder::Group<>(this)
		.Add(virtualBox)
		.Add(combineBox)
		.Add(duckBox)
		.AddGlue();
	// Open (batch 11): the mixer connects the inputs, but their recording
	// does not reach the mix yet (media_mixing_test's "combined input"
	// line); not offered until it does
	combineBox->Hide();
}


void
MixingPage::AttachedToWindow()
{
	AudioPage::AttachedToWindow();
	fVirtualList->SetTarget(this);
	fVirtualName->SetTarget(this);
	if (BButton* add = dynamic_cast<BButton*>(FindView("virtualAdd")))
		add->SetTarget(this);
	fVirtualRemove->SetTarget(this);
	fDucking->SetTarget(this);
	fDuckAmount->SetTarget(this);
	Refresh();
}


void
MixingPage::MessageReceived(BMessage* message)
{
	switch (message->what) {
		case kMsgVirtualAdd:
		{
			BString name = fVirtualName->Text();
			name.Trim();
			if (name.IsEmpty() || name.FindFirst('\n') >= 0
				|| name.Length() >= 64 || fVirtual.HasString(name)) {
				break;
			}
			fVirtual.Add(name);
			BString text;
			for (int32 i = 0; i < fVirtual.CountStrings(); i++)
				text << fVirtual.StringAt(i) << "\n";
			fControl.SetMixerText(RENGA_VIRTUAL_DEVICES_KIND, text);
			fVirtualName->SetText("");
			fChanged = system_time();
			Refresh();
			break;
		}

		case kMsgVirtualRemove:
		{
			int32 index = fVirtualList->CurrentSelection();
			if (index < 0 || index >= fVirtual.CountStrings())
				break;
			BString name = fVirtual.StringAt(index);
			fVirtual.Remove(index);
			BString text;
			for (int32 i = 0; i < fVirtual.CountStrings(); i++)
				text << fVirtual.StringAt(i) << "\n";
			fControl.SetMixerText(RENGA_VIRTUAL_DEVICES_KIND, text);
			// its combined inputs go with it
			for (int32 i = fCombined.CountStrings() - 1; i >= 0; i--) {
				if (fCombined.StringAt(i).StartsWith(BString(name) << "\t"))
					fCombined.Remove(i);
			}
			_WriteCombined();
			fChanged = system_time();
			Refresh();
			break;
		}

		case kMsgVirtualSelect:
			fVirtualRemove->SetEnabled(fVirtualList->CurrentSelection() >= 0);
			break;

		case kMsgCombineTarget:
			fCombineKey = "";
			_RebuildCombine();
			break;

		case kMsgCombineDevice:
		{
			BMenuItem* item = fCombineTarget->Menu()->FindMarked();
			const char* device = message->GetString("device", NULL);
			if (item == NULL || device == NULL)
				break;
			BString line;
			line << item->Label() << "\t" << device;
			BCheckBox* box = NULL;
			message->FindPointer("source", (void**)&box);
			bool on = box != NULL && box->Value() == B_CONTROL_ON;
			if (on && !fCombined.HasString(line))
				fCombined.Add(line);
			else if (!on)
				fCombined.Remove(line);
			fChanged = system_time();
			_WriteCombined();
			break;
		}

		case kMsgDucking:
		case kMsgDuckTrigger:
		{
			if (message->what == kMsgDuckTrigger) {
				const char* signature = message->GetString("signature", NULL);
				BCheckBox* box = NULL;
				message->FindPointer("source", (void**)&box);
				if (signature != NULL && box != NULL) {
					if (box->Value() == B_CONTROL_ON
						&& !fTriggers.HasString(signature)) {
						fTriggers.Add(signature);
					} else if (box->Value() != B_CONTROL_ON)
						fTriggers.Remove(signature);
				}
			}
			fChanged = system_time();
			_WriteDucking();
			break;
		}

		default:
			AudioPage::MessageReceived(message);
			break;
	}
}


void
MixingPage::SetAdvanced(bool advanced)
{
}


void
MixingPage::_WriteDucking()
{
	BString text;
	text << "on " << (fDucking->Value() == B_CONTROL_ON ? 1 : 0) << "\n"
		<< "amount " << fDuckAmount->Value() << "\n";
	for (int32 i = 0; i < fTriggers.CountStrings(); i++)
		text << "trigger " << fTriggers.StringAt(i) << "\n";
	fControl.SetMixerText(RENGA_DUCKING_KIND, text);
	fDuckAmount->SetEnabled(fDucking->Value() == B_CONTROL_ON);
}


void
MixingPage::_WriteCombined()
{
	BString text;
	for (int32 i = 0; i < fCombined.CountStrings(); i++)
		text << fCombined.StringAt(i) << "\n";
	fControl.SetMixerText(RENGA_COMBINED_INPUTS_KIND, text);
}


void
MixingPage::Refresh()
{
	if (fControl.Update() != B_OK)
		return;
	bool userChanged = system_time() - fChanged < kChangeGrace;

	if (!userChanged) {
		fControl.GetVirtualDevices(fVirtual);
		BString text;
		fControl.GetMixerText(RENGA_COMBINED_INPUTS_KIND, text);
		fCombined.MakeEmpty();
		text.Split("\n", true, fCombined);
	}
	// the list of virtual devices and the target menu
	BString key;
	for (int32 i = 0; i < fVirtual.CountStrings(); i++)
		key << fVirtual.StringAt(i) << ";";
	if (key != fVirtualKey) {
		fVirtualKey = key;
		int32 selected = fVirtualList->CurrentSelection();
		while (BListItem* item = fVirtualList->RemoveItem((int32)0))
			delete item;
		for (int32 i = 0; i < fVirtual.CountStrings(); i++)
			fVirtualList->AddItem(new BStringItem(fVirtual.StringAt(i)));
		if (selected >= 0 && selected < fVirtualList->CountItems())
			fVirtualList->Select(selected);
		fVirtualRemove->SetEnabled(fVirtualList->CurrentSelection() >= 0);

		BMenu* menu = fCombineTarget->Menu();
		BMenuItem* marked = menu->FindMarked();
		BString was = marked != NULL ? marked->Label() : "";
		while (BMenuItem* item = menu->RemoveItem((int32)0))
			delete item;
		for (int32 i = 0; i < fVirtual.CountStrings(); i++) {
			BMenuItem* item = new BMenuItem(fVirtual.StringAt(i),
				new BMessage(kMsgCombineTarget));
			item->SetTarget(this);
			menu->AddItem(item);
		}
		BMenuItem* item = menu->FindItem(was);
		if (item == NULL)
			item = menu->ItemAt(0);
		if (item != NULL)
			item->SetMarked(true);
		fCombineTarget->SetEnabled(!fVirtual.IsEmpty());
		fCombineKey = "";
	}
	_RebuildCombine();

	// ducking
	if (!userChanged) {
		BString text;
		fControl.GetMixerText(RENGA_DUCKING_KIND, text);
		BStringList lines;
		text.Split("\n", true, lines);
		fTriggers.MakeEmpty();
		bool on = false;
		int32 amount = -12;
		fDuckActive = false;
		for (int32 i = 0; i < lines.CountStrings(); i++) {
			const BString& line = lines.StringAt(i);
			if (line.StartsWith("on "))
				on = atoi(line.String() + 3) != 0;
			else if (line.StartsWith("amount "))
				amount = atoi(line.String() + 7);
			else if (line.StartsWith("trigger "))
				fTriggers.Add(line.String() + 8);
			else if (line.StartsWith("active "))
				fDuckActive = atoi(line.String() + 7) != 0;
		}
		fDucking->SetValue(on);
		if (fDuckAmount->Value() != amount)
			fDuckAmount->SetValue(amount);
		fDuckAmount->SetEnabled(on);
	}
	fDuckState->SetText(fDucking->Value() != B_CONTROL_ON ? ""
		: fDuckActive ? B_TRANSLATE("Other sounds are lowered now.")
		: B_TRANSLATE("Nothing is lowered now."));
	_RebuildTriggers();
}


void
MixingPage::_RebuildCombine()
{
	BMenuItem* target = fCombineTarget->Menu()->FindMarked();
	BStringList inputs = fDevices.InputNames();
	BString key;
	key << (target != NULL ? target->Label() : "") << "|";
	for (int32 i = 0; i < inputs.CountStrings(); i++)
		key << inputs.StringAt(i) << ";";
	if (key != fCombineKey) {
		fCombineKey = key;
		while (BView* child = fCombineGroup->ChildAt(0)) {
			child->RemoveSelf();
			delete child;
		}
		for (int32 i = 0; target != NULL && i < inputs.CountStrings(); i++) {
			BMessage* message = new BMessage(kMsgCombineDevice);
			message->AddString("device", inputs.StringAt(i));
			BString name("combine:");
			name << inputs.StringAt(i);
			BCheckBox* box = new BCheckBox(name, inputs.StringAt(i), message);
			box->SetTarget(this);
			fCombineGroup->AddChild(box);
		}
	}
	for (int32 i = 0; BView* child = fCombineGroup->ChildAt(i); i++) {
		BCheckBox* box = dynamic_cast<BCheckBox*>(child);
		if (box == NULL || target == NULL)
			continue;
		BString line;
		line << target->Label() << "\t" << box->Label();
		box->SetValue(fCombined.HasString(line));
	}
}


void
MixingPage::_RebuildTriggers()
{
	// the applications that play now, and those already chosen
	BStringList signatures, names;
	for (int32 i = 0; i < fControl.CountStreams(); i++) {
		const BPrivate::AudioStream* stream = fControl.StreamAt(i);
		if (stream->signature.IsEmpty()
			|| signatures.HasString(stream->signature)) {
			continue;
		}
		signatures.Add(stream->signature);
		names.Add(stream->name);
	}
	for (int32 i = 0; i < fTriggers.CountStrings(); i++) {
		if (!signatures.HasString(fTriggers.StringAt(i))) {
			signatures.Add(fTriggers.StringAt(i));
			names.Add(fTriggers.StringAt(i));
		}
	}
	BString key;
	for (int32 i = 0; i < signatures.CountStrings(); i++)
		key << signatures.StringAt(i) << ";";
	if (key != fTriggerKey) {
		fTriggerKey = key;
		while (BView* child = fTriggerGroup->ChildAt(0)) {
			child->RemoveSelf();
			delete child;
		}
		for (int32 i = 0; i < signatures.CountStrings(); i++) {
			BMessage* message = new BMessage(kMsgDuckTrigger);
			message->AddString("signature", signatures.StringAt(i));
			BString name("duck:");
			name << signatures.StringAt(i);
			BCheckBox* box = new BCheckBox(name, names.StringAt(i), message);
			box->SetTarget(this);
			box->SetToolTip(signatures.StringAt(i));
			fTriggerGroup->AddChild(box);
		}
	}
	for (int32 i = 0; BView* child = fTriggerGroup->ChildAt(i); i++) {
		BCheckBox* box = dynamic_cast<BCheckBox*>(child);
		const char* signature = box != NULL && box->Message() != NULL
			? box->Message()->GetString("signature", NULL) : NULL;
		if (signature != NULL)
			box->SetValue(fTriggers.HasString(signature));
	}
}


void
MixingPage::GetState(BMessage* state)
{
	for (int32 i = 0; i < fVirtual.CountStrings(); i++)
		state->AddString("virtual_device", fVirtual.StringAt(i));
	for (int32 i = 0; i < fCombined.CountStrings(); i++)
		state->AddString("combined_input", fCombined.StringAt(i));
	state->AddBool("ducking", fDucking->Value() == B_CONTROL_ON);
	state->AddInt32("duck_amount", fDuckAmount->Value());
	state->AddBool("duck_active", fDuckActive);
	for (int32 i = 0; i < fTriggers.CountStrings(); i++)
		state->AddString("duck_trigger", fTriggers.StringAt(i));
	for (int32 i = 0; BView* child = fTriggerGroup->ChildAt(i); i++)
		state->AddString("duck_choice", child->Name());
}


// #pragma mark - PresetsPage


static status_t
presets_directory(BPath& path)
{
	status_t status = find_directory(B_USER_SETTINGS_DIRECTORY, &path);
	if (status == B_OK)
		status = path.Append("Media/audio_presets");
	return status;
}


static bool
is_built_in(const char* name)
{
	for (size_t i = 0; i < sizeof(kBuiltInPresets) / sizeof(char*); i++) {
		if (strcmp(name, kBuiltInPresets[i]) == 0)
			return true;
	}
	return false;
}


//!	The current settings, as a saved preset holds them.
static void
capture_settings(BMessage& preset, AudioDeviceList& devices)
{
	BPrivate::AudioStreamControl control;
	BString text;
	if (control.Update() == B_OK
		&& control.GetMixerText(RENGA_DUCKING_KIND, text) == B_OK) {
		preset.AddString("ducking", text);
	}
	BString input = devices.DefaultInputName();
	if (!input.IsEmpty() && get_input_processing(input, text) == B_OK)
		preset.AddString("mic", text);
	audio_keys keys;
	read_audio_keys(keys);
	preset.AddInt32("ptt", keys.pushToTalk);
	preset.AddInt32("mute_key", keys.muteToggle);
	preset.AddString("default_output", devices.DefaultOutputName());
	preset.AddString("default_input", input);
}


/*!	Replaces the "on"/"amount" lines of the ducking text and keeps its
	triggers (built-in presets do not know the user's applications).
*/
static BString
ducking_with(const BString& current, bool on, int32 amount)
{
	BString result;
	result << "on " << (on ? 1 : 0) << "\n" << "amount " << amount << "\n";
	BStringList lines;
	current.Split("\n", true, lines);
	for (int32 i = 0; i < lines.CountStrings(); i++) {
		if (lines.StringAt(i).StartsWith("trigger "))
			result << lines.StringAt(i) << "\n";
	}
	return result;
}


status_t
PresetsPage::Apply(const char* name, AudioDeviceList& devices)
{
	BPrivate::AudioStreamControl control;
	status_t status = control.Update();
	if (status != B_OK)
		return status;
	BString ducking;
	control.GetMixerText(RENGA_DUCKING_KIND, ducking);
	BString input = devices.DefaultInputName();

	if (is_built_in(name)) {
		BString mic;
		bool duckOn = false;
		int32 amount = -12;
		if (strcmp(name, "Default") == 0)
			mic = "mute 0\ngate 0\nagc 0\nhighpass 0\n";
		else if (strcmp(name, "Voice call") == 0) {
			mic = "mute 0\ngate -50\nagc 1\nhighpass 1\n";
			duckOn = true;
			amount = -15;
		} else if (strcmp(name, "Streaming") == 0) {
			mic = "mute 0\ngate -45\nagc 0\nhighpass 1\n";
			duckOn = true;
			amount = -10;
		} else
			mic = "mute 0\ngate 0\nagc 0\nhighpass 0\n";
		control.SetMixerText(RENGA_DUCKING_KIND,
			ducking_with(ducking, duckOn, amount));
		if (!input.IsEmpty())
			set_input_processing(input, mic);
		if (strcmp(name, "Default") == 0 || strcmp(name, "Music") == 0) {
			audio_keys keys;
			read_audio_keys(keys);
			if (keys.pushToTalk != 0) {
				keys.pushToTalk = 0;
				write_audio_keys(keys);
				set_all_inputs_muted(false);
			}
		}
		return B_OK;
	}

	BPath path;
	status = presets_directory(path);
	if (status == B_OK)
		status = path.Append(name);
	BFile file(path.Path(), B_READ_ONLY);
	BMessage preset;
	if (status == B_OK)
		status = file.InitCheck();
	if (status == B_OK)
		status = preset.Unflatten(&file);
	if (status != B_OK)
		return status;

	const char* text;
	if (preset.FindString("ducking", &text) == B_OK)
		control.SetMixerText(RENGA_DUCKING_KIND, text);
	const char* output;
	if (preset.FindString("default_output", &output) == B_OK
		&& output[0] != '\0' && output != devices.DefaultOutputName()
		&& devices.FindDevice(output) != NULL) {
		devices.SetDefaultOutput(output);
	}
	const char* inputName;
	if (preset.FindString("default_input", &inputName) == B_OK
		&& inputName[0] != '\0' && devices.FindDevice(inputName) != NULL) {
		if (inputName != input)
			devices.SetDefaultInput(inputName);
		input = inputName;
	}
	if (preset.FindString("mic", &text) == B_OK && !input.IsEmpty())
		set_input_processing(input, text);
	audio_keys keys;
	read_audio_keys(keys);
	bool wasPushToTalk = keys.pushToTalk != 0;
	keys.pushToTalk = preset.GetInt32("ptt", 0);
	keys.muteToggle = preset.GetInt32("mute_key", 0);
	write_audio_keys(keys);
	if (wasPushToTalk != (keys.pushToTalk != 0))
		set_all_inputs_muted(keys.pushToTalk != 0);
	return B_OK;
}


PresetsPage::PresetsPage(AudioDeviceList& devices)
	:
	AudioPage("presets", B_TRANSLATE("Presets")),
	fDevices(devices)
{
	AddKeywords(B_TRANSLATE_COMMENT("preset, presets, profile, voice call, "
		"streaming, music, save settings",
		"Search words for the Presets page, comma separated"), false);

	fList = new BListView("presetList");
	fList->SetSelectionMessage(new BMessage(kMsgPresetSelect));
	fList->SetInvocationMessage(new BMessage(kMsgPresetApply));
	BScrollView* scroll = new BScrollView("presetScroll", fList, 0, false,
		true);
	scroll->SetExplicitMinSize(BSize(B_SIZE_UNSET, 110));
	fApply = new BButton("presetApply", B_TRANSLATE("Apply"),
		new BMessage(kMsgPresetApply));
	fDelete = new BButton("presetDelete", B_TRANSLATE("Delete"),
		new BMessage(kMsgPresetDelete));
	fName = new BTextControl("presetName", B_TRANSLATE("Name:"), "",
		new BMessage(kMsgPresetSave));
	BButton* save = new BButton("presetSave",
		B_TRANSLATE("Save current settings"), new BMessage(kMsgPresetSave));
	fStatus = make_note("presetStatus", "");

	BGroupView* group = new BGroupView(B_VERTICAL, B_USE_SMALL_SPACING);
	BLayoutBuilder::Group<>(group)
		.SetInsets(B_USE_DEFAULT_SPACING, B_USE_SMALL_SPACING,
			B_USE_DEFAULT_SPACING, B_USE_DEFAULT_SPACING)
		.Add(make_note("presetNote", B_TRANSLATE("A preset sets ducking, "
			"the default input's microphone processing, push-to-talk and "
			"the default devices at once.")))
		.Add(scroll)
		.AddGroup(B_HORIZONTAL)
			.Add(fApply)
			.Add(fDelete)
			.AddGlue()
		.End()
		.AddGroup(B_HORIZONTAL)
			.Add(fName)
			.Add(save)
		.End()
		.Add(fStatus);
	BLayoutBuilder::Group<>(this)
		.Add(make_box("presetBox", B_TRANSLATE("Presets"), group))
		.AddGlue();
}


void
PresetsPage::AttachedToWindow()
{
	AudioPage::AttachedToWindow();
	fList->SetTarget(this);
	fApply->SetTarget(this);
	fDelete->SetTarget(this);
	fName->SetTarget(this);
	if (BButton* save = dynamic_cast<BButton*>(FindView("presetSave")))
		save->SetTarget(this);
	_Fill();
}


void
PresetsPage::_Fill()
{
	BString selected;
	if (BStringItem* item = dynamic_cast<BStringItem*>(
			fList->ItemAt(fList->CurrentSelection()))) {
		selected = item->Text();
	}
	while (BListItem* item = fList->RemoveItem((int32)0))
		delete item;
	for (size_t i = 0; i < sizeof(kBuiltInPresets) / sizeof(char*); i++)
		fList->AddItem(new BStringItem(kBuiltInPresets[i]));
	BPath path;
	if (presets_directory(path) == B_OK) {
		BDirectory directory(path.Path());
		BEntry entry;
		BStringList saved;
		while (directory.GetNextEntry(&entry) == B_OK) {
			char name[B_FILE_NAME_LENGTH];
			if (entry.GetName(name) == B_OK && !is_built_in(name))
				saved.Add(name);
		}
		saved.Sort();
		for (int32 i = 0; i < saved.CountStrings(); i++)
			fList->AddItem(new BStringItem(saved.StringAt(i)));
	}
	for (int32 i = 0; i < fList->CountItems(); i++) {
		if (selected == ((BStringItem*)fList->ItemAt(i))->Text())
			fList->Select(i);
	}
	BStringItem* item = dynamic_cast<BStringItem*>(
		fList->ItemAt(fList->CurrentSelection()));
	fApply->SetEnabled(item != NULL);
	fDelete->SetEnabled(item != NULL && !is_built_in(item->Text()));
}


void
PresetsPage::MessageReceived(BMessage* message)
{
	switch (message->what) {
		case kMsgPresetSelect:
		{
			BStringItem* item = dynamic_cast<BStringItem*>(
				fList->ItemAt(fList->CurrentSelection()));
			fApply->SetEnabled(item != NULL);
			fDelete->SetEnabled(item != NULL && !is_built_in(item->Text()));
			break;
		}

		case kMsgPresetApply:
		{
			BStringItem* item = dynamic_cast<BStringItem*>(
				fList->ItemAt(fList->CurrentSelection()));
			if (item == NULL)
				break;
			status_t status = Apply(item->Text(), fDevices);
			BString text(status == B_OK ? B_TRANSLATE("Applied \"%name%\".")
				: B_TRANSLATE("Could not apply \"%name%\"."));
			text.ReplaceFirst("%name%", item->Text());
			fStatus->SetText(text);
			if (status == B_OK)
				fLastApplied = item->Text();
			break;
		}

		case kMsgPresetSave:
		{
			BString name = fName->Text();
			name.Trim();
			name.ReplaceAll('/', '-');
			if (name.IsEmpty() || is_built_in(name))
				break;
			BMessage preset('AuPr');
			capture_settings(preset, fDevices);
			BPath path;
			if (presets_directory(path) != B_OK)
				break;
			create_directory(path.Path(), 0755);
			path.Append(name);
			BFile file(path.Path(), B_WRITE_ONLY | B_CREATE_FILE
				| B_ERASE_FILE);
			if (file.InitCheck() == B_OK && preset.Flatten(&file) == B_OK) {
				BString text(B_TRANSLATE("Saved \"%name%\"."));
				text.ReplaceFirst("%name%", name);
				fStatus->SetText(text);
				fName->SetText("");
			}
			_Fill();
			break;
		}

		case kMsgPresetDelete:
		{
			BStringItem* item = dynamic_cast<BStringItem*>(
				fList->ItemAt(fList->CurrentSelection()));
			if (item == NULL || is_built_in(item->Text()))
				break;
			BPath path;
			if (presets_directory(path) == B_OK
				&& path.Append(item->Text()) == B_OK) {
				BEntry(path.Path()).Remove();
			}
			fList->DeselectAll();
			_Fill();
			break;
		}

		default:
			AudioPage::MessageReceived(message);
			break;
	}
}


void
PresetsPage::Refresh()
{
}


void
PresetsPage::GetState(BMessage* state)
{
	for (int32 i = 0; i < fList->CountItems(); i++) {
		state->AddString("preset",
			((BStringItem*)fList->ItemAt(i))->Text());
	}
	state->AddString("preset_applied", fLastApplied);
	state->AddString("preset_status", fStatus->Text());
}
