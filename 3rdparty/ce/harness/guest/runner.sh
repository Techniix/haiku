#!/bin/sh
# Renga test runner (installed only into harness images).
#
# Started by launch_daemon as a system job (harness/guest/ce-harness), right
# after the boot volumes are mounted. It does not depend on app_server or a
# desktop session, so it also works on images without a working GUI.
#
# Everything is written to /dev/dprintf, i.e. the kernel debug output, which
# QEMU's serial port carries to the host harness. Lines starting with CE- are
# parsed by 3rdparty/ce/harness/ce-test.py; anything else is kept as context.

exec >/dev/dprintf 2>&1

echo "CE-HARNESS START $(uname -a)"

# Keep this to tools in every image: the minimum image has no grep.
echo "CE-INFO devices:"
listdev 2>&1
echo "CE-INFO /dev before tests:"
ls -R /dev/audio 2>&1

# What the image is: the boot volume's name and the package repositories it
# comes configured with (ce-test.py checks both for the renga-* profiles).
echo "CE-INFO boot volume:"
df /boot 2>&1
echo "CE-INFO package repositories: $(ls /boot/system/settings/package-repositories 2>&1 | tr '\n' ' ')"
if [ -x /bin/pkgman ]; then
	echo "CE-INFO pkgman list-repos:"
	pkgman list-repos 2>&1
fi

# Give device drivers a moment to finish publishing.
sleep 2

# The screen saver's timer is off for the run: long stretches of tests
# without input must not blank the screen for the tests after them
# (tests/harness_support/ce_screensaver_off.cpp).
if [ -x /boot/system/non-packaged/bin/ce_screensaver_off ]; then
	/boot/system/non-packaged/bin/ce_screensaver_off
fi

# The per-program watchdog (batch 13): a test program still running after
# $suite_timeout seconds is killed, recorded as the failed check
# harness.watchdog.<program>, and the run goes on with the next one. Before
# it, one hung program held the whole run until the host's timeout and every
# program after it was lost. The limit is four times the longest program of
# a renga-raw run (ce-test.py's SUITE_TIMEOUT_DEFAULT); "ce-test.py
# --suite-timeout <s>" puts another one into the image. The watcher wakes
# every 5 s and ends with the program. selftest.py runs this function on the
# host (between the two "watchdog" marker lines) with a program that hangs.
suite_timeout=1200
if [ -f /boot/system/non-packaged/data/ce-harness/suite-timeout ]; then
	suite_timeout=$(cat /boot/system/non-packaged/data/ce-harness/suite-timeout)
fi
# watchdog: begin
run_watched() {
	# $1: the program, $2...: its arguments. Returns its exit status (137
	# when the watchdog killed it).
	watched_name=$(basename "$1")
	"$@" &
	watched_pid=$!
	(
		waited=0
		while kill -0 "$watched_pid" 2>/dev/null; do
			if [ "$waited" -ge "$suite_timeout" ]; then
				echo "CE-TEST harness.watchdog.$watched_name FAIL still running after ${suite_timeout}s, killed"
				kill -9 "$watched_pid" 2>/dev/null
				exit 0
			fi
			sleep 5
			waited=$((waited + 5))
		done
	) &
	watcher_pid=$!
	wait "$watched_pid"
	watched_status=$?
	kill "$watcher_pid" 2>/dev/null
	wait "$watcher_pid" 2>/dev/null
	return $watched_status
}
# watchdog: end

# Every test program installed by 3rdparty/ce/tests runs, one after the other.
# A copy of the image made by "ce-test.py --only" carries a selection file
# (one program name per line); then only those run, and the host labels the
# run PARTIAL. Built images never have the file.
selection=/boot/system/non-packaged/data/ce-harness/only
if [ -f "$selection" ]; then
	echo "CE-INFO selection: $(tr '\n' ' ' < "$selection")"
fi

# Crash reports (crashlog_test, roadmap F-51) span several boots of the same
# QEMU session: the end of a run panics on purpose, the harness answers the
# kernel debugger with "reboot", and the boots after it only check what the
# crash left (the test keeps its phase in this file).
final_tests=/boot/system/non-packaged/bin/ce-tests-final
crash_phase=/boot/system/non-packaged/data/ce-harness/crash-phase
if [ -f "$crash_phase" ] && [ -x "$final_tests/crashlog_test" ]; then
	echo "CE-INFO crash phase $(cat "$crash_phase"): only crashlog_test runs"
	echo "CE-INFO running crashlog_test"
	run_watched "$final_tests/crashlog_test"
	echo "CE-INFO crashlog_test exited with $?"
	echo "CE-HARNESS END"
	sync
	shutdown -q
	exit 0
fi

for test in /boot/system/non-packaged/bin/ce-tests/*; do
	if [ -f "$selection" ]; then
		wanted=no
		while read -r name; do
			[ "$name" = "$(basename "$test")" ] && wanted=yes
		done < "$selection"
		[ "$wanted" = yes ] || continue
	fi
	if [ -x "$test" ]; then
		echo "CE-INFO running $(basename "$test")"
		run_watched "$test"
		echo "CE-INFO $(basename "$test") exited with $?"
	fi
done

# Second phase: the Media Kit tests, with the media services running. The
# harness job keeps media_server from starting at boot (see ce-harness), so
# that the tests above have the sound cards to themselves; it is started
# here, by hand, once they are done. The tests stop it again when they end.
# Only images with the Media Kit (the renga-* profiles) can run them; the
# minimum images have no media_server or libmedia.
media_tests=/boot/system/non-packaged/bin/ce-tests-media
started_media=no
if [ ! -x /boot/system/servers/media_server ]; then
	echo "CE-INFO no Media Kit in this image, media tests skipped"
	media_tests=/nonexistent
fi
for test in "$media_tests"/*; do
	[ -x "$test" ] || continue
	if [ -f "$selection" ]; then
		wanted=no
		while read -r name; do
			[ "$name" = "$(basename "$test")" ] && wanted=yes
		done < "$selection"
		[ "$wanted" = yes ] || continue
	fi
	if [ "$started_media" = no ]; then
		echo "CE-INFO starting media_server"
		/boot/system/servers/media_server &
		started_media=yes
	fi
	echo "CE-INFO running $(basename "$test")"
	run_watched "$test"
	echo "CE-INFO $(basename "$test") exited with $?"
done

# Last: the tests that end the boot (ce-tests-final). crashlog_test --crash
# checks what it can before a crash, then panics; the next boot runs it again
# (above). If it returns, the panic did not happen; its checks say so.
if [ -f /boot/system/non-packaged/data/ce-harness/no-final-tests ]; then
	echo "CE-INFO tests that end the boot skipped (no-final-tests)"
	final_tests=/nonexistent
fi
for test in "$final_tests"/*; do
	[ -x "$test" ] || continue
	if [ -f "$selection" ]; then
		wanted=no
		while read -r name; do
			[ "$name" = "$(basename "$test")" ] && wanted=yes
		done < "$selection"
		[ "$wanted" = yes ] || continue
	fi
	echo "CE-INFO crash phase starts: $(basename "$test")"
	run_watched "$test" --crash
	echo "CE-INFO $(basename "$test") returned from its crash phase with $?"
done

echo "CE-INFO /dev after tests:"
ls -R /dev/audio 2>&1
echo "CE-HARNESS END"
sync
shutdown -q
