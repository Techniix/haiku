/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 *
 * Gamepads (roadmap F-42): the preferences for game controllers. The
 * small background agent behind two of its settings is a separate server,
 * gamepad_agent (GamepadAgent.cpp); "Gamepads --agent" starts it.
 *
 * Preferences window: the connected pads (name, player number, battery,
 * mapping), a live tester drawing the standard layout as the pad reports
 * it, a rumble test, the player number (and so the pad's LEDs), and the
 * settings:
 *  - Guide button overlay: holding the guide button (half a second) shows
 *    the pad's name, player number and battery; a short press still goes
 *    to the game. On by default.
 *  - Low battery notification: once per pad when its battery falls below
 *    15 % while not charging. On by default.
 *  - Raw controller access for games (hidraw): SDL's HIDAPI drivers use it
 *    for gyro, HD rumble, lightbar and adaptive triggers; only game
 *    controllers ever get raw nodes. On by default; takes effect for pads
 *    connected after the change.
 *
 * For scripts and the harness: Gamepads --get <setting>,
 * Gamepads --set <setting> on|off (settings: overlay, low_battery,
 * raw_access).
 */

#include "GamepadsCommon.h"

#include <Roster.h>


static const char* kAppSignature = "application/x-vnd.Renga-Gamepads";




// #pragma mark - tester view


class TesterView : public BView {
public:
	TesterView()
		:
		BView("tester", B_WILL_DRAW | B_FULL_UPDATE_ON_RESIZE)
	{
		memset(&fState, 0, sizeof(fState));
		SetExplicitMinSize(BSize(360, 220));
	}

	void SetState(const gamepad_state& state, bool valid)
	{
		if (valid == fValid
			&& memcmp(&state, &fState, sizeof(state)) == 0)
			return;
		fState = state;
		fValid = valid;
		Invalidate();
	}

	virtual void Draw(BRect)
	{
		BRect bounds = Bounds();
		rgb_color base = ui_color(B_PANEL_BACKGROUND_COLOR);
		rgb_color text = ui_color(B_PANEL_TEXT_COLOR);
		rgb_color accent = (rgb_color){ 240, 128, 40, 255 };
		rgb_color body = tint_color(base, B_DARKEN_2_TINT);
		rgb_color idle = tint_color(base, B_DARKEN_1_TINT);

		SetHighColor(base);
		FillRect(bounds);
		if (!fValid) {
			SetHighColor(text);
			const char* message = "Connect a gamepad to test it.";
			DrawString(message, BPoint(bounds.Width() / 2
				- StringWidth(message) / 2, bounds.Height() / 2));
			return;
		}

		float w = bounds.Width(), h = bounds.Height();
		float scale = std::min(w / 360, h / 220);
		BPoint origin((w - 360 * scale) / 2, (h - 220 * scale) / 2);
#define P(px, py) BPoint(origin.x + (px) * scale, origin.y + (py) * scale)
#define R(l, t, r, b) BRect(P(l, t), P(r, b))

		// body
		SetHighColor(body);
		FillRoundRect(R(40, 50, 320, 170), 50 * scale, 50 * scale);
		FillEllipse(R(30, 100, 130, 215));
		FillEllipse(R(230, 100, 330, 215));

		// shoulders and triggers
		_Button(R(60, 30, 130, 45), B_GAMEPAD_BUTTON_LEFT_SHOULDER, accent,
			idle);
		_Button(R(230, 30, 300, 45), B_GAMEPAD_BUTTON_RIGHT_SHOULDER, accent,
			idle);
		_Trigger(R(60, 5, 130, 25), fState.axes[B_GAMEPAD_AXIS_LEFT_TRIGGER],
			accent, idle);
		_Trigger(R(230, 5, 300, 25),
			fState.axes[B_GAMEPAD_AXIS_RIGHT_TRIGGER], accent, idle);

		// D-pad
		_Button(R(80, 85, 100, 105), B_GAMEPAD_BUTTON_DPAD_UP, accent, idle);
		_Button(R(80, 125, 100, 145), B_GAMEPAD_BUTTON_DPAD_DOWN, accent,
			idle);
		_Button(R(60, 105, 80, 125), B_GAMEPAD_BUTTON_DPAD_LEFT, accent,
			idle);
		_Button(R(100, 105, 120, 125), B_GAMEPAD_BUTTON_DPAD_RIGHT, accent,
			idle);

		// face buttons, by position
		_Round(P(270, 135), 11 * scale, B_GAMEPAD_BUTTON_A, "A", accent, idle);
		_Round(P(292, 113), 11 * scale, B_GAMEPAD_BUTTON_B, "B", accent, idle);
		_Round(P(248, 113), 11 * scale, B_GAMEPAD_BUTTON_X, "X", accent, idle);
		_Round(P(270, 91), 11 * scale, B_GAMEPAD_BUTTON_Y, "Y", accent, idle);

		// back, guide, start
		_Round(P(150, 85), 7 * scale, B_GAMEPAD_BUTTON_BACK, NULL, accent,
			idle);
		_Round(P(180, 80), 10 * scale, B_GAMEPAD_BUTTON_GUIDE, NULL, accent,
			idle);
		_Round(P(210, 85), 7 * scale, B_GAMEPAD_BUTTON_START, NULL, accent,
			idle);

		// sticks
		_Stick(P(135, 150), 22 * scale, fState.axes[B_GAMEPAD_AXIS_LEFT_X],
			fState.axes[B_GAMEPAD_AXIS_LEFT_Y], B_GAMEPAD_BUTTON_LEFT_STICK,
			accent, idle);
		_Stick(P(225, 150), 22 * scale, fState.axes[B_GAMEPAD_AXIS_RIGHT_X],
			fState.axes[B_GAMEPAD_AXIS_RIGHT_Y], B_GAMEPAD_BUTTON_RIGHT_STICK,
			accent, idle);
#undef P
#undef R
	}

private:
	bool _Pressed(int button) const
	{
		return (fState.buttons & (1UL << button)) != 0;
	}

	void _Button(BRect frame, int button, rgb_color on, rgb_color off)
	{
		SetHighColor(_Pressed(button) ? on : off);
		FillRoundRect(frame, 3, 3);
	}

	void _Round(BPoint center, float radius, int button, const char* label,
		rgb_color on, rgb_color off)
	{
		SetHighColor(_Pressed(button) ? on : off);
		FillEllipse(center, radius, radius);
		if (label != NULL) {
			SetHighColor(ui_color(B_PANEL_TEXT_COLOR));
			SetLowColor(_Pressed(button) ? on : off);
			DrawString(label, BPoint(center.x - StringWidth(label) / 2,
				center.y + 4));
		}
	}

	void _Trigger(BRect frame, int16 value, rgb_color on, rgb_color off)
	{
		SetHighColor(off);
		FillRect(frame);
		BRect filled = frame;
		filled.right = frame.left + frame.Width() * (value / 32767.0f);
		SetHighColor(on);
		if (value > 0)
			FillRect(filled);
	}

	void _Stick(BPoint center, float radius, int16 x, int16 y, int button,
		rgb_color on, rgb_color off)
	{
		SetHighColor(tint_color(off, B_DARKEN_1_TINT));
		FillEllipse(center, radius, radius);
		BPoint knob(center.x + x / 32768.0f * radius * 0.6f,
			center.y + y / 32768.0f * radius * 0.6f);
		SetHighColor(_Pressed(button) ? on : off);
		FillEllipse(knob, radius * 0.45f, radius * 0.45f);
	}

	gamepad_state	fState;
	bool			fValid = false;
};


// #pragma mark - preferences window


class GamepadItem : public BStringItem {
public:
	GamepadItem(const char* path, const char* name)
		:
		BStringItem(name),
		fPath(path)
	{
	}

	BString	fPath;
};


class GamepadsWindow : public BWindow {
public:
	GamepadsWindow()
		:
		BWindow(BRect(100, 100, 700, 480), "Gamepads", B_TITLED_WINDOW,
			B_ASYNCHRONOUS_CONTROLS | B_AUTO_UPDATE_SIZE_LIMITS
				| B_QUIT_ON_WINDOW_CLOSE),
		fRunner(NULL)
	{
		fSettings.Load();

		fList = new BListView("pads");
		fList->SetSelectionMessage(new BMessage(MSG_SELECT));
		BScrollView* scroll = new BScrollView("pads scroll", fList, 0, false,
			true);
		scroll->SetExplicitMinSize(BSize(190, 150));

		fTester = new TesterView;
		fInfo = new BStringView("info", "");
		fBattery = new BStringView("battery", "");
		fMapping = new BStringView("mapping", "");
		fMapping->SetFont(be_plain_font);
		fMapping->SetTruncation(B_TRUNCATE_END);

		BPopUpMenu* players = new BPopUpMenu("player");
		for (int32 i = 1; i <= 8; i++) {
			BString label;
			label << i;
			BMessage* message = new BMessage(MSG_PLAYER);
			message->AddInt32("player", i);
			players->AddItem(new BMenuItem(label, message));
		}
		fPlayer = new BMenuField("player", "Player:", players);
		fRumble = new BButton("rumble", "Test rumble",
			new BMessage(MSG_RUMBLE));

		fOverlay = _CheckBox("overlay",
			"Show the gamepad overlay when the guide button is held");
		fLowBattery = _CheckBox("low_battery",
			"Notify when a gamepad's battery is low");
		fRawAccess = _CheckBox("raw_access",
			"Raw controller access for games (gyro, HD rumble, lightbar)");
		fDefaults = new BButton("defaults", "Defaults",
			new BMessage(MSG_DEFAULTS));

		BLayoutBuilder::Group<>(this, B_VERTICAL)
			.SetInsets(B_USE_WINDOW_SPACING)
			.AddGroup(B_HORIZONTAL)
				.Add(scroll, 1)
				.AddGroup(B_VERTICAL, B_USE_SMALL_SPACING, 3)
					.Add(fTester, 1)
					.Add(fInfo)
					.Add(fBattery)
					.Add(fMapping)
					.AddGroup(B_HORIZONTAL)
						.Add(fPlayer)
						.Add(fRumble)
						.AddGlue()
					.End()
				.End()
			.End()
			.Add(new BSeparatorView(B_HORIZONTAL))
			.Add(fOverlay)
			.Add(fLowBattery)
			.Add(fRawAccess)
			.AddGroup(B_HORIZONTAL)
				.Add(fDefaults)
				.AddGlue()
			.End();

		_UpdateCheckBoxes();
		_Rescan();
		BGamepad::StartWatching(BMessenger(this));
		CenterOnScreen();
	}

	virtual ~GamepadsWindow()
	{
		BGamepad::StopWatching(BMessenger(this));
		delete fRunner;
	}

	virtual void MessageReceived(BMessage* message)
	{
		switch (message->what) {
			case B_GAMEPAD_ADDED:
			case B_GAMEPAD_REMOVED:
				_Rescan();
				break;

			case MSG_SELECT:
				_Select();
				break;

			case MSG_TICK:
				_Tick();
				break;

			case MSG_RUMBLE:
				if (fGamepad.InitCheck() == B_OK)
					fGamepad.Rumble(0xc000, 0xc000, 600000);
				break;

			case MSG_PLAYER:
			{
				int32 player;
				if (message->FindInt32("player", &player) == B_OK
					&& fGamepad.InitCheck() == B_OK)
					fGamepad.SetPlayer(player);
				_UpdateInfo();
				break;
			}

			case MSG_SETTING:
			{
				const char* name;
				int32 value;
				if (message->FindString("setting", &name) == B_OK
					&& message->FindInt32("be:value", &value) == B_OK) {
					bool* setting = fSettings.Find(name);
					if (setting != NULL) {
						*setting = value == B_CONTROL_ON;
						fSettings.Save();
						_UpdateCheckBoxes();
					}
				}
				break;
			}

			case MSG_DEFAULTS:
				fSettings.Defaults();
				fSettings.Save();
				_UpdateCheckBoxes();
				break;

			default:
				BWindow::MessageReceived(message);
		}
	}

private:
	BCheckBox* _CheckBox(const char* setting, const char* label)
	{
		BMessage* message = new BMessage(MSG_SETTING);
		message->AddString("setting", setting);
		return new BCheckBox(setting, label, message);
	}

	void _UpdateCheckBoxes()
	{
		fOverlay->SetValue(fSettings.overlay);
		fLowBattery->SetValue(fSettings.lowBattery);
		fRawAccess->SetValue(fSettings.rawAccess);
		Settings defaults;
		fDefaults->SetEnabled(fSettings.overlay != defaults.overlay
			|| fSettings.lowBattery != defaults.lowBattery
			|| fSettings.rawAccess != defaults.rawAccess);
	}

	void _Rescan()
	{
		BString selected;
		GamepadItem* current
			= (GamepadItem*)fList->ItemAt(fList->CurrentSelection());
		if (current != NULL)
			selected = current->fPath;

		while (BListItem* item = fList->RemoveItem((int32)0))
			delete item;
		int32 count = BGamepad::CountGamepads();
		int32 select = 0;
		for (int32 i = 0; i < count; i++) {
			BString path;
			if (BGamepad::GetGamepadPath(i, &path) != B_OK)
				continue;
			BGamepad pad;
			if (pad.Open(path.String()) != B_OK)
				continue;
			BString label;
			label << pad.Name();
			int32 player = pad.Player();
			if (player > 0)
				label << " (" << player << ")";
			fList->AddItem(new GamepadItem(path, label));
			if (path == selected)
				select = fList->CountItems() - 1;
		}
		if (fList->CountItems() > 0)
			fList->Select(select);
		else
			_Select();
	}

	void _Select()
	{
		GamepadItem* item
			= (GamepadItem*)fList->ItemAt(fList->CurrentSelection());
		fGamepad.Close();
		if (item != NULL)
			fGamepad.Open(item->fPath.String());

		bool valid = fGamepad.InitCheck() == B_OK;
		fRumble->SetEnabled(valid && fGamepad.CanRumble());
		fPlayer->SetEnabled(valid);
		_UpdateInfo();

		delete fRunner;
		fRunner = NULL;
		if (valid) {
			// 60 updates a second only while a pad is shown
			BMessage tick(MSG_TICK);
			fRunner = new BMessageRunner(BMessenger(this), &tick, 16666);
		}
		_Tick();
	}

	void _UpdateInfo()
	{
		if (fGamepad.InitCheck() != B_OK) {
			fInfo->SetText("");
			fBattery->SetText("");
			fMapping->SetText("");
			return;
		}
		BString info;
		info.SetToFormat("%s (%04x:%04x)", fGamepad.Name(),
			fGamepad.VendorID(), fGamepad.ProductID());
		fInfo->SetText(info);

		int32 level = -1;
		bool charging = false, wired = false;
		fGamepad.GetBattery(&level, &charging, &wired);
		BString battery;
		if (wired)
			battery = "Battery: powered by the cable";
		else if (level < 0)
			battery = "Battery: not reported";
		else {
			battery.SetToFormat("Battery: %d %%%s", (int)level,
				charging ? ", charging" : "");
		}
		fBattery->SetText(battery);

		BString mapping = "Layout: ";
		const char* line = fGamepad.Mapping();
		if (line == NULL)
			mapping << "no standard layout known";
		else if (strstr(line, ",default") != NULL)
			mapping << "standard gamepad codes";
		else
			mapping << "SDL mapping database";
		fMapping->SetText(mapping);

		int32 player = fGamepad.Player();
		BMenuItem* item = fPlayer->Menu()->ItemAt(player - 1);
		if (item != NULL)
			item->SetMarked(true);
	}

	void _Tick()
	{
		gamepad_state state;
		memset(&state, 0, sizeof(state));
		bool valid = fGamepad.InitCheck() == B_OK
			&& fGamepad.Update() == B_OK
			&& fGamepad.GetState(&state) == B_OK;
		fTester->SetState(state, valid);
	}

	Settings			fSettings;
	BListView*			fList;
	TesterView*			fTester;
	BStringView*		fInfo;
	BStringView*		fBattery;
	BStringView*		fMapping;
	BMenuField*			fPlayer;
	BButton*			fRumble;
	BCheckBox*			fOverlay;
	BCheckBox*			fLowBattery;
	BCheckBox*			fRawAccess;
	BButton*			fDefaults;
	BGamepad			fGamepad;
	BMessageRunner*		fRunner;
};




// #pragma mark - main


class GamepadsApp : public BApplication {
public:
	GamepadsApp()
		:
		BApplication(kAppSignature)
	{
	}

	virtual void ReadyToRun()
	{
		(new GamepadsWindow)->Show();
	}
};


static int
command_line(int argc, char** argv)
{
	Settings settings;
	settings.Load();
	if (argc == 3 && strcmp(argv[1], "--get") == 0) {
		bool* value = settings.Find(argv[2]);
		if (value == NULL)
			return 2;
		printf("%s\n", *value ? "on" : "off");
		return 0;
	}
	if (argc == 4 && strcmp(argv[1], "--set") == 0) {
		bool* value = settings.Find(argv[2]);
		if (value == NULL
			|| (strcmp(argv[3], "on") != 0 && strcmp(argv[3], "off") != 0))
			return 2;
		*value = strcmp(argv[3], "on") == 0;
		if (settings.Save() != B_OK)
			return 1;
		return 0;
	}
	fprintf(stderr, "usage: %s [--agent | --get <setting> | "
		"--set <setting> on|off]\n"
		"settings: overlay, low_battery, raw_access\n", argv[0]);
	return 2;
}


int
main(int argc, char** argv)
{
	if (argc >= 2 && strcmp(argv[1], "--agent") == 0) {
		// Renga: the agent is its own background server now
		// (GamepadAgent.cpp); this starts it, for old launch scripts
		status_t status = be_roster->Launch(
			"application/x-vnd.Renga-GamepadAgent");
		return status == B_OK || status == B_ALREADY_RUNNING ? 0 : 1;
	}
	if (argc >= 2)
		return command_line(argc, argv);

	GamepadsApp app;
	app.Run();
	return 0;
}
