/*
 * Copyright (C) 2010 Stephan Aßmus <superstippi@gmx.de>
 *
 * All rights reserved. Distributed under the terms of the MIT License.
 */

#include "BrowsingHistory.h"

#include <new>
#include <stdio.h>

#include <Autolock.h>
#include <Entry.h>
#include <File.h>
#include <FindDirectory.h>
#include <Message.h>
#include <Path.h>

#include "BrowserApp.h"


BrowsingHistoryItem::BrowsingHistoryItem(const BString& url)
	:
	fURL(url),
	fDateTime(BDateTime::CurrentDateTime(B_LOCAL_TIME)),
	fInvokationCount(0)
{
}


BrowsingHistoryItem::BrowsingHistoryItem(const BrowsingHistoryItem& other)
{
	*this = other;
}


BrowsingHistoryItem::BrowsingHistoryItem(const BMessage* archive)
{
	if (!archive)
		return;
	BMessage dateTimeArchive;
	if (archive->FindMessage("date time", &dateTimeArchive) == B_OK)
		fDateTime = BDateTime(&dateTimeArchive);
	archive->FindString("url", &fURL);
	archive->FindUInt32("invokations", &fInvokationCount);
}


BrowsingHistoryItem::~BrowsingHistoryItem()
{
}


status_t
BrowsingHistoryItem::Archive(BMessage* archive) const
{
	if (!archive)
		return B_BAD_VALUE;
	BMessage dateTimeArchive;
	status_t status = fDateTime.Archive(&dateTimeArchive);
	if (status == B_OK)
		status = archive->AddMessage("date time", &dateTimeArchive);
	if (status == B_OK)
		status = archive->AddString("url", fURL.String());
	if (status == B_OK)
		status = archive->AddUInt32("invokations", fInvokationCount);
	return status;
}


BrowsingHistoryItem&
BrowsingHistoryItem::operator=(const BrowsingHistoryItem& other)
{
	if (this == &other)
		return *this;

	fURL = other.fURL;
	fDateTime = other.fDateTime;
	fInvokationCount = other.fInvokationCount;

	return *this;
}


bool
BrowsingHistoryItem::operator==(const BrowsingHistoryItem& other) const
{
	if (this == &other)
		return true;

	return fURL == other.fURL && fDateTime == other.fDateTime
		&& fInvokationCount == other.fInvokationCount;
}


bool
BrowsingHistoryItem::operator!=(const BrowsingHistoryItem& other) const
{
	return !(*this == other);
}


bool
BrowsingHistoryItem::operator<(const BrowsingHistoryItem& other) const
{
	if (this == &other)
		return false;

	return fDateTime < other.fDateTime || fURL < other.fURL;
}


bool
BrowsingHistoryItem::operator<=(const BrowsingHistoryItem& other) const
{
	return (*this == other) || (*this < other);
}


bool
BrowsingHistoryItem::operator>(const BrowsingHistoryItem& other) const
{
	if (this == &other)
		return false;

	return fDateTime > other.fDateTime || fURL > other.fURL;
}


bool
BrowsingHistoryItem::operator>=(const BrowsingHistoryItem& other) const
{
	return (*this == other) || (*this > other);
}


void
BrowsingHistoryItem::Invoked()
{
	// Eventually, we may overflow...
	uint32 count = fInvokationCount + 1;
	if (count > fInvokationCount)
		fInvokationCount = count;
	fDateTime = BDateTime::CurrentDateTime(B_LOCAL_TIME);
}


// #pragma mark - BrowsingHistory


BrowsingHistory
BrowsingHistory::sDefaultInstance;


BrowsingHistory::BrowsingHistory()
	:
	BLocker("browsing history"),
	fHistoryItems(64),
	fMaxHistoryItemAge(7),
	fSettingsLoaded(false),
	fSaveRequest(-1),
	fSaveThread(-1),
	fSaveNeeded(false),
	fQuitting(false)
{
}


BrowsingHistory::~BrowsingHistory()
{
	if (fSaveThread >= 0) {
		Lock();
		fQuitting = true;
		Unlock();
		release_sem(fSaveRequest);
		status_t result;
		wait_for_thread(fSaveThread, &result);
		delete_sem(fSaveRequest);
	}
	if (Lock()) {
		// Whatever the saver had not written yet, and the usual save at
		// quit time.
		_SaveSettings();
		Unlock();
	}
	_Clear();
}


/*static*/ BrowsingHistory*
BrowsingHistory::DefaultInstance()
{
	if (sDefaultInstance.Lock()) {
		sDefaultInstance._LoadSettings();
		sDefaultInstance.Unlock();
	}
	return &sDefaultInstance;
}


bool
BrowsingHistory::AddItem(const BrowsingHistoryItem& item)
{
	BAutolock _(this);

	return _AddItem(item, false);
}


int32
BrowsingHistory::BrowsingHistory::CountItems() const
{
	BAutolock _(const_cast<BrowsingHistory*>(this));

	return fHistoryItems.CountItems();
}


BrowsingHistoryItem
BrowsingHistory::HistoryItemAt(int32 index) const
{
	BAutolock _(const_cast<BrowsingHistory*>(this));

	BrowsingHistoryItem* existingItem = reinterpret_cast<BrowsingHistoryItem*>(
		fHistoryItems.ItemAt(index));
	if (!existingItem)
		return BrowsingHistoryItem(BString());

	return BrowsingHistoryItem(*existingItem);
}


void
BrowsingHistory::Clear()
{
	BAutolock _(this);
	_Clear();
	_SaveSettings();
}	


void
BrowsingHistory::SetMaxHistoryItemAge(int32 days)
{
	BAutolock _(this);
	if (fMaxHistoryItemAge != days) {
		fMaxHistoryItemAge = days;
		_SaveSettings();
	}
}	


int32
BrowsingHistory::MaxHistoryItemAge() const
{
	return fMaxHistoryItemAge;
}	


// #pragma mark - private


void
BrowsingHistory::_Clear()
{
	int32 count = CountItems();
	for (int32 i = 0; i < count; i++) {
		BrowsingHistoryItem* item = reinterpret_cast<BrowsingHistoryItem*>(
			fHistoryItems.ItemAtFast(i));
		delete item;
	}
	fHistoryItems.MakeEmpty();
	fItemsByURL.clear();
}


/*!	Renga: a URL already in the history is found through fItemsByURL; a
	new one is appended, so the list stays in the order the pages were
	first visited (the order the file keeps). It used to compare the new
	item with every item of the list, by URL and by date, to find both an
	existing entry and an insertion place, which made each visit cost time
	in proportion to the length of the history.
*/
bool
BrowsingHistory::_AddItem(const BrowsingHistoryItem& item, bool internal)
{
	std::map<BString, BrowsingHistoryItem*>::iterator existing
		= fItemsByURL.find(item.URL());
	if (existing != fItemsByURL.end()) {
		if (!internal) {
			existing->second->Invoked();
			_ScheduleSave();
		}
		return true;
	}

	BrowsingHistoryItem* newItem = new(std::nothrow) BrowsingHistoryItem(item);
	if (!newItem || !fHistoryItems.AddItem(newItem)) {
		delete newItem;
		return false;
	}
	try {
		fItemsByURL[newItem->URL()] = newItem;
	} catch (...) {
		fHistoryItems.RemoveItem(newItem);
		delete newItem;
		return false;
	}

	if (!internal) {
		newItem->Invoked();
		_ScheduleSave();
	}

	return true;
}


void
BrowsingHistory::_LoadSettings()
{
	if (fSettingsLoaded)
		return;

	fSettingsLoaded = true;

	BFile settingsFile;
	if (_OpenSettingsFile(settingsFile, B_READ_ONLY)) {
		BMessage settingsArchive;
		settingsArchive.Unflatten(&settingsFile);
		if (settingsArchive.FindInt32("max history item age",
				&fMaxHistoryItemAge) != B_OK) {
			fMaxHistoryItemAge = 7;
		}
		BDateTime oldestAllowedDateTime
			= BDateTime::CurrentDateTime(B_LOCAL_TIME);
		oldestAllowedDateTime.Date().AddDays(-fMaxHistoryItemAge);

		// Renga: the file holds the list in its order, so the items are
		// appended as they come (_AddItem() drops duplicates a hand-edited
		// file might have). Loading used to compare each item with every
		// item before it: quadratic time on the application thread at
		// start-up.
		BMessage historyItemArchive;
		for (int32 i = 0; settingsArchive.FindMessage("history item", i,
				&historyItemArchive) == B_OK; i++) {
			BrowsingHistoryItem item(&historyItemArchive);
			if (oldestAllowedDateTime < item.DateTime())
				_AddItem(item, true);
			historyItemArchive.MakeEmpty();
		}
	}
}


/*!	Writes the history now. Called with the object locked. */
void
BrowsingHistory::_SaveSettings()
{
	BMessage settingsArchive;
	_ArchiveTo(settingsArchive);
	_WriteArchive(settingsArchive);
	fSaveNeeded = false;
}


/*!	Called with the object locked. */
void
BrowsingHistory::_ArchiveTo(BMessage& settingsArchive)
{
	settingsArchive.AddInt32("max history item age", fMaxHistoryItemAge);
	BMessage historyItemArchive;
	int32 count = CountItems();
	for (int32 i = 0; i < count; i++) {
		BrowsingHistoryItem item = HistoryItemAt(i);
		if (item.Archive(&historyItemArchive) != B_OK)
			break;
		if (settingsArchive.AddMessage("history item",
				&historyItemArchive) != B_OK) {
			break;
		}
		historyItemArchive.MakeEmpty();
	}
}


void
BrowsingHistory::_WriteArchive(const BMessage& settingsArchive)
{
	BFile settingsFile;
	if (_OpenSettingsFile(settingsFile,
			B_CREATE_FILE | B_ERASE_FILE | B_WRITE_ONLY)) {
		settingsArchive.Flatten(&settingsFile);
	}
}


/*!	Renga: every visited page used to rewrite the whole history file on the
	browser window's thread (UpdateGlobalHistory()), so a long history made
	each page load wait for a disk write. Now a visit only marks the history
	as changed; a low-priority thread writes it kSaveDelay after the first
	change it has not written yet (later changes in that time are written
	with it), and the destructor writes what is left at quit time.
	Called with the object locked.
*/
static const bigtime_t kSaveDelay = 2000000;


void
BrowsingHistory::_ScheduleSave()
{
	fSaveNeeded = true;
	if (fSaveThread < 0) {
		fSaveRequest = create_sem(0, "history save request");
		if (fSaveRequest < 0) {
			_SaveSettings();
			return;
		}
		fSaveThread = spawn_thread(&_SaveThreadEntry, "history saver",
			B_LOW_PRIORITY, this);
		if (fSaveThread < 0 || resume_thread(fSaveThread) != B_OK) {
			delete_sem(fSaveRequest);
			fSaveRequest = -1;
			fSaveThread = -1;
			_SaveSettings();
			return;
		}
	}
	release_sem_etc(fSaveRequest, 1, B_DO_NOT_RESCHEDULE);
}


/*static*/ status_t
BrowsingHistory::_SaveThreadEntry(void* data)
{
	static_cast<BrowsingHistory*>(data)->_SaveThread();
	return B_OK;
}


void
BrowsingHistory::_SaveThread()
{
	while (acquire_sem(fSaveRequest) == B_OK) {
		// Changes in the next kSaveDelay are written with this one.
		bigtime_t until = system_time() + kSaveDelay;
		while (acquire_sem_etc(fSaveRequest, 1, B_ABSOLUTE_TIMEOUT, until)
				== B_OK) {
			BAutolock _(this);
			if (fQuitting)
				return;
		}

		BMessage settingsArchive;
		{
			BAutolock _(this);
			if (fQuitting)
				return;
			if (!fSaveNeeded)
				continue;
			_ArchiveTo(settingsArchive);
			fSaveNeeded = false;
		}
		// The disk write happens without the lock, so a page load that
		// adds an item meanwhile does not wait for it.
		_WriteArchive(settingsArchive);
	}
}


bool
BrowsingHistory::_OpenSettingsFile(BFile& file, uint32 mode)
{
	BPath path;
	if (find_directory(B_USER_SETTINGS_DIRECTORY, &path) != B_OK
		|| path.Append(kApplicationName) != B_OK
		|| path.Append("BrowsingHistory") != B_OK) {
		return false;
	}
	return file.SetTo(path.Path(), mode) == B_OK;
}

