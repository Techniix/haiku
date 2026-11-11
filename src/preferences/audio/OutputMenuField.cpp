/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 */


#include "OutputMenuField.h"

#include <Catalog.h>
#include <MenuItem.h>
#include <PopUpMenu.h>


#undef B_TRANSLATION_CONTEXT
#define B_TRANSLATION_CONTEXT "OutputMenuField"


static const uint32 kMsgChooseOutput = 'omCh';

// After a choice, the menu shows it for this long even if the media
// services have not switched yet, so that it does not jump back.
static const bigtime_t kChoiceGrace = 2000000;


OutputMenuField::OutputMenuField(const char* name, const char* label)
	:
	BMenuField(name, label, new BPopUpMenu(B_TRANSLATE("None"))),
	fChanged(0)
{
	SetToolTip(B_TRANSLATE("Where sound plays unless an application is set "
		"to play elsewhere"));
}


void
OutputMenuField::AttachedToWindow()
{
	BMenuField::AttachedToWindow();
	Refresh();
}


void
OutputMenuField::MessageReceived(BMessage* message)
{
	if (message->what != kMsgChooseOutput) {
		BMenuField::MessageReceived(message);
		return;
	}

	const char* name;
	if (message->FindString("device", &name) != B_OK)
		return;
	fChanged = system_time();
	if (fDevices.CountDevices() == 0)
		fDevices.Update(false);
	if (fDevices.SetDefaultOutput(name) != B_OK) {
		// gone meanwhile: show the real state again
		fChanged = 0;
		Refresh();
	}
}


void
OutputMenuField::Refresh()
{
	// a failed update leaves the list empty: no media services, no choice
	fDevices.Update(false);
	_Rebuild(fDevices);
}


void
OutputMenuField::Refresh(const AudioDeviceList& devices)
{
	_Rebuild(devices);
}


BString
OutputMenuField::SelectedName() const
{
	BMenuItem* item = Menu()->FindMarked();
	return item != NULL ? BString(item->Label()) : BString();
}


int32
OutputMenuField::CountChoices() const
{
	return Menu()->CountItems();
}


void
OutputMenuField::_Rebuild(const AudioDeviceList& devices)
{
	BStringList names = devices.OutputNames();
	BString key;
	for (int32 i = 0; i < names.CountStrings(); i++)
		key << names.StringAt(i) << ";";

	BMenu* menu = Menu();
	if (key != fKey) {
		fKey = key;
		while (BMenuItem* item = menu->RemoveItem((int32)0))
			delete item;
		for (int32 i = 0; i < names.CountStrings(); i++) {
			BMessage* message = new BMessage(kMsgChooseOutput);
			message->AddString("device", names.StringAt(i));
			BMenuItem* item = new BMenuItem(names.StringAt(i), message);
			item->SetTarget(this);
			menu->AddItem(item);
		}
		SetEnabled(names.CountStrings() > 0);
	}

	if (system_time() - fChanged < kChoiceGrace)
		return;
	BString current = devices.DefaultOutputName();
	BMenuItem* item = current.Length() > 0 ? menu->FindItem(current) : NULL;
	if (item != NULL) {
		if (!item->IsMarked())
			item->SetMarked(true);
	} else if (BMenuItem* marked = menu->FindMarked())
		marked->SetMarked(false);
}
