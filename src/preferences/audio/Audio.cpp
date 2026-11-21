/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 */


/*!	The Audio preferences (roadmap F-32): one place for everything the
	Media Kit work of F-31 added, with the everyday controls up front and
	the rest behind "Show advanced settings". The window only holds an
	AudioSettingsView, which the Control Panel (F-28) will take over as its
	Sound page.

	Command line: Audio [--page output|input|applications|devices]
	[--advanced] [--search <text>] [--device <name>]; given to a running
	instance, it shows that page there (--device selects the device on the
	Devices page).

	Scripting for tests and automation (the window answers; see
	3rdparty/ce/README.md, suite sound_settings):
	- 'AuCt' "control" <view name> with "value" <int32> (a checkbox, slider
	  or button: set and invoked, as a click does), "item" <label> (a menu
	  field: that item chosen) or "select" <index> (a list): replies
	  "error" <status_t>; a text field takes "text" <string> (set, then
	  invoked as Enter does);
	- 'AuSt': replies with the state (page, advanced, sidebar pages, devices
	  and what is listed, default output and input, input level);
	- 'AuPg' "page", 'AuAv' "advanced", 'AuSe' "text".
*/


#include <Application.h>
#include <ControlLook.h>
#include <Button.h>
#include <Catalog.h>
#include <CheckBox.h>
#include <File.h>
#include <FindDirectory.h>
#include <LayoutBuilder.h>
#include <ListView.h>
#include <MenuField.h>
#include <MenuItem.h>
#include <Path.h>
#include <TextControl.h>
#include <Screen.h>
#include <Window.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <ScreenFit.h>

#include "AudioSettingsView.h"


#undef B_TRANSLATION_CONTEXT
#define B_TRANSLATION_CONTEXT "Audio"


static const char* kSignature = "application/x-vnd.Renga-Audio";
static const char* kSettingsFile = "Audio settings";

enum {
	kMsgControl		= 'AuCt',
	kMsgState		= 'AuSt',
	kMsgPage		= 'AuPg',
	kMsgAdvanced	= 'AuAv',
	kMsgSearch		= 'AuSe',
	kMsgSelectDevice = 'AuDe'
};


class AudioWindow : public BWindow {
public:
	AudioWindow(BRect frame, bool advanced, bool savedFrame)
		:
		BWindow(frame, B_TRANSLATE_SYSTEM_NAME("Audio"), B_TITLED_WINDOW,
			B_ASYNCHRONOUS_CONTROLS | B_AUTO_UPDATE_SIZE_LIMITS)
	{
		fView = new AudioSettingsView();
		fView->SetAdvanced(advanced);

		// batch 12 (large-font audit): the pages scroll when the screen is
		// smaller than they need (the Devices page at 20 points on a
		// 768-line screen), and the window starts at its layout's size,
		// clamped to the screen, unless the user's saved frame is there
		float inset = be_control_look->ComposeSpacing(B_USE_WINDOW_SPACING);
		BSize limit = BPrivate::screen_content_limit();
		limit.width -= 2 * inset;
		limit.height -= 2 * inset;
		BLayoutBuilder::Group<>(this, B_VERTICAL, 0)
			.SetInsets(B_USE_WINDOW_SPACING)
			.Add(BPrivate::fit_to_screen(fView, limit));
		if (!savedFrame)
			BPrivate::size_window_to_screen(this);
		else
			MoveOnScreen(B_MOVE_IF_PARTIALLY_OFFSCREEN);
	}

	virtual bool QuitRequested()
	{
		_SaveSettings();
		be_app->PostMessage(B_QUIT_REQUESTED);
		return true;
	}

	virtual void MessageReceived(BMessage* message)
	{
		switch (message->what) {
			case kMsgControl:
			{
				BMessage reply(B_REPLY);
				reply.AddInt32("error", _Control(message));
				message->SendReply(&reply);
				break;
			}

			case kMsgState:
			{
				BMessage reply(B_REPLY);
				fView->GetState(&reply);
				message->SendReply(&reply);
				break;
			}

			case kMsgPage:
				fView->SelectPage(message->GetString("page", ""));
				break;

			case kMsgAdvanced:
				fView->SetAdvanced(message->GetBool("advanced", true));
				break;

			case kMsgSearch:
				fView->Search(message->GetString("text", ""));
				break;

			case kMsgSelectDevice:
				fView->SelectDevice(message->GetString("device", ""));
				break;

			default:
				BWindow::MessageReceived(message);
				break;
		}
	}

private:
	/*!	Does what a click on the named control does. */
	status_t _Control(BMessage* message)
	{
		const char* name;
		if (message->FindString("control", &name) != B_OK)
			return B_BAD_VALUE;
		BView* view = FindView(name);
		if (view == NULL)
			return B_NAME_NOT_FOUND;

		const char* label;
		if (BMenuField* field = dynamic_cast<BMenuField*>(view)) {
			if (message->FindString("item", &label) != B_OK)
				return B_BAD_VALUE;
			BMenuItem* item = field->Menu()->FindItem(label);
			if (item == NULL)
				return B_ENTRY_NOT_FOUND;
			if (!field->IsEnabled() || !item->IsEnabled())
				return B_NOT_ALLOWED;
			item->SetMarked(true);
			// (BMenuItem::Invoke() is protected; through BInvoker)
			return static_cast<BInvoker*>(item)->Invoke();
		}

		// batch 11: a text field: "text" set, then invoked (Enter)
		if (BTextControl* text = dynamic_cast<BTextControl*>(view)) {
			const char* string;
			if (message->FindString("text", &string) != B_OK)
				return B_BAD_VALUE;
			text->SetText(string);
			return text->Invoke();
		}

		int32 value;
		if (BListView* list = dynamic_cast<BListView*>(view)) {
			if (message->FindInt32("select", &value) != B_OK)
				return B_BAD_VALUE;
			if (value < 0 || value >= list->CountItems())
				return B_BAD_INDEX;
			list->Select(value);
			return B_OK;
		}

		if (BControl* control = dynamic_cast<BControl*>(view)) {
			if (!control->IsEnabled())
				return B_NOT_ALLOWED;
			if (message->FindInt32("value", &value) == B_OK)
				control->SetValue(value);
			return control->Invoke();
		}
		return B_BAD_TYPE;
	}

	void _SaveSettings()
	{
		BPath path;
		if (find_directory(B_USER_SETTINGS_DIRECTORY, &path) != B_OK)
			return;
		path.Append(kSettingsFile);
		BMessage settings;
		settings.AddRect("frame", Frame());
		BMessage state;
		fView->GetState(&state);
		settings.AddBool("advanced", state.GetBool("advanced", false));
		BFile file(path.Path(), B_WRITE_ONLY | B_CREATE_FILE | B_ERASE_FILE);
		if (file.InitCheck() == B_OK)
			settings.Flatten(&file);
	}

	AudioSettingsView* fView;
};


class AudioApp : public BApplication {
public:
	AudioApp()
		:
		BApplication(kSignature),
		fWindow(NULL)
	{
	}

	virtual void ArgvReceived(int32 argc, char** argv)
	{
		for (int32 i = 1; i < argc; i++) {
			if (strcmp(argv[i], "--page") == 0 && i + 1 < argc)
				fPage = argv[++i];
			else if (strcmp(argv[i], "--device") == 0 && i + 1 < argc)
				fDevice = argv[++i];
			else if (strcmp(argv[i], "--advanced") == 0)
				fAdvanced = true;
			else if (strcmp(argv[i], "--search") == 0 && i + 1 < argc)
				fSearch = argv[++i];
		}
		if (fWindow != NULL)
			_Apply();
	}

	virtual void ReadyToRun()
	{
		BRect frame(100, 80, 100 + 720, 80 + 480);
		bool advanced = false;
		bool savedFrame = false;
		BPath path;
		if (find_directory(B_USER_SETTINGS_DIRECTORY, &path) == B_OK) {
			path.Append(kSettingsFile);
			BFile file(path.Path(), B_READ_ONLY);
			BMessage settings;
			if (file.InitCheck() == B_OK && settings.Unflatten(&file) == B_OK) {
				BRect saved;
				if (settings.FindRect("frame", &saved) == B_OK
					&& BScreen().Frame().Contains(saved.LeftTop())) {
					frame = saved;
					savedFrame = true;
				}
				advanced = settings.GetBool("advanced", false);
			}
		}
		fWindow = new AudioWindow(frame, advanced, savedFrame);
		_Apply();
		fWindow->Show();
	}

private:
	void _Apply()
	{
		if (fAdvanced) {
			BMessage message(kMsgAdvanced);
			message.AddBool("advanced", true);
			fWindow->PostMessage(&message);
		}
		if (fPage.Length() > 0) {
			BMessage message(kMsgPage);
			message.AddString("page", fPage);
			fWindow->PostMessage(&message);
		}
		if (fSearch.Length() > 0) {
			BMessage message(kMsgSearch);
			message.AddString("text", fSearch);
			fWindow->PostMessage(&message);
		}
		if (fDevice.Length() > 0) {
			BMessage message(kMsgSelectDevice);
			message.AddString("device", fDevice);
			fWindow->PostMessage(&message);
		}
		fDevice = "";
		fPage = "";
		fSearch = "";
		fAdvanced = false;
		fWindow->Activate();
	}

	AudioWindow*	fWindow;
	BString			fPage;
	BString			fSearch;
	BString			fDevice;
	bool			fAdvanced = false;
};


int
main()
{
	AudioApp app;
	app.Run();
	return 0;
}
