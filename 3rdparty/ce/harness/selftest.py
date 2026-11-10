#!/usr/bin/env python3
#
# Copyright 2026, Renga contributors.
# Distributed under the terms of the MIT License.
#
"""Self-test for ce-test.py's result analysis.

A harness that silently passes broken output is worse than none, so this
feeds the audio and serial analysis known-good and known-bad input and
checks the verdicts. Runs on the host in a few seconds:

    3rdparty/ce/harness/selftest.py
"""

import importlib.util
import math
import os
import struct
import subprocess
import sys
import tempfile


HERE = os.path.dirname(os.path.abspath(__file__))
spec = importlib.util.spec_from_file_location("ce_test",
	os.path.join(HERE, "ce-test.py"))
ce_test = importlib.util.module_from_spec(spec)
spec.loader.exec_module(ce_test)

RATE = 48000
failures = 0


def write_wav(path, frames, finalize=True):
	"""frames: list of (left, right) int16 tuples."""
	payload = b"".join(struct.pack("<hh", l, r) for l, r in frames)
	size = len(payload) if finalize else 0	# QEMU killed: sizes stay 0
	header = b"RIFF" + struct.pack("<I", 36 + size if finalize else 0) \
		+ b"WAVE" + b"fmt " + struct.pack("<IHHIIHH", 16, 1, 2, RATE,
			RATE * 4, 4, 16) + b"data" + struct.pack("<I", size)
	with open(path, "wb") as wavFile:
		wavFile.write(header + payload)


def tone(left, right, seconds, rate=RATE):
	count = int(seconds * rate)
	return [(int(12000 * math.sin(2 * math.pi * left * i / rate)),
		int(12000 * math.sin(2 * math.pi * right * i / rate)))
		for i in range(count)]


def silence(seconds):
	return [(0, 0)] * int(seconds * RATE)


def analyze(frames, finalize=True):
	directory = tempfile.mkdtemp()
	write_wav(os.path.join(directory, "audio.wav"), frames, finalize)
	report = {}
	ce_test.analyze_audio(directory, report, ce_test.VIRTIO_SOUND_PHASES)
	return {c["name"]: c["pass"] for c in report["audio"]["checks"]}, report


def expect(name, condition, detail=""):
	global failures
	print("%s %s %s" % ("ok  " if condition else "FAIL", name, detail))
	if not condition:
		failures += 1


good = silence(0.5) + tone(440, 880, 3.0) + silence(0.3) \
	+ tone(660, 330, 1.5) + silence(0.3) + tone(1000, 1000, 1.5) \
	+ silence(0.5) + tone(500, 250, 1.0) + silence(0.2)
checks, report = analyze(good)
expect("clean recording passes", all(checks.values()), str(checks))

# QEMU may record nothing while the stream is stopped, so phases can be
# back to back, with arbitrary (unaligned) boundaries.
adjacent = silence(0.37) + tone(440, 880, 3.03) + tone(660, 330, 1.52) \
	+ tone(1000, 1000, 1.49) + tone(500, 250, 1.01)
checks, report = analyze(adjacent)
expect("back-to-back phases pass", all(checks.values()),
	str(report["audio"]["segments"]))

checks, _ = analyze(good, finalize=False)
expect("unfinalized WAV header is handled", all(checks.values()),
	str(checks))

# A 50 ms dropout in the middle of phase 1 must be caught.
gap = silence(0.5) + tone(440, 880, 1.5) + silence(0.05) \
	+ tone(440, 880, 1.45) + silence(0.3) + tone(660, 330, 1.5) \
	+ silence(0.3) + tone(1000, 1000, 1.5) + tone(500, 250, 1.0)
checks, report = analyze(gap)
expect("dropout in phase 1 fails", not checks["audio.phase1"],
	str(report["audio"]["segments"][:3]))
expect("other phases unaffected by dropout",
	checks["audio.phase2"] and checks["audio.phase3"], str(checks))

# Missing phase 3 (e.g. the 44.1 kHz switch broke playback).
checks, _ = analyze(silence(0.5) + tone(440, 880, 3.0) + silence(0.3)
	+ tone(660, 330, 1.5))
expect("missing phase fails", not checks["audio.phase3"], str(checks))

# Swapped channels must not be accepted.
checks, _ = analyze(tone(880, 440, 3.0))
expect("swapped channels fail", not checks["audio.phase1"], str(checks))

# Serial parsing: test lines, suite markers and a kernel panic.
directory = tempfile.mkdtemp()
with open(os.path.join(directory, "serial.log"), "w") as serial:
	serial.write("boot noise\r\n"
		"CE-SUITE virtio_sound START\n"
		"CE-TEST virtio_sound.open PASS No error\n"
		"virtio_sound: using output stream 0\n"
		"CE-TEST virtio_sound.phase1.pacing FAIL 1000 frames/s measured\n"
		"PANIC: vm_page_fault: kernel fault\n"
		"CE-SUITE virtio_sound DONE failures=1\n"
		"KERN: CE-TEST virtio_sound.open PASS echoed by syslog\n"
		"\x1b[33mvirtio_sound:\x1b[0m device stopped returning buffers\n")
report = {}
ce_test.parse_serial(directory, report)
# Two test lines (the echoed one is ignored) plus one driver error check per
# watched driver.
expect("serial: tests parsed", len(report["tests"])
	== 2 + len(ce_test.WATCHED_DRIVERS)
	and report["tests"][1]["pass"] is False, str(report["tests"]))
expect("serial: panic detected", len(report["problems"]) == 1,
	str(report["problems"]))
expect("serial: driver log kept", any("output stream" in l
	for l in report["driver_log"]))
driverCheck = [t for t in report["tests"]
	if t["name"] == "driver.virtio_sound.errors"]
expect("serial: driver error line fails the run", len(driverCheck) == 1
	and not driverCheck[0]["pass"], str(driverCheck))
otherDrivers = [t for t in report["tests"] if t["name"].startswith("driver.")
	and t["name"] != "driver.virtio_sound.errors"]
expect("serial: other drivers unaffected", all(t["pass"] for t in otherDrivers),
	str(otherDrivers))

# Guest requests: only block_resize on a scratch disk is carried out, and
# only complete lines count.
reader = ce_test.SerialReader(None, os.path.join(directory, "x.log"))
reader._host_requests(b"CE-HOST block_resize vblk0 128M\nCE-HOST block_resize ")
reader._host_requests(b"boot 1G\nCE-HOST quit\nCE-HOST block_resize vblk1 64M;quit\n")
expect("host requests: only safe ones accepted",
	[r["accepted"] for r in reader.host_requests] == [True, False, False, False],
	str(reader.host_requests))

# NVMe namespaces: a backing file with exactly the expected content passes;
# data that belongs to the other namespace, or a missing write, fails.
def nvme_check(mutate):
	directory = tempfile.mkdtemp()
	size = 8 * 1024 * 1024
	namespaces = [("nvme_ns2", 2, size, 4096, 0xb2)]
	content = bytearray(size)
	for offset, data in ce_test.nvme_expected_layout(2, size, 4096, 0xb2):
		content[offset:offset + len(data)] = data
	mutate(content)
	# Written sparse, like the guest leaves it: only blocks with data.
	with open(os.path.join(directory, "nvme_ns2.img"), "wb") as image:
		image.truncate(size)
		for offset in range(0, size, 4096):
			block = bytes(content[offset:offset + 4096])
			if block.count(0) != len(block):
				image.seek(offset)
				image.write(block)
	report = {"suites": {"nvme": ["START", "DONE failures=0"]}}
	ce_test.check_nvme_namespaces(directory, report, namespaces)
	return {c["name"]: c for c in report["host_checks"]}

checks = nvme_check(lambda content: None)
expect("nvme: exact content passes", checks["host.nvme_ns2_content"]["pass"]
	and checks["host.nvme_ns2_space_returned"]["pass"], str(checks))
def untrimmed(content):
	content[1024 * 1024:1536 * 1024] = bytes([0xb2]) * (512 * 1024)
checks = nvme_check(untrimmed)
expect("nvme: untrimmed space fails",
	not checks["host.nvme_ns2_space_returned"]["pass"], str(checks))
def cross_talk(content):
	content[1024 * 1024 + 600 * 1024] = 0xa1
checks = nvme_check(cross_talk)
expect("nvme: other namespace's data fails",
	not checks["host.nvme_ns2_content"]["pass"]
	and "1662976" in checks["host.nvme_ns2_content"]["detail"], str(checks))
def missing_tail(content):
	content[-4096:] = bytes(4096)
checks = nvme_check(missing_tail)
expect("nvme: missing write fails", not checks["host.nvme_ns2_content"]["pass"],
	str(checks))

def media_changed(kernel_line):
	directory = tempfile.mkdtemp()
	with open(os.path.join(directory, "serial.log"), "w") as serial:
		serial.write("CE-INFO /dev/disk/virtual/virtio_block/0/raw: "
			"scratch disk\r\n"
			"CE-INFO /dev/disk/virtual/virtio_block/1/raw: scratch disk\r\n"
			"CE-HOST block_resize vblk0 128M\r\n" + kernel_line +
			"CE-TEST vblk.resize_media_changed FAIL B_GET_MEDIA_STATUS -> "
			"No error (want media changed)\r\n")
	report = {}
	ce_test.parse_serial(directory, report)
	return [t for t in report["tests"]
		if t["name"] == "vblk.resize_media_changed"][0]["pass"]

expect("vblk: media change seen by the kernel's checker passes",
	media_changed("Media changed from "
		"/dev/disk/virtual/virtio_block/0/raw\r\n"))
expect("vblk: media change seen by nobody fails", not media_changed(""))
expect("vblk: media change of another disk fails", not media_changed(
	"Media changed from /dev/disk/virtual/virtio_block/1/raw\r\n"))

class TwoInstallsArgs:
	two_installs = True

def two_installs(volume_line):
	directory = tempfile.mkdtemp()
	with open(os.path.join(directory, "serial.log"), "w") as serial:
		serial.write("CE-INFO boot volume:\r\n" + volume_line)
	report = {"boot": {"volume_name": "Haiku"}}
	ce_test.check_two_installs(TwoInstallsArgs, directory, report)
	return report["host_checks"][0]["pass"]

expect("two installs: booted volume passes",
	two_installs('  Volume Name: "Haiku"\r\n'))
expect("two installs: decoy volume fails",
	not two_installs('  Volume Name: "Decoy"\r\n'))
expect("two installs: no volume reported fails", not two_installs(""))

# make_decoy() on a miniature anyboot image: an MBR with a BFS partition
# (LBA 128) and an EFI system partition (LBA 256), an El Torito boot record
# at 2 KiB sector 17, and a BFS superblock.
directory = tempfile.mkdtemp()
original = os.path.join(directory, "anyboot.img")
image = bytearray(256 * 1024)
image[446:462] = bytes([0x80, 0, 0, 0, 0xeb, 0, 0, 0]) \
	+ struct.pack("<II", 128, 64)
image[462:478] = bytes([0, 0, 0, 0, 0xef, 0, 0, 0]) \
	+ struct.pack("<II", 256, 64)
image[510:512] = b"\x55\xaa"
for sector, kind in ((16, 1), (17, 0), (18, 0xff)):
	image[sector * 2048:sector * 2048 + 6] = bytes([kind]) + b"CD001"
image[17 * 2048 + 7:17 * 2048 + 30] = b"EL TORITO SPECIFICATION"
image[128 * 512 + 512:128 * 512 + 517] = b"Haiku"
image[128 * 512 + 544:128 * 512 + 548] = b"1SFB"
with open(original, "wb") as imageFile:
	imageFile.write(image)
decoy = os.path.join(directory, "decoy.img")
made = ce_test.make_decoy(original, decoy)
with open(decoy, "rb") as decoyFile:
	copy = decoyFile.read()
expect("decoy: made", made and len(copy) == len(image))
expect("decoy: renamed", ce_test.bfs_volume_name(decoy)
	== ce_test.DECOY_VOLUME_NAME and ce_test.bfs_volume_name(original)
	== "Haiku", str(ce_test.bfs_volume_name(decoy)))
expect("decoy: BFS partition kept, EFI system partition gone",
	copy[446:462] == image[446:462] and copy[462:478] == bytes(16))
expect("decoy: El Torito boot record gone",
	copy[17 * 2048:18 * 2048] == bytes(2048)
	and copy[16 * 2048:17 * 2048] == image[16 * 2048:17 * 2048])

class MenuArgs:
	boot_menu_timeout = 0

def boot_menu(seconds, lines):
	directory = tempfile.mkdtemp()
	with open(os.path.join(directory, "serial.timed.log"), "w") as serial:
		serial.write("[   0.100] Welcome to the Haiku boot loader!\r\n")
		for stamp, text in lines:
			serial.write("[%8.3f] %s\r\n" % (stamp, text))
	MenuArgs.boot_menu_timeout = seconds
	report = {}
	ce_test.check_boot_menu(MenuArgs, directory, report)
	return report["host_checks"][0]["pass"]

countdown = [(1.0, 'boot menu: "Continue booting" in 2 s unless a key is '
	'pressed'), (2.0, "\x1b[07;20HContinue booting in 1 s, press any key to "
	"stopboot menu: 1 s"), (2.0, 'boot menu: timeout, choosing "Continue '
	'booting"'), (3.2, "load kernel kernel_x86_64...")]
expect("boot menu: none configured, none shown passes", boot_menu(0, []))
expect("boot menu: none configured but shown fails",
	not boot_menu(0, countdown))
expect("boot menu: 2 s countdown passes", boot_menu(2, countdown))
expect("boot menu: wrong length fails", not boot_menu(3, countdown))
expect("boot menu: countdown stopped by a key fails", not boot_menu(2,
	countdown[:1] + [(1.5, "boot menu: countdown stopped by a key")]))
expect("boot menu: countdown too fast fails", not boot_menu(2,
	[(1.0, countdown[0][1]), (1.1, "boot menu: 1 s"),
		(1.2, countdown[2][1]), (1.3, "load kernel kernel_x86_64...")]))
expect("boot menu: no countdown at all fails", not boot_menu(2, []))

class BrandingArgs:
	profile = "minimum-raw"
	output = None

def branding(welcome, volume_id=None):
	"""Runs check_branding on a serial log with the given welcome line and,
	if volume_id is given, on an ISO made with that volume ID. Returns the
	verdict of every check, by name."""
	directory = tempfile.mkdtemp()
	with open(os.path.join(directory, "serial.log"), "w") as serial:
		serial.write("[   0.100] %s\r\nload kernel kernel_x86_64...\r\n"
			% welcome)
	BrandingArgs.output = directory
	BrandingArgs.profile = "minimum-raw"
	if volume_id is not None:
		BrandingArgs.profile = "minimum-anyboot"
		content = os.path.join(directory, "content")
		os.mkdir(content)
		with open(os.path.join(content, "file"), "w") as contentFile:
			contentFile.write("x")
		subprocess.run(["xorriso", "-as", "mkisofs", "-V", volume_id, "-o",
			ce_test.image_path(BrandingArgs), content],
			stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, check=True)
	report = {}
	ce_test.check_branding(BrandingArgs, directory, report)
	return {c["name"]: c["pass"] for c in report["host_checks"]}

verdicts = branding("Welcome to the Renga boot loader!")
expect("branding: Renga boot loader passes, raw image has no ISO check",
	verdicts == {"branding.boot_loader": True}, str(verdicts))
expect("branding: Haiku boot loader fails", branding(
	"Welcome to the Haiku boot loader!") == {"branding.boot_loader": False})
expect("branding: no welcome line fails",
	branding("load kernel") == {"branding.boot_loader": False})
expect("branding: renga ISO volume ID passes", branding(
	"Welcome to the Renga boot loader!", "renga-r1~beta6-x86_64")
	== {"branding.boot_loader": True, "branding.iso_volume_id": True})
expect("branding: haiku ISO volume ID fails", not branding(
	"Welcome to the Renga boot loader!", "haiku-r1~beta6-x86_64")
	["branding.iso_volume_id"])
expect("branding: haiku nightly-style ID with a renga prefix fails",
	not branding("Welcome to the Renga boot loader!",
		"renga-haiku-nightly")["branding.iso_volume_id"])

class IdentityArgs:
	profile = "renga-raw"

def repository_priority(priorities, noise=True):
	"""Runs check_image_identity on a serial log whose "pkgman list-repos"
	output gives the repositories these priorities (name -> number), with
	package_daemon lines in between as in real runs. Returns the verdict of
	image.repository_priority."""
	directory = tempfile.mkdtemp()
	lines = ['  Volume Name: "Renga"',
		"CE-INFO package repositories: " + " ".join(priorities),
		"CE-INFO pkgman list-repos:"]
	for name, priority in priorities.items():
		lines.append("        " + name)
		lines.append("\t\tbase-url:  https://example.invalid/" + name)
		if noise:
			lines.append("package_daemon: [1:  59] Doing first boot "
				"processing #7 for package which-2.21-6-x86_64.hpkg.")
		lines.append("\t\tidentifier: tag:example.invalid," + name)
		lines.append("\t\tpriority:  %d" % priority)
	lines.append("CE-INFO running nvme_test")
	with open(os.path.join(directory, "serial.log"), "w") as serial:
		serial.write("\r\n".join(lines) + "\r\n")
	report = {}
	ce_test.check_image_identity(IdentityArgs, directory, report)
	return {c["name"]: c["pass"] for c in report["host_checks"]}[
		"image.repository_priority"]

expect("identity: Techniix 1 over HaikuPorts 2 passes", repository_priority(
	{"Haiku": 1, "HaikuPorts": 2, "Techniix": 1}))
expect("identity: Techniix and HaikuPorts both 1 fails",
	not repository_priority({"Haiku": 1, "HaikuPorts": 1, "Techniix": 1}))
expect("identity: HaikuPorts better than Techniix fails",
	not repository_priority({"HaikuPorts": 1, "Techniix": 3}))
expect("identity: no Techniix fails",
	not repository_priority({"Haiku": 1, "HaikuPorts": 2}))
expect("identity: priority after the list (another CE- line) is ignored",
	not repository_priority({"HaikuPorts": 2}, noise=False)
	and ce_test.repository_priorities("CE-INFO pkgman list-repos:\r\n"
		"        Techniix\r\nCE-INFO x\r\n\t\tpriority:  1\r\n") == {})

# Pointer devices: every attached device must have been tested in the guest
# and every pointer request carried out; an image without a desktop skips.
def pointer_checks(tests, requests, serial=""):
	directory = tempfile.mkdtemp()
	with open(os.path.join(directory, "serial.log"), "w") as log:
		log.write(serial)
	args = type("Args", (), {"pointer_devices": ["usb", "virtio"]})()
	report = {"suites": {"pointer": ["START", "DONE failures=0"]},
		"tests": [{"name": n, "pass": True, "detail": ""} for n in tests],
		"boot": {"host_requests": requests}}
	ce_test.check_pointer_devices(args, directory, report)
	return {c["name"]: c["pass"] for c in report.get("host_checks", [])}

pointerOk = {"request": "pointer usb 0 0", "accepted": True, "error": None}
both = ["pointer.usb.pos.top_left", "pointer.virtio.pos.top_left"]
expect("pointer: both devices tested passes", all(pointer_checks(both,
	[pointerOk]).values()) and len(pointer_checks(both, [pointerOk])) == 3)
expect("pointer: an attached device the guest did not test fails",
	not pointer_checks(both[:1], [pointerOk])["host.pointer_virtio_tested"])
expect("pointer: a request QEMU did not carry out fails",
	not pointer_checks(both, [pointerOk, {"request": "pointer usb 1 1",
		"accepted": True, "error": "no mouse"}])["host.pointer_requests"])
expect("pointer: no requests at all fails",
	not pointer_checks(both, [])["host.pointer_requests"])
expect("pointer: no desktop skips the checks",
	pointer_checks([], [], "CE-INFO pointer: no desktop (x), skipped\n") == {})

for words, allowed in [
		(["sendkey", "ctrl-v"], True),
		(["sendkey", "ctrl-shift-v"], True),
		(["sendkey", "alt-tab"], True),
		(["sendkey", "ret"], True),
		(["sendkey", "alt-sysrq-d"], False),
		(["sendkey", "ctrl-alt-delete"], False),
		(["sendkey", "ctrl-alt-shift-v"], False),
		(["sendkey", "ctrl-ctrl-v"], False),
		(["sendkey", "ctrl-v", "extra"], False),
		(["sendkey", "ctrl-v;quit"], False),
		(["pointer", "usb", "0", "32767"], True),
		(["pointer", "virtio", "16384", "100"], True),
		(["pointer", "usb", "32768", "0"], False),
		(["pointer", "usb", "-1", "0"], False),
		(["pointer", "ps2", "0", "0"], False),
		(["pointer", "usb", "0"], False),
		(["pointer", "usb", "0", "0;quit"], False),
		(["pointer_rel", "40", "-30"], True),
		(["pointer_rel", "201", "0"], False),
		(["pointer_rel", "0x10", "0"], False),
		(["screenshot", "pointer_usb"], True),
		(["screenshot", "../evil"], False),
		(["screenshot", "a;quit"], False),
		(["quit"], False),
		(["block_resize", "vblk0", "128M"], True),
		(["block_resize", "hda", "128M"], False),
		(["audio_in", "700", "1300", "12000"], True),
		(["audio_in", "0", "20000", "32767"], True),
		(["audio_in", "20001", "100", "1000"], False),
		(["audio_in", "100", "100", "32768"], False),
		(["audio_in", "-5", "100", "1000"], False),
		(["audio_in", "100", "100"], False),
		(["audio_in", "100", "100", "1000;quit"], False)]:
	expect("host request %s %s" % (" ".join(words),
			"allowed" if allowed else "refused"),
		ce_test.host_request_allowed(words) == allowed)


# The capture feed: the file must hold the requested tone everywhere (SDL
# rereads it from the start on every PCM_PREPARE), seamlessly from one second
# to the next and across rewrites, and a new tone must replace the old one
# from the reader's position on.
def feed_tones(path):
	"""Dominant (left, right) Hz and purity of 100 ms windows of a feed."""
	with open(path, "rb") as feed:
		data = feed.read()
	values = struct.unpack("<%dh" % (len(data) // 2), data)
	window = RATE // 10
	results = []
	for start in range(0, len(values) // 2 - window, RATE * 5):
		result = []
		for channel in range(2):
			chunk = values[2 * start + channel:2 * (start + window):2]
			energy = sum(x * x for x in chunk)
			best = max((440, 700, 880, 1300),
				key=lambda hz: ce_test.goertzel_power(chunk, RATE, hz))
			result.append((best, 2 * ce_test.goertzel_power(chunk, RATE, best)
				/ (window * energy) if energy else 0))
		results.append(result)
	return results

directory = tempfile.mkdtemp()
feeder = ce_test.AudioFeeder(os.path.join(directory, "feed.raw"))
windows = feed_tones(feeder.path)
expect("capture feed holds the default tone",
	len(windows) >= 5 and all(w[0][0] == 700 and w[1][0] == 1300
		and w[0][1] > 0.99 and w[1][1] > 0.99 for w in windows),
	str(windows[:2]))
feeder.set_tone(440, 880, 8000)
feeder._write_tone(12345 * 4 + 2)
windows = feed_tones(feeder.path)
expect("capture feed is rewritten with the new tone everywhere",
	all(w[0][0] == 440 and w[1][0] == 880 and w[0][1] > 0.99
		and w[1][1] > 0.99 for w in windows), str(windows[:2]))
expect("capture feed stays ahead of the reader",
	os.path.getsize(feeder.path) >= (12345 + ce_test.CAPTURE_LEAD_SECONDS
		* RATE) * 4)
# Known bad: a feed written in blocks whose phase restarts at every block
# (what an offset-independent writer would produce) must show as impure.
with open(feeder.path, "r+b") as feed:
	block = feeder._one_second((440, 880, 8000))
	feed.seek(RATE * 4 * 5 + 1000 * 4)
	feed.write(block[:RATE * 2])
windows = feed_tones(feeder.path)
expect("capture feed check notices a phase jump",
	min(w[0][1] for w in windows) < 0.9, str([w[0] for w in windows]))


# The driver phases must still be found when the Media Kit tests play other
# tones on the same card afterwards.
checks, report = analyze(good + tone(1500, 2500, 2.0) + tone(1600, 2500, 1.0))
expect("foreign tones after the phases do not disturb them",
	all(checks.values()), str(checks))
checks, report = analyze(good[:int(2.0 * RATE)] + silence(0.05)
	+ good[int(2.05 * RATE):] + tone(1500, 2500, 2.0))
expect("a gap is still found with foreign tones present",
	not checks["audio.phase1"], str(checks))


# The Media Kit phases (MEDIA_PHASES) in synthetic recordings of both cards.
def media_tone(left, right, seconds, level=1.0, rate=RATE):
	count = int(seconds * rate)
	amplitude = 5800
	return [(int(level * amplitude * math.sin(2 * math.pi * left * i / rate))
			if left else 0,
		int(amplitude * math.sin(2 * math.pi * right * i / rate))
			if right else 0) for i in range(count)]


def fade(frames, start, end):
	"""Scales frames linearly from start to end (one buffer of fade)."""
	count = len(frames)
	return [(int(l * (start + (end - start) * i / count)),
		int(r * (start + (end - start) * i / count)))
		for i, (l, r) in enumerate(frames)]


def media_recordings(volume=None, click=False, drop_phase=None):
	"""Synthetic recordings of the Media Kit phases; volume (A's level in
	the volume phase), click (B cut off on the second card) and drop_phase
	(a predicate on audio.wav frames to leave out) make known-bad ones."""
	gain12 = ce_test.mixer_gain(-12.0412) if volume is None else volume
	gain6 = ce_test.mixer_gain(-6.0206)
	wav = silence(0.5) + media_tone(1500, 2500, 2.0) \
		+ media_tone(1600, 2500, 2.0, gain12) + media_tone(0, 2600, 1.5) \
		+ media_tone(1800, 0, 2.0) + media_tone(1800, 2750, 1.5) \
		+ media_tone(1900, 0, 2.0, gain6) + silence(0.5)
	b = media_tone(0, 2700, 2.0)
	sdl = silence(0.5) + fade(b[:1024], 0, 1) + b[1024:-1024] \
		+ fade(b[-1024:], 1, 0) + silence(0.5)
	b = media_tone(0, 2750, 1.5)
	# Known bad: the stream cut off without its fade-out when it moves
	# back to the default output.
	sdl += fade(b[:1024], 0, 1) + b[1024:-1024] \
		+ (b[-1024:-700] if click else fade(b[-1024:], 1, 0))
	sdl += silence(0.5) + media_tone(0, 2800, 1.5) + silence(0.3) \
		+ media_tone(0, 2850, 1.5) + media_tone(2000, 2900, 1.5)
	directory = tempfile.mkdtemp()
	write_wav(os.path.join(directory, "audio.wav"),
		[f for f in wav if drop_phase is None or not drop_phase(f)])
	with open(os.path.join(directory, "sdl-out.raw"), "wb") as raw:
		raw.write(b"".join(struct.pack("<hh", l, r) for l, r in sdl))
	report = {}
	ce_test.analyze_media_audio(directory, report)
	return {c["name"]: c for c in report["media_audio"]["checks"]}


checks = media_recordings()
expect("media phases: clean recordings pass",
	all(c["pass"] for c in checks.values()),
	str([c["detail"] for c in checks.values() if not c["pass"]]))
checks = media_recordings(volume=0.5)
expect("media phases: a wrong application volume fails",
	not checks["media_audio.app_volume"]["pass"],
	checks["media_audio.app_volume"]["detail"])
checks = media_recordings(click=True)
expect("media phases: a stream cut off on the second card fails",
	not checks["media_audio.route_click_free"]["pass"],
	checks["media_audio.route_click_free"]["detail"])
# Known bad: B never left the default output (its 2750 Hz phase is missing
# from audio.wav).
directory = tempfile.mkdtemp()
frames = silence(0.5) + media_tone(1500, 2500, 2.0) + silence(0.5)
write_wav(os.path.join(directory, "audio.wav"), frames)
with open(os.path.join(directory, "sdl-out.raw"), "wb") as raw:
	raw.write(b"\0" * 48000 * 4)
report = {}
ce_test.analyze_media_audio(directory, report)
missing = {c["name"]: c["pass"] for c in report["media_audio"]["checks"]}
expect("media phases: missing phases fail",
	missing["media_audio.both"] and not missing["media_audio.route_back"]
		and not missing["media_audio.route_second_card"]
		and not missing["media_audio.route_click_free"], str(missing))

print("selftest: %d failure(s)" % failures)
sys.exit(1 if failures else 0)
