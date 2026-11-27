# Questions for the maintainer

## Q-1 (2026-11-27 19:34 UTC)
This PC's Claude Code permission settings block the PR merge call (`gh api -X PUT repos/Techniix/haiku/pulls/N/merge`), so reviewed PRs cannot be merged from here. Haiku PR #28 (the pointer.usb.pos.top_left test-order fix, reviewed as sound, QEMU Tested) is waiting.
Options: A) add a Bash allow rule for `gh api -X PUT repos/Techniix/haiku/pulls/*/merge -f merge_method=merge` (in .claude/settings.local.json or via /permissions) / B) you merge each reviewed PR yourself on GitHub (merge commit).
Recommendation: A, so the batch flows as it did in the cloud.
If no answer by: the end of batch 14, the session keeps opening reviewed PRs and lists the ones waiting for you in status.md.

## Q-2 (2026-11-27 19:44 UTC)
haikuwebkit 1.10.0-102 is ready for your native build: RengaPorts PR #1 (branch batch/14-haikuwebkit-102, head 733f346). (a) the media data source now blocks until a requested range has downloaded (libcurl Range requests, a 200 answer read from the start); (b) video frames are shown by PTS against the audio clock, late frames dropped, one frame at least every 250 ms. Reviewed: patches apply, the new data source compiles with Renga's cross g++ without warnings, no deadlock found; the whole of WebKit was not compiled here.
Commands on Renga: (haikuports.conf as for -101, with VENDOR="Techniix" and ALLOW_UNTESTED="yes")
  cd ~/RengaPorts && git fetch origin && git checkout batch/14-haikuwebkit-102
  haikuporter --get-dependencies haikuwebkit      (no -S)
  mkdir -p ~/haikuwebkittest && cd ~/haikuwebkittest && package extract ~/RengaPorts/packages/haikuwebkit-1.10.0-102-x86_64.hpkg
  Quit WebPositive (it is single-launch), then: LIBRARY_PATH=/home/Techniix/haikuwebkittest/lib: /boot/system/apps/WebPositive
Try an H.264+AAC MP4 and a WebM page: speed, A/V sync, seeking, leaving the page mid-load. Report back: good / still choppy or fast / crash (crash report into relay/files/).
Options: A) build and test now / B) later.
Recommendation: A; if good, the session merges PR #1 and you publish with tools/update-repo.sh (haikuwebkit and haikuwebkit_devel -102).
If no answer by: the end of batch 14, it stays an open PR and item 4 is listed as waiting for the hardware build.

## Q-3 (2026-11-27 19:46 UTC)
renga-guard blocks every push from a linked git worktree (it looks for tools/renga/tools/renga/protected-paths: Git hooks set GIT_DIR, which shifts the root). Fix ready as a local commit on branch infra/guard-worktree (in ~/renga/wtguard.IQHC): lib.sh unsets GIT_DIR/GIT_WORK_TREE, plus a new tools/renga/selftest whose check fails on the old scripts and passes on the new. It changes the guard itself, and this PC's permission settings (rightly) stop the session from pushing it under its own Architect-Approved line.
Options: A) you push it and open the PR (cd ~/renga/wtguard.IQHC && git push origin HEAD:infra/guard-worktree) / B) leave it; the session pushes worktree commits from the main repository by SHA, which works.
Recommendation: A when convenient; B meanwhile.
If no answer by: the end of batch 14, B, and the commit stays local.
