# Architect briefs and reviews

Load this when writing a brief (architect), starting work from one (implementation session) or reviewing a
pull request. A brief is self-contained: an implementation session reads `CLAUDE.md`, the brief and the
files the brief names, and nothing else by default.

## A brief contains

1. **Title and branch.** One line, and the branch to work on (`batch/<NN>-<topic>`, `fix/<topic>`,
   `ai/local/<topic>`), cut from current `origin/ce/main` unless stated.
2. **Goal.** What the user or the partner gets, in two or three sentences. The roadmap F-entry and, where it
   applies, the decisions-log row (date) that asked for it.
3. **Read first.** The exact files and sections: roadmap F-section and the newest work order, the relevant
   `docs/claude/*.md`, source files or directories, PORTING.md sections, the partner request by number.
   No "read the roadmap".
4. **Scope.** The pieces, in order, each small enough for one commit. For each: the files to change, the
   design decided by the architect (interfaces, data formats, settings names and defaults, where the GUI
   switch lives), and what is explicitly out of scope.
5. **Constraints.** Add-only ABI; GUI and on/off setting for user-facing features; HVIF icon for new
   programs; protected paths the brief allows (the PR then carries `Architect-Approved: yes`) and those it
   does not; anything that must not change.
6. **Tests.** Per piece: the suites to run (`renga-test <suites> --profile ...`), the new regression or
   feature checks to add and in which suite, the known-bad case for new host-side checks
   (`selftest.py`), and whether a boot run or the four-way set is needed.
7. **Done when.** Concrete acceptance: checks passing, screenshots taken, numbers to report.
8. **Report format.** What the session hands back: commits (hash, one line each), `renga-summary` output
   per run, failing or skipped checks with reasons, open questions, anything not done. No log excerpts
   beyond a few lines.

Keep a brief to about one screen. If it needs more, split it into two briefs.

## An implementation session

- Works only on the brief's branch; commits each piece with explicit paths, a very detailed message
  (what, why, test tier and numbers) and the trailers from the standing rules.
- Runs the suites itself with `tools/renga/renga-test` and reads only `renga-summary` output.
- Stops and asks when the brief is wrong, when a protected path not allowed by the brief needs a change, or
  when a design decision is missing. It does not decide architecture.
- Pushes its branch and opens a pull request to `ce/main` with the gate summary; it never merges.

## A review contains

The architect reads, in this order and no more than needed:

1. `git diff --stat origin/ce/main...<branch>` and the PR description (brief reference, gate summary,
   `Architect-Approved:` line).
2. `tools/renga/renga-guard origin/ce/main`, `tools/renga/renga-changed-suites origin/ce/main` and, when
   libraries changed, `tools/renga/renga-abi-check`.
3. The gate results: `renga-summary` of the suite runs and, for a batch branch, of the four-way gate.
4. The hunks that matter: protected paths, public headers, locking, error paths, settings and defaults.
   Not generated files, not unchanged context.

The review answers, in a short PR comment:

- **Scope:** does the branch do what the brief asked, and nothing it did not?
- **Rules:** ABI add-only; GUI and switch; icon; explicit-path commits with detailed messages and trailers;
  docs professional; roadmap status line; regression test named for fixes; QEMU Tested versus Hardware
  Tested stated correctly.
- **Tests:** the right suites ran and passed; flaky retries reported; nothing dropped silently.
- **Risk:** what could break elsewhere, and whether the four-way set is needed before merging.
- **Verdict:** approve (then merge with a merge commit), or the list of changes needed. Protected-path
  approval is given by editing the PR description to `Architect-Approved: yes`.
