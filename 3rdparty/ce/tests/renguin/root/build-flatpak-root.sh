#!/bin/sh
# Builds the Flatpak layer of the harness's Linux root (batch 14, Flatpak
# step 1; docs/design/flatpak-and-steam-under-renguin.md, steps 3 and 4):
#
#   build-flatpak-root.sh <output.tar.gz> <flatpak pins> <base pins>
#       <download cache dir> <test app source dir> <host cc>
#
# The layer is extracted on top of the regular Linux root (build-root.sh)
# in every harness image (about 12 MB to download once, 30 MB in the image).
# It has:
#  - Alpine's flatpak and ostree with the libraries they load (flatpak.pins,
#    pinned by version and SHA-256 like alpine.pins). Not bubblewrap: the
#    "linux" command puts Renga's bwrap at /usr/bin/bwrap (switch
#    "flatpak"). /var/lib/flatpak/repo/config is written as the package's
#    post-install script does (apk does not run here).
#  - A local Flatpak repository at /opt/renguin-data/flatpak/repo, built here
#    on the build host because the harness has no network:
#      runtime/org.renga.TestPlatform/x86_64/1  a hand-made runtime: Alpine's
#        static busybox as /usr/bin/sh and a few applets, and an ldconfig
#        that writes the empty ld.so.cache "flatpak run" asks for (the
#        runtime's programs are static);
#      app/org.renga.Hello/x86_64/1  ../flatpak/hello.c, a static program
#        built with <host cc> (the build host's glibc, like the libc test
#        programs; harness images are never distributed).
#    The repository is made with the pinned flatpak itself, run on the build
#    host through Alpine's musl loader from a scratch copy of the root
#    (minirootfs, <base pins> and <flatpak pins>), so the build host needs
#    no flatpak or ostree of its own; it must be Linux x86-64, as for the
#    other Linux test programs. The commits carry a fixed timestamp and are
#    not signed (the tests add the remote with --no-gpg-verify).
#    busybox is branded Linux (EI_OSABI 3) like the raw test programs:
#    Alpine builds it static-pie, which Renguin's fallback brand does not
#    take.
#
# As in build-chromium-root.sh: absolute symbolic links become relative
# ones (here after each package, checked with unpack-deb.py --check), /tmp
# and /var/tmp are left out, and documentation, man pages and translations
# other than English are dropped.
set -e

output=$1
pins=$2
basePins=$3
cache=$4
appSources=$5
hostCC=$6

mirror=${RENGUIN_ALPINE_MIRROR:-https://dl-cdn.alpinelinux.org/alpine}
unpackDeb=$(dirname "$0")/unpack-deb.py
mkdir -p "$cache"

work=$(mktemp -d "${TMPDIR:-/tmp}/renguin-flatpak.XXXXXX")
trap 'rm -rf "$work"' EXIT
root=$work/root
host=$work/host
mkdir -p "$root" "$host"

# fetch <pins file> <kind> <file> <sha256>: a pinned file into the cache,
# checked
fetch()
{
	fetchBranch=$(sed -n 's/^branch //p' "$1")
	if [ "$2" = release ]; then
		url=$mirror/$fetchBranch/releases/x86_64/$3
	else
		url=$mirror/$fetchBranch/$2/x86_64/$3
	fi
	if [ ! -f "$cache/$3" ]; then
		wget -q --timeout 60 -O "$cache/$3.part" "$url" || {
			rm -f "$cache/$3.part"
			echo "build-flatpak-root.sh: cannot fetch $3 (Alpine replaced" \
				"it? root/update-pins.py $(basename "$1"))" >&2
			exit 1
		}
		mv "$cache/$3.part" "$cache/$3"
	fi
	if [ "$(sha256sum "$cache/$3" | cut -d' ' -f1)" != "$4" ]; then
		echo "build-flatpak-root.sh: $3 does not match its pinned SHA-256" >&2
		rm -f "$cache/$3"
		exit 1
	fi
}

# relative_links <directory>: absolute symbolic links become relative ones
relative_links()
{
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
' "$1"
}

# unpack <archive> <directory>: an Alpine package or the minirootfs. After
# each one, its absolute links are made relative and every link must stay
# inside <directory> by the build host's rules (unpack-deb.py --check), so
# that a package's link to a directory cannot send a later package's files
# elsewhere on the build host.
unpack()
{
	tar -C "$2" -xzf "$1" --warning=no-unknown-keyword 2>/dev/null \
		|| tar -C "$2" -xzf "$1"
	rm -f "$2"/.PKGINFO "$2"/.SIGN.* "$2"/.pre-* "$2"/.post-* "$2"/.trigger
	relative_links "$2"
	python3 -I "$unpackDeb" --check "$2" || {
		echo "build-flatpak-root.sh: $(basename "$1") leaves" \
			"$2 through a link" >&2
		exit 1
	}
}

# The build host's copy: the base root's files, then the Flatpak packages.
grep -v '^#' "$basePins" | grep -v '^branch ' | while read -r kind file sum; do
	[ -n "$kind" ] || continue
	fetch "$basePins" "$kind" "$file" "$sum"
	unpack "$cache/$file" "$host"
done

mkdir -p "$work/busybox"
grep -v '^#' "$pins" | grep -v '^branch ' | while read -r kind file sum; do
	[ -n "$kind" ] || continue
	fetch "$pins" "$kind" "$file" "$sum"
	case "$file" in
		busybox-static-*) unpack "$cache/$file" "$work/busybox" ;;
		*)
			unpack "$cache/$file" "$root"
			unpack "$cache/$file" "$host"
			;;
	esac
done

# What flatpak's post-install script does (Alpine issue 16912).
mkdir -p "$root/var/lib/flatpak/repo"
cat > "$root/var/lib/flatpak/repo/config" <<EOF
[core]
repo_version=1
mode=bare-user-only
min-free-space-size=500MB
EOF

# flatpak on the build host: Alpine's loader and libraries only, a clean
# environment, no GIO modules of the host's, a private home.
mkdir -p "$work/home" "$work/gio-modules"
flatpak()
{
	env -i PATH=/usr/bin:/bin HOME="$work/home" \
		XDG_DATA_HOME="$work/home/data" XDG_CACHE_HOME="$work/home/cache" \
		XDG_CONFIG_HOME="$work/home/config" GIO_USE_VFS=local \
		GIO_MODULE_DIR="$work/gio-modules" \
		"$host/lib/ld-musl-x86_64.so.1" \
		--library-path "$host/lib:$host/usr/lib" "$host/usr/bin/flatpak" "$@"
}
flatpak --version > /dev/null || {
	echo "build-flatpak-root.sh: Alpine's flatpak does not run on this" \
		"build host (Linux x86-64 needed)" >&2
	exit 1
}

# The runtime.
runtime=$work/runtime
mkdir -p "$runtime/files/bin"
cp "$appSources/org.renga.TestPlatform.metadata" "$runtime/metadata"
cp "$work/busybox/bin/busybox.static" "$runtime/files/bin/busybox"
printf '\003' | dd of="$runtime/files/bin/busybox" bs=1 seek=7 conv=notrunc \
	status=none
for applet in sh cat echo env ls mkdir rm test true; do
	ln -s busybox "$runtime/files/bin/$applet"
done
cp "$appSources/ldconfig" "$runtime/files/bin/ldconfig"
chmod 755 "$runtime/files/bin/busybox" "$runtime/files/bin/ldconfig"

# The application.
app=$work/app
mkdir -p "$app/files/bin"
cp "$appSources/org.renga.Hello.metadata" "$app/metadata"
$hostCC -O2 -static -o "$app/files/bin/renga-hello" "$appSources/hello.c"

repo=$root/opt/renguin-data/flatpak/repo
mkdir -p "$(dirname "$repo")"
stamp=2026-11-27T00:00:00Z
flatpak build-export --runtime --files=files --timestamp=$stamp \
	--subject="Renga test runtime" "$repo" "$runtime" 1 > /dev/null
flatpak build-finish --command=renga-hello "$app" > /dev/null
flatpak build-export --timestamp=$stamp --subject="Renga test app" \
	"$repo" "$app" 1 > /dev/null
flatpak build-update-repo "$repo" > /dev/null
for ref in runtime/org.renga.TestPlatform/x86_64/1 app/org.renga.Hello/x86_64/1
do
	[ -f "$repo/refs/heads/$ref" ] || {
		echo "build-flatpak-root.sh: $ref is missing from the repository" >&2
		exit 1
	}
done
rm -rf "$repo/tmp"/* "$repo/.lock"

# No documentation, only English translations.
rm -rf "$root/usr/share/doc" "$root/usr/share/man" "$root/usr/share/info" \
	"$root/usr/share/gtk-doc" "$root/usr/share/gir-1.0"
if [ -d "$root/usr/share/locale" ]; then
	find "$root/usr/share/locale" -mindepth 1 -maxdepth 1 ! -name 'en*' \
		-exec rm -rf {} +
fi

relative_links "$root"

rm -rf "$root/tmp" "$root/var/tmp"

tar -C "$root" --owner=0 --group=0 --numeric-owner -czf "$output.tmp" .
mv "$output.tmp" "$output"
