/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 */


/*!	The boot loader's side of the boot state (Renga, roadmap F-20/F-22,
	boot loader batch A). See headers/private/shared/boot_state.h for the
	file and who writes it.

	At every start, before the menu is decided, loader_state_load() reads
	the newest valid copy from the boot volume and tells main() what it
	asks for: the recovery menu after two starts that never reached the
	desktop (unless "boot_auto_recovery false" is in the kernel settings
	file), or the one-shot choice "Restart into..." left. Right before the
	kernel starts, loader_state_record_start() writes the next copy into
	the other slot: one more unconfirmed start, the package state being
	started, and the one-shot choice cleared.

	Everything is logged with a "boot state:" prefix on the serial console,
	which the harness (--restore-points) and a hardware report read. A
	volume without the file (images before Renga batch 5, read-only media,
	another file system) has no boot counting; nothing is created here,
	since the loader cannot allocate blocks (Stream::WriteInPlace()).
*/


#include <boot/loader_state.h>

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <boot/vfs.h>
#include <boot/stdio.h>
#include <boot_state.h>
#include <driver_settings.h>

#include "package_support.h"


static BootState::Record sRecord;
static int sSlot = -1;
static bool sLoaded = false;
static Directory* sLoadedRoot = NULL;
static bool sAutoRecovery = true;
static char sNextOnce[BootState::kNextLength];
	// the one-shot choice this start acts on; cleared on disk at once


static bool
read_auto_recovery_setting(Directory* root)
{
	int fd = open_from(root, "home/config/settings/kernel/drivers/kernel",
		O_RDONLY);
	if (fd < 0)
		return true;
	void* settings = load_driver_settings_file(fd);
	close(fd);
	if (settings == NULL)
		return true;
	bool enabled = get_driver_boolean_parameter(settings,
		"boot_auto_recovery", true, true);
	unload_driver_settings(settings);
	return enabled;
}


/*!	Reads the boot state of the volume with the root \a root into sRecord.
	Returns false if it has none (or none that is valid).
*/
static bool
load_from(Directory* root)
{
	sLoaded = false;
	sSlot = -1;
	sLoadedRoot = root;
	sNextOnce[0] = '\0';
	BootState::init(sRecord);
	if (root == NULL)
		return false;

	int fd = open_from(root, BootState::kPath, O_RDONLY);
	if (fd < 0) {
		dprintf("boot state: none on this volume\n");
		return false;
	}

	struct stat st;
	if (fstat(fd, &st) != 0 || st.st_size != (off_t)BootState::kFileSize) {
		dprintf("boot state: unexpected size %" B_PRIdOFF ", ignored\n",
			(off_t)st.st_size);
		close(fd);
		return false;
	}

	uint8* file = (uint8*)malloc(BootState::kFileSize);
	if (file == NULL) {
		close(fd);
		return false;
	}
	ssize_t bytesRead = read_pos(fd, 0, file, BootState::kFileSize);
	close(fd);
	if (bytesRead != (ssize_t)BootState::kFileSize) {
		dprintf("boot state: cannot read it\n");
		free(file);
		return false;
	}

	sSlot = BootState::newest(file, BootState::kFileSize, sRecord);
	free(file);
	sLoaded = true;
		// also without a valid copy: the file is there and can be written

	if (sSlot < 0) {
		dprintf("boot state: no valid copy, starting afresh\n");
		return true;
	}

	dprintf("boot state: slot %d, sequence %" B_PRIu32 ", %" B_PRIu32
		" failed start(s), %" B_PRIu32 " start(s), last working \"%s\"%s\n",
		sSlot, sRecord.sequence, sRecord.failedStarts, sRecord.totalStarts,
		sRecord.lastGood[0] != '\0' ? sRecord.lastGood : "latest",
		sRecord.lastGoodValid ? "" : " (none confirmed yet)");
	if (sRecord.nextOnce[0] != '\0')
		dprintf("boot state: next start: \"%s\"\n", sRecord.nextOnce);
	return true;
}


/*!	Writes sRecord as the next copy (the slot the newest one is not in)
	of \a root's boot state. \a what is for the log.
*/
static bool
write_record(Directory* root, const char* what)
{
	int slot = BootState::next_slot(sSlot);
	uint8* buffer = (uint8*)malloc(BootState::kSlotSize);
	if (buffer == NULL)
		return false;
	BootState::prepare_write(sRecord, buffer);

	int fd = open_from(root, BootState::kPath, O_RDWR);
	ssize_t written = fd < 0 ? fd
		: write_pos(fd, slot * BootState::kSlotSize, buffer,
			BootState::kSlotSize);
	if (fd >= 0)
		close(fd);
	free(buffer);

	if (written != (ssize_t)BootState::kSlotSize) {
		dprintf("boot state: cannot write (%s): %s\n", what,
			strerror(written < 0 ? (status_t)written : B_IO_ERROR));
		return false;
	}
	sSlot = slot;
	return true;
}


loader_state_request
loader_state_load(BootVolume& volume)
{
	if (!volume.IsValid()) {
		load_from(NULL);
		return LOADER_STATE_NOTHING;
	}

	sAutoRecovery = read_auto_recovery_setting(volume.RootDirectory());
	if (!load_from(volume.RootDirectory()))
		return LOADER_STATE_NOTHING;

	// The one-shot choice is taken off the disk before it is acted on: an
	// operating system started from the menu never comes back here, and
	// the choice must not repeat at every start after it.
	strlcpy(sNextOnce, sRecord.nextOnce, sizeof(sNextOnce));
	if (sNextOnce[0] != '\0') {
		BootState::set_name(sRecord.nextOnce, sizeof(sRecord.nextOnce),
			NULL);
		if (write_record(volume.RootDirectory(), "one-shot choice"))
			dprintf("boot state: one-shot choice taken\n");
	}

	if (strcmp(sNextOnce, "menu") == 0)
		return LOADER_STATE_MENU;
	if (strncmp(sNextOnce, "system:", 7) == 0)
		return LOADER_STATE_SYSTEM;
	if (strncmp(sNextOnce, "state:", 6) == 0)
		return LOADER_STATE_STATE;

	if (loader_state_needs_recovery())
		return LOADER_STATE_RECOVERY;
	if (BootState::needs_recovery(sRecord)) {
		dprintf("boot state: %" B_PRIu32 " failed starts, automatic "
			"recovery is off\n", sRecord.failedStarts);
	}
	return LOADER_STATE_NOTHING;
}


bool
loader_state_needs_recovery()
{
	return sLoaded && sAutoRecovery && BootState::needs_recovery(sRecord);
}


uint32
loader_state_failed_starts()
{
	return sLoaded ? sRecord.failedStarts : 0;
}


/*!	The package state that last reached the desktop, among \a volume's
	states; NULL if none was confirmed, if it is the latest state, or if
	it is gone (package_daemon prunes old states).
*/
PackageVolumeState*
loader_state_last_good(BootVolume& volume)
{
	if (!sLoaded || !sRecord.lastGoodValid || sRecord.lastGood[0] == '\0'
		|| !volume.IsValid() || volume.GetPackageVolumeInfo() == NULL)
		return NULL;

	PackageVolumeInfo* info = volume.GetPackageVolumeInfo();
	if (info->LoadOldStates() != B_OK)
		return NULL;
	for (PackageVolumeStateList::ConstIterator it = info->States().GetIterator();
			PackageVolumeState* state = it.Next();) {
		if (state->Name() != NULL
			&& strcmp(state->Name(), sRecord.lastGood) == 0)
			return state;
	}
	dprintf("boot state: last working state \"%s\" is gone\n",
		sRecord.lastGood);
	return NULL;
}


bool
loader_state_next_system(char* name, size_t size)
{
	if (strncmp(sNextOnce, "system:", 7) != 0)
		return false;
	strlcpy(name, sNextOnce + 7, size);
	return name[0] != '\0';
}


status_t
loader_state_select(BootVolume& volume, PackageVolumeState* state)
{
	if (!volume.IsValid() || volume.GetPackageVolumeInfo() == NULL)
		return B_BAD_VALUE;
	dprintf("boot state: starting \"%s\"\n",
		state != NULL && state->Name() != NULL ? state->Name() : "latest");
	// SetTo() unsets the volume first: keep what it is set to again
	BReference<PackageVolumeInfo> info(volume.GetPackageVolumeInfo());
	Directory* root = volume.RootDirectory();
	root->Acquire();
	status_t status = volume.SetTo(root, info, state);
	root->Release();
	return status;
}


//!	"Restart into..." an older state ("state:<directory>").
status_t
loader_state_apply_next_state(BootVolume& volume)
{
	if (strncmp(sNextOnce, "state:", 6) != 0
		|| !volume.IsValid() || volume.GetPackageVolumeInfo() == NULL)
		return B_BAD_VALUE;

	const char* wanted = sNextOnce + 6;
	PackageVolumeInfo* info = volume.GetPackageVolumeInfo();
	if (info->LoadOldStates() != B_OK)
		return B_ERROR;
	for (PackageVolumeStateList::ConstIterator it = info->States().GetIterator();
			PackageVolumeState* state = it.Next();) {
		if (state->Name() != NULL && strcmp(state->Name(), wanted) == 0)
			return loader_state_select(volume, state);
	}
	dprintf("boot state: next start's state \"%s\" is gone\n", wanted);
	return B_ENTRY_NOT_FOUND;
}


void
loader_state_record_start(BootVolume& volume)
{
	if (!volume.IsValid())
		return;
	if (sLoadedRoot != volume.RootDirectory()) {
		// another volume was chosen in the menu: count its starts
		load_from(volume.RootDirectory());
	}
	if (!sLoaded)
		return;

	PackageVolumeState* state = volume.GetPackageVolumeState();
	BootState::start_attempt(sRecord, state != NULL ? state->Name() : NULL);
	if (!write_record(volume.RootDirectory(), "start"))
		return;
	dprintf("boot state: start %" B_PRIu32 " of \"%s\" recorded (slot %d, "
		"sequence %" B_PRIu32 ", %" B_PRIu32 " unconfirmed)\n",
		sRecord.totalStarts,
		sRecord.booting[0] != '\0' ? sRecord.booting : "latest", sSlot,
		sRecord.sequence, sRecord.failedStarts);
}
