/*
 * Renga: wl_fixes, fractional scale, idle inhibit, cursor shape and
 * toplevel icons for the Wayland compositor.
 * Copyright 2026, Renga contributors. Distributed under the terms of the
 * MIT License.
 */
#include "RengaProtocols.h"
#include "HaikuCompositor.h"
#include "HaikuSeat.h"
#include "AppKitPtrs.h"

#include "Wayland.h"
#include "FractionalScaleV1.h"
#include "IdleInhibitUnstableV1.h"
#include "CursorShapeV1.h"
#include "XdgToplevelIconV1.h"
#include "XdgActivationV1.h"
#include "xdg-activation-v1-protocol.h"
#include "HaikuXdgSurface.h"
#include <Window.h>

#include <Application.h>
#include <AutoDeleter.h>
#include <Cursor.h>
#include <Font.h>
#include <OS.h>

#include <math.h>
#include <stdio.h>
#include <new>

extern const struct wl_interface wl_fixes_interface;
extern const struct wl_interface wp_fractional_scale_manager_v1_interface;
extern const struct wl_interface zwp_idle_inhibit_manager_v1_interface;
extern const struct wl_interface wp_cursor_shape_manager_v1_interface;
extern const struct wl_interface xdg_toplevel_icon_manager_v1_interface;

// The screen saver filter (input_server) treats the existence of a port with
// this name as activity; ports go away with their team, so an inhibitor
// can never outlive the compositor. See ScreenSaverFilter.cpp.
#define RENGA_SCREEN_SAVER_INHIBIT_PORT "renga:screen saver inhibit"


#define DEFINE_CREATE(GlobalClass, interface, version) \
	GlobalClass *GlobalClass::Create(struct wl_display *display) \
	{ \
		ObjectDeleter<GlobalClass> global(new(std::nothrow) GlobalClass()); \
		if (!global.IsSet()) return NULL; \
		if (!global->Init(display, &interface, version)) return NULL; \
		return global.Detach(); \
	}

template<typename Resource>
static Resource *
bind_simple(struct wl_client *client, uint32_t version, uint32_t id)
{
	Resource *resource = new(std::nothrow) Resource();
	if (resource == NULL) {
		wl_client_post_no_memory(client);
		return NULL;
	}
	if (!resource->Init(client, version, id))
		return NULL;
	return resource;
}


//#pragma mark - wl_fixes

class HaikuFixes: public WlFixes {
public:
	void HandleDestroyRegistry(struct wl_resource *registry) final
	{
		// The client no longer uses the registry: libwayland can free it.
		wl_resource_destroy(registry);
	}
};

DEFINE_CREATE(HaikuFixesGlobal, wl_fixes_interface, 1)

void HaikuFixesGlobal::Bind(struct wl_client *client, uint32_t version, uint32_t id)
{
	bind_simple<HaikuFixes>(client, version, id);
}


//#pragma mark - fractional scale

uint32_t HaikuPreferredScale120()
{
	// Haiku scales its interface with the system font size (12 pt is 1.0).
	float scale = be_plain_font->Size() / 12.0f;
	if (scale < 1.0f || isnan(scale))
		scale = 1.0f;
	if (scale > 4.0f)
		scale = 4.0f;
	return (uint32_t)lroundf(scale * 120);
}

class HaikuFractionalScale: public WpFractionalScaleV1 {
};

class HaikuFractionalScaleManager: public WpFractionalScaleManagerV1 {
public:
	void HandleGetFractionalScale(uint32_t id, struct wl_resource *surface) final
	{
		HaikuFractionalScale *scale = bind_simple<HaikuFractionalScale>(Client(), Version(), id);
		if (scale != NULL)
			scale->SendPreferredScale(HaikuPreferredScale120());
	}
};

DEFINE_CREATE(HaikuFractionalScaleGlobal, wp_fractional_scale_manager_v1_interface, 1)

void HaikuFractionalScaleGlobal::Bind(struct wl_client *client, uint32_t version, uint32_t id)
{
	bind_simple<HaikuFractionalScaleManager>(client, version, id);
}


//#pragma mark - idle inhibit

class HaikuIdleInhibitor: public ZwpIdleInhibitorV1 {
private:
	port_id fPort;

public:
	HaikuIdleInhibitor()
	{
		fPort = create_port(1, RENGA_SCREEN_SAVER_INHIBIT_PORT);
	}

	virtual ~HaikuIdleInhibitor()
	{
		if (fPort >= 0)
			delete_port(fPort);
	}
};

class HaikuIdleInhibitManager: public ZwpIdleInhibitManagerV1 {
public:
	void HandleCreateInhibitor(uint32_t id, struct wl_resource *surface) final
	{
		// Linux compositors inhibit only while the surface is visible;
		// Renga inhibits while the inhibitor exists (video players destroy
		// theirs when playback stops or the window goes).
		bind_simple<HaikuIdleInhibitor>(Client(), Version(), id);
	}
};

// #pragma mark - xdg_activation_v1

/*	Renga (2026-11-14): tokens are handed out on commit; activate() with a
	token raises the surface's window (BWindow::Activate()). Haiku has no
	focus-stealing prevention to feed with the serial and the app ID, so
	every token the compositor made is accepted once. (foot warned that the
	compositor "does not implement XDG activation": the global was listed in
	the notes but never created.)
*/
static int32 sNextActivationToken = 1;

class HaikuActivationToken: public XdgActivationTokenV1 {
public:
	void HandleSetSerial(uint32_t serial, struct wl_resource *seat) final {}
	void HandleSetAppId(const char *app_id) final {}
	void HandleSetSurface(struct wl_resource *surface) final {}
	void HandleCommit() final
	{
		char token[48];
		snprintf(token, sizeof(token), "renga-%" B_PRId32 "-%" B_PRIx64,
			atomic_add(&sNextActivationToken, 1), (uint64)system_time());
		SendDone(token);
	}
};

class HaikuActivation: public XdgActivationV1 {
public:
	void HandleGetActivationToken(uint32_t id) final
	{
		bind_simple<HaikuActivationToken>(Client(), Version(), id);
	}

	void HandleActivate(const char *token, struct wl_resource *_surface) final
	{
		if (token == NULL || strncmp(token, "renga-", 6) != 0)
			return;
		HaikuSurface *surface = HaikuSurface::FromResource(_surface);
		if (surface == NULL || surface->XdgSurface() == NULL)
			return;
		BWindow *window = surface->XdgSurface()->Window();
		if (window != NULL)
			window->Activate(true);
	}
};

DEFINE_CREATE(HaikuActivationGlobal, xdg_activation_v1_interface, 1)

void HaikuActivationGlobal::Bind(struct wl_client *client, uint32_t version, uint32_t id)
{
	bind_simple<HaikuActivation>(client, version, id);
}


DEFINE_CREATE(HaikuIdleInhibitGlobal, zwp_idle_inhibit_manager_v1_interface, 1)

void HaikuIdleInhibitGlobal::Bind(struct wl_client *client, uint32_t version, uint32_t id)
{
	bind_simple<HaikuIdleInhibitManager>(client, version, id);
}


//#pragma mark - cursor shape

static BCursorID
cursor_for_shape(uint32_t shape)
{
	switch (shape) {
		case WpCursorShapeDeviceV1::shapeContextMenu: return B_CURSOR_ID_CONTEXT_MENU;
		case WpCursorShapeDeviceV1::shapeHelp: return B_CURSOR_ID_HELP;
		case WpCursorShapeDeviceV1::shapePointer: return B_CURSOR_ID_FOLLOW_LINK;
		case WpCursorShapeDeviceV1::shapeProgress:
		case WpCursorShapeDeviceV1::shapeWait: return B_CURSOR_ID_PROGRESS;
		case WpCursorShapeDeviceV1::shapeCell:
		case WpCursorShapeDeviceV1::shapeCrosshair: return B_CURSOR_ID_CROSS_HAIR;
		case WpCursorShapeDeviceV1::shapeText: return B_CURSOR_ID_I_BEAM;
		case WpCursorShapeDeviceV1::shapeVerticalText: return B_CURSOR_ID_I_BEAM_HORIZONTAL;
		case WpCursorShapeDeviceV1::shapeAlias: return B_CURSOR_ID_CREATE_LINK;
		case WpCursorShapeDeviceV1::shapeCopy: return B_CURSOR_ID_COPY;
		case WpCursorShapeDeviceV1::shapeMove:
		case WpCursorShapeDeviceV1::shapeAllScroll:
		case WpCursorShapeDeviceV1::shapeAllResize: return B_CURSOR_ID_MOVE;
		case WpCursorShapeDeviceV1::shapeNoDrop:
		case WpCursorShapeDeviceV1::shapeNotAllowed: return B_CURSOR_ID_NOT_ALLOWED;
		case WpCursorShapeDeviceV1::shapeGrab: return B_CURSOR_ID_GRAB;
		case WpCursorShapeDeviceV1::shapeGrabbing: return B_CURSOR_ID_GRABBING;
		case WpCursorShapeDeviceV1::shapeEResize: return B_CURSOR_ID_RESIZE_EAST;
		case WpCursorShapeDeviceV1::shapeNResize: return B_CURSOR_ID_RESIZE_NORTH;
		case WpCursorShapeDeviceV1::shapeNeResize: return B_CURSOR_ID_RESIZE_NORTH_EAST;
		case WpCursorShapeDeviceV1::shapeNwResize: return B_CURSOR_ID_RESIZE_NORTH_WEST;
		case WpCursorShapeDeviceV1::shapeSResize: return B_CURSOR_ID_RESIZE_SOUTH;
		case WpCursorShapeDeviceV1::shapeSeResize: return B_CURSOR_ID_RESIZE_SOUTH_EAST;
		case WpCursorShapeDeviceV1::shapeSwResize: return B_CURSOR_ID_RESIZE_SOUTH_WEST;
		case WpCursorShapeDeviceV1::shapeWResize: return B_CURSOR_ID_RESIZE_WEST;
		case WpCursorShapeDeviceV1::shapeEwResize:
		case WpCursorShapeDeviceV1::shapeColResize: return B_CURSOR_ID_RESIZE_EAST_WEST;
		case WpCursorShapeDeviceV1::shapeNsResize:
		case WpCursorShapeDeviceV1::shapeRowResize: return B_CURSOR_ID_RESIZE_NORTH_SOUTH;
		case WpCursorShapeDeviceV1::shapeNeswResize: return B_CURSOR_ID_RESIZE_NORTH_EAST_SOUTH_WEST;
		case WpCursorShapeDeviceV1::shapeNwseResize: return B_CURSOR_ID_RESIZE_NORTH_WEST_SOUTH_EAST;
		case WpCursorShapeDeviceV1::shapeZoomIn: return B_CURSOR_ID_ZOOM_IN;
		case WpCursorShapeDeviceV1::shapeZoomOut: return B_CURSOR_ID_ZOOM_OUT;
		case WpCursorShapeDeviceV1::shapeDndAsk: return B_CURSOR_ID_CONTEXT_MENU;
		case WpCursorShapeDeviceV1::shapeDefault:
		default:
			return B_CURSOR_ID_SYSTEM_DEFAULT;
	}
}

class HaikuCursorShapeDevice: public WpCursorShapeDeviceV1 {
public:
	void HandleSetShape(uint32_t serial, uint32_t shape) final
	{
		if (shape < shapeDefault || shape > shapeAllResize) {
			wl_resource_post_error(ToResource(), errorInvalidShape,
				"unknown cursor shape %u", shape);
			return;
		}
		BCursor cursor(cursor_for_shape(shape));
		AppKitPtrs::LockedPtr(be_app)->SetCursor(&cursor);
	}
};

class HaikuCursorShapeManager: public WpCursorShapeManagerV1 {
public:
	void HandleGetPointer(uint32_t id, struct wl_resource *pointer) final
	{
		bind_simple<HaikuCursorShapeDevice>(Client(), Version(), id);
	}

	void HandleGetTabletToolV2(uint32_t id, struct wl_resource *tool) final
	{
		// No tablet seat is offered, so no client has a tablet tool.
		bind_simple<HaikuCursorShapeDevice>(Client(), Version(), id);
	}
};

DEFINE_CREATE(HaikuCursorShapeGlobal, wp_cursor_shape_manager_v1_interface, 1)

void HaikuCursorShapeGlobal::Bind(struct wl_client *client, uint32_t version, uint32_t id)
{
	bind_simple<HaikuCursorShapeManager>(client, version, id);
}


//#pragma mark - toplevel icons

class HaikuToplevelIcon: public XdgToplevelIconV1 {
public:
	void HandleSetName(const char *name) final {}
	void HandleAddBuffer(struct wl_resource *buffer, int32_t scale) final {}
};

class HaikuToplevelIconManager: public XdgToplevelIconManagerV1 {
public:
	void HandleCreateIcon(uint32_t id) final
	{
		bind_simple<HaikuToplevelIcon>(Client(), Version(), id);
	}

	void HandleSetIcon(struct wl_resource *toplevel, struct wl_resource *icon) final
	{
		// Accepted: Haiku windows have no icon of their own; the icon is
		// for a future Deskbar entry per Wayland program.
	}
};

DEFINE_CREATE(HaikuToplevelIconGlobal, xdg_toplevel_icon_manager_v1_interface, 1)

void HaikuToplevelIconGlobal::Bind(struct wl_client *client, uint32_t version, uint32_t id)
{
	HaikuToplevelIconManager *manager = bind_simple<HaikuToplevelIconManager>(client, version, id);
	if (manager == NULL)
		return;
	// Sizes the compositor prefers (none required), then done.
	manager->SendIconSize(32);
	manager->SendIconSize(64);
	manager->SendDone();
}
