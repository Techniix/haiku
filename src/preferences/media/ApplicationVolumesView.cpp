/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 */


#include "ApplicationVolumesView.h"

#include <stdio.h>

#include <Catalog.h>
#include <CheckBox.h>
#include <GridLayout.h>
#include <GridView.h>
#include <LayoutBuilder.h>
#include <MenuField.h>
#include <MenuItem.h>
#include <MessageRunner.h>
#include <PopUpMenu.h>
#include <Slider.h>
#include <StringView.h>
#include <Window.h>


#undef B_TRANSLATION_CONTEXT
#define B_TRANSLATION_CONTEXT "ApplicationVolumesView"


static const uint32 kMsgRefresh = 'avRf';
static const uint32 kMsgVolume = 'avVl';
static const uint32 kMsgMute = 'avMt';
static const uint32 kMsgOutput = 'avOt';
static const uint32 kMsgLowLatency = 'avLL';

// The mixer's gain range (its slider scale, see PORTING.md), in tenths.
static const int32 kMinGain = -600;
static const int32 kMaxGain = 180;

// A control the user just changed is not overwritten by the refresh for a
// moment, so that it does not jump back while the mixer catches up.
static const bigtime_t kUserChangeGrace = 1500000;


class GainSlider : public BSlider {
public:
	GainSlider(const char* name, BMessage* message)
		:
		BSlider(name, NULL, message, kMinGain, kMaxGain, B_HORIZONTAL)
	{
		SetModificationMessage(new BMessage(*message));
		SetExplicitMinSize(BSize(160, B_SIZE_UNSET));
	}

	virtual const char* UpdateText() const
	{
		snprintf(fText, sizeof(fText), "%.1f", Value() / 10.0);
		return fText;
	}

private:
	mutable char fText[16];
};


struct ApplicationVolumesView::Row {
	int32			id;
	BString			signature;
	BStringView*	name;
	BSlider*		volume;
	BCheckBox*		mute;
	BMenuField*		output;
	BCheckBox*		lowLatency;
	bigtime_t		changed;
};


ApplicationVolumesView::ApplicationVolumesView(const char* name,
	bool showOutputs)
	:
	BGroupView(name, B_VERTICAL, 0),
	fGrid(new BGridView(B_USE_SMALL_SPACING, B_USE_SMALL_SPACING)),
	fEmptyLabel(new BStringView("empty",
		B_TRANSLATE("No application is playing sound."))),
	fRunner(NULL),
	fShowOutputs(showOutputs),
	fShowLowLatency(false)
{
	fEmptyLabel->SetHighUIColor(B_PANEL_TEXT_COLOR, B_LIGHTEN_1_TINT);
	BLayoutBuilder::Group<>(this)
		.Add(fEmptyLabel)
		.Add(fGrid);
}


ApplicationVolumesView::~ApplicationVolumesView()
{
	delete fRunner;
}


void
ApplicationVolumesView::AttachedToWindow()
{
	BGroupView::AttachedToWindow();
	_Refresh();
	BMessage refresh(kMsgRefresh);
	fRunner = new BMessageRunner(BMessenger(this), &refresh, 1000000);
}


void
ApplicationVolumesView::DetachedFromWindow()
{
	delete fRunner;
	fRunner = NULL;
	BGroupView::DetachedFromWindow();
}


void
ApplicationVolumesView::SetShowOutputs(bool show)
{
	if (show == fShowOutputs)
		return;
	fShowOutputs = show;
	for (int32 i = 0; i < fRows.CountItems(); i++) {
		BMenuField* output = fRows.ItemAt(i)->output;
		if (output == NULL)
			continue;
		if (show && output->IsHidden(output))
			output->Show();
		else if (!show && !output->IsHidden(output))
			output->Hide();
	}
	InvalidateLayout();
}


void
ApplicationVolumesView::SetShowLowLatency(bool show)
{
	if (show == fShowLowLatency)
		return;
	fShowLowLatency = show;
	fLayoutKey = "";
	if (Window() != NULL)
		_Refresh();
}


void
ApplicationVolumesView::MessageReceived(BMessage* message)
{
	switch (message->what) {
		case kMsgRefresh:
			_Refresh();
			break;

		case kMsgVolume:
		{
			Row* row = _FindRow(message->GetInt32("id", -1));
			if (row == NULL)
				break;
			row->changed = system_time();
			fControl.SetGain(row->id, row->volume->Value() / 10.0f);
			break;
		}

		case kMsgMute:
		{
			Row* row = _FindRow(message->GetInt32("id", -1));
			if (row == NULL)
				break;
			row->changed = system_time();
			fControl.SetMuted(row->id, row->mute->Value() == B_CONTROL_ON);
			break;
		}

		case kMsgOutput:
		{
			Row* row = _FindRow(message->GetInt32("id", -1));
			if (row == NULL)
				break;
			row->changed = system_time();
			fControl.SetOutput(row->id, message->GetInt32("index", 0));
			break;
		}

		case kMsgLowLatency:
		{
			Row* row = _FindRow(message->GetInt32("id", -1));
			if (row == NULL || row->lowLatency == NULL)
				break;
			row->changed = system_time();
			fControl.SetLowLatency(row->id,
				row->lowLatency->Value() == B_CONTROL_ON);
			break;
		}

		default:
			BGroupView::MessageReceived(message);
			break;
	}
}


ApplicationVolumesView::Row*
ApplicationVolumesView::_FindRow(int32 id) const
{
	for (int32 i = 0; i < fRows.CountItems(); i++) {
		if (fRows.ItemAt(i)->id == id)
			return fRows.ItemAt(i);
	}
	return NULL;
}


void
ApplicationVolumesView::_Refresh()
{
	if (fControl.Update() != B_OK) {
		// No media services: nothing to show.
		if (fLayoutKey != "-") {
			fLayoutKey = "-";
			fRows.MakeEmpty();
			_Rebuild();
		}
		return;
	}

	// Rebuild when the streams or the devices changed, otherwise only
	// update the values.
	BString key;
	for (int32 i = 0; i < fControl.CountStreams(); i++) {
		const BPrivate::AudioStream* stream = fControl.StreamAt(i);
		key << stream->id << ":" << stream->name << ";";
	}
	key << "|";
	for (int32 i = 0; i < fControl.Outputs().CountStrings(); i++)
		key << fControl.Outputs().StringAt(i) << ";";

	if (key != fLayoutKey) {
		fLayoutKey = key;
		_Rebuild();
	}
	_UpdateValues();
}


void
ApplicationVolumesView::_Rebuild()
{
	// Take the old controls out of the grid.
	BGridLayout* layout = fGrid->GridLayout();
	for (int32 i = layout->CountItems() - 1; i >= 0; i--) {
		BLayoutItem* item = layout->RemoveItem(i);
		if (item == NULL)
			continue;
		BView* view = item->View();
		delete item;
		if (view != NULL) {
			view->RemoveSelf();
			delete view;
		}
	}
	fRows.MakeEmpty();

	for (int32 i = 0; i < fControl.CountStreams(); i++) {
		const BPrivate::AudioStream* stream = fControl.StreamAt(i);
		Row* row = new Row;
		row->id = stream->id;
		row->signature = stream->signature;
		row->changed = 0;

		// Views named after the application's signature, so that
		// scripting (the harness) can address one application's controls.
		BString suffix(":");
		suffix << (stream->signature.Length() > 0 ? stream->signature
			: stream->name);
		row->name = new BStringView((BString("name") << suffix).String(),
			stream->name.String());
		if (stream->signature.Length() > 0)
			row->name->SetToolTip(stream->signature.String());

		BMessage* volume = new BMessage(kMsgVolume);
		volume->AddInt32("id", stream->id);
		row->volume = new GainSlider((BString("volume") << suffix).String(),
			volume);
		row->volume->SetTarget(this);
		row->volume->SetToolTip(B_TRANSLATE("Volume of this application"));

		BMessage* mute = new BMessage(kMsgMute);
		mute->AddInt32("id", stream->id);
		row->mute = new BCheckBox((BString("mute") << suffix).String(),
			B_TRANSLATE("Mute"), mute);
		row->mute->SetTarget(this);

		row->output = NULL;
		if (stream->hasOutput && fControl.Outputs().CountStrings() > 1) {
			BPopUpMenu* menu = new BPopUpMenu("output");
			for (int32 j = 0; j < fControl.Outputs().CountStrings(); j++) {
				BMessage* choice = new BMessage(kMsgOutput);
				choice->AddInt32("id", stream->id);
				choice->AddInt32("index", j);
				BMenuItem* item = new BMenuItem(
					fControl.Outputs().StringAt(j).String(), choice);
				item->SetTarget(this);
				menu->AddItem(item);
			}
			row->output = new BMenuField(
				(BString("output") << suffix).String(), NULL, menu);
			row->output->SetToolTip(
				B_TRANSLATE("Where this application plays"));
		}

		row->lowLatency = NULL;
		if (fShowLowLatency && stream->hasLowLatency) {
			BMessage* lowLatency = new BMessage(kMsgLowLatency);
			lowLatency->AddInt32("id", stream->id);
			row->lowLatency = new BCheckBox(
				(BString("lowLatency") << suffix).String(),
				B_TRANSLATE("Low latency"), lowLatency);
			row->lowLatency->SetTarget(this);
			row->lowLatency->SetToolTip(B_TRANSLATE("Short sound card "
				"buffers for games and emulators while this application "
				"plays; takes effect when it starts playing again"));
		}

		int32 line = fRows.CountItems();
		layout->AddView(row->name, 0, line);
		layout->AddView(row->volume, 1, line);
		layout->AddView(row->mute, 2, line);
		if (row->output != NULL) {
			layout->AddView(row->output, 3, line);
			if (!fShowOutputs)
				row->output->Hide();
		}
		if (row->lowLatency != NULL)
			layout->AddView(row->lowLatency, 4, line);
		fRows.AddItem(row);
	}

	if (fRows.IsEmpty() && fEmptyLabel->IsHidden(fEmptyLabel))
		fEmptyLabel->Show();
	else if (!fRows.IsEmpty() && !fEmptyLabel->IsHidden(fEmptyLabel))
		fEmptyLabel->Hide();
	InvalidateLayout();
}


void
ApplicationVolumesView::_UpdateValues()
{
	bigtime_t now = system_time();
	for (int32 i = 0; i < fControl.CountStreams(); i++) {
		const BPrivate::AudioStream* stream = fControl.StreamAt(i);
		Row* row = _FindRow(stream->id);
		if (row == NULL || now - row->changed < kUserChangeGrace)
			continue;
		int32 value = (int32)(stream->gain * 10 + (stream->gain < 0 ? -0.5
			: 0.5));
		if (row->volume->Value() != value)
			row->volume->SetValue(value);
		row->mute->SetValue(stream->muted ? B_CONTROL_ON : B_CONTROL_OFF);
		if (row->lowLatency != NULL) {
			row->lowLatency->SetValue(stream->lowLatency
				? B_CONTROL_ON : B_CONTROL_OFF);
		}
		if (row->output != NULL) {
			BMenuItem* item = row->output->Menu()->ItemAt(stream->output);
			if (item != NULL && !item->IsMarked())
				item->SetMarked(true);
		}
	}
}
