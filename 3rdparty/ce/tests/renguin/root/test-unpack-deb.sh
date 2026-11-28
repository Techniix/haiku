#!/bin/sh
# Regression test for unpack-deb.py's containment (PR #38 review): crafted
# packages, made here in a private temporary directory, must not get a path
# or link out of the root.
#
#   test-unpack-deb.sh <unpack-deb.py>
#
# build-debian-root.sh runs it before it unpacks anything, so the Debian
# root is only built with an unpacker that passes it. Prints one line per
# case and exits 1 if any fails.
#
#   chain       one package: a/b/l -> ../.. and etc/passwd -> ../a/b/l/../x
#               (lexically inside; the kernel resolves it to <root>/../x)
#   absolute    a/abs -> /../../x: normalised as in a chroot, "/x", so it
#               must point to <root>/x
#   through     a/esc -> ../.. and a regular file a/esc/y below it
#   later       a first package with etc/p -> ../a/l/../x (a/l dangling), a
#               second with a/l -> .. that makes etc/p leave the root
#   benign      merged /usr (bin -> usr/bin), a file through it, an absolute
#               link to /usr/bin/tool: accepted, the link relative
set -e

unpacker=$1
work=$(mktemp -d "${TMPDIR:-/tmp}/test-unpack-deb.XXXXXX")
trap 'rm -rf "$work"' EXIT
failures=0

# package <name> <member...>: <name>.deb from $work/stage/<name>, with the
# members in that order (tar --no-recursion).
package()
{
	name=$1
	shift
	(cd "$work/stage/$name" && tar --no-recursion --owner=0 --group=0 \
		-cJf "$work/$name.data.tar.xz" "$@")
	(cd "$work" && printf '2.0\n' > debian-binary \
		&& tar -cJf control.tar.xz --files-from /dev/null \
		&& cp "$name.data.tar.xz" data.tar.xz \
		&& rm -f "$name.deb" \
		&& ar rc "$name.deb" debian-binary control.tar.xz data.tar.xz \
		&& rm -f data.tar.xz)
}

# fresh_root <name>: an empty root with an "outside" directory beside it
fresh_root()
{
	rm -rf "$work/$1-root" "$work/x"
	mkdir -p "$work/$1-root"
	root=$work/$1-root
}

result()
{
	if [ "$2" = pass ]; then
		echo "unpack-deb $1 ok"
	else
		echo "unpack-deb $1 FAIL: $3"
		failures=$((failures + 1))
	fi
}

# The host must not have been written outside the root ($work/x).
outside_untouched()
{
	[ ! -e "$work/x" ] && [ ! -L "$work/x" ]
}

mkdir -p "$work/stage"

# chain
mkdir -p "$work/stage/chain/a/b" "$work/stage/chain/etc"
ln -s ../.. "$work/stage/chain/a/b/l"
ln -s ../a/b/l/../x "$work/stage/chain/etc/passwd"
package chain a a/b a/b/l etc etc/passwd
fresh_root chain
if python3 -I "$unpacker" "$work/chain.deb" "$root" 2>/dev/null; then
	# what build-debian-root.sh does next
	echo hi > "$root/etc/passwd" 2>/dev/null || true
	result chain fail "accepted (outside written: $(outside_untouched \
		&& echo no || echo yes))"
else
	outside_untouched && result chain pass || result chain fail "refused" \
		"but wrote outside"
fi

# absolute
mkdir -p "$work/stage/absolute/a"
ln -s /../../x "$work/stage/absolute/a/abs"
package absolute a a/abs
fresh_root absolute
if python3 -I "$unpacker" "$work/absolute.deb" "$root" 2>/dev/null; then
	real=$(python3 -I -c 'import os, sys; print(os.path.realpath(sys.argv[1]))' \
		"$root/a/abs")
	want=$(python3 -I -c 'import os, sys; print(os.path.realpath(sys.argv[1]))' \
		"$root")/x
	[ "$real" = "$want" ] && result absolute pass \
		|| result absolute fail "a/abs -> $(readlink "$root/a/abs") is $real"
else
	outside_untouched && result absolute pass \
		|| result absolute fail "refused but wrote outside"
fi

# through
mkdir -p "$work/stage/through/a" "$work/stage/through/y-src"
ln -s ../.. "$work/stage/through/a/esc"
echo data > "$work/stage/through/y-src/y"
# the file member a/esc/y: tar it from a twin directory, then rename it in
# the archive with --transform
(cd "$work/stage/through" && tar --no-recursion --owner=0 --group=0 \
	-cf "$work/through.data.tar" a a/esc \
	&& tar --no-recursion --owner=0 --group=0 \
		--transform 's|^y-src/y$|a/esc/y|' -rf "$work/through.data.tar" \
		y-src/y \
	&& xz -f "$work/through.data.tar")
(cd "$work" && cp through.data.tar.xz data.tar.xz && rm -f through.deb \
	&& ar rc through.deb debian-binary control.tar.xz data.tar.xz \
	&& rm -f data.tar.xz)
fresh_root through
if python3 -I "$unpacker" "$work/through.deb" "$root" 2>/dev/null; then
	result through fail "accepted"
else
	[ ! -e "$work/y" ] && result through pass \
		|| result through fail "refused but wrote $work/y"
fi
rm -f "$work/y"

# later
mkdir -p "$work/stage/later1/etc" "$work/stage/later1/a" \
	"$work/stage/later2/a"
ln -s ../a/l/../x "$work/stage/later1/etc/p"
ln -s .. "$work/stage/later2/a/l"
package later1 etc a etc/p
package later2 a a/l
fresh_root later
python3 -I "$unpacker" "$work/later1.deb" "$root" 2>/dev/null || true
if python3 -I "$unpacker" "$work/later2.deb" "$root" 2>/dev/null; then
	result later fail "accepted: etc/p is now $(python3 -I -c \
		'import os, sys; print(os.path.realpath(sys.argv[1]))' \
		"$root/etc/p")"
else
	result later pass
fi

# benign
mkdir -p "$work/stage/benign/bin" "$work/stage/benign/etc"
echo tool > "$work/stage/benign/bin/tool"
ln -s /usr/bin/tool "$work/stage/benign/etc/tool-link"
package benign bin bin/tool etc etc/tool-link
fresh_root benign
mkdir -p "$root/usr/bin"
ln -s usr/bin "$root/bin"
if python3 -I "$unpacker" "$work/benign.deb" "$root" 2>/dev/null \
		&& [ -f "$root/usr/bin/tool" ] && [ -L "$root/bin" ] \
		&& [ "$(readlink "$root/etc/tool-link")" = ../usr/bin/tool ] \
		&& [ "$(cat "$root/etc/tool-link")" = tool ]; then
	result benign pass
else
	result benign fail "not unpacked as expected"
fi

[ $failures = 0 ]
