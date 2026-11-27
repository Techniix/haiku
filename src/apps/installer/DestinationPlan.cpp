/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 */


#include "DestinationPlan.h"

#include <DiskDevice.h>
#include <DiskDeviceRoster.h>
#include <DiskDeviceVisitor.h>
#include <Partition.h>
#include <PartitioningInfo.h>

#include <string.h>


const off_t kMinimumInstallSize = 2048LL * 1024 * 1024;
const off_t kEFIPartitionSize = 256LL * 1024 * 1024;
static const off_t kAlignment = RengaDisk::kPartitionAlignment;
	// where the disk engine starts every partition


bool
destination_layout_for_name(const char* name, destination_layout& layout)
{
	if (strcmp(name, "erase") == 0)
		layout = DESTINATION_ERASE_DISK;
	else if (strcmp(name, "free") == 0)
		layout = DESTINATION_FREE_SPACE;
	else
		return false;
	return true;
}


/*!	The most a new partition can get from one partitionable free space of
	the disk at \a path, or -1: the space from its first MiB boundary on,
	since the disk engine starts partitions there (a free space at the
	start of a GUID partition table begins at block 34, so its full size
	never fits).
*/
static off_t
largest_free_space(const char* path)
{
	BDiskDeviceRoster roster;
	BDiskDevice device;
	if (roster.GetDeviceForPath(path, &device) != B_OK)
		return -1;
	// the partitioning info comes from the disk system's add-on, which is
	// only there while modifications are prepared (without it the free
	// space read as "0 B" on every disk)
	if (device.PrepareModifications() != B_OK)
		return -1;
	BPartitioningInfo info;
	status_t status = device.GetPartitioningInfo(&info);
	device.CancelModifications();
	if (status != B_OK)
		return -1;
	off_t largest = 0;
	for (int32 i = 0; i < info.CountPartitionableSpaces(); i++) {
		off_t start, size;
		if (info.GetPartitionableSpaceAt(i, &start, &size) != B_OK)
			continue;
		off_t aligned = (start + kAlignment - 1) / kAlignment * kAlignment;
		off_t room = start + size - aligned;
		if (room > largest)
			largest = room;
	}
	return largest;
}


class VolumeNameFinder : public BDiskDeviceVisitor {
public:
	VolumeNameFinder(const char* name)
		:
		fName(name),
		fFound(false)
	{
	}

	virtual bool Visit(BDiskDevice* device)
	{
		return Visit(device, 0);
	}

	virtual bool Visit(BPartition* partition, int32 level)
	{
		if (partition->ContentName() == fName)
			fFound = true;
		return fFound;
	}

	BString	fName;
	bool	fFound;
};


bool
volume_name_exists(const char* name)
{
	VolumeNameFinder finder(name);
	BDiskDeviceRoster roster;
	BDiskDevice device;
	BPartition* partition;
	roster.VisitEachPartition(&finder, &device, &partition);
	return finder.fFound;
}


BString
unique_volume_name(const char* wanted)
{
	BString name(wanted);
	for (int32 number = 2; volume_name_exists(name.String())
			&& number < 100; number++) {
		name.SetToFormat("%s %" B_PRId32, wanted, number);
	}
	return name;
}


status_t
make_destination_plan(const RengaDisk::DiskInfo& disk,
	destination_layout layout, const char* volumeName, RengaDisk::Plan& plan,
	BString& why)
{
	RengaDisk::Operation operation;
	operation.index = -1;
	operation.size = 0;

	// the safety rule that matters most first: whatever else is wrong with
	// it, the disk the system runs from is never offered
	if (disk.isBootDisk) {
		why = "this is the disk the running system started from";
		return B_NOT_ALLOWED;
	}

	if (layout == DESTINATION_ERASE_DISK) {
		if (disk.size < kEFIPartitionSize + kMinimumInstallSize) {
			why.SetToFormat("the disk has %s; installing needs at least %s",
				RengaDisk::SizeString(disk.size).String(),
				RengaDisk::SizeString(kEFIPartitionSize
					+ kMinimumInstallSize).String());
			return B_DEVICE_FULL;
		}
		operation.kind = RengaDisk::kInitializePartitioning;
		operation.system = "gpt";
		plan.Add(operation);

		operation.kind = RengaDisk::kCreatePartition;
		operation.system = "";
		operation.size = kEFIPartitionSize;
		operation.name = "EFI system partition";
		operation.type = "EFI system data";
		plan.Add(operation);

		operation.size = 0;
		operation.name = volumeName;
		operation.type = "";
		plan.Add(operation);

		// on a new GPT table the two partitions are indices 0 and 1
		operation.kind = RengaDisk::kFormat;
		operation.index = 0;
		operation.system = "fat32";
		operation.name = "EFI";
		plan.Add(operation);

		operation.index = 1;
		operation.system = "bfs";
		operation.name = volumeName;
		plan.Add(operation);
		return B_OK;
	}

	if (disk.partitioning.Length() == 0) {
		why = "the disk has no partition table; erase it instead";
		return B_BAD_VALUE;
	}
	off_t free = largest_free_space(disk.path.String());
	if (free < 0) {
		why = "the disk's free space could not be read";
		return B_ERROR;
	}
	if (free < kMinimumInstallSize) {
		why.SetToFormat("the largest free space is %s; installing needs at "
			"least %s", RengaDisk::SizeString(free).String(),
			RengaDisk::SizeString(kMinimumInstallSize).String());
		return B_DEVICE_FULL;
	}
	operation.kind = RengaDisk::kCreatePartition;
	operation.size = free;
	operation.name = volumeName;
	plan.Add(operation);

	// The new partition's index is not known before it exists (a GUID
	// partition table gives it its first free entry, which comes before
	// the others when the first partition was deleted): the engine finds
	// it. "One past the highest index" formatted another partition then.
	operation.kind = RengaDisk::kFormat;
	operation.index = RengaDisk::kNewPartition;
	operation.system = "bfs";
	plan.Add(operation);
	return B_OK;
}
