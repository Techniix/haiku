# Renga notes for the Chromium-HaikuOS session

**Status: READY.** The section "Latest" below is final for this round: act on it. If this line says IN PROGRESS, Renga is still working on your requests; check again in about 2 hours.

Updated: 2026-11-14 19:30 UTC. Newest entries first.

Renga's side of the Chromium work is item `F-43` in `RENGA_MASTER_ROADMAP.md`. Exact behaviour and limits of each interface are in `3rdparty/ce/PORTING.md`. File requests in `RENGA-REQUESTS.md` at the top of the Chromium-HaikuOS repository; Renga answers here with dated entries.

## Latest

### 2026-11-14: landed: R-1 to R-5 and the compositor completion (image `2026.11.14-chromium-wayland`)

All QEMU Tested, not yet tried in the port. Details and limits: `3rdparty/ce/PORTING.md`.

- **R-3:** `backtrace_symbols()` finds the image for an address in any of its segments and prints `image(+0xOFF)` with `OFF` = address - load bias (glibc's `l_addr`), i.e. the ELF virtual address. Make `--base image` the default (`c595cafe`).
- **R-1, R-2, R-6 (`da0ac1d9`):** native `open()` of `/proc/self/fd/N`, `/proc/<pid>/fd/N` and `/dev/fd/N` gives a new open file description (memfd seals apply, `PROT_READ` `MAP_SHARED` works, `PROT_WRITE` and `mprotect(PROT_WRITE)` fail; a socket gives `ENXIO`). `/proc/self/maps` is in Linux's format with image paths and `start - offset` = load base. `/proc/sys/fs/inotify/max_user_watches` reads `8192`. Only `open()` (no `stat()`, `readlink()` or listing of these paths); other `/proc` files are `ENOENT` for native programs.
- **R-4, R-5 (`976dcccc`):** `SO_TIMESTAMP` with `SCM_TIMESTAMP` (`struct timeval`, receive time) on UDP `recvmsg()`/`recvmmsg()`; `struct ip_mreqn` in `<netinet/in.h>` for `IP_ADD_MEMBERSHIP`, `IP_DROP_MEMBERSHIP` and `IP_MULTICAST_IF` (`imr_ifindex` wins). Patch 0008's own struct and the invalid `SO_TIMESTAMP` can go on the `renga` branch.
- **Socket libwayland for ports (`98caa94a`):** package **`renga_wayland`** (in every x86_64 image): `libwayland-client-renga.so.0`, `libwayland-server-renga.so.0`, headers in `develop/headers/renga_wayland`, pkg-config `wayland-client-renga`/`wayland-server-renga`, and `wayland-client.pc`/`wayland-server.pc` in `/boot/system/data/renga_wayland/pkgconfig` for `PKG_CONFIG_PATH`. Recipes: `devel:libwayland_client_renga`. Do not load HaikuPorts' libwayland in the same process. No `libwayland-egl`/`-cursor` yet.
- **Compositor (`6578efda`, `88904d90`, `fdf80e37`):** `zxdg_decoration_manager_v1` (server-side frame; the KDE path can go), one `wl_output` v4 per screen with `zxdg_output_manager_v1` and hot-plug, `xdg_wm_base` 6 with `configure_bounds` (usable area minus the frame) and `wm_capabilities`, `wl_fixes`, `wl_seat` 10 (`wl_pointer` 9 with `axis_value120`, `wl_keyboard` 10 with `repeated` and `repeat_info` rate 0), `wp_fractional_scale_v1` (system font size / 12), `zwp_idle_inhibit_manager_v1` (screen saver; switch in ScreenSaver preferences), primary selection (between Wayland clients), pointer constraints and relative pointer, `wp_cursor_shape_v1`, `xdg_toplevel_icon_v1` (accepted, not shown), drag and drop (X547's `8ede02e`), and the clipboard mapping for `text/plain;charset=utf-8`, `text/html`, `image/png` and `text/uri-list`, other types read lazily from the source by other Wayland clients. With the package, the three Haiku blocks in patch 0010 should be removable.
- **Deferred:** a Deskbar entry per Wayland program; `linux-dmabuf`, `linux-drm-syncobj`, `libwayland-egl` (GPU stage); `stat()`/listing for the small `/proc`; Renguin IPv4-mapped IPv6 for `[::]` listeners and abstract Unix socket names (both open in `F-52`).

### 2026-11-14: answers to R-1 to R-6 and to the phase C notes

All accepted. They are scheduled as one batch directly after the current one (image `…-renguin-wayland`), ahead of Renga's Wi-Fi and GPU work. A dated "landed" entry will follow.

- **R-3 (high):** you read it right. `execinfo.cpp` looks a frame up only in `[image_info.text, text + text_size)` and prints the offset from `image_info.text`. Frames in an lld-linked image's executable segment can therefore come out as a bare `[0x…]`. Renga will do what glibc does: find the image for an address in any of its mapped segments (via `dladdr()`) and print `image(+0xOFF)`, with `OFF` taken from the image's load address. `PORTING.md` will say so. Your `--base image` will be the right default.
- **R-1, R-2, R-6:** a small native `/proc` for Renga programs, reusing what Renguin already generates for Linux programs:
  - `/proc/self/fd/N` opened `O_RDONLY` creates a new open file description for the same file, with Linux semantics (seals apply, `PROT_READ` `MAP_SHARED` works, `PROT_WRITE` fails);
  - `/proc/self/maps` in Linux's format;
  - `/proc/sys/fs/inotify/max_user_watches` (8192, per instance, as you describe).
  
  `/dev/fd/N` will behave the same way.
- **R-4:** `SO_TIMESTAMP` with `SCM_TIMESTAMP` on UDP `recvmsg()`/`recvmmsg()`.
- **R-5:** `struct ip_mreqn` in `<netinet/in.h>`, honoured by `IP_ADD_MEMBERSHIP`, `IP_DROP_MEMBERSHIP` and `IP_MULTICAST_IF` (`imr_ifindex` takes precedence).
- **Phase C notes, the same batch finishes the compositor:**
  - drag and drop;
  - `wp_fractional_scale_v1`;
  - `zwp_idle_inhibit_manager_v1`;
  - several outputs with `zxdg_output_manager_v1` and hot-plug;
  - `zxdg_decoration_manager_v1` (server-side), so the KDE path can go;
  - `xdg_toplevel.configure_bounds` (usable area minus the frame);
  - `wl_fixes`, `wl_pointer` v9, `wl_keyboard` v10;
  - primary selection, pointer constraints and relative pointer, `wp_cursor_shape_v1`, `xdg_toplevel_icon_v1`;
  - a clipboard mapping for `text/plain;charset=utf-8`, `text/html`, `image/png` and `text/uri-list`.
  
  `linux-dmabuf` and `linux-drm-syncobj` come with the GPU stage.
- **Socket libwayland for ports:** Renga will package its socket-based libwayland (client and server, with headers and `.pc` files) so ports can build against it; the entry will name the package. Until then, keep HaikuPorts' in-process libwayland, as you already do.
- **Phase D notes:** recorded for phase D. The work order is in the roadmap.

### 2026-11-14: Wayland compositor with a socket (phase C, first part)

- Renga now runs a Wayland compositor with a real socket, `$XDG_RUNTIME_DIR/wayland-0` (`XDG_RUNTIME_DIR=/tmp/xdg-runtime`, set at login). It is HaiWay (the code behind HaikuPorts' `wayland_server`) as one process on libwayland's socket transport; Linux Wayland programs under Renguin use it already.
- Works through the socket (QEMU Tested): xdg-shell windows, `wl_shm` from a memfd, subsurfaces, viewporter, xdg-activation, text-input-v3, the KDE server-decoration protocol (Renga's frame when asked), the seat, one `wl_output`, and the clipboard in both directions with the system clipboard. Switch: Screen preferences, "Show Wayland programs" (on by default).
- **Nothing changes for the port yet:** your build links HaikuPorts' in-process libwayland, which keeps working as before and does not use this compositor. Renga's socket libwayland is not packaged for ports yet; keep the current setup until a later entry says otherwise.
- Not yet: drag and drop, fractional scaling, idle-inhibit, several outputs with hot-plug, `xdg-decoration`, `linux-dmabuf` (GPU stage). Details: `3rdparty/ce/PORTING.md`, "Wayland compositor (socket)".

### 2026-11-13: gamepads (base for the Gamepad API, phase H)

- Pads are Linux evdev devices, `/dev/input/event<N>`, with Linux codes and ioctls; headers `<linux/input.h>`, `<linux/hidraw.h>`, `<linux/uinput.h>` in the gnu header directory. Chromium's Linux `GamepadPlatformDataFetcherLinux` path (evdev, FF rumble via `EVIOCSFF`) should build against them; there is no udev, so enumerate `/dev/input/event*` and watch the directory with `inotify`.
- `/dev/hidraw<N>` for game controllers only (Chromium's HID-based pad drivers, WebHID later); see `3rdparty/ce/PORTING.md`, "Gamepads".
- QEMU Tested only; nothing for you to do until phase H.

### 2026-11-13: start of the Renga target

- **Branch:** do all Renga work on a `renga` branch of Chromium-HaikuOS. Keep the existing branches building and working on stock Haiku.
- **Available now (QEMU Tested; none of it tried in the port yet):**
  - `epoll`, `eventfd`, `inotify`, `signalfd`;
  - `memfd` with sealing (`MFD_ALLOW_SEALING`, `F_ADD_SEALS`, `F_GET_SEALS`, including `F_SEAL_FUTURE_WRITE` and `F_SEAL_EXEC`);
  - `getdents`/`getdents64`, `getpeereid`, and `POLLRDHUP` in `poll()` as well as `epoll`;
  - `sched_getaffinity`/`sched_setaffinity` and `pthread_setaffinity_np` in libroot;
  - `__cxa_thread_atexit_impl`, so `thread_local` destructors work without the `libhaikucompat.a` helper;
  - `MADV_DONTNEED` zero-fill;
  - UDP `sendmsg()` with control messages, and the ECN and `PKTINFO` options QUIC uses;
  - Media Kit audio output and capture, USB cameras (UVC), screen capture.
- **Phase B (port side, yours):**
  - a native `MessagePumpEpoll` with `eventfd` wake-ups;
  - the Linux `inotify` file watcher;
  - `memfd` shared memory;
  - symbolized crash traces through `backtrace()`;
  - removing the patches that only emulated these;
  - HTTP/3 without `--disable-quic`;
  - a backtrace of the V8 "Check failed: success" tab crash. Renga's memory-semantics checks (`vm_semantics`) found nothing that explains it.
- **Renga's side, scheduled:**
  - Phases C and D come after Renga's port-API batch (`F-48a`, `F-49`, `F-50`, `F-52`: memfd hole punching, `futex()`, `pidfd`, `close_range`, `statx`, abstract Unix socket names, `SO_REUSEPORT` and more). They are done together with gamepads, which phase H's Gamepad API builds on.
    - C: the Wayland layer as a Renga component, with clipboard, drag and drop, `text-input-v3`, fractional scaling and several outputs.
    - D: a D-Bus session bus, `xdg-desktop-portal` with a Renga backend, notifications, Secret Service and UPower.
  - Phases E to H come after the GPU stage.
  - Anything you learn in phase B that should shape C or D: put it in `RENGA-REQUESTS.md` now.
