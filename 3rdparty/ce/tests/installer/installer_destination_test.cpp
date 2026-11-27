/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 */

/*!	In-guest test: the Installer wizard's Destination layouts on the shared
	disk engine (roadmap F-23, batch 14), through "Installer --plan-disk".

	The disks are image files, attached as file disks, on the harness's
	spare disk (ce-test.py SPARE_DISK: a blank, sparse 16 GiB virtio disk
	with a marker in its first block; the test formats it as BFS "CESpare"
	only when the marker is there). The boot volume has no room for 4 GiB
	images, and a RAM file system commits memory for a file's whole size.
	The engine and the Installer write only to the image files, and the
	boot disk is only ever asked for a preview (refused before it is
	shown). Without the spare disk, attach fails and nothing else runs.

	  installer_destination.attach            four images on the spare disk
	                                          (4 GiB
	                                          "erase", "behind" and "front",
	                                          1 GiB "small") are disks
	  installer_destination.erase_preview     "--layout erase" without
	                                          --apply prints GPT, a 256 MiB
	                                          EFI system partition (FAT32)
	                                          and a BFS volume "Renga...", and
	                                          writes nothing
	  installer_destination.erase_apply       with --apply: a GUID
	                                          Partition Map, FAT32 "EFI"
	                                          (type EFI system data,
	                                          256 MiB) and BFS with the
	                                          rest, named "Renga" or, when
	                                          a volume has that name (the
	                                          harness image's own),
	                                          "Renga 2"; "volume: <name>"
	  installer_destination.refuses_taken_name
	                                          --name of an existing volume
	                                          is refused
	  installer_destination.erase_backup      the table was saved first into
	                                          ~/config/settings/Renga/
	                                          disk-backups, with a journal
	                                          that ends "complete"
	  installer_destination.refuses_boot_disk a preview (no --apply) for the
	                                          disk the system runs from is
	                                          refused as such (not for its
	                                          size), before any preview
	  installer_destination.refuses_small     a 1 GiB disk is refused: an
	                                          installation needs 2.25 GiB
	  installer_destination.refuses_free_without_table
	                                          "free" on a disk without a
	                                          partition table is refused
	  installer_destination.free_after_partitions
	                                          "free" on a GPT disk whose
	                                          first partition was deleted
	                                          (a hole of 512 MiB before the
	                                          partition "Keep", the rest
	                                          free behind it): the new BFS
	                                          volume fills the space behind
	                                          "Keep", which is untouched
	  installer_destination.free_space_at_start
	                                          the same with the largest free
	                                          space at the start of the
	                                          table (2.5 GiB from block 34):
	                                          the new volume is there, the
	                                          partition behind it untouched
	  installer_destination.install           "Installer --unattended
	                                          --target Renga" onto the
	                                          erased disk's new volume: the
	                                          system packages, the first
	                                          boot flag and the log are
	                                          there.

	Output format: see 3rdparty/ce/README.md.
*/


#include <DiskDevice.h>
#include <DiskDeviceRoster.h>
#include <DiskDeviceTypes.h>
#include <DiskEngine.h>
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
#include <errno.h>
#include <fcntl.h>
#include <fs_volume.h>
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


#define SUITE "installer_destination"

static const char* kInstaller = "/boot/system/apps/Installer";
static const char kSpareMarker[] = "CE-SPARE-DISK installer_destination\n";
static const char* kSpareName = "CESpare";
static const off_t kMiB = 1024 * 1024;
static const off_t kGiB = 1024 * kMiB;
static const off_t kSpareSize = 16 * kGiB;

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


//!	\a text on one line, at most 500 characters.
static BString
one_line(BString text)
{
	text.ReplaceAll("\n", " | ");
	text.Trim();
	if (text.Length() > 500) {
		text.Truncate(497);
		text << "...";
	}
	return text;
}


//!	Runs \a command with its error output; its output and exit status.
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


static int
plan_disk(const BString& disk, const char* arguments, BString& output)
{
	BString command;
	command << kInstaller << " --plan-disk " << disk << " " << arguments;
	int status = run(command, output);
	printf("CE-INFO %s: exit %d: %s\n", command.String(), status,
		one_line(output).String());
	fflush(stdout);
	return status;
}


//!	BFile::SetSize() does not write zeros into the new blocks (the spare
//!	disk is blank, so they read as zeros anyway): no 4 GiB of writes.
static status_t
make_image(const BString& path, off_t size)
{
	BFile file(path.String(), B_CREATE_FILE | B_ERASE_FILE | B_READ_WRITE);
	status_t status = file.InitCheck();
	if (status == B_OK)
		status = file.SetSize(size);
	return status;
}


//!	The harness's spare disk (its marker in the first block), or "".
static BString
find_spare_disk()
{
	for (int index = 0; index < 16; index++) {
		BString path;
		path.SetToFormat("/dev/disk/virtual/virtio_block/%d/raw", index);
		int fd = open(path.String(), O_RDONLY);
		if (fd < 0)
			continue;
		device_geometry geometry;
		char start[sizeof(kSpareMarker)] = {};
		bool spare = ioctl(fd, B_GET_GEOMETRY, &geometry, sizeof(geometry))
				== 0
			&& (off_t)geometry.bytes_per_sector * geometry.sectors_per_track
				* geometry.cylinder_count * geometry.head_count == kSpareSize
			&& pread(fd, start, sizeof(start) - 1, 0)
				== (ssize_t)sizeof(start) - 1
			&& memcmp(start, kSpareMarker, sizeof(start) - 1) == 0;
		close(fd);
		if (spare)
			return path;
	}
	return "";
}


//!	BFS "CESpare" on the spare disk, mounted; its mount point.
static status_t
prepare_spare_disk(const BString& path, BString& mountPoint)
{
	BDiskDeviceRoster roster;
	BDiskDevice device;
	status_t status = roster.GetDeviceForPath(path.String(), &device);
	if (status == B_OK)
		status = device.PrepareModifications();
	if (status == B_OK) {
		status = device.Initialize(kPartitionTypeBFS, kSpareName, NULL);
		if (status == B_OK)
			status = device.CommitModifications();
		else
			device.CancelModifications();
	}
	if (status == B_OK)
		status = roster.GetDeviceForPath(path.String(), &device);
	if (status == B_OK) {
		dev_t volume = device.Mount();
		status = volume < 0 ? volume : B_OK;
	}
	BPath where;
	if (status == B_OK)
		status = device.GetMountPoint(&where);
	if (status == B_OK)
		mountPoint = where.Path();
	return status;
}


//!	The backup folder's file names.
static std::set<std::string>
backup_files(const BString& directory)
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


static BString
describe_disk(const BString& path)
{
	RengaDisk::DiskInfo disk;
	BString text;
	if (RengaDisk::GetDisk(path.String(), disk) != B_OK)
		return "no such disk";
	text << (disk.partitioning.Length() > 0 ? disk.partitioning.String()
		: "no table");
	for (const RengaDisk::PartitionInfo& partition : disk.partitions) {
		BString line;
		line.SetToFormat("; %" B_PRId32 " at %" B_PRIdOFF " MiB, %" B_PRIdOFF
			" MiB, \"%s\" type \"%s\", %s \"%s\"%s", partition.index,
			partition.offset / kMiB, partition.size / kMiB,
			partition.name.String(), partition.type.String(),
			partition.content.Length() > 0 ? partition.content.String()
				: "empty", partition.contentName.String(),
			partition.mounted ? " mounted" : "");
		text << line;
	}
	return text;
}


//!	The partition with volume (content) name \a name, or NULL.
static const RengaDisk::PartitionInfo*
volume_named(const RengaDisk::DiskInfo& disk, const char* name)
{
	for (const RengaDisk::PartitionInfo& partition : disk.partitions) {
		if (partition.contentName == name)
			return &partition;
	}
	return NULL;
}


static bool
is_bfs(const RengaDisk::PartitionInfo* partition)
{
	return partition != NULL && partition->content == kPartitionTypeBFS;
}


/*!	A GPT disk with \a firstSize "Gone" and then 512 MiB "Keep" (BFS
	named \a keepName), "Gone" deleted again: a hole at the start of the
	table, then "Keep", then the rest of the disk free.
*/
static status_t
prepare_hole(const BString& disk, off_t firstSize, const char* keepName,
	const BString& backups, BString& log)
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
	operation.size = firstSize;
	operation.name = "Gone";
	plan.Add(operation);
	operation.size = 512 * kMiB;
	operation.name = keepName;
	plan.Add(operation);
	operation.kind = RengaDisk::kFormat;
	operation.index = 1;
	operation.system = "bfs";
	operation.size = 0;
	plan.Add(operation);
	operation.kind = RengaDisk::kDeletePartition;
	operation.index = 0;
	plan.Add(operation);
	return plan.Apply(backups.String(), log);
}


/*!	"free" on a disk prepared by prepare_hole(): the new volume \a name
	must be BFS with at least 2 GiB, before "Keep" when \a before, behind
	it otherwise, and "Keep" must be what it was.
*/
static void
check_free(const char* check, const BString& disk, off_t firstSize,
	const char* keepName, const char* name, bool before,
	const BString& backups)
{
	BString log;
	status_t prepared = prepare_hole(disk, firstSize, keepName, backups, log);
	RengaDisk::DiskInfo info;
	RengaDisk::GetDisk(disk.String(), info);
	const RengaDisk::PartitionInfo* keep = volume_named(info, keepName);
	off_t keepOffset = keep != NULL ? keep->offset : -1;
	partition_id keepID = keep != NULL ? keep->id : -1;
	if (prepared != B_OK || !is_bfs(keep) || info.partitions.size() != 1) {
		report(check, false, "preparing the disk failed (%s): %s; disk: %s",
			strerror(prepared), one_line(log).String(),
			describe_disk(disk).String());
		return;
	}

	BString arguments, output;
	arguments << "--layout free --name " << name << " --apply";
	int status = plan_disk(disk, arguments.String(), output);
	RengaDisk::GetDisk(disk.String(), info);
	keep = volume_named(info, keepName);
	const RengaDisk::PartitionInfo* created = volume_named(info, name);
	BString expected;
	expected << "volume: " << name;
	bool placed = created != NULL && (before
		? created->offset + created->size <= keepOffset
		: created->offset >= keepOffset + 512 * kMiB);
	report(check, status == 0 && output.FindFirst(expected) >= 0
			&& is_bfs(keep) && keep->offset == keepOffset
			&& keep->id == keepID && keep->size == 512 * kMiB
			&& is_bfs(created) && created->size >= 2 * kGiB && placed
			&& created->offset % kMiB == 0 && info.partitions.size() == 2,
		"exit %d; \"%s\" was at %" B_PRIdOFF " MiB; disk now: %s", status,
		keepName, keepOffset / kMiB, describe_disk(disk).String());
}


//!	Unmounts every mounted partition of the disk at \a path.
static void
unmount_all(const BString& path)
{
	RengaDisk::DiskInfo disk;
	if (RengaDisk::GetDisk(path.String(), disk) != B_OK)
		return;
	BDiskDeviceRoster roster;
	for (const RengaDisk::PartitionInfo& partition : disk.partitions) {
		if (!partition.mounted)
			continue;
		BDiskDevice device;
		BPartition* child;
		if (roster.GetPartitionWithID(partition.id, &device, &child) == B_OK)
			child->Unmount(B_FORCE_UNMOUNT);
	}
}


//!	The mount point of the volume named \a name, mounting it if needed.
static BString
mount_point_of(const BString& disk, const char* name)
{
	RengaDisk::DiskInfo info;
	if (RengaDisk::GetDisk(disk.String(), info) != B_OK)
		return "";
	const RengaDisk::PartitionInfo* volume = volume_named(info, name);
	if (volume == NULL)
		return "";
	BDiskDeviceRoster roster;
	BDiskDevice device;
	BPartition* partition;
	if (roster.GetPartitionWithID(volume->id, &device, &partition) != B_OK)
		return "";
	if (!partition->IsMounted() && partition->Mount() < B_OK)
		return "";
	BPath path;
	if (partition->GetMountPoint(&path) != B_OK)
		return "";
	return path.Path();
}


static int
count_packages(const BString& directory)
{
	int count = 0;
	if (DIR* dir = opendir(directory.String())) {
		while (dirent* entry = readdir(dir)) {
			if (BString(entry->d_name).EndsWith(".hpkg"))
				count++;
		}
		closedir(dir);
	}
	return count;
}


static bool
exists(const BString& path)
{
	struct stat st;
	return stat(path.String(), &st) == 0;
}


static void
check_install(const BString& disk, const BString& name)
{
	BString command, output;
	command << kInstaller << " --unattended --target '" << name << "'";
	int status = run(command, output);
	// the Installer's own lines, for the serial log
	printf("%s", output.String());
	fflush(stdout);

	BString root = mount_point_of(disk, name.String());
	BString packages = root;
	packages << "/system/packages";
	int copied = root.Length() > 0 ? count_packages(packages) : 0;
	int source = count_packages("/boot/system/packages");
	BString firstBoot = packages;
	firstBoot << "/administrative/FirstBootProcessingNeeded";
	BString log = root;
	log << "/system/var/log/installer.log";
	BString system = packages;
	system << "/haiku.hpkg";
	bool found = false;
	if (DIR* dir = opendir(packages.String())) {
		while (dirent* entry = readdir(dir)) {
			BString name(entry->d_name);
			if (name.StartsWith("haiku-") && name.EndsWith(".hpkg"))
				found = true;
		}
		closedir(dir);
	}
	BString home = root;
	home << "/home";
	BString loader = root;
	loader << "/system/haiku_loader.efi";
	report("install", status == 0 && root.Length() > 0
			&& copied > 0 && copied >= source
			&& (found || exists(system))
			&& exists(firstBoot)
			&& read_file(log).Length() > 0 && exists(home),
		"exit %d, mounted at \"%s\": %d of the source's %d packages, system "
		"package %s, first boot flag %s, installer.log %s, home %s, "
		"haiku_loader.efi %s; %s", status, root.String(), copied, source,
		found || exists(system) ? "there" : "missing",
		exists(firstBoot) ? "there"
			: "missing", read_file(log).Length() > 0 ? "there" : "missing",
		exists(home) ? "there" : "missing",
		exists(loader) ? "there" : "missing", one_line(output).String());
}


int
main()
{
	printf("CE-SUITE %s START\n", SUITE);
	fflush(stdout);
	if (access(kInstaller, X_OK) != 0) {
		printf("CE-INFO no %s on this image, nothing to test\n", kInstaller);
		printf("CE-SUITE %s DONE failures=0\n", SUITE);
		return 0;
	}

	// the disks: image files on the harness's spare disk
	BString spare = find_spare_disk();
	if (spare.Length() == 0) {
		// the harness attaches it whenever this test is selected: without
		// it nothing here is tested, which must not pass silently
		report("attach", false, "no spare disk (ce-test.py SPARE_DISK: a "
			"16 GiB virtio disk starting with \"%.*s\")",
			(int)sizeof(kSpareMarker) - 2, kSpareMarker);
		printf("CE-SUITE %s DONE failures=%d\n", SUITE, sFailures);
		return 1;
	}
	BString spareRoot;
	status_t mounted = prepare_spare_disk(spare, spareRoot);
	struct Image {
		const char*	name;
		off_t		size;
		BString		path;
		BString		disk;
	} images[] = {
		{ "erase", 4 * kGiB },
		{ "behind", 4 * kGiB },
		{ "front", 4 * kGiB },
		{ "small", 1 * kGiB },
	};
	const int kImageCount = sizeof(images) / sizeof(images[0]);
	bool attached = mounted >= B_OK;
	BString problem;
	for (int i = 0; attached && i < kImageCount; i++) {
		images[i].path << spareRoot << "/" << images[i].name << ".img";
		status_t status = make_image(images[i].path, images[i].size);
		if (status != B_OK) {
			problem << "; creating " << images[i].path << ": "
				<< strerror(status);
		} else {
			status = RengaDisk::AttachImage(images[i].path.String(),
				images[i].disk);
			if (status != B_OK) {
				problem << "; attaching " << images[i].path << ": "
					<< strerror(status);
				images[i].disk = "";
			}
		}
		attached = status == B_OK;
	}
	report("attach", attached, "spare disk %s at %s: %s%s; disks %s, %s, "
		"%s, %s", spare.String(), spareRoot.String(),
		mounted >= B_OK ? "BFS, mounted" : strerror(mounted),
		problem.String(),
		images[0].disk.String(), images[1].disk.String(),
		images[2].disk.String(), images[3].disk.String());
	if (!attached) {
		for (int i = 0; i < kImageCount; i++) {
			if (images[i].disk.Length() > 0)
				RengaDisk::DetachImage(images[i].path.String());
			unlink(images[i].path.String());
		}
		if (mounted >= B_OK)
			fs_unmount_volume(spareRoot.String(), 0);
		printf("CE-SUITE %s DONE failures=%d\n", SUITE, sFailures);
		return 1;
	}
	const BString& erase = images[0].disk;
	const BString& behind = images[1].disk;
	const BString& front = images[2].disk;
	const BString& small = images[3].disk;

	BPath settings;
	find_directory(B_USER_SETTINGS_DIRECTORY, &settings);
	BString backups = settings.Path();
	backups << "/Renga/disk-backups";
	std::set<std::string> before = backup_files(backups);
	// the test's own preparations keep their backups on the spare disk
	BString ownBackups;
	ownBackups << spareRoot << "/backups";

	// erase: the preview first, nothing written
	BString output;
	int status = plan_disk(erase, "--layout erase", output);
	RengaDisk::DiskInfo info;
	RengaDisk::GetDisk(erase.String(), info);
	report("erase_preview", status == 0
			&& output.FindFirst("GUID Partition Map") >= 0
			&& output.FindFirst("256.0 MiB \"EFI system partition\", type "
				"EFI system data") >= 0
			&& output.FindFirst("FAT32") >= 0
			&& output.FindFirst("Be File System named \"Renga") >= 0
			&& info.partitioning.Length() == 0 && info.partitions.empty()
			&& backup_files(backups) == before,
		"exit %d: %s; disk afterwards: %s", status, one_line(output).String(),
		describe_disk(erase).String());

	// erase, applied
	status = plan_disk(erase, "--layout erase --apply", output);
	RengaDisk::GetDisk(erase.String(), info);
	const RengaDisk::PartitionInfo* esp = info.partitions.size() == 2
		? &info.partitions[0] : NULL;
	// the new volume's name, from "volume: <name>"
	BString newName;
	int32 line = output.FindFirst("\nvolume: ");
	if (line >= 0) {
		output.CopyInto(newName, line + 9, output.Length());
		int32 end = newName.FindFirst('\n');
		if (end >= 0)
			newName.Truncate(end);
	}
	BString bootName;
	{
		BVolume boot;
		char bootLabel[B_FILE_NAME_LENGTH] = {};
		if (BVolumeRoster().GetBootVolume(&boot) == B_OK)
			boot.GetName(bootLabel);
		bootName = bootLabel;
	}
	const RengaDisk::PartitionInfo* renga = volume_named(info,
		newName.String());
	BString expectedName = bootName == "Renga" ? "Renga 2" : "Renga";
	report("erase_apply", status == 0 && newName == expectedName
			&& info.partitioning == kPartitionTypeEFI
			&& info.partitions.size() == 2 && esp != NULL
			&& esp->size == 256 * kMiB && esp->type == "EFI system data"
			&& esp->content.FindFirst("FAT") >= 0
			&& esp->contentName.ICompare("EFI") == 0
			&& is_bfs(renga) && renga != esp
			&& renga->size >= 4 * kGiB - 256 * kMiB - 4 * kMiB
			&& renga->offset % kMiB == 0,
		"exit %d, volume \"%s\" (boot volume \"%s\"): %s; disk: %s", status,
		newName.String(), bootName.String(), one_line(output).String(),
		describe_disk(erase).String());

	// its backup and journal
	std::set<std::string> after = backup_files(backups);
	int tables = 0;
	BString journal, tableName;
	off_t tableSize = -1;
	for (const std::string& name : after) {
		if (before.count(name) != 0)
			continue;
		BString path(backups);
		path << "/" << name.c_str();
		if (BString(name.c_str()).EndsWith(".table")) {
			tables++;
			tableName = name.c_str();
			struct stat st;
			if (stat(path.String(), &st) == 0)
				tableSize = st.st_size;
		} else if (BString(name.c_str()).EndsWith(".journal"))
			journal = read_file(path);
	}
	report("erase_backup", tables == 1 && tableSize >= 2 * kMiB
			&& journal.FindFirst("step 1:") >= 0
			&& journal.FindFirst("step 5:") >= 0
			&& journal.FindFirst("complete") >= 0,
		"%d new table(s) in %s (%s, %" B_PRIdOFF " bytes); journal: %s",
		tables, backups.String(), tableName.String(), tableSize,
		one_line(journal).String());

	// the boot disk: refused as such, nothing written
	BString bootDisk;
	BVolume bootVolume;
	BDiskDeviceRoster roster;
	BDiskDevice bootDevice;
	BPartition* bootPartition;
	if (BVolumeRoster().GetBootVolume(&bootVolume) == B_OK
		&& roster.FindPartitionByVolume(bootVolume, &bootDevice,
			&bootPartition) == B_OK) {
		BPath path;
		bootDevice.GetPath(&path);
		bootDisk = path.Path();
	}
	if (bootDisk.Length() > 0) {
		BString was = describe_disk(bootDisk);
		std::set<std::string> beforeBoot = backup_files(backups);
		// a preview only: the refusal comes before it, and no write path
		// to the real boot disk is ever taken
		status = plan_disk(bootDisk, "--layout erase", output);
		report("refuses_boot_disk", status == 1
				&& output.FindFirst("started from") >= 0
				&& output.FindFirst("volume:") < 0
				&& output.FindFirst("new partition table") < 0
				&& describe_disk(bootDisk) == was
				&& backup_files(backups) == beforeBoot,
			"%s: exit %d: %s", bootDisk.String(), status,
			one_line(output).String());
	} else
		report("refuses_boot_disk", false, "no boot disk found");

	// too small, and "free" without a table
	std::set<std::string> beforeSmall = backup_files(backups);
	status = plan_disk(small, "--layout erase --apply", output);
	report("refuses_small", status == 1
			&& output.FindFirst("installing needs at least") >= 0
			&& output.FindFirst("disk has 1.0 GiB") >= 0
			&& describe_disk(small) == "no table"
			&& backup_files(backups) == beforeSmall,
		"exit %d: %s; disk: %s", status, one_line(output).String(),
		describe_disk(small).String());
	status = plan_disk(small, "--layout free --apply", output);
	report("refuses_free_without_table", status == 1
			&& output.FindFirst("no partition table") >= 0
			&& describe_disk(small) == "no table"
			&& backup_files(backups) == beforeSmall,
		"exit %d: %s", status, one_line(output).String());

	// a name another volume has
	BString arguments;
	arguments << "--layout erase --name '" << bootName << "' --apply";
	status = plan_disk(small, arguments.String(), output);
	report("refuses_taken_name", bootName.Length() > 0 && status == 1
			&& output.FindFirst("exists already") >= 0
			&& describe_disk(small) == "no table",
		"--name \"%s\": exit %d: %s", bootName.String(), status,
		one_line(output).String());

	// "free" beside other partitions
	check_free("free_after_partitions", behind, 512 * kMiB, "Keep",
		"RengaBehind", false, ownBackups);
	check_free("free_space_at_start", front, 2560 * kMiB, "KeepFront",
		"RengaFront", true, ownBackups);

	// an installation onto the erased disk's new volume
	check_install(erase, newName);

	// clean up: the volumes, the disks, the spare disk and the Installer's
	// backups
	for (int i = 0; i < kImageCount; i++) {
		unmount_all(images[i].disk);
		RengaDisk::DetachImage(images[i].path.String());
		unlink(images[i].path.String());
	}
	BString command;
	command << "rm -rf '" << ownBackups << "'";
	system(command.String());
	fs_unmount_volume(spareRoot.String(), 0);
	for (const std::string& name : backup_files(backups)) {
		if (before.count(name) == 0) {
			BString path(backups);
			path << "/" << name.c_str();
			unlink(path.String());
		}
	}

	printf("CE-SUITE %s DONE failures=%d\n", SUITE, sFailures);
	return sFailures == 0 ? 0 : 1;
}
