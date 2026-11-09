/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 *
 * Linux additions to <sys/mman.h>. Link with -lgnu.
 *
 * memfd_create() returns a file descriptor for an anonymous, memory-backed
 * file: it can be sized with ftruncate(), read and written, mapped with
 * mmap(MAP_SHARED) and passed to other processes (fork(), or over a UNIX
 * socket), and it disappears when the last descriptor and mapping are gone.
 *
 * File sealing (F_ADD_SEALS / F_GET_SEALS) is not implemented. The fcntl()
 * commands are deliberately left undefined so that code guarded by
 * "#ifdef F_ADD_SEALS" builds without it; MFD_ALLOW_SEALING is accepted and
 * has no effect. MFD_HUGETLB is rejected with EINVAL.
 */
#ifndef _GNU_SYS_MMAN_H
#define _GNU_SYS_MMAN_H


#include_next <sys/mman.h>

#include <sys/cdefs.h>


/* flags for memfd_create() */
#define MFD_CLOEXEC			0x0001U
#define MFD_ALLOW_SEALING	0x0002U	/* accepted; sealing is not supported */
#define MFD_HUGETLB			0x0004U	/* not supported: EINVAL */
#define MFD_NOEXEC_SEAL		0x0008U	/* accepted; no effect */
#define MFD_EXEC			0x0010U	/* accepted; no effect */


__BEGIN_DECLS


int		memfd_create(const char* name, unsigned int flags);


__END_DECLS


#endif	/* _GNU_SYS_MMAN_H */
