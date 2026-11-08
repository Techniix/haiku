#!/usr/bin/env python3
#
# Copyright 2026, Haiku Community Edition contributors.
# Distributed under the terms of the MIT License.
#
"""Haiku Community Edition test harness.

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
WATCHED_DRIVERS = ("virtio_sound",)

# Compile-only targets: fast feedback before the image build.
COMPILE_TARGETS = ["kernel_x86_64", "libgnu.so", "virtio_sound",
	"vsnd_test", "eventfd_test", "timerfd_test"]

# Sources whose TRACE() output --verbose turns on (via -sCE_TRACE=1).
TRACE_SOURCES = [
	"src/add-ons/kernel/drivers/audio/virtio/virtio_sound.cpp",
]

# Serial console patterns
KDL_PATTERNS = ("Welcome to Kernel Debugging Land", "kdebug>")
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


def image_path(args):
	names = {
		"minimum-raw": "haiku-minimum.image",
		"nightly-raw": "haiku-nightly.image",
		"test-raw": "haiku-test.image",
	}
	return os.path.join(args.output, names.get(args.profile,
		"haiku-%s.image" % args.profile.replace("-raw", "")))


# #pragma mark - boot


class SerialReader(threading.Thread):
	"""Copies the guest's serial output to a file and watches it.

	When the kernel drops into its debugger it asks for a backtrace and
	syslog tail, then lets the main thread stop the VM.
	"""

	def __init__(self, process, log_path):
		threading.Thread.__init__(self, daemon=True)
		self.process = process
		self.log_path = log_path
		self.done = threading.Event()
		self.kdl = threading.Event()
		self.finished = threading.Event()
		self.last_output = time.time()

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
				buffer = (buffer + chunk)[-8192:]
				text = buffer.decode("latin-1")
				if "CE-HARNESS END" in text:
					self.finished.set()
				if not self.kdl.is_set() and any(p in text
						for p in KDL_PATTERNS):
					self.kdl.set()
					self._debug_commands()
		self.done.set()

	def _debug_commands(self):
		try:
			for command in (b"bt\n", b"threads\n"):
				time.sleep(1)
				self.process.stdin.write(command)
				self.process.stdin.flush()
		except OSError:
			pass


def boot(args, results_dir, report):
	image = image_path(args)
	if not os.path.exists(image):
		report["boot"] = {"status": "no-image", "image": image}
		return False

	serial_log = os.path.join(results_dir, "serial.log")
	audio = os.path.join(results_dir, "audio.wav")

	qemu = [
		"qemu-system-x86_64",
		"-m", str(args.memory),
		"-smp", "1",
		"-display", "none",
		"-vga", "std",
		"-serial", "stdio",
		"-monitor", "none",
		"-no-reboot",
		"-snapshot",
		"-drive", "file=%s,format=raw,if=ide" % image,
		"-audiodev", "wav,id=snd0,path=%s,out.frequency=48000,"
			"out.channels=2,out.format=s16" % audio,
		"-device", "virtio-sound-pci,audiodev=snd0",
		"-nic", "none",
		# Device models report malformed guest requests as guest errors.
		"-d", "guest_errors,unimp",
		"-D", os.path.join(results_dir, "qemu.log"),
	]
	if os.path.exists("/dev/kvm") and os.access("/dev/kvm", os.W_OK):
		qemu += ["-enable-kvm", "-cpu", "host"]
	if args.firmware == "uefi":
		qemu += ["-bios", args.ovmf]

	report["boot"] = {"command": " ".join(qemu), "image": image}
	log("booting: " + " ".join(qemu))

	started = time.time()
	qemu_stderr = open(os.path.join(results_dir, "qemu.stderr.log"), "wb")
	process = subprocess.Popen(qemu, stdin=subprocess.PIPE,
		stdout=subprocess.PIPE, stderr=qemu_stderr)
	reader = SerialReader(process, serial_log)
	reader.start()

	status = "exited"
	while process.poll() is None:
		time.sleep(1)
		elapsed = time.time() - started
		if reader.kdl.is_set():
			time.sleep(6)	# let the backtrace commands finish
			status = "kernel-debugger"
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
	report["boot"]["status"] = status
	report["boot"]["seconds"] = round(time.time() - started, 1)
	report["boot"]["runner_finished"] = reader.finished.is_set()
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

	lines = ["# CE test run %s" % report["started"], ""]
	verdict = report["verdict"]
	lines.append("**Verdict: %s** (%d checks, %d failed)" % (
		"PASS" if verdict["pass"] else "FAIL", verdict["checks"],
		verdict["failed"]))
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
	parser.add_argument("--profile", default="minimum-raw",
		help="build profile of the test image (default: minimum-raw)")
	parser.add_argument("--no-build", action="store_true",
		help="boot the existing image without building")
	parser.add_argument("--compile-only", action="store_true",
		help="only compile the test targets, do not build or boot an image")
	parser.add_argument("--firmware", choices=("bios", "uefi"),
		default="bios")
	parser.add_argument("--ovmf", default="/usr/share/OVMF/OVMF_CODE_4M.fd")
	parser.add_argument("--timeout", type=int, default=1800,
		help="seconds before the guest is considered hung")
	parser.add_argument("--memory", type=int, default=1024)
	parser.add_argument("--jobs", type=int, default=os.cpu_count() or 2)
	parser.add_argument("--verbose", action="store_true",
		help="build the drivers under test with their TRACE() output on")
	args = parser.parse_args()
	args.output = os.path.abspath(args.output)

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
		"compile_only": args.compile_only}

	ok = True
	if not args.no_build:
		ok = build(args, results_dir, report)
	if ok and not args.compile_only:
		boot(args, results_dir, report)
		parse_serial(results_dir, report)
		parse_qemu_log(results_dir, report)
		analyze_audio(results_dir, report, VIRTIO_SOUND_PHASES)

	summary, passed = write_report(results_dir, report)
	print(summary)
	log("results in " + results_dir)
	return 0 if passed else 1


if __name__ == "__main__":
	sys.exit(main())
