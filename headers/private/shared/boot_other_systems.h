/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 */
#ifndef _BOOT_OTHER_SYSTEMS_H
#define _BOOT_OTHER_SYSTEMS_H


/*!	Names for the other operating systems the boot menu can start.

	The boot loader (src/system/boot/loader/other_systems.cpp and the
	platform code behind it) and the Boot menu preferences
	(src/preferences/bootmenu) both detect other systems, the loader to list
	them and the preferences to offer them as the default entry. The setting
	stores the name, so both sides have to arrive at the same name for the
	same system. This header is the one place that decides it. It is plain
	C++ without library calls besides string comparisons, so that it compiles
	in the boot loader as well as in applications.

	Two kinds of systems are named here:

	- EFI loaders found in "\EFI\<directory>\" on an EFI system partition
	  (any FAT volume the firmware can read). Known directories get a
	  product name ("Windows Boot Manager", "Ubuntu", ...), and for each
	  directory one loader file is chosen: the one the system itself registers
	  with the firmware (shim before GRUB, for example). The idea and most of
	  the names follow rEFInd's and systemd-boot's auto-detection; no code was
	  taken from either.
	- BIOS boot records: the first sector of a primary MBR partition (or the
	  MBR of a whole disk) that carries the 0x55aa signature. They are named
	  from the partition type and from strings the well-known boot records
	  contain (Windows' NTFS boot record names "BOOTMGR", MS-DOS's names
	  "IO      SYS", GRUB's says "GRUB").
*/


#include <SupportDefs.h>
#include <string.h>


namespace BootOtherSystems {


// Longest name either side produces, including the terminating null.
static const size_t kMaxNameLength = 64;


struct EFIDirectoryName {
	const char*	directory;	// below \EFI, compared case-insensitively
	const char*	name;		// what the menu shows
};


// Directories of systems whose name is known. Directories not listed here
// are shown with their own name.
static const EFIDirectoryName kEFIDirectoryNames[] = {
	{ "Microsoft",				"Windows Boot Manager" },
	{ "ubuntu",					"Ubuntu" },
	{ "debian",					"Debian" },
	{ "fedora",					"Fedora" },
	{ "redhat",					"Red Hat Enterprise Linux" },
	{ "centos",					"CentOS" },
	{ "rocky",					"Rocky Linux" },
	{ "almalinux",				"AlmaLinux" },
	{ "opensuse",				"openSUSE" },
	{ "opensuse-secureboot",	"openSUSE" },
	{ "sles",					"SUSE Linux Enterprise" },
	{ "arch",					"Arch Linux" },
	{ "manjaro",				"Manjaro" },
	{ "endeavouros",			"EndeavourOS" },
	{ "linuxmint",				"Linux Mint" },
	{ "pop",					"Pop!_OS" },
	{ "elementary",				"elementary OS" },
	{ "gentoo",					"Gentoo" },
	{ "kali",					"Kali Linux" },
	{ "neon",					"KDE neon" },
	{ "zorin",					"Zorin OS" },
	{ "nixos",					"NixOS" },
	{ "void",					"Void Linux" },
	{ "systemd",				"Linux Boot Manager" },
	{ "refind",					"rEFInd" },
	{ "grub",					"GRUB" },
	{ "freebsd",				"FreeBSD" },
	{ "OC",						"OpenCore" },
	{ "CLOVER",					"Clover" },
};


// Loader files, most preferred first. A directory holding several of them
// is one system: shim starts GRUB, so shim is the one to start (it is what
// the distribution registers with the firmware, and it keeps Secure Boot
// working).
static const char* const kPreferredEFILoaders[] = {
	"bootmgfw.efi",			// Windows Boot Manager
	"shimx64.efi",
	"shim.efi",
	"grubx64.efi",
	"systemd-bootx64.efi",
	"refind_x64.efi",
	"loader.efi",			// FreeBSD
	"OpenCore.efi",
	"CLOVERX64.efi",
};


// Files that are never a system of their own: shim's MOK manager and
// fallback helpers, Windows Boot Manager's companions, firmware updaters.
static const char* const kIgnoredEFILoaders[] = {
	"mmx64.efi",
	"MokManager.efi",
	"fbx64.efi",
	"fallback.efi",
	"bootmgr.efi",
	"memtest.efi",
	"fwupdx64.efi",
	"fwupx64.efi",
};


// Directories never listed: Renga's and Haiku's own loaders (their volumes
// are in the menu already), the firmware's fallback directory (handled by
// the caller: it is listed only on removable media or when it is the only
// loader of a volume), and vendor tool directories.
static const char* const kIgnoredEFIDirectories[] = {
	"BOOT",
	"HAIKU",
	"RENGA",
	"Dell",
	"HP",
	"Lenovo",
	"tools",
};


static inline int
compare_ignoring_case(const char* a, const char* b)
{
	for (;; a++, b++) {
		char ca = *a >= 'A' && *a <= 'Z' ? *a + 'a' - 'A' : *a;
		char cb = *b >= 'A' && *b <= 'Z' ? *b + 'a' - 'A' : *b;
		if (ca != cb || ca == '\0')
			return (unsigned char)ca - (unsigned char)cb;
	}
}


static inline bool
has_efi_suffix(const char* fileName)
{
	size_t length = strlen(fileName);
	return length > 4
		&& compare_ignoring_case(fileName + length - 4, ".efi") == 0;
}


static inline bool
is_ignored_efi_directory(const char* directory)
{
	for (size_t i = 0; i < sizeof(kIgnoredEFIDirectories)
			/ sizeof(kIgnoredEFIDirectories[0]); i++) {
		if (compare_ignoring_case(directory, kIgnoredEFIDirectories[i]) == 0)
			return true;
	}
	return false;
}


static inline bool
is_ignored_efi_loader(const char* fileName)
{
	for (size_t i = 0; i < sizeof(kIgnoredEFILoaders)
			/ sizeof(kIgnoredEFILoaders[0]); i++) {
		if (compare_ignoring_case(fileName, kIgnoredEFILoaders[i]) == 0)
			return true;
	}
	return false;
}


/*!	Rank of \a fileName as the loader of its directory: lower is better.
	Returns -1 for files that are not a loader of their own.
*/
static inline int
efi_loader_rank(const char* fileName)
{
	if (!has_efi_suffix(fileName) || is_ignored_efi_loader(fileName))
		return -1;

	const int count = sizeof(kPreferredEFILoaders)
		/ sizeof(kPreferredEFILoaders[0]);
	for (int i = 0; i < count; i++) {
		if (compare_ignoring_case(fileName, kPreferredEFILoaders[i]) == 0)
			return i;
	}
	return count;
}


/*!	Copies the menu name of the system in "\EFI\<directory>" to \a name.
	Windows' directory holds other loaders as well; only bootmgfw.efi is
	Windows Boot Manager, which the caller ensures through
	efi_loader_rank() and is_windows_directory().
*/
static inline void
efi_system_name(const char* directory, char* name, size_t size)
{
	const char* known = directory;
	for (size_t i = 0; i < sizeof(kEFIDirectoryNames)
			/ sizeof(kEFIDirectoryNames[0]); i++) {
		if (compare_ignoring_case(directory, kEFIDirectoryNames[i].directory)
				== 0) {
			known = kEFIDirectoryNames[i].name;
			break;
		}
	}

	size_t i = 0;
	for (; known[i] != '\0' && i + 1 < size; i++) {
		// The boot loader's text menu prints labels as format strings.
		name[i] = known[i] == '%' ? '_' : known[i];
	}
	if (size > 0)
		name[i] = '\0';
}


static inline bool
is_windows_directory(const char* directory)
{
	return compare_ignoring_case(directory, "Microsoft") == 0;
}


// #pragma mark - BIOS boot records


static const uint8 kMBRTypeEmpty = 0x00;
static const uint8 kMBRTypeExtendedCHS = 0x05;
static const uint8 kMBRTypeExtendedLBA = 0x0f;
static const uint8 kMBRTypeLinuxExtended = 0x85;
static const uint8 kMBRTypeBFS = 0xeb;
static const uint8 kMBRTypeEFISystem = 0xef;
static const uint8 kMBRTypeGPTProtective = 0xee;


static inline bool
contains(const uint8* data, size_t size, const char* text)
{
	size_t length = strlen(text);
	for (size_t i = 0; i + length <= size; i++) {
		if (memcmp(data + i, text, length) == 0)
			return true;
	}
	return false;
}


/*!	Whether a partition of MBR type \a type can hold a boot record worth
	listing. Extended partitions, GPT disks, EFI system partitions (UEFI
	loaders, not boot records) and BFS partitions (Haiku and Renga volumes,
	listed by the boot menu itself) cannot.
*/
static inline bool
is_chainloadable_partition_type(uint8 type)
{
	return type != kMBRTypeEmpty && type != kMBRTypeExtendedCHS
		&& type != kMBRTypeExtendedLBA && type != kMBRTypeLinuxExtended
		&& type != kMBRTypeBFS && type != kMBRTypeEFISystem
		&& type != kMBRTypeGPTProtective;
}


/*!	Whether \a sector (512 bytes) is a boot record: the 0x55aa signature and
	code in front of it (an empty boot record, all zeros before the
	signature, as partitioning tools leave behind, is not).
*/
static inline bool
is_boot_record(const uint8* sector, bool wholeDisk)
{
	if (sector[510] != 0x55 || sector[511] != 0xaa)
		return false;

	// A whole disk's MBR has its code in the first 440 bytes, before the
	// disk signature and the partition table.
	size_t codeSize = wholeDisk ? 440 : 510;
	for (size_t i = 0; i < codeSize; i++) {
		if (sector[i] != 0)
			return true;
	}
	return false;
}


/*!	Names the system whose boot record is \a sector, the first sector of a
	partition of MBR type \a type; \a active is the partition's "active"
	(bootable) flag. Returns false if there is nothing to start there.

	Windows formats every NTFS and FAT volume with the same boot code, which
	names BOOTMGR (NTLDR before Vista), data volumes included. Windows Setup
	marks the partition it boots from active, so such boot records count
	only on the active partition. Boot code that only says the volume is not
	bootable (mkfs.fat, mkntfs) does not count either.
*/
static inline bool
boot_record_name(const uint8* sector, uint8 type, bool active, char* name,
	size_t size)
{
	if (!is_boot_record(sector, false))
		return false;
	if (contains(sector, 512, "not a bootable disk")
		|| contains(sector, 512, "is not bootable"))
		return false;

	const char* text;
	if (contains(sector, 512, "BOOTMGR") || contains(sector, 512, "NTLDR")) {
		if (!active)
			return false;
		text = "Windows";
	} else if (contains(sector, 512, "IO      SYS")
		|| contains(sector, 512, "MSDOS   SYS"))
		text = "MS-DOS";
	else if (contains(sector, 512, "KERNEL  SYS"))
		text = "FreeDOS";
	else if (contains(sector, 512, "GRUB"))
		text = "GRUB";
	else if (contains(sector, 512, "SYSLINUX") || contains(sector, 512,
			"EXTLINUX"))
		text = "SYSLINUX";
	else if (type == 0xa5)
		text = "FreeBSD";
	else if (type == 0xa6)
		text = "OpenBSD";
	else if (type == 0xa9)
		text = "NetBSD";
	else if (type == 0x83)
		text = "Linux";
	else if (memcmp(sector + 3, "NTFS    ", 8) == 0
		|| memcmp(sector + 3, "EXFAT   ", 8) == 0) {
		// NTFS or exFAT without Windows' boot code: a data volume
		return false;
	} else
		text = "Other system";

	strlcpy(name, text, size);
	return true;
}


}	// namespace BootOtherSystems


#endif	// _BOOT_OTHER_SYSTEMS_H
