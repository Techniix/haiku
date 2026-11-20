/*
 * Copyright (C) 2010 Stephan Aßmus <superstippi@gmx.de>
 *
 * All rights reserved. Distributed under the terms of the MIT License.
 */
#ifndef BROWSING_HISTORY_H
#define BROWSING_HISTORY_H

#include "DateTime.h"
#include <List.h>
#include <Locker.h>
#include <OS.h>
#include <String.h>

#include <map>

class BFile;
class BString;


class BrowsingHistoryItem {
public:
								BrowsingHistoryItem(const BString& url);
								BrowsingHistoryItem(
									const BrowsingHistoryItem& other);
								BrowsingHistoryItem(const BMessage* archive);
								~BrowsingHistoryItem();

			status_t			Archive(BMessage* archive) const;

			BrowsingHistoryItem& operator=(const BrowsingHistoryItem& other);

			bool				operator==(
									const BrowsingHistoryItem& other) const;
			bool				operator!=(
									const BrowsingHistoryItem& other) const;
			bool				operator<(
									const BrowsingHistoryItem& other) const;
			bool				operator<=(
									const BrowsingHistoryItem& other) const;
			bool				operator>(
									const BrowsingHistoryItem& other) const;
			bool				operator>=(
									const BrowsingHistoryItem& other) const;

			const BString&		URL() const { return fURL; }
			const BDateTime&	DateTime() const { return fDateTime; }
			uint32				InvokationCount() const {
									return fInvokationCount; }
			void				Invoked();

private:
			BString				fURL;
			BDateTime			fDateTime;
			uint32				fInvokationCount;
};


class BrowsingHistory : public BLocker {
public:
	static	BrowsingHistory*	DefaultInstance();

			bool				AddItem(const BrowsingHistoryItem& item);

	// Should Lock() the object when using these in some loop or so:
			int32				CountItems() const;
			BrowsingHistoryItem	HistoryItemAt(int32 index) const;
			void				Clear();

			void				SetMaxHistoryItemAge(int32 days);
			int32				MaxHistoryItemAge() const;

private:
								BrowsingHistory();
	virtual						~BrowsingHistory();

			void				_Clear();
			bool				_AddItem(const BrowsingHistoryItem& item,
									bool invoke);

			void				_LoadSettings();
			void				_SaveSettings();
			void				_ScheduleSave();
			void				_ArchiveTo(BMessage& archive);
			void				_WriteArchive(const BMessage& archive);
	static	status_t			_SaveThreadEntry(void* data);
			void				_SaveThread();
			bool				_OpenSettingsFile(BFile& file, uint32 mode);

private:
			BList				fHistoryItems;
			// Renga: the items by URL, so a visit finds its item without
			// comparing it with every other one.
			std::map<BString, BrowsingHistoryItem*> fItemsByURL;
			int32				fMaxHistoryItemAge;

	static	BrowsingHistory		sDefaultInstance;
			bool				fSettingsLoaded;

			// Renga: saving is done by a thread of its own a moment after
			// the last change, not on the window thread for every page.
			sem_id				fSaveRequest;
			thread_id			fSaveThread;
			bool				fSaveNeeded;
			bool				fQuitting;
};


#endif // BROWSING_HISTORY_H

