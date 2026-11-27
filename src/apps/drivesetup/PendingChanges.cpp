/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 */


#include "PendingChanges.h"

#include <Alert.h>
#include <Button.h>
#include <Catalog.h>
#include <FindDirectory.h>
#include <GridView.h>
#include <LayoutBuilder.h>
#include <Path.h>
#include <ScrollView.h>
#include <StringView.h>
#include <TextControl.h>
#include <TextView.h>
#include <Window.h>

#include <sys/stat.h>


#undef B_TRANSLATION_CONTEXT
#define B_TRANSLATION_CONTEXT "PendingChanges"


PendingChangesView::PendingChangesView()
	:
	BGroupView("pending changes", B_VERTICAL, B_USE_SMALL_SPACING),
	fAllowRealDisks(false)
{
	fTitle = new BStringView("title", "");
	fText = new BTextView("pending");
	fText->MakeEditable(false);
	fText->SetWordWrap(true);
	BScrollView* scroll = new BScrollView("pending scroll", fText, 0, false,
		true);
	float lines = ceilf(be_plain_font->Size() * 1.4f) * 4;
	scroll->SetExplicitMinSize(BSize(B_SIZE_UNSET, lines));
	scroll->SetExplicitPreferredSize(BSize(B_SIZE_UNSET, lines));

	fApplyButton = new BButton("apply", B_TRANSLATE("Apply changes"),
		new BMessage(MSG_APPLY_PENDING));
	fDiscardButton = new BButton("discard", B_TRANSLATE("Discard"),
		new BMessage(MSG_DISCARD_PENDING));

	BLayoutBuilder::Group<>(this)
		.SetInsets(B_USE_WINDOW_SPACING, B_USE_SMALL_SPACING)
		.Add(fTitle)
		.Add(scroll)
		.AddGroup(B_HORIZONTAL)
			.AddGlue()
			.Add(fDiscardButton)
			.Add(fApplyButton)
		.End();
	_Update();
}


void
PendingChangesView::SetTarget(BHandler* target)
{
	fApplyButton->SetTarget(target);
	fDiscardButton->SetTarget(target);
}


bool
PendingChangesView::Add(const char* devicePath, const char* diskName,
	const RengaDisk::Operation& operation, BString& why)
{
	if (!fOperations.empty() && fDevicePath != devicePath) {
		why.SetToFormat(B_TRANSLATE("Changes for %s are pending. Apply or "
			"discard them first."), fDiskName.String());
		return false;
	}
	fDevicePath = devicePath;
	fDiskName = diskName;
	fOperations.push_back(operation);
	_Update();
	return true;
}


void
PendingChangesView::Clear()
{
	fOperations.clear();
	_Update();
}


RengaDisk::Plan
PendingChangesView::_Plan() const
{
	RengaDisk::Plan plan(fDevicePath.String());
	plan.AllowRealDisks(fAllowRealDisks);
	for (size_t i = 0; i < fOperations.size(); i++)
		plan.Add(fOperations[i]);
	return plan;
}


BString
PendingChangesView::Preview() const
{
	return fOperations.empty() ? BString() : _Plan().Preview();
}


status_t
PendingChangesView::Check(BString& why) const
{
	return _Plan().Check(why);
}


status_t
PendingChangesView::Apply(BString& log)
{
	BPath backups;
	status_t status = find_directory(B_USER_SETTINGS_DIRECTORY, &backups,
		true);
	if (status != B_OK)
		return status;
	backups.Append("Renga/disk-backups");
	mkdir(backups.Path(), 0700);
	BString backupPath;
	status = _Plan().Apply(backups.Path(), log, &backupPath);
	if (backupPath.Length() > 0)
		log << "\n" << B_TRANSLATE("Partition table backup:") << " "
			<< backupPath;
	return status;
}


void
PendingChangesView::_Update()
{
	if (fOperations.empty()) {
		fTitle->SetText(B_TRANSLATE("Pending changes: none. Changes are "
			"queued here and written together with \"Apply changes\"."));
		fText->SetText("");
	} else {
		BString title;
		title.SetToFormat(B_TRANSLATE("Pending changes for %s (%d), nothing "
			"is written until you apply them:"), fDiskName.String(),
			(int)fOperations.size());
		fTitle->SetText(title.String());
		fText->SetText(Preview().String());
	}
	fApplyButton->SetEnabled(!fOperations.empty());
	fDiscardButton->SetEnabled(!fOperations.empty());
}


// #pragma mark - QueueDialog


class QueueDialog : public BWindow {
public:
	QueueDialog(const char* title, bool askSize, const char* defaultName,
			BMessage* message, BHandler* target)
		:
		BWindow(BRect(0, 0, 300, 100), title, B_MODAL_WINDOW_LOOK,
			B_MODAL_APP_WINDOW_FEEL, B_NOT_RESIZABLE | B_NOT_ZOOMABLE
				| B_AUTO_UPDATE_SIZE_LIMITS | B_CLOSE_ON_ESCAPE),
		fMessage(message),
		fTarget(target),
		fSize(NULL)
	{
		fName = new BTextControl(B_TRANSLATE("Name:"), defaultName, NULL);
		if (askSize) {
			fSize = new BTextControl(B_TRANSLATE("Size:"), "", NULL);
			fSize->SetToolTip(B_TRANSLATE("For example 512M or 4G; empty: "
				"all of the free space"));
		}
		BButton* ok = new BButton(B_TRANSLATE("Queue"), new BMessage('ok  '));
		BButton* cancel = new BButton(B_TRANSLATE("Cancel"),
			new BMessage(B_QUIT_REQUESTED));
		BGridView* fields = new BGridView();
		BLayoutBuilder::Grid<>(fields)
			.AddTextControl(fName, 0, 0);
		if (fSize != NULL) {
			BLayoutBuilder::Grid<>(fields)
				.AddTextControl(fSize, 0, 1);
		}
		BLayoutBuilder::Group<>(this, B_VERTICAL)
			.SetInsets(B_USE_WINDOW_SPACING)
			.Add(fields)
			.AddGroup(B_HORIZONTAL)
				.AddGlue()
				.Add(cancel)
				.Add(ok)
			.End();
		SetDefaultButton(ok);
		fName->MakeFocus(true);
		CenterOnScreen();
	}

	~QueueDialog()
	{
		delete fMessage;
	}

	virtual void MessageReceived(BMessage* message)
	{
		if (message->what != 'ok  ') {
			BWindow::MessageReceived(message);
			return;
		}
		off_t size = 0;
		if (fSize != NULL && fSize->Text()[0] != '\0') {
			size = RengaDisk::ParseSize(fSize->Text());
			if (size <= 0) {
				BAlert* alert = new BAlert("size", B_TRANSLATE("The size is "
					"not valid. Use for example 512M or 4G, or leave it "
					"empty for all of the free space."), B_TRANSLATE("OK"));
				alert->SetFlags(alert->Flags() | B_CLOSE_ON_ESCAPE);
				alert->Go();
				return;
			}
		}
		fMessage->AddString("name", fName->Text());
		fMessage->AddInt64("size", size);
		BMessenger(fTarget).SendMessage(fMessage);
		Quit();
	}

private:
	BMessage*		fMessage;
	BHandler*		fTarget;
	BTextControl*	fName;
	BTextControl*	fSize;
};


void
show_queue_dialog(const char* title, bool askSize, const char* defaultName,
	BMessage* message, BHandler* target)
{
	(new QueueDialog(title, askSize, defaultName, message, target))->Show();
}
