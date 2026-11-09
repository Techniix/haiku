/*
 * Copyright 2013, Jérôme Duval, korli@users.berlios.de.
 * Distributed under the terms of the MIT License.
 */


#include <algorithm>

#include <condition_variable.h>
#include <kernel.h>
#include <lock.h>
#include <StackOrHeapArray.h>
#include <util/AutoLock.h>
#include <virtio.h>

#include "virtio_blk.h"


class DMAResource;
class IOScheduler;


static const uint8 kDriveIcon[] = {
	0x6e, 0x63, 0x69, 0x66, 0x08, 0x03, 0x01, 0x00, 0x00, 0x02, 0x00, 0x16,
	0x02, 0x3c, 0xc7, 0xee, 0x38, 0x9b, 0xc0, 0xba, 0x16, 0x57, 0x3e, 0x39,
	0xb0, 0x49, 0x77, 0xc8, 0x42, 0xad, 0xc7, 0x00, 0xff, 0xff, 0xd3, 0x02,
	0x00, 0x06, 0x02, 0x3c, 0x96, 0x32, 0x3a, 0x4d, 0x3f, 0xba, 0xfc, 0x01,
	0x3d, 0x5a, 0x97, 0x4b, 0x57, 0xa5, 0x49, 0x84, 0x4d, 0x00, 0x47, 0x47,
	0x47, 0xff, 0xa5, 0xa0, 0xa0, 0x02, 0x00, 0x16, 0x02, 0xbc, 0x59, 0x2f,
	0xbb, 0x29, 0xa7, 0x3c, 0x0c, 0xe4, 0xbd, 0x0b, 0x7c, 0x48, 0x92, 0xc0,
	0x4b, 0x79, 0x66, 0x00, 0x7d, 0xff, 0xd4, 0x02, 0x00, 0x06, 0x02, 0x38,
	0xdb, 0xb4, 0x39, 0x97, 0x33, 0xbc, 0x4a, 0x33, 0x3b, 0xa5, 0x42, 0x48,
	0x6e, 0x66, 0x49, 0xee, 0x7b, 0x00, 0x59, 0x67, 0x56, 0xff, 0xeb, 0xb2,
	0xb2, 0x03, 0xa7, 0xff, 0x00, 0x03, 0xff, 0x00, 0x00, 0x04, 0x01, 0x80,
	0x07, 0x0a, 0x06, 0x22, 0x3c, 0x22, 0x49, 0x44, 0x5b, 0x5a, 0x3e, 0x5a,
	0x31, 0x39, 0x25, 0x0a, 0x04, 0x22, 0x3c, 0x44, 0x4b, 0x5a, 0x31, 0x39,
	0x25, 0x0a, 0x04, 0x44, 0x4b, 0x44, 0x5b, 0x5a, 0x3e, 0x5a, 0x31, 0x0a,
	0x04, 0x22, 0x3c, 0x22, 0x49, 0x44, 0x5b, 0x44, 0x4b, 0x08, 0x02, 0x27,
	0x43, 0xb8, 0x14, 0xc1, 0xf1, 0x08, 0x02, 0x26, 0x43, 0x29, 0x44, 0x0a,
	0x05, 0x44, 0x5d, 0x49, 0x5d, 0x60, 0x3e, 0x5a, 0x3b, 0x5b, 0x3f, 0x08,
	0x0a, 0x07, 0x01, 0x06, 0x00, 0x0a, 0x00, 0x01, 0x00, 0x10, 0x01, 0x17,
	0x84, 0x00, 0x04, 0x0a, 0x01, 0x01, 0x01, 0x00, 0x0a, 0x02, 0x01, 0x02,
	0x00, 0x0a, 0x03, 0x01, 0x03, 0x00, 0x0a, 0x04, 0x01, 0x04, 0x10, 0x01,
	0x17, 0x85, 0x20, 0x04, 0x0a, 0x06, 0x01, 0x05, 0x30, 0x24, 0xb3, 0x99,
	0x01, 0x17, 0x82, 0x00, 0x04, 0x0a, 0x05, 0x01, 0x05, 0x30, 0x20, 0xb2,
	0xe6, 0x01, 0x17, 0x82, 0x00, 0x04
};


#define VIRTIO_BLOCK_DRIVER_MODULE_NAME "drivers/disk/virtual/virtio_block/driver_v1"
#define VIRTIO_BLOCK_DEVICE_MODULE_NAME "drivers/disk/virtual/virtio_block/device_v1"
#define VIRTIO_BLOCK_DEVICE_ID_GENERATOR	"virtio_block/device_id"


typedef struct {
	device_node*			node;
	::virtio_device			virtio_device;
	virtio_device_interface*	virtio;
	::virtio_queue			virtio_queue;
	IOScheduler*			io_scheduler;
	DMAResource*			dma_resource;

	struct virtio_blk_config	config;

	area_id					bufferArea;
	addr_t					bufferAddr;
	phys_addr_t				bufferPhysAddr;

	uint64 					features;
	uint64					capacity;
	uint32					block_size;
	uint32					physical_block_size;
	status_t				media_status;

	mutex					lock;
	int32					currentRequest;
	ConditionVariable		interruptCondition;
} virtio_block_driver_info;


typedef struct {
	virtio_block_driver_info*		info;
} virtio_block_handle;


#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include <fs/devfs.h>

#include "dma_resources.h"
#include "IORequest.h"
#include "IOSchedulerSimple.h"


//#define TRACE_VIRTIO_BLOCK
#ifdef TRACE_VIRTIO_BLOCK
#	define TRACE(x...) dprintf("virtio_block: " x)
#else
#	define TRACE(x...) ;
#endif
#define ERROR(x...)			dprintf("\33[33mvirtio_block:\33[0m " x)
#define CALLED() 			TRACE("CALLED %s\n", __PRETTY_FUNCTION__)


static device_manager_info* sDeviceManager;


bool virtio_block_set_capacity(virtio_block_driver_info* info);


const char *
get_feature_name(uint64 feature)
{
	switch (feature) {
		case VIRTIO_BLK_F_BARRIER:
			return "host barrier";
		case VIRTIO_BLK_F_SIZE_MAX:
			return "maximum segment size";
		case VIRTIO_BLK_F_SEG_MAX:
			return "maximum segment count";
		case VIRTIO_BLK_F_GEOMETRY:
			return "disk geometry";
		case VIRTIO_BLK_F_RO:
			return "read only";
		case VIRTIO_BLK_F_BLK_SIZE:
			return "block size";
		case VIRTIO_BLK_F_SCSI:
			return "scsi commands";
		case VIRTIO_BLK_F_FLUSH:
			return "flush command";
		case VIRTIO_BLK_F_TOPOLOGY:
			return "topology";
		case VIRTIO_BLK_F_CONFIG_WCE:
			return "config wce";
		case VIRTIO_BLK_F_DISCARD:
			return "discard";
		case VIRTIO_BLK_F_WRITE_ZEROES:
			return "write zeroes";
	}
	return NULL;
}


/*!	Reads the device configuration. The fields for discard and write zeroes
	come after the original ones and are only read when one of those features
	was negotiated: a device that offers neither may not implement them, and
	reading past its configuration space yields garbage.
*/
static status_t
read_config(virtio_block_driver_info* info)
{
	status_t status = info->virtio->read_device_config(info->virtio_device, 0,
		&info->config, VIRTIO_BLK_CONFIG_BASE_SIZE);
	if (status != B_OK)
		return status;

	if ((info->features
			& (VIRTIO_BLK_F_DISCARD | VIRTIO_BLK_F_WRITE_ZEROES)) == 0) {
		return B_OK;
	}

	// Start at the aligned offset before the extension (re-reading the
	// writeback byte): read_device_config() reads in 32-bit units, and QEMU
	// rejects unaligned accesses to the legacy I/O port configuration, which
	// then read as zero.
	const uint8 start = VIRTIO_BLK_CONFIG_BASE_SIZE & ~3;
	return info->virtio->read_device_config(info->virtio_device, start,
		(uint8*)&info->config + start,
		sizeof(struct virtio_blk_config) - start);
}


/*!	Queues one request and waits for it. \a entries holds \a readable
	device-readable entries (the header first) followed by \a writable
	device-writable ones (the status byte last). The header and status byte
	live in the command buffer, so the caller must hold info->lock, which
	also keeps requests one at a time (currentRequest identifies the one the
	interrupt handler wakes us for).
*/
static status_t
execute_request(virtio_block_driver_info* info, physical_entry* entries,
	size_t readable, size_t writable, bigtime_t timeout)
{
	ASSERT_LOCKED_MUTEX(&info->lock);

	uint8* ack = (uint8*)info->bufferAddr + sizeof(struct virtio_blk_outhdr);
	*ack = 0xff;

	atomic_add(&info->currentRequest, 1);

	ConditionVariableEntry entry;
	info->interruptCondition.Add(&entry);

	status_t result = info->virtio->queue_request_v(info->virtio_queue, entries,
		readable, writable, (void *)(addr_t)info->currentRequest);
	if (result == B_OK)
		result = entry.Wait(B_RELATIVE_TIMEOUT, timeout);
	if (result != B_OK)
		return EIO;

	switch (*ack) {
		case VIRTIO_BLK_S_OK:
			return B_OK;
		case VIRTIO_BLK_S_UNSUPP:
			return ENOTSUP;
		default:
			return EIO;
	}
}


/*!	Fills in the request header at the start of the command buffer and
	returns the physical entries for the header and the status byte.
*/
static void
prepare_header(virtio_block_driver_info* info, uint32 type, uint64 sector,
	physical_entry& header, physical_entry& status)
{
	struct virtio_blk_outhdr* outHeader
		= (struct virtio_blk_outhdr*)info->bufferAddr;
	outHeader->type = type;
	outHeader->sector = sector;
	outHeader->ioprio = 1;

	header.address = info->bufferPhysAddr;
	header.size = sizeof(struct virtio_blk_outhdr);
	status.address = info->bufferPhysAddr + sizeof(struct virtio_blk_outhdr);
	status.size = sizeof(uint8);
}


static status_t
get_geometry(virtio_block_handle* handle, device_geometry* geometry)
{
	virtio_block_driver_info* info = handle->info;

	devfs_compute_geometry_size(geometry, info->capacity, info->block_size);
	geometry->bytes_per_physical_sector = info->physical_block_size;

	geometry->device_type = B_DISK;
	geometry->removable = false;

	geometry->read_only = ((info->features & VIRTIO_BLK_F_RO) != 0);
	geometry->write_once = false;

	TRACE("virtio_block: get_geometry(): %" B_PRIu32 ", %" B_PRIu32 ", %" B_PRIu32 ", %" B_PRIu32
		", %d, %d, %d, %d\n", geometry->bytes_per_sector, geometry->sectors_per_track,
		geometry->cylinder_count, geometry->head_count, geometry->device_type,
		geometry->removable, geometry->read_only, geometry->write_once);

	return B_OK;
}


static void
virtio_block_config_callback(void* driverCookie)
{
	virtio_block_driver_info* info = (virtio_block_driver_info*)driverCookie;

	if (read_config(info) != B_OK)
		return;

	if (virtio_block_set_capacity(info))
		info->media_status = B_DEV_MEDIA_CHANGED;
}


static void
virtio_block_callback(void* driverCookie, void* _cookie)
{
	virtio_block_driver_info* info = (virtio_block_driver_info*)_cookie;

	void* cookie = NULL;
	while (info->virtio->queue_dequeue(info->virtio_queue, &cookie, NULL)) {
		if ((int32)(addr_t)cookie == atomic_get(&info->currentRequest))
			info->interruptCondition.NotifyAll();
	}
}


static status_t
do_io(void* cookie, IOOperation* operation)
{
	virtio_block_driver_info* info = (virtio_block_driver_info*)cookie;

	// The I/O scheduler ignores this function's return value and waits for
	// OperationCompleted(), so an operation must never be dropped. The lock
	// used to be taken with a try-lock, returning B_BUSY without completing
	// the operation if it was held; that was harmless only while the
	// scheduler thread was the lock's sole user. The flush and trim ioctls
	// now use the command buffer too, so wait for them instead.
	MutexLocker locker(&info->lock);

	BStackOrHeapArray<physical_entry, 16> entries(operation->VecCount() + 2);

	prepare_header(info,
		operation->IsWrite() ? VIRTIO_BLK_T_OUT : VIRTIO_BLK_T_IN,
		operation->Offset() / 512, entries[0],
		entries[operation->VecCount() + 1]);

	memcpy(entries + 1, operation->Vecs(), operation->VecCount()
		* sizeof(physical_entry));

	status_t status = execute_request(info, entries,
		1 + (operation->IsWrite() ? operation->VecCount() : 0),
		1 + (operation->IsWrite() ? 0 : operation->VecCount()),
		10 * 1000 * 1000);

	info->io_scheduler->OperationCompleted(operation, status,
		status == B_OK ? operation->Length() : 0);
	return status;
}


/*!	B_FLUSH_DRIVE_CACHE: asks the host to make all completed writes
	durable (VIRTIO_BLK_T_FLUSH). Without VIRTIO_BLK_F_FLUSH the device has
	no volatile cache to flush (writes complete only once durable), so there
	is nothing to do.
*/
static status_t
flush_cache(virtio_block_driver_info* info)
{
	if ((info->features & VIRTIO_BLK_F_FLUSH) == 0)
		return B_OK;

	MutexLocker locker(&info->lock);

	physical_entry entries[2];
	prepare_header(info, VIRTIO_BLK_T_FLUSH, 0, entries[0], entries[1]);

	// A flush can take a while on a host with a lot of dirty data.
	return execute_request(info, entries, 1, 1, 60 * 1000 * 1000);
}


/*!	B_TRIM_DEVICE: tells the host that the given byte ranges are unused,
	so a sparse or thin-provisioned image can give the space back.

	Uses DISCARD when the device offers it, otherwise WRITE ZEROES with the
	unmap flag if the device says it may unmap (the data reads as zeroes
	afterwards, which is fine for space BFS has freed). Ranges are shrunk
	inwards to whole, aligned sectors - a partial block is never discarded -
	split to the device's per-segment limit, and sent in batches of as many
	segments as the device and the command buffer allow.
*/
static status_t
trim_device(virtio_block_driver_info* info, fs_trim_data* trimData)
{
	trimData->trimmed_size = 0;

	uint32 type;
	uint32 maxSectors;
	uint32 maxSegments;
	uint32 alignment = 1;
	uint32 flags = 0;
	if ((info->features & VIRTIO_BLK_F_DISCARD) != 0) {
		type = VIRTIO_BLK_T_DISCARD;
		maxSectors = info->config.max_discard_sectors;
		maxSegments = info->config.max_discard_seg;
		if (info->config.discard_sector_alignment > 1)
			alignment = info->config.discard_sector_alignment;
	} else if ((info->features & VIRTIO_BLK_F_WRITE_ZEROES) != 0
		&& info->config.write_zeroes_may_unmap != 0) {
		type = VIRTIO_BLK_T_WRITE_ZEROES;
		maxSectors = info->config.max_write_zeroes_sectors;
		maxSegments = info->config.max_write_zeroes_seg;
		flags = VIRTIO_BLK_WRITE_ZEROES_FLAG_UNMAP;
	} else
		return B_UNSUPPORTED;

	if (maxSectors == 0 || maxSegments == 0)
		return B_UNSUPPORTED;

	// Never discard less than a logical block.
	uint32 blockSectors = info->block_size / 512;
	if (blockSectors > alignment)
		alignment = blockSectors;
	// Keep segments a multiple of the alignment.
	maxSectors -= maxSectors % alignment;
	if (maxSectors == 0)
		return B_UNSUPPORTED;

	// The segments follow the header and status byte in the command buffer.
	const size_t segmentOffset = 64;
	struct virtio_blk_discard_write_zeroes* segments
		= (struct virtio_blk_discard_write_zeroes*)(info->bufferAddr
			+ segmentOffset);
	uint32 bufferSegments = (B_PAGE_SIZE - segmentOffset)
		/ sizeof(struct virtio_blk_discard_write_zeroes);
	maxSegments = std::min(maxSegments, bufferSegments);

	const uint64 deviceSectors = info->capacity * (info->block_size / 512);

	MutexLocker locker(&info->lock);

	uint32 count = 0;
	uint64 batchBytes = 0;
	uint64 trimmed = 0;

	// Sends the segments collected so far.
	auto flushBatch = [&]() -> status_t {
		if (count == 0)
			return B_OK;

		physical_entry entries[3];
		prepare_header(info, type, 0, entries[0], entries[2]);
		entries[1].address = info->bufferPhysAddr + segmentOffset;
		entries[1].size = count * sizeof(struct virtio_blk_discard_write_zeroes);

		status_t status = execute_request(info, entries, 2, 1,
			60 * 1000 * 1000);
		if (status != B_OK)
			return status;

		trimmed += batchBytes;
		count = 0;
		batchBytes = 0;
		return B_OK;
	};

	for (uint32 i = 0; i < trimData->range_count; i++) {
		uint64 start = trimData->ranges[i].offset;
		uint64 end = start + trimData->ranges[i].size;
		if (end < start)
			return B_BAD_VALUE;

		// In sectors, rounded inwards to the alignment.
		uint64 first = (start + 511) / 512;
		first = (first + alignment - 1) / alignment * alignment;
		uint64 last = std::min(end / 512, deviceSectors);
		last -= last % alignment;

		while (first < last) {
			uint32 sectors = (uint32)std::min(last - first, (uint64)maxSectors);

			segments[count].sector = first;
			segments[count].num_sectors = sectors;
			segments[count].flags = flags;
			count++;
			batchBytes += (uint64)sectors * 512;
			first += sectors;

			if (count == maxSegments) {
				status_t status = flushBatch();
				if (status != B_OK) {
					trimData->trimmed_size = trimmed;
					return status;
				}
			}
		}
	}

	status_t status = flushBatch();
	trimData->trimmed_size = trimmed;
	return status;
}


//	#pragma mark - device module API


static status_t
virtio_block_init_device(void* _info, void** _cookie)
{
	CALLED();
	virtio_block_driver_info* info = (virtio_block_driver_info*)_info;

	device_node* parent = sDeviceManager->get_parent_node(info->node);
	sDeviceManager->get_driver(parent, (driver_module_info **)&info->virtio,
		(void **)&info->virtio_device);
	sDeviceManager->put_node(parent);

	info->virtio->negotiate_features(info->virtio_device,
		VIRTIO_BLK_F_BARRIER | VIRTIO_BLK_F_SIZE_MAX
			| VIRTIO_BLK_F_SEG_MAX | VIRTIO_BLK_F_GEOMETRY
			| VIRTIO_BLK_F_RO | VIRTIO_BLK_F_BLK_SIZE
			| VIRTIO_BLK_F_FLUSH | VIRTIO_BLK_F_TOPOLOGY
			| VIRTIO_BLK_F_DISCARD | VIRTIO_BLK_F_WRITE_ZEROES
			| VIRTIO_FEATURE_RING_INDIRECT_DESC,
		&info->features, &get_feature_name);

	status_t status = read_config(info);
	if (status != B_OK)
		return status;

	virtio_block_set_capacity(info);

	TRACE("virtio_block: capacity: %" B_PRIu64 ", block_size %" B_PRIu32 "\n",
		info->capacity, info->block_size);

	uint16 requestedSize = 0;
	if ((info->features & VIRTIO_BLK_F_SEG_MAX) != 0)
		requestedSize = info->config.seg_max + 2;
			// two entries are taken up by the header and result

	status = info->virtio->alloc_queues(info->virtio_device, 1,
		&info->virtio_queue, &requestedSize);
	if (status != B_OK) {
		ERROR("queue allocation failed (%s)\n", strerror(status));
		return status;
	}

	status = info->virtio->setup_interrupt(info->virtio_device,
		virtio_block_config_callback, info);

	if (status == B_OK) {
		status = info->virtio->queue_setup_interrupt(info->virtio_queue,
			virtio_block_callback, info);
	}

	*_cookie = info;
	return status;
}


static void
virtio_block_uninit_device(void* _cookie)
{
	CALLED();
	virtio_block_driver_info* info = (virtio_block_driver_info*)_cookie;

	delete info->io_scheduler;
	info->io_scheduler = NULL;
	delete info->dma_resource;
	info->dma_resource = NULL;
}


static status_t
virtio_block_open(void* _info, const char* path, int openMode, void** _cookie)
{
	CALLED();
	virtio_block_driver_info* info = (virtio_block_driver_info*)_info;

	virtio_block_handle* handle = (virtio_block_handle*)malloc(
		sizeof(virtio_block_handle));
	if (handle == NULL)
		return B_NO_MEMORY;

	handle->info = info;

	*_cookie = handle;
	return B_OK;
}


static status_t
virtio_block_close(void* cookie)
{
	//virtio_block_handle* handle = (virtio_block_handle*)cookie;
	CALLED();

	return B_OK;
}


static status_t
virtio_block_free(void* cookie)
{
	CALLED();
	virtio_block_handle* handle = (virtio_block_handle*)cookie;

	free(handle);
	return B_OK;
}


static status_t
virtio_block_io(void *cookie, io_request *request)
{
	CALLED();
	virtio_block_handle* handle = (virtio_block_handle*)cookie;

	return handle->info->io_scheduler->ScheduleRequest(request);
}


static status_t
virtio_block_ioctl(void* cookie, uint32 op, void* buffer, size_t length)
{
	CALLED();
	virtio_block_handle* handle = (virtio_block_handle*)cookie;
	virtio_block_driver_info* info = handle->info;

	TRACE("ioctl(op = %" B_PRIu32 ")\n", op);

	switch (op) {
		case B_GET_MEDIA_STATUS:
		{
			user_memcpy(buffer, &info->media_status, sizeof(info->media_status));
			TRACE("B_GET_MEDIA_STATUS: 0x%08" B_PRIx32 "\n", info->media_status);
			info->media_status = B_OK;
			return B_OK;
		}

		case B_GET_DEVICE_SIZE:
		{
			size_t size = info->capacity * info->block_size;
			return user_memcpy(buffer, &size, sizeof(size_t));
		}

		case B_GET_GEOMETRY:
		{
			if (buffer == NULL || length > sizeof(device_geometry))
				return B_BAD_VALUE;

		 	device_geometry geometry;
			status_t status = get_geometry(handle, &geometry);
			if (status != B_OK)
				return status;

			return user_memcpy(buffer, &geometry, length);
		}

		case B_GET_ICON_NAME:
			return user_strlcpy((char*)buffer, "devices/drive-harddisk",
				B_FILE_NAME_LENGTH);

		case B_GET_VECTOR_ICON:
		{
			// TODO: take device type into account!
			device_icon iconData;
			if (length != sizeof(device_icon))
				return B_BAD_VALUE;
			if (user_memcpy(&iconData, buffer, sizeof(device_icon)) != B_OK)
				return B_BAD_ADDRESS;

			if (iconData.icon_size >= (int32)sizeof(kDriveIcon)) {
				if (user_memcpy(iconData.icon_data, kDriveIcon,
						sizeof(kDriveIcon)) != B_OK)
					return B_BAD_ADDRESS;
			}

			iconData.icon_size = sizeof(kDriveIcon);
			return user_memcpy(buffer, &iconData, sizeof(device_icon));
		}

		case B_FLUSH_DRIVE_CACHE:
			return flush_cache(info);

		case B_TRIM_DEVICE:
			// devfs has copied the data into the kernel (fs_trim_support.h)
			ASSERT(IS_KERNEL_ADDRESS(buffer));
			return trim_device(info, (fs_trim_data*)buffer);
	}

	return B_DEV_INVALID_IOCTL;
}


bool
virtio_block_set_capacity(virtio_block_driver_info* info)
{
	// get capacity
	uint32 blockSize = 512;
	if ((info->features & VIRTIO_BLK_F_BLK_SIZE) != 0)
		blockSize = info->config.blk_size;
	uint64 capacity = info->config.capacity * 512 / blockSize;
	uint32 physicalBlockSize = blockSize;

	if ((info->features & VIRTIO_BLK_F_TOPOLOGY) != 0
		&& info->config.topology.physical_block_exp > 0) {
		physicalBlockSize = blockSize * (1 << info->config.topology.physical_block_exp);
	}

	TRACE("set_capacity(device = %p, capacity = %" B_PRIu64 ", blockSize = %" B_PRIu32 ")\n",
		info, capacity, blockSize);

	if (info->block_size == blockSize && info->capacity == capacity)
		return false;

	info->capacity = capacity;

	if (info->block_size != 0) {
		ERROR("old %" B_PRId32 ", new %" B_PRId32 "\n", info->block_size,
			blockSize);
		panic("updating DMAResource not yet implemented...");
	}

	dma_restrictions restrictions;
	memset(&restrictions, 0, sizeof(restrictions));
	if ((info->features & VIRTIO_BLK_F_SIZE_MAX) != 0)
		restrictions.max_segment_size = info->config.size_max;
	if ((info->features & VIRTIO_BLK_F_SEG_MAX) != 0)
		restrictions.max_segment_count = info->config.seg_max;

	// TODO: we need to replace the DMAResource in our IOScheduler
	status_t status = info->dma_resource->Init(restrictions, blockSize,
		1024, 32);
	if (status != B_OK)
		panic("initializing DMAResource failed: %s", strerror(status));

	info->io_scheduler = new(std::nothrow) IOSchedulerSimple(
		info->dma_resource);
	if (info->io_scheduler == NULL)
		panic("allocating IOScheduler failed.");

	// TODO: use whole device name here
	status = info->io_scheduler->Init("virtio");
	if (status != B_OK)
		panic("initializing IOScheduler failed: %s", strerror(status));

	info->io_scheduler->SetCallback(do_io, info);

	info->block_size = blockSize;
	info->physical_block_size = physicalBlockSize;
	return true;
}


//	#pragma mark - driver module API


static float
virtio_block_supports_device(device_node *parent)
{
	CALLED();
	const char *bus;
	uint16 deviceType;

	// make sure parent is really the Virtio bus manager
	if (sDeviceManager->get_attr_string(parent, B_DEVICE_BUS, &bus, false))
		return -1;

	if (strcmp(bus, "virtio"))
		return 0.0;

	// check whether it's really a Direct Access Device
	if (sDeviceManager->get_attr_uint16(parent, VIRTIO_DEVICE_TYPE_ITEM,
			&deviceType, true) != B_OK || deviceType != VIRTIO_DEVICE_ID_BLOCK)
		return 0.0;

	TRACE("Virtio block device found!\n");

	return 0.6;
}


static status_t
virtio_block_register_device(device_node *node)
{
	CALLED();

	device_attr attrs[] = {
		{ B_DEVICE_PRETTY_NAME, B_STRING_TYPE, {.string = "Virtio Block"} },
		{ NULL }
	};

	return sDeviceManager->register_node(node, VIRTIO_BLOCK_DRIVER_MODULE_NAME,
		attrs, NULL, NULL);
}


static status_t
virtio_block_init_driver(device_node *node, void **cookie)
{
	CALLED();

	virtio_block_driver_info* info = (virtio_block_driver_info*)malloc(
		sizeof(virtio_block_driver_info));
	if (info == NULL)
		return B_NO_MEMORY;

	memset((void*)info, 0, sizeof(*info));

	info->media_status = B_OK;
	info->dma_resource = new(std::nothrow) DMAResource;
	if (info->dma_resource == NULL) {
		free(info);
		return B_NO_MEMORY;
	}

	// create command buffer area
	info->bufferArea = create_area("virtio_block command buffer", (void**)&info->bufferAddr,
		B_ANY_KERNEL_BLOCK_ADDRESS, B_PAGE_SIZE,
		B_FULL_LOCK, B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA);
	if (info->bufferArea < B_OK) {
		status_t status = info->bufferArea;
		delete info->dma_resource;
		free(info);
		return status;
	}

	physical_entry entry;
	status_t status = get_memory_map((void*)info->bufferAddr, B_PAGE_SIZE, &entry, 1);
	if (status != B_OK) {
		delete_area(info->bufferArea);
		delete info->dma_resource;
		free(info);
		return status;
	}

	info->bufferPhysAddr = entry.address;
	info->interruptCondition.Init(info, "virtio block transfer");
	info->currentRequest = 0;
	mutex_init(&info->lock, "virtio block request");

	info->node = node;

	*cookie = info;
	return B_OK;
}


static void
virtio_block_uninit_driver(void *_cookie)
{
	CALLED();
	virtio_block_driver_info* info = (virtio_block_driver_info*)_cookie;
	// Device manager skips uninit_device unless init_device set
	// *_cookie (after config read / alloc_queues). Same if
	// create_id or publish_device fails after init_driver.
	virtio_block_uninit_device(info);
	mutex_destroy(&info->lock);
	delete_area(info->bufferArea);
	free(info);
}


static status_t
virtio_block_register_child_devices(void* _cookie)
{
	CALLED();
	virtio_block_driver_info* info = (virtio_block_driver_info*)_cookie;
	status_t status;

	int32 id = sDeviceManager->create_id(VIRTIO_BLOCK_DEVICE_ID_GENERATOR);
	if (id < 0)
		return id;

	char name[64];
	snprintf(name, sizeof(name), "disk/virtual/virtio_block/%" B_PRId32 "/raw",
		id);

	status = sDeviceManager->publish_device(info->node, name,
		VIRTIO_BLOCK_DEVICE_MODULE_NAME);
	if (status != B_OK)
		sDeviceManager->free_id(VIRTIO_BLOCK_DEVICE_ID_GENERATOR, id);
	return status;
}


//	#pragma mark -


module_dependency module_dependencies[] = {
	{ B_DEVICE_MANAGER_MODULE_NAME, (module_info**)&sDeviceManager },
	{ NULL }
};

struct device_module_info sVirtioBlockDevice = {
	{
		VIRTIO_BLOCK_DEVICE_MODULE_NAME,
		0,
		NULL
	},

	virtio_block_init_device,
	virtio_block_uninit_device,
	NULL, // remove,

	virtio_block_open,
	virtio_block_close,
	virtio_block_free,
	NULL,	// read
	NULL,	// write
	virtio_block_io,
	virtio_block_ioctl,

	NULL,	// select
	NULL,	// deselect
};

struct driver_module_info sVirtioBlockDriver = {
	{
		VIRTIO_BLOCK_DRIVER_MODULE_NAME,
		0,
		NULL
	},

	virtio_block_supports_device,
	virtio_block_register_device,
	virtio_block_init_driver,
	virtio_block_uninit_driver,
	virtio_block_register_child_devices,
	NULL,	// rescan
	NULL,	// removed
};

module_info* modules[] = {
	(module_info*)&sVirtioBlockDriver,
	(module_info*)&sVirtioBlockDevice,
	NULL
};
