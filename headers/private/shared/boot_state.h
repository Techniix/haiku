/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 */
#ifndef _RENGA_BOOT_STATE_H
#define _RENGA_BOOT_STATE_H


/*!	The boot state of a Renga volume (roadmap F-20/F-22, boot loader batch
	A): boot counting, the last working system state and the one-shot
	choice for the next start ("Restart into...").

	One file, /boot/system/settings/boot/state, kFileSize bytes, holds two
	copies of a Record, one per kSlotSize slot. Every writer writes the
	slot the newest valid copy is not in, with the next sequence number and
	a CRC-32, and never truncates, extends or replaces the file: the boot
	loader writes in place into the blocks the file already has (it cannot
	allocate), and a power cut in the middle of a write leaves the other
	copy intact, which the next reader then takes.

	Who writes what:
	- the boot loader, at every start it begins: failedStarts + 1, the
	  package state it starts ("booting"), the one-shot choice cleared;
	- "bootstate boot" (a launch job), once the desktop is up (or, on a
	  system without a desktop, once the boot is through): failedStarts = 0,
	  lastGood = booting;
	- package_daemon, when an update turns the current state into an old
	  one (state_<date>): the names that meant "the latest state" are
	  renamed to that directory, so lastGood keeps naming the system that
	  worked, not the update that replaced it;
	- "bootstate next ..." and the Boot menu preferences: nextOnce.

	After kFailedStartsForRecovery starts in a row that never got confirmed
	the boot loader opens its menu by itself, with the last working system
	state selected and a countdown that starts it.

	This header is used by the boot loader, the bootstate tool,
	package_daemon, the Boot menu preferences and the tests, so it only
	needs <string.h> and <stdio.h>. The layout is add-only: a later version
	appends fields after "checksum" (with a checksum of their own) and
	raises "version" and "size"; readers ignore what they do not know.
*/


#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>


namespace BootState {


static const char* const kPath = "system/settings/boot/state";
	// relative to the root of the boot volume
static const char* const kDirectory = "system/settings/boot";
static const size_t kSlotSize = 4096;
static const size_t kSlotCount = 2;
static const size_t kFileSize = kSlotSize * kSlotCount;
static const uint32_t kMagic = 0x52624253;
	// 'RbBS'
static const uint32_t kVersion = 1;
static const uint32_t kFailedStartsForRecovery = 2;
static const size_t kNameLength = 64;
static const size_t kNextLength = 128;

// The restore point note package_daemon writes into a state directory it
// creates: "replaced <YYYY-MM-DD_HH:MM:SS>" (when the update replaced that
// state) and "changes <count>" (packages added or removed).
static const char* const kRestorePointFile = "restore-point";


struct Record {
	uint32_t	magic;
	uint32_t	version;
	uint32_t	size;
		// sizeof(Record) of the writer
	uint32_t	sequence;
	uint32_t	failedStarts;
		// starts the boot loader began that were not confirmed
	uint32_t	totalStarts;
	uint32_t	flags;
	uint32_t	reserved;
	char		booting[kNameLength];
		// package state directory the loader started, "" for the latest
	char		lastGood[kNameLength];
		// the state that last reached the desktop, "" for the latest
	uint32_t	lastGoodValid;
		// 0 until a start was confirmed once
	char		nextOnce[kNextLength];
		// for the next start only: "", "menu", "state:<directory>" or
		// "system:<name of another operating system in the menu>"
	uint32_t	checksum;
		// CRC-32 of everything before it
};


static inline uint32_t
crc32(const void* data, size_t length)
{
	const uint8_t* bytes = (const uint8_t*)data;
	uint32_t crc = 0xffffffff;
	for (size_t i = 0; i < length; i++) {
		crc ^= bytes[i];
		for (int bit = 0; bit < 8; bit++)
			crc = (crc >> 1) ^ (0xedb88320 & (0 - (crc & 1)));
	}
	return ~crc;
}


static inline void
init(Record& record)
{
	memset(&record, 0, sizeof(record));
	record.magic = kMagic;
	record.version = kVersion;
	record.size = sizeof(Record);
}


static inline void
seal(Record& record)
{
	record.magic = kMagic;
	record.version = kVersion;
	record.size = sizeof(Record);
	record.booting[kNameLength - 1] = '\0';
	record.lastGood[kNameLength - 1] = '\0';
	record.nextOnce[kNextLength - 1] = '\0';
	record.checksum = crc32(&record, offsetof(Record, checksum));
}


static inline bool
is_valid(const Record& record)
{
	return record.magic == kMagic && record.version >= 1
		&& record.size >= sizeof(Record)
		&& record.checksum == crc32(&record, offsetof(Record, checksum))
		&& record.booting[kNameLength - 1] == '\0'
		&& record.lastGood[kNameLength - 1] == '\0'
		&& record.nextOnce[kNextLength - 1] == '\0';
}


/*!	Picks the newest valid copy of the \a fileSize bytes in \a file into
	\a record. Returns its slot, or -1 if there is none (then \a record is
	a fresh, empty one). Sequence numbers are compared as a signed
	difference, so they may wrap.
*/
static inline int
newest(const void* file, size_t fileSize, Record& record)
{
	int best = -1;
	Record copy;
	for (size_t slot = 0; slot < kSlotCount; slot++) {
		if ((slot + 1) * kSlotSize > fileSize)
			break;
		memcpy(&copy, (const uint8_t*)file + slot * kSlotSize, sizeof(copy));
		if (!is_valid(copy))
			continue;
		if (best < 0 || (int32_t)(copy.sequence - record.sequence) > 0) {
			record = copy;
			best = (int)slot;
		}
	}
	if (best < 0)
		init(record);
	return best;
}


/*!	The slot the next write goes to when the newest copy is in \a slot
	(-1: none).
*/
static inline int
next_slot(int slot)
{
	return slot < 0 ? 0 : (slot + 1) % (int)kSlotCount;
}


/*!	Fills \a buffer (kSlotSize bytes) with \a record as the copy that
	follows the newest one: sequence + 1, sealed, zero padding.
*/
static inline void
prepare_write(Record& record, void* buffer)
{
	record.sequence++;
	seal(record);
	memset(buffer, 0, kSlotSize);
	memcpy(buffer, &record, sizeof(record));
}


static inline void
set_name(char* target, size_t size, const char* name)
{
	// by hand: the boot loader's libroot has no strncpy()
	memset(target, 0, size);
	for (size_t i = 0; name != NULL && i + 1 < size && name[i] != '\0'; i++)
		target[i] = name[i];
}


/*!	The boot loader begins a start of the package state \a stateName (NULL
	or "" for the latest state).
*/
static inline void
start_attempt(Record& record, const char* stateName)
{
	record.failedStarts++;
	record.totalStarts++;
	set_name(record.booting, sizeof(record.booting), stateName);
	set_name(record.nextOnce, sizeof(record.nextOnce), NULL);
}


//!	The start reached the desktop.
static inline void
confirm(Record& record)
{
	record.failedStarts = 0;
	memcpy(record.lastGood, record.booting, sizeof(record.lastGood));
	record.lastGoodValid = 1;
}


/*!	package_daemon turned the latest state into the old state directory
	\a directoryName: what named the latest state now names that directory.
*/
static inline bool
latest_became(Record& record, const char* directoryName)
{
	bool changed = false;
	if (record.lastGoodValid && record.lastGood[0] == '\0') {
		set_name(record.lastGood, sizeof(record.lastGood), directoryName);
		changed = true;
	}
	if (record.booting[0] == '\0') {
		set_name(record.booting, sizeof(record.booting), directoryName);
		changed = true;
	}
	return changed;
}


static inline bool
needs_recovery(const Record& record)
{
	return record.failedStarts >= kFailedStartsForRecovery;
}


/*!	"20 November 2026, 14:05" from a package state directory name,
	"state_2026-11-20_14:05:33" (or a restore point note's
	"2026-11-20_14:05:33"), into \a buffer. A "-<n>" suffix (two states in
	the same second) is kept as " (<n>)". Returns false, with \a buffer
	untouched, if \a name has no date.
*/
static inline bool
format_date(const char* name, char* buffer, size_t size)
{
	static const char* const kMonths[] = { "January", "February", "March",
		"April", "May", "June", "July", "August", "September", "October",
		"November", "December" };
	if (name == NULL)
		return false;
	if (strncmp(name, "state_", 6) == 0)
		name += 6;

	int field[6];
	const char separators[] = "--_::";
	const char* p = name;
	for (int i = 0; i < 6; i++) {
		int digits = i == 0 ? 4 : 2;
		int value = 0;
		for (int d = 0; d < digits; d++, p++) {
			if (*p < '0' || *p > '9')
				return false;
			value = value * 10 + (*p - '0');
		}
		field[i] = value;
		if (i < 5) {
			if (*p != separators[i])
				return false;
			p++;
		}
	}
	if (field[1] < 1 || field[1] > 12 || field[2] < 1 || field[2] > 31
		|| field[3] > 23 || field[4] > 59 || field[5] > 60)
		return false;

	int written = snprintf(buffer, size, "%d %s %d, %02d:%02d", field[2],
		kMonths[field[1] - 1], field[0], field[3], field[4]);
	if (*p == '-' && p[1] >= '0' && p[1] <= '9' && written > 0
		&& (size_t)written < size) {
		snprintf(buffer + written, size - written, " (%s)", p + 1);
	}
	return true;
}


/*!	The value of \a key in a restore point note (\a text, "key value"
	lines), into \a buffer. False if it has none.
*/
static inline bool
note_value(const char* text, const char* key, char* buffer, size_t size)
{
	size_t keyLength = strlen(key);
	for (const char* line = text; line != NULL && *line != '\0';) {
		if (strncmp(line, key, keyLength) == 0 && line[keyLength] == ' ') {
			const char* value = line + keyLength + 1;
			size_t length = 0;
			while (value[length] != '\0' && value[length] != '\n'
				&& value[length] != '\r')
				length++;
			if (length >= size)
				length = size - 1;
			memcpy(buffer, value, length);
			buffer[length] = '\0';
			return true;
		}
		line = strchr(line, '\n');
		if (line != NULL)
			line++;
	}
	return false;
}


/*!	The label the boot menu shows for an old package state: "Before the
	update of <date>" when its restore point \a note (may be NULL) says
	when an update replaced it, otherwise "System of <date>" from the
	directory \a name. Returns false if neither has a date.
*/
static inline bool
format_state_label(const char* name, const char* note, char* buffer,
	size_t size)
{
	char date[64];
	char replaced[32];
	if (note != NULL && note_value(note, "replaced", replaced,
			sizeof(replaced))
		&& format_date(replaced, date, sizeof(date))) {
		snprintf(buffer, size, "Before the update of %s", date);
		return true;
	}
	if (format_date(name, date, sizeof(date))) {
		snprintf(buffer, size, "System of %s", date);
		return true;
	}
	return false;
}


}	// namespace BootState


#endif	// _RENGA_BOOT_STATE_H
