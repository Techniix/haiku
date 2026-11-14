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
