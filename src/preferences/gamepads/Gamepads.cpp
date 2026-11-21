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
 *  - Connect notification (gamepad_agent): a pad's name, player number,
 *    battery and whether its layout is known when it is plugged in, and a
 *    short notice when it goes. On by default.
 *
 * Per pad (configurator polish, roadmap F-42; stored by the pad's GUID):
 *  - Tuning, applied by BGamepad for native programs (<GamepadTuning.h>):
 *    stick dead zone, response curve, trigger threshold, A/B and X/Y swap.
 *    All off by default; the tester shows the dead zone as a ring.
 *  - "Map buttons…": a wizard that asks for each control in turn and
 *    saves an SDL mapping line in ~/config/settings/Gamepads/mappings
 *    (MappingLearner.h); "Reset mapping" removes it again.
 *
 * For scripts and the harness: Gamepads --get <setting>,
 * Gamepads --set <setting> on|off (settings: overlay, low_battery,
 * raw_access, connect_notice); Gamepads --learn <device> (the wizard on
 * the command line: prompts on standard output, the line saved at the
 * end); Gamepads --tune <device> <dead zone %> <curve> <trigger %>
 * <swap 0|1> and --get-tuning <device>.
 *
 * Batch 11 (configurator polish): layout families (GamepadsCommon.h:
 * offset, symmetric, Nintendo, retro, generic, arcade) with their own
 * pictures (GamepadLayouts.rdef) and face labels in the tester; the
 * connect notice's actions (Gamepads --connected <device>: Identify,
 * Change player, Settings…, Don't show again for this controller); quiet
 * notices in full screen and streamer mode ("quiet_mode", on). For tests:
 * --identify, --family, --silenced, --unsilence <device>. The Mapping tab
 * (MappingView.h): gamepads as keyboard and mouse through the
 * <input>gamepad_keys input_server add-on, off by default; --mapping
 * on|off|get.
 */

#include "GamepadsCommon.h"
#include "MappingLearner.h"
#include "MappingView.h"

#include <TabView.h>

#include <Roster.h>

#include <GamepadTuning.h>

using BPrivate::GamepadTuning;


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

	~TesterView()
	{
		delete fPicture;
	}

	//!	Batch 11: the pad's layout family: its picture, the stick and
	//	D-pad arrangement and the face labels.
	void SetFamily(layout_family family)
	{
		if (family == fFamily && fPicture != NULL)
			return;
		fFamily = family;
		delete fPicture;
		fPicture = gamepad_layout_icon(family, 48);
		Invalidate();
	}

	layout_family Family() const { return fFamily; }

	void SetDeadZone(int32 percent)
	{
		if (percent != fDeadZone) {
			fDeadZone = percent;
			Invalidate();
		}
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

		// batch 11: the family's picture in the corner
		if (fPicture != NULL) {
			SetDrawingMode(B_OP_ALPHA);
			DrawBitmap(fPicture, BPoint(4, 4));
			SetDrawingMode(B_OP_COPY);
		}
		// offset families have the left stick up and the D-pad below it;
		// arcade sticks have no sticks to draw, the lever is the D-pad
		bool offset = fFamily == LAYOUT_OFFSET || fFamily == LAYOUT_NINTENDO;
		float dx = offset ? 45 : 0, dy = offset ? 45 : 0;

		// D-pad
		_Button(R(80 + dx, 85 + dy, 100 + dx, 105 + dy),
			B_GAMEPAD_BUTTON_DPAD_UP, accent, idle);
		_Button(R(80 + dx, 125 + dy, 100 + dx, 145 + dy),
			B_GAMEPAD_BUTTON_DPAD_DOWN, accent, idle);
		_Button(R(60 + dx, 105 + dy, 80 + dx, 125 + dy),
			B_GAMEPAD_BUTTON_DPAD_LEFT, accent, idle);
		_Button(R(100 + dx, 105 + dy, 120 + dx, 125 + dy),
			B_GAMEPAD_BUTTON_DPAD_RIGHT, accent, idle);

		// face buttons, by position, labelled as the family names them
		const char* const* labels = kFaceLabels[fFamily];
		_Round(P(270, 135), 11 * scale, B_GAMEPAD_BUTTON_A, labels[0], accent,
			idle);
		_Round(P(292, 113), 11 * scale, B_GAMEPAD_BUTTON_B, labels[1], accent,
			idle);
		_Round(P(248, 113), 11 * scale, B_GAMEPAD_BUTTON_X, labels[2], accent,
			idle);
		_Round(P(270, 91), 11 * scale, B_GAMEPAD_BUTTON_Y, labels[3], accent,
			idle);

		// back, guide, start
		_Round(P(150, 85), 7 * scale, B_GAMEPAD_BUTTON_BACK, NULL, accent,
			idle);
		_Round(P(180, 80), 10 * scale, B_GAMEPAD_BUTTON_GUIDE, NULL, accent,
			idle);
		_Round(P(210, 85), 7 * scale, B_GAMEPAD_BUTTON_START, NULL, accent,
			idle);

		// sticks (none on arcade and retro pads)
		bool sticks = fFamily != LAYOUT_ARCADE && fFamily != LAYOUT_RETRO;
		if (sticks) {
			_Stick(offset ? P(90, 115) : P(135, 150), 22 * scale,
				fState.axes[B_GAMEPAD_AXIS_LEFT_X],
				fState.axes[B_GAMEPAD_AXIS_LEFT_Y],
				B_GAMEPAD_BUTTON_LEFT_STICK, accent, idle);
			_Stick(P(225, 150), 22 * scale,
				fState.axes[B_GAMEPAD_AXIS_RIGHT_X],
				fState.axes[B_GAMEPAD_AXIS_RIGHT_Y],
				B_GAMEPAD_BUTTON_RIGHT_STICK, accent, idle);
		}
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
		if (fDeadZone > 0) {
			// the dead zone, at the knob's travel scale
			float zone = radius * 0.6f * fDeadZone / 100.0f;
			SetHighColor(tint_color(off, B_DARKEN_3_TINT));
			StrokeEllipse(center, zone, zone);
		}
		BPoint knob(center.x + x / 32768.0f * radius * 0.6f,
			center.y + y / 32768.0f * radius * 0.6f);
		SetHighColor(_Pressed(button) ? on : off);
		FillEllipse(knob, radius * 0.45f, radius * 0.45f);
	}

	gamepad_state	fState;
	bool			fValid = false;
	int32			fDeadZone = 0;
	layout_family	fFamily = LAYOUT_GENERIC;
	BBitmap*		fPicture = NULL;
};


// #pragma mark - mapping wizard


class MapWindow : public BWindow {
public:
	MapWindow(BWindow* owner, const char* path)
		:
		BWindow(BRect(0, 0, 420, 160), "Map buttons", B_TITLED_WINDOW_LOOK,
			B_FLOATING_APP_WINDOW_FEEL, B_ASYNCHRONOUS_CONTROLS
				| B_AUTO_UPDATE_SIZE_LIMITS | B_NOT_ZOOMABLE
				| B_NOT_RESIZABLE),
		fOwner(owner),
		fRunner(NULL)
	{
		fStatus = new BStringView("step", "");
		fPrompt = new BStringView("prompt", "");
		BFont font(be_bold_font);
		fPrompt->SetFont(&font);
		fSkip = new BButton("skip", "Skip", new BMessage(MSG_MAP_SKIP));
		fBack = new BButton("back", "Back", new BMessage(MSG_MAP_BACK));
		fSave = new BButton("save", "Save", new BMessage(MSG_MAP_SAVE));
		BButton* cancel = new BButton("cancel", "Cancel",
			new BMessage(B_QUIT_REQUESTED));
		BLayoutBuilder::Group<>(this, B_VERTICAL)
			.SetInsets(B_USE_WINDOW_SPACING)
			.Add(fStatus)
			.Add(fPrompt)
			.AddGlue()
			.AddGroup(B_HORIZONTAL)
				.Add(fBack)
				.Add(fSkip)
				.AddGlue()
				.Add(cancel)
				.Add(fSave)
			.End();
		AddToSubset(owner);

		status_t status = fLearner.Open(path);
		if (status != B_OK) {
			BString text;
			text.SetToFormat("Cannot read the gamepad: %s", strerror(status));
			fPrompt->SetText(text);
			fSkip->SetEnabled(false);
			fBack->SetEnabled(false);
			fSave->SetEnabled(false);
		} else {
			BMessage tick(MSG_MAP_TICK);
			fRunner = new BMessageRunner(BMessenger(this), &tick, 30000);
			_Update();
		}
		CenterIn(owner->Frame());
	}

	virtual ~MapWindow()
	{
		delete fRunner;
	}

	virtual void MessageReceived(BMessage* message)
	{
		switch (message->what) {
			case MSG_MAP_TICK:
				if (fLearner.Poll())
					_Update();
				break;
			case MSG_MAP_SKIP:
				fLearner.Skip();
				_Update();
				break;
			case MSG_MAP_BACK:
				fLearner.Back();
				_Update();
				break;
			case MSG_MAP_SAVE:
			{
				status_t status = MappingLearner::Save(fLearner.Line());
				if (status != B_OK) {
					BString text;
					text.SetToFormat("Could not save: %s", strerror(status));
					fPrompt->SetText(text);
					break;
				}
				fOwner->PostMessage(MSG_MAP_DONE);
				Quit();
				break;
			}
			default:
				BWindow::MessageReceived(message);
		}
	}

private:
	void _Update()
	{
		BString status;
		status.SetToFormat("%s — step %d of %d", fLearner.Name(),
			(int)(fLearner.Done() ? kLearnStepCount : fLearner.Step() + 1),
			(int)kLearnStepCount);
		fStatus->SetText(status);
		fPrompt->SetText(fLearner.Done()
			? "Done. Save the mapping, or go back to change a step."
			: fLearner.Prompt());
		fSkip->SetEnabled(!fLearner.Done());
		fBack->SetEnabled(fLearner.Step() > 0);
		fSave->SetEnabled(fLearner.Step() > 0);
	}

	BWindow*			fOwner;
	MappingLearner		fLearner;
	BMessageRunner*		fRunner;
	BStringView*		fStatus;
	BStringView*		fPrompt;
	BButton*			fSkip;
	BButton*			fBack;
	BButton*			fSave;
};


//!	Removes the user's mapping line for \a guid, if there is one.
static status_t
remove_user_mapping(const char* guid)
{
	BPath path;
	status_t status = find_directory(B_USER_SETTINGS_DIRECTORY, &path);
	if (status != B_OK || (status = path.Append("Gamepads/mappings")) != B_OK)
		return status;
	BFile in(path.Path(), B_READ_ONLY);
	off_t size;
	if (in.InitCheck() != B_OK || in.GetSize(&size) != B_OK || size <= 0)
		return B_OK;
	BString contents;
	char* buffer = contents.LockBuffer(size + 1);
	ssize_t bytes = in.Read(buffer, size);
	contents.UnlockBuffer(bytes > 0 ? bytes : 0);
	in.Unset();

	BString kept;
	int32 start = 0;
	while (start < contents.Length()) {
		int32 end = contents.FindFirst('\n', start);
		if (end < 0)
			end = contents.Length();
		BString line(contents.String() + start, end - start);
		if (!line.IsEmpty() && strncasecmp(line.String(), guid, 32) != 0)
			kept << line << "\n";
		start = end + 1;
	}
	BFile out(path.Path(), B_WRITE_ONLY | B_CREATE_FILE | B_ERASE_FILE);
	if ((status = out.InitCheck()) != B_OK)
		return status;
	return out.Write(kept.String(), kept.Length()) == kept.Length()
		? B_OK : B_IO_ERROR;
}


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
		fConnectNotice = _CheckBox("connect_notice",
			"Notify when a gamepad is connected or disconnected");
		fQuietMode = _CheckBox("quiet_mode",
			"Keep those notices quiet in full screen and streamer mode");

		// per-pad tuning
		fDeadZone = _Slider("dead_zone", "Stick dead zone", 0, 30, "%");
		fTrigger = _Slider("trigger", "Trigger threshold", 0, 50, "%");
		BPopUpMenu* curves = new BPopUpMenu("curve");
		static const struct { const char* label; float value; } kCurves[] = {
			{ "Linear", 1.0f }, { "Gentle (1.5)", 1.5f },
			{ "Strong (2.0)", 2.0f }, { "Very strong (3.0)", 3.0f } };
		for (size_t i = 0; i < sizeof(kCurves) / sizeof(kCurves[0]); i++) {
			BMessage* message = new BMessage(MSG_TUNING);
			message->AddFloat("curve", kCurves[i].value);
			curves->AddItem(new BMenuItem(kCurves[i].label, message));
		}
		fCurve = new BMenuField("curve", "Response curve:", curves);
		fSwap = new BCheckBox("swap", "Swap A/B and X/Y (Nintendo layout)",
			new BMessage(MSG_TUNING));
		fMap = new BButton("map", "Map buttons" B_UTF8_ELLIPSIS,
			new BMessage(MSG_MAP));
		fMapReset = new BButton("map_reset", "Reset mapping",
			new BMessage(MSG_MAP_RESET));
		BBox* tuning = new BBox("tuning");
		tuning->SetLabel("This gamepad");
		BGroupView* tuningGroup = new BGroupView(B_VERTICAL,
			B_USE_SMALL_SPACING);
		BLayoutBuilder::Group<>(tuningGroup)
			.SetInsets(B_USE_DEFAULT_SPACING, B_USE_DEFAULT_SPACING * 1.5,
				B_USE_DEFAULT_SPACING, B_USE_DEFAULT_SPACING)
			.Add(fDeadZone)
			.Add(fTrigger)
			.AddGroup(B_HORIZONTAL)
				.Add(fCurve)
				.AddGlue()
			.End()
			.Add(fSwap)
			.Add(new BStringView("tuning_note", "Applies to native programs "
				"(BGamepad); the mapping also to SDL games."))
			.AddGroup(B_HORIZONTAL)
				.Add(fMap)
				.Add(fMapReset)
				.AddGlue()
			.End();
		tuning->AddChild(tuningGroup);
		fDefaults = new BButton("defaults", "Defaults",
			new BMessage(MSG_DEFAULTS));

		// batch 11: two tabs, the pads and the Mapping tab
		BGroupView* padsTab = new BGroupView("Gamepads", B_VERTICAL);
		BLayoutBuilder::Group<>(padsTab)
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
			.Add(tuning)
			.Add(new BSeparatorView(B_HORIZONTAL))
			.Add(fOverlay)
			.Add(fLowBattery)
			.Add(fConnectNotice)
			.Add(fQuietMode)
			.Add(fRawAccess)
			.AddGroup(B_HORIZONTAL)
				.Add(fDefaults)
				.AddGlue()
			.End();
		BTabView* tabs = new BTabView("tabs", B_WIDTH_FROM_LABEL);
		tabs->SetBorder(B_NO_BORDER);
		tabs->AddTab(padsTab);
		tabs->AddTab(new MappingView);
		BLayoutBuilder::Group<>(this, B_VERTICAL, 0)
			.AddStrut(B_USE_SMALL_SPACING)
			.Add(tabs);

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

			case MSG_TUNING:
			{
				if (fGamepad.InitCheck() != B_OK)
					break;
				float curve;
				if (message->FindFloat("curve", &curve) == B_OK)
					fTuning.curve = curve;
				fTuning.deadZone = fDeadZone->Value();
				fTuning.triggerThreshold = fTrigger->Value();
				fTuning.swapFaceButtons = fSwap->Value() == B_CONTROL_ON;
				fTuning.Clamp();
				GamepadTuning::Save(fGamepad.GUID(), fTuning);
				_Reopen();
				break;
			}

			case MSG_MAP:
				if (fGamepad.InitCheck() == B_OK)
					(new MapWindow(this, fGamepad.Path()))->Show();
				break;

			case MSG_MAP_RESET:
				if (fGamepad.InitCheck() == B_OK) {
					remove_user_mapping(fGamepad.GUID());
					_Reopen();
				}
				break;

			case MSG_MAP_DONE:
				_Reopen();
				break;

			default:
				BWindow::MessageReceived(message);
		}
	}

private:
	BSlider* _Slider(const char* name, const char* label, int32 min,
		int32 max, const char* unit)
	{
		BSlider* slider = new BSlider(name, label, new BMessage(MSG_TUNING),
			min, max, B_HORIZONTAL);
		slider->SetModificationMessage(NULL);
		slider->SetHashMarks(B_HASH_MARKS_BOTTOM);
		slider->SetHashMarkCount(max / 5 + 1);
		BString minLabel, maxLabel;
		minLabel << min << " " << unit;
		maxLabel << max << " " << unit;
		slider->SetLimitLabels(minLabel, maxLabel);
		return slider;
	}

	//!	Opens the selected pad again: new mappings and tuning apply.
	void _Reopen()
	{
		BPrivate::gamepad_reload_mappings();
		BString path = fGamepad.Path();
		fGamepad.Close();
		fGamepad.Open(path.String());
		_UpdateInfo();
		_UpdateTuning();
	}

	void _UpdateTuning()
	{
		bool valid = fGamepad.InitCheck() == B_OK;
		fTuning.SetDefaults();
		if (valid)
			GamepadTuning::Load(fGamepad.GUID(), fTuning);
		fDeadZone->SetValue(fTuning.deadZone);
		fTrigger->SetValue(fTuning.triggerThreshold);
		fSwap->SetValue(fTuning.swapFaceButtons);
		int32 curveIndex = fTuning.curve >= 3.0f ? 3
			: fTuning.curve >= 2.0f ? 2 : fTuning.curve >= 1.5f ? 1 : 0;
		BMenuItem* item = fCurve->Menu()->ItemAt(curveIndex);
		if (item != NULL)
			item->SetMarked(true);
		fTester->SetDeadZone(fTuning.deadZone);
		fDeadZone->SetEnabled(valid);
		fTrigger->SetEnabled(valid);
		fCurve->SetEnabled(valid);
		fSwap->SetEnabled(valid);
		fMap->SetEnabled(valid);
		fMapReset->SetEnabled(valid);
	}

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
		fConnectNotice->SetValue(fSettings.connectNotice);
		fQuietMode->SetValue(fSettings.quietMode);
		fQuietMode->SetEnabled(fSettings.connectNotice);
		fDefaults->SetEnabled(!fSettings.IsDefault());
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
		if (valid) {
			fTester->SetFamily(gamepad_layout_family(fGamepad.VendorID(),
				fGamepad.ProductID(), fGamepad.Name()));
		}
		fRumble->SetEnabled(valid && fGamepad.CanRumble());
		fPlayer->SetEnabled(valid);
		_UpdateInfo();
		_UpdateTuning();

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
		else if (_HasUserMapping())
			mapping << "your mapping (Map buttons)";
		else
			mapping << "SDL mapping database";
		fMapping->SetText(mapping);

		int32 player = fGamepad.Player();
		BMenuItem* item = fPlayer->Menu()->ItemAt(player - 1);
		if (item != NULL)
			item->SetMarked(true);
	}

	bool _HasUserMapping()
	{
		BPath path;
		if (find_directory(B_USER_SETTINGS_DIRECTORY, &path) != B_OK
			|| path.Append("Gamepads/mappings") != B_OK)
			return false;
		BFile file(path.Path(), B_READ_ONLY);
		char buffer[8192];
		ssize_t bytes = file.InitCheck() == B_OK
			? file.Read(buffer, sizeof(buffer) - 1) : 0;
		if (bytes <= 0)
			return false;
		buffer[bytes] = '\0';
		BString lowered(buffer);
		lowered.ToLower();
		BString guid(fGamepad.GUID());
		guid.ToLower();
		return lowered.FindFirst(guid) >= 0;
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
	BCheckBox*			fConnectNotice;
	BCheckBox*			fQuietMode;
	BButton*			fDefaults;
	BSlider*			fDeadZone;
	BSlider*			fTrigger;
	BMenuField*			fCurve;
	BCheckBox*			fSwap;
	BButton*			fMap;
	BButton*			fMapReset;
	GamepadTuning		fTuning;
	BGamepad			fGamepad;
	BMessageRunner*		fRunner;
};




// #pragma mark - main


/*!	Batch 11: "Identify": the pad rumbles and its player LEDs run through
	the player numbers twice, then show its own number again.
*/
static status_t
identify_gamepad(const char* path)
{
	BGamepad pad;
	status_t status = pad.Open(path);
	if (status != B_OK)
		return status;
	if (pad.CanRumble())
		pad.Rumble(0xffff, 0xffff, 300000);
	int32 player = pad.Player();
	for (int32 round = 0; round < 2; round++) {
		for (int32 number = 1; number <= 4; number++) {
			pad.SetPlayer(number);
			snooze(120000);
		}
	}
	pad.SetPlayer(player > 0 ? player : 1);
	return B_OK;
}


/*!	Batch 11: what a click on the connect notice opens: the pad's picture
	and name with Identify, Change player, Settings… and "Don't show again
	for this controller". Scripting for tests: the buttons' messages.
*/
class ConnectedWindow : public BWindow {
public:
	ConnectedWindow(const char* path)
		:
		BWindow(BRect(0, 0, 320, 120), "Gamepad connected",
			B_FLOATING_WINDOW_LOOK, B_FLOATING_ALL_WINDOW_FEEL,
			B_NOT_ZOOMABLE | B_NOT_RESIZABLE | B_AUTO_UPDATE_SIZE_LIMITS
				| B_ASYNCHRONOUS_CONTROLS | B_CLOSE_ON_ESCAPE),
		fPath(path)
	{
		BGamepad pad;
		BString name("Gamepad");
		layout_family family = LAYOUT_GENERIC;
		if (pad.Open(path) == B_OK) {
			name = pad.Name();
			fGUID = pad.GUID();
			family = gamepad_layout_family(pad.VendorID(), pad.ProductID(),
				pad.Name());
			fPlayer = pad.Player();
		}
		SetTitle(name);
		BPopUpMenu* players = new BPopUpMenu("player");
		for (int32 i = 1; i <= 8; i++) {
			BString label;
			label << "Player " << i;
			BMessage* message = new BMessage(MSG_PLAYER);
			message->AddInt32("player", i);
			BMenuItem* item = new BMenuItem(label, message);
			item->SetMarked(i == fPlayer);
			players->AddItem(item);
		}
		BMenuField* player = new BMenuField("change_player",
			"Change player:", players);
		BString familyText(kLayoutTitles[family]);
		fPicture = new PictureView(family);
		BLayoutBuilder::Group<>(this, B_VERTICAL, B_USE_SMALL_SPACING)
			.SetInsets(B_USE_WINDOW_SPACING)
			.AddGroup(B_HORIZONTAL)
				.Add(fPicture)
				.AddGroup(B_VERTICAL, 0)
					.Add(new BStringView("name", name))
					.Add(new BStringView("family", familyText))
				.End()
				.AddGlue()
			.End()
			.Add(player)
			.AddGroup(B_HORIZONTAL)
				.Add(new BButton("identify", "Identify",
					new BMessage(MSG_IDENTIFY)))
				.Add(new BButton("settings", "Settings" B_UTF8_ELLIPSIS,
					new BMessage(MSG_OPEN_SETTINGS)))
			.End()
			.Add(new BButton("silence",
				"Don't show again for this controller",
				new BMessage(MSG_SILENCE)));
		CenterOnScreen();
	}

	virtual void MessageReceived(BMessage* message)
	{
		switch (message->what) {
			case MSG_IDENTIFY:
				identify_gamepad(fPath.String());
				break;
			case MSG_PLAYER:
			{
				BGamepad pad;
				if (pad.Open(fPath.String()) == B_OK)
					pad.SetPlayer(message->GetInt32("player", 1));
				break;
			}
			case MSG_OPEN_SETTINGS:
				be_app->PostMessage(MSG_OPEN_SETTINGS);
				PostMessage(B_QUIT_REQUESTED);
				break;
			case MSG_SILENCE:
			{
				Settings settings;
				settings.Load();
				if (!fGUID.IsEmpty() && !settings.silenced.HasString(fGUID))
					settings.silenced.Add(fGUID);
				settings.Save();
				PostMessage(B_QUIT_REQUESTED);
				break;
			}
			default:
				BWindow::MessageReceived(message);
		}
	}

	virtual bool QuitRequested()
	{
		be_app->PostMessage(B_QUIT_REQUESTED);
		return true;
	}

private:
	class PictureView : public BView {
	public:
		PictureView(layout_family family)
			:
			BView("picture", B_WILL_DRAW)
		{
			fBitmap = gamepad_layout_icon(family, 48);
			SetExplicitSize(BSize(48, 48));
		}
		~PictureView() { delete fBitmap; }
		virtual void AttachedToWindow()
		{
			SetViewUIColor(B_PANEL_BACKGROUND_COLOR);
		}
		virtual void Draw(BRect)
		{
			if (fBitmap == NULL)
				return;
			SetDrawingMode(B_OP_ALPHA);
			DrawBitmap(fBitmap, BPoint(0, 0));
		}
	private:
		BBitmap* fBitmap;
	};

	BString			fPath;
	BString			fGUID;
	int32			fPlayer = 0;
	PictureView*	fPicture;
};


class GamepadsApp : public BApplication {
public:
	GamepadsApp(const char* connected = NULL)
		:
		BApplication(kAppSignature),
		fConnected(connected)
	{
	}

	virtual void ReadyToRun()
	{
		if (fConnected != NULL)
			(new ConnectedWindow(fConnected))->Show();
		else
			(new GamepadsWindow)->Show();
	}

	virtual void MessageReceived(BMessage* message)
	{
		if (message->what == MSG_OPEN_SETTINGS) {
			fConnected = NULL;
			(new GamepadsWindow)->Show();
			return;
		}
		BApplication::MessageReceived(message);
	}

private:
	const char*	fConnected;
};


/*!	The mapping wizard on the command line: each prompt on its own line
	("step <n>: <prompt>"), "line: <SDL mapping>" and "saved" at the end.
	A step not answered within 20 seconds is skipped.
*/
static int
learn_command(const char* path)
{
	MappingLearner learner;
	status_t status = learner.Open(path);
	if (status != B_OK) {
		fprintf(stderr, "cannot open %s: %s\n", path, strerror(status));
		return 1;
	}
	while (!learner.Done()) {
		printf("step %d: %s\n", (int)learner.Step() + 1, learner.Prompt());
		fflush(stdout);
		bigtime_t deadline = system_time() + 20000000;
		int32 step = learner.Step();
		while (learner.Step() == step) {
			struct pollfd pollFD = { learner.FileDescriptor(), POLLIN, 0 };
			poll(&pollFD, 1, 100);
			learner.Poll();
			if (system_time() > deadline) {
				learner.Skip();
				break;
			}
		}
	}
	BString line = learner.Line();
	printf("line: %s\n", line.String());
	status = MappingLearner::Save(line);
	printf("%s\n", status == B_OK ? "saved" : strerror(status));
	return status == B_OK ? 0 : 1;
}


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
	if (argc == 3 && strcmp(argv[1], "--learn") == 0)
		return learn_command(argv[2]);
	if (argc == 3 && strcmp(argv[1], "--get-tuning") == 0) {
		BGamepad pad;
		if (pad.Open(argv[2]) != B_OK)
			return 1;
		GamepadTuning tuning;
		GamepadTuning::Load(pad.GUID(), tuning);
		printf("%d %.1f %d %d\n", (int)tuning.deadZone, tuning.curve,
			(int)tuning.triggerThreshold, tuning.swapFaceButtons ? 1 : 0);
		return 0;
	}
	if (argc == 7 && strcmp(argv[1], "--tune") == 0) {
		BGamepad pad;
		if (pad.Open(argv[2]) != B_OK)
			return 1;
		GamepadTuning tuning;
		tuning.deadZone = atoi(argv[3]);
		tuning.curve = atof(argv[4]);
		tuning.triggerThreshold = atoi(argv[5]);
		tuning.swapFaceButtons = atoi(argv[6]) != 0;
		tuning.Clamp();
		return GamepadTuning::Save(pad.GUID(), tuning) == B_OK ? 0 : 1;
	}
	if (argc == 3 && strcmp(argv[1], "--mapping") == 0) {
		// batch 11: the Mapping tab's master switch
		if (strcmp(argv[2], "get") == 0) {
			printf("%s\n", MappingView::ReadEnabled() ? "on" : "off");
			return 0;
		}
		if (strcmp(argv[2], "on") != 0 && strcmp(argv[2], "off") != 0)
			return 2;
		return MappingView::SetEnabledSetting(strcmp(argv[2], "on") == 0)
			== B_OK ? 0 : 1;
	}
	if (argc == 3 && strcmp(argv[1], "--identify") == 0)
		return identify_gamepad(argv[2]) == B_OK ? 0 : 1;
	if (argc == 3 && strcmp(argv[1], "--family") == 0) {
		BGamepad pad;
		if (pad.Open(argv[2]) != B_OK)
			return 1;
		layout_family family = gamepad_layout_family(pad.VendorID(),
			pad.ProductID(), pad.Name());
		printf("%s %s %s %s %s\n", kLayoutNames[family],
			kFaceLabels[family][0], kFaceLabels[family][1],
			kFaceLabels[family][2], kFaceLabels[family][3]);
		return 0;
	}
	if (argc == 3 && strcmp(argv[1], "--silenced") == 0) {
		BGamepad pad;
		if (pad.Open(argv[2]) != B_OK)
			return 1;
		printf("%s\n", settings.silenced.HasString(pad.GUID()) ? "yes"
			: "no");
		return 0;
	}
	if (argc == 3 && strcmp(argv[1], "--unsilence") == 0) {
		BGamepad pad;
		if (pad.Open(argv[2]) != B_OK)
			return 1;
		settings.silenced.Remove(pad.GUID());
		return settings.Save() == B_OK ? 0 : 1;
	}
	fprintf(stderr, "usage: %s [--agent | --get <setting> | "
		"--set <setting> on|off | --learn <device> | --get-tuning <device> "
		"| --tune <device> <dead zone %%> <curve> <trigger %%> <swap 0|1>]\n"
		"settings: overlay, low_battery, raw_access, connect_notice, "
		"quiet_mode\n"
		"also: --identify <device>, --family <device>, --connected <device>, "
		"--silenced <device>, --unsilence <device>, --mapping on|off|get\n",
		argv[0]);
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
	if (argc == 3 && strcmp(argv[1], "--connected") == 0) {
		// batch 11: the connect notice's actions
		GamepadsApp app(argv[2]);
		app.Run();
		return 0;
	}
	if (argc >= 2)
		return command_line(argc, argv);

	GamepadsApp app;
	app.Run();
	return 0;
}
