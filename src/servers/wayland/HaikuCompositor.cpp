#include "HaikuCompositor.h"
#include "HaikuSubcompositor.h"
#include "HaikuShm.h"
#include "HaikuXdgSurface.h"
#include "HaikuXdgToplevel.h"
#include "HaikuXdgPopup.h"
#include "HaikuSeat.h"
#include "HaikuDataDeviceManager.h"
#include "HaikuOutput.h"
#include "RengaProtocols.h"
#include "HaikuServerDecoration.h"
#include "HaikuGpuSync.h"
#include <unistd.h>
#include <Window.h>
#include "Wayland.h"
#include "WaylandEnv.h"
#include <wayland-server-core.h>
#include <wayland-server-protocol.h>
#include <stdio.h>
#include <bit>

#include "AppKitPtrs.h"
#include <Application.h>
#include <View.h>
#include <Window.h>
#include <Bitmap.h>
#include <Region.h>
#include <Cursor.h>

extern const struct wl_interface wl_compositor_interface;


#define COMPOSITOR_VERSION 4

static void Assert(bool cond) {if (!cond) abort();}


//#pragma mark - HaikuRegion

class HaikuRegion: public WlRegion {
private:
	BRegion fRegion;

public:
	virtual ~HaikuRegion() = default;
	static HaikuRegion *FromResource(struct wl_resource *resource) {return (HaikuRegion*)WlResource::FromResource(resource);}

	const BRegion &Region() {return fRegion;}

	void HandleAdd(int32_t x, int32_t y, int32_t width, int32_t height) final;
	void HandleSubtract(int32_t x, int32_t y, int32_t width, int32_t height) final;
};

void HaikuRegion::HandleAdd(int32_t x, int32_t y, int32_t width, int32_t height)
{
	width = std::min(width, 1 << 24);
	height = std::min(height, 1 << 24);
	fRegion.Include(BRect(x, y, x + width - 1, y + height - 1));
}

void HaikuRegion::HandleSubtract(int32_t x, int32_t y, int32_t width, int32_t height)
{
	width = std::min(width, 1 << 24);
	height = std::min(height, 1 << 24);
	fRegion.Exclude(BRect(x, y, x + width - 1, y + height - 1));
}


//#pragma mark - HaikuCompositor

class HaikuCompositor: public WlCompositor {
protected:
	virtual ~HaikuCompositor() = default;

public:
	void HandleCreateSurface(uint32_t id) override;
	void HandleCreateRegion(uint32_t id) override;
};


HaikuCompositorGlobal *HaikuCompositorGlobal::Create(struct wl_display *display)
{
	ObjectDeleter<HaikuCompositorGlobal> global(new(std::nothrow) HaikuCompositorGlobal());
	if (!global.IsSet()) return NULL;
	if (!global->Init(display, &wl_compositor_interface, COMPOSITOR_VERSION)) return NULL;
	return global.Detach();
}

void HaikuCompositorGlobal::Bind(struct wl_client *wl_client, uint32_t version, uint32_t id)
{
	HaikuCompositor *manager = new(std::nothrow) HaikuCompositor();
	if (manager == NULL) {
		wl_client_post_no_memory(wl_client);
		return;
	}
	if (!manager->Init(wl_client, version, id)) {
		return;
	}
}


void HaikuCompositor::HandleCreateSurface(uint32_t id)
{
	HaikuSurface *surface = HaikuSurface::Create(Client(), Version(), id);
}

void HaikuCompositor::HandleCreateRegion(uint32_t id)
{
	HaikuRegion *region = new(std::nothrow) HaikuRegion();
	if (region == NULL) {
		wl_client_post_no_memory(Client());
		return;
	}
	if (!region->Init(Client(), Version(), id)) {
		return;
	}
}


//#pragma mark - WaylandView

class WaylandView: public BView {
private:
	friend class HaikuSurface;

	HaikuSurface *fSurface;
	uint32 fOldMouseBtns = 0;
	bool fFramerateLimitDisabled = false;
	WaylandEnv *fActiveWlEnv {};

public:
	WaylandView(HaikuSurface *surface);
	virtual ~WaylandView();

	HaikuSurface *Surface() {return fSurface;}

	void RemoveDescendantsAndSelf();

	void WindowActivated(bool active) final;
	void MessageReceived(BMessage *msg) final;
	void Draw(BRect dirty);
	void Pulse(void);
};


WaylandView::WaylandView(HaikuSurface *surface):
	BView(BRect(), "WaylandView", B_FOLLOW_NONE, B_WILL_DRAW | B_TRANSPARENT_BACKGROUND | B_INPUT_METHOD_AWARE | B_PULSE_NEEDED),
	fSurface(surface)
{
	SetViewColor(B_TRANSPARENT_COLOR);

	char *envValue = getenv("HAIWAY_FRAMERATE_LIMIT");
	if (envValue != NULL) {
		fFramerateLimitDisabled = (strcmp(envValue, "disabled") == 0);
		if (fFramerateLimitDisabled) {
			SetFlags(Flags() & ~B_PULSE_NEEDED);
			fprintf(stderr, "Frame rate limiting disabled\n");
		}
	}
}

WaylandView::~WaylandView()
{
	if (fSurface != NULL) {
		debugger("[!] ~WaylandView: bad deletion");
	}
}

void WaylandView::RemoveDescendantsAndSelf()
{
	while (BView* child = ChildAt(0)) {
		WaylandView* view = dynamic_cast<WaylandView*>(child);
		view->RemoveDescendantsAndSelf();
	}

	if (fActiveWlEnv != NULL)
		WaylandEnv::Wait(&fActiveWlEnv, Looper());
	RemoveSelf();
}

void WaylandView::WindowActivated(bool active)
{
	WaylandEnv wlEnv(this, &fActiveWlEnv);
	HaikuSeatGlobal *seat = HaikuGetSeat(fSurface->Client());
	if (seat == NULL) return;

	if (fSurface->Subsurface() != NULL) {
		return;
	}

	seat->SetKeyboardFocus(fSurface, active);
}

void WaylandView::MessageReceived(BMessage *msg)
{
	{
		WaylandEnv wlEnv(this, &fActiveWlEnv);

		HaikuSeatGlobal *seat = HaikuGetSeat(fSurface->Client());
		if (seat != NULL) {
			if (msg->what == B_COPY_TARGET) {
				entry_ref dirRef;
				BString name;
				if (msg->FindRef("directory", &dirRef) == B_OK && msg->FindString("name", &name) == B_OK) {
					if (seat->DataDevice() != NULL) {
						seat->DataDevice()->HandleCopyTargetReply(msg, dirRef, name);
						return;
					}
				}
			}

			bool isPointerMessage = true;
			BPoint where;
			if (msg->WasDropped()) {
				where = msg->DropPoint();
				AppKitPtrs::LockedPtr(this)->ConvertFromScreen(&where);
			} else if (msg->FindPoint("be:view_where", &where) < B_OK) {
				isPointerMessage = false;
			}
			HaikuSurface *surface = fSurface;
			while (isPointerMessage && !surface->InputRgnContains(where) && surface->Subsurface() != NULL) {
				surface = surface->Subsurface()->Parent();
			}
			if (surface != fSurface) {
				BPoint offset = AppKitPtrs::LockedPtr(fSurface->View())->Frame().LeftTop() - AppKitPtrs::LockedPtr(surface->View())->Frame().LeftTop();
				msg->ReplacePoint("be:view_where", where + offset);
			}
			if (seat->MessageReceived(surface, msg)) {
				return;
			}
		}
	}
	BView::MessageReceived(msg);
}

void WaylandView::Draw(BRect dirty)
{
	WaylandEnv wlEnv(this, &fActiveWlEnv);

	BBitmap *bmp = fSurface->Bitmap();
	if (bmp != NULL) {
		auto viewLocked = AppKitPtrs::LockedPtr(this);
		drawing_mode mode;
		switch (bmp->ColorSpace()) {
			case B_RGBA64:
			case B_RGBA32:
			case B_RGBA15:
			case B_RGBA64_BIG:
			case B_RGBA32_BIG:
			case B_RGBA15_BIG:
				mode = B_OP_ALPHA;
				break;
			default:
				mode = B_OP_COPY;
				break;
		}
		if (mode == B_OP_ALPHA && fSurface->fState.opaqueRgn.has_value()) {
			viewLocked->ConstrainClippingRegion(&fSurface->fState.opaqueRgn.value());
			viewLocked->SetDrawingMode(B_OP_COPY);
			viewLocked->DrawBitmap(bmp);
			BRegion remaining = viewLocked->Bounds();
			remaining.Exclude(&fSurface->fState.opaqueRgn.value());
			viewLocked->ConstrainClippingRegion(&remaining);
		}
		viewLocked->SetDrawingMode(mode);
		const auto &viewportSrc = fSurface->fState.viewportSrc;
		const auto &viewportDst = fSurface->fState.viewportDst;
		if (viewportSrc.IsValid() && viewportDst.IsValid()) {
			BRect bitmapRect(viewportSrc.x, viewportSrc.y, viewportSrc.x + viewportSrc.width - 1, viewportSrc.y + viewportSrc.height - 1);
			BRect viewRect(0, 0, viewportDst.width - 1, viewportDst.height - 1);
			viewLocked->DrawBitmap(bmp, bitmapRect, viewRect);
		} else {
			viewLocked->DrawBitmap(bmp);
		}
	}

	if (fSurface && fFramerateLimitDisabled)
		fSurface->CallFrameCallbacks();
}

void WaylandView::Pulse(void)
{
	if (fSurface && !fFramerateLimitDisabled)
		fSurface->CallFrameCallbacks();
}

//#pragma mark - HaikuSurface

HaikuSurface::FrameCallback *HaikuSurface::FrameCallback::Create(struct wl_client *client, uint32_t version, uint32_t id)
{
	FrameCallback *callback = new(std::nothrow) FrameCallback();
	if (!callback->Init(client, version, id)) {
		return NULL;
	}
	return callback;
}

HaikuSurface *HaikuSurface::Create(struct wl_client *client, uint32_t version, uint32_t id)
{
	HaikuSurface *surface = new(std::nothrow) HaikuSurface();
	if (!surface->Init(client, version, id)) {
		return NULL;
	}
	return surface;
}

HaikuSurface::~HaikuSurface()
{
	// Renga: explicit sync. Queued commits are dropped (their buffers were
	// never shown: signal their release points), the buffer on screen is
	// released, and the syncobj object forgets the surface (no_surface).
	HaikuSyncobjSurfaceGone(this);
	while (!fDeferred.empty()) {
		DeferredCommit *deferred = fDeferred.front();
		fDeferred.pop_front();
		SignalRelease(deferred->state);
		delete deferred;
	}
	SignalRelease(fState);
	if (fSyncEventSource != NULL)
		wl_event_source_remove(fSyncEventSource);
	if (fSyncEventFd >= 0)
		close(fSyncEventFd);

	HaikuOutputSurfaceGone(this);
	HaikuPointerConstraintsSurfaceGone(this);

	HaikuSeatGlobal *seat = HaikuGetSeat(Client());
	if (seat != NULL) {
		seat->SetPointerFocus(this, false, BMessage());
		seat->SetKeyboardFocus(this, false);
	}

	if (fView != NULL) {
		Detach();
	}

	for (HaikuSubsurface *subsurface = SurfaceList().First(); subsurface != NULL; subsurface = SurfaceList().GetNext(subsurface)) {
		subsurface->fParent = NULL;
	}
}

BSize HaikuSurface::Size() const
{
	if (fState.viewportDst.IsValid()) {
		return BSize(fState.viewportDst.width - 1, fState.viewportDst.height - 1);
	}
	if (Bitmap() != NULL) {
		return BSize(Bitmap()->Bounds().Width(), Bitmap()->Bounds().Height());
	}
	return BSize(-1, -1);
}

void HaikuSurface::AttachWindow(BWindow *window)
{
	Assert(fView == NULL);

	fView = new WaylandView(this);
	window->AddChild(fView);
	fView->MakeFocus();
}

void HaikuSurface::AttachView(BView *view)
{
	if (view == NULL) {
		fprintf(stderr, "[!] HaikuSurface::AttachView(): view == NULL\n");
		return;
	}
	fView = new WaylandView(this);
	view->AddChild(fView);
}

void HaikuSurface::AttachViewsToEarlierSubsurfaces()
{
	if (fView == NULL) {
		fprintf(stderr, "[!] HaikuSurface::AttachViewsToEarlierSubsurfaces(): fView == NULL\n");
		return;
	}
	for (HaikuSubsurface *subsurface = SurfaceList().First(); subsurface != NULL; subsurface = SurfaceList().GetNext(subsurface)) {
		subsurface->Surface()->AttachView(fView);
	}
}

void HaikuSurface::Detach()
{
	if (fView == NULL) {
		return;
	}

	fView->LockLooper();
	BLooper *looper = fView->Looper();
	fView->RemoveDescendantsAndSelf();

	fView->fSurface = NULL;
	delete fView;
	fView = NULL;

	if (looper != NULL) {
		looper->Unlock();
	}
}

void HaikuSurface::Invalidate()
{
	if (fView == NULL) {
		return;
	}
	auto viewLocked = AppKitPtrs::LockedPtr(fView);
	if (fSubsurface != NULL) {
		viewLocked->Invalidate();
	} else {
		viewLocked->Invalidate(&fDirty);
	}
	fDirty.MakeEmpty();
}

void HaikuSurface::CallFrameCallbacks()
{
	while (!fState.frameCallbacks.IsEmpty()) {
		FrameCallback *callback = fState.frameCallbacks.RemoveHead();
		callback->SendDone(system_time()/1000);
		callback->Destroy();
	}
}

bool HaikuSurface::WantsServerFrame()
{
	if (fXdgDecorationMode != decorationNone)
		return fXdgDecorationMode == decorationServer;
	return fServerDecoration != NULL
		&& fServerDecoration->Mode() == OrgKdeKwinServerDecoration::modeServer;
}

bool HaikuSurface::HasDecorationProtocol()
{
	return fXdgDecorationMode != decorationNone || fServerDecoration != NULL;
}

void HaikuSurface::SetXdgDecorationMode(int32 mode)
{
	if (mode == fXdgDecorationMode)
		return;
	fXdgDecorationMode = mode;
	// A window already on screen changes its look now; one not shown yet
	// gets it at its first commit (XdgSurfaceHook::HandleCommit()).
	if (fXdgSurface == NULL || fXdgSurface->Window() == NULL
		|| !fXdgSurface->fSurfaceInitalized)
		return;
	fXdgSurface->Window()->SetLook(
		WantsServerFrame() ? B_TITLED_WINDOW_LOOK : B_NO_BORDER_WINDOW_LOOK);
		// (BWindow locks itself, as in the rest of HaiWay)
}

void HaikuSurface::SetHook(Hook *hook)
{
	if (hook != NULL) {hook->fBase = this;}
	fHook.SetTo(hook);
}


void HaikuSurface::SetViewportSrc(float x, float y, float width, float height)
{
	fPendingState.viewportSrc = {x, y, width, height};
	fPendingFields |= (1 << fieldViewport);
}

void HaikuSurface::SetViewportDst(int32_t width, int32_t height)
{
	fPendingState.viewportDst = {width, height};
	fPendingFields |= (1 << fieldViewport);
}

void HaikuSurface::HandleAttach(struct wl_resource *buffer_resource, int32_t dx, int32_t dy)
{
	fPendingState.buffer = HaikuShmBuffer::FromResource(buffer_resource);
	fPendingState.dx = dx;
	fPendingState.dy = dy;
	fPendingFields |= (1 << fieldBuffer) | (1 << fieldOffset);
}

void HaikuSurface::HandleDamage(int32_t x, int32_t y, int32_t width, int32_t height)
{
	width = std::min(width, 1 << 24);
	height = std::min(height, 1 << 24);
	fDirty.Include(BRect(x, y, x + width - 1, y + height - 1));
}

void HaikuSurface::HandleFrame(uint32_t callback_id)
{
	fPendingState.frameCallbacks.Insert(FrameCallback::Create(Client(), 1, callback_id));
	fPendingFields |= (1 << fieldFrameCallbacks);
}

void HaikuSurface::HandleSetOpaqueRegion(struct wl_resource *region_resource)
{
	if (region_resource == NULL) {
		fPendingState.opaqueRgn.reset();
	} else {
		fPendingState.opaqueRgn.emplace(HaikuRegion::FromResource(region_resource)->Region());
	}
	fPendingFields |= (1 << fieldOpaqueRgn);
}

void HaikuSurface::HandleSetInputRegion(struct wl_resource *region_resource)
{
	if (region_resource == NULL) {
		fPendingState.inputRgn.reset();
	} else {
		fPendingState.inputRgn.emplace(HaikuRegion::FromResource(region_resource)->Region());
	}
	fPendingFields |= (1 << fieldInputRgn);
}

/*!	Renga: moves the double-buffered fields in \a fields from \a from to
	\a to (for commits that wait for their acquire point).
*/
void HaikuSurface::MoveState(State &to, State &from, uint32 fields)
{
	if ((fields & (1 << fieldBuffer)) != 0) {
		to.buffer = from.buffer;
		from.buffer.Unset();
	}
	if ((fields & (1 << fieldOffset)) != 0) {
		to.dx = from.dx;
		to.dy = from.dy;
	}
	if ((fields & (1 << fieldTransform)) != 0)
		to.transform = from.transform;
	if ((fields & (1 << fieldScale)) != 0)
		to.scale = from.scale;
	if ((fields & (1 << fieldOpaqueRgn)) != 0)
		to.opaqueRgn = std::move(from.opaqueRgn);
	if ((fields & (1 << fieldInputRgn)) != 0)
		to.inputRgn = std::move(from.inputRgn);
	if ((fields & (1 << fieldFrameCallbacks)) != 0)
		to.frameCallbacks.TakeFrom(&from.frameCallbacks);
	if ((fields & (1 << fieldViewport)) != 0) {
		to.viewportSrc = from.viewportSrc;
		to.viewportDst = from.viewportDst;
	}
	// the sync points always travel with their commit
	to.acquireTimeline = from.acquireTimeline;
	to.acquirePoint = from.acquirePoint;
	to.releaseTimeline = from.releaseTimeline;
	to.releasePoint = from.releasePoint;
	from.acquireTimeline.Unset();
	from.acquirePoint = 0;
	from.releaseTimeline.Unset();
	from.releasePoint = 0;
}

void HaikuSurface::SignalRelease(State &state)
{
	if (state.releaseTimeline.IsSet())
		HaikuSyncTimelineSignal(state.releaseTimeline, state.releasePoint);
	state.releaseTimeline.Unset();
	state.releasePoint = 0;
}

void HaikuSurface::DeferCommit()
{
	DeferredCommit *deferred = new(std::nothrow) DeferredCommit();
	if (deferred == NULL) {
		wl_client_post_no_memory(Client());
		return;
	}
	MoveState(deferred->state, fPendingState, fPendingFields);
	deferred->fields = fPendingFields;
	fPendingFields = 0;
	fDeferred.push_back(deferred);

	if (fSyncEventFd < 0) {
		fSyncEventFd = HaikuSyncCreateEventFd();
		if (fSyncEventFd >= 0) {
			struct wl_event_loop *loop = wl_display_get_event_loop(
				wl_client_get_display(Client()));
			fSyncEventSource = wl_event_loop_add_fd(loop, fSyncEventFd,
				WL_EVENT_READABLE, [](int fd, uint32_t mask, void *data) {
					uint64 count;
					read(fd, &count, sizeof(count));
					((HaikuSurface*)data)->ProcessDeferredCommits();
					return 0;
				}, this);
		}
	}
	// one-shot: written once the point has signalled (at once if it has)
	if (fSyncEventFd < 0 || HaikuSyncTimelineWatch(
			deferred->state.acquireTimeline, deferred->state.acquirePoint,
			fSyncEventFd) != B_OK) {
		// cannot wait without blocking: show it now rather than never
		ProcessDeferredCommits();
	}
}

void HaikuSurface::ProcessDeferredCommits()
{
	bool watchFailed = false;
	while (!fDeferred.empty()) {
		DeferredCommit *deferred = fDeferred.front();
		if (deferred->state.acquireTimeline.IsSet() && !watchFailed
			&& !HaikuSyncTimelineReady(deferred->state.acquireTimeline,
				deferred->state.acquirePoint)) {
			// the next one waits; its watch is registered
			if (fSyncEventFd >= 0)
				break;
			watchFailed = true;
		}
		fDeferred.pop_front();

		// Apply it through the normal path, keeping what the client has
		// set since for its next commit.
		State newer;
		uint32 newerFields = fPendingFields;
		MoveState(newer, fPendingState, fPendingFields);
		MoveState(fPendingState, deferred->state, deferred->fields);
		fPendingFields = deferred->fields;
		delete deferred;
		ApplyCommit();
		MoveState(fPendingState, newer, newerFields);
		fPendingFields = newerFields;
	}
}

void HaikuSurface::HandleCommit()
{
	// Renga: explicit sync. With a wp_linux_drm_syncobj_surface_v1 the
	// commit is checked (protocol errors), and a commit whose acquire point
	// has not signalled waits, without blocking: an eventfd on the
	// compositor's event loop applies it once it has (SYNCOBJ_EVENTFD).
	if (fSyncobjSurface != NULL || !fDeferred.empty()) {
		if (!HaikuSyncobjCheckCommit(this, fSyncobjSurface,
				(fPendingFields & (1 << fieldBuffer)) != 0
					&& fPendingState.buffer != NULL,
				fPendingState.buffer.Get(), fPendingState.acquireTimeline,
				fPendingState.acquirePoint, fPendingState.releaseTimeline,
				fPendingState.releasePoint)) {
			return;
		}
		if (!fDeferred.empty() || (fPendingState.acquireTimeline.IsSet()
				&& !HaikuSyncTimelineReady(fPendingState.acquireTimeline,
					fPendingState.acquirePoint))) {
			DeferCommit();
			return;
		}
	}
	ApplyCommit();
}

void HaikuSurface::ApplyCommit()
{
	//printf("HaikuSurface::HandleCommit()\n");

	for (;;) {
		uint32 field = std::countr_zero(fPendingFields);
		if (field >= 32) {
			break;
		}
		fPendingFields &= ~(1U << field);
		switch (field) {
			case fieldBuffer:
				if (fState.buffer != NULL && fState.buffer != fPendingState.buffer) {
					fState.buffer->SendReleaseIfAlive();
				}
				// Renga: the previous commit's release point. Signalled
				// once its buffer is off screen; for the same buffer again
				// it is signalled too, unless the new point is later on the
				// same timeline (which signals it as well).
				if (fState.releaseTimeline.IsSet()
					&& (fState.buffer != fPendingState.buffer
						|| fState.releaseTimeline != fPendingState.releaseTimeline
						|| fState.releasePoint > fPendingState.releasePoint)) {
					SignalRelease(fState);
				}
				fState.buffer = fPendingState.buffer;
				fState.releaseTimeline = fPendingState.releaseTimeline;
				fState.releasePoint = fPendingState.releasePoint;
				break;
			case fieldOffset:
				fState.dx = fPendingState.dx;
				fState.dy = fPendingState.dy;
				break;
			case fieldTransform:
				fState.transform = fPendingState.transform;
				break;
			case fieldScale:
				fState.scale = fPendingState.scale;
				break;
			case fieldOpaqueRgn:
				fState.opaqueRgn = std::move(fPendingState.opaqueRgn);
				break;
			case fieldInputRgn:
				fState.inputRgn = std::move(fPendingState.inputRgn);
				break;
			case fieldFrameCallbacks:
				fState.frameCallbacks.TakeFrom(&fPendingState.frameCallbacks);
				break;
			case fieldViewport:
				fState.viewportSrc = fPendingState.viewportSrc;
				fState.viewportDst = fPendingState.viewportDst;
				break;
		}
	}

	fPendingState.acquireTimeline.Unset();
	fPendingState.acquirePoint = 0;
	fPendingState.releaseTimeline.Unset();
	fPendingState.releasePoint = 0;

	if (View() != NULL && View()->Window() != NULL) {
		auto viewLocked = AppKitPtrs::LockedPtr(View());
		if (fSubsurface != NULL) {
			viewLocked->MoveTo(fSubsurface->GetState().x, fSubsurface->GetState().y);
		}
		BSize size = Size();
		viewLocked->ResizeTo(size.width, size.height);
		Invalidate();
	}
	if (fHook.IsSet()) {
		fHook->HandleCommit();
	}
}

void HaikuSurface::HandleSetBufferTransform(int32_t transform)
{
	fPendingState.transform = transform;
	fPendingFields |= (1 << fieldTransform);
}

void HaikuSurface::HandleSetBufferScale(int32_t scale)
{
	fPendingState.scale = scale;
	fPendingFields |= (1 << fieldScale);
}

void HaikuSurface::HandleDamageBuffer(int32_t x, int32_t y, int32_t width, int32_t height)
{
	width = std::min(width, 1 << 24);
	height = std::min(height, 1 << 24);
	fDirty.Include(BRect(x, y, x + width - 1, y + height - 1));
}

void HaikuSurface::HandleOffset(int32_t x, int32_t y)
{
	fPendingState.dx = x;
	fPendingState.dy = y;
	fPendingFields |= (1 << fieldOffset);
}
