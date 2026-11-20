/*
 * Copyright 2003-2013, Axel Dörfler, axeld@pinc-software.de.
 * Distributed under the terms of the MIT License.
 */


#include <fcntl.h>
#include <stdlib.h>
#include <ByteOrder.h>
#include <unistd.h>
#include <string.h>
#include <stdio.h>

#include <boot/partitions.h>
#include <boot/platform.h>
#include <boot/vfs.h>

#include "Volume.h"
#include "Directory.h"
#include "Utility.h"


#define TRACE_BFS 0
#if TRACE_BFS
#	define TRACE(x) dprintf x
#else
#	define TRACE(x) ;
#endif


using namespace BFS;
using std::nothrow;


Volume::Volume(boot::Partition *partition)
	:
	fRootNode(NULL)
{
	fPartitionSize = partition->Size();
	fDevice = open_node(partition, O_RDONLY);
	if (fDevice < B_OK)
		return;

	if (read_pos(fDevice, 512, &fSuperBlock, sizeof(disk_super_block)) < B_OK)
		return;

	if (!IsValidSuperBlock()) {
#ifndef BFS_LITTLE_ENDIAN_ONLY
		// try block 0 again (can only happen on the big endian BFS)
		if (read_pos(fDevice, 0, &fSuperBlock, sizeof(disk_super_block)) < B_OK)
			return;

		if (!IsValidSuperBlock())
			return;
#else
		return;
#endif
	}

	TRACE(("bfs: we do have a valid superblock (name = %s)!\n", fSuperBlock.name));

	fRootNode = new(nothrow) BFS::Directory(*this, Root());
	if (fRootNode == NULL)
		return;

	if (fRootNode->InitCheck() < B_OK) {
		TRACE(("bfs: init check for root node failed\n"));
		delete fRootNode;
		fRootNode = NULL;
		return;
	}
}


Volume::~Volume()
{
	close(fDevice);
}


status_t
Volume::InitCheck()
{
	if (fDevice < B_OK)
		return B_ERROR;

	return fRootNode != NULL ? B_OK : B_ERROR;
}


bool
Volume::IsValidSuperBlock()
{
	if (fSuperBlock.Magic1() != (int32)SUPER_BLOCK_MAGIC1
		|| fSuperBlock.Magic2() != (int32)SUPER_BLOCK_MAGIC2
		|| fSuperBlock.Magic3() != (int32)SUPER_BLOCK_MAGIC3
		|| (int32)fSuperBlock.block_size != fSuperBlock.inode_size
		|| fSuperBlock.ByteOrder() != SUPER_BLOCK_FS_LENDIAN
		|| (1UL << fSuperBlock.BlockShift()) != fSuperBlock.BlockSize()
		|| fSuperBlock.AllocationGroups() < 1
		|| fSuperBlock.AllocationGroupShift() < 1
		|| fSuperBlock.BlocksPerAllocationGroup() < 1
		|| fSuperBlock.NumBlocks() < 10
		|| fSuperBlock.AllocationGroups() != divide_roundup(fSuperBlock.NumBlocks(), 1L << fSuperBlock.AllocationGroupShift()))
		return false;

	return true;
}


status_t
Volume::ValidateBlockRun(block_run run)
{
	if (run.AllocationGroup() < 0 || run.AllocationGroup() > (int32)AllocationGroups()
		|| run.Start() > (1UL << AllocationGroupShift())
		|| run.length == 0
		|| uint32(run.Length() + run.Start()) > (1UL << AllocationGroupShift())) {
		dprintf("bfs: invalid run(%" B_PRId32 ",%d,%d)\n",
			run.AllocationGroup(), run.Start(), run.Length());
		return B_BAD_DATA;
	}
	return B_OK;
}


/*!	Renga (batch 6, boot loader write safety): whether the boot loader may
	write file data into \a run, a run of the file whose inode is at block
	\a inodeBlock. Stricter than ValidateBlockRun(), which lets the
	allocation group equal the group count and is meant for reading:

	- the run lies inside one allocation group of this volume and inside
	  the volume's block count;
	- it is past the super block, the block bitmap and the log, so that no
	  file system structure the kernel relies on can be overwritten;
	- it does not contain the file's own inode;
	- the volume, and so the run, ends inside the partition it is on (a
	  super block claiming more blocks than the partition has is refused);
	- every block of it is marked used in the block bitmap.

	Any failure means "do not write"; the caller boots normally.
*/
status_t
Volume::CheckWritableRun(block_run run, off_t inodeBlock)
{
	if (run.AllocationGroup() < 0
		|| run.AllocationGroup() >= (int32)AllocationGroups()
		|| run.Length() == 0
		|| (uint32)run.Start() + run.Length()
			> (1UL << AllocationGroupShift())) {
		dprintf("bfs: write refused: run (%" B_PRId32 ",%u,%u) is not inside "
			"an allocation group\n", run.AllocationGroup(), run.Start(),
			run.Length());
		return B_BAD_DATA;
	}

	off_t block = ToBlock(run);
	off_t end = block + run.Length();
	if (end > NumBlocks()
		|| (NumBlocks() << BlockShift()) > fPartitionSize) {
		dprintf("bfs: write refused: blocks %" B_PRIdOFF "-%" B_PRIdOFF
			" are outside the volume or the partition\n", block, end - 1);
		return B_BAD_DATA;
	}

	// The block bitmap starts at block 1 and has one bit per block; the log
	// follows it. Data blocks come after both.
	off_t bitmapBlocks = (NumBlocks() + (BlockSize() * 8) - 1)
		/ (BlockSize() * 8);
	off_t firstData = 1 + bitmapBlocks;
	off_t logEnd = ToBlock(fSuperBlock.log_blocks)
		+ fSuperBlock.log_blocks.Length();
	if (logEnd > firstData)
		firstData = logEnd;
	if (block < firstData) {
		dprintf("bfs: write refused: block %" B_PRIdOFF " is in the file "
			"system's own area (data starts at %" B_PRIdOFF ")\n", block,
			firstData);
		return B_BAD_DATA;
	}
	if (inodeBlock >= block && inodeBlock < end) {
		dprintf("bfs: write refused: run contains the inode\n");
		return B_BAD_DATA;
	}

	return CheckBlocksAllocated(block, run.Length());
}


//!	Renga: whether \a count blocks from \a block are all used in the bitmap.
status_t
Volume::CheckBlocksAllocated(off_t block, off_t count)
{
	uint32 bitsPerBlock = BlockSize() * 8;
	uint8* bitmap = (uint8*)malloc(BlockSize());
	if (bitmap == NULL)
		return B_NO_MEMORY;

	off_t loaded = -1;
	status_t status = B_OK;
	for (off_t i = block; i < block + count; i++) {
		off_t bitmapBlock = 1 + i / bitsPerBlock;
		if (bitmapBlock != loaded) {
			if (read_pos(fDevice, ToOffset(bitmapBlock), bitmap, BlockSize())
					!= (ssize_t)BlockSize()) {
				status = B_IO_ERROR;
				break;
			}
			loaded = bitmapBlock;
		}
		uint32 bit = i % bitsPerBlock;
		// BFS's bitmap is an array of little-endian 32-bit words
		uint32 word = ((uint32*)bitmap)[bit / 32];
		word = B_LENDIAN_TO_HOST_INT32(word);
		if ((word & (1UL << (bit % 32))) == 0) {
			dprintf("bfs: write refused: block %" B_PRIdOFF " is free in "
				"the bitmap\n", i);
			status = B_BAD_DATA;
			break;
		}
	}
	free(bitmap);
	return status;
}


block_run
Volume::ToBlockRun(off_t block) const
{
	block_run run;
	run.allocation_group = HOST_ENDIAN_TO_BFS_INT32(block >> fSuperBlock.AllocationGroupShift());
	run.start = HOST_ENDIAN_TO_BFS_INT16(block & ~((1LL << fSuperBlock.AllocationGroupShift()) - 1));
	run.length = HOST_ENDIAN_TO_BFS_INT16(1);
	return run;
}


//	#pragma mark -


float
bfs_identify_file_system(boot::Partition *partition, off_t *_blockForChecksum)
{
	Volume volume(partition);

	if (volume.InitCheck() < B_OK)
		return 0;

	// Root inode has a unique be:volume_id attribute
	// within small_data plus some unique timestamps
	*_blockForChecksum = volume.BlockSize() * volume.ToBlock(volume.Root());
	return 0.8;
}


static status_t
bfs_get_file_system(boot::Partition *partition, ::Directory **_root)
{
	Volume *volume = new(nothrow) Volume(partition);
	if (volume == NULL)
		return B_NO_MEMORY;

	if (volume->InitCheck() < B_OK) {
		delete volume;
		return B_ERROR;
	}

	*_root = volume->RootNode();
	return B_OK;
}


file_system_module_info gBFSFileSystemModule = {
	"file_systems/bfs/v1",
	kPartitionTypeBFS,
	bfs_identify_file_system,
	bfs_get_file_system
};
