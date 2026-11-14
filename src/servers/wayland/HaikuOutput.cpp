#include "HaikuOutput.h"
#include "HaikuCompositor.h"
#include "XdgOutputUnstableV1.h"

#include <Screen.h>
#include <Deskbar.h>
#include <AutoDeleter.h>

#include <algorithm>
#include <cmath>
#include <stdio.h>

extern const struct wl_interface wl_output_interface;
extern const struct wl_interface zxdg_output_manager_v1_interface;


enum {
	OUTPUT_VERSION = 4,
		// Renga: 4 for name and description
	XDG_OUTPUT_MANAGER_VERSION = 3,
};

static std::vector<HaikuOutputGlobal*> sOutputGlobals;


class HaikuOutput: public WlOutput {
private:
	friend class HaikuOutputGlobal;
	HaikuOutputGlobal *fGlobal;

public:
	std::vector<HaikuSurface*> fEntered;
		// surfaces that got wl_surface.enter for this output

	HaikuOutput(HaikuOutputGlobal *global): fGlobal(global) {}
	virtual ~HaikuOutput();
	HaikuOutputGlobal *Global() {return fGlobal;}
};


HaikuOutput::~HaikuOutput()
{
	if (fGlobal == NULL)
		return;
	auto& list = fGlobal->fOutputs;
	list.erase(std::remove(list.begin(), list.end(), this), list.end());
}


static void
read_screen(int32 index, BRect& frame, float& refresh)
{
	BScreen screen(B_MAIN_SCREEN_ID);
	for (int32 i = 0; i < index && screen.IsValid(); i++)
		screen.SetToNext();
	frame = screen.IsValid() ? screen.Frame() : BRect(0, 0, 1023, 767);
	refresh = 60;
	display_mode mode;
	if (screen.IsValid() && screen.GetMode(&mode) == B_OK
		&& mode.timing.h_total != 0 && mode.timing.v_total != 0) {
		float rate = float(mode.timing.pixel_clock * 1000)
			/ float(mode.timing.h_total * mode.timing.v_total);
		if (!std::isnan(rate) && rate > 1)
			refresh = rate;
	}
}


static int32
count_screens()
{
	BScreen screen(B_MAIN_SCREEN_ID);
	int32 count = 0;
	while (screen.IsValid()) {
		count++;
		if (screen.SetToNext() != B_OK)
			break;
	}
	return count < 1 ? 1 : count;
}


HaikuOutputGlobal *HaikuOutputGlobal::Create(struct wl_display *display, int32 index)
{
	ObjectDeleter<HaikuOutputGlobal> global(new(std::nothrow) HaikuOutputGlobal());
	if (!global.IsSet()) return NULL;
	global->fIndex = index;
	read_screen(index, global->fFrame, global->fRefresh);
	if (!global->Init(display, &wl_output_interface, OUTPUT_VERSION)) return NULL;
	sOutputGlobals.push_back(global.Get());
	return global.Detach();
}

HaikuOutputGlobal::~HaikuOutputGlobal()
{
	for (HaikuOutput *output : fOutputs)
		output->fGlobal = NULL;
	sOutputGlobals.erase(std::remove(sOutputGlobals.begin(),
		sOutputGlobals.end(), this), sOutputGlobals.end());
}

void HaikuOutputGlobal::SendState(HaikuOutput *output)
{
	int32_t width = (int32_t)fFrame.Width() + 1;
	int32_t height = (int32_t)fFrame.Height() + 1;
	// Physical size from a 96 dpi assumption (0.2646 mm per pixel); Haiku
	// does not know the monitor's.
	output->SendGeometry((int32_t)fFrame.left, (int32_t)fFrame.top,
		(int32_t)(width * 0.2646), (int32_t)(height * 0.2646),
		WlOutput::subpixelUnknown, "Renga", "Screen", WlOutput::transformNormal);
	output->SendMode(WlOutput::modeCurrent | WlOutput::modePreferred, width,
		height, (int32_t)lroundf(fRefresh * 1000));
	if (output->Version() >= 2)
		output->SendScale(1);
	if (output->Version() >= 4) {
		char name[32];
		snprintf(name, sizeof(name), "RENGA-%" B_PRId32, fIndex + 1);
		output->SendName(name);
		char description[64];
		snprintf(description, sizeof(description), "Screen %" B_PRId32 " (%dx%d)",
			fIndex + 1, (int)width, (int)height);
		output->SendDescription(description);
	}
	if (output->Version() >= 2)
		output->SendDone();
}

void HaikuOutputGlobal::Bind(struct wl_client *wl_client, uint32_t version, uint32_t id)
{
	HaikuOutput *output = new(std::nothrow) HaikuOutput(this);
	if (output == NULL) {
		wl_client_post_no_memory(wl_client);
		return;
	}
	if (!output->Init(wl_client, version, id)) {
		return;
	}
	fOutputs.push_back(output);
	SendState(output);
}

void HaikuOutputGlobal::Update(BRect frame, float refresh)
{
	if (frame == fFrame && fabsf(refresh - fRefresh) < 0.5f)
		return;
	fFrame = frame;
	fRefresh = refresh;
	for (HaikuOutput *output : fOutputs)
		SendState(output);
}

HaikuOutput *HaikuOutputGlobal::OutputFor(struct wl_client *client)
{
	for (HaikuOutput *output : fOutputs) {
		if (output->Client() == client)
			return output;
	}
	return NULL;
}


void
HaikuOutputCreateGlobals(struct wl_display *display)
{
	int32 count = count_screens();
	for (int32 i = 0; i < count; i++)
		HaikuOutputGlobal::Create(display, i);
}


void
HaikuOutputCheckScreens(struct wl_display *display)
{
	int32 count = count_screens();
	// screens that went away: the global goes (clients get
	// wl_registry.global_remove)
	while ((int32)sOutputGlobals.size() > count) {
		HaikuOutputGlobal *last = sOutputGlobals.back();
		wl_global_destroy(last->ToGlocal());
		delete last;
	}
	for (HaikuOutputGlobal *global : sOutputGlobals) {
		BRect frame;
		float refresh;
		read_screen(global->Index(), frame, refresh);
		global->Update(frame, refresh);
	}
	for (int32 i = sOutputGlobals.size(); i < count; i++)
		HaikuOutputGlobal::Create(display, i);
}


void
HaikuOutputSurfaceShown(HaikuSurface *surface, BRect windowFrame)
{
	for (HaikuOutputGlobal *global : sOutputGlobals) {
		if (!global->Frame().Intersects(windowFrame) && global->Index() != 0)
			continue;
		HaikuOutput *output = global->OutputFor(surface->Client());
		if (output == NULL)
			continue;
		if (std::find(output->fEntered.begin(), output->fEntered.end(), surface)
				!= output->fEntered.end())
			continue;
		output->fEntered.push_back(surface);
		surface->SendEnter(output->ToResource());
		break;
	}
}


void
HaikuOutputSurfaceGone(HaikuSurface *surface)
{
	for (HaikuOutputGlobal *global : sOutputGlobals) {
		for (HaikuOutput *output : global->fOutputs) {
			auto& list = output->fEntered;
			list.erase(std::remove(list.begin(), list.end(), surface), list.end());
		}
	}
}


BRect
HaikuOutputUsableFrame(BRect windowFrame)
{
	BRect frame(0, 0, 1023, 767);
	for (HaikuOutputGlobal *global : sOutputGlobals) {
		if (global->Index() == 0 || global->Frame().Intersects(windowFrame)) {
			frame = global->Frame();
			if (global->Frame().Intersects(windowFrame))
				break;
		}
	}
	// Deskbar takes a strip or a corner of the main screen. Its location
	// names (and whether it is expanded) do not say reliably which part of
	// the screen it covers, so go by its frame: a wide bar is a strip at
	// the top or the bottom, a tall one a column at the left or the right
	// (the side it is nearer to).
	BDeskbar deskbar;
	BRect bar = deskbar.Frame() & frame;
	if (bar.IsValid()) {
		BRect usable = frame;
		if (bar.Width() >= bar.Height()) {
			if (bar.top - frame.top <= frame.bottom - bar.bottom)
				usable.top = bar.bottom + 1;
			else
				usable.bottom = bar.top - 1;
		} else {
			if (bar.left - frame.left <= frame.right - bar.right)
				usable.left = bar.right + 1;
			else
				usable.right = bar.left - 1;
		}
		// Never less than half the screen (an unusual Deskbar setup).
		if (usable.Width() >= frame.Width() / 2
			&& usable.Height() >= frame.Height() / 2)
			frame = usable;
	}
	return frame;
}


//#pragma mark - xdg-output

class HaikuXdgOutput: public ZxdgOutputV1 {
};

class HaikuXdgOutputManager: public ZxdgOutputManagerV1 {
protected:
	virtual ~HaikuXdgOutputManager() = default;
public:
	void HandleGetXdgOutput(uint32_t id, struct wl_resource *outputResource) final;
};


HaikuXdgOutputManagerGlobal *HaikuXdgOutputManagerGlobal::Create(struct wl_display *display)
{
	ObjectDeleter<HaikuXdgOutputManagerGlobal> global(new(std::nothrow) HaikuXdgOutputManagerGlobal());
	if (!global.IsSet()) return NULL;
	if (!global->Init(display, &zxdg_output_manager_v1_interface, XDG_OUTPUT_MANAGER_VERSION)) return NULL;
	return global.Detach();
}

void HaikuXdgOutputManagerGlobal::Bind(struct wl_client *wl_client, uint32_t version, uint32_t id)
{
	HaikuXdgOutputManager *manager = new(std::nothrow) HaikuXdgOutputManager();
	if (manager == NULL) {
		wl_client_post_no_memory(wl_client);
		return;
	}
	manager->Init(wl_client, version, id);
}

void HaikuXdgOutputManager::HandleGetXdgOutput(uint32_t id, struct wl_resource *outputResource)
{
	HaikuXdgOutput *xdgOutput = new(std::nothrow) HaikuXdgOutput();
	if (xdgOutput == NULL) {
		wl_client_post_no_memory(Client());
		return;
	}
	if (!xdgOutput->Init(Client(), Version(), id))
		return;
	HaikuOutput *output = (HaikuOutput*)WlResource::FromResource(outputResource);
	HaikuOutputGlobal *global = output != NULL ? output->Global() : NULL;
	BRect frame = global != NULL ? global->Frame() : BRect(0, 0, 1023, 767);
	xdgOutput->SendLogicalPosition((int32_t)frame.left, (int32_t)frame.top);
	xdgOutput->SendLogicalSize((int32_t)frame.Width() + 1,
		(int32_t)frame.Height() + 1);
	if (xdgOutput->Version() >= 2) {
		char name[32];
		snprintf(name, sizeof(name), "RENGA-%" B_PRId32,
			global != NULL ? global->Index() + 1 : 1);
		xdgOutput->SendName(name);
		xdgOutput->SendDescription("Renga screen");
	}
	// Version 3: wl_output.done ends the sequence instead of xdg_output.done.
	if (xdgOutput->Version() >= 3) {
		if (output != NULL)
			output->SendDone();
	} else
		xdgOutput->SendDone();
}
