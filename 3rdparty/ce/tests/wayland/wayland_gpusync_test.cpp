/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 */

/*!	The compositor's linux-dmabuf (version 4) and wp_linux_drm_syncobj_v1,
	as a native client sees them, in QEMU: buffers are memfds and fences
	are CPU-signalled DRM syncobj timeline points (/dev/syncobj).

	  dmabuf.version        zwp_linux_dmabuf_v1 is offered at version 4
	  dmabuf.feedback       default feedback: a format table of 4 entries
	                        (ARGB8888/XRGB8888, LINEAR/INVALID), an 8-byte
	                        main device, one tranche with all 4, done
	  dmabuf.create_immed   a memfd buffer becomes a wl_buffer that a
	                        surface shows (the connection stays healthy)
	  dmabuf.create_failed  create() with an unknown format: "failed"
	  dmabuf.out_of_bounds  a stride past the end of the memfd is the
	                        out_of_bounds error
	  syncobj.version       wp_linux_drm_syncobj_manager_v1 version 1
	  syncobj.deferred      a commit whose acquire point has not signalled
	                        is not applied: the buffer on screen keeps its
	                        release point unsignalled
	  syncobj.applied       once the client signals the acquire point, the
	                        compositor applies the commit (from its event
	                        loop, no further request) and signals the
	                        previous buffer's release point
	  syncobj.surface_gone  destroying the surface signals the release
	                        point of the buffer it showed
	  syncobj.errors        unsupported_buffer (wl_shm), no_acquire_point,
	                        conflicting_points, surface_exists,
	                        invalid_timeline
	Output format: see 3rdparty/ce/README.md.
*/


#include <Application.h>
#include <OS.h>
#include <Roster.h>

#include <drm/drm_syncobj.h>

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include <string>

#include <wayland-client.h>
#include "linux-dmabuf-v1-client-protocol.h"
#include "linux-drm-syncobj-v1-client-protocol.h"


#define SUITE "wayland_gpusync"

static const char* kCompositorSignature
	= "application/x-vnd.Renga-WaylandCompositor";
static const uint32_t kXrgb8888 = 0x34325258;

static int sFailures = 0;
static int sNode = -1;


static void
report(const char* name, bool pass, const char* format, ...)
{
	char detail[400];
	va_list args;
	va_start(args, format);
	vsnprintf(detail, sizeof(detail), format, args);
	va_end(args);
	printf("CE-TEST %s.%s %s %s\n", SUITE, name, pass ? "PASS" : "FAIL",
		detail);
	fflush(stdout);
	if (!pass)
		sFailures++;
}


// #pragma mark - connection


struct Connection {
	wl_display*		display = NULL;
	wl_registry*	registry = NULL;
	wl_compositor*	compositor = NULL;
	wl_shm*			shm = NULL;
	zwp_linux_dmabuf_v1* dmabuf = NULL;
	wp_linux_drm_syncobj_manager_v1* syncobj = NULL;
	uint32_t		dmabufVersion = 0;
	uint32_t		syncobjVersion = 0;
};


static void
registry_global(void* data, wl_registry* registry, uint32_t name,
	const char* interface, uint32_t version)
{
	Connection* connection = (Connection*)data;
	if (strcmp(interface, "wl_compositor") == 0) {
		connection->compositor = (wl_compositor*)wl_registry_bind(registry,
			name, &wl_compositor_interface, 4);
	} else if (strcmp(interface, "wl_shm") == 0) {
		connection->shm = (wl_shm*)wl_registry_bind(registry, name,
			&wl_shm_interface, 1);
	} else if (strcmp(interface, "zwp_linux_dmabuf_v1") == 0) {
		connection->dmabufVersion = version;
		connection->dmabuf = (zwp_linux_dmabuf_v1*)wl_registry_bind(registry,
			name, &zwp_linux_dmabuf_v1_interface, version < 4 ? version : 4);
	} else if (strcmp(interface, "wp_linux_drm_syncobj_manager_v1") == 0) {
		connection->syncobjVersion = version;
		connection->syncobj = (wp_linux_drm_syncobj_manager_v1*)
			wl_registry_bind(registry, name,
				&wp_linux_drm_syncobj_manager_v1_interface, 1);
	}
}


static void registry_global_remove(void*, wl_registry*, uint32_t) {}

static const wl_registry_listener kRegistryListener = {
	registry_global, registry_global_remove
};


static bool
connect(Connection& connection)
{
	connection.display = wl_display_connect(NULL);
	if (connection.display == NULL)
		return false;
	connection.registry = wl_display_get_registry(connection.display);
	wl_registry_add_listener(connection.registry, &kRegistryListener,
		&connection);
	return wl_display_roundtrip(connection.display) >= 0;
}


static void
disconnect(Connection& connection)
{
	if (connection.display != NULL)
		wl_display_disconnect(connection.display);
	connection = Connection();
}


//! The protocol error the connection died with, or 0.
static uint32_t
protocol_error(Connection& connection)
{
	wl_display_roundtrip(connection.display);
	if (wl_display_get_error(connection.display) != EPROTO)
		return 0;
	const wl_interface* interface;
	uint32_t id;
	return wl_display_get_protocol_error(connection.display, &interface, &id);
}


static int
memfd(size_t size)
{
	int fd = memfd_create("gpusync-test", MFD_CLOEXEC);
	if (fd >= 0 && ftruncate(fd, size) != 0) {
		close(fd);
		return -1;
	}
	return fd;
}


//! A 16x16 XRGB8888 dmabuf (memfd) buffer, made with create_immed.
static wl_buffer*
dmabuf_buffer(Connection& connection, uint32_t stride = 64)
{
	int fd = memfd(64 * 16);
	if (fd < 0)
		return NULL;
	zwp_linux_buffer_params_v1* params
		= zwp_linux_dmabuf_v1_create_params(connection.dmabuf);
	zwp_linux_buffer_params_v1_add(params, fd, 0, 0, stride, 0, 0);
	close(fd);
	wl_buffer* buffer = zwp_linux_buffer_params_v1_create_immed(params, 16,
		16, kXrgb8888, 0);
	zwp_linux_buffer_params_v1_destroy(params);
	return buffer;
}


// #pragma mark - syncobj helpers


static uint32_t
timeline_create()
{
	drm_syncobj_create data = {};
	return ioctl(sNode, DRM_IOCTL_SYNCOBJ_CREATE, &data) == 0
		? data.handle : 0;
}


static int
timeline_fd(uint32_t handle)
{
	drm_syncobj_handle data = {};
	data.handle = handle;
	return ioctl(sNode, DRM_IOCTL_SYNCOBJ_HANDLE_TO_FD, &data) == 0
		? data.fd : -1;
}


static void
timeline_signal(uint32_t handle, uint64_t point)
{
	drm_syncobj_timeline_array data = {};
	data.handles = (uint64_t)(uintptr_t)&handle;
	data.points = (uint64_t)(uintptr_t)&point;
	data.count_handles = 1;
	ioctl(sNode, DRM_IOCTL_SYNCOBJ_TIMELINE_SIGNAL, &data);
}


//! Has \a point signalled within \a timeout microseconds?
static bool
timeline_signalled(uint32_t handle, uint64_t point, bigtime_t timeout)
{
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	drm_syncobj_timeline_wait data = {};
	data.handles = (uint64_t)(uintptr_t)&handle;
	data.points = (uint64_t)(uintptr_t)&point;
	data.count_handles = 1;
	data.flags = DRM_SYNCOBJ_WAIT_FLAGS_WAIT_FOR_SUBMIT;
	data.timeout_nsec = timeout == 0 ? 0
		: (int64_t)now.tv_sec * 1000000000LL + now.tv_nsec + timeout * 1000;
	return ioctl(sNode, DRM_IOCTL_SYNCOBJ_TIMELINE_WAIT, &data) == 0;
}


static wp_linux_drm_syncobj_timeline_v1*
import_timeline(Connection& connection, uint32_t handle)
{
	int fd = timeline_fd(handle);
	wp_linux_drm_syncobj_timeline_v1* timeline
		= wp_linux_drm_syncobj_manager_v1_import_timeline(connection.syncobj,
			fd);
	close(fd);
	return timeline;
}


static void
set_points(wp_linux_drm_syncobj_surface_v1* syncobj,
	wp_linux_drm_syncobj_timeline_v1* acquire, uint64_t acquirePoint,
	wp_linux_drm_syncobj_timeline_v1* release, uint64_t releasePoint)
{
	wp_linux_drm_syncobj_surface_v1_set_acquire_point(syncobj, acquire,
		acquirePoint >> 32, (uint32_t)acquirePoint);
	wp_linux_drm_syncobj_surface_v1_set_release_point(syncobj, release,
		releasePoint >> 32, (uint32_t)releasePoint);
}


// #pragma mark - dmabuf


struct Feedback {
	uint32_t	tableSize = 0;
	int			tableFd = -1;
	size_t		mainDeviceSize = 0;
	size_t		trancheFormats = 0;
	int			tranches = 0;
	bool		done = false;
};

static void fb_done(void* data, zwp_linux_dmabuf_feedback_v1*)
	{ ((Feedback*)data)->done = true; }
static void fb_table(void* data, zwp_linux_dmabuf_feedback_v1*, int32_t fd,
	uint32_t size)
	{ ((Feedback*)data)->tableFd = fd; ((Feedback*)data)->tableSize = size; }
static void fb_main(void* data, zwp_linux_dmabuf_feedback_v1*, wl_array* a)
	{ ((Feedback*)data)->mainDeviceSize = a->size; }
static void fb_tranche_done(void* data, zwp_linux_dmabuf_feedback_v1*)
	{ ((Feedback*)data)->tranches++; }
static void fb_target(void*, zwp_linux_dmabuf_feedback_v1*, wl_array*) {}
static void fb_formats(void* data, zwp_linux_dmabuf_feedback_v1*,
	wl_array* a)
	{ ((Feedback*)data)->trancheFormats = a->size / sizeof(uint16_t); }
static void fb_flags(void*, zwp_linux_dmabuf_feedback_v1*, uint32_t) {}

static const zwp_linux_dmabuf_feedback_v1_listener kFeedbackListener = {
	fb_done, fb_table, fb_main, fb_tranche_done, fb_target, fb_formats,
	fb_flags
};


struct ParamsResult {
	bool created = false;
	bool failed = false;
};

static void params_created(void* data, zwp_linux_buffer_params_v1*,
	wl_buffer*) { ((ParamsResult*)data)->created = true; }
static void params_failed(void* data, zwp_linux_buffer_params_v1*)
	{ ((ParamsResult*)data)->failed = true; }

static const zwp_linux_buffer_params_v1_listener kParamsListener = {
	params_created, params_failed
};


static void
test_dmabuf(Connection& connection)
{
	report("dmabuf.version", connection.dmabufVersion >= 4,
		"zwp_linux_dmabuf_v1 version %u", connection.dmabufVersion);
	if (connection.dmabuf == NULL)
		return;

	Feedback feedback;
	zwp_linux_dmabuf_feedback_v1* object
		= zwp_linux_dmabuf_v1_get_default_feedback(connection.dmabuf);
	zwp_linux_dmabuf_feedback_v1_add_listener(object, &kFeedbackListener,
		&feedback);
	wl_display_roundtrip(connection.display);
	bool tableOK = false;
	if (feedback.tableFd >= 0 && feedback.tableSize == 64) {
		void* table = mmap(NULL, feedback.tableSize, PROT_READ, MAP_PRIVATE,
			feedback.tableFd, 0);
		if (table != MAP_FAILED) {
			tableOK = ((uint32_t*)table)[0] == 0x34325241;
			munmap(table, feedback.tableSize);
		}
		close(feedback.tableFd);
	}
	report("dmabuf.feedback", feedback.done && tableOK
			&& feedback.mainDeviceSize == 8 && feedback.trancheFormats == 4
			&& feedback.tranches == 1,
		"done %d, table %u bytes (readable %d), main device %zu bytes,"
		" tranches %d with %zu formats", feedback.done, feedback.tableSize,
		tableOK, feedback.mainDeviceSize, feedback.tranches,
		feedback.trancheFormats);
	zwp_linux_dmabuf_feedback_v1_destroy(object);

	wl_buffer* buffer = dmabuf_buffer(connection);
	wl_surface* surface = wl_compositor_create_surface(connection.compositor);
	wl_surface_attach(surface, buffer, 0, 0);
	wl_surface_commit(surface);
	bool healthy = wl_display_roundtrip(connection.display) >= 0
		&& wl_display_get_error(connection.display) == 0;
	report("dmabuf.create_immed", buffer != NULL && healthy,
		"memfd buffer shown, connection %s", healthy ? "healthy" : "broken");
	wl_surface_destroy(surface);
	wl_buffer_destroy(buffer);

	int fd = memfd(64 * 16);
	zwp_linux_buffer_params_v1* params
		= zwp_linux_dmabuf_v1_create_params(connection.dmabuf);
	ParamsResult result;
	zwp_linux_buffer_params_v1_add_listener(params, &kParamsListener,
		&result);
	zwp_linux_buffer_params_v1_add(params, fd, 0, 0, 64, 0, 0);
	close(fd);
	zwp_linux_buffer_params_v1_create(params, 16, 16, 0x3231564e /* NV12 */,
		0);
	wl_display_roundtrip(connection.display);
	report("dmabuf.create_failed", result.failed && !result.created,
		"failed %d, created %d", result.failed, result.created);
	zwp_linux_buffer_params_v1_destroy(params);

	Connection bad;
	connect(bad);
	dmabuf_buffer(bad, 4096);
	uint32_t error = protocol_error(bad);
	report("dmabuf.out_of_bounds",
		error == ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_OUT_OF_BOUNDS,
		"error %u", error);
	disconnect(bad);
}


// #pragma mark - syncobj


static void
test_syncobj(Connection& connection)
{
	report("syncobj.version", connection.syncobjVersion == 1,
		"wp_linux_drm_syncobj_manager_v1 version %u",
		connection.syncobjVersion);
	if (connection.syncobj == NULL || connection.dmabuf == NULL)
		return;

	uint32_t acquire = timeline_create();
	uint32_t releaseA = timeline_create();
	uint32_t releaseB = timeline_create();
	wp_linux_drm_syncobj_timeline_v1* acquireTimeline
		= import_timeline(connection, acquire);
	wp_linux_drm_syncobj_timeline_v1* releaseATimeline
		= import_timeline(connection, releaseA);
	wp_linux_drm_syncobj_timeline_v1* releaseBTimeline
		= import_timeline(connection, releaseB);

	wl_surface* surface = wl_compositor_create_surface(connection.compositor);
	wp_linux_drm_syncobj_surface_v1* syncobj
		= wp_linux_drm_syncobj_manager_v1_get_surface(connection.syncobj,
			surface);
	wl_buffer* bufferA = dmabuf_buffer(connection);
	wl_buffer* bufferB = dmabuf_buffer(connection);

	// A: acquire point 1 signalled up front, so it is applied at once
	timeline_signal(acquire, 1);
	wl_surface_attach(surface, bufferA, 0, 0);
	set_points(syncobj, acquireTimeline, 1, releaseATimeline, 1);
	wl_surface_commit(surface);

	// B: acquire point 2 not signalled yet
	wl_surface_attach(surface, bufferB, 0, 0);
	set_points(syncobj, acquireTimeline, 2, releaseBTimeline, 1);
	wl_surface_commit(surface);
	wl_display_roundtrip(connection.display);
	snooze(100000);
	wl_display_roundtrip(connection.display);
	bool aKept = !timeline_signalled(releaseA, 1, 0);
	report("syncobj.deferred", aKept && wl_display_get_error(
			connection.display) == 0,
		"buffer A %s while B waits for its acquire point",
		aKept ? "stays on screen" : "was released early");

	// the client's GPU work "finishes": no request, only the signal
	bigtime_t start = system_time();
	timeline_signal(acquire, 2);
	bool aReleased = timeline_signalled(releaseA, 1, 2000000);
	bigtime_t latency = system_time() - start;
	bool bShown = !timeline_signalled(releaseB, 1, 0);
	report("syncobj.applied", aReleased && bShown && latency < 1000000,
		"A's release point %s after %lld us, B on screen %d",
		aReleased ? "signalled" : "not signalled", (long long)latency,
		bShown);

	wl_surface_destroy(surface);
	wl_display_roundtrip(connection.display);
	bool bReleased = timeline_signalled(releaseB, 1, 1000000);
	report("syncobj.surface_gone", bReleased,
		"B's release point %s with the surface gone",
		bReleased ? "signalled" : "not signalled");
	wp_linux_drm_syncobj_surface_v1_destroy(syncobj);
	wl_buffer_destroy(bufferA);
	wl_buffer_destroy(bufferB);

	// protocol errors, each on its own connection
	std::string detail;
	bool ok = true;
	for (int test = 0; test < 5; test++) {
		Connection bad;
		connect(bad);
		wl_surface* badSurface = wl_compositor_create_surface(bad.compositor);
		wp_linux_drm_syncobj_surface_v1* badSyncobj
			= wp_linux_drm_syncobj_manager_v1_get_surface(bad.syncobj,
				badSurface);
		wp_linux_drm_syncobj_timeline_v1* timeline
			= import_timeline(bad, timeline_create());
		uint32_t expected = 0;
		const char* name = "";
		switch (test) {
			case 0:
			{
				name = "unsupported_buffer";
				expected = WP_LINUX_DRM_SYNCOBJ_SURFACE_V1_ERROR_UNSUPPORTED_BUFFER;
				int fd = memfd(64 * 16);
				wl_shm_pool* pool = wl_shm_create_pool(bad.shm, fd, 64 * 16);
				close(fd);
				wl_buffer* shmBuffer = wl_shm_pool_create_buffer(pool, 0, 16,
					16, 64, WL_SHM_FORMAT_XRGB8888);
				wl_surface_attach(badSurface, shmBuffer, 0, 0);
				set_points(badSyncobj, timeline, 1, timeline, 2);
				wl_surface_commit(badSurface);
				break;
			}
			case 1:
				name = "no_acquire_point";
				expected = WP_LINUX_DRM_SYNCOBJ_SURFACE_V1_ERROR_NO_ACQUIRE_POINT;
				wl_surface_attach(badSurface, dmabuf_buffer(bad), 0, 0);
				wl_surface_commit(badSurface);
				break;
			case 2:
				name = "conflicting_points";
				expected = WP_LINUX_DRM_SYNCOBJ_SURFACE_V1_ERROR_CONFLICTING_POINTS;
				wl_surface_attach(badSurface, dmabuf_buffer(bad), 0, 0);
				set_points(badSyncobj, timeline, 3, timeline, 3);
				wl_surface_commit(badSurface);
				break;
			case 3:
				name = "surface_exists";
				expected = WP_LINUX_DRM_SYNCOBJ_MANAGER_V1_ERROR_SURFACE_EXISTS;
				wp_linux_drm_syncobj_manager_v1_get_surface(bad.syncobj,
					badSurface);
				break;
			case 4:
			{
				name = "invalid_timeline";
				expected = WP_LINUX_DRM_SYNCOBJ_MANAGER_V1_ERROR_INVALID_TIMELINE;
				int fd = memfd(16);
				wp_linux_drm_syncobj_manager_v1_import_timeline(bad.syncobj,
					fd);
				close(fd);
				break;
			}
		}
		uint32_t error = protocol_error(bad);
		bool pass = error == expected
			&& wl_display_get_error(bad.display) == EPROTO;
		ok = ok && pass;
		char line[96];
		snprintf(line, sizeof(line), "%s%s %u (want %u)",
			detail.empty() ? "" : ", ", name, error, expected);
		detail += line;
		disconnect(bad);
	}
	report("syncobj.errors", ok, "%s", detail.c_str());

	// the compositor is still serving the first client
	report("syncobj.healthy", wl_display_roundtrip(connection.display) >= 0
		&& wl_display_get_error(connection.display) == 0, "");
}


int
main()
{
	printf("CE-SUITE %s START\n", SUITE);
	fflush(stdout);
	BApplication application("application/x-vnd.Renga-WaylandGpuSyncTest");

	if (getenv("XDG_RUNTIME_DIR") == NULL)
		setenv("XDG_RUNTIME_DIR", "/tmp/xdg-runtime", 1);
	std::string socketPath = std::string(getenv("XDG_RUNTIME_DIR"))
		+ "/wayland-0";
	if (!be_roster->IsRunning(kCompositorSignature))
		be_roster->Launch(kCompositorSignature);
	for (int i = 0; i < 100 && access(socketPath.c_str(), F_OK) != 0; i++)
		snooze(100000);

	sNode = open("/dev/syncobj", O_RDWR | O_CLOEXEC);

	Connection connection;
	if (!connect(connection)) {
		report("connect", false, "wl_display_connect: %s", strerror(errno));
		printf("CE-SUITE %s DONE failures=%d\n", SUITE, sFailures);
		return 1;
	}

	test_dmabuf(connection);
	test_syncobj(connection);

	disconnect(connection);
	printf("CE-SUITE %s DONE failures=%d\n", SUITE, sFailures);
	return sFailures == 0 ? 0 : 1;
}
