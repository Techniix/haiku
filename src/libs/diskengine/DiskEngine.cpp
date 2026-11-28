/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 */

/*!	Renga's shared disk-management engine, first part (F-24). See
	headers/private/diskengine/DiskEngine.h for what it does and its
	safety rules. Built on the Disk Device API (BDiskDeviceRoster,
	BDiskDevice, BPartition), as DriveSetup is; each step runs in a
	modification session of its own (PrepareModifications(), the change,
	CommitModifications()), so a failed step leaves the earlier ones done
	and the journal says which.
*/


#include <DiskEngine.h>

#include <DiskDevice.h>
#include <DiskDeviceRoster.h>
#include <DiskDeviceTypes.h>
#include <DiskSystem.h>
#include <Entry.h>
#include <File.h>
#include <Partition.h>
#include <PartitioningInfo.h>
#include <Path.h>
#include <Volume.h>
#include <VolumeRoster.h>

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>


namespace RengaDisk {


static const off_t kTableArea = 1024 * 1024;
	// saved from each end of the disk: MBR and EBRs at the start, GPT
	// header and table at both ends
static const off_t kAlignment = kPartitionAlignment;
static const char kBackupMagic[8] = { 'R', 'G', 'D', 'T', 'B', 'L', '0', '1' };


struct BackupHeader {
	char	magic[8];
	int64	diskSize;
	int64	areaSize;
};


// #pragma mark - helpers


off_t
ParseSize(const char* text)
{
	if (text == NULL || text[0] == '\0')
		return -1;
	char* end;
	double value = strtod(text, &end);
	if (end == text || value < 0)
		return -1;
	off_t factor = 1;
	switch (*end) {
		case '\0': break;
		case 'k': case 'K': factor = 1024; end++; break;
		case 'm': case 'M': factor = 1024 * 1024; end++; break;
		case 'g': case 'G': factor = 1024LL * 1024 * 1024; end++; break;
		case 't': case 'T': factor = 1024LL * 1024 * 1024 * 1024; end++;
			break;
		default: return -1;
	}
	if (*end == 'i' || *end == 'I')
		end++;
	if (*end == 'b' || *end == 'B')
		end++;
	if (*end != '\0')
		return -1;
	return (off_t)(value * factor);
}


BString
SizeString(off_t size)
{
	const char* units[] = { "B", "KiB", "MiB", "GiB", "TiB" };
	double value = size;
	int unit = 0;
	while (value >= 1024 && unit < 4) {
		value /= 1024;
		unit++;
	}
	BString text;
	if (unit == 0)
		text.SetToFormat("%" B_PRIdOFF " B", size);
	else
		text.SetToFormat("%.1f %s", value, units[unit]);
	return text;
}


const char*
DiskSystemName(const char* shortName)
{
	if (shortName == NULL)
		return NULL;
	if (strcasecmp(shortName, "gpt") == 0)
		return kPartitionTypeEFI;
	if (strcasecmp(shortName, "mbr") == 0)
		return kPartitionTypeIntel;
	if (strcasecmp(shortName, "bfs") == 0)
		return kPartitionTypeBFS;
	if (strcasecmp(shortName, "fat32") == 0 || strcasecmp(shortName, "fat")
			== 0)
		return kPartitionTypeFAT32;
	return shortName;
}


static bool
exists(const BString& base, const char* relative)
{
	BString path(base);
	path << "/" << relative;
	return access(path.String(), F_OK) == 0;
}


/*!	The operating system on a partition: from its files when it is
	mounted, else from its file system. Warnings for what makes a
	partition unsafe to change (Windows' hibernation and BitLocker).
*/
static void
detect_system(BPartition* partition, PartitionInfo& info)
{
	if (info.mounted && info.mountPoint.Length() > 0) {
		const BString& root = info.mountPoint;
		if (exists(root, "system/packages")) {
			info.system = exists(root, "system/apps/Welcome")
				|| exists(root, "system/preferences/Boot menu")
				? "Renga" : "Haiku";
		} else if (exists(root, "Windows/System32")) {
			info.system = "Windows";
			if (exists(root, "hiberfil.sys")) {
				info.warning = "Windows may be hibernated or use Fast "
					"Startup: shut it down fully before changing this "
					"partition";
			}
		} else if (exists(root, "EFI/Microsoft/Boot/bootmgfw.efi")) {
			info.system = "EFI system partition (Windows)";
		} else if (exists(root, "EFI")) {
			info.system = "EFI system partition";
		} else if (exists(root, "etc/os-release")) {
			info.system = "Linux";
		} else if (exists(root, "System/Library/CoreServices")) {
			info.system = "macOS";
		}
		return;
	}
	const char* content = partition->ContentType();
	if (content == NULL)
		return;
	if (strstr(content, "NT File System") != NULL) {
		info.system = "Windows";
		info.warning = "NTFS: Windows may be hibernated, use Fast Startup "
			"or BitLocker; check in Windows before resizing";
	} else if (strstr(content, "ext") != NULL
		|| strstr(content, "Btrfs") != NULL
		|| strstr(content, "XFS") != NULL) {
		info.system = "Linux";
	} else if (strcmp(content, kPartitionTypeBFS) == 0)
		info.system = "Haiku or Renga";
	else if (strstr(content, "HFS") != NULL || strstr(content, "APFS") != NULL)
		info.system = "macOS";
}


static partition_id
boot_device_id()
{
	BVolume bootVolume;
	if (BVolumeRoster().GetBootVolume(&bootVolume) != B_OK)
		return -1;
	BDiskDeviceRoster roster;
	BDiskDevice device;
	BPartition* partition;
	if (roster.FindPartitionByVolume(bootVolume, &device, &partition)
			!= B_OK)
		return -1;
	return device.ID();
}


static void
fill_disk(BDiskDevice& device, partition_id bootID, DiskInfo& disk)
{
	BPath path;
	device.GetPath(&path);
	disk.id = device.ID();
	disk.path = path.Path();
	disk.size = device.Size();
	disk.blockSize = device.BlockSize();
	disk.readOnly = device.IsReadOnly() || device.IsReadOnlyMedia();
	disk.removable = device.IsRemovableMedia();
	disk.isFile = device.IsFile();
	disk.isBootDisk = bootID >= 0 && device.ID() == bootID;
	disk.filePath = "";
	if (disk.isFile) {
		BPath file;
		if (device.GetFilePath(&file) == B_OK)
			disk.filePath = file.Path();
	}
	disk.partitioning = device.ContainsPartitioningSystem()
		&& device.ContentType() != NULL ? device.ContentType() : "";
	disk.partitions.clear();
	for (int32 i = 0; i < device.CountChildren(); i++) {
		BPartition* child = device.ChildAt(i);
		PartitionInfo info;
		info.id = child->ID();
		info.index = child->Index();
		info.offset = child->Offset();
		info.size = child->Size();
		info.name = child->Name() != NULL ? child->Name() : "";
		info.type = child->Type() != NULL ? child->Type() : "";
		info.content = child->ContentType() != NULL ? child->ContentType()
			: "";
		info.contentName = child->ContentName();
		info.mounted = child->IsMounted();
		BPath mountPoint;
		if (info.mounted && child->GetMountPoint(&mountPoint) == B_OK)
			info.mountPoint = mountPoint.Path();
		detect_system(child, info);
		disk.partitions.push_back(info);
	}
	// a disk that is a volume itself (no partition table)
	if (disk.partitioning.Length() == 0 && device.ContainsFileSystem()) {
		PartitionInfo info;
		info.id = device.ID();
		info.index = -1;
		info.offset = 0;
		info.size = device.Size();
		info.content = device.ContentType() != NULL ? device.ContentType()
			: "";
		info.contentName = device.ContentName();
		info.mounted = device.IsMounted();
		BPath mountPoint;
		if (info.mounted && device.GetMountPoint(&mountPoint) == B_OK)
			info.mountPoint = mountPoint.Path();
		detect_system(&device, info);
		disk.partitions.push_back(info);
	}
}


status_t
ListDisks(std::vector<DiskInfo>& disks)
{
	disks.clear();
	partition_id bootID = boot_device_id();
	BDiskDeviceRoster roster;
	BDiskDevice device;
	while (roster.GetNextDevice(&device) == B_OK) {
		DiskInfo disk;
		fill_disk(device, bootID, disk);
		disks.push_back(disk);
	}
	return B_OK;
}


static status_t
get_device(const char* path, BDiskDevice& device)
{
	if (path == NULL)
		return B_BAD_VALUE;
	std::vector<DiskInfo> disks;
	ListDisks(disks);
	for (const DiskInfo& disk : disks) {
		if (disk.path == path || (disk.isFile && disk.filePath == path))
			return device.SetTo(disk.id);
	}
	return B_ENTRY_NOT_FOUND;
}


status_t
GetDisk(const char* path, DiskInfo& disk)
{
	BDiskDevice device;
	status_t status = get_device(path, device);
	if (status != B_OK)
		return status;
	fill_disk(device, boot_device_id(), disk);
	return B_OK;
}


status_t
AttachImage(const char* imagePath, BString& devicePath)
{
	BDiskDeviceRoster roster;
	partition_id id = roster.RegisterFileDevice(imagePath);
	if (id < 0)
		return id;
	BDiskDevice device;
	status_t status = device.SetTo(id);
	if (status != B_OK)
		return status;
	BPath path;
	device.GetPath(&path);
	devicePath = path.Path();
	return B_OK;
}


status_t
DetachImage(const char* imagePath)
{
	return BDiskDeviceRoster().UnregisterFileDevice(imagePath);
}


// #pragma mark - table backup


static status_t
read_exactly(int fd, void* buffer, size_t size, off_t offset)
{
	ssize_t done = pread(fd, buffer, size, offset);
	return done == (ssize_t)size ? B_OK : (done < 0 ? errno : B_IO_ERROR);
}


static status_t
write_exactly(int fd, const void* buffer, size_t size, off_t offset)
{
	ssize_t done = pwrite(fd, buffer, size, offset);
	return done == (ssize_t)size ? B_OK : (done < 0 ? errno : B_IO_ERROR);
}


static off_t
device_size(int fd)
{
	off_t size = lseek(fd, 0, SEEK_END);
	return size < 0 ? 0 : size;
}


status_t
BackupTable(const char* devicePath, const char* backupPath)
{
	int fd = open(devicePath, O_RDONLY);
	if (fd < 0)
		return errno;
	off_t size = device_size(fd);
	off_t area = std::min(kTableArea, size / 2);
	BackupHeader header;
	memcpy(header.magic, kBackupMagic, sizeof(header.magic));
	header.diskSize = size;
	header.areaSize = area;
	char* buffer = (char*)malloc(area * 2);
	status_t status = buffer != NULL ? B_OK : B_NO_MEMORY;
	if (status == B_OK)
		status = read_exactly(fd, buffer, area, 0);
	if (status == B_OK)
		status = read_exactly(fd, buffer + area, area, size - area);
	close(fd);
	if (status == B_OK) {
		FILE* file = fopen(backupPath, "wb");
		if (file == NULL)
			status = errno;
		else {
			if (fwrite(&header, sizeof(header), 1, file) != 1
				|| fwrite(buffer, area * 2, 1, file) != 1)
				status = B_IO_ERROR;
			if (fclose(file) != 0 && status == B_OK)
				status = errno;
		}
	}
	free(buffer);
	return status;
}


//!	Whether \a partition or any partition inside it (logical partitions
//!	of an extended one) is mounted.
static bool
mounted_within(BPartition* partition)
{
	if (partition == NULL)
		return false;
	if (partition->IsMounted())
		return true;
	for (int32 i = 0; i < partition->CountChildren(); i++) {
		if (mounted_within(partition->ChildAt(i)))
			return true;
	}
	return false;
}


//!	mounted_within() for the disk at \a path (\a index -1) or its
//!	first-level partition \a index.
static bool
mounted_on_disk(const char* path, int32 index = -1)
{
	BDiskDevice device;
	if (get_device(path, device) != B_OK)
		return false;
	if (index < 0)
		return mounted_within(&device);
	for (int32 i = 0; i < device.CountChildren(); i++) {
		if (device.ChildAt(i)->Index() == index)
			return mounted_within(device.ChildAt(i));
	}
	return false;
}


bool
TableExceedsDisk(const DiskInfo& disk, BString& why)
{
	for (const PartitionInfo& partition : disk.partitions) {
		if (partition.offset + partition.size > disk.size) {
			why.SetToFormat("partition %" B_PRId32 " ends at %s, past the "
				"disk's %s", partition.index,
				SizeString(partition.offset + partition.size).String(),
				SizeString(disk.size).String());
			return true;
		}
	}
	int fd = open(disk.path.String(), O_RDONLY);
	if (fd < 0)
		return false;
	uint32 blockSize = disk.blockSize > 0 ? disk.blockSize : 512;
	uint8 mbr[512];
	uint8* gpt = (uint8*)malloc(blockSize);
	bool exceeds = false;
	// MBR entries only when the disk has an Intel partition map: a volume
	// on the whole disk (FAT, NTFS, exFAT) has 0x55aa too, with boot code
	// where the entries would be
	if (disk.partitioning == kPartitionTypeIntel
		&& read_exactly(fd, mbr, sizeof(mbr), 0) == B_OK
		&& mbr[510] == 0x55 && mbr[511] == 0xaa) {
		// the four primary entries (a GPT's protective entry, type 0xee,
		// covers the whole disk or more by design)
		for (int i = 0; i < 4 && !exceeds; i++) {
			const uint8* entry = mbr + 446 + i * 16;
			uint32 start, count;
			memcpy(&start, entry + 8, 4);
			memcpy(&count, entry + 12, 4);
			if (entry[4] != 0 && entry[4] != 0xee
				&& ((off_t)start + count) * 512 > disk.size) {
				why.SetToFormat("MBR entry %d ends at %s, past the disk's %s",
					i, SizeString(((off_t)start + count) * 512).String(),
					SizeString(disk.size).String());
				exceeds = true;
			}
		}
	}
	if (!exceeds && gpt != NULL
		&& read_exactly(fd, gpt, blockSize, blockSize) == B_OK
		&& memcmp(gpt, "EFI PART", 8) == 0) {
		uint64 alternate, lastUsable;
		memcpy(&alternate, gpt + 32, 8);
		memcpy(&lastUsable, gpt + 48, 8);
		uint64 blocks = disk.size / blockSize;
		if (alternate >= blocks || lastUsable >= blocks) {
			why.SetToFormat("its GPT header is for a disk of %s, the disk "
				"has %s", SizeString((std::max(alternate, lastUsable) + 1)
					* blockSize).String(), SizeString(disk.size).String());
			exceeds = true;
		}
	}
	free(gpt);
	close(fd);
	return exceeds;
}


status_t
WipeTable(const char* devicePath, const char* backupDir, bool allowRealDisks,
	BString& log, BString* newDevicePath)
{
	if (newDevicePath != NULL)
		*newDevicePath = devicePath;
	DiskInfo disk;
	status_t status = GetDisk(devicePath, disk);
	if (status != B_OK) {
		log << "refused: " << devicePath << ": no such disk\n";
		return status;
	}
	BString why;
	if (disk.isBootDisk)
		why = "this is the disk the running system started from";
	else if (disk.readOnly)
		why = "the disk is read-only";
	else if (!disk.isFile && !allowRealDisks)
		why = "not a disk image: real disks need --allow-real-disks";
	if (why.Length() == 0 && mounted_on_disk(disk.path.String()))
		why = "a partition of the disk is mounted";
	if (why.Length() > 0) {
		log << "refused: " << why << "\n";
		return B_NOT_ALLOWED;
	}

	BString leaf(disk.path);
	leaf.ReplaceAll('/', '_');
	char stamp[32];
	time_t now = time(NULL);
	strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", localtime(&now));
	BString backupPath;
	backupPath.SetToFormat("%s/%s-%s-wipe.table", backupDir, leaf.String(),
		stamp);
	status = BackupTable(disk.path.String(), backupPath.String());
	if (status != B_OK) {
		log << "refused: the partition table could not be saved ("
			<< strerror(status) << ")\n";
		return status;
	}
	log << "table saved: " << backupPath << "\n";

	int fd = open(disk.path.String(), O_RDWR);
	if (fd < 0) {
		status = errno;
		log << "cannot open " << disk.path << " for writing: "
			<< strerror(status) << "; nothing was cleared\n";
		return status;
	}
	off_t size = device_size(fd);
	off_t area = std::min(kTableArea, size / 2);
	char* zeros = (char*)calloc(1, area);
	status = zeros != NULL ? B_OK : B_NO_MEMORY;
	if (status == B_OK)
		status = write_exactly(fd, zeros, area, 0);
	if (status == B_OK)
		status = write_exactly(fd, zeros, area, size - area);
	fsync(fd);
	close(fd);
	free(zeros);
	log << "first and last " << SizeString(area) << " cleared, with any "
		"data there: " << strerror(status) << "\n";

	// the system reads the table again: an image is attached again
	BDiskDevice device;
	BPath imageFile;
	if (status == B_OK && get_device(disk.path.String(), device) == B_OK
		&& device.IsFile() && device.GetFilePath(&imageFile) == B_OK) {
		BDiskDeviceRoster roster;
		roster.UnregisterFileDevice(device.ID());
		BString path;
		status = AttachImage(imageFile.Path(), path);
		if (status == B_OK && newDevicePath != NULL)
			*newDevicePath = path;
	} else if (status == B_OK)
		log << "the cleared table is read at the next restart\n";
	return status;
}


status_t
RestoreTable(const char* devicePath, const char* backupPath,
	BString* newDevicePath)
{
	if (newDevicePath != NULL)
		*newDevicePath = devicePath;
	FILE* file = fopen(backupPath, "rb");
	if (file == NULL)
		return errno;
	BackupHeader header;
	status_t status = fread(&header, sizeof(header), 1, file) == 1
		&& memcmp(header.magic, kBackupMagic, sizeof(header.magic)) == 0
		&& header.areaSize > 0 && header.areaSize <= kTableArea
		? B_OK : B_BAD_DATA;
	char* buffer = NULL;
	if (status == B_OK) {
		buffer = (char*)malloc(header.areaSize * 2);
		if (buffer == NULL)
			status = B_NO_MEMORY;
		else if (fread(buffer, header.areaSize * 2, 1, file) != 1)
			status = B_BAD_DATA;
	}
	fclose(file);
	if (status != B_OK) {
		free(buffer);
		return status;
	}
	int fd = open(devicePath, O_RDWR);
	if (fd < 0) {
		free(buffer);
		return errno;
	}
	if (device_size(fd) != header.diskSize)
		status = B_MISMATCHED_VALUES;
		// not the disk it was taken from (or it was resized)
	if (status == B_OK)
		status = write_exactly(fd, buffer, header.areaSize, 0);
	if (status == B_OK) {
		status = write_exactly(fd, buffer + header.areaSize,
			header.areaSize, header.diskSize - header.areaSize);
	}
	fsync(fd);
	close(fd);
	free(buffer);
	// the system reads the restored table again: there is no call for
	// that, so an image is detached and attached again
	BDiskDevice device;
	BPath imageFile;
	if (status == B_OK && get_device(devicePath, device) == B_OK
		&& device.IsFile() && device.GetFilePath(&imageFile) == B_OK) {
		BDiskDeviceRoster roster;
		roster.UnregisterFileDevice(device.ID());
		BString path;
		status = AttachImage(imageFile.Path(), path);
		if (status == B_OK && newDevicePath != NULL)
			*newDevicePath = path;
	}
	return status;
}


// #pragma mark - Plan


Plan::Plan(const char* devicePath)
	:
	fDevicePath(devicePath),
	fAllowRealDisks(false),
	fCreatedOffset(-1),
	fExpectedDevice(-1)
{
}


static BString
partition_name(int32 index)
{
	BString text;
	if (index == kNewPartition)
		text = "the new partition";
	else
		text.SetToFormat("partition %" B_PRId32, index);
	return text;
}


static BString
describe(const Operation& operation)
{
	BString text;
	switch (operation.kind) {
		case kInitializePartitioning:
			text.SetToFormat("new partition table: %s (every partition on "
				"the disk is lost)", DiskSystemName(operation.system));
			break;
		case kCreatePartition:
			text.SetToFormat("new partition %s\"%s\"%s%s",
				operation.size > 0 ? (SizeString(operation.size) << " ")
					.String() : "with the free space ",
				operation.name.String(),
				operation.type.Length() > 0 ? ", type " : "",
				operation.type.String());
			break;
		case kDeletePartition:
			text.SetToFormat("delete partition %" B_PRId32 " (its data is "
				"lost)", operation.index);
			break;
		case kFormat:
			text.SetToFormat("format %s as %s named \"%s\"%s",
				partition_name(operation.index).String(),
				DiskSystemName(operation.system), operation.name.String(),
				operation.index == kNewPartition ? "" : " (its data is lost)");
			break;
		case kSetName:
			text.SetToFormat("name %s \"%s\"",
				partition_name(operation.index).String(),
				operation.name.String());
			break;
	}
	return text;
}


BString
Plan::Describe(int32 index) const
{
	if (index < 0 || index >= (int32)fOperations.size())
		return "";
	return describe(fOperations[index]);
}


status_t
Plan::Check(BString& why) const
{
	DiskInfo disk;
	status_t status = GetDisk(fDevicePath.String(), disk);
	if (status != B_OK) {
		why.SetToFormat("%s: no such disk", fDevicePath.String());
		return status;
	}
	if (disk.isBootDisk) {
		why = "this is the disk the running system started from";
		return B_NOT_ALLOWED;
	}
	if (fExpectedDevice >= 0 && disk.id != fExpectedDevice) {
		why.SetToFormat("%s is another disk now (device %" B_PRId32 ", the "
			"plan was made for device %" B_PRId32 "): it was replaced",
			fDevicePath.String(), disk.id, fExpectedDevice);
		return B_DEV_ID_ERROR;
	}
	if (disk.readOnly) {
		why = "the disk is read-only";
		return B_READ_ONLY_DEVICE;
	}
	if (!disk.isFile && !fAllowRealDisks) {
		why = "not a disk image: real disks need --allow-real-disks";
		return B_NOT_ALLOWED;
	}
	if (fOperations.empty()) {
		why = "nothing to do";
		return B_BAD_VALUE;
	}
	// changing a table that describes a larger disk crashed libbe
	// (BPartition::Delegate::Uninitialize, 20261128-003328)
	BString exceeds;
	if (TableExceedsDisk(disk, exceeds)) {
		if (fOperations[0].kind == kInitializePartitioning) {
			// the plan replaces the table anyway
			why.SetToFormat("the partition table describes a larger disk "
				"(%s); clear it first with \"rengadisk wipe %s\" (the table "
				"is saved before; the first and last MiB are cleared, with "
				"any data there)", exceeds.String(), disk.path.String());
		} else {
			// the disk's other partitions are to stay: no wiping
			why.SetToFormat("the partition table describes a larger disk "
				"(%s): it is inconsistent. Repair it with the tools of the "
				"system that made it, or erase the disk instead (a new "
				"table)", exceeds.String());
		}
		return B_BAD_DATA;
	}

	// the partitions as each step leaves them (by index; mounted or not)
	// mounted here means: the partition or one inside it (the logical
	// partitions of an extended one)
	std::vector<bool> mounted;
	for (const PartitionInfo& partition : disk.partitions) {
		if (partition.index >= 0) {
			mounted.push_back(partition.mounted
				|| mounted_on_disk(disk.path.String(), partition.index));
		}
	}
	bool hasTable = disk.partitioning.Length() > 0;
	bool created = false;
	for (size_t i = 0; i < fOperations.size(); i++) {
		const Operation& operation = fOperations[i];
		BString step;
		step.SetToFormat("step %d (%s): ", (int)i + 1,
			describe(operation).String());
		switch (operation.kind) {
			case kInitializePartitioning:
				for (bool isMounted : mounted) {
					if (isMounted) {
						why = step << "a partition of the disk is mounted";
						return B_BUSY;
					}
				}
				// a volume on the whole disk (no table) counts too
				if (i == 0 && mounted_on_disk(disk.path.String())) {
					why = step << "a volume on the disk is mounted";
					return B_BUSY;
				}
				if (strcmp(DiskSystemName(operation.system),
						kPartitionTypeEFI) != 0
					&& strcmp(DiskSystemName(operation.system),
						kPartitionTypeIntel) != 0) {
					why = step << "partition tables are gpt or mbr";
					return B_BAD_VALUE;
				}
				mounted.clear();
				hasTable = true;
				break;
			case kCreatePartition:
				if (!hasTable) {
					why = step << "the disk has no partition table";
					return B_BAD_VALUE;
				}
				if (operation.size < 0) {
					why = step << "bad size";
					return B_BAD_VALUE;
				}
				mounted.push_back(false);
				created = true;
				break;
			case kDeletePartition:
			case kFormat:
			case kSetName:
				if (operation.index == kNewPartition) {
					// a partition this plan makes is never mounted
					if (!created || operation.kind == kDeletePartition) {
						why = step << "no new partition before this step";
						return B_BAD_INDEX;
					}
					break;
				}
				if (operation.index < 0
					|| operation.index >= (int32)mounted.size()) {
					why = step << "no such partition";
					return B_BAD_INDEX;
				}
				if (mounted[operation.index]) {
					why = step << "the partition is mounted (unmount it "
						"first)";
					return B_BUSY;
				}
				if (operation.kind == kDeletePartition)
					mounted.erase(mounted.begin() + operation.index);
				break;
		}
	}
	return B_OK;
}


BString
Plan::Preview() const
{
	BString text;
	DiskInfo disk;
	if (GetDisk(fDevicePath.String(), disk) == B_OK) {
		text.SetToFormat("disk %s (%s%s), %s\n", disk.path.String(),
			SizeString(disk.size).String(), disk.isFile ? ", disk image" : "",
			disk.partitioning.Length() > 0 ? disk.partitioning.String()
				: "no partition table");
		for (const PartitionInfo& partition : disk.partitions) {
			BString line;
			line.SetToFormat("  now: %" B_PRId32 " %s \"%s\" %s%s%s\n",
				partition.index, SizeString(partition.size).String(),
				partition.name.String(), partition.content.String(),
				partition.system.Length() > 0 ? ", " : "",
				partition.system.String());
			text << line;
			if (partition.warning.Length() > 0)
				text << "  warning: " << partition.warning << "\n";
		}
	}
	for (size_t i = 0; i < fOperations.size(); i++) {
		BString line;
		line.SetToFormat("  %d. %s\n", (int)i + 1,
			describe(fOperations[i]).String());
		text << line;
	}
	text << "  the partition table is saved first (rengadisk restore)\n";
	return text;
}


static BPartition*
child(BDiskDevice& device, int32 index)
{
	for (int32 i = 0; i < device.CountChildren(); i++) {
		BPartition* partition = device.ChildAt(i);
		if (partition->Index() == index)
			return partition;
	}
	return NULL;
}


//!	A child type the parent offers: \a wanted if it is offered, else BFS
//!	("Be File System"), else the first one.
static BString
child_type(BPartition* parent, const BString& wanted)
{
	BString first;
	BString bfs;
	BString type;
	int32 cookie = 0;
	while (parent->GetNextSupportedChildType(&cookie, &type) == B_OK) {
		if (wanted.Length() > 0 && wanted.ICompare(type) == 0)
			return type;
		if (first.Length() == 0)
			first = type;
		if (type == kPartitionTypeBFS)
			bfs = type;
	}
	return bfs.Length() > 0 ? bfs : first;
}


static status_t
apply_one(BDiskDevice& device, const Operation& operation, BString& detail,
	off_t& createdOffset)
{
	status_t status = device.PrepareModifications();
	if (status != B_OK) {
		detail = "cannot prepare the disk";
		return status;
	}
	switch (operation.kind) {
		case kInitializePartitioning:
		{
			const char* system = DiskSystemName(operation.system);
			if (device.ContainsPartitioningSystem()
				|| device.ContainsFileSystem()) {
				status = device.Uninitialize();
			}
			if (status == B_OK)
				status = device.ValidateInitialize(system, NULL, "");
			if (status == B_OK)
				status = device.Initialize(system, NULL, "");
			break;
		}
		case kCreatePartition:
		{
			BPartitioningInfo info;
			status = device.GetPartitioningInfo(&info);
			off_t start = 0;
			off_t size = 0;
			// the first free space that holds it
			for (int32 i = 0; status == B_OK
					&& i < info.CountPartitionableSpaces(); i++) {
				off_t spaceStart, spaceSize;
				info.GetPartitionableSpaceAt(i, &spaceStart, &spaceSize);
				// partitions start on a MiB boundary (what other systems'
				// tools do; good for SSDs and 4K sectors)
				off_t aligned = (spaceStart + kAlignment - 1) / kAlignment
					* kAlignment;
				off_t room = spaceStart + spaceSize - aligned;
				if (room <= 0)
					continue;
				if (operation.size == 0 || room >= operation.size) {
					start = aligned;
					size = operation.size == 0 ? room : operation.size;
					break;
				}
			}
			if (status == B_OK && size == 0) {
				detail = "not enough free space";
				status = B_DEVICE_FULL;
			}
			BString type = child_type(&device, operation.type);
			BString name = operation.name;
			if (status == B_OK) {
				status = device.ValidateCreateChild(&start, &size,
					type.String(), &name, "");
			}
			if (status == B_OK) {
				status = device.CreateChild(start, size, type.String(),
					name.String(), "");
				createdOffset = start;
				detail.SetToFormat("at %" B_PRIdOFF ", %s, type %s", start,
					SizeString(size).String(), type.String());
			}
			break;
		}
		case kDeletePartition:
		{
			BPartition* partition = child(device, operation.index);
			if (partition == NULL)
				status = B_BAD_INDEX;
			else if (!device.CanDeleteChild(partition->Index()))
				status = B_NOT_ALLOWED;
			else
				status = device.DeleteChild(partition->Index());
			break;
		}
		case kFormat:
		{
			BPartition* partition = child(device, operation.index);
			const char* system = DiskSystemName(operation.system);
			BString name = operation.name;
			if (partition == NULL)
				status = B_BAD_INDEX;
			else {
				if (partition->ContainsFileSystem()
					|| partition->ContainsPartitioningSystem())
					status = partition->Uninitialize();
				if (status == B_OK) {
					status = partition->ValidateInitialize(system, &name,
						"");
				}
				if (status == B_OK)
					status = partition->Initialize(system, name.String(),
						"");
			}
			break;
		}
		case kSetName:
		{
			BPartition* partition = child(device, operation.index);
			BString name = operation.name;
			if (partition == NULL)
				status = B_BAD_INDEX;
			else if (!partition->CanSetName())
				status = B_NOT_SUPPORTED;
			else {
				status = partition->ValidateSetName(&name);
				if (status == B_OK)
					status = partition->SetName(name.String());
			}
			break;
		}
	}
	if (status == B_OK)
		status = device.CommitModifications();
	else
		device.CancelModifications();
	return status;
}


status_t
Plan::Apply(const char* backupDir, BString& log, BString* _backupPath)
{
	BString why;
	status_t status = Check(why);
	if (status != B_OK) {
		log << "refused: " << why << "\n";
		return status;
	}

	// the table first, and a journal beside it
	mkdir(backupDir, 0700);
	BString leaf(fDevicePath);
	leaf.ReplaceAll('/', '_');
	char stamp[32];
	time_t now = time(NULL);
	strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", localtime(&now));
	BString backupPath;
	backupPath.SetToFormat("%s/%s-%s.table", backupDir, leaf.String(), stamp);
	DiskInfo disk;
	GetDisk(fDevicePath.String(), disk);
	if (fExpectedDevice >= 0 && disk.id != fExpectedDevice) {
		log << "refused: " << fDevicePath << " is another disk now (device "
			<< disk.id << ", the plan was made for device " << fExpectedDevice
			<< "): it was replaced\n";
		return B_DEV_ID_ERROR;
	}
	status = BackupTable(disk.path.String(), backupPath.String());
	if (status != B_OK) {
		log << "refused: the partition table could not be saved ("
			<< strerror(status) << ")\n";
		return status;
	}
	if (_backupPath != NULL)
		*_backupPath = backupPath;
	BString journalPath(backupPath);
	journalPath.RemoveLast(".table");
	journalPath << ".journal";
	FILE* journal = fopen(journalPath.String(), "w");
	log << "table saved: " << backupPath << "\n";
	if (journal != NULL)
		fprintf(journal, "disk %s\ntable %s\n", disk.path.String(),
			backupPath.String());

	off_t createdOffset = -1;
	fCreatedOffset = -1;
	for (size_t i = 0; i < fOperations.size(); i++) {
		BDiskDevice device;
		status = get_device(disk.path.String(), device);
		BString detail;
		Operation operation = fOperations[i];
		// the same disk as when the plan was checked: a disk swapped in
		// the same port while the plan runs gets nothing
		partition_id expected = fExpectedDevice >= 0 ? fExpectedDevice
			: disk.id;
		if (status == B_OK && device.ID() != expected) {
			detail.SetToFormat("%s is another disk now (device %" B_PRId32
				", expected %" B_PRId32 ")", disk.path.String(), device.ID(),
				expected);
			status = B_DEV_ID_ERROR;
		}
		if (status == B_OK && operation.index == kNewPartition) {
			// the partition the latest create step made, by its offset
			operation.index = -1;
			for (int32 c = 0; c < device.CountChildren(); c++) {
				if (createdOffset >= 0
					&& device.ChildAt(c)->Offset() == createdOffset)
					operation.index = device.ChildAt(c)->Index();
			}
			if (operation.index < 0) {
				detail = "the new partition is not there";
				status = B_BAD_INDEX;
			} else
				detail.SetToFormat("partition %" B_PRId32, operation.index);
		}
		off_t offset = -1;
		if (status == B_OK) {
			BString stepDetail;
			status = apply_one(device, operation, stepDetail, offset);
			if (detail.Length() > 0 && stepDetail.Length() > 0)
				detail << ", ";
			detail << stepDetail;
		}
		if (status == B_OK && operation.kind == kCreatePartition) {
			createdOffset = offset;
			fCreatedOffset = offset;
		}
		BString line;
		line.SetToFormat("step %d: %s: %s%s%s\n", (int)i + 1,
			describe(fOperations[i]).String(),
			status == B_OK ? "done" : strerror(status),
			detail.Length() > 0 ? "; " : "", detail.String());
		log << line;
		if (journal != NULL) {
			fputs(line.String(), journal);
			fflush(journal);
		}
		if (status != B_OK) {
			log << "stopped; earlier steps are done. To undo them: "
				"rengadisk restore " << disk.path << " " << backupPath
				<< "\n";
			break;
		}
	}
	if (journal != NULL) {
		fprintf(journal, "%s\n", status == B_OK ? "complete" : "stopped");
		fclose(journal);
	}
	return status;
}


}	// namespace RengaDisk
