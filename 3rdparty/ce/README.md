# Haiku Community Edition: test harness and fork tooling

This directory holds tooling specific to the Haiku Community Edition fork.
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
3rdparty/ce/harness/selftest.py                 # check the harness itself
```

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
- **Kernel debugger.** When the guest drops into KDL, the harness sends `bt`
  and `threads` and stores the output in the report.

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

## Current suites

- **virtio_sound**: drives `/dev/audio/hmulti/virtio_sound/*` with the same
  ioctl sequence and buffer-cycle rules as the multi_audio media add-on. It
  plays three tone phases (initial start, restart after force-stop, and after
  a 44.1 kHz format change) and checks the description, formats, buffer
  layout, buffer cycles, frame counters and real-time pacing. The harness
  then checks that every phase is present in the recording, on the right
  channels, without gaps or corruption.
