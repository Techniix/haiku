#include "HaikuXdgToplevel.h"
#include "HaikuXdgShell.h"
#include "HaikuXdgSurface.h"
#include "HaikuCompositor.h"
#include "HaikuSeat.h"
#include "HaikuServerDecoration.h"
#include "HaikuXdgDecoration.h"
#include "HaikuOutput.h"
#include "RengaProtocols.h"
#include "RengaClients.h"
#include "WaylandEnv.h"
#include <wayland-server-core.h>
#include <wayland-server-protocol.h>
#include <xdg-shell-protocol.h>

#include "AppKitPtrs.h"

#include <Window.h>
#include <Screen.h>

#include <cmath>
#include <algorithm>

static void Assert(bool cond) {if (!cond) abort();}

// Renga harness: set by WaylandServer's 'RgTz' (harness images only)
bool gRengaTestResizeAfterDestroy = false;


class WaylandWindow: public BWindow {
private:
	friend class HaikuXdgToplevel;
	HaikuXdgToplevel *fToplevel;

	void UpdateRefreshRate();

public:
	WaylandWindow(HaikuXdgToplevel *toplevel, BRect frame, const char* title, window_look look, window_feel feel, uint32 flags, uint32 workspace = B_CURRENT_WORKSPACE);
	virtual ~WaylandWindow() = default;

	HaikuXdgToplevel *Toplevel() {return fToplevel;}

	bool QuitRequested() final;
	void WindowActivated(bool isActive) final;
	void WorkspacesChanged(uint32 oldWorkspace, uint32 newWorkspace) final;
	void FrameResized(float newWidth, float newHeight) final;
	void DispatchMessage(BMessage *msg, BHandler *target) final;
};

WaylandWindow::WaylandWindow(HaikuXdgToplevel *toplevel, BRect frame, const char* title, window_look look, window_feel feel, uint32 flags, uint32 workspace):
	BWindow(frame, title, look, feel, flags, workspace),
	fToplevel(toplevel)
{
}

bool WaylandWindow::QuitRequested()
{
	WaylandEnv vlEnv(this);
	if (fToplevel == NULL)
		return true;
	fToplevel->SendClose();
	return false;
}

void WaylandWindow::WindowActivated(bool isActive)
{
	WaylandEnv vlEnv(this);
	// Renga: the activation message can arrive after the client destroyed
	// the toplevel (or went away); the window then has none.
	if (fToplevel == NULL)
		return;
	// Renga: a locked or confined pointer is released with the window
	if (!isActive && fToplevel->XdgSurface() != NULL
		&& fToplevel->XdgSurface()->Surface() != NULL)
		HaikuPointerConstraintsFocusLost(fToplevel->XdgSurface()->Surface());
	if (isActive != fToplevel->fState.activated
		&& fToplevel->XdgSurface() != NULL) {
		fToplevel->fState.activated = !fToplevel->fState.activated;
		fToplevel->DoSendConfigure();
		fToplevel->XdgSurface()->SendConfigure(fToplevel->XdgSurface()->NextSerial());
	}

	if (isActive)
		UpdateRefreshRate();
}

void WaylandWindow::WorkspacesChanged(uint32 oldWorkspace, uint32 newWorkspace)
{
	UpdateRefreshRate();
}

void WaylandWindow::FrameResized(float newWidth, float newHeight)
{
	WaylandEnv vlEnv(this);

	// Renga (batch 14, hardware report): a resize app_server queued before
	// the client destroyed its xdg_toplevel arrives after
	// ~HaikuXdgToplevel() cleared fToplevel (the window only quits once
	// the B_QUIT_REQUESTED posted then is handled); the compositor
	// crashed here, taking every Linux window with it
	if (fToplevel == NULL)
		return;

	if (fToplevel->fSizeChanged) {
		fToplevel->fSizeChanged = false;
		return;
	}

	// Renga (batch 11): Renga draws the frame, so the user resizes the
	// window and the client must hear of it, also when it set no window
	// geometry (Linux Chromium does not: its page kept its old size and
	// was cut off after a resize on the maintainer's machine)
	if (fToplevel->XdgSurface() == NULL
		|| fToplevel->XdgSurface()->Surface() == NULL
		|| !fToplevel->XdgSurface()->Surface()->WantsServerFrame())
		return;

	if ((int32_t)newWidth + 1 == fToplevel->fWidth && (int32_t)newHeight + 1 == fToplevel->fHeight)
		return;

	fToplevel->fWidth = (int32_t)newWidth + 1;
	fToplevel->fHeight = (int32_t)newHeight + 1;

	fToplevel->fResizeSerial = fToplevel->XdgSurface()->NextSerial();

	fToplevel->DoSendConfigure();
	fToplevel->XdgSurface()->SendConfigure(fToplevel->fResizeSerial);
}

void WaylandWindow::DispatchMessage(BMessage *msg, BHandler *target)
{
	switch (msg->what) {
	case B_KEY_UP:
	case B_UNMAPPED_KEY_UP:
	case B_KEY_DOWN:
	case B_UNMAPPED_KEY_DOWN:
		{
			int32 key;
			uint32 modifiers;
			if (msg->FindInt32("key", &key) == B_OK) {
				msg->FindInt32("modifiers", (int32*)&modifiers);
				if (key == 0x0e) { // Print Screen
					BWindow::DispatchMessage(msg, target);
					return;
				}
				if (modifiers & B_CONTROL_KEY) {
					switch (key) {
						case 0x26: // Command + Tab
							BWindow::DispatchMessage(msg, target);
							return;
					}
				}
			}
			target->MessageReceived(msg);
			return;
		}
	}
	BWindow::DispatchMessage(msg, target);
}

void WaylandWindow::UpdateRefreshRate()
{
	float rate = 60.0;

	BScreen scr(B_MAIN_SCREEN_ID);
	display_mode mode;
	if (scr.GetMode(&mode) == B_OK)	{
		float screenrate = rint(10 * float(mode.timing.pixel_clock * 1000)
						/ float(mode.timing.h_total * mode.timing.v_total)) / 10.0;
		if (!std::isnan(screenrate))
			rate = std::clamp(screenrate, 25.0f, 144.0f);
	}

	SetPulseRate(1000000 / rate);
}


//#pragma mark - HaikuXdgToplevel

/*!	Renga (xdg_toplevel version 4): the largest content size that fits:
	the usable area of the window's screen (Deskbar excluded) minus Renga's
	frame when the window has one. Chromium sizes its first window from
	it.
*/
void HaikuXdgToplevel::SendBounds()
{
	if (Version() < 4)
		return;
	BRect windowFrame = fWindow != NULL ? fWindow->Frame() : BRect();
	BRect usable = HaikuOutputUsableFrame(windowFrame);
	float left = 0, top = 0, right = 0, bottom = 0;
	HaikuSurface *surface = fXdgSurface != NULL ? fXdgSurface->Surface() : NULL;
	if (surface != NULL && surface->WantsServerFrame()) {
		BRect decorator = fWindow != NULL && fWindow->Look() == B_TITLED_WINDOW_LOOK
			? fWindow->DecoratorFrame() : BRect();
		if (decorator.IsValid() && windowFrame.IsValid()) {
			left = windowFrame.left - decorator.left;
			top = windowFrame.top - decorator.top;
			right = decorator.right - windowFrame.right;
			bottom = decorator.bottom - windowFrame.bottom;
		} else {
			// before the window has its frame: the default decorator's
			// borders and tab
			left = right = bottom = 5;
			top = 5 + 21;
		}
	}
	int32_t width = (int32_t)(usable.Width() + 1 - left - right);
	int32_t height = (int32_t)(usable.Height() + 1 - top - bottom);
	// Renga (batch 12): in logical pixels (Renguin display scale)
	float scale = HaikuClientScale(Client());
	width = (int32_t)(width / scale);
	height = (int32_t)(height / scale);
	SendConfigureBounds(width > 0 ? width : 0, height > 0 ? height : 0);
}

void HaikuXdgToplevel::SendCapabilities()
{
	if (Version() < 5)
		return;
	uint32_t capabilities[] = {
		XdgToplevel::wmCapabilitiesMaximize,
		XdgToplevel::wmCapabilitiesFullscreen,
		XdgToplevel::wmCapabilitiesMinimize
	};
	struct wl_array array {
		.size = sizeof(capabilities),
		.alloc = sizeof(capabilities),
		.data = capabilities
	};
	SendWmCapabilities(&array);
}

void HaikuXdgToplevel::DoSendConfigure()
{
	SendBounds();
	uint32_t stateArray[3];
	struct wl_array state {
		.size = 0,
		.alloc = sizeof(stateArray),
		.data = &stateArray[0]
	};
	uint32_t *p = (uint32_t*)state.data;

	if (fState.maximized ) {*p++ = XdgToplevel::stateMaximized;}
	if (fState.fullscreen) {*p++ = XdgToplevel::stateFullscreen;}
	if (fState.activated ) {*p++ = XdgToplevel::stateActivated;}

	state.size = (uint8_t*)p - (uint8_t*)state.data;
	// Renga (batch 12): fWidth and fHeight are the window's size in screen
	// pixels; the client hears logical pixels (Renguin display scale)
	float scale = HaikuClientScale(Client());
	SendConfigure((int32_t)lroundf(fWidth / scale),
		(int32_t)lroundf(fHeight / scale), &state);
}

void HaikuXdgToplevel::HandleSetParent(struct wl_resource *_parent)
{
	// TODO: update root window when parent window is closed
	HaikuXdgToplevel *parent = HaikuXdgToplevel::FromResource(_parent);
	if (parent == NULL) {
		if (XdgSurface()->fRoot != XdgSurface()) {
			fWindow->RemoveFromSubset(XdgSurface()->fRoot->Window());
			fWindow->SetFeel(B_NORMAL_WINDOW_FEEL);
			XdgSurface()->fRoot = XdgSurface();
		}
	} else {
		XdgSurface()->fRoot = parent->XdgSurface()->fRoot;
		fWindow->SetFeel(B_FLOATING_SUBSET_WINDOW_FEEL);
		fWindow->AddToSubset(parent->XdgSurface()->fRoot->Window());
	}
}

void HaikuXdgToplevel::HandleSetTitle(const char *title)
{
	fWindow->SetTitle(title);
}

void HaikuXdgToplevel::HandleSetAppId(const char *app_id)
{
	// Renga (batch 12): names the program's Deskbar entry
	RengaClientsSetAppId(this, app_id);
}

void HaikuXdgToplevel::HandleShowWindowMenu(struct wl_resource *seat, uint32_t serial, int32_t x, int32_t y)
{
}

void HaikuXdgToplevel::HandleMove(struct wl_resource *_seat, uint32_t serial)
{
	HaikuSeat *seat = HaikuSeat::FromResource(_seat);
	seat->GetGlobal()->DoTrack(HaikuSeatGlobal::trackMove);
}

void HaikuXdgToplevel::HandleResize(struct wl_resource *_seat, uint32_t serial, uint32_t edges)
{
	HaikuSeat *seat = HaikuSeat::FromResource(_seat);
	seat->GetGlobal()->DoTrack(HaikuSeatGlobal::trackResize, {.resizeEdge = (XdgToplevel::ResizeEdge)edges});
}

void HaikuXdgToplevel::HandleSetMaxSize(int32_t width, int32_t height)
{
	fSizeLimitsDirty = true;
	fMaxWidth = width;
	fMaxHeight = height;
}

void HaikuXdgToplevel::HandleSetMinSize(int32_t width, int32_t height)
{
	fSizeLimitsDirty = true;
	fMinWidth = width;
	fMinHeight = height;
}

/*!	Renga (batch 11): maximize and restore from the client (Chromium's
	title bar buttons, double clicks): the usable area of the window's
	screen less Renga's frame, the old frame kept for the restore; the
	configure carries the new size and the maximized state.
*/
void HaikuXdgToplevel::HandleSetMaximized()
{
	if (fState.maximized || fState.fullscreen || fWindow == NULL)
		return;
	fState.maximized = true;
	fSavedPos = fWindow->Frame();
	BRect usable = HaikuOutputUsableFrame(fWindow->Frame());
	BRect decorator = fWindow->Look() == B_TITLED_WINDOW_LOOK
		? fWindow->DecoratorFrame() : fWindow->Frame();
	BRect frame = fWindow->Frame();
	float left = frame.left - decorator.left, top = frame.top - decorator.top;
	float right = decorator.right - frame.right;
	float bottom = decorator.bottom - frame.bottom;
	BRect target(usable.left + left, usable.top + top, usable.right - right,
		usable.bottom - bottom);
	fSizeChanged = true;
	fWindow->MoveTo(target.LeftTop());
	fWindow->ResizeTo(target.Width(), target.Height());
	fWidth = (int32_t)target.Width() + 1;
	fHeight = (int32_t)target.Height() + 1;
	fResizeSerial = XdgSurface()->NextSerial();
	DoSendConfigure();
	XdgSurface()->SendConfigure(fResizeSerial);
}

void HaikuXdgToplevel::HandleUnsetMaximized()
{
	if (!fState.maximized || fWindow == NULL)
		return;
	fState.maximized = false;
	fSizeChanged = true;
	fWindow->MoveTo(fSavedPos.LeftTop());
	fWindow->ResizeTo(fSavedPos.Width(), fSavedPos.Height());
	fWidth = (int32_t)fSavedPos.Width() + 1;
	fHeight = (int32_t)fSavedPos.Height() + 1;
	fResizeSerial = XdgSurface()->NextSerial();
	DoSendConfigure();
	XdgSurface()->SendConfigure(fResizeSerial);
}

void HaikuXdgToplevel::HandleSetFullscreen(struct wl_resource *output)
{
	if (fState.fullscreen) return;
	fState.fullscreen = true;
	fSizeChanged = true;
	fSavedPos = fWindow->Frame();
	BRect screenFrame = BScreen(fWindow).Frame();
	fWindow->MoveTo(screenFrame.LeftTop());
	fWindow->ResizeTo(screenFrame.Width(), screenFrame.Height());
	fWidth = (int32_t)screenFrame.Width() + 1;
	fHeight = (int32_t)screenFrame.Height() + 1;
	DoSendConfigure();
	XdgSurface()->SendConfigure(XdgSurface()->NextSerial());
}

void HaikuXdgToplevel::HandleUnsetFullscreen()
{
	if (!fState.fullscreen) return;
	fState.fullscreen = false;
	fSizeChanged = true;
	fWindow->MoveTo(fSavedPos.LeftTop());
	fWindow->ResizeTo(fSavedPos.Width(), fSavedPos.Height());
	fWidth = (int32_t)fSavedPos.Width() + 1;
	fHeight = (int32_t)fSavedPos.Height() + 1;
	DoSendConfigure();
	XdgSurface()->SendConfigure(XdgSurface()->NextSerial());
}

void HaikuXdgToplevel::HandleSetMinimized()
{
	// Renga: offered in wm_capabilities
	if (fWindow != NULL)
		fWindow->Minimize(true);
}


HaikuXdgToplevel *HaikuXdgToplevel::Create(HaikuXdgSurface *xdgSurface, uint32_t id)
{
	HaikuXdgToplevel *xdgToplevel = new(std::nothrow) HaikuXdgToplevel();
	if (!xdgToplevel) {
		wl_client_post_no_memory(xdgSurface->Client());
		return NULL;
	}
	if (!xdgToplevel->Init(xdgSurface->Client(), xdgSurface->Version(), id)) {
		return NULL;
	}

	xdgToplevel->fXdgSurface = xdgSurface;
	xdgToplevel->fWindow = new WaylandWindow(xdgToplevel, BRect(), "", B_NO_BORDER_WINDOW_LOOK, B_NORMAL_WINDOW_FEEL, 0);
	xdgSurface->fSurface->AttachWindow(xdgToplevel->fWindow);
	xdgToplevel->SendCapabilities();
	RengaClientsToplevelAdded(xdgToplevel);

	return xdgToplevel;
}

HaikuXdgToplevel::~HaikuXdgToplevel()
{
	HaikuXdgDecorationToplevelGone(fDecoration);
	RengaClientsToplevelRemoved(this);

	// Renga: the xdg_surface and the wl_surface may be gone already (a
	// client that went away; see ~HaikuXdgSurface()).
	if (fXdgSurface != NULL) {
		if (fXdgSurface->fSurface != NULL)
			fXdgSurface->fSurface->Detach();
		fXdgSurface->fToplevel = NULL;
	}

	if (fWindow != NULL) {
		fWindow->fToplevel = NULL;
		if (gRengaTestResizeAfterDestroy) {
			// harness (wayland_test toplevel_gone_resize): the resize
			// app_server may have queued before this destruction
			gRengaTestResizeAfterDestroy = false;
			BMessage resized(B_WINDOW_RESIZED);
			resized.AddInt32("width", 619);
			resized.AddInt32("height", 619);
			resized.AddInt64("when", system_time());
			fWindow->PostMessage(&resized);
		}
		fWindow->PostMessage(B_QUIT_REQUESTED);
		//fWindow->Lock();
		//fWindow->Quit();
		fWindow = NULL;
	}
}
