#!/bin/sh
# Builds the Debian root that harness images carry beside the Alpine one
# (roadmap F-27, Steam groundwork; docs/design/flatpak-and-steam-under-
# renguin.md, "Steam", items 2 and 3, 64-bit only):
#
#   build-debian-root.sh <output.tar.gz> <host cc> <pins file>
#       <download cache dir> <test source dir> <layers...>
#
# A glibc-based x86_64 root: Debian 13 "trixie" packages pinned by pool path
# and SHA-256 in debian.pins (update-debian-pins.py resolves them), unpacked
# without dpkg (no maintainer scripts run). <layers> is "base" (glibc,
# libstdc++, dash and SDL2 with the libraries it links; about 15 MB to
# download, 50 MB unpacked) or "base gl" (also Mesa's EGL and GLES with
# llvmpipe and LLVM, and desktop GL through libglvnd; about 40 MB more to
# download, 200 MB unpacked: only
# the renga-* harness images have the room). Nothing of it is kept in the
# git tree; the files exist only in harness images, which are never
# distributed.
#
# The image extracts the archive at /boot/system/non-packaged/compat/
# linux-roots/debian. Renguin's fixed Linux root stays Alpine's; a program
# runs in the Debian root through chroot() (batch 14), for example
#   /compat/linux/bin/busybox chroot <that directory> /opt/renguin-tests/x
#
# Besides the packages:
#  - merged /usr, as Debian 13 has it (/bin, /sbin, /lib and /lib64 are
#    relative links into /usr; the packages are unpacked through them);
#  - what dpkg's maintainer scripts would make: /etc/passwd, /etc/group
#    and /etc/shadow (root and the Renga user 1000), /etc/nsswitch.conf,
#    /etc/hosts, /etc/hostname; no /etc/ld.so.cache (renguin_test runs the
#    root's ldconfig and checks the loader with and without it);
#  - as in build-root.sh: absolute links become relative ones, /tmp and
#    /var/tmp are left out (Linux programs share Haiku's /tmp: the Wayland
#    socket), /etc/resolv.conf links to Haiku's network settings;
#  - no documentation, man pages or translations;
#  - the test programs from <test source dir> in /opt/renguin-tests, built
#    by the host compiler against the root's own glibc, libstdc++ and
#    libraries (a sysroot of the root plus the "dev" layer's headers and
#    link files, which are not installed). gl-check only with "gl".
set -e

output=$1
hostCC=$2
pins=$3
cache=$4/debian
sources=$5
shift 5
layers="$*"

mirror=${RENGUIN_DEBIAN_MIRROR:-https://deb.debian.org/debian}
snapshot=$(sed -n 's/^snapshot //p' "$pins")
mkdir -p "$cache"

work=$(mktemp -d "${TMPDIR:-/tmp}/renguin-debian.XXXXXX")
trap 'rm -rf "$work"' EXIT
root=$work/root
sysroot=$work/sysroot

# fetch <pool path> <sha256>: a pinned file into the cache, checked. The
# mirror has a stable release's current packages only; after a point
# release replaced one, snapshot.debian.org still has the pinned file.
fetch()
{
	path=$1 sum=$2
	file=$cache/$(basename "$path")
	if [ ! -f "$file" ]; then
		if ! wget -q --timeout 60 -O "$file.part" "$mirror/$path"; then
			echo "build-debian-root.sh: $path is not on $mirror; trying" \
				"snapshot.debian.org ($snapshot; update debian.pins)" >&2
			wget -q --timeout 120 -O "$file.part" \
				"https://snapshot.debian.org/archive/debian/$snapshot/$path" \
				|| { rm -f "$file.part"; echo "cannot fetch $path" >&2;
					exit 1; }
		fi
		mv "$file.part" "$file"
	fi
	if [ "$(sha256sum "$file" | cut -d' ' -f1)" != "$sum" ]; then
		echo "build-debian-root.sh: $(basename "$path") does not match its" \
			"pinned SHA-256" >&2
		rm -f "$file"
		exit 1
	fi
	fetched=$file
}

# unpack <.deb> <directory>: the data member, through the merged-/usr links.
unpack()
{
	member=$(ar t "$1" | grep '^data\.tar')
	case "$member" in
		*.xz)	decompress="xz -dc" ;;
		*.zst)	decompress="zstd -dc" ;;
		*.gz)	decompress="gzip -dc" ;;
		*)		decompress=cat ;;
	esac
	ar p "$1" "$member" | $decompress \
		| tar -C "$2" -x --keep-directory-symlink --no-same-owner -f -
}

# merged_usr <directory>
merged_usr()
{
	mkdir -p "$1/usr/bin" "$1/usr/sbin" "$1/usr/lib" "$1/usr/lib64"
	for link in bin sbin lib lib64; do
		ln -s "usr/$link" "$1/$link"
	done
}

merged_usr "$root"
for layer in $layers dev; do
	grep "^$layer " "$pins" | while read -r kind path sum; do
		fetch "$path" "$sum"
		echo "$fetched"
	done > "$work/$layer.files"
	[ "$layer" = dev ] && continue
	while read -r file; do
		unpack "$file" "$root"
	done < "$work/$layer.files"
done

# What the maintainer scripts would have made.
cat > "$root/etc/passwd" <<'EOF'
root:x:0:0:root:/root:/bin/sh
user:x:1000:1000:Renga user:/home/user:/bin/sh
nobody:x:65534:65534:nobody:/nonexistent:/usr/sbin/nologin
EOF
cat > "$root/etc/group" <<'EOF'
root:x:0:
users:x:100:user
user:x:1000:
nogroup:x:65534:
EOF
printf 'root:*:20000:0:99999:7:::\nuser:*:20000:0:99999:7:::\n' \
	> "$root/etc/shadow"
chmod 640 "$root/etc/shadow"
cat > "$root/etc/nsswitch.conf" <<'EOF'
passwd:         files
group:          files
shadow:         files
hosts:          files dns
networks:       files
protocols:      files
services:       files
EOF
printf '127.0.0.1\tlocalhost\n::1\t\tlocalhost ip6-localhost\n' \
	> "$root/etc/hosts"
echo renga > "$root/etc/hostname"
mkdir -p "$root/root" "$root/home/user"

rm -rf "$root/usr/share/doc" "$root/usr/share/man" "$root/usr/share/info" \
	"$root/usr/share/lintian" "$root/usr/share/locale" "$root/tmp" \
	"$root/var/tmp"
rm -f "$root/etc/resolv.conf"
ln -s /boot/system/settings/network/resolv.conf "$root/etc/resolv.conf"

# Relative links instead of absolute ones (see build-root.sh); the
# resolv.conf link to Haiku's file stays.
python3 -I -c '
import os, sys
root = sys.argv[1]
for directory, dirs, files in os.walk(root):
    for name in dirs + files:
        path = os.path.join(directory, name)
        if not os.path.islink(path):
            continue
        target = os.readlink(path)
        if not target.startswith("/") or target.startswith("/boot/"):
            continue
        linkDir = "/" + os.path.relpath(directory, root)
        os.unlink(path)
        os.symlink(os.path.relpath(target, os.path.normpath(linkDir)), path)
' "$root"

# The sysroot for the test programs: the root, plus the dev layer.
cp -a "$root" "$sysroot"
while read -r file; do
	unpack "$file" "$sysroot"
done < "$work/dev.files"

tests=$root/opt/renguin-tests
mkdir -p "$tests"
gccVersion=$(ls "$sysroot/usr/include/c++" | sort -n | tail -1)
# The host compiler with the root's headers and libraries only: its own
# headers (stddef.h and friends) and crtbegin/crtend, the root's glibc,
# libgcc_s and libstdc++ (GCC $gccVersion's headers), Debian's loader path.
cc="$hostCC --sysroot=$sysroot -O2 -Wall -Wno-unused-result \
	-L$sysroot/usr/lib/gcc/x86_64-linux-gnu/$gccVersion \
	-L$sysroot/usr/lib/x86_64-linux-gnu -Wl,--build-id=none \
	-Wl,-rpath-link,$sysroot/usr/lib/x86_64-linux-gnu"
$cc -pthread -o "$tests/glibc-check" "$sources/glibc-check.c" -ldl
$cc -x c++ -std=gnu++17 -nostdinc++ \
	-isystem "$sysroot/usr/include/c++/$gccVersion" \
	-isystem "$sysroot/usr/include/x86_64-linux-gnu/c++/$gccVersion" \
	-pthread -o "$tests/cxx-check" "$sources/cxx-check.cpp" -x none \
	-lstdc++ -lm
$cc -o "$tests/sdl-game" "$sources/sdl-game.c" -lSDL2
case " $layers " in
	*" gl "*)
		$cc -o "$tests/gl-check" "$sources/gl-check.c" -lEGL -lGLESv2 ;;
esac

# Nothing may need a newer glibc or libstdc++ than the root's.
for program in "$tests"/*; do
	if readelf -V "$program" | grep -Eq 'GLIBC_2\.(4[2-9]|[5-9][0-9])|GLIBCXX_3\.4\.(3[4-9])'; then
		echo "build-debian-root.sh: $program needs symbols newer than" \
			"the root's glibc or libstdc++" >&2
		exit 1
	fi
done

# Owned by root, as on Linux; the archive keeps modes and links.
tar -C "$root" --owner=0 --group=0 --numeric-owner -czf "$output.tmp" .
mv "$output.tmp" "$output"
