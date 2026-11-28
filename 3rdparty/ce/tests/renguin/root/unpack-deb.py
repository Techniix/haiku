#!/usr/bin/env python3
"""Unpacks a Debian package's files into a root (build-debian-root.sh).

    unpack-deb.py <package.deb> <root directory>
    unpack-deb.py --check <root directory>

Does what "dpkg-deb -x" does, without depending on the host's dpkg or tar
for safety: every path stays inside <root>, whatever the package contains.
Paths are resolved as a chroot into <root> would resolve them, one real
component at a time: ".." goes back to the directory actually reached
(after links), and fails instead of leaving <root>; a link on the way is
followed with the same walk (an absolute target starting again at <root>).
- Member names that are absolute or contain ".." are refused.
- Each member's parent directory is resolved with that walk; the member is
  written there, a link already at its own name is replaced, never
  followed.
- A symbolic link member: an absolute target is normalised as in a chroot
  (posixpath.normpath("/" + target), so "/../../x" is "/x") and made
  relative; the target must then resolve inside <root> with the walk.
- Hard links become copies (their source resolved with the walk); device
  nodes and FIFOs are refused.
- Modes come from the member (no umask), ownership is not kept (the
  archive of the root is written with owner 0).
After the members, every link in <root> must resolve inside <root> with the
host kernel's rules too (os.path.realpath): a link made by one member can
change what an earlier, dangling one means. "--check" does only that pass
(build-debian-root.sh runs it again once the root is complete). Links whose
target starts with /boot/ are host paths for Renguin and are left out of
that pass; packages may not make them (only build-debian-root.sh does, for
/etc/resolv.conf).
The data member may be xz, gzip, uncompressed or zstd (through "zstd").
test-unpack-deb.sh is its regression test (crafted packages).
"""

import io
import os
import posixpath
import shutil
import stat
import subprocess
import sys
import tarfile


class Refused(Exception):
	pass


def data_member(path):
	with open(path, "rb") as file:
		if file.read(8) != b"!<arch>\n":
			raise Refused("not an ar archive")
		while True:
			header = file.read(60)
			if len(header) < 60:
				raise Refused("no data.tar member")
			name = header[0:16].decode().strip().rstrip("/")
			size = int(header[48:58].decode().strip())
			content = file.read(size)
			if size % 2:
				file.read(1)
			if name.startswith("data.tar"):
				return name, content


def open_data(name, content):
	if name.endswith(".zst"):
		content = subprocess.run(["zstd", "-dc"], input=content,
			stdout=subprocess.PIPE, check=True).stdout
	return tarfile.open(fileobj=io.BytesIO(content), mode="r:*")


def walk(root, stack, path, depth=0):
	"""Resolves path from the directory stack (real names below root) as a
	chroot into root would: links followed, ".." against the directory
	reached. Returns the new stack; raises Refused when the walk would
	leave root."""
	if depth > 40:
		raise Refused("too many links resolving %s" % path)
	stack = [] if path.startswith("/") else list(stack)
	for part in path.split("/"):
		if part in ("", "."):
			continue
		if part == "..":
			if not stack:
				raise Refused("%s leaves the root" % path)
			stack.pop()
			continue
		candidate = os.path.join(root, *stack, part)
		if os.path.islink(candidate):
			target = os.readlink(candidate)
			if target.startswith("/boot/"):
				raise Refused("%s goes through the host link %s"
					% (path, "/".join(stack + [part])))
			stack = walk(root, stack, target, depth + 1)
		else:
			stack.append(part)
	return stack


def check_links(root):
	"""Every link resolves inside root by the host kernel's rules."""
	prefix = root.rstrip("/") + "/"
	for directory, dirs, files in os.walk(root):
		for name in dirs + files:
			path = os.path.join(directory, name)
			if not os.path.islink(path):
				continue
			if os.readlink(path).startswith("/boot/"):
				continue
			real = os.path.realpath(path)
			if real != root and not real.startswith(prefix):
				raise Refused("link %s -> %s resolves to %s, outside the root"
					% (os.path.relpath(path, root), os.readlink(path), real))


def unpack(package, root):
	name, content = data_member(package)
	with open_data(name, content) as archive:
		for member in archive.getmembers():
			path = posixpath.normpath(member.name)
			if member.name.startswith("/") or path == ".." \
					or path.startswith("../") or "/../" in "/" + path + "/":
				raise Refused("member %s leaves the root" % member.name)
			if path == ".":
				continue
			parent = walk(root, [], posixpath.dirname(path))
			base = posixpath.basename(path)
			destination = os.path.join(root, *parent, base)
			if member.isdir():
				if os.path.islink(destination):
					walk(root, parent, base)	# must stay inside
					continue
				os.makedirs(destination, exist_ok=True)
				os.chmod(destination, stat.S_IMODE(member.mode))
				continue
			if os.path.islink(destination) or (os.path.lexists(destination)
					and not os.path.isdir(destination)):
				os.unlink(destination)
			if member.issym():
				target = member.linkname
				if target.startswith("/"):
					absolute = posixpath.normpath("/" + target.lstrip("/"))
					target = posixpath.relpath(absolute,
						"/" + "/".join(parent))
				# it resolves inside the root from the link's directory
				walk(root, parent, target)
				os.symlink(target, destination)
			elif member.islnk():
				source = os.path.join(root, *walk(root, [],
					posixpath.normpath(member.linkname)))
				shutil.copy2(source, destination)
			elif member.isreg():
				with archive.extractfile(member) as source, \
						open(destination, "wb") as target:
					shutil.copyfileobj(source, target)
				os.chmod(destination, stat.S_IMODE(member.mode))
			else:
				raise Refused("member %s is a device or FIFO" % member.name)
	check_links(root)


def main():
	try:
		if len(sys.argv) == 3 and sys.argv[1] == "--check":
			check_links(os.path.realpath(sys.argv[2]))
		elif len(sys.argv) == 3:
			unpack(sys.argv[1], os.path.realpath(sys.argv[2]))
		else:
			sys.exit(__doc__)
	except Refused as error:
		sys.exit("unpack-deb.py: %s: %s" % (sys.argv[-2], error))


if __name__ == "__main__":
	main()
