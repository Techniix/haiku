# Renga: instructions for Claude Code sessions

Renga is an operating system based on Haiku, developed in the fork `Techniix/haiku` on branch `ce/main`.
Maintainer: Techniix. This file is loaded in every session; keep it short. Load the files under
"Where to find what" only when the task needs them.

## Identity and upstream policy

- `ce/main`: all Renga work. `master`: identical to upstream Haiku; never touch it or upstream tags.
- `AGENTS.md` at the root is upstream Haiku's policy: no AI-generated or AI-assisted contributions upstream.
  It is not deleted, and it is the reason all AI-assisted work stays in this fork. Upstream reports are
  written by the maintainer in his own words.
- Technical identifiers keep their old names on purpose: `ce/main`, `3rdparty/ce`, `CE_HARNESS`,
  `ce-test.py`, `CE-TEST`/`CE-SUITE` markers. Platform identity stays Haiku (`uname`, `__HAIKU__`).
- `RENGA_MASTER_ROADMAP.md` is the authoritative status and plan. `docs/SESSION-HANDOVER.md` holds the
  working method, the rules and the newest "Batch N: continue from here" notes.

## Standing rules (maintainer's), verbatim from docs/SESSION-HANDOVER.md

- Commits: explicit paths only (`git commit -- <paths>`, `git add -- <new files>`), never `-A`/`-a`; very detailed messages ending with the two trailer lines `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>` and `Claude-Session: <session URL>`; push with `git push -q origin HEAD:ce/main`; never force-push.
- Upstream Haiku does not accept AI-assisted work, so everything stays in the fork. Upstream tickets are written by the maintainer in his own words.
- Add-only ABI. Every user-facing feature gets a GUI and an on/off setting with sensible defaults ("Advanced customization, easy to find"). Lean by design; responsiveness first. "Polish while in there". Every Renga program gets its own HVIF icon in the Renga style.
- Testing (maintainer, 2026-11-14): per piece, build and run only that piece's own suites as a partial run (`ce-test.py --only …`, about 5-10 minutes), then commit; kernel or boot-path changes also get one quick default-profile boot run; the full four-way sweep (default, `--smp 2`, `--firmware uefi`, `--profile renga-raw`) once per batch before an image. Known-flaky checks are retried once (`KNOWN_FLAKY` in `ce-gates.sh`, each with a reason). Every fix gets a regression test shown failing on the old code where practical. QEMU results are never presented as hardware results. Failed tests are never dropped silently.
- Docs are professional and never mention personal matters. Roadmap and partner-note entries are short (a line or two); commit messages carry the detail.
- Usage is limited: prefer fewer, larger helper briefs; keep check-ins short.

Under the hybrid workflow (decisions log, 2026-11-21; `docs/HYBRID-WORKFLOW.md`), work is pushed to its own
branch and reaches `ce/main` through a pull request after the gates and an architect review, merged with a
merge commit. The `Claude-Session:` trailer carries the session's identifier where one exists (the
maintainer may drop it for local sessions). Use the handle "Techniix" in docs, vendor fields and copyright
lines, never a personal name.

## Repositories and branches

| Repository | Role |
| --- | --- |
| `Techniix/haiku` (this tree) | Renga: `ce/main` integration, work branches below |
| `Techniix/renga-builds` | Images: one branch `build/<name>` per image, published by its "Publish build" workflow |
| `Techniix/RengaPorts` | HaikuPorter recipes and the Renga package repository (`packages` branch, GitHub Pages) |
| `Techniix/nvidia-haiku` (Quasar), `Techniix/Chromium-HaikuOS`, `Techniix/OBS-Studio_HaikuOS` | Partner ports; they file `RENGA-REQUESTS.md`, Renga answers in `docs/partners/*.md` |

Branches here: `batch/<NN>-<topic>` (a batch), `fix/<topic>` (hardware-report fixes), `ai/local/<topic>`
(local-model work, rebased into a Claude-reviewed branch before merging), `wip/<topic>` (parked work),
`infra/<topic>` (tooling). Milestone tags: `ckpt/<YYYY-MM-DD>-<topic>`. Rollback is `git revert` on
`ce/main`; never rewrite its history.

## The batch method

- The maintainer decides direction. Proposals come with a recommendation; his decisions are recorded with
  their date in the roadmap's decisions log (section 10.3) before acting.
- A batch is a list of pieces in the newest work order (top of the roadmap; batch order in the hand-over).
- Per piece: implement, run that piece's suites (`tools/renga/renga-test`), commit with explicit paths and
  a detailed message stating which tier ran and its numbers. Kernel or boot-path pieces also get one quick
  default-profile boot run.
- Per batch: one four-way sweep (`tools/renga/renga-gate`), then one image (`tools/renga/renga-image`).
- Every fix gets a regression test, shown failing on the old code where practical. Report results as
  "QEMU Tested"; only a real-machine result is "Hardware Tested".
- Every change keeps the roadmap honest: a short status line in the register entry, detail in the commit.

## Tiers: who does what

| Tier | Who | Does | Never does |
| --- | --- | --- | --- |
| 0 | `tools/renga/` scripts | Build, run suites, sweep, image, split and publish, summarise | Decide anything |
| 1 | Local model (optional) | Summarise logs, draft commit messages and catalog strings, scaffold tests | Touch protected paths, push `ce/main`, merge |
| 2 | Claude Code implementation session (Sonnet by default) | One brief at a time on a feature branch; runs its suites; commits pieces | Merge to `ce/main`; change protected paths without an architect brief |
| 3 | Claude architect (Opus) | Briefs, architecture, hard debugging, reviews, merge approval, the roadmap | Long build or test loops |

Only the architect: writes and changes briefs; decides architecture and ABI; approves changes to protected
paths (`Architect-Approved: yes` in the PR); reviews and approves merges to `ce/main`; edits the decisions
log (after the maintainer decides). Kernel, boot path, app_server and final review stay with Opus.

## Protected paths and gates

Protected (`tools/renga/protected-paths`): `src/system/kernel/`, `src/system/boot/`,
`src/system/runtime_loader/`, `headers/os/`, `headers/posix/`, `src/servers/app/`,
`src/add-ons/kernel/drivers/`, the LinuxKPI and Renguin kernel modules, `build/jam/`, the harness core and
the roadmap's decision entries. A branch touching them needs `Architect-Approved: yes` in its PR
description; `renga-guard` (pre-push hook and PR check) enforces this. It stops accidents, not bypasses.

Gates before a merge to `ce/main`: build with no new warnings; the suites for the changed paths
(`renga-changed-suites`); `selftest.py` once per tree; the four-way sweep for a batch branch (known-flaky
failures retried once); `largefont` for any change under `src/apps`, `src/preferences`, `src/kits/tracker`
or `src/kits/interface`; the ABI check (no removed symbols); a roadmap status line and, for fixes, the name
of the regression test.

## Commands (run from the repository root)

| Command | What it does |
| --- | --- |
| `tools/renga/renga-build [--image <profile>] [targets]` | Configure if needed, then `jam` (default `@renga-raw`) |
| `tools/renga/renga-test <suite...> [ce-test args]` | `ce-test.py --only` for those suites, then a short summary |
| `tools/renga/renga-gate [label]` | The four-way sweep through `ce-gates.sh` (resumable) |
| `tools/renga/renga-summary [dir]` | At most ~40 lines: verdict, counts, failing checks |
| `tools/renga/renga-changed-suites [base]` | Suites for the branch's changed paths; says when the full set is required |
| `tools/renga/renga-guard [base]` | Protected paths, `git diff --check`, trailer warnings |
| `tools/renga/renga-abi-check [base]` | Exported symbols of the system libraries: removals fail |
| `tools/renga/renga-image <name> [--push]` | Anyboot image, parts, sha256, `build.txt`, branch on renga-builds |
| `tools/renga/install-hooks` | Installs the pre-push hook (renga-guard) |

Each has `--help`. Partial runs exit 2 when the selection passed: a partial run never counts as a full pass.

## Saving tokens

- Never watch a build or a run. The maintainer starts it; read `renga-summary` output (or `summary.md`),
  not `serial.log`, `build.log` or other logs. Open a log only for a named failing check, with `grep` or a
  line range.
- One fresh session per brief; `/clear` between unrelated tasks. No polling and no scheduled check-ins.
- Do not read `RENGA_MASTER_ROADMAP.md` (about 3,000 lines) whole: read its top (current work order), the
  F-section of the task (`grep -n "^#### F-" RENGA_MASTER_ROADMAP.md`) and section 1.2 when rules matter.
- Read `3rdparty/ce/README.md` by heading, not whole. Old batches and work orders are in `docs/history/`;
  open them only when a task refers to them.
- Reviews read `git diff --stat`, the gate summary and the hunks that matter.

## Where to find what

| Need | File |
| --- | --- |
| Operating manual (setup, daily loop, Git, models) | `docs/HYBRID-WORKFLOW.md` |
| Component map with roadmap links | `docs/claude/architecture.md` |
| Harness, suites, profiles, flaky list, reading results | `docs/claude/testing.md` |
| Configure, build, images, renga-builds, RengaPorts | `docs/claude/build-and-release.md` |
| Partner requests and replies | `docs/claude/partners.md` |
| What an architect brief and a review contain | `docs/claude/brief-template.md` |
| Status, work order, decisions | `RENGA_MASTER_ROADMAP.md` (top; section 10.3) |
| Rules, batch log, continue-from-here | `docs/SESSION-HANDOVER.md` |
| Port-visible interfaces | `3rdparty/ce/PORTING.md` |
| Harness usage and suites | `3rdparty/ce/README.md` |
| Upstream build instructions | `ReadMe.Compiling.md` |
| Superseded work orders, earlier batches, cloud-era procedure | `docs/history/` |
