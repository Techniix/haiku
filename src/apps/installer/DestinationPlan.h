/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 */
#ifndef DESTINATION_PLAN_H
#define DESTINATION_PLAN_H


/*!	Renga (batch 13, roadmap F-23, the wizard's Destination step, first
	part): the destination layouts the installer wizard offers, as plans of
	the shared disk engine (headers/private/diskengine/DiskEngine.h), so
	that the Installer prepares a disk exactly as the partition manager
	would, with the same safety rules, table backup and journal.

	  erase    the whole disk: a GUID partition table, an EFI system
	           partition (FAT32 "EFI", 256 MiB) and the rest as a BFS
	           volume for Renga
	  free     the largest free space on the disk: one new BFS volume
	           (the disk keeps its table and its other partitions;
	           "alongside" without resizing)

	The wizard's window (pages Welcome, Destination, Summary, Progress)
	and "alongside" with a resize come later; DestinationPlan is shared by
	the command line (Installer --plan-disk) and the window.
*/


#include <DiskEngine.h>
#include <String.h>


enum destination_layout {
	DESTINATION_ERASE_DISK,
	DESTINATION_FREE_SPACE
};


/*!	Fills \a plan for \a layout on \a disk. \a volumeName names the new BFS
	volume. Returns B_DEVICE_FULL when the disk (or its largest free space)
	is smaller than kMinimumInstallSize, B_BAD_VALUE for "free" on a disk
	without a partition table; \a why says it in words.
*/
status_t make_destination_plan(const RengaDisk::DiskInfo& disk,
	destination_layout layout, const char* volumeName,
	RengaDisk::Plan& plan, BString& why);

//!	"erase" or "free"; false for anything else.
bool destination_layout_for_name(const char* name,
	destination_layout& layout);

//!	The smallest volume the wizard installs onto (an image's size plus
//!	room for the first updates).
extern const off_t kMinimumInstallSize;
//!	The EFI system partition's size on an erased disk.
extern const off_t kEFIPartitionSize;


#endif	// DESTINATION_PLAN_H
