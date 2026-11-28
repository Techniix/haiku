#!/usr/bin/env python3
"""Unpacks a Debian package's files into a root (build-debian-root.sh).

    unpack-deb.py <package.deb> <root directory>

Does what "dpkg-deb -x" does, without depending on the host's dpkg or tar
for safety: every path stays inside <root>, whatever the package contains.
- Member names that are absolute or contain ".." are refused.
- Each parent directory of a member is resolved inside <root>: an
  existing link on the way (the merged-/usr links bin -> usr/bin, or a
  link an earlier package made) is followed only while it stays inside
  <root>.
- A symbolic link member with an absolute target gets the equivalent
  relative target (the root's "/" is <root>); a relative target that
  leaves <root> is refused. Links whose target is under /boot/ are host
  paths for Renguin and are kept (build-debian-root.sh makes the only
  one, resolv.conf).
- Modes come from the member (no umask), ownership is not kept (the
  archive of the root is written with owner 0).
- Hard links become copies; device nodes and FIFOs are refused.
The data member may be xz, gzip, uncompressed or zstd (through "zstd").
"""

import io
import os
import shutil
import stat
import subprocess
import sys
import tarfile


def fail(message):
	sys.exit("unpack-deb.py: %s: %s" % (sys.argv[1], message))


def data_member(path):
	with open(path, "rb") as file:
		if file.read(8) != b"!<arch>\n":
			fail("not an ar archive")
		while True:
			header = file.read(60)
			if len(header) < 60:
				fail("no data.tar member")
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
		name = name[:-4]
	return tarfile.open(fileobj=io.BytesIO(content), mode="r:*")


def resolve_inside(root, relative, depth=0):
	"""The real path of root/relative with links followed inside root."""
	if depth > 40:
		fail("too many links resolving %s" % relative)
	current = root
	parts = [part for part in relative.split("/") if part not in ("", ".")]
	for index, part in enumerate(parts):
		if part == "..":
			fail("'..' in %s" % relative)
		candidate = os.path.join(current, part)
		if os.path.islink(candidate):
			target = os.readlink(candidate)
			if target.startswith("/"):
				fail("absolute link %s inside the root" % candidate)
			joined = os.path.normpath(os.path.join(
				os.path.relpath(current, root), target))
			if joined == ".." or joined.startswith("../"):
				fail("link %s leaves the root" % candidate)
			rest = "/".join(parts[index + 1:])
			return resolve_inside(root, joined + ("/" + rest if rest else ""),
				depth + 1)
		current = candidate
	return current


def main():
	if len(sys.argv) != 3:
		sys.exit(__doc__)
	package, root = sys.argv[1], os.path.realpath(sys.argv[2])
	name, content = data_member(package)
	with open_data(name, content) as archive:
		for member in archive.getmembers():
			path = os.path.normpath(member.name)
			if path.startswith("/") or path == ".." or path.startswith("../") \
					or "/../" in "/" + path + "/":
				fail("member %s leaves the root" % member.name)
			if path == ".":
				continue
			parent = resolve_inside(root, os.path.dirname(path))
			if not parent.startswith(root):
				fail("member %s leaves the root" % member.name)
			destination = os.path.join(parent, os.path.basename(path))
			if member.isdir():
				if os.path.islink(destination):
					resolve_inside(root, path)	# checks it stays inside
					continue
				os.makedirs(destination, exist_ok=True)
				os.chmod(destination, stat.S_IMODE(member.mode))
				continue
			if os.path.islink(destination) or (os.path.lexists(destination)
					and not os.path.isdir(destination)):
				os.unlink(destination)
			if member.issym():
				target = member.linkname
				if target.startswith("/") and not target.startswith("/boot/"):
					linkDir = os.path.relpath(parent, root)
					target = os.path.relpath(target.lstrip("/") or ".", linkDir)
				elif not target.startswith("/"):
					joined = os.path.normpath(os.path.join(
						os.path.relpath(parent, root), target))
					if joined == ".." or joined.startswith("../"):
						fail("link %s -> %s leaves the root" % (member.name,
							member.linkname))
				os.symlink(target, destination)
			elif member.islnk():
				source = resolve_inside(root, os.path.normpath(member.linkname))
				shutil.copy2(source, destination)
			elif member.isreg():
				with archive.extractfile(member) as source, \
						open(destination, "wb") as target:
					shutil.copyfileobj(source, target)
				os.chmod(destination, stat.S_IMODE(member.mode))
			else:
				fail("member %s is a device or FIFO" % member.name)


if __name__ == "__main__":
	main()
