#!/bin/sh
# Builds the Linux root ("/compat/linux") that harness images carry for
# Renguin's dynamic Linux programs (roadmap F-27).
#
#   build-root.sh <output.tar.gz> <host cc> <dyn source dir> <pins file>
#       <download cache dir>
#
# The root is Alpine Linux's minirootfs (musl and busybox, about 3.7 MB),
# pinned by version and SHA-256 in alpine.pins, plus a few Alpine packages
# (GNU coreutils with the libraries it needs, musl-dev only to link the test
# programs below; musl-dev itself is not installed into the root). Nothing of
# it is kept in the git tree: the build downloads the pinned files.
#
# On top of that:
#  - Absolute symbolic links (busybox's applets point at /bin/busybox) are
#    rewritten as relative ones. Renguin's own path lookup resolves absolute
#    links inside the Linux root, but Haiku programs (Tracker, a native
#    shell starting /compat/linux/bin/ls) see the links through Haiku's VFS,
#    which would resolve them against Haiku's "/".
#  - /tmp and /var/tmp are left out, so that Linux programs share Haiku's
#    /tmp (sockets such as the Wayland socket, files handed between native
#    and Linux programs); Renguin falls back to Haiku's tree for paths the
#    root does not have.
#  - /etc/resolv.conf links to Haiku's own network settings
#    (/boot/system/settings/network/resolv.conf), so Linux programs use the
#    name servers Haiku's network preferences or DHCP set, live. Renguin
#    resolves link targets under /boot/system and /boot/home on Haiku's
#    volume (path.cpp). /etc/hosts is Alpine's (localhost for IPv4 and IPv6).
#  - The test programs from <dyn source dir>, linked dynamically: against
#    musl (Alpine's own ld-musl, with musl-dev's start files and headers),
#    and against the build host's glibc, whose loader and libc are copied
#    into /lib/x86_64-linux-gnu and /lib64 (the glibc files exist only in
#    harness images, which are never distributed; Alpine's root has no glibc
#    of its own).
set -e

output=$1
hostCC=$2
dynSources=$3
pins=$4
cache=$5

mirror=${RENGUIN_ALPINE_MIRROR:-https://dl-cdn.alpinelinux.org/alpine}
branch=$(sed -n 's/^branch //p' "$pins")
mkdir -p "$cache"

# fetch <kind> <file> <sha256>: a pinned file into the cache, checked.
fetch()
{
	kind=$1 file=$2 sum=$3
	if [ "$kind" = release ]; then
		url=$mirror/$branch/releases/x86_64/$file
	else
		url=$mirror/$branch/$kind/x86_64/$file
	fi
	if [ ! -f "$cache/$file" ]; then
		if ! wget -q --timeout 30 -O "$cache/$file.part" "$url"; then
			rm -f "$cache/$file.part"
			[ "$kind" = release ] && { echo "cannot fetch $url" >&2; exit 1; }
			# Replaced in the branch: take its current version.
			name=$(echo "$file" | sed 's/-[0-9][^-]*-r[0-9]*\.apk$//')
			[ -f "$cache/APKINDEX-$kind" ] || {
				wget -q --timeout 30 -O "$cache/APKINDEX-$kind.tar.gz" \
					"$mirror/$branch/$kind/x86_64/APKINDEX.tar.gz"
				tar -xzf "$cache/APKINDEX-$kind.tar.gz" -O APKINDEX \
					> "$cache/APKINDEX-$kind"
			}
			version=$(awk -v p="$name" 'BEGIN { RS = ""; FS = "\n" }
				{ n = ""; v = ""; for (i = 1; i <= NF; i++) {
					if ($i ~ /^P:/) n = substr($i, 3);
					if ($i ~ /^V:/) v = substr($i, 3) }
				  if (n == p) print v }' "$cache/APKINDEX-$kind")
			[ -n "$version" ] || { echo "no $name in $branch" >&2; exit 1; }
			echo "build-root.sh: $file is gone from Alpine $branch;" \
				"using $name-$version (update alpine.pins)" >&2
			file=$name-$version.apk
			wget -q --timeout 30 -O "$cache/$file" \
				"$mirror/$branch/$kind/x86_64/$file" || exit 1
			fetched=$cache/$file
			return
		fi
		mv "$cache/$file.part" "$cache/$file"
	fi
	if [ "$(sha256sum "$cache/$file" | cut -d' ' -f1)" != "$sum" ]; then
		echo "build-root.sh: $file does not match its pinned SHA-256" >&2
		rm -f "$cache/$file"
		exit 1
	fi
	fetched=$cache/$file
}

work=$(mktemp -d "${TMPDIR:-/tmp}/renguin-root.XXXXXX")
trap 'rm -rf "$work"' EXIT
root=$work/root
dev=$work/musl-dev
mkdir -p "$root" "$dev"

grep -v '^#' "$pins" | grep -v '^branch ' | while read -r kind file sum; do
	[ -n "$kind" ] || continue
	fetch "$kind" "$file" "$sum"
	echo "$kind $fetched"
done > "$work/files"

# The minirootfs first, then the packages. Alpine packages (.apk) are
# gzipped tar streams with a signature and a control segment first; GNU tar
# reads all of them.
while read -r kind file; do
	case "$kind:$(basename "$file")" in
		release:*)		tar -C "$root" -xzf "$file"; continue ;;
		*:musl-dev-*)	target=$dev ;;
		*)				target=$root ;;
	esac
	tar -C "$target" -xzf "$file" --warning=no-unknown-keyword 2>/dev/null \
		|| tar -C "$target" -xzf "$file"
	rm -f "$target"/.PKGINFO "$target"/.SIGN.* "$target"/.pre-* \
		"$target"/.post-* "$target"/.trigger
done < "$work/files"

# Relative links instead of absolute ones (see above).
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
rm -f "$root/etc/resolv.conf"
ln -s /boot/system/settings/network/resolv.conf "$root/etc/resolv.conf"

# Dynamic test programs.
mkdir -p "$root/opt/renguin-tests"
gccInclude=$($hostCC -print-file-name=include)
libgcc=$($hostCC -print-libgcc-file-name)
for source in "$dynSources"/*.c; do
	name=$(basename "$source" .c)
	$hostCC -O2 -fno-stack-protector -nostdinc -isystem "$dev/usr/include" \
		-isystem "$gccInclude" -nostdlib -no-pie -o \
		"$root/opt/renguin-tests/$name-musl" \
		"$dev/usr/lib/crt1.o" "$dev/usr/lib/crti.o" "$source" \
		"$root/lib/ld-musl-x86_64.so.1" "$libgcc" "$dev/usr/lib/crtn.o" \
		-Wl,-dynamic-linker,/lib/ld-musl-x86_64.so.1 -Wl,--build-id=none
	$hostCC -O2 -Wno-unused-result -pthread -o "$root/opt/renguin-tests/$name-glibc" "$source"
done

# The host's glibc loader and libraries for the -glibc programs.
interp=$(readelf -l "$root/opt/renguin-tests/hello-glibc" \
	| sed -n 's/.*Requesting program interpreter: \(.*\)]/\1/p')
mkdir -p "$root/lib/x86_64-linux-gnu" "$root/lib64"
for lib in $(ldd "$root"/opt/renguin-tests/*-glibc | sed -n 's/.*=> \(\/[^ ]*\) .*/\1/p' | sort -u); do
	cp -L "$lib" "$root/lib/x86_64-linux-gnu/"
done
cp -L "$interp" "$root/lib/x86_64-linux-gnu/$(basename "$interp")"
ln -sfn "../lib/x86_64-linux-gnu/$(basename "$interp")" \
	"$root/lib64/$(basename "$interp")"

# Owned by root, as on Linux; the archive keeps modes and links.
tar -C "$root" --owner=0 --group=0 --numeric-owner -czf "$output.tmp" .
mv "$output.tmp" "$output"
