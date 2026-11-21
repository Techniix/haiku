# Renga hybrid local/cloud workflow

The operating manual for developing Renga with builds and tests on the maintainer's PC and Claude
sessions for decisions, code and review. Adopted 2026-11-21 (roadmap decisions log). The working method
and the standing rules are unchanged: see [docs/SESSION-HANDOVER.md](SESSION-HANDOVER.md) and
[CLAUDE.md](../CLAUDE.md).

Steps marked **(verify on first setup)** were written from the documentation and the cloud host
(Ubuntu 24.04) but not yet run on Windows 11 / WSL2.

## Why

Claude Code on the PC uses the same Claude plan as a cloud session. What saves usage is taking Claude out
of everything that needs no judgement: builds, test runs, sweeps and image delivery run as plain scripts
(`tools/renga/`), and a Claude session only reads their short summaries. KVM on the PC makes the QEMU runs
several times faster than the cloud host's full emulation (an estimate, to be measured in the first
verification).

| Tier | Who | Does | Never does |
| --- | --- | --- | --- |
| 0 | Scripts (`tools/renga/`) | Build, run suites, sweep, image, split and publish, summarise | Decide anything |
| 1 | Local model (optional) | Summarise failing logs, draft commit messages and catalog strings, scaffold tests | Touch protected paths, push `ce/main`, merge |
| 2 | Claude Code implementation session (Sonnet by default) | One brief at a time on a feature branch; runs its suites; commits | Merge to `ce/main`; change protected paths without an architect brief |
| 3 | Claude architect (Opus) | Briefs, architecture, hard debugging, reviews, merge approval, the roadmap | Long build or test loops |

## 1. One-time setup on Windows 11

### 1.1 WSL2 and its resources

1. Install WSL with Ubuntu 24.04 from an administrator PowerShell: `wsl --install -d Ubuntu-24.04`, then
   restart and create the Linux user. **(verify on first setup)**
2. Give WSL most of the machine. Create `%UserProfile%\.wslconfig` (Windows side):

   ```ini
   [wsl2]
   # most of the RAM; leave Windows 6-8 GB
   memory=24GB
   # most of the cores
   processors=12
   swap=16GB
   nestedVirtualization=true
   ```

   Adjust the numbers to the PC. Then `wsl --shutdown` in PowerShell and start Ubuntu again.
   **(verify on first setup)**
3. Check KVM inside WSL:

   ```sh
   ls -l /dev/kvm                      # must exist
   sudo usermod -aG kvm "$USER"        # then close and reopen the WSL terminal
   [ -w /dev/kvm ] && echo "KVM usable"
   ```

   The harness uses KVM automatically when `/dev/kvm` is writable (`-enable-kvm -cpu host`). Without it,
   QEMU still works at about the cloud host's speed. **(verify on first setup)**
4. Disk: a full build plus harness results need about 40-60 GB inside WSL.

### 1.2 Packages

The build prerequisites of [ReadMe.Compiling.md](../ReadMe.Compiling.md) ("Required Software"), as Ubuntu
packages, plus the harness and workflow tools:

```sh
sudo apt update
sudo apt install git nasm bc autoconf automake texinfo flex bison gawk build-essential \
    unzip zip wget less zlib1g-dev libzstd-dev xorriso mtools libtool python3 \
    libexpat1-dev libffi-dev libssl-dev python3-dev \
    qemu-system-x86 qemu-system-gui qemu-utils ovmf gh cppcheck shellcheck
```

`qemu-system-gui` provides QEMU's SDL audio driver, which the harness's second sound card uses; `ovmf`
provides `/usr/share/OVMF/OVMF_CODE_4M.fd` and `OVMF_VARS_4M.fd`, the harness's UEFI defaults. These
package names match the cloud host (Ubuntu 24.04); **(verify on first setup)**. Then `gh auth login`.

### 1.3 Clones (inside WSL, never under /mnt/c)

Build only on WSL's own ext4 file system (the home directory). NTFS through `/mnt/c` is slow and its case
handling breaks the build.

```sh
git config --global core.autocrlf false
mkdir -p ~/renga && cd ~/renga
git clone https://github.com/haiku/buildtools.git            # or https://review.haiku-os.org/buildtools.git
git clone -b ce/main https://github.com/Techniix/haiku.git
git clone https://github.com/Techniix/renga-builds.git
cd haiku
git config user.name Techniix        # the maintainer's own commits
tools/renga/install-hooks            # pre-push: renga-guard
```

The scripts expect `buildtools` and `renga-builds` next to the `haiku` tree (override with
`RENGA_BUILDTOOLS` and `RENGA_BUILDS_DIR`).

### 1.4 Claude Code

Install it as the official quickstart describes (<https://code.claude.com/docs/en/quickstart>): for Linux
and WSL the native installer is

```sh
curl -fsSL https://claude.ai/install.sh | bash
claude --version
```

If `claude` is not found afterwards, see the documentation's "Troubleshoot installation" page. Always start
it **from the repository root** (`cd ~/renga/haiku && claude`), so it loads `CLAUDE.md`. Log in with the
Claude account on first start. Note: with `CLAUDE.md` present, Claude Code does not load upstream's
`AGENTS.md` as instructions; `CLAUDE.md` explains that file's policy.

## 2. First build

```sh
cd ~/renga/haiku
tools/renga/renga-build               # builds jam if missing, configures, then @renga-raw
```

The first run builds the cross-tools (1-2 hours; mostly waiting). Later builds are incremental. The full
output is in `generated.x86_64/renga-build.log`; the terminal shows only the end on failure.

First verification (phase 2 of the plan): run

```sh
tools/renga/renga-test welcome controlpanel largefont --profile renga-raw
tools/renga/renga-gate verify-local
```

and compare with the cloud results for the same commit (they should match; known-flaky checks may differ).
Note the run times next to the cloud ones.

## 3. The daily loop

1. **Architect session (Opus):** the maintainer describes the goal; the architect writes a brief
   ([docs/claude/brief-template.md](claude/brief-template.md)) naming the branch, files, design and tests.
2. **Implementation session (Sonnet):** a fresh `claude` session in the repository root gets the brief.
   It creates the branch, implements piece by piece and runs the piece's suites:

   ```sh
   tools/renga/renga-test $(tools/renga/renga-changed-suites) --profile renga-raw
   ```

   It reads only the `renga-summary` output, commits each piece with explicit paths, pushes its branch and
   opens a PR. Long runs are started by the maintainer or by the session's terminal tool, and nobody
   watches them: the summary is pasted back when they finish.
3. **Gates:** before a batch branch merges, `tools/renga/renga-gate` (the four-way sweep, resumable),
   `tools/renga/renga-guard`, and `tools/renga/renga-abi-check` when libraries changed. The PR carries
   the gate summary.
4. **Review (Opus):** the architect reviews the PR (`git diff --stat`, the summaries, the hunks that
   matter) and approves or asks for changes.
5. **Merge and image:** the maintainer merges the PR with a merge commit. For an image:
   `tools/renga/renga-image <name> --sweep "<gate line>" --push`, then Actions → "Publish build".
6. **Hardware test:** on the real PC (dual boot) or a Renga VM; results recorded as Hardware Tested.

Between tasks: `/clear`, or start a new session. No scheduled check-ins.

## 4. Git workflow

- `ce/main` is the integration branch: merges only through pull requests after the gates and the review,
  with a merge commit (keeps the batch history readable). Never force-push it. `master` stays upstream;
  never touch it or upstream tags.
- Branch names:
  - `batch/<NN>-<topic>`: a batch, e.g. `batch/12-install-boot`;
  - `fix/<topic>`: hardware-report fixes;
  - `ai/local/<topic>`: local-model work; rebased into a Claude-reviewed branch before merging;
  - `wip/<topic>`: parked work (e.g. `wip/batch12-depot-renguinscale`);
  - `infra/<topic>`: tooling.
- One commit per finished piece, explicit paths (`git add -- <paths>`, `git commit -- <paths>`), very
  detailed messages with the trailers from the standing rules. The `Claude-Session:` line carries the
  session identifier where one exists; for local sessions it is the maintainer's call to keep or drop it.
- Checkpoints: tag milestones `ckpt/<YYYY-MM-DD>-<topic>` (`git tag -a ckpt/2026-11-22-batch12 -m ...`,
  `git push origin ckpt/2026-11-22-batch12`).
- Rollback: `git revert <commit>` (or `git revert -m 1 <merge>` for a whole PR) on a branch, through a PR.
  Feature branches can be reset freely. Earlier images stay on renga-builds' releases.
- Pull requests: `gh pr create --base ce/main`, with the gate summary, the suites run and their numbers,
  and the line `Architect-Approved: yes` only when the architect approved a protected-path change
  (otherwise `Architect-Approved: pending` or omitted).
- Protected paths ([tools/renga/protected-paths](../tools/renga/protected-paths)): the pre-push hook and the
  PR check run `renga-guard`. For a local push the architect approved: `RENGA_ARCHITECT_APPROVED=yes git push`.

## 5. Which model when

| Work | Model |
| --- | --- |
| Briefs, architecture, ABI and interface design, kernel, boot path, app_server, Renguin syscalls, LinuxKPI, hard debugging, PR review, roadmap decisions | Opus (architect) |
| Implementing a precise brief, tests from an existing suite, GUI pages, docs, partner-note drafts | Sonnet (implementation) |
| Log shortening, commit-message drafts, catalog strings, test scaffolds | Optional local model, checked by a script or Claude |

Switch the model in a running session with `/model`. Remaining Opus time is best spent on the
architecture-heavy items in the newest work order (for example the Linux Chrome shared-memory SIGBUS, the
zygote and GPU process, the Installer and partition-manager design, the boot menu graphics, Renga Depot's
multi-source design) and on briefs that let the rest be done by Sonnet.

## 6. Optional: a local model (Ollama)

Set this up last, only if time remains. A 12 GB GPU runs a 7-14B code model at 4-bit through Ollama on
Windows; WSL reaches it over the Windows host's address. **(verify on first setup)**

- Good jobs: shorten a failing `serial.log` to the relevant lines; draft commit messages from a diff for
  Claude to approve; fill catalog entries; scaffold a test from an existing suite; reformat docs.
- Not: kernel, drivers, boot loader, app_server, ABI, Renguin syscalls, architecture, or anything without a
  test.
- Guardrails: it works only on `ai/local/*` branches and never pushes `ce/main`; it cannot touch protected
  paths (`renga-guard` fails); every change goes through the same gates and a Claude review.

## 7. Optional: a self-hosted GitHub runner in WSL

With a runner, every PR gets the build and suite gates automatically, without Claude tokens.

1. In `Techniix/haiku` on GitHub: Settings → Actions → Runners → "New self-hosted runner" (Linux, x64),
   and follow the commands it shows inside WSL. Give it the extra label `renga`. **(verify on first setup)**
2. To run it as a service, systemd must be enabled in WSL (`/etc/wsl.conf`: `[boot]` `systemd=true`); then
   the runner's own `./svc.sh install` and `./svc.sh start`. **(verify on first setup)**
3. The runner needs the same packages as section 1.2, and the `buildtools` clone next to its work
   directory's checkout (or `RENGA_BUILDTOOLS` in the runner's environment).
4. Set the repository variable `RENGA_SELF_HOSTED` to `true` (Settings → Secrets and variables → Actions →
   Variables). Without it, the self-hosted job is skipped and nothing queues.

The workflow ([.github/workflows/renga-pr-checks.yml](../.github/workflows/renga-pr-checks.yml)) always runs
the cheap checks on GitHub's runners (guard, `git diff --check`, shellcheck). The self-hosted job builds,
runs the suites for the changed paths and the ABI check; it runs only for PRs from branches of this
repository, never from forks, since it executes the PR's code on the PC.

## 8. Troubleshooting

| Symptom | Cause and fix |
| --- | --- |
| `/dev/kvm` missing in WSL | Nested virtualization off: check `nestedVirtualization=true`, virtualization enabled in the firmware (SVM on AMD), `wsl --update`, `wsl --shutdown`. Runs still work without KVM, slowly. |
| `/dev/kvm` exists but runs are slow | Not writable: `sudo usermod -aG kvm $USER`, reopen the terminal; `summary.md`'s Boot section shows the QEMU command (look for `-enable-kvm`). |
| Build fails with odd file errors or is very slow | The tree is under `/mnt/c`: clone into the WSL home instead. |
| Files show as modified right after cloning | Line endings: `git config core.autocrlf false`, then re-clone. |
| `jam: command not found` | `renga-build` builds it into `~/bin`; add `~/bin` to `PATH`. |
| `generated.x86_64 is not configured` | Run `tools/renga/renga-build --configure-only`. |
| QEMU: audio driver `sdl` not found | Install `qemu-system-gui`. |
| UEFI runs fail at start | `ovmf` missing, or its files have other names: pass `--ovmf` and `--ovmf-vars` to the harness. |
| `renga-image` refuses: tree not clean | `build-handover-image` builds images from a clean tree only: commit or stash first. |
| Publish fails "build.txt names no iso" | `build.txt` must start with `iso: <iso>`; `renga-image` writes it. |
| pre-push blocked by renga-guard | A protected path changed without approval, or `git diff --check` found whitespace errors. Fix, or get the architect's approval. |
| Timing checks fail under load | Known-flaky media checks are retried once by `renga-gate`; a busy host (see the Boot section's host load) is not a regression. |
| WSL runs out of memory during the build | Lower `-j` (`renga-build -j 8`) or raise `memory` in `.wslconfig`. |

## 9. Where the cloud-era procedure went

The cloud host's procedure (server restarts, the hourly watchdog, `/home/claude` paths, disk pruning,
starting in a fresh cloud workspace) no longer applies. It is kept verbatim in
[docs/history/handover-batches.md](history/handover-batches.md#cloud-era-procedure-historical).
