/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 */


#include "SearchWindow.h"

#include <stdio.h>
#include <string.h>

#include <Bitmap.h>
#include <Catalog.h>
#include <Clipboard.h>
#include <ControlLook.h>
#include <LayoutBuilder.h>
#include <ListItem.h>
#include <ListView.h>
#include <MessageFilter.h>
#include <MessageRunner.h>
#include <NodeInfo.h>
#include <Roster.h>
#include <Screen.h>
#include <ScrollView.h>
#include <StringView.h>
#include <TextControl.h>

#include "DeskbarMenu.h"


#undef B_TRANSLATION_CONTEXT
#define B_TRANSLATION_CONTEXT "SearchWindow"


static const uint32 kTextChanged = 'DStc';
static const uint32 kRunSearch = 'DSrn';
static const uint32 kFileResults = 'DSfr';
static const uint32 kOpenResult = 'DSop';

static const bigtime_t kSearchDelay = 120000;
static const int32 kMaxFiles = 20;
static const char* kControlPanelSignature
	= "application/x-vnd.Renga-ControlPanel";
static const uint32 kControlPanelShowPage = 'cpPg';
	// ControlPanel.cpp, kMsgShowPage


// #pragma mark - ResultItem


class ResultItem : public BListItem {
public:
	ResultItem(const SearchResult& result)
		:
		fTitle(result.title),
		fDetail(result.detail),
		fIcon(NULL)
	{
		float size = be_control_look->ComposeIconSize(B_MINI_ICON).Width()
			+ 1;
		entry_ref ref = result.ref;
		if (result.kind == kResultSetting)
			get_ref_for_path("/boot/system/apps/ControlPanel", &ref);
		else if (result.kind == kResultCalculator
			|| result.kind == kResultConversion) {
			get_ref_for_path("/boot/system/apps/DeskCalc", &ref);
		}
		if (ref.name != NULL) {
			fIcon = new BBitmap(BRect(0, 0, size - 1, size - 1),
				B_RGBA32);
			if (BNodeInfo::GetTrackerIcon(&ref, fIcon,
					(icon_size)(int32)size) != B_OK) {
				delete fIcon;
				fIcon = NULL;
			}
		}
	}

	~ResultItem()
	{
		delete fIcon;
	}

	virtual void DrawItem(BView* owner, BRect frame, bool complete)
	{
		rgb_color background = IsSelected()
			? ui_color(B_LIST_SELECTED_BACKGROUND_COLOR)
			: ui_color(B_LIST_BACKGROUND_COLOR);
		rgb_color text = IsSelected()
			? ui_color(B_LIST_SELECTED_ITEM_TEXT_COLOR)
			: ui_color(B_LIST_ITEM_TEXT_COLOR);
		owner->SetLowColor(background);
		owner->FillRect(frame, B_SOLID_LOW);

		float spacing = be_control_look->DefaultLabelSpacing();
		float iconSize = be_control_look->ComposeIconSize(B_MINI_ICON)
			.Width() + 1;
		float x = frame.left + spacing;
		if (fIcon != NULL) {
			owner->SetDrawingMode(B_OP_ALPHA);
			owner->DrawBitmap(fIcon, BPoint(x,
				frame.top + (frame.Height() - iconSize) / 2));
			owner->SetDrawingMode(B_OP_COPY);
		}
		x += iconSize + spacing;

		font_height height;
		owner->GetFontHeight(&height);
		float baseline = frame.top + (frame.Height() + height.ascent
			- height.descent) / 2;

		BFont font;
		owner->GetFont(&font);
		BFont small(font);
		small.SetSize(font.Size() * 0.85f);

		// the detail on the right, in a lighter colour, at most 40%
		BString detail(fDetail);
		float detailWidth = std::min(small.StringWidth(detail),
			frame.Width() * 0.4f);
		small.TruncateString(&detail, B_TRUNCATE_BEGINNING, detailWidth);
		owner->SetFont(&small);
		owner->SetHighColor(tint_color(text, text.IsLight()
			? B_DARKEN_2_TINT : B_LIGHTEN_2_TINT));
		owner->DrawString(detail, BPoint(frame.right - spacing
			- small.StringWidth(detail), baseline));

		owner->SetFont(&font);
		owner->SetHighColor(text);
		BString title(fTitle);
		font.TruncateString(&title, B_TRUNCATE_END,
			frame.right - spacing * 3 - detailWidth - x);
		owner->DrawString(title, BPoint(x, baseline));
	}

	virtual void Update(BView* owner, const BFont* font)
	{
		BListItem::Update(owner, font);
		float iconSize = be_control_look->ComposeIconSize(B_MINI_ICON)
			.Height() + 1;
		SetHeight(std::max(Height(), iconSize) + 6);
	}

private:
	BString		fTitle;
	BString		fDetail;
	BBitmap*	fIcon;
};


// #pragma mark - KeyFilter


//!	Up, Down, Return and Escape typed in the text field.
class KeyFilter : public BMessageFilter {
public:
	KeyFilter(TSearchWindow* window)
		:
		BMessageFilter(B_KEY_DOWN),
		fWindow(window)
	{
	}

	virtual filter_result Filter(BMessage* message, BHandler** target)
	{
		const char* bytes;
		if (message->FindString("bytes", &bytes) != B_OK)
			return B_DISPATCH_MESSAGE;
		BMessage command;
		switch (bytes[0]) {
			case B_UP_ARROW:
				command.what = 'DSup';
				break;
			case B_DOWN_ARROW:
				command.what = 'DSdn';
				break;
			case B_ENTER:
				command.what = kOpenResult;
				break;
			case B_ESCAPE:
				command.what = 'DShd';
				break;
			default:
				return B_DISPATCH_MESSAGE;
		}
		fWindow->PostMessage(&command);
		return B_SKIP_MESSAGE;
	}

private:
	TSearchWindow*	fWindow;
};


// #pragma mark - TSearchWindow


TSearchWindow::TSearchWindow()
	:
	BWindow(BRect(0, 0, 459, 299), B_TRANSLATE("Search"),
		B_BORDERED_WINDOW_LOOK, B_FLOATING_ALL_WINDOW_FEEL,
		B_NOT_RESIZABLE | B_NOT_ZOOMABLE | B_NOT_MOVABLE
			| B_AUTO_UPDATE_SIZE_LIMITS | B_ASYNCHRONOUS_CONTROLS,
		B_ALL_WORKSPACES),
	fRunner(NULL),
	fResults(20),
	fGeneration(0),
	fFileThread(-1),
	fCancelFiles(false),
	fFiles(true),
	fTest(false)
{
	fText = new BTextControl("text", NULL, "", NULL);
	fText->SetModificationMessage(new BMessage(kTextChanged));
	fText->TextView()->AddFilter(new KeyFilter(this));

	fList = new BListView("results");
	fList->SetInvocationMessage(new BMessage(kOpenResult));
	BScrollView* scroll = new BScrollView("scroll", fList, 0, false, true,
		B_NO_BORDER);

	fStatus = new BStringView("status", B_TRANSLATE("Applications, "
		"settings, recent items and files. Try \"2*21\" or \"5 km in "
		"mi\"."));
	BFont font(be_plain_font);
	font.SetSize(font.Size() * 0.85f);
	fStatus->SetFont(&font);
	fStatus->SetHighUIColor(B_PANEL_TEXT_COLOR, B_LIGHTEN_1_TINT);
	fStatus->SetExplicitMinSize(BSize(10, B_SIZE_UNSET));

	float width = be_plain_font->StringWidth("M") * 34;
	scroll->SetExplicitMinSize(BSize(width, width * 0.6f));
	BLayoutBuilder::Group<>(this, B_VERTICAL, B_USE_SMALL_SPACING)
		.SetInsets(B_USE_SMALL_SPACING)
		.Add(fText)
		.Add(scroll)
		.Add(fStatus);
}


TSearchWindow::~TSearchWindow()
{
	_StopFileSearch();
	delete fRunner;
}


bool
TSearchWindow::QuitRequested()
{
	// Only the Deskbar quitting closes it; Escape and clicking elsewhere
	// hide it.
	_StopFileSearch();
	return true;
}


void
TSearchWindow::WindowActivated(bool active)
{
	BWindow::WindowActivated(active);
	if (!active && !fTest && !IsHidden())
		Hide();
}


void
TSearchWindow::SetCalculatorEnabled(bool enabled)
{
	fSearch.SetCalculatorEnabled(enabled);
}


void
TSearchWindow::ShowNear(BRect deskbarFrame, bool vertical, const char* text,
	bool test)
{
	fTest = test;
	// the menu's entries may have changed since the last search
	fSearch.Invalidate();
	fText->SetText(text != NULL ? text : "");
	fText->TextView()->SelectAll();

	// next to the Deskbar: below or above a horizontal one, beside a
	// vertical one, inside the screen
	Layout(true);
	BRect screen = BScreen(this).Frame();
	BRect frame = Frame();
	BPoint where;
	if (vertical) {
		where.x = deskbarFrame.left > screen.Width() / 2
			? deskbarFrame.left - frame.Width() - 1 : deskbarFrame.right + 1;
		where.y = deskbarFrame.top;
	} else {
		where.x = deskbarFrame.left;
		where.y = deskbarFrame.top > screen.Height() / 2
			? deskbarFrame.top - frame.Height() - 1 : deskbarFrame.bottom + 1;
	}
	where.x = std::max(screen.left, std::min(where.x,
		screen.right - frame.Width()));
	where.y = std::max(screen.top, std::min(where.y,
		screen.bottom - frame.Height()));
	MoveTo(where);

	if (IsHidden())
		Show();
	Activate();
	fText->MakeFocus(true);
	_Search();
}


void
TSearchWindow::GetResults(BMessage& reply)
{
	for (int32 i = 0; i < fResults.CountItems(); i++) {
		reply.AddString("title", fResults.ItemAt(i)->title);
		reply.AddInt32("kind", fResults.ItemAt(i)->kind);
	}
	reply.AddBool("searching_files", fFileThread >= 0);
	reply.AddBool("hidden", IsHidden());
}


void
TSearchWindow::MessageReceived(BMessage* message)
{
	switch (message->what) {
		case kTextChanged:
			delete fRunner;
			fRunner = NULL;
			{
				BMessage run(kRunSearch);
				fRunner = new BMessageRunner(BMessenger(this), &run,
					kSearchDelay, 1);
			}
			break;

		case kRunSearch:
			_Search();
			break;

		case kFileResults:
		{
			SearchResultList* files = NULL;
			int32 generation = -1;
			message->FindPointer("results", (void**)&files);
			message->FindInt32("generation", &generation);
			fFileThread = -1;
			if (files == NULL)
				break;
			if (generation == fGeneration) {
				for (int32 i = 0; i < files->CountItems(); i++) {
					SearchResult* result = files->ItemAt(i);
					fResults.AddItem(new SearchResult(*result));
				}
				_ShowResults();
			}
			delete files;
			break;
		}

		case 'DSup':
			_MoveSelection(-1);
			break;
		case 'DSdn':
			_MoveSelection(1);
			break;
		case 'DShd':
			Hide();
			break;

		case kOpenResult:
			_OpenSelection();
			break;

		default:
			BWindow::MessageReceived(message);
	}
}


void
TSearchWindow::_StopFileSearch()
{
	if (fFileThread >= 0) {
		fCancelFiles = true;
		status_t result;
		wait_for_thread(fFileThread, &result);
		fFileThread = -1;
	}
	fCancelFiles = false;
}


struct FileSearchData {
	TSearchWindow*	window;
	BString			text;
	int32			generation;
	volatile bool*	cancel;
};


/*static*/ status_t
TSearchWindow::_FileSearchThread(void* _data)
{
	FileSearchData* data = (FileSearchData*)_data;
	SearchResultList* results = new SearchResultList(kMaxFiles);
	MenuSearch::SearchFiles(data->text, kMaxFiles, *results, data->cancel);
	BMessage message(kFileResults);
	message.AddPointer("results", results);
	message.AddInt32("generation", data->generation);
	if (*data->cancel || data->window->PostMessage(&message) != B_OK)
		delete results;
	delete data;
	return B_OK;
}


void
TSearchWindow::_Search()
{
	_StopFileSearch();
	fGeneration++;
	fResults.MakeEmpty();
	const char* text = fText->Text();
	fSearch.SearchQuick(text, fResults);
	_ShowResults();

	BString trimmed(text);
	trimmed.Trim();
	if (fFiles && trimmed.Length() >= 2) {
		FileSearchData* data = new FileSearchData;
		data->window = this;
		data->text = trimmed;
		data->generation = fGeneration;
		data->cancel = &fCancelFiles;
		fFileThread = spawn_thread(_FileSearchThread, "deskbar file search",
			B_LOW_PRIORITY, data);
		if (fFileThread < 0 || resume_thread(fFileThread) != B_OK) {
			fFileThread = -1;
			delete data;
		}
	}
}


void
TSearchWindow::_ShowResults()
{
	int32 selected = std::max((int32)0, fList->CurrentSelection());
	while (BListItem* item = fList->RemoveItem((int32)0))
		delete item;
	for (int32 i = 0; i < fResults.CountItems(); i++)
		fList->AddItem(new ResultItem(*fResults.ItemAt(i)));
	if (fList->CountItems() > 0)
		fList->Select(std::min(selected, fList->CountItems() - 1));

	BString status;
	BString text(fText->Text());
	text.Trim();
	if (text.IsEmpty()) {
		status = B_TRANSLATE("Applications, settings, recent items and "
			"files. Try \"2*21\" or \"5 km in mi\".");
	} else if (fResults.IsEmpty() && fFileThread < 0)
		status = B_TRANSLATE("Nothing found.");
	else if (fFileThread >= 0)
		status = B_TRANSLATE("Searching files" B_UTF8_ELLIPSIS);
	else {
		status = B_TRANSLATE("Return opens, Up and Down choose, Escape "
			"closes.");
	}
	fStatus->SetText(status);
}


void
TSearchWindow::_MoveSelection(int32 delta)
{
	int32 count = fList->CountItems();
	if (count == 0)
		return;
	int32 index = fList->CurrentSelection() + delta;
	index = std::max((int32)0, std::min(index, count - 1));
	fList->Select(index);
	fList->ScrollToSelection();
}


void
TSearchWindow::_OpenSelection()
{
	int32 index = fList->CurrentSelection();
	if (index < 0)
		index = 0;
	SearchResult* result = fResults.ItemAt(index);
	if (result == NULL)
		return;
	if (result->kind == kResultCalculator
		|| result->kind == kResultConversion) {
		if (be_clipboard->Lock()) {
			be_clipboard->Clear();
			be_clipboard->Data()->AddData("text/plain", B_MIME_TYPE,
				result->value.String(), result->value.Length());
			be_clipboard->Commit();
			be_clipboard->Unlock();
		}
		BString status;
		status.SetToFormat(B_TRANSLATE("Copied \"%s\"."),
			result->value.String());
		fStatus->SetText(status);
		return;
	}
	if (Open(*result) == B_OK)
		Hide();
}


/*static*/ status_t
TSearchWindow::Open(const SearchResult& result)
{
	if (result.kind == kResultSetting) {
		BMessenger controlPanel(kControlPanelSignature);
		if (controlPanel.IsValid()) {
			BMessage show(kControlPanelShowPage);
			show.AddString("id", result.page);
			controlPanel.SendMessage(&show);
			app_info info;
			if (be_roster->GetAppInfo(kControlPanelSignature, &info) == B_OK)
				be_roster->ActivateApp(info.team);
			return B_OK;
		}
		const char* argv[] = { "--page", result.page.String(), NULL };
		return be_roster->Launch(kControlPanelSignature, 2, argv);
	}

	// as the Deskbar menu does: Tracker opens it (applications start,
	// documents open in their application, folders in a window)
	entry_ref ref = result.ref;
	BEntry entry(&ref, true);
	if (entry.GetRef(&ref) != B_OK)
		return B_ENTRY_NOT_FOUND;
	BMessage refs(B_REFS_RECEIVED);
	refs.AddRef("refs", &ref);
	BMessenger tracker = TDeskbarMenu::DefaultTarget();
	if (tracker.IsValid() && tracker.SendMessage(&refs) == B_OK)
		return B_OK;
	status_t status = be_roster->Launch(&ref);
	return status == B_ALREADY_RUNNING ? B_OK : status;
}
