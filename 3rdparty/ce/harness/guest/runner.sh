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

# Every test program installed by 3rdparty/ce/tests runs, one after the other.
for test in /boot/system/non-packaged/bin/ce-tests/*; do
	if [ -x "$test" ]; then
		echo "CE-INFO running $(basename "$test")"
		"$test"
		echo "CE-INFO $(basename "$test") exited with $?"
	fi
done

echo "CE-INFO /dev after tests:"
ls -R /dev/audio 2>&1
echo "CE-HARNESS END"
sync
shutdown -q
