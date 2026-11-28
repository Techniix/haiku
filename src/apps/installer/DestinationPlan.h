/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 */
#ifndef DESTINATION_PLAN_H
#define DESTINATION_PLAN_H


/*!	Renga (batch 13, roadmap F-23, the wizard's Destination step, first
	part): the destination layouts the installer wizard is to offer, as plans of
	the shared disk engine (headers/private/diskengine/DiskEngine.h), so
	that the Installer prepares a disk exactly as the partition manager
	would, with the same safety rules, table backup and journal.

	  erase    the whole disk: a GUID partition table, an EFI system
	           partition (FAT32 "EFI", 256 MiB) and the rest as a BFS
	           volume for Renga
	  free     the largest free space on the disk: one new BFS volume
	           (the disk keeps its table and its other partitions;
	           "alongside" without resizing)

	The wizard (WizardWindow.cpp, batch 14) and the command line
	(Installer --plan-disk, --wizard-summary; InstallerCLI.cpp) both call
	these functions. "Alongside" with a resize comes later.
*/


#include <DiskEngine.h>
#include <Path.h>
#include <String.h>
#include <StringList.h>

#include <vector>


enum destination_layout {
	DESTINATION_ERASE_DISK,
	DESTINATION_FREE_SPACE
};


/*!	Fills \a plan for \a layout on \a disk. \a volumeName names the new BFS
	volume. Returns B_NOT_ALLOWED for the disk the running system started
	from (before anything else), B_DEVICE_FULL when the disk (or its
	largest free space) is smaller than kMinimumInstallSize, B_BAD_VALUE
	for "free" on a disk without a partition table; \a why says it in
	words. The plan's Check() applies the engine's other safety rules.
*/
status_t make_destination_plan(const RengaDisk::DiskInfo& disk,
	destination_layout layout, const char* volumeName,
	RengaDisk::Plan& plan, BString& why);

//!	Whether a partition on any disk has the volume name \a name.
bool volume_name_exists(const char* name);
//!	\a wanted, or "<wanted> 2", "<wanted> 3", ...: a volume name no
//!	partition has yet. The installation medium's own volume is "Renga",
//!	and "Installer --unattended --target <name>" needs the new volume's
//!	name to be unique. (Called by InstallerCLI.cpp only so far.)
BString unique_volume_name(const char* wanted);

//!	"erase" or "free"; false for anything else.
bool destination_layout_for_name(const char* name,
	destination_layout& layout);

/*!	A stretch of a disk for the wizard's before/after picture
	(DiskBarView): a partition, a free space, or a partition the plan
	makes.
*/
enum disk_segment_kind {
	SEGMENT_FREE,		// no partition
	SEGMENT_KEPT,		// a partition that stays as it is
	SEGMENT_NEW,		// a partition the plan makes
	SEGMENT_TARGET		// an existing volume the installation goes onto
};

struct DiskSegment {
	off_t				offset;
	off_t				size;
	BString				label;		// volume name, or system, or ""
	disk_segment_kind	kind;
};

/*!	The disk as it is: its partitions (\a target marked SEGMENT_TARGET,
	if it is one of them) and the free spaces between them, in order.
*/
void disk_segments_now(const RengaDisk::DiskInfo& disk,
	std::vector<DiskSegment>& segments, partition_id target = -1);
/*!	The disk as \a layout leaves it, placed the way the engine's Apply()
	places new partitions (the first free space that holds them, from its
	first MiB boundary).
*/
status_t disk_segments_planned(const RengaDisk::DiskInfo& disk,
	destination_layout layout, const char* volumeName,
	std::vector<DiskSegment>& segments);

/*!	The partition whose volume (content) name is \a name; -1 if none, or
	if more than one has it (\a count gets how many do).
*/
partition_id find_volume_named(const char* name, int32* count = NULL);

/*!	Whether \a now is still the disk \a planned describes: the same
	device, size, partition table and partitions (offset, size, type,
	name, content and volume name). The wizard checks it when Install is
	pressed, so that a plan never runs on a disk that changed after its
	summary was shown (another stick in the same port, DriveSetup started
	from the Deskbar). \a why gets the first difference.
*/
bool same_disk_layout(const RengaDisk::DiskInfo& planned,
	const RengaDisk::DiskInfo& now, BString& why);

/*!	The partition a plan created on the disk at \a diskPath: the one at
	\a offset (RengaDisk::Plan::CreatedOffset()), with a file system.
	-1 if there is none.
*/
partition_id created_partition(const char* diskPath, off_t offset);

//!	Whether \a name can name a new BFS volume (1 to 31 bytes, no '/');
//!	\a why says what is wrong.
bool valid_volume_name(const char* name, BString& why);

/*!	The volumes on the disk at \a diskPath, on every level (logical
	partitions inside an extended one too): "<name>, <size>[, <system>]"
	for the erase confirmation's list of what is lost.
*/
void disk_volume_descriptions(const RengaDisk::DiskInfo& disk,
	BStringList& descriptions);

//!	How the running system was started.
enum firmware_kind {
	FIRMWARE_UNKNOWN,
	FIRMWARE_BIOS,
	FIRMWARE_UEFI
};

/*!	The firmware that started the running system: BIOS when the boot loader
	passed the VESA mode list (the vesa graphics driver publishes
	/dev/graphics/vesa), UEFI when it passed only a frame buffer (the
	framebuffer driver publishes /dev/graphics/framebuffer, as on UEFI
	machines), unknown otherwise.
*/
firmware_kind running_firmware();
const char* firmware_name(firmware_kind firmware);

//!	Whether an EFI system partition of \a disk holds Haiku's or Renga's
//!	EFI loader (EFI/BOOT, EFI/HAIKU or EFI/RENGA); unmounted ones are
//!	mounted read-only for the look.
bool disk_has_haiku_efi_loader(const RengaDisk::DiskInfo& disk);

//!	Where the Installer keeps the disk engine's table backups and journals
//!	(~/config/settings/Renga/disk-backups), created if needed.
status_t disk_backup_directory(BPath& path);

//!	"4.0 GiB disk image, GUID Partition Map: Haiku, Windows": a disk the
//!	way the wizard names it (its size, kind, table and detected systems).
BString describe_disk(const RengaDisk::DiskInfo& disk);

//!	The smallest volume the wizard installs onto (an image's size plus
//!	room for the first updates).
extern const off_t kMinimumInstallSize;
//!	The EFI system partition's size on an erased disk.
extern const off_t kEFIPartitionSize;


#endif	// DESTINATION_PLAN_H
