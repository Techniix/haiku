/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 */

/*!	Screen recording preferences (Renga, F-31 phase 3).

	One program, four ways to run it:

	  ScreenRecording                 the settings window: which applications
	                                  may record the screen, and the
	                                  recordings running now (Stop)
	  ScreenRecording --prompt <id>   asks the user whether an application
	                                  may record (app_server starts it)
	  ScreenRecording --indicator     the floating "recording" indicator with
	                                  a Stop button, while any capture runs
	                                  (app_server starts it)
	  ScreenRecording --set <signature> allow|deny|ask
	  ScreenRecording --list          for scripts and tests

	app_server accepts answers and permission changes only from system
	software, which this program is (it lives in /boot/system/preferences).
*/


#include <Application.h>
#include <Box.h>
#include <Button.h>
#include <Catalog.h>
#include <CheckBox.h>
#include <ColumnListView.h>
#include <ColumnTypes.h>
#include <LayoutBuilder.h>
#include <MessageRunner.h>
#include <Roster.h>
#include <Screen.h>
#include <StringView.h>
#include <View.h>
#include <Window.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <vector>

#include <ScreenCapture.h>


#undef B_TRANSLATION_CONTEXT
#define B_TRANSLATION_CONTEXT "ScreenRecording"


using BPrivate::BScreenCapture;


static const uint32 kMsgRefresh = 'rfsh';
static const uint32 kMsgAllow = 'alow';
static const uint32 kMsgDeny = 'deny';
static const uint32 kMsgStop = 'stop';
static const uint32 kMsgStopAll = 'stpa';
static const uint32 kMsgSettings = 'sets';
static const uint32 kMsgAllowSelected = 'asel';
static const uint32 kMsgDenySelected = 'dsel';
static const uint32 kMsgForgetSelected = 'fsel';
static const uint32 kMsgStopSelected = 'ssel';


static const char*
source_label(int32 source)
{
	switch (source) {
		case BPrivate::SCREEN_CAPTURE_WINDOW:
			return B_TRANSLATE("a window");
		case BPrivate::SCREEN_CAPTURE_REGION:
			return B_TRANSLATE("part of the screen");
		default:
			return B_TRANSLATE("the whole screen");
	}
}


static const char*
state_label(int32 state)
{
	switch (state) {
		case BPrivate::SCREEN_CAPTURE_PERMISSION_ALLOWED:
			return B_TRANSLATE("Allowed");
		case BPrivate::SCREEN_CAPTURE_PERMISSION_DENIED:
			return B_TRANSLATE("Not allowed");
		default:
			return B_TRANSLATE("Ask");
	}
}


// #pragma mark - Prompt


class PromptWindow : public BWindow {
public:
	PromptWindow(int32 request, const BString& name, const BString& path,
			int32 source)
		:
		BWindow(BRect(0, 0, 100, 100), B_TRANSLATE("Screen recording"),
			B_MODAL_WINDOW_LOOK, B_MODAL_APP_WINDOW_FEEL,
			B_NOT_RESIZABLE | B_NOT_ZOOMABLE | B_AUTO_UPDATE_SIZE_LIMITS
				| B_CLOSE_ON_ESCAPE),
		fRequest(request),
		fAnswered(false)
	{
		BString text;
		text.SetToFormat(B_TRANSLATE("\"%s\" wants to record %s."),
			name.String(), source_label(source));
		BStringView* title = new BStringView("title", text.String());
		BFont font(be_bold_font);
		title->SetFont(&font);

		// Lines of their own: a BTextView's height depends on its width,
		// which the window's layout does not know when it sizes itself.
		BStringView* details1 = new BStringView("details1", B_TRANSLATE(
			"The application will see everything shown there, including"));
		BStringView* details2 = new BStringView("details2", B_TRANSLATE(
			"passwords and private messages, while it records."));
		BStringView* details3 = new BStringView("details3", B_TRANSLATE(
			"An indicator is shown during the recording."));
		BStringView* details4 = new BStringView("details4", B_TRANSLATE(
			"You can change this later in the Screen recording preferences."));
		BStringView* pathView = new BStringView("path", path.String());

		fRemember = new BCheckBox("remember",
			B_TRANSLATE("Remember my decision"), NULL);
		fRemember->SetValue(B_CONTROL_ON);

		BButton* deny = new BButton("deny", B_TRANSLATE("Don't allow"),
			new BMessage(kMsgDeny));
		BButton* allow = new BButton("allow", B_TRANSLATE("Allow"),
			new BMessage(kMsgAllow));

		BLayoutBuilder::Group<>(this, B_VERTICAL)
			.SetInsets(B_USE_WINDOW_SPACING)
			.Add(title)
			.Add(pathView)
			.AddStrut(B_USE_SMALL_SPACING)
			.AddGroup(B_VERTICAL, 0)
				.Add(details1)
				.Add(details2)
				.Add(details3)
			.End()
			.Add(details4)
			.Add(fRemember)
			.AddGroup(B_HORIZONTAL)
				.AddGlue()
				.Add(deny)
				.Add(allow)
			.End();

		SetDefaultButton(allow);
		CenterOnScreen();
	}

	virtual void MessageReceived(BMessage* message)
	{
		switch (message->what) {
			case kMsgAllow:
			case kMsgDeny:
				_Answer(message->what == kMsgAllow);
				PostMessage(B_QUIT_REQUESTED);
				break;
			default:
				BWindow::MessageReceived(message);
		}
	}

	virtual bool QuitRequested()
	{
		// Closing the window is a "no" for now, not remembered.
		if (!fAnswered) {
			BScreenCapture::AnswerRequest(fRequest, false, false);
			fAnswered = true;
		}
		be_app->PostMessage(B_QUIT_REQUESTED);
		return true;
	}

private:
	void _Answer(bool allow)
	{
		BScreenCapture::AnswerRequest(fRequest, allow,
			fRemember->Value() == B_CONTROL_ON);
		fAnswered = true;
	}

	int32		fRequest;
	bool		fAnswered;
	BCheckBox*	fRemember;
};


// #pragma mark - Indicator


class RecordingDot : public BView {
public:
	RecordingDot()
		:
		BView("dot", B_WILL_DRAW)
	{
		SetExplicitSize(BSize(14, 14));
		SetViewUIColor(B_PANEL_BACKGROUND_COLOR);
	}

	virtual void Draw(BRect)
	{
		SetHighColor(220, 30, 30);
		FillEllipse(Bounds().InsetByCopy(1, 1));
	}
};


class IndicatorWindow : public BWindow {
public:
	IndicatorWindow()
		:
		BWindow(BRect(0, 0, 100, 30), B_TRANSLATE("Recording"),
			B_BORDERED_WINDOW_LOOK, B_FLOATING_ALL_WINDOW_FEEL,
			B_NOT_MOVABLE | B_NOT_RESIZABLE | B_NOT_CLOSABLE
				| B_NOT_ZOOMABLE | B_NOT_MINIMIZABLE | B_AVOID_FOCUS
				| B_AUTO_UPDATE_SIZE_LIMITS, B_ALL_WORKSPACES),
		fEmptySince(0)
	{
		fLabel = new BStringView("label", "");
		BButton* stop = new BButton("stop", B_TRANSLATE("Stop"),
			new BMessage(kMsgStopAll));
		BButton* settings = new BButton("settings",
			B_TRANSLATE("Settings" B_UTF8_ELLIPSIS), new BMessage(kMsgSettings));
		BLayoutBuilder::Group<>(this, B_HORIZONTAL)
			.SetInsets(B_USE_SMALL_SPACING)
			.Add(new RecordingDot())
			.Add(fLabel)
			.Add(stop)
			.Add(settings);
		_Refresh();
		fRunner = new BMessageRunner(BMessenger(this),
			new BMessage(kMsgRefresh), 500000);
	}

	~IndicatorWindow()
	{
		delete fRunner;
	}

	virtual void MessageReceived(BMessage* message)
	{
		switch (message->what) {
			case kMsgRefresh:
				_Refresh();
				break;
			case kMsgStopAll:
			{
				BMessage sessions;
				BScreenCapture::GetSessions(&sessions);
				int32 id;
				for (int32 i = 0; sessions.FindInt32("id", i, &id) == B_OK;
						i++) {
					BScreenCapture::StopSession(id);
				}
				_Refresh();
				break;
			}
			case kMsgSettings:
			{
				const char* args[] = { NULL };
				be_roster->Launch(SCREEN_CAPTURE_AGENT_SIGNATURE, 0,
					(char**)args);
				break;
			}
			default:
				BWindow::MessageReceived(message);
		}
	}

private:
	void _Refresh()
	{
		BMessage sessions;
		BScreenCapture::GetSessions(&sessions);
		BString names;
		const char* name;
		int32 count = 0;
		for (int32 i = 0; sessions.FindString("name", i, &name) == B_OK; i++) {
			if (names.FindFirst(name) < 0) {
				if (names.Length() > 0)
					names << ", ";
				names << name;
			}
			count++;
		}
		if (count == 0) {
			// Leave once nothing has been recorded for a moment (a recorder
			// that restarts its session keeps the indicator).
			if (fEmptySince == 0)
				fEmptySince = system_time();
			else if (system_time() - fEmptySince > 1500000)
				be_app->PostMessage(B_QUIT_REQUESTED);
			fLabel->SetText(B_TRANSLATE("Recording stopped"));
		} else {
			fEmptySince = 0;
			BString text;
			text.SetToFormat(B_TRANSLATE("Recording the screen: %s"),
				names.String());
			fLabel->SetText(text.String());
		}
		// Top right corner of the screen, below where the Deskbar sits by
		// default.
		BRect screen = BScreen(this).Frame();
		ResizeToPreferred();
		MoveTo(screen.right - Bounds().Width() - 8, screen.top + 8);
	}

	BStringView*	fLabel;
	BMessageRunner*	fRunner;
	bigtime_t		fEmptySince;
};


// #pragma mark - Settings


class SettingsWindow : public BWindow {
public:
	SettingsWindow()
		:
		BWindow(BRect(100, 100, 620, 480), B_TRANSLATE("Screen recording"),
			B_TITLED_WINDOW, B_QUIT_ON_WINDOW_CLOSE
				| B_AUTO_UPDATE_SIZE_LIMITS)
	{
		fApplications = new BColumnListView("applications", 0,
			B_FANCY_BORDER, true);
		fApplications->AddColumn(new BStringColumn(B_TRANSLATE("Application"),
			200, 50, 400, B_TRUNCATE_END), 0);
		fApplications->AddColumn(new BStringColumn(B_TRANSLATE("Permission"),
			110, 50, 200, B_TRUNCATE_END), 1);
		fApplications->AddColumn(new BStringColumn(B_TRANSLATE("Signature"),
			200, 50, 400, B_TRUNCATE_MIDDLE), 2);
		fApplications->SetExplicitMinSize(BSize(460, 120));

		fSessions = new BColumnListView("sessions", 0, B_FANCY_BORDER, true);
		fSessions->AddColumn(new BStringColumn(B_TRANSLATE("Application"),
			160, 50, 400, B_TRUNCATE_END), 0);
		fSessions->AddColumn(new BStringColumn(B_TRANSLATE("Recording"),
			130, 50, 300, B_TRUNCATE_END), 1);
		fSessions->AddColumn(new BStringColumn(B_TRANSLATE("Size"),
			90, 50, 200, B_TRUNCATE_END), 2);
		fSessions->AddColumn(new BStringColumn(B_TRANSLATE("Frames"),
			70, 40, 200, B_TRUNCATE_END), 3);
		fSessions->SetExplicitMinSize(BSize(460, 80));

		BBox* appsBox = new BBox("apps box");
		appsBox->SetLabel(B_TRANSLATE("Applications that asked to record the "
			"screen"));
		BGroupView* appsGroup = new BGroupView(B_VERTICAL);
		BLayoutBuilder::Group<>(appsGroup)
			.SetInsets(B_USE_DEFAULT_SPACING)
			.Add(fApplications)
			.AddGroup(B_HORIZONTAL)
				.Add(new BButton("allow", B_TRANSLATE("Allow"),
					new BMessage(kMsgAllowSelected)))
				.Add(new BButton("deny", B_TRANSLATE("Don't allow"),
					new BMessage(kMsgDenySelected)))
				.Add(new BButton("forget", B_TRANSLATE("Ask again"),
					new BMessage(kMsgForgetSelected)))
				.AddGlue()
			.End();
		appsBox->AddChild(appsGroup);

		BBox* sessionsBox = new BBox("sessions box");
		sessionsBox->SetLabel(B_TRANSLATE("Recording now"));
		BGroupView* sessionsGroup = new BGroupView(B_VERTICAL);
		BLayoutBuilder::Group<>(sessionsGroup)
			.SetInsets(B_USE_DEFAULT_SPACING)
			.Add(fSessions)
			.AddGroup(B_HORIZONTAL)
				.Add(new BButton("stop", B_TRANSLATE("Stop recording"),
					new BMessage(kMsgStopSelected)))
				.AddGlue()
			.End();
		sessionsBox->AddChild(sessionsGroup);

		BStringView* note = new BStringView("note", B_TRANSLATE("Applications "
			"ask the first time they record; a red indicator is shown while "
			"they do."));

		BLayoutBuilder::Group<>(this, B_VERTICAL)
			.SetInsets(B_USE_WINDOW_SPACING)
			.Add(note)
			.Add(appsBox)
			.Add(sessionsBox);

		_Refresh();
		fRunner = new BMessageRunner(BMessenger(this),
			new BMessage(kMsgRefresh), 1000000);
	}

	~SettingsWindow()
	{
		delete fRunner;
	}

	virtual void MessageReceived(BMessage* message)
	{
		switch (message->what) {
			case kMsgRefresh:
				_Refresh();
				break;
			case kMsgAllowSelected:
			case kMsgDenySelected:
			case kMsgForgetSelected:
			{
				BRow* row = fApplications->CurrentSelection();
				if (row == NULL)
					break;
				BStringField* field = (BStringField*)row->GetField(2);
				int32 state = message->what == kMsgAllowSelected
					? BPrivate::SCREEN_CAPTURE_PERMISSION_ALLOWED
					: message->what == kMsgDenySelected
						? BPrivate::SCREEN_CAPTURE_PERMISSION_DENIED
						: BPrivate::SCREEN_CAPTURE_PERMISSION_UNKNOWN;
				BScreenCapture::SetPermission(field->String(), state);
				_Refresh();
				break;
			}
			case kMsgStopSelected:
			{
				BRow* row = fSessions->CurrentSelection();
				if (row == NULL)
					break;
				int32 index = fSessions->IndexOf(row);
				if (index >= 0 && index < (int32)fSessionIDs.size())
					BScreenCapture::StopSession(fSessionIDs[index]);
				_Refresh();
				break;
			}
			default:
				BWindow::MessageReceived(message);
		}
	}

private:
	void _Refresh()
	{
		BMessage permissions;
		BScreenCapture::GetPermissions(&permissions);
		BString selected;
		if (BRow* row = fApplications->CurrentSelection())
			selected = ((BStringField*)row->GetField(2))->String();
		fApplications->Clear();
		const char* signature;
		for (int32 i = 0; permissions.FindString("signature", i, &signature)
				== B_OK; i++) {
			BRow* row = new BRow();
			row->SetField(new BStringField(
				permissions.GetString("name", i, signature)), 0);
			row->SetField(new BStringField(
				state_label(permissions.GetInt32("state", i, 0))), 1);
			row->SetField(new BStringField(signature), 2);
			fApplications->AddRow(row);
			if (selected == signature)
				fApplications->AddToSelection(row);
		}

		BMessage sessions;
		BScreenCapture::GetSessions(&sessions);
		fSessions->Clear();
		fSessionIDs.clear();
		int32 id;
		for (int32 i = 0; sessions.FindInt32("id", i, &id) == B_OK; i++) {
			BRow* row = new BRow();
			row->SetField(new BStringField(
				sessions.GetString("name", i, "")), 0);
			row->SetField(new BStringField(
				source_label(sessions.GetInt32("source", i, 0))), 1);
			BRect frame = sessions.GetRect("frame", i, BRect());
			BString size;
			size << frame.IntegerWidth() + 1 << " x "
				<< frame.IntegerHeight() + 1;
			row->SetField(new BStringField(size), 2);
			BString frames;
			frames << sessions.GetInt64("frames", i, 0);
			row->SetField(new BStringField(frames), 3);
			fSessions->AddRow(row);
			fSessionIDs.push_back(id);
		}
	}

	BColumnListView*	fApplications;
	BColumnListView*	fSessions;
	std::vector<int32>	fSessionIDs;
	BMessageRunner*		fRunner;
};


// #pragma mark - Application


class ScreenRecordingApp : public BApplication {
public:
	ScreenRecordingApp()
		:
		BApplication(SCREEN_CAPTURE_AGENT_SIGNATURE),
		fWindow(NULL)
	{
	}

	virtual void ArgvReceived(int32 argc, char** argv)
	{
		if (argc >= 3 && strcmp(argv[1], "--prompt") == 0) {
			int32 request = atoi(argv[2]);
			BString signature;
			BString name;
			BString path;
			int32 source = 0;
			if (BScreenCapture::GetRequest(request, signature, name, path,
					source) != B_OK) {
				PostMessage(B_QUIT_REQUESTED);
				return;
			}
			if (name.Length() == 0)
				name = signature;
			fWindow = new PromptWindow(request, name, path, source);
		} else if (argc >= 2 && strcmp(argv[1], "--indicator") == 0) {
			fWindow = new IndicatorWindow();
		}
	}

	virtual void ReadyToRun()
	{
		if (fWindow == NULL)
			fWindow = new SettingsWindow();
		fWindow->Show();
		if (dynamic_cast<PromptWindow*>(fWindow) != NULL)
			fWindow->Activate();
	}

private:
	BWindow*	fWindow;
};


static int
command_line(int argc, char** argv)
{
	if (strcmp(argv[1], "--list") == 0) {
		BMessage permissions;
		status_t status = BScreenCapture::GetPermissions(&permissions);
		const char* signature;
		for (int32 i = 0; permissions.FindString("signature", i, &signature)
				== B_OK; i++) {
			printf("permission %s %s\n", signature,
				state_label(permissions.GetInt32("state", i, 0)));
		}
		BMessage sessions;
		BScreenCapture::GetSessions(&sessions);
		int32 id;
		for (int32 i = 0; sessions.FindInt32("id", i, &id) == B_OK; i++) {
			printf("session %" B_PRId32 " %s %s\n", id,
				sessions.GetString("signature", i, ""),
				source_label(sessions.GetInt32("source", i, 0)));
		}
		return status == B_OK ? 0 : 1;
	}
	if (strcmp(argv[1], "--set") == 0 && argc == 4) {
		int32 state = BPrivate::SCREEN_CAPTURE_PERMISSION_UNKNOWN;
		if (strcmp(argv[3], "allow") == 0)
			state = BPrivate::SCREEN_CAPTURE_PERMISSION_ALLOWED;
		else if (strcmp(argv[3], "deny") == 0)
			state = BPrivate::SCREEN_CAPTURE_PERMISSION_DENIED;
		else if (strcmp(argv[3], "ask") != 0) {
			fprintf(stderr, "usage: %s --set <signature> allow|deny|ask\n",
				argv[0]);
			return 1;
		}
		status_t status = BScreenCapture::SetPermission(argv[2], state);
		if (status != B_OK)
			fprintf(stderr, "%s: %s\n", argv[0], strerror(status));
		return status == B_OK ? 0 : 1;
	}
	fprintf(stderr, "usage: %s [--list | --set <signature> allow|deny|ask]\n",
		argv[0]);
	return 1;
}


int
main(int argc, char** argv)
{
	if (argc >= 2 && (strcmp(argv[1], "--list") == 0
			|| strcmp(argv[1], "--set") == 0)) {
		// The app_server link needs a BApplication.
		BApplication app("application/x-vnd.Renga-ScreenRecording-cli");
		return command_line(argc, argv);
	}

	ScreenRecordingApp app;
	app.Run();
	return 0;
}
