/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 */

/*!	The Installer's in-place replacement of an existing installation
	(roadmap F-23, early part), run by ce-test.py --installer-upgrade in
	three boots. The phase is in /boot/home/ce-installer-phase (written by
	this test and the harness on the scratch disk):

	A (no phase file; the harness image, with two scratch disks "OldHaiku"
	  and "OldHaiku2", copies of the image made to look like an older,
	  stock Haiku: an older system package, a user-installed package
	  ce_userapp, an activated-packages file and an older package state):
	  user files with attributes and settings are added to both; then
	  - "Installer --list-targets" names the installation;
	  - "Installer --unattended --target OldHaiku --mode upgrade": Renga's
	    packages in system/packages, the old system package in a new state
	    directory whose activated-packages is the old list, the user
	    package still installed, the older state still there, the new
	    activated-packages, user files, attributes and settings untouched
	    (a settings file Renga also ships keeps the user's change), the
	    log, the first boot flag; the disk is then prepared for phase B;
	  - on OldHaiku2: "--mode replace" (system and installed packages gone,
	    Renga's in, files and settings kept), "--mode erase" without and
	    with the wrong confirmation (refused, nothing changed), with the
	    right one (everything gone, Renga installed).
	B ("upgraded", booted from the upgraded OldHaiku): Renga's system
	  package is the active one, the user package is active, files,
	  attributes and settings are there.
	C ("oldstate", booted from OldHaiku with the old state chosen in the
	  boot menu by the harness): the old system package is the active one.

	Without the scratch disks (every other run) it only says so.
*/


#include <DiskDevice.h>
#include <DiskDeviceRoster.h>
#include <DiskDeviceVisitor.h>
#include <Entry.h>
#include <File.h>
#include <Node.h>
#include <Partition.h>
#include <Path.h>
#include <String.h>
#include <StringList.h>
#include <TypeConstants.h>

#include <dirent.h>
#include <fcntl.h>
#include <fs_attr.h>
#include <fs_info.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>


#define SUITE "installer"

static const char* kInstaller = "/boot/system/apps/Installer";
static const char* kPhaseFile = "home/ce-installer-phase";
static const char* kOldSystemPackage
	= "haiku-r1~beta5_hrev57937-1-x86_64.hpkg";
static const char* kUserPackage = "ce_userapp-1.0-1-any.hpkg";
static const char* kOlderState = "state_2025-01-01_12:00:00";
static const char* kNote = "kept by the upgrade";

static int sFailures = 0;


static void
report(const char* name, bool pass, const char* format, ...)
{
	char detail[400];
	va_list args;
	va_start(args, format);
	vsnprintf(detail, sizeof(detail), format, args);
	va_end(args);
	printf("CE-TEST %s.%s %s %s\n", SUITE, name, pass ? "PASS" : "FAIL",
		detail);
	fflush(stdout);
	if (!pass)
		sFailures++;
}


static BString
read_file(const char* path)
{
	BString text;
	FILE* file = fopen(path, "r");
	if (file == NULL)
		return text;
	char buffer[4096];
	size_t bytes;
	while ((bytes = fread(buffer, 1, sizeof(buffer), file)) > 0)
		text.Append(buffer, bytes);
	fclose(file);
	return text;
}


static bool
write_file(const char* path, const char* text)
{
	FILE* file = fopen(path, "w");
	if (file == NULL)
		return false;
	fputs(text, file);
	fclose(file);
	return true;
}


static bool
exists(const char* path)
{
	struct stat st;
	return lstat(path, &st) == 0;
}


static BString
path_in(const char* root, const char* relative)
{
	BString path(root);
	path << "/" << relative;
	return path;
}


static int
run(const char* command, BString* output = NULL)
{
	BString text;
	FILE* pipe = popen(command, "r");
	if (pipe == NULL)
		return -1;
	char buffer[1024];
	size_t bytes;
	while ((bytes = fread(buffer, 1, sizeof(buffer), pipe)) > 0)
		text.Append(buffer, bytes);
	int status = pclose(pipe);
	// The Installer's own lines, for the serial log.
	printf("%s", text.String());
	fflush(stdout);
	if (output != NULL)
		*output = text;
	return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}


static BStringList
packages_in(const char* directory)
{
	BStringList files;
	DIR* dir = opendir(directory);
	if (dir == NULL)
		return files;
	while (dirent* entry = readdir(dir)) {
		BString name(entry->d_name);
		if (name.EndsWith(".hpkg"))
			files.Add(name);
	}
	closedir(dir);
	files.Sort();
	return files;
}


static BString
joined(const BStringList& list)
{
	return list.Join(" ");
}


//!	The system package this system booted with (the boot volume's).
static BString
own_system_package()
{
	BStringList files = packages_in("/boot/system/packages");
	for (int32 i = 0; i < files.CountStrings(); i++) {
		if (files.StringAt(i).StartsWith("haiku-"))
			return files.StringAt(i);
	}
	return "";
}


//!	The package a file of the running system comes from (packagefs).
static BString
package_of(const char* path)
{
	char buffer[B_FILE_NAME_LENGTH];
	int fd = open(path, O_RDONLY);
	if (fd < 0)
		return "";
	ssize_t size = fs_read_attr(fd, "SYS:PACKAGE", B_STRING_TYPE, 0, buffer,
		sizeof(buffer) - 1);
	close(fd);
	if (size < 0)
		return "";
	buffer[size] = '\0';
	return buffer;
}


class VolumeFinder : public BDiskDeviceVisitor {
public:
	VolumeFinder(const char* name) : fName(name), fID(-1) {}

	virtual bool Visit(BDiskDevice* device) { return Visit(device, 0); }

	virtual bool Visit(BPartition* partition, int32 level)
	{
		if (partition->ContentName() == fName)
			fID = partition->ID();
		return false;
	}

	BString			fName;
	partition_id	fID;
};


//!	Mounts the volume named \a name (read-write); returns its mount point.
static BString
mount_volume(const char* name)
{
	BDiskDeviceRoster roster;
	VolumeFinder finder(name);
	BDiskDevice device;
	BPartition* partition;
	roster.VisitEachPartition(&finder, &device, &partition);
	if (finder.fID < 0
		|| roster.GetPartitionWithID(finder.fID, &device, &partition) != B_OK)
		return "";
	if (!partition->IsMounted() && partition->Mount() != B_OK)
		return "";
	BPath mountPoint;
	if (partition->GetMountPoint(&mountPoint) != B_OK)
		return "";
	return mountPoint.Path();
}


//!	Whether files \a a and \a b have the same bytes (binary).
static bool
same_contents(const char* a, const char* b, off_t& size)
{
	BFile fileA(a, B_READ_ONLY);
	BFile fileB(b, B_READ_ONLY);
	off_t sizeB;
	if (fileA.GetSize(&size) != B_OK || fileB.GetSize(&sizeB) != B_OK
		|| size != sizeB || size <= 0 || size > 64 * 1024 * 1024)
		return false;
	char* bufferA = (char*)malloc(size);
	char* bufferB = (char*)malloc(size);
	bool same = bufferA != NULL && bufferB != NULL
		&& fileA.Read(bufferA, size) == size
		&& fileB.Read(bufferB, size) == size
		&& memcmp(bufferA, bufferB, size) == 0;
	free(bufferA);
	free(bufferB);
	return same;
}


class ESPFinder : public BDiskDeviceVisitor {
public:
	ESPFinder() : fID(-1) {}

	virtual bool Visit(BDiskDevice* device) { return Visit(device, 0); }

	virtual bool Visit(BPartition* partition, int32 level)
	{
		if (partition->Type() != NULL
			&& strcmp(partition->Type(), "EFI system data") == 0)
			fID = partition->ID();
		return false;
	}

	partition_id	fID;
};


/*!	Compares the EFI system partition on the disk of the volume \a name
	with what the upgrade should have left there: this system's loader as
	\EFI\BOOT\BOOTX64.EFI and the old one as BOOTX64_old.EFI. Returns false
	(and says why) if there is no such partition (BIOS installations).
*/
static bool
check_efi_loader(const char* name, bool& updated, BString& detail)
{
	BDiskDeviceRoster roster;
	VolumeFinder finder(name);
	BDiskDevice device;
	BPartition* partition;
	roster.VisitEachPartition(&finder, &device, &partition);
	if (finder.fID < 0
		|| roster.GetPartitionWithID(finder.fID, &device, &partition) != B_OK)
		return false;
	ESPFinder esp;
	device.VisitEachDescendant(&esp);
	BPartition* espPartition = device.FindDescendant(esp.fID);
	if (esp.fID < 0 || espPartition == NULL)
		return false;
	bool mountedHere = false;
	if (!espPartition->IsMounted()) {
		if (espPartition->Mount() != B_OK)
			return false;
		mountedHere = true;
	}
	BPath mountPoint;
	espPartition->GetMountPoint(&mountPoint);
	off_t loaderSize = 0;
	bool same = same_contents(path_in(mountPoint.Path(),
		"EFI/BOOT/BOOTX64.EFI"), "/boot/system/haiku_loader.efi", loaderSize);
	bool backup = exists(path_in(mountPoint.Path(), "EFI/BOOT/BOOTX64_old.EFI"));
	updated = same && backup;
	detail.SetToFormat("\\EFI\\BOOT\\BOOTX64.EFI %s this system's loader "
		"(%" B_PRIdOFF " bytes), BOOTX64_old.EFI %s", same ? "is" : "is not",
		loaderSize, backup ? "kept" : "missing");
	if (mountedHere)
		espPartition->Unmount();
	return true;
}


// #pragma mark - user data


struct UserData {
	const char*	path;
	const char*	content;
};


static const UserData kUserFiles[] = {
	{ "home/Documents/ce-letter.txt", "A letter the upgrade must keep.\n" },
	{ "home/config/settings/ce-user.settings", "theme dark\n" },
	{ "system/settings/ce-system.settings", "value 42\n" },
	{ "system/non-packaged/data/ce-user-data", "non-packaged data\n" },
};


static void
add_user_data(const char* root)
{
	BString documents = path_in(root, "home/Documents");
	mkdir(documents.String(), 0755);
	for (size_t i = 0; i < sizeof(kUserFiles) / sizeof(kUserFiles[0]); i++)
		write_file(path_in(root, kUserFiles[i].path), kUserFiles[i].content);

	BNode node(path_in(root, kUserFiles[0].path));
	node.WriteAttrString("ce:note", new BString(kNote));
	node.WriteAttrString("BEOS:TYPE", new BString("text/plain"));
	int32 rating = 7;
	node.WriteAttr("ce:rating", B_INT32_TYPE, 0, &rating, sizeof(rating));

	// A settings file the source has too: the user's version must stay.
	BString kernel = path_in(root, "home/config/settings/kernel/drivers/kernel");
	BString text = read_file(kernel);
	text << "\n# ce-installer-test: the user's own line\n";
	write_file(kernel, text);
}


static bool
user_data_intact(const char* root, BString& problem)
{
	for (size_t i = 0; i < sizeof(kUserFiles) / sizeof(kUserFiles[0]); i++) {
		if (read_file(path_in(root, kUserFiles[i].path))
				!= kUserFiles[i].content) {
			problem.SetToFormat("%s changed or missing", kUserFiles[i].path);
			return false;
		}
	}
	BNode node(path_in(root, kUserFiles[0].path));
	BString note;
	int32 rating = 0;
	if (node.ReadAttrString("ce:note", &note) != B_OK || note != kNote
		|| node.ReadAttr("ce:rating", B_INT32_TYPE, 0, &rating,
			sizeof(rating)) != sizeof(rating) || rating != 7) {
		problem = "attributes of ce-letter.txt changed or missing";
		return false;
	}
	if (read_file(path_in(root, "home/config/settings/kernel/drivers/kernel"))
			.FindFirst("# ce-installer-test: the user's own line") < 0) {
		problem = "the user's line in the kernel settings file is gone";
		return false;
	}
	problem = "files, attributes and settings intact";
	return true;
}


// #pragma mark - phases


static void
phase_upgrade(const char* root)
{
	BString newSystem = own_system_package();
	BString packages = path_in(root, "system/packages");
	BStringList before = packages_in(packages);
	add_user_data(root);

	BString list;
	BString command;
	command << kInstaller << " --list-targets";
	run(command, &list);
	report("detects_installation", list.FindFirst("OldHaiku\t") >= 0
		&& list.FindFirst("Haiku r1~beta5_hrev57937") >= 0,
		"--list-targets names the installation: %s",
		list.FindFirst("Haiku r1~beta5_hrev57937") >= 0 ? "yes" : "no");

	// The running system's volume is never a destination.
	command = "";
	command << kInstaller << " --unattended --target /boot --mode upgrade";
	int status = run(command);
	report("refuses_running_system", status != 0
		&& own_system_package() == newSystem,
		"upgrading /boot from itself: exit %d", status);

	command = "";
	command << kInstaller << " --unattended --target OldHaiku --mode upgrade";
	status = run(command);
	report("upgrade_runs", status == 0, "exit status %d", status);

	BStringList after = packages_in(packages);
	report("upgrade_system_package", after.HasString(newSystem)
		&& !after.HasString(kOldSystemPackage),
		"system/packages has %s, not %s", newSystem.String(),
		kOldSystemPackage);
	report("upgrade_keeps_user_package", after.HasString(kUserPackage),
		"%s %s", kUserPackage, after.HasString(kUserPackage)
			? "still installed" : "gone");

	// the old system, as a state
	BString administrative = path_in(packages, "administrative");
	BString state;
	DIR* dir = opendir(administrative);
	while (dir != NULL) {
		dirent* entry = readdir(dir);
		if (entry == NULL)
			break;
		if (strncmp(entry->d_name, "state_", 6) == 0
			&& strcmp(entry->d_name, kOlderState) != 0)
			state = entry->d_name;
	}
	if (dir != NULL)
		closedir(dir);
	BString statePath = path_in(administrative, state);
	BStringList stateFiles = packages_in(statePath);
	BString oldList = read_file(path_in(statePath, "activated-packages"));
	BString beforeList;
	for (int32 i = 0; i < before.CountStrings(); i++)
		beforeList << before.StringAt(i) << "\n";
	report("upgrade_old_state", !state.IsEmpty()
		&& stateFiles.HasString(kOldSystemPackage)
		&& !stateFiles.HasString(kUserPackage)
		&& oldList.FindFirst(kOldSystemPackage) >= 0
		&& oldList.FindFirst(kUserPackage) >= 0,
		"state %s holds %" B_PRId32 " packages (the old system package "
		"%s), its list has %s", state.String(), stateFiles.CountStrings(),
		stateFiles.HasString(kOldSystemPackage) ? "too" : "missing",
		oldList.FindFirst(kUserPackage) >= 0 ? "the user package"
			: "not the user package");
	report("upgrade_keeps_older_state",
		exists(path_in(administrative, kOlderState)),
		"%s still there", kOlderState);

	BString current = read_file(path_in(administrative, "activated-packages"));
	bool listed = true;
	for (int32 i = 0; i < after.CountStrings(); i++) {
		if (current.FindFirst(after.StringAt(i)) < 0)
			listed = false;
	}
	report("upgrade_activated_packages", listed
		&& current.FindFirst(kOldSystemPackage) < 0,
		"%" B_PRId32 " packages listed", after.CountStrings());

	BString problem;
	bool intact = user_data_intact(root, problem);
	report("upgrade_keeps_files", intact, "%s", problem.String());
	bool updated = false;
	BString detail;
	if (check_efi_loader("OldHaiku", updated, detail))
		report("upgrade_efi_loader", updated, "%s", detail.String());
	else
		printf("CE-INFO no EFI system partition on OldHaiku's disk\n");

	report("upgrade_log", read_file(path_in(root,
			"system/var/log/installer.log")).FindFirst("package state") >= 0
		&& exists(path_in(administrative, "FirstBootProcessingNeeded")),
		"installer.log and the first boot flag on the volume");

	// Prepare the next boot: this test only, phase B.
	BString phase;
	phase << "upgraded\nnew=" << newSystem << "\nold=" << kOldSystemPackage
		<< "\nstate=" << state << "\n";
	write_file(path_in(root, kPhaseFile), phase);
	write_file(path_in(root, "system/non-packaged/data/ce-harness/only"),
		"installer_test\n");
	// The boot menu shows from now on (5 s, then this system), so that the
	// harness can choose the old state in phase C.
	BString kernel = path_in(root, "home/config/settings/kernel/drivers/kernel");
	BString settings = read_file(kernel);
	settings << "boot_menu_timeout 5\n";
	write_file(kernel, settings);
}


static void
phase_replace_and_erase(const char* root)
{
	BString newSystem = own_system_package();
	BString packages = path_in(root, "system/packages");
	add_user_data(root);

	BString command;
	command << kInstaller << " --unattended --target OldHaiku2 --mode replace";
	int status = run(command);
	BStringList after = packages_in(packages);
	report("replace_runs", status == 0, "exit status %d", status);
	report("replace_system", after.HasString(newSystem)
		&& !after.HasString(kOldSystemPackage)
		&& !after.HasString(kUserPackage)
		&& !exists(path_in(packages, BString("administrative/")
			<< kOlderState)),
		"system/packages: %s", joined(after).String());
	BString problem;
	bool intact = user_data_intact(root, problem);
	report("replace_keeps_files", intact, "%s", problem.String());

	command = "";
	command << kInstaller << " --unattended --target OldHaiku2 --mode erase";
	status = run(command);
	bool kept = exists(path_in(root, kUserFiles[0].path));
	report("erase_needs_confirmation", status != 0 && kept,
		"without --confirm-erase: exit %d, files %s", status,
		kept ? "kept" : "gone");

	command = "";
	command << kInstaller << " --unattended --target OldHaiku2 --mode erase "
		"--confirm-erase OldHaiku";
	status = run(command);
	kept = exists(path_in(root, kUserFiles[0].path));
	report("erase_wrong_name_refused", status != 0 && kept,
		"confirmation naming another volume: exit %d, files %s", status,
		kept ? "kept" : "gone");

	command = "";
	command << kInstaller << " --unattended --target OldHaiku2 --mode erase "
		"--confirm-erase OldHaiku2";
	status = run(command);
	after = packages_in(packages);
	report("erase_runs", status == 0
		&& !exists(path_in(root, kUserFiles[0].path))
		&& !exists(path_in(root, kUserFiles[2].path))
		&& after.HasString(newSystem) && !after.HasString(kUserPackage),
		"exit %d; user files %s; %" B_PRId32 " packages", status,
		exists(path_in(root, kUserFiles[0].path)) ? "still there" : "gone",
		after.CountStrings());
}


/*!	What packagefs' SYS:PACKAGE attribute says for a package file:
	"haiku-r1~beta5_hrev57937-1" for haiku-r1~beta5_hrev57937-1-x86_64.hpkg
	(name, version and revision, without the architecture).
*/
static BString
package_id(const BString& fileName)
{
	BString id(fileName);
	id.RemoveLast(".hpkg");
	int32 dash = id.FindLast('-');
	if (dash > 0)
		id.Truncate(dash);
	return id;
}


static BString
phase_value(const BString& phase, const char* key)
{
	BString prefix(key);
	prefix << "=";
	int32 start = phase.FindFirst(prefix);
	if (start < 0)
		return "";
	start += prefix.Length();
	int32 end = phase.FindFirst('\n', start);
	BString value;
	phase.CopyInto(value, start, (end < 0 ? phase.Length() : end) - start);
	return value;
}


static void
phase_booted(const BString& phase, bool oldState)
{
	BString expected = package_id(phase_value(phase, oldState ? "old"
		: "new"));
	BString active = package_of("/boot/system/lib/libroot.so");
	fs_info info;
	fs_stat_dev(dev_for_path("/boot"), &info);
	report(oldState ? "old_state_boots" : "upgraded_boots",
		strcmp(info.volume_name, "OldHaiku") == 0 && active == expected,
		"booted \"%s\"; libroot.so from %s (want %s)", info.volume_name,
		active.String(), expected.String());

	BString user = package_of("/boot/system/data/ce_userapp/README");
	report(oldState ? "old_state_user_package" : "upgraded_user_package",
		user == package_id(kUserPackage), "ce_userapp from %s",
		user.IsEmpty()
			? "nowhere (not active)" : user.String());

	BString problem;
	bool intact = user_data_intact("/boot", problem);
	report(oldState ? "old_state_files" : "upgraded_files", intact, "%s",
		problem.String());
}


int
main()
{
	printf("CE-SUITE %s START\n", SUITE);

	BString phase = read_file("/boot/home/ce-installer-phase");
	if (phase.StartsWith("upgraded"))
		phase_booted(phase, false);
	else if (phase.StartsWith("oldstate"))
		phase_booted(phase, true);
	else {
		BString old = mount_volume("OldHaiku");
		BString old2 = mount_volume("OldHaiku2");
		if (old.IsEmpty() && old2.IsEmpty()) {
			printf("CE-INFO no OldHaiku scratch disks, nothing to test "
				"(ce-test.py --installer-upgrade attaches them)\n");
		} else {
			report("scratch_disks", !old.IsEmpty() && !old2.IsEmpty(),
				"OldHaiku at %s, OldHaiku2 at %s", old.String(),
				old2.String());
			if (!old.IsEmpty())
				phase_upgrade(old);
			if (!old2.IsEmpty())
				phase_replace_and_erase(old2);
			sync();
		}
	}

	printf("CE-SUITE %s DONE failures=%d\n", SUITE, sFailures);
	return sFailures != 0;
}
