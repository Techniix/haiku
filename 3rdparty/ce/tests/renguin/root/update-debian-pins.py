#!/usr/bin/env python3
"""Brings debian.pins up to date (roadmap F-27, Steam groundwork).

    update-debian-pins.py [<pins file>]

debian.pins names the Debian packages of the harness's Debian root
(build-debian-root.sh), pinned by pool path and SHA-256. Its header says
what they come from:

    suite <name>             the Debian suite (trixie)
    snapshot <timestamp>     the snapshot.debian.org time the pins are from;
                             build-debian-root.sh fetches a pinned file from
                             there when deb.debian.org no longer has it
    seeds <layer> <pkg...>   packages a layer needs; their Depends and
                             Pre-Depends are added (first alternative)
    nodeps <layer> <pkg...>  packages taken without their dependencies
                             (the dev layer: headers and link files only)
    exclude <layer> <pkg...> dependencies a layer leaves out

Layers, in order: base (always in harness images), gl (Mesa, renga-*
harness images only), dev (only used to build the test programs, never
installed). A package already in an earlier layer is not repeated.

The script downloads the suite's Packages index for amd64 (about 10 MB),
resolves the layers and rewrites the pin lines, keeping the header. It then
sets "snapshot" to the current time (UTC), so the files stay fetchable from
snapshot.debian.org after a point release replaces them.
"""

import datetime
import lzma
import os
import re
import sys
import tempfile
import urllib.request

LAYERS = ("base", "gl", "dev")
MIRROR = os.environ.get("RENGUIN_DEBIAN_MIRROR", "https://deb.debian.org/debian")


def parse_index(text):
	packages = {}
	provides = {}
	for block in text.split("\n\n"):
		fields = {}
		key = None
		for line in block.split("\n"):
			if line.startswith(" ") and key is not None:
				continue
			if ":" in line:
				key, value = line.split(":", 1)
				fields[key] = value.strip()
		name = fields.get("Package")
		if name is None:
			continue
		# several versions of one package (rare): keep the newest listed
		packages[name] = fields
		for entry in fields.get("Provides", "").split(","):
			entry = entry.strip().split(" ")[0]
			if entry:
				provides.setdefault(entry, name)
	return packages, provides


def dependencies(fields):
	for field in ("Pre-Depends", "Depends"):
		for group in fields.get(field, "").split(","):
			group = group.strip()
			if not group:
				continue
			yield [re.sub(r"[ (].*", "", choice.strip()).split(":")[0]
				for choice in group.split("|")]


def resolve(packages, provides, seeds, excluded, taken):
	result = {}
	todo = list(seeds)
	while todo:
		name = todo.pop()
		if name in result or name in taken or name in excluded:
			continue
		if name not in packages:
			if name in provides:
				todo.append(provides[name])
				continue
			sys.exit("update-debian-pins.py: no package %s" % name)
		result[name] = packages[name]
		for choices in dependencies(packages[name]):
			if any(c in result or c in taken for c in choices):
				continue
			todo.append(choices[0])
	return result


def main():
	pins = sys.argv[1] if len(sys.argv) > 1 else os.path.join(
		os.path.dirname(os.path.abspath(__file__)), "debian.pins")
	header = []
	suite = None
	seeds = {layer: [] for layer in LAYERS}
	nodeps = {layer: [] for layer in LAYERS}
	excluded = {layer: set() for layer in LAYERS}
	with open(pins) as file:
		for line in file:
			words = line.split()
			if words and words[0] in LAYERS:
				continue
			header.append(line)
			if not words or words[0].startswith("#"):
				continue
			if words[0] == "suite":
				suite = words[1]
			elif words[0] in ("seeds", "nodeps", "exclude"):
				target = {"seeds": seeds, "nodeps": nodeps,
					"exclude": excluded}[words[0]]
				if words[0] == "exclude":
					target[words[1]].update(words[2:])
				else:
					target[words[1]].extend(words[2:])
	if suite is None:
		sys.exit("update-debian-pins.py: no suite in %s" % pins)

	url = "%s/dists/%s/main/binary-amd64/Packages.xz" % (MIRROR, suite)
	with tempfile.TemporaryDirectory() as work:
		index = os.path.join(work, "Packages.xz")
		urllib.request.urlretrieve(url, index)
		with lzma.open(index, "rt") as file:
			packages, provides = parse_index(file.read())

	taken = set()
	lines = []
	for layer in LAYERS:
		chosen = resolve(packages, provides, seeds[layer], excluded[layer],
			taken)
		for name in nodeps[layer]:
			if name not in packages:
				sys.exit("update-debian-pins.py: no package %s" % name)
			chosen[name] = packages[name]
		for name in sorted(chosen):
			fields = chosen[name]
			lines.append("%s %s %s\n" % (layer, fields["Filename"],
				fields["SHA256"]))
		taken.update(chosen)

	now = datetime.datetime.now(datetime.timezone.utc)
	stamp = now.strftime("%Y%m%dT%H%M%SZ")
	header = [("snapshot %s\n" % stamp) if line.startswith("snapshot ")
		else line for line in header]
	with open(pins + ".tmp", "w") as file:
		file.writelines(header)
		file.writelines(lines)
	os.replace(pins + ".tmp", pins)
	print("%s: %d packages, snapshot %s" % (pins, len(lines), stamp))


if __name__ == "__main__":
	main()
