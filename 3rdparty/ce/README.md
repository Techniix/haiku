# Renga: test harness and fork tooling

Renga is an operating system based on Haiku®, an open source operating
system. It is not the official Haiku distribution, which is available at
https://www.haiku-os.org. Haiku® and the HAIKU logo® are registered
trademarks of Haiku, Inc. and are developed by the Haiku Project; Renga uses
neither its name nor its logo for itself.

This directory holds tooling specific to the Renga fork.
Upstream's `3rdparty/Jamfile` picks it up only when the build is run with
`HAIKU_INCLUDE_3RDPARTY=1`, so ordinary builds are unaffected. No upstream
build file had to change for any of it.

## Layout

| Path | Purpose |
| --- | --- |
| `harness/ce-test.py` | Host-side harness: build, boot in QEMU, collect and judge results |
| `harness/selftest.py` | Tests the harness's own analysis with known-good and known-bad input |
| `harness/guest/runner.sh`, `harness/guest/ce-harness` | In-guest runner and the launch_daemon job that starts it (harness images only) |
| `tests/<suite>/` | In-guest test programs, one directory per suite |

## Running

From the root of the Haiku tree, after `configure` has created
`generated.x86_64`:

```sh
3rdparty/ce/harness/ce-test.py --compile-only   # seconds: compile test targets
3rdparty/ce/harness/ce-test.py                  # build image, boot, report
3rdparty/ce/harness/ce-test.py --no-build       # boot the last image again
3rdparty/ce/harness/ce-test.py --verbose        # drivers built with TRACE() on
3rdparty/ce/harness/ce-test.py --screenshots 2,4,8   # save the screen then
3rdparty/ce/harness/ce-test.py --stall 45       # quicker hang detection
3rdparty/ce/harness/ce-test.py --smp 4          # several CPUs
3rdparty/ce/harness/ce-test.py --firmware uefi  # OVMF, anyboot image
3rdparty/ce/harness/ce-test.py --media cdrom    # anyboot as a CD (El Torito)
3rdparty/ce/harness/ce-test.py --firmware uefi --two-installs   # pick the right disk
3rdparty/ce/harness/ce-test.py --boot-menu-timeout 2   # boot menu countdown
3rdparty/ce/harness/ce-test.py --usb-controller ohci    # USB tablet on OHCI
3rdparty/ce/harness/ce-test.py --pointer-devices virtio # only the virtio tablet
3rdparty/ce/harness/ce-test.py --only epoll,signalfd  # quick, PARTIAL run
3rdparty/ce/harness/ce-test.py --list-tests     # test programs in the image
3rdparty/ce/harness/selftest.py                 # check the harness itself
```

`--only <tests>` is for quick checks while developing. It boots a copy of
the image, changed with `bfs_shell` to carry a selection file
(`system/non-packaged/data/ce-harness/only`); the guest runner then runs only
the named test programs. Names are those `--list-tests` prints, with or
without `_test`. The QEMU machine, its devices and the image's other files
stay exactly as in a full run, so a test behaves the same either way. Host
checks of programs that did not run (the audio recording of `vsnd_test`, the
scratch disks of `vblk_test` and `nvme_test`, the pointer devices of
`pointer_test`) are skipped. The verdict reads **PARTIAL** and the exit
status is 2 when the selection passed (1 when it failed), never 0: a partial
run does not count. Before a commit, the full runs are still required
(default, `--smp 2`, `--firmware uefi`).

Every run, full or partial, checks `harness.tests_ran`: every program that
should have run (all of them, or the selection) printed its start and exit.
A program missing from the image, or one that never returns, fails it
instead of only showing as fewer checks. The **Timing** section of
`summary.md` lists the seconds from QEMU start to the runner, the runner's
total, and each program's run time and exit status, slowest first, taken
from `serial.timed.log`.

`--firmware uefi` and `--media cdrom` switch the image to `minimum-anyboot`
(only anyboot images have an EFI system partition and a CD boot catalog);
`--profile` overrides that. The four combinations cover every way a release
image boots: BIOS from disk or USB (MBR), BIOS from CD (the El Torito floppy
image), and UEFI from either. UEFI runs use a fresh copy of the OVMF variable
store each time.

`--two-installs` (UEFI, hard disk only) checks that the EFI boot loader boots
the install on the disk the firmware started it from. The harness attaches a
second copy of the image in front of the test image, first on the bus and
first in the boot order. The copy, the decoy, has its BFS volume renamed to
"Decoy", and its EFI system partition entry and El Torito boot record
removed, like an install made for BIOS booting: OVMF cannot start it, goes
on to the test image, but still lists the decoy's disk first. The check
`boot.two_installs` passes if the guest's `/boot` is the test image's volume.
A loader that takes the first disk in firmware order boots "Decoy" instead.
Works with `--disk-bus virtio` too.

`--boot-menu-timeout <seconds>` boots a copy of the image whose kernel
settings file (`home/config/settings/kernel/drivers/kernel`, written with the
build's `bfs_shell`) sets `boot_menu_timeout`. The boot loader then shows its
menu, counts down and continues booting on its own. The boot loader logs the
countdown ("boot menu: ..." lines) on the serial console; the `boot.menu`
check wants the configured start, one line per second and the timeout, after
about the configured time. Every other run checks that no boot menu appears.
Works with either firmware.

Every run attaches two absolute pointing devices next to QEMU's PS/2 mouse
for the `pointer` suite: a USB tablet (`usb-tablet`, on a `qemu-xhci`
controller unless `--usb-controller ehci|uhci|ohci` picks another) and a
virtio tablet (`virtio-tablet-pci`). `--pointer-devices usb`, `virtio` or
`none` attaches fewer. The USB tablet was also checked on `ehci`, `uhci`
and `ohci`. For each attached device, `host.pointer_<device>_tested`
fails if the guest did not test it, and `host.pointer_requests` fails if QEMU
did not carry out a pointer request.

Every run also checks the branding: `branding.boot_loader` wants "Welcome to
the Renga boot loader!" on the serial console, and for anyboot images
`branding.iso_volume_id` reads the ISO 9660 volume ID with `xorriso
-pvd_info` on the host and wants `renga-<version>-<arch>`.

Each run writes to `generated.x86_64/ce-test-results/<timestamp>/`, and
`ce-test-results/latest` always points to the newest run:

| File | Contents |
| --- | --- |
| `summary.md` | Verdict, every check, audio segments, kernel problems, driver log |
| `results.json` | The same data, for scripts |
| `serial.log` | Full serial console of the guest (kernel log + test output) |
| `serial.timed.log` | The same, each line prefixed with host seconds since boot |
| `qemu.log`, `qemu.stderr.log` | QEMU diagnostics, including guest errors its device models detect |
| `build.log` | Full build output |
| `audio.wav` | What the guest played on the virtio sound card |

The exit status is 0 only for a full run in which the build succeeded, the guest ran to the end
without a kernel crash, and every check passed.

## How a run works

1. `jam` compiles the test targets first, for fast feedback, then builds the
   image (`@minimum-raw` by default) with `CE_HARNESS=1`. That variable makes
   `3rdparty/ce` add the test programs and the runner to the image.
2. QEMU boots the image headless with a snapshot disk, the serial console on
   a pipe, and a virtio sound card that records into `audio.wav`.
3. Once the boot volumes are mounted, a system launch_daemon job starts the
   runner (no desktop session needed). It
   writes all output to `/dev/dprintf`, which goes to the serial console, and
   powers the guest off when done.
4. If the kernel drops into its debugger, the harness asks for a backtrace
   and the thread list, then stops the VM and puts that output in the report.
5. The harness parses `CE-TEST <name> PASS|FAIL <detail>` lines, analyzes the
   recorded audio, and writes the report.

## Debugging aids

- **Independent checks.** Besides the tests' own results, a run fails if a
  watched driver printed an error (its `ERROR()` lines), if QEMU's device
  models logged a guest error for the device under test (e.g. a malformed
  virtio request), or if the kernel panicked.
- **`--verbose`** compiles the drivers listed in `TRACE_SOURCES` with their
  `TRACE()` output enabled (`jam -sCE_TRACE=1`; the drivers' Jamfiles define
  the trace macro then). The next normal run rebuilds them without it.
- **`serial.timed.log`** shows when each line arrived, for timing problems,
  without any change to the guest.
- **Kernel debugger.** When the guest drops into KDL (a panic), the harness
  runs a debugger session: `bt`, `threads`, `mutex <address>` for every
  mutex a thread waits on (it names the holder), `bt` for every waiting,
  running or ready thread and every mutex holder, and `running`. The whole
  session is saved as `kdl.log`; `summary.md` shows its start.
- **Hangs.** With no serial output for `--stall` seconds (default 90), the
  harness presses Alt+SysRq+D through the QEMU monitor socket, Haiku's
  emergency key for the kernel debugger, and runs the same session, so a
  deadlock comes with every backtrace and lock holder. The run is reported
  as `stall-kernel-debugger` (or `stall` if KDL does not answer, which means
  interrupts are off).
- **Screenshots.** `--screenshots` saves `screen-<t>s.png` at the given
  seconds after QEMU starts, through the monitor's `screendump`: the boot
  splash, or any UI a test brings up.

## Writing a test suite

A suite is a program in `tests/<suite>/` that prints one line per check:

    CE-SUITE <suite> START
    CE-TEST <suite>.<check> PASS|FAIL <short detail>
    CE-SUITE <suite> DONE failures=<n>

Add it to `tests/Jamfile`, install it from its own Jamfile under
`if $(CE_HARNESS)`, and list it in `harness/guest/runner.sh`. Checks that
need host-side evidence, like the audio analysis, go in `ce-test.py`. Each
should get a known-bad case in `selftest.py`, so a broken check can't pass
silently.

## Renga images

`jam -q @renga-raw` (or `@renga-anyboot`, `@renga-vmware`) builds a full
Renga desktop image: the regular image plus the volume label "Renga", the
Techniix package repository and `HAIKU_RENGA_EXTRA_PACKAGES`. The profiles
are defined in `build/RengaBuildProfiles`; Jamrules includes that file. To
test one:

```sh
3rdparty/ce/harness/ce-test.py --profile renga-raw
```

The harness then also checks the volume name and the repositories, and
that Techniix has a better (lower) priority than HaikuPorts
(`image.repository_priority`, from `pkgman list-repos`).

### Repository priorities

When Techniix and HaikuPorts both offer a package with the same name, the
Techniix one is installed, even if HaikuPorts has a newer version. The
package solver prefers the repository with the lower configured priority
(libsolv gets `-1 - priority`; a better repository beats a higher version).
`0` means "unset", so Techniix (priority 1, from its `repo.info`) cannot go
lower; instead Renga images configure HaikuPorts with priority 2
(`HAIKU_RENGA_HAIKUPORTS_PRIORITY`). Haiku itself stays at 1. The Renga
config is generated by `3rdparty/ce/Jamfile` from a copy of HaikuPorts'
repository info into `objects/.../packaging/repositories/renga/`; the
minimum images keep upstream's.

Upstream's package kit read the priority only from a repository's cache
(the server's own `repo.info`), so a configured priority was shown by
`pkgman list-repos` but ignored when installing.
`BPackageManager::_AddRemoteRepository` now uses the configured priority
when it is set; this is what pkgman and HaikuDepot use. The
`pkg_priority` suite tests it in every run.

The `repo.info` of github.com/Techniix/haiku-packages itself still says
priority 1, the same as HaikuPorts' server. That only matters for someone
who adds the repository by hand with `pkgman add-repo` on another system:
the config then gets 1 and ties with HaikuPorts (higher version wins).

## Branding

Haiku, Inc.'s distribution guidelines reserve the name "Haiku" and the HAIKU
logo for the official distribution, so every build that is not
`HAIKU_DISTRO_COMPATIBILITY=official` shows itself as Renga, whatever the
profile (the harness's minimum images included):

- The display name is one macro, `DISTRO_NAME` in
  `headers/private/shared/distro_name.h` ("Renga", or "Haiku" in official
  builds). The boot loader (welcome line, menu title, help texts),
  AboutSystem (window title, "Renga hrev...", a note in the credits that
  Renga is based on Haiku), the Deskbar menu's "About Renga" and the
  Installer's texts use it.
- AboutSystem and the Installer show the Renga wordmark instead of the HAIKU
  logo: `artwork/renga_logo_*.png`, made from the boot splash logo by
  `artwork/make_app_logos_renga.py`.
- CD and anyboot images get the ISO volume ID
  `$(HAIKU_CD_VOLUME_ID_PREFIX)-<version>-<arch>`, `renga-...` by default
  (`build/jam/BuildSetup`).

Everything software uses to recognise the platform stays Haiku: `uname -s`,
`__HAIKU__`, `B_HAIKU_*`, package and repository names, MIME signatures,
paths, and the revision ("Haiku revision: hrev...", the Haiku sources the
system is built from). A string that names the system is a new catalog key
in Renga builds and shows in English until it is translated; FirstBootPrompt
keeps upstream's translated, name-free texts for that reason, since it is
where the user picks a language.

## Shortcut key: Ctrl

Renga builds (every build that is not `HAIKU_DISTRO_COMPATIBILITY=official`)
use Ctrl as the shortcut key, like Windows and Linux; BeOS and Haiku use
Alt. It is Haiku's own "Windows/Linux mode": the keymap puts
`B_COMMAND_KEY` on the Ctrl keys and `B_CONTROL_KEY` on the Alt keys.
`headers/private/shared/ShortcutKeyStyle.h` recognises and sets the two
styles; Renga's default is `kDefaultShortcutKeyStyle` there.

- **Default keymap.** input_server's built-in keymap (US-International,
  used when `~/config/settings/Key_map` does not exist, and by Keymap
  preferences' "Defaults") gets the Ctrl style, so fresh images and new
  installs start with it. The keymap files in `/boot/system/data/Keymaps`
  are unchanged (Alt style).
- **Keymap preferences** shows "Shortcut key: Ctrl (Windows/Linux) /
  Alt (Haiku)" radio buttons under the keyboard instead of the "Switch
  shortcut keys" button. Picking a keyboard layout from the System list
  keeps the shortcut key in use; maps from the User list keep their own.
  Keymaps that assign the keys some other way (the Mac keymaps) disable the
  buttons. FirstBootPrompt's keymap choice also keeps the shortcut key.
- **Terminal.** With Ctrl as the shortcut key, Ctrl+<key> sends the control
  character (Ctrl+C interrupts, Ctrl+D, Ctrl+R, Ctrl+W, Ctrl+Z reach the
  shell) and Ctrl+Shift+<key> runs Terminal's shortcut (Ctrl+Shift+C copy,
  Ctrl+Shift+V paste, Ctrl+Shift+T new tab, Ctrl+Shift+W close tab,
  Ctrl+Shift+N new window, Ctrl+Shift+Q quit, Ctrl+Shift+F find), like the
  Linux terminals. Ctrl+digits, Ctrl+arrows, Ctrl++/- stay shortcuts.
  Upstream Terminal ran the shortcuts on Ctrl+letter in this mode, so
  Ctrl+C copied and never interrupted. Alt+<letter> still sends the control
  character there (Alt is the Control modifier in this style).
- **Elsewhere** the keys follow from the swap: window switcher Alt+Tab
  (Haiku mode: Ctrl+Tab), workspaces Ctrl+F1...F12 (Alt+F1...), previous
  workspace Ctrl+grave accent, input method Ctrl+Space, Ctrl+Q quits
  applications.
  Unchanged: Ctrl+Alt+Del (Team monitor), Ctrl+Alt+Shift+Esc (safe video
  mode), the kernel debugger's Alt+SysRq (raw scan codes in the keyboard
  driver) and the boot loader's keys.

The `shortcut_keys` suite checks it: the active keymap, the saved
`Key_map`, a system keymap converted the way Keymap preferences does it,
and typing through QEMU: the test prints `CE-HOST sendkey <keys>` and the
harness presses them with the monitor's `sendkey` (only modifier+letter,
digit, ret, tab or esc; never sysrq or delete). Ctrl+V must paste into a
BTextView and Alt+V must not; in Terminal, Ctrl+C must interrupt a `sleep`
(a trap records SIGINT) and Ctrl+Shift+V must paste a line the shell reads.

## Mouse integration in virtual machines

With an absolute pointing device the pointer moves into and out of the VM
window freely, is never captured, and lands exactly on the host pointer.
Renga handles these:

| Hypervisor | Setting |
| --- | --- |
| QEMU | `-device qemu-xhci -device usb-tablet` (or `-usb -device usb-tablet`), or `-device virtio-tablet-pci` |
| VirtualBox | Settings > System > Pointing Device: *USB Tablet*, with a USB controller enabled (Settings > USB); the same driver path as QEMU's USB tablet, but not tested in VirtualBox here |
| VMware | a USB controller and its virtual USB mouse (`mouse.vusb.enable = "TRUE"` in the .vmx), which is absolute unless `mouse.vusb.absDisabled = "TRUE"`; not tested here, its HID descriptor may differ from QEMU's |

Not supported: VMware's vmmouse backdoor (QEMU `-device vmmouse`) and
VirtualBox's PS/2 mouse integration, which needs the guest additions'
VMMDev driver.

The USB tablet goes through `usb_hid`'s `TabletProtocolHandler` and
input_server's `tablet` add-on, the virtio tablet through the
`virtio_input` driver and the `virtio` add-on. Both hand input_server
positions from 0 to 1; input_server spreads them over all W pixels of the
screen (0..1 times W, rounded, the last pixel for 1). Hypervisors send pixel
px as px * 32767 / W, so that lands on px again; scaling to W - 1, as before,
put the pointer one pixel left of the host pointer on the right half of the
screen and one above it on the lower half. The virtio add-on takes the axis
ranges from the device's configuration instead of assuming 0..32767.

The `pointer` suite tests it: for each attached device it prints
`CE-HOST pointer <device> <x> <y>` with the device coordinates QEMU's display
sends for a pixel, and the harness sends them through QMP
(`mouse_set` to make the device the active one, then `input-send-event`).
The pointer (`get_mouse()`) has to land exactly on that pixel, for the
corners, the center and points between, and for device coordinates 0 and
32767. A PS/2 move afterwards (`CE-HOST pointer_rel <dx> <dy>`) has to start
from where the tablet left the pointer. After a switch to 800x600 (BIOS
images; the UEFI framebuffer has one mode) the corners are checked again.
`CE-HOST screenshot <name>` saves `screen-<name>.png`; the suite saves one
per device with the pointer at 12.5 %, 87.5 % of the screen. Without a
desktop the suite prints an INFO line and reports nothing.

## Current suites

- **nvme**: one NVMe controller with two namespaces (48 MiB with 512-byte
  blocks, 80 MiB with 4096-byte blocks, each with an MBR partition). Checks
  that both are published (`/dev/disk/nvme/<id>/raw` and
  `/dev/disk/nvme/<id>/ns2/raw`) with the right size and block size, that
  writes to one never reach the other, I/O through the partition devices, an
  unaligned write on the 4K namespace, trim and flush. The harness then
  checks on the host that each backing file holds exactly what was written
  to that namespace and that the trimmed space was freed.
- **pkg_priority**: builds two small package repositories in the guest
  (`package create`, `package_repo create`) that offer the same package,
  with equal priorities in their own `repo.info` but configured priorities
  1 and 2, the preferred one with the older version. `pkgman install -R`,
  answered "no", has to plan the package from the repository with priority
  1. Nothing is installed; the test removes what it made.
- **pointer**: absolute pointing devices, see "Mouse integration" above.
- **posix_compat**: eventfd, timerfd, memfd, POSIX message queues, System V
  shared memory, the libc additions, user contexts, and TCP (keepalive,
  `TCP_INFO`, orderly and abortive close).
- **shortcut_keys**: Ctrl as the shortcut key, see above.
- **virtio_block**: flush and trim on two scratch disks, one offering
  DISCARD and one only WRITE ZEROES; the harness then checks on the host
  that the trimmed space was freed in the image files.
  It also grows the first scratch disk while the guest runs. The media
  changed flag that follows is cleared by whoever reads it first, the test
  or the kernel's once-a-second media checker; the check passes if either
  saw it (the checker logs "Media changed from <disk>").
- **virtio_sound**: drives `/dev/audio/hmulti/virtio_sound/*` with the same
  ioctl sequence and buffer-cycle rules as the multi_audio media add-on. It
  plays three tone phases (initial start, restart after force-stop, and after
  a 44.1 kHz format change) and checks the description, formats, buffer
  layout, buffer cycles, frame counters and real-time pacing. The harness
  then checks that every phase is present in the recording, on the right
  channels, without gaps or corruption.

## Revision tags

The build names itself after the nearest upstream `hrev` tag. The
`Renga hrev tag` workflow (`.github/workflows/ce-hrev-tag.yml`) publishes that
tag automatically on every push to `ce/main`, so a fresh clone of the fork
builds without setting `HAIKU_REVISION`. Fetch it with `git fetch --tags
origin`. If ce/main is rebased onto a newer upstream revision, the next push
publishes the new tag; "Run workflow" on the Actions tab does the same on
demand.
