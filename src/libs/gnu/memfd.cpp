/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 *
 * memfd_create() on top of Haiku's POSIX shared memory directory.
 *
 * The launch_daemon mounts a ramfs at /var/shared_memory at boot (that is
 * where shm_open() keeps its objects). A memfd is a file created there with
 * O_EXCL under a unique name and unlinked right away: from then on only
 * descriptors and mappings keep it alive, exactly the lifetime Linux gives
 * a memfd, and its pages live in RAM like any other ramfs file. Nothing
 * needs to be added to the kernel for this.
 *
 * The unique name combines the team ID, a per-process counter and the
 * caller's name (truncated) so that a file left behind by a crash between
 * open() and unlink() can still be traced to its creator.
 */


#include <sys/mman.h>

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <OS.h>

#include <errno_private.h>


static const char* kMemFDDirectory = "/var/shared_memory";

// Linux limits the name to 249 bytes ("memfd:" + name must fit NAME_MAX).
static const size_t kMaxNameLength = 249;

static int32 sCounter = 0;


int
memfd_create(const char* name, unsigned int flags)
{
	if (name == NULL) {
		__set_errno(EFAULT);
		return -1;
	}
	if (strlen(name) > kMaxNameLength) {
		__set_errno(EINVAL);
		return -1;
	}
	if ((flags & ~(MFD_CLOEXEC | MFD_ALLOW_SEALING | MFD_NOEXEC_SEAL
			| MFD_EXEC)) != 0
		|| (flags & (MFD_NOEXEC_SEAL | MFD_EXEC))
			== (MFD_NOEXEC_SEAL | MFD_EXEC)) {
		// Unknown flags, MFD_HUGETLB (and its size bits), or the
		// contradictory NOEXEC_SEAL + EXEC pair, as Linux rejects them.
		__set_errno(EINVAL);
		return -1;
	}

	int openMode = O_RDWR | O_CREAT | O_EXCL;
	if ((flags & MFD_CLOEXEC) != 0)
		openMode |= O_CLOEXEC;

	// Keep only harmless characters of the caller's name for the file name.
	char tag[32];
	size_t tagLength = 0;
	for (const char* c = name; *c != '\0' && tagLength < sizeof(tag) - 1;
			c++) {
		bool safe = (*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z')
			|| (*c >= '0' && *c <= '9') || *c == '-' || *c == '_'
			|| *c == '.';
		tag[tagLength++] = safe ? *c : '_';
	}
	tag[tagLength] = '\0';

	team_id team = getpid();
	for (int attempt = 0; attempt < 16; attempt++) {
		char path[B_PATH_NAME_LENGTH];
		snprintf(path, sizeof(path), "%s/memfd-%" B_PRId32 "-%" B_PRId32
			"-%" B_PRIx64 "-%s", kMemFDDirectory, team,
			atomic_add(&sCounter, 1), (uint64)system_time(), tag);

		// Owner-only permissions: the file is visible in the directory for
		// the moment between open() and unlink().
		int fd = open(path, openMode, 0600);
		if (fd < 0) {
			if (errno == EEXIST)
				continue;
			if (errno == ENOENT) {
				// No shared memory volume (e.g. very early boot).
				__set_errno(ENOSYS);
			}
			return -1;
		}

		unlink(path);
		return fd;
	}

	__set_errno(EEXIST);
	return -1;
}
