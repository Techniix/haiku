/*
 * Renga: linux-dmabuf and explicit synchronisation for the compositor.
 * Copyright 2026, Renga contributors. Distributed under the terms of the
 * MIT License.
 *
 * zwp_linux_dmabuf_v1, version 4 (stable linux-dmabuf-v1.xml):
 * - Buffers are imported by mapping their descriptor, so anything the
 *   compositor can map works: memfds and areas today, CPU-mappable GPU
 *   buffers once Quasar's buffer import exists (QUASAR.md R-18 notes).
 *   Formats ARGB8888 and XRGB8888, one plane, modifiers LINEAR and INVALID
 *   (implicit). Other formats and modifiers get "failed" (create) or the
 *   invalid_format error (create_immed), as the protocol asks.
 * - Version 4 feedback (get_default_feedback, get_surface_feedback): one
 *   format table (a sealed-by-convention memfd shared by all clients), the
 *   main device and one tranche with every entry. There is no DRM device
 *   behind these buffers, so main_device is a Linux dev_t of 0 (8 bytes,
 *   Linux's size, since linux-dmabuf clients are Linux-style: Mesa compares
 *   the array size with its dev_t); Quasar's render node replaces it.
 * - Buffers made here are HaikuShmBuffers with explicit-sync support.
 *
 * wp_linux_drm_syncobj_manager_v1, version 1 (staging):
 * - import_timeline takes a DRM syncobj descriptor (HANDLE_TO_FD) and
 *   imports it into the compositor's own /dev/syncobj descriptor.
 * - Acquire points: a commit whose point has not signalled is queued, and
 *   later commits queue behind it; SYNCOBJ_EVENTFD on an eventfd in the
 *   compositor's event loop applies them once it has, without a thread
 *   (HaikuCompositor.cpp, HaikuSurface::DeferCommit()).
 * - Release points: signalled by the compositor once the buffer is off
 *   screen (replaced, or the surface is gone). Surfaces show client memory
 *   without a copy, so that is when the compositor is done with it.
 * - All protocol errors at commit time: no_surface, unsupported_buffer
 *   (wl_shm buffers), no_buffer, no_acquire_point, no_release_point and
 *   conflicting_points.
 */
#include "HaikuGpuSync.h"
#include "HaikuCompositor.h"
#include "HaikuShm.h"
#include "LinuxDmabufV1.h"
#include "LinuxDrmSyncobjV1.h"

#include <drm/drm_syncobj.h>
#include <sys/eventfd.h>

#include <AutoDeleter.h>
#include <OS.h>
#include <syscalls.h>
#include <vm_defs.h>

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <new>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

extern const struct wl_interface zwp_linux_dmabuf_v1_interface;
extern const struct wl_interface wp_linux_drm_syncobj_manager_v1_interface;


enum {
	DMABUF_VERSION = 4,
	SYNCOBJ_VERSION = 1,
};

// DRM fourcc codes and modifiers (drm_fourcc.h)
static const uint32 kFormatArgb8888 = 0x34325241;	// 'AR24'
static const uint32 kFormatXrgb8888 = 0x34325258;	// 'XR24'
static const uint64 kModifierLinear = 0;
static const uint64 kModifierInvalid = 0x00ffffffffffffffULL;

struct FormatTableEntry {
	uint32 format;
	uint32 pad;
	uint64 modifier;
};

static const FormatTableEntry kFormatTable[] = {
	{ kFormatArgb8888, 0, kModifierLinear },
	{ kFormatXrgb8888, 0, kModifierLinear },
	{ kFormatArgb8888, 0, kModifierInvalid },
	{ kFormatXrgb8888, 0, kModifierInvalid },
};


//#pragma mark - /dev/syncobj


static int sSyncNode = -1;


static int
sync_node()
{
	if (sSyncNode < 0)
		sSyncNode = open("/dev/syncobj", O_RDWR | O_CLOEXEC);
	return sSyncNode;
}


HaikuSyncTimeline::~HaikuSyncTimeline()
{
	drm_syncobj_destroy data = {};
	data.handle = fHandle;
	ioctl(sync_node(), DRM_IOCTL_SYNCOBJ_DESTROY, &data);
}


bool
HaikuSyncTimelineReady(HaikuSyncTimeline *timeline, uint64 point)
{
	if (timeline == NULL)
		return true;
	uint32 handle = timeline->Handle();
	drm_syncobj_timeline_wait data = {};
	data.handles = (uint64)(addr_t)&handle;
	data.points = (uint64)(addr_t)&point;
	data.count_handles = 1;
	data.timeout_nsec = 0;
	return ioctl(sync_node(), DRM_IOCTL_SYNCOBJ_TIMELINE_WAIT, &data) == 0;
}


void
HaikuSyncTimelineSignal(HaikuSyncTimeline *timeline, uint64 point)
{
	if (timeline == NULL)
		return;
	uint32 handle = timeline->Handle();
	drm_syncobj_timeline_array data = {};
	data.handles = (uint64)(addr_t)&handle;
	data.points = (uint64)(addr_t)&point;
	data.count_handles = 1;
	if (ioctl(sync_node(), DRM_IOCTL_SYNCOBJ_TIMELINE_SIGNAL, &data) != 0)
		fprintf(stderr, "wayland: release point %" PRIu64 ": %s\n", point,
			strerror(errno));
}


status_t
HaikuSyncTimelineWatch(HaikuSyncTimeline *timeline, uint64 point, int eventFd)
{
	if (timeline == NULL || eventFd < 0)
		return B_BAD_VALUE;
	drm_syncobj_eventfd data = {};
	data.handle = timeline->Handle();
	data.point = point;
	data.fd = eventFd;
	return ioctl(sync_node(), DRM_IOCTL_SYNCOBJ_EVENTFD, &data) == 0
		? B_OK : errno;
}


int
HaikuSyncCreateEventFd()
{
	return eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
}


//#pragma mark - linux-dmabuf


static int sFormatTableFd = -1;


static int
format_table_fd()
{
	if (sFormatTableFd >= 0)
		return sFormatTableFd;
	int fd = memfd_create("wayland-dmabuf-formats", MFD_CLOEXEC);
	if (fd < 0)
		return -1;
	if (write(fd, kFormatTable, sizeof(kFormatTable))
			!= (ssize_t)sizeof(kFormatTable)) {
		close(fd);
		return -1;
	}
	sFormatTableFd = fd;
	return fd;
}


class HaikuDmabufFeedback: public ZwpLinuxDmabufFeedbackV1 {
public:
	void SendAll()
	{
		int table = format_table_fd();
		if (table < 0) {
			wl_client_post_no_memory(Client());
			return;
		}
		SendFormatTable(table, sizeof(kFormatTable));

		uint64 device = 0;
			// Linux dev_t: no DRM device behind CPU-mappable buffers
		struct wl_array array;
		wl_array_init(&array);
		*(uint64*)wl_array_add(&array, sizeof(device)) = device;
		SendMainDevice(&array);

		SendTrancheTargetDevice(&array);
		wl_array_release(&array);

		wl_array_init(&array);
		for (uint16 i = 0; i < B_COUNT_OF(kFormatTable); i++)
			*(uint16*)wl_array_add(&array, sizeof(uint16)) = i;
		SendTrancheFormats(&array);
		wl_array_release(&array);

		SendTrancheFlags(0);
		SendTrancheDone();
		SendDone();
	}
};


class HaikuDmabufParams: public ZwpLinuxBufferParamsV1 {
public:
	virtual ~HaikuDmabufParams()
	{
		for (int i = 0; i < 4; i++) {
			if (fPlanes[i].fd >= 0)
				close(fPlanes[i].fd);
		}
	}

	void HandleAdd(int32_t fd, uint32_t plane_idx, uint32_t offset,
		uint32_t stride, uint32_t modifier_hi, uint32_t modifier_lo) final
	{
		FileDescriptorCloser closer(fd);
		if (fUsed) {
			wl_resource_post_error(ToResource(), errorAlreadyUsed,
				"params already used");
			return;
		}
		if (plane_idx >= 4) {
			wl_resource_post_error(ToResource(), errorPlaneIdx,
				"plane %" PRIu32 " out of range", plane_idx);
			return;
		}
		if (fPlanes[plane_idx].fd >= 0) {
			wl_resource_post_error(ToResource(), errorPlaneSet,
				"plane %" PRIu32 " already set", plane_idx);
			return;
		}
		Plane &plane = fPlanes[plane_idx];
		plane.fd = closer.Detach();
		plane.offset = offset;
		plane.stride = stride;
		plane.modifier = ((uint64)modifier_hi << 32) | modifier_lo;
	}

	void HandleCreate(int32_t width, int32_t height, uint32_t format,
		uint32_t flags) final
	{
		HaikuShmBuffer *buffer = Import(0, width, height, format, flags,
			false);
		if (buffer != NULL)
			SendCreated(buffer->ToResource());
		else if (ToResource() != NULL && !fErrorPosted)
			SendFailed();
	}

	void HandleCreateImmed(uint32_t buffer_id, int32_t width, int32_t height,
		uint32_t format, uint32_t flags) final
	{
		Import(buffer_id, width, height, format, flags, true);
	}

private:
	struct Plane {
		int fd = -1;
		uint32 offset = 0;
		uint32 stride = 0;
		uint64 modifier = 0;
	};

	Plane fPlanes[4];
	bool fUsed = false;
	bool fErrorPosted = false;

	void PostError(uint32 code, const char *message)
	{
		fErrorPosted = true;
		wl_resource_post_error(ToResource(), code, "%s", message);
	}

	/*!	With \a immediate, failures the protocol leaves to the compositor
		are the invalid_wl_buffer error (create_immed has no "failed" event
		it could use for a buffer the client already named).
	*/
	HaikuShmBuffer *Import(uint32 id, int32_t width, int32_t height,
		uint32_t format, uint32_t flags, bool immediate)
	{
		if (fUsed) {
			PostError(errorAlreadyUsed, "params already used");
			return NULL;
		}
		fUsed = true;

		if (fPlanes[0].fd < 0) {
			PostError(errorIncomplete, "no plane 0");
			return NULL;
		}
		color_space colorSpace;
		if (format == kFormatArgb8888)
			colorSpace = B_RGBA32;
		else if (format == kFormatXrgb8888)
			colorSpace = B_RGB32;
		else {
			if (immediate)
				PostError(errorInvalidFormat, "unsupported format");
			return NULL;
		}
		for (int i = 1; i < 4; i++) {
			if (fPlanes[i].fd >= 0) {
				PostError(errorIncomplete, "too many planes for the format");
				return NULL;
			}
		}
		if (width <= 0 || height <= 0) {
			PostError(errorInvalidDimensions, "invalid size");
			return NULL;
		}

		Plane &plane = fPlanes[0];
		if (plane.modifier != kModifierLinear
			&& plane.modifier != kModifierInvalid) {
			if (immediate)
				PostError(errorInvalidFormat, "unsupported modifier");
			return NULL;
		}
		if (flags != 0) {
			// y_invert, interlaced, bottom_first: not supported
			if (immediate)
				PostError(errorInvalidWlBuffer, "unsupported flags");
			return NULL;
		}

		uint64 rowBytes = (uint64)width * 4;
		uint64 end = (uint64)plane.offset + (uint64)plane.stride * (height - 1)
			+ rowBytes;
		struct stat st;
		if (plane.stride < rowBytes || fstat(plane.fd, &st) != 0
			|| (st.st_size > 0 && end > (uint64)st.st_size)) {
			PostError(errorOutOfBounds, "plane exceeds the buffer");
			return NULL;
		}
		size_t size = st.st_size > 0 ? (size_t)st.st_size : (size_t)end;

		void *address = NULL;
		AreaDeleter area(_kern_map_file("wl_dmabuf", &address, B_ANY_ADDRESS,
			size, B_READ_AREA | B_WRITE_AREA | B_CLONEABLE_AREA,
			REGION_NO_PRIVATE_MAP, true, plane.fd, 0));
		if (!area.IsSet()) {
			if (immediate)
				PostError(errorInvalidWlBuffer, "cannot map the buffer");
			return NULL;
		}
		BReference<HaikuShmAreaRef> areaRef(
			new(std::nothrow) HaikuShmAreaRef(area.Get()), true);
		if (!areaRef.IsSet()) {
			wl_client_post_no_memory(Client());
			return NULL;
		}
		area.Detach();

		HaikuShmBuffer *buffer = new(std::nothrow) HaikuShmBuffer();
		if (buffer == NULL) {
			wl_client_post_no_memory(Client());
			return NULL;
		}
		if (!buffer->Init(Client(), 1, id))
			return NULL;
		if (buffer->InitForArea(areaRef, plane.offset, width, height,
				plane.stride, colorSpace, true) != B_OK) {
			buffer->Destroy();
			if (immediate)
				PostError(errorInvalidWlBuffer, "cannot use the buffer");
			return NULL;
		}
		return buffer;
	}
};


class HaikuDmabuf: public ZwpLinuxDmabufV1 {
public:
	void HandleCreateParams(uint32_t params_id) final
	{
		HaikuDmabufParams *params = new(std::nothrow) HaikuDmabufParams();
		if (params == NULL) {
			wl_client_post_no_memory(Client());
			return;
		}
		params->Init(Client(), Version(), params_id);
	}

	void HandleGetDefaultFeedback(uint32_t id) final
	{
		CreateFeedback(id);
	}

	void HandleGetSurfaceFeedback(uint32_t id, struct wl_resource *surface)
		final
	{
		// the same for every surface: no scan-out tranche yet
		CreateFeedback(id);
	}

private:
	void CreateFeedback(uint32_t id)
	{
		HaikuDmabufFeedback *feedback
			= new(std::nothrow) HaikuDmabufFeedback();
		if (feedback == NULL) {
			wl_client_post_no_memory(Client());
			return;
		}
		if (!feedback->Init(Client(), Version(), id))
			return;
		feedback->SendAll();
	}
};


HaikuDmabufGlobal *HaikuDmabufGlobal::Create(struct wl_display *display)
{
	ObjectDeleter<HaikuDmabufGlobal> global(
		new(std::nothrow) HaikuDmabufGlobal());
	if (!global.IsSet()) return NULL;
	if (!global->Init(display, &zwp_linux_dmabuf_v1_interface,
			DMABUF_VERSION)) {
		return NULL;
	}
	return global.Detach();
}


void HaikuDmabufGlobal::Bind(struct wl_client *wl_client, uint32_t version,
	uint32_t id)
{
	HaikuDmabuf *dmabuf = new(std::nothrow) HaikuDmabuf();
	if (dmabuf == NULL) {
		wl_client_post_no_memory(wl_client);
		return;
	}
	if (!dmabuf->Init(wl_client, version, id))
		return;
	if (version < 4) {
		// formats and modifiers events (deprecated from version 4)
		for (const FormatTableEntry &entry : kFormatTable) {
			if (entry.modifier == kModifierInvalid) {
				dmabuf->SendFormat(entry.format);
				continue;
			}
			if (version >= 3) {
				dmabuf->SendModifier(entry.format,
					(uint32)(entry.modifier >> 32), (uint32)entry.modifier);
			}
		}
	}
}


//#pragma mark - wp_linux_drm_syncobj_v1


class HaikuSyncobjTimelineResource: public WpLinuxDrmSyncobjTimelineV1 {
public:
	BReference<HaikuSyncTimeline> fTimeline;
};


class HaikuSyncobjSurface: public WpLinuxDrmSyncobjSurfaceV1 {
public:
	HaikuSurface *fSurface = NULL;

	virtual ~HaikuSyncobjSurface()
	{
		if (fSurface != NULL) {
			fSurface->fSyncobjSurface = NULL;
			// points set since the last commit may be discarded
			fSurface->fPendingState.acquireTimeline.Unset();
			fSurface->fPendingState.releaseTimeline.Unset();
		}
	}

	void HandleSetAcquirePoint(struct wl_resource *timeline, uint32_t point_hi,
		uint32_t point_lo) final
	{
		if (fSurface == NULL) {
			wl_resource_post_error(ToResource(), errorNoSurface,
				"the surface is gone");
			return;
		}
		HaikuSyncobjTimelineResource *resource
			= (HaikuSyncobjTimelineResource*)WlResource::FromResource(
				timeline);
		fSurface->fPendingState.acquireTimeline = resource->fTimeline;
		fSurface->fPendingState.acquirePoint
			= ((uint64)point_hi << 32) | point_lo;
	}

	void HandleSetReleasePoint(struct wl_resource *timeline, uint32_t point_hi,
		uint32_t point_lo) final
	{
		if (fSurface == NULL) {
			wl_resource_post_error(ToResource(), errorNoSurface,
				"the surface is gone");
			return;
		}
		HaikuSyncobjTimelineResource *resource
			= (HaikuSyncobjTimelineResource*)WlResource::FromResource(
				timeline);
		fSurface->fPendingState.releaseTimeline = resource->fTimeline;
		fSurface->fPendingState.releasePoint
			= ((uint64)point_hi << 32) | point_lo;
	}
};


bool
HaikuSyncobjCheckCommit(HaikuSurface *surface, HaikuSyncobjSurface *syncobj,
	bool bufferAttached, HaikuShmBuffer *buffer, HaikuSyncTimeline *acquire,
	uint64 acquirePoint, HaikuSyncTimeline *release, uint64 releasePoint)
{
	if (syncobj == NULL)
		return true;
	struct wl_resource *resource = syncobj->ToResource();

	if (!bufferAttached) {
		if (acquire != NULL || release != NULL) {
			wl_resource_post_error(resource,
				WpLinuxDrmSyncobjSurfaceV1::errorNoBuffer,
				"timeline point set without a buffer");
			return false;
		}
		return true;
	}
	if (!buffer->SupportsExplicitSync()) {
		wl_resource_post_error(resource,
			WpLinuxDrmSyncobjSurfaceV1::errorUnsupportedBuffer,
			"the buffer does not support explicit sync (wl_shm)");
		return false;
	}
	if (acquire == NULL) {
		wl_resource_post_error(resource,
			WpLinuxDrmSyncobjSurfaceV1::errorNoAcquirePoint,
			"buffer without an acquire point");
		return false;
	}
	if (release == NULL) {
		wl_resource_post_error(resource,
			WpLinuxDrmSyncobjSurfaceV1::errorNoReleasePoint,
			"buffer without a release point");
		return false;
	}
	if (acquire == release && acquirePoint >= releasePoint) {
		wl_resource_post_error(resource,
			WpLinuxDrmSyncobjSurfaceV1::errorConflictingPoints,
			"the release point must come after the acquire point");
		return false;
	}
	return true;
}


void
HaikuSyncobjSurfaceGone(HaikuSurface *surface)
{
	if (surface->fSyncobjSurface != NULL) {
		surface->fSyncobjSurface->fSurface = NULL;
		surface->fSyncobjSurface = NULL;
	}
}


class HaikuSyncobjManager: public WpLinuxDrmSyncobjManagerV1 {
public:
	void HandleGetSurface(uint32_t id, struct wl_resource *surfaceResource)
		final
	{
		HaikuSurface *surface = HaikuSurface::FromResource(surfaceResource);
		if (surface->fSyncobjSurface != NULL) {
			wl_resource_post_error(ToResource(), errorSurfaceExists,
				"the surface has a syncobj surface already");
			return;
		}
		HaikuSyncobjSurface *syncobj = new(std::nothrow) HaikuSyncobjSurface();
		if (syncobj == NULL) {
			wl_client_post_no_memory(Client());
			return;
		}
		if (!syncobj->Init(Client(), Version(), id))
			return;
		syncobj->fSurface = surface;
		surface->fSyncobjSurface = syncobj;
	}

	void HandleImportTimeline(uint32_t id, int32_t fd) final
	{
		FileDescriptorCloser closer(fd);
		drm_syncobj_handle data = {};
		data.fd = fd;
		if (ioctl(sync_node(), DRM_IOCTL_SYNCOBJ_FD_TO_HANDLE, &data) != 0) {
			wl_resource_post_error(ToResource(), errorInvalidTimeline,
				"not a DRM syncobj timeline: %s", strerror(errno));
			return;
		}
		BReference<HaikuSyncTimeline> timeline(
			new(std::nothrow) HaikuSyncTimeline(data.handle), true);
		HaikuSyncobjTimelineResource *resource
			= new(std::nothrow) HaikuSyncobjTimelineResource();
		if (!timeline.IsSet() || resource == NULL) {
			delete resource;
			wl_client_post_no_memory(Client());
			return;
		}
		if (!resource->Init(Client(), Version(), id))
			return;
		resource->fTimeline = timeline;
	}
};


HaikuSyncobjManagerGlobal *HaikuSyncobjManagerGlobal::Create(
	struct wl_display *display)
{
	if (sync_node() < 0) {
		fprintf(stderr, "wayland: /dev/syncobj: %s; no explicit sync\n",
			strerror(errno));
		return NULL;
	}
	ObjectDeleter<HaikuSyncobjManagerGlobal> global(
		new(std::nothrow) HaikuSyncobjManagerGlobal());
	if (!global.IsSet()) return NULL;
	if (!global->Init(display, &wp_linux_drm_syncobj_manager_v1_interface,
			SYNCOBJ_VERSION)) {
		return NULL;
	}
	return global.Detach();
}


void HaikuSyncobjManagerGlobal::Bind(struct wl_client *wl_client,
	uint32_t version, uint32_t id)
{
	HaikuSyncobjManager *manager = new(std::nothrow) HaikuSyncobjManager();
	if (manager == NULL) {
		wl_client_post_no_memory(wl_client);
		return;
	}
	manager->Init(wl_client, version, id);
}
