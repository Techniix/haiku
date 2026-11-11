/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 */
#ifndef INSTALLATION_INFO_H
#define INSTALLATION_INFO_H


#include <String.h>
#include <StringList.h>


/*!	What to do with a destination volume that is not empty (Renga, roadmap
	F-23, in-place replacement of an existing installation).
*/
enum install_mode {
	INSTALL_MODE_ASK = 0,
		// the window asks (an unattended install has to say)
	INSTALL_MODE_UPGRADE,
		// Renga's system packages replace the installation's; home,
		// system/settings, system/non-packaged and the packages the user
		// installed stay; the old system stays bootable as a package state
	INSTALL_MODE_REPLACE,
		// the installation's packages go (system and installed ones);
		// home, settings and everything else stay
	INSTALL_MODE_ERASE,
		// everything on the volume goes, after a confirmation naming it
	INSTALL_MODE_CANCEL
};


const char* install_mode_name(install_mode mode);
install_mode install_mode_for_name(const char* name);


/*!	An existing Haiku or Renga installation found on a volume.
*/
struct InstallationInfo {
								InstallationInfo();

			bool				found;
			BString				product;		// "Haiku" or "Renga"
			BString				version;		// of the system package
			BString				systemPackage;	// its file name
			int32				packageCount;
			int32				stateCount;		// old package states

			BString				Description() const;
};


status_t inspect_installation(const char* root, InstallationInfo& info);

class BPartition;
// The same for a BFS partition, mounted or not (an unmounted one is
// mounted read-only for it and unmounted again).
status_t inspect_partition(BPartition* partition, InstallationInfo& info);

// The package name in a package file name ("haiku" for
// "haiku-r1~beta5_hrev57937-1-x86_64.hpkg"): package names cannot contain
// '-', so it is everything before the first one.
BString package_name_of_file(const char* fileName);
bool is_package_file(const char* fileName);
status_t list_package_files(const char* directory, BStringList& files);


#endif	// INSTALLATION_INFO_H
