/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 */


/*!	Other operating systems for the boot menu, UEFI side (Renga, roadmap
	F-22). See headers/private/kernel/boot/other_systems.h.

	Installed systems: every volume the firmware can read (it exposes them
	with the Simple File System protocol; EFI system partitions are FAT) on
	a fixed disk is searched for "\EFI\<directory>\<loader>.efi"; the names
	and which file of a directory to start come from
	headers/private/shared/boot_other_systems.h, shared with the Boot menu
	preferences. The firmware's fallback loader "\EFI\BOOT\BOOTX64.EFI"
	counts only when it is the only loader on its volume.

	Before that, the background search connects the firmware's drivers to
	all its controllers, a slice of at most kConnectSlice at a time: firmware
	that boots fast connects only the devices of its boot order, so a CD
	drive, a USB drive or another disk may have no Block I/O protocol yet.
	Systems on fixed disks that appear then are listed too.

	Removable media ("Boot other media"): every removable block device (CD
	and DVD drives, USB drives, floppies) is looked at in the background
	while the menu waits for keys, one step per call of
	platform_probe_other_media(): a device without media is read once to
	let the firmware notice a disc or stick inserted since, with the
	asynchronous Block I/O 2 protocol where the firmware has it, so that a
	slow drive costs the menu nothing but the time to start the read; after
	kProbeTimeout without an answer the device is skipped (and tried again
	later). A device with media gets its drivers connected and is listed if
	it has the removable media loader, "\EFI\BOOT\BOOTX64.EFI". Empty drives
	are read again every kEmptyRetryInterval, so media inserted while the
	menu is open appear too. The device the boot loader itself came from is
	never listed.

	Starting a system uses the firmware's LoadImage() and StartImage() with
	the file's full device path, the way the firmware's own boot manager
	does: the firmware checks Secure Boot signatures, and the started loader
	finds its own device and path in its Loaded Image protocol (Windows Boot
	Manager reads its BCD store from there). If the started application
	returns, the text and graphics modes are put back and the menu goes on.
*/


#include <boot/other_systems.h>

#include <string.h>

#include <boot/platform.h>
#include <boot/stdio.h>
#include <util/DoublyLinkedList.h>

#include "efi_platform.h"
#include <efi/protocol/block-io.h>
#include <efi/protocol/device-path.h>
#include <efi/protocol/file.h>
#include <efi/protocol/graphics-output.h>
#include <efi/protocol/loaded-image.h>
#include <efi/protocol/simple-file-system.h>


//#define TRACE_OTHER_SYSTEMS
#ifdef TRACE_OTHER_SYSTEMS
#	define TRACE(x...) dprintf("efi/other_systems: " x)
#else
#	define TRACE(x...) ;
#endif


using namespace BootOtherSystems;


// Not in the tree's EFI headers: UEFI 2.3.1, section 13.10.
#define EFI_BLOCK_IO2_PROTOCOL_GUID \
	{0xa77b2472, 0xe282, 0x4e9f, {0xa2, 0x45, 0xc2, 0xc0, 0xe2, 0x7b, 0xbc, \
		0xc1}}

struct efi_block_io2_token {
	efi_event	Event;
	efi_status	TransactionStatus;
};

struct efi_block_io2_protocol {
	efi_block_io_media*	Media;
	efi_status (*Reset)(efi_block_io2_protocol* self,
		bool extendedVerification) EFIAPI;
	efi_status (*ReadBlocksEx)(efi_block_io2_protocol* self, uint32_t mediaId,
		uint64_t lba, efi_block_io2_token* token, size_t bufferSize,
		void* buffer) EFIAPI;
	efi_status (*WriteBlocksEx)(efi_block_io2_protocol* self,
		uint32_t mediaId, uint64_t lba, efi_block_io2_token* token,
		size_t bufferSize, void* buffer) EFIAPI;
	efi_status (*FlushBlocksEx)(efi_block_io2_protocol* self,
		efi_block_io2_token* token) EFIAPI;
};


static efi_guid sBlockIoGUID = EFI_BLOCK_IO_PROTOCOL_GUID;
static efi_guid sBlockIo2GUID = EFI_BLOCK_IO2_PROTOCOL_GUID;
static efi_guid sDevicePathGUID = EFI_DEVICE_PATH_PROTOCOL_GUID;
static efi_guid sFileSystemGUID = EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID;
static efi_guid sFileInfoGUID = EFI_FILE_INFO_GUID;
static efi_guid sLoadedImageGUID = EFI_LOADED_IMAGE_PROTOCOL_GUID;
static efi_guid sGraphicsOutputGUID = EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID;

static const size_t kMaxDevicePathSize = 4096;
static const size_t kMaxRemovableDevices = 16;
static const bigtime_t kProbeTimeout = 3000000;
static const bigtime_t kEmptyRetryInterval = 3000000;
static const bigtime_t kSlowRetryInterval = 15000000;

static const char* const kRemovableLoader = "\\EFI\\BOOT\\BOOTX64.EFI";


struct EfiOtherSystem {
	efi_device_path_protocol*	path;	// device + file, for LoadImage()
	efi_handle					fileSystem;
};


enum probe_state {
	PROBE_NEW = 0,		// not looked at yet
	PROBE_READING,		// asynchronous read pending
	PROBE_EMPTY,		// no media (or no answer); retry at fNextProbe
	PROBE_LISTED,		// has media; its file systems were searched
};


struct RemovableDevice {
	efi_handle					handle;
	efi_block_io_protocol*		blockIo;
	efi_block_io2_protocol*		blockIo2;
	efi_device_path_protocol*	path;
	probe_state					state;
	bool						probedOnce;
	bigtime_t					started;
	bigtime_t					nextProbe;
	efi_block_io2_token			token;
	void*						buffer;
	uint32						mediaId;
	char						kind[24];
};


static RemovableDevice sRemovable[kMaxRemovableDevices];
static size_t sRemovableCount = 0;
static bool sRemovableInitialized = false;
static size_t sNextRemovable = 0;
static efi_handle* sListedFileSystems = NULL;
static size_t sListedFileSystemCount = 0;
static size_t sListedFileSystemCapacity = 0;
static int32 sVolumeNumber = 0;


// #pragma mark - device paths


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


static efi_device_path_protocol*
handle_device_path(efi_handle handle)
{
	efi_device_path_protocol* path;
	if (kBootServices->HandleProtocol(handle, &sDevicePathGUID,
			(void**)&path) != EFI_SUCCESS)
		return NULL;
	return path;
}


/*!	Whether \a path starts with all of \a prefix (both without their end
	nodes).
*/
static bool
device_path_starts_with(const efi_device_path_protocol* path,
	const efi_device_path_protocol* prefix)
{
	if (path == NULL || prefix == NULL)
		return false;
	size_t prefixSize = device_path_size(prefix);
	size_t pathSize = device_path_size(path);
	return prefixSize > 0 && pathSize >= prefixSize
		&& memcmp(path, prefix, prefixSize) == 0;
}


/*!	Returns a new device path: \a device followed by a file path node for
	\a fileName ("\EFI\...", ASCII) and the end node. Allocated on the boot
	loader's heap; LoadImage() copies what it needs.
*/
static efi_device_path_protocol*
file_device_path(const efi_device_path_protocol* device, const char* fileName)
{
	size_t deviceSize = device_path_size(device);
	size_t nameLength = strlen(fileName);
	size_t nodeSize = sizeof(efi_device_path_protocol)
		+ (nameLength + 1) * sizeof(char16_t);
	if (deviceSize == 0 || nodeSize > 0xffff)
		return NULL;

	uint8* path = (uint8*)malloc(deviceSize + nodeSize
		+ sizeof(efi_device_path_protocol));
	if (path == NULL)
		return NULL;

	memcpy(path, device, deviceSize);
	efi_device_path_protocol* node
		= (efi_device_path_protocol*)(path + deviceSize);
	node->Type = DEVICE_PATH_MEDIA;
	node->SubType = MEDIA_FILEPATH_DP;
	node->Length = nodeSize;
	char16_t* name = (char16_t*)(node + 1);
	for (size_t i = 0; i <= nameLength; i++)
		name[i] = (uint8)fileName[i];

	efi_device_path_protocol* end
		= (efi_device_path_protocol*)(path + deviceSize + nodeSize);
	end->Type = DEVICE_PATH_END;
	end->SubType = DEVICE_PATH_ENTIRE_END;
	end->Length = sizeof(efi_device_path_protocol);
	return (efi_device_path_protocol*)path;
}


/*!	What kind of removable device \a path is, for "Boot other media: ...".
*/
static void
device_kind(const efi_device_path_protocol* path, uint32 blockSize,
	char* kind, size_t size)
{
	const char* text = blockSize == 2048 ? "CD/DVD" : "Removable disk";
	size_t offset = 0;
	size_t total = device_path_size(path);
	while (offset < total) {
		const efi_device_path_protocol* node
			= (const efi_device_path_protocol*)((const uint8*)path + offset);
		if (node->Type == DEVICE_PATH_MESSAGING
			&& (node->SubType == DEVICE_PATH_MESSAGING_USB
				|| node->SubType == DEVICE_PATH_MESSAGING_USB_CLASS
				|| node->SubType == DEVICE_PATH_MESSAGING_USB_WWID)) {
			text = blockSize == 2048 ? "USB CD/DVD" : "USB drive";
			break;
		}
		if (node->Type == DEVICE_PATH_ACPI && node->SubType == 1
			&& node->Length >= 12) {
			// ACPI _HID: EISA "PNP0604" and "PNP0700" are floppy drives
			uint32 hid;
			memcpy(&hid, (const uint8*)node + 4, sizeof(hid));
			if (hid == ((0x0604u << 16) | 0x41d0)
				|| hid == ((0x0700u << 16) | 0x41d0)) {
				text = "Floppy disk";
				break;
			}
		}
		offset += node->Length;
	}
	strlcpy(kind, text, size);
}


// #pragma mark - files


static void
to_ascii(const char16_t* name, char* buffer, size_t size)
{
	size_t i = 0;
	for (; name[i] != 0 && i + 1 < size; i++)
		buffer[i] = name[i] < 0x80 && name[i] != '%' ? (char)name[i] : '_';
	buffer[i] = '\0';
}


static void
to_char16(const char* name, char16_t* buffer, size_t count)
{
	size_t i = 0;
	for (; name[i] != '\0' && i + 1 < count; i++)
		buffer[i] = (uint8)name[i];
	buffer[i] = 0;
}


static efi_file_protocol*
open_file(efi_file_protocol* directory, const char* path)
{
	char16_t name[256];
	to_char16(path, name, sizeof(name) / sizeof(name[0]));
	efi_file_protocol* file;
	if (directory->Open(directory, &file, name, EFI_FILE_MODE_READ, 0)
			!= EFI_SUCCESS)
		return NULL;
	return file;
}


/*!	Reads the next entry of \a directory: its ASCII name and whether it is a
	directory. Returns false at the end.
*/
static bool
next_entry(efi_file_protocol* directory, char* name, size_t nameSize,
	bool& isDirectory)
{
	uint8 buffer[sizeof(efi_file_info) + 512 * sizeof(char16_t)]
		__attribute__((aligned(8)));
	while (true) {
		size_t size = sizeof(buffer);
		if (directory->Read(directory, &size, buffer) != EFI_SUCCESS
			|| size == 0)
			return false;

		efi_file_info* info = (efi_file_info*)buffer;
		to_ascii(info->FileName, name, nameSize);
		if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
			continue;
		isDirectory = (info->Attribute & EFI_FILE_DIRECTORY) != 0;
		return true;
	}
}


static bool
file_exists(efi_file_protocol* root, const char* path)
{
	efi_file_protocol* file = open_file(root, path);
	if (file == NULL)
		return false;
	file->Close(file);
	return true;
}


static efi_file_protocol*
open_volume(efi_handle handle)
{
	efi_simple_file_system_protocol* fileSystem;
	if (kBootServices->HandleProtocol(handle, &sFileSystemGUID,
			(void**)&fileSystem) != EFI_SUCCESS)
		return NULL;
	efi_file_protocol* root;
	if (fileSystem->OpenVolume(fileSystem, &root) != EFI_SUCCESS)
		return NULL;
	return root;
}


// #pragma mark - this boot loader


static efi_handle sOwnDevice = NULL;
static char sOwnPath[256];
static efi_device_path_protocol* sOwnDevicePath = NULL;


static void
init_own_image()
{
	static bool initialized = false;
	if (initialized)
		return;
	initialized = true;

	efi_loaded_image_protocol* loadedImage;
	if (kBootServices->HandleProtocol(kImage, &sLoadedImageGUID,
			(void**)&loadedImage) != EFI_SUCCESS)
		return;

	sOwnDevice = loadedImage->DeviceHandle;
	if (sOwnDevice != NULL)
		sOwnDevicePath = handle_device_path(sOwnDevice);

	// The file path: one or more file path nodes ("\EFI\BOOT",
	// "BOOTX64.EFI", or all of it in one).
	sOwnPath[0] = '\0';
	const efi_device_path_protocol* node = loadedImage->FilePath;
	for (size_t offset = 0; node != NULL && offset < kMaxDevicePathSize;) {
		const efi_device_path_protocol* current
			= (const efi_device_path_protocol*)((const uint8*)node + offset);
		if (current->Type == DEVICE_PATH_END
			|| current->Length < sizeof(efi_device_path_protocol))
			break;
		if (current->Type == DEVICE_PATH_MEDIA
			&& current->SubType == MEDIA_FILEPATH_DP) {
			char part[256];
			to_ascii((const char16_t*)(current + 1), part, sizeof(part));
			size_t length = strlen(sOwnPath);
			if (length > 0 && sOwnPath[length - 1] != '\\' && part[0] != '\\')
				strlcat(sOwnPath, "\\", sizeof(sOwnPath));
			strlcat(sOwnPath, part, sizeof(sOwnPath));
		}
		offset += current->Length;
	}
	dprintf("boot menu: this boot loader is \"%s\"\n", sOwnPath);
}


static bool
is_own_loader(efi_handle fileSystem, const char* path)
{
	return fileSystem == sOwnDevice
		&& compare_ignoring_case(path, sOwnPath) == 0;
}


// #pragma mark - finding systems


static bool
already_listed(efi_handle fileSystem)
{
	for (size_t i = 0; i < sListedFileSystemCount; i++) {
		if (sListedFileSystems[i] == fileSystem)
			return true;
	}
	return false;
}


static void
remember_listed(efi_handle fileSystem)
{
	if (sListedFileSystemCount == sListedFileSystemCapacity) {
		size_t capacity = sListedFileSystemCapacity + 16;
		efi_handle* handles = (efi_handle*)malloc(capacity
			* sizeof(efi_handle));
		if (handles == NULL)
			return;
		if (sListedFileSystems != NULL) {
			memcpy(handles, sListedFileSystems,
				sListedFileSystemCount * sizeof(efi_handle));
			free(sListedFileSystems);
		}
		sListedFileSystems = handles;
		sListedFileSystemCapacity = capacity;
	}
	sListedFileSystems[sListedFileSystemCount++] = fileSystem;
}


static OtherSystem*
new_system(efi_handle fileSystem, const char* name, const char* file,
	const char* where, other_system_kind kind)
{
	efi_device_path_protocol* device = handle_device_path(fileSystem);
	if (device == NULL)
		return NULL;

	OtherSystem* system = new(std::nothrow) OtherSystem;
	EfiOtherSystem* efi = new(std::nothrow) EfiOtherSystem;
	if (system == NULL || efi == NULL) {
		delete system;
		delete efi;
		return NULL;
	}
	efi->path = file_device_path(device, file);
	efi->fileSystem = fileSystem;
	if (efi->path == NULL) {
		delete system;
		delete efi;
		return NULL;
	}

	strlcpy(system->name, name, sizeof(system->name));
	snprintf(system->location, sizeof(system->location), "%s on %s", file,
		where);
	system->kind = kind;
	system->cookie = efi;
	return system;
}


/*!	Lists the systems in "\EFI" of the volume \a fileSystem (a fixed disk).
*/
static void
find_systems_on_volume(efi_handle fileSystem, other_system_found_hook found)
{
	efi_file_protocol* root = open_volume(fileSystem);
	if (root == NULL)
		return;

	char where[48];
	snprintf(where, sizeof(where), "EFI volume %" B_PRId32, ++sVolumeNumber);

	int32 count = 0;
	efi_file_protocol* efiDirectory = open_file(root, "\\EFI");
	if (efiDirectory != NULL) {
		char directoryName[128];
		bool isDirectory;
		while (next_entry(efiDirectory, directoryName, sizeof(directoryName),
				isDirectory)) {
			if (!isDirectory || is_ignored_efi_directory(directoryName))
				continue;

			char path[256];
			snprintf(path, sizeof(path), "\\EFI\\%s", directoryName);
			bool windows = is_windows_directory(directoryName);
			if (windows)
				strlcat(path, "\\Boot", sizeof(path));

			efi_file_protocol* directory = open_file(root, path);
			if (directory == NULL)
				continue;

			char best[128] = "";
			int bestRank = -1;
			char fileName[128];
			while (next_entry(directory, fileName, sizeof(fileName),
					isDirectory)) {
				if (isDirectory)
					continue;
				int rank = efi_loader_rank(fileName);
				if (rank < 0 || (windows
						&& compare_ignoring_case(fileName, "bootmgfw.efi")
							!= 0)) {
					continue;
				}
				if (bestRank < 0 || rank < bestRank) {
					bestRank = rank;
					strlcpy(best, fileName, sizeof(best));
				}
			}
			directory->Close(directory);
			if (bestRank < 0)
				continue;

			strlcat(path, "\\", sizeof(path));
			strlcat(path, best, sizeof(path));
			if (is_own_loader(fileSystem, path))
				continue;

			char name[kMaxNameLength];
			efi_system_name(directoryName, name, sizeof(name));
			OtherSystem* system = new_system(fileSystem, name, path, where,
				OTHER_SYSTEM_INSTALLED);
			if (system != NULL) {
				found(system);
				count++;
			}
		}
		efiDirectory->Close(efiDirectory);
	}

	// The fallback loader only when nothing else is there: on a volume with
	// other loaders it is usually a copy of one of them.
	if (count == 0 && file_exists(root, kRemovableLoader)
		&& !is_own_loader(fileSystem, kRemovableLoader)) {
		OtherSystem* system = new_system(fileSystem, "EFI fallback loader",
			kRemovableLoader, where, OTHER_SYSTEM_INSTALLED);
		if (system != NULL)
			found(system);
	}

	root->Close(root);
}


static bool
is_removable(efi_handle handle)
{
	efi_block_io_protocol* blockIo;
	if (kBootServices->HandleProtocol(handle, &sBlockIoGUID,
			(void**)&blockIo) != EFI_SUCCESS)
		return false;
	return blockIo->Media->RemovableMedia;
}


extern "C" void
platform_find_other_systems(other_system_found_hook found)
{
	init_own_image();

	size_t count = 0;
	efi_handle* handles = NULL;
	if (kBootServices->LocateHandleBuffer(ByProtocol, &sFileSystemGUID, NULL,
			&count, &handles) != EFI_SUCCESS) {
		dprintf("boot menu: no file systems from the firmware\n");
		return;
	}

	for (size_t i = 0; i < count; i++) {
		// Removable media are the background search's business.
		if (is_removable(handles[i]))
			continue;
		find_systems_on_volume(handles[i], found);
		remember_listed(handles[i]);
	}
	kBootServices->FreePool(handles);
}


// #pragma mark - removable media


static void
init_removable_devices()
{
	if (sRemovableInitialized)
		return;
	sRemovableInitialized = true;
	init_own_image();

	size_t count = 0;
	efi_handle* handles = NULL;
	if (kBootServices->LocateHandleBuffer(ByProtocol, &sBlockIoGUID, NULL,
			&count, &handles) != EFI_SUCCESS)
		return;

	dprintf("boot menu: %" B_PRIuSIZE " block devices\n", count);
	for (size_t i = 0; i < count && sRemovableCount < kMaxRemovableDevices;
			i++) {
		efi_block_io_protocol* blockIo;
		if (kBootServices->HandleProtocol(handles[i], &sBlockIoGUID,
				(void**)&blockIo) != EFI_SUCCESS)
			continue;
		efi_block_io_media* media = blockIo->Media;
		if (!media->RemovableMedia || media->LogicalPartition)
			continue;

		// A drive without media may report a block size of 0.
		efi_device_path_protocol* path = handle_device_path(handles[i]);
		if (path == NULL || media->BlockSize > 8192) {
			dprintf("boot menu: removable device skipped (%s)\n",
				path == NULL ? "no device path" : "block size");
			continue;
		}
		if (device_path_starts_with(sOwnDevicePath, path)) {
			// Renga's own boot medium (an install USB drive or CD).
			dprintf("boot menu: removable device skipped (boot medium)\n");
			continue;
		}

		RemovableDevice& device = sRemovable[sRemovableCount++];
		memset(&device, 0, sizeof(device));
		device.handle = handles[i];
		device.blockIo = blockIo;
		device.path = path;
		device.state = PROBE_NEW;
		if (kBootServices->HandleProtocol(handles[i], &sBlockIo2GUID,
				(void**)&device.blockIo2) != EFI_SUCCESS)
			device.blockIo2 = NULL;
		device_kind(path, media->BlockSize, device.kind,
			sizeof(device.kind));
		dprintf("boot menu: removable device %" B_PRIuSIZE ": %s, %s, "
			"block I/O%s\n", sRemovableCount, device.kind,
			media->MediaPresent ? "media present" : "no media",
			device.blockIo2 != NULL ? " 2 (asynchronous)" : " (synchronous)");
	}
	kBootServices->FreePool(handles);
}


/*!	Lists the removable media loader of each file system on \a device.
*/
static void
list_removable_media(RemovableDevice& device, other_system_found_hook found)
{
	size_t count = 0;
	efi_handle* handles = NULL;
	if (kBootServices->LocateHandleBuffer(ByProtocol, &sFileSystemGUID, NULL,
			&count, &handles) != EFI_SUCCESS)
		return;

	for (size_t i = 0; i < count; i++) {
		if (already_listed(handles[i])
			|| !device_path_starts_with(handle_device_path(handles[i]),
				device.path))
			continue;
		remember_listed(handles[i]);
		if (handles[i] == sOwnDevice)
			continue;

		efi_file_protocol* root = open_volume(handles[i]);
		if (root == NULL)
			continue;
		bool bootable = file_exists(root, kRemovableLoader);
		root->Close(root);
		if (!bootable) {
			dprintf("boot menu: %s has no %s\n", device.kind,
				kRemovableLoader);
			continue;
		}

		char where[48];
		snprintf(where, sizeof(where), "the %s", device.kind);
		OtherSystem* system = new_system(handles[i], device.kind,
			kRemovableLoader, where, OTHER_SYSTEM_REMOVABLE);
		if (system != NULL)
			found(system);
	}
	kBootServices->FreePool(handles);
}


static void
media_arrived(RemovableDevice& device, other_system_found_hook found)
{
	dprintf("boot menu: media in %s after %" B_PRId64 " ms\n", device.kind,
		(system_time() - device.started) / 1000);
	// Let the partition and file system drivers see it: the firmware
	// connected the device at boot, possibly before the media was there.
	kBootServices->ConnectController(device.handle, NULL, NULL, true);
	device.state = PROBE_LISTED;
	device.probedOnce = true;
	list_removable_media(device, found);
}


static void
media_absent(RemovableDevice& device, efi_status status, bool timedOut)
{
	if (!device.probedOnce || timedOut) {
		dprintf("boot menu: %s: %s after %" B_PRId64 " ms\n", device.kind,
			timedOut ? "no answer, skipped for now"
				: status == EFI_NO_MEDIA ? "no media" : "not readable",
			(system_time() - device.started) / 1000);
	}
	device.state = PROBE_EMPTY;
	device.probedOnce = true;
	device.nextProbe = system_time()
		+ (timedOut ? kSlowRetryInterval : kEmptyRetryInterval);
}


/*!	One step for \a device: start or check a read, or list its media.
*/
static void
probe_device(RemovableDevice& device, other_system_found_hook found)
{
	efi_block_io_media* media = device.blockIo->Media;

	switch (device.state) {
		case PROBE_NEW:
		case PROBE_EMPTY:
		{
			if (device.state == PROBE_EMPTY
				&& system_time() < device.nextProbe)
				return;
			device.started = system_time();

			if (media->MediaPresent && device.state == PROBE_NEW) {
				// Media were there when the firmware started: nothing to
				// read, the file systems are there already.
				device.state = PROBE_LISTED;
				device.probedOnce = true;
				list_removable_media(device, found);
				return;
			}

			// Reading block 0 makes the firmware check for media; a
			// changed medium answers EFI_MEDIA_CHANGED (and updates
			// MediaId), a missing one EFI_NO_MEDIA.
			if (device.buffer == NULL) {
				if (kBootServices->AllocatePool(EfiBootServicesData,
						8192 + 4096, &device.buffer) != EFI_SUCCESS) {
					device.state = PROBE_LISTED;
					return;
				}
			}
			void* buffer = (void*)(((addr_t)device.buffer + 4095)
				& ~(addr_t)4095);
			uint32 blockSize = media->BlockSize != 0 ? media->BlockSize
				: 2048;
			device.mediaId = media->MediaId;

			if (device.blockIo2 != NULL) {
				if (device.token.Event == NULL
					&& kBootServices->CreateEvent(0, 0, NULL, NULL,
						&device.token.Event) != EFI_SUCCESS) {
					device.blockIo2 = NULL;
				}
			}
			if (device.blockIo2 != NULL) {
				device.token.TransactionStatus = EFI_NOT_READY;
				efi_status status = device.blockIo2->ReadBlocksEx(
					device.blockIo2, device.mediaId, 0, &device.token,
					blockSize, buffer);
				if (status == EFI_SUCCESS) {
					// queued; CheckEvent() tells when it is done
					device.state = PROBE_READING;
					return;
				}
				// Some drivers check for media before queueing.
				if (status == EFI_MEDIA_CHANGED && media->MediaPresent)
					media_arrived(device, found);
				else
					media_absent(device, status, false);
				return;
			}

			// Synchronous: bounded only by the firmware's own timeouts.
			efi_status status = device.blockIo->ReadBlocks(device.blockIo,
				device.mediaId, 0, blockSize, buffer);
			if (status == EFI_MEDIA_CHANGED) {
				// The first read after a change fails; the next works.
				status = device.blockIo->ReadBlocks(device.blockIo,
					media->MediaId, 0, media->BlockSize != 0
						? media->BlockSize : blockSize, buffer);
			}
			if (status == EFI_SUCCESS && media->MediaPresent)
				media_arrived(device, found);
			else
				media_absent(device, status, false);
			return;
		}

		case PROBE_READING:
		{
			efi_status status = kBootServices->CheckEvent(device.token.Event);
			if (status == EFI_NOT_READY) {
				if (system_time() - device.started > kProbeTimeout) {
					// Leave the request (and its buffer) alone; the device
					// is not read again until it answered.
					media_absent(device, EFI_NOT_READY, true);
					device.state = PROBE_READING;
				}
				return;
			}
			status = device.token.TransactionStatus;
			if ((status == EFI_SUCCESS || status == EFI_MEDIA_CHANGED)
				&& media->MediaPresent) {
				media_arrived(device, found);
			} else
				media_absent(device, status, false);
			return;
		}

		case PROBE_LISTED:
			return;
	}
}


/*!	Systems on fixed disks whose drivers were connected only now (by
	connect_all_step()).
*/
static void
find_new_installed_systems(other_system_found_hook found)
{
	size_t count = 0;
	efi_handle* handles = NULL;
	if (kBootServices->LocateHandleBuffer(ByProtocol, &sFileSystemGUID, NULL,
			&count, &handles) != EFI_SUCCESS)
		return;
	for (size_t i = 0; i < count; i++) {
		if (already_listed(handles[i]) || is_removable(handles[i]))
			continue;
		dprintf("boot menu: new volume after connecting drivers\n");
		find_systems_on_volume(handles[i], found);
		remember_listed(handles[i]);
	}
	kBootServices->FreePool(handles);
}


static efi_handle* sAllHandles = NULL;
static size_t sAllHandleCount = 0;
static size_t sNextConnect = 0;
static bool sConnectDone = false;
static const bigtime_t kConnectSlice = 30000;


/*!	Connects the firmware's drivers to its controllers, a slice at a time.
	Firmware that boots fast connects only the devices of its boot order;
	a CD drive, a USB drive or a second disk may have no Block I/O protocol
	until something connects them (the firmware's own boot menu does that
	when it is opened; rEFInd and GRUB do the same). Returns true when all
	are done.
*/
static bool
connect_all_step(other_system_found_hook found)
{
	if (sConnectDone)
		return true;

	if (sAllHandles == NULL) {
		if (kBootServices->LocateHandleBuffer(AllHandles, NULL, NULL,
				&sAllHandleCount, &sAllHandles) != EFI_SUCCESS) {
			sConnectDone = true;
			return true;
		}
		dprintf("boot menu: connecting drivers to %" B_PRIuSIZE
			" handles\n", sAllHandleCount);
	}

	bigtime_t started = system_time();
	while (sNextConnect < sAllHandleCount) {
		kBootServices->ConnectController(sAllHandles[sNextConnect++], NULL,
			NULL, true);
		if (system_time() - started >= kConnectSlice)
			return false;
	}

	kBootServices->FreePool(sAllHandles);
	sAllHandles = NULL;
	sConnectDone = true;
	dprintf("boot menu: drivers connected\n");
	find_new_installed_systems(found);
	return true;
}


extern "C" bool
platform_probe_other_media(other_system_found_hook found)
{
	if (!connect_all_step(found))
		return true;

	init_removable_devices();
	if (sRemovableCount == 0)
		return false;

	// One device per call, round robin, so that one slow device does not
	// keep the others from being looked at.
	RemovableDevice& device = sRemovable[sNextRemovable];
	sNextRemovable = (sNextRemovable + 1) % sRemovableCount;
	probe_device(device, found);

	for (size_t i = 0; i < sRemovableCount; i++) {
		if (!sRemovable[i].probedOnce)
			return true;
	}
	return false;
}


// #pragma mark - starting


extern "C" status_t
platform_start_other_system(OtherSystem* system)
{
	EfiOtherSystem* efi = (EfiOtherSystem*)system->cookie;
	if (efi == NULL)
		return B_BAD_VALUE;

	efi_handle image = NULL;
	efi_status status = kBootServices->LoadImage(false, kImage, efi->path,
		NULL, 0, &image);
	if (status != EFI_SUCCESS) {
		dprintf("boot menu: LoadImage() failed: 0x%" B_PRIx64 "\n",
			(uint64)status);
		return status == EFI_NOT_FOUND ? B_ENTRY_NOT_FOUND
			: status == EFI_SECURITY_VIOLATION ? B_PERMISSION_DENIED
			: B_ERROR;
	}

	// Remember the console and graphics modes; the started program may
	// change them and return.
	efi_simple_text_output_protocol* conOut = kSystemTable->ConOut;
	int32 textMode = conOut->Mode->Mode;
	int32 attribute = conOut->Mode->Attribute;
	efi_graphics_output_protocol* graphics = NULL;
	uint32 graphicsMode = 0;
	if (kBootServices->LocateProtocol(&sGraphicsOutputGUID, NULL,
			(void**)&graphics) == EFI_SUCCESS && graphics != NULL) {
		graphicsMode = graphics->Mode->Mode;
	} else
		graphics = NULL;

	conOut->SetAttribute(conOut, 0x07);
	conOut->ClearScreen(conOut);

	size_t exitDataSize = 0;
	char16_t* exitData = NULL;
	status = kBootServices->StartImage(image, &exitDataSize, &exitData);
	if (exitData != NULL)
		kBootServices->FreePool(exitData);
	dprintf("boot menu: StartImage() returned 0x%" B_PRIx64 "\n",
		(uint64)status);

	if (graphics != NULL && graphics->Mode->Mode != graphicsMode)
		graphics->SetMode(graphics, graphicsMode);
	if (conOut->Mode->Mode != textMode)
		conOut->SetMode(conOut, textMode);
	conOut->SetAttribute(conOut, attribute);
	conOut->EnableCursor(conOut, false);

	return status == EFI_SUCCESS ? B_OK : B_ERROR;
}
