# Shared helpers for the tools/renga scripts. Sourced, not run.
# shellcheck shell=sh

# The calling script, as an absolute path (taken before the cd below).
renga_self=$(cd "$(dirname "$0")" && pwd)/$(basename "$0")

# Every script runs from the root of the Renga tree, wherever it was called.
# Git hooks run with GIT_DIR set (and, in a linked worktree, without
# GIT_WORK_TREE), which makes "rev-parse --show-toplevel" answer the
# current directory: from the pre-push hook in a worktree the root came out
# as <tree>/tools/renga. The scripts find the repository from the tree
# itself, so these are dropped.
unset GIT_DIR GIT_WORK_TREE
renga_root=$(git -C "$(dirname "$renga_self")" rev-parse --show-toplevel 2>/dev/null) || {
	echo "$(basename "$0"): not inside the Renga git tree" >&2
	exit 2
}
cd "$renga_root" || exit 2

# The configured build output directory (ReadMe.Compiling.md; harness
# default). HAIKU_OUTPUT_DIR overrides it, as for build-handover-image.
# Used by the scripts that source this file.
# shellcheck disable=SC2034
RENGA_OUTPUT=${HAIKU_OUTPUT_DIR:-$renga_root/generated.x86_64}
# shellcheck disable=SC2034
RENGA_RESULTS=$RENGA_OUTPUT/ce-test-results
# shellcheck disable=SC2034
RENGA_TOOLS=$renga_root/tools/renga

renga_die() {
	echo "$(basename "$0"): $*" >&2
	exit 1
}

renga_note() {
	echo "$(basename "$0"): $*" >&2
}

# Prints the --help text: the comment block at the top of the script,
# from its second line up to the first line that is not a comment.
renga_usage() {
	awk 'NR == 1 { next } /^#/ { sub(/^# ?/, ""); print; next } { exit }' "$renga_self"
}
