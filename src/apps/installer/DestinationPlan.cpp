/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 */


#include "DestinationPlan.h"

#include <DiskDevice.h>
#include <DiskDeviceRoster.h>
#include <PartitioningInfo.h>

#include <string.h>


const off_t kMinimumInstallSize = 2048LL * 1024 * 1024;
const off_t kEFIPartitionSize = 256LL * 1024 * 1024;


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


//!	The largest partitionable free space of the disk at \a path, or -1.
static off_t
largest_free_space(const char* path)
{
	BDiskDeviceRoster roster;
	BDiskDevice device;
	if (roster.GetDeviceForPath(path, &device) != B_OK)
		return -1;
	BPartitioningInfo info;
	if (device.GetPartitioningInfo(&info) != B_OK)
		return -1;
	off_t largest = 0;
	for (int32 i = 0; i < info.CountPartitionableSpaces(); i++) {
		off_t start, size;
		if (info.GetPartitionableSpaceAt(i, &start, &size) == B_OK
			&& size > largest) {
			largest = size;
		}
	}
	return largest;
}


status_t
make_destination_plan(const RengaDisk::DiskInfo& disk,
	destination_layout layout, const char* volumeName, RengaDisk::Plan& plan,
	BString& why)
{
	RengaDisk::Operation operation;
	operation.index = -1;
	operation.size = 0;

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
	if (free < kMinimumInstallSize) {
		why.SetToFormat("the largest free space is %s; installing needs at "
			"least %s", RengaDisk::SizeString(free < 0 ? 0 : free).String(),
			RengaDisk::SizeString(kMinimumInstallSize).String());
		return B_DEVICE_FULL;
	}
	// the new partition's index: one past the highest one there
	int32 index = 0;
	for (size_t i = 0; i < disk.partitions.size(); i++) {
		if (disk.partitions[i].index >= index)
			index = disk.partitions[i].index + 1;
	}
	operation.kind = RengaDisk::kCreatePartition;
	operation.size = free;
	operation.name = volumeName;
	plan.Add(operation);

	operation.kind = RengaDisk::kFormat;
	operation.index = index;
	operation.system = "bfs";
	plan.Add(operation);
	return B_OK;
}
