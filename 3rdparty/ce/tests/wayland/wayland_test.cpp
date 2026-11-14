/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 */

/*!	In-guest test: Renga's Wayland compositor (roadmap F-43 phase C), as a
	native Wayland client sees it over the socket. The client is linked
	against Renga's libwayland (src/libs/wayland, socket transport).

	  compositor          the compositor runs (launch_daemon starts it; the
	                      test starts it itself if the job is off)
	  socket              $XDG_RUNTIME_DIR/wayland-0 is a socket
	  connect             wl_display_connect() and a roundtrip
	  globals             wl_compositor, wl_subcompositor, wl_shm, wl_seat,
	                      wl_data_device_manager, xdg_wm_base, wp_viewporter
	                      and at least one wl_output are offered
	  outputs             one wl_output per screen, whose current mode is
	                      the screen's size
	  shm_formats         wl_shm offers ARGB8888 and XRGB8888
	  shm_window          an xdg_toplevel with a wl_shm buffer in a memfd is
	                      configured and appears: the screen shows its
	                      colour (BScreen::GetBitmap), and the harness saves
	                      a screenshot (screen-wayland_shm_window)
	  clipboard_to_native a wl_data_source set as the selection lands in the
	                      system clipboard as text/plain
	  clipboard_from_native text put on the system clipboard reaches the
	                      client as a selection offer it can read
	  disconnect          the same compositor team keeps running after the
	                      client and its objects go away

	Output format: see 3rdparty/ce/README.md.
*/


#include <Application.h>
#include <Bitmap.h>
#include <Clipboard.h>
#include <Entry.h>
#include <Roster.h>
#include <Screen.h>

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <string>
#include <vector>

#include <wayland-client.h>
#include "xdg-shell-client-protocol.h"


#define SUITE "wayland"

static const char* kCompositorSignature
	= "application/x-vnd.Renga-WaylandCompositor";
static const char* kRuntimeDir = "/tmp/xdg-runtime";

// The window: an unusual colour the desktop does not show anywhere else.
static const int32 kWidth = 240;
static const int32 kHeight = 160;
static const uint32 kColor = 0x00129a5e;		// XRGB8888

static int sFailures = 0;


static void
report(const char* name, bool pass, const char* format, ...)
{
	char detail[600];
	va_list args;
	va_start(args, format);
	vsnprintf(detail, sizeof(detail), format, args);
	va_end(args);
	printf("CE-TEST %s.%s %s %s\n", SUITE, name, pass ? "PASS" : "FAIL",
		detail);
	fflush(stdout);
	if (!pass)
		sFailures++;
}


// #pragma mark - client state


struct OutputInfo {
	wl_output*	output;
	int32		width;
	int32		height;
	bool		done;
};

struct Client {
	wl_display*		display = NULL;
	wl_registry*	registry = NULL;
	wl_compositor*	compositor = NULL;
	wl_shm*			shm = NULL;
	wl_seat*		seat = NULL;
	wl_data_device_manager* dataDeviceManager = NULL;
	xdg_wm_base*	wmBase = NULL;
	std::vector<std::string> globals;
	std::vector<OutputInfo*> outputs;
	bool			argb = false;
	bool			xrgb = false;

	// window
	bool			configured = false;
	uint32			configureSerial = 0;

	// selection
	wl_data_offer*	pendingOffer = NULL;
	std::vector<std::string> pendingTypes;
	wl_data_offer*	selection = NULL;
	std::vector<std::string> selectionTypes;
	int				selections = 0;
};


static void
output_geometry(void*, wl_output*, int32_t, int32_t, int32_t, int32_t,
	int32_t, const char*, const char*, int32_t)
{
}


static void
output_mode(void* data, wl_output*, uint32_t flags, int32_t width,
	int32_t height, int32_t)
{
	OutputInfo* info = (OutputInfo*)data;
	if ((flags & WL_OUTPUT_MODE_CURRENT) != 0) {
		info->width = width;
		info->height = height;
	}
}


static void
output_done(void* data, wl_output*)
{
	((OutputInfo*)data)->done = true;
}


static void
output_scale(void*, wl_output*, int32_t)
{
}


static const wl_output_listener kOutputListener = {
	output_geometry, output_mode, output_done, output_scale
};


static void
shm_format(void* data, wl_shm*, uint32_t format)
{
	Client* client = (Client*)data;
	if (format == WL_SHM_FORMAT_ARGB8888)
		client->argb = true;
	if (format == WL_SHM_FORMAT_XRGB8888)
		client->xrgb = true;
}


static const wl_shm_listener kShmListener = { shm_format };


static void
wm_base_ping(void*, xdg_wm_base* base, uint32_t serial)
{
	xdg_wm_base_pong(base, serial);
}


static const xdg_wm_base_listener kWmBaseListener = { wm_base_ping };


static void
registry_global(void* data, wl_registry* registry, uint32_t name,
	const char* interface, uint32_t version)
{
	Client* client = (Client*)data;
	client->globals.push_back(interface);
	if (strcmp(interface, "wl_compositor") == 0) {
		client->compositor = (wl_compositor*)wl_registry_bind(registry, name,
			&wl_compositor_interface, 1);
	} else if (strcmp(interface, "wl_shm") == 0) {
		client->shm = (wl_shm*)wl_registry_bind(registry, name,
			&wl_shm_interface, 1);
		wl_shm_add_listener(client->shm, &kShmListener, client);
	} else if (strcmp(interface, "wl_seat") == 0) {
		client->seat = (wl_seat*)wl_registry_bind(registry, name,
			&wl_seat_interface, 1);
	} else if (strcmp(interface, "wl_data_device_manager") == 0) {
		client->dataDeviceManager = (wl_data_device_manager*)
			wl_registry_bind(registry, name, &wl_data_device_manager_interface,
				version < 3 ? version : 3);
	} else if (strcmp(interface, "xdg_wm_base") == 0) {
		client->wmBase = (xdg_wm_base*)wl_registry_bind(registry, name,
			&xdg_wm_base_interface, 1);
		xdg_wm_base_add_listener(client->wmBase, &kWmBaseListener, client);
	} else if (strcmp(interface, "wl_output") == 0) {
		OutputInfo* info = new OutputInfo { NULL, 0, 0, false };
		info->output = (wl_output*)wl_registry_bind(registry, name,
			&wl_output_interface, version < 2 ? version : 2);
		wl_output_add_listener(info->output, &kOutputListener, info);
		client->outputs.push_back(info);
	}
}


static void
registry_global_remove(void*, wl_registry*, uint32_t)
{
}


static const wl_registry_listener kRegistryListener = {
	registry_global, registry_global_remove
};


static bool
has_global(Client& client, const char* name)
{
	for (const std::string& global : client.globals) {
		if (global == name)
			return true;
	}
	return false;
}


//! Dispatches events for up to \a timeout microseconds or until \a done.
static void
dispatch_until(Client& client, bigtime_t timeout, bool* done)
{
	bigtime_t end = system_time() + timeout;
	while ((done == NULL || !*done) && system_time() < end) {
		wl_display_flush(client.display);
		if (wl_display_prepare_read(client.display) != 0) {
			wl_display_dispatch_pending(client.display);
			continue;
		}
		struct pollfd pollFD = { wl_display_get_fd(client.display), POLLIN,
			0 };
		int remaining = (int)((end - system_time()) / 1000);
		if (remaining < 1)
			remaining = 1;
		if (poll(&pollFD, 1, remaining > 50 ? 50 : remaining) > 0)
			wl_display_read_events(client.display);
		else
			wl_display_cancel_read(client.display);
		wl_display_dispatch_pending(client.display);
	}
}


// #pragma mark - window


static void
xdg_surface_configure(void* data, xdg_surface*, uint32_t serial)
{
	Client* client = (Client*)data;
	client->configured = true;
	client->configureSerial = serial;
}


static const xdg_surface_listener kXdgSurfaceListener = {
	xdg_surface_configure
};


static void
toplevel_configure(void*, xdg_toplevel*, int32_t, int32_t, wl_array*)
{
}


static void
toplevel_close(void*, xdg_toplevel*)
{
}


static const xdg_toplevel_listener kToplevelListener = {
	toplevel_configure, toplevel_close
};


//! Counts the screen's pixels of the window's colour.
static int32
count_window_pixels()
{
	BScreen screen;
	BBitmap* bitmap = NULL;
	if (screen.GetBitmap(&bitmap, false) != B_OK || bitmap == NULL)
		return -1;
	int32 count = 0;
	if (bitmap->ColorSpace() == B_RGB32 || bitmap->ColorSpace() == B_RGBA32) {
		const uint8* bits = (const uint8*)bitmap->Bits();
		int32 width = bitmap->Bounds().IntegerWidth() + 1;
		int32 height = bitmap->Bounds().IntegerHeight() + 1;
		for (int32 y = 0; y < height; y++) {
			const uint8* row = bits + y * bitmap->BytesPerRow();
			for (int32 x = 0; x < width; x++) {
				const uint8* pixel = row + x * 4;
				if (pixel[0] == 0x5e && pixel[1] == 0x9a && pixel[2] == 0x12)
					count++;
			}
		}
	} else
		count = -2;
	delete bitmap;
	return count;
}


static void
window_test(Client& client)
{
	if (client.compositor == NULL || client.shm == NULL
		|| client.wmBase == NULL) {
		report("shm_window", false, "missing globals");
		return;
	}

	size_t size = kWidth * kHeight * 4;
	int fd = memfd_create("wayland-test", MFD_CLOEXEC | MFD_ALLOW_SEALING);
	if (fd < 0 || ftruncate(fd, size) != 0) {
		report("shm_window", false, "memfd: %s", strerror(errno));
		return;
	}
	uint32* pixels = (uint32*)mmap(NULL, size, PROT_READ | PROT_WRITE,
		MAP_SHARED, fd, 0);
	if (pixels == MAP_FAILED) {
		report("shm_window", false, "mmap: %s", strerror(errno));
		close(fd);
		return;
	}
	for (size_t i = 0; i < size / 4; i++)
		pixels[i] = kColor;

	wl_shm_pool* pool = wl_shm_create_pool(client.shm, fd, size);
	wl_buffer* buffer = wl_shm_pool_create_buffer(pool, 0, kWidth, kHeight,
		kWidth * 4, WL_SHM_FORMAT_XRGB8888);
	wl_surface* surface = wl_compositor_create_surface(client.compositor);
	xdg_surface* xdgSurface = xdg_wm_base_get_xdg_surface(client.wmBase,
		surface);
	xdg_surface_add_listener(xdgSurface, &kXdgSurfaceListener, &client);
	xdg_toplevel* toplevel = xdg_surface_get_toplevel(xdgSurface);
	xdg_toplevel_add_listener(toplevel, &kToplevelListener, &client);
	xdg_toplevel_set_title(toplevel, "Renga Wayland test");
	wl_surface_commit(surface);

	dispatch_until(client, 10000000, &client.configured);
	if (!client.configured) {
		report("shm_window", false, "no xdg_surface.configure in 10 s");
		return;
	}
	xdg_surface_ack_configure(xdgSurface, client.configureSerial);
	wl_surface_attach(surface, buffer, 0, 0);
	wl_surface_damage(surface, 0, 0, kWidth, kHeight);
	wl_surface_commit(surface);
	wl_display_roundtrip(client.display);

	int32 found = 0;
	for (int attempt = 0; attempt < 20; attempt++) {
		dispatch_until(client, 250000, NULL);
		found = count_window_pixels();
		if (found >= kWidth * kHeight * 9 / 10)
			break;
	}
	printf("CE-HOST screenshot wayland_shm_window\n");
	fflush(stdout);
	dispatch_until(client, 1000000, NULL);
	report("shm_window", found >= kWidth * kHeight * 9 / 10,
		"configured (serial %u), %d of %d pixels of the buffer's colour on "
		"the screen", client.configureSerial, (int)found,
		(int)(kWidth * kHeight));
	// The window stays until the client disconnects.
}


// #pragma mark - clipboard


static const char* kToNative = "renga-wayland-to-native";
static const char* kFromNative = "renga-native-to-wayland";


static void
source_target(void*, wl_data_source*, const char*)
{
}


static void
source_send(void*, wl_data_source*, const char* mimeType, int32_t fd)
{
	write(fd, kToNative, strlen(kToNative));
	close(fd);
}


static void
source_cancelled(void*, wl_data_source* source)
{
}


static void
source_dnd_drop_performed(void*, wl_data_source*)
{
}


static void
source_dnd_finished(void*, wl_data_source*)
{
}


static void
source_action(void*, wl_data_source*, uint32_t)
{
}


// libwayland aborts on an event whose listener slot is NULL: all are set.
static const wl_data_source_listener kSourceListener = {
	source_target, source_send, source_cancelled, source_dnd_drop_performed,
	source_dnd_finished, source_action
};


static void
offer_offer(void* data, wl_data_offer* offer, const char* mimeType)
{
	Client* client = (Client*)data;
	if (offer == client->pendingOffer)
		client->pendingTypes.push_back(mimeType);
}


static void
offer_source_actions(void*, wl_data_offer*, uint32_t)
{
}


static void
offer_action(void*, wl_data_offer*, uint32_t)
{
}


static const wl_data_offer_listener kOfferListener = {
	offer_offer, offer_source_actions, offer_action
};


static void
device_data_offer(void* data, wl_data_device*, wl_data_offer* offer)
{
	Client* client = (Client*)data;
	client->pendingOffer = offer;
	client->pendingTypes.clear();
	wl_data_offer_add_listener(offer, &kOfferListener, client);
}


static void
device_enter(void*, wl_data_device*, uint32_t, wl_surface*, wl_fixed_t,
	wl_fixed_t, wl_data_offer*)
{
}


static void
device_leave(void*, wl_data_device*)
{
}


static void
device_motion(void*, wl_data_device*, uint32_t, wl_fixed_t, wl_fixed_t)
{
}


static void
device_drop(void*, wl_data_device*)
{
}


static void
device_selection(void* data, wl_data_device*, wl_data_offer* offer)
{
	Client* client = (Client*)data;
	client->selection = offer;
	client->selectionTypes = offer == client->pendingOffer
		? client->pendingTypes : std::vector<std::string>();
	client->selections++;
}


static const wl_data_device_listener kDeviceListener = {
	device_data_offer, device_enter, device_leave, device_motion, device_drop,
	device_selection
};


static std::string
native_clipboard_text()
{
	std::string text;
	if (!be_clipboard->Lock())
		return text;
	BMessage* data = be_clipboard->Data();
	const void* bytes;
	ssize_t length;
	if (data != NULL && data->FindData("text/plain", B_MIME_TYPE, &bytes,
			&length) == B_OK) {
		text.assign((const char*)bytes, length);
	}
	be_clipboard->Unlock();
	return text;
}


static void
clipboard_test(Client& client)
{
	if (client.dataDeviceManager == NULL || client.seat == NULL) {
		report("clipboard_to_native", false, "no data device manager or seat");
		report("clipboard_from_native", false, "no data device manager or "
			"seat");
		return;
	}
	wl_data_device* device = wl_data_device_manager_get_data_device(
		client.dataDeviceManager, client.seat);
	wl_data_device_add_listener(device, &kDeviceListener, &client);
	wl_display_roundtrip(client.display);

	// Wayland to native.
	wl_data_source* source = wl_data_device_manager_create_data_source(
		client.dataDeviceManager);
	wl_data_source_add_listener(source, &kSourceListener, &client);
	wl_data_source_offer(source, "text/plain;charset=utf-8");
	wl_data_device_set_selection(device, source, 0);
	std::string text;
	for (int attempt = 0; attempt < 40; attempt++) {
		dispatch_until(client, 100000, NULL);
		text = native_clipboard_text();
		if (text == kToNative)
			break;
	}
	report("clipboard_to_native", text == kToNative,
		"system clipboard text/plain: [%s]", text.c_str());

	// Native to Wayland.
	int selectionsBefore = client.selections;
	if (be_clipboard->Lock()) {
		be_clipboard->Clear();
		be_clipboard->Data()->AddData("text/plain", B_MIME_TYPE, kFromNative,
			strlen(kFromNative));
		be_clipboard->Commit();
		be_clipboard->Unlock();
	}
	for (int attempt = 0; attempt < 40
			&& client.selections == selectionsBefore; attempt++) {
		dispatch_until(client, 100000, NULL);
	}
	std::string received;
	std::string types;
	for (const std::string& type : client.selectionTypes)
		types += type + " ";
	if (client.selections != selectionsBefore && client.selection != NULL) {
		const char* type = "text/plain;charset=utf-8";
		for (const std::string& offered : client.selectionTypes) {
			if (offered == "text/plain")
				type = "text/plain";
		}
		int pipeFDs[2];
		if (pipe(pipeFDs) == 0) {
			wl_data_offer_receive(client.selection, type, pipeFDs[1]);
			close(pipeFDs[1]);
			wl_display_flush(client.display);
			char buffer[256];
			bigtime_t end = system_time() + 5000000;
			while (system_time() < end) {
				dispatch_until(client, 20000, NULL);
				struct pollfd pollFD = { pipeFDs[0], POLLIN, 0 };
				if (poll(&pollFD, 1, 20) <= 0)
					continue;
				ssize_t bytes = read(pipeFDs[0], buffer, sizeof(buffer));
				if (bytes <= 0)
					break;
				received.append(buffer, bytes);
			}
			close(pipeFDs[0]);
		}
	}
	report("clipboard_from_native", received == kFromNative,
		"%d new selection(s), types [%s], read [%s]",
		client.selections - selectionsBefore, types.c_str(),
		received.c_str());
}


// #pragma mark -


int
main()
{
	printf("CE-SUITE %s START\n", SUITE);
	fflush(stdout);

	// BScreen and the clipboard need a BApplication.
	BApplication application("application/x-vnd.Renga-WaylandTest");

	if (getenv("XDG_RUNTIME_DIR") == NULL)
		setenv("XDG_RUNTIME_DIR", kRuntimeDir, 1);
	std::string socketPath = std::string(getenv("XDG_RUNTIME_DIR"))
		+ "/wayland-0";

	bool running = be_roster->IsRunning(kCompositorSignature);
	bool started = false;
	if (!running) {
		started = be_roster->Launch(kCompositorSignature) == B_OK;
		entry_ref ref;
		if (!started && get_ref_for_path(
				"/boot/system/servers/wayland_compositor", &ref) == B_OK) {
			started = be_roster->Launch(&ref) == B_OK;
		}
		for (int i = 0; i < 100 && access(socketPath.c_str(), F_OK) != 0; i++)
			snooze(100000);
		running = be_roster->IsRunning(kCompositorSignature);
	}
	report("compositor", running, "%s%s", kCompositorSignature,
		started ? " (started by the test: launch job off or not ready)"
			: " (started by launch_daemon)");

	struct stat st;
	bool isSocket = stat(socketPath.c_str(), &st) == 0 && S_ISSOCK(st.st_mode);
	report("socket", isSocket, "%s %s", socketPath.c_str(),
		isSocket ? "is a socket" : strerror(errno));

	Client client;
	client.display = wl_display_connect(NULL);
	if (client.display == NULL) {
		report("connect", false, "wl_display_connect: %s", strerror(errno));
		printf("CE-SUITE %s DONE failures=%d\n", SUITE, sFailures);
		return 1;
	}
	client.registry = wl_display_get_registry(client.display);
	wl_registry_add_listener(client.registry, &kRegistryListener, &client);
	int roundtrip = wl_display_roundtrip(client.display);
	report("connect", roundtrip >= 0, "roundtrip %d", roundtrip);

	wl_display_roundtrip(client.display);
		// the bound globals' initial events
	std::string list;
	for (const std::string& global : client.globals)
		list += global + " ";
	const char* wanted[] = { "wl_compositor", "wl_subcompositor", "wl_shm",
		"wl_seat", "wl_data_device_manager", "xdg_wm_base", "wp_viewporter",
		"wl_output", NULL };
	std::string missing;
	for (int i = 0; wanted[i] != NULL; i++) {
		if (!has_global(client, wanted[i]))
			missing += std::string(wanted[i]) + " ";
	}
	report("globals", missing.empty(), "offered: %s%s%s", list.c_str(),
		missing.empty() ? "" : "; missing: ", missing.c_str());

	int32 screens = 0;
	BRect frame;
	{
		BScreen screen;
		frame = screen.Frame();
		screens = 1;
		while (screen.SetToNext() == B_OK)
			screens++;
	}
	bool outputsMatch = (int32)client.outputs.size() == screens;
	std::string modes;
	for (OutputInfo* info : client.outputs) {
		char mode[48];
		snprintf(mode, sizeof(mode), "%dx%d%s ", (int)info->width,
			(int)info->height, info->done ? "" : " (no done)");
		modes += mode;
		if (!info->done)
			outputsMatch = false;
	}
	if (!client.outputs.empty()
		&& (client.outputs[0]->width != frame.IntegerWidth() + 1
			|| client.outputs[0]->height != frame.IntegerHeight() + 1)) {
		outputsMatch = false;
	}
	report("outputs", outputsMatch, "%d wl_output for %d screen(s): %s"
		"(first screen %dx%d)", (int)client.outputs.size(), (int)screens,
		modes.c_str(), (int)frame.IntegerWidth() + 1,
		(int)frame.IntegerHeight() + 1);

	report("shm_formats", client.argb && client.xrgb, "ARGB8888 %s, "
		"XRGB8888 %s", client.argb ? "yes" : "no",
		client.xrgb ? "yes" : "no");

	window_test(client);
	clipboard_test(client);

	// The same compositor team keeps running once the client and its
	// window, surfaces and buffers are gone (launch_daemon would restart a
	// crashed one under a new team ID).
	team_id before = be_roster->TeamFor(kCompositorSignature);
	wl_display_disconnect(client.display);
	snooze(1500000);
	team_id after = be_roster->TeamFor(kCompositorSignature);
	report("disconnect", before >= 0 && after == before,
		"compositor team %d before, %d after the client went away",
		(int)before, (int)after);

	printf("CE-SUITE %s DONE failures=%d\n", SUITE, sFailures);
	fflush(stdout);
	return sFailures != 0;
}
