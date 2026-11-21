#include "HaikuXdgSurface.h"
#include <math.h>
#include "RengaProtocols.h"
#include "HaikuXdgShell.h"
#include "HaikuXdgToplevel.h"
#include "HaikuXdgPopup.h"
#include "HaikuCompositor.h"
#include "HaikuServerDecoration.h"
#include "HaikuOutput.h"
#include <wayland-server-core.h>
#include <wayland-server-protocol.h>
#include <xdg-shell-protocol.h>

#include "AppKitPtrs.h"
#include <View.h>
#include <Window.h>
#include <Bitmap.h>

#include <stdio.h>

static void Assert(bool cond) {if (!cond) abort();}


class XdgSurfaceHook: public HaikuSurface::Hook {
private:
	HaikuXdgSurface *fXdgSurface;
public:
	XdgSurfaceHook(HaikuXdgSurface *xdgSurface);
	~XdgSurfaceHook();
	void HandleCommit() final;
};

XdgSurfaceHook::XdgSurfaceHook(HaikuXdgSurface *xdgSurface):
	fXdgSurface(xdgSurface)
{}

XdgSurfaceHook::~XdgSurfaceHook()
{
	// Renga: the surface owns its hook. When a client goes away, libwayland
	// destroys its objects in ID order, so the wl_surface goes before the
	// xdg_surface made for it; the xdg_surface must not use it afterwards.
	fXdgSurface->fSurface = NULL;
}

void XdgSurfaceHook::HandleCommit()
{
	// TODO: move to HaikuXdgToplevel/HaikuXdgPopup
	if (fXdgSurface->HasServerDecoration()) {
		// toplevel: window size limits
		if (fXdgSurface->fToplevel != NULL && fXdgSurface->fToplevel->fSizeLimitsDirty) {
			// Renga (batch 12): logical sizes times the display scale
			float scale = HaikuClientScale(fXdgSurface->Client());
			fXdgSurface->Window()->SetSizeLimits(
				fXdgSurface->fToplevel->fMinWidth  == 0 ? 0     : roundf(fXdgSurface->fToplevel->fMinWidth * scale) - 1,
				fXdgSurface->fToplevel->fMaxWidth  == 0 ? 32768 : roundf(fXdgSurface->fToplevel->fMaxWidth * scale) - 1,
				fXdgSurface->fToplevel->fMinHeight == 0 ? 0     : roundf(fXdgSurface->fToplevel->fMinHeight * scale) - 1,
				fXdgSurface->fToplevel->fMaxHeight == 0 ? 32768 : roundf(fXdgSurface->fToplevel->fMaxHeight * scale) - 1
			);
			if (
				fXdgSurface->fToplevel->fMinWidth != 0 && fXdgSurface->fToplevel->fMinWidth == fXdgSurface->fToplevel->fMaxWidth &&
				fXdgSurface->fToplevel->fMinHeight != 0 && fXdgSurface->fToplevel->fMinHeight == fXdgSurface->fToplevel->fMaxHeight
			) {
				fXdgSurface->Window()->SetFlags(fXdgSurface->Window()->Flags() | B_NOT_RESIZABLE);
			} else {
				fXdgSurface->Window()->SetFlags(fXdgSurface->Window()->Flags() & ~B_NOT_RESIZABLE);
			}
			fXdgSurface->fToplevel->fSizeLimitsDirty = false;
		}

		if ((int32_t)fXdgSurface->fToplevel->fResizeSerial - (int32_t)(int32_t)fXdgSurface->fAckSerial > 0) {
		} else if (!fXdgSurface->fGeometry.Equals(fXdgSurface->fPendingGeometry)) {
			BSize oldSize(fXdgSurface->fToplevel->fWidth - 1, fXdgSurface->fToplevel->fHeight - 1);
			BSize newSize = oldSize;

			// Renga (batch 12): geometry is logical; the window and the
			// surface's view are in screen pixels (Renguin display scale)
			float scale = HaikuClientScale(fXdgSurface->Client());
			if (fXdgSurface->Surface()->View() != NULL)
				AppKitPtrs::LockedPtr(fXdgSurface->Surface()->View())->MoveTo(-roundf(fXdgSurface->fPendingGeometry.x * scale), -roundf(fXdgSurface->fPendingGeometry.y * scale));
			newSize.width = roundf(fXdgSurface->fPendingGeometry.width * scale) - 1;
			newSize.height = roundf(fXdgSurface->fPendingGeometry.height * scale) - 1;

			if (oldSize != newSize) {
				fXdgSurface->fToplevel->fSizeChanged = true;
				fXdgSurface->fToplevel->fWidth = (int32_t)newSize.width + 1;
				fXdgSurface->fToplevel->fHeight = (int32_t)newSize.height + 1;

				fXdgSurface->Window()->ResizeTo(newSize.width, newSize.height);
			}
		}
	} else if (fXdgSurface->fToplevel != NULL
		&& fXdgSurface->Surface()->WantsServerFrame()) {
		// Renga (batch 11): a server frame without window geometry (Linux
		// Chromium): the size limits, and once the client has answered the
		// last resize, the window takes the size of what it drew
		HaikuXdgToplevel* toplevel = fXdgSurface->fToplevel;
		if (toplevel->fSizeLimitsDirty) {
			float scale = HaikuClientScale(fXdgSurface->Client());
			fXdgSurface->Window()->SetSizeLimits(
				toplevel->fMinWidth == 0 ? 0 : roundf(toplevel->fMinWidth * scale) - 1,
				toplevel->fMaxWidth == 0 ? 32768 : roundf(toplevel->fMaxWidth * scale) - 1,
				toplevel->fMinHeight == 0 ? 0 : roundf(toplevel->fMinHeight * scale) - 1,
				toplevel->fMaxHeight == 0 ? 32768 : roundf(toplevel->fMaxHeight * scale) - 1);
			toplevel->fSizeLimitsDirty = false;
		}
		if ((int32_t)toplevel->fResizeSerial - (int32_t)fXdgSurface->fAckSerial
				<= 0 && fXdgSurface->Surface()->Bitmap() != NULL) {
			BSize oldSize = fXdgSurface->Window()->Size();
			BSize newSize = fXdgSurface->Surface()->Size();
			if (oldSize != newSize) {
				toplevel->fSizeChanged = true;
				toplevel->fWidth = (int32_t)newSize.width + 1;
				toplevel->fHeight = (int32_t)newSize.height + 1;
				fXdgSurface->Window()->ResizeTo(newSize.width, newSize.height);
			}
		}
	} else {
		if (
			fXdgSurface->fPendingGeometry.x != fXdgSurface->fGeometry.x ||
			fXdgSurface->fPendingGeometry.y != fXdgSurface->fGeometry.y
		) {
			float scale = HaikuClientScale(fXdgSurface->Client());
			fXdgSurface->Window()->MoveBy(
				roundf((fXdgSurface->fGeometry.x - fXdgSurface->fPendingGeometry.x) * scale),
				roundf((fXdgSurface->fGeometry.y - fXdgSurface->fPendingGeometry.y) * scale)
			);
		}
		if (fXdgSurface->Surface()->Bitmap() != NULL) {
			BSize oldSize = fXdgSurface->Window()->Size();
			BSize newSize = fXdgSurface->Surface()->Size();
			if (oldSize != newSize) {
				fXdgSurface->Window()->ResizeTo(newSize.width, newSize.height);
			}
		}
	}
	fXdgSurface->fGeometry = fXdgSurface->fPendingGeometry;

	// initial window show
	if (!fXdgSurface->fSurfaceInitalized && fXdgSurface->Surface()->Bitmap() != NULL) {
		if (fXdgSurface->Surface()->HasDecorationProtocol()) {
			// Renga: the KDE protocol or xdg-decoration
			fXdgSurface->Window()->SetLook(
				fXdgSurface->Surface()->WantsServerFrame()
					? B_TITLED_WINDOW_LOOK : B_NO_BORDER_WINDOW_LOOK);
		}
		if (fXdgSurface->fToplevel != NULL) {
			fXdgSurface->Window()->CenterOnScreen();
		}
		fXdgSurface->Window()->Show();
		fXdgSurface->fSurfaceInitalized = true;
		// Renga: wl_surface.enter for the output it is on
		HaikuOutputSurfaceShown(fXdgSurface->Surface(),
			fXdgSurface->Window()->Frame());
	}

	if (fXdgSurface->fPopup != NULL) {
		BRect wndRect = fXdgSurface->fPopup->fPosition;
		if (fXdgSurface->Geometry().valid && !fXdgSurface->HasServerDecoration())
			wndRect.OffsetBy(-fXdgSurface->Geometry().x, -fXdgSurface->Geometry().y);
		// (logical; ConvertToScreen() applies the display scale)
		fXdgSurface->fPopup->fParent->ConvertToScreen(wndRect);
		fXdgSurface->Window()->MoveTo(wndRect.left, wndRect.top);
	}

	// initial configure
	if (!fXdgSurface->fConfigureCalled) {
		if (fXdgSurface->fToplevel != NULL) {
			fXdgSurface->fToplevel->DoSendConfigure();
		}
		if (fXdgSurface->fPopup != NULL) {
			BRect wndRect = fXdgSurface->fPopup->fPosition;
			fXdgSurface->fPopup->SendConfigure(wndRect.left, wndRect.top, (int32_t)wndRect.Width() + 1, (int32_t)wndRect.Height() + 1);
		}
		fXdgSurface->fConfigureCalled = true;
		fXdgSurface->SendConfigure(fXdgSurface->NextSerial());
	}
}


//#pragma mark - HaikuXdgSurface
HaikuXdgSurface::~HaikuXdgSurface()
{
	// Renga: in ID order (a client that went away) the role objects outlive
	// the xdg_surface; they must not use it afterwards.
	if (fToplevel != NULL)
		fToplevel->fXdgSurface = NULL;
	if (fPopup != NULL)
		fPopup->fXdgSurface = NULL;
	if (fSurface != NULL)
		fSurface->SetHook(NULL);
}


uint32_t HaikuXdgSurface::NextSerial()
{
	return (uint32_t)atomic_add((int32*)&fSerial, 1);
}

BWindow *HaikuXdgSurface::Window()
{
	if (fToplevel != NULL) return fToplevel->Window();
	if (fPopup != NULL) return fPopup->Window();
	return NULL;
}

bool HaikuXdgSurface::HasServerDecoration()
{
	return
		Geometry().valid &&
		Surface()->WantsServerFrame();
}

// Renga (batch 12): the points are logical (Renguin display scale)
void HaikuXdgSurface::ConvertFromScreen(BPoint &pt)
{
	float scale = HaikuClientScale(Client());
	Window()->ConvertFromScreen(&pt);
	pt.x /= scale;
	pt.y /= scale;
	if (fGeometry.valid)
		pt -= BPoint(fGeometry.x, fGeometry.y);
}

void HaikuXdgSurface::ConvertToScreen(BPoint &pt)
{
	float scale = HaikuClientScale(Client());
	if (fGeometry.valid)
		pt += BPoint(fGeometry.x, fGeometry.y);
	pt.x *= scale;
	pt.y *= scale;

	Window()->ConvertToScreen(&pt);
}

static void
scale_rect(BRect &rect, float scale)
{
	rect.left *= scale;
	rect.top *= scale;
	rect.right = (rect.right + 1) * scale - 1;
	rect.bottom = (rect.bottom + 1) * scale - 1;
}

void HaikuXdgSurface::ConvertFromScreen(BRect &rect)
{
	Window()->ConvertFromScreen(&rect);
	scale_rect(rect, 1 / HaikuClientScale(Client()));
	if (fGeometry.valid && !HasServerDecoration())
		rect.OffsetBy(-fGeometry.x, -fGeometry.y);
}

void HaikuXdgSurface::ConvertToScreen(BRect &rect)
{
	if (fGeometry.valid && !HasServerDecoration())
		rect.OffsetBy(fGeometry.x, fGeometry.y);
	scale_rect(rect, HaikuClientScale(Client()));

	Window()->ConvertToScreen(&rect);
}


void HaikuXdgSurface::HandleGetToplevel(uint32_t id)
{
	fToplevel = HaikuXdgToplevel::Create(this, id);
}

void HaikuXdgSurface::HandleGetPopup(uint32_t id, struct wl_resource *parent, struct wl_resource *positioner)
{
	fPopup = HaikuXdgPopup::Create(this, id, parent, positioner);
}

void HaikuXdgSurface::HandleSetWindowGeometry(int32_t x, int32_t y, int32_t width, int32_t height)
{
	fPendingGeometry = {
		.valid = true,
		.x = x,
		.y = y,
		.width = width,
		.height = height
	};
}

void HaikuXdgSurface::HandleAckConfigure(uint32_t serial)
{
	fAckSerial = serial;
	fConfigurePending = false;
}


HaikuXdgSurface *HaikuXdgSurface::Create(struct HaikuXdgWmBase *client, struct HaikuSurface *surface, uint32_t id)
{
	HaikuXdgSurface *xdgSurface = new(std::nothrow) HaikuXdgSurface();
	if (!xdgSurface) {
		wl_client_post_no_memory(client->Client());
		return NULL;
	}
	if (!xdgSurface->Init(client->Client(), client->Version(), id)) {
		return NULL;
	}
	xdgSurface->fSurface = surface;
	xdgSurface->fRoot = xdgSurface;
	surface->fXdgSurface = xdgSurface;
	surface->SetHook(new XdgSurfaceHook(xdgSurface));

	return xdgSurface;
}
