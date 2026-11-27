/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 */
#ifndef _RENGA_DISK_ENGINE_H
#define _RENGA_DISK_ENGINE_H


/*!	Renga's shared disk-management engine (roadmap F-24, first part; the
	maintainer's requirement of 2026-11-09): one implementation of disk
	discovery, operating-system detection, planned changes with a preview,
	safety checks, applying, a journal and partition-table backups, used
	by every front end (the rengadisk command, and later the partition
	manager and the Installer's wizard, F-23), so that they can never
	handle disks differently.

	Nothing is written until Plan::Apply(). Its safety rules:
	  - never the disk the running system booted from;
	  - never a read-only disk, and no operation on a mounted partition
	    (deleting, formatting or renaming one, or re-partitioning a disk
	    with a mounted partition);
	  - only file-backed disk images (BDiskDeviceRoster::RegisterFileDevice)
	    unless the caller allows real disks explicitly (Plan::AllowRealDisks;
	    the harness never does);
	  - the first and last MiB of the disk (MBR, GPT header and table, and
	    the GPT backup) are saved to a backup file before the first write,
	    and RestoreTable() writes them back;
	  - every step is validated with the disk system first
	    (ValidateInitialize, ValidateCreateChild), applied and committed
	    one at a time, and written to a journal file beside the backup.

	Static library (libdiskengine.a), private: no ABI.
*/


#include <DiskDeviceDefs.h>
#include <String.h>
#include <SupportDefs.h>

#include <vector>


namespace RengaDisk {


struct PartitionInfo {
	partition_id	id;
	int32			index;
	off_t			offset;
	off_t			size;
	BString			name;			// in the partition table
	BString			type;			// partition type
	BString			content;		// file or partitioning system, "" none
	BString			contentName;	// volume name
	bool			mounted;
	BString			mountPoint;
	BString			system;			// detected operating system, or ""
	BString			warning;		// e.g. hibernated Windows, or ""
};


struct DiskInfo {
	partition_id	id;
	BString			path;			// /dev/disk/.../raw
	BString			filePath;		// the image file of a file device
	off_t			size;
	uint32			blockSize;
	bool			readOnly;
	bool			removable;
	bool			isFile;
	bool			isBootDisk;
	BString			partitioning;	// "GUID Partition Map", ... or ""
	std::vector<PartitionInfo> partitions;
};


//!	Every disk the system knows, with its partitions (first level).
status_t ListDisks(std::vector<DiskInfo>& disks);
//!	The disk with the raw device path or image file \a path.
status_t GetDisk(const char* path, DiskInfo& disk);

//!	A disk image file as a disk (BDiskDeviceRoster::RegisterFileDevice).
status_t AttachImage(const char* imagePath, BString& devicePath);
status_t DetachImage(const char* imagePath);

//!	Saves the first and last MiB of the disk at \a devicePath.
status_t BackupTable(const char* devicePath, const char* backupPath);
//!	Writes a BackupTable() file back to the same disk (same size). The
//!	system has no call to read a disk's table again, so a disk image is
//!	detached and attached again (\a newDevicePath gets its new path); a
//!	real disk would show the restored table after a restart.
status_t RestoreTable(const char* devicePath, const char* backupPath,
	BString* newDevicePath = NULL);

//!	Disk system names: "gpt", "mbr", "bfs", "fat32" -> pretty names.
const char* DiskSystemName(const char* shortName);


enum OperationKind {
	kInitializePartitioning,	// system: the disk's new partition table
	kCreatePartition,			// size (0: the rest), name, type
	kDeletePartition,			// index
	kFormat,					// index, system, name
	kSetName					// index, name
};


//!	Where new partitions start: on a multiple of this from the disk's
//!	start (what other systems' tools do; good for SSDs and 4K sectors).
static const off_t kPartitionAlignment = 1024 * 1024;


//!	Operation::index of a kFormat or kSetName step: the partition the
//!	plan's latest kCreatePartition step made. Its index is only known once
//!	it exists (a GUID partition table puts a new partition into its first
//!	free entry, which can be before the partitions there), so Apply()
//!	finds it by its offset.
static const int32 kNewPartition = -2;


struct Operation {
	OperationKind	kind;
	int32			index;		// partition index; -1 for the disk;
								// kNewPartition
	BString			system;
	off_t			size;
	BString			name;
	BString			type;
};


class Plan {
public:
								Plan(const char* devicePath);

			void				AllowRealDisks(bool allow)
									{ fAllowRealDisks = allow; }
			void				Add(const Operation& operation)
									{ fOperations.push_back(operation); }
			int32				CountOperations() const
									{ return fOperations.size(); }

	//!	The safety rules and each step against the current disk; \a why
	//!	gets the reason when it fails.
			status_t			Check(BString& why) const;
	//!	What will change, one line per step, and the warnings.
			BString				Preview() const;
	//!	Backs the table up into \a backupDir, then applies every step;
	//!	\a log gets one line per step (also written to the journal).
			status_t			Apply(const char* backupDir, BString& log,
									BString* backupPath = NULL);

private:
			BString				fDevicePath;
			bool				fAllowRealDisks;
			std::vector<Operation> fOperations;
};


//!	"16M", "2G", "512K", "1048576" -> bytes; -1 on a bad size.
off_t ParseSize(const char* text);
//!	Bytes as "64.0 MiB".
BString SizeString(off_t size);


}	// namespace RengaDisk


#endif	// _RENGA_DISK_ENGINE_H
