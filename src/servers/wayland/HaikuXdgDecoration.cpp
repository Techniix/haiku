/*
 * Renga: xdg-decoration (zxdg_decoration_manager_v1), server-side frames.
 * Copyright 2026, Renga contributors. Distributed under the terms of the
 * MIT License.
 */
#include "HaikuXdgDecoration.h"
#include "HaikuCompositor.h"
#include "HaikuXdgSurface.h"
#include "HaikuXdgToplevel.h"

#include <AutoDeleter.h>
#include <Window.h>

#include <new>

extern const struct wl_interface zxdg_decoration_manager_v1_interface;


enum {
	XDG_DECORATION_VERSION = 1,
};


class HaikuXdgToplevelDecoration: public ZxdgToplevelDecorationV1 {
private:
	friend void HaikuXdgDecorationToplevelGone(HaikuXdgToplevelDecoration*);
	HaikuXdgToplevel *fToplevel;

public:
	HaikuXdgToplevelDecoration(HaikuXdgToplevel *toplevel): fToplevel(toplevel)
	{
		toplevel->fDecoration = this;
	}
	virtual ~HaikuXdgToplevelDecoration();

	void Apply(uint32_t mode);
	void HandleSetMode(uint32_t mode) final;
	void HandleUnsetMode() final;
};


class HaikuXdgDecorationManager: public ZxdgDecorationManagerV1 {
protected:
	virtual ~HaikuXdgDecorationManager() = default;

public:
	void HandleGetToplevelDecoration(uint32_t id, struct wl_resource *toplevel) final;
};


HaikuXdgDecorationManagerGlobal *HaikuXdgDecorationManagerGlobal::Create(struct wl_display *display)
{
	ObjectDeleter<HaikuXdgDecorationManagerGlobal> global(new(std::nothrow) HaikuXdgDecorationManagerGlobal());
	if (!global.IsSet()) return NULL;
	if (!global->Init(display, &zxdg_decoration_manager_v1_interface, XDG_DECORATION_VERSION)) return NULL;
	return global.Detach();
}

void HaikuXdgDecorationManagerGlobal::Bind(struct wl_client *wl_client, uint32_t version, uint32_t id)
{
	HaikuXdgDecorationManager *manager = new(std::nothrow) HaikuXdgDecorationManager();
	if (manager == NULL) {
		wl_client_post_no_memory(wl_client);
		return;
	}
	manager->Init(wl_client, version, id);
}


void HaikuXdgDecorationManager::HandleGetToplevelDecoration(uint32_t id, struct wl_resource *toplevelResource)
{
	HaikuXdgToplevel *toplevel = HaikuXdgToplevel::FromResource(toplevelResource);
	HaikuSurface *surface = toplevel != NULL && toplevel->XdgSurface() != NULL
		? toplevel->XdgSurface()->Surface() : NULL;
	if (surface == NULL) {
		wl_resource_post_error(ToResource(), 0, "toplevel has no surface");
		return;
	}
	if (surface->XdgDecorationMode() != HaikuSurface::decorationNone) {
		wl_resource_post_error(toplevelResource,
			ZxdgToplevelDecorationV1::errorAlreadyConstructed,
			"the toplevel has a decoration object already");
		return;
	}
	HaikuXdgToplevelDecoration *decoration = new(std::nothrow) HaikuXdgToplevelDecoration(toplevel);
	if (decoration == NULL) {
		wl_client_post_no_memory(Client());
		return;
	}
	if (!decoration->Init(Client(), Version(), id))
		return;
	// The compositor's preference until the client says otherwise.
	decoration->Apply(ZxdgToplevelDecorationV1::modeServerSide);
}


void HaikuXdgDecorationToplevelGone(HaikuXdgToplevelDecoration *decoration)
{
	if (decoration != NULL)
		decoration->fToplevel = NULL;
}


HaikuXdgToplevelDecoration::~HaikuXdgToplevelDecoration()
{
	if (fToplevel != NULL)
		fToplevel->fDecoration = NULL;
	// The toplevel goes back to having no decoration object (client-side,
	// as without the protocol).
	if (fToplevel != NULL && fToplevel->XdgSurface() != NULL
		&& fToplevel->XdgSurface()->Surface() != NULL) {
		fToplevel->XdgSurface()->Surface()->SetXdgDecorationMode(
			HaikuSurface::decorationNone);
	}
}

void HaikuXdgToplevelDecoration::Apply(uint32_t mode)
{
	if (fToplevel == NULL)
		return;
	HaikuXdgSurface *xdgSurface = fToplevel->XdgSurface();
	if (xdgSurface == NULL || xdgSurface->Surface() == NULL)
		return;
	xdgSurface->Surface()->SetXdgDecorationMode(
		mode == modeServerSide ? HaikuSurface::decorationServer
			: HaikuSurface::decorationClient);
	SendConfigure(mode);
	// A decoration configure is part of an xdg_surface configure sequence;
	// once the first configure went out, this one needs its own.
	if (xdgSurface->ConfigureCalled()) {
		fToplevel->DoSendConfigure();
		xdgSurface->SendConfigure(xdgSurface->NextSerial());
	}
}

void HaikuXdgToplevelDecoration::HandleSetMode(uint32_t mode)
{
	if (mode != modeClientSide && mode != modeServerSide) {
		wl_resource_post_error(ToResource(), errorInvalidMode, "invalid mode");
		return;
	}
	Apply(mode);
}

void HaikuXdgToplevelDecoration::HandleUnsetMode()
{
	Apply(modeServerSide);
}
