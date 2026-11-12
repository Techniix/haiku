#!/bin/sh
# ce-gates.sh: run a set of harness configurations as a resumable gate.
#
# Usage:
#   3rdparty/ce/harness/ce-gates.sh <label> <run> [<run> ...]
#
# Each <run> is one quoted string of ce-test.py arguments; "" is the default
# configuration. Example, the full four-way set of roadmap section 1.2:
#
#   3rdparty/ce/harness/ce-gates.sh f59-capture "" "--smp 2" \
#       "--firmware uefi" "--profile renga-raw"
#
# Why: the build host restarts every few hours and kills running test runs.
# This script records every finished configuration under a key made from
# the exact tree being tested (HEAD, the uncommitted diff and the untracked
# files). Run it again after a restart with the same arguments and it skips
# the configurations that already passed on the same tree, so only the
# unfinished ones run again. Any change to the tree gives a new key, so a
# result is never reused for code it did not test.
#
# selftest.py runs once per tree key. A failing configuration whose failed
# checks are all on the known-flaky list (roadmap 1.2; KNOWN_FLAKY below) is
# retried once, as the rule says, and both results are recorded.
#
# Results: generated.x86_64/ce-gates/<key>/, one file per configuration,
# plus summary.txt; the script prints the summary and exits 0 only if
# selftest and every configuration passed.

set -u

KNOWN_FLAKY='media_latency\.low_latency_latency|media\.click_free_volume|sound_settings\.output_device_back|linuxkpi\.edu_dma_|linuxkpi\.edu_|linuxkpi\.reload_results|linuxkpi\.no_errors|media_drift\.skew_|media_audio\.route_click_free|media_routing\.exclude_cleared'

[ $# -ge 2 ] || { sed -n '2,12p' "$0" >&2; exit 2; }
label=$1; shift

top=$(git rev-parse --show-toplevel) || exit 2
cd "$top" || exit 2
harness=3rdparty/ce/harness
results=generated.x86_64/ce-test-results

# The key of the tree under test.
key=$( {
	git rev-parse HEAD
	git diff HEAD --binary
	git ls-files --others --exclude-standard -z | sort -z \
		| xargs -0 -r sha256sum
} | sha256sum | cut -c1-16)
state=generated.x86_64/ce-gates/$key
mkdir -p "$state"
echo "$label" > "$state/label"
echo "ce-gates: $label, tree key $key"

ok=1

# selftest, once per tree.
if [ -f "$state/selftest.pass" ]; then
	echo "selftest: passed earlier on this tree, skipped"
else
	if python3 $harness/selftest.py > "$state/selftest.log" 2>&1; then
		touch "$state/selftest.pass"
		echo "selftest: passed"
	else
		echo "selftest: FAILED ($(tail -1 "$state/selftest.log"))"
		ok=0
	fi
fi

run_once() {
	# $1: argument string. Prints the result directory.
	# shellcheck disable=SC2086
	$harness/ce-test.py $1 > /dev/null 2>&1
	readlink -f $results/latest
}

passed() {
	# $1: summary.md; PASS, or a partial run whose selection passed.
	grep -q -m1 -e '^\*\*Verdict: PASS\*\*' \
		-e '^\*\*Verdict: PARTIAL, selection passed\*\*' "$1"
}

only_flaky() {
	# $1: summary.md; true if it has failures and all are known flaky.
	fails=$(grep -o '^| [^|]* | \*\*FAIL\*\*' "$1" \
		| sed 's/^| \([^ ]*\) .*/\1/')
	[ -n "$fails" ] || return 1
	echo "$fails" | grep -v -E "^($KNOWN_FLAKY)" | grep -q . && return 1
	return 0
}

for args in "$@"; do
	name=$(printf '%s' "${args:-default}" | tr -c 'A-Za-z0-9._-' '_')
	if [ -f "$state/$name.pass" ]; then
		echo "[$args]: passed earlier on this tree ($(cat "$state/$name.pass")), skipped"
		continue
	fi
	dir=$(run_once "$args")
	verdict=$(grep -m1 'Verdict' "$dir/summary.md" 2>/dev/null)
	if ! passed "$dir/summary.md" 2>/dev/null \
		&& only_flaky "$dir/summary.md" 2>/dev/null; then
		echo "[$args]: only known-flaky checks failed in $(basename "$dir"), retrying once"
		echo "first run $(basename "$dir"): $verdict" >> "$state/$name.retries"
		dir=$(run_once "$args")
		verdict=$(grep -m1 'Verdict' "$dir/summary.md" 2>/dev/null)
	fi
	if passed "$dir/summary.md" 2>/dev/null; then
		echo "$(basename "$dir") $verdict" > "$state/$name.pass"
		echo "[$args]: $verdict ($(basename "$dir"))"
	else
		echo "$(basename "$dir") $verdict" > "$state/$name.fail"
		echo "[$args]: ${verdict:-no summary} ($(basename "$dir"))"
		grep '| \*\*FAIL\*\*' "$dir/summary.md" 2>/dev/null | cut -c1-200
		ok=0
	fi
done

{
	echo "$label, tree key $key, $(date -u +%Y-%m-%dT%H:%MZ)"
	[ -f "$state/selftest.pass" ] && echo "selftest: passed" || echo "selftest: FAILED"
	for args in "$@"; do
		name=$(printf '%s' "${args:-default}" | tr -c 'A-Za-z0-9._-' '_')
		if [ -f "$state/$name.pass" ]; then
			echo "[$args] $(cat "$state/$name.pass")"
		elif [ -f "$state/$name.fail" ]; then
			echo "[$args] FAILED: $(cat "$state/$name.fail")"
		else
			echo "[$args] not run"
		fi
		[ -f "$state/$name.retries" ] && sed 's/^/    retried after /' "$state/$name.retries"
	done
} > "$state/summary.txt"
echo
cat "$state/summary.txt"
[ $ok = 1 ]
