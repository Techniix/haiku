# Questions for the maintainer

## Q-1 (2026-11-27 19:34 UTC)
This PC's Claude Code permission settings block the PR merge call (`gh api -X PUT repos/Techniix/haiku/pulls/N/merge`), so reviewed PRs cannot be merged from here. Haiku PR #28 (the pointer.usb.pos.top_left test-order fix, reviewed as sound, QEMU Tested) is waiting.
Options: A) add a Bash allow rule for `gh api -X PUT repos/Techniix/haiku/pulls/*/merge -f merge_method=merge` (in .claude/settings.local.json or via /permissions) / B) you merge each reviewed PR yourself on GitHub (merge commit).
Recommendation: A, so the batch flows as it did in the cloud.
If no answer by: the end of batch 14, the session keeps opening reviewed PRs and lists the ones waiting for you in status.md.
