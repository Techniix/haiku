# Renga coordination: session hand-over

This file lets a new coordinating session continue the work exactly as before. The roadmap (`RENGA_MASTER_ROADMAP.md`) is the authoritative plan: its newest "Work order" paragraph near the top says what comes next. This file holds the working method and the standing rules. Read both before doing anything.

## Repositories and paths

- Renga: `Techniix/haiku`, branch `ce/main`. `master` stays identical to upstream Haiku; never touch it or upstream tags.
- Builds: `Techniix/renga-builds`. Each image goes on a branch `build/<YYYY.MM.DD>-<name>` with `parts/<iso>.partNNN` (`split -b 95M -d -a 3`), `<iso>.sha256` and `build.txt`, whose first line must be `iso: <iso>` (for example `iso: renga-2026.11.21-batch11-anyboot.iso`; the Publish workflow fails with "build.txt names no iso" without it; builds from 2026.11.15 to batch 10 lacked it and were fixed by the coordinator). Cut each build branch from current renga-builds main (its `tools/join.sh`). The maintainer publishes it with Actions → "Publish build" and the build name.
- Partner ports read notes that Renga writes: `docs/partners/OBS.md`, `docs/partners/QUASAR.md` (NVIDIA driver, `Techniix/nvidia-haiku` branch `renga`), `docs/partners/CHROMIUM.md` (`Techniix/Chromium-HaikuOS` branch `renga`). Their requests are in each port's `RENGA-REQUESTS.md`. When an entry lands, tell the maintainer so he can point that session at it.
- Harness: `3rdparty/ce/harness/ce-gates.sh`, `ce-test.py`, documented in `3rdparty/ce/README.md`. Port-visible interfaces: `3rdparty/ce/PORTING.md`. Image build: `3rdparty/ce/build/build-handover-image renga-anyboot`, from a clean tree.

## How the work runs

- The coordinating session plans, answers the maintainer, records his decisions in the roadmap, writes partner notes, and runs **one helper agent at a time** for implementation batches. Each helper gets a complete, self-contained brief: the roadmap sections to read, the scope, the rules below, and a report format.
- **Local execution (2026-11-21):** builds, tests, gates, images and the Git workflow now run on the maintainer's PC as described in [docs/HYBRID-WORKFLOW.md](HYBRID-WORKFLOW.md) (scripts in `tools/renga/`, summaries with `renga-summary`; no watchdog, no restarts). The cloud-era procedure (server restarts, the watchdog, `/home/claude` paths, disk pruning, starting in a fresh cloud workspace) is kept verbatim in [docs/history/handover-batches.md](history/handover-batches.md#cloud-era-procedure-historical).
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
- Usage is limited until the plan ends in early December; time-boxed priorities are in the newest work order.
- Earlier conversations can be searched (past-chat search) if a detail is missing; the roadmap and partner notes take precedence.

## Where things stand

See the roadmap's newest work-order paragraph and the dated decision log. At the time of writing (2026-11-13, 20:10 ET): images `2026.11.13-gamepads` and `2026.11.13-devel-vulkan` are delivered (the maintainer publishes them). Open from that batch: publishing Renga's packages (needs the system packages' vendor set to "Techniix", queued) and GitHub Pages on RengaPorts' `packages` branch (the maintainer's step); FFmpeg with Vulkan is a RengaPorts recipe (`07b61ff`) that the maintainer builds with HaikuPorter on a Renga machine; vulkan_loader is not yet in the image. Next, per the newest work order: the pointer and packaging fix, then Renguin (F-27), then the GPU stage. No helper is running at hand-over; start one for the pointer fix, then one for Renguin.

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
12. Installer wizard `F-23`, partition manager `F-24`, graphical themed boot menu `F-22`, and Renga Depot (maintainer, 2026-11-21: HaikuDepot renamed and refaced, stock-compatible, one catalogue over every source with badges and a choose-source menu, richer app pages, Update all with scheduled updates and restore points, repository manager, developer side, pkgman multi-source search, every feature switchable; held off: curated front page, signed sources; roadmap decisions log).

Later: `F-44`, `F-53`, `F-47`/`F-35`.

## Batches 2-10: continue from here (finished)

Moved verbatim to [docs/history/handover-batches.md](history/handover-batches.md); the newest two batches stay below.


## Batch 11: continue from here

Batch 11 (2026-11-21) is finished. Commits: Renga Depot decision (`f3b1ae0c`), build.txt rule (`ea40ff10`), harness health (`21f149ec`), audio routing (`35ce7b3b`), gamepad configurator (`6a76247d`), Renguin Open with / posix_spawn / dual-stack / HiDPI (`de1f4da4`), Welcome paragraphs sized from the font (`f67c662d`), usb_hid hidraw policy for the 8BitDo keyboard receiver (`982546cc`), Wayland resize/maximize/scale 1.0 (`238208d5`), Renguin normal user / hard links as copies / mincore / Chromium flags / Linux fault log (`fba02669`), Control Panel menu widths and audio pages read when shown (`1a1c7679`), the harness counting a Linux program's logged fault as handled (`e8c601bd`), roadmap and partner notes. Mapping a file beyond its end for Linux programs was tried and reverted (VMCache commitment panic).

- **Sweep** (`ce-gates.sh batch11 "" "--smp 2" "--firmware uefi" "--profile renga-raw"`, tree `e8c601bd`): default 1382, --smp 2 1382, --firmware uefi 1378 checks, all pass; renga-raw 1905 checks, 3 failed: `linux_apps.open_with`, `linux_apps.appimage` (open Renguin items), `sound_audio.default_output_back` (audio timing); selftest has its known clean-tree failure. Result directories 20261121-143211, -145449, -150652, -151935 (large captures and disk images removed).
- **Image:** `2026.11.21-batch11` on renga-builds (`fecda26`), `renga-2026.11.21-batch11-anyboot.iso`, Renga `e8c601bd` (hrev60381+427), sha256 `0d836c226dcb6eeb8d5d8c4f59fa1f314bf4dd2d0937d7e828d1531ecefad4bd`; publish with Actions → "Publish build" → `2026.11.21-batch11`.
- **Batch 12** gets the Renguin display-scale setting the maintainer asked for at the end of batch 11 (not started).

Open, queued by the coordinator in batch 11 and not done: the large-font UI audit of every Renga window with a harness mode (Control Panel sidebar width, resizable window, minimum size; only the Windows page menus done); Linux Chromium zygote and separate GPU process (flags work around it); the initial-size mismatch of Linux Chromium's window (needs a hardware retest with the scale fix); several inputs at once (no buffers reach the mixer); AppImage through the roster; Open with copy flaky; ptrace, sysfs/udev, V4L2, Debian/Arch roots, Renguin badge on window tab/list; Chrome 10 of 10; system bus socket in the Linux root.

## Batch 12: continue from here

**Local session, 2026-11-21 (first under the hybrid workflow; branch `fix/linux-chromium`, PR to `ce/main`).** Done, QEMU Tested: hardware items 1-5 below (`02a7d936`, `02f9edc2`, `20d215eb`, `23dcea60`, `e12407f5`), plus a kernel fork panic with `B_FULL_LOCK` areas (`b2f10df0`; upstream bug 67), Renguin termios2 for glibc 2.42+ (`941c56df`), the clang 21 IFUNC test (`2930cf51`), harness KDL cache dump and priority parser (`0f05fb8f`). Decisions recorded: the architect merges (section 3a) and writes the briefs for batch 12's protected paths. Next, in order: (6) per-client Deskbar entries for Linux programs (design: the compositor answers a "list clients" message with each client's name, icon signature and its windows' client tokens, `window_info.client_token`, from `_get_object_token_()`; Deskbar shows one entry per client and filters window menus by those tokens; a Renguin switch); then the Renguin Manager scale controls and Renga Depot from `wip/batch12-depot-renguinscale`, `F-23`, `F-24`, `F-22`, the sweep and the image.

Found in this session, open: windowed Linux Chromium crashed once in QEMU (run 20261121-203345) with SIGSEGV in libxkbcommon 1.13.1 at offset 0xfea1: a parser's buffer pointer, reloaded from its state, was NULL after 67 bytes (exit 191); and the same class in libbrotlidec 1.2.1 at offset 0x60ab (reads 0xa; headless `--single-process`, runs -205401, -220812, and on the old kernel -221500): possibly the `--single-process` segfault seen on hardware; suspect a Renguin memory bug (a mapping or copy-on-write page that reads as missing). Next Renguin investigation. On this WSL host QEMU's PS/2 keyboard is found only at its first key, which is lost (tests that press keys send a harmless key first). `selftest.py` fails "sound settings: clean recordings pass" on HEAD too (pre-existing).

Paused by the maintainer on 2026-11-21 (evening). Committed: Quasar note and R-16 record (`74c31ad5`), large-font audit suite with window fixes (`1f5a9d57`), compositor side of the Renguin display scale (`de6e7d71`). No sweep and no image yet.

Uncommitted in the working tree (`/home/claude/haiku`), file by file:

Preserved on 2026-11-21 on branch `wip/batch12-depot-renguinscale` (`c45a3b04`, unfinished and untested); `ce/main` does not contain it. Continue there or cherry-pick from it onto a `batch/12-…` branch.

- **Renguin Manager display scale and large-font fit** (builds; partly tested):
  - `src/apps/renguinmanager/RenguinView.cpp`, `RenguinView.h`: Settings tab "Display scale for Linux programs" menu (100% off to 300%), Programs tab per-program "Display scale" (saved with Apply, keyed by the Exec line's program name), writes `~/config/settings/Renga/renguin_display_scale` and sends 'RgSc' to the compositor; `SetDisplayScale()`, `SetProgramDisplayScale()`, `DisplayScale()`, `ProgramOfExec()`. Notes changed to `BPrivate::WrappingNote`.
  - `src/apps/renguinmanager/RenguinManager.cpp`: window wrapped with `fit_to_screen()`/`size_window_to_screen()`; new `--display-scale [<S> [<program>]]` command line.
  - `src/apps/renguinmanager/Jamfile`: `UsePrivateHeaders shared`.
  - `3rdparty/ce/tests/renguinmanager/renguin_manager_test.cpp`: new `renguin_manager.display_scale` check.
  - State: the window menus exist (test sees them). Remaining: (1) `--display-scale` fails: the CLI's BApplication uses the signature `application/x-vnd.Renga-RenguinManager-cli`, which does not match the resource signature, so the program stops ("Signature in rsrc doesn't match constructor arg"); `--status` has the same pattern, so check how it avoids that (or use the app signature without launching a window); (2) the large-font audit still flags `programNote`, `processNote` (WrappingNote placed in a vertical group directly: its explicit min 892x30 is set before the width is known; the measured height (60) is not applied while the tab is hidden) — give WrappingNote a min width from the font only and re-measure in `AllAttached`/on show, or keep those two as BStringView with truncation; (3) rerun `ui_large_font_test`, `renguin_manager_test`, `control_panel_ui_test`.
- **Renga Depot, first part** (builds not checked, untested):
  - `src/apps/haikudepot/HaikuDepotConstants.h` (`RENGA_DEPOT_NAME`), `ui/App.cpp`, `ui/MainWindow.cpp`, `ui/ShuttingDownWindow.cpp`, `ui/UserUsageConditionsWindow.cpp`: shown name "Renga Depot" (file name, signature, settings and server protocol unchanged); `MainWindow.cpp` also "Update all…" (Terminal with `pkgman full-sync`; the package daemon keeps the previous state as a restore point).
  - `ui/PackageListView.cpp`: the Repository column is "Source" with labels Haiku / HaikuPorts / RengaPorts.
  - `build/jam/packages/Haiku`: Deskbar link for HaikuDepot named "Renga Depot".
  - Remaining: build (`jam -q HaikuDepot`), a test (window title, Update all item, Source column), then the rest of Renga Depot per the decisions log (one catalogue with apk/Flatpak, choose-source menu, richer pages, scheduled updates, repository manager with priorities, developer side, pkgman multi-source search, switches, HVIF icon).

Open, not started in batch 12: Linux Chrome leftovers (SIGBUS when a mapped shared-memory file grows: the batch 11 beyond-end mapping panicked in VMCache commitment and was reverted; zygote and GPU process error 1002), installer wizard `F-23`, partition manager `F-24`, graphical boot menu `F-22`, batch 11 carry-overs. The compositor's scale does not yet convert cursor surfaces, pointer-constraint regions or text-input rectangles.

Known test state (QEMU Tested, partial runs): `large_font` passes for Control Panel pages except `control_panel_renguin` (Renguin fix uncommitted) and one `control_panel_time` run without a report (not reproduced); `renguin_manager` window too big until the uncommitted fix lands. `linux_apps.appimage` fails as in batch 11. This session's result directories: 20261121-163726 onward (raw captures removed).

Hardware results on the batch 11 image (maintainer, 2026-11-21 evening), first in the next session's queue:
- Confirmed working: `apk add chromium` ends "OK" (hard links as copies); the Linux shell runs as `user`; Linux Chromium content fills its window and follows resize (`238208d5`).
- Open, Linux windows and Chromium (items 1-5 done on 2026-11-21, see the top of this section; 6 next):
  1. Plain `chromium` as the normal user dies in the sandbox (`sandbox/linux/services/credentials.cc:131 Check failed: Function not implemented`): the batch 11 flags (`/etc/chromium/renguin.conf`) are not read by Alpine's launcher. Write them where the launcher reads them; add a test that starts plain `chromium`.
  2. Without `--single-process`, Chromium exits silently with no window (zygote and GPU process, error 1002).
  3. As the normal user, Chromium reaches the session bus but its calls time out ("Did not receive a reply"); `/run/dbus/system_bus_socket` is missing.
  4. Linux windows lack Renga's title bar and borders: add xdg-decoration (server-side) to the compositor (F-27 "First-class Linux windows").
  5. Linux programs installed with apk do not appear in Deskbar > Applications until login or a Renguin Manager refresh: refresh automatically after apk changes.
  6. Running Linux programs do not appear in the Deskbar task list (all windows belong to the compositor's team): give each Linux client its own Deskbar entry and icon.

Next steps: finish the Renguin Manager items above, commit; build and test Renga Depot's first part; then the four-way sweep and the image `build/<date>-batch12` (build.txt starts with `iso: renga-<name>-anyboot.iso`).

**2026-11-26 (cloud session):** the Renguin Manager items and Renga Depot's first part above are done on `batch/12-renguinscale-depot` (QEMU Tested; `renguin_manager.*`, `large_font.*`, `renga_depot.*`); the WIP branch is superseded. The stale branch `batch/12-depot-renguinscale` (same content, one commit with a wrong message) could not be deleted from the cloud host: delete it. Remaining Renga Depot work: one catalogue with apk/Flatpak, choose-source menu, richer pages, scheduled updates, repository manager with priorities, developer side, pkgman multi-source search, switches, HVIF icon.
