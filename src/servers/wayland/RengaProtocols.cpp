/*
 * Renga: wl_fixes, fractional scale, idle inhibit, cursor shape and
 * toplevel icons for the Wayland compositor.
 * Copyright 2026, Renga contributors. Distributed under the terms of the
 * MIT License.
 */
#include <Path.h>
#include <FindDirectory.h>
#include "RengaProtocols.h"
#include "HaikuCompositor.h"
#include "HaikuSeat.h"
#include "HaikuOutput.h"
#include "AppKitPtrs.h"

#include "Wayland.h"
#include "FractionalScaleV1.h"
#include <string>
#include <map>
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

#include <algorithm>
#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <vector>
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

/*	Removal of globals (wl_fixes version 2). A removed global must stay
	bindable until no client can still send a bind for it: a client that
	saw wl_registry.global_remove but had already queued a bind would
	otherwise be disconnected for binding an unknown name. At removal,
	every client with a wl_registry is waited for. A client with wl_fixes
	version 2 tells when it is done (ack_global_remove, required by the
	protocol even if it never bound the global); a client without it
	cannot, so it is waited for only a grace period, the timed approach
	libwayland documents for wl_global_remove(). Clients that go away are
	done. When none is left, the global is destroyed and its object
	deleted.
*/

static const int32_t kRetireGraceMs = 2000;

struct RetiredGlobal;

struct RetireWaiter {
	struct wl_listener clientDestroyed;
	RetiredGlobal *retired;
	struct wl_client *client;
};

struct RetiredGlobal {
	WlGlocal *object;
	struct wl_global *global;
	uint32_t name;
	std::vector<RetireWaiter*> waiters;
	struct wl_event_source *graceTimer;
	bool graceOver;
};

static std::vector<RetiredGlobal*> sRetiredGlobals;


static bool
client_has_resource(struct wl_client *client, const char *className,
	uint32_t minVersion)
{
	struct Search {
		const char *className;
		uint32_t minVersion;
		bool found;
	} search = {className, minVersion, false};
	wl_client_for_each_resource(client,
		[](struct wl_resource *resource, void *data) {
			Search *search = (Search*)data;
			if (strcmp(wl_resource_get_class(resource), search->className) == 0
				&& (uint32_t)wl_resource_get_version(resource)
					>= search->minVersion) {
				search->found = true;
				return WL_ITERATOR_STOP;
			}
			return WL_ITERATOR_CONTINUE;
		}, &search);
	return search.found;
}


static void
retired_remove_waiter(RetiredGlobal *retired, RetireWaiter *waiter)
{
	wl_list_remove(&waiter->clientDestroyed.link);
	retired->waiters.erase(std::remove(retired->waiters.begin(),
		retired->waiters.end(), waiter), retired->waiters.end());
	delete waiter;
}


static void
retired_check(RetiredGlobal *retired)
{
	if (retired->graceOver) {
		// clients that cannot acknowledge have had their time
		for (size_t i = retired->waiters.size(); i-- > 0;) {
			RetireWaiter *waiter = retired->waiters[i];
			if (!client_has_resource(waiter->client, "wl_fixes", 2))
				retired_remove_waiter(retired, waiter);
		}
	}
	if (!retired->waiters.empty() || !retired->graceOver)
		return;

	sRetiredGlobals.erase(std::remove(sRetiredGlobals.begin(),
		sRetiredGlobals.end(), retired), sRetiredGlobals.end());
	if (retired->graceTimer != NULL)
		wl_event_source_remove(retired->graceTimer);
	wl_global_destroy(retired->global);
	delete retired->object;
	delete retired;
}


static void
retired_client_destroyed(struct wl_listener *listener, void *)
{
	RetireWaiter *waiter = wl_container_of(listener, waiter, clientDestroyed);
	RetiredGlobal *retired = waiter->retired;
	retired_remove_waiter(retired, waiter);
	retired_check(retired);
}


static int
retired_grace_over(void *data)
{
	RetiredGlobal *retired = (RetiredGlobal*)data;
	retired->graceOver = true;
	retired_check(retired);
	return 0;
}


void
RengaRetireGlobal(WlGlocal *object)
{
	struct wl_global *global = object->ReleaseGlobal();
	if (global == NULL) {
		delete object;
		return;
	}
	struct wl_display *display = wl_global_get_display(global);
	RetiredGlobal *retired = new(std::nothrow) RetiredGlobal;
	if (retired == NULL) {
		// no memory: the old way, at once
		wl_global_destroy(global);
		delete object;
		return;
	}
	retired->object = object;
	retired->global = global;
	retired->name = wl_global_get_name(global, NULL);
	retired->graceTimer = NULL;
	retired->graceOver = false;
	wl_global_remove(global);

	bool legacyClients = false;
	struct wl_client *client;
	wl_client_for_each(client, wl_display_get_client_list(display)) {
		if (!client_has_resource(client, "wl_registry", 1))
			continue;
		RetireWaiter *waiter = new(std::nothrow) RetireWaiter;
		if (waiter == NULL)
			continue;
		waiter->retired = retired;
		waiter->client = client;
		waiter->clientDestroyed.notify = retired_client_destroyed;
		wl_client_add_destroy_listener(client, &waiter->clientDestroyed);
		retired->waiters.push_back(waiter);
		if (!client_has_resource(client, "wl_fixes", 2))
			legacyClients = true;
	}
	sRetiredGlobals.push_back(retired);

	if (legacyClients) {
		retired->graceTimer = wl_event_loop_add_timer(
			wl_display_get_event_loop(display), retired_grace_over, retired);
		if (retired->graceTimer != NULL
			&& wl_event_source_timer_update(retired->graceTimer,
				kRetireGraceMs) == 0) {
			return;
		}
	}
	retired->graceOver = true;
	retired_check(retired);
}


bool
RengaRetiredGlobalPending(uint32_t name)
{
	for (RetiredGlobal *retired : sRetiredGlobals) {
		if (retired->name == name)
			return true;
	}
	return false;
}


class HaikuFixes: public WlFixes {
public:
	void HandleDestroyRegistry(struct wl_resource *registry) final
	{
		// The client no longer uses the registry: libwayland can free it.
		wl_resource_destroy(registry);
	}

	void HandleAckGlobalRemove(struct wl_resource *registry, uint32_t name)
		final
	{
		for (RetiredGlobal *retired : sRetiredGlobals) {
			if (retired->name != name)
				continue;
			for (RetireWaiter *waiter : retired->waiters) {
				if (waiter->client == Client()) {
					retired_remove_waiter(retired, waiter);
					retired_check(retired);
					break;
				}
			}
			// a client that was not waited for (its registry came later)
			// may still acknowledge: nothing to do
			return;
		}
		wl_resource_post_error(ToResource(), WL_FIXES_ERROR_INVALID_ACK_REMOVE,
			"global %" PRIu32 " is unknown or not removed", name);
	}
};

DEFINE_CREATE(HaikuFixesGlobal, wl_fixes_interface, 2)

void HaikuFixesGlobal::Bind(struct wl_client *client, uint32_t version, uint32_t id)
{
	bind_simple<HaikuFixes>(client, version, id);
}


//#pragma mark - fractional scale

/*!	Renga (batch 11): 1.0 unless fractional scaling is switched on
	(~/config/settings/Renga/wayland_fractional_scale containing "on").
	It used to follow the system font size; with a large font Linux
	Chromium drew at 1.5x while its window was sized at 1x on the
	maintainer's machine (content in the top left, the window larger).

	Renga (batch 12): the Renguin display scale. Renga's windows are in
	screen pixels and Linux programs draw in logical pixels; with a scale
	S the compositor treats S screen pixels as one logical pixel, end to
	end: a surface's window is its logical size (viewport destination, or
	buffer size over its buffer scale) times S, its buffer is drawn into
	that (one to one for a client that drew at S), pointer positions are
	divided by S, configure sizes and bounds are window sizes over S, and
	window geometry, subsurface and popup positions are multiplied by S.
	wl_output.scale is S rounded up (an integer), wp_fractional_scale_v1's
	preferred scale is S itself. The setting is
	~/config/settings/Renga/renguin_display_scale, written by Renguin
	Manager:

		scale 1.5
		program chrome 1.25

	("program" lines: per-program overrides, by the name of the client's
	executable). The compositor reads it again when Renguin Manager sends
	it kMsgDisplayScaleChanged; the outputs and fractional scale objects
	of every client then get the new scale (live, as the protocols allow),
	and windows follow the client's next buffer. The old switch still
	works when the file sets no scale: "on" follows the system font size.
*/
static bool
fractional_scale_enabled()
{
	BPath path;
	if (find_directory(B_USER_SETTINGS_DIRECTORY, &path) != B_OK
		|| path.Append("Renga/wayland_fractional_scale") != B_OK)
		return false;
	FILE* file = fopen(path.Path(), "r");
	if (file == NULL)
		return false;
	char text[16] = "";
	fgets(text, sizeof(text), file);
	fclose(file);
	return strncmp(text, "on", 2) == 0;
}


static float
valid_scale(float scale)
{
	if (isnan(scale) || scale < 0.5f)
		return 1.0f;
	if (scale > 4.0f)
		return 4.0f;
	// 1/120 steps, as wp_fractional_scale_v1 has them
	return roundf(scale * 120) / 120;
}


struct DisplayScaleSettings {
	bool							loaded = false;
	float							scale = 1.0f;
	std::map<std::string, float>	programs;
};

static DisplayScaleSettings sScaleSettings;
static std::map<struct wl_client*, float> sClientScales;


static void
load_scale_settings()
{
	sScaleSettings = DisplayScaleSettings();
	sScaleSettings.loaded = true;
	bool haveScale = false;
	BPath path;
	if (find_directory(B_USER_SETTINGS_DIRECTORY, &path) == B_OK
		&& path.Append("Renga/renguin_display_scale") == B_OK) {
		FILE* file = fopen(path.Path(), "r");
		if (file != NULL) {
			char line[256];
			while (fgets(line, sizeof(line), file) != NULL) {
				char name[128];
				float value;
				if (sscanf(line, "scale %f", &value) == 1) {
					sScaleSettings.scale = valid_scale(value);
					haveScale = true;
				} else if (sscanf(line, "program %127s %f", name, &value)
						== 2)
					sScaleSettings.programs[name] = valid_scale(value);
			}
			fclose(file);
		}
	}
	if (!haveScale && fractional_scale_enabled())
		sScaleSettings.scale = valid_scale(be_plain_font->Size() / 12.0f);
}


/*!	The name of the client's program, from its team's arguments: the file
	name of the first word that is not Renguin's launcher ("/bin/linux"),
	"env" or a variable setting (Renguin Manager names programs the same
	way, from their Exec lines).
*/
static std::string
client_program(struct wl_client* client)
{
	pid_t pid = -1;
	wl_client_get_credentials(client, &pid, NULL, NULL);
	team_info info;
	if (pid <= 0 || get_team_info(pid, &info) != B_OK)
		return "";
	std::string args(info.args);
	size_t start = 0;
	while (start < args.size()) {
		while (start < args.size() && args[start] == ' ')
			start++;
		size_t end = args.find(' ', start);
		if (end == std::string::npos)
			end = args.size();
		std::string word = args.substr(start, end - start);
		start = end;
		if (word.empty() || word == "env" || word == "/bin/linux"
			|| word == "linux" || word.find('=') != std::string::npos)
			continue;
		size_t slash = word.rfind('/');
		if (slash != std::string::npos)
			word.erase(0, slash + 1);
		return word;
	}
	return "";
}


struct ClientScaleListener {
	struct wl_listener	listener;
};


static void
client_scale_destroyed(struct wl_listener* listener, void* data)
{
	sClientScales.erase((struct wl_client*)data);
	wl_list_remove(&listener->link);
	delete (ClientScaleListener*)listener;
}


float HaikuClientScale(struct wl_client* client)
{
	if (client == NULL)
		return 1.0f;
	auto found = sClientScales.find(client);
	if (found != sClientScales.end())
		return found->second;
	if (!sScaleSettings.loaded)
		load_scale_settings();
	float scale = sScaleSettings.scale;
	if (!sScaleSettings.programs.empty()) {
		auto program = sScaleSettings.programs.find(client_program(client));
		if (program != sScaleSettings.programs.end())
			scale = program->second;
	}
	ClientScaleListener* listener = new(std::nothrow) ClientScaleListener;
	if (listener != NULL) {
		listener->listener.notify = client_scale_destroyed;
		wl_client_add_destroy_listener(client, &listener->listener);
		sClientScales[client] = scale;
	}
	return scale;
}


int32_t HaikuClientIntegerScale(struct wl_client* client)
{
	return (int32_t)ceilf(HaikuClientScale(client) - 0.001f);
}


uint32_t HaikuPreferredScale120(struct wl_client* client)
{
	return (uint32_t)lroundf(HaikuClientScale(client) * 120);
}


class HaikuFractionalScale: public WpFractionalScaleV1 {
public:
	HaikuFractionalScale();
	virtual ~HaikuFractionalScale();
};

static std::vector<HaikuFractionalScale*> sFractionalScales;

HaikuFractionalScale::HaikuFractionalScale()
{
	sFractionalScales.push_back(this);
}

HaikuFractionalScale::~HaikuFractionalScale()
{
	sFractionalScales.erase(std::remove(sFractionalScales.begin(),
		sFractionalScales.end(), this), sFractionalScales.end());
}

class HaikuFractionalScaleManager: public WpFractionalScaleManagerV1 {
public:
	void HandleGetFractionalScale(uint32_t id, struct wl_resource *surface) final
	{
		HaikuFractionalScale *scale = bind_simple<HaikuFractionalScale>(Client(), Version(), id);
		if (scale != NULL)
			scale->SendPreferredScale(HaikuPreferredScale120(Client()));
	}
};


void HaikuDisplayScaleChanged(struct wl_display* display)
{
	load_scale_settings();
	// every client's scale anew (a client's program can have its own)
	std::vector<struct wl_client*> clients;
	for (auto& entry : sClientScales)
		clients.push_back(entry.first);
	for (struct wl_client* client : clients) {
		float scale = sScaleSettings.scale;
		auto program = sScaleSettings.programs.find(client_program(client));
		if (program != sScaleSettings.programs.end())
			scale = program->second;
		sClientScales[client] = scale;
	}
	for (HaikuFractionalScale* scale : sFractionalScales)
		scale->SendPreferredScale(HaikuPreferredScale120(scale->Client()));
	HaikuOutputResendState(display);
	printf("wayland: display scale %.3f, %zu program overrides, %zu "
		"clients\n", sScaleSettings.scale, sScaleSettings.programs.size(),
		clients.size());
}

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
