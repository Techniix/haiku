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
3rdparty/ce/harness/selftest.py                 # check the harness itself
```

`--firmware uefi` and `--media cdrom` switch the image to `minimum-anyboot`
(only anyboot images have an EFI system partition and a CD boot catalog);
`--profile` overrides that. The four combinations cover every way a release
image boots: BIOS from disk or USB (MBR), BIOS from CD (the El Torito floppy
image), and UEFI from either. UEFI runs use a fresh copy of the OVMF variable
store each time.

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

The exit status is 0 only if the build succeeded, the guest ran to the end
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
are defined in `build/RengaBuildProfiles`; Jamrules includes that file, the
only upstream build file the fork touches. To test one:

```sh
3rdparty/ce/harness/ce-test.py --profile renga-raw
```

The harness then also checks the volume name and the repositories.

## Current suites

- **posix_compat**: eventfd, timerfd, memfd, POSIX message queues, System V
  shared memory, the libc additions, user contexts, and TCP (keepalive,
  `TCP_INFO`, orderly and abortive close).
- **virtio_block**: flush and trim on two scratch disks, one offering
  DISCARD and one only WRITE ZEROES; the harness then checks on the host
  that the trimmed space was freed in the image files.
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
