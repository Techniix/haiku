/*
 * Copyright 2016-2020 Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */


#include <boot/partitions.h>
#include <boot/platform.h>
#include <boot/stage2.h>

#include <boot/net/Ethernet.h>
#include <boot/net/NetStack.h>
#include <boot/net/RemoteDisk.h>

#include "Header.h"

#include "efi_platform.h"
#include <efi/protocol/block-io.h>
#include <efi/protocol/device-path.h>
#include <efi/protocol/loaded-image.h>

#include "gpt.h"
#include "gpt_known_guids.h"


//#define TRACE_DEVICES
#ifdef TRACE_DEVICES
#   define TRACE(x...) dprintf("efi/devices: " x)
#else
#   define TRACE(x...) ;
#endif


static efi_guid BlockIoGUID = EFI_BLOCK_IO_PROTOCOL_GUID;
static efi_guid sLoadedImageProtocolGUID = EFI_LOADED_IMAGE_PROTOCOL_GUID;
static efi_guid sDevicePathProtocolGUID = EFI_DEVICE_PATH_PROTOCOL_GUID;

// Upper bound for the device paths compared below; real ones are a few
// dozen bytes. It only guards against a malformed path without an end node.
static const size_t kMaxDevicePathSize = 4096;


class EfiDevice : public Node
{
	public:
		EfiDevice(efi_block_io_protocol *blockIo);
		virtual ~EfiDevice();

		virtual ssize_t ReadAt(void *cookie, off_t pos, void *buffer,
			size_t bufferSize);
		virtual ssize_t WriteAt(void *cookie, off_t pos, const void *buffer,
			size_t bufferSize) { return B_UNSUPPORTED; }
		virtual off_t Size() const {
			return (fBlockIo->Media->LastBlock + 1) * BlockSize(); }

		uint32 BlockSize() const { return fBlockIo->Media->BlockSize; }
	private:
		efi_block_io_protocol*		fBlockIo;
};


EfiDevice::EfiDevice(efi_block_io_protocol *blockIo)
	:
	fBlockIo(blockIo)
{
}


EfiDevice::~EfiDevice()
{
}


ssize_t
EfiDevice::ReadAt(void *cookie, off_t pos, void *buffer, size_t bufferSize)
{
	TRACE("%s called. pos: %" B_PRIdOFF ", %p, %" B_PRIuSIZE "\n", __func__,
		pos, buffer, bufferSize);

	off_t offset = pos % BlockSize();
	pos /= BlockSize();

	uint32 numBlocks = (offset + bufferSize + BlockSize() - 1) / BlockSize();

	// TODO: We really should implement memalign and align all requests to
	// fBlockIo->Media->IoAlign. This static alignment is large enough though
	// to catch most required alignments.
	char readBuffer[numBlocks * BlockSize()]
		__attribute__((aligned(2048)));

	if (fBlockIo->ReadBlocks(fBlockIo, fBlockIo->Media->MediaId,
		pos, sizeof(readBuffer), readBuffer) != EFI_SUCCESS) {
		dprintf("%s: blockIo error reading from device!\n", __func__);
		return B_ERROR;
	}

	memcpy(buffer, readBuffer + offset, bufferSize);

	return bufferSize;
}


static bool
device_contains_partition(EfiDevice *device, boot::Partition *partition)
{
	EFI::Header *header = (EFI::Header*)partition->content_cookie;
	if (header != NULL && header->InitCheck() == B_OK) {
		// check if device is GPT, and contains partition entry
		uint32 blockSize = device->BlockSize();
		gpt_table_header *deviceHeader =
			(gpt_table_header*)malloc(blockSize);
		ssize_t bytesRead = device->ReadAt(NULL, blockSize, deviceHeader,
			blockSize);
		if (bytesRead != (ssize_t)blockSize)
			return false;

		if (memcmp(deviceHeader, &header->TableHeader(),
				sizeof(gpt_table_header)) != 0)
			return false;

		// partition->cookie == int partition entry index
		uint32 index = (uint32)(addr_t)partition->cookie;
		uint32 size = sizeof(gpt_partition_entry) * (index + 1);
		gpt_partition_entry *entries = (gpt_partition_entry*)malloc(size);
		bytesRead = device->ReadAt(NULL,
			deviceHeader->entries_block * blockSize, entries, size);
		if (bytesRead != (ssize_t)size)
			return false;

		if (memcmp(&entries[index], &header->EntryAt(index),
				sizeof(gpt_partition_entry)) != 0)
			return false;

		for (size_t i = 0; i < sizeof(kTypeMap) / sizeof(struct type_map); ++i)
			if (strcmp(kTypeMap[i].type, BFS_NAME) == 0)
				if (kTypeMap[i].guid == header->EntryAt(index).partition_type)
					return true;

		// Our partition has an EFI header, but we couldn't find one, so bail
		return false;
	}

	if ((partition->offset + partition->size) <= device->Size())
			return true;

	return false;
}


/*!	Returns the size in bytes of \a path without its end node, or 0 if the
	path is malformed (a node shorter than its own header, or no end node
	within kMaxDevicePathSize bytes).
*/
static size_t
device_path_size(const efi_device_path_protocol* path)
{
	size_t size = 0;
	while (size < kMaxDevicePathSize) {
		const efi_device_path_protocol* node
			= (const efi_device_path_protocol*)((const uint8*)path + size);
		if (node->Type == DEVICE_PATH_END)
			return size;
		if (node->Length < sizeof(efi_device_path_protocol))
			return 0;
		size += node->Length;
	}
	return 0;
}


/*!	Returns the device path of the device the firmware loaded this boot
	loader from: the EFI system partition, or the El Torito boot image of a
	CD. Returns NULL if the firmware cannot tell (for example when the loader
	came over the network or from a memory-mapped image).
*/
static efi_device_path_protocol*
boot_loader_device_path()
{
	efi_loaded_image_protocol* loadedImage;
	if (kBootServices->HandleProtocol(kImage, &sLoadedImageProtocolGUID,
			(void**)&loadedImage) != EFI_SUCCESS
		|| loadedImage->DeviceHandle == NULL)
		return NULL;

	efi_device_path_protocol* devicePath;
	if (kBootServices->HandleProtocol(loadedImage->DeviceHandle,
			&sDevicePathProtocolGUID, (void**)&devicePath) != EFI_SUCCESS)
		return NULL;

	return devicePath;
}


/*!	Returns whether \a diskPath is the device path of the whole disk that
	contains \a loaderPath. The firmware names a partition by the path of
	its disk followed by one media node (HardDrive for an MBR or GPT
	partition, CDROM for an El Torito image), so the disk's path has to be a
	prefix of the partition's, followed by such a node. A loader started
	from an unpartitioned disk has the disk's own path.
*/
static bool
is_boot_loader_disk(const efi_device_path_protocol* diskPath,
	const efi_device_path_protocol* loaderPath)
{
	size_t diskSize = device_path_size(diskPath);
	size_t loaderSize = device_path_size(loaderPath);
	if (diskSize == 0 || loaderSize < diskSize
		|| memcmp(diskPath, loaderPath, diskSize) != 0)
		return false;

	if (loaderSize == diskSize)
		return true;

	const efi_device_path_protocol* next
		= (const efi_device_path_protocol*)((const uint8*)loaderPath
			+ diskSize);
	return next->Type == DEVICE_PATH_MEDIA
		&& (next->SubType == MEDIA_HARDDRIVE_DP
			|| next->SubType == MEDIA_CDROM_DP);
}


status_t
platform_add_boot_device(struct stage2_args *args, NodeList *devicesList)
{
	TRACE("%s: called\n", __func__);

	efi_block_io_protocol *blockIo;
	size_t memSize = 0;

	// If the bootloader was started from the network (net_stack_init will find
	// the ip= parameter in the LoadOptions), add network booting as the first
	// entry if available
	status_t error = net_stack_init();
	if (error != B_OK) {
		TRACE("Can't init network...\n");
	} else {
		TRACE("Network is initialized! Search for remote disk...\n");
		RemoteDisk *remoteDisk = RemoteDisk::FindAnyRemoteDisk();
		if (remoteDisk != NULL) {
			devicesList->Add(remoteDisk);
		}
	}

	// Read to zero sized buffer to get memory needed for handles
	if (kBootServices->LocateHandle(ByProtocol, &BlockIoGUID, 0, &memSize, 0)
			!= EFI_BUFFER_TOO_SMALL)
		panic("Cannot read size of block device handles!");

	uint32 noOfHandles = memSize / sizeof(efi_handle);

	efi_handle handles[noOfHandles];
	if (kBootServices->LocateHandle(ByProtocol, &BlockIoGUID, 0, &memSize,
			handles) != EFI_SUCCESS)
		panic("Failed to locate block devices!");

	// The generic loader boots the first BFS boot volume it finds, going
	// through the devices in list order. With several disks that each carry
	// an install, that has to be the one on the disk the firmware loaded us
	// from, the one the user picked in the firmware's boot menu; firmware
	// handle order has nothing to do with that choice. So find that disk
	// below and move it in front of the other disks. If the firmware does
	// not tell us where we came from, the order stays as it was.
	efi_device_path_protocol* loaderPath = boot_loader_device_path();
	EfiDevice* firstDevice = NULL;
	EfiDevice* loaderDisk = NULL;

	// All block devices has one for the disk and one per partition
	// There is a special case for a device with one fixed partition
	// But we probably do not care about booting on that kind of device
	// So find all disk block devices and let Haiku do partition scan
	for (uint32 n = 0; n < noOfHandles; n++) {
		if (kBootServices->HandleProtocol(handles[n], &BlockIoGUID,
				(void**)&blockIo) != EFI_SUCCESS)
			panic("Cannot get block device handle!");

		TRACE("%s: %p: present: %s, logical: %s, removeable: %s, "
			"blocksize: %" PRIu32 ", lastblock: %" PRIu64 "\n",
			__func__, blockIo,
			blockIo->Media->MediaPresent ? "true" : "false",
			blockIo->Media->LogicalPartition ? "true" : "false",
			blockIo->Media->RemovableMedia ? "true" : "false",
			blockIo->Media->BlockSize, blockIo->Media->LastBlock);

		if (!blockIo->Media->MediaPresent
			|| blockIo->Media->LogicalPartition
			|| blockIo->Media->BlockSize == 0)
			continue;

		// The qemu flash device with a 256K block sizes sometime show up
		// in edk2. If flash is unconfigured, bad things happen on arm.
		// edk2 bug: https://bugzilla.tianocore.org/show_bug.cgi?id=2856
		// We're not ready for flash devices in efi, so skip anything odd.
		if (blockIo->Media->BlockSize > 8192)
			continue;

		EfiDevice *device = new(std::nothrow)EfiDevice(blockIo);
		if (device == NULL)
			panic("Can't allocate memory for block devices!");
		devicesList->Insert(device);
		if (firstDevice == NULL)
			firstDevice = device;

		efi_device_path_protocol* diskPath;
		if (loaderPath != NULL && loaderDisk == NULL
			&& kBootServices->HandleProtocol(handles[n],
				&sDevicePathProtocolGUID, (void**)&diskPath) == EFI_SUCCESS
			&& is_boot_loader_disk(diskPath, loaderPath)) {
			loaderDisk = device;
		}
	}

	if (loaderDisk != NULL && loaderDisk != firstDevice) {
		// Only reorder among the disks: a remote disk found above stays
		// first, as before.
		dprintf("efi/devices: boot loader disk is not the first disk, "
			"trying it first\n");
		devicesList->Remove(loaderDisk);
		devicesList->InsertBefore(firstDevice, loaderDisk);
	}

	return devicesList->Count() > 0 ? B_OK : B_ENTRY_NOT_FOUND;
}


status_t
platform_add_block_devices(struct stage2_args *args, NodeList *devicesList)
{
	TRACE("%s: called\n", __func__);

	//TODO: Currently we add all in platform_add_boot_device
	return devicesList->Count() > 0 ? B_OK : B_ENTRY_NOT_FOUND;
}


status_t
platform_get_boot_partitions(struct stage2_args *args, Node *bootDevice,
		NodeList *partitions, NodeList *bootPartitions)
{
	NodeIterator iterator = partitions->GetIterator();
	boot::Partition *partition = NULL;
	while ((partition = (boot::Partition*)iterator.Next()) != NULL) {
		if (device_contains_partition((EfiDevice*)bootDevice, partition)) {
			bootPartitions->Insert(partition);
		}
	}

	return bootPartitions->Count() > 0 ? B_OK : B_ENTRY_NOT_FOUND;
}


status_t
platform_register_boot_device(Node *device, disk_identifier* defaultDiskID)
{
	TRACE("%s: called\n", __func__);

	// TODO is there no better way to identify network boot?
	char buffer[11];
	device->GetName(buffer, sizeof(buffer));
	TRACE("Booting from %s\n", buffer);

	if (strcmp(buffer, "RemoteDisk") == 0) {
		TRACE("Booting from network\n");
		RemoteDisk* remoteDisk = static_cast<RemoteDisk*>(device);
		// Tell the kernel we're booting from network and forward the network
		// configuration
		auto ethernetInterface = NetStack::Default()->GetEthernetInterface();
		if (gBootParams.SetInt32(BOOT_METHOD, BOOT_METHOD_NET)
			|| gBootParams.AddInt64("client MAC",
				ethernetInterface->MACAddress().ToUInt64()) != B_OK
			|| gBootParams.AddInt32("client IP",
				ethernetInterface->IPAddress()) != B_OK
			|| gBootParams.AddInt32("server IP", remoteDisk->ServerIPAddress())
				!= B_OK
			|| gBootParams.AddInt32("server port", remoteDisk->ServerPort())
				!= B_OK) {
			return B_NO_MEMORY;
		}
	} else {
		// ...HARD_DISK, as we pick partition and have checksum (no need to use _CD)
		gBootParams.SetInt32(BOOT_METHOD, BOOT_METHOD_HARD_DISK);
	}

	gBootParams.SetData(BOOT_VOLUME_DISK_IDENTIFIER, B_RAW_TYPE,
		defaultDiskID, sizeof(disk_identifier));

	return B_OK;
}


void
platform_cleanup_devices()
{
}
