/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 */


#include "ImageWriter.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <string.h>
#include <unistd.h>

#include <Drivers.h>
#include <fs_volume.h>
#include <DiskDevice.h>
#include <DiskDeviceRoster.h>
#include <Partition.h>


static const size_t kBufferSize = 1024 * 1024;


//! Unmounts \a partition and everything below it that is mounted.
static status_t
unmount_all(BPartition* partition, BString& error)
{
	for (int32 i = 0; i < partition->CountChildren(); i++) {
		status_t status = unmount_all(partition->ChildAt(i), error);
		if (status != B_OK)
			return status;
	}
	if (!partition->IsMounted())
		return B_OK;
	status_t status = partition->Unmount(B_FORCE_UNMOUNT);
	if (status != B_OK) {
		error.SetToFormat("A volume on the target (%s) could not be "
			"unmounted: %s.", partition->ContentName().String(),
			strerror(status));
	}
	return status;
}


static status_t
write_all(int fd, const char* buffer, size_t size)
{
	while (size > 0) {
		ssize_t written = write(fd, buffer, size);
		if (written < 0) {
			if (errno == EINTR)
				continue;
			return errno;
		}
		if (written == 0)
			return B_DEVICE_FULL;
		buffer += written;
		size -= written;
	}
	return B_OK;
}


static ssize_t
read_all(int fd, char* buffer, size_t size)
{
	size_t total = 0;
	while (total < size) {
		ssize_t bytes = read(fd, buffer + total, size - total);
		if (bytes < 0) {
			if (errno == EINTR)
				continue;
			return -errno;
		}
		if (bytes == 0)
			break;
		total += bytes;
	}
	return total;
}


status_t
write_disk_image(const char* sourcePath, const char* targetPath, bool verify,
	image_progress_hook progress, void* cookie, BString& error)
{
	// 1. nothing mounted on the target
	BDiskDevice device;
	BPartition* partition = NULL;
	if (BDiskDeviceRoster().GetPartitionForPath(targetPath, &device,
			&partition) == B_OK && partition != NULL) {
		status_t status = unmount_all(partition, error);
		if (status != B_OK)
			return status;
	}

	int source = open(sourcePath, O_RDONLY);
	if (source < 0) {
		error.SetToFormat("The image cannot be opened: %s.",
			strerror(errno));
		return errno;
	}
	struct stat sourceStat;
	fstat(source, &sourceStat);
	off_t total = sourceStat.st_size;

	int target = open(targetPath, O_RDWR);
	if (target < 0) {
		status_t status = errno;
		error.SetToFormat("The target cannot be opened: %s.",
			strerror(status));
		close(source);
		return status;
	}

	char* buffer = (char*)malloc(kBufferSize);
	char* check = (char*)malloc(kBufferSize);
	if (buffer == NULL || check == NULL) {
		free(buffer);
		free(check);
		close(source);
		close(target);
		error = "Not enough memory.";
		return B_NO_MEMORY;
	}

	// 2. every write checked
	status_t status = B_OK;
	off_t done = 0;
	while (status == B_OK && done < total) {
		ssize_t bytes = read_all(source, buffer, kBufferSize);
		if (bytes < 0) {
			status = bytes;
			error.SetToFormat("Reading the image failed at %lld bytes: %s.",
				(long long)done, strerror(status));
			break;
		}
		if (bytes == 0)
			break;
		status = write_all(target, buffer, bytes);
		if (status != B_OK) {
			error.SetToFormat("Writing to the target failed at %lld bytes: "
				"%s.", (long long)done, strerror(status));
			break;
		}
		done += bytes;
		if (progress != NULL)
			progress(cookie, false, done, total);
	}
	if (status == B_OK && done != total) {
		status = B_IO_ERROR;
		error.SetToFormat("Only %lld of %lld bytes of the image could be "
			"read.", (long long)done, (long long)total);
	}

	// 3. on the medium, not in a cache
	if (status == B_OK) {
		if (fsync(target) != 0) {
			status = errno;
			error.SetToFormat("Syncing the target failed: %s.",
				strerror(status));
		} else
			ioctl(target, B_FLUSH_DRIVE_CACHE, NULL, 0);
				// devices without a cache say no; that is fine
	}

	// 4. read back and compare
	if (status == B_OK && verify) {
		lseek(source, 0, SEEK_SET);
		lseek(target, 0, SEEK_SET);
		done = 0;
		while (done < total) {
			ssize_t expected = read_all(source, buffer, kBufferSize);
			if (expected <= 0)
				break;
			ssize_t got = read_all(target, check, expected);
			if (got != expected || memcmp(buffer, check, expected) != 0) {
				status = B_IO_ERROR;
				error.SetToFormat("The target does not hold the image: it "
					"differs at about %lld bytes (of %lld). The medium may be "
					"faulty or smaller than it claims.", (long long)done,
					(long long)total);
				break;
			}
			done += expected;
			if (progress != NULL)
				progress(cookie, true, done, total);
		}
	}

	free(buffer);
	free(check);
	close(source);
	close(target);
	return status;
}
