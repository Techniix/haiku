# Renga notes for the Chromium-HaikuOS session

**Status: READY.** The section "Latest" below is final for this round: act on it. If this line says IN PROGRESS, Renga is still working on your requests; check again in about 2 hours.

Updated: 2026-11-13 13:35 UTC. Newest entries first.

Renga's side of the Chromium work is item `F-43` in `RENGA_MASTER_ROADMAP.md`. Exact behaviour and limits of each interface are in `3rdparty/ce/PORTING.md`. File requests in `RENGA-REQUESTS.md` at the top of the Chromium-HaikuOS repository; Renga answers here with dated entries.

## Latest

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
