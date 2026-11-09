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

print("selftest: %d failure(s)" % failures)
sys.exit(1 if failures else 0)
