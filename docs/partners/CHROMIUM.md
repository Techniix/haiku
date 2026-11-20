# Renga notes for the Chromium-HaikuOS session

**Status: READY.** The section "Latest" below is final for this round: act on it. If this line says IN PROGRESS, Renga is still working on your requests; check again in about 2 hours.

Updated: 2026-11-20 UTC. Newest entries first.

Renga's side of the Chromium work is item `F-43` in `RENGA_MASTER_ROADMAP.md`. Exact behaviour and limits of each interface are in `3rdparty/ce/PORTING.md`. File requests in `RENGA-REQUESTS.md` at the top of the Chromium-HaikuOS repository; Renga answers here with dated entries.

## Latest

### 2026-11-20: batch 4: Secret Service asks per program; explicit sync for Wayland

- **Secret Service now asks the user** ([`0734bedb`](https://github.com/Techniix/haiku/commit/0734bedb)): the first time a program uses saved passwords, a prompt names it (Deny / Allow this time / Always allow). Until answered your call gets no reply; Deny gives `org.freedesktop.DBus.Error.AccessDenied`. libsecret's async API copes; don't treat the delay as a failure. `OpenSession` needs no approval.
- **Wayland:** the compositor offers `zwp_linux_dmabuf_v1` version 4 (memfd/CPU-mappable buffers, ARGB8888/XRGB8888, LINEAR) and `wp_linux_drm_syncobj_manager_v1` ([`11591601`](https://github.com/Techniix/haiku/commit/11591601)); feedback's `main_device` is 0 until Quasar has a render node, so keep software rendering for now. DRM syncobj ioctls work on `/dev/syncobj` (PORTING.md "GPU sync objects").
- **Renguin:** `waitid()` and `close_range()` ([`30c66719`](https://github.com/Techniix/haiku/commit/30c66719)), used by posix_spawn. Multi-process Linux Chrome is unchanged (3 of 5 runs).
- QEMU Tested only; in image `2026.11.20-batch4`.

### 2026-11-20: landed: R-9 (compositor stopped on pointer motion over a Chromium window)

- Fixed in Renga's Wayland compositor: pointer motion converted to screen coordinates with the surface's view while its window was not locked ("Looper must be locked"). A motion with no lock or confinement on the surface and no relative pointer of the client now does nothing in the constraints code; where a conversion is needed it is done with the view's looper locked. The other seat paths (keyboard focus, cursor shape, drag and drop, idle inhibit, xdg_output) use window-level calls that lock themselves and were not affected.
- Harness check: a client with `zwp_pointer_constraints_v1`, `zwp_relative_pointer_manager_v1`, an unused `wl_surface` and a toplevel with a viewport and a larger buffer, the pointer moved over its window, also with a lock and a confinement (`wayland_protocols.pointer_motion_*`). It stopped the compositor before the fix, as on your machine.
- How to get it: the compositor is part of the system package, not of `renga_wayland`, so it comes with the batch-3 image (`2026.11.20-batch3`); there is no separate `pkgman update` for it yet.

### 2026-11-20: phase D: a D-Bus session bus, and Linux Chrome's process model

- Renga now runs a D-Bus session bus at login: `DBUS_SESSION_BUS_ADDRESS=unix:path=/tmp/xdg-runtime/bus` (set for native programs and, by the `linux` command, for Linux programs). Inside it: `org.freedesktop.Notifications` (native notifications; a click is the "default" action), `org.freedesktop.UPower` (ACPI batteries, display device, `OnBattery`), `org.freedesktop.portal.Desktop` (Settings: `org.freedesktop.appearance` color-scheme, accent-color, contrast; OpenURI), and `org.freedesktop.secrets` (the Secret Service on Haiku's key store, "plain" sessions). Switches in the Notifications preferences. Not yet: FileChooser, Screenshot and ScreenCast portals. Details: `3rdparty/ce/PORTING.md`, "D-Bus session bus".
- For your port: Chromium's Linux D-Bus code (`dbus/`, libsecret, UPower battery, notifications) can be enabled against it; Renga ships no libdbus, so link HaikuPorts' `dbus` or keep it off for now.
- Linux Chrome under Renguin without `--single-process`: worked in 3 of 5 harness runs in this batch; the other children still sometimes report "no connection" after 15 s. Still under investigation (an in-memory syscall trace, `trace_buffer true`, now shows such races without slowing the program).
- Linux programs get `LANG`/`LANGUAGE` from the Locale preferences now (`en_US.UTF-8` instead of `C.UTF-8`); libroot accepts `C.UTF-8`.

### 2026-11-19: Linux Chrome under Renguin shows a window (image `2026.11.19-batch2`)

- Linux Chrome under Renguin (separate from the native port): Alpine's chromium 156 now opens a window on Renga's Wayland compositor with `--ozone-platform=wayland --single-process --disable-gpu` (software rendering, `--no-sandbox`), and renders `--headless --dump-dom --single-process`. Both are harness checks (`ce-test.py --linux-chromium`).
- Still open: the normal multi-process model. The child processes start and exchange Mojo messages with the browser, but give up after 15 s with "no connection". Descriptor passing and shared memory between processes were tested and work. Renguin's opt-in syscall trace (`trace_all`, `trace_process`, see `3rdparty/ce/PORTING.md`) is there if you hit something similar.
- Nothing changes for the native port.

### 2026-11-15: midway image for batch 2 (`2026.11.15-batch2-midway`)

- Nothing changes for the native port: use the `2026.11.15-chromium-readiness` image for builds, since it went through the full checks.
- Linux Chrome under Renguin (separate from this port): Alpine's chromium renders a page with `--headless --dump-dom --single-process`. Multi-process mode and a window come later.
- `renga_wayland` now also has `libwayland-egl` and `libwayland-cursor`.

### 2026-11-15: landed: image `2026.11.15-chromium-readiness` (R-7 compositor side, R-8, SDL3)

Renga `143b8c30`, QEMU Tested. This image includes the compositor's `wl_fixes` v2 (R-7), R-8 with `R_X86_64_IRELATIVE` support, the LinuxKPI `idr_remove()` fix for the mt7921e panic during long builds, the kernel supplied-stack fix, and Renga's libsdl3 3.4.16-104 (SDL's evdev and HIDAPI gamepad back ends; Gamepad API base).
- **Building Chromium on this image:** what we checked on the image with HaikuPorts' nodejs24 24.21.0, python3.14 3.14.7, gn 2385, ninja 1.13.2 and llvm23 clang/lld: `node -e`, 40 node starts with a large heap, a node script using worker_threads, fs and child_process, python3, gn, ninja, `clang -fuse-ld=lld`, and your `tools/renga-probe.c` (032337d) built and run as `renga-tests.sh probe` does. All checks passed, and every tool runs natively. Things to watch: the node crash in `v8::Isolate::Initialize` you hit on `ea9753b2` did not reproduce here (2 to 12 CPUs, `-cpu max`), so tell us your node version and environment if it happens again. HaikuPorts' clang ignores `__attribute__((ifunc))` for the Haiku target. clang finds `ld.lld` only when `PATH` is exported.
- **Moved to Renga's next batch:** Linux Chrome under Renguin (the browser starts but calls a null function pointer during start-up) and the GPU stage.

### 2026-11-15: landed: R-8, native programs branded GNU stay native (next image)

QEMU Tested (`b2484110`); kernel and runtime_loader side, so it arrives with the next image. Renguin takes `EI_OSABI` 3 as a Linux brand only for programs without `PT_INTERP` and without `DT_NEEDED` (glibc's static and static-pie programs); `mksnapshot` and other lld-linked Haiku programs run natively. Faults of Linux threads now reach an attached debugger as for native threads and are logged; without a debugger a Linux program still dies by the signal. Also: Renga's runtime_loader now applies `R_X86_64_IRELATIVE` (after the segments are mapped executable, before initializers), so native programs with IFUNCs built by clang and lld load; on the current image they fail with "Troubles relocating: Bad data". Not yet: IFUNC symbols referenced from another image. Until the image: keep resetting byte 7 as you do.

### 2026-11-14: landed: R-7, wl_fixes version 2 (package `renga_wayland` `r1~beta6_hrev60381_305-1`)

QEMU Tested (`c855b4b3`). `renga_wayland` `r1~beta6_hrev60381_305-1` is in the Renga repository (Techniix/RengaPorts): `pkgman refresh && pkgman update renga_wayland`; it needs no system update. Its headers and both libraries' interface tables have `wl_fixes` version 2 from upstream Wayland (`wl_fixes_ack_global_remove()`, opcode 2, `WL_FIXES_ACK_GLOBAL_REMOVE_SINCE_VERSION`, error `invalid_ack_remove`); other interfaces keep their versions. The compositor side (advertising v2; a removed global, such as a `wl_output` whose screen went away, stays bindable until every v2 client acks, then goes; a bad ack is `invalid_ack_remove`) ships with the next image; until then the installed compositor offers `wl_fixes` v1, so Chromium's version guard skips the ack.

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
