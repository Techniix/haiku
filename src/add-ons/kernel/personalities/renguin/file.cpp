/*
 * Copyright 2026, Renguin contributors.
 * Distributed under the terms of the MIT License.
 *
 * Linux file and descriptor system calls.
 *
 * These wrap Haiku's VFS, translating Linux open/fcntl flags, struct stat,
 * struct flock, linux_dirent64 and *at() conventions; the FreeBSD
 * counterparts are in sys/compat/linux/linux_file.c and linux_stats.c.
 * Linux and Haiku both use AT_FDCWD == -100, so directory FDs pass through.
 *
 * Haiku's _user_* calls accept the Linux program's own buffers directly.
 * Where they need a pointer the program did not supply (a translated path, a
 * size_t*, a converted struct), it is placed in user scratch space; see
 * usermem.cpp. Kernel-side calls that take kernel buffers (vfs_read_stat()
 * with kernel = false) are used where Haiku exports them.
 *
 * Absolute paths are first looked up in the Linux root; see path.cpp.
 */


#include "renguin.h"
#include "linux_defs.h"

#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include <KernelExport.h>
#include <NodeMonitor.h>
#include <fs_info.h>

#include <AutoDeleter.h>

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <kernel.h>
#include <syscalls.h>
#include <team.h>
#include <thread.h>
#include <vfs.h>
#include <fs/fd.h>


static int
open_flags_to_haiku(uint64 flags)
{
	int result = (int)(flags & LX_O_ACCMODE);	// RDONLY/WRONLY/RDWR match
	if ((flags & LX_O_PATH) == LX_O_PATH)
		result = O_RDONLY;		// Haiku has no O_PATH; a read-only open is
								// the closest thing for directories and
								// readable files
	if ((flags & LX_O_CREAT) != 0)
		result |= O_CREAT;
	if ((flags & LX_O_EXCL) != 0)
		result |= O_EXCL;
	if ((flags & LX_O_TRUNC) != 0)
		result |= O_TRUNC;
	if ((flags & LX_O_APPEND) != 0)
		result |= O_APPEND;
	if ((flags & LX_O_NONBLOCK) != 0)
		result |= O_NONBLOCK;
	if ((flags & LX_O_CLOEXEC) != 0)
		result |= O_CLOEXEC;
	if ((flags & LX_O_DIRECTORY) != 0)
		result |= O_DIRECTORY;
	if ((flags & LX_O_NOFOLLOW) != 0)
		result |= O_NOFOLLOW;
	if ((flags & LX_O_NOCTTY) != 0)
		result |= O_NOCTTY;
	if ((flags & LX_O_SYNC) == LX_O_SYNC)
		result |= O_SYNC;
	else if ((flags & LX_O_DSYNC) != 0)
		result |= O_DSYNC;
	if ((flags & LX_O_DIRECT) != 0)
		result |= O_DIRECT;
	// O_LARGEFILE and O_NOATIME have no meaning on Haiku.
	return result;
}


static uint64
open_flags_from_haiku(int flags)
{
	uint64 result = flags & O_ACCMODE;
	if ((flags & O_APPEND) != 0)
		result |= LX_O_APPEND;
	if ((flags & O_NONBLOCK) != 0)
		result |= LX_O_NONBLOCK;
	if ((flags & O_SYNC) != 0)
		result |= LX_O_SYNC;
	else if ((flags & O_DSYNC) != 0)
		result |= LX_O_DSYNC;
	if ((flags & O_DIRECT) != 0)
		result |= LX_O_DIRECT;
	return result | LX_O_LARGEFILE;
}


/*!	The access time Linux programs are shown for \a source.

	BFS stores no access time: it reports the current time as st_atime
	(bfs/kernel_interface.cpp), so a file always looks as if it had just
	been read, and an access time set with utimensat() is not kept. Linux
	programs that look at access times misbehave with "now": find -atime and
	-amin match nothing old, tmpreaper and systemd-tmpfiles never see a file
	as unused, shells and mail readers decide whether an mbox has new mail
	from mtime > atime, backup and sync tools see every file as changed.
	Renguin shows such files the way Linux shows them on a volume mounted
	with noatime: the access time equals the modification time. packagefs
	already does this for Haiku programs. Volumes that keep an access time
	(ramfs, the ext2/3/4, FAT and NTFS drivers...) report theirs unchanged.

	Keeping explicitly set access times in an attribute was the other
	option; it was not taken because every stat() would then have to open
	and read an attribute (ls -l of a large directory would cost several
	times as much), and native programs would still see another time.
	(Decision recorded in the roadmap's F-27 entry, 2026-11-14.)
*/
static bool
volume_stores_access_time(dev_t device)
{
	static struct {
		dev_t	device;
		bool	stores;
	} sCache[8];
	static int32 sNext = 0;

	for (int i = 0; i < 8; i++) {
		if (sCache[i].device == device && device != 0)
			return sCache[i].stores;
	}
	fs_info info;
	bool stores = true;
	if (_kern_read_fs_info(device, &info) == B_OK)
		stores = strcmp(info.fsh_name, "bfs") != 0;
	int slot = atomic_add(&sNext, 1) & 7;
	sCache[slot].device = device;
	sCache[slot].stores = stores;
	return stores;
}


void
renguin_linux_atime(const struct stat& source, struct timespec* _atime)
{
	*_atime = volume_stores_access_time(source.st_dev)
		? source.st_atim : source.st_mtim;
}


//! Translate a Haiku struct stat into the Linux x86-64 layout.
static void
stat_to_linux(const struct stat& source, linux_stat& dest)
{
	memset(&dest, 0, sizeof(dest));
	dest.st_dev = (uint32)source.st_dev;
	dest.st_ino = source.st_ino;
	dest.st_nlink = source.st_nlink;
	// The S_IF* type bits and permission bits match Linux. Drop Haiku's
	// attribute/index type bits, which Linux has no room for.
	dest.st_mode = source.st_mode & 0177777;
	dest.st_uid = source.st_uid;
	dest.st_gid = source.st_gid;
	dest.st_rdev = (uint32)source.st_rdev;
	dest.st_size = source.st_size;
	dest.st_blksize = source.st_blksize;
	dest.st_blocks = source.st_blocks;
	struct timespec atime;
	renguin_linux_atime(source, &atime);
	dest.st_atime_sec = atime.tv_sec;
	dest.st_atime_nsec = atime.tv_nsec;
	dest.st_mtime_sec = source.st_mtim.tv_sec;
	dest.st_mtime_nsec = source.st_mtim.tv_nsec;
	dest.st_ctime_sec = source.st_ctim.tv_sec;
	dest.st_ctime_nsec = source.st_ctim.tv_nsec;
}


static int64
copy_stat_out(const struct stat& st, uint64 statbuf)
{
	linux_stat out;
	stat_to_linux(st, out);
	if (renguin_copy_out(statbuf, &out, sizeof(out)) != B_OK)
		return -LX_EFAULT;
	return 0;
}


//! A kernel path buffer that frees itself.
struct PathBuffer {
	PathBuffer() : path((char*)malloc(B_PATH_NAME_LENGTH)) {}
	~PathBuffer() { free(path); }
	char* path;
};


/*!	For a path whose translation led into /proc (RENGUIN_STATUS_IN_PROC):
	the Linux /proc path, copied from the scratch copy at \a userCopy and
	normalized. 0, or a negative Linux errno.
*/
static int64
proc_path_from_scratch(uint64 userCopy, char* procPath, size_t procPathSize)
{
	PathBuffer linuxPath;
	if (linuxPath.path == NULL)
		return -LX_ENOMEM;
	if (user_strlcpy(linuxPath.path, (const char*)(addr_t)userCopy,
			B_PATH_NAME_LENGTH) < 0) {
		return -LX_EFAULT;
	}
	return renguin_proc_path(LX_AT_FDCWD, linuxPath.path, procPath,
		procPathSize) ? 0 : -LX_ENOENT;
}


uint32
renguin_current_umask()
{
	renguin_team* data = renguin_current_team();
	return data != NULL ? data->umask : 022;
}


// #pragma mark - I/O


int64
renguin_sys_read(int fd, uint64 buffer, uint64 count)
{
	if (renguin_is_signalfd(fd))
		return renguin_signalfd_read(fd, buffer, count);
	return renguin_encode(_user_read(fd, -1, (void*)(addr_t)buffer,
		(size_t)count));
}


int64
renguin_sys_write(int fd, uint64 buffer, uint64 count)
{
	bool handled;
	int64 result = renguin_netlink_write(fd, buffer, count, false, &handled);
	if (handled)
		return result;
	return renguin_encode(_user_write(fd, -1, (const void*)(addr_t)buffer,
		(size_t)count));
}


int64
renguin_sys_pread64(int fd, uint64 buffer, uint64 count, int64 offset)
{
	if (offset < 0)
		return -LX_EINVAL;
	return renguin_encode(_user_read(fd, offset, (void*)(addr_t)buffer,
		(size_t)count));
}


int64
renguin_sys_pwrite64(int fd, uint64 buffer, uint64 count, int64 offset)
{
	if (offset < 0)
		return -LX_EINVAL;
	return renguin_encode(_user_write(fd, offset, (const void*)(addr_t)buffer,
		(size_t)count));
}


//! Linux readv/writev: loop over the iovec in user memory, one _user_* call
//! each. Partial progress is returned, matching Linux.
static int64
do_vector_io(int fd, uint64 iov, uint64 count, bool write, off_t position = -1)
{
	if ((int64)count < 0 || count > 1024)	// UIO_MAXIOV
		return -LX_EINVAL;

	int64 total = 0;
	for (uint64 i = 0; i < count; i++) {
		linux_iovec vec;
		if (renguin_copy_in(&vec, iov + i * sizeof(linux_iovec), sizeof(vec))
				!= B_OK) {
			return total > 0 ? total : -LX_EFAULT;
		}
		if (vec.iov_len == 0)
			continue;

		ssize_t done = write
			? _user_write(fd, position, (const void*)(addr_t)vec.iov_base,
				vec.iov_len)
			: _user_read(fd, position, (void*)(addr_t)vec.iov_base,
				vec.iov_len);
		if (done < 0) {
			if (total > 0) {
				renguin_cancel_restart();
				return total;
			}
			return renguin_encode(done);
		}
		total += done;
		if (position >= 0)
			position += done;
		if ((uint64)done < vec.iov_len)
			break;		// short read/write ends the sequence
	}
	return total;
}


/*!	preadv(), pwritev() and their "2" variants (glibc's and musl's
	pwritev() use pwritev2 when available). An offset of -1 in the "2"
	variants means the current file position. RWF_* flags (hints such as
	RWF_HIPRI, RWF_DSYNC) are accepted; RWF_NOWAIT and RWF_APPEND are not
	supported (EOPNOTSUPP), as on file systems without them.
*/
int64
renguin_sys_preadv(int fd, uint64 iov, uint64 count, int64 position,
	uint64 flags, bool write, bool variant2)
{
	const uint64 kNoWait = 0x8, kAppend = 0x10;
	if ((flags & (kNoWait | kAppend)) != 0)
		return -LX_EOPNOTSUPP;
	if (position < 0 && !(variant2 && position == -1))
		return -LX_EINVAL;
	if (position >= 0) {
		// Positioned I/O on a pipe or socket is ESPIPE on Linux.
		struct stat st;
		if (vfs_read_stat(fd, NULL, false, &st, false) == B_OK
			&& (S_ISFIFO(st.st_mode) || S_ISSOCK(st.st_mode))) {
			return -LX_ESPIPE;
		}
	}
	return do_vector_io(fd, iov, count, write, position);
}


int64
renguin_sys_writev(int fd, uint64 iov, uint64 count)
{
	bool handled;
	int64 result = renguin_netlink_write(fd, iov, count, true, &handled);
	if (handled)
		return result;
	return do_vector_io(fd, iov, count, true);
}


int64
renguin_sys_readv(int fd, uint64 iov, uint64 count)
{
	return do_vector_io(fd, iov, count, false);
}


// #pragma mark - Opening and closing


int64
renguin_sys_openat(int dirfd, uint64 path, uint64 flags, uint64 mode)
{
	if ((flags & LX_O_TMPFILE) == LX_O_TMPFILE)
		return -LX_EOPNOTSUPP;

	char procPath[RENGUIN_PROC_PATH_MAX];
	int64 isProc = renguin_user_proc_path(dirfd, path, procPath,
		sizeof(procPath));
	if (isProc < 0)
		return isProc;
	if (isProc > 0)
		return renguin_proc_open(procPath, flags);

	uint32 pathFlags = 0;
	if ((flags & LX_O_CREAT) != 0)
		pathFlags |= RENGUIN_PATH_MAY_CREATE;
	if ((flags & LX_O_NOFOLLOW) == 0)
		pathFlags |= RENGUIN_PATH_FOLLOW_LEAF;

	renguin_scratch scratch;
	renguin_scratch_init(&scratch);
	status_t status;
	uint64 hostPath = renguin_user_path(&scratch, path, pathFlags, &status);
	if (status == RENGUIN_STATUS_IN_PROC) {
		int64 result = proc_path_from_scratch(hostPath, procPath,
			sizeof(procPath));
		return result != 0 ? result : renguin_proc_open(procPath, flags);
	}
	if (status != B_OK)
		return renguin_encode(status);

	int fd = _user_open(dirfd, (const char*)(addr_t)hostPath,
		open_flags_to_haiku(flags),
		(int)(mode & 07777 & ~renguin_current_umask()));
	if (fd < 0 && (flags & (LX_O_CREAT | LX_O_ACCMODE)) == 0) {
		// Some file systems may refuse open() on a directory outright. A
		// read-only open of a directory is always an open_dir() for us.
		int dirFD = _user_open_dir(dirfd, (const char*)(addr_t)hostPath);
		if (dirFD >= 0) {
			if ((flags & LX_O_CLOEXEC) != 0)
				_user_fcntl(dirFD, F_SETFD, FD_CLOEXEC);
			return dirFD;
		}
	}
	if (fd < 0)
		return renguin_encode(fd);

	// On Linux, any FD opened on a directory can be read with getdents64:
	// that is how glibc's and musl's opendir() work. A Haiku FD opened with
	// open() on a directory is a file FD without a read_dir hook. Turn it into
	// a directory FD with the same number, exactly as Haiku's own fdopendir()
	// does (src/system/libroot/posix/dirent.cpp).
	struct stat st;
	if ((flags & LX_O_CREAT) == 0
		&& vfs_read_stat(fd, NULL, false, &st, false) == B_OK
		&& S_ISDIR(st.st_mode)) {
		int dirFD = _user_open_dir(fd, NULL);
		if (dirFD < 0) {
			_user_close(fd);
			return renguin_encode(dirFD);
		}
		int result = _user_dup2(dirFD, fd,
			(flags & LX_O_CLOEXEC) != 0 ? O_CLOEXEC : 0);
		_user_close(dirFD);
		if (result < 0) {
			_user_close(fd);
			return renguin_encode(result);
		}
	}
	return fd;
}


int64
renguin_sys_close(int fd)
{
	status_t status = _user_close(fd);
	if (status == B_OK)
		renguin_proc_closed(fd);
	return renguin_encode(status);
}


int64
renguin_sys_lseek(int fd, int64 offset, int whence)
{
	// Linux SEEK_SET/CUR/END/DATA/HOLE (0-4) match Haiku's.
	if (whence < 0 || whence > 4)
		return -LX_EINVAL;
	int64 procResult;
	if (renguin_proc_lseek(fd, offset, whence, &procResult))
		return procResult;
	return renguin_encode(_user_seek(fd, offset, whence));
}


// #pragma mark - stat


int64
renguin_sys_fstat(int fd, uint64 statbuf)
{
	struct stat st;
	if (renguin_proc_fstat(fd, &st))
		return copy_stat_out(st, statbuf);
	status_t status = vfs_read_stat(fd, NULL, false, &st, false);
	if (status != B_OK)
		return renguin_encode(status);
	return copy_stat_out(st, statbuf);
}


/*!	stat() of a path relative to \a dirfd, with the *at() flags
	AT_SYMLINK_NOFOLLOW and AT_EMPTY_PATH (and AT_NO_AUTOMOUNT, ignored).
	Shared by newfstatat and statx. Returns 0 or a negative Linux errno.
*/
int64
renguin_stat_at(int dirfd, uint64 path, uint64 flags, struct stat* st)
{
	bool traverse = (flags & LX_AT_SYMLINK_NOFOLLOW) == 0;

	char procPath[RENGUIN_PROC_PATH_MAX];
	int64 isProc = renguin_user_proc_path(dirfd, path, procPath,
		sizeof(procPath));
	if (isProc < 0)
		return isProc;
	if (isProc > 0)
		return renguin_proc_stat(procPath, traverse, st);

	PathBuffer buffer;
	if (buffer.path == NULL)
		return -LX_ENOMEM;

	status_t status = renguin_get_user_path(path,
		traverse ? RENGUIN_PATH_FOLLOW_LEAF : 0, buffer.path,
		B_PATH_NAME_LENGTH);
	if (status == RENGUIN_STATUS_IN_PROC) {
		if (!renguin_proc_path(LX_AT_FDCWD, buffer.path, procPath,
				sizeof(procPath))) {
			return -LX_ENOENT;
		}
		return renguin_proc_stat(procPath, traverse, st);
	}
	if (status != B_OK)
		return renguin_encode(status);

	if (buffer.path[0] == '\0') {
		// fstat(dirfd), or stat(".") for AT_FDCWD.
		if ((flags & LX_AT_EMPTY_PATH) == 0)
			return -LX_ENOENT;
		if (renguin_proc_fstat(dirfd, st))
			return 0;
		if (dirfd == LX_AT_FDCWD)
			status = vfs_read_stat(dirfd, ".", traverse, st, false);
		else
			status = vfs_read_stat(dirfd, NULL, traverse, st, false);
	} else {
		status = vfs_read_stat(dirfd, buffer.path, traverse, st, false);
	}
	return status != B_OK ? renguin_encode(status) : 0;
}


int64
renguin_sys_newfstatat(int dirfd, uint64 path, uint64 statbuf, uint64 flags)
{
	if ((flags & ~(uint64)(LX_AT_SYMLINK_NOFOLLOW | LX_AT_EMPTY_PATH
			| LX_AT_NO_AUTOMOUNT)) != 0) {
		return -LX_EINVAL;
	}

	struct stat st;
	int64 result = renguin_stat_at(dirfd, path, flags, &st);
	if (result != 0)
		return result;
	return copy_stat_out(st, statbuf);
}


// #pragma mark - Links and access


int64
renguin_sys_readlinkat(int dirfd, uint64 path, uint64 buffer, uint64 size)
{
	if ((int64)size <= 0)
		return -LX_EINVAL;

	PathBuffer kernelPath;
	if (kernelPath.path == NULL)
		return -LX_ENOMEM;
	ssize_t length = renguin_copy_string_in(kernelPath.path, path,
		B_PATH_NAME_LENGTH);
	if (length < 0)
		return renguin_encode(length);

	char procPath[RENGUIN_PROC_PATH_MAX];
	if (renguin_proc_path(dirfd, kernelPath.path, procPath, sizeof(procPath))) {
		int64 length = renguin_proc_readlink(procPath, kernelPath.path,
			B_PATH_NAME_LENGTH);
		if (length < 0)
			return length;
		size_t toCopy = (uint64)length < size ? (size_t)length : (size_t)size;
		if (renguin_copy_out(buffer, kernelPath.path, toCopy) != B_OK)
			return -LX_EFAULT;
		return toCopy;
	}

	status_t status = renguin_get_user_path(path, 0, kernelPath.path,
		B_PATH_NAME_LENGTH);
	if (status == RENGUIN_STATUS_IN_PROC) {
		if (!renguin_proc_path(LX_AT_FDCWD, kernelPath.path, procPath,
				sizeof(procPath))) {
			return -LX_ENOENT;
		}
		int64 length = renguin_proc_readlink(procPath, kernelPath.path,
			B_PATH_NAME_LENGTH);
		if (length < 0)
			return length;
		size_t toCopy = (uint64)length < size ? (size_t)length : (size_t)size;
		if (renguin_copy_out(buffer, kernelPath.path, toCopy) != B_OK)
			return -LX_EFAULT;
		return toCopy;
	}
	if (status != B_OK)
		return renguin_encode(status);

	// _user_read_link() wants the path, the buffer and the size in user
	// memory. Read into scratch space, then copy what fits: Linux truncates
	// silently and does not NUL-terminate.
	renguin_scratch scratch;
	renguin_scratch_init(&scratch);
	size_t linkBufferSize = B_PATH_NAME_LENGTH;
	uint64 userPath = renguin_scratch_put(&scratch, kernelPath.path,
		strlen(kernelPath.path) + 1);
	uint64 userSize = renguin_scratch_put(&scratch, &linkBufferSize,
		sizeof(linkBufferSize));
	uint64 userLink = renguin_scratch_alloc(&scratch, linkBufferSize);
	if (userPath == 0 || userSize == 0 || userLink == 0)
		return -LX_EFAULT;

	status = _user_read_link(dirfd, (const char*)(addr_t)userPath,
		(char*)(addr_t)userLink, (size_t*)(addr_t)userSize);
	if (status != B_OK)
		return status == B_BAD_VALUE ? -LX_EINVAL : renguin_encode(status);

	size_t linkLength;
	if (user_memcpy(&linkLength, (void*)(addr_t)userSize, sizeof(linkLength))
			!= B_OK) {
		return -LX_EFAULT;
	}
	if (linkLength > B_PATH_NAME_LENGTH)
		linkLength = B_PATH_NAME_LENGTH;
	size_t toCopy = linkLength < size ? linkLength : (size_t)size;
	if (user_memcpy(kernelPath.path, (void*)(addr_t)userLink, toCopy) != B_OK
		|| renguin_copy_out(buffer, kernelPath.path, toCopy) != B_OK) {
		return -LX_EFAULT;
	}
	return toCopy;
}


int64
renguin_sys_faccessat(int dirfd, uint64 path, uint64 mode, uint64 flags)
{
	// R_OK/W_OK/X_OK/F_OK have the same values on both systems.
	if ((mode & ~(uint64)7) != 0
		|| (flags & ~(uint64)(LX_AT_EACCESS | LX_AT_SYMLINK_NOFOLLOW
			| LX_AT_EMPTY_PATH)) != 0) {
		return -LX_EINVAL;
	}

	char procPath[RENGUIN_PROC_PATH_MAX];
	int64 isProc = renguin_user_proc_path(dirfd, path, procPath,
		sizeof(procPath));
	if (isProc < 0)
		return isProc;
	if (isProc > 0)
		return renguin_proc_access(procPath, (int)mode);

	renguin_scratch scratch;
	renguin_scratch_init(&scratch);
	status_t status;
	uint64 hostPath = renguin_user_path(&scratch, path,
		RENGUIN_PATH_FOLLOW_LEAF, &status);
	if (status == RENGUIN_STATUS_IN_PROC) {
		int64 result = proc_path_from_scratch(hostPath, procPath,
			sizeof(procPath));
		return result != 0 ? result : renguin_proc_access(procPath, (int)mode);
	}
	if (status != B_OK)
		return renguin_encode(status);

	return renguin_encode(_user_access(dirfd, (const char*)(addr_t)hostPath,
		(int)mode, (flags & LX_AT_EACCESS) != 0));
}


// #pragma mark - Directories


/*!	getdents64: Haiku's read_dir() fills the program's buffer with Haiku
	struct dirent records, which are larger than linux_dirent64 records for
	the same name (a 26-byte header versus 19). Each record is therefore
	converted in place, front to back; the write position never overtakes the
	read position. d_type is DT_UNKNOWN, which Linux allows and libc handles
	by calling stat().
*/
int64
renguin_sys_getdents64(int fd, uint64 buffer, uint64 count)
{
	if (count == 0)
		return -LX_EINVAL;
	if (!IS_USER_ADDRESS((addr_t)buffer)
		|| !IS_USER_ADDRESS((addr_t)(buffer + count - 1))) {
		return -LX_EFAULT;
	}
	if (count > 64 * 1024)
		count = 64 * 1024;

	int64 procResult;
	if (renguin_proc_getdents(fd, buffer, count, &procResult))
		return procResult;

	ssize_t entries = _user_read_dir(fd, (struct dirent*)(addr_t)buffer,
		(size_t)count, 0x7fffffff);
	if (entries < 0) {
		if (entries == B_BUFFER_OVERFLOW)
			return -LX_EINVAL;		// buffer too small for one entry
		return renguin_encode(entries);
	}

	uint8* record = (uint8*)malloc(sizeof(struct dirent) + B_FILE_NAME_LENGTH
		+ 8);
	if (record == NULL)
		return -LX_ENOMEM;
	MemoryDeleter recordDeleter(record);

	const size_t haikuHeader = offsetof(struct dirent, d_name);
	const size_t linuxHeader = offsetof(linux_dirent64, d_name);

	uint64 readPos = 0;
	uint64 writePos = 0;
	for (ssize_t i = 0; i < entries; i++) {
		// Read the Haiku record's header to learn its length.
		struct dirent header;
		if (readPos + haikuHeader > count
			|| user_memcpy(&header, (void*)(addr_t)(buffer + readPos),
				haikuHeader) != B_OK) {
			return -LX_EFAULT;
		}
		size_t haikuLength = header.d_reclen;
		if (haikuLength <= haikuHeader || readPos + haikuLength > count
			|| haikuLength > sizeof(struct dirent) + B_FILE_NAME_LENGTH) {
			break;
		}
		if (user_memcpy(record, (void*)(addr_t)(buffer + readPos),
				haikuLength) != B_OK) {
			return -LX_EFAULT;
		}
		const char* name = (const char*)record + haikuHeader;
		size_t nameLength = strnlen(name, haikuLength - haikuHeader);

		size_t linuxLength = (linuxHeader + nameLength + 1 + 7) & ~(size_t)7;
		uint8 linuxRecord[sizeof(linux_dirent64) + B_FILE_NAME_LENGTH + 8];
		if (linuxLength > sizeof(linuxRecord))
			break;
		memset(linuxRecord, 0, linuxLength);
		linux_dirent64* entry = (linux_dirent64*)linuxRecord;
		entry->d_ino = header.d_ino;
		entry->d_off = (int64)(i + 1);
		entry->d_reclen = (uint16)linuxLength;
		entry->d_type = LX_DT_UNKNOWN;
		memcpy(linuxRecord + linuxHeader, name, nameLength);

		if (user_memcpy((void*)(addr_t)(buffer + writePos), linuxRecord,
				linuxLength) != B_OK) {
			return -LX_EFAULT;
		}
		writePos += linuxLength;
		readPos += haikuLength;
	}
	return writePos;
}


int64
renguin_sys_getcwd(uint64 buffer, uint64 size)
{
	renguin_scratch scratch;
	renguin_scratch_init(&scratch);
	uint64 userCwd = renguin_scratch_alloc(&scratch, B_PATH_NAME_LENGTH);
	if (userCwd == 0)
		return -LX_EFAULT;

	status_t status = _user_getcwd((char*)(addr_t)userCwd, B_PATH_NAME_LENGTH);
	if (status != B_OK)
		return renguin_encode(status);

	PathBuffer cwd;
	if (cwd.path == NULL)
		return -LX_ENOMEM;
	if (user_strlcpy(cwd.path, (const char*)(addr_t)userCwd,
			B_PATH_NAME_LENGTH) < 0) {
		return -LX_EFAULT;
	}
	renguin_host_to_linux_path(cwd.path);

	// The raw syscall returns the length including the NUL.
	size_t length = strlen(cwd.path) + 1;
	if (length > size)
		return -LX_ERANGE;
	if (renguin_copy_out(buffer, cwd.path, length) != B_OK)
		return -LX_EFAULT;
	return length;
}


int64
renguin_sys_chdir(uint64 path)
{
	renguin_scratch scratch;
	renguin_scratch_init(&scratch);
	status_t status;
	uint64 hostPath = renguin_user_path(&scratch, path,
		RENGUIN_PATH_FOLLOW_LEAF, &status);
	if (status != B_OK)
		return renguin_encode(status);
	return renguin_encode(_user_setcwd(-1, (const char*)(addr_t)hostPath));
}


int64
renguin_sys_fchdir(int fd)
{
	return renguin_encode(_user_setcwd(fd, NULL));
}


/*!	The directory FD of a /proc directory is really open on a placeholder
	directory: nothing may be created or changed relative to it.
*/
bool
renguin_proc_dirfd_refused(int dirfd)
{
	return dirfd != LX_AT_FDCWD && renguin_proc_is_dir_fd(dirfd);
}


int64
renguin_sys_mkdirat(int dirfd, uint64 path, uint64 mode)
{
	if (renguin_proc_dirfd_refused(dirfd))
		return -LX_EACCES;
	renguin_scratch scratch;
	renguin_scratch_init(&scratch);
	status_t status;
	uint64 hostPath = renguin_user_path(&scratch, path,
		RENGUIN_PATH_MAY_CREATE, &status);
	if (status != B_OK)
		return renguin_encode(status);
	return renguin_encode(_user_create_dir(dirfd,
		(const char*)(addr_t)hostPath,
		(int)(mode & 07777 & ~renguin_current_umask())));
}


int64
renguin_sys_unlinkat(int dirfd, uint64 path, uint64 flags)
{
	if ((flags & ~(uint64)LX_AT_REMOVEDIR) != 0)
		return -LX_EINVAL;
	if (renguin_proc_dirfd_refused(dirfd))
		return -LX_EACCES;

	renguin_scratch scratch;
	renguin_scratch_init(&scratch);
	status_t status;
	uint64 hostPath = renguin_user_path(&scratch, path, 0, &status);
	if (status != B_OK)
		return renguin_encode(status);

	if ((flags & LX_AT_REMOVEDIR) != 0) {
		return renguin_encode(_user_remove_dir(dirfd,
			(const char*)(addr_t)hostPath));
	}
	return renguin_encode(_user_unlink(dirfd, (const char*)(addr_t)hostPath));
}


int64
renguin_sys_renameat(int olddirfd, uint64 oldpath, int newdirfd,
	uint64 newpath)
{
	if (renguin_proc_dirfd_refused(olddirfd)
		|| renguin_proc_dirfd_refused(newdirfd)) {
		return -LX_EACCES;
	}
	renguin_scratch scratch;
	renguin_scratch_init(&scratch);
	status_t status;
	uint64 hostOld = renguin_user_path(&scratch, oldpath, 0, &status);
	if (status != B_OK)
		return renguin_encode(status);
	uint64 hostNew = renguin_user_path(&scratch, newpath,
		RENGUIN_PATH_MAY_CREATE, &status);
	if (status != B_OK)
		return renguin_encode(status);
	return renguin_encode(_user_rename(olddirfd, (const char*)(addr_t)hostOld,
		newdirfd, (const char*)(addr_t)hostNew));
}


int64
renguin_sys_fsync(int fd)
{
	return renguin_encode(_user_fsync(fd, false));
}


int64
renguin_sys_ftruncate(int fd, int64 length)
{
	if (length < 0)
		return -LX_EINVAL;

	// Haiku truncates through write_stat(B_STAT_SIZE), as libroot's
	// ftruncate() does.
	struct stat st;
	memset(&st, 0, sizeof(st));
	st.st_size = length;

	renguin_scratch scratch;
	renguin_scratch_init(&scratch);
	uint64 userStat = renguin_scratch_put(&scratch, &st, sizeof(st));
	if (userStat == 0)
		return -LX_EFAULT;
	return renguin_encode(_user_write_stat(fd, NULL, false,
		(const struct stat*)(addr_t)userStat, sizeof(st), B_STAT_SIZE));
}


// #pragma mark - Descriptors


int64
renguin_sys_dup(int fd)
{
	int result = _user_dup(fd);
	if (result >= 0)
		renguin_proc_dup(fd, result);
	return renguin_encode(result);
}


int64
renguin_sys_dup3(int oldfd, int newfd, uint64 flags, bool isDup2)
{
	if (isDup2) {
		if (oldfd == newfd) {
			// dup2(fd, fd) checks fd and returns it.
			status_t status = _user_fcntl(oldfd, F_GETFD, 0);
			return status < 0 ? renguin_encode(status) : newfd;
		}
		flags = 0;
	} else {
		if (oldfd == newfd)
			return -LX_EINVAL;
		if ((flags & ~(uint64)LX_O_CLOEXEC) != 0)
			return -LX_EINVAL;
	}

	int haikuFlags = (flags & LX_O_CLOEXEC) != 0 ? O_CLOEXEC : 0;
	int result = _user_dup2(oldfd, newfd, haikuFlags);
	if (result >= 0)
		renguin_proc_dup(oldfd, result);
	return renguin_encode(result);
}


int64
renguin_sys_pipe2(uint64 fds, uint64 flags)
{
	if ((flags & ~(uint64)(LX_O_CLOEXEC | LX_O_NONBLOCK | LX_O_DIRECT)) != 0)
		return -LX_EINVAL;

	int haikuFlags = 0;
	if ((flags & LX_O_CLOEXEC) != 0)
		haikuFlags |= O_CLOEXEC;
	if ((flags & LX_O_NONBLOCK) != 0)
		haikuFlags |= O_NONBLOCK;
	// O_DIRECT (packet mode) is not supported; Haiku's FIFOs are streams.

	// The program's int[2] is passed straight through; _user_create_pipe()
	// checks it is a user address.
	return renguin_encode(_user_create_pipe((int*)(addr_t)fds, haikuFlags));
}


static int16
lock_type_to_haiku(int16 type)
{
	switch (type) {
		case LX_F_RDLCK:	return F_RDLCK;
		case LX_F_WRLCK:	return F_WRLCK;
		case LX_F_UNLCK:	return F_UNLCK;
		default:			return -1;
	}
}


static int16
lock_type_from_haiku(int16 type)
{
	switch (type) {
		case F_RDLCK:	return LX_F_RDLCK;
		case F_WRLCK:	return LX_F_WRLCK;
		default:		return LX_F_UNLCK;
	}
}


//! F_GETLK/F_SETLK/F_SETLKW. The struct layouts match; the l_type values do
//! not.
static int64
fcntl_lock(int fd, int haikuCommand, uint64 arg)
{
	linux_flock linuxLock;
	if (renguin_copy_in(&linuxLock, arg, sizeof(linuxLock)) != B_OK)
		return -LX_EFAULT;

	struct flock lock;
	memset(&lock, 0, sizeof(lock));
	lock.l_type = lock_type_to_haiku(linuxLock.l_type);
	if (lock.l_type < 0)
		return -LX_EINVAL;
	lock.l_whence = linuxLock.l_whence;
	lock.l_start = linuxLock.l_start;
	lock.l_len = linuxLock.l_len;
	lock.l_pid = linuxLock.l_pid;

	renguin_scratch scratch;
	renguin_scratch_init(&scratch);
	uint64 userLock = renguin_scratch_put(&scratch, &lock, sizeof(lock));
	if (userLock == 0)
		return -LX_EFAULT;

	status_t status = _user_fcntl(fd, haikuCommand, (size_t)userLock);
	if (status != B_OK)
		return renguin_encode(status);

	if (haikuCommand == F_GETLK) {
		if (user_memcpy(&lock, (void*)(addr_t)userLock, sizeof(lock)) != B_OK)
			return -LX_EFAULT;
		linuxLock.l_type = lock_type_from_haiku(lock.l_type);
		linuxLock.l_whence = lock.l_whence;
		linuxLock.l_start = lock.l_start;
		linuxLock.l_len = lock.l_len;
		linuxLock.l_pid = lock.l_pid;
		if (renguin_copy_out(arg, &linuxLock, sizeof(linuxLock)) != B_OK)
			return -LX_EFAULT;
	}
	return 0;
}


int64
renguin_sys_fcntl(int fd, uint64 cmd, uint64 arg)
{
	switch (cmd) {
		case LX_F_DUPFD:
		case LX_F_DUPFD_CLOEXEC:
		{
			int result = _user_fcntl(fd, cmd == LX_F_DUPFD
				? F_DUPFD : F_DUPFD_CLOEXEC, (size_t)arg);
			if (result >= 0)
				renguin_proc_dup(fd, result);
			return renguin_encode(result);
		}

		case LX_F_GETFD:
			// FD_CLOEXEC is 1 on both systems.
			return renguin_encode(_user_fcntl(fd, F_GETFD, 0));
		case LX_F_SETFD:
			return renguin_encode(_user_fcntl(fd, F_SETFD,
				(arg & LX_FD_CLOEXEC) != 0 ? FD_CLOEXEC : 0));

		case LX_F_GETFL:
		{
			status_t flags = _user_fcntl(fd, F_GETFL, 0);
			if (flags < 0)
				return renguin_encode(flags);
			return (int64)open_flags_from_haiku(flags);
		}
		case LX_F_SETFL:
		{
			// Only O_APPEND and O_NONBLOCK can change; Haiku ignores the
			// rest, as Linux does.
			int haikuFlags = 0;
			if ((arg & LX_O_APPEND) != 0)
				haikuFlags |= O_APPEND;
			if ((arg & LX_O_NONBLOCK) != 0)
				haikuFlags |= O_NONBLOCK;
			return renguin_encode(_user_fcntl(fd, F_SETFL, haikuFlags));
		}

		case LX_F_GETLK:
			return fcntl_lock(fd, F_GETLK, arg);
		case LX_F_SETLK:
			return fcntl_lock(fd, F_SETLK, arg);
		case LX_F_SETLKW:
			return fcntl_lock(fd, F_SETLKW, arg);

		// Linux's open file description locks lock like POSIX ones here;
		// the difference (owned by the description, not the process)
		// matters only to programs sharing a file between their threads.
		case LX_F_OFD_GETLK:
			return fcntl_lock(fd, F_GETLK, arg);
		case LX_F_OFD_SETLK:
			return fcntl_lock(fd, F_SETLK, arg);
		case LX_F_OFD_SETLKW:
			return fcntl_lock(fd, F_SETLKW, arg);

		case LX_F_ADD_SEALS:
			return renguin_fcntl_seals(fd, true, arg);
		case LX_F_GET_SEALS:
			return renguin_fcntl_seals(fd, false, 0);

		// Pipe sizes: Haiku's pipes have a fixed buffer; Linux's default
		// size is reported and requests are accepted.
		case LX_F_GETPIPE_SZ:
		case LX_F_SETPIPE_SZ:
		{
			status_t flags = _user_fcntl(fd, F_GETFL, 0);
			if (flags < 0)
				return renguin_encode(flags);
			return 65536;
		}

		// SIGIO ownership: accepted, never signalled.
		case LX_F_SETOWN:
		case LX_F_SETSIG:
			return 0;
		case LX_F_GETOWN:
		case LX_F_GETSIG:
			return 0;

		default:
			return -LX_EINVAL;
	}
}
