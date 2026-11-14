#pragma once
#include "WlGlobal.h"
#include "XdgDecorationUnstableV1.h"


/*	Renga: zxdg_decoration_manager_v1 (xdg-decoration-unstable-v1). A
	toplevel whose client asks for server-side decorations (or does not
	ask either way once it has a decoration object) gets Renga's window
	frame, as with the KDE protocol (HaikuServerDecoration). The
	compositor honours a client's request for client-side decorations.
*/
class HaikuXdgToplevelDecoration;

//! The toplevel goes away before its decoration object (a client that went).
void HaikuXdgDecorationToplevelGone(HaikuXdgToplevelDecoration *decoration);

class HaikuXdgDecorationManagerGlobal: public WlGlocal {
public:
	static HaikuXdgDecorationManagerGlobal *Create(struct wl_display *display);
	virtual ~HaikuXdgDecorationManagerGlobal() = default;
	void Bind(struct wl_client *wl_client, uint32_t version, uint32_t id) override;
};
