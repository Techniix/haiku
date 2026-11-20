/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 *
 * DRM sync objects for Linux programs (Quasar R-18): Renguin passes the
 * DRM_IOCTL_SYNCOBJ_*, sync_file and sw_sync requests through to Renga's
 * syncobj module unchanged. The definitions are Linux's <drm/drm.h> and
 * <linux/sync_file.h> values, written out so the build host needs no DRM
 * headers. Prints one word per step; the first failure stops with a
 * message on stderr.
 */

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

struct drm_syncobj_create { uint32_t handle, flags; };
struct drm_syncobj_handle { uint32_t handle, flags; int32_t fd; uint32_t pad;
	uint64_t point; };
struct drm_syncobj_timeline_wait { uint64_t handles, points; int64_t timeout;
	uint32_t count, flags, first, pad; uint64_t deadline; };
struct drm_syncobj_timeline_array { uint64_t handles, points;
	uint32_t count, flags; };
struct drm_syncobj_eventfd { uint32_t handle, flags; uint64_t point;
	int32_t fd; uint32_t pad; };
struct sw_sync_create_fence_data { uint32_t value; char name[32];
	int32_t fence; };
struct sync_file_info { char name[32]; int32_t status; uint32_t flags,
	num_fences, pad; uint64_t info; };

#define CREATE		_IOWR('d', 0xBF, struct drm_syncobj_create)
#define FD_TO_HANDLE _IOWR('d', 0xC2, struct drm_syncobj_handle)
#define TL_WAIT		_IOWR('d', 0xCA, struct drm_syncobj_timeline_wait)
#define QUERY		_IOWR('d', 0xCB, struct drm_syncobj_timeline_array)
#define TL_SIGNAL	_IOWR('d', 0xCD, struct drm_syncobj_timeline_array)
#define EVENTFD		_IOWR('d', 0xCF, struct drm_syncobj_eventfd)
#define SW_CREATE	_IOWR('W', 0, struct sw_sync_create_fence_data)
#define SW_INC		_IOW('W', 1, uint32_t)
#define FILE_INFO	_IOWR('>', 4, struct sync_file_info)

#define FAIL(what) do { fprintf(stderr, "%s: %s\n", what, strerror(errno)); \
	return 1; } while (0)

static int
readable(int fd)
{
	struct pollfd pfd = { fd, POLLIN, 0 };
	return poll(&pfd, 1, 0) == 1 && (pfd.revents & POLLIN);
}

int
main(void)
{
	int node = open("/dev/syncobj", O_RDWR | O_CLOEXEC);
	if (node < 0)
		FAIL("open /dev/syncobj");

	struct drm_syncobj_create create = { 0, 0 };
	if (ioctl(node, CREATE, &create) != 0 || create.handle == 0)
		FAIL("create");
	puts("create");

	uint32_t handle = create.handle;
	uint64_t point = 3;
	struct drm_syncobj_timeline_array array = { (uintptr_t)&handle,
		(uintptr_t)&point, 1, 0 };
	if (ioctl(node, TL_SIGNAL, &array) != 0)
		FAIL("timeline signal");
	point = 0;
	if (ioctl(node, QUERY, &array) != 0 || point != 3)
		FAIL("query");
	puts("timeline");

	struct sw_sync_create_fence_data fence = { 1, "linux", -1 };
	if (ioctl(node, SW_CREATE, &fence) != 0 || readable(fence.fence))
		FAIL("sw_sync fence");
	struct sync_file_info info;
	memset(&info, 0, sizeof(info));
	if (ioctl(fence.fence, FILE_INFO, &info) != 0 || info.status != 0)
		FAIL("sync_file info");
	puts("sync_file");

	struct drm_syncobj_handle import = { handle, 3, fence.fence, 0, 5 };
		/* IMPORT_SYNC_FILE | TIMELINE */
	if (ioctl(node, FD_TO_HANDLE, &import) != 0)
		FAIL("import");
	int event = eventfd(0, EFD_NONBLOCK);
	struct drm_syncobj_eventfd watch = { handle, 0, 5, event, 0 };
	if (ioctl(node, EVENTFD, &watch) != 0 || readable(event))
		FAIL("eventfd");

	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	point = 5;
	struct drm_syncobj_timeline_wait wait = { (uintptr_t)&handle,
		(uintptr_t)&point, (int64_t)now.tv_sec * 1000000000 + now.tv_nsec,
		1, 0, 0, 0, 0 };
	if (ioctl(node, TL_WAIT, &wait) != -1 || errno != ETIME)
		FAIL("wait times out with ETIME");

	uint32_t one = 1;
	if (ioctl(node, SW_INC, &one) != 0)
		FAIL("sw_sync increment");
	uint64_t count = 0;
	if (read(event, &count, sizeof(count)) != sizeof(count) || count != 1)
		FAIL("eventfd fired");
	puts("eventfd");

	wait.timeout = 0;
	if (ioctl(node, TL_WAIT, &wait) != 0 || !readable(fence.fence))
		FAIL("signalled");
	puts("wait");

	if (ioctl(node, _IOWR('d', 0xA0, uint64_t), &count) != -1
		|| errno != ENOTTY)
		FAIL("unknown request is ENOTTY");

	close(event);
	close(fence.fence);
	close(node);
	puts("ok");
	return 0;
}
