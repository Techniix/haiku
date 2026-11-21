# Renga coordination: session hand-over

This file lets a new coordinating session continue the work exactly as before. The roadmap (`RENGA_MASTER_ROADMAP.md`) is the authoritative plan: its newest "Work order" paragraph near the top says what comes next. This file holds the working method and the standing rules. Read both before doing anything.

## Repositories and paths

- Renga: `Techniix/haiku`, branch `ce/main`, clone at `/home/claude/haiku`. `master` stays identical to upstream Haiku; never touch it or upstream tags.
- Builds: `Techniix/renga-builds`, clone at `/home/claude/renga-builds`. Each image goes on a branch `build/<YYYY.MM.DD>-<name>` with `parts/<iso>.partNNN` (`split -b 95M -d -a 3`), `<iso>.sha256` and `build.txt`. The maintainer publishes it with Actions → "Publish build" and the build name.
- Partner ports read notes that Renga writes: `docs/partners/OBS.md`, `docs/partners/QUASAR.md` (NVIDIA driver, `Techniix/nvidia-haiku` branch `renga`), `docs/partners/CHROMIUM.md` (`Techniix/Chromium-HaikuOS` branch `renga`). Their requests are in each port's `RENGA-REQUESTS.md`. When an entry lands, tell the maintainer so he can point that session at it.
- Harness: `3rdparty/ce/harness/ce-gates.sh`, `ce-test.py`, documented in `3rdparty/ce/README.md`. Port-visible interfaces: `3rdparty/ce/PORTING.md`. Image build: `3rdparty/ce/build/build-handover-image renga-anyboot`, from a clean tree.

## Starting in a fresh workspace

A new session usually gets its own machine. If `/home/claude/haiku` does not exist, clone `Techniix/haiku` (branch `ce/main`) there and `Techniix/renga-builds` to `/home/claude/renga-builds`, then configure and build as `3rdparty/ce/README.md` describes (cross-tools first; about 2-3 hours on the 2-CPU host) before starting any helper that needs builds or gates.

## How the work runs

- The coordinating session plans, answers the maintainer, records his decisions in the roadmap, writes partner notes, and runs **one helper agent at a time** for implementation batches. Each helper gets a complete, self-contained brief: the roadmap sections to read, the scope, the rules below, and a report format.
- Server restarts happen every few hours and stop helpers. Long runs started with `&` or `nohup` by a session are killed; use the tool's background mode. After a restart: check `ps` for running gates first, then resume the helper with SendMessage to its id (never start duplicate gates).
- A watchdog check-in runs every hour (every 30 minutes before 2026-11-13 evening) (`send_later`, short message carrying the current helper id and queue). It checks `git log`, `git status`, `ps`, and disk, resumes stopped helpers, and reschedules itself. Ignore stale check-ins that arrive late.
- Disk is tight: keep `generated.x86_64/ce-test-results` to the newest 6 (never the running one), and keep at least 3 GB free before an image build.
- The maintainer is on a phone: short, plain replies that lead with the answer, and no message on routine check-ins. Report image deliveries with name, sha256, the Publish step and a hardware test list.

## Standing rules (maintainer's)

- Commits: explicit paths only (`git commit -- <paths>`, `git add -- <new files>`), never `-A`/`-a`; very detailed messages ending with the two trailer lines `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>` and `Claude-Session: <session URL>`; push with `git push -q origin HEAD:ce/main`; never force-push.
- Upstream Haiku does not accept AI-assisted work, so everything stays in the fork. Upstream tickets are written by the maintainer in his own words.
- Add-only ABI. Every user-facing feature gets a GUI and an on/off setting with sensible defaults ("Advanced customization, easy to find"). Lean by design; responsiveness first. "Polish while in there". Every Renga program gets its own HVIF icon in the Renga style.
- Testing (maintainer, 2026-11-14): per piece, build and run only that piece's own suites as a partial run (`ce-test.py --only …`, about 5-10 minutes), then commit; kernel or boot-path changes also get one quick default-profile boot run; the full four-way sweep (default, `--smp 2`, `--firmware uefi`, `--profile renga-raw`) once per batch before an image. Known-flaky checks are retried once (`KNOWN_FLAKY` in `ce-gates.sh`, each with a reason). Every fix gets a regression test shown failing on the old code where practical. QEMU results are never presented as hardware results. Failed tests are never dropped silently.
- Docs are professional and never mention personal matters. Roadmap and partner-note entries are short (a line or two); commit messages carry the detail.
- Usage is limited: prefer fewer, larger helper briefs; keep check-ins short.

## Working with the maintainer

- He decides direction; the session proposes options with a recommendation, then records his decision (with date) in the roadmap's decision log before acting. Details he leaves "at your discretion" are decided and stated in one line.
- When he brings ideas, review them honestly, expand where useful, say what not to take and why, then place them in the work order and confirm where.
- His test hardware: an AMD desktop with an RTX 5070 (Quasar), a MediaTek MT7922 Wi-Fi card (mt7921e via LinuxKPI), a mesh Wi-Fi network, USB audio, many gamepads (8BitDo Ultimate 2C, Switch Pro, Xbox, DualSense, generic), and a Windows install he boots from Renga's boot menu.
- Usage is limited until the plan ends in early December; time-boxed priorities are in the newest work order. Check-ins are hourly (2026-11-13).
- Earlier conversations can be searched (past-chat search) if a detail is missing; the roadmap and partner notes take precedence.

## Where things stand

See the roadmap's newest work-order paragraph and the dated decision log. At the time of writing (2026-11-13, 20:10 ET): images `2026.11.13-gamepads` and `2026.11.13-devel-vulkan` are delivered (the maintainer publishes them). Open from that batch: publishing Renga's packages (needs the system packages' vendor set to "Techniix", queued) and GitHub Pages on RengaPorts' `packages` branch (the maintainer's step); FFmpeg with Vulkan is a RengaPorts recipe (`07b61ff`) that the maintainer builds with HaikuPorter on a Renga machine; vulkan_loader is not yet in the image. Next, per the newest work order: the pointer and packaging fix, then Renguin (F-27), then the GPU stage. No helper is running at hand-over; start one for the pointer fix, then one for Renguin. Restart the hourly watchdog with `send_later`.

## Agreed batch plan (2026-11-15)

Each batch: per-piece own suites, one four-way sweep, one ISO. GPU parts go last in a batch, and the ISO ships without them if they aren't ready.

1. Current batch, trimmed for usage: SDL3 gamepads, the pointer at 1000 Hz, Renguin fixes, R-7/R-8, the node crash, the FreeBSD Wi-Fi sync with the idr fix; short Chromium readiness checks.
2. Linux Chrome under Renguin; the GPU stage for Quasar.
3. Gamepad configurator polish; Chromium phase D; audio routing; GPU leftovers with a Screen → Displays panel (arrangement, per-display mode/scale/rotation, VRR/HDR where supported, colour profile, Wayland switch, workspaces, GPU info; built as a Control Panel page).
4. Multi-monitor in app_server (accelerant multi-head interface, QUASAR.md entry); boot loader A with automatic restore points before every update; DriveSetup "Write image" fix; KDL USB keyboard input; Renguin leftovers (Renguin Manager, /proc stat and readdir, netlink notifications); icon pass; test health; streaming last.
5. Control Panel F-28 with search; LinuxKPI completion with Bluetooth and a Devices & Drivers page; first-boot welcome; Linux apps native, and Windows apps native through WINE; HaikuDepot showing every source, including Alpine apk; hardware report tool. No game library.
6. Lumina, with:
   - customizable snap layouts: any rows × columns, a zone editor, spanning, modifier-drag to show the grid, per-display and per-workspace layouts, gaps, zone shortcuts, snap assist, saved arrangements, per-app rules;
   - visible workspaces: a Deskbar switcher with "+" and previews, an overview, per-workspace name, wallpaper and layout, saved sets, "Move to workspace" in the tab menu, a mention in the first-boot welcome.
7. README redo.

## Agreed batch plan, continued (maintainer, 2026-11-20)

7. Lumina (snap layouts, visible workspaces; S1-S8 first), the default display driver (native mode, changed-region copies), software OpenGL without the extra copy, the README redo, batch 5/6 carry-overs; last, current Mesa in RengaPorts and virtio-gpu 3D.
8. `F-57` WebKit pulled forward: WebPositive quick fixes and speed pass; a time-boxed HaikuWebKit cross-compile on the Linux host (fallback: RengaPorts recipe and patches the maintainer builds); WebKit's own HTML5 sound fix; MSE rebase after the Chromium phases unless the cross-compile goes well.
9. Finishing half-done work: multi-process Linux Chrome, the rest of audio routing, the rest of the gamepad configurator, the Wi-Fi data path if `wifi-stats` numbers arrive.
10. Installer wizard `F-23`, partition manager `F-24`, graphical themed boot menu `F-22`.

Later: bundled apps `F-44`, Secure Boot `F-53`, NVIDIA-hardware GPU features `F-47`/`F-35`.

## Batch order (maintainer, 2026-11-20, 15:29 ET; replaces items 9-10 above)

8. `F-57` WebKit (this batch), then batch 7 carry-overs as time allows.
9. Tracker and Deskbar: `F-60` review of both first, then `F-66` Tracker and `F-41` Deskbar, every feature switchable; start-button designs shown to the maintainer before implementing.
10. Renguin (`F-27`): multi-process Linux Chrome; Linux-programs integration (Open with, Renguin badge, AppImage, Flatpak); the rest of the Renguin Manager; syscall coverage per `COVERAGE.md`; vDSO and shared futexes; gamepad evdev and webcam V4L2 bridges; matching GTK/Qt themes; Linux home and xattr mapping; Wine/Proton under Renguin; the harness ladder.
11. The rest of audio routing, the rest of the gamepad configurator, the Wi-Fi data path once `lkpictl wifi-stats` numbers arrive.
12. Installer wizard `F-23`, partition manager `F-24`, graphical themed boot menu `F-22`.

Later: `F-44`, `F-53`, `F-47`/`F-35`.

## Batch 2: continue from here

Batch 2 ended 2026-11-19 with image `2026.11.19-batch2`; the roadmap's batch-2 status lists what continues (multi-process Chrome, GPU stage, Wi-Fi data path after measurements, node crash).

## Batch 3: continue from here

Batch 3 ended 2026-11-20 with image `2026.11.20-batch3`; the roadmap's batch-3 status lists what landed. Continue with:

- **GPU stage** (not started in batch 3): the vendor-neutral `syncobj` kernel module per QUASAR.md R-18 (DRM syncobj semantics and ioctl layouts, exported ioctl handler for render nodes, `/dev/syncobj`, binary and timeline, sync_file, `SYNCOBJ_EVENTFD`, kernel fence API, Renguin passthrough), then `wp_linux_drm_syncobj_v1` and `linux-dmabuf` v4, then the Screen → Displays panel (built as a Control Panel page, F-28).
- **Multi-process Linux Chrome:** works in 3 of 5 runs; when it fails, children report "no connection" after 15 s. Use `trace_process "chromium"` with `trace_buffer true` (in-memory trace, PORTING.md) on a failing run.
- **Rest of the routing batch** (several inputs, virtual devices, mic processing, mute/push-to-talk, ducking, presets) and the rest of the gamepad configurator list (layout pictures, Identify/Change player actions, Mapping tab, quiet mode).
- **Wi-Fi:** waiting for the maintainer's `lkpictl wifi-stats` numbers (now with chip-reset reasons); the two receive copies per frame are unchanged.
- **Harness:** the renga-raw profile's default timeout is now 2700 s (crashlog_test's last boot was cut off at 1800 s); media_latency low-latency checks fail in QEMU under the full renga-raw load (timing; pre-existing class).
- Node crash attempt under build load: still not tried.

## Batch 4: continue from here

Batch 4 ended 2026-11-20 with image `2026.11.20-batch4`; the roadmap's batch-4 status lists what landed. Continue with:

- **Not started in batch 4:** boot loader A with restore points before every package update (item 4) and streaming (item 10).
- **Multi-monitor in app_server:** the accelerant table (`MultiHead.h`) and the Displays page exist; app_server itself still drives one screen. Wait for Quasar's answer in its `RENGA-REQUESTS.md` before building on the table.
- **Renguin:** Renguin Manager, /proc stat/readdir, netlink notifications, the rest of COVERAGE.md's "next" list, multi-process Chrome (3 of 5 runs).
- **Hardware results pending:** KDL USB keyboard (if still no input, look at `XHCI::StartDebugTransfer()` giving up on held locks), DriveSetup Write image on a USB stick, the Secret Service prompt with Linux Chrome.
- **Harness:** selftest.py fails one check on a clean tree ("sound settings: clean recordings pass", the synthetic mirror phases); media_usb.unplug_fallback fails in renga-raw (open since F-31).

## Batch 5: continue from here

Batch 5 ended 2026-11-20 with image `2026.11.20-batch5`; the roadmap's batch-5 status lists what landed. Continue with:

- **Multi-monitor in app_server**, on `MultiHead.h` version 2 as Quasar asked (R-19, accepted in QUASAR.md `ddfe7e55`): output numbering with `B_BUSY` past the head count; an atomic `set_configuration(multi_head_config*, count)` with `ENABLED`, `ALLOW_VRR` and `TEST_ONLY`; mirroring through `source_head`; the classic shared cursor image with an output-local `move_head_cursor()`; `*_millihz` names; `max_bpc` and `gamma_size` in reserved fields. Then app_server's screens, windows between screens, Deskbar and workspaces per screen, hot-plug.
- **Not started in batch 5:** Bluetooth through LinuxKPI (btusb, btintel, btmtk, btrtl, btbcm; HID host; A2DP; QEMU has no USB Bluetooth, so unit tests and the btusb probe path), HaikuDepot with Renga, HaikuPorts, Flatpak and apk sources, Renguin netlink notifications, multi-process Linux Chrome (3 of 5), the streaming list after Hide from capture (replay buffer first), Open with / Renguin badge / AppImage / Flatpak, Windows programs through WINE.
- **Devices and drivers:** test-and-revert switching, per-device policy, "Native drivers only", USB devices; native drivers show "Native or none" (the device manager does not record legacy drivers).
- **Boot loader A leftovers:** menu shown by default when another system is found, several Renga installs pinned by device path, "Restart into..." in the Deskbar, "Native drivers only" and "Collect diagnostics" recovery entries, boot progress recording.
- **Hardware results pending:** boot counting and recovery on the maintainer's PC (BIOS writes and UEFI BlockIo writes are QEMU Tested only), the Windows entry, SDL_Init time with the 8BitDo Ultimate 2C (libsdl3 -105 and the hidraw timeouts), the audio rows after quitting OpenCE, the welcome on a fresh install, Hide from capture with OBS.
- **Harness:** the full renga-raw run blanked the screen before screen_capture_test once (capture.* failed, passed on the retry); pointer_rate failed on a stale ce_vmouse object (now logs why a plug is refused). A stray `/boot` directory on the build host came from a `cmake --install` without DESTDIR while building libsdl3 -105; the safety check blocked removing it, so the maintainer may want to delete it.

## Batch 6: continue from here

Batch 6 ended 2026-11-20 with image `2026.11.20-batch6`; the roadmap's batch-6 status lists what landed. Continue with:

- **Lumina (item 3), not started:** snap layouts (rows x columns, zone editor, spanning, modifier-drag grid, per-display and per-workspace layouts, gaps, zone shortcuts, snap assist, saved arrangements, per-app rules, edge sensitivity) and visible workspaces (Deskbar switcher, overview, per-workspace name/wallpaper/layout, saved sets, "Move to workspace", Workspaces page, welcome tip). Lumina lives in `Techniix/Lumina_HaikuOS` and is not in the Renga tree yet. Its S1-S8 acceptance (`F-25`) still comes first.
- **Batch 5 carry-overs (item 4), not started:** Bluetooth through LinuxKPI, HaikuDepot with every source, Open with / Renguin badge / AppImage / Flatpak, the replay buffer and the rest of streaming, Devices and drivers leftovers, boot loader A leftovers, multi-process Linux Chrome.
- **Several screens, next steps:** read `event_fd()` for hot-plug (a thread in AccelerantHWInterface that calls Desktop::ReloadDisplays()), Deskbar and workspaces per output, `move_output_cursor()` for hardware cursors, per-output mode lists for programs, and direct windows (BDirectWindow, BWindowScreen) with several screens. Test with `ce-test.py --displays 2`.
- **Hardware results pending:** boot counting on the maintainer's PC with the new checks (BIOS and UEFI BlockIo writes are QEMU Tested only), the Windows entry, and several screens with Quasar once it offers `MultiHead.h` version 2.
- **Harness:** `sound_audio.mirror_second` failed in renga-raw and on its retry, as in batch 5 (known-flaky). `ce-test-results` holds three runs from before batch 6 (100816, 101445, 102244) that the batch-6 helper did not delete.


## Batch 7: continue from here

Batch 7 ended 2026-11-20 with image `2026.11.20-batch7`; the roadmap's batch-7 status lists what landed. Continue with:

- **Mesa (superseded in batch 8):** RengaPorts `mesa-26.3.0` revision 100 with Zink (`3621a65`, Quasar R-20) replaces `mesa-26.2.2`; build it with `haikuporter -S mesa`, then `tools/update-repo.sh`. Previously: `mesa-26.2.2` revision 100 was a recipe only; the maintainer builds it with HaikuPorter (`haikuporter -S mesa`, then `tools/update-repo.sh`). The image still has Mesa 22.0.5 until the packages exist. Quasar to confirm its NVK ICD next to it (QUASAR.md 2026-11-20). Zink is not enabled (no Haiku window-system support upstream).
- **Not started:** virtio-gpu 3D (virgl); every batch 5/6 carry-over (several screens' `event_fd()` hot-plug, Deskbar and workspaces per output, per-output cursor and mode lists; HaikuDepot with every source; Open with / Renguin badge / AppImage / Flatpak; replay buffer; Devices and drivers leftovers; boot loader A leftovers; Bluetooth through LinuxKPI).
- **Lumina leftovers:** snap assist (offering other windows for the empty zones), edge snapping without a key, the window overview, the panel, title-bar minimize button and double-click setting; window menu strings are not translated (libbe).
- **Hardware results pending:** native mode at boot on the maintainer's PC (BIOS and UEFI), the changed-pixel copies' speed with the default driver and with Quasar (switch on the Displays page), snap layouts and the Deskbar switcher on a real screen.
- **Harness:** `snap.deskbar_switcher` is only checked on minimum-* images (renga-* show the first-boot prompt in that phase). This session left its own result directories (20261120-143709 onward); the older ones are the earlier sessions'.

## Batch 8: continue from here

Batch 8 ended 2026-11-20 with image `2026.11.20-batch8`; the roadmap's batch-8 status lists what landed. Continue with:

- **Maintainer builds on Renga (recipes only so far):** RengaPorts `mesa-26.3.0` revision 100 (Zink, Quasar R-20) and `haikuwebkit-1.10.0` revision 101 (HTML5 sound fix, user agent): `haikuporter -S mesa`, `haikuporter -S haikuwebkit`, then `tools/update-repo.sh` with the packages. After that: run `media_html5_test` (renga-raw) with the new haikuwebkit, and check virgl/Zink with the new Mesa; tell Quasar when Mesa 26.3 is in RengaPorts so it can drop its own Zink, libglvnd and libEGL_mesa.
- **Not started (carry-overs):** snap assist, title-bar minimize button, the window overview and the panel (Lumina); several screens' `event_fd()` hot-plug (virtio-gpu offers no event_fd yet), Deskbar and workspaces per output, per-output cursor and mode lists; HaikuDepot with every source; replay buffer; Devices and drivers leftovers; boot loader A leftovers; virgl.
- **HaikuWebKit on the Linux host:** configures with Renga's cross-tools (CMake toolchain file, a sysroot from `haiku`/`haiku_devel` plus the recipe's HaikuPorts packages); single files can be compile-checked; a full build is 5-7 hours on 2 cores (F-57).
- **Harness:** the screen saver's timer is now off during runs (`b293b908`); `snap.s3_snap_drop` was racy (fixed in `74b3b0e8`). This session left its own result directories from 20261120-185437 on; the older ones are earlier sessions'.
- **Next batch:** 9, Tracker and Deskbar (review first; start-button designs to the maintainer before implementing).

## Batch 9: continue from here

Batch 9 (Tracker and Deskbar, maintainer 2026-11-20) is in progress. Landed so far: Deskbar menu search with calculator and unit conversion, "Restart into", per-application recent documents (`02de5116`, suite `deskbar`). Next in the batch: the F-60 review with measurements, Tracker (F-66) pieces by value and size, start-button design options (designs only), then the four-way sweep and the image `build/2026.11.21-batch9`. This section is updated as pieces land.
