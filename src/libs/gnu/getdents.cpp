/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 *
 * getdents64() and getdents(): directory entries in the record layouts of
 * the Linux system calls, read through Haiku's _kern_read_dir().
 *
 * The kernel fills Haiku dirents into a scratch buffer of the caller's size;
 * each is then converted into the caller's buffer. A Linux record is never
 * longer than the Haiku record it comes from: the Haiku header is 26 bytes
 * plus the NUL-terminated name, so at least 27 + n bytes for a name of n
 * bytes; linux_dirent64 is 19 bytes plus the name and NUL rounded up to 8,
 * at most 27 + n; linux_dirent is 18 + n + 2 rounded up to 8, likewise. So
 * whatever the kernel returned also fits after conversion and no entry is
 * consumed without being returned.
 *
 * d_type: Haiku's dirent has no type, so it comes from fstatat() on the
 * entry (without following symbolic links), the same information a Linux
 * file system provides; DT_UNKNOWN if that fails (entry removed meanwhile).
 *
 * Descriptors from open() are turned into directory descriptors on first use
 * (see reopen_as_directory()).
 *
 * d_off: Linux returns a file system cookie that lseek() on the directory
 * accepts. Haiku directories cannot be positioned with lseek(), so d_off is
 * the 1-based index of the next entry within the returned buffer, only
 * meaningful as "not 0".
 */


#include <dirent.h>

#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <errno_private.h>
#include <syscalls.h>


namespace {

enum {
	kDT_UNKNOWN = 0,
	kDT_FIFO = 1,
	kDT_CHR = 2,
	kDT_DIR = 4,
	kDT_BLK = 6,
	kDT_REG = 8,
	kDT_LNK = 10,
	kDT_SOCK = 12,
};


unsigned char
entry_type(int fd, const char* name)
{
	struct stat st;
	if (fstatat(fd, name, &st, AT_SYMLINK_NOFOLLOW) != 0)
		return kDT_UNKNOWN;

	switch (st.st_mode & S_IFMT) {
		case S_IFIFO:	return kDT_FIFO;
		case S_IFCHR:	return kDT_CHR;
		case S_IFDIR:	return kDT_DIR;
		case S_IFBLK:	return kDT_BLK;
		case S_IFREG:	return kDT_REG;
		case S_IFLNK:	return kDT_LNK;
		case S_IFSOCK:	return kDT_SOCK;
		default:		return kDT_UNKNOWN;
	}
}


/*!	open() returns a file descriptor even for a directory, and Haiku's file
	descriptors cannot read directory entries (only those of opendir() /
	_kern_open_dir() can). Like fdopendir() in libroot, replace the
	descriptor with a directory descriptor for the same directory, at the
	same number, keeping its close-on-exec flag. Nothing was read from it
	before, so starting at the first entry is right.
*/
bool
reopen_as_directory(int fd)
{
	struct stat st;
	if (fstat(fd, &st) != 0 || !S_ISDIR(st.st_mode))
		return false;

	int descriptorFlags = fcntl(fd, F_GETFD);
	int directory = _kern_open_dir(fd, NULL);
	if (directory < 0)
		return false;

	bool replaced = dup2(directory, fd) == fd;
	close(directory);
	if (replaced && descriptorFlags >= 0)
		fcntl(fd, F_SETFD, descriptorFlags);
	return replaced;
}


size_t
align8(size_t size)
{
	return (size + 7) & ~(size_t)7;
}


ssize_t
read_entries(int fd, void* buffer, size_t length, bool large)
{
	if (buffer == NULL) {
		__set_errno(EFAULT);
		return -1;
	}

	size_t minimum = large
		? align8(offsetof(struct linux_dirent64, d_name) + 2)
		: align8(offsetof(struct linux_dirent, d_name) + 3);
	if (length < minimum) {
		__set_errno(EINVAL);
		return -1;
	}

	// The kernel cannot report more than 64 KiB per call anyway (d_reclen
	// is 16 bits); keep the scratch buffer bounded.
	if (length > 65536)
		length = 65536;

	char* scratch = (char*)malloc(length);
	if (scratch == NULL) {
		__set_errno(ENOMEM);
		return -1;
	}

	ssize_t count = _kern_read_dir(fd, (struct dirent*)scratch, length,
		INT32_MAX);
	if ((count == B_UNSUPPORTED || count == B_NOT_A_DIRECTORY)
		&& reopen_as_directory(fd)) {
		count = _kern_read_dir(fd, (struct dirent*)scratch, length,
			INT32_MAX);
	}
	if (count < 0) {
		free(scratch);
		if (count == B_BUFFER_OVERFLOW)
			count = EINVAL;
				// the first entry does not fit, as Linux reports it
		else if (count == B_BAD_VALUE || count == B_NOT_A_DIRECTORY
			|| count == B_UNSUPPORTED)
			count = ENOTDIR;
				// a file descriptor that is not a directory
		__set_errno(count);
		return -1;
	}

	char* out = (char*)buffer;
	size_t used = 0;
	const char* in = scratch;
	for (ssize_t i = 0; i < count; i++) {
		const struct dirent* entry = (const struct dirent*)in;
		size_t nameLength = strlen(entry->d_name);
		size_t recordLength = large
			? align8(offsetof(struct linux_dirent64, d_name) + nameLength + 1)
			: align8(offsetof(struct linux_dirent, d_name) + nameLength + 2);
		if (used + recordLength > length)
			break;
				// cannot happen (see the top), but never overrun

		unsigned char type = entry_type(fd, entry->d_name);
		if (large) {
			struct linux_dirent64 record;
			record.d_ino = entry->d_ino;
			record.d_off = i + 1;
			record.d_reclen = recordLength;
			record.d_type = type;
			char* target = out + used;
			memset(target, 0, recordLength);
			memcpy(target, &record, offsetof(struct linux_dirent64, d_name));
			memcpy(target + offsetof(struct linux_dirent64, d_name),
				entry->d_name, nameLength);
		} else {
			struct linux_dirent record;
			record.d_ino = entry->d_ino;
			record.d_off = i + 1;
			record.d_reclen = recordLength;
			char* target = out + used;
			memset(target, 0, recordLength);
			memcpy(target, &record, offsetof(struct linux_dirent, d_name));
			memcpy(target + offsetof(struct linux_dirent, d_name),
				entry->d_name, nameLength);
			target[recordLength - 1] = type;
		}

		used += recordLength;
		in += entry->d_reclen;
	}

	free(scratch);
	return used;
}

}	// namespace


extern "C" ssize_t
getdents64(int fd, void* buffer, size_t length)
{
	return read_entries(fd, buffer, length, true);
}


extern "C" ssize_t
getdents(int fd, void* buffer, size_t length)
{
	return read_entries(fd, buffer, length, false);
}
