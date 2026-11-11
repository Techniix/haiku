/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 */


/*!	Finding an existing Haiku or Renga installation on a volume (Renga,
	roadmap F-23).

	An installation is a volume with system/packages holding the system
	package ("haiku-<version>-<revision>-<arch>.hpkg", or "haiku.hpkg").
	Its version is taken from the file name. Renga and Haiku use the same
	package names (the platform identity stays Haiku), so Renga is told
	apart by content: its system package contains applications Haiku does
	not have (the Screen recording and Boot menu preferences, regular and
	Renga images; the minimum images used by the test harness have neither
	and show as Haiku). Only the package's table of contents is read.
*/


#include "InstallationInfo.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

#include <Catalog.h>
#include <DiskDeviceTypes.h>
#include <Directory.h>
#include <Entry.h>
#include <Partition.h>
#include <Path.h>

#include <fs_volume.h>

#include <package/hpkg/ErrorOutput.h>
#include <package/hpkg/PackageContentHandler.h>
#include <package/hpkg/PackageEntry.h>
#include <package/hpkg/PackageReader.h>

#include <algorithm>


#undef B_TRANSLATION_CONTEXT
#define B_TRANSLATION_CONTEXT "InstallationInfo"


using namespace BPackageKit::BHPKG;


const char*
install_mode_name(install_mode mode)
{
	switch (mode) {
		case INSTALL_MODE_UPGRADE:
			return "upgrade";
		case INSTALL_MODE_REPLACE:
			return "replace";
		case INSTALL_MODE_ERASE:
			return "erase";
		case INSTALL_MODE_CANCEL:
			return "cancel";
		default:
			return "ask";
	}
}


install_mode
install_mode_for_name(const char* name)
{
	if (strcmp(name, "upgrade") == 0)
		return INSTALL_MODE_UPGRADE;
	if (strcmp(name, "replace") == 0)
		return INSTALL_MODE_REPLACE;
	if (strcmp(name, "erase") == 0)
		return INSTALL_MODE_ERASE;
	return INSTALL_MODE_ASK;
}


InstallationInfo::InstallationInfo()
	:
	found(false),
	packageCount(0),
	stateCount(0)
{
}


BString
InstallationInfo::Description() const
{
	if (!found)
		return "";
	BString text(product);
	if (!version.IsEmpty())
		text << " " << version;
	return text;
}


bool
is_package_file(const char* fileName)
{
	size_t length = strlen(fileName);
	return length > 5 && strcmp(fileName + length - 5, ".hpkg") == 0;
}


BString
package_name_of_file(const char* fileName)
{
	BString name(fileName);
	int32 dash = name.FindFirst('-');
	if (dash >= 0)
		name.Truncate(dash);
	else if (name.EndsWith(".hpkg"))
		name.Truncate(name.Length() - 5);
	return name;
}


status_t
list_package_files(const char* directory, BStringList& files)
{
	files.MakeEmpty();
	DIR* dir = opendir(directory);
	if (dir == NULL)
		return errno;
	while (dirent* entry = readdir(dir)) {
		if (is_package_file(entry->d_name))
			files.Add(entry->d_name);
	}
	closedir(dir);
	files.Sort();
	return B_OK;
}


namespace {


class SilentErrorOutput : public BErrorOutput {
public:
	virtual void PrintErrorVarArgs(const char* format, va_list args)
	{
	}
};


/*!	Looks for Renga-only entries in a system package's table of contents.
*/
class RengaMarkerFinder : public BPackageContentHandler {
public:
	RengaMarkerFinder()
		:
		found(false)
	{
	}

	virtual status_t HandleEntry(BPackageEntry* entry)
	{
		const BPackageEntry* parent = entry->Parent();
		if (parent != NULL && parent->Parent() == NULL
			&& strcmp(parent->Name(), "preferences") == 0
			&& (strcmp(entry->Name(), "ScreenRecording") == 0
				|| strcmp(entry->Name(), "BootMenu") == 0)) {
			found = true;
		}
		return B_OK;
	}

	virtual status_t HandleEntryAttribute(BPackageEntry* entry,
		BPackageEntryAttribute* attribute)
	{
		return B_OK;
	}

	virtual status_t HandleEntryDone(BPackageEntry* entry)
	{
		return B_OK;
	}

	virtual status_t HandlePackageAttribute(
		const BPackageInfoAttributeValue& value)
	{
		return B_OK;
	}

	virtual void HandleErrorOccurred()
	{
	}

	bool found;
};


}	// namespace


status_t
inspect_installation(const char* root, InstallationInfo& info)
{
	info = InstallationInfo();

	BPath packages(root, "system/packages");
	BStringList files;
	if (list_package_files(packages.Path(), files) != B_OK)
		return B_ENTRY_NOT_FOUND;

	for (int32 i = 0; i < files.CountStrings(); i++) {
		BString file = files.StringAt(i);
		if (package_name_of_file(file) == "haiku")
			info.systemPackage = file;
	}
	info.packageCount = files.CountStrings();
	if (info.systemPackage.IsEmpty())
		return B_ENTRY_NOT_FOUND;
	info.found = true;

	// "haiku-r1~beta5_hrev57937-1-x86_64.hpkg": name-version-revision-arch
	BStringList parts;
	BString base(info.systemPackage);
	base.Truncate(base.Length() - 5);
	base.Split("-", false, parts);
	if (parts.CountStrings() >= 2)
		info.version = parts.StringAt(1);

	BPath administrative(packages.Path(), "administrative");
	if (DIR* dir = opendir(administrative.Path())) {
		while (dirent* entry = readdir(dir)) {
			if (strncmp(entry->d_name, "state_", 6) == 0)
				info.stateCount++;
		}
		closedir(dir);
	}

	info.product = "Haiku";
	BPath systemPackage(packages.Path(), info.systemPackage);
	SilentErrorOutput errorOutput;
	BPackageReader reader(&errorOutput);
	if (reader.Init(systemPackage.Path()) == B_OK) {
		RengaMarkerFinder finder;
		reader.ParseContent(&finder);
		if (finder.found)
			info.product = "Renga";
	}

	return B_OK;
}


status_t
inspect_partition(BPartition* partition, InstallationInfo& info)
{
	info = InstallationInfo();
	if (partition->ContentType() == NULL
		|| strcmp(partition->ContentType(), kPartitionTypeBFS) != 0)
		return B_BAD_TYPE;

	bool mountedHere = false;
	if (!partition->IsMounted()) {
		if (partition->Mount(NULL, B_MOUNT_READ_ONLY) != B_OK)
			return B_ERROR;
		mountedHere = true;
	}
	BPath mountPoint;
	status_t status = partition->GetMountPoint(&mountPoint);
	if (status == B_OK)
		status = inspect_installation(mountPoint.Path(), info);
	if (mountedHere)
		partition->Unmount();
	return status;
}
