/*
 * Copyright 2026, LinuxKPI_HaikuOS contributors.
 * Distributed under the terms of the MIT License.
 *
 * Edits a driver's settings file. Only settings its manifest describes are
 * shown; "unsafe" ones are shown read-only; every value is validated by
 * TunableSpec::Validate() before it is written.
 */


#include "SettingsWindow.h"

#include <Alert.h>
#include <Button.h>
#include <Catalog.h>
#include <CheckBox.h>
#include <GridLayout.h>
#include <LayoutBuilder.h>
#include <StringView.h>
#include <TextControl.h>

#include "MainWindow.h"


#undef B_TRANSLATION_CONTEXT
#define B_TRANSLATION_CONTEXT "SettingsWindow"


using namespace LKPI;


enum {
	kMsgSave	= 'save',
	kMsgReset	= 'rset'
};


static bool
is_on(const BString& value)
{
	return value == "1" || value == "true" || value == "yes" || value == "on";
}


SettingsWindow::SettingsWindow(const BString& driverName, BMessenger target)
	:
	BWindow(BRect(120, 120, 620, 420), "", B_TITLED_WINDOW,
		B_AUTO_UPDATE_SIZE_LIMITS | B_NOT_ZOOMABLE | B_CLOSE_ON_ESCAPE),
	fDriver(NULL),
	fTarget(target)
{
	fCatalog.Load();
	fDriver = fCatalog.FindDriver(driverName.String());

	BString title;
	title.SetToFormat(B_TRANSLATE("%s settings"), driverName.String());
	SetTitle(title.String());

	BGridLayout* grid = new BGridLayout(B_USE_DEFAULT_SPACING,
		B_USE_SMALL_SPACING);
	BStringList keys, values;
	if (fDriver != NULL)
		SettingsStore::Read(*fDriver, keys, values);

	for (int32 i = 0; fDriver != NULL && i < fDriver->tunables.CountItems();
			i++) {
		const TunableSpec* t = fDriver->tunables.ItemAt(i);
		int32 index = keys.IndexOf(t->key);
		BString current;
		if (index >= 0)
			current = values.StringAt(index);
		else
			current << t->defaultValue;

		BControl* control;
		if (t->type == "bool") {
			BCheckBox* box = new BCheckBox(t->key.String(), t->label.String(),
				NULL);
			box->SetValue(is_on(current) ? B_CONTROL_ON : B_CONTROL_OFF);
			control = box;
		} else {
			control = new BTextControl(t->key.String(), t->label.String(),
				current.String(), NULL);
		}
		control->SetEnabled(t->risk != kRiskUnsafe);
		fControls.AddItem(control);

		const char* risk = t->risk == kRiskSafe ? B_TRANSLATE("Safe")
			: t->risk == kRiskExperimental ? B_TRANSLATE("Experimental")
			: B_TRANSLATE("Locked");
		BStringView* riskView = new BStringView("risk", risk);
		BStringView* description = new BStringView("description",
			t->description.String());
		description->SetFont(be_plain_font);
		description->SetHighUIColor(B_PANEL_TEXT_COLOR, B_DARKEN_1_TINT);

		if (BTextControl* text = dynamic_cast<BTextControl*>(control)) {
			grid->AddItem(text->CreateLabelLayoutItem(), 0, i * 2);
			grid->AddItem(text->CreateTextViewLayoutItem(), 1, i * 2);
		} else
			grid->AddView(control, 0, i * 2, 2);
		grid->AddView(riskView, 2, i * 2);
		grid->AddView(description, 0, i * 2 + 1, 3);
	}

	BStringView* note = new BStringView("note", B_TRANSLATE(
		"Changes take effect the next time the driver loads (restart)."));
	BButton* reset = new BButton("reset", B_TRANSLATE("Defaults"),
		new BMessage(kMsgReset));
	BButton* save = new BButton("save", B_TRANSLATE("Save"),
		new BMessage(kMsgSave));
	save->MakeDefault(true);
	save->SetEnabled(fDriver != NULL);
	reset->SetEnabled(fDriver != NULL);

	BLayoutBuilder::Group<>(this, B_VERTICAL)
		.SetInsets(B_USE_WINDOW_SPACING)
		.Add(grid)
		.Add(note)
		.AddGroup(B_HORIZONTAL)
			.Add(reset)
			.AddGlue()
			.Add(save)
		.End();
	CenterOnScreen();
}


void
SettingsWindow::_Save()
{
	if (fDriver == NULL)
		return;

	// validate everything first, then write
	BStringList newValues;
	for (int32 i = 0; i < fDriver->tunables.CountItems(); i++) {
		const TunableSpec* t = fDriver->tunables.ItemAt(i);
		BControl* control = fControls.ItemAt(i);
		BString value;
		if (BCheckBox* box = dynamic_cast<BCheckBox*>(control))
			value = box->Value() == B_CONTROL_ON ? "1" : "0";
		else if (BTextControl* text = dynamic_cast<BTextControl*>(control))
			value = text->Text();
		value.Trim();

		BString error;
		if (t->risk != kRiskUnsafe && !t->Validate(value.String(), error)) {
			BString text;
			text << t->label << ": " << error;
			(new BAlert(Title(), text.String(), B_TRANSLATE("OK"), NULL,
				NULL, B_WIDTH_AS_USUAL, B_STOP_ALERT))->Go();
			return;
		}
		newValues.Add(value);
	}

	BStringList keys, values;
	SettingsStore::Read(*fDriver, keys, values);
	for (int32 i = 0; i < fDriver->tunables.CountItems(); i++) {
		const TunableSpec* t = fDriver->tunables.ItemAt(i);
		if (t->risk == kRiskUnsafe)
			continue;
		const BString& value = newValues.StringAt(i);
		BString defaultValue;
		defaultValue << t->defaultValue;
		bool isDefault = t->type == "bool"
			? is_on(value) == (t->defaultValue != 0) : value == defaultValue;
		bool wasSet = keys.HasString(t->key);
		BString error;
		status_t status = B_OK;
		if (isDefault && wasSet)
			status = SettingsStore::Unset(*fDriver, t->key.String(), error);
		else if (!isDefault)
			status = SettingsStore::Set(*fDriver, t->key.String(),
				value.String(), error);
		if (status != B_OK) {
			(new BAlert(Title(), error.String(), B_TRANSLATE("OK"), NULL,
				NULL, B_WIDTH_AS_USUAL, B_STOP_ALERT))->Go();
			return;
		}
	}
	fTarget.SendMessage(kMsgSettingsChanged);
	Quit();
}


void
SettingsWindow::_Reset()
{
	if (fDriver == NULL)
		return;
	BAlert* alert = new BAlert(Title(), B_TRANSLATE(
		"Return all settings of this driver to their defaults?"),
		B_TRANSLATE("Cancel"), B_TRANSLATE("Defaults"), NULL,
		B_WIDTH_AS_USUAL, B_WARNING_ALERT);
	alert->SetShortcut(0, B_ESCAPE);
	if (alert->Go() != 1)
		return;
	for (int32 i = 0; i < fDriver->tunables.CountItems(); i++) {
		const TunableSpec* t = fDriver->tunables.ItemAt(i);
		if (t->risk == kRiskUnsafe)
			continue;	// never touched from here (docs/MANAGER.md §6)
		BString error;
		SettingsStore::Unset(*fDriver, t->key.String(), error);
	}
	fTarget.SendMessage(kMsgSettingsChanged);
	Quit();
}


void
SettingsWindow::MessageReceived(BMessage* message)
{
	switch (message->what) {
		case kMsgSave:
			_Save();
			break;
		case kMsgReset:
			_Reset();
			break;
		default:
			BWindow::MessageReceived(message);
	}
}
