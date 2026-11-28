/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 */

/*!	In-guest test: an installation by the Installer's wizard (roadmap F-23,
	batch 14), end to end, on renga-* images (the wizard is a window).

	The disk is the harness's wizard disk (ce-test.py WIZARD_DISK: a blank,
	sparse 4 GiB virtio disk image on the host, with a marker in its first
	block); nothing is written to a disk without that marker. After the
	first boot the harness boots that disk with UEFI firmware (tag
	"wizard"), and this test runs again there (the phase file
	home/ce-wizard-phase, written onto the new installation, says so).

	The erase installation needs UEFI firmware (renga-anyboot with
	--firmware uefi): the wizard refuses to erase a disk on a BIOS machine,
	since its GPT layout would not start there. On BIOS this test checks
	that refusal instead (bios_refuses_erase, bios_wizard_refuses_erase,
	summary_uefi_text; the harness checks that the disk stayed blank).

	Both firmwares, on 4 GiB disk images on the harness's spare disk
	(ce-test.py SPARE_DISK):
	  installer_wizard.images             the images, GPT with a 512 MiB
	                                      BFS volume at their start
	  installer_wizard.stale_plan_refused another partition appears while
	                                      the wizard shows its Summary: at
	                                      Install the wizard sees the disk
	                                      changed and writes nothing
	  installer_wizard.alongside_install  "alongside": a new BFS volume in
	                                      the free space behind the kept
	                                      one, the system on it, and the
	                                      note how it is started

	First boot (UEFI):
	  installer_wizard.disk               the wizard disk is there
	  installer_wizard.summary            "Installer --wizard-summary
	                                      --choice erase" lists erasing the
	                                      disk, the engine's steps (GPT, EFI
	                                      system partition, BFS), the
	                                      installation and the boot loader,
	                                      and the disk after it; the disk is
	                                      unchanged
	  installer_wizard.refuses_unconfirmed
	                                      the wizard (--auto) with the
	                                      second confirmation naming
	                                      another disk writes nothing
	  installer_wizard.erase_install      the wizard (--auto, Welcome,
	                                      Destination, Summary, the
	                                      confirmation naming the disk,
	                                      Progress, Finish; screenshots
	                                      installer_wizard_erase_summary,
	                                      _progress, _finish) erases the disk
	                                      and installs: GPT, FAT32 "EFI",
	                                      BFS "RengaWizard" with the system
	  installer_wizard.efi_loader         the new EFI system partition has
	                                      EFI/BOOT/BOOTX64.EFI
	  installer_wizard.backup             the table was saved first, with a
	                                      journal that ends "complete"
	  installer_wizard.prepared           the new installation runs only
	                                      this test when the harness boots it
	Second boot (the installed disk):
	  installer_wizard.booted_volume      the system runs from
	                                      "RengaWizard"
	  installer_wizard.booted_efi_loader  its disk's EFI system partition
	                                      has the loader the firmware
	                                      started

	Output format: see 3rdparty/ce/README.md.
*/


#include <Application.h>
#include <DiskDevice.h>
#include <DiskDeviceRoster.h>
#include <DiskEngine.h>
#include <DiskDeviceTypes.h>
#include <Drivers.h>
#include <File.h>
#include <FindDirectory.h>
#include <OS.h>
#include <Partition.h>
#include <Path.h>
#include <String.h>
#include <Volume.h>
#include <VolumeRoster.h>

#include <dirent.h>
#include <fs_volume.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <set>
#include <string>


#define SUITE "installer_wizard"

static const char* kInstaller = "/boot/system/apps/Installer";
static const char kMarker[] = "CE-WIZARD-DISK installer_wizard\n";
static const off_t kDiskSize = 4LL * 1024 * 1024 * 1024;
static const char* kVolume = "RengaWizard";
static const char* kPhaseFile = "/boot/home/ce-wizard-phase";
// written last, at 512 KiB (before the first partition, which starts at
// 1 MiB): the harness boots the disk only when it is there
static const char kCompleteMarker[] = "CE-WIZARD-COMPLETE\n";
static const off_t kCompleteOffset = 512 * 1024;
static const char kSpareMarker[] = "CE-SPARE-DISK installer_destination\n";
static const off_t kSpareSize = 16LL * 1024 * 1024 * 1024;
static const off_t kMiB = 1024 * 1024;

static int sFailures = 0;


static void
report(const char* name, bool pass, const char* format, ...)
{
	char detail[900];
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


//!	\a a followed by \a b.
static BString
cat(const BString& a, const char* b)
{
	BString result(a);
	result << b;
	return result;
}


static BString
one_line(BString text)
{
	text.ReplaceAll("\n", " | ");
	text.Trim();
	if (text.Length() > 500) {
		text.Remove(0, text.Length() - 497);
		text.Prepend("...");
	}
	return text;
}


static int
run(const BString& command, BString& output)
{
	BString full(command);
	full << " 2>&1";
	output = "";
	FILE* pipe = popen(full.String(), "r");
	if (pipe == NULL)
		return -1;
	char buffer[1024];
	size_t bytes;
	while ((bytes = fread(buffer, 1, sizeof(buffer), pipe)) > 0)
		output.Append(buffer, bytes);
	int status = pclose(pipe);
	return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}


/*!	Runs the wizard; for every page line it prints, takes the screenshot
	\a shots names for that page ("summary=installer_wizard_erase_summary"
	...). Returns its output.
*/
typedef void (*page_hook)(const char* page, void* cookie);


static BString
run_wizard(const BString& arguments, const char* const* shots,
	page_hook hook = NULL, void* cookie = NULL)
{
	BString command;
	command << kInstaller << " --wizard --auto " << arguments << " 2>&1";
	printf("CE-INFO %s\n", command.String());
	fflush(stdout);
	BString output;
	FILE* pipe = popen(command.String(), "r");
	if (pipe == NULL)
		return output;
	char line[2048];
	while (fgets(line, sizeof(line), pipe) != NULL) {
		output << line;
		// the Installer's own lines, for the serial log
		printf("CE-INFO wizard: %s", line);
		fflush(stdout);
		const char* page = strstr(line, "wizard page ");
		if (hook != NULL && page != NULL) {
			BString name(page + 12);
			name.Trim();
			hook(name.String(), cookie);
		}
		for (int i = 0; shots != NULL && shots[i] != NULL; i++) {
			BString shot(shots[i]);
			int32 equals = shot.FindFirst('=');
			BString page, name;
			shot.CopyInto(page, 0, equals);
			shot.CopyInto(name, equals + 1, shot.Length());
			BString wanted;
			wanted << "wizard page " << page << "\n";
			if (strstr(line, wanted.String()) != NULL) {
				snooze(1000000);
				printf("CE-HOST screenshot %s\n", name.String());
				fflush(stdout);
				snooze(1500000);
			}
		}
	}
	pclose(pipe);
	return output;
}


static BString
find_wizard_disk()
{
	for (int index = 0; index < 16; index++) {
		BString path;
		path.SetToFormat("/dev/disk/virtual/virtio_block/%d/raw", index);
		int fd = open(path.String(), O_RDONLY);
		if (fd < 0)
			continue;
		device_geometry geometry;
		char start[sizeof(kMarker)] = {};
		bool found = ioctl(fd, B_GET_GEOMETRY, &geometry, sizeof(geometry))
				== 0
			&& (off_t)geometry.bytes_per_sector * geometry.sectors_per_track
				* geometry.cylinder_count * geometry.head_count == kDiskSize
			&& pread(fd, start, sizeof(start) - 1, 0)
				== (ssize_t)sizeof(start) - 1
			&& memcmp(start, kMarker, sizeof(start) - 1) == 0;
		close(fd);
		if (found)
			return path;
	}
	return "";
}


static bool
marker_there(const BString& disk)
{
	int fd = open(disk.String(), O_RDONLY);
	if (fd < 0)
		return false;
	char start[sizeof(kMarker)] = {};
	bool there = pread(fd, start, sizeof(start) - 1, 0)
			== (ssize_t)sizeof(start) - 1
		&& memcmp(start, kMarker, sizeof(start) - 1) == 0;
	close(fd);
	return there;
}


static bool
exists(const BString& path)
{
	struct stat st;
	return stat(path.String(), &st) == 0;
}


static BString
read_file(const BString& path)
{
	BString text;
	FILE* file = fopen(path.String(), "r");
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
write_file(const BString& path, const char* text)
{
	FILE* file = fopen(path.String(), "w");
	if (file == NULL)
		return false;
	bool ok = fputs(text, file) >= 0;
	return fclose(file) == 0 && ok;
}


static std::set<std::string>
directory_entries(const BString& directory)
{
	std::set<std::string> names;
	if (DIR* dir = opendir(directory.String())) {
		while (dirent* entry = readdir(dir)) {
			if (strcmp(entry->d_name, ".") != 0
				&& strcmp(entry->d_name, "..") != 0)
				names.insert(entry->d_name);
		}
		closedir(dir);
	}
	return names;
}


/*!	The mount point of partition \a id, mounting it (read-only with
	\a readOnly) if needed; \a _mounted says whether this did.
*/
static BString
mount_partition(partition_id id, bool readOnly, bool* _mounted)
{
	*_mounted = false;
	BDiskDeviceRoster roster;
	BDiskDevice device;
	BPartition* partition;
	if (roster.GetPartitionWithID(id, &device, &partition) != B_OK)
		return "";
	if (!partition->IsMounted()) {
		if (partition->Mount(NULL, readOnly ? B_MOUNT_READ_ONLY : 0) < B_OK)
			return "";
		*_mounted = true;
	}
	BPath path;
	if (partition->GetMountPoint(&path) != B_OK)
		return "";
	return path.Path();
}


static void
unmount_partition(partition_id id)
{
	BDiskDeviceRoster roster;
	BDiskDevice device;
	BPartition* partition;
	if (roster.GetPartitionWithID(id, &device, &partition) == B_OK
		&& partition->IsMounted())
		partition->Unmount();
}


static const RengaDisk::PartitionInfo*
partition_of_type(const RengaDisk::DiskInfo& disk, const char* type)
{
	for (const RengaDisk::PartitionInfo& partition : disk.partitions) {
		if (partition.type == type)
			return &partition;
	}
	return NULL;
}


static BString
describe(const RengaDisk::DiskInfo& disk)
{
	BString text = disk.partitioning.Length() > 0 ? disk.partitioning
		: BString("no table");
	for (const RengaDisk::PartitionInfo& partition : disk.partitions) {
		BString line;
		line.SetToFormat("; %" B_PRIdOFF " MiB \"%s\" %s \"%s\"",
			partition.size / (1024 * 1024), partition.type.String(),
			partition.content.String(), partition.contentName.String());
		text << line;
	}
	return text;
}


//!	The second boot: the installed disk, started by the firmware.
static void
check_booted()
{
	BVolume boot;
	char name[B_FILE_NAME_LENGTH] = "";
	if (BVolumeRoster().GetBootVolume(&boot) == B_OK)
		boot.GetName(name);
	report("booted_volume", strcmp(name, kVolume) == 0,
		"the system runs from \"%s\"", name);

	BString disk;
	BDiskDeviceRoster roster;
	BDiskDevice device;
	BPartition* partition;
	if (roster.FindPartitionByVolume(boot, &device, &partition) == B_OK) {
		BPath path;
		device.GetPath(&path);
		disk = path.Path();
	}
	RengaDisk::DiskInfo info;
	RengaDisk::GetDisk(disk.String(), info);
	const RengaDisk::PartitionInfo* esp = partition_of_type(info,
		"EFI system data");
	bool loader = false;
	if (esp != NULL) {
		bool mounted;
		BString root = mount_partition(esp->id, true, &mounted);
		loader = root.Length() > 0
			&& exists(cat(root, "/EFI/BOOT/BOOTX64.EFI"));
		if (mounted)
			unmount_partition(esp->id);
	}
	report("booted_efi_loader", loader, "%s: %s", disk.String(),
		describe(info).String());
	unlink(kPhaseFile);
}


/*!	BIOS (or unknown): erasing a disk is refused, on the command line and
	in the window, and the disk keeps its marker; the UEFI summary is
	still checked as text.
*/
static void
check_bios_refusal(const BString& disk, const BString& firmware)
{
	BString command, output;
	command << kInstaller << " --wizard-summary --choice erase --disk "
		<< disk << " --name " << kVolume;
	int status = run(command, output);
	report("bios_refuses_erase", status == 1
			&& (output.FindFirst("started by a BIOS") >= 0
				|| output.FindFirst("could not be told") >= 0)
			&& output.FindFirst("summary:") < 0 && marker_there(disk),
		"firmware %s: exit %d: %s", firmware.String(), status,
		one_line(output).String());

	BString arguments;
	arguments << "--choice erase --disk " << disk << " --name " << kVolume
		<< " --confirm-erase " << disk;
	output = run_wizard(arguments, NULL);
	report("bios_wizard_refuses_erase", output.FindFirst("wizard failed") >= 0
			&& output.FindFirst("BIOS") >= 0
			&& output.FindFirst("wizard page summary") < 0
			&& output.FindFirst("applying the disk plan") < 0
			&& marker_there(disk),
		"marker %s: %s", marker_there(disk) ? "kept" : "gone",
		one_line(output).String());

	// the UEFI summary as text, on this machine too
	command << " --firmware uefi";
	status = run(command, output);
	report("summary_uefi_text", status == 0
			&& output.FindFirst("Step 1: New partition table: GUID Partition "
				"Map") >= 0
			&& output.FindFirst("Put the boot loader into the new EFI system "
				"partition") >= 0
			&& marker_there(disk),
		"exit %d: %s", status, one_line(output).String());
}


// #pragma mark - disk images on the spare disk


/*!	The harness's spare disk, mounted: formatted as BFS "CESpare" when it
	still has its marker, or already (installer_destination_test ran
	first). Its mount point, or "".
*/
static BString
mount_spare_disk()
{
	BDiskDeviceRoster roster;
	for (int index = 0; index < 16; index++) {
		BString path;
		path.SetToFormat("/dev/disk/virtual/virtio_block/%d/raw", index);
		int fd = open(path.String(), O_RDONLY);
		if (fd < 0)
			continue;
		device_geometry geometry;
		char start[sizeof(kSpareMarker)] = {};
		bool size = ioctl(fd, B_GET_GEOMETRY, &geometry, sizeof(geometry))
				== 0
			&& (off_t)geometry.bytes_per_sector * geometry.sectors_per_track
				* geometry.cylinder_count * geometry.head_count == kSpareSize;
		bool marker = size && pread(fd, start, sizeof(start) - 1, 0)
				== (ssize_t)sizeof(start) - 1
			&& memcmp(start, kSpareMarker, sizeof(start) - 1) == 0;
		close(fd);
		if (!size)
			continue;
		BDiskDevice device;
		if (roster.GetDeviceForPath(path.String(), &device) != B_OK)
			continue;
		if (marker) {
			if (device.PrepareModifications() != B_OK)
				continue;
			if (device.Initialize(kPartitionTypeBFS, "CESpare", NULL) != B_OK
				|| device.CommitModifications() != B_OK) {
				device.CancelModifications();
				continue;
			}
			roster.GetDeviceForPath(path.String(), &device);
		} else if (device.ContentName() != "CESpare")
			continue;
		if (!device.IsMounted() && device.Mount() < B_OK)
			continue;
		BPath where;
		if (device.GetMountPoint(&where) == B_OK)
			return where.Path();
	}
	return "";
}


//!	A 4 GiB image on the spare disk, attached: its disk path, or "".
static BString
attach_image(const BString& spare, const char* name, BString& imagePath)
{
	imagePath = spare;
	imagePath << "/" << name;
	BFile file(imagePath.String(), B_CREATE_FILE | B_ERASE_FILE
		| B_READ_WRITE);
	if (file.InitCheck() != B_OK || file.SetSize(kDiskSize) != B_OK)
		return "";
	file.Unset();
	BString device;
	if (RengaDisk::AttachImage(imagePath.String(), device) != B_OK)
		return "";
	return device;
}


//!	GPT with a 512 MiB BFS volume \a keep at its start, the rest free.
static status_t
prepare_gpt_with(const BString& disk, const char* keep, const BString& backups)
{
	RengaDisk::Plan plan(disk.String());
	RengaDisk::Operation operation;
	operation.kind = RengaDisk::kInitializePartitioning;
	operation.index = -1;
	operation.system = "gpt";
	operation.size = 0;
	plan.Add(operation);
	operation.kind = RengaDisk::kCreatePartition;
	operation.system = "";
	operation.size = 512 * kMiB;
	operation.name = keep;
	plan.Add(operation);
	operation.kind = RengaDisk::kFormat;
	operation.index = 0;
	operation.system = "bfs";
	plan.Add(operation);
	BString log;
	return plan.Apply(backups.String(), log);
}


static const RengaDisk::PartitionInfo*
volume_named(const RengaDisk::DiskInfo& disk, const char* name)
{
	for (const RengaDisk::PartitionInfo& partition : disk.partitions) {
		if (partition.contentName == name)
			return &partition;
	}
	return NULL;
}


struct StaleCookie {
	BString		disk;
	BString		backups;
	status_t	status;
	bool		done;
};


//!	On the Summary page: another partition appears in the free space, as
//!	if DriveSetup (or another stick) changed the disk.
static void
change_disk_on_summary(const char* page, void* data)
{
	StaleCookie* cookie = (StaleCookie*)data;
	if (strcmp(page, "summary") != 0 || cookie->done)
		return;
	cookie->done = true;
	RengaDisk::Plan plan(cookie->disk.String());
	RengaDisk::Operation operation;
	operation.kind = RengaDisk::kCreatePartition;
	operation.index = -1;
	operation.size = 256 * kMiB;
	operation.name = "Intruder";
	plan.Add(operation);
	BString log;
	cookie->status = plan.Apply(cookie->backups.String(), log);
	printf("CE-INFO the disk changed on the Summary page: %s: %s\n",
		strerror(cookie->status), one_line(log).String());
	fflush(stdout);
}


/*!	Both firmwares, on disk images on the spare disk: "alongside" in the
	free space of a GPT disk, and a plan that went stale between Summary
	and Install (refused, nothing written by the wizard).
*/
static void
check_spare_disk_cases(const BString& firmware)
{
	BString spare = mount_spare_disk();
	BString alongPath, stalePath;
	BString along = spare.Length() > 0
		? attach_image(spare, "along.img", alongPath) : BString();
	BString stale = spare.Length() > 0
		? attach_image(spare, "stale.img", stalePath) : BString();
	BString backups = spare;
	backups << "/backups";
	mkdir(backups.String(), 0755);
	bool ready = along.Length() > 0 && stale.Length() > 0
		&& prepare_gpt_with(along, "KeepAlong", backups) == B_OK
		&& prepare_gpt_with(stale, "KeepStale", backups) == B_OK;
	report("images", ready, "spare disk at \"%s\": %s, %s", spare.String(),
		along.String(), stale.String());

	if (ready) {
		// the plan goes stale on the Summary page
		StaleCookie cookie = { stale, backups, B_ERROR, false };
		BString arguments;
		arguments << "--choice alongside --disk " << stale
			<< " --name RengaStale";
		BString output = run_wizard(arguments, NULL, change_disk_on_summary,
			&cookie);
		RengaDisk::DiskInfo info;
		RengaDisk::GetDisk(stale.String(), info);
		report("stale_plan_refused", cookie.status == B_OK
				&& output.FindFirst("the disk changed after the summary") >= 0
				&& output.FindFirst("wizard failed") >= 0
				&& output.FindFirst("applying the disk plan") < 0
				&& volume_named(info, "RengaStale") == NULL
				&& info.partitions.size() == 2,
			"change %s; disk now: %s; %s", strerror(cookie.status),
			describe(info).String(), one_line(output).String());

		// alongside
		RengaDisk::DiskInfo before;
		RengaDisk::GetDisk(along.String(), before);
		const RengaDisk::PartitionInfo* keep = volume_named(before,
			"KeepAlong");
		off_t keepOffset = keep != NULL ? keep->offset : -1;
		arguments = "";
		arguments << "--choice alongside --disk " << along
			<< " --name RengaAlong";
		output = run_wizard(arguments, NULL);
		RengaDisk::GetDisk(along.String(), info);
		keep = volume_named(info, "KeepAlong");
		const RengaDisk::PartitionInfo* created = volume_named(info,
			"RengaAlong");
		bool mounted = false;
		BString root = created != NULL
			? mount_partition(created->id, false, &mounted) : BString();
		int packages = 0;
		for (const std::string& name
				: directory_entries(cat(root, "/system/packages"))) {
			if (BString(name.c_str()).EndsWith(".hpkg"))
				packages++;
		}
		const char* note = firmware == "uefi"
			? "will not start until" : "started by a BIOS";
		report("alongside_install", output.FindFirst("wizard finished") >= 0
				&& output.FindFirst(note) >= 0
				&& keep != NULL && keep->offset == keepOffset
				&& keep->size == 512 * kMiB && created != NULL
				&& created->content == "Be File System"
				&& created->offset >= keepOffset + 512 * kMiB
				&& packages > 0,
			"note \"%s\"; disk: %s; %d packages; %s", note,
			describe(info).String(), packages, one_line(output).String());
		if (created != NULL)
			unmount_partition(created->id);
	}

	// clean up: only what this test made
	if (along.Length() > 0)
		RengaDisk::DetachImage(alongPath.String());
	if (stale.Length() > 0)
		RengaDisk::DetachImage(stalePath.String());
	if (alongPath.Length() > 0)
		unlink(alongPath.String());
	if (stalePath.Length() > 0)
		unlink(stalePath.String());
	if (spare.Length() > 0) {
		BString command;
		command << "rm -rf '" << backups << "'";
		system(command.String());
		fs_unmount_volume(spare.String(), 0);
	}
}


/*!	UEFI: the erase installation onto the wizard disk, which the harness
	then boots.
*/
static void
check_erase_install(const BString& disk)
{

	// the Summary page's list as text: nothing written
	BString command, output;
	command << kInstaller << " --wizard-summary --choice erase --disk "
		<< disk << " --name " << kVolume;
	int status = run(command, output);
	report("summary", status == 0
			&& output.FindFirst(cat("summary: Erase the whole disk ", disk.String())) >= 0
			&& output.FindFirst("Step 1: New partition table: GUID Partition "
				"Map") >= 0
			&& output.FindFirst("\"EFI system partition\", type EFI system "
				"data") >= 0
			&& output.FindFirst("as FAT32") >= 0
			&& output.FindFirst("as Be File System named \"RengaWizard\"")
				>= 0
			&& output.FindFirst("onto the new volume \"RengaWizard\"") >= 0
			&& output.FindFirst("Put the boot loader into the new EFI system "
				"partition") >= 0
			&& output.FindFirst("after: new 256 MiB at 1 MiB \"EFI\"") >= 0
			&& output.FindFirst("\"RengaWizard\"\n") >= 0
			&& marker_there(disk),
		"exit %d, marker %s: %s", status, marker_there(disk) ? "kept"
			: "gone", one_line(output).String());

	BPath settings;
	find_directory(B_USER_SETTINGS_DIRECTORY, &settings);
	BString backups(settings.Path());
	backups << "/Renga/disk-backups";
	std::set<std::string> before = directory_entries(backups);

	// the second confirmation naming another disk: nothing is written
	BString arguments;
	arguments << "--choice erase --disk " << disk << " --name " << kVolume
		<< " --confirm-erase /dev/disk/not-this-one/raw";
	output = run_wizard(arguments, NULL);
	report("refuses_unconfirmed", output.FindFirst("not confirmed") >= 0
			&& output.FindFirst("wizard failed") >= 0
			&& output.FindFirst("applying the disk plan") < 0
			&& marker_there(disk) && directory_entries(backups) == before,
		"marker %s: %s", marker_there(disk) ? "kept" : "gone",
		one_line(output).String());

	// the installation
	const char* shots[] = { "summary=installer_wizard_erase_summary",
		"progress=installer_wizard_progress",
		"finish=installer_wizard_finish", NULL };
	arguments = "";
	arguments << "--choice erase --disk " << disk << " --name " << kVolume
		<< " --confirm-erase " << disk;
	output = run_wizard(arguments, shots);

	RengaDisk::DiskInfo info;
	RengaDisk::GetDisk(disk.String(), info);
	const RengaDisk::PartitionInfo* esp = partition_of_type(info,
		"EFI system data");
	const RengaDisk::PartitionInfo* volume = NULL;
	for (const RengaDisk::PartitionInfo& partition : info.partitions) {
		if (partition.contentName == kVolume)
			volume = &partition;
	}
	bool mounted = false;
	BString root = volume != NULL
		? mount_partition(volume->id, false, &mounted) : BString();
	int packages = 0;
	for (const std::string& name
			: directory_entries(cat(root, "/system/packages"))) {
		if (BString(name.c_str()).EndsWith(".hpkg"))
			packages++;
	}
	report("erase_install", output.FindFirst("wizard finished") >= 0
			&& output.FindFirst("wizard page summary") >= 0
			&& output.FindFirst("confirmed (--confirm-erase)") >= 0
			&& info.partitioning == "GUID Partition Map" && esp != NULL
			&& esp->content.FindFirst("FAT") >= 0 && volume != NULL
			&& volume->content == "Be File System" && root.Length() > 0
			&& packages > 0 && exists(cat(root, "/system/packages/"
				"administrative/FirstBootProcessingNeeded")),
		"disk: %s; %d packages on \"%s\"; %s", describe(info).String(),
		packages, root.String(), one_line(output).String());

	bool espMounted = false;
	BString espRoot = esp != NULL
		? mount_partition(esp->id, true, &espMounted) : BString();
	report("efi_loader", espRoot.Length() > 0
			&& exists(cat(espRoot, "/EFI/BOOT/BOOTX64.EFI")),
		"EFI system partition at \"%s\"", espRoot.String());
	if (espMounted)
		unmount_partition(esp->id);

	// the engine's backup and journal (removed again: only what this test
	// made)
	int tables = 0;
	BString journal;
	for (const std::string& name : directory_entries(backups)) {
		if (before.count(name) != 0)
			continue;
		BString path(backups);
		path << "/" << name.c_str();
		if (BString(name.c_str()).EndsWith(".table"))
			tables++;
		else if (BString(name.c_str()).EndsWith(".journal"))
			journal = read_file(path);
		unlink(path.String());
	}
	// ... and their copies on the new volume
	int copies = 0;
	for (const std::string& name : directory_entries(cat(root,
			"/home/config/settings/Renga/disk-backups"))) {
		if (BString(name.c_str()).EndsWith(".table")
			|| BString(name.c_str()).EndsWith(".journal"))
			copies++;
	}
	report("backup", tables == 1 && journal.FindFirst("complete") >= 0
			&& copies == 2,
		"%d table backup(s), %d file(s) copied onto the new volume; "
		"journal: %s", tables, copies, one_line(journal).String());

	// the harness boots the disk next: there only this test runs, and it
	// knows the phase
	bool prepared = root.Length() > 0
		&& write_file(cat(root, "/home/ce-wizard-phase"), "installed\n")
		&& (mkdir(cat(root, "/system/non-packaged/data/ce-harness")
			.String(), 0755) == 0 || errno == EEXIST)
		&& write_file(cat(root, "/system/non-packaged/data/ce-harness/only"),
			"installer_wizard_test\n")
		&& write_file(cat(root, "/system/non-packaged/data/ce-harness/"
			"no-final-tests"), "skip the tests that end the boot\n");
	// no crash phase left over from the source
	if (root.Length() > 0) {
		unlink((cat(root, "/system/non-packaged/data/ce-harness/crash-phase"))
			.String());
	}
	sync();
	if (volume != NULL)
		unmount_partition(volume->id);
	sync();
	// last: the completion marker the harness looks for before it boots
	// the disk, in the unused space before the first partition
	if (prepared) {
		int fd = open(disk.String(), O_WRONLY);
		prepared = fd >= 0 && pwrite(fd, kCompleteMarker,
			sizeof(kCompleteMarker) - 1, kCompleteOffset)
				== (ssize_t)sizeof(kCompleteMarker) - 1;
		if (fd >= 0)
			close(fd);
		sync();
	}
	report("prepared", prepared, "phase file, selection and completion "
		"marker on \"%s\"", root.String());

}


int
main()
{
	BApplication app("application/x-vnd.Renga-installer-wizard-test");
	printf("CE-SUITE %s START\n", SUITE);
	fflush(stdout);

	if (read_file(kPhaseFile).StartsWith("installed")) {
		check_booted();
		printf("CE-SUITE %s DONE failures=%d\n", SUITE, sFailures);
		return sFailures == 0 ? 0 : 1;
	}

	BString firmware;
	{
		BString command, output;
		command << kInstaller << " --firmware";
		run(command, output);
		firmware = output.Trim();
		printf("CE-INFO firmware as the Installer tells it: %s\n",
			firmware.String());
	}

	BString disk = find_wizard_disk();
	report("disk", disk.Length() > 0, "%s", disk.Length() > 0
		? disk.String() : "no wizard disk (ce-test.py WIZARD_DISK: a 4 GiB "
			"virtio disk starting with the marker)");
	if (disk.Length() > 0) {
		if (firmware == "uefi")
			check_erase_install(disk);
		else
			check_bios_refusal(disk, firmware);
	}
	check_spare_disk_cases(firmware);

	printf("CE-SUITE %s DONE failures=%d\n", SUITE, sFailures);
	return sFailures == 0 ? 0 : 1;
}
