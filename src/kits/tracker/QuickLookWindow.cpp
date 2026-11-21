/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 */


#include "QuickLookWindow.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#include <Autolock.h>
#include <Bitmap.h>
#include <Catalog.h>
#include <ControlLook.h>
#include <Directory.h>
#include <File.h>
#include <IconUtils.h>
#include <LayoutBuilder.h>
#include <Locker.h>
#include <MessageFilter.h>
#include <Mime.h>
#include <Node.h>
#include <NodeInfo.h>
#include <Screen.h>
#include <ScrollView.h>
#include <StringForSize.h>
#include <StringView.h>
#include <TextView.h>
#include <TranslationUtils.h>

#include "FolderSizeCache.h"


#undef B_TRANSLATION_CONTEXT
#define B_TRANSLATION_CONTEXT "QuickLook"


static const off_t kMaxText = 64 * 1024;

static BLocker sLock("quick look");
static QuickLookWindow* sWindow = NULL;
static entry_ref sShown;
static BString sKind;


// #pragma mark - views


//!	A bitmap scaled to fit, centred, aspect kept.
class PictureView : public BView {
public:
	PictureView(BBitmap* bitmap, float iconSize)
		:
		BView("picture", B_WILL_DRAW | B_FULL_UPDATE_ON_RESIZE),
		fBitmap(bitmap)
	{
		SetViewUIColor(B_PANEL_BACKGROUND_COLOR);
		BRect bounds = bitmap->Bounds();
		float maxWidth = 720;
		float maxHeight = 540;
		float scale = std::min(1.0f, std::min(maxWidth / (bounds.Width() + 1),
			maxHeight / (bounds.Height() + 1)));
		float width = std::max(iconSize, (bounds.Width() + 1) * scale);
		float height = std::max(iconSize, (bounds.Height() + 1) * scale);
		SetExplicitPreferredSize(BSize(width, height));
		SetExplicitMinSize(BSize(std::min(width, 160.0f),
			std::min(height, 120.0f)));
	}

	~PictureView()
	{
		delete fBitmap;
	}

	virtual void Draw(BRect)
	{
		BRect bounds = Bounds();
		BRect source = fBitmap->Bounds();
		float scale = std::min((bounds.Width() + 1) / (source.Width() + 1),
			(bounds.Height() + 1) / (source.Height() + 1));
		if (scale > 1 && source.Width() > 64)
			scale = 1;
		float width = (source.Width() + 1) * scale;
		float height = (source.Height() + 1) * scale;
		BRect target(0, 0, width - 1, height - 1);
		target.OffsetTo((bounds.Width() - width) / 2,
			(bounds.Height() - height) / 2);
		SetDrawingMode(B_OP_ALPHA);
		DrawBitmap(fBitmap, source, target, B_FILTER_BITMAP_BILINEAR);
	}

private:
	BBitmap*	fBitmap;
};


//!	Space and Escape close Quick Look, also from inside the text view.
class CloseKeyFilter : public BMessageFilter {
public:
	CloseKeyFilter()
		:
		BMessageFilter(B_KEY_DOWN)
	{
	}

	virtual filter_result Filter(BMessage* message, BHandler** target)
	{
		const char* bytes;
		if (message->FindString("bytes", &bytes) == B_OK
			&& (bytes[0] == ' ' || bytes[0] == B_ESCAPE)) {
			QuickLookWindow::Close();
			return B_SKIP_MESSAGE;
		}
		return B_DISPATCH_MESSAGE;
	}
};


static BString
format_time(time_t time)
{
	char buffer[64];
	struct tm tm;
	localtime_r(&time, &tm);
	strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M", &tm);
	return buffer;
}


// #pragma mark - QuickLookWindow


QuickLookWindow::QuickLookWindow(const entry_ref& ref)
	:
	BWindow(BRect(0, 0, 100, 100), ref.name, B_FLOATING_WINDOW_LOOK,
		B_FLOATING_APP_WINDOW_FEEL, B_NOT_ZOOMABLE | B_AUTO_UPDATE_SIZE_LIMITS
			| B_ASYNCHRONOUS_CONTROLS),
	fRef(ref),
	fKind("info")
{
	AddCommonFilter(new CloseKeyFilter());
	_Build();
	CenterOnScreen();
}


QuickLookWindow::~QuickLookWindow()
{
}


void
QuickLookWindow::_Build()
{
	BEntry entry(&fRef, true);
	struct stat st;
	entry.GetStat(&st);
	BNode node(&entry);
	BNodeInfo info(&node);
	char type[B_MIME_TYPE_LENGTH] = "";
	info.GetType(type);
	BMimeType mime(type);
	BMimeType super;
	mime.GetSupertype(&super);

	float iconSize = be_control_look->ComposeIconSize(B_LARGE_ICON).Width()
		+ 1;
	BView* content = NULL;

	// a description: kind, size, dates
	char kindName[B_MIME_TYPE_LENGTH] = "";
	if (mime.GetShortDescription(kindName) != B_OK)
		strlcpy(kindName, type, sizeof(kindName));
	BString details;
	char sizeText[64];
	if (S_ISDIR(st.st_mode)) {
		BDirectory directory(&entry);
		int32 count = directory.CountEntries();
		node_ref folder;
		entry.GetNodeRef(&folder);
		off_t size;
		BString sizeString;
		if (FolderSizeCache::Default()->Lookup(folder, &size)) {
			string_for_size(size, sizeText, sizeof(sizeText));
			sizeString = sizeText;
		} else
			sizeString = B_TRANSLATE("size being computed");
		details.SetToFormat(B_TRANSLATE("Folder, %d items, %s"), (int)count,
			sizeString.String());
		fKind = "folder";
	} else {
		string_for_size(st.st_size, sizeText, sizeof(sizeText));
		details.SetToFormat("%s, %s", kindName[0] != '\0' ? kindName
			: B_TRANSLATE("File"), sizeText);
	}
	details << "\n" << B_TRANSLATE("Modified:") << " "
		<< format_time(st.st_mtime) << "   " << B_TRANSLATE("Created:") << " "
		<< format_time(st.st_crtime);

	if (!S_ISDIR(st.st_mode) && strcmp(super.Type(), "image") == 0) {
		BBitmap* bitmap = BTranslationUtils::GetBitmap(&fRef);
		if (bitmap != NULL) {
			BRect bounds = bitmap->Bounds();
			details.Prepend(BString().SetToFormat("%d \xC3\x97 %d, ",
				(int)bounds.Width() + 1, (int)bounds.Height() + 1));
			content = new PictureView(bitmap, iconSize);
			fKind = "image";
		}
	}

	if (content == NULL && !S_ISDIR(st.st_mode)
		&& (strcmp(super.Type(), "text") == 0 || strcmp(type,
			"application/x-sh") == 0 || strcmp(type, "application/json")
				== 0 || strcmp(type, "application/xml") == 0)) {
		BFile file(&entry, B_READ_ONLY);
		off_t length = std::min(st.st_size, kMaxText);
		char* buffer = new char[length + 1];
		ssize_t read = file.Read(buffer, length);
		if (read >= 0) {
			buffer[read] = '\0';
			BTextView* text = new BTextView("text");
			text->SetText(buffer);
			text->MakeEditable(false);
			BFont font(be_fixed_font);
			text->SetFontAndColor(&font);
			if (st.st_size > kMaxText) {
				text->Insert(text->TextLength(), B_TRANSLATE("\n\n(The rest "
					"is not shown.)"), strlen(B_TRANSLATE("\n\n(The rest "
					"is not shown.)")));
			}
			BScrollView* scroll = new BScrollView("scroll", text, 0, true,
				true);
			scroll->SetExplicitPreferredSize(BSize(560, 380));
			content = scroll;
			fKind = "text";
		}
		delete[] buffer;
	}

	if (content == NULL) {
		// the large icon
		BBitmap* icon = new BBitmap(BRect(0, 0, iconSize * 2 - 1,
			iconSize * 2 - 1), B_RGBA32);
		if (BNodeInfo::GetTrackerIcon(&fRef, icon,
				(icon_size)(int32)(iconSize * 2)) != B_OK) {
			delete icon;
			icon = NULL;
		}
		if (icon != NULL)
			content = new PictureView(icon, iconSize * 2);
	}

	BStringView* name = new BStringView("name", fRef.name);
	BFont bold(be_bold_font);
	name->SetFont(&bold);
	// two lines: what it is, and its dates
	BString firstLine(details);
	BString secondLine;
	int32 newline = details.FindFirst('\n');
	if (newline >= 0) {
		firstLine.Truncate(newline);
		details.CopyInto(secondLine, newline + 1,
			details.Length() - newline - 1);
	}
	BStringView* detailView = new BStringView("details", firstLine);
	BStringView* datesView = new BStringView("dates", secondLine);
	BFont small(be_plain_font);
	small.SetSize(small.Size() * 0.9f);
	datesView->SetFont(&small);

	BLayoutBuilder::Group<> builder(this, B_VERTICAL, B_USE_SMALL_SPACING);
	builder.SetInsets(B_USE_SMALL_SPACING);
	if (content != NULL)
		builder.Add(content);
	builder.Add(name).Add(detailView).Add(datesView);
}


/*static*/ void
QuickLookWindow::Toggle(const entry_ref& ref)
{
	BAutolock _(sLock);
	if (sWindow != NULL) {
		bool same = sShown == ref;
		sWindow->PostMessage(B_QUIT_REQUESTED);
		sWindow = NULL;
		if (same)
			return;
	}
	sWindow = new QuickLookWindow(ref);
	sShown = ref;
	sKind = sWindow->fKind;
	sWindow->Show();
}


/*static*/ void
QuickLookWindow::Close()
{
	BAutolock _(sLock);
	if (sWindow != NULL) {
		sWindow->PostMessage(B_QUIT_REQUESTED);
		sWindow = NULL;
	}
}


/*static*/ void
QuickLookWindow::GetState(BMessage* reply)
{
	BAutolock _(sLock);
	reply->AddBool("open", sWindow != NULL);
	if (sWindow != NULL) {
		reply->AddString("title", sShown.name);
		reply->AddString("kind", sKind);
	}
}


void
QuickLookWindow::MessageReceived(BMessage* message)
{
	BWindow::MessageReceived(message);
}


bool
QuickLookWindow::QuitRequested()
{
	BAutolock _(sLock);
	if (sWindow == this)
		sWindow = NULL;
	return true;
}
