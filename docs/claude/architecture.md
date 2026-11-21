# Renga component map

Load this to find where a component lives, which suites cover it and which roadmap entry owns its status.
Status is never kept here: it lives in the roadmap's F-entry (linked). Paths are repository-relative;
"P" marks protected paths (`tools/renga/protected-paths`). Suites are directories under
`3rdparty/ce/tests/` (see [testing.md](testing.md)); the authoritative path-to-suite map is
`tools/renga/suite-map`.

Roadmap: `R` = [RENGA_MASTER_ROADMAP.md](../../RENGA_MASTER_ROADMAP.md). Principles: R
[1.2](../../RENGA_MASTER_ROADMAP.md#12-architectural-principles); branch model: R
[1.3](../../RENGA_MASTER_ROADMAP.md#13-branch-and-release-model); upstream sync: R
[Upstream sync](../../RENGA_MASTER_ROADMAP.md#upstream-sync).

## Kernel, boot and system libraries

| Component | Where | Suites | Roadmap |
| --- | --- | --- | --- |
| Kernel (VM, scheduler, signals, events, foreign personalities) | `src/system/kernel/` P | full set | [F-48](../../RENGA_MASTER_ROADMAP.md#f-48-guest-memory-and-vm-performance), [F-49](../../RENGA_MASTER_ROADMAP.md#f-49-futex-style-waiting) |
| epoll, inotify, signalfd, eventfd, timerfd, memfd, mqueue, SysV shm | `src/system/kernel/events/`, `src/system/kernel/posix/`, `src/libs/gnu/`, `headers/posix/` P | posix_compat | [F-15](../../RENGA_MASTER_ROADMAP.md#f-15-epoll-inotify-signalfd), [F-06](../../RENGA_MASTER_ROADMAP.md#f-06-eventfd) to [F-12](../../RENGA_MASTER_ROADMAP.md#f-12-ucontext), [F-52](../../RENGA_MASTER_ROADMAP.md#f-52-linuxposix-api-batch-2) |
| libroot, libnetwork, runtime_loader | `src/system/libroot/`, `src/system/libnetwork/`, `src/system/runtime_loader/` P | posix_compat, full set | [F-11](../../RENGA_MASTER_ROADMAP.md#f-11-libc-additions), [F-46](../../RENGA_MASTER_ROADMAP.md#f-46-haiku-and-beos-program-compatibility) |
| Boot loader, boot menu, boot counting, restore points | `src/system/boot/` P, `src/bin/bootstate/`, `src/preferences/bootmenu/` | bootmenu, bootstate | [F-20](../../RENGA_MASTER_ROADMAP.md#f-20-efi-loader-and-boot-menu), [F-22](../../RENGA_MASTER_ROADMAP.md#f-22-graphical-multiboot-boot-loader) |
| Boot splash, branding | `src/system/kernel/boot_splash*` P, `3rdparty/ce/artwork/`, `headers/private/shared/distro_name.h` | welcome | [F-16](../../RENGA_MASTER_ROADMAP.md#f-16-boot-splash), [F-17](../../RENGA_MASTER_ROADMAP.md#f-17-renga-branding) |
| Kernel debugger, KDL USB keyboard | `src/system/kernel/debug/` P, `src/add-ons/kernel/debugger/` | kdl | [F-59](../../RENGA_MASTER_ROADMAP.md#f-59-real-hardware-feedback-batch-1-2026-11-11) |
| Crash reports | `src/bin/crash_collector/`, `src/apps/systemlogs/` | crashlog | [F-51](../../RENGA_MASTER_ROADMAP.md#f-51-crash-logs-and-system-log-viewer) |
| Profiling and tracing | (in kernel and Renguin trace) | renguin | [F-56](../../RENGA_MASTER_ROADMAP.md#f-56-profiling-and-tracing-tools) |

## Drivers and hardware

| Component | Where | Suites | Roadmap |
| --- | --- | --- | --- |
| virtio sound, block, GPU accelerant | `src/add-ons/kernel/drivers/audio/virtio/` P, `src/add-ons/kernel/drivers/disk/virtual/` P, `src/add-ons/accelerants/virtio/` | virtio_sound, virtio_block, appserver | [F-02](../../RENGA_MASTER_ROADMAP.md#f-02-virtio_sound), [F-03](../../RENGA_MASTER_ROADMAP.md#f-03-virtio_block) |
| NVMe | `src/add-ons/kernel/drivers/disk/nvme/` P | nvme | [F-04](../../RENGA_MASTER_ROADMAP.md#f-04-nvme-multi-namespace) |
| USB, usb_hid, pointer at high rates, VM mouse | `src/add-ons/kernel/busses/usb/`, `src/add-ons/kernel/drivers/input/` P, `src/add-ons/input_server/` | pointer | [F-36](../../RENGA_MASTER_ROADMAP.md#f-36-usb), [F-19](../../RENGA_MASTER_ROADMAP.md#f-19-vm-mouse-integration) |
| Gamepads (evdev core, xinput, uinput, hidraw, BGamepad, preferences) | `src/add-ons/kernel/generic/gamepad/`, `src/add-ons/kernel/drivers/input/` P, `src/kits/device/`, `src/preferences/gamepads/` | gamepad | [F-42](../../RENGA_MASTER_ROADMAP.md#f-42-gamepad-support-and-configurator) |
| USB audio, HDA (ELD), UVC | `src/add-ons/kernel/drivers/audio/usb/`, `.../audio/hda/` P, media add-ons | usb_audio, hda_eld, uvc | [F-31](../../RENGA_MASTER_ROADMAP.md#f-31-media-kit) |
| LinuxKPI (Linux drivers in the kernel), Wi-Fi mt7921e | `src/libs/compat/linuxkpi*` P, `src/add-ons/kernel/drivers/network/wlan/` P, `src/apps/linuxkpimanager/`, `src/bin/lkpictl/` | linuxkpi, wifi | [F-26](../../RENGA_MASTER_ROADMAP.md#f-26-linuxkpi-built-into-renga), [F-34](../../RENGA_MASTER_ROADMAP.md#f-34-wi-fi) |
| Networking (TCP additions, NTP) | `src/add-ons/kernel/network/` | posix_compat, wifi | [F-05](../../RENGA_MASTER_ROADMAP.md#f-05-tcp-additions-keepalive-tcp_info-abortive-close), [F-30](../../RENGA_MASTER_ROADMAP.md#f-30-periodic-ntp-sync), [F-39](../../RENGA_MASTER_ROADMAP.md#f-39-networking-beyond-tcp) |
| GPU stage: syncobj, GpuPresent, multi-head interface (Quasar) | `src/add-ons/kernel/generic/syncobj/`, `src/add-ons/kernel/drivers/common/syncobj_node.cpp` P, `headers/os/add-ons/graphics/` P | syncobj, appserver | [F-35](../../RENGA_MASTER_ROADMAP.md#f-35-gpu-and-graphics), [F-47](../../RENGA_MASTER_ROADMAP.md#f-47-zero-copy-presentation-and-a-haiku-vulkan-surface) |
| Power management | (not started) | — | [F-37](../../RENGA_MASTER_ROADMAP.md#f-37-power-management-and-suspend) |

## Linux and porting layers

| Component | Where | Suites | Roadmap |
| --- | --- | --- | --- |
| Renguin (Linux binary compatibility) | `src/add-ons/kernel/personalities/renguin/` P (`COVERAGE.md`), `src/system/kernel/foreign_personality.cpp` P | renguin | [F-27](../../RENGA_MASTER_ROADMAP.md#f-27-renguin-linux-binary-compatibility) |
| Linux programs integration, Renguin Manager, Linux shell | `src/bin/linux/`, `src/apps/renguinmanager/`, `src/apps/linuxshell/` | renguinmanager, largefont | [F-27](../../RENGA_MASTER_ROADMAP.md#f-27-renguin-linux-binary-compatibility) |
| Wayland compositor and libwayland | `src/servers/wayland/`, `src/libs/wayland/` | wayland | [F-43](../../RENGA_MASTER_ROADMAP.md#f-43-chromium-platform-readiness-native-fully-featured), [F-33](../../RENGA_MASTER_ROADMAP.md#f-33-compositor) |
| D-Bus session bus, Secret Service | `src/servers/dbus/` | dbus | [F-43](../../RENGA_MASTER_ROADMAP.md#f-43-chromium-platform-readiness-native-fully-featured) |
| Port-visible interfaces | [3rdparty/ce/PORTING.md](../../3rdparty/ce/PORTING.md) | — | [F-43](../../RENGA_MASTER_ROADMAP.md#f-43-chromium-platform-readiness-native-fully-featured), [F-58](../../RENGA_MASTER_ROADMAP.md#f-58-partner-requests-batch-1-obs-studio-and-quasar) |
| Toolchain, development packages | `src/data/package_infos/`, `build/jam/` P | devel, toolchain | [F-45](../../RENGA_MASTER_ROADMAP.md#f-45-rengaports-separate-ports-and-package-repository-for-renga) |

## Desktop and applications

| Component | Where | Suites | Roadmap |
| --- | --- | --- | --- |
| app_server: screen capture, snap layouts (Lumina), several screens, HiDPI | `src/servers/app/` P | appserver, largefont, full set | [F-25](../../RENGA_MASTER_ROADMAP.md#f-25-lumina-shell-lumina_haikuos), [F-38](../../RENGA_MASTER_ROADMAP.md#f-38-multi-monitor-and-hidpi), [F-55](../../RENGA_MASTER_ROADMAP.md#f-55-colour-emoji-in-app_server) |
| Control Panel | `src/apps/controlpanel/`, `headers/private/shared/control_panel_index.h` | controlpanel, largefont | [F-28](../../RENGA_MASTER_ROADMAP.md#f-28-control-panel-suite) |
| Preferences (Audio, Media, Screen, Input, Screen recording, ...) | `src/preferences/` | per area, controlpanel, largefont | [F-60](../../RENGA_MASTER_ROADMAP.md#f-60-existing-applications-and-preferences-review-and-renga-refresh), [F-32](../../RENGA_MASTER_ROADMAP.md#f-32-audio-settings-redesign) |
| Deskbar | `src/apps/deskbar/` | deskbar, largefont | [F-41](../../RENGA_MASTER_ROADMAP.md#f-41-deskbar-modernisation) |
| Tracker | `src/kits/tracker/` | tracker, largefont | [F-66](../../RENGA_MASTER_ROADMAP.md#f-66-tracker-modernisation) |
| Shortcut keys (Ctrl default) | `src/servers/input/`, `src/preferences/keymap/`, `src/apps/terminal/` | shortcut_keys | [F-14](../../RENGA_MASTER_ROADMAP.md#f-14-shortcut-keys-ctrl-default) |
| Welcome, Renga Doctor, background agents, notifications | `src/apps/welcome/`, `src/apps/rengadoctor/`, `src/servers/notification/` | welcome, rengadoctor, agents | [F-68](../../RENGA_MASTER_ROADMAP.md#f-68-first-public-release-readiness), [F-44](../../RENGA_MASTER_ROADMAP.md#f-44-bundled-applications-gap-review-2026-11-09) |
| Installer, DriveSetup, partition manager | `src/apps/installer/`, `src/apps/drivesetup/` | installer, storage | [F-23](../../RENGA_MASTER_ROADMAP.md#f-23-installer-redesign), [F-24](../../RENGA_MASTER_ROADMAP.md#f-24-advanced-partition-manager) |
| Renga Depot (HaikuDepot), package solver, repository priority | `src/apps/haikudepot/`, `src/kits/package/`, `3rdparty/ce/repositories/` | package_priority | [F-13](../../RENGA_MASTER_ROADMAP.md#f-13-package-repository-priority-techniix-over-haikuports), decisions log 2026-11-21 |
| WebPositive, HaikuWebKit | `src/apps/webpositive/` (HaikuWebKit in RengaPorts) | webpositive | [F-57](../../RENGA_MASTER_ROADMAP.md#f-57-webkit-keep-up-haikuwebkit-rebase-and-webpositive-quick-fixes) |
| Media Kit, audio routing, capture, codecs, streaming | `src/kits/media/`, `src/servers/media/`, `src/servers/media_addon/`, `src/add-ons/media/` | media | [F-31](../../RENGA_MASTER_ROADMAP.md#f-31-media-kit), [F-63](../../RENGA_MASTER_ROADMAP.md#f-63-media-graph-made-visible-patchbay-and-linux-audio-as-nodes) |
| Themes and icons | `3rdparty/ce/tools/`, `*.rdef` icons | largefont | [F-40](../../RENGA_MASTER_ROADMAP.md#f-40-visual-identity-pack-themes) |
| Responsiveness under load | (cross-cutting) | stress | [F-61](../../RENGA_MASTER_ROADMAP.md#f-61-responsiveness-under-load-the-beos-demo-as-a-release-gate) |

## Build, test and release infrastructure

| Component | Where | Roadmap |
| --- | --- | --- |
| Harness (ce-test.py, ce-gates.sh, selftest.py, guest runner) | `3rdparty/ce/harness/` P (core), `3rdparty/ce/tests/` | [F-01](../../RENGA_MASTER_ROADMAP.md#f-01-test-harness), [F-67](../../RENGA_MASTER_ROADMAP.md#f-67-renga-quality-engineering-test-station-bisect-warnings-fuzzing-stress-tests) |
| Build profiles, images, hand-over images | `3rdparty/ce/build/`, `build/jam/` P | [F-18](../../RENGA_MASTER_ROADMAP.md#f-18-build-profiles-and-images) |
| hrev tags, branch model | `.github/workflows/ce-hrev-tag.yml` | [F-21](../../RENGA_MASTER_ROADMAP.md#f-21-hrev-tag-workflow-and-branch-model) |
| Hybrid workflow scripts, PR checks | `tools/renga/`, `.github/workflows/renga-pr-checks.yml` | [docs/HYBRID-WORKFLOW.md](../HYBRID-WORKFLOW.md) |
| Upstream fixes carried by Renga | throughout | [F-29](../../RENGA_MASTER_ROADMAP.md#f-29-upstream-bug-fixes-carried-by-renga), R [10.2](../../RENGA_MASTER_ROADMAP.md#102-upstream-haiku-bugs-found-and-fixed-in-renga) |
