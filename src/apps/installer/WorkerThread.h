/*
 * Copyright 2009, Stephan Aßmus <superstippi@gmx.de>.
 * Copyright 2005, Jérôme DUVAL.
 * All rights reserved. Distributed under the terms of the MIT License.
 */
#ifndef WORKER_THREAD_H
#define WORKER_THREAD_H

#include <DiskDevice.h>
#include <DiskDeviceRoster.h>
#include <Looper.h>
#include <Messenger.h>
#include <Partition.h>
#include <String.h>
#include <Volume.h>

#include "InstallationInfo.h"


class BList;
class BMenu;
class ProgressReporter;

class WorkerThread : public BLooper {
public:
								WorkerThread(const BMessenger& owner);

	virtual	void				MessageReceived(BMessage* message);

			void 				InstallEFILoader(partition_id id, bool rename);

			void				ScanDisksPartitions(BMenu* srcMenu,
									BMenu* dstMenu, BMenu* EFIMenu);

			void				SetPackagesList(BList* list);
			void				SetSpaceRequired(off_t bytes)
									{ fSpaceRequired = bytes; };

			bool				Cancel();
			void				SetLock(sem_id cancelSemaphore)
									{ fCancelSemaphore = cancelSemaphore; }

			void				StartInstall(partition_id sourcePartitionID,
									partition_id targetPartitionID);
			void				WriteBootSector(BMenu* dstMenu);

			// Renga (F-23): what to do with a destination that is not
			// empty. INSTALL_MODE_ASK shows the choice (the window);
			// an unattended install sets one. With SetUnattended() no
			// alert is shown: whatever an alert would ask is refused.
			void				SetInstallMode(install_mode mode,
									const char* eraseConfirmation = NULL);
			void				SetUnattended(bool unattended)
									{ fUnattended = unattended; }
			// F-59: after an upgrade, rename the volume to DISTRO_NAME:
			// -1 (default) if its name is exactly "Haiku", 0 no, 1 yes
			void				SetRenameVolume(int32 rename)
									{ fRenameVolume = rename; }

private:
			status_t			_WriteBootSector(BPath& path);
			status_t			_LaunchFinishScript(BPath& path);

			status_t			_PerformInstall(partition_id sourcePartitionID,
									partition_id targetPartitionID);
			status_t			_PrepareCleanInstall(
									const BPath& targetDirectory) const;

			// Renga in-place replacement (WorkerThreadModes.cpp)
			install_mode		_ChooseMode(const BPath& targetDirectory,
									const InstallationInfo& existing,
									bool targetEmpty);
			bool				_ConfirmErase(const BPath& targetDirectory);
			status_t			_UpgradePackages(const BPath& source,
									const BPath& target);
			status_t			_RemoveSystemPackages(
									const BPath& target);
			status_t			_EraseVolume(const BPath& target);
			status_t			_WriteActivatedPackages(
									const BPath& target);
			status_t			_UpdateEFILoaders(partition_id disk);
			void				_Log(const char* format, ...);
			void				_RenameVolume(const BPath& target);
			void				_SaveLog(const BPath& target);
			status_t			_InstallationError(status_t error);
			status_t			_MirrorIndices(const BPath& srcDirectory,
									const BPath& targetDirectory) const;
			status_t			_CreateDefaultIndices(
									const BPath& targetDirectory) const;
			status_t			_ProcessZipPackages(const char* sourcePath,
									const char* targetPath,
									ProgressReporter* reporter,
									BList& unzipEngines);

			void				_SetStatusMessage(const char* status);

private:
			class EntryFilter;
			friend class EntryFilter;

private:
			BMessenger			fOwner;
			BDiskDeviceRoster	fDDRoster;
			BList*				fPackages;
			off_t				fSpaceRequired;
			sem_id				fCancelSemaphore;
			install_mode		fMode;
			BString				fEraseConfirmation;
			bool				fUnattended;
			int32				fRenameVolume;
			bool				fHadInstallerLink;
			BString				fLogText;
};

#endif // WORKER_THREAD_H
