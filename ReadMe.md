Renga
=====

Renga is a desktop operating system based on [Haiku](https://www.haiku-os.org),
developed on the `ce/main` branch of this repository. It keeps Haiku's
character (one coherent desktop, fast start, a responsive interface, the BeOS
heritage) and adds what a daily-use desktop needs today: broader hardware
support, Linux and POSIX interfaces for ports, a modern media stack, a
graphical boot menu with recovery, and more customisation that stays easy to
find.

Renga is not the official Haiku distribution, which is available at
https://www.haiku-os.org. Haiku® and the HAIKU logo® are registered trademarks
of Haiku, Inc. and are developed by the Haiku Project; Renga uses neither its
name nor its logo for itself.

Status
------

Renga is in active development and has not had a public release. The
authoritative status of every component is the feature register in
[`RENGA_MASTER_ROADMAP.md`](RENGA_MASTER_ROADMAP.md); this file only
summarises it.

Testing levels are reported separately and never mixed:

* **QEMU Tested**: passes its automated checks in the QEMU test harness
  (`3rdparty/ce/harness`), booted under BIOS and UEFI, from disk, CD and
  virtio-blk.
* **Hardware Tested**: additionally verified on real hardware, with the
  machine and result recorded in the roadmap.

Most of the work below is QEMU Tested. Items verified on real hardware so far
include the boot menu's entry for another installed system (Windows), the
installer's in-place upgrade, and MediaTek MT7922 Wi-Fi through LinuxKPI.

What Renga adds
---------------

**Desktop**

* Snap layouts: hold Shift while dragging a window to see the zones and drop
  it into one; any rows × columns layout, custom layouts from a zone editor,
  spanning, gaps, per-screen and per-workspace layouts, keyboard shortcuts,
  per-application rules and saved arrangements.
* Visible workspaces: a switcher with previews and a "+" in the Deskbar,
  workspace names, an overview, saved sets, and "Move to workspace" in the
  window menu (right-click a window's tab).
* A Control Panel with every setting in one window and a search across all
  pages, a first-login welcome, and Ctrl as the default shortcut key (Alt
  remains available).
* Several screens through the accelerant multi-head interface, with
  arrangement, primary screen and mirroring.

**Hardware**

* LinuxKPI: a layer that runs Linux kernel drivers in Renga's kernel,
  starting with MediaTek MT7921/MT7922 Wi-Fi, with a manager application.
* Audio: per-application volume and routing, low-latency mode, high-quality
  resampling and clock-drift correction, USB Audio Class 1 and 2, HDMI and
  DisplayPort audio, `virtio_sound` playback and capture.
* Gamepads (SDL3 based, with a configurator), pointer handling for 1000 Hz
  mice, NVMe namespaces, `virtio_block` improvements.
* The default display drivers start in the monitor's native resolution
  (EDID or the firmware's mode) and copy only changed pixels to the screen.

**Compatibility and ports**

* Linux interfaces for ports: `epoll`, `inotify`, `signalfd`, `eventfd`,
  `timerfd`, `memfd_create`, POSIX message queues, System V shared memory and
  more.
* Renguin, a Linux binary compatibility layer (in development).
* Readiness work for a native Chromium port and Wayland clients.
* Screen capture with per-application permission, used by the OBS Studio
  port.

**Boot and recovery**

* A boot menu that finds other installed systems, counts failed starts and
  offers restore points made before updates, with safety checks so the boot
  loader only ever writes its own state file.

Related repositories
--------------------

* [Techniix/renga-builds](https://github.com/Techniix/renga-builds): test
  images of Renga.
* [Techniix/RengaPorts](https://github.com/Techniix/RengaPorts): ports and
  packages built for Renga.
* [Techniix/nvidia-haiku](https://github.com/Techniix/nvidia-haiku): the
  Quasar NVIDIA driver (Vulkan through Mesa NVK, OpenGL through Zink).
* [Techniix/LinuxKPI_HaikuOS](https://github.com/Techniix/LinuxKPI_HaikuOS),
  [Techniix/Renguin](https://github.com/Techniix/Renguin),
  [Techniix/Chromium-HaikuOS](https://github.com/Techniix/Chromium-HaikuOS):
  sister projects.

Interfaces that ports can rely on are listed in
[`3rdparty/ce/PORTING.md`](3rdparty/ce/PORTING.md).

Building
--------

Renga builds like Haiku: see [`ReadMe.Compiling.md`](ReadMe.Compiling.md)
for the cross-tools and `configure`. Renga's own tooling (build profiles,
the test harness, image builds) is described in
[`3rdparty/ce/README.md`](3rdparty/ce/README.md). In short, from a configured
tree:

```sh
3rdparty/ce/harness/ce-test.py            # build an image, boot it in QEMU, report
3rdparty/ce/build/build-handover-image renga-anyboot   # a test image
```

Branches
--------

* `ce/main`: Renga.
* `master`: identical to upstream Haiku, kept for reference and merges.

Development
-----------

Renga's changes are developed with AI assistance and stay in this fork; they
are not submitted to Haiku, whose project does not accept AI-assisted
contributions. Bugs found in Haiku itself are recorded in the roadmap
(section 10.2) and reported upstream by the maintainer in his own words.

License
-------

Renga is distributed under the terms of the MIT License, as Haiku is; see
[`License.md`](License.md). Third-party components keep their own licenses.
