/*
 * Copyright 2010-2014, Haiku Inc. All rights reserved.
 * Copyright 2010 Wim van der Meer <WPJvanderMeer@gmail.com>
 * Copyright Karsten Heimrich, host.haiku@gmx.de.
 * All rights reserved. Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Karsten Heimrich
 *		Fredrik Modéen
 *		Christophe Huriaux
 *		Wim van der Meer
 *		Pawan Yerramilli
 */


#include "ScreenshotWindow.h"

#include <stdlib.h>

#include <Alert.h>
#include <Application.h>
#include <Bitmap.h>
#include <Box.h>
#include <Button.h>
#include <Catalog.h>
#include <CheckBox.h>
#include <ControlLook.h>
#include <File.h>
#include <FilePanel.h>
#include <FindDirectory.h>
#include <LayoutBuilder.h>
#include <Locale.h>
#include <Menu.h>
#include <MenuField.h>
#include <MenuItem.h>
#include <MessageFilter.h>
#include <Path.h>
#include <RadioButton.h>
#include <Roster.h>
#include <SeparatorView.h>
#include <SpaceLayoutItem.h>
#include <String.h>
#include <StringView.h>
#include <TextControl.h>
#include <TranslationUtils.h>
#include <TranslatorRoster.h>
#include <MessageRunner.h>
#include <PopUpMenu.h>

#include <stdio.h>

#include "Control.h"
#include "Recorder.h"
#include "Utility.h"


#undef B_TRANSLATION_CONTEXT
#define B_TRANSLATION_CONTEXT "ScreenshotWindow"


enum {
	kIncludeBorder,
	kIncludeCursor,
	kNewScreenshot,
	kImageFormat,
	kLocationChanged,
	kChooseLocation,
	kSaveScreenshot,
	kSettings,
	kCloseTranslatorSettings,
	kSelectArea,
	// Renga: screen recording
	kRecord,
	kRecordTick,
	kRecordOptionChanged,
	kFrameRate,
	kRecordFormat
};


// Renga (F-31 phase 4): the formats recordings can be written in, through
// the encode/decode API's codecs (the FFmpeg plug-in).
static const struct {
	const char*	label;
	const char*	container;
	const char*	extension;
	const char*	video;
	const char*	audio;
} kRecordFormats[] = {
	{ "Matroska (MPEG-4, PCM)", "mkv", "mkv", "mpeg4", "pcm" },
	{ "WebM (VP9, Opus)", "webm", "webm", "vp9", "opus" }
};


// #pragma mark - QuitMessageFilter


class QuitMessageFilter : public BMessageFilter {
public:
	QuitMessageFilter(BWindow* window)
		:
		BMessageFilter((uint32)B_QUIT_REQUESTED),
		fWindow(window)
	{
	}

	virtual filter_result Filter(BMessage* message, BHandler** target)
	{
		BMessenger(fWindow).SendMessage(kCloseTranslatorSettings);
		return B_SKIP_MESSAGE;
	}

private:
	BWindow* fWindow;
};


// #pragma mark - DirectoryRefFilter


class DirectoryRefFilter : public BRefFilter {
public:
	virtual ~DirectoryRefFilter()
	{
	}

	virtual bool Filter(const entry_ref* ref, BNode* node,
		struct stat_beos* stat, const char* filetype)
	{
		return node->IsDirectory();
	}
};


// #pragma mark - ScreenshotWindow


ScreenshotWindow::ScreenshotWindow(const Utility& utility, bool silent,
	bool clipboard)
	:
	BWindow(BRect(0, 0, 200.0, 100.0), B_TRANSLATE_SYSTEM_NAME("Screenshot"),
		B_TITLED_WINDOW, B_NOT_ZOOMABLE | B_NOT_RESIZABLE | B_AVOID_FRONT
			| B_QUIT_ON_WINDOW_CLOSE | B_AUTO_UPDATE_SIZE_LIMITS
			| B_CLOSE_ON_ESCAPE),
	fUtility(utility),
	fDelayControl(NULL),
	fScreenshot(NULL),
	fOutputPathPanel(NULL),
	fLastSelectedPath(NULL),
	fSettingsWindow(NULL),
	fDelay(0),
	fIncludeBorder(false),
	fIncludeCursor(false),
	fShotType(kWholeScreen),
	fOutputFilename(NULL),
	fExtension(""),
	fImageFileType(B_PNG_FORMAT),
	fRecorder(NULL),
	fRecordTicker(NULL),
	fRecordLimit(0),
	fQuitAfterRecording(false),
	fRecordSystemAudioSetting(true),
	fRecordMicrophoneSetting(false),
	fFrameRate(30),
	fRecordFormat(0)
{
	// _ReadSettings() needs a valid fOutputPathMenu
	fOutputPathMenu = new BMenu(B_TRANSLATE("Please select"));
	_ReadSettings();

	// _NewScreenshot() needs a valid fNameControl
	BString name(B_TRANSLATE_NOCOLLECT(fUtility.sDefaultFileNameBase));
	name << 1;
	name = _FindValidFileName(name.String());
	fNameControl = new BTextControl("", B_TRANSLATE("Name:"), name, NULL);

	// Check if fUtility contains valid data
	if (fUtility.wholeScreen == NULL) {
		_NewScreenshot(silent, clipboard, true);
		return;
	}

	fScreenshot = fUtility.MakeScreenshot(fIncludeCursor, fIncludeBorder, fShotType);

	fWholeScreen = new BRadioButton(B_TRANSLATE("Whole screen"),
		new BMessage(kWholeScreen));

	fActiveWindow = new BRadioButton(B_TRANSLATE("Active window"),
		new BMessage(kActiveWindow));

	fAreaSelect = new BRadioButton(B_TRANSLATE("Selected area"),
		new BMessage(kShowSelectedArea));
	fAreaSelect->SetEnabled(false);
	fActiveWindow->SetEnabled(fUtility.activeWindowFrame.IsValid());

	if (fShotType == kShowSelectedArea && fAreaSelect->IsEnabled())
		fAreaSelect->SetValue(B_CONTROL_ON);
	else if (fShotType == kActiveWindow && fUtility.activeWindowFrame.IsValid())
		fActiveWindow->SetValue(B_CONTROL_ON);
	else
		fWholeScreen->SetValue(B_CONTROL_ON);

	fWindowBorder = new BCheckBox(B_TRANSLATE("Include window border"),
		new BMessage(kIncludeBorder));
	if (fShotType != kActiveWindow || !fUtility.activeWindowFrame.IsValid())
		fWindowBorder->SetEnabled(false);
	if (fIncludeBorder && fUtility.activeWindowFrame.IsValid())
		fWindowBorder->SetValue(B_CONTROL_ON);

	fShowCursor = new BCheckBox(B_TRANSLATE("Include mouse pointer"),
		new BMessage(kIncludeCursor));
	if (fIncludeCursor)
		fShowCursor->SetValue(B_CONTROL_ON);

	BString delay;
	delay << fDelay / 1000000;
	fDelayControl = new BTextControl("", B_TRANSLATE("Delay:"), delay.String(),
		NULL);
	_DisallowChar(fDelayControl->TextView());
	fDelayControl->TextView()->SetAlignment(B_ALIGN_RIGHT);
	BStringView* seconds = new BStringView("", B_TRANSLATE("seconds"));
	seconds->SetExplicitMaxSize(BSize(B_SIZE_UNLIMITED, B_SIZE_UNSET));

	BMenuField* menuLocation = new BMenuField(B_TRANSLATE("Save in:"),
		fOutputPathMenu);
	menuLocation->SetViewUIColor(B_PANEL_BACKGROUND_COLOR);

	fTranslatorMenu = new BMenu(B_TRANSLATE("Please select"));
	_SetupTranslatorMenu();
	BMenuField* menuFormat = new BMenuField(B_TRANSLATE("Save as:"),
		fTranslatorMenu);
	menuFormat->SetViewUIColor(B_PANEL_BACKGROUND_COLOR);

	BButton* showSettings =  new BButton("",
		B_TRANSLATE("Settings" B_UTF8_ELLIPSIS), new BMessage(kSettings));
	showSettings->SetExplicitAlignment(
		BAlignment(B_ALIGN_RIGHT, B_ALIGN_BOTTOM));

	BBox* divider = new BBox(B_FANCY_BORDER, NULL);
	divider->SetExplicitMaxSize(BSize(B_SIZE_UNLIMITED, 1));

	BButton* saveScreenshot  = new BButton("", B_TRANSLATE("Save"),
		new BMessage(kSaveScreenshot));

	// Renga: screen recording of the chosen screen, window or area, with
	// optional system audio and microphone, through the Media Kit's
	// capture session (F-31 phase 3, F-44).
	fRecordButton = new BButton("record", B_TRANSLATE("Record video"),
		new BMessage(kRecord));
	fRecordStatus = new BStringView("record status", "");
	fRecordSystemAudio = new BCheckBox("system audio",
		B_TRANSLATE("Record system audio"),
		new BMessage(kRecordOptionChanged));
	fRecordMicrophone = new BCheckBox("microphone",
		B_TRANSLATE("Record microphone"),
		new BMessage(kRecordOptionChanged));
	BString currentRate;
	currentRate.SetToFormat(B_TRANSLATE("%" B_PRId32 " frames per second"),
		(int32)fFrameRate);
	fFrameRateMenu = new BPopUpMenu(currentRate.String());
	const int32 kFrameRates[] = { 10, 15, 30, 60 };
	for (uint32 i = 0; i < sizeof(kFrameRates) / sizeof(kFrameRates[0]);
			i++) {
		BString label;
		label.SetToFormat(B_TRANSLATE("%" B_PRId32 " frames per second"),
			kFrameRates[i]);
		BMessage* message = new BMessage(kFrameRate);
		message->AddFloat("rate", kFrameRates[i]);
		BMenuItem* item = new BMenuItem(label.String(), message);
		fFrameRateMenu->AddItem(item);
		if (kFrameRates[i] == (int32)fFrameRate)
			item->SetMarked(true);
	}
	BMenuField* frameRateField = new BMenuField(B_TRANSLATE("Video:"),
		fFrameRateMenu);
	BPopUpMenu* formatMenu = new BPopUpMenu(B_TRANSLATE_NOCOLLECT(
		kRecordFormats[fRecordFormat].label));
	for (int32 i = 0; i < (int32)B_COUNT_OF(kRecordFormats); i++) {
		BMessage* message = new BMessage(kRecordFormat);
		message->AddInt32("format", i);
		BMenuItem* item = new BMenuItem(B_TRANSLATE_NOCOLLECT(
			kRecordFormats[i].label), message);
		formatMenu->AddItem(item);
		if (i == fRecordFormat)
			item->SetMarked(true);
	}
	BMenuField* formatField = new BMenuField(B_TRANSLATE("Format:"),
		formatMenu);
	fStreamURL = new BTextControl("stream", B_TRANSLATE("Stream to:"), "",
		NULL);
	fStreamURL->SetToolTip(B_TRANSLATE("Instead of a file: udp://, rtp:// or "
		"srt:// address (MPEG-TS with MPEG-4 video and Opus audio)"));
	fRecordSystemAudio->SetValue(fRecordSystemAudioSetting);
	fRecordMicrophone->SetValue(fRecordMicrophoneSetting);

	const float kSpacing = be_control_look->DefaultItemSpacing();
	const float kLabelSpacing = be_control_look->DefaultLabelSpacing();

	fPreview = new BView("preview", B_WILL_DRAW | B_FULL_UPDATE_ON_RESIZE);
	BBox* previewBox = new BBox(B_FANCY_BORDER, fPreview);

	BLayoutBuilder::Group<>(this, B_VERTICAL, 0)
		.AddGroup(B_HORIZONTAL)
			.SetInsets(B_USE_WINDOW_SPACING, B_USE_WINDOW_SPACING,
				B_USE_WINDOW_SPACING, B_USE_DEFAULT_SPACING)
			.Add(previewBox)
			.AddGroup(B_VERTICAL, 0)
				.Add(fWholeScreen)
				.Add(fActiveWindow)
				.Add(fAreaSelect)
				.AddStrut(kSpacing)
				.Add(fWindowBorder)
				.Add(fShowCursor)
				.AddStrut(kSpacing)
				.AddGrid(0.0, kSpacing / 2)
					.Add(fDelayControl->CreateLabelLayoutItem(), 0, 0)
					.Add(fDelayControl->CreateTextViewLayoutItem(), 1, 0)
					.Add(BSpaceLayoutItem::CreateHorizontalStrut(kLabelSpacing),
						2, 0)
					.Add(seconds, 3, 0)
					.Add(fNameControl->CreateLabelLayoutItem(), 0, 1)
					.Add(fNameControl->CreateTextViewLayoutItem(), 1, 1, 3, 1)
					.Add(menuLocation->CreateLabelLayoutItem(), 0, 2)
					.Add(menuLocation->CreateMenuBarLayoutItem(), 1, 2, 3, 1)
					.Add(menuFormat->CreateLabelLayoutItem(), 0, 3)
					.Add(menuFormat->CreateMenuBarLayoutItem(), 1, 3, 3, 1)
				.End()
				.AddStrut(kSpacing / 2)
				.Add(showSettings)
				.AddStrut(kSpacing)
				.Add(new BSeparatorView(B_HORIZONTAL))
				.AddStrut(kSpacing / 2)
				.Add(fRecordSystemAudio)
				.Add(fRecordMicrophone)
				.AddGroup(B_HORIZONTAL, kLabelSpacing)
					.Add(frameRateField->CreateLabelLayoutItem())
					.Add(frameRateField->CreateMenuBarLayoutItem())
				.End()
				.AddGroup(B_HORIZONTAL, kLabelSpacing)
					.Add(formatField->CreateLabelLayoutItem())
					.Add(formatField->CreateMenuBarLayoutItem())
				.End()
				.AddGroup(B_HORIZONTAL, kLabelSpacing)
					.Add(fStreamURL->CreateLabelLayoutItem())
					.Add(fStreamURL->CreateTextViewLayoutItem())
				.End()
				.Add(fRecordStatus)
				.AddGlue()
			.End()
		.End()
		.Add(new BSeparatorView(B_HORIZONTAL))
		.AddGroup(B_HORIZONTAL)
			.SetInsets(B_USE_WINDOW_SPACING, B_USE_DEFAULT_SPACING,
				B_USE_WINDOW_SPACING, B_USE_WINDOW_SPACING)
			.Add(new BButton("", B_TRANSLATE("Copy to clipboard"),
				new BMessage(B_COPY)))
			.Add(new BButton("", B_TRANSLATE("New screenshot"),
				new BMessage(kNewScreenshot)))
			.Add(new BButton("", B_TRANSLATE("Select area"),
				new BMessage(kSelectArea)))
			.AddGlue()
			.Add(fRecordButton)
			.Add(saveScreenshot);

	saveScreenshot->MakeDefault(true);

	_UpdatePreviewPanel();
	_UpdateFilenameSelection();

	CenterOnScreen();
	Show();
}


ScreenshotWindow::~ScreenshotWindow()
{
	delete fRecordTicker;
	delete fRecorder;

	if (fOutputPathPanel)
		delete fOutputPathPanel->RefFilter();

	delete fOutputPathPanel;
	delete fScreenshot;
}


void
ScreenshotWindow::MessageReceived(BMessage* message)
{
	switch (message->what) {
		case kActiveWindow:
			fShotType = kActiveWindow;
			fWindowBorder->SetEnabled(true);

			delete fScreenshot;
			fScreenshot = fUtility.MakeScreenshot(fIncludeCursor, fIncludeBorder, fShotType);
			_UpdatePreviewPanel();
			break;

		case kWholeScreen:
			fShotType = kWholeScreen;
			fWindowBorder->SetEnabled(false);

			delete fScreenshot;
			fScreenshot = fUtility.MakeScreenshot(fIncludeCursor, fIncludeBorder, fShotType);
			_UpdatePreviewPanel();
			break;

		case kShowSelectedArea:
			fShotType = kShowSelectedArea;
			fWindowBorder->SetEnabled(false);

			delete fScreenshot;
			fScreenshot = fUtility.MakeScreenshot(fIncludeCursor, fIncludeBorder, fShotType,
				fSelectedArea);
			_UpdatePreviewPanel();
			break;

		case kIncludeBorder:
			fIncludeBorder = (fWindowBorder->Value() == B_CONTROL_ON);
			delete fScreenshot;
			fScreenshot = fUtility.MakeScreenshot(fIncludeCursor, fIncludeBorder, fShotType);
			_UpdatePreviewPanel();
			break;

		case kIncludeCursor:
			fIncludeCursor = (fShowCursor->Value() == B_CONTROL_ON);
			delete fScreenshot;
			fScreenshot = fUtility.MakeScreenshot(fIncludeCursor, fIncludeBorder, fShotType,
				fSelectedArea);
			_UpdatePreviewPanel();
			break;

		case kNewScreenshot:
			fDelay = (atoi(fDelayControl->Text()) * 1000000) + 50000;
			_NewScreenshot();
			break;

		case kImageFormat:
			message->FindInt32("be:type", &fImageFileType);
			fNameControl->SetText(_FindValidFileName(
				fNameControl->Text()).String());
			_UpdateFilenameSelection();
			_ShowSettings(false);
			break;

		case kLocationChanged:
		{
			void* source = NULL;
			if (message->FindPointer("source", &source) == B_OK)
				fLastSelectedPath = static_cast<BMenuItem*> (source);

			fNameControl->SetText(_FindValidFileName(
				fNameControl->Text()).String());

			_UpdateFilenameSelection();
			break;
		}

		case kChooseLocation:
		{
			if (!fOutputPathPanel) {
				BMessenger target(this);
				fOutputPathPanel = new BFilePanel(B_OPEN_PANEL, &target, NULL,
					B_DIRECTORY_NODE, false, NULL, new DirectoryRefFilter());
				fOutputPathPanel->Window()->SetTitle(
					B_TRANSLATE("Choose folder"));
				fOutputPathPanel->SetButtonLabel(B_DEFAULT_BUTTON,
					B_TRANSLATE("Select"));
				fOutputPathPanel->SetButtonLabel(B_CANCEL_BUTTON,
					B_TRANSLATE("Cancel"));
			}
			fOutputPathPanel->Show();
			break;
		}

		case B_REFS_RECEIVED:
		{
			entry_ref ref;
			if (message->FindRef("refs", &ref) == B_OK) {
				BEntry entry(&ref, true);
				if (entry.InitCheck() == B_OK) {
					BPath path;
					if (entry.GetPath(&path) == B_OK) {
						BString label(path.Path());
						_AddItemToPathMenu(path.Path(), label, 3, true);
					}
				}
			}
			break;
		}

		case B_CANCEL:
			fLastSelectedPath->SetMarked(true);
			break;

		case kSaveScreenshot:
			if (_SaveScreenshot() == B_OK)
				be_app->PostMessage(B_QUIT_REQUESTED);
			break;

		case B_COPY:
			fUtility.CopyToClipboard(fScreenshot);
			break;

		case kSettings:
			_ShowSettings(true);
			break;

		case kCloseTranslatorSettings:
			fSettingsWindow->Lock();
			fSettingsWindow->Quit();
			fSettingsWindow = NULL;
			break;

		case kSelectArea:
			be_app->PostMessage(new BMessage(SS_LAUNCH_AREA_SELECTOR));
			break;

		// Renga: screen recording
		case kRecord:
			if (fRecorder != NULL && fRecorder->IsActive())
				_StopRecording();
			else
				_StartRecording();
			break;
		case kRecordTick:
			_UpdateRecordingStatus();
			break;
		case kRecordOptionChanged:
			fRecordSystemAudioSetting = fRecordSystemAudio->Value() != 0;
			fRecordMicrophoneSetting = fRecordMicrophone->Value() != 0;
			break;
		case kFrameRate:
			message->FindFloat("rate", &fFrameRate);
			break;
		case kRecordFormat:
			fRecordFormat = message->GetInt32("format", 0);
			if (fRecordFormat < 0
				|| fRecordFormat >= (int32)B_COUNT_OF(kRecordFormats))
				fRecordFormat = 0;
			break;
		case kRecorderStarted:
			_RecordingStarted(message);
			break;
		case kRecorderStopped:
			_RecordingStopped(message);
			break;

		default:
			BWindow::MessageReceived(message);
			break;
	}
}


void
ScreenshotWindow::Quit()
{
	if (fRecorder != NULL && fRecorder->IsActive()) {
		// Finish the file before going.
		fRecorder->Stop();
		delete fRecorder;
		fRecorder = NULL;
	}
	if (fUtility.wholeScreen != NULL)
		_WriteSettings();
	BWindow::Quit();
}


void
ScreenshotWindow::SetSelectedArea(BRect frame)
{
	if (frame.IsValid()) {
		Lock();
		fSelectedArea = frame;
		fAreaSelect->SetEnabled(true);
		fAreaSelect->SetValue(B_CONTROL_ON);
		PostMessage(new BMessage(kShowSelectedArea));
		Unlock();
	}
}


void
ScreenshotWindow::_NewScreenshot(bool silent, bool clipboard, bool ignoreDelay, bool selectArea)
{
	BMessage message(B_ARGV_RECEIVED);
	int32 argc = 1;
	message.AddString("argv", "screenshot");

	if (!ignoreDelay) {
		argc += 2;
		BString delay;
		delay << fDelay / 1000000;
		message.AddString("argv", "--delay");
		message.AddString("argv", delay);
	}

	if (selectArea) {
		argc++;
		message.AddString("argv", "--select");
	}

	if (silent || clipboard) {
		if (silent) {
			argc++;
			message.AddString("argv", "--silent");
		}
		if (clipboard) {
			argc++;
			message.AddString("argv", "--clipboard");
		}
		if (fIncludeBorder) {
			argc++;
			message.AddString("argv", "--border");
		}
		if (fIncludeCursor) {
			argc++;
			message.AddString("argv", "--mouse-pointer");
		}
		if (fShotType == kActiveWindow) {
			argc++;
			message.AddString("argv", "--window");
		}
		if (fLastSelectedPath) {
			BPath path(_GetDirectory());
			if (path != NULL) {
				path.Append(fNameControl->Text());
				argc++;
				message.AddString("argv", path.Path());
			}
		}
	}
	message.AddInt32("argc", argc);

	be_roster->Launch("application/x-vnd.haiku-screenshot-cli", &message);
	be_app->PostMessage(B_QUIT_REQUESTED);
}


void
ScreenshotWindow::_UpdatePreviewPanel()
{
	// Set the height of fPreview to what the layout suggests
	fPreview->SetExplicitMinSize(BSize());
	fPreview->SetExplicitMaxSize(BSize());
	Layout(false);

	float height = fPreview->Bounds().Height();
	float width = (fScreenshot->Bounds().Width()
		/ fScreenshot->Bounds().Height()) * height;

	// to prevent a preview way too wide
	if (width > 400.0f) {
		width = 400.0f;
		height = (fScreenshot->Bounds().Height()
			/ fScreenshot->Bounds().Width()) * width;
	}

	fPreview->SetExplicitMinSize(BSize(width, height));
	fPreview->SetExplicitMaxSize(BSize(width, height));

	fPreview->ClearViewBitmap();
	fPreview->SetViewBitmap(fScreenshot, fScreenshot->Bounds(),
		fPreview->Bounds(), B_FOLLOW_ALL, B_FILTER_BITMAP_BILINEAR);
}


void
ScreenshotWindow::_DisallowChar(BTextView* textView)
{
	for (uint32 i = 0; i < '0'; ++i)
		textView->DisallowChar(i);

	for (uint32 i = '9' + 1; i < 255; ++i)
		textView->DisallowChar(i);
}


void
ScreenshotWindow::_SetupOutputPathMenu(const BMessage& settings)
{
	fOutputPathMenu->SetLabelFromMarked(true);

	BString lastSelectedPath;
	settings.FindString("lastSelectedPath", &lastSelectedPath);

	BPath path;
	find_directory(B_USER_DIRECTORY, &path);

	BString label(B_TRANSLATE("Home folder"));
	_AddItemToPathMenu(path.Path(), label, 0,
		path.Path() == lastSelectedPath, 'H');

	path.Append("Desktop");
	label.SetTo(B_TRANSLATE("Desktop"));
	_AddItemToPathMenu(path.Path(), label, 0,
		path.Path() == lastSelectedPath, 'D');

	find_directory(B_USER_NONPACKAGED_DATA_DIRECTORY, &path);
	path.Append("artwork");

	label.SetTo(B_TRANSLATE("Artwork folder"));
	_AddItemToPathMenu(path.Path(), label, 2,
		path.Path() == lastSelectedPath, 'A');

	int32 i = 0;
	BString userPath;
	while (settings.FindString("path", ++i, &userPath) == B_OK) {
		_AddItemToPathMenu(userPath.String(), userPath, 3,
			userPath == lastSelectedPath);
	}

	if (!fLastSelectedPath) {
		if (settings.IsEmpty() || lastSelectedPath.Length() == 0) {
			fOutputPathMenu->ItemAt(1)->SetMarked(true);
			fLastSelectedPath = fOutputPathMenu->ItemAt(1);
		} else {
			_AddItemToPathMenu(lastSelectedPath.String(), lastSelectedPath, 3,
				true);
		}
	}

	fOutputPathMenu->AddItem(new BSeparatorItem());
	fOutputPathMenu->AddItem(new BMenuItem(
		B_TRANSLATE("Choose folder" B_UTF8_ELLIPSIS),
		new BMessage(kChooseLocation), 'F'));
}


void
ScreenshotWindow::_AddItemToPathMenu(const char* path, BString& label,
	int32 index, bool markItem, uint32 shortcutKey)
{
	// Make sure that item won't be a duplicate of an existing one
	for (int32 i = fOutputPathMenu->CountItems() - 1; i >= 0; --i) {
		BMenuItem* menuItem = fOutputPathMenu->ItemAt(i);
		BMessage* message = menuItem->Message();
		const char* pathFromItem;
		if (message != NULL && message->what == kLocationChanged
			&& message->FindString("path", &pathFromItem) == B_OK
			&& !strcmp(path, pathFromItem)) {

			if (markItem) {
				fOutputPathMenu->ItemAt(i)->SetMarked(true);
				fLastSelectedPath = fOutputPathMenu->ItemAt(i);
			}
			return;
		}
	}

	BMessage* message = new BMessage(kLocationChanged);
	message->AddString("path", path);

	fOutputPathMenu->TruncateString(&label, B_TRUNCATE_MIDDLE,
		fOutputPathMenu->StringWidth("SomethingLongHere"));

	fOutputPathMenu->AddItem(new BMenuItem(label.String(), message,
		shortcutKey), index);

	if (markItem) {
		fOutputPathMenu->ItemAt(index)->SetMarked(true);
		fLastSelectedPath = fOutputPathMenu->ItemAt(index);
	}
}


void
ScreenshotWindow::_UpdateFilenameSelection()
{
	fNameControl->MakeFocus(true);
	fNameControl->TextView()->Select(0,	fNameControl->TextView()->TextLength()
		- fExtension.Length());

	fNameControl->TextView()->ScrollToSelection();
}


void
ScreenshotWindow::_SetupTranslatorMenu()
{
	BMessage message(kImageFormat);
	fTranslatorMenu = new BMenu("Please select");
	BTranslationUtils::AddTranslationItems(fTranslatorMenu, B_TRANSLATOR_BITMAP,
		&message, NULL, NULL, NULL);

	fTranslatorMenu->SetLabelFromMarked(true);

	if (fTranslatorMenu->ItemAt(0))
		fTranslatorMenu->ItemAt(0)->SetMarked(true);

	int32 imageFileType;
	for (int32 i = 0; i < fTranslatorMenu->CountItems(); ++i) {
		BMenuItem* item = fTranslatorMenu->ItemAt(i);
		if (item != NULL && item->Message()) {
			item->Message()->FindInt32("be:type", &imageFileType);
			if (fImageFileType == imageFileType) {
				item->SetMarked(true);
				MessageReceived(item->Message());
				break;
			}
		}
	}
}


void
ScreenshotWindow::_DisplaySaveError(BString _message) {
	BString alertText;
	alertText.SetToFormat(B_TRANSLATE("Error saving \"%s\":\n\t%s"),
		fNameControl->Text(), _message.String());

	BAlert* alert = new BAlert(B_TRANSLATE("Failed to save screenshot"),
		alertText.String(),	B_TRANSLATE("OK"),
		NULL, NULL, B_WIDTH_AS_USUAL, B_STOP_ALERT);

	alert->SetShortcut(0, B_ESCAPE);
	alert->Go();
}


status_t
ScreenshotWindow::_SaveScreenshot()
{
	if (!fScreenshot || !fLastSelectedPath)
		return B_ERROR;

	BPath path(_GetDirectory());

	if (path == NULL)
		return B_ERROR;

	BEntry directoryEntry;
	directoryEntry.SetTo(path.Path());

	// create folder if it doesn't exist
	// necessary, for example, when the user selects the Artwork folder from
	// the list of predefined folders.
	if (!directoryEntry.Exists()) {
		status_t directoryCreateStatus = create_directory(path.Path(), 0755);
		if (directoryCreateStatus != B_OK) {
			_DisplaySaveError(strerror(directoryCreateStatus));

			return directoryCreateStatus;
		}
	} else if (!directoryEntry.IsDirectory()) {
		// the entry exists but is not a directory.
		// not much we can do
		_DisplaySaveError(
			B_TRANSLATE("The destination path exists but is not a folder."));

		return B_NOT_A_DIRECTORY;
	}

	path.Append(fNameControl->Text());

	BEntry entry;
	entry.SetTo(path.Path());

	if (entry.Exists()) {
		BAlert* overwriteAlert = new BAlert(
			B_TRANSLATE("overwrite"),
			B_TRANSLATE("This file already exists.\n Are you sure you would "
				"like to overwrite it?"),
			B_TRANSLATE("Cancel"),
			B_TRANSLATE("Overwrite"),
			NULL, B_WIDTH_AS_USUAL, B_EVEN_SPACING, B_WARNING_ALERT);

		overwriteAlert->SetShortcut(0, B_ESCAPE);

		if (overwriteAlert->Go() == 0)
			return B_CANCELED;
	}

	status_t saveStatus = fUtility.Save(fScreenshot,
		path.Path(), fImageFileType);

	if (saveStatus != B_OK) {
		_DisplaySaveError(strerror(saveStatus));
		return saveStatus;
	}
	return B_OK;
}


void
ScreenshotWindow::_ShowSettings(bool activate)
{
	if (!fSettingsWindow && !activate)
		return;

	// Find a translator
	translator_id translator;
	if (fUtility.FindTranslator(fImageFileType, translator) != B_OK)
		return;

	// Create a window with a configuration view
	BView* view;
	BRect rect(0, 0, 239, 239);

	status_t status = BTranslatorRoster::Default()->MakeConfigurationView(
		translator, NULL, &view, &rect);
	if (status != B_OK || view == NULL) {
		// TODO: proper translation, better error dialog
		BAlert* alert = new BAlert(NULL, strerror(status), B_TRANSLATE("OK"));
		alert->SetFlags(alert->Flags() | B_CLOSE_ON_ESCAPE);
		alert->Go();
	} else if (fSettingsWindow != NULL) {
		fSettingsWindow->RemoveChild(fSettingsWindow->ChildAt(0));
		float width, height;
		view->GetPreferredSize(&width, &height);
		fSettingsWindow->ResizeTo(width, height);
		fSettingsWindow->AddChild(view);
		if (activate)
			fSettingsWindow->Activate();
	} else {
		fSettingsWindow = new BWindow(rect,
			B_TRANSLATE("Translator Settings"),
			B_TITLED_WINDOW_LOOK, B_NORMAL_WINDOW_FEEL,
			B_NOT_ZOOMABLE | B_NOT_RESIZABLE);
		fSettingsWindow->AddFilter(new QuitMessageFilter(this));
		fSettingsWindow->AddChild(view);
		fSettingsWindow->CenterOnScreen();
		fSettingsWindow->Show();
	}
}


BString
ScreenshotWindow::_FindValidFileName(const char* name)
{
	BString baseName(name);

	if (!fExtension.IsEmpty())
		baseName.RemoveLast(fExtension);

	if (!fLastSelectedPath)
		return baseName;

	BPath orgPath(_GetDirectory());
	if (orgPath == NULL)
		return baseName;

	fExtension = fUtility.FileNameExtension(fImageFileType);

	BPath outputPath = orgPath;
	BString fileName;
	fileName << baseName << fExtension;
	outputPath.Append(fileName);

	if (!BEntry(outputPath.Path()).Exists())
		return fileName;

	if (baseName.FindFirst(B_TRANSLATE_NOCOLLECT(
			fUtility.sDefaultFileNameBase)) == 0)
		baseName.SetTo(fUtility.sDefaultFileNameBase);

	BEntry entry;
	int32 index = 1;

	do {
		fileName = "";
		fileName << baseName << index++ << fExtension;
		outputPath.SetTo(orgPath.Path());
		outputPath.Append(fileName);
		entry.SetTo(outputPath.Path());
	} while (entry.Exists());

	return fileName;
}


BPath
ScreenshotWindow::_GetDirectory()
{
	BPath path;

	BMessage* message = fLastSelectedPath->Message();
	const char* stringPath;
	if (message && message->FindString("path", &stringPath) == B_OK)
		path.SetTo(stringPath);

	return path;
}


void
ScreenshotWindow::_ReadSettings()
{
	BMessage settings;

	BPath settingsPath;
	if (find_directory(B_USER_SETTINGS_DIRECTORY, &settingsPath) != B_OK)
		return;

	settingsPath.Append("Screenshot_settings");

	BFile file(settingsPath.Path(), B_READ_ONLY);
	if (file.InitCheck() == B_OK)
		settings.Unflatten(&file);

	if (settings.FindInt32("type", &fImageFileType) != B_OK)
		fImageFileType = B_PNG_FORMAT;
	bool activeWindow = false;
	settings.FindBool("includeBorder", &fIncludeBorder);
	settings.FindBool("includeCursor", &fIncludeCursor);
	settings.FindBool("grabActiveWindow", &activeWindow);
	settings.FindInt64("delay", &fDelay);
	settings.FindString("outputFilename", &fOutputFilename);
	settings.FindBool("recordSystemAudio", &fRecordSystemAudioSetting);
	settings.FindBool("recordMicrophone", &fRecordMicrophoneSetting);
	settings.FindFloat("recordFrameRate", &fFrameRate);
	if (activeWindow)
		fShotType = kActiveWindow;

	_SetupOutputPathMenu(settings);
}


void
ScreenshotWindow::_WriteSettings()
{
	if (fDelayControl)
		fDelay = (atoi(fDelayControl->Text()) * 1000000) + 50000;

	BMessage settings;

	settings.AddInt32("type", fImageFileType);
	settings.AddBool("includeBorder", fIncludeBorder);
	settings.AddBool("includeCursor", fIncludeCursor);
	settings.AddBool("grabActiveWindow", fShotType == kActiveWindow);
	settings.AddInt64("delay", fDelay);
	settings.AddString("outputFilename", fOutputFilename);
	settings.AddBool("recordSystemAudio", fRecordSystemAudioSetting);
	settings.AddBool("recordMicrophone", fRecordMicrophoneSetting);
	settings.AddFloat("recordFrameRate", fFrameRate);

	BString path;
	int32 count = fOutputPathMenu->CountItems();
	if (count > 5) {
		for (int32 i = count - 3; i > count - 8 && i > 2; --i) {
			BMenuItem* item = fOutputPathMenu->ItemAt(i);
			if (item) {
				BMessage* msg = item->Message();
				if (msg && msg->FindString("path", &path) == B_OK)
					settings.AddString("path", path.String());
			}
		}
	}

	if (fLastSelectedPath) {
		BMessage* msg = fLastSelectedPath->Message();
		if (msg && msg->FindString("path", &path) == B_OK)
			settings.AddString("lastSelectedPath", path.String());
	}

	BPath settingsPath;
	if (find_directory(B_USER_SETTINGS_DIRECTORY, &settingsPath) != B_OK)
		return;
	settingsPath.Append("Screenshot_settings");

	BFile file(settingsPath.Path(), B_CREATE_FILE | B_ERASE_FILE
		| B_WRITE_ONLY);
	if (file.InitCheck() == B_OK) {
		ssize_t size;
		settings.Flatten(&file, &size);
	}
}


// #pragma mark - Renga: screen recording


/*!	Records like Screenshot takes pictures, from the command line (the
	options come from ScreenshotApp, see there): "seconds" (stop after that
	long), "region" (BRect), "file" (path), "system audio", "microphone",
	"frame rate", "quit" (quit when the file is written; the result goes to
	standard output).
*/
void
ScreenshotWindow::RecordFromCommandLine(const BMessage& options)
{
	BMessage message(options);
	message.what = kRecord;
	fRecordLimit = (bigtime_t)(options.GetFloat("seconds", 0) * 1000000);
	fQuitAfterRecording = options.GetBool("quit", false);
	_StartRecording(&message);
}


void
ScreenshotWindow::_StartRecording(const BMessage* options)
{
	if (fRecorder != NULL && fRecorder->IsActive())
		return;
	if (fRecorder == NULL)
		fRecorder = new Recorder(BMessenger(this));

	BPrivate::CaptureOptions capture;
	capture.frameRate = fFrameRate;
	capture.systemAudio = fRecordSystemAudio->Value() != 0;
	capture.microphone = fRecordMicrophone->Value() != 0;
	if (fIncludeCursor)
		capture.flags |= BPrivate::SCREEN_CAPTURE_INCLUDE_CURSOR;

	BRect region;
	if (options != NULL && options->FindRect("region", &region) == B_OK) {
		capture.source = BPrivate::SCREEN_CAPTURE_REGION;
		capture.region = region;
	} else if (fShotType == kShowSelectedArea && fSelectedArea.IsValid()) {
		capture.source = BPrivate::SCREEN_CAPTURE_REGION;
		capture.region = fSelectedArea;
	} else if (fShotType == kActiveWindow) {
		capture.source = BPrivate::SCREEN_CAPTURE_WINDOW;
		capture.windowToken
			= Recorder::FindWindowToken(fUtility.activeWindowFrame);
		if (fIncludeBorder)
			capture.flags |= BPrivate::SCREEN_CAPTURE_WINDOW_BORDER;
		if (capture.windowToken < 0)
			capture.source = BPrivate::SCREEN_CAPTURE_SCREEN;
	}
	if (options != NULL) {
		capture.systemAudio = options->GetBool("system audio",
			capture.systemAudio);
		capture.microphone = options->GetBool("microphone",
			capture.microphone);
		capture.frameRate = options->GetFloat("frame rate",
			capture.frameRate);
		const char* format;
		if (options->FindString("format", &format) == B_OK) {
			for (int32 i = 0; i < (int32)B_COUNT_OF(kRecordFormats); i++) {
				if (strcmp(format, kRecordFormats[i].container) == 0)
					fRecordFormat = i;
			}
		}
	}
	capture.fileFormat = kRecordFormats[fRecordFormat].container;
	capture.videoCodec = kRecordFormats[fRecordFormat].video;
	capture.audioCodec = kRecordFormats[fRecordFormat].audio;

	// Renga (F-31 phase 4): streaming instead of a file
	BString stream = fStreamURL->Text();
	const char* streamOption;
	if (options != NULL
		&& options->FindString("stream", &streamOption) == B_OK)
		stream = streamOption;
	stream.Trim();
	if (stream.Length() > 0) {
		capture.streamURL = stream;
		capture.fileFormat = stream.StartsWith("rtmp") ? "flv" : "mpegts";
		capture.videoCodec = "mpeg4";
		capture.audioCodec = "opus";
		status_t status = fRecorder->Start(capture, entry_ref());
		if (status != B_OK) {
			BString text;
			text.SetToFormat(B_TRANSLATE("Could not stream: %s"),
				strerror(status));
			fRecordStatus->SetText(text.String());
			return;
		}
		fRecordButton->SetLabel(B_TRANSLATE("Stop recording"));
		fRecordStatus->SetText(B_TRANSLATE("Waiting for permission"
			B_UTF8_ELLIPSIS));
		delete fRecordTicker;
		fRecordTicker = new BMessageRunner(BMessenger(this),
			new BMessage(kRecordTick), 250000);
		return;
	}

	BPath path;
	const char* file;
	if (options != NULL && options->FindString("file", &file) == B_OK)
		path.SetTo(file);
	else {
		BString name = _FindValidFileName(fNameControl->Text());
		int32 dot = name.FindLast('.');
		if (dot > 0)
			name.Truncate(dot);
		name << "." << kRecordFormats[fRecordFormat].extension;
		path = _GetDirectory();
		path.Append(name.String());
	}
	// The file has to exist for an entry_ref of it only as a name; create
	// it, BMediaFile writes it from the start.
	BFile created(path.Path(), B_WRITE_ONLY | B_CREATE_FILE | B_ERASE_FILE);
	entry_ref ref;
	if (get_ref_for_path(path.Path(), &ref) != B_OK) {
		_DisplaySaveError(path.Path());
		return;
	}

	status_t status = fRecorder->Start(capture, ref);
	if (status != B_OK) {
		BString text;
		text.SetToFormat(B_TRANSLATE("Could not record: %s"),
			strerror(status));
		fRecordStatus->SetText(text.String());
		return;
	}
	fRecordButton->SetLabel(B_TRANSLATE("Stop recording"));
	fRecordStatus->SetText(B_TRANSLATE("Waiting for permission" B_UTF8_ELLIPSIS));
	delete fRecordTicker;
	fRecordTicker = new BMessageRunner(BMessenger(this),
		new BMessage(kRecordTick), 250000);
}


void
ScreenshotWindow::_StopRecording()
{
	if (fRecorder == NULL || !fRecorder->IsActive())
		return;
	fRecorder->Stop();
	fRecordStatus->SetText(B_TRANSLATE("Saving" B_UTF8_ELLIPSIS));
}


void
ScreenshotWindow::_RecordingStarted(BMessage* message)
{
	status_t status = message->GetInt32("status", B_ERROR);
	if (status == B_OK) {
		_UpdateRecordingStatus();
		return;
	}
	BString text;
	if (status == B_NOT_ALLOWED)
		text = B_TRANSLATE("Screen recording was not allowed.");
	else
		text.SetToFormat(B_TRANSLATE("Could not record: %s"),
			strerror(status));
	fRecordStatus->SetText(text.String());
}


void
ScreenshotWindow::_RecordingStopped(BMessage* message)
{
	delete fRecordTicker;
	fRecordTicker = NULL;
	fRecordButton->SetLabel(B_TRANSLATE("Record video"));

	status_t status = message->GetInt32("status", B_ERROR);
	const char* path = message->GetString("path", "");
	int64 frames = message->GetInt64("frames", 0);
	int64 audioFrames = message->GetInt64("audio frames", 0);
	bigtime_t duration = message->GetInt64("duration", 0);
	BString text;
	if (status == B_OK) {
		BPath file(path);
		text.SetToFormat(B_TRANSLATE("Saved %s (%.1f s)"), file.Leaf(),
			duration / 1000000.0);
	} else {
		text.SetToFormat(B_TRANSLATE("Recording failed: %s"),
			strerror(status));
	}
	fRecordStatus->SetText(text.String());

	// For scripts (and the harness): what was written.
	printf("Screenshot: recording %s: %s, %" B_PRId64 " video frames, %"
		B_PRId64 " audio frames, %.2f s (%s)\n",
		status == B_OK ? "saved" : "failed", path, frames, audioFrames,
		duration / 1000000.0, strerror(status));
	fflush(stdout);

	if (fQuitAfterRecording)
		be_app->PostMessage(B_QUIT_REQUESTED);
}


void
ScreenshotWindow::_UpdateRecordingStatus()
{
	if (fRecorder == NULL || !fRecorder->IsRecording())
		return;
	bigtime_t duration = fRecorder->Duration();
	if (fRecordLimit > 0 && duration >= fRecordLimit) {
		fRecordLimit = 0;
		_StopRecording();
		return;
	}
	BRect frame = fRecorder->Frame();
	int32 seconds = (int32)(duration / 1000000);
	BString text;
	text.SetToFormat(B_TRANSLATE("Recording %" B_PRId32 ":%02" B_PRId32
		", %" B_PRId32 " x %" B_PRId32), seconds / 60, seconds % 60,
		frame.IntegerWidth() + 1, frame.IntegerHeight() + 1);
	fRecordStatus->SetText(text.String());
}
