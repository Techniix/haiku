#pragma once
#include "Wayland.h"
#include "WlGlobal.h"

#include <GraphicsDefs.h>
#include <Rect.h>

#include <vector>


class HaikuOutput;
class HaikuSurface;

/*	Renga: one wl_output global per screen (BScreen), with xdg-output
	(zxdg_output_manager_v1) and hot-plug: HaikuOutputCheckScreens(),
	called periodically from the Wayland looper, adds and removes globals
	when screens come and go and sends the new mode to every bound output
	when a screen's frame changes.
*/
class HaikuOutputGlobal: public WlGlocal {
private:
	friend class HaikuOutput;
	friend void HaikuOutputSurfaceGone(HaikuSurface *surface);
	int32 fIndex;
	BRect fFrame;
	float fRefresh = 60;
	std::vector<HaikuOutput*> fOutputs;

	void SendState(HaikuOutput *output);

public:
	static HaikuOutputGlobal *Create(struct wl_display *display, int32 index = 0);
	virtual ~HaikuOutputGlobal();
	void Bind(struct wl_client *wl_client, uint32_t version, uint32_t id) override;

	int32 Index() const {return fIndex;}
	BRect Frame() const {return fFrame;}
	void Update(BRect frame, float refresh);
	HaikuOutput *OutputFor(struct wl_client *client);
};

class HaikuXdgOutputManagerGlobal: public WlGlocal {
public:
	static HaikuXdgOutputManagerGlobal *Create(struct wl_display *display);
	virtual ~HaikuXdgOutputManagerGlobal() = default;
	void Bind(struct wl_client *wl_client, uint32_t version, uint32_t id) override;
};

//! Creates the globals for the screens there are now.
void HaikuOutputCreateGlobals(struct wl_display *display);
//! Screens added, removed or changed since the last check.
void HaikuOutputCheckScreens(struct wl_display *display);
//! wl_surface.enter for the output the window is on (once per output).
void HaikuOutputSurfaceShown(HaikuSurface *surface, BRect windowFrame);
//! Forget a surface going away.
void HaikuOutputSurfaceGone(HaikuSurface *surface);
//! The usable area of the screen a window is on (Deskbar excluded).
BRect HaikuOutputUsableFrame(BRect windowFrame);
