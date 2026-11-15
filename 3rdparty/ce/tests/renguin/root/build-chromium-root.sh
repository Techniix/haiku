#!/bin/sh
# Builds the optional Chromium layer of the harness's Linux root (roadmap
# F-27, "Linux Chrome under Renguin"):
#
#   build-chromium-root.sh <output.tar.gz> <pins file> <download cache dir>
#       <test page dir>
#
# Alpine's chromium with every package it needs to start (chromium.pins,
# pinned by version and SHA-256 like alpine.pins; about 230 MB to download),
# extracted on top of the regular Linux root (build-root.sh) in harness
# images built with "ce-test.py --linux-chromium" only: it adds about
# 500 MB, too much for every run. Nothing of it is kept in the git tree.
#
# As in build-root.sh: absolute symbolic links become relative ones, and
# /tmp and /var/tmp are left out (Linux programs share Haiku's /tmp). Also:
#  - only the en-US locale of Chromium's and other packages' translations,
#    and no documentation or man pages;
#  - fontconfig's cache directory exists (/var/cache/fontconfig);
#  - the test pages from <test page dir> go to /opt/renguin-data/chrome.
set -e

output=$1
pins=$2
cache=$3
pages=$4

mirror=${RENGUIN_ALPINE_MIRROR:-https://dl-cdn.alpinelinux.org/alpine}
branch=$(sed -n 's/^branch //p' "$pins")
mkdir -p "$cache"

work=$(mktemp -d "${TMPDIR:-/tmp}/renguin-chromium.XXXXXX")
trap 'rm -rf "$work"' EXIT
root=$work/root
mkdir -p "$root"

grep -v '^#' "$pins" | grep -v '^branch ' | while read -r kind file sum; do
	[ -n "$kind" ] || continue
	if [ ! -f "$cache/$file" ]; then
		wget -q --timeout 60 -O "$cache/$file.part" \
			"$mirror/$branch/$kind/x86_64/$file" || {
			rm -f "$cache/$file.part"
			echo "build-chromium-root.sh: cannot fetch $file (Alpine" \
				"replaced it? update chromium.pins)" >&2
			exit 1
		}
		mv "$cache/$file.part" "$cache/$file"
	fi
	if [ "$(sha256sum "$cache/$file" | cut -d' ' -f1)" != "$sum" ]; then
		echo "build-chromium-root.sh: $file does not match its pinned" \
			"SHA-256" >&2
		rm -f "$cache/$file"
		exit 1
	fi
	tar -C "$root" -xzf "$cache/$file" --warning=no-unknown-keyword \
		2>/dev/null || tar -C "$root" -xzf "$cache/$file"
	rm -f "$root"/.PKGINFO "$root"/.SIGN.* "$root"/.pre-* "$root"/.post-* \
		"$root"/.trigger
done

# Only en-US translations; no documentation.
if [ -d "$root/usr/lib/chromium/locales" ]; then
	find "$root/usr/lib/chromium/locales" -type f ! -name 'en-US*' -delete
fi
rm -rf "$root/usr/share/doc" "$root/usr/share/man" "$root/usr/share/info" \
	"$root/usr/share/gtk-doc"
if [ -d "$root/usr/share/locale" ]; then
	find "$root/usr/share/locale" -mindepth 1 -maxdepth 1 ! -name 'en*' \
		-exec rm -rf {} +
fi

python3 -I -c '
import os, sys
root = sys.argv[1]
for directory, dirs, files in os.walk(root):
    for name in dirs + files:
        path = os.path.join(directory, name)
        if not os.path.islink(path):
            continue
        target = os.readlink(path)
        if not target.startswith("/"):
            continue
        linkDir = "/" + os.path.relpath(directory, root)
        os.unlink(path)
        os.symlink(os.path.relpath(target, os.path.normpath(linkDir)), path)
' "$root"

rm -rf "$root/tmp" "$root/var/tmp"
mkdir -p "$root/var/cache/fontconfig" "$root/opt/renguin-data/chrome"
cp "$pages"/* "$root/opt/renguin-data/chrome/"

tar -C "$root" --owner=0 --group=0 --numeric-owner -czf "$output.tmp" .
mv "$output.tmp" "$output"
