/*
 * Renga: pointer constraints (zwp_pointer_constraints_v1) and relative
 * pointer motion (zwp_relative_pointer_manager_v1), for pointer lock in
 * games and Chromium's Pointer Lock API.
 * Copyright 2026, Renga contributors. Distributed under the terms of the
 * MIT License.
 *
 * - Relative motion: every pointer motion over a client's surface also
 *   goes to its relative pointers as the screen delta since the previous
 *   motion (Haiku applies its acceleration before programs see the
 *   position, so the "unaccelerated" delta is the same).
 * - Lock: while active, the pointer is put back where it was locked after
 *   each motion (set_mouse_position()), so it does not move; wl_pointer
 *   gets no motion, the relative pointers get the deltas. Active while the
 *   surface's window is the active window; it ends when the window is
 *   deactivated (Command+Tab and clicks elsewhere always escape it), and
 *   a persistent lock comes back when the window is activated again.
 * - Confine: the pointer is kept inside the surface's window (the region
 *   argument is not used: the whole surface).
 */
#include "RengaProtocols.h"
#include "HaikuCompositor.h"
#include "PointerConstraintsUnstableV1.h"
#include "RelativePointerUnstableV1.h"
#include "AppKitPtrs.h"

#include <AutoDeleter.h>
#include <InterfaceDefs.h>
#include <View.h>
#include <Window.h>
#include <InputServerTypes.h>
#include <input_globals.h>

#include <algorithm>
#include <new>
#include <vector>

extern const struct wl_interface zwp_pointer_constraints_v1_interface;
extern const struct wl_interface zwp_relative_pointer_manager_v1_interface;


class HaikuRelativePointer;
class HaikuConstraint;

static std::vector<HaikuRelativePointer*> sRelativePointers;
static std::vector<HaikuConstraint*> sConstraints;
static BPoint sLastScreen;


//!	Moves the pointer, as libgame's set_mouse_position() does (without
//	linking libgame, which minimal images do not have).
static void
set_mouse_position(int32 x, int32 y)
{
	BMessage command(IS_SET_MOUSE_POSITION);
	BMessage reply;
	command.AddPoint("where", BPoint(x, y));
	_control_input_server_(&command, &reply);
}

static bool sHaveLast = false;


class HaikuRelativePointer: public ZwpRelativePointerV1 {
public:
	HaikuRelativePointer() {sRelativePointers.push_back(this);}
	virtual ~HaikuRelativePointer()
	{
		sRelativePointers.erase(std::remove(sRelativePointers.begin(),
			sRelativePointers.end(), this), sRelativePointers.end());
	}
};


class HaikuConstraint {
public:
	HaikuSurface *fSurface;
	bool fLock;
	bool fPersistent;
	bool fActive = false;
	bool fDefunct = false;
	BPoint fLockScreen;

	HaikuConstraint(HaikuSurface *surface, bool lock, bool persistent)
		: fSurface(surface), fLock(lock), fPersistent(persistent)
	{
		sConstraints.push_back(this);
	}

	virtual ~HaikuConstraint()
	{
		sConstraints.erase(std::remove(sConstraints.begin(),
			sConstraints.end(), this), sConstraints.end());
	}

	virtual void SendActivated() = 0;
	virtual void SendDeactivated() = 0;

	void Activate(BPoint screen)
	{
		if (fActive || fDefunct)
			return;
		fActive = true;
		fLockScreen = screen;
		SendActivated();
	}

	void Deactivate()
	{
		if (!fActive)
			return;
		fActive = false;
		SendDeactivated();
		if (!fPersistent)
			fDefunct = true;
	}
};


class HaikuLockedPointer: public ZwpLockedPointerV1, public HaikuConstraint {
public:
	HaikuLockedPointer(HaikuSurface *surface, bool persistent)
		: HaikuConstraint(surface, true, persistent) {}
	void SendActivated() final {SendLocked();}
	void SendDeactivated() final {SendUnlocked();}
	void HandleSetCursorPositionHint(wl_fixed_t x, wl_fixed_t y) final {}
	void HandleSetRegion(struct wl_resource *region) final {}
};


class HaikuConfinedPointer: public ZwpConfinedPointerV1, public HaikuConstraint {
public:
	HaikuConfinedPointer(HaikuSurface *surface, bool persistent)
		: HaikuConstraint(surface, false, persistent) {}
	void SendActivated() final {SendConfined();}
	void SendDeactivated() final {SendUnconfined();}
	void HandleSetRegion(struct wl_resource *region) final {}
};


static HaikuConstraint *
constraint_for(HaikuSurface *surface)
{
	for (HaikuConstraint *constraint : sConstraints) {
		if (constraint->fSurface == surface && !constraint->fDefunct)
			return constraint;
	}
	return NULL;
}


/*!	Called for every pointer motion over a surface, on the surface's window
	thread inside a WaylandEnv: the window's looper is NOT locked there (the
	server handler is), so the view is locked for the one conversion it
	needs. Returns true if the motion is consumed (pointer locked).

	R-9 (Chromium, 2026-11-19): this used to call ConvertToScreen() on the
	unlocked view for every motion, also for clients with no constraint and
	no relative pointer; BView's lock check stopped the compositor
	("Looper must be locked") as soon as the mouse moved over Chromium's
	first window. Now a motion with no constraint on the surface and no
	relative pointer of its client returns at once, and the screen point
	comes from the view with its looper locked.
*/
bool
HaikuPointerConstraintsMotion(HaikuSurface *surface, bigtime_t when,
	BPoint viewWhere)
{
	if (surface == NULL)
		return false;
	HaikuConstraint *constraint = constraint_for(surface);
	bool relative = false;
	for (HaikuRelativePointer *pointer : sRelativePointers) {
		if (pointer->Client() == surface->Client()) {
			relative = true;
			break;
		}
	}
	if (constraint == NULL && !relative) {
		sHaveLast = false;
		return false;
	}

	BView *view = surface->View();
	if (view == NULL)
		return false;
	BPoint screen;
	BRect windowFrame;
	bool windowActive = false;
	{
		AppKitPtrs::LockedPtr<BView> locked(view);
		if (view->Window() == NULL)
			return false;
		screen = view->ConvertToScreen(viewWhere);
		windowFrame = view->Window()->Frame();
		windowActive = view->Window()->IsActive();
	}

	if (constraint != NULL && !constraint->fActive && windowActive) {
		constraint->Activate(screen);
		sLastScreen = screen;
		sHaveLast = true;
	}

	BPoint delta(0, 0);
	if (constraint != NULL && constraint->fActive && constraint->fLock)
		delta = screen - constraint->fLockScreen;
	else if (sHaveLast)
		delta = screen - sLastScreen;
	sLastScreen = screen;
	sHaveLast = true;

	if (delta.x != 0 || delta.y != 0) {
		uint64 micros = (uint64)when;
		for (HaikuRelativePointer *pointer : sRelativePointers) {
			if (pointer->Client() != surface->Client())
				continue;
			wl_fixed_t dx = wl_fixed_from_double(delta.x);
			wl_fixed_t dy = wl_fixed_from_double(delta.y);
			pointer->SendRelativeMotion((uint32_t)(micros >> 32),
				(uint32_t)micros, dx, dy, dx, dy);
		}
	}

	if (constraint == NULL || !constraint->fActive)
		return false;

	if (constraint->fLock) {
		if (delta.x != 0 || delta.y != 0) {
			set_mouse_position((int32)constraint->fLockScreen.x,
				(int32)constraint->fLockScreen.y);
			sLastScreen = constraint->fLockScreen;
		}
		return true;
	}

	// confined: keep it inside the window's content
	BRect frame = windowFrame;
	BPoint clamped(std::clamp(screen.x, frame.left, frame.right),
		std::clamp(screen.y, frame.top, frame.bottom));
	if (clamped != screen) {
		set_mouse_position((int32)clamped.x, (int32)clamped.y);
		sLastScreen = clamped;
	}
	return false;
}


//! The surface's window was deactivated (or the surface lost focus).
void
HaikuPointerConstraintsFocusLost(HaikuSurface *surface)
{
	for (HaikuConstraint *constraint : sConstraints) {
		if (constraint->fSurface == surface)
			constraint->Deactivate();
	}
}


void
HaikuPointerConstraintsSurfaceGone(HaikuSurface *surface)
{
	for (HaikuConstraint *constraint : sConstraints) {
		if (constraint->fSurface == surface) {
			constraint->fSurface = NULL;
			constraint->fDefunct = true;
		}
	}
}


class HaikuPointerConstraintsManager: public ZwpPointerConstraintsV1 {
public:
	template<typename Type>
	void Create(uint32_t id, struct wl_resource *surfaceResource, uint32_t lifetime)
	{
		HaikuSurface *surface = HaikuSurface::FromResource(surfaceResource);
		if (constraint_for(surface) != NULL) {
			wl_resource_post_error(ToResource(), errorAlreadyConstrained,
				"the surface already has a pointer constraint");
			return;
		}
		Type *constraint = new(std::nothrow) Type(surface,
			lifetime == lifetimePersistent);
		if (constraint == NULL) {
			wl_client_post_no_memory(Client());
			return;
		}
		constraint->Init(Client(), Version(), id);
	}

	void HandleLockPointer(uint32_t id, struct wl_resource *surface,
		struct wl_resource *pointer, struct wl_resource *region,
		uint32_t lifetime) final
	{
		Create<HaikuLockedPointer>(id, surface, lifetime);
	}

	void HandleConfinePointer(uint32_t id, struct wl_resource *surface,
		struct wl_resource *pointer, struct wl_resource *region,
		uint32_t lifetime) final
	{
		Create<HaikuConfinedPointer>(id, surface, lifetime);
	}
};


class HaikuRelativePointerManager: public ZwpRelativePointerManagerV1 {
public:
	void HandleGetRelativePointer(uint32_t id, struct wl_resource *pointer) final
	{
		HaikuRelativePointer *relative = new(std::nothrow) HaikuRelativePointer();
		if (relative == NULL) {
			wl_client_post_no_memory(Client());
			return;
		}
		relative->Init(Client(), Version(), id);
	}
};


HaikuPointerConstraintsGlobal *HaikuPointerConstraintsGlobal::Create(struct wl_display *display)
{
	ObjectDeleter<HaikuPointerConstraintsGlobal> global(new(std::nothrow) HaikuPointerConstraintsGlobal());
	if (!global.IsSet()) return NULL;
	if (!global->Init(display, &zwp_pointer_constraints_v1_interface, 1)) return NULL;
	return global.Detach();
}

void HaikuPointerConstraintsGlobal::Bind(struct wl_client *client, uint32_t version, uint32_t id)
{
	HaikuPointerConstraintsManager *manager = new(std::nothrow) HaikuPointerConstraintsManager();
	if (manager == NULL) {
		wl_client_post_no_memory(client);
		return;
	}
	manager->Init(client, version, id);
}

HaikuRelativePointerGlobal *HaikuRelativePointerGlobal::Create(struct wl_display *display)
{
	ObjectDeleter<HaikuRelativePointerGlobal> global(new(std::nothrow) HaikuRelativePointerGlobal());
	if (!global.IsSet()) return NULL;
	if (!global->Init(display, &zwp_relative_pointer_manager_v1_interface, 1)) return NULL;
	return global.Detach();
}

void HaikuRelativePointerGlobal::Bind(struct wl_client *client, uint32_t version, uint32_t id)
{
	HaikuRelativePointerManager *manager = new(std::nothrow) HaikuRelativePointerManager();
	if (manager == NULL) {
		wl_client_post_no_memory(client);
		return;
	}
	manager->Init(client, version, id);
}
