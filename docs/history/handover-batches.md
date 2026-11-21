# Renga hand-over: earlier batches and the cloud-era procedure

Moved verbatim from [`docs/SESSION-HANDOVER.md`](../SESSION-HANDOVER.md) on 2026-11-21, when the hybrid local/cloud workflow was adopted ([docs/HYBRID-WORKFLOW.md](../HYBRID-WORKFLOW.md)). Nothing was reworded. The hand-over keeps the working method, the standing rules and the newest two "continue from here" sections. The roadmap's batch status paragraphs referred to below are in [work-orders.md](work-orders.md) or, for the newest, still in the roadmap.

## Cloud-era procedure (historical)

These parts of the hand-over applied to the cloud build host only. Shown as they stood, by their original section.

From "Repositories and paths":

- Renga: `Techniix/haiku`, branch `ce/main`, clone at `/home/claude/haiku`. `master` stays identical to upstream Haiku; never touch it or upstream tags.
- Builds: `Techniix/renga-builds`, clone at `/home/claude/renga-builds`. Each image goes on a branch `build/<YYYY.MM.DD>-<name>` with `parts/<iso>.partNNN` (`split -b 95M -d -a 3`), `<iso>.sha256` and `build.txt`, whose first line must be `iso: <iso>` (for example `iso: renga-2026.11.21-batch11-anyboot.iso`; the Publish workflow fails with "build.txt names no iso" without it; builds from 2026.11.15 to batch 10 lacked it and were fixed by the coordinator). Cut each build branch from current renga-builds main (its `tools/join.sh`). The maintainer publishes it with Actions → "Publish build" and the build name.

From the section of that name:

### Starting in a fresh workspace

A new session usually gets its own machine. If `/home/claude/haiku` does not exist, clone `Techniix/haiku` (branch `ce/main`) there and `Techniix/renga-builds` to `/home/claude/renga-builds`, then configure and build as `3rdparty/ce/README.md` describes (cross-tools first; about 2-3 hours on the 2-CPU host) before starting any helper that needs builds or gates.

From "How the work runs":

- Server restarts happen every few hours and stop helpers. Long runs started with `&` or `nohup` by a session are killed; use the tool's background mode. After a restart: check `ps` for running gates first, then resume the helper with SendMessage to its id (never start duplicate gates).
- A watchdog check-in runs every hour (every 30 minutes before 2026-11-13 evening) (`send_later`, short message carrying the current helper id and queue). It checks `git log`, `git status`, `ps`, and disk, resumes stopped helpers, and reschedules itself. Ignore stale check-ins that arrive late.
- Disk is tight: keep `generated.x86_64/ce-test-results` to the newest 6 (never the running one), and keep at least 3 GB free before an image build.

From "Working with the maintainer" (the full bullet; the hand-over keeps it without its last sentence):

- Usage is limited until the plan ends in early December; time-boxed priorities are in the newest work order. Check-ins are hourly (2026-11-13).

From "Where things stand" (the full paragraph; the hand-over keeps it without its last sentence):

See the roadmap's newest work-order paragraph and the dated decision log. At the time of writing (2026-11-13, 20:10 ET): images `2026.11.13-gamepads` and `2026.11.13-devel-vulkan` are delivered (the maintainer publishes them). Open from that batch: publishing Renga's packages (needs the system packages' vendor set to "Techniix", queued) and GitHub Pages on RengaPorts' `packages` branch (the maintainer's step); FFmpeg with Vulkan is a RengaPorts recipe (`07b61ff`) that the maintainer builds with HaikuPorter on a Renga machine; vulkan_loader is not yet in the image. Next, per the newest work order: the pointer and packaging fix, then Renguin (F-27), then the GPU stage. No helper is running at hand-over; start one for the pointer fix, then one for Renguin. Restart the hourly watchdog with `send_later`.

## Earlier batches

Batches 2-10, as they stood in the hand-over (headings one level lower).

### Batch 2: continue from here

Batch 2 ended 2026-11-19 with image `2026.11.19-batch2`; the roadmap's batch-2 status lists what continues (multi-process Chrome, GPU stage, Wi-Fi data path after measurements, node crash).

### Batch 3: continue from here

Batch 3 ended 2026-11-20 with image `2026.11.20-batch3`; the roadmap's batch-3 status lists what landed. Continue with:

- **GPU stage** (not started in batch 3): the vendor-neutral `syncobj` kernel module per QUASAR.md R-18 (DRM syncobj semantics and ioctl layouts, exported ioctl handler for render nodes, `/dev/syncobj`, binary and timeline, sync_file, `SYNCOBJ_EVENTFD`, kernel fence API, Renguin passthrough), then `wp_linux_drm_syncobj_v1` and `linux-dmabuf` v4, then the Screen → Displays panel (built as a Control Panel page, F-28).
- **Multi-process Linux Chrome:** works in 3 of 5 runs; when it fails, children report "no connection" after 15 s. Use `trace_process "chromium"` with `trace_buffer true` (in-memory trace, PORTING.md) on a failing run.
- **Rest of the routing batch** (several inputs, virtual devices, mic processing, mute/push-to-talk, ducking, presets) and the rest of the gamepad configurator list (layout pictures, Identify/Change player actions, Mapping tab, quiet mode).
- **Wi-Fi:** waiting for the maintainer's `lkpictl wifi-stats` numbers (now with chip-reset reasons); the two receive copies per frame are unchanged.
- **Harness:** the renga-raw profile's default timeout is now 2700 s (crashlog_test's last boot was cut off at 1800 s); media_latency low-latency checks fail in QEMU under the full renga-raw load (timing; pre-existing class).
- Node crash attempt under build load: still not tried.

### Batch 4: continue from here

Batch 4 ended 2026-11-20 with image `2026.11.20-batch4`; the roadmap's batch-4 status lists what landed. Continue with:

- **Not started in batch 4:** boot loader A with restore points before every package update (item 4) and streaming (item 10).
- **Multi-monitor in app_server:** the accelerant table (`MultiHead.h`) and the Displays page exist; app_server itself still drives one screen. Wait for Quasar's answer in its `RENGA-REQUESTS.md` before building on the table.
- **Renguin:** Renguin Manager, /proc stat/readdir, netlink notifications, the rest of COVERAGE.md's "next" list, multi-process Chrome (3 of 5 runs).
- **Hardware results pending:** KDL USB keyboard (if still no input, look at `XHCI::StartDebugTransfer()` giving up on held locks), DriveSetup Write image on a USB stick, the Secret Service prompt with Linux Chrome.
- **Harness:** selftest.py fails one check on a clean tree ("sound settings: clean recordings pass", the synthetic mirror phases); media_usb.unplug_fallback fails in renga-raw (open since F-31).

### Batch 5: continue from here

Batch 5 ended 2026-11-20 with image `2026.11.20-batch5`; the roadmap's batch-5 status lists what landed. Continue with:

- **Multi-monitor in app_server**, on `MultiHead.h` version 2 as Quasar asked (R-19, accepted in QUASAR.md `ddfe7e55`): output numbering with `B_BUSY` past the head count; an atomic `set_configuration(multi_head_config*, count)` with `ENABLED`, `ALLOW_VRR` and `TEST_ONLY`; mirroring through `source_head`; the classic shared cursor image with an output-local `move_head_cursor()`; `*_millihz` names; `max_bpc` and `gamma_size` in reserved fields. Then app_server's screens, windows between screens, Deskbar and workspaces per screen, hot-plug.
- **Not started in batch 5:** Bluetooth through LinuxKPI (btusb, btintel, btmtk, btrtl, btbcm; HID host; A2DP; QEMU has no USB Bluetooth, so unit tests and the btusb probe path), HaikuDepot with Renga, HaikuPorts, Flatpak and apk sources, Renguin netlink notifications, multi-process Linux Chrome (3 of 5), the streaming list after Hide from capture (replay buffer first), Open with / Renguin badge / AppImage / Flatpak, Windows programs through WINE.
- **Devices and drivers:** test-and-revert switching, per-device policy, "Native drivers only", USB devices; native drivers show "Native or none" (the device manager does not record legacy drivers).
- **Boot loader A leftovers:** menu shown by default when another system is found, several Renga installs pinned by device path, "Restart into..." in the Deskbar, "Native drivers only" and "Collect diagnostics" recovery entries, boot progress recording.
- **Hardware results pending:** boot counting and recovery on the maintainer's PC (BIOS writes and UEFI BlockIo writes are QEMU Tested only), the Windows entry, SDL_Init time with the 8BitDo Ultimate 2C (libsdl3 -105 and the hidraw timeouts), the audio rows after quitting OpenCE, the welcome on a fresh install, Hide from capture with OBS.
- **Harness:** the full renga-raw run blanked the screen before screen_capture_test once (capture.* failed, passed on the retry); pointer_rate failed on a stale ce_vmouse object (now logs why a plug is refused). A stray `/boot` directory on the build host came from a `cmake --install` without DESTDIR while building libsdl3 -105; the safety check blocked removing it, so the maintainer may want to delete it.

### Batch 6: continue from here

Batch 6 ended 2026-11-20 with image `2026.11.20-batch6`; the roadmap's batch-6 status lists what landed. Continue with:

- **Lumina (item 3), not started:** snap layouts (rows x columns, zone editor, spanning, modifier-drag grid, per-display and per-workspace layouts, gaps, zone shortcuts, snap assist, saved arrangements, per-app rules, edge sensitivity) and visible workspaces (Deskbar switcher, overview, per-workspace name/wallpaper/layout, saved sets, "Move to workspace", Workspaces page, welcome tip). Lumina lives in `Techniix/Lumina_HaikuOS` and is not in the Renga tree yet. Its S1-S8 acceptance (`F-25`) still comes first.
- **Batch 5 carry-overs (item 4), not started:** Bluetooth through LinuxKPI, HaikuDepot with every source, Open with / Renguin badge / AppImage / Flatpak, the replay buffer and the rest of streaming, Devices and drivers leftovers, boot loader A leftovers, multi-process Linux Chrome.
- **Several screens, next steps:** read `event_fd()` for hot-plug (a thread in AccelerantHWInterface that calls Desktop::ReloadDisplays()), Deskbar and workspaces per output, `move_output_cursor()` for hardware cursors, per-output mode lists for programs, and direct windows (BDirectWindow, BWindowScreen) with several screens. Test with `ce-test.py --displays 2`.
- **Hardware results pending:** boot counting on the maintainer's PC with the new checks (BIOS and UEFI BlockIo writes are QEMU Tested only), the Windows entry, and several screens with Quasar once it offers `MultiHead.h` version 2.
- **Harness:** `sound_audio.mirror_second` failed in renga-raw and on its retry, as in batch 5 (known-flaky). `ce-test-results` holds three runs from before batch 6 (100816, 101445, 102244) that the batch-6 helper did not delete.


### Batch 7: continue from here

Batch 7 ended 2026-11-20 with image `2026.11.20-batch7`; the roadmap's batch-7 status lists what landed. Continue with:

- **Mesa (superseded in batch 8):** RengaPorts `mesa-26.3.0` revision 100 with Zink (`3621a65`, Quasar R-20) replaces `mesa-26.2.2`; build it with `haikuporter -S mesa`, then `tools/update-repo.sh`. Previously: `mesa-26.2.2` revision 100 was a recipe only; the maintainer builds it with HaikuPorter (`haikuporter -S mesa`, then `tools/update-repo.sh`). The image still has Mesa 22.0.5 until the packages exist. Quasar to confirm its NVK ICD next to it (QUASAR.md 2026-11-20). Zink is not enabled (no Haiku window-system support upstream).
- **Not started:** virtio-gpu 3D (virgl); every batch 5/6 carry-over (several screens' `event_fd()` hot-plug, Deskbar and workspaces per output, per-output cursor and mode lists; HaikuDepot with every source; Open with / Renguin badge / AppImage / Flatpak; replay buffer; Devices and drivers leftovers; boot loader A leftovers; Bluetooth through LinuxKPI).
- **Lumina leftovers:** snap assist (offering other windows for the empty zones), edge snapping without a key, the window overview, the panel, title-bar minimize button and double-click setting; window menu strings are not translated (libbe).
- **Hardware results pending:** native mode at boot on the maintainer's PC (BIOS and UEFI), the changed-pixel copies' speed with the default driver and with Quasar (switch on the Displays page), snap layouts and the Deskbar switcher on a real screen.
- **Harness:** `snap.deskbar_switcher` is only checked on minimum-* images (renga-* show the first-boot prompt in that phase). This session left its own result directories (20261120-143709 onward); the older ones are the earlier sessions'.

### Batch 8: continue from here

Batch 8 ended 2026-11-20 with image `2026.11.20-batch8`; the roadmap's batch-8 status lists what landed. Continue with:

- **Maintainer builds on Renga (recipes only so far):** RengaPorts `mesa-26.3.0` revision 100 (Zink, Quasar R-20) and `haikuwebkit-1.10.0` revision 101 (HTML5 sound fix, user agent): `haikuporter -S mesa`, `haikuporter -S haikuwebkit`, then `tools/update-repo.sh` with the packages. After that: run `media_html5_test` (renga-raw) with the new haikuwebkit, and check virgl/Zink with the new Mesa; tell Quasar when Mesa 26.3 is in RengaPorts so it can drop its own Zink, libglvnd and libEGL_mesa.
- **Not started (carry-overs):** snap assist, title-bar minimize button, the window overview and the panel (Lumina); several screens' `event_fd()` hot-plug (virtio-gpu offers no event_fd yet), Deskbar and workspaces per output, per-output cursor and mode lists; HaikuDepot with every source; replay buffer; Devices and drivers leftovers; boot loader A leftovers; virgl.
- **HaikuWebKit on the Linux host:** configures with Renga's cross-tools (CMake toolchain file, a sysroot from `haiku`/`haiku_devel` plus the recipe's HaikuPorts packages); single files can be compile-checked; a full build is 5-7 hours on 2 cores (F-57).
- **Harness:** the screen saver's timer is now off during runs (`b293b908`); `snap.s3_snap_drop` was racy (fixed in `74b3b0e8`). This session left its own result directories from 20261120-185437 on; the older ones are earlier sessions'.
- **Next batch:** 9, Tracker and Deskbar (review first; start-button designs to the maintainer before implementing).

### Batch 9: continue from here

Batch 9 ended 2026-11-21 with image `2026.11.21-batch9`; the roadmap's batch-9 status lists what landed. Continue with:

- **Maintainer decision pending:** the start button, one of four options in `docs/design/start-button/start-button-options.png` (A three leaves, B leaf and sun, C leaf pinwheel, D badge with a ray); then implement it (HVIF from the SVG, light/dark, hover and press).
- **Not started (F-66):** tabs, sidebar, clickable breadcrumb, one operation queue window, Quick Look for video/audio/PDF/fonts and their thumbnails, split and gallery views, grouping, Open with defaults with Renguin, attribute editor, crash restart with windows, add-ons out of process, dark/HiDPI. **(F-41):** pinned apps with drag and drop, menu editing, typing in the open menu goes to the search, grouped window list, status area, per-monitor Deskbars; the Deskbar menu rebuilds on every opening (25-217 ms in QEMU), a prefetch like F-59's would help.
- **Batch 8 carry-overs:** still not started (snap assist, title-bar minimize button, window overview, HaikuDepot every source, replay buffer, Devices and drivers, boot loader A leftovers, virgl once Mesa 26.3 packages exist).
- **Harness:** renga-raw: media_routing.exclude_beside_plain and transient_live_switch failed in the full run (passed alone), media_routing.exclude_normalized failed once alone; candidates for the known-flaky list or a fix. A scripted B_KEY_DOWN never reaches KeyDown() (tests use hooks). This session's result directories: 20261121-012441 onward.
- **Upstream ticket material (maintainer's words):** Tracker's Revert of "Hide dotfiles" (fixed in Renga, `5f3dc53b`); CheckName()'s file-versus-folder guard only fires for relative links.

### Batch 10: continue from here

Batch 10 ended 2026-11-21 with image `2026.11.21-batch10` (Renga `139bba14`); the roadmap's batch-10 status lists what landed. Continue with:

- **Open from batch 10:** Tracker's Open with list (the registrar answers B_NAME_NOT_FOUND to SetSupportedTypes for the launcher signatures, `linux_apps.open_with`); AppImage double-click through the roster (`linux_apps.appimage`; running linux-apps on the file works); why a native cat spun in full renga-raw runs (seen three times, never alone); IPv4-mapped IPv6 (dual-stack lookups in TCP/UDP); ptrace; V4L2; sysfs/udev enumeration for SDL; Debian/Arch roots (the module's root path is fixed); Flatpak (namespaces); Wine from the Alpine root on real hardware; badge on the window tab and window list.
- **Harness:** renga-raw hit the 2700 s guest timeout before crashlog_test finished (4 checks); sound_settings.output_device_menu and media_latency.device_continuous failed once (no media code changed).
- This session's result directories: 20261121-042301 onward (images removed).
