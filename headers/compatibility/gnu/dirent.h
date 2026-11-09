/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 *
 * Linux additions to <dirent.h>: getdents64() (glibc 2.30) and getdents(),
 * returning records in the layout of the Linux system calls. Link with
 * -lgnu.
 */
#ifndef _GNU_DIRENT_H
#define _GNU_DIRENT_H


#include_next <dirent.h>

#include <stdint.h>
#include <sys/cdefs.h>
#include <sys/types.h>


/* Record of getdents64(), as Linux's struct linux_dirent64 (and glibc's
   struct dirent64): 8-byte aligned, d_reclen bytes long, name
   NUL-terminated. */
struct linux_dirent64 {
	uint64_t		d_ino;
	int64_t			d_off;
	unsigned short	d_reclen;
	unsigned char	d_type;
	char			d_name[];
};

/* Record of getdents(), as Linux's struct linux_dirent: the type is the last
   byte of the record, ((char*)entry)[entry->d_reclen - 1]. */
struct linux_dirent {
	unsigned long	d_ino;
	unsigned long	d_off;
	unsigned short	d_reclen;
	char			d_name[];
};

/* d_type values are Linux's (and the BSDs'): 0 unknown, 1 FIFO, 2 character
   device, 4 directory, 6 block device, 8 regular file, 10 symbolic link,
   12 socket. The DT_* names are deliberately not defined here: Haiku's
   struct dirent has no d_type member, and code commonly takes "#ifdef
   DT_DIR" to mean it has one. */


__BEGIN_DECLS

/* Read directory entries of the directory open as fd into buffer, as many as
   fit in length bytes. Returns the number of bytes filled, 0 at the end of
   the directory, or -1 with errno set (EINVAL if not even one entry fits,
   ENOTDIR, EBADF, EFAULT). */
ssize_t	getdents64(int fd, void* buffer, size_t length);
ssize_t	getdents(int fd, void* buffer, size_t length);

__END_DECLS


#endif	/* _GNU_DIRENT_H */
