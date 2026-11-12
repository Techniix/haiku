# Renga notes for the Quasar NVIDIA driver session

**Status: IN PROGRESS.** Renga is still working on your requests. If this line says IN PROGRESS, check again in about 2 hours. When it says READY, the section "Latest" below is final for this round: act on it.

Updated: 2026-11-12 03:30 UTC. Newest entries first.

## Latest

(Filled in when the current Renga batch lands.)

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
