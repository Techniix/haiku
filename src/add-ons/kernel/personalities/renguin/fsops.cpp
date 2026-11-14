/*
 * Copyright 2026, Renguin contributors.
 * Distributed under the terms of the MIT License.
 *
 * More file system calls: statx, statfs, permissions and ownership, times,
 * links, special files, truncate, sendfile, flock, sync, advice and
 * extended attributes.
 *
 * FreeBSD counterparts: sys/compat/linux/linux_file.c, linux_stats.c and
 * linux_xattr.c. Haiku changes permissions, owners, sizes and times through
 * one call, write_stat() with a mask of what to change (libroot's chmod(),
 * chown(), truncate() and utimensat() are built on it), so the Linux calls
 * map onto _user_write_stat() with a struct stat in user scratch space.
 */


#include "renguin.h"
#include "linux_defs.h"

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include <KernelExport.h>
#include <NodeMonitor.h>
#include <OS.h>
#include <fs_info.h>

#include <fcntl.h>
#include <sys/stat.h>

#include <kernel.h>
#include <real_time_clock.h>
#include <syscalls.h>
#include <team.h>
#include <thread.h>
#include <vfs.h>


// *at() flags beyond those in linux_defs.h
#define LX_AT_SYMLINK_FOLLOW	0x400
#define LX_AT_STATX_SYNC_TYPE	0x6000

// statx (include/uapi/linux/stat.h)
#define LX_STATX_TYPE			0x0001
#define LX_STATX_MODE			0x0002
#define LX_STATX_NLINK			0x0004
#define LX_STATX_UID			0x0008
#define LX_STATX_GID			0x0010
#define LX_STATX_ATIME			0x0020
#define LX_STATX_MTIME			0x0040
#define LX_STATX_CTIME			0x0080
#define LX_STATX_INO			0x0100
#define LX_STATX_SIZE			0x0200
#define LX_STATX_BLOCKS			0x0400
#define LX_STATX_BASIC_STATS	0x07ff
#define LX_STATX_BTIME			0x0800
#define LX_STATX__RESERVED		0x80000000U

// utimensat
#define LX_UTIME_NOW			((1L << 30) - 1)
#define LX_UTIME_OMIT			((1L << 30) - 2)

// statfs f_flags
#define LX_ST_RDONLY			0x0001
#define LX_ST_VALID				0x0020

// fallocate
#define LX_POSIX_FADV_NOREUSE	5

#define LX_S_IFMT				0170000
#define LX_S_IFSOCK				0140000
#define LX_S_IFREG				0100000
#define LX_S_IFBLK				0060000
#define LX_S_IFDIR				0040000
#define LX_S_IFCHR				0020000
#define LX_S_IFIFO				0010000


struct linux_statx_timestamp {
	int64	tv_sec;
	uint32	tv_nsec;
	int32	reserved;
};

struct linux_statx {
	uint32	stx_mask;
	uint32	stx_blksize;
	uint64	stx_attributes;
	uint32	stx_nlink;
	uint32	stx_uid;
	uint32	stx_gid;
	uint16	stx_mode;
	uint16	spare0;
	uint64	stx_ino;
	uint64	stx_size;
	uint64	stx_blocks;
	uint64	stx_attributes_mask;
	linux_statx_timestamp stx_atime;
	linux_statx_timestamp stx_btime;
	linux_statx_timestamp stx_ctime;
	linux_statx_timestamp stx_mtime;
	uint32	stx_rdev_major;
	uint32	stx_rdev_minor;
	uint32	stx_dev_major;
	uint32	stx_dev_minor;
	uint64	stx_mnt_id;
	uint32	stx_dio_mem_align;
	uint32	stx_dio_offset_align;
	uint64	spare3[12];
};
static_assert(sizeof(linux_statx) == 256, "Linux ABI");
static_assert(offsetof(linux_statx, stx_atime) == 0x40, "Linux ABI");
static_assert(offsetof(linux_statx, stx_rdev_major) == 0x80, "Linux ABI");

struct linux_statfs {
	int64	f_type;
	int64	f_bsize;
	uint64	f_blocks;
	uint64	f_bfree;
	uint64	f_bavail;
	uint64	f_files;
	uint64	f_ffree;
	int32	f_fsid[2];
	int64	f_namelen;
	int64	f_frsize;
	int64	f_flags;
	int64	f_spare[4];
};
static_assert(sizeof(linux_statfs) == 120, "Linux ABI");


// #pragma mark - helpers


//! glibc's gnu_dev_major()/gnu_dev_minor(), so that makedev() of the two
//! gives back the st_dev that stat() reports.
static uint32
dev_major(uint64 device)
{
	return ((device >> 8) & 0xfff) | ((uint32)(device >> 32) & ~0xfffU);
}


static uint32
dev_minor(uint64 device)
{
	return (device & 0xff) | ((uint32)(device >> 12) & ~0xffU);
}


/*!	Changes what \a statMask selects of a path (or of \a fd if the path is
	empty and \a emptyPathOK) through _user_write_stat().
*/
static int64
write_stat_at(int fd, uint64 path, bool traverse, bool emptyPathOK,
	const struct stat& st, int statMask)
{
	if (renguin_proc_dirfd_refused(fd))
		return -LX_EACCES;
	renguin_scratch scratch;
	renguin_scratch_init(&scratch);
	uint64 userStat = renguin_scratch_put(&scratch, &st, sizeof(st));
	if (userStat == 0)
		return -LX_EFAULT;

	const char* hostPath = NULL;
	if (path != 0) {
		char first;
		if (renguin_copy_in(&first, path, 1) != B_OK)
			return -LX_EFAULT;
		if (first == '\0') {
			if (!emptyPathOK)
				return -LX_ENOENT;
		} else {
			status_t status;
			uint64 translated = renguin_user_path(&scratch, path,
				traverse ? RENGUIN_PATH_FOLLOW_LEAF : 0, &status);
			if (status != B_OK)
				return renguin_encode(status);
			hostPath = (const char*)(addr_t)translated;
		}
	}
	if (hostPath == NULL && fd == LX_AT_FDCWD) {
		// futimens(AT_FDCWD) and friends mean the current directory
		renguin_scratch_init(&scratch);
		userStat = renguin_scratch_put(&scratch, &st, sizeof(st));
		hostPath = (const char*)(addr_t)renguin_scratch_put(&scratch, ".", 2);
		if (userStat == 0 || hostPath == NULL)
			return -LX_EFAULT;
	}

	return renguin_encode(_user_write_stat(fd, hostPath, traverse,
		(const struct stat*)(addr_t)userStat, sizeof(st), statMask));
}


// #pragma mark - statx


int64
renguin_sys_statx(int dirfd, uint64 path, uint64 flags, uint64 mask,
	uint64 buffer)
{
	if ((mask & LX_STATX__RESERVED) != 0)
		return -LX_EINVAL;
	if ((flags & ~(uint64)(LX_AT_SYMLINK_NOFOLLOW | LX_AT_EMPTY_PATH
			| LX_AT_NO_AUTOMOUNT | LX_AT_STATX_SYNC_TYPE)) != 0
		|| (flags & LX_AT_STATX_SYNC_TYPE) == LX_AT_STATX_SYNC_TYPE) {
		return -LX_EINVAL;
	}

	struct stat st;
	int64 result = renguin_stat_at(dirfd, path,
		flags & (LX_AT_SYMLINK_NOFOLLOW | LX_AT_EMPTY_PATH), &st);
	if (result != 0)
		return result;

	linux_statx out;
	memset(&out, 0, sizeof(out));
	out.stx_mask = LX_STATX_BASIC_STATS | LX_STATX_BTIME;
	out.stx_blksize = st.st_blksize;
	out.stx_nlink = st.st_nlink;
	out.stx_uid = st.st_uid;
	out.stx_gid = st.st_gid;
	out.stx_mode = st.st_mode & 0177777;
	out.stx_ino = st.st_ino;
	out.stx_size = st.st_size;
	out.stx_blocks = st.st_blocks;
	struct timespec atime;
	renguin_linux_atime(st, &atime);
	out.stx_atime.tv_sec = atime.tv_sec;
	out.stx_atime.tv_nsec = atime.tv_nsec;
	out.stx_btime.tv_sec = st.st_crtim.tv_sec;
	out.stx_btime.tv_nsec = st.st_crtim.tv_nsec;
	out.stx_ctime.tv_sec = st.st_ctim.tv_sec;
	out.stx_ctime.tv_nsec = st.st_ctim.tv_nsec;
	out.stx_mtime.tv_sec = st.st_mtim.tv_sec;
	out.stx_mtime.tv_nsec = st.st_mtim.tv_nsec;
	// stat() reports the low 32 bits of the numbers (struct stat has
	// room for 64; keep the two calls consistent).
	out.stx_rdev_major = dev_major((uint32)st.st_rdev);
	out.stx_rdev_minor = dev_minor((uint32)st.st_rdev);
	out.stx_dev_major = dev_major((uint32)st.st_dev);
	out.stx_dev_minor = dev_minor((uint32)st.st_dev);

	if (renguin_copy_out(buffer, &out, sizeof(out)) != B_OK)
		return -LX_EFAULT;
	return 0;
}


// #pragma mark - statfs


static int64
fs_type_magic(const char* name)
{
	static const struct { const char* name; int64 magic; } kTypes[] = {
		{ "bfs", 0x42465331 },			// BEFS_SUPER_MAGIC
		{ "ext2", 0xef53 },				// EXT2/3/4_SUPER_MAGIC
		{ "ext3", 0xef53 },
		{ "ext4", 0xef53 },
		{ "fat", 0x4d44 },				// MSDOS_SUPER_MAGIC
		{ "ntfs", 0x5346544e },			// NTFS_SB_MAGIC
		{ "iso9660", 0x9660 },			// ISOFS_SUPER_MAGIC
		{ "udf", 0x15013346 },			// UDF_SUPER_MAGIC
		{ "nfs", 0x6969 },				// NFS_SUPER_MAGIC
		{ "nfs4", 0x6969 },
		{ "ramfs", 0x01021994 },		// TMPFS_MAGIC
		{ "devfs", 0x1373 },			// DEVFS_SUPER_MAGIC
		{ "rootfs", 0x01021994 },
		{ "btrfs", 0x9123683e },		// BTRFS_SUPER_MAGIC
		{ "xfs", 0x58465342 },			// XFS_SUPER_MAGIC
		{ "reiserfs", 0x52654973 },		// REISERFS_SUPER_MAGIC
	};
	for (size_t i = 0; i < B_COUNT_OF(kTypes); i++) {
		if (strcmp(name, kTypes[i].name) == 0)
			return kTypes[i].magic;
	}
	// packagefs and anything else: not a Linux file system type; Linux
	// tools print the number as unknown.
	int64 magic = 0;
	for (const char* p = name; *p != '\0' && p < name + 4; p++)
		magic = (magic << 8) | (uint8)*p;
	return magic;
}


static int64
statfs_device(dev_t device, uint64 buffer)
{
	fs_info info;
	status_t status = _kern_read_fs_info(device, &info);
	if (status != B_OK)
		return renguin_encode(status);

	linux_statfs out;
	memset(&out, 0, sizeof(out));
	out.f_type = fs_type_magic(info.fsh_name);
	out.f_bsize = info.block_size;
	out.f_frsize = info.block_size;
	out.f_blocks = info.total_blocks;
	out.f_bfree = info.free_blocks;
	out.f_bavail = info.free_blocks;
	out.f_files = info.total_nodes;
	out.f_ffree = info.free_nodes;
	out.f_fsid[0] = (int32)device;
	out.f_namelen = B_FILE_NAME_LENGTH - 1;
	out.f_flags = LX_ST_VALID
		| ((info.flags & B_FS_IS_READONLY) != 0 ? LX_ST_RDONLY : 0);
	if (renguin_copy_out(buffer, &out, sizeof(out)) != B_OK)
		return -LX_EFAULT;
	return 0;
}


static int64
statfs_proc(uint64 buffer)
{
	linux_statfs out;
	memset(&out, 0, sizeof(out));
	out.f_type = renguin_proc_super_magic();
	out.f_bsize = B_PAGE_SIZE;
	out.f_frsize = B_PAGE_SIZE;
	out.f_namelen = 255;
	out.f_flags = LX_ST_VALID;
	if (renguin_copy_out(buffer, &out, sizeof(out)) != B_OK)
		return -LX_EFAULT;
	return 0;
}


int64
renguin_sys_statfs(uint64 path, uint64 buffer)
{
	char procPath[RENGUIN_PROC_PATH_MAX];
	int64 isProc = renguin_user_proc_path(LX_AT_FDCWD, path, procPath,
		sizeof(procPath));
	if (isProc < 0)
		return isProc;
	if (isProc > 0) {
		struct stat st;
		int64 result = renguin_proc_stat(procPath, true, &st);
		return result != 0 ? result : statfs_proc(buffer);
	}

	struct stat st;
	int64 result = renguin_stat_at(LX_AT_FDCWD, path, 0, &st);
	if (result != 0)
		return result;
	return statfs_device(st.st_dev, buffer);
}


int64
renguin_sys_fstatfs(int fd, uint64 buffer)
{
	if (renguin_proc_is_dir_fd(fd))
		return statfs_proc(buffer);
	struct stat st;
	status_t status = vfs_read_stat(fd, NULL, false, &st, false);
	if (status != B_OK)
		return renguin_encode(status);
	return statfs_device(st.st_dev, buffer);
}


// #pragma mark - permissions and owners


int64
renguin_sys_fchmodat(int dirfd, uint64 path, uint64 mode, uint64 flags)
{
	if ((flags & ~(uint64)(LX_AT_SYMLINK_NOFOLLOW | LX_AT_EMPTY_PATH)) != 0)
		return -LX_EINVAL;
	struct stat st;
	memset(&st, 0, sizeof(st));
	st.st_mode = mode & 07777;
	return write_stat_at(dirfd, path,
		(flags & LX_AT_SYMLINK_NOFOLLOW) == 0,
		(flags & LX_AT_EMPTY_PATH) != 0, st, B_STAT_MODE);
}


int64
renguin_sys_fchmod(int fd, uint64 mode)
{
	if (fd == LX_AT_FDCWD)
		return -LX_EBADF;
	struct stat st;
	memset(&st, 0, sizeof(st));
	st.st_mode = mode & 07777;
	return write_stat_at(fd, 0, true, true, st, B_STAT_MODE);
}


int64
renguin_sys_fchownat(int dirfd, uint64 path, uint64 owner, uint64 group,
	uint64 flags)
{
	if ((flags & ~(uint64)(LX_AT_SYMLINK_NOFOLLOW | LX_AT_EMPTY_PATH)) != 0)
		return -LX_EINVAL;
	struct stat st;
	memset(&st, 0, sizeof(st));
	int mask = 0;
	if ((uint32)owner != (uint32)-1) {
		st.st_uid = (uid_t)owner;
		mask |= B_STAT_UID;
	}
	if ((uint32)group != (uint32)-1) {
		st.st_gid = (gid_t)group;
		mask |= B_STAT_GID;
	}
	bool traverse = (flags & LX_AT_SYMLINK_NOFOLLOW) == 0;
	bool emptyPathOK = (flags & LX_AT_EMPTY_PATH) != 0;
	if (mask == 0) {
		// Nothing to change, but the file must exist.
		struct stat current;
		return renguin_stat_at(dirfd, path,
			flags & (LX_AT_SYMLINK_NOFOLLOW | LX_AT_EMPTY_PATH), &current);
	}
	return write_stat_at(dirfd, path, traverse, emptyPathOK, st, mask);
}


int64
renguin_sys_fchown(int fd, uint64 owner, uint64 group)
{
	if (fd == LX_AT_FDCWD)
		return -LX_EBADF;
	struct stat st;
	memset(&st, 0, sizeof(st));
	int mask = 0;
	if ((uint32)owner != (uint32)-1) {
		st.st_uid = (uid_t)owner;
		mask |= B_STAT_UID;
	}
	if ((uint32)group != (uint32)-1) {
		st.st_gid = (gid_t)group;
		mask |= B_STAT_GID;
	}
	if (mask == 0) {
		struct stat current;
		status_t status = vfs_read_stat(fd, NULL, false, &current, false);
		return status == B_OK ? 0 : renguin_encode(status);
	}
	return write_stat_at(fd, 0, true, true, st, mask);
}


// #pragma mark - times


static bool
timespec_for(const linux_timespec& time, struct timespec* out, bool* _set)
{
	if (time.tv_nsec == LX_UTIME_OMIT) {
		*_set = false;
		return true;
	}
	*_set = true;
	if (time.tv_nsec == LX_UTIME_NOW) {
		bigtime_t now = real_time_clock_usecs();
		out->tv_sec = now / 1000000;
		out->tv_nsec = (now % 1000000) * 1000;
		return true;
	}
	if (time.tv_nsec < 0 || time.tv_nsec >= 1000000000)
		return false;
	out->tv_sec = time.tv_sec;
	out->tv_nsec = time.tv_nsec;
	return true;
}


static int64
set_times(int dirfd, uint64 path, const linux_timespec* times, uint64 flags)
{
	struct stat st;
	memset(&st, 0, sizeof(st));
	int mask = 0;
	linux_timespec now[2] = {
		{ 0, LX_UTIME_NOW }, { 0, LX_UTIME_NOW }
	};
	if (times == NULL)
		times = now;
	bool set;
	if (!timespec_for(times[0], &st.st_atim, &set))
		return -LX_EINVAL;
	if (set)
		mask |= B_STAT_ACCESS_TIME;
	if (!timespec_for(times[1], &st.st_mtim, &set))
		return -LX_EINVAL;
	if (set)
		mask |= B_STAT_MODIFICATION_TIME;

	bool traverse = (flags & LX_AT_SYMLINK_NOFOLLOW) == 0;
	if (mask == 0) {
		// Linux still checks the file (and fails for a bad one).
		struct stat current;
		if (path == 0) {
			status_t status = vfs_read_stat(dirfd, NULL, false, &current,
				false);
			return status == B_OK ? 0 : renguin_encode(status);
		}
		return renguin_stat_at(dirfd, path,
			flags & (LX_AT_SYMLINK_NOFOLLOW | LX_AT_EMPTY_PATH), &current);
	}
	// A null path (futimens) means dirfd itself.
	return write_stat_at(dirfd, path, traverse,
		path == 0 || (flags & LX_AT_EMPTY_PATH) != 0, st, mask);
}


int64
renguin_sys_utimensat(int dirfd, uint64 path, uint64 times, uint64 flags)
{
	if ((flags & ~(uint64)(LX_AT_SYMLINK_NOFOLLOW | LX_AT_EMPTY_PATH)) != 0)
		return -LX_EINVAL;
	if (path == 0 && dirfd == LX_AT_FDCWD)
		return -LX_EFAULT;
	linux_timespec values[2];
	if (times != 0
		&& renguin_copy_in(values, times, sizeof(values)) != B_OK) {
		return -LX_EFAULT;
	}
	return set_times(dirfd, path, times != 0 ? values : NULL, flags);
}


//! utimes() and futimesat(): microseconds; NULL means now.
int64
renguin_sys_futimesat(int dirfd, uint64 path, uint64 times)
{
	if (times == 0)
		return set_times(dirfd, path, NULL, 0);
	linux_timeval values[2];
	if (renguin_copy_in(values, times, sizeof(values)) != B_OK)
		return -LX_EFAULT;
	linux_timespec converted[2];
	for (int i = 0; i < 2; i++) {
		if (values[i].tv_usec < 0 || values[i].tv_usec >= 1000000)
			return -LX_EINVAL;
		converted[i].tv_sec = values[i].tv_sec;
		converted[i].tv_nsec = values[i].tv_usec * 1000;
	}
	return set_times(dirfd, path, converted, 0);
}


//! utime(): struct utimbuf { time_t actime, modtime; }; NULL means now.
int64
renguin_sys_utime(uint64 path, uint64 times)
{
	if (times == 0)
		return set_times(LX_AT_FDCWD, path, NULL, 0);
	int64 values[2];
	if (renguin_copy_in(values, times, sizeof(values)) != B_OK)
		return -LX_EFAULT;
	linux_timespec converted[2] = {
		{ values[0], 0 }, { values[1], 0 }
	};
	return set_times(LX_AT_FDCWD, path, converted, 0);
}


// #pragma mark - links and special files


int64
renguin_sys_linkat(int oldDirFD, uint64 oldPath, int newDirFD,
	uint64 newPath, uint64 flags)
{
	if ((flags & ~(uint64)(LX_AT_SYMLINK_FOLLOW | LX_AT_EMPTY_PATH)) != 0)
		return -LX_EINVAL;
	if ((flags & LX_AT_EMPTY_PATH) != 0)
		return -LX_ENOENT;		// linking an FD needs CAP_DAC_READ_SEARCH
	if (renguin_proc_dirfd_refused(oldDirFD)
		|| renguin_proc_dirfd_refused(newDirFD)) {
		return -LX_EACCES;
	}
	bool follow = (flags & LX_AT_SYMLINK_FOLLOW) != 0;

	renguin_scratch scratch;
	renguin_scratch_init(&scratch);
	status_t status;
	uint64 hostOld = renguin_user_path(&scratch, oldPath,
		follow ? RENGUIN_PATH_FOLLOW_LEAF : 0, &status);
	if (status != B_OK)
		return renguin_encode(status);
	uint64 hostNew = renguin_user_path(&scratch, newPath,
		RENGUIN_PATH_MAY_CREATE, &status);
	if (status != B_OK)
		return renguin_encode(status);

	// Haiku's argument order: the new link first, then what it links to.
	status = _user_create_link(newDirFD, (const char*)(addr_t)hostNew,
		oldDirFD, (const char*)(addr_t)hostOld, follow);
	if (status == B_READ_ONLY_DEVICE) {
		// Haiku says this both for a read-only volume and for a file system
		// without hard links (BFS); Linux says EPERM for the latter.
		char* oldPath = (char*)malloc(B_PATH_NAME_LENGTH);
		fs_info* info = (fs_info*)malloc(sizeof(fs_info));
		struct stat st;
		bool readOnly = oldPath != NULL && info != NULL
			&& user_strlcpy(oldPath, (const char*)(addr_t)hostOld,
				B_PATH_NAME_LENGTH) >= 0
			&& vfs_read_stat(oldDirFD, oldPath, follow, &st, false) == B_OK
			&& _kern_read_fs_info(st.st_dev, info) == B_OK
			&& (info->flags & B_FS_IS_READONLY) != 0;
		free(oldPath);
		free(info);
		if (!readOnly)
			return -LX_EPERM;
	}
	return renguin_encode(status);
}


int64
renguin_sys_symlinkat(uint64 target, int dirfd, uint64 path)
{
	// The target is stored as given: it is interpreted when the link is
	// followed, by whoever follows it.
	char first;
	if (renguin_copy_in(&first, target, 1) != B_OK)
		return -LX_EFAULT;
	if (first == '\0')
		return -LX_ENOENT;
	if (renguin_proc_dirfd_refused(dirfd))
		return -LX_EACCES;

	renguin_scratch scratch;
	renguin_scratch_init(&scratch);
	status_t status;
	uint64 hostPath = renguin_user_path(&scratch, path,
		RENGUIN_PATH_MAY_CREATE, &status);
	if (status != B_OK)
		return renguin_encode(status);
	return renguin_encode(_user_create_symlink(dirfd,
		(const char*)(addr_t)hostPath, (const char*)(addr_t)target, 0777));
}


int64
renguin_sys_mknodat(int dirfd, uint64 path, uint64 mode, uint64 device)
{
	if (renguin_proc_dirfd_refused(dirfd))
		return -LX_EACCES;
	uint32 type = mode & LX_S_IFMT;
	uint32 permissions = mode & 07777 & ~renguin_current_umask();

	switch (type) {
		case 0:
		case LX_S_IFREG:
		{
			int64 fd = renguin_sys_openat(dirfd, path,
				LX_O_CREAT | LX_O_EXCL | LX_O_WRONLY | LX_O_CLOEXEC,
				mode & 07777);
			if (fd < 0)
				return fd;
			_user_close((int)fd);
			return 0;
		}
		case LX_S_IFIFO:
		{
			renguin_scratch scratch;
			renguin_scratch_init(&scratch);
			status_t status;
			uint64 hostPath = renguin_user_path(&scratch, path,
				RENGUIN_PATH_MAY_CREATE, &status);
			if (status != B_OK)
				return renguin_encode(status);
			return renguin_encode(_user_create_fifo(dirfd,
				(const char*)(addr_t)hostPath, permissions));
		}
		case LX_S_IFCHR:
		case LX_S_IFBLK:
		case LX_S_IFSOCK:
			// Haiku's devices live in devfs only, and sockets are created by
			// bind().
			return -LX_EPERM;
		case LX_S_IFDIR:
			return -LX_EPERM;
		default:
			return -LX_EINVAL;
	}
}


int64
renguin_sys_truncate(uint64 path, int64 length)
{
	if (length < 0)
		return -LX_EINVAL;
	struct stat st;
	memset(&st, 0, sizeof(st));
	st.st_size = length;
	return write_stat_at(LX_AT_FDCWD, path, true, false, st, B_STAT_SIZE);
}


// #pragma mark - descriptor operations


//! The FD must be open; advice for pipes fails as on Linux.
static int64
check_seekable(int fd)
{
	struct stat st;
	status_t status = vfs_read_stat(fd, NULL, false, &st, false);
	if (status != B_OK)
		return renguin_encode(status);
	if (S_ISFIFO(st.st_mode) || S_ISSOCK(st.st_mode))
		return -LX_ESPIPE;
	return 0;
}


int64
renguin_sys_fadvise64(int fd, int64 offset, int64 length, uint64 advice)
{
	int64 result = check_seekable(fd);
	if (result != 0)
		return result;
	if (advice > LX_POSIX_FADV_NOREUSE)
		return -LX_EINVAL;
	return 0;		// advice only
}


int64
renguin_sys_readahead(int fd, int64 offset, uint64 count)
{
	struct stat st;
	status_t status = vfs_read_stat(fd, NULL, false, &st, false);
	if (status != B_OK)
		return renguin_encode(status);
	if (!S_ISREG(st.st_mode))
		return -LX_EINVAL;
	return 0;
}


int64
renguin_sys_fallocate(int fd, uint64 mode, int64 offset, int64 length)
{
	if (offset < 0 || length <= 0)
		return -LX_EINVAL;
	if (mode != 0)
		return -LX_EOPNOTSUPP;		// KEEP_SIZE, PUNCH_HOLE, ...
	return renguin_encode(_user_preallocate(fd, offset, length));
}


int64
renguin_sys_sync()
{
	_user_sync();
	return 0;
}


int64
renguin_sys_syncfs(int fd)
{
	struct stat st;
	status_t status = vfs_read_stat(fd, NULL, false, &st, false);
	if (status != B_OK)
		return renguin_encode(status);
	_user_sync();
	return 0;
}


int64
renguin_sys_flock(int fd, uint64 operation)
{
	// LOCK_SH/EX/NB/UN have the same values on both systems.
	return renguin_encode(_user_flock(fd, (int)operation));
}


/*!	sendfile: copies through a bounce buffer in user scratch space. Linux
	reads from \a offset if given (and updates it), else from the file
	position.
*/
int64
renguin_sys_sendfile(int outFD, int inFD, uint64 offsetAddress, uint64 count)
{
	off_t position = -1;
	if (offsetAddress != 0) {
		int64 offset;
		if (renguin_copy_in(&offset, offsetAddress, sizeof(offset)) != B_OK)
			return -LX_EFAULT;
		if (offset < 0)
			return -LX_EINVAL;
		position = offset;
	}
	if (count > 0x7ffff000)
		count = 0x7ffff000;		// MAX_RW_COUNT

	renguin_scratch scratch;
	renguin_scratch_init(&scratch);
	// A bounce buffer on the stack, smaller if the stack has little room.
	size_t kChunk = 16 * 1024;
	uint64 bounce = 0;
	while (kChunk >= 512
		&& (bounce = renguin_scratch_alloc(&scratch, kChunk)) == 0) {
		kChunk /= 4;
	}
	if (bounce == 0)
		return -LX_EFAULT;

	int64 total = 0;
	status_t error = B_OK;
	while ((uint64)total < count) {
		size_t want = min_c(kChunk, (size_t)(count - total));
		ssize_t got = _user_read(inFD, position, (void*)(addr_t)bounce, want);
		if (got <= 0) {
			error = got;
			break;
		}
		ssize_t put = 0;
		while (put < got) {
			ssize_t written = _user_write(outFD, -1,
				(const void*)(addr_t)(bounce + put), got - put);
			if (written <= 0) {
				error = written < 0 ? written : B_IO_ERROR;
				break;
			}
			put += written;
		}
		total += put;
		if (position >= 0)
			position += put;
		if (put < got) {
			// The rest was read but not written: give it back if it came
			// from the file position.
			if (position < 0)
				_user_seek(inFD, put - got, SEEK_CUR);
			break;
		}
		if ((size_t)got < want)
			break;
	}

	if (offsetAddress != 0) {
		int64 offset = position;
		if (renguin_copy_out(offsetAddress, &offset, sizeof(offset)) != B_OK)
			return -LX_EFAULT;
	}
	if (total > 0) {
		// What was sent stays sent, even if a signal interrupted the rest.
		renguin_cancel_restart();
		return total;
	}
	return error < 0 ? renguin_encode(error) : 0;
}
