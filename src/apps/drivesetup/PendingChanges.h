/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 */
#ifndef PENDING_CHANGES_H
#define PENDING_CHANGES_H


/*!	Renga (batch 13, roadmap F-24 GUI, first part): DriveSetup's queue of
	pending changes. With "Queue changes" on (the default), creating,
	deleting, formatting, naming a partition and a new partition table are
	not written at once: each becomes a step of a RengaDisk::Plan (the
	shared disk engine, headers/private/diskengine/DiskEngine.h), shown
	here as the engine's preview, and one Apply writes them all, with the
	engine's safety rules (never the boot disk, nothing mounted, disk
	images only unless real disks are allowed), its table backup and its
	journal. Discard forgets them.
*/


#include <GroupView.h>
#include <String.h>

#include <vector>

#include <DiskEngine.h>


class BButton;
class BStringView;
class BTextView;


enum {
	MSG_APPLY_PENDING		= 'apcg',
	MSG_DISCARD_PENDING		= 'dscg',
	MSG_QUEUE_DIALOG_DONE	= 'qdlg'
};


class PendingChangesView : public BGroupView {
public:
								PendingChangesView();

			void				SetTarget(BHandler* target);

			bool				HasChanges() const
									{ return !fOperations.empty(); }
			int32				CountChanges() const
									{ return fOperations.size(); }
			const BString&		DevicePath() const
									{ return fDevicePath; }

	//!	Queues \a operation for the disk at \a devicePath; false (and
	//!	\a why) when changes for another disk are pending.
			bool				Add(const char* devicePath,
									const char* diskName,
									const RengaDisk::Operation& operation,
									BString& why);
			void				Clear();

			void				SetAllowRealDisks(bool allow)
									{ fAllowRealDisks = allow; }

			BString				Preview() const;
			status_t			Check(BString& why) const;
	//!	Backs the table up into the user's Renga settings
	//!	(disk-backups) and applies every step; \a log gets the engine's
	//!	lines.
			status_t			Apply(BString& log);

private:
			RengaDisk::Plan		_Plan() const;
			void				_Update();

			BString				fDevicePath;
			BString				fDiskName;
			std::vector<RengaDisk::Operation> fOperations;
			bool				fAllowRealDisks;

			BStringView*		fTitle;
			BTextView*			fText;
			BButton*			fApplyButton;
			BButton*			fDiscardButton;
};


/*!	A small dialog for the queued steps that need text: a name, and for a
	new partition its size. Posts \a message to \a target with "name" and
	"size" (bytes, 0: the free space) added.
*/
class QueueDialog;
void show_queue_dialog(const char* title, bool askSize,
	const char* defaultName, BMessage* message, BHandler* target);


#endif	// PENDING_CHANGES_H
