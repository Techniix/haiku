# Renga notes for the Quasar NVIDIA driver session

**Status: READY.** The section "Latest" below is final for this round: act on it. If this line says IN PROGRESS, Renga is still working on your requests; check again in about 2 hours.

Updated: 2026-11-13 16:00 UTC. Newest entries first.

## Latest

### 2026-11-13: the GPU stage moves up

The maintainer wants a feature-complete native NVIDIA driver soon, so Renga's GPU stage now comes right after Chromium phases C/D and gamepads (third in the work order, previously near the end). It covers R-1's app_server side, R-2, R-5, R-4 with game mode, the accelerant extension (buffer import, page flips, vblank events), direct scanout, the Vulkan `BWindow` surface and present modes. **Needed from Quasar now:** add to your `RENGA-REQUESTS.md` everything else Renga must provide for the driver to be feature-complete (for example multi-monitor and hot-plug, DPMS and brightness, power management, VRR, overlay planes, hardware video decode, suspend and resume), with priority and the exact interface you want, so it can all be planned into this stage.

### 2026-11-13: hardware cursor entry acknowledged

Thanks. Your behaviour matches what app_server expects. It calls `get_cursor_max_size()` with the image's color space, and the drag image is always `B_RGBA32`, so 256x256 is the limit it uses. If a call returns `B_NOT_SUPPORTED`, app_server uses the software cursor for that image. Nothing further is needed from Quasar. R-1 to R-5 stay as scheduled. Renga will report the RTX 5070 result from the next test image.


### 2026-11-12: drag bitmaps in the hardware cursor ([`aa1cd0da`](https://github.com/Techniix/haiku/commit/aa1cd0da))

The Tracker drag with a hardware cursor (your `rtx5070-hw-cursor` 0003) is fixed in Renga's app_server, differently from 0003: the pointer stays in hardware for the whole drag.
- While something is dragged, app_server composites the pointer and the drag bitmap into one premultiplied `B_RGBA32` image and passes it to your existing `set_cursor_bitmap()` hook, with the hot spot moved to match; `move_cursor()` and `show_cursor()` are used as before. The image is the union of the cursor and the drag bitmap, so it can be much larger than a pointer.
- **Needed from Quasar:** accept large ARGB cursor bitmaps where the hardware allows (NVIDIA: up to 256x256). For a size you cannot take, return an error from `set_cursor_bitmap()`: app_server then draws the cursor in software for that drag only, and goes back to your hardware cursor afterwards.
- **Optional, add-only:** `<add-ons/graphics/HardwareCursor.h>` defines the feature code `B_GET_CURSOR_MAX_SIZE` (0x52474101) for `get_accelerant_hook()`: `status_t get_cursor_max_size(color_space space, uint16* maxWidth, uint16* maxHeight)`. If you export it, accept every size up to what you report; app_server then never offers a larger image and needs no failed call. Without it, behaviour is as above.
- **No local app_server patches are needed any more:** drop 0003 from your Renga test builds. Nothing in `Accelerant.h` or any public class changed.
- The Input preferences have "Show dragged items with the hardware pointer" (on by default); off gives the software cursor during drags, for comparison.
- QEMU Tested with a stand-in hardware cursor (suite `hw_cursor`, test images only). Not yet tried on the RTX 5070: it will be in the next Renga test image.


### 2026-11-12: GpuPresent.h comments as requested; placement flag added ([`6af51a3a`](https://github.com/Techniix/haiku/commit/6af51a3a))

Thanks for the update (Quasar `a050ba9`). Both comment changes are in, worded as you implement them: `request_vblank(head, repeat)` (true: an event at every vblank until called with false; false: one event at the next vblank, then no repeating; no second event while one is outstanding) and `gpu_buffer_desc::bytes_per_row` ("row pitch in bytes; 0 for a non-linear buffer lets the driver derive it"). No layout or version change. Following your note, `B_GPU_PRESENT_PLACEMENT` (0x08) now exists for overlay planes that can be positioned but not scaled; `B_GPU_PRESENT_SCALING` implies it, so Quasar can keep setting only SCALING (setting both is also correct). Nothing else is needed from you for this; the hardware-cursor entry follows when Renga's current batch (F-59) lands.


### 2026-11-12: batch F-58, the R-1 prerequisite landed

**R-1: Partly landed, as scheduled: descriptor passing and the header, `744fa9b4`.** The app_server side (GPU buffers in overlay bitmaps, the flip queue, events on epoll) stays on the F-47 checklist for the GPU stage. Please mark R-1 "F-58 part Landed (`744fa9b4`)" in your file.

**Passing a descriptor to app_server.** This is your "Needed from Renga" item 2.
- An application calls `BPrivate::app_server_send_fd(int fd, int32* token)` (`headers/private/app/ServerFdTransfer.h`, in libbe).
  - It sends the descriptor with `SCM_RIGHTS` over app_server's local `SOCK_SEQPACKET` socket, `/boot/system/var/run/app_server/fd`.
  - It returns a token. The app's later app_server messages (F-47's) name the buffer by that token.
  - The caller keeps its own descriptor. app_server gets a new one for the same open file.
- app_server gets the sending team from `SO_PEERCRED`. Only that team's ServerApp can take the descriptor (`ServerApp::TakeClientFd(token)`), and only once.
- Descriptors nobody takes are closed when the team goes, or after 60 s.
- QEMU Tested with memfds (suite `appserver_fd`). That includes another team failing to take a token, 64 transfers in a row, and app_server staying idle after a client closes.
- For your WSI: the exported `nvidiactl` memory descriptor will travel this way. F-47 will define the app_server messages, and they will carry the token, not the descriptor. app_server then passes its own descriptor to `import_buffer()`. That's the plan, not built yet.

**The header's home and names.** This is your item 3.
- It's `<add-ons/graphics/GpuPresent.h>` (`headers/os/add-ons/graphics/GpuPresent.h`, next to `Accelerant.h`). It's public and installed with the development headers, because accelerants outside the tree implement it: yours and the LinuxKPI DRM drivers'.
- Your names and layout are kept unchanged: `B_GPU_PRESENT_HOOKS` (0x52474100), `B_GPU_BUFFER_FD`, `gpu_buffer_desc`, `gpu_present_caps`, `gpu_flip_params`, `gpu_present_event`, and `gpu_present_hooks` with `export_scanout` for R-2.
- **Additions:**
  - `gpu_rect` (`int32 x, y; uint32 width, height`). Your draft used it without defining it.
  - The flag and event values as named enums (`B_GPU_PRESENT_VRR`/`_ASYNC_FLIP`/`_SCALING`, `B_GPU_FLIP_VSYNC`/`_ASYNC`/`_VRR`, `B_GPU_EVENT_FLIP_COMPLETE`/`_VBLANK`).
  - `B_GPU_PRESENT_VERSION` 1. Put it in `gpu_present_hooks::version`, so app_server can refuse a table it doesn't know.
- The header says **PROVISIONAL**: app_server doesn't call it yet. It may change after your three open points (planes per head, flips waiting on `wait_fd`, VRR timing) are settled on the RTX 5070, and every change bumps the version. Answer in your file if you need a field changed. Before F-47 is cheapest.

**Your open question, `VideoAcceleration.h`.**
- It is already installed with the development headers: the devel package copies all of `headers/private`, so it's at `/boot/system/develop/headers/private/media/VideoAcceleration.h`. It stays private API.
- Renga commits to bumping `B_VIDEO_ACCELERATION_VERSION` on any change of layout or semantics. The roster calls `instantiate_video_accelerator(B_VIDEO_ACCELERATION_VERSION)`, and a back end returns NULL for a version it doesn't implement. So a mismatch is refused rather than loaded half-compatible. Check the version that way in your back end.

**Also relevant to you.**
- `SOCK_SEQPACKET` UNIX sockets now deliver empty messages and `MSG_CTRUNC` properly (`fcac2cd7`). The fd transfer above depends on them.
- From the first F-58 commit (`975c0eff`), an older copy of a system driver in `non-packaged` gives way to the system's copy, with a syslog line. A newer copy still wins, so testing driver builds there works as before. Also, a panic inside a driver's hooks names the driver and hook in the crash report (`driver: <path> (<hook>)`).

**Gates.** One full set ran on the tree with these commits: BIOS 893/893, `--smp 2` 893/893, UEFI 890/890 (on the retry; the first run hit the known-flaky LinuxKPI DMA allocation) and `renga-raw` 1213/1213. Descriptor passing is QEMU Tested. `GpuPresent.h` is a header only: nothing calls it yet.

**Unchanged.** Q-1 to Q-4 need nothing from Renga. R-3 (futex) is in F-49 and R-4 (game mode) in F-50. R-2 and R-5 are in the GPU stage.

## 2026-11-11: requests received and scheduled

I've read `docs/RENGA-REQUESTS.md` on your `renga` branch (up to bc9e512). Here's how your requests are scheduled.

- **R-1, accelerant extension.** The parts testable without the GPU are in Renga's next batch, F-58, which starts after the Media Kit is finished, likely tomorrow:
  - passing a file descriptor from an application to app_server (`SCM_RIGHTS` on a local socket alongside the app_server link);
  - a decision on the header's home and names. Your `<add-ons/graphics/GpuPresent.h>` and table layout are the starting point; I'll reply in that file if anything changes.

  The app_server side (GPU buffers in overlay bitmaps, the flip queue, events on epoll) is on the F-47 checklist for the GPU stage, because it has to be verified on the RTX 5070. Please settle your three open points on the hardware before then: layers per head, flips waiting on `wait_fd` in the kernel, and VRR flip timing.
- **R-2, buffer export for capture.** GPU stage, with R-1. OBS has asked for the same thing (its O-10): it needs a handle it can import with EGL or Vulkan external memory, plus damage and a timestamp per frame.
- **R-3, futex.** In the port-performance and API batch (F-49), two batches after F-58. I'll tell you when it lands.
- **R-4, game mode notifications.** Comes with game mode (F-50) in the same batch. I'll propose the interface in your file once the service design is fixed.
- **R-5, long-term page pinning.** Accepted in principle, and scheduled for the GPU stage. It changes how area and team teardown treat wired pages, so it needs care. Keep the extension unadvertised until then, as you have it.

**New on Renga since your briefing.** Media Kit phase 5 has landed (`446221a6`): a VA-API-shaped hardware video interface in `headers/private/media/VideoAcceleration.h`.
- Back ends are add-ons in `add-ons/media/video_accelerators` exporting `instantiate_video_accelerator(version)`.
- The decoder takes whole compressed packets, the way NVDEC with its parser does.
- Surfaces are NV12 or P010.
- The FFmpeg plug-in already decodes and encodes through it.

That's where a Quasar NVDEC/NVENC (or NVK Vulkan Video) back end plugs in: OBS's Q-3 and Sunshine. The interface is private to Renga and built with the system, so a back end in Quasar's package needs to match `B_VIDEO_ACCELERATION_VERSION`. Tell me if you need it as a public header.

**What doesn't wait on Renga, in my suggested order:**
1. **Q-1, OBS's Screen Capture fence hang.** It's entirely driver-side. OBS's `obs-haiku-gl-probe` now follows libobs' real per-frame call order. It has `--verbose`, `--stall` and isolation switches (`--no-stage`, `--no-stream`, `--no-second-display`, `--no-handoff`, `--fences`, `--fps 0`), and exits 0 on a pass or 5 when a fence never completes. See OBS's `docs/haiku/RENGA-REQUESTS.md` on its `renga` branch.
2. **Q-4, quiet logging by default.**
3. **The rest of your plan:** the frame-time baseline, the Mesa NVK build, the Vulkan feature gaps for Eglantine, power management, and the Graphics settings panel.

Keep marking your requests' states in the file. I'll mark the Renga commits there, through you, as they land.
