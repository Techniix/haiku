/*
 * Renga: linux-dmabuf (zwp_linux_dmabuf_v1, version 4) and explicit
 * synchronisation (wp_linux_drm_syncobj_manager_v1) for the compositor.
 * Copyright 2026, Renga contributors. Distributed under the terms of the
 * MIT License.
 */
#pragma once

#include "WlGlobal.h"

#include <Referenceable.h>
#include <SupportDefs.h>


class HaikuShmBuffer;
class HaikuSurface;
class HaikuSyncobjSurface;


/*!	A client's DRM syncobj timeline, imported into the compositor's own
	/dev/syncobj descriptor (one handle per timeline object).
*/
class HaikuSyncTimeline: public BReferenceable {
public:
	explicit HaikuSyncTimeline(uint32 handle): fHandle(handle) {}
	virtual ~HaikuSyncTimeline();
	uint32 Handle() const {return fHandle;}

private:
	uint32 fHandle;
};


class HaikuDmabufGlobal: public WlGlocal {
public:
	static HaikuDmabufGlobal *Create(struct wl_display *display);
	virtual ~HaikuDmabufGlobal() = default;
	void Bind(struct wl_client *wl_client, uint32_t version, uint32_t id) override;
};


class HaikuSyncobjManagerGlobal: public WlGlocal {
public:
	//! NULL (no global) if /dev/syncobj cannot be opened.
	static HaikuSyncobjManagerGlobal *Create(struct wl_display *display);
	virtual ~HaikuSyncobjManagerGlobal() = default;
	void Bind(struct wl_client *wl_client, uint32_t version, uint32_t id) override;
};


//! Has \a point of \a timeline signalled (a fence exists and is done)?
bool HaikuSyncTimelineReady(HaikuSyncTimeline *timeline, uint64 point);
//! Signals \a point (a CPU-signalled release point).
void HaikuSyncTimelineSignal(HaikuSyncTimeline *timeline, uint64 point);
//! Writes \a eventFd once \a point has signalled (SYNCOBJ_EVENTFD).
status_t HaikuSyncTimelineWatch(HaikuSyncTimeline *timeline, uint64 point,
	int eventFd);
//! A non-blocking eventfd for HaikuSyncTimelineWatch(), or -1.
int HaikuSyncCreateEventFd();

/*!	The commit checks of wp_linux_drm_syncobj_surface_v1; posts the
	protocol error and returns false when one applies. \a syncobj may be
	NULL (the object was destroyed: no checks).
*/
bool HaikuSyncobjCheckCommit(HaikuSurface *surface,
	HaikuSyncobjSurface *syncobj, bool bufferAttached, HaikuShmBuffer *buffer,
	HaikuSyncTimeline *acquire, uint64 acquirePoint,
	HaikuSyncTimeline *release, uint64 releasePoint);

//! The surface is being destroyed.
void HaikuSyncobjSurfaceGone(HaikuSurface *surface);
