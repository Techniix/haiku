#!/usr/bin/env python3
#
# Copyright 2026, Renga contributors.
# Distributed under the terms of the MIT License.
#
"""Renga test harness.

Builds a test image, boots it headless in QEMU, collects the results the
in-guest runner prints on the serial console, checks device output (such as
the audio QEMU records from the virtual sound card), and writes a report.

Typical use, from the root of the Haiku tree:

    3rdparty/ce/harness/ce-test.py                  # build + boot + report
    3rdparty/ce/harness/ce-test.py --compile-only   # only compile test targets
    3rdparty/ce/harness/ce-test.py --no-build       # reuse the last image

Every run writes to <output>/ce-test-results/<timestamp>/ (and refreshes the
"latest" symlink next to it):

    summary.md      human/agent readable report (also printed at the end)
    results.json    the same data, machine readable
    serial.log      complete serial console output of the guest
    serial.timed.log  the same, each line prefixed with host seconds since boot
    qemu.log        QEMU's own diagnostics, including guest errors reported
                    by device models (e.g. malformed virtio requests)
    build.log       build output (if a build ran)
    audio.wav       what the guest played on the virtio sound card

The exit status is 0 only if the build succeeded, the guest finished its
run without crashing, and every check passed.
"""

import argparse
import datetime
import glob
import json
import math
import os
import re
import shutil
import struct
import subprocess
import sys
import threading
import time


HAIKU_TOP = os.path.abspath(os.path.join(os.path.dirname(__file__), "..",
	"..", ".."))


# Expected tone phases of 3rdparty/ce/tests/virtio_sound/vsnd_test.cpp,
# as (name, left Hz, right Hz, nominal seconds).
VIRTIO_SOUND_PHASES = [
	("phase1", 440, 880, 3.0),
	("phase2", 660, 330, 1.5),
	("phase3", 1000, 1000, 1.5),
	("phase4", 500, 250, 1.0),
]

# Drivers whose error output (their ERROR() macro prints the name in yellow,
# "\33[33m<name>:") fails the run. A healthy test run prints none.
WATCHED_DRIVERS = ("virtio_sound", "virtio_block", "nvme_disk")

# Compile-only targets: fast feedback before the image build.
COMPILE_TARGETS = ["kernel_x86_64", "libroot.so", "libgnu.so",
	"libnetwork.so", "libc_ext_test", "virtio_sound",
	"vsnd_test", "eventfd_test", "memfd_test", "mqueue_test", "shm_test", "timerfd_test",
	"ucontext_test", "tcp_keepalive_test", "tcp_close_test", "tcp_info_test", "tcp",
	"virtio_block", "vblk_test", "nvme_disk", "nvme_test",
	"package_priority_test", "libpackage.so", "shortcut_keys_test",
	"input_server", "Terminal", "Keymap", "pointer_test", "usb_hid",
	"virtio_input", "<input>tablet", "<input>virtio"]

# Scratch disks for vblk_test: sparse files attached as virtio block devices
# without -snapshot, so what the guest does to them (trimming above all) can
# be checked on the host afterwards. Each entry is the file name and the
# virtio-blk-pci properties; the guest sees them as virtio_block/0, /1, ...
# in this order.
SCRATCH_DISK_SIZE = 64 * 1024 * 1024
SCRATCH_DISKS = [
	("vblk0", "discard=on,write-zeroes=on"),	# trims use DISCARD
	("vblk1", "discard=off,write-zeroes=on"),	# trims use WRITE ZEROES + unmap
]
# vblk_test writes 8 MiB and trims 2 MiB, about 1 MiB more in an unaligned
# range, then rewrites 4 KiB; so between 4 and 6 MiB stay allocated. Without
# working trims all 8 MiB would.
SCRATCH_ALLOCATED_RANGE = (4 * 1024 * 1024, 6 * 1024 * 1024)

# --two-installs: BFS volume name of the second install attached in front of
# the test image.
DECOY_VOLUME_NAME = "Decoy"

# Scratch namespaces for nvme_test: one NVMe controller with two namespaces
# of different sizes and logical block sizes, each backed by its own sparse
# file (attached without -snapshot like the scratch disks above). Entries are
# the file name, nsid, size, logical block size and the byte nvme_test fills
# its range with. The guest sees the first namespace as
# /dev/disk/nvme/<id>/raw and the second as /dev/disk/nvme/<id>/ns2/raw.
# Each file starts with an MBR (nvme_mbr()) with one partition from 4 to
# 8 MiB, so the guest also publishes a partition device per namespace.
NVME_NAMESPACES = [
	("nvme_ns1", 1, 48 * 1024 * 1024, 512, 0xa1),
	("nvme_ns2", 2, 80 * 1024 * 1024, 4096, 0xb2),
]

# Absolute pointing devices --pointer-devices can attach next to QEMU's PS/2
# mouse, for the pointer suite: the QEMU options, and the name QEMU's
# "info mice" lists the device under (the harness selects it by that name
# with "mouse_set" before it sends a position). All of them take absolute
# positions 0..32767 (INPUT_EVENT_ABS_MAX) on both axes.
POINTER_DEVICES = {
	# What QEMU, VirtualBox and VMware call a USB tablet: a HID mouse
	# collection with absolute X/Y, on the controller --usb-controller picks.
	"usb": (["-device", "usb-tablet,bus=ptrusb.0"], "QEMU HID Tablet"),
	# virtio-input with EV_ABS axes.
	"virtio": (["-device", "virtio-tablet-pci"], "QEMU Virtio Tablet"),
}
# --usb-controller: the QEMU device of the USB tablet's host controller.
USB_CONTROLLERS = {
	"xhci": "qemu-xhci",
	"ehci": "usb-ehci",
	"uhci": "piix3-usb-uhci",
	"ohci": "pci-ohci",
}
POINTER_ABS_MAX = 32767
POINTER_REL_MAX = 200

# Sources whose TRACE() output --verbose turns on (via -sCE_TRACE=1).
TRACE_SOURCES = [
	"src/add-ons/kernel/drivers/audio/virtio/virtio_sound.cpp",
]

# Serial console patterns
KDL_PATTERNS = ("Welcome to Kernel Debugging Land", "kdebug>")
KDL_MAX_BACKTRACES = 120
PANIC_PATTERN = re.compile(r"PANIC: (.*)")
TEST_PATTERN = re.compile(r"^CE-TEST (\S+) (PASS|FAIL) ?(.*)")
SUITE_PATTERN = re.compile(r"^CE-SUITE (\S+) (START|DONE)(.*)")


# #pragma mark - helpers


def log(message):
	print("[ce-test] " + message, flush=True)


def run_logged(command, cwd, log_path, timeout=None):
	"""Runs command, teeing its output into log_path. Returns exit code."""
	with open(log_path, "a") as logFile:
		logFile.write("$ " + " ".join(command) + "\n")
		logFile.flush()
		# jam builds absolute paths from $PWD, which subprocess does not
		# update when it changes the working directory.
		environment = dict(os.environ, PWD=cwd)
		process = subprocess.Popen(command, cwd=cwd, env=environment,
			stdout=logFile, stderr=subprocess.STDOUT)
		try:
			return process.wait(timeout=timeout)
		except subprocess.TimeoutExpired:
			process.kill()
			return -1


# Source paths whose compiler warnings we want to see. Warnings elsewhere are
# upstream's and only drown out the signal.
WATCHED_PATHS = ("3rdparty/ce", "drivers/audio/virtio", "kernel/events",
	"libs/gnu")


def interesting_build_lines(log_path, limit=60):
	"""Build problems for the summary: every error, plus warnings in the
	code we are working on (WATCHED_PATHS)."""
	lines = []
	try:
		with open(log_path, errors="replace") as logFile:
			for line in logFile:
				line = line.rstrip()
				if re.search(r"\berror:|\.\.\.failed|don't know how|"
						r"unknown rule|^Error:|^\s+Error:", line):
					lines.append(line)
				elif re.search(r"warning", line, re.IGNORECASE) \
						and any(path in line for path in WATCHED_PATHS):
					lines.append(line)
	except OSError:
		pass
	return lines[:limit]


# #pragma mark - build


def haiku_revision():
	"""The build stamps the image with the nearest upstream hrev tag. Clones
	from GitHub carry no such tags; in that case fall back to a revision
	derived from the commit, so that a missing tag never blocks testing."""
	try:
		subprocess.check_output(["git", "describe", "--tags",
			"--match=hrev*", "--abbrev=1"], cwd=HAIKU_TOP,
			stderr=subprocess.DEVNULL)
		return None
	except (subprocess.CalledProcessError, OSError):
		pass
	try:
		commit = subprocess.check_output(["git", "rev-parse", "--short",
			"HEAD"], cwd=HAIKU_TOP, text=True).strip()
	except (subprocess.CalledProcessError, OSError):
		commit = "unknown"
	return "hrev0+ce-" + commit


def build(args, results_dir, report):
	build_log = os.path.join(results_dir, "build.log")
	jam = ["jam", "-q", "-j%d" % args.jobs, "-sHAIKU_INCLUDE_3RDPARTY=1"]
	revision = haiku_revision()
	if revision is not None:
		log("no hrev tags in this clone, building as " + revision)
		jam.append("-sHAIKU_REVISION=" + revision)
		report["revision_fallback"] = revision

	if args.verbose:
		# Trace output is a compile-time switch (see the drivers' Jamfiles).
		# jam does not notice changed defines, so force the recompile.
		jam.append("-sCE_TRACE=1")
		for source in TRACE_SOURCES:
			os.utime(os.path.join(HAIKU_TOP, source), None)
		report["verbose"] = True
	elif os.path.exists(os.path.join(args.output, ".ce-trace-build")):
		# The previous build had tracing on; rebuild those objects without.
		for source in TRACE_SOURCES:
			os.utime(os.path.join(HAIKU_TOP, source), None)
	marker = os.path.join(args.output, ".ce-trace-build")
	if args.verbose:
		open(marker, "w").close()
	elif os.path.exists(marker):
		os.remove(marker)

	log("compiling test targets: " + " ".join(COMPILE_TARGETS))
	started = time.time()
	code = run_logged(jam + COMPILE_TARGETS, args.output, build_log)
	report["build"] = {
		"compile_targets": COMPILE_TARGETS,
		"compile_status": code,
		"compile_seconds": round(time.time() - started, 1),
	}
	if code != 0 or args.compile_only:
		report["build"]["issues"] = interesting_build_lines(build_log)
		return code == 0

	log("building image @%s (this can take a while)" % args.profile)
	started = time.time()
	code = run_logged(jam + ["-sCE_HARNESS=1", "@" + args.profile],
		args.output, build_log)
	report["build"]["image_status"] = code
	report["build"]["image_seconds"] = round(time.time() - started, 1)
	report["build"]["issues"] = interesting_build_lines(build_log)
	return code == 0


def boot_drive(args, image, decoy=None):
	"""Returns the QEMU options that attach the test image.

	"disk" attaches the image as an IDE hard disk (raw images, and anyboot
	images the way they boot from a USB stick); "cdrom" attaches it as an
	IDE CD-ROM, which exercises the El Torito boot paths of anyboot images.

	With a decoy (--two-installs), a second install is attached in front of
	the test image, first on the bus and first in the boot order. OVMF
	connects the devices in boot order, so the decoy's block device is the
	first one the boot loader sees. The decoy has no EFI system partition
	(see make_decoy()), so the firmware cannot start it and goes on to the
	test image, whose boot loader then has to pick its own disk.
	"""
	if decoy is not None:
		if args.disk_bus == "virtio":
			return ["-drive", "file=%s,format=raw,if=none,id=decoy" % decoy,
				"-device", "virtio-blk-pci,drive=decoy,bootindex=0",
				"-drive", "file=%s,format=raw,if=none,id=boot" % image,
				"-device", "virtio-blk-pci,drive=boot,bootindex=1"]
		return ["-drive", "file=%s,format=raw,if=none,id=decoy" % decoy,
			"-device", "ide-hd,drive=decoy,bus=ide.0,unit=0,bootindex=0",
			"-drive", "file=%s,format=raw,if=none,id=boot" % image,
			"-device", "ide-hd,drive=boot,bus=ide.0,unit=1,bootindex=1"]
	if args.media == "cdrom":
		return ["-drive",
			"file=%s,format=raw,if=ide,media=cdrom,readonly=on" % image]
	if args.disk_bus == "virtio":
		# bootindex makes the firmware boot this disk and not one of the
		# (blank) virtio scratch disks.
		return ["-drive", "file=%s,format=raw,if=none,id=boot" % image,
			"-device", "virtio-blk-pci,drive=boot,bootindex=0"]
	return ["-drive", "file=%s,format=raw,if=ide" % image]


def image_path(args):
	names = {
		"minimum-raw": "haiku-minimum.image",
		"nightly-raw": "haiku-nightly.image",
		"test-raw": "haiku-test.image",
		"minimum-anyboot": "haiku-minimum-anyboot.iso",
		"nightly-anyboot": "haiku-nightly-anyboot.iso",
		"renga-raw": "renga.image",
		"renga-anyboot": "renga-anyboot.iso",
	}
	return os.path.join(args.output, names.get(args.profile,
		"haiku-%s.image" % args.profile.replace("-raw", "")))


def bfs_volume_offset(path):
	"""Byte offset of the BFS volume in an image: the first BFS partition
	(type 0xeb) of its MBR, or 0 for an unpartitioned image. None if there
	is no BFS superblock where one is expected."""
	with open(path, "rb") as image:
		mbr = image.read(512)
		offsets = []
		if mbr[510:512] == b"\x55\xaa":
			for index in range(4):
				entry = mbr[446 + 16 * index:462 + 16 * index]
				if entry[4] == 0xeb:
					offsets.append(struct.unpack("<I", entry[8:12])[0] * 512)
		offsets.append(0)
		for offset in offsets:
			image.seek(offset + 512)
			superblock = image.read(64)
			if superblock[32:36] == b"1SFB":
				return offset
	return None


def bfs_volume_name(path):
	offset = bfs_volume_offset(path)
	if offset is None:
		return None
	with open(path, "rb") as image:
		image.seek(offset + 512)
		return image.read(32).split(b"\0")[0].decode("utf-8", "replace")


def make_decoy(image, decoy):
	"""Copies the test image into a second, distinguishable install for
	--two-installs: its BFS volume is renamed to DECOY_VOLUME_NAME (the
	runner's "df /boot" shows which of the two the guest booted), and its
	MBR loses every partition entry except the BFS one, the EFI system
	partition above all. Anyboot images also carry an El Torito boot catalog
	that firmware honours on hard disks too, so its boot record descriptor
	goes as well. Like an install made for BIOS booting, the firmware then
	has nothing to start on the decoy. The changed MBR also lets the kernel's
	boot disk identifier (which checksums the first disk blocks) tell the
	two disks apart, the way it can two different real installs."""
	offset = bfs_volume_offset(image)
	if offset is None or offset == 0:
		return False
	shutil.copyfile(image, decoy)
	with open(decoy, "r+b") as copy:
		for index in range(4):
			copy.seek(446 + 16 * index)
			entry = copy.read(16)
			if entry[4] != 0xeb:
				copy.seek(446 + 16 * index)
				copy.write(b"\0" * 16)
		# ISO 9660 volume descriptors start at 2 KiB sector 16; a boot
		# record (type 0) names the El Torito catalog.
		for sector in range(16, 32):
			copy.seek(sector * 2048)
			descriptor = copy.read(39)
			if descriptor[1:6] != b"CD001" or descriptor[0] == 0xff:
				break
			if descriptor[0] == 0 \
					and descriptor[7:30] == b"EL TORITO SPECIFICATION":
				copy.seek(sector * 2048)
				copy.write(b"\0" * 2048)
		copy.seek(offset + 512)
		copy.write(DECOY_VOLUME_NAME.encode().ljust(32, b"\0"))
	return True


def make_menu_timeout_image(args, image, copy):
	"""Copies the test image for --boot-menu-timeout and sets
	"boot_menu_timeout <seconds>" in the copy's kernel settings file, through
	the build's bfs_shell. The file written is the image's own (the tree's
	data/settings/kernel/drivers/kernel, which the build copies) plus that
	line. Returns an error text, or None on success."""
	offset = bfs_volume_offset(image)
	if offset is None:
		return "no BFS volume in " + image
	shells = glob.glob(os.path.join(args.output, "objects", "*", "*", "*",
		"tools", "bfs_shell", "bfs_shell"))
	if not shells:
		return "bfs_shell not built (jam -q '<build>bfs_shell')"

	with open(os.path.join(HAIKU_TOP, "data", "settings", "kernel",
			"drivers", "kernel")) as original:
		settings = original.read()
	settings_path = copy + ".kernel-settings"
	with open(settings_path, "w") as patched:
		patched.write(settings.rstrip("\n") + "\n\nboot_menu_timeout %d\n"
			% args.boot_menu_timeout)

	shutil.copyfile(image, copy)
	target = "/myfs/home/config/settings/kernel/drivers/kernel"
	result = subprocess.run([shells[0], "--start-offset", str(offset), copy],
		input=("cp -f :%s %s\ncat %s\nsync\nquit\n" % (settings_path, target,
			target)).encode(), stdout=subprocess.PIPE,
		stderr=subprocess.STDOUT, timeout=120)
	os.remove(settings_path)
	output = result.stdout.decode("utf-8", "replace")
	if result.returncode != 0 \
			or "boot_menu_timeout %d" % args.boot_menu_timeout not in output:
		return "bfs_shell could not update the settings: " + output[-300:]
	return None


def check_boot_menu(args, results_dir, report):
	"""The boot loader's menu countdown, from its "boot menu:" lines. Without
	--boot-menu-timeout there must be none: the menu stays off unless a key
	is held, as it always was. With it, the countdown must start at the
	configured number of seconds, run out after about that long, and
	continue booting on its own (the rest of the run shows that it did)."""
	# serial.timed.log stamps a line when its first byte arrives. On UEFI
	# the firmware copies the menu's screen output to the serial port too,
	# so a log line can follow the countdown drawn a second earlier on the
	# same line, and carry that earlier stamp. The time the countdown took
	# is therefore measured from the line that starts it to the first line
	# after it ran out, which cannot be stamped before the countdown ended.
	lines = []
	after_end = None
	try:
		with open(os.path.join(results_dir, "serial.timed.log"),
				errors="replace") as serial:
			for line in serial:
				match = re.match(r"\[\s*([0-9.]+)\] (.*)", line)
				if match is None:
					continue
				stamp = float(match.group(1))
				menu = re.search(r"boot menu: .*", match.group(2))
				if menu:
					lines.append((stamp, menu.group(0).rstrip("\r\n")))
				elif lines and after_end is None \
						and lines[-1][1].startswith("boot menu: timeout"):
					after_end = stamp
	except OSError:
		pass

	seconds = args.boot_menu_timeout
	if not seconds:
		report.setdefault("host_checks", []).append({"name": "boot.menu",
			"pass": len(lines) == 0,
			"detail": "no boot menu (none configured)" if not lines
				else "unexpected boot menu: " + lines[0][1]})
		return

	start = [t for t, text in lines if text.startswith(
		'boot menu: "Continue booting" in %d s' % seconds)]
	end = [t for t, text in lines
		if text == 'boot menu: timeout, choosing "Continue booting"']
	ticks = [text for t, text in lines if re.match(r"boot menu: \d+ s$", text)]
	took = after_end - start[0] if start and end and after_end is not None \
		else None
	report.setdefault("host_checks", []).append({"name": "boot.menu",
		"pass": len(start) == 1 and len(end) == 1
			and len(ticks) == seconds - 1
			and took is not None and seconds - 0.5 <= took <= seconds + 5,
		"detail": "%d s countdown, %d tick(s), %s (want %d s, %d ticks, then "
			"\"Continue booting\")%s" % (seconds, len(ticks),
			"ran out after %.1f s" % took if took is not None
				else "did not run out", seconds, seconds - 1,
			"; last line: " + lines[-1][1] if lines and not end else "")})


def check_two_installs(args, results_dir, report):
	"""--two-installs: the boot loader must have booted the install whose
	boot loader the firmware started, not the decoy enumerated before it."""
	if not args.two_installs:
		return
	want = report.get("boot", {}).get("volume_name")
	try:
		with open(os.path.join(results_dir, "serial.log"), errors="replace") \
				as serial:
			text = serial.read()
	except OSError:
		text = ""
	volume = re.search(r'Volume Name: "([^"]*)"', text)
	got = volume.group(1) if volume else None
	report.setdefault("host_checks", []).append({
		"name": "boot.two_installs",
		"pass": want is not None and got == want,
		"detail": "booted volume %s (want \"%s\" from the boot disk, not "
			"\"%s\" from the disk enumerated first)" % (
			'"%s"' % got if got is not None else "not reported", want,
			DECOY_VOLUME_NAME)})


# #pragma mark - boot


class SerialReader(threading.Thread):
	"""Copies the guest's serial output to a file and watches it.

	When the kernel drops into its debugger it asks for a backtrace and
	syslog tail, then lets the main thread stop the VM.
	"""

	def __init__(self, process, log_path, monitor_path=None, qmp_path=None):
		threading.Thread.__init__(self, daemon=True)
		self.process = process
		self.log_path = log_path
		self.monitor_path = monitor_path
		self.qmp_path = qmp_path
		self.screenshots = []	# saved on request, as PPM
		self.line = b""		# incomplete last line, for CE-HOST requests
		self.host_requests = []
		self.done = threading.Event()
		self.kdl = threading.Event()
		self.finished = threading.Event()
		self.debug_done = threading.Event()
		self.last_output = time.time()
		self.lock = threading.Lock()
		self.kdl_text = ""	# everything printed since KDL was entered

	def run(self):
		buffer = b""
		started = time.time()
		at_line_start = True
		timed_path = self.log_path.replace(".log", ".timed.log")
		with open(self.log_path, "wb") as logFile, \
				open(timed_path, "wb") as timedFile:
			while True:
				chunk = self.process.stdout.read1(4096)
				if not chunk:
					break
				logFile.write(chunk)
				logFile.flush()
				# Prefix every line with the host time since boot, to see
				# where time goes without changing the guest.
				stamp = ("[%8.3f] " % (time.time() - started)).encode()
				for piece in chunk.splitlines(keepends=True):
					if at_line_start:
						timedFile.write(stamp)
					timedFile.write(piece)
					at_line_start = piece.endswith(b"\n")
				timedFile.flush()
				self.last_output = time.time()
				self._host_requests(chunk)
				buffer = (buffer + chunk)[-8192:]
				text = buffer.decode("latin-1")
				if "CE-HARNESS END" in text:
					self.finished.set()
				if self.kdl.is_set():
					with self.lock:
						self.kdl_text += chunk.decode("latin-1")
				elif any(p in text for p in KDL_PATTERNS):
					with self.lock:
						self.kdl_text = text
					self.kdl.set()
					# The session runs in its own thread so that this one
					# keeps draining the serial port while it waits.
					threading.Thread(target=self._debug_session,
						daemon=True).start()
		self.done.set()

	def _host_requests(self, chunk):
		"""Carries out requests a test makes of the host, written as lines
		"CE-HOST <command> <arguments>". Only commands on a short list are
		accepted, and only for the scratch disks, so a test cannot make the
		harness do anything else with the VM:

		  CE-HOST block_resize <scratch disk> <size>
		      resize a scratch disk, e.g. "block_resize vblk0 128M"; the
		      guest sees a configuration change interrupt
		  CE-HOST sendkey <keys>
		      press keys on the guest's keyboard, e.g. "sendkey ctrl-v":
		      at most two of ctrl, alt and shift, then a letter, a digit,
		      ret, tab or esc (no sysrq, so no kernel debugger, and no
		      delete, so no Team monitor)
		  CE-HOST pointer <device> <x> <y>
		      put the pointer of an absolute device the harness attached
		      (POINTER_DEVICES: usb, virtio) at x, y in its coordinates,
		      0..32767 on both axes
		  CE-HOST pointer_rel <dx> <dy>
		      move the PS/2 mouse by dx, dy (at most 200 either way)
		  CE-HOST screenshot <name>
		      save the screen as screen-<name>.png in the results (name:
		      up to 40 lower-case letters, digits and underscores)

		Pointer and screenshot requests are carried out before the next
		line is read, so they happen in order and before anything the test
		prints after them.
		"""
		self.line += chunk
		lines = self.line.split(b"\n")
		self.line = lines.pop()
		for raw in lines:
			line = raw.decode("latin-1").strip()
			if not line.startswith("CE-HOST "):
				continue
			words = line.split()[1:]
			accepted = host_request_allowed(words)
			request = {"request": " ".join(words), "accepted": accepted}
			if accepted and words[0] in ("pointer", "pointer_rel"):
				if self.qmp_path is not None:
					request["error"] = qmp_pointer(self.qmp_path, words)
			elif accepted and words[0] == "screenshot":
				path = os.path.join(os.path.dirname(self.log_path),
					"screen-%s.ppm" % words[1])
				if self.monitor_path is not None \
						and monitor_command(self.monitor_path,
							"screendump " + path):
					self.screenshots.append(path)
			elif accepted and self.monitor_path is not None:
				threading.Thread(target=monitor_command, daemon=True,
					args=(self.monitor_path, " ".join(words))).start()
			self.host_requests.append(request)
			log("guest request %s: %s%s" % ("carried out" if accepted
				else "refused", " ".join(words), ": " + request["error"]
				if request.get("error") else ""))

	def _send(self, command, settle=1.0):
		try:
			self.process.stdin.write(command.encode() + b"\n")
			self.process.stdin.flush()
		except OSError:
			return False
		time.sleep(settle)
		return True

	def _debug_session(self):
		"""Collects what is needed to debug a panic or hang without a
		second run: the current stack, all threads, the backtrace of every
		thread that is waiting or running (bounded), and the lock owners."""
		try:
			time.sleep(1)
			self._send("bt", 2)
			self._send("threads", 4)
			with self.lock:
				listing = self.kdl_text
			ids = []
			mutexes = []
			for line in listing.splitlines():
				fields = line.split()
				# "0xffff... <id> <state> [<wait type> <object>] ..." rows
				if len(fields) >= 3 and fields[0].startswith("0x") \
						and fields[1].isdigit():
					if fields[2] in ("waiting", "running", "ready"):
						ids.append(fields[1])
					if len(fields) >= 5 and fields[3] == "mutex" \
							and fields[4] not in mutexes:
						mutexes.append(fields[4])
			# For a deadlock, the holder of each contended mutex is the key
			# fact: print it, and backtrace holders the list did not show
			# (the thread list stops early if a thread is being created).
			for address in mutexes:
				self._send("mutex " + address, 0.6)
			with self.lock:
				infos = self.kdl_text[len(listing):]
			for holder in re.findall(r"holder:\s+(\d+)", infos):
				if holder not in ids and holder != "-1":
					ids.insert(0, holder)
			if ids:
				# Threads created after the list broke off: probe the next
				# ids, cheap and often where the culprit is.
				highest = max(int(i) for i in ids)
				ids += [str(i) for i in range(highest + 1, highest + 25)]
			for thread_id in ids[:KDL_MAX_BACKTRACES]:
				if not self._send("bt " + thread_id, 0.4):
					break
			self._send("running", 2)
		finally:
			self.debug_done.set()


def host_request_allowed(words):
	"""Whether a CE-HOST request (its words after "CE-HOST") is on the list
	SerialReader._host_requests documents."""
	if len(words) == 3 and words[0] == "block_resize":
		return words[1] in [name for name, _ in SCRATCH_DISKS] \
			and re.fullmatch(r"\d+[KMG]?", words[2]) is not None
	if len(words) == 2 and words[0] == "sendkey":
		return re.fullmatch(r"(?:(?:ctrl|alt|shift)-){0,2}"
			r"(?:[a-z0-9]|ret|tab|esc)", words[1]) is not None \
			and len(set(words[1].split("-")[:-1])) \
				== len(words[1].split("-")[:-1])
	if len(words) == 4 and words[0] == "pointer":
		return words[1] in POINTER_DEVICES \
			and all(re.fullmatch(r"\d{1,5}", w) is not None
				and int(w) <= POINTER_ABS_MAX for w in words[2:])
	if len(words) == 2 and words[0] == "screenshot":
		return re.fullmatch(r"[a-z0-9_]{1,40}", words[1]) is not None
	if len(words) == 3 and words[0] == "pointer_rel":
		return all(re.fullmatch(r"-?\d{1,3}", w) is not None
			and abs(int(w)) <= POINTER_REL_MAX for w in words[1:])
	return False


def qmp_pointer(path, words):
	"""Carries out an accepted pointer request (see
	SerialReader._host_requests) through QEMU's QMP socket. Returns None,
	or what went wrong.

	Absolute events go to the active absolute mouse handler, so "pointer"
	first makes the device active with HMP's "mouse_set" (by the index
	"info mice" gives it); relative events go to the first handler that
	takes them, the PS/2 mouse (the tablets take none).
	"""
	import socket
	try:
		with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as sock:
			sock.settimeout(5)
			sock.connect(path)
			stream = sock.makefile("rw")
			stream.readline()		# greeting

			def execute(command, **arguments):
				message = {"execute": command}
				if arguments:
					message["arguments"] = arguments
				stream.write(json.dumps(message) + "\n")
				stream.flush()
				while True:
					answer = json.loads(stream.readline())
					if "event" not in answer:
						break
				if "error" in answer:
					raise RuntimeError("%s: %s" % (command,
						answer["error"].get("desc")))
				return answer.get("return")

			def hmp(command):
				output = execute("human-monitor-command",
					**{"command-line": command})
				if output.startswith("Error") or "not a mouse" in output:
					raise RuntimeError("%s: %s" % (command, output.strip()))
				return output

			execute("qmp_capabilities")
			if words[0] == "pointer":
				name = POINTER_DEVICES[words[1]][1]
				index = None
				for line in hmp("info mice").splitlines():
					match = re.match(r"\W*Mouse #(\d+): (.*?)( \(absolute\))?$",
						line.strip())
					if match and match.group(2) == name:
						index = match.group(1)
				if index is None:
					return "no mouse \"%s\" in info mice" % name
				hmp("mouse_set " + index)
				execute("input-send-event", events=[
					{"type": "abs", "data": {"axis": "x",
						"value": int(words[2])}},
					{"type": "abs", "data": {"axis": "y",
						"value": int(words[3])}}])
			else:
				execute("input-send-event", events=[
					{"type": "rel", "data": {"axis": "x",
						"value": int(words[1])}},
					{"type": "rel", "data": {"axis": "y",
						"value": int(words[2])}}])
		return None
	except (OSError, ValueError, RuntimeError) as error:
		return str(error)


def monitor_command(path, command):
	"""Sends one command to the QEMU human monitor socket."""
	import socket
	try:
		with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as sock:
			sock.settimeout(5)
			sock.connect(path)
			time.sleep(0.3)
			sock.recv(4096)		# banner
			sock.sendall(command.encode() + b"\n")
			time.sleep(0.5)
		return True
	except OSError as error:
		log("monitor command failed: %s" % error)
		return False


def convert_screenshots(paths):
	"""QEMU writes PPM; convert to PNG when Pillow is there."""
	names = []
	for path in paths:
		time.sleep(0.2)
		if not os.path.exists(path):
			continue
		try:
			from PIL import Image
			png = path[:-4] + ".png"
			Image.open(path).save(png)
			os.unlink(path)
			path = png
		except Exception:
			pass
		names.append(os.path.basename(path))
	return names


def boot(args, results_dir, report):
	image = image_path(args)
	if not os.path.exists(image):
		report["boot"] = {"status": "no-image", "image": image}
		return False

	serial_log = os.path.join(results_dir, "serial.log")
	audio = os.path.join(results_dir, "audio.wav")
	# Unix socket paths are limited to ~107 bytes; keep it short.
	monitor_path = "/tmp/ce-monitor-%d.sock" % os.getpid()
	qmp_path = "/tmp/ce-qmp-%d.sock" % os.getpid()
	for path in (monitor_path, qmp_path):
		if os.path.exists(path):
			os.unlink(path)

	patched = None
	if args.boot_menu_timeout:
		patched = os.path.join(results_dir, "menu-timeout.img")
		error = make_menu_timeout_image(args, image, patched)
		if error is not None:
			report["boot"] = {"status": "image-setup-failed", "image": image,
				"error": error}
			log(error)
			return False
		image = patched

	decoy = None
	if args.two_installs:
		decoy = os.path.join(results_dir, "decoy.img")
		if not make_decoy(image, decoy):
			report["boot"] = {"status": "no-bfs-volume", "image": image}
			return False

	qemu = [
		"qemu-system-x86_64",
		"-m", str(args.memory),
		"-smp", str(args.smp),
		"-display", "none",
		"-vga", "std",
		"-serial", "stdio",
		# A monitor socket lets the harness press keys when the guest stalls.
		"-monitor", "unix:%s,server=on,wait=off" % monitor_path,
		# QMP sends the pointer positions the pointer suite asks for.
		"-qmp", "unix:%s,server=on,wait=off" % qmp_path,
		"-no-reboot",
		"-snapshot",
	] + boot_drive(args, image, decoy) + [
		"-audiodev", "wav,id=snd0,path=%s,out.frequency=48000,"
			"out.channels=2,out.format=s16" % audio,
		"-device", "virtio-sound-pci,audiodev=snd0",
		"-nic", "none",
		# Device models report malformed guest requests as guest errors.
		"-d", "guest_errors,unimp",
		"-D", os.path.join(results_dir, "qemu.log"),
	]
	for name, properties in SCRATCH_DISKS:
		path = os.path.join(results_dir, name + ".img")
		with open(path, "wb") as scratch:
			scratch.truncate(SCRATCH_DISK_SIZE)
		qemu += [
			"-drive", "file=%s,if=none,id=%s,format=raw,snapshot=off,"
				"discard=unmap" % (path, name),
			"-device", "virtio-blk-pci,drive=%s,%s" % (name, properties),
		]
	if "usb" in args.pointer_devices:
		qemu += ["-device", "%s,id=ptrusb" % USB_CONTROLLERS[args.usb_controller]]
	for device in args.pointer_devices:
		qemu += POINTER_DEVICES[device][0]
	if NVME_NAMESPACES:
		qemu += ["-device", "nvme,id=nvme0,serial=renga0"]
	for name, nsid, size, block_size, _ in NVME_NAMESPACES:
		path = os.path.join(results_dir, name + ".img")
		with open(path, "wb") as scratch:
			scratch.truncate(size)
			scratch.write(nvme_mbr(block_size))
		qemu += [
			"-drive", "file=%s,if=none,id=%s,format=raw,snapshot=off,"
				"discard=unmap" % (path, name),
			"-device", "nvme-ns,drive=%s,bus=nvme0,nsid=%d,"
				"logical_block_size=%d,physical_block_size=%d"
				% (name, nsid, block_size, block_size),
		]
	if os.path.exists("/dev/kvm") and os.access("/dev/kvm", os.W_OK):
		qemu += ["-enable-kvm", "-cpu", "host"]
	if args.firmware == "uefi":
		# OVMF comes as read-only code plus a variable store the firmware
		# writes to (boot entries, the boot order); each run gets a fresh
		# copy of the store so runs cannot influence each other.
		vars_copy = os.path.join(results_dir, "OVMF_VARS.fd")
		shutil.copyfile(args.ovmf_vars, vars_copy)
		qemu += [
			"-drive", "if=pflash,format=raw,unit=0,readonly=on,file=%s"
				% args.ovmf,
			"-drive", "if=pflash,format=raw,unit=1,file=%s" % vars_copy,
		]
	if args.media == "cdrom":
		# BIOS boots a CD through the El Torito floppy image (the
		# haiku-floppyboot.tgz boot archive), UEFI through the ESP image.
		qemu += ["-boot", "d"]

	report["boot"] = {"command": " ".join(qemu), "image": image}
	if decoy is not None:
		report["boot"]["volume_name"] = bfs_volume_name(image)
	log("booting: " + " ".join(qemu))

	started = time.time()
	qemu_stderr = open(os.path.join(results_dir, "qemu.stderr.log"), "wb")
	process = subprocess.Popen(qemu, stdin=subprocess.PIPE,
		stdout=subprocess.PIPE, stderr=qemu_stderr)
	reader = SerialReader(process, serial_log, monitor_path, qmp_path)
	reader.start()

	status = "exited"
	stalled_at = None
	pending_shots = sorted(args.screenshots)
	shots = []
	while process.poll() is None:
		time.sleep(0.25 if pending_shots else 1)
		elapsed = time.time() - started
		while pending_shots and elapsed >= pending_shots[0]:
			at = pending_shots.pop(0)
			path = os.path.join(results_dir, "screen-%05.1fs.ppm" % at)
			if monitor_command(monitor_path, "screendump " + path):
				shots.append(path)
		if reader.kdl.is_set():
			reader.debug_done.wait(timeout=KDL_MAX_BACKTRACES * 0.5 + 30)
			time.sleep(2)
			status = "stall-kernel-debugger" if stalled_at else \
				"kernel-debugger"
			break
		silent = time.time() - reader.last_output
		if stalled_at is None and silent > args.stall \
				and not reader.finished.is_set():
			# No serial output for a long time: press Alt+SysRq+D, Haiku's
			# emergency key for the kernel debugger, so that the session
			# above records where every thread is stuck.
			stalled_at = elapsed
			log("no serial output for %.0fs: entering the kernel debugger"
				% silent)
			report["boot"]["stall_after_seconds"] = round(elapsed - silent, 1)
			if not monitor_command(monitor_path, "sendkey alt-sysrq-d"):
				status = "stall"
				break
		if stalled_at is not None and not reader.kdl.is_set() \
				and elapsed - stalled_at > 30:
			log("the kernel debugger did not respond (interrupts off?)")
			status = "stall"
			break
		if elapsed > args.timeout:
			status = "timeout"
			break
		if reader.finished.is_set() and elapsed > 5:
			# The guest is shutting down; give it a little time.
			try:
				process.wait(timeout=60)
			except subprocess.TimeoutExpired:
				status = "shutdown-hang"
			break

	if process.poll() is None:
		process.kill()
		process.wait()
	reader.done.wait(timeout=10)

	qemu_stderr.close()
	for copy in (decoy, patched):
		# Full copies of the image, only needed while the guest runs.
		if copy is not None:
			os.remove(copy)
	report["boot"]["screenshots"] = convert_screenshots(shots
		+ reader.screenshots)
	if reader.kdl.is_set():
		# The whole debugger session (every backtrace); summary.md only
		# shows the start of it.
		with open(os.path.join(results_dir, "kdl.log"), "w") as kdlFile:
			kdlFile.write(reader.kdl_text.replace("\r", ""))
		report["boot"]["kdl_log"] = "kdl.log"
	for path in (monitor_path, qmp_path):
		if os.path.exists(path):
			os.unlink(path)
	report["boot"]["status"] = status
	report["boot"]["seconds"] = round(time.time() - started, 1)
	report["boot"]["runner_finished"] = reader.finished.is_set()
	report["boot"]["host_requests"] = reader.host_requests
	log("guest %s after %.0fs" % (status, time.time() - started))
	return status == "exited" and reader.finished.is_set()


# #pragma mark - analysis


def parse_serial(results_dir, report):
	serial_log = os.path.join(results_dir, "serial.log")
	tests = []
	suites = {}
	driver_lines = []
	problems = []
	try:
		with open(serial_log, "rb") as logFile:
			text = logFile.read().decode("latin-1")
	except OSError:
		text = ""

	for line in text.splitlines():
		line = line.rstrip("\r")
		# Test output is only accepted at the start of a line: the same text
		# can show up again inside other log lines (e.g. syslog echoes).
		match = TEST_PATTERN.match(line)
		if match:
			tests.append({"name": match.group(1),
				"pass": match.group(2) == "PASS",
				"detail": match.group(3).strip()})
			continue
		match = SUITE_PATTERN.match(line)
		if match:
			suites.setdefault(match.group(1), []).append(
				(match.group(2) + match.group(3)).strip())
			continue
		if "virtio_sound" in line:
			driver_lines.append(line)
		if PANIC_PATTERN.search(line) or "vm_page_fault" in line \
				or "Welcome to Kernel Debugging Land" in line:
			problems.append(line)

	settle_media_changed(text, tests)

	# Keep the debugger output (backtrace) if there was a crash.
	kdl_index = text.find("Welcome to Kernel Debugging Land")
	if kdl_index >= 0:
		start = text.rfind("\n", 0, max(0, kdl_index - 2000))
		report["kernel_debugger"] = text[start:start + 8000]

	for driver in WATCHED_DRIVERS:
		marker = "\x1b[33m" + driver + ":"
		errors = [line.replace("\x1b[33m", "").replace("\x1b[0m", "")
			for line in text.splitlines()
			if marker in line and not line.startswith("KERN: ")]
		tests.append({"name": "driver.%s.errors" % driver,
			"pass": len(errors) == 0,
			"detail": "%d error line(s)%s" % (len(errors),
				": " + errors[0].strip() if errors else "")})

	report["tests"] = tests
	report["suites"] = suites
	report["driver_log"] = driver_lines[-80:]
	report["problems"] = problems[:40]


def settle_media_changed(text, tests):
	"""vblk_test's resize_media_changed check reads B_GET_MEDIA_STATUS once
	after the resize, and that ioctl clears the flag. The kernel's media
	checker thread polls every disk once a second and clears it too; when it
	gets there first it rescans the disk and logs "Media changed from
	<path>". Either reader seeing the change shows the driver reported it,
	so a miss in the test counts as a pass if the kernel logged the change
	for the disk the test resized (the first scratch disk it found)."""
	resized = re.search(r"^CE-INFO (\S+): scratch disk", text, re.MULTILINE)
	for test in tests:
		if test["name"] != "vblk.resize_media_changed" or test["pass"] \
				or resized is None:
			continue
		if "Media changed from %s" % resized.group(1) in text:
			test["pass"] = True
			test["detail"] += "; the kernel's media checker read it first " \
				"and logged \"Media changed from %s\"" % resized.group(1)


def parse_qemu_log(results_dir, report):
	"""Guest errors QEMU's device models logged about the devices under test.
	These are independent of the driver's own view: a malformed virtio
	request or an invalid state transition shows up here."""
	lines = []
	for name in ("qemu.log", "qemu.stderr.log"):
		try:
			with open(os.path.join(results_dir, name), errors="replace") as f:
				lines += [l.rstrip() for l in f if l.strip()]
		except OSError:
			pass
	report["qemu_log"] = lines[-60:]
	# QEMU's wav backend has no input, so it always warns about the sound
	# card's capture voice; that is not a guest error.
	# Lines saying something is "unimplemented" describe QEMU's own limits
	# (-d unimp), not mistakes of the guest.
	device_errors = [l for l in lines
		if ("virtio-snd" in l or "virtio_snd" in l or "virtio-sound" in l)
			and "backend for voice" not in l and "unimplemented" not in l]
	report.setdefault("tests", []).append({"name": "qemu.virtio_sound",
		"pass": len(device_errors) == 0,
		"detail": "%d device model complaint(s)%s" % (len(device_errors),
			": " + device_errors[0] if device_errors else "")})

	# Accesses the memory API rejects (unaligned, or to unmapped addresses)
	# are guest bugs whatever the device: a driver reading its registers the
	# wrong way gets zeroes or all-ones back without any error. This is how
	# virtio_block's unaligned configuration reads showed up.
	# OVMF probes for a TPM at 0xfed40000 on every boot; with no TPM
	# configured QEMU rejects those reads. That is the firmware, not Haiku.
	invalid = [l for l in lines if (l.startswith("Invalid read")
			or l.startswith("Invalid write"))
		and not re.search(r"addr 0x[Ff][Ee][Dd]4[0-4][0-9A-Fa-f]{3},", l)]
	report["tests"].append({"name": "qemu.invalid_accesses",
		"pass": len(invalid) == 0,
		"detail": "%d rejected access(es)%s" % (len(invalid),
			": " + invalid[0] if invalid else "")})


def read_wav(path):
	"""Returns (rate, channels, samples) where samples is a list per channel.

	QEMU only fixes up the WAV header sizes when it exits cleanly, so the
	data chunk size is ignored and everything after its header is read.
	"""
	with open(path, "rb") as wavFile:
		data = wavFile.read()
	if len(data) < 44 or data[0:4] != b"RIFF" or data[8:12] != b"WAVE":
		raise ValueError("not a WAV file")

	offset = 12
	rate = channels = bits = None
	payload = b""
	while offset + 8 <= len(data):
		chunk_id = data[offset:offset + 4]
		chunk_size = struct.unpack_from("<I", data, offset + 4)[0]
		body = offset + 8
		if chunk_id == b"fmt ":
			_, channels, rate, _, _, bits = struct.unpack_from("<HHIIHH",
				data, body)
		elif chunk_id == b"data":
			payload = data[body:]
			break
		offset = body + chunk_size + (chunk_size & 1)

	if bits != 16 or channels is None:
		raise ValueError("unsupported WAV format (%s bits)" % bits)

	frame_count = len(payload) // (2 * channels)
	values = struct.unpack("<%dh" % (frame_count * channels),
		payload[:frame_count * channels * 2])
	samples = [values[c::channels] for c in range(channels)]
	return rate, channels, samples


def goertzel_power(samples, rate, hertz):
	coefficient = 2 * math.cos(2 * math.pi * hertz / rate)
	s1 = s2 = 0.0
	for x in samples:
		s0 = x + coefficient * s1 - s2
		s2 = s1
		s1 = s0
	return s1 * s1 + s2 * s2 - coefficient * s1 * s2


def check_image_identity(args, results_dir, report):
	"""For Renga images (renga-* profiles): the boot volume is named Renga and
	the Techniix package repository is configured, as runner.sh reports."""
	if not args.profile.startswith("renga-"):
		return
	try:
		with open(os.path.join(results_dir, "serial.log"), errors="replace") \
				as serial:
			text = serial.read()
	except OSError:
		text = ""
	checks = report.setdefault("host_checks", [])
	volume = re.search(r'Volume Name: "([^"]*)"', text)
	checks.append({"name": "image.volume_name",
		"pass": volume is not None and volume.group(1) == "Renga",
		"detail": "boot volume %s (want \"Renga\")" % (
			'"%s"' % volume.group(1) if volume else "not reported")})
	repositories = re.search(r"CE-INFO package repositories: ([^\r\n]*)",
		text)
	names = repositories.group(1).split() if repositories else []
	checks.append({"name": "image.repositories",
		"pass": "Techniix" in names and "HaikuPorts" in names,
		"detail": "configured: %s (want Techniix next to HaikuPorts)" % (
			" ".join(names) if names else "not reported")})

	# The user's own repository wins over HaikuPorts for a package both
	# offer: the package solver prefers the lower configured priority.
	priorities = repository_priorities(text)
	techniix = priorities.get("Techniix")
	haikuports = priorities.get("HaikuPorts")
	checks.append({"name": "image.repository_priority",
		"pass": techniix is not None and haikuports is not None
			and techniix < haikuports,
		"detail": "priorities: %s (want Techniix lower, i.e. better, than "
			"HaikuPorts)" % (", ".join("%s %d" % item
				for item in sorted(priorities.items()))
			if priorities else "not reported")})


def repository_priorities(text):
	"""Repository name -> configured priority, from the "pkgman list-repos"
	output runner.sh prints. Other console output (package_daemon, the
	kernel) may be interleaved; only the repository name lines (" " or
	" [User]", six columns, the name) and their "priority:" lines count."""
	start = text.find("CE-INFO pkgman list-repos:")
	if start < 0:
		return {}
	priorities = {}
	current = None
	for line in text[start:].splitlines()[1:]:
		if line.startswith("CE-"):
			break
		name = re.match(r"^ (?:\[User\]| {6}) (\S+)\s*$", line)
		if name:
			current = name.group(1)
			continue
		priority = re.match(r"^\t\tpriority:\s+(\d+)\s*$", line)
		if priority and current is not None:
			priorities[current] = int(priority.group(1))
			current = None
	return priorities


def iso_volume_id(path):
	"""The ISO 9660 volume ID of an anyboot or CD image, as xorriso reads it
	from the primary volume descriptor, or None."""
	try:
		result = subprocess.run(["xorriso", "-indev", path, "-pvd_info"],
			stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=60)
	except (OSError, subprocess.TimeoutExpired):
		return None
	match = re.search(r"^Volume Id\s*:\s*(.*?)\s*$",
		result.stdout.decode("utf-8", "replace"), re.MULTILINE)
	return match.group(1) if match else None


def check_branding(args, results_dir, report):
	"""Renga, not Haiku, as the OS name users see (Haiku, Inc.'s distribution
	guidelines reserve the name for the official distribution). Every run:
	the boot loader's welcome line on the serial console. Anyboot/CD images
	also: the ISO volume ID, which is what file managers and firmware boot
	menus show for the medium."""
	try:
		with open(os.path.join(results_dir, "serial.log"), errors="replace") \
				as serial:
			text = serial.read()
	except OSError:
		text = ""
	checks = report.setdefault("host_checks", [])
	welcome = re.search(r"Welcome to the (.*) boot loader!", text)
	checks.append({"name": "branding.boot_loader",
		"pass": welcome is not None and welcome.group(1) == "Renga",
		"detail": "boot loader says \"%s\" (want \"Welcome to the Renga boot "
			"loader!\")" % (welcome.group(0) if welcome
				else "no welcome line")})

	image = image_path(args)
	if not image.endswith(".iso"):
		return
	volume_id = iso_volume_id(image)
	checks.append({"name": "branding.iso_volume_id",
		"pass": volume_id is not None and volume_id.startswith("renga-")
			and "haiku" not in volume_id.lower(),
		"detail": "volume ID %s (want renga-<version>-<arch>)" % (
			'"%s"' % volume_id if volume_id is not None else "not readable")})


def check_pointer_devices(args, results_dir, report):
	"""Host-side half of the pointer suite: every absolute device the
	harness attached was found and tested in the guest, unless the image
	has no desktop to test with, and QEMU took every pointer request."""
	checks = report.setdefault("host_checks", [])
	if not args.pointer_devices or "pointer" not in report.get("suites", {}):
		return
	try:
		with open(os.path.join(results_dir, "serial.log"), "rb") as serial:
			text = serial.read().decode("latin-1")
	except OSError:
		text = ""
	if "CE-INFO pointer: no desktop" in text:
		return
	names = [t["name"] for t in report.get("tests", [])]
	for device in args.pointer_devices:
		positions = [n for n in names
			if n.startswith("pointer.%s.pos." % device)]
		checks.append({
			"name": "host.pointer_%s_tested" % device,
			"pass": len(positions) > 0,
			"detail": "%d positions checked in the guest for the attached "
				"%s" % (len(positions), POINTER_DEVICES[device][1]),
		})
	requests = [r for r in report.get("boot", {}).get("host_requests", [])
		if r["request"].split()[0] in ("pointer", "pointer_rel")]
	failed = [r for r in requests if not r["accepted"] or r.get("error")]
	checks.append({
		"name": "host.pointer_requests",
		"pass": len(requests) > 0 and not failed,
		"detail": "%d of %d carried out%s" % (len(requests) - len(failed),
			len(requests), "; first problem: %s (%s)" % (failed[0]["request"],
				failed[0].get("error", "refused")) if failed else ""),
	})


def check_scratch_disks(results_dir, report):
	"""Host-side half of vblk_test: the trimmed space must be free again."""
	checks = report.setdefault("host_checks", [])
	low, high = SCRATCH_ALLOCATED_RANGE
	# Only meaningful if vblk_test ran (an older image may not have it).
	ran = "vblk" in report.get("suites", {})
	for name, properties in SCRATCH_DISKS:
		path = os.path.join(results_dir, name + ".img")
		if not os.path.exists(path):
			continue
		allocated = os.stat(path).st_blocks * 512
		if not ran:
			os.remove(path)
			continue
		checks.append({
			"name": "host.%s_space_returned" % name,
			"pass": low <= allocated <= high,
			"detail": "%.2f MiB allocated after the run (want %.0f-%.0f MiB; "
				"8 MiB means trims never reached the file) [%s]" % (
				allocated / 1048576.0, low / 1048576.0, high / 1048576.0,
				properties),
		})
		# The image is only useful for this check.
		os.remove(path)


def nvme_mbr(block_size):
	"""A master boot record with one partition from 4 to 8 MiB, in units of
	the namespace's logical block size (which is what an MBR counts in)."""
	MiB = 1024 * 1024
	entry = struct.pack("<B3sB3sII", 0, b"\xfe\xff\xff", 0x83,
		b"\xfe\xff\xff", 4 * MiB // block_size, 4 * MiB // block_size)
	return bytes(446) + entry + bytes(48) + b"\x55\xaa"


def nvme_expected_layout(nsid, size, block_size, pattern):
	"""What a namespace holds after nvme_test: (offset, data) in the order
	it was written; everything else must be zero. Keep this in step with the
	layout comment in tests/nvme/nvme_test.cpp."""
	KiB = 1024
	MiB = 1024 * KiB
	fill = lambda length, value: bytes([value]) * length
	layout = [
		(0, nvme_mbr(block_size)),				# written by the harness
		(1 * MiB, fill(1 * MiB, pattern)),		# the pattern ...
		(size - 4 * KiB, fill(4 * KiB, pattern)),	# ... and the last blocks
		(4 * MiB, fill(64 * KiB, 0xe5)),		# through the partition ...
		(8 * MiB - 4 * KiB, fill(4 * KiB, 0xe5)),	# ... start and end
	]
	if nsid == 2:
		layout += [
			(1 * MiB + 768 * KiB, fill(64 * KiB, 0xc3)),	# cross-talk write
			(2 * MiB - 4086, fill(100, 0xd4)),			# unaligned write
		]
	layout.append((1 * MiB, fill(512 * KiB, 0)))	# trimmed at the end
	return layout


def check_nvme_namespaces(results_dir, report, namespaces=None):
	"""Host-side half of nvme_test: each namespace's backing file must hold
	exactly what the guest wrote to that namespace, at the same offsets, and
	nothing else (a request sent to the wrong namespace shows up as data in
	the other file, or as missing data in this one); and the trimmed space
	must have been given back."""
	if namespaces is None:
		namespaces = NVME_NAMESPACES
	checks = report.setdefault("host_checks", [])
	ran = "nvme" in report.get("suites", {})
	for name, nsid, size, block_size, pattern in namespaces:
		path = os.path.join(results_dir, name + ".img")
		if not os.path.exists(path):
			continue
		if not ran:
			os.remove(path)
			continue
		with open(path, "rb") as image:
			data = image.read()
		expected = bytearray(size)
		for offset, content in nvme_expected_layout(nsid, size, block_size,
				pattern):
			expected[offset:offset + len(content)] = content
		problem = None
		if len(data) != size:
			problem = "file is %d bytes, want %d" % (len(data), size)
		elif data != expected:
			chunk = 4096
			start = next(i for i in range(0, size, chunk)
				if data[i:i + chunk] != expected[i:i + chunk])
			first = next(i for i in range(start, min(size, start + chunk))
				if data[i] != expected[i])
			problem = "first difference at %d: 0x%02x, want 0x%02x" % (
				first, data[first], expected[first])
		written = size - expected.count(0)
		checks.append({
			"name": "host.%s_content" % name,
			"pass": problem is None,
			"detail": "%s (nsid %d, %d-byte blocks, %d non-zero bytes "
				"expected)" % (problem or "exactly what the guest wrote here",
				nsid, block_size, written),
		})
		# 1 MiB + 4 KiB is written, then 512 KiB of it trimmed; the MBR and
		# the partition writes add 76 KiB.
		allocated = os.stat(path).st_blocks * 512
		low, high = 512 * 1024, 1024 * 1024
		checks.append({
			"name": "host.%s_space_returned" % name,
			"pass": low <= allocated < high,
			"detail": "%.2f MiB allocated after the run (want %.2f to under "
				"%.2f MiB; more means the trim never reached the file)" % (
				allocated / 1048576.0, low / 1048576.0, high / 1048576.0),
		})
		os.remove(path)


def analyze_audio(results_dir, report, phases):
	"""Splits the recording into 100 ms windows, finds the dominant test tone
	per channel, groups windows into segments and compares them with the
	expected phases. Purity (share of window energy at the tone frequency)
	exposes glitches, gaps and corrupted buffers.
	"""
	path = os.path.join(results_dir, "audio.wav")
	audio = {"file": path, "checks": []}
	report["audio"] = audio
	try:
		rate, channels, samples = read_wav(path)
	except (OSError, ValueError, struct.error) as error:
		audio["error"] = str(error)
		audio["checks"].append({"name": "audio.recording", "pass": False,
			"detail": str(error)})
		return

	window = rate // 10
	candidates = sorted({hz for phase in phases for hz in phase[1:3]})
	frames = len(samples[0])
	audio["seconds"] = round(frames / rate, 2)

	labels = []
	for start in range(0, frames - window + 1, window):
		label = []
		for channel in range(min(channels, 2)):
			chunk = samples[channel][start:start + window]
			energy = sum(x * x for x in chunk)
			rms = math.sqrt(energy / window)
			if rms < 300:
				label.append((None, 0.0))
				continue
			best = max(candidates,
				key=lambda hz: goertzel_power(chunk, rate, hz))
			purity = 2 * goertzel_power(chunk, rate, best) / (window * energy)
			label.append((best, purity))
		labels.append(label)

	# Group consecutive windows with the same (left, right) tone.
	segments = []
	for index, label in enumerate(labels):
		key = tuple(hz for hz, _ in label)
		if segments and segments[-1]["key"] == key:
			segments[-1]["windows"].append(label)
		else:
			segments.append({"key": key, "start": index / 10.0,
				"windows": [label]})

	summary = []
	for segment in segments:
		if all(hz is None for hz in segment["key"]):
			continue
		# The first and last window of a segment straddle its edges (silence
		# or the neighbouring phase), so only interior windows count.
		windows = segment["windows"]
		interior = windows[1:-1] if len(windows) > 2 else windows
		purities = [p for label in interior for _, p in label]
		summary.append({
			"start": segment["start"],
			"seconds": len(segment["windows"]) / 10.0,
			"left_hz": segment["key"][0],
			"right_hz": segment["key"][1] if len(segment["key"]) > 1 else None,
			"min_purity": round(min(purities), 3),
		})
	audio["segments"] = summary

	for name, left, right, seconds in phases:
		matching = [s for s in summary
			if s["left_hz"] == left and s["right_hz"] == right]
		total = sum(s["seconds"] for s in matching)
		# Interior windows of a clean sine are > 0.95; a 50 ms gap drops a
		# window to about 0.5.
		worst = min((s["min_purity"] for s in matching if s["seconds"] > 0.3),
			default=0.0)
		pieces = len([s for s in matching if s["seconds"] >= 0.2])
		passed = total >= 0.8 * seconds and pieces == 1 and worst >= 0.6
		audio["checks"].append({
			"name": "audio.%s" % name,
			"pass": passed,
			"detail": "%d/%d Hz: %.1fs of %.1fs expected, %d segment(s), "
				"min purity %.3f" % (left, right, total, seconds, pieces,
					worst),
		})


# #pragma mark - report


def write_report(results_dir, report):
	checks = list(report.get("tests", []))
	checks += report.get("audio", {}).get("checks", [])
	checks += report.get("host_checks", [])
	failed = [c for c in checks if not c["pass"]]

	build_ok = report.get("build", {}).get("compile_status", 0) == 0 \
		and report.get("build", {}).get("image_status", 0) == 0
	boot = report.get("boot", {})
	boot_ok = boot.get("status") in (None, "exited") \
		and boot.get("runner_finished", True)
	suites_done = all(any(s.startswith("DONE") for s in states)
		for states in report.get("suites", {}).values())

	report["verdict"] = {
		"pass": build_ok and boot_ok and suites_done and not failed
			and not report.get("problems") and len(checks) > 0
			or (report.get("compile_only") and build_ok),
		"checks": len(checks),
		"failed": len(failed),
	}

	with open(os.path.join(results_dir, "results.json"), "w") as jsonFile:
		json.dump(report, jsonFile, indent=2, default=str)

	lines = ["# Renga test run %s" % report["started"], ""]
	verdict = report["verdict"]
	lines.append("**Verdict: %s** (%d checks, %d failed)" % (
		"PASS" if verdict["pass"] else "FAIL", verdict["checks"],
		verdict["failed"]))
	lines.append("")
	lines.append("Image `%s`, %s firmware, booted as %s." % (
		report.get("profile"), report.get("firmware"),
		"a CD-ROM" if report.get("media") == "cdrom"
			else "a %s hard disk" % ("virtio" if report.get("disk_bus")
				== "virtio" else "IDE")))
	if report.get("boot_menu_timeout"):
		lines[-1] += " Its settings set a %d s boot menu timeout." \
			% report["boot_menu_timeout"]
	if report.get("two_installs"):
		lines[-1] += (" A renamed second install (\"%s\") was attached in "
			"front of it." % DECOY_VOLUME_NAME)
	lines.append("")

	build_info = report.get("build")
	if build_info:
		lines.append("## Build")
		lines.append("- compile targets: status %s in %ss" % (
			build_info.get("compile_status"),
			build_info.get("compile_seconds")))
		if "image_status" in build_info:
			lines.append("- image: status %s in %ss" % (
				build_info["image_status"], build_info["image_seconds"]))
		for issue in build_info.get("issues", []):
			lines.append("    " + issue)
		lines.append("")

	if boot:
		lines.append("## Boot")
		lines.append("- %s after %ss, runner finished: %s" % (
			boot.get("status"), boot.get("seconds"),
			boot.get("runner_finished")))
		if "stall_after_seconds" in boot:
			lines.append("- serial output stopped at %ss; the harness pressed "
				"Alt+SysRq+D" % boot["stall_after_seconds"])
		if boot.get("screenshots"):
			lines.append("- screenshots: " + ", ".join(boot["screenshots"]))
		if boot.get("kdl_log"):
			lines.append("- full kernel debugger session: %s"
				% boot["kdl_log"])
		lines.append("")

	if checks:
		lines.append("## Checks")
		lines.append("| Check | Result | Detail |")
		lines.append("| --- | --- | --- |")
		for check in checks:
			lines.append("| %s | %s | %s |" % (check["name"],
				"PASS" if check["pass"] else "**FAIL**",
				check["detail"].replace("|", "/")))
		lines.append("")

	segments = report.get("audio", {}).get("segments")
	if segments is not None:
		lines.append("## Audio segments (%ss recorded)" %
			report["audio"].get("seconds"))
		for segment in segments:
			lines.append("- %5.1fs  %4.1fs  L %s Hz / R %s Hz  purity >= %s" % (
				segment["start"], segment["seconds"], segment["left_hz"],
				segment["right_hz"], segment["min_purity"]))
		lines.append("")

	if report.get("problems"):
		lines.append("## Kernel problems")
		lines += ["    " + p for p in report["problems"]]
		lines.append("")
	if report.get("kernel_debugger"):
		lines.append("## Kernel debugger output")
		lines.append("```")
		lines.append(report["kernel_debugger"])
		lines.append("```")
		lines.append("")
	if report.get("qemu_log"):
		lines.append("## QEMU log")
		lines += ["    " + l for l in report["qemu_log"][-20:]]
		lines.append("")
	if report.get("driver_log"):
		lines.append("## Driver log")
		lines += ["    " + l for l in report["driver_log"][-30:]]
		lines.append("")

	summary = "\n".join(lines)
	with open(os.path.join(results_dir, "summary.md"), "w") as summaryFile:
		summaryFile.write(summary + "\n")
	return summary, verdict["pass"]


def main():
	parser = argparse.ArgumentParser(description=__doc__,
		formatter_class=argparse.RawDescriptionHelpFormatter)
	parser.add_argument("--output", default=os.path.join(HAIKU_TOP,
		"generated.x86_64"), help="configured Haiku output directory")
	parser.add_argument("--profile", default=None,
		help="build profile of the test image (default: minimum-raw, or "
			"minimum-anyboot with --firmware uefi or --media cdrom, since "
			"only anyboot images carry an EFI system partition and an El "
			"Torito boot catalog)")
	parser.add_argument("--no-build", action="store_true",
		help="boot the existing image without building")
	parser.add_argument("--compile-only", action="store_true",
		help="only compile the test targets, do not build or boot an image")
	parser.add_argument("--firmware", choices=("bios", "uefi"),
		default="bios")
	parser.add_argument("--ovmf", default="/usr/share/OVMF/OVMF_CODE_4M.fd",
		help="OVMF firmware code (read-only pflash)")
	parser.add_argument("--ovmf-vars", default="/usr/share/OVMF/OVMF_VARS_4M.fd",
		help="OVMF variable store template; copied for every run")
	parser.add_argument("--media", choices=("disk", "cdrom"), default="disk",
		help="attach the image as a hard disk (default) or as a CD-ROM")
	parser.add_argument("--disk-bus", choices=("ide", "virtio"), default="ide",
		help="bus of the boot disk: IDE (default) or virtio-blk, which boots "
			"through the virtio_block driver")
	parser.add_argument("--two-installs", action="store_true",
		help="with --firmware uefi: attach a second, renamed install "
			"without an EFI system partition in front of the image (first "
			"on the bus and in the boot order) and check that the guest "
			"booted the install whose boot loader the firmware started")
	parser.add_argument("--boot-menu-timeout", type=int, default=0,
		metavar="SECONDS",
		help="boot a copy of the image with boot_menu_timeout SECONDS in its "
			"kernel settings and check the boot loader's countdown")
	parser.add_argument("--pointer-devices", default="usb,virtio",
		type=lambda text: [t for t in text.split(",")
			if t.strip() and t != "none"],
		help="comma-separated absolute pointing devices to attach for the "
			"pointer suite: %s, or none (default: usb,virtio)"
			% ", ".join(sorted(POINTER_DEVICES)))
	parser.add_argument("--usb-controller", choices=sorted(USB_CONTROLLERS),
		default="xhci",
		help="host controller of the USB tablet (default: xhci; VirtualBox "
			"uses ohci+ehci or xhci)")
	parser.add_argument("--timeout", type=int, default=1800,
		help="seconds before the guest is considered hung")
	parser.add_argument("--memory", type=int, default=1024)
	parser.add_argument("--smp", type=int, default=1,
		help="number of virtual CPUs (2 or more to exercise SMP code)")
	parser.add_argument("--screenshots", default="",
		type=lambda text: [float(t) for t in text.split(",") if t.strip()],
		help="comma-separated seconds after QEMU starts at which to save "
			"screen-<t>s.png (e.g. 2,4,8 for the boot splash)")
	parser.add_argument("--stall", type=int, default=90,
		help="seconds without serial output before the harness enters the "
			"kernel debugger to record backtraces")
	parser.add_argument("--jobs", type=int, default=os.cpu_count() or 2)
	parser.add_argument("--verbose", action="store_true",
		help="build the drivers under test with their TRACE() output on")
	args = parser.parse_args()
	args.output = os.path.abspath(args.output)
	if args.two_installs and (args.media == "cdrom"
			or args.firmware != "uefi"):
		parser.error("--two-installs tests the EFI boot loader's choice "
			"between hard disks; it needs --firmware uefi and --media disk")
	for device in args.pointer_devices:
		if device not in POINTER_DEVICES:
			parser.error("unknown pointer device %s (choose from %s)"
				% (device, ", ".join(sorted(POINTER_DEVICES))))
	if args.profile is None:
		args.profile = "minimum-anyboot" \
			if args.firmware == "uefi" or args.media == "cdrom" \
			else "minimum-raw"

	started = datetime.datetime.now()
	results_root = os.path.join(args.output, "ce-test-results")
	results_dir = os.path.join(results_root,
		started.strftime("%Y%m%d-%H%M%S"))
	os.makedirs(results_dir, exist_ok=True)
	latest = os.path.join(results_root, "latest")
	if os.path.islink(latest) or os.path.exists(latest):
		os.remove(latest)
	os.symlink(os.path.basename(results_dir), latest)

	report = {"started": started.isoformat(timespec="seconds"),
		"profile": args.profile, "firmware": args.firmware,
		"media": args.media, "disk_bus": args.disk_bus,
		"two_installs": args.two_installs,
		"boot_menu_timeout": args.boot_menu_timeout,
		"pointer_devices": args.pointer_devices,
		"usb_controller": args.usb_controller,
		"compile_only": args.compile_only}

	ok = True
	if not args.no_build:
		ok = build(args, results_dir, report)
	if ok and not args.compile_only:
		boot(args, results_dir, report)
		parse_serial(results_dir, report)
		parse_qemu_log(results_dir, report)
		check_scratch_disks(results_dir, report)
		check_pointer_devices(args, results_dir, report)
		check_nvme_namespaces(results_dir, report)
		check_image_identity(args, results_dir, report)
		check_branding(args, results_dir, report)
		check_two_installs(args, results_dir, report)
		check_boot_menu(args, results_dir, report)
		analyze_audio(results_dir, report, VIRTIO_SOUND_PHASES)

	summary, passed = write_report(results_dir, report)
	print(summary)
	log("results in " + results_dir)
	return 0 if passed else 1


if __name__ == "__main__":
	sys.exit(main())
