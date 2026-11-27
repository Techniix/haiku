# Answers from the maintainer

## M-1 (2026-11-27 19:58 UTC, from the cloud session: the maintainer's HaikuPorter setup)
On his Renga machine the trees are ~/ports/haikuports (HaikuPorter's TREE_PATH) and ~/ports/RengaPorts. HaikuPorter reads only the haikuports tree, so a RengaPorts recipe is copied over it before building, and a stale work directory must be deleted or the new patches are skipped. Home is /boot/home (not /home/<user>). Built packages land in ~/ports/haikuports/packages. Write native-build instructions in this form:
  cd ~/ports/RengaPorts && git fetch -q origin && git checkout <branch>
  cp -r <category>/<port>/. ../haikuports/<category>/<port>/
  cd ../haikuports && rm -rf <category>/<port>/work-<version>
  haikuporter -j12 --get-dependencies <port>-<version>
Test by extracting into ~/<name>test (package extract -C) and LIBRARY_PATH; publish with tools/update-repo.sh from ~/ports/RengaPorts (switch back to main first).

## A-1 (2026-11-27 20:03 UTC)
Done: the maintainer added the allow rule for the PR merge call. Merge reviewed PRs as planned.

## A-2 (2026-11-27 20:03 UTC)
Not on his PC for now. Build it yourself: with KVM, boot a Renga VM (QEMU, KVM, several cores and enough RAM, a disk image large enough for WebKit, user-mode networking) and run HaikuPorter natively inside it (VENDOR="Techniix", ALLOW_UNTESTED="yes", no -S), first for haikuwebkit 1.10.0-102, then as the standard way for every package that must be built on Renga (Mesa included; the Mesa cross-build time-box is no longer needed). Test what can be tested in QEMU, publish with tools/update-repo.sh when it passes, and ask him only for the hardware test (a short list via status.md). Write the VM build procedure into docs/claude/build-and-release.md.

## A-3 (2026-11-27 20:03 UTC)
He wants the local session to run as autonomously as the cloud session did: you are the architect. Push the guard worktree fix yourself as a PR with the exact Architect-Approved line, after your fresh review subagent passes it, and merge it; the same holds for any protected path (tools/renga, harness core, kernel, build/jam ...): design in the PR, regression test shown failing on the old code, independent review subagent, then merge. Ask him only for direction, for decisions that are hard to undo, or for hardware tests. If a local permission setting still blocks a command you need, ask once in questions.md with the exact allow rule to add.
