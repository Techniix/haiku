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
 * File sealing works as on Linux: with MFD_ALLOW_SEALING, fcntl(F_ADD_SEALS)
 * and fcntl(F_GET_SEALS) (<fcntl.h>) take F_SEAL_SEAL, F_SEAL_SHRINK,
 * F_SEAL_GROW, F_SEAL_WRITE, F_SEAL_FUTURE_WRITE and F_SEAL_EXEC; without
 * it the file starts with F_SEAL_SEAL. MFD_NOEXEC_SEAL implies
 * MFD_ALLOW_SEALING and adds F_SEAL_EXEC. MFD_HUGETLB is rejected with
 * EINVAL.
 */
#ifndef _GNU_SYS_MMAN_H
#define _GNU_SYS_MMAN_H


#include_next <sys/mman.h>

#include <sys/cdefs.h>


/* flags for memfd_create() */
#define MFD_CLOEXEC			0x0001U
#define MFD_ALLOW_SEALING	0x0002U
#define MFD_HUGETLB			0x0004U	/* not supported: EINVAL */
#define MFD_NOEXEC_SEAL		0x0008U	/* sealable, with F_SEAL_EXEC */
#define MFD_EXEC			0x0010U	/* accepted; files are never executable */


__BEGIN_DECLS


int		memfd_create(const char* name, unsigned int flags);


__END_DECLS


#endif	/* _GNU_SYS_MMAN_H */
