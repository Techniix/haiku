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
	  clients_list        (batch 12) the compositor's client list
	                      (kMsgListWaylandClients) has this client: the
	                      app_id it set, its signature, its process and one
	                      client token, the token of the compositor's
	                      window with the test window's title
	  clients_deskbar     the Deskbar has an entry of its own for it whose
	                      window menu lists exactly that window (the
	                      harness hook kTestWaylandClients)
	  clients_switch      the Renguin switch "deskbar_entries" off: the
	                      compositor's list is empty and the entry goes;
	                      on again: it comes back
	  switcher_list       (batch 13) the window switcher (Ctrl+Tab) has a
	                      group of its own for this program, with its icon
	                      and exactly the test window (harness hook
	                      kTestSwitcher)
	  switcher_activate   switching to that group brings that window, not
	                      another of the compositor's, to the front
	  toplevel_gone_resize (batch 14) a resize queued for a window whose
	                      xdg_toplevel the client destroyed does not crash
	                      the compositor
	  switcher_switch     the Deskbar setting "Window switcher lists Linux
	                      programs" off: the group goes; on again: it is back
	  clipboard_to_native a wl_data_source set as the selection lands in the
	                      system clipboard as text/plain
	  clipboard_from_native text put on the system clipboard reaches the
	                      client as a selection offer it can read
	  linux_client        (when the image has Renguin's Linux root) a Linux
	                      Wayland client, musl with Alpine's
	                      libwayland-client, under Renguin: it connects,
	                      sees the globals and shows its wl_shm window,
	                      whose colour appears on the screen (screenshot
	                      screen-wayland_linux_window)
	  linux_clipboard     its selection reaches the system clipboard (the
	                      compositor reads it over a pipe it passes with
	                      SCM_RIGHTS through Renguin)
	  disconnect          the same compositor team keeps running after the
	                      client and its objects go away
	  switch_off          "Screen --wayland off" (the Screen preferences'
	                      "Show Wayland programs" switch) stops the
	                      compositor through the launch daemon, writes
	                      "disabled" to ~/config/settings/Renga/wayland and
	                      the socket goes away
	  switch_on           "Screen --wayland on" starts it again; a client
	                      connects; the harness saves a screenshot of the
	                      Screen preferences with the switch
	                      (screen-wayland_switch)

	Output format: see 3rdparty/ce/README.md.
*/


#include <Application.h>
#include <Bitmap.h>
#include <Clipboard.h>
#include <Control.h>
#include <Entry.h>
#include <FindDirectory.h>
#include <Path.h>
#include <Roster.h>
#include <Screen.h>
#include <image.h>
#include <WindowInfo.h>
#include <wayland_clients.h>

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
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
static const char* kAppId = "renga-wayland-test";

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


//! Counts the screen's pixels of the colour (in B_RGB32 byte order).
static int32
count_pixels(uint8 blue, uint8 green, uint8 red)
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
				if (pixel[0] == blue && pixel[1] == green && pixel[2] == red)
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
	xdg_toplevel_set_app_id(toplevel, kAppId);
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
		found = count_pixels(0x5e, 0x9a, 0x12);
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


// #pragma mark - toplevel gone, resize queued (batch 14)


/*!	wayland.toplevel_gone_resize: a second window whose xdg_toplevel the
	client destroys while a resize is still queued for its BWindow (the
	compositor's harness hook 'RgTz' posts one right then). The compositor
	crashed in WaylandWindow::FrameResized() on the old code (a hardware
	report with Linux Chromium); it must keep running, the same team, and
	still answer.
*/
static bool compositor_client(BMessage& list, BMessage& entry);


static void
toplevel_gone_test(Client& client)
{
	BMessenger compositor(kCompositorSignature);
	team_id before = be_roster->TeamFor(kCompositorSignature);
	BMessage arm('RgTz');
	BMessage armed;
	compositor.SendMessage(&arm, &armed, 5000000, 5000000);

	const int32 kSide = 64;
	size_t size = kSide * kSide * 4;
	int fd = memfd_create("wayland-gone", MFD_CLOEXEC);
	if (fd < 0 || ftruncate(fd, size) != 0) {
		report("toplevel_gone_resize", false, "memfd: %s", strerror(errno));
		return;
	}
	wl_shm_pool* pool = wl_shm_create_pool(client.shm, fd, size);
	wl_buffer* buffer = wl_shm_pool_create_buffer(pool, 0, kSide, kSide,
		kSide * 4, WL_SHM_FORMAT_XRGB8888);
	wl_surface* surface = wl_compositor_create_surface(client.compositor);
	xdg_surface* xdgSurface = xdg_wm_base_get_xdg_surface(client.wmBase,
		surface);
	client.configured = false;
	xdg_surface_add_listener(xdgSurface, &kXdgSurfaceListener, &client);
	xdg_toplevel* toplevel = xdg_surface_get_toplevel(xdgSurface);
	xdg_toplevel_add_listener(toplevel, &kToplevelListener, &client);
	xdg_toplevel_set_title(toplevel, "Renga Wayland gone");
	wl_surface_commit(surface);
	dispatch_until(client, 10000000, &client.configured);
	xdg_surface_ack_configure(xdgSurface, client.configureSerial);
	wl_surface_attach(surface, buffer, 0, 0);
	wl_surface_commit(surface);
	wl_display_roundtrip(client.display);
	snooze(500000);

	// Chromium's order: the toplevel goes first
	xdg_toplevel_destroy(toplevel);
	wl_display_roundtrip(client.display);
	snooze(2000000);
	xdg_surface_destroy(xdgSurface);
	wl_surface_destroy(surface);
	wl_buffer_destroy(buffer);
	wl_shm_pool_destroy(pool);
	close(fd);
	wl_display_roundtrip(client.display);
	snooze(1000000);

	team_id after = be_roster->TeamFor(kCompositorSignature);
	BMessage list;
	BMessage entry;
	bool answers = compositor_client(list, entry);
	report("toplevel_gone_resize", armed.GetBool("armed", false)
			&& before >= 0 && after == before && answers,
		"hook armed %d; compositor team %d before, %d after; still answers "
		"%d", (int)armed.GetBool("armed", false), (int)before, (int)after,
		(int)answers);
}


// #pragma mark - Deskbar entries (batch 12)


static const uint32 kTestWaylandClients = 'Twcl';
	// the Deskbar's harness hook (src/apps/deskbar/BarApp.h)
static const char* kClientSignature
	= RENGA_WAYLAND_CLIENT_SIGNATURE_PREFIX "renga-wayland-test";


//!	The compositor's list; returns the entry of this client (or false).
static bool
compositor_client(BMessage& list, BMessage& entry)
{
	BMessenger compositor(kCompositorSignature);
	BMessage request(kMsgListWaylandClients);
	list.MakeEmpty();
	if (compositor.SendMessage(&request, &list, 5000000, 5000000) != B_OK
		|| list.what != kMsgWaylandClients)
		return false;
	for (int32 i = 0; list.FindMessage("client", i, &entry) == B_OK; i++) {
		if (strcmp(entry.GetString("app_id", ""), kAppId) == 0)
			return true;
	}
	return false;
}


/*!	The Deskbar's entry with kClientSignature: its windows, one string;
	"" when there is none. Waits up to \a seconds for \a present.
*/
static bool
deskbar_entry(bool present, int seconds, BString& windows, int32& count)
{
	BMessenger deskbar("application/x-vnd.Be-TSKB");
	for (int attempt = 0; attempt < seconds * 4; attempt++) {
		BMessage request(kTestWaylandClients);
		BMessage reply;
		windows = "";
		count = 0;
		bool found = false;
		if (deskbar.SendMessage(&request, &reply, 2000000, 2000000) == B_OK
			&& reply.what == B_REPLY) {
			BMessage entry;
			for (int32 i = 0; reply.FindMessage("entry", i, &entry) == B_OK;
					i++) {
				if (strcasecmp(entry.GetString("sig", ""), kClientSignature)
						!= 0)
					continue;
				found = true;
				const char* window;
				for (int32 j = 0; entry.FindString("window", j, &window)
						== B_OK; j++) {
					windows << (j > 0 ? ", " : "") << window;
					count++;
				}
			}
		}
		if (found == present)
			return true;
		snooze(250000);
	}
	return false;
}


static void
push_clients()
{
	BMessenger compositor(kCompositorSignature);
	BMessage push(kMsgListWaylandClients);
	push.AddBool("push", true);
	BMessage reply;
	compositor.SendMessage(&push, &reply, 2000000, 2000000);
}


// #pragma mark - window switcher (batch 13)


static const uint32 kTestSwitcher = 'Twsw';
static const uint32 kToggleSwitcherLinuxPrograms = 'DStW';
	// the Deskbar's harness hook and setting (src/apps/deskbar/BarApp.h)


/*!	The switcher's group with kClientSignature: its windows, one string.
	Waits up to \a seconds for it to be \a present.
*/
static bool
switcher_group(const char* deskbar, bool present, int seconds,
	BString& windows, int32& count, bool& wayland, bool& icon)
{
	BMessenger messenger(deskbar);
	for (int attempt = 0; attempt < seconds * 4; attempt++) {
		BMessage request(kTestSwitcher);
		BMessage reply;
		windows = "";
		count = 0;
		bool found = false;
		if (messenger.SendMessage(&request, &reply, 2000000, 2000000) == B_OK
			&& reply.what == B_REPLY) {
			BMessage group;
			for (int32 i = 0; reply.FindMessage("group", i, &group) == B_OK;
					i++) {
				if (strcasecmp(group.GetString("sig", ""), kClientSignature)
						!= 0)
					continue;
				found = true;
				wayland = group.GetBool("wayland", false);
				icon = group.GetBool("icon", false);
				const char* window;
				for (int32 j = 0; group.FindString("window", j, &window)
						== B_OK; j++) {
					windows << (j > 0 ? ", " : "") << window;
					count++;
				}
			}
		}
		if (found == present)
			return true;
		snooze(250000);
	}
	return false;
}


static void
set_switcher_setting(const char* deskbar, bool on)
{
	BMessage toggle(kToggleSwitcherLinuxPrograms);
	toggle.AddInt32("be:value", on ? B_CONTROL_ON : B_CONTROL_OFF);
	BMessenger(deskbar).SendMessage(&toggle);
}


static void
switcher_test(const char* deskbar, team_id compositor, int32 clientToken)
{
	BString windows;
	int32 count = 0;
	bool wayland = false;
	bool icon = false;
	bool listed = switcher_group(deskbar, true, 10, windows, count, wayland,
		icon);
	report("switcher_list", listed && wayland && icon && count == 1
			&& windows == "Renga Wayland test",
		"switcher group %s (wayland %d, icon %d); its windows (%d): %s",
		listed ? "found" : "not found in 10 s", (int)wayland, (int)icon,
		(int)count, windows.String());

	BMessage request(kTestSwitcher);
	request.AddString("activate", kClientSignature);
	BMessage reply;
	BMessenger(deskbar).SendMessage(&request, &reply, 5000000, 5000000);
	int32 activated = reply.GetInt32("client_token", -1);
	// the frontmost window of the compositor's team afterwards
	snooze(500000);
	int32 front = -1;
	int32 tokenCount = 0;
	int32* tokens = get_token_list(-1, &tokenCount);
	for (int32 i = 0; tokens != NULL && i < tokenCount && front < 0; i++) {
		client_window_info* info = get_window_info(tokens[i]);
		if (info != NULL && info->team == compositor
			&& !info->is_mini && info->show_hide_level <= 0)
			front = info->client_token;
		free(info);
	}
	free(tokens);
	report("switcher_activate", reply.GetBool("activated", false)
			&& activated == clientToken && front == clientToken,
		"switched to the window with client token %d (this program's: %d, "
		"activated %d); the compositor's front window now %d",
		(int)activated, (int)clientToken,
		(int)reply.GetBool("activated", false), (int)front);

	set_switcher_setting(deskbar, false);
	bool gone = switcher_group(deskbar, false, 10, windows, count, wayland,
		icon);
	set_switcher_setting(deskbar, true);
	bool back = switcher_group(deskbar, true, 10, windows, count, wayland,
		icon);
	report("switcher_switch", gone && back && count == 1,
		"setting off: group %s; on again: %s", gone ? "gone" : "still there",
		back ? "back" : "not back in 10 s");
}


static void
clients_test(Client& client)
{
	wl_display_roundtrip(client.display);
	BMessage list;
	BMessage entry;
	bool found = compositor_client(list, entry);
	team_id compositor = list.GetInt32("team", -1);
	int32 token = entry.GetInt32("token", -1);
	int32 tokens = 0;
	type_code type;
	entry.GetInfo("token", &type, &tokens);
	// the token is the client token of the compositor's window with the
	// test window's title
	bool windowFound = false;
	int32 count = 0;
	int32* serverTokens = get_token_list(compositor, &count);
	for (int32 i = 0; i < count; i++) {
		client_window_info* info = get_window_info(serverTokens[i]);
		if (info == NULL)
			continue;
		if (info->client_token == token
			&& strcmp(info->name, "Renga Wayland test") == 0)
			windowFound = true;
		free(info);
	}
	free(serverTokens);
	int32 clientCount = 0;
	list.GetInfo("client", &type, &clientCount);
	report("clients_list", found && windowFound && tokens == 1
			&& entry.GetInt32("pid", -1) == getpid()
			&& strcmp(entry.GetString("signature", ""), kClientSignature)
				== 0
			&& strcmp(entry.GetString("name", ""), kAppId) == 0,
		"list %s (enabled %d, team %d, %d clients); this client: name "
		"\"%s\", signature %s, pid %d (mine %d), %d token(s), window with "
		"that token and the title %s",
		list.what == kMsgWaylandClients ? "answered" : "not answered",
		(int)list.GetBool("enabled", false), (int)compositor,
		(int)clientCount,
		entry.GetString("name", ""), entry.GetString("signature", ""),
		(int)entry.GetInt32("pid", -1), (int)getpid(), (int)tokens,
		windowFound ? "found" : "not found");

	// The renga-* images' first-login phase has no Deskbar (deskbar_test
	// starts it and quits it again): start it here too, and quit it at the
	// end if this test started it. It asks the compositor for the list
	// when it starts.
	static const char* kDeskbarSignature = "application/x-vnd.Be-TSKB";
	bool startedDeskbar = false;
	if (!be_roster->IsRunning(kDeskbarSignature)) {
		startedDeskbar = true;
		be_roster->Launch(kDeskbarSignature);
		for (int32 i = 0; i < 120
				&& !be_roster->IsRunning(kDeskbarSignature); i++) {
			snooze(250000);
		}
		snooze(2000000);
	}

	BString windows;
	int32 windowCount = 0;
	bool listed = deskbar_entry(true, 10, windows, windowCount);
	report("clients_deskbar", listed && windowCount == 1
			&& windows == "Renga Wayland test",
		"Deskbar entry %s; its windows (%d): %s",
		listed ? "found" : "not found in 10 s", (int)windowCount,
		windows.String());

	// the Renguin switch: off, then on again (the previous settings kept)
	// (find_directory(), as the compositor and linux-apps find it: the
	// harness's environment need not have HOME)
	BPath settingsDir;
	find_directory(B_USER_SETTINGS_DIRECTORY, &settingsDir, true);
	settingsDir.Append("Renguin");
	BString dir = settingsDir.Path();
	BString settingsPath = dir;
	settingsPath << "/settings";
	BString previous;
	bool hadSettings = false;
	if (FILE* file = fopen(settingsPath.String(), "r")) {
		char buffer[4096];
		size_t length = fread(buffer, 1, sizeof(buffer) - 1, file);
		buffer[length] = '\0';
		previous = buffer;
		hadSettings = true;
		fclose(file);
	}
	mkdir(dir.String(), 0755);
	if (FILE* file = fopen(settingsPath.String(), "w")) {
		fprintf(file, "%s\n%s false\n", previous.String(),
			RENGA_WAYLAND_CLIENTS_SWITCH);
		fclose(file);
	}
	push_clients();
	bool gone = deskbar_entry(false, 10, windows, windowCount);
	BMessage offList;
	BMessage offEntry;
	bool offFound = compositor_client(offList, offEntry);
	bool offEnabled = offList.GetBool("enabled", true);

	if (hadSettings) {
		if (FILE* file = fopen(settingsPath.String(), "w")) {
			fputs(previous.String(), file);
			fclose(file);
		}
	} else
		unlink(settingsPath.String());
	push_clients();
	bool back = deskbar_entry(true, 10, windows, windowCount);
	report("clients_switch", gone && !offFound && !offEnabled && back,
		"switched off: entry %s, compositor lists it %s (enabled %d); on "
		"again: entry %s", gone ? "gone" : "still there",
		offFound ? "yes" : "no", (int)offEnabled,
		back ? "back" : "not back in 10 s");

	switcher_test(kDeskbarSignature, compositor, token);

	if (startedDeskbar) {
		BMessenger(kDeskbarSignature).SendMessage(B_QUIT_REQUESTED);
		for (int32 i = 0; i < 40 && be_roster->IsRunning(kDeskbarSignature);
				i++) {
			snooze(250000);
		}
	}
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


static const char* kLinuxClient
	= "/boot/system/non-packaged/compat/linux/opt/renguin-tests/wlclient-musl";


static void
linux_client_test()
{
	if (access(kLinuxClient, X_OK) != 0) {
		printf("CE-INFO wayland: no Linux root with the Linux client on "
			"this image; linux_client not checked\n");
		return;
	}
	std::string command = std::string("timeout 60 ") + kLinuxClient
		+ " 8 2>&1";
	FILE* pipe = popen(command.c_str(), "r");
	if (pipe == NULL) {
		report("linux_client", false, "popen: %s", strerror(errno));
		return;
	}

	std::string lines;
	bool window = false, selection = false;
	int32 found = 0;
	std::string clipboard;
	char line[256];
	while (fgets(line, sizeof(line), pipe) != NULL) {
		lines += line;
		if (strncmp(line, "window ok", 9) == 0) {
			window = true;
			for (int attempt = 0; attempt < 20; attempt++) {
				snooze(250000);
				found = count_pixels(0x12, 0x5e, 0x9a);
				if (found >= 200 * 120 * 9 / 10)
					break;
			}
			printf("CE-HOST screenshot wayland_linux_window\n");
			fflush(stdout);
		} else if (strncmp(line, "selection set", 13) == 0) {
			selection = true;
			for (int attempt = 0; attempt < 40; attempt++) {
				snooze(100000);
				clipboard = native_clipboard_text();
				if (clipboard == "renga-linux-wayland")
					break;
			}
		}
	}
	int status = pclose(pipe);
	for (char& c : lines) {
		if (c == '\n')
			c = '|';
	}
	report("linux_client", window && found >= 200 * 120 * 9 / 10
			&& lines.find("ok|", lines.size() >= 3 ? lines.size() - 3 : 0)
				!= std::string::npos,
		"output [%s] exit %d, %d of %d pixels of its colour on the screen",
		lines.c_str(), WEXITSTATUS(status), (int)found, 200 * 120);
	report("linux_clipboard", selection && clipboard == "renga-linux-wayland",
		"system clipboard text/plain: [%s]", clipboard.c_str());
}



static const char* kChromium
	= "/boot/system/non-packaged/compat/linux/usr/lib/chromium/chromium";


//! Whether the compositor has a window whose title contains \a text.
static bool
compositor_window_with(const char* text)
{
	team_id team = be_roster->TeamFor("application/x-vnd.Renga-WaylandCompositor");
	int32 count = 0;
	int32* tokens = get_token_list(team, &count);
	bool found = false;
	for (int32 i = 0; tokens != NULL && i < count && !found; i++) {
		client_window_info* info = get_window_info(tokens[i]);
		if (info == NULL)
			continue;
		found = strstr(info->name, text) != NULL && !info->is_mini;
		free(info);
	}
	free(tokens);
	return found;
}


/*!	Linux Chrome under Renguin on the compositor (images built with
	ce-test.py --linux-chromium): Alpine's chromium with Ozone's Wayland
	platform and software rendering opens the test page in a window, which
	the compositor shows with the page's title.
*/
static void
chrome_window_test()
{
	if (access(kChromium, X_OK) != 0) {
		printf("CE-INFO wayland: no Chromium in the Linux root; "
			"chrome_window not checked\n");
		return;
	}
	// The Linux shell records its PID (= the team, which exec() keeps for
	// Chromium) before it becomes Chromium, so that it can be ended here.
	system("rm -rf /tmp/rg-chrome-wl /tmp/rg-chrome-wl.pid; "
		"(timeout 75 /bin/linux /bin/sh -c 'echo $$ > /tmp/rg-chrome-wl.pid; "
		"exec /usr/lib/chromium/chromium --ozone-platform=wayland "
		"--no-sandbox --no-zygote --single-process --in-process-gpu "
		"--disable-gpu --no-first-run --user-data-dir=/tmp/rg-chrome-wl "
		"file:///opt/renguin-data/chrome/index.html' "
		"> /tmp/rg-chrome-wl.log 2>&1 &)");
	bigtime_t start = system_time();
	bool found = false;
	while (!found && system_time() - start < 70000000) {
		snooze(1000000);
		found = compositor_window_with("Renguin Chromium test");
	}
	if (found) {
		snooze(3000000);
		printf("CE-HOST screenshot wayland_chrome_window\n");
		fflush(stdout);
		snooze(1000000);
	}
	report("chrome_window", found, "window \"Renguin Chromium test\" %s "
		"after %.0f s", found ? "shown" : "not shown",
		(system_time() - start) / 1000000.0);
	if (!found) {
		FILE* log = popen("grep -v -E 'dbus|udev' /tmp/rg-chrome-wl.log "
			"| tail -n 8 | cut -c 1-200", "r");
		char line[256];
		while (log != NULL && fgets(line, sizeof(line), log) != NULL)
			printf("CE-INFO wayland chrome: %s", line);
		if (log != NULL)
			pclose(log);
	}
	// end Chromium (and with it its Wayland connection) before the next
	// checks, which stop and start the compositor
	FILE* pidFile = fopen("/tmp/rg-chrome-wl.pid", "r");
	int pid = -1;
	if (pidFile != NULL) {
		if (fscanf(pidFile, "%d", &pid) != 1)
			pid = -1;
		fclose(pidFile);
	}
	if (pid > 0) {
		kill_team(pid);
		for (int i = 0; i < 50; i++) {
			team_info info;
			if (get_team_info(pid, &info) != B_OK)
				break;
			snooze(100000);
		}
	}
	snooze(1000000);
}


static const char* kScreen = "/boot/system/preferences/Screen";


static bool
wait_for(bool (*condition)(const char*), const char* argument, int seconds)
{
	for (int i = 0; i < seconds * 10; i++) {
		if (condition(argument))
			return true;
		snooze(100000);
	}
	return condition(argument);
}


static bool
compositor_stopped(const char*)
{
	return !be_roster->IsRunning(kCompositorSignature);
}


static bool
compositor_listening(const char* socketPath)
{
	return be_roster->IsRunning(kCompositorSignature)
		&& access(socketPath, F_OK) == 0;
}


static void
switch_test(const std::string& socketPath)
{
	if (access(kScreen, X_OK) != 0) {
		printf("CE-INFO wayland: no Screen preferences on this image; the "
			"switch is not checked\n");
		return;
	}
	std::string settings = std::string(getenv("HOME") != NULL
		? getenv("HOME") : "/boot/home") + "/config/settings/Renga/wayland";

	int status = system((std::string(kScreen) + " --wayland off").c_str());
	bool stopped = wait_for(compositor_stopped, NULL, 10);
	bool socketGone = access(socketPath.c_str(), F_OK) != 0;
	std::string text;
	FILE* file = fopen(settings.c_str(), "r");
	if (file != NULL) {
		char buffer[256];
		size_t bytes = fread(buffer, 1, sizeof(buffer) - 1, file);
		buffer[bytes] = '\0';
		text = buffer;
		fclose(file);
	}
	snooze(2000000);
		// a service the launch daemon still owned would be back by now
	bool stillStopped = !be_roster->IsRunning(kCompositorSignature);
	report("switch_off", status == 0 && stopped && stillStopped && socketGone
			&& text.find("disabled true") != std::string::npos,
		"exit %d, compositor %s%s, socket %s, settings %s", status,
		stopped ? "stopped" : "still running",
		stopped && !stillStopped ? " (but restarted)" : "",
		socketGone ? "gone" : "still there",
		text.find("disabled true") != std::string::npos ? "say disabled"
			: "do not say disabled");

	status = system((std::string(kScreen) + " --wayland on").c_str());
	bool listening = wait_for(compositor_listening, socketPath.c_str(), 15);
	bool connected = false;
	if (listening) {
		wl_display* display = wl_display_connect(NULL);
		if (display != NULL) {
			connected = wl_display_roundtrip(display) >= 0;
			wl_display_disconnect(display);
		}
	}
	report("switch_on", status == 0 && listening && connected,
		"exit %d, compositor %s, connect %s", status,
		listening ? "listening again" : "not back",
		connected ? "works" : "fails");

	// The switch in its window, for the screenshot.
	entry_ref ref;
	if (get_ref_for_path(kScreen, &ref) == B_OK
		&& be_roster->Launch(&ref) == B_OK) {
		snooze(3000000);
		printf("CE-HOST screenshot wayland_switch\n");
		fflush(stdout);
		snooze(1000000);
		BMessenger("application/x-vnd.Haiku-Screen")
			.SendMessage(B_QUIT_REQUESTED);
	}
}


// #pragma mark -


/*!	The package renga_wayland (ports build against it): this test links its
	shared client library, and the development files are installed.
*/
static void
package_test()
{
	bool linked = false;
	char linkedPath[B_PATH_NAME_LENGTH] = "-";
	image_info info;
	int32 cookie = 0;
	while (get_next_image_info(B_CURRENT_TEAM, &cookie, &info) == B_OK) {
		if (strstr(info.name, "libwayland-client-renga.so") != NULL) {
			linked = strncmp(info.name, "/boot/system/lib/", 17) == 0;
			strlcpy(linkedPath, info.name, sizeof(linkedPath));
		}
	}
	report("package_library", linked, "loaded %s", linkedPath);

	const char* files[] = {
		"/boot/system/lib/libwayland-server-renga.so.0",
		"/boot/system/develop/lib/libwayland-client-renga.so",
		"/boot/system/develop/lib/libwayland-server-renga.so",
		"/boot/system/develop/headers/renga_wayland/wayland-client.h",
		"/boot/system/develop/headers/renga_wayland/wayland-client-protocol.h",
		"/boot/system/develop/headers/renga_wayland/wayland-server.h",
		"/boot/system/develop/headers/renga_wayland/wayland-server-protocol.h",
		"/boot/system/develop/headers/renga_wayland/wayland-util.h",
		"/boot/system/develop/headers/renga_wayland/wayland-version.h",
		"/boot/system/develop/lib/pkgconfig/wayland-client-renga.pc",
		"/boot/system/develop/lib/pkgconfig/wayland-server-renga.pc",
		"/boot/system/data/renga_wayland/pkgconfig/wayland-client.pc",
		"/boot/system/data/renga_wayland/pkgconfig/wayland-server.pc",
		"/boot/system/data/renga_wayland/protocols/wayland.xml",
		NULL
	};
	std::string missing;
	int present = 0;
	for (int i = 0; files[i] != NULL; i++) {
		struct stat st;
		if (stat(files[i], &st) == 0 && S_ISREG(st.st_mode))
			present++;
		else
			missing += std::string(files[i]) + " ";
	}
	report("package_files", missing.empty(), "%d present%s%s", present,
		missing.empty() ? "" : "; missing: ", missing.c_str());

	// The .pc file under the usual name points at Renga's library.
	FILE* file = fopen("/boot/system/data/renga_wayland/pkgconfig/"
		"wayland-client.pc", "r");
	bool pcOK = false;
	char line[256];
	while (file != NULL && fgets(line, sizeof(line), file) != NULL) {
		if (strncmp(line, "Libs:", 5) == 0
			&& strstr(line, "-lwayland-client-renga") != NULL)
			pcOK = true;
	}
	if (file != NULL)
		fclose(file);
	report("package_pkgconfig", pcOK, "wayland-client.pc Libs: "
		"-lwayland-client-renga %s", pcOK ? "found" : "missing");
}


int
main()
{
	printf("CE-SUITE %s START\n", SUITE);
	fflush(stdout);

	// BScreen and the clipboard need a BApplication.
	BApplication application("application/x-vnd.Renga-WaylandTest");

	package_test();

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
	clients_test(client);
	toplevel_gone_test(client);
	clipboard_test(client);
	linux_client_test();

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

	switch_test(socketPath);
	// last: Chromium's start-up is long, and the compositor's switch test
	// above must not see its connection
	chrome_window_test();

	printf("CE-SUITE %s DONE failures=%d\n", SUITE, sFailures);
	fflush(stdout);
	return sFailures != 0;
}
