/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 */


/*!	Other operating systems for the boot menu, BIOS side (Renga, roadmap
	F-22). See headers/private/kernel/boot/other_systems.h.

	Installed systems: the MBR of every BIOS hard disk (the BIOS data area
	counts them) is read; each primary partition whose type can hold a boot
	record is read too, and listed if its first sector is one (names from
	headers/private/shared/boot_other_systems.h, shared with the Boot menu
	preferences). The MBR of a disk other than Renga's own is listed as
	"Hard disk <n>" if it has boot code. GPT disks (protective MBR) are
	skipped: a GPT disk boots through UEFI, or through a BIOS boot partition
	that is GRUB's own. Logical partitions are not looked at; DOS and
	Windows only boot from primary ones.

	Removable media: the floppy drives (A: and B:) are read in the
	background while the menu waits for keys, one read per call of
	platform_probe_other_media(), at most kFloppyTries per drive, so an
	empty or slow drive costs the menu one BIOS read at a time. USB drives
	are hard disks for the BIOS and are listed with them. CDs cannot be
	started by a boot loader on a BIOS (only the BIOS itself can boot El
	Torito); that is documented, not done.

	Starting a system is what an MBR does with a partition's boot record:
	the record goes to 0x7c00, a copy of the MBR (with the partition marked
	active) to 0x0600, and chainload_boot_record() jumps there in real mode
	with DL = drive and DS:SI = the partition table entry. The idea is that
	of Haiku's own boot manager (src/bin/makebootable, bootman) and of every
	MBR; the code is new.
*/


#include <boot/other_systems.h>

#include <string.h>

#include <boot/platform.h>
#include <boot/stdio.h>

#include "bios.h"


//#define TRACE_OTHER_SYSTEMS
#ifdef TRACE_OTHER_SYSTEMS
#	define TRACE(x...) dprintf("bios/other_systems: " x)
#else
#	define TRACE(x...) ;
#endif


using namespace BootOtherSystems;


extern uint8 gBootDriveID;
	// from shell.S

extern "C" void chainload_boot_record(uint32 drive, uint32 partitionEntry)
	__attribute__((noreturn));

static const addr_t kBootRecordAddress = 0x7c00;
static const addr_t kMBRCopyAddress = 0x0600;
static const uint32 kMaxHardDisks = 8;
static const int32 kFloppyTries = 3;


/*!	Copies to a fixed low memory address (the boot record's place, for
	example); a plain memcpy() to a constant address makes the compiler
	think it writes out of bounds.
*/
static void
copy_to_low_memory(addr_t address, const uint8* data, size_t size)
{
	volatile uint8* target = (volatile uint8*)address;
	for (size_t i = 0; i < size; i++)
		target[i] = data[i];
}


struct BiosOtherSystem {
	uint8	drive;
	int32	partition;		// MBR table index, or -1 for the whole disk
	uint32	offset;			// first sector of the boot record
};


struct mbr_entry {
	uint8	status;
	uint8	chs_start[3];
	uint8	type;
	uint8	chs_end[3];
	uint32	start;
	uint32	sectors;
} _PACKED;


struct disk_address_packet {
	uint8	size;
	uint8	reserved;
	uint16	blocks;
	uint32	buffer;
	uint64	lba;
} _PACKED;


/*!	Reads sector \a lba of BIOS drive \a drive (512 bytes) into \a buffer:
	with the BIOS's LBA extensions when it has them, else, for sector 0 and
	floppies, with a CHS read. One attempt; the caller decides about
	retries.
*/
static status_t
read_sector(uint8 drive, uint32 lba, uint8* buffer, bool chsOnly = false)
{
	bios_regs regs;
	if (!chsOnly) {
		disk_address_packet* packet
			= (disk_address_packet*)kDataSegmentScratch;
		memset(packet, 0, sizeof(*packet));
		packet->size = sizeof(*packet);
		packet->blocks = 1;
		packet->buffer = kExtraSegmentScratch;
		packet->lba = lba;

		regs.eax = 0x4200;
		regs.edx = drive;
		regs.esi = (addr_t)packet - kDataSegmentBase;
		call_bios(0x13, &regs);
		if ((regs.flags & CARRY_FLAG) == 0) {
			memcpy(buffer, (void*)kExtraSegmentScratch, 512);
			return B_OK;
		}
	}

	if (lba != 0)
		return B_ERROR;

	// cylinder 0, head 0, sector 1
	regs = bios_regs();
	regs.eax = 0x0201;
	regs.ecx = 0x0001;
	regs.edx = drive;
	regs.es = 0;
	regs.ebx = kExtraSegmentScratch;
	call_bios(0x13, &regs);
	if ((regs.flags & CARRY_FLAG) != 0)
		return B_ERROR;

	memcpy(buffer, (void*)kExtraSegmentScratch, 512);
	return B_OK;
}


static OtherSystem*
new_system(const char* name, uint8 drive, int32 partition, uint32 offset,
	other_system_kind kind)
{
	OtherSystem* system = new(std::nothrow) OtherSystem;
	BiosOtherSystem* bios = new(std::nothrow) BiosOtherSystem;
	if (system == NULL || bios == NULL) {
		delete system;
		delete bios;
		return NULL;
	}

	bios->drive = drive;
	bios->partition = partition;
	bios->offset = offset;

	strlcpy(system->name, name, sizeof(system->name));
	if (partition >= 0) {
		snprintf(system->location, sizeof(system->location),
			"partition %" B_PRId32 " of BIOS drive 0x%02x", partition + 1,
			drive);
	} else if (drive < 0x80) {
		snprintf(system->location, sizeof(system->location),
			"the floppy disk in drive %c:", 'A' + drive);
	} else {
		snprintf(system->location, sizeof(system->location),
			"the master boot record of BIOS drive 0x%02x", drive);
	}
	system->kind = kind;
	system->cookie = bios;
	return system;
}


extern "C" void
platform_find_other_systems(other_system_found_hook found)
{
	uint32 count = *(volatile uint8*)0x475;
		// number of hard disks, BIOS data area
	if (count > kMaxHardDisks)
		count = kMaxHardDisks;

	uint8 mbr[512];
	uint8 record[512];
	for (uint32 i = 0; i < count; i++) {
		uint8 drive = 0x80 + i;
		if (read_sector(drive, 0, mbr) != B_OK) {
			dprintf("boot menu: BIOS drive 0x%02x: cannot read its MBR\n",
				drive);
			continue;
		}
		if (mbr[510] != 0x55 || mbr[511] != 0xaa)
			continue;

		const mbr_entry* entries = (const mbr_entry*)(mbr + 446);
		bool gpt = false;
		for (int32 p = 0; p < 4; p++) {
			if (entries[p].type == kMBRTypeGPTProtective)
				gpt = true;
		}
		if (gpt) {
			dprintf("boot menu: BIOS drive 0x%02x is a GPT disk, not "
				"searched\n", drive);
			continue;
		}

		for (int32 p = 0; p < 4; p++) {
			const mbr_entry& entry = entries[p];
			if (!is_chainloadable_partition_type(entry.type)
				|| entry.start == 0 || entry.sectors == 0)
				continue;
			if (read_sector(drive, entry.start, record) != B_OK)
				continue;

			char name[kMaxNameLength];
			if (!boot_record_name(record, entry.type,
					(entry.status & 0x80) != 0, name, sizeof(name))) {
				continue;
			}
			OtherSystem* system = new_system(name, drive, p, entry.start,
				OTHER_SYSTEM_INSTALLED);
			if (system != NULL)
				found(system);
		}

		// Another disk's own MBR (GRUB or Windows in the MBR, say). Renga's
		// own disk's MBR would only start Renga again.
		if (drive != gBootDriveID && is_boot_record(mbr, true)) {
			char name[kMaxNameLength];
			snprintf(name, sizeof(name), "Hard disk %" B_PRIu32, i + 1);
			OtherSystem* system = new_system(name, drive, -1, 0,
				OTHER_SYSTEM_INSTALLED);
			if (system != NULL)
				found(system);
		}
	}
}


// #pragma mark - floppies


struct FloppyDrive {
	bool	checked;		// presence checked
	bool	present;
	bool	done;
	int32	tries;
};

static FloppyDrive sFloppies[2];


/*!	One step for floppy drive \a drive. */
static void
probe_floppy(uint8 drive, other_system_found_hook found)
{
	FloppyDrive& floppy = sFloppies[drive];
	if (floppy.done)
		return;

	bios_regs regs;
	if (!floppy.checked) {
		floppy.checked = true;
		// "get drive parameters": BL is the drive type, 0 if none
		regs.eax = 0x0800;
		regs.edx = drive;
		regs.es = 0;
		regs.edi = 0;
		call_bios(0x13, &regs);
		floppy.present = (regs.flags & CARRY_FLAG) == 0
			&& (regs.ebx & 0xff) != 0 && drive < ((regs.edx & 0xff));
		if (!floppy.present || drive == gBootDriveID) {
			// No drive, or Renga's own boot floppy (a CD booted through
			// its floppy emulation)
			dprintf("boot menu: floppy drive %c: %s\n", 'A' + drive,
				floppy.present ? "is the boot drive" : "not present");
			floppy.done = true;
		}
		return;
	}

	bigtime_t started = system_time();
	uint8 sector[512];
	status_t status = read_sector(drive, 0, sector, true);
	bigtime_t took = system_time() - started;
	floppy.tries++;

	if (status == B_OK) {
		floppy.done = true;
		if (!is_boot_record(sector, false)) {
			dprintf("boot menu: floppy disk in %c: is not bootable (%"
				B_PRId64 " ms)\n", 'A' + drive, took / 1000);
			return;
		}
		dprintf("boot menu: floppy disk in %c: is bootable (%" B_PRId64
			" ms)\n", 'A' + drive, took / 1000);
		char name[kMaxNameLength];
		snprintf(name, sizeof(name), "Floppy disk %c:", 'A' + drive);
		OtherSystem* system = new_system(name, drive, -1, 0,
			OTHER_SYSTEM_REMOVABLE);
		if (system != NULL)
			found(system);
		return;
	}

	// The first read after the motor started often fails: reset, try
	// again next time.
	regs = bios_regs();
	regs.eax = 0x0000;
	regs.edx = drive;
	call_bios(0x13, &regs);
	if (floppy.tries >= kFloppyTries) {
		dprintf("boot menu: floppy drive %c: no disk (%" B_PRId32 " reads, "
			"last %" B_PRId64 " ms)\n", 'A' + drive, floppy.tries,
			took / 1000);
		floppy.done = true;
	}
}


extern "C" bool
platform_probe_other_media(other_system_found_hook found)
{
	static uint32 sNext = 0;

	for (uint32 i = 0; i < 2; i++) {
		uint8 drive = (sNext + i) % 2;
		if (!sFloppies[drive].done) {
			probe_floppy(drive, found);
			sNext = drive + 1;
			break;
		}
	}

	return !sFloppies[0].done || !sFloppies[1].done;
}


// #pragma mark - starting


extern "C" status_t
platform_start_other_system(OtherSystem* system)
{
	BiosOtherSystem* bios = (BiosOtherSystem*)system->cookie;
	if (bios == NULL)
		return B_BAD_VALUE;

	if ((addr_t)&chainload_boot_record >= 0x20000) {
		// The real mode part could not run (see chainload.S).
		dprintf("boot menu: chainload_boot_record at %p is out of reach of "
			"real mode\n", &chainload_boot_record);
		return B_ERROR;
	}

	// Read everything again now: the menu may have been open a while, and
	// a floppy may have been changed.
	uint8 mbr[512];
	uint8 record[512];
	bool floppy = bios->drive < 0x80;
	status_t status = B_OK;
	for (int32 tries = 0; tries < (floppy ? kFloppyTries : 1); tries++) {
		status = read_sector(bios->drive, bios->offset, record, floppy);
		if (status == B_OK)
			break;
	}
	if (status != B_OK)
		return B_IO_ERROR;
	if (record[510] != 0x55 || record[511] != 0xaa)
		return B_BAD_DATA;

	uint32 entryAddress = 0;
	if (bios->partition >= 0) {
		if (read_sector(bios->drive, 0, mbr) != B_OK)
			return B_IO_ERROR;
		// Mark the partition active in the copy, as an MBR does before it
		// starts one; some boot records look at the flag.
		mbr_entry* entries = (mbr_entry*)(mbr + 446);
		for (int32 p = 0; p < 4; p++)
			entries[p].status = p == bios->partition ? 0x80 : 0x00;
		entryAddress = kMBRCopyAddress + 446 + 16 * bios->partition;
	}

	dprintf("boot menu: chainloading BIOS drive 0x%02x, sector %" B_PRIu32
		"\n", bios->drive, bios->offset);

	// Text mode 3 (80x25), as the BIOS leaves it for a boot record. This
	// is the last BIOS call; nothing may touch low memory after it.
	bios_regs regs;
	regs.eax = 0x0003;
	call_bios(0x10, &regs);

	if (bios->partition >= 0)
		copy_to_low_memory(kMBRCopyAddress, mbr, 512);
	copy_to_low_memory(kBootRecordAddress, record, 512);
	chainload_boot_record(bios->drive, entryAddress);
}
