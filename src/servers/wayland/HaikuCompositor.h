#pragma once

#include "Wayland.h"
#include "WlGlobal.h"
#include "HaikuSubcompositor.h"
#include "HaikuShm.h"
#include <AutoDeleter.h>
#include <Point.h>
#include <Region.h>

#include <deque>
#include <optional>

class HaikuXdgSurface;
class HaikuSyncTimeline;
class HaikuSyncobjSurface;
class HaikuServerDecoration;
class WaylandView;
class BBitmap;
class BWindow;
class BView;


class HaikuCompositorGlobal: public WlGlocal {
public:
	static HaikuCompositorGlobal *Create(struct wl_display *display);
	virtual ~HaikuCompositorGlobal() = default;
	void Bind(struct wl_client *wl_client, uint32_t version, uint32_t id) override;
};


class HaikuSurface: public WlSurface {
public:
	class Hook {
	private:
		friend class HaikuSurface;
		HaikuSurface *fBase{};
	public:
		virtual ~Hook() = default;
		HaikuSurface *Base() {return fBase;}
		virtual void HandleCommit() = 0;
	};

private:
	friend class HaikuXdgSurface;
	friend class HaikuXdgToplevel;
	friend class HaikuServerDecoration;
	friend class HaikuSubsurface;
	friend class WaylandView;

	class FrameCallback: public WlCallback {
	private:
		DoublyLinkedListLink<FrameCallback> fLink;

	public:
		typedef DoublyLinkedList<FrameCallback, DoublyLinkedListMemberGetLink<FrameCallback, &FrameCallback::fLink>> List;

		static FrameCallback *Create(struct wl_client *client, uint32_t version, uint32_t id);
		virtual ~FrameCallback() = default;
	};

	enum StateField {
			fieldBuffer,
			fieldOffset,
			fieldTransform,
			fieldScale,
			fieldOpaqueRgn,
			fieldInputRgn,
			fieldFrameCallbacks,
			fieldViewport
	};

	struct ViewportSrc {
			float x = -1, y = -1, width = -1, height = -1;

			inline bool IsValid() const
			{
				return x != -1 && y != -1 && width != -1 && height != -1;
			}
	};

	struct ViewportDst {
			int32_t width = -1, height = -1;

			inline bool IsValid() const
			{
				return width != -1 && height != -1;
			}
	};

	struct State {
		BReference<HaikuShmBuffer> buffer;
		int32_t dx, dy;
		int32_t transform = WlOutput::transformNormal;
		int32_t scale = 1;
		std::optional<BRegion> opaqueRgn;
		std::optional<BRegion> inputRgn;
		FrameCallback::List frameCallbacks;
		ViewportSrc viewportSrc;
		ViewportDst viewportDst;

		// Renga: explicit sync (wp_linux_drm_syncobj_v1, HaikuGpuSync.cpp).
		// Pending: the points set for the next commit. Current: the release
		// point of the buffer on screen, signalled once it is replaced.
		BReference<HaikuSyncTimeline> acquireTimeline;
		uint64 acquirePoint = 0;
		BReference<HaikuSyncTimeline> releaseTimeline;
		uint64 releasePoint = 0;
	};

	// Renga: a commit that waits for its acquire point. Commits apply in
	// order, so later ones queue behind it.
	struct DeferredCommit {
		State state;
		uint32 fields = 0;
	};

	ObjectDeleter<Hook> fHook;

	State fState{};
	State fPendingState{};
	uint32 fPendingFields{};
	BRegion fDirty;
	WaylandView *fView{};
	HaikuXdgSurface *fXdgSurface{};
	HaikuServerDecoration *fServerDecoration{};
	HaikuSubsurface *fSubsurface{};

	HaikuSubsurface::SurfaceList fSurfaceList;

	int32 fXdgDecorationMode = 0;
		// Renga: DecorationMode from xdg-decoration (HaikuXdgDecoration.cpp)

	friend class HaikuSyncobjSurface;
	friend class HaikuSyncobjManager;
	friend void HaikuSyncobjSurfaceGone(HaikuSurface *surface);
	HaikuSyncobjSurface *fSyncobjSurface{};
	std::deque<DeferredCommit*> fDeferred;
	int fSyncEventFd = -1;
	struct wl_event_source *fSyncEventSource{};

	static void MoveState(State &to, State &from, uint32 fields);
	void ApplyCommit();
	void DeferCommit();
	void SignalRelease(State &state);

public:
	enum DecorationMode {
		decorationNone = 0,		// no xdg-decoration object
		decorationClient,
		decorationServer
	};

	static HaikuSurface *Create(struct wl_client *client, uint32_t version, uint32_t id);
	static HaikuSurface *FromResource(struct wl_resource *resource) {return (HaikuSurface*)WlResource::FromResource(resource);}
	virtual ~HaikuSurface();

	BView *View() const {return (BView*)fView;}
	BBitmap *Bitmap() const {return fState.buffer == NULL ? NULL : &fState.buffer->Bitmap();}
	bool InputRgnContains(BPoint p) {return !fState.inputRgn ? true : fState.inputRgn->Contains(p);}
	void GetOffset(int32_t &x, int32_t &y) const {x = fState.dx; y = fState.dy;}
	BSize Size() const;
	HaikuXdgSurface *XdgSurface() {return fXdgSurface;}
	HaikuSubsurface *Subsurface() {return fSubsurface;}
	HaikuServerDecoration *ServerDecoration() {return fServerDecoration;}
	int32 XdgDecorationMode() const {return fXdgDecorationMode;}
	void SetXdgDecorationMode(int32 mode);
	bool WantsServerFrame();
		// Renga: the KDE protocol or xdg-decoration asked for Renga's frame
	bool HasDecorationProtocol();
		// either protocol is in use for this surface
	HaikuSubsurface::SurfaceList &SurfaceList() {return fSurfaceList;}
	void AttachWindow(BWindow *window);
	void AttachView(BView *view);
	void AttachViewsToEarlierSubsurfaces();
	void Detach();
	void Invalidate();
	void CallFrameCallbacks();

	void SetHook(Hook *hook);

	//! Renga: applies queued commits whose acquire points have signalled.
	void ProcessDeferredCommits();
	size_t CountDeferredCommits() const {return fDeferred.size();}

	void SetViewportSrc(float x, float y, float width, float height);
	void SetViewportDst(int32_t width, int32_t height);

	void HandleAttach(struct wl_resource *buffer_resource, int32_t dx, int32_t dy) override;
	void HandleDamage(int32_t x, int32_t y, int32_t width, int32_t height) override;
	void HandleFrame(uint32_t callback) override;
	void HandleSetOpaqueRegion(struct wl_resource *region_resource) override;
	void HandleSetInputRegion(struct wl_resource *region_resource) override;
	void HandleCommit() override;
	void HandleSetBufferTransform(int32_t transform) override;
	void HandleSetBufferScale(int32_t scale) override;
	void HandleDamageBuffer(int32_t x, int32_t y, int32_t width, int32_t height) override;
	void HandleOffset(int32_t x, int32_t y) override;
};
