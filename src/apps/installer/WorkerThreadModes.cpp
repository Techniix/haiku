/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 */


/*!	Installing over an existing installation (Renga, roadmap F-23, early
	part): the three modes, the package swap that keeps the old system as a
	package state, erasing, and updating the EFI loaders that are Haiku's.

	Upgrade (INSTALL_MODE_UPGRADE), in this order, so that a failure leaves
	the installation as it was:

	1. Renga's packages (the source's system/packages) are copied into a
	   staging directory on the destination,
	   system/packages/administrative/renga-upgrade-new. If that fails (no
	   space, I/O error) the staging directory is removed and nothing else
	   has changed.
	2. A package state directory is made, administrative/state_<date>_
	   <time>, named like package_daemon names its states. Its
	   activated-packages file is the installation's current one (or, if
	   it has none, the list of its packages): that is the old system.
	3. Every package of the installation that Renga has a package of the
	   same name for (the system packages, and user-installed packages
	   Renga ships its own version of) is moved into the state directory.
	   Packages Renga does not have stay where they are. A failure moves
	   everything back.
	4. The staged packages are moved into system/packages.
	5. After the rest of the files are copied, administrative/
	   activated-packages lists the packages now in system/packages.

	An old state's packages are found by packagefs and the boot loader in
	its own state directory, newer state directories and system/packages,
	so the new state directory makes the old system bootable from the boot
	menu ("Select boot volume/state"), and the installation's older states
	keep working. Moving files within a volume is a rename: no data is
	copied, so the old system costs no additional space.

	The rest of the source (home, system/settings and the like) is then
	copied without overwriting anything: only what the destination does not
	have is added. A folder that exists keeps its attributes too.
*/


#include "WorkerThread.h"

#include <errno.h>
#include <stdarg.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <syslog.h>
#include <time.h>

#include <Alert.h>
#include <Catalog.h>
#include <Directory.h>
#include <DiskDeviceTypes.h>
#include <DiskDeviceVisitor.h>
#include <Entry.h>
#include <File.h>
#include <FindDirectory.h>
#include <Path.h>
#include <String.h>
#include <StringList.h>
#include <Volume.h>

#include <vector>

#include <fs_volume.h>

#include "CopyEngine.h"
#include "InstallModeWindow.h"


#undef B_TRANSLATION_CONTEXT
#define B_TRANSLATION_CONTEXT "InstallProgress"


static const char* kStagingDirectory = "renga-upgrade-new";
static const char* kActivationFile = "activated-packages";


void
WorkerThread::SetInstallMode(install_mode mode, const char* eraseConfirmation)
{
	fMode = mode;
	fEraseConfirmation = eraseConfirmation != NULL ? eraseConfirmation : "";
}


/*!	Logs a step of an in-place installation: on standard output (the
	command line mode), in the syslog, and in the log the installation
	leaves on the destination (system/var/log/installer.log, written at the
	end).
*/
void
WorkerThread::_Log(const char* format, ...)
{
	char text[1024];
	va_list args;
	va_start(args, format);
	vsnprintf(text, sizeof(text), format, args);
	va_end(args);

	printf("Installer: %s\n", text);
	fflush(stdout);
	syslog(LOG_INFO, "Installer: %s", text);
	fLogText << text << "\n";
}


void
WorkerThread::_SaveLog(const BPath& target)
{
	if (fLogText.IsEmpty() || target.InitCheck() != B_OK)
		return;
	BPath directory(target.Path(), "system/var/log");
	if (create_directory(directory.Path(), 0755) != B_OK)
		return;
	BPath path(directory.Path(), "installer.log");
	BFile file(path.Path(), B_WRITE_ONLY | B_CREATE_FILE | B_OPEN_AT_END);
	if (file.InitCheck() == B_OK) {
		char stamp[64];
		time_t now = time(NULL);
		strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", localtime(&now));
		BString header;
		header << "--- " << stamp << "\n";
		file.Write(header.String(), header.Length());
		file.Write(fLogText.String(), fLogText.Length());
	}
}


install_mode
WorkerThread::_ChooseMode(const BPath& targetDirectory,
	const InstallationInfo& existing, bool targetEmpty)
{
	if (targetEmpty)
		return INSTALL_MODE_ASK;
	if (fMode != INSTALL_MODE_ASK)
		return fMode;
	if (fUnattended) {
		_Log("the destination is not empty and no mode was given");
		return INSTALL_MODE_CANCEL;
	}

	BVolume volume;
	char name[B_FILE_NAME_LENGTH] = "";
	if (volume.SetTo(dev_for_path(targetDirectory.Path())) == B_OK)
		volume.GetName(name);
	InstallModeWindow* window = new InstallModeWindow(name, existing);
	return window->Go();
}


/*!	The second confirmation of "Erase the volume and install", naming the
	volume. Unattended, the name has to be given (--confirm-erase).
*/
bool
WorkerThread::_ConfirmErase(const BPath& targetDirectory)
{
	BVolume volume;
	char name[B_FILE_NAME_LENGTH] = "";
	if (volume.SetTo(dev_for_path(targetDirectory.Path())) != B_OK
		|| volume.GetName(name) != B_OK || name[0] == '\0')
		return false;

	if (fUnattended) {
		if (fEraseConfirmation != name) {
			_Log("not erasing \"%s\": the confirmation names \"%s\"", name,
				fEraseConfirmation.String());
			return false;
		}
		return true;
	}

	char size[32] = "";
	off_t capacity = volume.Capacity();
	snprintf(size, sizeof(size), "%.1f GiB",
		capacity / (1024.0 * 1024 * 1024));
	BString text;
	text.SetToFormat(B_TRANSLATE("Erase everything on \"%s\" (%s, mounted at "
		"%s)?\n\nAll files, settings and installed software on this volume "
		"will be deleted. This cannot be undone."), name, size,
		targetDirectory.Path());
	BString eraseLabel;
	eraseLabel.SetToFormat(B_TRANSLATE("Erase \"%s\""), name);
	BAlert* alert = new BAlert(B_TRANSLATE("Erase volume"), text,
		B_TRANSLATE("Cancel"), eraseLabel, NULL, B_WIDTH_AS_USUAL,
		B_STOP_ALERT);
	alert->SetShortcut(0, B_ESCAPE);
	return alert->Go() == 1;
}


static status_t
read_lines(const char* path, BStringList& lines)
{
	lines.MakeEmpty();
	BFile file(path, B_READ_ONLY);
	off_t size;
	status_t status = file.InitCheck();
	if (status == B_OK)
		status = file.GetSize(&size);
	if (status != B_OK)
		return status;
	if (size > 10 * 1024 * 1024)
		return B_BAD_DATA;
	BString text;
	char* buffer = text.LockBuffer(size + 1);
	ssize_t bytesRead = file.Read(buffer, size);
	text.UnlockBuffer(bytesRead > 0 ? bytesRead : 0);
	text.Split("\n", true, lines);
	return B_OK;
}


static status_t
write_lines(const char* path, const BStringList& lines)
{
	BString text;
	for (int32 i = 0; i < lines.CountStrings(); i++)
		text << lines.StringAt(i) << "\n";
	BString temporary(path);
	temporary << ".new";
	BFile file(temporary.String(), B_WRITE_ONLY | B_CREATE_FILE
		| B_ERASE_FILE);
	status_t status = file.InitCheck();
	if (status != B_OK)
		return status;
	if (file.Write(text.String(), text.Length()) != text.Length())
		return B_IO_ERROR;
	file.Sync();
	return BEntry(temporary.String()).Rename(path, true);
}


/*!	The name package_daemon gives a state directory, for now; unique in
	\a administrative.
*/
static BString
new_state_name(const BPath& administrative)
{
	time_t now = time(NULL);
	struct tm local;
	localtime_r(&now, &local);
	BString base;
	base.SetToFormat("state_%d-%02d-%02d_%02d:%02d:%02d",
		1900 + local.tm_year, local.tm_mon + 1, local.tm_mday, local.tm_hour,
		local.tm_min, local.tm_sec);
	BString name = base;
	for (int i = 1; BEntry(BPath(administrative.Path(), name).Path())
			.Exists(); i++) {
		name.SetToFormat("%s-%d", base.String(), i);
	}
	return name;
}


status_t
WorkerThread::_UpgradePackages(const BPath& source, const BPath& target)
{
	BPath sourcePackages(source.Path(), "system/packages");
	BPath targetPackages(target.Path(), "system/packages");
	BPath administrative(targetPackages.Path(), "administrative");
	BPath staging(administrative.Path(), kStagingDirectory);
	create_directory(administrative.Path(), 0755);

	BStringList sourceFiles;
	BStringList targetFiles;
	status_t status = list_package_files(sourcePackages.Path(), sourceFiles);
	if (status == B_OK)
		status = list_package_files(targetPackages.Path(), targetFiles);
	if (status != B_OK || sourceFiles.IsEmpty()) {
		_Log("cannot list the packages (%s)", strerror(status));
		return status != B_OK ? status : B_ENTRY_NOT_FOUND;
	}

	BStringList sourceNames;
	off_t bytesNeeded = 0;
	for (int32 i = 0; i < sourceFiles.CountStrings(); i++) {
		sourceNames.Add(package_name_of_file(sourceFiles.StringAt(i)));
		BEntry entry(BPath(sourcePackages.Path(), sourceFiles.StringAt(i))
			.Path());
		off_t size;
		if (entry.GetSize(&size) == B_OK)
			bytesNeeded += size;
	}

	// Enough room for the new packages (the old ones stay, as the old
	// state), with some to spare for everything else.
	BVolume volume(dev_for_path(target.Path()));
	off_t spare = 64 * 1024 * 1024;
	if (volume.InitCheck() == B_OK
		&& volume.FreeBytes() < bytesNeeded + spare) {
		_Log("not enough space: %" B_PRIdOFF " MiB free, %" B_PRIdOFF " MiB "
			"needed; nothing was changed", volume.FreeBytes() / 1048576,
			(bytesNeeded + spare) / 1048576);
		_SetStatusMessage(B_TRANSLATE("Not enough free space on the "
			"destination for the upgrade. Nothing was changed."));
		return B_DEVICE_FULL;
	}

	// 1. stage the new packages
	BEntry stagingEntry(staging.Path());
	if (stagingEntry.Exists()) {
		_Log("removing an earlier, unfinished staging directory");
		CopyEngine::RemoveFolder(stagingEntry);
	}
	status = create_directory(staging.Path(), 0755);
	if (status != B_OK)
		return status;

	_SetStatusMessage(B_TRANSLATE("Copying the new system packages."));
	CopyEngine stager(NULL, NULL);
	for (int32 i = 0; i < sourceFiles.CountStrings(); i++) {
		BPath from(sourcePackages.Path(), sourceFiles.StringAt(i));
		BPath to(staging.Path(), sourceFiles.StringAt(i));
		status = stager.Copy(from.Path(), to.Path(), fCancelSemaphore);
		if (status != B_OK) {
			_Log("copying %s failed (%s); nothing was changed",
				sourceFiles.StringAt(i).String(), strerror(status));
			stagingEntry.SetTo(staging.Path());
			CopyEngine::RemoveFolder(stagingEntry);
			return status;
		}
	}
	_Log("staged %" B_PRId32 " packages (%" B_PRIdOFF " MiB)",
		sourceFiles.CountStrings(), bytesNeeded / 1048576);

	// 2. the old system becomes a package state
	BString stateName = new_state_name(administrative);
	BPath state(administrative.Path(), stateName);
	status = create_directory(state.Path(), 0755);
	BStringList oldActivated;
	BPath activation(administrative.Path(), kActivationFile);
	if (status == B_OK && read_lines(activation.Path(), oldActivated) != B_OK)
		oldActivated = targetFiles;
	if (status == B_OK) {
		status = write_lines(BPath(state.Path(), kActivationFile).Path(),
			oldActivated);
	}
	if (status != B_OK) {
		_Log("cannot create the state directory %s (%s); nothing was "
			"changed", stateName.String(), strerror(status));
		BEntry(state.Path()).Remove();
		stagingEntry.SetTo(staging.Path());
		CopyEngine::RemoveFolder(stagingEntry);
		return status;
	}

	// 3. move the packages Renga replaces into it
	BDirectory stateDirectory(state.Path());
	BStringList moved;
	BStringList kept;
	for (int32 i = 0; i < targetFiles.CountStrings(); i++) {
		BString file = targetFiles.StringAt(i);
		if (!sourceNames.HasString(package_name_of_file(file))) {
			kept.Add(file);
			continue;
		}
		BEntry entry(BPath(targetPackages.Path(), file).Path());
		status = entry.MoveTo(&stateDirectory);
		if (status != B_OK) {
			_Log("moving %s into %s failed (%s); undoing", file.String(),
				stateName.String(), strerror(status));
			BDirectory packagesDirectory(targetPackages.Path());
			for (int32 j = 0; j < moved.CountStrings(); j++) {
				BEntry back(BPath(state.Path(), moved.StringAt(j)).Path());
				back.MoveTo(&packagesDirectory);
			}
			BEntry(BPath(state.Path(), kActivationFile).Path()).Remove();
			BEntry(state.Path()).Remove();
			stagingEntry.SetTo(staging.Path());
			CopyEngine::RemoveFolder(stagingEntry);
			return status;
		}
		moved.Add(file);
	}

	// 4. the new packages take their place
	BDirectory packagesDirectory(targetPackages.Path());
	for (int32 i = 0; i < sourceFiles.CountStrings(); i++) {
		BEntry entry(BPath(staging.Path(), sourceFiles.StringAt(i)).Path());
		status = entry.MoveTo(&packagesDirectory, NULL, false);
		if (status != B_OK) {
			// A user package with the very same file name as one of
			// Renga's would be in the state directory now; anything else
			// is an I/O error. The old state is complete and bootable.
			_Log("moving the new %s into place failed (%s)",
				sourceFiles.StringAt(i).String(), strerror(status));
			return status;
		}
	}
	BEntry(staging.Path()).Remove();

	_Log("old system kept as package state %s (%" B_PRId32 " packages "
		"moved there)", stateName.String(), moved.CountStrings());
	for (int32 i = 0; i < kept.CountStrings(); i++)
		_Log("kept installed package %s", kept.StringAt(i).String());
	_Log("installed %" B_PRId32 " new system packages",
		sourceFiles.CountStrings());
	return B_OK;
}


/*!	Replace mode: the installation's packages go: system/packages (with its
	states) and the package cache. Everything else stays.
*/
status_t
WorkerThread::_RemoveSystemPackages(const BPath& target)
{
	BPath packages(target.Path(), "system/packages");
	BDirectory directory(packages.Path());
	BEntry entry;
	int32 count = 0;
	while (directory.InitCheck() == B_OK
		&& directory.GetNextEntry(&entry) == B_OK) {
		status_t status = entry.IsDirectory()
			? CopyEngine::RemoveFolder(entry) : entry.Remove();
		if (status != B_OK) {
			_Log("removing %s failed: %s", BPath(&entry).Path(),
				strerror(status));
			return status;
		}
		count++;
	}
	BEntry cache(BPath(target.Path(), "system/cache").Path());
	if (cache.Exists())
		CopyEngine::RemoveFolder(cache);
	_Log("removed the installed system: %" B_PRId32 " entries of "
		"system/packages", count);
	return B_OK;
}


/*!	Erase mode, after _ConfirmErase(): everything on the volume goes.
*/
status_t
WorkerThread::_EraseVolume(const BPath& target)
{
	BDirectory directory(target.Path());
	BEntry entry;
	int32 count = 0;
	while (directory.InitCheck() == B_OK
		&& directory.GetNextEntry(&entry) == B_OK) {
		status_t status = entry.IsDirectory()
			? CopyEngine::RemoveFolder(entry) : entry.Remove();
		if (status != B_OK) {
			_Log("erasing %s failed: %s", BPath(&entry).Path(),
				strerror(status));
			return status;
		}
		count++;
		directory.Rewind();
	}
	_Log("erased %s (%" B_PRId32 " top-level entries)", target.Path(),
		count);
	return B_OK;
}


/*!	Upgrade mode, at the end: the new state's activated-packages lists the
	packages in system/packages.
*/
status_t
WorkerThread::_WriteActivatedPackages(const BPath& target)
{
	BPath packages(target.Path(), "system/packages");
	BStringList files;
	status_t status = list_package_files(packages.Path(), files);
	if (status != B_OK)
		return status;
	BPath activation(packages.Path(), "administrative");
	activation.Append(kActivationFile);
	status = write_lines(activation.Path(), files);
	_Log("activated-packages: %" B_PRId32 " packages (%s)",
		files.CountStrings(), strerror(status));
	return status;
}


// #pragma mark - EFI loaders


/*!	Whether the file at \a path is Haiku's or Renga's EFI boot loader: a PE
	file that says "Haiku revision" and "boot loader!" (the loader's welcome
	line, "Welcome to the Haiku boot loader!" or "... Renga ...").
*/
static bool
is_haiku_efi_loader(const char* path)
{
	BFile file(path, B_READ_ONLY);
	off_t size;
	if (file.InitCheck() != B_OK || file.GetSize(&size) != B_OK
		|| size < 64 || size > 32 * 1024 * 1024)
		return false;
	char* buffer = (char*)malloc(size);
	if (buffer == NULL)
		return false;
	bool loader = file.Read(buffer, size) == size
		&& buffer[0] == 'M' && buffer[1] == 'Z'
		&& memmem(buffer, size, "Haiku revision", 14) != NULL
		&& memmem(buffer, size, "boot loader!", 12) != NULL;
	free(buffer);
	return loader;
}


class EFIPartitionCollector : public BDiskDeviceVisitor {
public:
	EFIPartitionCollector(partition_id disk)
		:
		fDisk(disk)
	{
	}

	virtual bool Visit(BDiskDevice* device)
	{
		return Visit(device, 0);
	}

	virtual bool Visit(BPartition* partition, int32 level)
	{
		if (partition->Device() == NULL
			|| partition->Device()->ID() != fDisk
			|| partition->IsReadOnly() || partition->Type() == NULL
			|| strcmp(partition->Type(), "EFI system data") != 0
			|| partition->ContentType() == NULL
			|| strstr(partition->ContentType(), "FAT") == NULL)
			return false;
		ids.push_back(partition->ID());
		return false;
	}

	partition_id fDisk;
	std::vector<partition_id> ids;
};


/*!	For the in-place modes: every EFI loader on an EFI system partition of
	the destination's disk (\a disk) that is Haiku's or Renga's (in
	\EFI\BOOT, \EFI\HAIKU, \EFI\RENGA) is replaced by this system's, the
	old one kept next to it as <name>_old.EFI, as the Installer's "Install
	EFI loader" does. Other systems' loaders are never touched, nor are
	other disks (the installation medium, say); "Install EFI loader" in
	the Tools menu covers an EFI system partition elsewhere.
*/
status_t
WorkerThread::_UpdateEFILoaders(partition_id disk)
{
	BPath loader;
	if (find_directory(B_SYSTEM_DIRECTORY, &loader) != B_OK
		|| loader.Append("haiku_loader.efi") != B_OK
		|| !BEntry(loader.Path()).Exists()) {
		_Log("no EFI loader on the source, EFI loaders not updated");
		return B_OK;
	}

	EFIPartitionCollector collector(disk);
	BDiskDevice device;
	BPartition* partition;
	fDDRoster.VisitEachPartition(&collector, &device, &partition);

	int32 updated = 0;
	for (size_t i = 0; i < collector.ids.size(); i++) {
		if (fDDRoster.GetPartitionWithID(collector.ids[i], &device,
				&partition) != B_OK)
			continue;
		bool mountedHere = false;
		if (!partition->IsMounted()) {
			if (partition->Mount() != B_OK)
				continue;
			mountedHere = true;
		}
		BPath mountPoint;
		if (partition->GetMountPoint(&mountPoint) != B_OK)
			continue;

		const char* directories[] = { "EFI/BOOT", "EFI/HAIKU", "EFI/RENGA" };
		for (size_t d = 0; d < sizeof(directories) / sizeof(directories[0]);
				d++) {
			BPath directoryPath(mountPoint.Path(), directories[d]);
			BDirectory directory(directoryPath.Path());
			BStringList names;
			BEntry entry;
			while (directory.InitCheck() == B_OK
				&& directory.GetNextEntry(&entry) == B_OK) {
				char name[B_FILE_NAME_LENGTH];
				if (entry.GetName(name) != B_OK)
					continue;
				BString nameString(name);
				if (nameString.IEndsWith(".efi")
					&& nameString.IFindFirst("_old.efi") < 0)
					names.Add(nameString);
			}

			for (int32 n = 0; n < names.CountStrings(); n++) {
				BString name = names.StringAt(n);
				BPath path(directoryPath.Path(), name);
				if (!is_haiku_efi_loader(path.Path()))
					continue;

				BString backup(name);
				backup.Insert("_old", backup.Length() - 4);
				BEntry existing(path.Path());
				status_t status = existing.Rename(backup, true);
				if (status == B_OK) {
					CopyEngine copier(NULL, NULL);
					status = copier.Copy(loader.Path(), path.Path(), -1,
						false);
				}
				_Log("EFI loader %s: %s (old one kept as %s)", path.Path(),
					status == B_OK ? "updated" : strerror(status),
					backup.String());
				if (status == B_OK)
					updated++;
			}
		}
		if (mountedHere)
			partition->Unmount();
	}
	_Log("%" B_PRId32 " EFI loader(s) updated on %" B_PRIuSIZE " EFI system "
		"partition(s)", updated, collector.ids.size());
	return B_OK;
}
