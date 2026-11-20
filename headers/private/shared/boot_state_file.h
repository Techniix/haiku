/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 */
#ifndef _RENGA_BOOT_STATE_FILE_H
#define _RENGA_BOOT_STATE_FILE_H


/*!	Reading and writing the boot state file from a running system (the
	bootstate tool, package_daemon, the Boot menu preferences, the tests);
	the boot loader has its own (src/system/boot/loader/loader_state.cpp).
	See boot_state.h for the format.
*/


#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

#include <FindDirectory.h>
#include <SupportDefs.h>

#include <boot_state.h>


namespace BootState {


//!	/boot/system/settings/boot/state (kPath on the boot volume).
static inline void
file_path(char* path, size_t size)
{
	char settings[B_PATH_NAME_LENGTH];
	if (find_directory(B_SYSTEM_SETTINGS_DIRECTORY, -1, false, settings,
			sizeof(settings)) != B_OK)
		strlcpy(settings, "/boot/system/settings", sizeof(settings));
	snprintf(path, size, "%s/boot/state", settings);
}


//!	/boot/system/packages/administrative, into \a path.
static inline void
administrative_path(char* path, size_t size)
{
	char packages[B_PATH_NAME_LENGTH];
	if (find_directory(B_SYSTEM_PACKAGES_DIRECTORY, -1, false, packages,
			sizeof(packages)) != B_OK)
		strlcpy(packages, "/boot/system/packages", sizeof(packages));
	snprintf(path, size, "%s/administrative", packages);
}


/*!	Reads the newest copy of \a path into \a record. Returns its slot, -1
	if no copy is valid, or -2 if the file is missing or has another size
	(then it cannot be written in place).
*/
static inline int
read_file(const char* path, Record& record)
{
	init(record);
	int fd = open(path, O_RDONLY);
	if (fd < 0)
		return -2;
	struct stat st;
	uint8_t* file = (uint8_t*)malloc(kFileSize);
	if (file == NULL || fstat(fd, &st) != 0
		|| st.st_size != (off_t)kFileSize
		|| pread(fd, file, kFileSize, 0) != (ssize_t)kFileSize) {
		free(file);
		close(fd);
		return -2;
	}
	close(fd);
	int slot = newest(file, kFileSize, record);
	free(file);
	return slot;
}


/*!	Writes \a record as the copy after the one in \a newestSlot, in place
	(the file keeps its size and blocks), and syncs it.
*/
static inline status_t
write_file(const char* path, Record& record, int newestSlot)
{
	uint8_t buffer[kSlotSize];
	prepare_write(record, buffer);
	int slot = next_slot(newestSlot);

	int fd = open(path, O_WRONLY);
	if (fd < 0)
		return errno;
	ssize_t written = pwrite(fd, buffer, sizeof(buffer), slot * kSlotSize);
	status_t status = written == (ssize_t)sizeof(buffer) ? B_OK
		: written < 0 ? errno : B_IO_ERROR;
	if (status == B_OK && fsync(fd) != 0)
		status = errno;
	close(fd);
	return status;
}


static inline int
compare_state_names(const void* a, const void* b)
{
	// newest first: the names sort by date
	return -strcmp((const char*)a, (const char*)b);
}


/*!	The old package states of the boot volume's system packages (restore
	points), newest first, into \a names (at most \a max). Returns the
	count.
*/
static inline int
list_restore_points(char (*names)[kNameLength], int max)
{
	char administrative[B_PATH_NAME_LENGTH];
	administrative_path(administrative, sizeof(administrative));

	DIR* dir = opendir(administrative);
	if (dir == NULL)
		return 0;
	int count = 0;
	while (dirent* entry = readdir(dir)) {
		if (strncmp(entry->d_name, "state_", 6) == 0 && count < max
			&& strlen(entry->d_name) < kNameLength)
			strlcpy(names[count++], entry->d_name, kNameLength);
	}
	closedir(dir);
	qsort(names, count, kNameLength, compare_state_names);
	return count;
}


/*!	The label of the restore point \a name ("Before the update of ...",
	see format_state_label()), into \a label.
*/
static inline void
restore_point_label(const char* name, char* label, size_t size)
{
	char administrative[B_PATH_NAME_LENGTH];
	administrative_path(administrative, sizeof(administrative));
	char path[B_PATH_NAME_LENGTH];
	snprintf(path, sizeof(path), "%s/%s/%s", administrative, name,
		kRestorePointFile);
	char note[512] = "";
	int fd = open(path, O_RDONLY);
	if (fd >= 0) {
		ssize_t bytes = read(fd, note, sizeof(note) - 1);
		note[bytes > 0 ? bytes : 0] = '\0';
		close(fd);
	}
	if (!format_state_label(name, note, label, size))
		strlcpy(label, name, size);
}


}	// namespace BootState


#endif	// _RENGA_BOOT_STATE_FILE_H
