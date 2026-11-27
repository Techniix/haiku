# Testing Renga

Load this when a task runs, chooses or reads tests. The full harness reference is
[3rdparty/ce/README.md](../../3rdparty/ce/README.md) (read it by heading); the rules are in the roadmap's
[section 1.2](../../RENGA_MASTER_ROADMAP.md#12-architectural-principles) ("Tiered test gates") and the
standing rules in [docs/SESSION-HANDOVER.md](../SESSION-HANDOVER.md).

## The harness

`3rdparty/ce/harness/ce-test.py` builds a test image (`CE_HARNESS=1` adds the test programs and the
in-guest runner), boots it headless in QEMU, collects the `CE-TEST <suite>.<check> PASS|FAIL <detail>` lines
from the serial console, runs host-side checks (recorded audio, screenshots, ISO volume ID, QEMU's own
diagnostics) and writes a report. On a kernel panic or a stall it runs a kernel debugger session (`kdl.log`).

Each run writes `<output>/ce-test-results/<timestamp>/` and points `latest` at it:

| File | Contents |
| --- | --- |
| `summary.md` | Verdict, build, boot, timing per program, every check, QEMU and driver logs |
| `results.json` | The same, machine readable (`verdict`, `tests`, `host_checks`, `problems`, `timing`) |
| `serial.log`, `serial.timed.log` | Guest console, the second with host seconds per line |
| `build.log`, `qemu.log`, `kdl.log` | Build output, QEMU diagnostics, debugger session (if any) |
| `screen-*.png`, `audio.wav`, `sdl-out.raw` | Screenshots and recordings the checks used |

Exit status: 0 only for a full run where everything passed. A partial (`--only`) run exits 2 when its
selection passed and 1 when it failed: **a partial run never counts as a full pass**. A command-line error
(an unknown option, or a test program the image does not have) exits 64; `renga-test` reports any run
that wrote no results as failed.

`3rdparty/ce/harness/selftest.py` tests the harness's own analysis against known-good and known-bad input.
Every new host-side check gets a known-bad case there. It has one known failure on a clean tree ("sound
settings: clean recordings pass", hand-over batch 4).

## Suites, programs and profiles

A suite is a directory under [3rdparty/ce/tests/](../../3rdparty/ce/tests/); its Jamfile's
`Application <name>_test` lines are its programs. `--only` takes program names (comma-separated, with or
without `_test`); `tools/renga/renga-test` also accepts suite directory names and expands them.

| Suites (directories) | Notes |
| --- | --- |
| `posix_compat`, `virtio_block`, `virtio_sound`, `nvme`, `pointer`, `shortcut_keys`, `syncobj`, `kdl`, `storage`, `package_priority`, `packagefs`, `renguin`, `linuxkpi`, `bootstate` | Run on the default `minimum-raw` image (some checks only on renga-*) |
| `media`, `controlpanel`, `largefont`, `welcome`, `deskbar`, `tracker`, `webpositive`, `gamepad`, `appserver`, `wayland`, `dbus`, `devel`, `agents`, `rengadoctor`, `renguinmanager`, `bootmenu`, `crashlog`, `usb_audio`, `uvc`, `hda_eld`, `wifi`, `toolchain` | Renga's own programs: most need `--profile renga-raw` |
| `installer` | `installer_test` only with `--installer-upgrade`; `installer_ui_test` on renga-* |
| `bootstate` | `restore_point_test` only with `--restore-points` |
| `appserver` | `displays_multi_test` with `--displays 2` |

The authoritative per-suite description, including which flags and profiles each needs, is
"Current suites" in the harness README. `ce-test.py --list-tests` prints the programs of the last image.

**`largefont`** (`ui_large_font_test`, renga-* images): runs every Renga window at a 20 pt font and
1024x768 and reports clipped or overlapping views per window (`large_font.<window>`), with a screenshot
each. Run it for any change under `src/apps`, `src/preferences`, `src/kits/tracker` or
`src/kits/interface`.

## Profiles, firmware and the four-way set

- Profiles (`--profile`): `minimum-raw` (default), `minimum-anyboot` (implied by `--firmware uefi` or
  `--media cdrom`), `renga-raw` (full Renga desktop; needed for media, GUI and Renga programs),
  `renga-anyboot`. Definitions: [3rdparty/ce/build/RengaBuildProfiles](../../3rdparty/ce/build/RengaBuildProfiles).
- Other switches: `--smp N`, `--firmware bios|uefi`, `--media disk|cdrom`, `--disk-bus ide|virtio`,
  `--two-installs`, `--other-systems`, `--displays N`, `--screenshots`, `--stall`, `--timeout`.
- **Four-way set:** default, `--smp 2`, `--firmware uefi`, `--profile renga-raw`, plus `selftest.py` once
  per tree: `tools/renga/renga-gate` (wraps `ce-gates.sh`). Resumable: configurations that passed on the
  exact same tree are skipped. Notes-only changes (the roadmap, `docs/partners/`) keep the tree key.
  A configuration a restart killed is resumed from its last finished test program (`ce-test.py --resume`);
  a hung test program is killed after `--suite-timeout` (1200 s) and reported as `harness.watchdog.*`.

## Which tests when (roadmap 1.2; standing rules)

| When | What |
| --- | --- |
| Every piece | Its own suites as a partial run (`renga-test <suites> --profile renga-raw`, and on default where the suite runs there), then commit |
| Kernel or boot-path piece | Also one quick default-profile boot run |
| Shared foundations (kernel, drivers, boot loader, installer, app_server, libbe/libroot, locking, memory, the harness, unmapped paths) | The full four-way set on that commit |
| End of every batch, before every image | The four-way sweep on the final tree; a regression is bisected over the batch and fixed |
| Independent app, media, UI or test items | May be grouped 2-4 per combined four-way run, then committed one item per commit citing it |

`tools/renga/renga-changed-suites [base]` maps the changed paths through `tools/renga/suite-map` and says
when the full set is required. The map is conservative; when it is wrong, fix the map in the same branch.
Commit messages state which tier ran and its numbers.

## Known-flaky checks: one retry

`KNOWN_FLAKY` in [3rdparty/ce/harness/ce-gates.sh](../../3rdparty/ce/harness/ce-gates.sh) lists checks
that fail intermittently for reasons outside the change under test (timing on an emulated, shared host),
each with its reason in the comment above it. Today they are all Media Kit timing checks
(`media_latency.*`, `media.click_free_*`, `media_routing.*`, `media_drift.skew_*`, `sound_settings.*`,
`sound_audio.*`, `media_usb*.*`). The rule: a configuration whose failures are *all* on the list is
retried **once**, automatically, and both results are recorded; a repeated failure blocks. A check is
added to the list only with a reason line, never to make a gate pass. Failed tests are never dropped
silently: report them, flaky or not.

Recurring failures that are **not** flaky (open work, hand-over batch 11 and 12): `linux_apps.open_with`,
`linux_apps.appimage`; `renguin_manager.display_scale` and two `large_font` windows until the batch 12
Renguin Manager work lands; `large_font.deskbar_preferences` (window larger than the screen). With the
Chromium layer, `renguin.chrome.*` fail intermittently: Linux Chromium crashes with a NULL buffer pointer in
libxkbcommon or libbrotlidec (exit 191; open Renguin bug, same on the old kernel). On the WSL host:
`crashlog.debugger_reboots` fails when QMP's socket is gone after the reboot ("set-action: No such file"),
and `selftest.py` fails "sound settings: clean recordings pass" (also on HEAD).

## Reading results cheaply

1. `tools/renga/renga-summary` (newest run) or `renga-summary <dir>`: verdict, counts, failing checks with
   one line each, run problems, programs that exited non-zero. At most about 40 lines.
2. For a gate: `renga-summary <output>/ce-gates/<key>` prints its `summary.txt`.
3. Only then open `summary.md` at the failing check (`grep -n '<check>' summary.md`), and the serial log
   around that program (`grep -n 'CE-INFO running <program>' serial.timed.log`), never whole logs.
4. The **Boot** section of `summary.md` shows host load. Timing checks that fail on a busy host are not
   regressions; say so rather than retrying silently.

## QEMU Tested versus Hardware Tested

- Everything the harness passes is **QEMU Tested** (status `QT`). It is never presented as a hardware
  result.
- **Hardware Tested** (`HT`) needs a run on a real machine, with the machine and the result recorded in the
  roadmap entry. The maintainer's test hardware is listed in the hand-over.
- Roadmap section 8.4 lists what has been verified where. Hardware results pending are kept in each
  batch's "continue from here" notes until they arrive.
