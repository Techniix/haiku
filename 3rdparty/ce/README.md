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
3rdparty/ce/harness/ce-test.py --other-systems  # boot other systems from the menu
3rdparty/ce/harness/ce-test.py --installer-upgrade  # upgrade an old install in place
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

### Other operating systems in the boot menu

`--other-systems` (BIOS or `--firmware uefi`, hard disk only) tests the
boot menu's other systems (roadmap F-22). The harness builds test media in
the results directory (with `nasm` and mtools) and attaches them:

- a second disk (BIOS drive 0x81) with an MBR whose boot code is a test
  record, an EFI system partition holding `\EFI\Microsoft\Boot\bootmgfw.efi`,
  `bootmgr.efi`, `\EFI\ubuntu\shimx64.efi`, `grubx64.efi`, `mmx64.efi` and
  `\EFI\BOOT\BOOTX64.EFI`, an active partition whose boot record looks like
  Windows' (NTFS OEM name, "BOOTMGR") and an inactive one with the same
  record (a data volume);
- a USB drive (throttled) with `\EFI\BOOT\BOOTX64.EFI` and an empty CD
  drive; on UEFI a slow, empty second USB drive (QEMU's `null-co` with a
  400 ms delay per read), on BIOS a bootable floppy in A: and a floppy
  drive B: without a disk. (A delayed floppy B: was tried first: keys sent
  while it was being read never reached the menu. QEMU's floppy controller
  reads synchronously; the cause was not pinned down further, and real
  hardware is untested.)

The `.efi` files are `harness/otheros/efi_test_app.asm`, a 8 KiB PE32+
application that writes `CE-OTHEROS started <marker>` to the serial port,
says whether its Loaded Image protocol has a device handle, and returns.
The boot records are `harness/otheros/boot_record.asm`: they write
`CE-OTHEROS started <marker> dl=<DL> lba=<start of the DS:SI entry>` and
boot BIOS drive 0x80 (Renga's disk) again, so one QEMU session sees several
boot loader starts.

The image's kernel settings get `boot_menu_timeout 3` (and on UEFI
`boot_menu_default "Windows Boot Manager"`). A `BootMenuDriver` thread
plays the user: it reads the menu's "boot menu:" serial lines and presses
keys through the monitor (UEFI: let the countdown start Windows Boot
Manager, then arrow keys while drivers are still being connected, the
shortcuts of Ubuntu and the USB drive, then `b`; BIOS: the shortcuts of the
Windows partition, the other disk's MBR and the floppy in three successive
boot loader starts, an arrow key while the empty floppy drive B: is read,
and the fourth start continues booting). Checks (`other_systems.*`,
`other_media.*`): the right entries and names (shim over GRUB, no MOK
manager, no fallback copy, no inactive NTFS volume, no EFI system partition
on BIOS), the default started by the countdown, the started loader got its
device handle, the menu came back after a loader returned, the boot records
got DL and DS:SI, the removable media were found in the background and
started, the empty CD drive answered within 3 s, every key was answered
within 1.5 s and no search step took over a second, and Renga booted
afterwards. They replace `boot.menu` in such a run.

### Installing over an existing installation

`--installer-upgrade` (BIOS, hard disk) tests the Installer's in-place
modes (roadmap F-23). The image is built at 1200 MB
(`-sHAIKU_IMAGE_SIZE`), so that an upgrade that keeps the old system fits.
Two copies of it, the volumes renamed "OldHaiku" and "OldHaiku2", are
made to look like an older stock Haiku installation with the build's
`bfs_shell` and host `package` tool: the system package re-packed as
`haiku-r1~beta5_hrev57937-1-x86_64.hpkg` (cached in
`generated.x86_64/ce-installer-cache`), a user-installed package
`ce_userapp-1.0-1-any.hpkg` (`data/ce_userapp/README`), an
`activated-packages` file and an older state `state_2025-01-01_12:00:00`
without the user package. They are attached as IDE disks (written, not
snapshots), and the run boots three times:

1. The test image: `installer_test` adds user files with attributes and
   settings to both (and a line of its own to a settings file the source
   has too), checks `Installer --list-targets`, runs `Installer
   --unattended --target OldHaiku --mode upgrade` and checks the result on
   disk, then prepares OldHaiku for the next boots (a phase file, the
   harness's selection file, and a 5 s boot menu). On OldHaiku2 it runs `--mode replace`, then
   `--mode erase` without and with a wrong `--confirm-erase` (both must be
   refused, nothing changed) and with the right one.
2. OldHaiku itself (`upgraded/`): Renga's system package is the one in use
   (packagefs' `SYS:PACKAGE` attribute of `libroot.so`), the user package
   is active, files, attributes and settings are there.
3. OldHaiku again (`oldstate/`; phase A set `boot_menu_timeout 5` in it,
   and the harness changed its phase file): a `BootMenuDriver` script chooses "Select boot
   volume/state", the volume, the state below "Latest state", Escape,
   `b`; the old system package must be the one in use.

The second and third boot write into their own subdirectories; their
serial logs are appended to `serial.log`, so their `installer.*` checks
count like the others. Host checks: `installer.setup`,
`installer.boot_upgraded`, `installer.boot_oldstate` (each ran to the
end) and `installer.old_state_chosen` (the boot loader logged a package
state other than "Latest state" as chosen). In any other run
`installer_test` finds no OldHaiku volumes and reports nothing.

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
| `audio.wav` | What the guest played on the first virtio sound card |
| `sdl-out.raw` | What it played on the second one (raw s16le, stereo, 48 kHz) |

The exit status is 0 only for a full run in which the build succeeded, the guest ran to the end
without a kernel crash, and every check passed.

## How a run works

1. `jam` compiles the test targets first, for fast feedback, then builds the
   image (`@minimum-raw` by default) with `CE_HARNESS=1`. That variable makes
   `3rdparty/ce` add the test programs and the runner to the image.
2. QEMU boots the image headless with a snapshot disk, the serial console on
   a pipe, and two virtio sound cards: one records into `audio.wav` (QEMU's
   `wav` backend, which has no input), the other runs on QEMU's SDL
   backend with SDL's `disk` driver, which writes its output to
   `sdl-out.raw` and reads its capture input from a file the harness keeps
   filled with a test tone (see "Audio capture" below).
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
- **Host load.** QEMU without KVM runs the guest at the host's mercy. The
  harness samples the host's one-minute load average every 10 s while the
  guest runs and `summary.md` shows the highest and the mean next to the
  host's CPU count, marked when the host was busier than the guest needs
  (another build or QEMU). The timing checks of the Media Kit tests
  (`media.click_free_volume`, `media_latency.*`) fail first on a busy host;
  the line tells such a run from a regression.
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

## Audio capture

The second sound card's capture input is a file the harness writes,
`capture-in.raw` (removed after the run). SDL's disk driver only takes a
regular file, reads it in real time (it sleeps one 10 ms period per read),
starts again at its beginning each time QEMU opens the input voice (on
every `PCM_PREPARE`), and goes silent for good at its end. So the harness
(`AudioFeeder`) keeps the whole file filled with the current tone, a stereo
sine at whole-Hz frequencies, which repeats seamlessly every second, and
keeps it 30 s longer than QEMU has read (its position comes from
`/proc/<qemu>/fdinfo`). A test chooses the tone with
`CE-HOST audio_in <left Hz> <right Hz> <amplitude>`; the harness rewrites
the file from QEMU's read position on, so the new tone arrives after QEMU's
and SDL's buffering, well under half a second. The default is 700/1300 Hz
at amplitude 12000. `results.json` lists every tone written and how far QEMU
read (`boot.capture_feed`).

## USB audio

Every run attaches an xhci controller (`audiousb`) with no devices on it,
and five QEMU `wav` audio backends, `usbsnd0` to `usbsnd4` (`usb_audio_test`
plugs in twice, `media_sound_test` once, `media_usb_test` twice). A test plugs a
QEMU `usb-audio` device in with `CE-HOST usb_audio add` and pulls the one
plugged in last out with `CE-HOST usb_audio remove` (the harness uses the
monitor's `device_add` and `device_del`). The n-th device plugged in plays
into `usb-<n>.wav`, since QEMU closes a backend's file when its device goes
and would start it again on the next plug-in. QEMU's `usb-audio` is a USB
Audio Class 1 device: 48 kHz, 16-bit stereo playback, a feature unit with a
master mute and a volume per channel. QEMU maps that volume linearly
from -128 dB to +8 dB onto 0..255 and plays at that / 255, so 0 dB is
240/255 (`qemu_usb_audio_gain()` in `ce-test.py`). It has no capture, no
other rates and no Audio Class 2 mode (checked in QEMU 8.2's
`hw/usb/dev-audio.c`).

The minimum images get the `usb_audio` driver in
`system/non-packaged/add-ons/kernel/drivers` (the Renga images have it in
the system package), so the driver tests run on every boot path.

Audio Class 2 is tested without hardware by `usb_audio_uac2_test`: the
driver's own sources, built into a userland program against a mock USB
stack (`tests/usb_audio/shim/` stands in for the few kernel headers they
include) with one synthetic high-speed UAC2 device (see "Current
suites").

## Media Kit tests

The harness image keeps media_server from starting at boot (the driver tests
need the sound cards to themselves). After every program in `ce-tests/` has
run, the runner starts media_server by hand and runs the programs in
`ce-tests-media/` (Jamfiles install them with `bin ce-tests-media :`). They
need the Media Kit, which only the `renga-*` images have: on the minimum
images the runner says "media tests skipped" and the harness does not
expect them (`installed_tests()` leaves them out for those profiles). So
the Media Kit checks run with `--profile renga-raw`, which is required for
any desktop or Media Kit change anyway. `--only media` works there too.

`media_test` plays two applications (child processes with their own
signatures, A on the left channel, B on the right, each with a tone of its
own per phase), records through the loopback in the guest, and the harness
finds the same phases in `audio.wav` and `sdl-out.raw` (`MEDIA_PHASES` in
`ce-test.py`): per-application volume and mute, a stream moved to the
second card and back, settings remembered after the applications and the
media services restart, and the default output switching to the second
card. `media_audio.route_click_free` checks that moving a stream to and from
the second card left no step between two samples larger than the tone
itself makes. The mixer's gain values follow its bent "dB" scale
(`mixer_gain()`).

## Crash reports and planned reboots

Programs in `/boot/system/non-packaged/bin/ce-tests-final` (Jamfiles:
`bin ce-tests-final :`) run last, after the media tests, on every image,
with `--crash`: they may end the boot. `crashlog_test` does: it prints
`CE-INFO crashlog: entering the kernel debugger on purpose` and
`CE-HOST kdl_reboot`, then panics through upstream's `kdl` test driver
(added to harness images only). The harness answers that one debugger
entry instead of stopping the run: it waits for the prompt, lets QEMU
reset the machine on a guest reboot (QMP `set-action reboot=reset`; the run
uses `-no-reboot` otherwise), types `reboot`, and turns `-no-reboot` on
again. The debugger output is saved as `kdl-expected-<n>.log`. The test
keeps its phase in `data/ce-harness/crash-phase`; while it exists, the
runner runs only `crashlog_test` (as `running`/`exited`, which
`harness.tests_ran` counts) and powers off. The second boot checks the
saved report and enters the debugger again with `kernel_debugger()`; the
third checks that report.

Between the marker line and the next `CE-HARNESS START`, only the expected
panic and the debugger's welcome are not kernel problems; any other panic
there still is. `crashlog.debugger_reboots` (host) wants both entries
answered and three runner starts. Boots after such a reboot get a stall
limit of at least 240 s (they are slow in QEMU without KVM).
`--other-systems` and `--installer-upgrade` put
`data/ce-harness/no-final-tests` into their image copy, so these programs
do not run there.

## LinuxKPI

LinuxKPI (`src/libs/compat/linuxkpi`, from LinuxKPI_HaikuOS) runs Linux
drivers on Renga; its first driver, `mt7921e`, needs a MediaTek card, and
QEMU has no Wi-Fi device. To test the core anyway, every run attaches QEMU's
educational PCI device, `-device edu,dma_mask=0xffffff` (MMIO registers,
MSI, a DMA engine, a computation that interrupts when done; QEMU
`hw/misc/edu.c`), and harness images carry `lkpi_pcitest`, LinuxKPI_HaikuOS'
test driver written against the Linux API only (it would match MediaTek
cards too, so it is never in release images). Matched against edu, its
probe runs a functional test through LinuxKPI and prints `edu: <check>
PASS|FAIL` lines: MMIO, an MSI interrupt raised on request and one at the
end of a computation, a coherent DMA buffer to the device and back,
`dma_map_single()` of buffers above the device's DMA mask in each direction
(bounced by the DMA API) with `dma_sync_single_for_cpu()`, `timer_list`,
delayed work and an `hrtimer` (each fired, and each cancelled before it
fired). Then it declines the device, so Haiku unloads it again.

`linuxkpi_test` loads it four times by changing the binary's modification
time (devfs reloads updated drivers none of whose devices is open, once a
second; writing a driver name to `/dev` as `src/bin/rescan.c` does is
refused by today's devfs), reads the driver's lines back from the syslog
and reports the edu checks, LinuxKPI's own load-time self-test
(`selftest_*`), that every load ran and the later ones passed too, that the
kernel's area count stays put from the second load on, and that the system
package has `mt7921e`, its firmware and the Manager's data. The host check
`linuxkpi.edu_no_clamp` fails if the device printed `EDU: clamping DMA`
(an address above its mask reached it). At 24 bits every streaming buffer
is above the mask; at edu's default 28 bits none of 64 `kmalloc()` buffers
of a 1 GiB guest was, so the bounce path went untested.

## Current suites

- **installer** (`installer_test`): see "Installing over an existing
  installation"; only with `--installer-upgrade`.
- **installer_ui** (renga-* images, `installer_ui_test`): the harness
  attaches "CETarget", a 64 MiB BFS disk made with `bfs_shell
  --initialize` holding a system package file
  `haiku-r1~beta5_hrev57937-1-x86_64.hpkg` and a user file (mkfs on a
  `diskimage` file in the guest refused to initialize it). The test checks
  `Installer --list-targets`, starts `Installer --target CETarget
  --begin` (no licence window, Begin pressed after the scan), checks after
  10 s that it is still waiting, takes screenshot `installer_modes`
  (`installer_ui.screenshots`; the mode window over the Installer), kills
  it and checks that CETarget is unchanged. (Talking to the Installer
  through messengers and scripting hung the test three times; not
  resolved.)
- **bootmenu** (renga-* images, `bootmenu_ui_test`): the Boot menu
  preferences. From the command line: `--set-timeout 7` writes the line
  and keeps every other line of the kernel settings file, `--set-default`
  and `--show`, `--set-timeout 0` removes the boot menu lines, `--list`
  runs. From the window (the messages its controls send, sent by the test):
  the file gets timeout 4 and the default, and the checkbox and spinner
  read back through scripting show it. Screenshot `bootmenu_preferences`
  (`bootmenu.screenshots`). The settings file is put back afterwards.

- **crashlog** (every image, `crashlog_test`): crash reports and System
  logs (roadmap F-51), over three boots of one QEMU session; see "Crash
  reports and planned reboots". Collector on made-up syslogs, an
  application crash (debug_server's report), the kernel report after a
  panic (reason, message, revision, CPUs with backtraces, memory, kernel
  images, syslog tail) and after `kernel_debugger()`, no report saved
  twice. On renga-* images also: the Deskbar entry, the notification at
  login (notification_server's window is shown), System logs opened on the
  report as a click on the notification opens it, the live log (a line
  logged now appears; component, level and search filters), the previous
  boot, "Save a report" from the window and the command line (the ZIP is
  read back: members, CRC of an inflated entry), and screenshots
  `crash_notification`, `systemlogs_crash`, `systemlogs_live`,
  `systemlogs_previous` (`crashlog.screenshots`).

- **linuxkpi** (every image, `linuxkpi_test`): the LinuxKPI core
  (roadmap F-26), see "LinuxKPI" below. **linuxkpi_ui** (renga-* images,
  `linuxkpi_ui_test`): `lkpictl` (drivers, hardware, status of a driver
  that is not running, coverage, report), LinuxKPI Manager (window, the
  overview's text read through scripting, `--tab` to three tabs on the
  running instance), System logs (FreeBSD/LinuxKPI driver lines filed
  under their driver, `hardware/linuxkpi.txt` in "Save a report");
  screenshots `linuxkpi_overview`, `linuxkpi_drivers`, `linuxkpi_hardware`,
  `linuxkpi_compatibility` (`linuxkpi_ui.screenshots`).

- **media** (renga-* images): see "Media Kit tests". In the guest it checks
  recording from the fed card through the Media Kit and its recording gain,
  the output time source's rate while playing, the stream list with
  signatures, the loopback of everything and of one application, volume
  and mute (also that changing them does not click), routing, settings
  remembered by application and across a media service restart, and the
  default output switch.
- **media_ui** (renga-* images): with two applications playing ("Music"
  and "Browser", copies of media_test's player), one of them quieter and on
  the second card, it opens the Deskbar volume popup (`desklink
  --volume-control`: master volume, the output device menu shared with the
  Audio preferences, the applications, "Audio settings…"), the Media preferences (`Media`, and `Media
  --advanced-audio` with the output menus shown) and SoundRecorder recording
  one application (`SoundRecorder --input "What you hear: Music"`), checks
  each window is up and has the harness save `screen-volume_popup.png`,
  `screen-media_preferences.png`, `screen-media_preferences_advanced.png`
  and `screen-soundrecorder_application.png`. `media_ui.screenshots` (host)
  checks that all four were saved and are not blank.
- **sound_settings** (`media_sound_test`, renga-* images, before
  `media_test`): the Audio preferences (roadmap F-32, `src/preferences/
  audio`). With two players ("Music" on the left, "Browser" on the
  right), every page is shown and saved (`sound_output`, `sound_input`,
  `sound_applications`, `sound_devices`, and with advanced settings,
  after the output switch, the hot-plug and a search: 11 screenshots,
  `sound_settings.screenshots`), and every control is used as a click
  uses it, through the window's `'AuCt'` message (a control's value set
  and invoked, a menu item chosen; `'AuSt'` returns the window's state),
  then checked against what it changes: the master volume slider at -12
  (the mixer's master gain) and Mute, the output device menu (the default
  output and the card the mixer plays on, there and back), "Play a test
  sound" (1250 Hz at 0.25 in a loopback recording of the Audio
  preferences' own stream), the buffer size menu (the card's parameter),
  the resampling menu (advanced; the mixer's algorithm, windowed sinc by
  default),
  an application's volume (loopback level ratio on the mixer's scale) and
  mute, its output menu and "Low latency" switch (advanced), the input
  device menu with the level meter (it must show the fed tone's peak on
  the fed card and nothing on the other), the recording gain (-6 dB halves
  what the input records, and the card back at 0 dB afterwards), the device list (both cards; QEMU's
  `usb-audio` plugged in through `CE-HOST usb_audio add` must appear and,
  after `remove`, leave), the search ("microphone" leads to Input,
  "low latency" to Applications with the advanced settings shown) and the
  command line (`Audio --page applications --advanced`). The harness finds
  the phases in what the cards played (`sound_audio.*`): the master
  volume's level, nothing while muted, the default output on the second
  card and back, Browser routed to the second card while Music stays.
  Everything is put back for `media_test`.
- **media_usb** (renga-* images, last of the audio Media Kit tests): plugs QEMU's
  `usb-audio` in with the media services running. The device must be
  listed and get a node within seconds (no restart). One application is
  routed to it by name, another plays on the default output; then the
  device becomes the default output, is pulled out (both streams fall
  back to the first virtio card, the default output with them, the
  mixer connected again), and plugged in again (the saved default and the
  route return by themselves). Screenshots: `Media --device "QEMU USB
  Audio"` (the device's page with its feature unit controls, format and
  rate) and the Media preferences after the unplug. The harness finds the
  phases (`media_usb_audio.*`) in the last two `usb-<n>.wav` files and
  `audio.wav`, and checks the two screenshots (`media_usb.screenshots`).
- **codycam** (`media_video_test`, renga-* images, after the audio
  Media Kit tests): camera
  selection and preview with the synthetic video source every image
  carries (`video_producer_demo`, "Demo Video Producer"). The video
  sources CodyCam's Camera menu lists (the dormant nodes producing raw
  video), `CodyCam --camera "Demo Video Producer"` (window title, preview
  node fed by that source), a camera switch through the menu's message
  (the preview built again in the same window, the old node released),
  quit. Screenshots `codycam_preview` and `codycam_switched`, checked by
  the harness (`codycam.screenshots`).
- **media_codec** (`media_codec_test`, renga-* images): the encode/decode
  API (`headers/private/media/MediaCodec.h`, roadmap F-31 phase 4) and the
  FFmpeg plug-in's encoders behind it. `MediaWriter` writes two seconds of
  160x120 video at 15 fps (red, green, blue, white quarters) and audio
  (440 Hz left, 660 Hz right); `MediaReader` decodes the file again: the
  tracks with their codecs and sizes, every frame in the right colour,
  the audio's length and tones (Goertzel), and seeking where it works.
  Checked: WebM with VP9 + Opus and VP8 + Opus (float samples), Ogg Opus,
  Matroska with MPEG-4 + FLAC (16-bit and float samples), Ogg Vorbis, MP4
  with MPEG-4 + AAC, MP3, and FLV with Sorenson Spark (`flv1`) + AAC, each
  with a seek to 1 s that must land between 0.5 and 1.1 s;
  `ListCodecs("webm")` must list vp8, vp9, opus and vorbis.
- **video_accel** (`media_hwvideo_test`, renga-* images): the hardware
  video plumbing (roadmap F-31 phase 5,
  `headers/private/media/VideoAcceleration.h`) with the "software"
  reference back end (`software_video_accelerator`, in the harness
  images' non-packaged `add-ons/media/video_accelerators`), which
  implements the interface with FFmpeg in software. The roster loads it
  but does not choose it by itself (it is no hardware), chooses it by
  name (`RENGA_VIDEO_ACCELERATION=software`) and nothing with `off`; its
  capabilities; VP9 encoding through the interface (NV12 surfaces in
  areas, one colour per second, key frames asked for once a second) and
  decoding of those packets back to NV12 surfaces in order and in their
  colours; then the FFmpeg plug-in: `MediaWriter` encodes VP9 through the
  back end (the plug-in says so on stderr) and `MediaReader` decodes the
  file through it (the codec name ends in "(accelerated: software)",
  every frame in its colour) and, with `off`, in software.
- **media_stream** (`media_stream_test`, renga-* images): streaming through
  the FFmpeg plug-in's protocols (`FFmpegURLIO.h`, roadmap F-31 phase 4)
  on the guest's loopback interface (the harness attaches no network
  card). HLS: three one-second MPEG-TS segments written with
  `MediaWriter` and a playlist, served by an HTTP/1.0 server thread in the
  test; `MediaReader` opens `http://127.0.0.1:8093/index.m3u8`, all 45
  frames must decode in their segment's colour and the server must see
  the playlist and segment requests. UDP and RTP output: `MediaWriter`
  sends MPEG-TS to `udp://127.0.0.1:5004` and `rtp://127.0.0.1:5006`; a
  socket receives whole TS packets (sync byte every 188 bytes), RTP
  version 2 with payload type 33 and consecutive sequence numbers, and
  what arrived decodes again to the frames sent. The capture session's
  stream output: `Screenshot --record 3 --stream udp://... --system-audio`
  must send MPEG-TS with video and audio that decodes. MediaPlayer opens
  the HLS URL (screenshot `mediaplayer_hls`, `media_stream.screenshots`).
  RTMP: the guest has no network card, so the test runs FFmpeg's RTMP
  protocol in listen mode as the server (through libavformat, on
  `rtmp://127.0.0.1:19350/live/ce`); `MediaWriter` publishes three seconds
  of FLV (Sorenson Spark + AAC), the server must receive both streams and
  every video packet, and what it received must decode to the frames sent.
  SRT is not tested.
- **media_resample** (`media_resample_test`, renga-* images): the mixer's
  resampling quality (roadmap F-31). A 44.1 kHz stream of a pure tone
  (1.1 kHz, then 15 kHz) plays into the 48 kHz mix with each algorithm set
  through the mixer's parameter; the test records the stream's loopback
  and measures THD+N (everything but the tone, against the tone, over 16
  windows). The windowed sinc (32 taps, Kaiser window, 256 phases, the
  new default) must stay under -70 dB at 1.1 kHz and -50 dB at 15 kHz
  (the image at 18.9 kHz included) and beat linear interpolation by
  20 dB. Measured: drop/repeat -26.3 / -2.5 dB, linear interpolation
  -34.7 / -8.8 dB, windowed sinc -74.0 / -52.4 dB. New settings must
  default to the windowed sinc. (Not 1 kHz: the host looks for
  `virtio_sound_test`'s 1 kHz phase on the first card.)
- **media_drift** (`media_drift_test`, renga-* images): clock-drift
  correction of mixer sinks (F-31). A stream routed to the second virtio
  card goes through a sink that compares that card's clock with the
  mixer's (a least-squares line through 16 samples, one a second) and
  resamples the mix to the card's pace. QEMU's two cards do not keep the
  same pace (each follows its own audio back end: the second measured
  4800 to 8000 ppm slow, wandering by about 1000 ppm), so the test skews
  the second card's published clock by +6000 ppm with multi_audio's
  hidden `renga:clock_skew_ppm` test hook and checks the change: the
  estimate settles (full window, no restarts), moves by 6000 +- 1500 ppm,
  the sink produces as many more frames as the estimate says (within
  400 ppm), and comes back when the skew is removed. Two cards with
  their own crystals are needed to hear the correction.
- **html5_media** (`media_html5_test`, renga-* images): HTML5 media in
  WebPositive through the Media Kit (F-57 item 4). A local HTTP server
  in the test (byte ranges, a thread per connection) serves a WebM
  (VP9 + Opus) and an MP4 (MPEG-4 + AAC, index moved to the front with
  libavformat as web MP4s have it) clip with 440 Hz left and 660 Hz
  right, and a page with each. In-tree checks: `BNetworkAddress` resolves
  `127.0.0.1` at once without a network (it took 56 s and failed, so the
  HTTP streamer could not reach a local server), `BMediaFile` on each
  clip's URL decodes both tracks, and WebKit's audio path without WebKit
  (the decoded format handed to a `BSoundPlayer`, one `ReadFrames()` per
  callback) plays both tones cleanly. In WebPositive: `canPlayType()`
  answers for WebM, MP4, Ogg Vorbis, FLAC, MP3 and HLS, and both clips
  start playing ("playing" event); screenshots `html5_webm` and
  `html5_mp4` (`html5_media.screenshots`). WebPositive's own sound is
  logged, not checked: the packaged HaikuWebKit 1.10.0 plays it with gaps
  (WebM) or not at all (MP4), see the roadmap (F-57).
- **media_latency** (`sound_latency_test`, renga-* images, last of the
  Media Kit tests): the low-latency audio mode (roadmap F-31,
  `headers/private/media/LowLatencyAudio.h`). A child process plays
  through `BSoundPlayer` with the buffer size left open, logs every
  callback and five 2 kHz bursts, each with `system_time()` at the start
  of its callback; the test records the mix through the loopback and
  converts each burst's performance time to real time with the mixer's
  time source as the buffer arrives: the latency from the callback to the
  card's playout as the Media Kit schedules it (the card's clock, not an
  acoustic measurement). Phases: normal mode (10 ms callbacks); the
  "Low latency" switch set through the mixer (the default output asked
  for 256-frame periods, the mixer's buffers following, the output's
  clock without steps); low-latency mode (callbacks of exactly 256 frames,
  steady, at most half the normal latency and 30 ms, a 6 s tone at
  3100/3300 Hz in the mix with at most one gap); the switch off (the card
  back to its default period after the game); `RENGA_AUDIO_LOW_LATENCY=1`;
  the device's "Buffer size" fixed at 512 frames and back; the Media
  preferences with the switch on and the device page (screenshots
  `media_low_latency`, `media_buffer_size`). On the host,
  `media_latency.device_continuous` looks for the tone on every card's
  recording: each piece of 5 s or more may have at most one gap (QEMU
  without KVM runs the guest slowly and ticks its audio every 10 ms), and
  `media_latency.screenshots` checks the two screenshots. Measured in the
  harness (TCG): about 50-80 ms normally, 16-25 ms in low-latency mode.
- **hda_eld**: the hda driver's ELD parser (HDMI/DisplayPort sinks:
  monitor name, connection type, LPCM channels, rates and sizes, other
  formats, speaker allocation, the output name and the multi_audio
  rates) with ELDs laid out after the HDA specification and CEA-861-D for
  an HDMI TV, a DisplayPort monitor and an AV receiver without a name,
  and five malformed ones it must refuse. Since the HDMI playback commit
  also the playback set-up of `hda_hdmi.h`: the audio InfoFrame bytes
  (HDMI after CEA-861-D with its checksum, DisplayPort as the InfoFrame
  SDP Linux sends, stereo and 8 channels), the stereo ASP channel mapping,
  the verb sequence for a pin and its converter in Linux's order (pin
  output, digital enable, channel count, 8 slots, InfoFrame off, index,
  bytes, best-effort transmission) for HDMI and DisplayPort, the rates
  and sample sizes from the sink's ELD (CEA-861 basic audio without one),
  and the choice of pin. QEMU emulates no HDMI codec: the verbs reaching a
  real codec need hardware.
- **uvc**: the UVC payload code of the usb_webcam media add-on
  (`addons/uvc/UVCPayload.cpp`) with replayed payloads: frames split into
  1024-byte payloads with 12-byte headers, the FID toggling, EOF set or
  not, header-only payloads, the ERR bit, a lost payload, invalid headers,
  a stream joined mid-frame; the YUY2 to `B_RGB32` conversion (BT.601);
  the default Huffman tables that MJPEG frames leave out. QEMU 8.2 emulates
  no USB video device, so the driver's USB side needs a real camera.
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
  `TCP_INFO`, orderly and abortive close). `user_locks_test` (suite
  `user_locks`): libroot's `rw_lock` (app_server's `MultiLocker`),
  `pthread_rwlock_t` and libshared's `RWLockManager` when a signal
  interrupts a waiting thread (SIGCHLD, which interrupts even when ignored,
  and SIGUSR1 with a handler): the waiter must use no CPU time while it
  waits, get the lock within 3 s of its release, and the lock must be free
  afterwards. A second writer or a reader must wait for an `RWLockManager`
  write lock.
- **shortcut_keys**: Ctrl as the shortcut key, see above.
- **usb_audio**: plugs QEMU's `usb-audio` in while the guest runs and
  drives `/dev/audio/hmulti/usb/<n>` with the multi_audio add-on's ioctl
  sequence: the device's product string as its name, its one rate and
  format (others refused), the feature unit's mute and volume controls
  with QEMU's range (-127.996 to +8 dB in steps of 0.53125 dB) and their
  values read back, buffer cycles, frame counters and pacing in five tone
  phases (0 dB, -40 dB, muted, after a force stop, and while the device is
  pulled out), a force stop from a second thread. Pulled out while it
  plays, the waiting exchange must fail within 3 s (not time out), the
  node must go, and ioctls on the open handle must fail; plugged in again
  it must be published and play. The harness then finds the phases in
  `usb-0.wav` and `usb-1.wav` (`usb_audio_out.*`): each in one piece, the
  -40 dB phase at QEMU's level for it (0.6875 of the 0 dB phases), the
  muted one absent.
- **usb_audio_uac2**: the `usb_audio` driver's sources against a mock USB
  stack with a synthetic USB Audio Class 2 device, "Renga UAC2 Test DAC":
  high speed, a programmable clock source (six discrete rates, 44.1 to
  192 kHz) behind a clock selector, playback (24-bit 3-byte and 16-bit
  alternates, asynchronous with an explicit feedback endpoint) and capture
  (24-bit), feature units with mute and volume. The mock answers only the
  UAC2 forms of the class requests, checks every isochronous transfer as
  Haiku's xhci driver does, runs its endpoints in real time, plays and
  records a frame counter, and runs its clock 1000 ppm fast. Checks: rates
  from the clock's RANGE answer, the formats of both alternates, gain
  ranges from RANGE, volume and mute set and read back in UAC2 requests
  only, the input following the output on the shared clock, the clock set
  by SET CUR, the right alternates, playback and capture bit-exact (no
  frame lost, repeated or misplaced) at 44.1 kHz/24-bit and 48 kHz/16-bit,
  packets of only the two neighbours of the feedback's average (5.518 and
  6.006 frames per microframe), force stop, removal while playing (the
  exchange returns `B_DEV_NOT_READY` within 0.2 s, the node goes, the slot
  is free again), and the packet and feedback arithmetic (44.1 kHz at full
  speed, 11.025 kHz at high speed, 10.14 and 16.16 feedback values, and a
  10.14 value sent at high speed).
- **virtio_block**: flush and trim on two scratch disks, one offering
  DISCARD and one only WRITE ZEROES; the harness then checks on the host
  that the trimmed space was freed in the image files.
  It also grows the first scratch disk while the guest runs. The media
  changed flag that follows is cleared by whoever reads it first, the test
  or the kernel's once-a-second media checker; the check passes if either
  saw it (the checker logs "Media changed from <disk>").
- **virtio_sound**: drives `/dev/audio/hmulti/virtio_sound/*` with the same
  ioctl sequence and buffer-cycle rules as the multi_audio media add-on. It
  first tells the two cards apart by recording half a second from each
  (only the SDL card's input carries the fed tone). On the other card it
  plays four tone phases (initial start, restart after force-stop, after a
  44.1 kHz format change, after reopening) and checks the description,
  formats, buffer layout, buffer cycles, frame counters and real-time
  pacing; the harness then checks that every phase is present in
  `audio.wav`, on the right channels, without gaps or corruption. On the SDL
  card it records five phases (48 kHz, after a tone change, after a
  force-stop, at 44.1 kHz, after reopening with longer periods): record
  cycles and frame counters advance one period at a time, capture runs in
  real time, and each channel carries the tone the harness fed, at the
  right level, with no gap (Goertzel purity per 100 ms window). It also
  checks that capture on the card without input does not disturb playback,
  and that playback works without record buffers.

## Revision tags

The build names itself after the nearest upstream `hrev` tag. The
`Renga hrev tag` workflow (`.github/workflows/ce-hrev-tag.yml`) publishes that
tag automatically on every push to `ce/main`, so a fresh clone of the fork
builds without setting `HAIKU_REVISION`. Fetch it with `git fetch --tags
origin`. If ce/main is rebased onto a newer upstream revision, the next push
publishes the new tag; "Run workflow" on the Actions tab does the same on
demand.
