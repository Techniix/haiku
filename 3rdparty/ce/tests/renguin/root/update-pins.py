#!/usr/bin/env python3
# Copyright 2026, Renga contributors.
# Distributed under the terms of the MIT License.
"""Brings a Renguin pins file (alpine.pins, chromium.pins) up to date.

  update-pins.py <pins file> [--cache <dir>] [--check]

Alpine keeps only the current version of a package in a branch, so a
pinned package that was replaced can no longer be downloaded
(build-chromium-root.sh: "cannot fetch ... update chromium.pins"). This
reads the branch's APKINDEX for every repository the file uses, moves each
"main"/"community" line to the package's current version, downloads only
the changed packages and writes their SHA-256. "release" lines are kept.
Comments, the branch line and the order stay as they are.

It also lists the dependencies of the pinned packages that no pinned
package provides (new ones a newer version brought), so the file can be
completed by hand; it does not add packages on its own, because the files
leave some out on purpose (see their headers).

--check only says what would change. --cache keeps the downloads (default:
a temporary directory). Mirror: $RENGUIN_ALPINE_MIRROR or
https://dl-cdn.alpinelinux.org/alpine.
"""

import hashlib
import io
import os
import re
import sys
import tarfile
import tempfile
import urllib.request

MIRROR = os.environ.get("RENGUIN_ALPINE_MIRROR",
	"https://dl-cdn.alpinelinux.org/alpine")


def fetch(url):
	with urllib.request.urlopen(url, timeout=120) as response:
		return response.read()


def read_index(branch, repository):
	"""Package name -> (version, provides, depends) from APKINDEX."""
	data = fetch("%s/%s/%s/x86_64/APKINDEX.tar.gz"
		% (MIRROR, branch, repository))
	with tarfile.open(fileobj=io.BytesIO(data), mode="r:gz") as archive:
		text = archive.extractfile("APKINDEX").read().decode()
	packages = {}
	for block in text.split("\n\n"):
		fields = {}
		for line in block.splitlines():
			if len(line) > 2 and line[1] == ":":
				fields.setdefault(line[0], []).append(line[2:])
		if "P" not in fields:
			continue
		name = fields["P"][0]
		packages[name] = (fields["V"][0],
			" ".join(fields.get("p", [])).split(),
			" ".join(fields.get("D", [])).split())
	return packages


def split_file(file):
	"""chromium-156.0.8078.79-r0.apk -> ("chromium", "156.0.8078.79-r0")."""
	match = re.match(r"^(.+)-([^-]+-r\d+)\.apk$", file)
	return (match.group(1), match.group(2)) if match else (None, None)


def strip_version(dependency):
	return re.split(r"[<>=~]", dependency.lstrip("!"), maxsplit=1)[0]


def main():
	args = sys.argv[1:]
	if not args or args[0] in ("-h", "--help"):
		print(__doc__)
		return 0 if args else 2
	path = args[0]
	check = "--check" in args
	cache = None
	if "--cache" in args:
		cache = args[args.index("--cache") + 1]
		os.makedirs(cache, exist_ok=True)
	lines = open(path).read().splitlines()
	branch = next(l.split()[1] for l in lines if l.startswith("branch "))
	repositories = sorted({l.split()[0] for l in lines
		if l.split() and l.split()[0] in ("main", "community")})
	index = {}
	for repository in repositories:
		for name, entry in read_index(branch, repository).items():
			index.setdefault(name, (repository,) + entry)

	pinned = set()
	provided = set()
	changes = []
	work = cache or tempfile.mkdtemp(prefix="renguin-pins.")
	output = []
	for line in lines:
		fields = line.split()
		if len(fields) != 3 or fields[0] not in ("main", "community"):
			output.append(line)
			continue
		kind, file, digest = fields
		name, version = split_file(file)
		if name is None or name not in index:
			print("update-pins: %s is not in %s's index any more (removed "
				"or renamed): edit by hand" % (file, branch))
			output.append(line)
			continue
		repository, current, provides, depends = index[name]
		pinned.add(name)
		provided.update(strip_version(p) for p in provides)
		if current != version or repository != kind:
			newFile = "%s-%s.apk" % (name, current)
			changes.append("%s -> %s" % (file, newFile))
			if not check:
				target = os.path.join(work, newFile)
				if not os.path.exists(target):
					data = fetch("%s/%s/%s/x86_64/%s"
						% (MIRROR, branch, repository, newFile))
					with open(target, "wb") as handle:
						handle.write(data)
				digest = hashlib.sha256(open(target, "rb").read()).hexdigest()
			line = "%s %s %s" % (repository, newFile, digest)
		output.append(line)

	missing = set()
	for name in pinned:
		for dependency in index[name][3]:
			if dependency.startswith("!"):
				continue
			dependency = strip_version(dependency)
			if dependency not in pinned and dependency not in provided:
				missing.add(dependency)
	for change in changes:
		print("update-pins: " + change)
	if missing:
		print("update-pins: dependencies no pinned package provides (new, "
			"or left out on purpose): " + " ".join(sorted(missing)))
	if not changes:
		print("update-pins: %s is current" % path)
	elif not check:
		with open(path, "w") as handle:
			handle.write("\n".join(output) + "\n")
		print("update-pins: %d package(s) updated in %s" % (len(changes), path))
	return 0


if __name__ == "__main__":
	sys.exit(main())
