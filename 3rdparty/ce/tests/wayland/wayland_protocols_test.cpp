/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 */

/*!	In-guest test: the protocols that complete Renga's Wayland compositor
	(the 2026-11-14 batch for the Chromium port), as a native client sees
	them over the socket, linked against the packaged
	libwayland-client-renga.

	  globals             the new globals are offered: xdg-decoration,
	                      xdg-output, wl_fixes, fractional scale, idle
	                      inhibit, primary selection, pointer constraints,
	                      relative pointer, cursor shape, toplevel icon
	  versions            xdg_wm_base 6, wl_seat 10, wl_output 4
	  xdg_output          one per wl_output: logical position and size are
	                      the screen's, names "RENGA-<n>", wl_output's name
	  wl_fixes            a registry destroyed through wl_fixes; the
	                      connection stays healthy
	  fixes_v2.version    wl_fixes is offered at version 2 (Chromium 156)
	  fixes_v2.kept       a removed global stays (and can be bound) until
	                      the wl_fixes v2 client acknowledges it, past the
	                      grace period of a client without wl_fixes v2
	  fixes_v2.ack        ack_global_remove: the global is destroyed
	  fixes_v2.bad_ack    acknowledging an unknown name or a global that
	                      is not removed is the invalid_ack_remove error
	  bounds              xdg_toplevel.configure_bounds before the first
	                      configure: positive and within the screen
	  capabilities        wm_capabilities: maximize, fullscreen, minimize
	  decoration_server   zxdg_toplevel_decoration_v1 configures
	                      server_side; the window has Renga's frame (a tab)
	  decoration_client   after set_mode(client_side): configure
	                      client_side, the window has no frame
	  fractional_scale    preferred_scale is 120 (1.0) unless fractional
	                      scaling is on (batch 11)
	  resize_*            (batch 11) a toplevel without window geometry
	                      and with Renga's frame, as Linux Chromium makes
	                      it: a resize from Renga's side configures the new
	                      size, the window takes the client's next buffer
	                      size; set_maximized configures the usable area
	                      and the maximized state; the shm pool grows
	                      (ftruncate, wl_shm_pool.resize) while mapped
	  idle_inhibit        an inhibitor makes the screen saver's inhibit port
	                      exist; destroying it removes the port
	  cursor_shape        set_shape on a cursor shape device is accepted
	  keyboard_repeat     wl_keyboard 10: repeat_info rate 0 (Renga sends
	                      repeated keys); wl_keyboard 9: Haiku's rate
	  primary_selection   one client's primary selection reaches another,
	                      which reads it from the source
	  clipboard_lazy      a Wayland selection with a private type: another
	                      Wayland client is offered that type and reads it
	                      from the source; the system clipboard gets only
	                      the mapped text; the source is not cancelled by
	                      its own commit
	  pointer_motion_plain, pointer_motion_constrained (Chromium R-9): the
	                      pointer moved over a window of a client with
	                      pointer constraints, a relative pointer manager
	                      and an unused surface keeps the compositor up,
	                      also with a lock and a confinement
	  constraints         a locked pointer and a relative pointer are created
	                      without error (activation needs real motion)
	  toplevel_icon       icon sizes are announced; an icon is accepted

	Output format: see 3rdparty/ce/README.md.
*/


#include <Application.h>
#include <Clipboard.h>
#include <Font.h>
#include <OS.h>
#include <Messenger.h>
#include <Roster.h>
#include <String.h>
#include <Screen.h>
#include <WindowInfo.h>

#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <poll.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <map>
#include <string>
#include <vector>

#include <wayland-client.h>
#include <wayland-cursor.h>
#include <wayland-egl.h>
#include <wayland-egl-backend.h>
#include "xdg-shell-client-protocol.h"
#include "xdg-decoration-unstable-v1-client-protocol.h"
#include "xdg-output-unstable-v1-client-protocol.h"
#include "fractional-scale-v1-client-protocol.h"
#include "idle-inhibit-unstable-v1-client-protocol.h"
#include "primary-selection-unstable-v1-client-protocol.h"
#include "pointer-constraints-unstable-v1-client-protocol.h"
#include "relative-pointer-unstable-v1-client-protocol.h"
#include "cursor-shape-v1-client-protocol.h"
#include "xdg-toplevel-icon-v1-client-protocol.h"
#include "xdg-activation-v1-client-protocol.h"
#include "viewporter-client-protocol.h"

#include <InputServerTypes.h>
#include <input_globals.h>


#define SUITE "wayland_protocols"

static const char* kCompositorSignature
	= "application/x-vnd.Renga-WaylandCompositor";
static const char* kTitle = "Renga Wayland protocols test";
static const int32 kWidth = 200;
static const int32 kHeight = 120;

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


// #pragma mark - connection


struct Global {
	uint32	name;
	uint32	version;
};

struct Connection {
	wl_display*		display = NULL;
	wl_registry*	registry = NULL;
	std::multimap<std::string, Global> globals;
	std::vector<uint32> removed;

	bool Has(const char* interface) const
	{
		return globals.find(interface) != globals.end();
	}

	uint32 Version(const char* interface) const
	{
		auto found = globals.find(interface);
		return found == globals.end() ? 0 : found->second.version;
	}

	void* Bind(const char* interface, const wl_interface* type,
		uint32 version)
	{
		auto found = globals.find(interface);
		if (found == globals.end())
			return NULL;
		uint32 bindVersion = found->second.version < version
			? found->second.version : version;
		return wl_registry_bind(registry, found->second.name, type,
			bindVersion);
	}
};


static void
registry_global(void* data, wl_registry*, uint32_t name,
	const char* interface, uint32_t version)
{
	((Connection*)data)->globals.insert(
		std::make_pair(std::string(interface), Global { name, version }));
}


static void
registry_global_remove(void* data, wl_registry*, uint32_t name)
{
	((Connection*)data)->removed.push_back(name);
}


static const wl_registry_listener kRegistryListener = {
	registry_global, registry_global_remove
};


static bool
connect(Connection& connection)
{
	connection.display = wl_display_connect(NULL);
	if (connection.display == NULL)
		return false;
	connection.registry = wl_display_get_registry(connection.display);
	wl_registry_add_listener(connection.registry, &kRegistryListener,
		&connection);
	return wl_display_roundtrip(connection.display) >= 0;
}


//! Dispatches events for up to \a timeout microseconds or until \a done.
static void
dispatch_until(wl_display* display, bigtime_t timeout, bool* done)
{
	bigtime_t end = system_time() + timeout;
	while ((done == NULL || !*done) && system_time() < end) {
		wl_display_flush(display);
		if (wl_display_prepare_read(display) != 0) {
			wl_display_dispatch_pending(display);
			continue;
		}
		struct pollfd pollFD = { wl_display_get_fd(display), POLLIN, 0 };
		if (poll(&pollFD, 1, 20) > 0)
			wl_display_read_events(display);
		else
			wl_display_cancel_read(display);
		wl_display_dispatch_pending(display);
	}
}


static bool
connection_ok(Connection& connection)
{
	return wl_display_roundtrip(connection.display) >= 0
		&& wl_display_get_error(connection.display) == 0;
}


// #pragma mark - globals and outputs


static void
test_globals(Connection& connection)
{
	const char* wanted[] = { "zxdg_decoration_manager_v1",
		"zxdg_output_manager_v1", "wl_fixes",
		"wp_fractional_scale_manager_v1", "zwp_idle_inhibit_manager_v1",
		"zwp_primary_selection_device_manager_v1",
		"zwp_pointer_constraints_v1", "zwp_relative_pointer_manager_v1",
		"wp_cursor_shape_manager_v1", "xdg_toplevel_icon_manager_v1",
		"xdg_activation_v1", NULL };
	std::string missing;
	for (int i = 0; wanted[i] != NULL; i++) {
		if (!connection.Has(wanted[i]))
			missing += std::string(wanted[i]) + " ";
	}
	report("globals", missing.empty(), "%zu globals%s%s",
		connection.globals.size(), missing.empty() ? "" : "; missing: ",
		missing.c_str());

	uint32 wmBase = connection.Version("xdg_wm_base");
	uint32 seat = connection.Version("wl_seat");
	uint32 output = connection.Version("wl_output");
	report("versions", wmBase >= 6 && seat >= 10 && output >= 4,
		"xdg_wm_base %u, wl_seat %u, wl_output %u", wmBase, seat, output);
}


struct OutputState {
	wl_output*		output = NULL;
	zxdg_output_v1*	xdgOutput = NULL;
	std::string		name;
	std::string		xdgName;
	int32			x = -1, y = -1, width = 0, height = 0;
};


static void out_geometry(void*, wl_output*, int32_t, int32_t, int32_t,
	int32_t, int32_t, const char*, const char*, int32_t) {}
static void out_mode(void*, wl_output*, uint32_t, int32_t, int32_t,
	int32_t) {}
static void out_done(void*, wl_output*) {}
static void out_scale(void*, wl_output*, int32_t) {}
static void out_name(void* data, wl_output*, const char* name)
{
	((OutputState*)data)->name = name;
}
static void out_description(void*, wl_output*, const char*) {}

static const wl_output_listener kOutputListener = {
	out_geometry, out_mode, out_done, out_scale, out_name, out_description
};

static void xo_position(void* data, zxdg_output_v1*, int32_t x, int32_t y)
{
	((OutputState*)data)->x = x;
	((OutputState*)data)->y = y;
}
static void xo_size(void* data, zxdg_output_v1*, int32_t w, int32_t h)
{
	((OutputState*)data)->width = w;
	((OutputState*)data)->height = h;
}
static void xo_done(void*, zxdg_output_v1*) {}
static void xo_name(void* data, zxdg_output_v1*, const char* name)
{
	((OutputState*)data)->xdgName = name;
}
static void xo_description(void*, zxdg_output_v1*, const char*) {}

static const zxdg_output_v1_listener kXdgOutputListener = {
	xo_position, xo_size, xo_done, xo_name, xo_description
};


static void
test_outputs(Connection& connection)
{
	zxdg_output_manager_v1* manager = (zxdg_output_manager_v1*)
		connection.Bind("zxdg_output_manager_v1",
			&zxdg_output_manager_v1_interface, 3);
	std::vector<OutputState*> outputs;
	auto range = connection.globals.equal_range("wl_output");
	for (auto it = range.first; it != range.second; it++) {
		OutputState* state = new OutputState;
		state->output = (wl_output*)wl_registry_bind(connection.registry,
			it->second.name, &wl_output_interface, 4);
		wl_output_add_listener(state->output, &kOutputListener, state);
		if (manager != NULL) {
			state->xdgOutput = zxdg_output_manager_v1_get_xdg_output(manager,
				state->output);
			zxdg_output_v1_add_listener(state->xdgOutput, &kXdgOutputListener,
				state);
		}
		outputs.push_back(state);
	}
	wl_display_roundtrip(connection.display);
	wl_display_roundtrip(connection.display);

	BRect frame = BScreen().Frame();
	bool ok = manager != NULL && !outputs.empty();
	std::string detail;
	for (OutputState* state : outputs) {
		char line[160];
		snprintf(line, sizeof(line), "%s/%s %d,%d %dx%d; ",
			state->name.c_str(), state->xdgName.c_str(), (int)state->x,
			(int)state->y, (int)state->width, (int)state->height);
		detail += line;
		if (strncmp(state->name.c_str(), "RENGA-", 6) != 0
			|| state->xdgName != state->name)
			ok = false;
	}
	if (!outputs.empty()) {
		OutputState* first = outputs[0];
		if (first->x != (int32)frame.left || first->y != (int32)frame.top
			|| first->width != frame.IntegerWidth() + 1
			|| first->height != frame.IntegerHeight() + 1)
			ok = false;
	}
	report("xdg_output", ok, "%s(screen %dx%d)", detail.c_str(),
		(int)frame.IntegerWidth() + 1, (int)frame.IntegerHeight() + 1);
}


static void
test_fixes(Connection& connection)
{
	wl_fixes* fixes = (wl_fixes*)connection.Bind("wl_fixes",
		&wl_fixes_interface, 1);
	if (fixes == NULL) {
		report("wl_fixes", false, "no wl_fixes");
		return;
	}
	wl_registry* extra = wl_display_get_registry(connection.display);
	wl_display_roundtrip(connection.display);
	wl_fixes_destroy_registry(fixes, extra);
	wl_registry_destroy(extra);
	bool ok = connection_ok(connection);
	wl_fixes_destroy(fixes);
	report("wl_fixes", ok, "registry destroyed, connection %s",
		ok ? "healthy" : "failed");
}


/*!	wl_fixes version 2 (Chromium 156, R-7): a removed global stays until
	every client with wl_fixes v2 acknowledged its removal; a bad
	acknowledgement is the protocol error invalid_ack_remove. The
	compositor adds and retires a spare wl_fixes global on request
	(messages 'RgTa', 'RgTr', 'RgTq', for the harness). Connection b is
	a client without wl_fixes: it is waited for only the 2 s grace.
*/
#ifndef WL_FIXES_ACK_GLOBAL_REMOVE_SINCE_VERSION
#	error "wayland-client-protocol.h lacks wl_fixes version 2 (R-7)"
#endif

static bool
compositor_request(uint32 what, uint32 name, BMessage& reply)
{
	BMessage request(what);
	request.AddUInt32("name", name);
	return BMessenger(kCompositorSignature).SendMessage(&request, &reply,
		5000000, 5000000) == B_OK;
}


static bool
retired_pending(uint32 name)
{
	BMessage reply;
	return compositor_request('RgTq', name, reply)
		&& reply.GetBool("pending", false);
}


static bool
bad_ack_error(uint32 name, uint32* code)
{
	Connection connection;
	if (!connect(connection))
		return false;
	wl_fixes* fixes = (wl_fixes*)connection.Bind("wl_fixes",
		&wl_fixes_interface, 2);
	bool error = false;
	if (fixes != NULL && wl_proxy_get_version((wl_proxy*)fixes) >= 2) {
		wl_fixes_ack_global_remove(fixes, connection.registry, name);
		wl_display_roundtrip(connection.display);
		const wl_interface* interface = NULL;
		uint32 id;
		*code = wl_display_get_protocol_error(connection.display, &interface,
			&id);
		error = interface == &wl_fixes_interface
			&& *code == WL_FIXES_ERROR_INVALID_ACK_REMOVE;
	}
	wl_display_disconnect(connection.display);
	return error;
}


static void
test_fixes_v2(Connection& connection, Connection& legacy)
{
	uint32 version = connection.Version("wl_fixes");
	report("fixes_v2.version", version >= 2, "wl_fixes version %u (want 2)",
		version);
	wl_fixes* fixes = (wl_fixes*)connection.Bind("wl_fixes",
		&wl_fixes_interface, 2);
	BMessage reply;
	uint32 name = 0;
	if (fixes == NULL || version < 2 || !compositor_request('RgTa', 0, reply)
		|| (name = reply.GetUInt32("name", 0)) == 0) {
		report("fixes_v2.kept", false, "no spare global (version %u)",
			version);
		if (fixes != NULL)
			wl_fixes_destroy(fixes);
		return;
	}
	wl_display_roundtrip(connection.display);
	wl_display_roundtrip(legacy.display);
	bool offered = false;
	for (auto& global : connection.globals) {
		if (global.second.name == name)
			offered = true;
	}

	reply.MakeEmpty();
	bool retired = compositor_request('RgTr', name, reply)
		&& reply.GetBool("retired", false);
	wl_display_roundtrip(connection.display);
	wl_display_roundtrip(legacy.display);
	bool removeSeen = std::find(connection.removed.begin(),
		connection.removed.end(), name) != connection.removed.end();
	// past the grace period of the client without wl_fixes v2
	dispatch_until(legacy.display, 2600000, NULL);
	bool kept = retired_pending(name);
	// a bind queued before the client saw the removal still works
	wl_fixes* late = (wl_fixes*)wl_registry_bind(connection.registry, name,
		&wl_fixes_interface, 1);
	bool bindOK = connection_ok(connection);
	if (late != NULL)
		wl_fixes_destroy(late);
	report("fixes_v2.kept", offered && retired && removeSeen && kept
			&& bindOK,
		"offered %d, retired %d, global_remove seen %d, kept after 2.6 s "
		"without ack %d, late bind ok %d", offered, retired, removeSeen, kept,
		bindOK);

	wl_fixes_ack_global_remove(fixes, connection.registry, name);
	bool ackOK = connection_ok(connection);
	bool gone = false;
	for (int i = 0; i < 20 && !gone; i++) {
		gone = !retired_pending(name);
		if (!gone)
			snooze(50000);
	}
	report("fixes_v2.ack", ackOK && gone, "ack accepted %d, global destroyed "
		"after the ack %d", ackOK, gone);
	wl_fixes_destroy(fixes);

	uint32 unknownCode = ~0u;
	uint32 liveCode = ~0u;
	bool unknown = bad_ack_error(0x7ffffff0, &unknownCode);
	auto compositor = connection.globals.find("wl_compositor");
	bool live = compositor != connection.globals.end()
		&& bad_ack_error(compositor->second.name, &liveCode);
	report("fixes_v2.bad_ack", unknown && live, "unknown name: error %d "
		"(code %d), live global: error %d (code %d), want "
		"invalid_ack_remove", unknown, (int)unknownCode, live, (int)liveCode);
}


// #pragma mark - window, decoration, scale, inhibit


struct WindowState {
	bool		configured = false;
	uint32		serial = 0;
	int32		boundsWidth = -1, boundsHeight = -1;
	bool		boundsBeforeConfigure = false;
	bool		toplevelConfigured = false;
	std::vector<uint32> capabilities;
	uint32		decorationMode = 0;
	int			decorationConfigures = 0;
	uint32		preferredScale = 0;
};


static void xs_configure(void* data, xdg_surface*, uint32_t serial)
{
	((WindowState*)data)->configured = true;
	((WindowState*)data)->serial = serial;
}
static const xdg_surface_listener kXdgSurfaceListener = { xs_configure };

static void tl_configure(void* data, xdg_toplevel*, int32_t, int32_t,
	wl_array*)
{
	((WindowState*)data)->toplevelConfigured = true;
}
static void tl_close(void*, xdg_toplevel*) {}
static void tl_bounds(void* data, xdg_toplevel*, int32_t w, int32_t h)
{
	WindowState* state = (WindowState*)data;
	state->boundsWidth = w;
	state->boundsHeight = h;
	state->boundsBeforeConfigure = !state->toplevelConfigured;
}
static void tl_capabilities(void* data, xdg_toplevel*, wl_array* array)
{
	// (wl_array_for_each does not compile as C++)
	const uint32_t* values = (const uint32_t*)array->data;
	for (size_t i = 0; i < array->size / sizeof(uint32_t); i++)
		((WindowState*)data)->capabilities.push_back(values[i]);
}
static const xdg_toplevel_listener kToplevelListener = {
	tl_configure, tl_close, tl_bounds, tl_capabilities
};

static void deco_configure(void* data, zxdg_toplevel_decoration_v1*,
	uint32_t mode)
{
	((WindowState*)data)->decorationMode = mode;
	((WindowState*)data)->decorationConfigures++;
}
static const zxdg_toplevel_decoration_v1_listener kDecorationListener = {
	deco_configure
};

static void scale_preferred(void* data, wp_fractional_scale_v1*,
	uint32_t scale)
{
	((WindowState*)data)->preferredScale = scale;
}
static const wp_fractional_scale_v1_listener kScaleListener = {
	scale_preferred
};

static void wm_ping(void*, xdg_wm_base* base, uint32_t serial)
{
	xdg_wm_base_pong(base, serial);
}
static const xdg_wm_base_listener kWmBaseListener = { wm_ping };


//! The compositor's window with our title: its tab height, or -1.
static float
window_tab_height()
{
	team_id team = be_roster->TeamFor(kCompositorSignature);
	int32 count = 0;
	int32* tokens = get_token_list(team, &count);
	float tab = -1;
	for (int32 i = 0; tokens != NULL && i < count; i++) {
		client_window_info* info = get_window_info(tokens[i]);
		if (info == NULL)
			continue;
		if (strcmp(info->name, kTitle) == 0 && !info->is_mini
			&& info->show_hide_level <= 0)
			tab = info->tab_height;
		free(info);
	}
	free(tokens);
	return tab;
}


static wl_buffer*
create_buffer(wl_shm* shm)
{
	size_t size = kWidth * kHeight * 4;
	int fd = memfd_create("wayland-protocols-test", MFD_CLOEXEC);
	if (fd < 0 || ftruncate(fd, size) != 0)
		return NULL;
	uint32* pixels = (uint32*)mmap(NULL, size, PROT_READ | PROT_WRITE,
		MAP_SHARED, fd, 0);
	for (size_t i = 0; pixels != MAP_FAILED && i < size / 4; i++)
		pixels[i] = 0x00804020;
	wl_shm_pool* pool = wl_shm_create_pool(shm, fd, size);
	wl_buffer* buffer = wl_shm_pool_create_buffer(pool, 0, kWidth, kHeight,
		kWidth * 4, WL_SHM_FORMAT_XRGB8888);
	wl_shm_pool_destroy(pool);
	close(fd);
	return buffer;
}


static void
test_window(Connection& connection)
{
	wl_compositor* compositor = (wl_compositor*)connection.Bind(
		"wl_compositor", &wl_compositor_interface, 4);
	wl_shm* shm = (wl_shm*)connection.Bind("wl_shm", &wl_shm_interface, 1);
	xdg_wm_base* wmBase = (xdg_wm_base*)connection.Bind("xdg_wm_base",
		&xdg_wm_base_interface, 6);
	zxdg_decoration_manager_v1* decorations = (zxdg_decoration_manager_v1*)
		connection.Bind("zxdg_decoration_manager_v1",
			&zxdg_decoration_manager_v1_interface, 1);
	wp_fractional_scale_manager_v1* scales = (wp_fractional_scale_manager_v1*)
		connection.Bind("wp_fractional_scale_manager_v1",
			&wp_fractional_scale_manager_v1_interface, 1);
	zwp_idle_inhibit_manager_v1* inhibits = (zwp_idle_inhibit_manager_v1*)
		connection.Bind("zwp_idle_inhibit_manager_v1",
			&zwp_idle_inhibit_manager_v1_interface, 1);
	xdg_toplevel_icon_manager_v1* icons = (xdg_toplevel_icon_manager_v1*)
		connection.Bind("xdg_toplevel_icon_manager_v1",
			&xdg_toplevel_icon_manager_v1_interface, 1);
	if (compositor == NULL || shm == NULL || wmBase == NULL
		|| decorations == NULL) {
		report("decoration_server", false, "missing globals");
		return;
	}
	xdg_wm_base_add_listener(wmBase, &kWmBaseListener, NULL);

	WindowState state;
	wl_surface* surface = wl_compositor_create_surface(compositor);
	xdg_surface* xdgSurface = xdg_wm_base_get_xdg_surface(wmBase, surface);
	xdg_surface_add_listener(xdgSurface, &kXdgSurfaceListener, &state);
	xdg_toplevel* toplevel = xdg_surface_get_toplevel(xdgSurface);
	xdg_toplevel_add_listener(toplevel, &kToplevelListener, &state);
	xdg_toplevel_set_title(toplevel, kTitle);
	zxdg_toplevel_decoration_v1* decoration
		= zxdg_decoration_manager_v1_get_toplevel_decoration(decorations,
			toplevel);
	zxdg_toplevel_decoration_v1_add_listener(decoration, &kDecorationListener,
		&state);
	wp_fractional_scale_v1* scale = scales != NULL
		? wp_fractional_scale_manager_v1_get_fractional_scale(scales, surface)
		: NULL;
	if (scale != NULL)
		wp_fractional_scale_v1_add_listener(scale, &kScaleListener, &state);
	xdg_surface_set_window_geometry(xdgSurface, 0, 0, kWidth, kHeight);
	wl_surface_commit(surface);
	dispatch_until(connection.display, 10000000, &state.configured);

	BRect screen = BScreen().Frame();
	report("bounds", state.boundsWidth > 0 && state.boundsHeight > 0
			&& state.boundsWidth <= screen.IntegerWidth() + 1
			&& state.boundsHeight <= screen.IntegerHeight() + 1
			&& state.boundsBeforeConfigure,
		"configure_bounds %dx%d (screen %dx%d), before the configure: %s",
		(int)state.boundsWidth, (int)state.boundsHeight,
		(int)screen.IntegerWidth() + 1, (int)screen.IntegerHeight() + 1,
		state.boundsBeforeConfigure ? "yes" : "no");

	bool maximize = false, fullscreen = false, minimize = false;
	for (uint32 capability : state.capabilities) {
		maximize |= capability == XDG_TOPLEVEL_WM_CAPABILITIES_MAXIMIZE;
		fullscreen |= capability == XDG_TOPLEVEL_WM_CAPABILITIES_FULLSCREEN;
		minimize |= capability == XDG_TOPLEVEL_WM_CAPABILITIES_MINIMIZE;
	}
	report("capabilities", maximize && fullscreen && minimize,
		"%zu capabilities: maximize %d, fullscreen %d, minimize %d",
		state.capabilities.size(), maximize, fullscreen, minimize);

	// The first buffer shows the window.
	wl_buffer* buffer = create_buffer(shm);
	xdg_surface_ack_configure(xdgSurface, state.serial);
	wl_surface_attach(surface, buffer, 0, 0);
	wl_surface_damage(surface, 0, 0, kWidth, kHeight);
	wl_surface_commit(surface);
	float tab = -1;
	for (int i = 0; i < 40 && tab <= 0; i++) {
		dispatch_until(connection.display, 100000, NULL);
		tab = window_tab_height();
	}
	report("decoration_server", state.decorationMode
			== ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE && tab > 0,
		"configure mode %u, window tab height %.0f", state.decorationMode,
		tab);

	// Client-side on request: the frame goes.
	state.configured = false;
	zxdg_toplevel_decoration_v1_set_mode(decoration,
		ZXDG_TOPLEVEL_DECORATION_V1_MODE_CLIENT_SIDE);
	dispatch_until(connection.display, 3000000, &state.configured);
	xdg_surface_ack_configure(xdgSurface, state.serial);
	wl_surface_commit(surface);
	float noTab = -1;
	for (int i = 0; i < 20; i++) {
		dispatch_until(connection.display, 100000, NULL);
		noTab = window_tab_height();
		if (noTab == 0)
			break;
	}
	report("decoration_client", state.decorationMode
			== ZXDG_TOPLEVEL_DECORATION_V1_MODE_CLIENT_SIDE && noTab == 0,
		"configure mode %u (%d configures), window tab height %.0f",
		state.decorationMode, state.decorationConfigures, noTab);

	// batch 11: 1.0 unless fractional scaling is switched on (a large
	// system font made Linux Chromium draw at 1.5x into a window sized at
	// 1x on the maintainer's machine)
	uint32 expected = 120;
	report("fractional_scale", scale != NULL
			&& state.preferredScale == expected,
		"preferred_scale %u (want %u, font %.1f pt)", state.preferredScale,
		expected, be_plain_font->Size());

	// Idle inhibit: the screen saver filter looks for this port.
	if (inhibits != NULL) {
		zwp_idle_inhibitor_v1* inhibitor
			= zwp_idle_inhibit_manager_v1_create_inhibitor(inhibits, surface);
		wl_display_roundtrip(connection.display);
		bool during = find_port("renga:screen saver inhibit") >= 0;
		zwp_idle_inhibitor_v1_destroy(inhibitor);
		wl_display_roundtrip(connection.display);
		bool after = find_port("renga:screen saver inhibit") >= 0;
		report("idle_inhibit", during && !after,
			"inhibit port while inhibited: %s, after: %s",
			during ? "yes" : "no", after ? "yes" : "no");
	} else
		report("idle_inhibit", false, "no idle inhibit manager");

	// Toplevel icon: sizes announced at bind, an icon accepted.
	if (icons != NULL) {
		xdg_toplevel_icon_v1* icon
			= xdg_toplevel_icon_manager_v1_create_icon(icons);
		xdg_toplevel_icon_v1_set_name(icon, "renga-test");
		xdg_toplevel_icon_manager_v1_set_icon(icons, toplevel, icon);
		xdg_toplevel_icon_v1_destroy(icon);
	}
	report("toplevel_icon", icons != NULL && connection_ok(connection),
		"icon set, connection %s", wl_display_get_error(connection.display)
			== 0 ? "healthy" : "failed");

	if (scale != NULL)
		wp_fractional_scale_v1_destroy(scale);
	zxdg_toplevel_decoration_v1_destroy(decoration);
	xdg_toplevel_destroy(toplevel);
	xdg_surface_destroy(xdgSurface);
	wl_surface_destroy(surface);
	wl_buffer_destroy(buffer);
	wl_display_roundtrip(connection.display);
}


// #pragma mark - seat: keyboard, cursor shape, constraints


struct KeyboardState {
	int32	rate = -1;
	int32	delay = -1;
};

static void kb_keymap(void*, wl_keyboard*, uint32_t, int32_t fd, uint32_t)
{
	close(fd);
}
static void kb_enter(void*, wl_keyboard*, uint32_t, wl_surface*, wl_array*) {}
static void kb_leave(void*, wl_keyboard*, uint32_t, wl_surface*) {}
static void kb_key(void*, wl_keyboard*, uint32_t, uint32_t, uint32_t,
	uint32_t) {}
static void kb_modifiers(void*, wl_keyboard*, uint32_t, uint32_t, uint32_t,
	uint32_t, uint32_t) {}
static void kb_repeat(void* data, wl_keyboard*, int32_t rate, int32_t delay)
{
	((KeyboardState*)data)->rate = rate;
	((KeyboardState*)data)->delay = delay;
}
static const wl_keyboard_listener kKeyboardListener = {
	kb_keymap, kb_enter, kb_leave, kb_key, kb_modifiers, kb_repeat
};


// #pragma mark - resize (batch 11)


struct ResizeState {
	bool		configured = false;
	uint32		serial = 0;
	int32		width = 0, height = 0;
	bool		maximized = false;
	uint32		decorationMode = 0;
};

static void rs_configure(void* data, xdg_surface*, uint32_t serial)
{
	((ResizeState*)data)->configured = true;
	((ResizeState*)data)->serial = serial;
}
static const xdg_surface_listener kResizeSurfaceListener = { rs_configure };

static void rt_configure(void* data, xdg_toplevel*, int32_t width,
	int32_t height, wl_array* states)
{
	ResizeState* state = (ResizeState*)data;
	state->width = width;
	state->height = height;
	state->maximized = false;
	const uint32_t* items = (const uint32_t*)states->data;
	for (size_t i = 0; i < states->size / sizeof(uint32_t); i++) {
		if (items[i] == XDG_TOPLEVEL_STATE_MAXIMIZED)
			state->maximized = true;
	}
}
static void rt_close(void*, xdg_toplevel*) {}
static void rt_bounds(void*, xdg_toplevel*, int32_t, int32_t) {}
static void rt_capabilities(void*, xdg_toplevel*, wl_array*) {}
static const xdg_toplevel_listener kResizeToplevelListener = {
	rt_configure, rt_close, rt_bounds, rt_capabilities
};
static void rd_configure(void* data, zxdg_toplevel_decoration_v1*,
	uint32_t mode)
{
	((ResizeState*)data)->decorationMode = mode;
}
static const zxdg_toplevel_decoration_v1_listener kResizeDecorationListener
	= { rd_configure };


/*!	One growing shm pool, as clients keep it: ftruncate() and
	wl_shm_pool.resize() while the old mapping stays, then a buffer at the
	new size drawn through a new mapping.
*/
struct GrowingPool {
	int			fd = -1;
	size_t		size = 0;
	wl_shm_pool* pool = NULL;
	bool		ok = true;

	wl_buffer* Buffer(wl_shm* shm, int32 width, int32 height)
	{
		size_t needed = (size_t)width * height * 4;
		if (fd < 0) {
			fd = memfd_create("resize-pool", MFD_CLOEXEC);
			size = needed;
			ok = fd >= 0 && ftruncate(fd, size) == 0;
			pool = wl_shm_create_pool(shm, fd, size);
		} else if (needed > size) {
			// the old mapping stays while the file grows
			void* old = mmap(NULL, size, PROT_READ | PROT_WRITE,
				MAP_SHARED, fd, 0);
			ok = ok && ftruncate(fd, needed) == 0;
			if (old != MAP_FAILED) {
				((volatile uint8*)old)[size - 1] = 1;
				munmap(old, size);
			}
			size = needed;
			wl_shm_pool_resize(pool, size);
		}
		uint32* pixels = (uint32*)mmap(NULL, needed, PROT_READ | PROT_WRITE,
			MAP_SHARED, fd, 0);
		if (pixels == MAP_FAILED) {
			ok = false;
			return NULL;
		}
		for (size_t i = 0; i < needed / 4; i++)
			pixels[i] = 0x00205080;
		munmap(pixels, needed);
		return wl_shm_pool_create_buffer(pool, 0, width, height, width * 4,
			WL_SHM_FORMAT_XRGB8888);
	}
};


static const char* kResizeTitle = "Renga Wayland resize test";
static BRect window_frame(const char* title);


static bool
set_window_frame(const char* title, BRect frame)
{
	BMessenger compositor(kCompositorSignature);
	BMessage request(B_SET_PROPERTY);
	request.AddRect("data", frame);
	request.AddSpecifier("Frame");
	request.AddSpecifier("Window", title);
	BMessage reply;
	return compositor.SendMessage(&request, &reply, 2000000, 2000000) == B_OK
		&& reply.GetInt32("error", B_OK) == B_OK;
}


static void
test_resize(Connection& connection)
{
	wl_compositor* compositor = (wl_compositor*)connection.Bind(
		"wl_compositor", &wl_compositor_interface, 4);
	wl_shm* shm = (wl_shm*)connection.Bind("wl_shm", &wl_shm_interface, 1);
	xdg_wm_base* wmBase = (xdg_wm_base*)connection.Bind("xdg_wm_base",
		&xdg_wm_base_interface, 6);
	zxdg_decoration_manager_v1* decorations = (zxdg_decoration_manager_v1*)
		connection.Bind("zxdg_decoration_manager_v1",
			&zxdg_decoration_manager_v1_interface, 1);
	if (compositor == NULL || shm == NULL || wmBase == NULL
		|| decorations == NULL) {
		report("resize_configure", false, "missing globals");
		return;
	}

	ResizeState state;
	wl_surface* surface = wl_compositor_create_surface(compositor);
	xdg_surface* xdgSurface = xdg_wm_base_get_xdg_surface(wmBase, surface);
	xdg_surface_add_listener(xdgSurface, &kResizeSurfaceListener, &state);
	xdg_toplevel* toplevel = xdg_surface_get_toplevel(xdgSurface);
	xdg_toplevel_add_listener(toplevel, &kResizeToplevelListener, &state);
	xdg_toplevel_set_title(toplevel, kResizeTitle);
	zxdg_toplevel_decoration_v1* decoration
		= zxdg_decoration_manager_v1_get_toplevel_decoration(decorations,
			toplevel);
	zxdg_toplevel_decoration_v1_add_listener(decoration,
		&kResizeDecorationListener, &state);
	// no set_window_geometry, as Linux Chromium
	wl_surface_commit(surface);
	dispatch_until(connection.display, 10000000, &state.configured);

	GrowingPool pool;
	int32 width = 300, height = 200;
	xdg_surface_ack_configure(xdgSurface, state.serial);
	wl_surface_attach(surface, pool.Buffer(shm, width, height), 0, 0);
	wl_surface_damage(surface, 0, 0, width, height);
	wl_surface_commit(surface);
	BRect frame;
	for (int i = 0; i < 40 && !frame.IsValid(); i++) {
		dispatch_until(connection.display, 100000, NULL);
		frame = window_frame(kResizeTitle);
	}

	// Renga's side makes it bigger: the client hears the new size
	bool rounds = true;
	BString sizes;
	for (int round = 0; round < 3; round++) {
		BRect bigger = window_frame(kResizeTitle);
		bigger.right += 120;
		bigger.bottom += 80;
		state.configured = false;
		state.width = 0;
		bool set = set_window_frame(kResizeTitle, bigger);
		dispatch_until(connection.display, 3000000, &state.configured);
		int32 wantWidth = bigger.IntegerWidth() + 1;
		int32 wantHeight = bigger.IntegerHeight() + 1;
		bool told = set && state.width == wantWidth
			&& state.height == wantHeight;
		// the client draws at that size; the window keeps it
		xdg_surface_ack_configure(xdgSurface, state.serial);
		wl_buffer* buffer = pool.Buffer(shm, state.width > 0 ? state.width
			: width, state.height > 0 ? state.height : height);
		wl_surface_attach(surface, buffer, 0, 0);
		wl_surface_damage(surface, 0, 0, INT32_MAX, INT32_MAX);
		wl_surface_commit(surface);
		for (int i = 0; i < 10; i++)
			dispatch_until(connection.display, 100000, NULL);
		BRect after = window_frame(kResizeTitle);
		bool kept = after.IntegerWidth() + 1 == wantWidth
			&& after.IntegerHeight() + 1 == wantHeight;
		sizes << (round > 0 ? "; " : "") << wantWidth << "x" << wantHeight
			<< " told " << state.width << "x" << state.height << ", window "
			<< after.IntegerWidth() + 1 << "x" << after.IntegerHeight() + 1;
		rounds = rounds && told && kept;
	}
	report("resize_configure", state.decorationMode
			== ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE && rounds,
		"server frame %d, no geometry; %s",
		state.decorationMode == ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE,
		sizes.String());
	report("resize_shm_grow", pool.ok && connection_ok(connection),
		"pool grown with ftruncate and wl_shm_pool.resize while mapped "
		"to %zu bytes: %s", pool.size, pool.ok ? "no fault" : "failed");
	printf("CE-HOST screenshot wayland_resized\n");
	fflush(stdout);
	snooze(1500000);

	// set_maximized: the usable area, the maximized state
	state.configured = false;
	xdg_toplevel_set_maximized(toplevel);
	dispatch_until(connection.display, 3000000, &state.configured);
	bool maximized = state.maximized && state.width > 300;
	xdg_surface_ack_configure(xdgSurface, state.serial);
	wl_surface_attach(surface, pool.Buffer(shm, state.width, state.height),
		0, 0);
	wl_surface_damage(surface, 0, 0, INT32_MAX, INT32_MAX);
	wl_surface_commit(surface);
	for (int i = 0; i < 10; i++)
		dispatch_until(connection.display, 100000, NULL);
	BRect big = window_frame(kResizeTitle);
	state.configured = false;
	xdg_toplevel_unset_maximized(toplevel);
	dispatch_until(connection.display, 3000000, &state.configured);
	bool restored = !state.maximized && state.width < big.IntegerWidth();
	report("resize_maximize", maximized && restored,
		"maximized %d (window %dx%d), restored %d to %dx%d", maximized,
		(int)big.IntegerWidth() + 1, (int)big.IntegerHeight() + 1, restored,
		(int)state.width, (int)state.height);

	zxdg_toplevel_decoration_v1_destroy(decoration);
	xdg_toplevel_destroy(toplevel);
	xdg_surface_destroy(xdgSurface);
	wl_surface_destroy(surface);
	wl_display_roundtrip(connection.display);
}


static void
test_seat(Connection& connection)
{
	wl_seat* seat10 = (wl_seat*)connection.Bind("wl_seat", &wl_seat_interface,
		10);
	wl_seat* seat9 = (wl_seat*)connection.Bind("wl_seat", &wl_seat_interface,
		9);
	if (seat10 == NULL || seat9 == NULL) {
		report("keyboard_repeat", false, "no wl_seat");
		return;
	}
	KeyboardState state10, state9;
	wl_keyboard* keyboard10 = wl_seat_get_keyboard(seat10);
	wl_keyboard_add_listener(keyboard10, &kKeyboardListener, &state10);
	wl_keyboard* keyboard9 = wl_seat_get_keyboard(seat9);
	wl_keyboard_add_listener(keyboard9, &kKeyboardListener, &state9);
	wl_display_roundtrip(connection.display);
	wl_display_roundtrip(connection.display);
	report("keyboard_repeat", state10.rate == 0 && state9.rate > 0
			&& state9.delay > 0,
		"version 10: rate %d, delay %d; version 9: rate %d, delay %d",
		(int)state10.rate, (int)state10.delay, (int)state9.rate,
		(int)state9.delay);

	wl_pointer* pointer = wl_seat_get_pointer(seat10);
	wp_cursor_shape_manager_v1* shapes = (wp_cursor_shape_manager_v1*)
		connection.Bind("wp_cursor_shape_manager_v1",
			&wp_cursor_shape_manager_v1_interface, 1);
	if (shapes != NULL) {
		wp_cursor_shape_device_v1* device
			= wp_cursor_shape_manager_v1_get_pointer(shapes, pointer);
		wp_cursor_shape_device_v1_set_shape(device, 0,
			WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_TEXT);
		wp_cursor_shape_device_v1_set_shape(device, 0,
			WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_DEFAULT);
		wp_cursor_shape_device_v1_destroy(device);
	}
	report("cursor_shape", shapes != NULL && connection_ok(connection),
		"text, then default: connection %s",
		wl_display_get_error(connection.display) == 0 ? "healthy" : "failed");

	wl_compositor* compositor = (wl_compositor*)connection.Bind(
		"wl_compositor", &wl_compositor_interface, 4);
	zwp_pointer_constraints_v1* constraints = (zwp_pointer_constraints_v1*)
		connection.Bind("zwp_pointer_constraints_v1",
			&zwp_pointer_constraints_v1_interface, 1);
	zwp_relative_pointer_manager_v1* relatives
		= (zwp_relative_pointer_manager_v1*)connection.Bind(
			"zwp_relative_pointer_manager_v1",
			&zwp_relative_pointer_manager_v1_interface, 1);
	bool created = false;
	if (constraints != NULL && relatives != NULL && compositor != NULL) {
		wl_surface* surface = wl_compositor_create_surface(compositor);
		zwp_locked_pointer_v1* locked = zwp_pointer_constraints_v1_lock_pointer(
			constraints, surface, pointer, NULL,
			ZWP_POINTER_CONSTRAINTS_V1_LIFETIME_PERSISTENT);
		zwp_relative_pointer_v1* relative
			= zwp_relative_pointer_manager_v1_get_relative_pointer(relatives,
				pointer);
		created = connection_ok(connection);
		zwp_relative_pointer_v1_destroy(relative);
		zwp_locked_pointer_v1_destroy(locked);
		wl_surface_destroy(surface);
		created = created && connection_ok(connection);
	}
	report("constraints", created, "locked pointer and relative pointer %s",
		created ? "created and destroyed" : "failed");
}


// #pragma mark - pointer motion over a constrained client (R-9)


static void
move_mouse(float x, float y)
{
	BMessage command(IS_SET_MOUSE_POSITION);
	BMessage reply;
	command.AddPoint("where", BPoint(x, y));
	_control_input_server_(&command, &reply);
}


//!	The frame of our window on screen, or an invalid rect.
static BRect
window_frame(const char* title)
{
	team_id team = be_roster->TeamFor(kCompositorSignature);
	int32 count = 0;
	int32* tokens = get_token_list(team, &count);
	BRect frame;
	for (int32 i = 0; tokens != NULL && i < count; i++) {
		client_window_info* info = get_window_info(tokens[i]);
		if (info == NULL)
			continue;
		if (strcmp(info->name, title) == 0 && !info->is_mini)
			frame.Set(info->window_left, info->window_top,
				info->window_right, info->window_bottom);
		free(info);
	}
	free(tokens);
	return frame;
}


//!	Moves the pointer over \a frame on a grid, with pauses for the events.
static void
sweep_pointer(Connection& connection, BRect frame)
{
	for (int row = 0; row < 5; row++) {
		for (int column = 0; column < 6; column++) {
			move_mouse(frame.left + frame.Width() * (column + 0.5f) / 6,
				frame.top + frame.Height() * (row + 0.5f) / 5);
			snooze(30000);
			dispatch_until(connection.display, 10000, NULL);
		}
	}
}


/*!	Chromium R-9 (2026-11-19): the compositor stopped with "Looper must be
	locked" as soon as the pointer moved over a window of a client that had
	bound zwp_pointer_constraints_v1 and zwp_relative_pointer_manager_v1,
	with an unused role-less wl_surface next to its toplevel (fractional
	scale, a viewport, a larger buffer). The motion path converted to
	screen coordinates with the surface's view while its window was not
	locked. Here: the same set-up, pointer motion over the window, then
	again with a relative pointer, a lock and a confinement; the compositor
	must stay up and the connection healthy each time.
*/
static void
test_pointer_motion()
{
	team_id compositorTeam = be_roster->TeamFor(kCompositorSignature);
	Connection connection;
	if (!connect(connection)) {
		report("pointer_motion_plain", false, "no connection");
		return;
	}
	wl_compositor* compositor = (wl_compositor*)connection.Bind(
		"wl_compositor", &wl_compositor_interface, 4);
	wl_shm* shm = (wl_shm*)connection.Bind("wl_shm", &wl_shm_interface, 1);
	xdg_wm_base* wmBase = (xdg_wm_base*)connection.Bind("xdg_wm_base",
		&xdg_wm_base_interface, 6);
	wl_seat* seat = (wl_seat*)connection.Bind("wl_seat", &wl_seat_interface,
		9);
	zwp_pointer_constraints_v1* constraints = (zwp_pointer_constraints_v1*)
		connection.Bind("zwp_pointer_constraints_v1",
			&zwp_pointer_constraints_v1_interface, 1);
	zwp_relative_pointer_manager_v1* relatives
		= (zwp_relative_pointer_manager_v1*)connection.Bind(
			"zwp_relative_pointer_manager_v1",
			&zwp_relative_pointer_manager_v1_interface, 1);
	wp_fractional_scale_manager_v1* scales = (wp_fractional_scale_manager_v1*)
		connection.Bind("wp_fractional_scale_manager_v1",
			&wp_fractional_scale_manager_v1_interface, 1);
	wp_viewporter* viewporter = (wp_viewporter*)connection.Bind(
		"wp_viewporter", &wp_viewporter_interface, 1);
	if (compositor == NULL || shm == NULL || wmBase == NULL || seat == NULL
		|| constraints == NULL || relatives == NULL) {
		report("pointer_motion_plain", false, "missing globals");
		wl_display_disconnect(connection.display);
		return;
	}
	xdg_wm_base_add_listener(wmBase, &kWmBaseListener, NULL);
	wl_pointer* pointer = wl_seat_get_pointer(seat);

	// Chromium's set-up: an unused surface first, then the toplevel
	wl_surface* unused = wl_compositor_create_surface(compositor);
	static const char* kMotionTitle = "Renga Wayland pointer test";
	WindowState state;
	wl_surface* surface = wl_compositor_create_surface(compositor);
	xdg_surface* xdgSurface = xdg_wm_base_get_xdg_surface(wmBase, surface);
	xdg_surface_add_listener(xdgSurface, &kXdgSurfaceListener, &state);
	xdg_toplevel* toplevel = xdg_surface_get_toplevel(xdgSurface);
	xdg_toplevel_add_listener(toplevel, &kToplevelListener, &state);
	xdg_toplevel_set_title(toplevel, kMotionTitle);
	wp_fractional_scale_v1* scale = scales != NULL
		? wp_fractional_scale_manager_v1_get_fractional_scale(scales, surface)
		: NULL;
	if (scale != NULL)
		wp_fractional_scale_v1_add_listener(scale, &kScaleListener, &state);
	wp_viewport* viewport = viewporter != NULL
		? wp_viewporter_get_viewport(viewporter, surface) : NULL;
	wl_surface_commit(surface);
	dispatch_until(connection.display, 10000000, &state.configured);
	wl_buffer* buffer = create_buffer(shm);
	xdg_surface_ack_configure(xdgSurface, state.serial);
	if (viewport != NULL) {
		// a buffer at 1.5 times the size shown, as at scale 180/120
		wp_viewport_set_destination(viewport, kWidth * 2 / 3,
			kHeight * 2 / 3);
	}
	wl_surface_attach(surface, buffer, 0, 0);
	wl_surface_damage(surface, 0, 0, kWidth, kHeight);
	wl_surface_commit(surface);
	BRect frame;
	for (int i = 0; i < 40 && !frame.IsValid(); i++) {
		dispatch_until(connection.display, 100000, NULL);
		frame = window_frame(kMotionTitle);
	}

	sweep_pointer(connection, frame);
	bool alive = be_roster->TeamFor(kCompositorSignature) == compositorTeam
		&& connection_ok(connection);
	report("pointer_motion_plain", frame.IsValid() && alive,
		"window %s, pointer moved over it 30 times: compositor %s, "
		"connection %s", frame.IsValid() ? "found" : "not found",
		be_roster->TeamFor(kCompositorSignature) == compositorTeam
			? "still running" : "gone",
		wl_display_get_error(connection.display) == 0 ? "healthy" : "failed");

	zwp_relative_pointer_v1* relative
		= zwp_relative_pointer_manager_v1_get_relative_pointer(relatives,
			pointer);
	zwp_locked_pointer_v1* locked = zwp_pointer_constraints_v1_lock_pointer(
		constraints, surface, pointer, NULL,
		ZWP_POINTER_CONSTRAINTS_V1_LIFETIME_ONESHOT);
	wl_display_roundtrip(connection.display);
	sweep_pointer(connection, frame);
	bool lockAlive = be_roster->TeamFor(kCompositorSignature)
			== compositorTeam && connection_ok(connection);
	zwp_locked_pointer_v1_destroy(locked);
	wl_display_roundtrip(connection.display);

	zwp_confined_pointer_v1* confined
		= zwp_pointer_constraints_v1_confine_pointer(constraints, surface,
			pointer, NULL, ZWP_POINTER_CONSTRAINTS_V1_LIFETIME_PERSISTENT);
	wl_display_roundtrip(connection.display);
	sweep_pointer(connection, frame);
	bool confineAlive = be_roster->TeamFor(kCompositorSignature)
			== compositorTeam && connection_ok(connection);
	zwp_confined_pointer_v1_destroy(confined);
	zwp_relative_pointer_v1_destroy(relative);
	wl_display_roundtrip(connection.display);
	report("pointer_motion_constrained", lockAlive && confineAlive,
		"with a relative pointer and a lock: %s; with a confinement: %s",
		lockAlive ? "compositor up" : "compositor gone or error",
		confineAlive ? "compositor up" : "compositor gone or error");

	// leave the pointer outside, where the other tests expect it
	move_mouse(5, 5);
	if (viewport != NULL)
		wp_viewport_destroy(viewport);
	if (scale != NULL)
		wp_fractional_scale_v1_destroy(scale);
	xdg_toplevel_destroy(toplevel);
	xdg_surface_destroy(xdgSurface);
	wl_surface_destroy(surface);
	wl_surface_destroy(unused);
	wl_buffer_destroy(buffer);
	wl_display_roundtrip(connection.display);
	wl_display_disconnect(connection.display);
}


// #pragma mark - selections


struct OfferState {
	void*		offer = NULL;
	std::vector<std::string> types;
	void*		selection = NULL;
	std::vector<std::string> selectionTypes;
};

struct SourceState {
	const char*	data;
	const char*	lazyType;
	const char*	lazyData;
	int			sends = 0;
	bool		cancelled = false;
};


static void
write_and_close(int fd, const char* text)
{
	size_t length = strlen(text);
	while (length > 0) {
		ssize_t written = write(fd, text, length);
		if (written <= 0)
			break;
		text += written;
		length -= written;
	}
	close(fd);
}


static std::string
read_pipe(int fd, wl_display* sourceDisplay)
{
	std::string result;
	fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
	bigtime_t end = system_time() + 5000000;
	while (system_time() < end) {
		dispatch_until(sourceDisplay, 20000, NULL);
		char buffer[256];
		ssize_t bytes = read(fd, buffer, sizeof(buffer));
		if (bytes > 0)
			result.append(buffer, bytes);
		else if (bytes == 0)
			break;
	}
	close(fd);
	return result;
}


// primary selection listeners
static void ps_offer_offer(void* data, zwp_primary_selection_offer_v1*,
	const char* type)
{
	((OfferState*)data)->types.push_back(type);
}
static const zwp_primary_selection_offer_v1_listener kPrimaryOfferListener = {
	ps_offer_offer
};
static void ps_device_data_offer(void* data, zwp_primary_selection_device_v1*,
	zwp_primary_selection_offer_v1* offer)
{
	OfferState* state = (OfferState*)data;
	state->offer = offer;
	state->types.clear();
	zwp_primary_selection_offer_v1_add_listener(offer, &kPrimaryOfferListener,
		state);
}
static void ps_device_selection(void* data, zwp_primary_selection_device_v1*,
	zwp_primary_selection_offer_v1* offer)
{
	OfferState* state = (OfferState*)data;
	state->selection = offer;
	state->selectionTypes = offer != NULL ? state->types
		: std::vector<std::string>();
}
static const zwp_primary_selection_device_v1_listener kPrimaryDeviceListener = {
	ps_device_data_offer, ps_device_selection
};
static void ps_source_send(void* data, zwp_primary_selection_source_v1*,
	const char*, int32_t fd)
{
	SourceState* state = (SourceState*)data;
	state->sends++;
	write_and_close(fd, state->data);
}
static void ps_source_cancelled(void* data, zwp_primary_selection_source_v1*)
{
	((SourceState*)data)->cancelled = true;
}
static const zwp_primary_selection_source_v1_listener kPrimarySourceListener = {
	ps_source_send, ps_source_cancelled
};


static bool
has_type(const std::vector<std::string>& types, const char* type)
{
	for (const std::string& entry : types) {
		if (entry == type)
			return true;
	}
	return false;
}


static void
test_primary(Connection& a, Connection& b)
{
	auto bindManager = [](Connection& c) {
		return (zwp_primary_selection_device_manager_v1*)c.Bind(
			"zwp_primary_selection_device_manager_v1",
			&zwp_primary_selection_device_manager_v1_interface, 1);
	};
	zwp_primary_selection_device_manager_v1* managerA = bindManager(a);
	zwp_primary_selection_device_manager_v1* managerB = bindManager(b);
	wl_seat* seatA = (wl_seat*)a.Bind("wl_seat", &wl_seat_interface, 1);
	wl_seat* seatB = (wl_seat*)b.Bind("wl_seat", &wl_seat_interface, 1);
	if (managerA == NULL || managerB == NULL || seatA == NULL
		|| seatB == NULL) {
		report("primary_selection", false, "missing globals");
		return;
	}
	OfferState offers;
	zwp_primary_selection_device_v1* deviceB
		= zwp_primary_selection_device_manager_v1_get_device(managerB, seatB);
	zwp_primary_selection_device_v1_add_listener(deviceB,
		&kPrimaryDeviceListener, &offers);
	wl_display_roundtrip(b.display);

	SourceState source = { "renga-primary", NULL, NULL };
	zwp_primary_selection_source_v1* sourceA
		= zwp_primary_selection_device_manager_v1_create_source(managerA);
	zwp_primary_selection_source_v1_add_listener(sourceA,
		&kPrimarySourceListener, &source);
	zwp_primary_selection_source_v1_offer(sourceA, "text/plain;charset=utf-8");
	zwp_primary_selection_device_v1* deviceA
		= zwp_primary_selection_device_manager_v1_get_device(managerA, seatA);
	zwp_primary_selection_device_v1_set_selection(deviceA, sourceA, 0);
	wl_display_roundtrip(a.display);

	for (int i = 0; i < 50 && !has_type(offers.selectionTypes,
			"text/plain;charset=utf-8"); i++)
		dispatch_until(b.display, 50000, NULL);

	std::string text;
	if (offers.selection != NULL) {
		int fds[2];
		pipe(fds);
		zwp_primary_selection_offer_v1_receive(
			(zwp_primary_selection_offer_v1*)offers.selection,
			"text/plain;charset=utf-8", fds[1]);
		close(fds[1]);
		wl_display_flush(b.display);
		text = read_pipe(fds[0], a.display);
	}
	report("primary_selection", text == "renga-primary" && source.sends == 1,
		"offer types %zu, read \"%s\", source asked %d time(s)",
		offers.selectionTypes.size(), text.c_str(), source.sends);

	zwp_primary_selection_source_v1_destroy(sourceA);
	wl_display_roundtrip(a.display);
}


// wl_data_device listeners (clipboard)
static void dd_offer_offer(void* data, wl_data_offer*, const char* type)
{
	((OfferState*)data)->types.push_back(type);
}
static void dd_offer_source_actions(void*, wl_data_offer*, uint32_t) {}
static void dd_offer_action(void*, wl_data_offer*, uint32_t) {}
static const wl_data_offer_listener kDataOfferListener = {
	dd_offer_offer, dd_offer_source_actions, dd_offer_action
};
static void dd_data_offer(void* data, wl_data_device*, wl_data_offer* offer)
{
	OfferState* state = (OfferState*)data;
	state->offer = offer;
	state->types.clear();
	wl_data_offer_add_listener(offer, &kDataOfferListener, state);
}
static void dd_enter(void*, wl_data_device*, uint32_t, wl_surface*,
	wl_fixed_t, wl_fixed_t, wl_data_offer*) {}
static void dd_leave(void*, wl_data_device*) {}
static void dd_motion(void*, wl_data_device*, uint32_t, wl_fixed_t,
	wl_fixed_t) {}
static void dd_drop(void*, wl_data_device*) {}
static void dd_selection(void* data, wl_data_device*, wl_data_offer* offer)
{
	OfferState* state = (OfferState*)data;
	state->selection = offer;
	state->selectionTypes = offer != NULL ? state->types
		: std::vector<std::string>();
}
static const wl_data_device_listener kDataDeviceListener = {
	dd_data_offer, dd_enter, dd_leave, dd_motion, dd_drop, dd_selection
};
static void ds_target(void*, wl_data_source*, const char*) {}
static void ds_send(void* data, wl_data_source*, const char* type,
	int32_t fd)
{
	SourceState* state = (SourceState*)data;
	state->sends++;
	write_and_close(fd, strcmp(type, state->lazyType) == 0
		? state->lazyData : state->data);
}
static void ds_cancelled(void* data, wl_data_source*)
{
	((SourceState*)data)->cancelled = true;
}
static void ds_dnd_drop_performed(void*, wl_data_source*) {}
static void ds_dnd_finished(void*, wl_data_source*) {}
static void ds_action(void*, wl_data_source*, uint32_t) {}
static const wl_data_source_listener kDataSourceListener = {
	ds_target, ds_send, ds_cancelled, ds_dnd_drop_performed, ds_dnd_finished,
	ds_action
};


static void
test_clipboard(Connection& a, Connection& b)
{
	auto bindManager = [](Connection& c) {
		return (wl_data_device_manager*)c.Bind("wl_data_device_manager",
			&wl_data_device_manager_interface, 3);
	};
	wl_data_device_manager* managerA = bindManager(a);
	wl_data_device_manager* managerB = bindManager(b);
	wl_seat* seatA = (wl_seat*)a.Bind("wl_seat", &wl_seat_interface, 1);
	wl_seat* seatB = (wl_seat*)b.Bind("wl_seat", &wl_seat_interface, 1);
	if (managerA == NULL || managerB == NULL || seatA == NULL
		|| seatB == NULL) {
		report("clipboard_lazy", false, "missing globals");
		return;
	}
	OfferState offers;
	wl_data_device* deviceB = wl_data_device_manager_get_data_device(managerB,
		seatB);
	wl_data_device_add_listener(deviceB, &kDataDeviceListener, &offers);
	wl_display_roundtrip(b.display);

	static const char* kLazyType = "application/x-renga-lazy-test";
	SourceState source = { "renga-lazy-text", kLazyType, "renga-lazy-private" };
	wl_data_source* sourceA = wl_data_device_manager_create_data_source(
		managerA);
	wl_data_source_add_listener(sourceA, &kDataSourceListener, &source);
	wl_data_source_offer(sourceA, "text/plain;charset=utf-8");
	wl_data_source_offer(sourceA, kLazyType);
	wl_data_device* deviceA = wl_data_device_manager_get_data_device(managerA,
		seatA);
	wl_data_device_add_listener(deviceA, &kDataDeviceListener, &offers);
	wl_data_device_set_selection(deviceA, sourceA, 0);
	wl_display_flush(a.display);
	// The compositor reads the mapped text from the source now (for the
	// system clipboard), while it handles set_selection.
	for (int i = 0; i < 100 && source.sends == 0; i++)
		dispatch_until(a.display, 20000, NULL);
	int sendsForClipboard = source.sends;

	for (int i = 0; i < 100 && !has_type(offers.selectionTypes, kLazyType);
			i++) {
		dispatch_until(b.display, 20000, NULL);
		dispatch_until(a.display, 5000, NULL);
	}
	std::string text;
	if (offers.selection != NULL && has_type(offers.selectionTypes, kLazyType)) {
		int fds[2];
		pipe(fds);
		wl_data_offer_receive((wl_data_offer*)offers.selection, kLazyType,
			fds[1]);
		close(fds[1]);
		wl_display_flush(b.display);
		text = read_pipe(fds[0], a.display);
	}
	dispatch_until(a.display, 500000, NULL);

	// The system clipboard has the text, not the private type.
	bool clipboardText = false, clipboardPrivate = true;
	if (be_clipboard->Lock()) {
		BMessage* data = be_clipboard->Data();
		const void* value;
		ssize_t size;
		clipboardText = data != NULL && data->FindData("text/plain",
			B_MIME_TYPE, &value, &size) == B_OK
			&& std::string((const char*)value, size) == "renga-lazy-text";
		clipboardPrivate = data != NULL && data->HasData(kLazyType,
			B_MIME_TYPE);
		be_clipboard->Unlock();
	}
	report("clipboard_lazy", text == "renga-lazy-private" && clipboardText
			&& !clipboardPrivate && !source.cancelled,
		"other client read \"%s\" (source asked %d time(s), %d for the "
		"system clipboard); system clipboard: text %s, private type %s; "
		"source cancelled: %s", text.c_str(), source.sends,
		sendsForClipboard, clipboardText ? "yes" : "no",
		clipboardPrivate ? "present" : "absent",
		source.cancelled ? "yes" : "no");

	wl_data_source_destroy(sourceA);
	wl_display_roundtrip(a.display);
}


// #pragma mark -


// #pragma mark - activation and the seat's name


static void
activation_done(void* data, xdg_activation_token_v1*, const char* token)
{
	*(std::string*)data = token != NULL ? token : "";
}

static const xdg_activation_token_v1_listener kActivationTokenListener = {
	activation_done
};


static void
seat_capabilities(void*, wl_seat*, uint32_t)
{
}

static void
seat_name(void* data, wl_seat*, const char* name)
{
	*(std::string*)data = name != NULL ? name : "";
}

static const wl_seat_listener kSeatNameListener = {
	seat_capabilities, seat_name
};


/*	2026-11-14 (hardware: foot said the compositor "does not implement XDG
	activation", wayland-info showed the seat's name as "(null)"):
	xdg_activation_v1 gives a token on commit; wl_seat names itself
	"seat0". */
static void
test_activation(Connection& connection)
{
	xdg_activation_v1* activation = (xdg_activation_v1*)connection.Bind(
		"xdg_activation_v1", &xdg_activation_v1_interface, 1);
	std::string token;
	if (activation != NULL) {
		xdg_activation_token_v1* request
			= xdg_activation_v1_get_activation_token(activation);
		xdg_activation_token_v1_add_listener(request,
			&kActivationTokenListener, &token);
		xdg_activation_token_v1_set_app_id(request, "renga.test");
		xdg_activation_token_v1_commit(request);
		bigtime_t end = system_time() + 2000000;
		while (token.empty() && system_time() < end)
			dispatch_until(connection.display, 50000, NULL);
		xdg_activation_token_v1_destroy(request);
	}
	report("activation", activation != NULL && !token.empty()
			&& connection_ok(connection),
		"xdg_activation_v1 %s, token \"%s\"", activation != NULL ? "bound"
			: "missing", token.c_str());
	if (activation != NULL)
		xdg_activation_v1_destroy(activation);

	wl_seat* seat = (wl_seat*)connection.Bind("wl_seat", &wl_seat_interface,
		10);
	std::string name;
	if (seat != NULL) {
		wl_seat_add_listener(seat, &kSeatNameListener, &name);
		wl_display_roundtrip(connection.display);
		wl_display_roundtrip(connection.display);
		wl_seat_release(seat);
	}
	report("seat_name", name == "seat0", "wl_seat name \"%s\" (want "
		"seat0)", name.c_str());
}


/*!	libwayland-egl and libwayland-cursor (package renga_wayland, the GPU
	stage): a wl_egl_window on a surface keeps and reports its size and
	resize (what an EGL driver reads through wayland-egl-backend.h), and a
	cursor theme loads (the built-in cursors when no theme is installed)
	with a "left_ptr" cursor whose image is a wl_shm buffer of its size.
*/
static void
test_egl_cursor(Connection& connection)
{
	wl_compositor* compositor = (wl_compositor*)connection.Bind(
		"wl_compositor", &wl_compositor_interface, 4);
	wl_shm* shm = (wl_shm*)connection.Bind("wl_shm", &wl_shm_interface, 1);
	if (compositor == NULL || shm == NULL) {
		report("egl_window", false, "no wl_compositor or wl_shm");
		return;
	}
	wl_surface* surface = wl_compositor_create_surface(compositor);
	wl_egl_window* window = wl_egl_window_create(surface, 64, 48);
	int width = 0, height = 0;
	if (window != NULL)
		wl_egl_window_get_attached_size(window, &width, &height);
	bool created = window != NULL && window->width == 64
		&& window->height == 48 && window->surface == (void*)surface
		&& window->version >= 3 && width == 0 && height == 0;
	if (window != NULL) {
		// An EGL driver records the size it attached a buffer with.
		window->attached_width = 64;
		window->attached_height = 48;
		wl_egl_window_resize(window, 100, 80, 2, 3);
		wl_egl_window_get_attached_size(window, &width, &height);
	}
	bool resized = window != NULL && window->width == 100
		&& window->height == 80 && window->dx == 2 && window->dy == 3
		&& width == 64 && height == 48;
	report("egl_window", created && resized, "wl_egl_window %s, version %d, "
		"resize %s, attached size %dx%d", window != NULL ? "made" : "failed",
		window != NULL ? window->version : 0, resized ? "kept" : "wrong",
		width, height);
	if (window != NULL)
		wl_egl_window_destroy(window);
	wl_surface_destroy(surface);

	wl_cursor_theme* theme = wl_cursor_theme_load(NULL, 24, shm);
	wl_cursor* cursor = theme != NULL
		? wl_cursor_theme_get_cursor(theme, "left_ptr") : NULL;
	wl_cursor_image* image = cursor != NULL && cursor->image_count > 0
		? cursor->images[0] : NULL;
	wl_buffer* buffer = image != NULL ? wl_cursor_image_get_buffer(image)
		: NULL;
	wl_display_roundtrip(connection.display);
	report("cursor_theme", buffer != NULL && image->width > 0
			&& image->height > 0,
		"theme %s, left_ptr %s, image %ux%u, buffer %s",
		theme != NULL ? "loaded" : "missing",
		cursor != NULL ? "found" : "missing", image != NULL ? image->width : 0,
		image != NULL ? image->height : 0, buffer != NULL ? "made" : "none");
	if (theme != NULL)
		wl_cursor_theme_destroy(theme);
	wl_shm_destroy(shm);
	wl_compositor_destroy(compositor);
}


int
main()
{
	printf("CE-SUITE %s START\n", SUITE);
	fflush(stdout);
	BApplication application("application/x-vnd.Renga-WaylandProtocolsTest");

	if (getenv("XDG_RUNTIME_DIR") == NULL)
		setenv("XDG_RUNTIME_DIR", "/tmp/xdg-runtime", 1);
	std::string socketPath = std::string(getenv("XDG_RUNTIME_DIR"))
		+ "/wayland-0";
	if (!be_roster->IsRunning(kCompositorSignature))
		be_roster->Launch(kCompositorSignature);
	for (int i = 0; i < 100 && access(socketPath.c_str(), F_OK) != 0; i++)
		snooze(100000);

	Connection a, b;
	if (!connect(a) || !connect(b)) {
		report("connect", false, "wl_display_connect: %s", strerror(errno));
		printf("CE-SUITE %s DONE failures=%d\n", SUITE, sFailures);
		return 1;
	}

	test_globals(a);
	test_outputs(a);
	test_fixes(a);
	test_fixes_v2(a, b);
	test_window(a);
	test_resize(a);
	test_seat(a);
	test_pointer_motion();
	test_activation(a);
	test_primary(a, b);
	test_clipboard(a, b);
	test_egl_cursor(a);

	wl_display_disconnect(b.display);
	wl_display_disconnect(a.display);
	printf("CE-SUITE %s DONE failures=%d\n", SUITE, sFailures);
	fflush(stdout);
	return sFailures != 0;
}
