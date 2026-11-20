/*
 * Copyright 2003-2013, Axel Dörfler, axeld@pinc-software.de.
 * This file may be used under the terms of the MIT License.
 */


//!	Inode stream access functions


#include "Stream.h"
#include "Directory.h"
#include "File.h"
#include "Link.h"

#include <stdlib.h>
#include <unistd.h>
#include <string.h>


using namespace BFS;
using std::nothrow;


class CachedBlock {
public:
								CachedBlock(Volume& volume);
								CachedBlock(Volume& volume, block_run run);
								~CachedBlock();

			uint8*				SetTo(block_run run);
			uint8*				SetTo(off_t offset);

			void				Unset();

			uint8*				Block() const { return fBlock; }
			off_t				BlockNumber() const { return fBlockNumber; }
			uint32				BlockSize() const { return fVolume.BlockSize(); }
			uint32				BlockShift() const
									{ return fVolume.BlockShift(); }

private:
			Volume&				fVolume;
			off_t				fBlockNumber;
			uint8*				fBlock;
};


CachedBlock::CachedBlock(Volume& volume)
	:
	fVolume(volume),
	fBlockNumber(-1LL),
	fBlock(NULL)
{
}


CachedBlock::CachedBlock(Volume &volume, block_run run)
	:
	fVolume(volume),
	fBlockNumber(-1LL),
	fBlock(NULL)
{
	SetTo(run);
}


CachedBlock::~CachedBlock()
{
	free(fBlock);
}


inline void
CachedBlock::Unset()
{
	fBlockNumber = -1;
}


inline uint8*
CachedBlock::SetTo(off_t block)
{
	if (block == fBlockNumber)
		return fBlock;
	if (fBlock == NULL) {
		fBlock = (uint8*)malloc(BlockSize());
		if (fBlock == NULL)
			return NULL;
	}

	fBlockNumber = block;
	if (read_pos(fVolume.Device(), block << BlockShift(), fBlock, BlockSize())
			< (ssize_t)BlockSize())
		return NULL;

	return fBlock;
}


inline uint8*
CachedBlock::SetTo(block_run run)
{
	return SetTo(fVolume.ToBlock(run));
}


//	#pragma mark -


Stream::Stream(Volume& volume, block_run run)
	:
	fVolume(volume),
	fBlock(volume.ToBlock(run))
{
	if (read_pos(volume.Device(), volume.ToOffset(run), this, sizeof(bfs_inode))
			!= sizeof(bfs_inode))
		return;
}


Stream::Stream(Volume& volume, off_t id)
	:
	fVolume(volume),
	fBlock(id)
{
	if (read_pos(volume.Device(), volume.ToOffset(id), this, sizeof(bfs_inode))
			!= sizeof(bfs_inode))
		return;
}


Stream::~Stream()
{
}


status_t
Stream::InitCheck()
{
	return bfs_inode::InitCheck(&fVolume);
}


status_t
Stream::GetNextSmallData(const small_data** _smallData) const
{
	// TODO: Stream derives from bfs_inode and we read only sizeof(bfs_inode)
	// bytes from disk, i.e. the small data region is not in memory.
	panic("Stream::GetNextSmallData(): small data region is not loaded!");

	const small_data* smallData = *_smallData;

	// begin from the start?
	if (smallData == NULL)
		smallData = small_data_start;
	else
		smallData = smallData->Next();

	// is already last item?
	if (smallData->IsLast(this))
		return B_ENTRY_NOT_FOUND;

	*_smallData = smallData;

	return B_OK;
}


status_t
Stream::GetName(char* name, size_t size) const
{
	const small_data* smallData = NULL;
	while (GetNextSmallData(&smallData) == B_OK) {
		if (*smallData->Name() == FILE_NAME_NAME
			&& smallData->NameSize() == FILE_NAME_NAME_LENGTH) {
			strlcpy(name, (const char*)smallData->Data(), size);
			return B_OK;
		}
	}
	return B_ERROR;
}


status_t
Stream::ReadLink(char* buffer, size_t bufferSize)
{
	// link in the stream

	if (Flags() & INODE_LONG_SYMLINK)
		return ReadAt(0, (uint8*)buffer, &bufferSize);

	// link in the inode

	strlcpy(buffer, short_symlink, bufferSize);
	return B_OK;
}


status_t
Stream::FindBlockRun(off_t pos, block_run& run, off_t& offset)
{
	// find matching block run

	if (data.MaxDirectRange() > 0 && pos >= data.MaxDirectRange()) {
		if (data.MaxDoubleIndirectRange() > 0
			&& pos >= data.MaxIndirectRange()) {
			// access to double indirect blocks

			CachedBlock cached(fVolume);

			int32 runsPerBlock;
			int32 directSize;
			int32 indirectSize;
			get_double_indirect_sizes(data.double_indirect.Length(),
				cached.BlockSize(), runsPerBlock, directSize, indirectSize);

			off_t start = pos - data.MaxIndirectRange();
			int32 index = start / indirectSize;

			block_run* indirect = (block_run*)cached.SetTo(
				fVolume.ToBlock(data.double_indirect) + index / runsPerBlock);
			if (indirect == NULL)
				return B_ERROR;

			//printf("\tstart = %lld, indirectSize = %ld, directSize = %ld, index = %ld\n",start,indirectSize,directSize,index);
			//printf("\tlook for indirect block at %ld,%d\n",indirect[index].allocation_group,indirect[index].start);

			int32 current = (start % indirectSize) / directSize;

			indirect = (block_run*)cached.SetTo(fVolume.ToBlock(indirect[
					index % runsPerBlock]) + current / runsPerBlock);
			if (indirect == NULL)
				return B_ERROR;

			run = indirect[current % runsPerBlock];
			offset = data.MaxIndirectRange() + (index * indirectSize)
				+ (current * directSize);
			//printf("\tfCurrent = %ld, fRunFileOffset = %lld, fRunBlockEnd = %lld, fRun = %ld,%d\n",fCurrent,fRunFileOffset,fRunBlockEnd,fRun.allocation_group,fRun.start);
		} else {
			// access to indirect blocks

			int32 runsPerBlock = fVolume.BlockSize() / sizeof(block_run);
			off_t runBlockEnd = data.MaxDirectRange();

			CachedBlock cached(fVolume);
			off_t block = fVolume.ToBlock(data.indirect);

			for (int32 i = 0; i < data.indirect.Length(); i++) {
				block_run* indirect = (block_run *)cached.SetTo(block + i);
				if (indirect == NULL)
					return B_IO_ERROR;

				int32 current = -1;
				while (++current < runsPerBlock) {
					if (indirect[current].IsZero())
						break;

					runBlockEnd
						+= (uint32)indirect[current].Length() << cached.BlockShift();
					if (runBlockEnd > pos) {
						run = indirect[current];
						offset = runBlockEnd
							- ((uint32)run.Length() << cached.BlockShift());
						//printf("reading from indirect block: %ld,%d\n",fRun.allocation_group,fRun.start);
						//printf("### indirect-run[%ld] = (%ld,%d,%d), offset = %lld\n",fCurrent,fRun.allocation_group,fRun.start,fRun.length,fRunFileOffset);
						return fVolume.ValidateBlockRun(run);
					}
				}
			}
			return B_ERROR;
		}
	} else {
		// access from direct blocks

		off_t runBlockEnd = 0LL;
		int32 current = -1;

		while (++current < NUM_DIRECT_BLOCKS) {
			if (data.direct[current].IsZero())
				break;

			runBlockEnd += (uint32)data.direct[current].Length() << fVolume.BlockShift();
			if (runBlockEnd > pos) {
				run = data.direct[current];
				offset = runBlockEnd - ((uint32)run.Length() << fVolume.BlockShift());
				//printf("### run[%ld] = (%ld,%d,%d), offset = %lld\n",fCurrent,fRun.allocation_group,fRun.start,fRun.length,fRunFileOffset);
				return fVolume.ValidateBlockRun(run);
			}
		}
		//PRINT(("FindBlockRun() failed in direct range: size = %lld, pos = %lld\n",data.size,pos));
		return B_ENTRY_NOT_FOUND;
	}
	return fVolume.ValidateBlockRun(run);
}


status_t
Stream::ReadAt(off_t pos, uint8* buffer, size_t* _length)
{
	// set/check boundaries for pos/length

	if (pos < 0)
		return B_BAD_VALUE;
	if (pos >= data.Size()) {
		*_length = 0;
		return B_NO_ERROR;
	}

	size_t length = *_length;

	if (pos + (off_t)length > data.Size())
		length = data.Size() - pos;

	block_run run;
	off_t offset;
	if (FindBlockRun(pos, run, offset) < B_OK) {
		*_length = 0;
		return B_BAD_VALUE;
	}

	uint32 bytesRead = 0;
	uint32 blockSize = fVolume.BlockSize();
	uint32 blockShift = fVolume.BlockShift();
	uint8* block;

	// the first block_run we read could not be aligned to the block_size boundary
	// (read partial block at the beginning)

	// pos % block_size == (pos - offset) % block_size, offset % block_size == 0
	if (pos % blockSize != 0) {
		run.start = HOST_ENDIAN_TO_BFS_INT16(run.Start()
			+ ((pos - offset) >> blockShift));
		run.length = HOST_ENDIAN_TO_BFS_INT16(run.Length()
			- ((pos - offset) >> blockShift));

		CachedBlock cached(fVolume, run);
		if ((block = cached.Block()) == NULL) {
			*_length = 0;
			return B_BAD_VALUE;
		}

		bytesRead = blockSize - (pos % blockSize);
		if (length < bytesRead)
			bytesRead = length;

		memcpy(buffer, block + (pos % blockSize), bytesRead);
		pos += bytesRead;

		length -= bytesRead;
		if (length == 0) {
			*_length = bytesRead;
			return B_OK;
		}

		if (FindBlockRun(pos, run, offset) < B_OK) {
			*_length = bytesRead;
			return B_BAD_VALUE;
		}
	}

	// the first block_run is already filled in at this point
	// read the following complete blocks using cached_read(),
	// the last partial block is read using the generic Cache class

	bool partial = false;

	while (length > 0) {
		// offset is the offset to the current pos in the block_run
		run.start = HOST_ENDIAN_TO_BFS_INT16(run.Start()
			+ ((pos - offset) >> blockShift));
		run.length = HOST_ENDIAN_TO_BFS_INT16(run.Length()
			- ((pos - offset) >> blockShift));

		if (uint32(run.Length() << blockShift) > length) {
			if (length < blockSize) {
				CachedBlock cached(fVolume, run);
				if ((block = cached.Block()) == NULL) {
					*_length = bytesRead;
					return B_BAD_VALUE;
				}
				memcpy(buffer + bytesRead, block, length);
				bytesRead += length;
				break;
			}
			run.length = HOST_ENDIAN_TO_BFS_INT16(length >> blockShift);
			partial = true;
		}

		if (read_pos(fVolume.Device(), fVolume.ToOffset(run), buffer + bytesRead,
				run.Length() << fVolume.BlockShift()) < B_OK) {
			*_length = bytesRead;
			return B_BAD_VALUE;
		}

		int32 bytes = run.Length() << blockShift;
		length -= bytes;
		bytesRead += bytes;
		if (length == 0)
			break;

		pos += bytes;

		if (partial) {
			// if the last block was read only partially, point block_run
			// to the remaining part
			run.start = HOST_ENDIAN_TO_BFS_INT16(run.Start() + run.Length());
			run.length = 1;
			offset = pos;
		} else if (FindBlockRun(pos, run, offset) < B_OK) {
			*_length = bytesRead;
			return B_BAD_VALUE;
		}
	}

	*_length = bytesRead;
	return B_NO_ERROR;
}


/*!	Renga (boot loader batch A; checks from batch 6): writes \a buffer
	over the file's data at \a pos, into the blocks the file already has.
	The boot loader cannot allocate blocks or change the inode, so no
	metadata changes; this is enough for the boot state file
	(headers/private/shared/boot_state.h), which its writers keep at a
	fixed size. File data is not journalled by BFS, so a later log replay by
	the kernel cannot bring back old data.

	The maintainer's machines dual-boot other systems from the same disk,
	so a write must never land anywhere but this file's own blocks. Before
	anything is written, the whole write is planned and checked:

	- the inode: valid (magic, in use, not deleted, the size the volume
	  uses), at the block it says it is at, a plain regular file (no
	  directory, symlink, attribute or index), all its data in direct runs;
	- the range: inside the file's size; a write that would not fit is
	  refused, not cut short;
	- every block run the write touches: Volume::CheckWritableRun() (inside
	  an allocation group, the volume and the partition, past the bitmap and
	  the log, not the inode itself, marked used in the bitmap).

	If any check fails nothing is written and B_NOT_ALLOWED or B_BAD_DATA
	is returned. After writing, every piece is read back and compared. The
	pieces go to the partition (Partition::WriteAt() refuses writes past
	its end), which the platform's device writes as whole sectors.
*/
status_t
Stream::WriteInPlace(off_t pos, const uint8* buffer, size_t* _length)
{
	size_t length = *_length;
	*_length = 0;

	if (InitCheck() != B_OK || fBlock != fVolume.ToBlock(inode_num)
		|| !S_ISREG(Mode()) || S_ISLNK(Mode()) || IsContainer()
		|| (Flags() & (INODE_ATTR_INODE | INODE_DELETED | INODE_NOT_READY))
			!= 0) {
		dprintf("bfs: write refused: inode %" B_PRIdOFF " is not a plain, "
			"valid file\n", fBlock);
		return B_NOT_ALLOWED;
	}
	if (data.Size() <= 0 || data.Size() > data.MaxDirectRange()
		|| data.MaxIndirectRange() > data.MaxDirectRange()
		|| data.MaxDoubleIndirectRange() > 0) {
		dprintf("bfs: write refused: file data is not all in direct runs\n");
		return B_NOT_ALLOWED;
	}
	if (pos < 0 || length == 0 || pos + (off_t)length > data.Size()) {
		dprintf("bfs: write refused: %" B_PRIuSIZE " bytes at %" B_PRIdOFF
			" do not fit the file's %" B_PRIdOFF " bytes\n", length, pos,
			data.Size());
		return B_BAD_VALUE;
	}

	// plan and check every piece first
	static const int32 kMaxPieces = NUM_DIRECT_BLOCKS;
	off_t diskOffsets[kMaxPieces];
	size_t sizes[kMaxPieces];
	int32 pieces = 0;
	size_t planned = 0;
	while (planned < length) {
		block_run run;
		off_t runOffset;
		off_t position = pos + planned;
		if (pieces == kMaxPieces
			|| FindBlockRun(position, run, runOffset) < B_OK)
			return B_BAD_DATA;
		status_t status = fVolume.CheckWritableRun(run, fBlock);
		if (status != B_OK)
			return status;

		off_t runEnd = runOffset
			+ ((off_t)run.Length() << fVolume.BlockShift());
		size_t chunk = length - planned;
		if (position + (off_t)chunk > runEnd)
			chunk = runEnd - position;
		if (chunk == 0)
			return B_BAD_DATA;

		diskOffsets[pieces] = fVolume.ToOffset(run) + (position - runOffset);
		sizes[pieces] = chunk;
		pieces++;
		planned += chunk;
	}

	size_t written = 0;
	for (int32 i = 0; i < pieces; i++) {
		ssize_t result = write_pos(fVolume.Device(), diskOffsets[i],
			buffer + written, sizes[i]);
		if (result < 0 || (size_t)result != sizes[i]) {
			*_length = written;
			return result < 0 ? (status_t)result : B_IO_ERROR;
		}
		written += sizes[i];
	}

	// read back and compare
	uint8* check = (uint8*)malloc(fVolume.BlockSize());
	if (check == NULL)
		return B_NO_MEMORY;
	size_t verified = 0;
	status_t status = B_OK;
	for (int32 i = 0; i < pieces && status == B_OK; i++) {
		for (size_t done = 0; done < sizes[i]; ) {
			size_t part = sizes[i] - done;
			if (part > fVolume.BlockSize())
				part = fVolume.BlockSize();
			if (read_pos(fVolume.Device(), diskOffsets[i] + done, check, part)
					!= (ssize_t)part
				|| memcmp(check, buffer + verified + done, part) != 0) {
				dprintf("bfs: write did not read back the same\n");
				status = B_IO_ERROR;
				break;
			}
			done += part;
		}
		verified += sizes[i];
	}
	free(check);

	*_length = written;
	return status;
}


Node*
Stream::NodeFactory(Volume& volume, off_t id)
{
	Stream stream(volume, id);
	if (stream.InitCheck() != B_OK)
		return NULL;

	if (stream.IsContainer())
		return new(nothrow) Directory(stream);

	if (stream.IsSymlink())
		return new(nothrow) Link(stream);

	return new(nothrow) File(stream);
}


//	#pragma mark -


status_t
bfs_inode::InitCheck(Volume* volume) const
{
	if ((Flags() & INODE_NOT_READY) != 0) {
		// the other fields may not yet contain valid values
		return B_BUSY;
	}

	if (Magic1() != INODE_MAGIC1
		|| !(Flags() & INODE_IN_USE)
		|| inode_num.Length() != 1
		// matches inode size?
		|| (uint32)InodeSize() != volume->InodeSize()
		// parent resides on disk?
		|| parent.AllocationGroup() > int32(volume->AllocationGroups())
		|| parent.AllocationGroup() < 0
		|| parent.Start() > (1L << volume->AllocationGroupShift())
		|| parent.Length() != 1
		// attributes, too?
		|| attributes.AllocationGroup() > int32(volume->AllocationGroups())
		|| attributes.AllocationGroup() < 0
		|| attributes.Start() > (1L << volume->AllocationGroupShift()))
		return B_BAD_DATA;

	// TODO: Add some tests to check the integrity of the other stuff here,
	// especially for the data_stream!

	return B_OK;
}
