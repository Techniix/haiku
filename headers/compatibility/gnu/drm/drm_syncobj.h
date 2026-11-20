/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 *
 * DRM sync objects, as in Linux's <drm/drm.h> (DRM_IOCTL_SYNCOBJ_*), for
 * programs that talk to Renga's syncobj module directly: /dev/syncobj, or a
 * GPU render node that forwards these requests (see <syncobj_module.h>).
 *
 * Layouts and ioctl numbers are Linux's. Programs that use libdrm or Mesa's
 * own copy of drm.h get the same values from there; when that drm.h was
 * included first (_DRM_H_), only SYNCOBJ_EVENTFD is added if it lacks it.
 *
 * Time-outs are absolute CLOCK_MONOTONIC nanoseconds, as on Linux; on Renga
 * that is system_time() * 1000. A wait that times out fails with ETIME.
 */
#ifndef _RENGA_DRM_SYNCOBJ_H
#define _RENGA_DRM_SYNCOBJ_H

#include <stdint.h>

#include <linux/ioctl.h>


#define RENGA_DRM_IOCTL_BASE	'd'

#ifndef _DRM_H_

#define DRM_SYNCOBJ_CREATE_SIGNALED		(1 << 0)
struct drm_syncobj_create {
	uint32_t	handle;
	uint32_t	flags;
};

struct drm_syncobj_destroy {
	uint32_t	handle;
	uint32_t	pad;
};

#define DRM_SYNCOBJ_FD_TO_HANDLE_FLAGS_IMPORT_SYNC_FILE	(1 << 0)
#define DRM_SYNCOBJ_FD_TO_HANDLE_FLAGS_TIMELINE			(1 << 1)
#define DRM_SYNCOBJ_HANDLE_TO_FD_FLAGS_EXPORT_SYNC_FILE	(1 << 0)
#define DRM_SYNCOBJ_HANDLE_TO_FD_FLAGS_TIMELINE			(1 << 1)
struct drm_syncobj_handle {
	uint32_t	handle;
	uint32_t	flags;
	int32_t		fd;
	uint32_t	pad;
	uint64_t	point;		/* with the _TIMELINE flags only */
};

struct drm_syncobj_transfer {
	uint32_t	src_handle;
	uint32_t	dst_handle;
	uint64_t	src_point;
	uint64_t	dst_point;
	uint32_t	flags;
	uint32_t	pad;
};

#define DRM_SYNCOBJ_WAIT_FLAGS_WAIT_ALL			(1 << 0)
#define DRM_SYNCOBJ_WAIT_FLAGS_WAIT_FOR_SUBMIT	(1 << 1)
#define DRM_SYNCOBJ_WAIT_FLAGS_WAIT_AVAILABLE	(1 << 2)
#define DRM_SYNCOBJ_WAIT_FLAGS_WAIT_DEADLINE	(1 << 3)
struct drm_syncobj_wait {
	uint64_t	handles;		/* user pointer to uint32_t[count_handles] */
	int64_t		timeout_nsec;	/* absolute CLOCK_MONOTONIC */
	uint32_t	count_handles;
	uint32_t	flags;
	uint32_t	first_signaled;	/* out: index, for "any" waits */
	uint32_t	pad;
	uint64_t	deadline_nsec;	/* hint, with WAIT_DEADLINE */
};

struct drm_syncobj_timeline_wait {
	uint64_t	handles;
	uint64_t	points;			/* user pointer to uint64_t[count_handles] */
	int64_t		timeout_nsec;
	uint32_t	count_handles;
	uint32_t	flags;
	uint32_t	first_signaled;
	uint32_t	pad;
	uint64_t	deadline_nsec;
};

struct drm_syncobj_array {
	uint64_t	handles;
	uint32_t	count_handles;
	uint32_t	pad;
};

#define DRM_SYNCOBJ_QUERY_FLAGS_LAST_SUBMITTED	(1 << 0)
struct drm_syncobj_timeline_array {
	uint64_t	handles;
	uint64_t	points;
	uint32_t	count_handles;
	uint32_t	flags;
};

#define DRM_IOCTL_SYNCOBJ_CREATE		_LINUX_IOWR(RENGA_DRM_IOCTL_BASE, \
	0xBF, struct drm_syncobj_create)
#define DRM_IOCTL_SYNCOBJ_DESTROY		_LINUX_IOWR(RENGA_DRM_IOCTL_BASE, \
	0xC0, struct drm_syncobj_destroy)
#define DRM_IOCTL_SYNCOBJ_HANDLE_TO_FD	_LINUX_IOWR(RENGA_DRM_IOCTL_BASE, \
	0xC1, struct drm_syncobj_handle)
#define DRM_IOCTL_SYNCOBJ_FD_TO_HANDLE	_LINUX_IOWR(RENGA_DRM_IOCTL_BASE, \
	0xC2, struct drm_syncobj_handle)
#define DRM_IOCTL_SYNCOBJ_WAIT			_LINUX_IOWR(RENGA_DRM_IOCTL_BASE, \
	0xC3, struct drm_syncobj_wait)
#define DRM_IOCTL_SYNCOBJ_RESET			_LINUX_IOWR(RENGA_DRM_IOCTL_BASE, \
	0xC4, struct drm_syncobj_array)
#define DRM_IOCTL_SYNCOBJ_SIGNAL		_LINUX_IOWR(RENGA_DRM_IOCTL_BASE, \
	0xC5, struct drm_syncobj_array)
#define DRM_IOCTL_SYNCOBJ_TIMELINE_WAIT	_LINUX_IOWR(RENGA_DRM_IOCTL_BASE, \
	0xCA, struct drm_syncobj_timeline_wait)
#define DRM_IOCTL_SYNCOBJ_QUERY			_LINUX_IOWR(RENGA_DRM_IOCTL_BASE, \
	0xCB, struct drm_syncobj_timeline_array)
#define DRM_IOCTL_SYNCOBJ_TRANSFER		_LINUX_IOWR(RENGA_DRM_IOCTL_BASE, \
	0xCC, struct drm_syncobj_transfer)
#define DRM_IOCTL_SYNCOBJ_TIMELINE_SIGNAL _LINUX_IOWR(RENGA_DRM_IOCTL_BASE, \
	0xCD, struct drm_syncobj_timeline_array)
#endif	/* !_DRM_H_ */

/* Newer than some drm.h copies (Linux 6.6). */
#if !defined(_DRM_H_) || !defined(DRM_IOCTL_SYNCOBJ_EVENTFD)
/*! One-shot: the eventfd \c fd is written once when \c point signals (or,
	with WAIT_AVAILABLE, once a fence for it exists). */
struct drm_syncobj_eventfd {
	uint32_t	handle;
	uint32_t	flags;
	uint64_t	point;
	int32_t		fd;
	uint32_t	pad;
};

#define DRM_IOCTL_SYNCOBJ_EVENTFD		_LINUX_IOWR(RENGA_DRM_IOCTL_BASE, \
	0xCF, struct drm_syncobj_eventfd)

#endif

/*
 * CPU timelines (Linux's sw_sync test interface, which Linux has in
 * debugfs): on a /dev/syncobj descriptor, SW_SYNC_IOC_CREATE_FENCE returns
 * a sync_file that signals once the descriptor's counter reaches \c value;
 * SW_SYNC_IOC_INC adds to the counter. Closing the descriptor signals the
 * remaining fences with ENOENT. GPU render nodes do not offer these.
 */
#ifndef SW_SYNC_IOC_CREATE_FENCE
struct sw_sync_create_fence_data {
	uint32_t	value;
	char		name[32];
	int32_t		fence;		/* out */
};

#define SW_SYNC_IOC_MAGIC			'W'
#define SW_SYNC_IOC_CREATE_FENCE	_LINUX_IOWR(SW_SYNC_IOC_MAGIC, 0, \
										struct sw_sync_create_fence_data)
#define SW_SYNC_IOC_INC				_LINUX_IOW(SW_SYNC_IOC_MAGIC, 1, uint32_t)
#endif

#endif	/* _RENGA_DRM_SYNCOBJ_H */
