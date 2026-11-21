/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 */
#ifndef MAPPING_VIEW_H
#define MAPPING_VIEW_H


/*!	Batch 11 (roadmap F-42, configurator polish): the Mapping tab of the
	Gamepads preferences, gamepads as keyboard and mouse (JoyToKey-style).
	It writes the settings the <input>gamepad_keys input_server add-on reads
	(see its source for the format): the master switch, then per profile
	("All applications" or one running application) a key for each button,
	what the D-pad and the sticks do, and the pointer speed. Off by
	default; guide and start held for 3 seconds switch it off again.
	Not done yet: tap/long-press/double-tap, chords, turbo, layers, macros,
	gyro, pad-to-pad remapping, radial menus.
*/


#include "GamepadsCommon.h"

#include <GridView.h>
#include <ScreenFit.h>
#include <Roster.h>


struct mapping_key {
	int32		code;
	const char*	name;
};

// raw key codes (US layout positions)
static const mapping_key kMappingKeys[] = {
	{ 0, "None" }, { 0x5e, "Space" }, { 0x47, "Enter" }, { 0x01, "Escape" },
	{ 0x26, "Tab" }, { 0x1e, "Backspace" }, { 0x4b, "Shift" },
	{ 0x5c, "Control" }, { 0x5d, "Alt" }, { 0x57, "Up" }, { 0x62, "Down" },
	{ 0x61, "Left" }, { 0x63, "Right" }, { 0x3c, "A" }, { 0x50, "B" },
	{ 0x4e, "C" }, { 0x3e, "D" }, { 0x29, "E" }, { 0x3f, "F" },
	{ 0x27, "Q" }, { 0x2a, "R" }, { 0x3d, "S" }, { 0x28, "W" },
	{ 0x4d, "X" }, { 0x4c, "Z" }, { 0x12, "1" }, { 0x13, "2" },
	{ 0x14, "3" }, { 0x15, "4" }, { 0x02, "F1" }, { 0x06, "F5" },
	{ 0x0a, "F9" }, { 0x0d, "F12" }, { 0x21, "Home" }, { 0x36, "End" },
	{ 0x22, "Page Up" }, { 0x37, "Page Down" }
};

static const char* const kMappingButtons[][2] = {
	{ "a", "A (south)" }, { "b", "B (east)" }, { "x", "X (west)" },
	{ "y", "Y (north)" }, { "lb", "Left shoulder" },
	{ "rb", "Right shoulder" }, { "lt", "Left trigger" },
	{ "rt", "Right trigger" }, { "back", "Back" }, { "start", "Start" },
	{ "l3", "Left stick click" }, { "r3", "Right stick click" }
};
static const int kMappingButtonCount = 12;

enum {
	MSG_MAPPING_ENABLE	= 'mpEn',
	MSG_MAPPING_PROFILE	= 'mpPr',
	MSG_MAPPING_CHANGED	= 'mpCh'
};


class MappingView : public BGroupView {
public:
	MappingView()
		:
		BGroupView("Mapping", B_VERTICAL, B_USE_SMALL_SPACING)
	{
		fEnable = new BCheckBox("mapping_enable",
			"Use gamepads as keyboard and mouse",
			new BMessage(MSG_MAPPING_ENABLE));
		fProfile = new BMenuField("mapping_profile", "Settings for:",
			new BPopUpMenu("profile"));

		BGridView* gridView = new BGridView(B_USE_SMALL_SPACING, 2);
		BGridLayout* layout = gridView->GridLayout();
		for (int i = 0; i < kMappingButtonCount; i++) {
			BPopUpMenu* menu = new BPopUpMenu("key");
			for (size_t k = 0; k < sizeof(kMappingKeys)
					/ sizeof(kMappingKeys[0]); k++) {
				BMessage* message = new BMessage(MSG_MAPPING_CHANGED);
				message->AddInt32("key", kMappingKeys[k].code);
				menu->AddItem(new BMenuItem(kMappingKeys[k].name, message));
			}
			BString name("map:");
			name << kMappingButtons[i][0];
			fKeys[i] = new BMenuField(name, kMappingButtons[i][1], menu);
			layout->AddItem(fKeys[i]->CreateLabelLayoutItem(), (i % 2) * 2,
				i / 2);
			layout->AddItem(fKeys[i]->CreateMenuBarLayoutItem(),
				(i % 2) * 2 + 1, i / 2);
		}
		fDpad = _UseMenu("map_dpad", "D-pad:", "arrows,wasd,none");
		fLeft = _UseMenu("map_left_stick", "Left stick:",
			"none,arrows,wasd,mouse,scroll");
		fRight = _UseMenu("map_right_stick", "Right stick:",
			"none,mouse,scroll");
		fSpeed = new BSlider("map_speed", "Pointer speed",
			new BMessage(MSG_MAPPING_CHANGED), 1, 20, B_HORIZONTAL);
		fSpeed->SetModificationMessage(new BMessage(MSG_MAPPING_CHANGED));
		fMouseButtons = new BCheckBox("map_mouse_buttons",
			"A and B click while a stick moves the pointer",
			new BMessage(MSG_MAPPING_CHANGED));
		// batch 12: wraps (as one line it made the window 1051 pixels
		// wide at a 20-point font)
		BView* note = new BPrivate::WrappingNote("map_note", "Hold guide and "
			"start for 3 seconds to switch mapping off. Turbo and macros can "
			"break some online games' rules.", B_LIGHTEN_1_TINT);

		BLayoutBuilder::Group<>(this)
			.SetInsets(B_USE_WINDOW_SPACING)
			.Add(fEnable)
			.Add(fProfile)
			.Add(gridView)
			.AddGroup(B_HORIZONTAL)
				.Add(fDpad)
				.Add(fLeft)
				.Add(fRight)
			.End()
			.Add(fSpeed)
			.Add(fMouseButtons)
			.Add(note)
			.AddGlue();
	}

	virtual void AttachedToWindow()
	{
		BGroupView::AttachedToWindow();
		fEnable->SetTarget(this);
		fSpeed->SetTarget(this);
		fMouseButtons->SetTarget(this);
		for (int i = 0; i < kMappingButtonCount; i++)
			fKeys[i]->Menu()->SetTargetForItems(this);
		fDpad->Menu()->SetTargetForItems(this);
		fLeft->Menu()->SetTargetForItems(this);
		fRight->Menu()->SetTargetForItems(this);
		_FillProfiles();
		_Load();
	}

	virtual void MessageReceived(BMessage* message)
	{
		switch (message->what) {
			case MSG_MAPPING_ENABLE:
				_SetEnabled(fEnable->Value() == B_CONTROL_ON);
				break;
			case MSG_MAPPING_PROFILE:
				fSignature = message->GetString("signature", "");
				_Load();
				break;
			case MSG_MAPPING_CHANGED:
				_Save();
				break;
			default:
				BGroupView::MessageReceived(message);
		}
	}

	static status_t MainPath(BPath& path)
	{
		status_t status = find_directory(B_USER_SETTINGS_DIRECTORY, &path);
		if (status == B_OK)
			status = path.Append("Gamepads/key_mapping");
		return status;
	}

	static status_t ProfilePath(const char* signature, BPath& path)
	{
		if (signature == NULL || signature[0] == '\0')
			return MainPath(path);
		status_t status = find_directory(B_USER_SETTINGS_DIRECTORY, &path);
		BString leaf(signature);
		leaf.ReplaceAll('/', '_');
		if (status == B_OK)
			status = path.Append("Gamepads/profiles");
		if (status == B_OK)
			status = path.Append(leaf.String());
		return status;
	}

	static bool ReadEnabled()
	{
		BPath path;
		if (MainPath(path) != B_OK)
			return false;
		FILE* file = fopen(path.Path(), "r");
		bool enabled = false;
		if (file != NULL) {
			char line[128];
			int value;
			while (fgets(line, sizeof(line), file) != NULL) {
				if (sscanf(line, "enabled %d", &value) == 1)
					enabled = value != 0;
			}
			fclose(file);
		}
		return enabled;
	}

	//!	For the command line and the harness: the master switch.
	static status_t SetEnabledSetting(bool enabled)
	{
		BPath path;
		status_t status = MainPath(path);
		if (status != B_OK)
			return status;
		BString text;
		text << "enabled " << (enabled ? 1 : 0) << "\n";
		FILE* file = fopen(path.Path(), "r");
		if (file != NULL) {
			char line[128];
			while (fgets(line, sizeof(line), file) != NULL) {
				if (strncmp(line, "enabled ", 8) != 0)
					text << line;
			}
			fclose(file);
		}
		BPath parent;
		path.GetParent(&parent);
		create_directory(parent.Path(), 0755);
		file = fopen(path.Path(), "w");
		if (file == NULL)
			return errno;
		fputs(text.String(), file);
		fclose(file);
		return B_OK;
	}

private:
	BMenuField* _UseMenu(const char* name, const char* label,
		const char* choices)
	{
		BPopUpMenu* menu = new BPopUpMenu("use");
		BStringList list;
		BString(choices).Split(",", true, list);
		for (int32 i = 0; i < list.CountStrings(); i++) {
			BMessage* message = new BMessage(MSG_MAPPING_CHANGED);
			message->AddString("use", list.StringAt(i));
			menu->AddItem(new BMenuItem(list.StringAt(i), message));
		}
		return new BMenuField(name, label, menu);
	}

	void _FillProfiles()
	{
		BMenu* menu = fProfile->Menu();
		while (BMenuItem* item = menu->RemoveItem((int32)0))
			delete item;
		BMessage* all = new BMessage(MSG_MAPPING_PROFILE);
		all->AddString("signature", "");
		BMenuItem* allItem = new BMenuItem("All applications", all);
		menu->AddItem(allItem);
		BList teams;
		be_roster->GetAppList(&teams);
		BStringList seen;
		for (int32 i = 0; i < teams.CountItems(); i++) {
			app_info info;
			if (be_roster->GetRunningAppInfo((team_id)(addr_t)teams.ItemAt(i),
					&info) != B_OK
				|| (info.flags & B_BACKGROUND_APP) != 0
				|| seen.HasString(info.signature)
				|| strcmp(info.signature, kAppSignatureMapping) == 0) {
				continue;
			}
			seen.Add(info.signature);
			BMessage* message = new BMessage(MSG_MAPPING_PROFILE);
			message->AddString("signature", info.signature);
			menu->AddItem(new BMenuItem(info.ref.name, message));
		}
		menu->SetTargetForItems(this);
		allItem->SetMarked(true);
	}

	static void _Mark(BMenuField* field, const char* label)
	{
		if (BMenuItem* item = field->Menu()->FindItem(label))
			item->SetMarked(true);
	}

	void _Load()
	{
		fEnable->SetValue(ReadEnabled());
		BPath path;
		ProfilePath(fSignature, path);
		BString text;
		FILE* file = fopen(path.Path(), "r");
		if (file == NULL && !fSignature.IsEmpty()) {
			// a new profile starts from the one for all applications
			MainPath(path);
			file = fopen(path.Path(), "r");
		}
		int32 keys[kMappingButtonCount] = {};
		BString dpad("arrows"), left("none"), right("none");
		int speed = 8, mouseButtons = 0;
		if (file != NULL) {
			char line[128];
			while (fgets(line, sizeof(line), file) != NULL) {
				char word[32], value[32];
				int number;
				if (sscanf(line, "button %31s %i", word, &number) == 2) {
					for (int i = 0; i < kMappingButtonCount; i++) {
						if (strcmp(word, kMappingButtons[i][0]) == 0)
							keys[i] = number;
					}
				} else if (sscanf(line, "dpad %31s", value) == 1)
					dpad = value;
				else if (sscanf(line, "left_stick %31s", value) == 1)
					left = value;
				else if (sscanf(line, "right_stick %31s", value) == 1)
					right = value;
				else if (sscanf(line, "speed %d", &number) == 1)
					speed = number;
				else if (sscanf(line, "mouse_buttons %d", &number) == 1)
					mouseButtons = number;
			}
			fclose(file);
		}
		for (int i = 0; i < kMappingButtonCount; i++) {
			for (size_t k = 0; k < sizeof(kMappingKeys)
					/ sizeof(kMappingKeys[0]); k++) {
				if (kMappingKeys[k].code == keys[i]) {
					fKeys[i]->Menu()->ItemAt(k)->SetMarked(true);
					break;
				}
			}
		}
		_Mark(fDpad, dpad);
		_Mark(fLeft, left);
		_Mark(fRight, right);
		fSpeed->SetValue(speed);
		fMouseButtons->SetValue(mouseButtons);
	}

	static BString _Marked(BMenuField* field, const char* fallback)
	{
		BMenuItem* item = field->Menu()->FindMarked();
		return item != NULL ? BString(item->Label()) : BString(fallback);
	}

	void _Save()
	{
		BString text;
		if (fSignature.IsEmpty())
			text << "enabled " << (ReadEnabled() ? 1 : 0) << "\n";
		for (int i = 0; i < kMappingButtonCount; i++) {
			BMenuItem* item = fKeys[i]->Menu()->FindMarked();
			int32 key = item != NULL && item->Message() != NULL
				? item->Message()->GetInt32("key", 0) : 0;
			text << "button " << kMappingButtons[i][0] << " " << key << "\n";
		}
		text << "dpad " << _Marked(fDpad, "arrows") << "\n"
			<< "left_stick " << _Marked(fLeft, "none") << "\n"
			<< "right_stick " << _Marked(fRight, "none") << "\n"
			<< "speed " << fSpeed->Value() << "\n"
			<< "mouse_buttons " << (fMouseButtons->Value() ? 1 : 0) << "\n";
		BPath path;
		if (ProfilePath(fSignature, path) != B_OK)
			return;
		BPath parent;
		path.GetParent(&parent);
		create_directory(parent.Path(), 0755);
		FILE* file = fopen(path.Path(), "w");
		if (file != NULL) {
			fputs(text.String(), file);
			fclose(file);
		}
	}

	void _SetEnabled(bool enabled)
	{
		BPath path;
		if (MainPath(path) == B_OK && access(path.Path(), F_OK) != 0) {
			// first use: write the profile as shown
			BString keep = fSignature;
			fSignature = "";
			_Save();
			fSignature = keep;
		}
		SetEnabledSetting(enabled);
	}

	static constexpr const char* kAppSignatureMapping
		= "application/x-vnd.Renga-Gamepads";

	BCheckBox*		fEnable;
	BMenuField*		fProfile;
	BMenuField*		fKeys[kMappingButtonCount];
	BMenuField*		fDpad;
	BMenuField*		fLeft;
	BMenuField*		fRight;
	BSlider*		fSpeed;
	BCheckBox*		fMouseButtons;
	BString			fSignature;
};


#endif	// MAPPING_VIEW_H
