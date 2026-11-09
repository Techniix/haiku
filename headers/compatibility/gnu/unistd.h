/*
 * Copyright 2026, Haiku Community Edition contributors.
 * Distributed under the terms of the MIT License.
 *
 * Linux additions to <unistd.h>. Link with -lgnu.
 */
#ifndef _GNU_UNISTD_H
#define _GNU_UNISTD_H


#include_next <unistd.h>

#include <sys/cdefs.h>
#include <sys/types.h>


__BEGIN_DECLS

/* Copies a range between two files. Done in user space with pread/pwrite
   style I/O through a buffer; works across file systems (as on Linux 5.19
   and later). "flags" must be 0. */
ssize_t	copy_file_range(int inFD, off_t* inOffset, int outFD,
			off_t* outOffset, size_t length, unsigned int flags);

__END_DECLS


#endif	/* _GNU_UNISTD_H */
