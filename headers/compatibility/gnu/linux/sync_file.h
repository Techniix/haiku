/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 *
 * Linux sync_file interface (<linux/sync_file.h>): a file descriptor that
 * stands for one GPU or CPU fence. It is readable (poll POLLIN) once the
 * fence has signalled. Renga's syncobj module (<syncobj_module.h>) creates
 * these descriptors; the layouts and ioctl numbers are Linux's, so Linux
 * programs under Renguin send the same requests as native ones.
 */
#ifndef _LINUX_SYNC_FILE_H
#define _LINUX_SYNC_FILE_H

#include <stdint.h>

#include <linux/ioctl.h>


/*! SYNC_IOC_MERGE: a new sync_file that signals once both this one and
	\c fd2 have signalled. The new descriptor is returned in \c fence. */
struct sync_merge_data {
	char		name[32];
	int32_t		fd2;
	int32_t		fence;
	uint32_t	flags;		/* must be 0 */
	uint32_t	pad;
};

/*! One fence of a sync_file (SYNC_IOC_FILE_INFO). \c status: 1 signalled,
	0 active, negative an error the fence was signalled with. */
struct sync_fence_info {
	char		obj_name[32];
	char		driver_name[32];
	int32_t		status;
	uint32_t	flags;
	uint64_t	timestamp_ns;	/* CLOCK_MONOTONIC when it signalled */
};

/*! SYNC_IOC_FILE_INFO. With \c num_fences 0 only the count is returned;
	otherwise \c sync_fence_info points to that many sync_fence_info. */
struct sync_file_info {
	char		name[32];
	int32_t		status;
	uint32_t	flags;
	uint32_t	num_fences;
	uint32_t	pad;
	uint64_t	sync_fence_info;
};

/*! SYNC_IOC_SET_DEADLINE: a hint when the fence is needed. Accepted and
	ignored by Renga for now. */
struct sync_set_deadline {
	uint64_t	deadline_ns;
	uint64_t	pad;
};

#define SYNC_IOC_MAGIC			'>'
#define SYNC_IOC_MERGE			_LINUX_IOWR(SYNC_IOC_MAGIC, 3, \
									struct sync_merge_data)
#define SYNC_IOC_FILE_INFO		_LINUX_IOWR(SYNC_IOC_MAGIC, 4, \
									struct sync_file_info)
#define SYNC_IOC_SET_DEADLINE	_LINUX_IOW(SYNC_IOC_MAGIC, 5, \
									struct sync_set_deadline)

#endif	/* _LINUX_SYNC_FILE_H */
