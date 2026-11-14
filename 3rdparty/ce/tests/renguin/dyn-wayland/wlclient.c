/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 *
 * Renguin test (F-27 with F-43 phase C): a Linux Wayland client, linked
 * against musl and Alpine's own libwayland-client, which connects to
 * Renga's compositor through Renguin's AF_UNIX sockets (SCM_RIGHTS for the
 * memfd and the clipboard pipes). The native wayland_test starts it and
 * watches the screen and the system clipboard while it runs:
 *
 *   wlclient <seconds>
 *
 * prints "connect ok", "globals ok", "window ok" (an xdg_toplevel showing
 * a wl_shm buffer of colour 0x9a5e12 from a memfd), "selection set" (a
 * wl_data_source with text/plain;charset=utf-8 "renga-linux-wayland" as
 * the selection), then keeps the window and answers the compositor for
 * <seconds> and prints "ok". A line "FAIL <why>" ends it early.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include <wayland-client.h>
#include "xdg-shell-client-protocol.h"

#define WIDTH 200
#define HEIGHT 120
#define COLOR 0x009a5e12u

static struct wl_compositor* sCompositor;
static struct wl_shm* sShm;
static struct wl_seat* sSeat;
static struct wl_data_device_manager* sDataDeviceManager;
static struct xdg_wm_base* sWmBase;
static int sConfigured;
static uint32_t sConfigureSerial;
static const char* kSelection = "renga-linux-wayland";


static void
fail(const char* why)
{
	printf("FAIL %s (errno %d)\n", why, errno);
	exit(1);
}


static void
ping(void* data, struct xdg_wm_base* base, uint32_t serial)
{
	xdg_wm_base_pong(base, serial);
}

static const struct xdg_wm_base_listener kWmBaseListener = { ping };


static void
global(void* data, struct wl_registry* registry, uint32_t name,
	const char* interface, uint32_t version)
{
	if (strcmp(interface, "wl_compositor") == 0)
		sCompositor = wl_registry_bind(registry, name, &wl_compositor_interface, 1);
	else if (strcmp(interface, "wl_shm") == 0)
		sShm = wl_registry_bind(registry, name, &wl_shm_interface, 1);
	else if (strcmp(interface, "wl_seat") == 0)
		sSeat = wl_registry_bind(registry, name, &wl_seat_interface, 1);
	else if (strcmp(interface, "wl_data_device_manager") == 0)
		sDataDeviceManager = wl_registry_bind(registry, name,
			&wl_data_device_manager_interface, 3);
	else if (strcmp(interface, "xdg_wm_base") == 0) {
		sWmBase = wl_registry_bind(registry, name, &xdg_wm_base_interface, 1);
		xdg_wm_base_add_listener(sWmBase, &kWmBaseListener, NULL);
	}
}

static void
global_remove(void* data, struct wl_registry* registry, uint32_t name)
{
}

static const struct wl_registry_listener kRegistryListener = {
	global, global_remove
};


static void
surface_configure(void* data, struct xdg_surface* surface, uint32_t serial)
{
	sConfigured = 1;
	sConfigureSerial = serial;
}

static const struct xdg_surface_listener kSurfaceListener = {
	surface_configure
};


static void
toplevel_configure(void* data, struct xdg_toplevel* toplevel, int32_t width,
	int32_t height, struct wl_array* states)
{
}

static void
toplevel_close(void* data, struct xdg_toplevel* toplevel)
{
}

static const struct xdg_toplevel_listener kToplevelListener = {
	toplevel_configure, toplevel_close
};


static void
source_target(void* data, struct wl_data_source* source, const char* type)
{
}

static void
source_send(void* data, struct wl_data_source* source, const char* type,
	int32_t fd)
{
	if (write(fd, kSelection, strlen(kSelection)) < 0)
		perror("write");
	close(fd);
}

static void
source_cancelled(void* data, struct wl_data_source* source)
{
}

static void
source_dnd_drop_performed(void* data, struct wl_data_source* source)
{
}

static void
source_dnd_finished(void* data, struct wl_data_source* source)
{
}

static void
source_action(void* data, struct wl_data_source* source, uint32_t action)
{
}

static const struct wl_data_source_listener kSourceListener = {
	source_target, source_send, source_cancelled, source_dnd_drop_performed,
	source_dnd_finished, source_action
};


static double
now(void)
{
	struct timespec time;
	clock_gettime(CLOCK_MONOTONIC, &time);
	return time.tv_sec + time.tv_nsec / 1e9;
}


static void
dispatch_for(struct wl_display* display, double seconds)
{
	double end = now() + seconds;
	while (now() < end) {
		wl_display_flush(display);
		if (wl_display_prepare_read(display) != 0) {
			wl_display_dispatch_pending(display);
			continue;
		}
		struct pollfd pollFD = { wl_display_get_fd(display), POLLIN, 0 };
		if (poll(&pollFD, 1, 50) > 0)
			wl_display_read_events(display);
		else
			wl_display_cancel_read(display);
		if (wl_display_dispatch_pending(display) < 0)
			fail("dispatch");
	}
}


int
main(int argc, char** argv)
{
	setvbuf(stdout, NULL, _IONBF, 0);
	double seconds = argc > 1 ? atof(argv[1]) : 5;

	struct wl_display* display = wl_display_connect(NULL);
	if (display == NULL)
		fail("wl_display_connect");
	struct wl_registry* registry = wl_display_get_registry(display);
	wl_registry_add_listener(registry, &kRegistryListener, NULL);
	if (wl_display_roundtrip(display) < 0)
		fail("roundtrip");
	printf("connect ok\n");
	if (sCompositor == NULL || sShm == NULL || sWmBase == NULL
		|| sSeat == NULL || sDataDeviceManager == NULL) {
		fail("globals missing");
	}
	printf("globals ok\n");

	size_t size = WIDTH * HEIGHT * 4;
	int fd = memfd_create("wlclient", MFD_CLOEXEC | MFD_ALLOW_SEALING);
	if (fd < 0 || ftruncate(fd, size) != 0)
		fail("memfd");
	uint32_t* pixels = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED,
		fd, 0);
	if (pixels == MAP_FAILED)
		fail("mmap");
	for (size_t i = 0; i < size / 4; i++)
		pixels[i] = COLOR;
	fcntl(fd, F_ADD_SEALS, F_SEAL_SHRINK);

	struct wl_shm_pool* pool = wl_shm_create_pool(sShm, fd, size);
	struct wl_buffer* buffer = wl_shm_pool_create_buffer(pool, 0, WIDTH,
		HEIGHT, WIDTH * 4, WL_SHM_FORMAT_XRGB8888);
	struct wl_surface* surface = wl_compositor_create_surface(sCompositor);
	struct xdg_surface* xdgSurface = xdg_wm_base_get_xdg_surface(sWmBase,
		surface);
	xdg_surface_add_listener(xdgSurface, &kSurfaceListener, NULL);
	struct xdg_toplevel* toplevel = xdg_surface_get_toplevel(xdgSurface);
	xdg_toplevel_add_listener(toplevel, &kToplevelListener, NULL);
	xdg_toplevel_set_title(toplevel, "Renga Linux Wayland test");
	wl_surface_commit(surface);
	double end = now() + 10;
	while (!sConfigured && now() < end)
		dispatch_for(display, 0.05);
	if (!sConfigured)
		fail("no configure");
	xdg_surface_ack_configure(xdgSurface, sConfigureSerial);
	wl_surface_attach(surface, buffer, 0, 0);
	wl_surface_damage(surface, 0, 0, WIDTH, HEIGHT);
	wl_surface_commit(surface);
	if (wl_display_roundtrip(display) < 0)
		fail("roundtrip after commit");
	printf("window ok\n");

	struct wl_data_device* device = wl_data_device_manager_get_data_device(
		sDataDeviceManager, sSeat);
	struct wl_data_source* source = wl_data_device_manager_create_data_source(
		sDataDeviceManager);
	wl_data_source_add_listener(source, &kSourceListener, NULL);
	wl_data_source_offer(source, "text/plain;charset=utf-8");
	wl_data_device_set_selection(device, source, 0);
	wl_display_flush(display);
	printf("selection set\n");

	dispatch_for(display, seconds);
	wl_display_disconnect(display);
	printf("ok\n");
	return 0;
}
