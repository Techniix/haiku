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
    audio.wav       what the guest played on the first virtio sound card
    sdl-out.raw     what it played on the second one (s16le, 2 ch, 48 kHz)

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

# The second virtio sound card runs on QEMU's SDL audio backend with SDL's
# "disk" driver: its capture input reads from a FIFO the harness writes
# (AudioFeeder, capture-in.raw), its output goes to sdl-out.raw. Both at 48 kHz, 16-bit
# stereo, in 10 ms periods (SDL's disk driver sleeps one period per read or
# write, in whole milliseconds, so 480 frames keep real time exactly).
CAPTURE_RATE = 48000
CAPTURE_PERIOD_FRAMES = 480
# The tone fed until the guest asks for another one (CE-HOST audio_in), and
# how far the feed file is kept ahead of the reader.
CAPTURE_DEFAULT_TONE = (700, 1300, 12000)
CAPTURE_LEAD_SECONDS = 30

# Phases of 3rdparty/ce/tests/media/media_test.cpp, as the cards play them.
# Application A plays on the left channel only, B on the right only, with a
# tone of its own in every phase, so that a phase is found by its tones.
# Entries: (check, recording, left Hz, right Hz, minimum seconds, left level
# relative to the "both" phase or None). None as a frequency: silent.
def mixer_gain(db):
	"""The gain the system mixer applies for a gain parameter value: its
	"dB" bend the scale (exponent 1.8 below 0, 1.4 above, over -60..+18),
	as upstream Haiku's mixer always did. -12 is about -3.3 real dB."""
	maximum, exponent = (18.0, 1.4) if db > 0 else (60.0, 1.8)
	scaled = (abs(db) * maximum ** (1 / exponent) / maximum) ** exponent
	return 10 ** ((scaled if db > 0 else -scaled) / 20)


MEDIA_REFERENCE = ("audio.wav", 1500, 2500)
MEDIA_PHASES = [
	("media_audio.both", "audio.wav", 1500, 2500, 1.0, None),
	("media_audio.app_volume", "audio.wav", 1600, 2500, 1.0,
		mixer_gain(-12.0412)),
	("media_audio.app_mute", "audio.wav", None, 2600, 0.8, None),
	("media_audio.route_default_side", "audio.wav", 1800, None, 1.0, 1.0),
	("media_audio.route_second_card", "sdl-out.raw", None, 2700, 1.0, None),
	("media_audio.route_back", "audio.wav", 1800, 2750, 0.8, None),
	("media_audio.remembered_route", "sdl-out.raw", None, 2800, 1.0, None),
	("media_audio.remembered_volume", "audio.wav", 1900, None, 1.0,
		mixer_gain(-6.0206)),
	("media_audio.route_after_restart", "sdl-out.raw", None, 2850, 1.0, None),
	("media_audio.default_output_switch", "sdl-out.raw", 2000, 2900, 1.0,
		None),
]
MEDIA_FREQUENCIES = sorted({hz for phase in MEDIA_PHASES
	for hz in phase[2:4] if hz} | {1550, 1700})

# Phases of 3rdparty/ce/tests/media/media_sound_test.cpp: the Audio
# preferences' controls, used as a click uses them (F-32). Entries: (check,
# recording, left Hz, right Hz, minimum seconds, level of the left channel
# relative to the first phase or None, present). "Music" plays on the left,
# "Browser" on the right; the master volume at -12 on its slider (the
# mixer's bent scale), muted (absent), the default output moved to the
# second card with the Output page's menu and back, Browser routed to the
# second card with its menu on the Applications page.
SOUND_PHASES = [
	("sound_audio.master_reference", "audio.wav", 4100, 4150, 1.0, None,
		True),
	("sound_audio.master_volume", "audio.wav", 4200, 4250, 1.0,
		mixer_gain(-12), True),
	("sound_audio.master_mute", "audio.wav", 4300, 4350, 0, None, False),
	("sound_audio.default_output_second", "sdl-out.raw", 4400, 4450, 1.0,
		None, True),
	("sound_audio.default_output_back", "audio.wav", 4500, 4550, 1.0, None,
		True),
	("sound_audio.app_output_default_side", "audio.wav", 4600, None, 1.0,
		None, True),
	("sound_audio.app_output_routed", "sdl-out.raw", None, 4650, 1.0, None,
		True),
]
SOUND_FREQUENCIES = sorted({hz for phase in SOUND_PHASES
	for hz in phase[2:4] if hz} | {4700})
SOUND_SCREENSHOTS = ["sound_output", "sound_input", "sound_applications",
	"sound_devices", "sound_output_second", "sound_output_advanced",
	"sound_applications_volume", "sound_applications_advanced",
	"sound_devices_advanced", "sound_devices_usb", "sound_search"]


def qemu_usb_audio_gain(db):
	"""The factor QEMU's usb-audio device plays at for a feature unit volume
	of db (hw/usb/dev-audio.c): it maps the 1/256 dB value from 0x8000
	(-128 dB) to 0x0800 (+8 dB) linearly onto 0..255 and the mixing engine
	scales by that / 255, so 0 dB is 240/255."""
	raw = int(round(db * 256)) & 0xffff
	step = ((raw - 0x8000) & 0xffff) * 255 + 0x4400
	return min(step // 0x8800, 255) / 255.0


# Phases of 3rdparty/ce/tests/usb_audio/usb_audio_test.cpp in what QEMU's
# usb-audio devices recorded: (check, file, left Hz, right Hz, minimum
# seconds, level relative to the "play" phase or None, present). Phases
# that must be absent (muted) have present False.
USB_AUDIO_PHASES = [
	("usb_audio_out.play", "usb-0.wav", 600, 900, 1.6, None, True),
	("usb_audio_out.volume", "usb-0.wav", 650, 950, 1.2,
		qemu_usb_audio_gain(-40) / qemu_usb_audio_gain(0), True),
	("usb_audio_out.mute", "usb-0.wav", 700, 1000, 0, None, False),
	("usb_audio_out.restart", "usb-0.wav", 750, 1050, 1.2, 1.0, True),
	("usb_audio_out.unplug", "usb-0.wav", 800, 1100, 1.0, 1.0, True),
	("usb_audio_out.replug", "usb-1.wav", 850, 1150, 1.2, 1.0, True),
]
USB_AUDIO_FREQUENCIES = sorted({hz for phase in USB_AUDIO_PHASES
	for hz in phase[2:4]})

# Phases of 3rdparty/ce/tests/media/media_usb_test.cpp: the usb-audio device
# plugged in with the media services running. It runs last and plugs the
# device in twice, so its recordings are the last two usb-<n>.wav files
# ("first plug-in", "second plug-in"; usb-2.wav and usb-3.wav after
# usb_audio_test's two). A plays on the left channel only, B on the right.
# Entries: (check, recording, left Hz, right Hz, minimum seconds); None:
# silent.
MEDIA_USB_FIRST = "first plug-in"
MEDIA_USB_SECOND = "second plug-in"
MEDIA_USB_PHASES = [
	("media_usb_audio.routed_to_usb", MEDIA_USB_FIRST, 1110, None, 1.5),
	("media_usb_audio.routed_default_side", "audio.wav", None, 3110, 1.5),
	("media_usb_audio.default_on_usb", MEDIA_USB_FIRST, 1230, 3170, 2.0),
	("media_usb_audio.unplugged_fallback", "audio.wav", 1350, 3290, 2.0),
	("media_usb_audio.back_on_usb", MEDIA_USB_SECOND, 1410, 3350, 2.0),
]
MEDIA_USB_FREQUENCIES = sorted({hz for phase in MEDIA_USB_PHASES
	for hz in phase[2:4] if hz} | {1170, 1290, 3230})
MEDIA_USB_SCREENSHOTS = ["media_usb_device", "media_usb_unplugged"]
CODYCAM_SCREENSHOTS = ["codycam_preview", "codycam_switched"]
# Screen capture (F-31 phase 3) and recording in Screenshot (F-44):
# the permission prompt, the indicator, the Screen recording preferences
# and Screenshot while it records.
CAPTURE_SCREENSHOTS = ["capture_prompt", "capture_indicator",
	"capture_settings", "screenshot_recording",
	"recording_indicator_settings"]
# A new audio device (F-59): the prompt, and the Devices page's settings.
DEVNOTIFY_SCREENSHOTS = ["audio_device_prompt", "audio_device_policy"]
# Tracker's folder menus built ahead of time (F-59): its setting.
TRACKER_MENU_SCREENSHOTS = ["tracker_prefetch_setting"]
# The low-latency mode (F-31): the audio settings with a game's "Low
# latency" switch on, and the default output's page with "Buffer size".
LATENCY_SCREENSHOTS = ["media_low_latency", "media_buffer_size"]
# sound_latency_test's tone in low-latency mode (left, right Hz), at least
# this long on the default output.
LATENCY_TONE = (3100, 3300)
LATENCY_TONE_SECONDS = 5.0
# Runs of zero samples longer than this inside it are gaps (a period the
# card did not get in time shows as hundreds); see analyze_latency_audio().
LATENCY_MAX_ZERO_RUN = 16
LATENCY_MAX_GAP = 4 * 256
# MediaPlayer playing an HLS stream from media_stream_test's server (F-31).
STREAM_SCREENSHOTS = ["mediaplayer_hls"]
# WebPositive playing media_html5_test's WebM and MP4 clips (F-57).
HTML5_SCREENSHOTS = ["html5_webm", "html5_mp4"]
# The Boot menu preferences (F-22) with the menu on and a default set.
BOOTMENU_SCREENSHOTS = ["bootmenu_preferences"]
LINUXKPI_SCREENSHOTS = ["linuxkpi_overview", "linuxkpi_drivers",
	"linuxkpi_hardware", "linuxkpi_compatibility",
	"linuxkpi_settings_mt7921e"]
# The Wayland compositor showing wayland_test's wl_shm window (F-43 C).
WAYLAND_SCREENSHOTS = ["wayland_shm_window", "wayland_switch"]
# ... and the Linux client's window under Renguin (F-27), x86_64 harness
# images with the Linux root.
WAYLAND_LINUX_SCREENSHOTS = ["wayland_linux_window"]
# The Installer with a destination that has an installation (F-23).
INSTALLER_SCREENSHOTS = ["installer_modes"]

# Drivers whose error output (their ERROR() macro prints the name in yellow,
# "\33[33m<name>:") fails the run. A healthy test run prints none.
WATCHED_DRIVERS = ("virtio_sound", "virtio_block", "nvme_disk", "usb_audio")

# Compile-only targets: fast feedback before the image build.
COMPILE_TARGETS = ["kernel_x86_64", "libroot.so", "libgnu.so",
	"libnetwork.so", "libbsd.so", "libc_ext_test", "virtio_sound",
	"vsnd_test", "affinity_test", "epoll_test", "eventfd_test", "getdents_test", "inotify_test", "memfd_test", "mqueue_test", "quic_udp_test", "shm_test", "signalfd_test", "socket_ext_test", "thread_atexit_test", "timerfd_test",
	"ucontext_test", "user_locks_test", "vm_semantics_test", "tcp_keepalive_test", "tcp_close_test", "tcp_info_test", "tcp",
	"virtio_block", "vblk_test", "nvme_disk", "nvme_test",
	"package_priority_test", "libpackage.so", "shortcut_keys_test",
	"input_server", "Terminal", "Keymap", "pointer_test", "usb_hid",
	"pointer_logic_test", "pointer_rate_test", "ce_vmouse", "<input>mouse",
	"Input", "background_agents_test", "gamepad_agent",
	"crash_report_notifier", "audio_device_agent", "Gamepads",
	"renguin", "renguin_test", "kernel_x86_64",
	"wayland_compositor", "wayland_test",
	"virtio_input", "<input>tablet", "<input>virtio",
	"usb_audio", "usb", "<usb>xhci", "usb_audio_test",
	"lkpi_pcitest", "linuxkpi_test", "fbsd_wlantest", "wlan_reload_test",
	"driver_override_test", "mt7921e", "lkpictl",
	"LinuxKPIManager", "linuxkpi_ui_test", "hda", "hda_eld_test",
	"usb_webcam.media_addon", "uvc_test", "CodyCam",
	"video_producer_demo.media_addon", "media_video_test",
	"mixer.media_addon", "hmulti_audio.media_addon", "media_server",
	"media_test", "media_ui_test", "media_usb_test", "Media", "desklink",
	"media_sound_test", "Audio", "media_hwvideo_test",
	"media_resample_test", "media_drift_test", "media_html5_test",
	"media_routing_test", "media_vcam_test", "virtual_camera.media_addon",
	"seqpacket_test", "app_server", "libbe.so", "appserver_fd_test",
	"software_video_accelerator",
	"sound_latency_test", "libmedia.so", "media_codec_test", "ffmpeg",
	"media_stream_test", "libbnetapi.so", "http_streamer", "Screenshot",
	"SoundRecorder", "media_addon_server", "crash_collector",
	"crashlog_test", "SystemLogs", "debug_server"]

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

# USB audio devices the guest plugs in and out (CE-HOST usb_audio add |
# remove): QEMU's usb-audio (USB Audio Class 1, 48 kHz 16-bit stereo, a
# feature unit with mute and per-channel volume), on an xhci controller of
# their own so that they are there with any --pointer-devices. Every add
# gets the next audiodev, a wav recorder writing usb-<n>.wav, because QEMU
# closes the file when the device goes and would start it again on the
# next add. USB_AUDIO_DEVICES bounds the adds per run: usb_audio_test plugs
# in twice, media_routing_test (a route by name kept while the device is
# absent, F-58) once, media_sound_test (the Audio preferences' device list)
# once, media_usb_test twice, media_devnotify_test (a new device, F-59)
# four times.
USB_AUDIO_DEVICES = 10
USB_AUDIO_CONTROLLER = "audiousb"

# Sources whose TRACE() output --verbose turns on (via -sCE_TRACE=1).
TRACE_SOURCES = [
	"src/add-ons/kernel/drivers/audio/virtio/virtio_sound.cpp",
]

# Serial console patterns
KDL_PATTERNS = ("Welcome to Kernel Debugging Land", "kdebug>")

# Kernel debugger entries the crashlog suite makes on purpose (F-51): it
# prints this line, then "CE-HOST kdl_reboot", then enters the debugger; the
# harness answers with "reboot" and the guest boots again in the same QEMU
# session. Between the line and the next "CE-HARNESS START", a debugger
# entry with one of these messages is not a kernel problem; any other one
# still is.
EXPECTED_KDL_MARKER = \
	"CE-INFO crashlog: entering the kernel debugger on purpose"
EXPECTED_KDL_MESSAGES = ("PANIC: requested from kdl driver.",
	"USER: CE crash test")
# Seconds to wait for the debugger's prompt before sending "reboot", and to
# ignore its output afterwards (until the machine has reset).
EXPECTED_KDL_PROMPT_TIMEOUT = 30
EXPECTED_KDL_QUIET_SECONDS = 10
# --stall for the boots after such a reboot (see boot()).
EXPECTED_KDL_STALL = 240
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

	if args.installer_upgrade:
		# room for an upgrade that keeps the old system as a state
		jam.append("-sHAIKU_IMAGE_SIZE=%d" % INSTALLER_IMAGE_SIZE)
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
	if args.other_systems and args.disk_bus == "ide":
		# First in the boot order: the other systems' disks have boot
		# indices too (other_systems_drives()).
		return ["-drive", "file=%s,format=raw,if=none,id=boot" % image,
			"-device", "ide-hd,drive=boot,bus=ide.0,unit=0,bootindex=0"]
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


def installed_tests(profile=None, final=True):
	"""Names of the test programs harness images carry, in the order the
	guest runner runs them: first /boot/system/non-packaged/bin/ce-tests/*,
	then, with media_server started, ce-tests-media/*, then ce-tests-final/*
	(each sorted, like the shell's glob): every name listed after
	"bin ce-tests :", "bin ce-tests-media :" or "bin ce-tests-final :" in an
	AddFilesToHaikuImage rule of the
	3rdparty/ce/tests Jamfiles. The media tests only run on images with the
	Media Kit (the renga-* profiles): for other profiles they are left out.
	"""
	names = {"ce-tests": set(), "ce-tests-media": set(),
		"ce-tests-final": set()}
	for jamfile in glob.glob(os.path.join(HAIKU_TOP, "3rdparty", "ce",
			"tests", "*", "Jamfile")):
		with open(jamfile) as jamFile:
			text = jamFile.read()
		for match in re.finditer(
				r"\bbin\s+(ce-tests(?:-media|-final)?)\s*:\s*([^;]*);", text):
			names[match.group(1)].update(match.group(2).split())
	media = sorted(names["ce-tests-media"]) \
		if profile is None or profile.startswith("renga-") else []
	# ce-tests-final runs last, on every image (it ends the boot).
	return sorted(names["ce-tests"]) + media \
		+ (sorted(names["ce-tests-final"]) if final else [])


def selected_tests(text, available):
	"""Parses --only: comma-separated test program names, with or without
	their "_test" ending ("epoll,vsnd" or "epoll_test,vsnd_test")."""
	selection = []
	for word in text.split(","):
		word = word.strip()
		if not word:
			continue
		name = word if word in available else word + "_test"
		if name not in available:
			raise ValueError("unknown test %s (choose from %s)" % (word,
				", ".join(t.replace("_test", "") for t in available)))
		if name not in selection:
			selection.append(name)
	if not selection:
		raise ValueError("--only needs at least one test")
	return selection


SELECTION_FILE = "/myfs/system/non-packaged/data/ce-harness/only"
KERNEL_SETTINGS_FILE = "/myfs/home/config/settings/kernel/drivers/kernel"
# With it, the runner skips the tests that end a boot (ce-tests-final):
# runs that count boot loader starts or boot other disks themselves
# (--other-systems, --installer-upgrade) must not get the crashlog reboots.
NO_FINAL_TESTS_FILE = "/myfs/system/non-packaged/data/ce-harness/no-final-tests"


def make_patched_image(args, image, copy):
	"""Copies the test image and changes files in the copy through the
	build's bfs_shell, in one session:

	- --boot-menu-timeout: "boot_menu_timeout <seconds>" in the kernel
	  settings file. The file written is the image's own (the tree's
	  data/settings/kernel/drivers/kernel, which the build copies) plus that
	  line.
	- --only: the runner's selection file (data/ce-harness/only), one test
	  program name per line; runner.sh then runs only those.

	Everything else in the image stays as built. Returns an error text, or
	None on success."""
	offset = bfs_volume_offset(image)
	if offset is None:
		return "no BFS volume in " + image
	shells = glob.glob(os.path.join(args.output, "objects", "*", "*", "*",
		"tools", "bfs_shell", "bfs_shell"))
	if not shells:
		return "bfs_shell not built (jam -q '<build>bfs_shell')"

	files = []	# (host path, image path, text the copy must then contain)
	timeout = args.boot_menu_timeout or (OTHER_SYSTEMS_TIMEOUT
		if args.other_systems else 0)
	if timeout:
		with open(os.path.join(HAIKU_TOP, "data", "settings", "kernel",
				"drivers", "kernel")) as original:
			settings = original.read()
		settings = settings.rstrip("\n") + "\n\nboot_menu_timeout %d\n" \
			% timeout
		if args.other_systems and args.firmware == "uefi":
			# what the Boot menu preferences write for a default entry
			settings += "boot_menu_default \"%s\"\n" \
				% OTHER_SYSTEMS_EFI_DEFAULT
		files.append((copy + ".kernel-settings", KERNEL_SETTINGS_FILE,
			settings, "boot_menu_timeout %d" % timeout))
	if args.other_systems or args.installer_upgrade:
		files.append((copy + ".no-final", NO_FINAL_TESTS_FILE,
			"skip the tests that end the boot\n",
			"skip the tests that end the boot"))
	if args.only:
		files.append((copy + ".only", SELECTION_FILE,
			"\n".join(args.only) + "\n", "\n".join(args.only)))

	commands = ""
	for host, target, text, expected in files:
		with open(host, "w") as hostFile:
			hostFile.write(text)
		commands += "cp -f :%s %s\ncat %s\n" % (host, target, target)

	shutil.copyfile(image, copy)
	result = subprocess.run([shells[0], "--start-offset", str(offset), copy],
		input=(commands + "sync\nquit\n").encode(), stdout=subprocess.PIPE,
		stderr=subprocess.STDOUT, timeout=120)
	output = result.stdout.decode("utf-8", "replace")
	for host, target, text, expected in files:
		os.remove(host)
		if result.returncode != 0 or expected not in output:
			return "bfs_shell could not write %s: %s" % (target,
				output[-300:])
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
				if line.startswith("[ce-test] --- boot "):
					# further boots of an --installer-upgrade run follow;
					# they have menus of their own
					break
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


# #pragma mark - other operating systems (--other-systems)


OTHER_SYSTEMS_SOURCES = os.path.join(HAIKU_TOP, "3rdparty", "ce", "harness",
	"otheros")
OTHER_SYSTEMS_TIMEOUT = 3
# The UEFI run's boot_menu_default: the countdown starts this entry.
OTHER_SYSTEMS_EFI_DEFAULT = "Windows Boot Manager"
# Layout of the "other systems" disk (512-byte sectors): an EFI system
# partition, an active NTFS-like partition with a Windows-like boot record,
# and an inactive one with the same record (a data volume, which must not
# be listed). The MBR has boot code of its own.
OTHER_DISK_SECTORS = 96 * 1024 * 2
OTHER_ESP = (2048, 65536)			# start, sectors
OTHER_WINDOWS = (2048 + 65536, 2048)
OTHER_DATA = (2048 + 65536 + 2048, 2048)
OTHER_USB_SECTORS = 24 * 1024 * 2
# QEMU throttling of the removable media, so that reading them is slow the
# way a real drive is: reads per second.
OTHER_USB_IOPS = 20
OTHER_FLOPPY_IOPS = 5
# A slow, empty drive (QEMU's null-co driver, zeros after a delay per
# request): floppy B: on BIOS, a second USB drive on UEFI.
OTHER_SLOW_LATENCY_NS = 400 * 1000 * 1000
# Limits of other_media.responsive: how long a key may wait for the menu,
# and how long one step of the background search may take.
OTHER_KEY_LATENCY = 1.5
OTHER_PROBE_STEP_MS = 1000


def nasm(source, output, defines):
	subprocess.run(["nasm", "-f", "bin"] + defines + ["-o", output,
		os.path.join(OTHER_SYSTEMS_SOURCES, source)], check=True,
		stdout=subprocess.PIPE, stderr=subprocess.STDOUT)


def efi_test_app(directory, marker):
	path = os.path.join(directory, "app-%s.efi" % marker)
	nasm("efi_test_app.asm", path, ["-DMARKER=\"%s\"" % marker])
	return path


def boot_record(directory, marker, kind=None):
	path = os.path.join(directory, "record-%s.bin" % marker)
	nasm("boot_record.asm", path, (["-D" + kind] if kind else [])
		+ ["-DMARKER=\"%s\"" % marker])
	with open(path, "rb") as recordFile:
		return recordFile.read()


def make_fat(path, sectors, files, label):
	"""Writes a FAT file system of \\a sectors sectors to \\a path with
	mtools; files: (path in the volume, host file). The boot sector code
	mformat writes is replaced by a plain "not bootable" record, as
	partitioning tools leave it, so that BIOS runs do not list it."""
	with open(path, "wb") as image:
		image.truncate(sectors * 512)
	environment = dict(os.environ, MTOOLS_SKIP_CHECK="1")
	subprocess.run(["mformat", "-i", path, "-T", str(sectors), "-h", "64",
		"-s", "32", "-v", label, "::"], check=True, env=environment,
		stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
	made = set()
	for target, host in files:
		parts = target.strip("/").split("/")
		for i in range(1, len(parts)):
			directory = "/".join(parts[:i])
			if directory not in made:
				made.add(directory)
				subprocess.run(["mmd", "-i", path, "::/" + directory],
					check=True, env=environment, stdout=subprocess.PIPE,
					stderr=subprocess.STDOUT)
		subprocess.run(["mcopy", "-i", path, host, "::/" + target.strip("/")],
			check=True, env=environment, stdout=subprocess.PIPE,
			stderr=subprocess.STDOUT)
	with open(path, "r+b") as image:
		sector = bytearray(image.read(512))
		# keep the jump and the BPB (to 0x5a), clear the code, and say so
		message = b"not a bootable disk"
		sector[0x5a:510] = b"\0" * (510 - 0x5a)
		sector[0x5a:0x5a + len(message)] = message
		image.seek(0)
		image.write(bytes(sector))


def mbr(code, partitions):
	"""An MBR: \\a code (up to 440 bytes) and up to four (status, type,
	start, sectors) entries."""
	sector = bytearray(512)
	sector[:len(code)] = code
	for index, (status, kind, start, count) in enumerate(partitions):
		entry = struct.pack("<B3sB3sII", status, b"\xfe\xff\xff", kind,
			b"\xfe\xff\xff", start, count)
		sector[446 + 16 * index:462 + 16 * index] = entry
	sector[510:512] = b"\x55\xaa"
	return bytes(sector)


def make_other_systems_media(results_dir):
	"""Builds the media of --other-systems in results_dir: other.img (a
	fixed disk with other systems, BIOS and UEFI), usb.img (a USB drive with
	a removable medium's EFI loader) and floppy.img (a bootable floppy).
	Returns their paths, or raises on a missing tool."""
	work = os.path.join(results_dir, "otheros")
	os.makedirs(work, exist_ok=True)

	esp = os.path.join(work, "esp.img")
	make_fat(esp, OTHER_ESP[1], [
		("EFI/Microsoft/Boot/bootmgfw.efi", efi_test_app(work, "windows-efi")),
		("EFI/Microsoft/Boot/bootmgr.efi", efi_test_app(work, "windows-bootmgr")),
		("EFI/ubuntu/shimx64.efi", efi_test_app(work, "ubuntu-shim")),
		("EFI/ubuntu/grubx64.efi", efi_test_app(work, "ubuntu-grub")),
		("EFI/ubuntu/mmx64.efi", efi_test_app(work, "ubuntu-mok")),
		("EFI/BOOT/BOOTX64.EFI", efi_test_app(work, "esp-fallback")),
	], "CEOTHER")

	other = os.path.join(results_dir, "other.img")
	windows = boot_record(work, "bios-windows", "KIND_NTFS")
	with open(other, "wb") as disk:
		disk.truncate(OTHER_DISK_SECTORS * 512)
		disk.write(mbr(boot_record(work, "bios-mbr", "KIND_MBR"), [
			(0x00, 0xef) + OTHER_ESP,
			(0x80, 0x07) + OTHER_WINDOWS,
			(0x00, 0x07) + OTHER_DATA]))
		with open(esp, "rb") as espFile:
			disk.seek(OTHER_ESP[0] * 512)
			shutil.copyfileobj(espFile, disk)
		for start, _ in (OTHER_WINDOWS, OTHER_DATA):
			disk.seek(start * 512)
			disk.write(windows)

	usbFat = os.path.join(work, "usb-fat.img")
	make_fat(usbFat, OTHER_USB_SECTORS - 2048, [
		("EFI/BOOT/BOOTX64.EFI", efi_test_app(work, "removable-usb"))],
		"CEUSB")
	usb = os.path.join(results_dir, "usb.img")
	with open(usb, "wb") as disk:
		disk.truncate(OTHER_USB_SECTORS * 512)
		disk.write(mbr(b"", [(0x00, 0x0e, 2048, OTHER_USB_SECTORS - 2048)]))
		with open(usbFat, "rb") as fatFile:
			disk.seek(2048 * 512)
			shutil.copyfileobj(fatFile, disk)

	floppy = os.path.join(results_dir, "floppy.img")
	with open(floppy, "wb") as disk:
		disk.truncate(1474560)
		disk.write(boot_record(work, "bios-floppy"))

	shutil.rmtree(work)
	return {"other": other, "usb": usb, "floppy": floppy}


def other_systems_drives(args, media):
	"""QEMU options for --other-systems: the fixed disk with other systems
	(BIOS drive 0x81), the USB drive (slow), an empty CD drive and, on BIOS,
	the floppy (slow). Boot indices keep the BIOS drive numbers and the
	firmware's boot order fixed: Renga's disk first."""
	options = [
		"-drive", "file=%s,format=raw,if=none,id=otheros" % media["other"],
		"-device", "ide-hd,drive=otheros,bus=ide.1,unit=0,bootindex=1",
		"-device", "qemu-xhci,id=otherusb",
		"-drive", "file=%s,format=raw,if=none,id=otherusbdisk,"
			"throttling.iops-total=%d" % (media["usb"], OTHER_USB_IOPS),
		"-device", "usb-storage,bus=otherusb.0,drive=otherusbdisk,"
			"removable=on,bootindex=2",
		"-drive", "if=none,id=emptycd,media=cdrom",
		"-device", "ide-cd,drive=emptycd,bus=ide.1,unit=1",
	]
	if args.firmware == "bios":
		options += ["-drive", "file=%s,format=raw,if=floppy,index=0,"
			"throttling.iops-total=%d" % (media["floppy"], OTHER_FLOPPY_IOPS),
			# B: is a drive without a disk
			"-drive", "if=floppy,index=1"]
	else:
		options += ["-blockdev", "driver=null-co,node-name=slowusb,"
			"size=16777216,latency-ns=%d,read-zeroes=on"
			% OTHER_SLOW_LATENCY_NS,
			"-device", "usb-storage,bus=otherusb.0,drive=slowusb,"
			"removable=on"]
	return options


# Keys as QEMU's sendkey names them, and as the boot menu logs them.
MENU_KEY_LOG = {"down": "0x40000001", "up": "0x40000000",
	"home": "0x40000006", "ret": "0xd", "esc": "0x1b"}


def menu_key_logged(key):
	return MENU_KEY_LOG.get(key, "'%s'" % key)


class BootMenuDriver(threading.Thread):
	"""Plays the user at the boot loader's menu for --other-systems: it
	reads the menu's serial log ("boot menu: ..." lines, fed by the
	SerialReader as they arrive) and presses keys through the QEMU monitor,
	in a script per firmware:

	UEFI (boot_menu_default starts Windows Boot Manager after the
	countdown): wait until it returned, press down and up three times in
	quick succession (while drivers are still being connected), press the
	shortcut of "Ubuntu",
	wait until it returned and the USB drive was found, press its shortcut,
	wait until it returned, press down, down, up (timed), then "b"
	(Continue booting).

	BIOS (no default): at the first countdown press the shortcut of
	"Windows"; the test boot record boots Renga's disk again, so at the
	second countdown press "Hard disk 2"; at the third press "down" (which
	stops the countdown), "up" while the slow, empty floppy B: is read,
	"down", and the shortcut of the floppy in A:; at the fourth do nothing,
	and Renga boots.

	Every key is recorded with the host time it was sent and the time the
	menu logged it ("boot menu: key ..."), for other_media.responsive. A
	step that waits more than 40 s for its line gives up and presses "b".
	"""

	def __init__(self, firmware, monitor_path):
		threading.Thread.__init__(self, daemon=True)
		self.firmware = firmware
		self.monitor_path = monitor_path
		self.started = time.time()
		self.lines = []
		self.condition = threading.Condition()
		self.keys = []		# {"key", "sent", "logged"}
		self.notes = []
		self.done = False

	def feed(self, line):
		if "boot menu:" not in line and "Welcome to the" not in line:
			return
		with self.condition:
			self.lines.append((time.time() - self.started, line.strip()))
			self.condition.notify_all()

	def _wait_for(self, pattern, since, timeout=40):
		"""Waits for a line matching \\a pattern from index \\a since on;
		returns (index, match) or (None, None)."""
		regex = re.compile(pattern)
		deadline = time.time() + timeout
		with self.condition:
			while True:
				for index in range(since, len(self.lines)):
					match = regex.search(self.lines[index][1])
					if match:
						return index, match
				left = deadline - time.time()
				if left <= 0:
					self.notes.append("gave up waiting for /%s/" % pattern)
					return None, None
				self.condition.wait(timeout=min(left, 1.0))

	def _shortcut(self, label, since):
		index, match = self._wait_for(r'boot menu: (?:other system|found '
			r'other media) (\d): "%s"' % re.escape(label), since)
		return match.group(1) if match else None

	def _press(self, key):
		import socket
		try:
			with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as sock:
				sock.settimeout(5)
				sock.connect(self.monitor_path)
				time.sleep(0.2)
				sock.recv(4096)
				with self.condition:
					sent = time.time() - self.started
					since = len(self.lines)
				sock.sendall(("sendkey %s\n" % key).encode())
				time.sleep(0.2)
		except OSError as error:
			self.notes.append("sendkey %s failed: %s" % (key, error))
			return
		entry = {"key": key, "sent": round(sent, 3), "logged": None}
		self.keys.append(entry)
		index, _ = self._wait_for(r"boot menu: key %s" % re.escape(
			menu_key_logged(key)), since, timeout=10)
		if index is not None:
			entry["logged"] = round(self.lines[index][0], 3)

	def _boots(self, since):
		return self._wait_for(r'boot menu: "[^"]*" in %d s'
			% OTHER_SYSTEMS_TIMEOUT, since)[0]

	def run(self):
		try:
			if self.firmware == "uefi":
				self._run_uefi()
			elif self.firmware == "old_state":
				self._run_old_state()
			else:
				self._run_bios()
		finally:
			self.done = True

	def _run_old_state(self):
		"""--installer-upgrade, phase C: choose the newest older package
		state of the boot volume ("Select boot volume/state", the volume,
		one down from "Latest state"), back to the main menu, continue."""
		if self._wait_for(r'boot menu: "Continue booting" in \d+ s', 0,
				timeout=120)[0] is None:
			return
		for key in ("home", "ret", "ret", "down", "ret", "esc", "b"):
			self._press(key)
			time.sleep(0.3)

	def _run_uefi(self):
		index, _ = self._wait_for(r'boot menu: "%s" returned'
			% re.escape(OTHER_SYSTEMS_EFI_DEFAULT), 0, timeout=120)
		if index is None:
			return self._press("b")
		# The drivers are still being connected (slowly: the slow USB
		# drive); keys in quick succession, some arrive during a step.
		for key in ("down", "up") * 3:
			self._press(key)
			time.sleep(0.15)
		key = self._shortcut("Ubuntu", 0)
		if key is not None:
			self._press(key)
			self._wait_for(r'boot menu: "Ubuntu" returned', index)
		key = self._shortcut("Boot other media: USB drive", 0)
		if key is not None:
			since = len(self.lines)
			self._press(key)
			self._wait_for(r'boot menu: "USB drive" returned', since)
		for key in ("down", "down", "up"):
			self._press(key)
			time.sleep(0.4)
		self._press("b")

	def _run_bios(self):
		since = 0
		for step in ("Windows", "Hard disk 2", "floppy", None):
			index = self._boots(since)
			if index is None:
				return
			# the entries of this boot are logged before its countdown
			start = max(i for i in range(index + 1) if "Welcome to the"
				in self.lines[i][1]) if any("Welcome to the" in l[1]
					for l in self.lines[:index + 1]) else 0
			if step is None:
				return
			if step == "floppy":
				# "down" stops the countdown. A: is read first; the next
				# step reads the slow, empty B:, so "up" right after A's
				# line arrives while that read runs (other_media.responsive
				# times it with the host clock: the BIOS tick count the
				# boot loader has does not advance during BIOS disk calls).
				self._press("down")
				self._wait_for(r"boot menu: floppy disk in A:", start)
				self._press("up")
				self._wait_for(r"boot menu: floppy (disk in|drive) B:", start)
				self._press("down")
				key = self._shortcut("Boot other media: Floppy disk A:",
					start)
				self._press(key if key is not None else "b")
			else:
				key = self._shortcut(step, start)
				self._press(key if key is not None else "b")
			since = index + 1

	def summary(self):
		return {"keys": self.keys, "notes": self.notes,
			"lines": len(self.lines)}


def other_systems_entries(text):
	"""The menu entries the boot loader logged: (kind, shortcut, label,
	location), in order, each once (a BIOS run starts the boot loader four
	times; a system listed twice in one menu would have a numbered label,
	"Windows (2)")."""
	entries = []
	for entry in re.findall(r'boot menu: (other system|found other media) '
			r'(\S): "([^"]*)" \((.*)\)\r?$', text, re.MULTILINE):
		if entry not in entries:
			entries.append(entry)
	return entries


def other_systems_checks(firmware, text, menu, probe_limit_ms=None):
	"""The checks of an --other-systems run, from the serial log \\a text and
	the BootMenuDriver's summary \\a menu. Returns check dictionaries."""
	checks = []

	def check(name, passed, detail):
		checks.append({"name": name, "pass": bool(passed), "detail": detail})

	entries = other_systems_entries(text)
	labels = ["%s=%s" % (label, location) for _, _, label, location
		in entries]
	listed = "listed: " + ("; ".join(labels) if labels else "nothing")

	def entry(label):
		return [e for e in entries if e[2] == label]

	def after(first, second):
		a = text.find(first)
		return a >= 0 and text.find(second, a) >= 0

	if firmware == "uefi":
		windows = entry("Windows Boot Manager")
		check("other_systems.detect_windows", len(windows) == 1
			and windows[0][3].startswith("\\EFI\\Microsoft\\Boot\\bootmgfw.efi"),
			listed)
		ubuntu = entry("Ubuntu")
		check("other_systems.detect_shim", len(ubuntu) == 1
			and ubuntu[0][3].startswith("\\EFI\\ubuntu\\shimx64.efi"), listed)
		check("other_systems.no_duplicates", not any(
			"fallback" in label.lower() or "grub" in location.lower()
			or "mmx64" in location.lower() or "bootmgr.efi" in location.lower()
			for _, _, label, location in entries), listed)
		check("other_systems.default_started", after('boot menu: "%s" in %d s'
			% (OTHER_SYSTEMS_EFI_DEFAULT, OTHER_SYSTEMS_TIMEOUT),
			'boot menu: timeout, choosing "%s"' % OTHER_SYSTEMS_EFI_DEFAULT)
			and after('boot menu: timeout, choosing',
				"CE-OTHEROS started windows-efi")
			and "CE-OTHEROS windows-efi device handle: yes" in text,
			"the countdown started the default, which got its device handle"
			if "CE-OTHEROS windows-efi device handle: yes" in text
			else "default not started, or without a device handle")
		check("other_systems.returned", after("CE-OTHEROS windows-efi "
			"returning", 'boot menu: "%s" returned: No error'
			% OTHER_SYSTEMS_EFI_DEFAULT), "the menu came back after the "
			"loader returned" if after("CE-OTHEROS windows-efi returning",
				"returned: No error") else "no return to the menu")
		check("other_systems.key_started", "CE-OTHEROS started ubuntu-shim"
			in text and "CE-OTHEROS started ubuntu-grub" not in text,
			"shim started by its shortcut" if "started ubuntu-shim" in text
			else "shim not started")
		usb = entry("Boot other media: USB drive")
		check("other_media.found", len(usb) == 1
			and "CE-OTHEROS started removable-usb" in text
			and "CE-OTHEROS removable-usb device handle: yes" in text,
			"USB drive found in the background and started" if usb
			else "USB drive not found; " + listed)
		empty = re.search(r"boot menu: CD/DVD: no media after (\d+) ms", text)
		check("other_media.empty_drive", empty is not None
			and int(empty.group(1)) < 3000,
			"empty CD drive answered after %s ms" % empty.group(1) if empty
			else "no answer from the empty CD drive logged")
	else:
		windows = entry("Windows")
		check("other_systems.detect_windows", len(windows) == 1
			and windows[0][3] == "partition 2 of BIOS drive 0x81", listed)
		disk = entry("Hard disk 2")
		check("other_systems.detect_mbr", len(disk) == 1
			and disk[0][3] == "the master boot record of BIOS drive 0x81",
			listed)
		check("other_systems.no_duplicates", not any(
			"partition 1 " in location or "partition 3 " in location
			or "0x82" in location for _, _, _, location in entries), listed)
		check("other_systems.chainload_partition",
			"CE-OTHEROS started bios-windows dl=81 lba=%08x" % OTHER_WINDOWS[0]
			in text, "boot record got DL and DS:SI" if "started bios-windows"
			in text else "partition boot record not started")
		check("other_systems.chainload_mbr",
			"CE-OTHEROS started bios-mbr dl=81 lba=00000000" in text,
			"MBR of the other disk started" if "started bios-mbr" in text
			else "other disk's MBR not started")
		floppy = entry("Boot other media: Floppy disk A:")
		check("other_media.found", len(floppy) == 1
			and "CE-OTHEROS started bios-floppy dl=00" in text,
			"floppy found in the background and started" if floppy
			else "floppy not found; " + listed)
		welcomes = text.count("Welcome to the Renga boot loader!")
		check("other_systems.returned", welcomes == 4,
			"%d boot loader starts (want 4: three systems booted Renga's "
			"disk again)" % welcomes)

	keys = (menu or {}).get("keys", [])
	late = [k for k in keys if k["logged"] is None
		or k["logged"] - k["sent"] > OTHER_KEY_LATENCY]
	steps = [int(ms) for ms in re.findall(r"boot menu: probe step took "
		r"(\d+) ms", text)]
	limit = OTHER_PROBE_STEP_MS if probe_limit_ms is None else probe_limit_ms
	check("other_media.responsive", keys and not late
		and all(ms <= limit for ms in steps),
		"%d key(s), slowest %s s; %d slow probe step(s), longest %s ms%s" % (
			len(keys), max((k["logged"] - k["sent"] for k in keys
				if k["logged"] is not None), default="-"),
			len(steps), max(steps, default=0),
			"; not answered or late: " + ", ".join(k["key"] for k in late)
				if late else ""))
	check("other_systems.continued", "CE-HARNESS START" in text[
		text.rfind("boot menu:"):] if "boot menu:" in text else False,
		"Renga booted after the menu" if "CE-HARNESS START" in text
		else "Renga did not boot")
	return checks


def check_other_systems(args, results_dir, report):
	if not args.other_systems:
		return
	try:
		with open(os.path.join(results_dir, "serial.log"),
				errors="replace") as serial:
			text = serial.read().replace("\r\n", "\n")
	except OSError:
		text = ""
	report.setdefault("host_checks", []).extend(other_systems_checks(
		args.firmware, text, report.get("boot", {}).get("boot_menu")))


# #pragma mark - installer (--installer-upgrade)


# The older, stock-Haiku-like installation the Installer upgrades: the
# image's own packages, its system package re-packed with this version, a
# user-installed package, an activated-packages file and an older state.
INSTALLER_OLD_VERSION = "r1~beta5_hrev57937-1"
INSTALLER_OLD_PACKAGE = "haiku-%s-x86_64.hpkg" % INSTALLER_OLD_VERSION
INSTALLER_USER_PACKAGE = "ce_userapp-1.0-1-any.hpkg"
INSTALLER_OLDER_STATE = "state_2025-01-01_12:00:00"
# Big enough for an upgrade that keeps the old system as a state.
INSTALLER_IMAGE_SIZE = 1200
INSTALLER_USER_PACKAGE_INFO = """name			ce_userapp
version			1.0-1
architecture	any
summary			"Renga harness: a package the user installed"
description		"Stands in for software the user installed, for the Installer's upgrade test."
packager		"Renga harness <harness@localhost>"
vendor			"Renga harness"
copyrights		"2026 Renga contributors"
licenses		"MIT"
provides {
	ce_userapp = 1.0
}
"""


def host_package_tool(args):
	tool = os.path.join(args.output, "objects", "linux", "x86_64", "release",
		"tools", "package", "package")
	environment = dict(os.environ, LD_LIBRARY_PATH=os.path.join(args.output,
		"objects", "linux", "lib"))
	return tool, environment


def bfs_shell_run(args, image, commands):
	"""Runs bfs_shell \a commands on the BFS volume of \a image; returns
	(exit status, output)."""
	offset = bfs_volume_offset(image)
	shells = glob.glob(os.path.join(args.output, "objects", "*", "*", "*",
		"tools", "bfs_shell", "bfs_shell"))
	if offset is None or not shells:
		return 1, "no BFS volume or no bfs_shell"
	result = subprocess.run([shells[0], "--start-offset", str(offset), image],
		input=(commands + "sync\nquit\n").encode(), stdout=subprocess.PIPE,
		stderr=subprocess.STDOUT, timeout=300)
	return result.returncode, result.stdout.decode("utf-8", "replace")


def rename_bfs_volume(image, name):
	offset = bfs_volume_offset(image)
	with open(image, "r+b") as disk:
		disk.seek(offset + 512)
		disk.write(name.encode().ljust(32, b"\0"))


def old_system_package(args, work):
	"""The image's system package, re-packed as INSTALLER_OLD_VERSION with
	the build's host package tool (cached per build of it)."""
	source = os.path.join(args.output, "objects", "haiku", "x86_64",
		"packaging", "packages", "haiku.hpkg")
	cache = os.path.join(args.output, "ce-installer-cache")
	os.makedirs(cache, exist_ok=True)
	cached = os.path.join(cache, "old-haiku-%d.hpkg"
		% int(os.path.getmtime(source)))
	if os.path.exists(cached):
		return cached
	for old in glob.glob(os.path.join(cache, "old-haiku-*.hpkg")):
		os.remove(old)
	tool, environment = host_package_tool(args)
	tree = os.path.join(work, "haiku-tree")
	os.makedirs(tree)
	subprocess.run([tool, "extract", "-C", tree, source], check=True,
		env=environment, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
	info = os.path.join(tree, ".PackageInfo")
	with open(info) as infoFile:
		text = infoFile.read()
	text = re.sub(r"(?m)^version\s.*$", "version\t\t\t%s"
		% INSTALLER_OLD_VERSION, text)
	with open(info, "w") as infoFile:
		infoFile.write(text)
	subprocess.run([tool, "create", "-1", "-C", tree, "-i", info, cached],
		check=True, env=environment, stdout=subprocess.PIPE,
		stderr=subprocess.STDOUT)
	shutil.rmtree(tree)
	return cached


def user_package(args, work):
	tool, environment = host_package_tool(args)
	tree = os.path.join(work, "user-tree")
	os.makedirs(os.path.join(tree, "data", "ce_userapp"))
	with open(os.path.join(tree, "data", "ce_userapp", "README"), "w") as f:
		f.write("A package the user installed.\n")
	info = os.path.join(work, "user.PackageInfo")
	with open(info, "w") as infoFile:
		infoFile.write(INSTALLER_USER_PACKAGE_INFO)
	path = os.path.join(work, INSTALLER_USER_PACKAGE)
	subprocess.run([tool, "create", "-C", tree, "-i", info, path],
		check=True, env=environment, stdout=subprocess.PIPE,
		stderr=subprocess.STDOUT)
	return path


def make_installer_disks(args, results_dir, report):
	"""--installer-upgrade: two copies of the (larger) test image made to
	look like an older stock Haiku installation, "OldHaiku" and
	"OldHaiku2" (see INSTALLER_*). Returns their paths, or None (with the
	reason in the report)."""
	image = image_path(args)
	work = os.path.join(results_dir, "installer-work")
	os.makedirs(work, exist_ok=True)
	try:
		oldSystem = old_system_package(args, work)
		userPackage = user_package(args, work)
	except (OSError, subprocess.CalledProcessError) as error:
		report["installer_setup"] = "packages: %s" % error
		return None

	status, listing = bfs_shell_run(args, image, "ls /myfs/system/packages\n")
	packages = sorted(set(re.findall(r"(\S+\.hpkg)", listing)))
	current = [p for p in packages if p.startswith("haiku-")]
	if status != 0 or len(current) != 1:
		report["installer_setup"] = "cannot list the image's packages: " \
			+ listing[-300:]
		return None
	installed = sorted([p for p in packages if p != current[0]]
		+ [INSTALLER_OLD_PACKAGE, INSTALLER_USER_PACKAGE])
	older = [p for p in installed if p != INSTALLER_USER_PACKAGE]
	activated = os.path.join(work, "activated")
	with open(activated, "w") as listFile:
		listFile.write("\n".join(installed) + "\n")
	olderList = os.path.join(work, "older")
	with open(olderList, "w") as listFile:
		listFile.write("\n".join(older) + "\n")

	disks = {}
	for key, name in (("old", "OldHaiku"), ("old2", "OldHaiku2")):
		path = os.path.join(results_dir, name.lower() + ".img")
		subprocess.run(["cp", "--sparse=always", image, path], check=True)
		packagesDir = "/myfs/system/packages"
		admin = packagesDir + "/administrative"
		commands = "\n".join([
			"rm %s/%s" % (packagesDir, current[0]),
			"cp :%s %s/%s" % (oldSystem, packagesDir, INSTALLER_OLD_PACKAGE),
			"cp :%s %s/%s" % (userPackage, packagesDir,
				INSTALLER_USER_PACKAGE),
			"mkdir %s" % admin,
			"mkdir %s/%s" % (admin, INSTALLER_OLDER_STATE),
			"cp :%s %s/activated-packages" % (activated, admin),
			"cp :%s %s/%s/activated-packages" % (olderList, admin,
				INSTALLER_OLDER_STATE),
			"ls %s" % packagesDir, ""])
		status, output = bfs_shell_run(args, path, commands)
		if status != 0 or INSTALLER_OLD_PACKAGE not in output \
				or INSTALLER_USER_PACKAGE not in output:
			report["installer_setup"] = "bfs_shell: " + output[-400:]
			return None
		rename_bfs_volume(path, name)
		disks[key] = path
	shutil.rmtree(work)
	report["installer_setup"] = "OldHaiku and OldHaiku2: %d packages, " \
		"%s, %s, older state %s" % (len(installed), INSTALLER_OLD_PACKAGE,
			INSTALLER_USER_PACKAGE, INSTALLER_OLDER_STATE)
	return disks


def installer_drives(disks):
	return ["-drive", "file=%s,format=raw,if=none,id=oldhaiku,snapshot=off"
			% disks["old"],
		"-device", "ide-hd,drive=oldhaiku,bus=ide.1,unit=0",
		"-drive", "file=%s,format=raw,if=none,id=oldhaiku2,snapshot=off"
			% disks["old2"],
		"-device", "ide-hd,drive=oldhaiku2,bus=ide.1,unit=1"]


def run_installer_boots(args, results_dir, report, disks):
	"""After the first boot upgraded OldHaiku: boot it (phase B, the new
	system), then set its boot menu timeout and boot it again choosing the
	old package state in the menu (phase C). Each boot's serial log is
	appended to serial.log, so its checks count like the others."""
	if not boot(args, results_dir, report, image=disks["old"],
			tag="upgraded"):
		log("booting the upgraded disk failed")

	status, phase = bfs_shell_run(args, disks["old"],
		"cat /myfs/home/ce-installer-phase\n")
	match = re.search(r"upgraded\n(new=.*\nold=.*\nstate=.*\n)", phase)
	work = os.path.join(results_dir, "installer-work")
	os.makedirs(work, exist_ok=True)
	if match is not None:
		phaseFile = os.path.join(work, "phase")
		with open(phaseFile, "w") as f:
			f.write("oldstate\n" + match.group(1))
		# (installer_test set the boot menu timeout in phase A)
		bfs_shell_run(args, disks["old"], "cp -f :%s /myfs/home/"
			"ce-installer-phase\n" % phaseFile)
	else:
		report["installer_setup"] += "; no phase B file: " + phase[-200:]
	shutil.rmtree(work)
	if not boot(args, results_dir, report, image=disks["old"],
			tag="oldstate", menu_script="old_state"):
		log("booting the old state failed")

	for tag in ("upgraded", "oldstate"):
		for name in ("serial.log", "serial.timed.log"):
			try:
				with open(os.path.join(results_dir, tag, name), "rb") as part:
					data = part.read()
			except OSError:
				continue
			with open(os.path.join(results_dir, name), "ab") as whole:
				whole.write(("\n[ce-test] --- boot %s ---\n" % tag).encode())
				whole.write(data)
	for path in disks.values():
		os.remove(path)


def check_installer_boots(args, results_dir, report):
	if not args.installer_upgrade:
		return
	checks = report.setdefault("host_checks", [])
	checks.append({"name": "installer.setup",
		"pass": "OldHaiku and OldHaiku2" in report.get("installer_setup", ""),
		"detail": report.get("installer_setup", "not prepared")})
	for tag, what in (("upgraded", "the upgraded disk"),
			("oldstate", "the old package state")):
		boot = report.get("boots", {}).get(tag, {})
		checks.append({"name": "installer.boot_" + tag,
			"pass": boot.get("status") == "exited"
				and boot.get("runner_finished", False),
			"detail": "%s: %s, runner finished: %s" % (what,
				boot.get("status", "not booted"),
				boot.get("runner_finished"))})
	try:
		with open(os.path.join(results_dir, "oldstate", "serial.log"),
				errors="replace") as serial:
			text = serial.read()
	except OSError:
		text = ""
	# F-59: "Select boot volume" names what the volume holds; the
	# upgraded OldHaiku holds Renga
	listed = re.findall(r'boot menu: volume "([^"]*)"\r?$', text,
		re.MULTILINE)
	checks.append({"name": "installer.boot_menu_system",
		"pass": any(name.startswith("OldHaiku - Renga") for name in listed),
		"detail": "volumes listed: %s" % (", ".join(sorted(set(listed)))
			or "none")})
	chosen = re.search(r'boot menu: package state "([^"]*)" chosen', text)
	checks.append({"name": "installer.old_state_chosen",
		"pass": chosen is not None and chosen.group(1) != "Latest state",
		"detail": "boot menu: state \"%s\" chosen" % chosen.group(1)
			if chosen else "no package state chosen in the boot menu"})


# installer_ui_test (renga-* images): a small BFS disk "CETarget" with what
# looks like a Haiku installation, for the Installer's window. Made on the
# host (registering a file as a disk and mkfs in the guest refused to
# initialize it), attached as an IDE disk; the guest's writes go to the
# snapshot.
INSTALLER_UI_TARGET_SIZE = 64 * 1024 * 1024


def make_installer_ui_target(args, results_dir):
	path = os.path.join(results_dir, "cetarget.img")
	shells = glob.glob(os.path.join(args.output, "objects", "*", "*", "*",
		"tools", "bfs_shell", "bfs_shell"))
	if not shells:
		return None
	with open(path, "wb") as image:
		image.truncate(INSTALLER_UI_TARGET_SIZE)
	result = subprocess.run([shells[0], "--initialize", path, "CETarget"],
		stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=120)
	if result.returncode != 0:
		log("cetarget: " + result.stdout.decode("utf-8", "replace")[-200:])
		return None
	files = os.path.join(results_dir, "cetarget-files")
	os.makedirs(files, exist_ok=True)
	with open(os.path.join(files, "package"), "w") as f:
		f.write("not a real package\n")
	with open(os.path.join(files, "letter"), "w") as f:
		f.write("letter\n")
	status, output = bfs_shell_run(args, path, "\n".join([
		"mkdir /myfs/system", "mkdir /myfs/system/packages",
		"mkdir /myfs/home", "mkdir /myfs/home/Documents",
		"cp :%s /myfs/system/packages/%s" % (os.path.join(files, "package"),
			INSTALLER_OLD_PACKAGE),
		"cp :%s /myfs/home/Documents/letter.txt" % os.path.join(files,
			"letter"),
		"ls /myfs/system/packages", ""]))
	shutil.rmtree(files)
	if status != 0 or INSTALLER_OLD_PACKAGE not in output:
		log("cetarget: " + output[-200:])
		return None
	return path


# #pragma mark - boot


class AudioFeeder(threading.Thread):
	"""Feeds the second sound card's capture input: a stereo sine in the
	regular file SDL's disk driver reads (SDL_DISKAUDIOFILEIN). SDL refuses
	FIFOs and reads the file from the start every time QEMU opens the input
	voice (on every PCM_PREPARE), and stops for good at its end. So the
	whole file always holds the current tone: frequencies are whole Hz, so
	one second of it repeats seamlessly. A new tone is written from where
	QEMU's SDL reads now (its file position, from /proc/<pid>/fdinfo) to the
	end first, then before it; the file is kept CAPTURE_LEAD_SECONDS ahead of
	the reader."""

	def __init__(self, path):
		threading.Thread.__init__(self, daemon=True)
		self.path = path
		self.pid = None			# QEMU's, set once it runs
		self.lock = threading.Lock()
		self.wake = threading.Event()
		self.tone = CAPTURE_DEFAULT_TONE
		self.written_tone = None
		self.changes = []		# (host seconds, tone, seconds to write it)
		self.most_read = 0		# furthest read position seen, in bytes
		self.stopped = threading.Event()
		self.started_at = time.time()
		self.error = None
		self.size = 0
		self._block = None
		self._write_tone(0)

	def set_tone(self, left, right, amplitude):
		with self.lock:
			self.tone = (left, right, amplitude)
		self.wake.set()

	def _one_second(self, tone):
		left, right, amplitude = tone
		samples = []
		for frame in range(CAPTURE_RATE):
			samples.append(int(amplitude * math.sin(
				2 * math.pi * left * frame / CAPTURE_RATE)))
			samples.append(int(amplitude * math.sin(
				2 * math.pi * right * frame / CAPTURE_RATE)))
		return struct.pack("<%dh" % len(samples), *samples)

	def _read_position(self):
		"""Where QEMU's SDL reads the file now, or None."""
		if self.pid is None:
			return None
		try:
			for fd in os.listdir("/proc/%d/fd" % self.pid):
				try:
					if os.readlink("/proc/%d/fd/%s" % (self.pid, fd)) \
							!= self.path:
						continue
					with open("/proc/%d/fdinfo/%s" % (self.pid, fd)) as info:
						for line in info:
							if line.startswith("pos:"):
								return int(line.split()[1])
				except OSError:
					continue
		except OSError:
			pass
		return None

	def _write_tone(self, position):
		"""Writes the current tone over the whole file (from \a position on
		first) and makes it CAPTURE_LEAD_SECONDS longer than \a position."""
		with self.lock:
			tone = self.tone
		started = time.time()
		block = self._one_second(tone)
		second = len(block)
		position -= position % 4
		end = max(self.size, position + CAPTURE_LEAD_SECONDS * second)
		with open(self.path, "r+b" if os.path.exists(self.path)
				else "w+b") as feed:
			for start, stop in ((position, end), (0, position)):
				offset = start
				while offset < stop:
					# Keep the phase of the tone tied to the file offset, so
					# that every block joins the next one seamlessly.
					phase = offset % second
					chunk = block[phase:] + block[:phase]
					chunk = chunk[:stop - offset]
					feed.seek(offset)
					feed.write(chunk)
					offset += len(chunk)
		self.size = end
		self.written_tone = tone
		self.changes.append((round(started - self.started_at, 2), tone,
			round(time.time() - started, 2)))

	def run(self):
		try:
			while not self.stopped.is_set():
				self.wake.wait(0.25)
				self.wake.clear()
				position = self._read_position()
				if position is not None:
					self.most_read = max(self.most_read, position)
				with self.lock:
					changed = self.tone != self.written_tone
				lead = self.size - (position or 0)
				if changed or lead < CAPTURE_LEAD_SECONDS // 2 \
						* CAPTURE_RATE * 4:
					self._write_tone(position or 0)
		except OSError as error:
			self.error = str(error)

	def stop(self):
		self.stopped.set()
		self.wake.set()
		self.join(timeout=10)


class SerialReader(threading.Thread):
	"""Copies the guest's serial output to a file and watches it.

	When the kernel drops into its debugger it asks for a backtrace and
	syslog tail, then lets the main thread stop the VM.
	"""

	def __init__(self, process, log_path, monitor_path=None, qmp_path=None,
			feeder=None):
		threading.Thread.__init__(self, daemon=True)
		self.feeder = feeder
		self.process = process
		self.log_path = log_path
		self.monitor_path = monitor_path
		self.qmp_path = qmp_path
		self.screenshots = []	# saved on request, as PPM
		self.line = b""		# incomplete last line, for CE-HOST requests
		self.host_requests = []
		self.menu_driver = None	# BootMenuDriver, with --other-systems
		self.usb_audio_added = 0		# usb-audio devices plugged in so far
		self.usb_audio_plugged = []		# their indices, still plugged in
		self.done = threading.Event()
		self.kdl = threading.Event()
		self.finished = threading.Event()
		self.debug_done = threading.Event()
		self.last_output = time.time()
		self.lock = threading.Lock()
		self.kdl_text = ""	# everything printed since KDL was entered
		# Debugger entries a test announced (CE-HOST kdl_reboot): how many
		# are still expected, whether one is being answered now, and what
		# happened to each (see _expected_kdl_reboot).
		self.kdl_expected = 0
		self.kdl_answering = False
		self.expected_kdls = []
		self.expected_kdl_text = ""

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
				if self.kdl_answering:
					# An announced debugger entry: its output is kept apart
					# and does not stop the run.
					with self.lock:
						self.expected_kdl_text += chunk.decode("latin-1")
					buffer = b""
				elif self.kdl.is_set():
					with self.lock:
						self.kdl_text += chunk.decode("latin-1")
				elif self.kdl_expected > 0 \
						and any(p in text for p in KDL_PATTERNS):
					self.kdl_expected -= 1
					with self.lock:
						self.expected_kdl_text = text
					buffer = b""
					self.kdl_answering = True
					threading.Thread(target=self._expected_kdl_reboot,
						daemon=True).start()
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
		  CE-HOST pointer_button <device> down|up
		      press or release the left button of an absolute device the
		      harness attached (drags, F-59)
		  CE-HOST screenshot <name>
		      save the screen as screen-<name>.png in the results (name:
		      up to 40 lower-case letters, digits and underscores)
		  CE-HOST audio_in <left Hz> <right Hz> <amplitude>
		      feed this stereo sine into the second sound card's capture
		      input from now on (0-20000 Hz, amplitude 0-32767)
		  CE-HOST usb_audio add|remove
		      plug a QEMU usb-audio device into the audio xhci controller
		      (it records into usb-<n>.wav, n counting the adds) or pull
		      the one plugged in last out (USB_AUDIO_DEVICES)
		  CE-HOST kdl_reboot
		      the guest is about to enter the kernel debugger on purpose:
		      answer the next entry with "reboot" instead of stopping the
		      run (see _expected_kdl_reboot)

		Pointer and screenshot requests are carried out before the next
		line is read, so they happen in order and before anything the test
		prints after them.
		"""
		self.line += chunk
		lines = self.line.split(b"\n")
		self.line = lines.pop()
		for raw in lines:
			line = raw.decode("latin-1").strip()
			if self.menu_driver is not None:
				self.menu_driver.feed(line)
			if not line.startswith("CE-HOST "):
				continue
			words = line.split()[1:]
			accepted = host_request_allowed(words)
			request = {"request": " ".join(words), "accepted": accepted}
			if accepted and words[0] == "audio_in":
				if self.feeder is not None:
					self.feeder.set_tone(*[int(w) for w in words[1:]])
				else:
					request["error"] = "no capture feed"
			elif accepted and words[0] in ("pointer", "pointer_rel",
					"pointer_button"):
				if self.qmp_path is not None:
					request["error"] = qmp_pointer(self.qmp_path, words)
			elif accepted and words[0] == "usb_audio":
				request["error"] = self._usb_audio(words[1])
			elif accepted and words[0] == "kdl_reboot":
				self.kdl_expected += 1
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

	def _usb_audio(self, action):
		"""Plugs a QEMU usb-audio device in (the next audiodev, see
		USB_AUDIO_DEVICES) or pulls the last one out, through the monitor.
		Returns None or what went wrong."""
		if self.monitor_path is None:
			return "no monitor"
		if action == "add":
			index = self.usb_audio_added
			if index >= USB_AUDIO_DEVICES:
				return "all %d usb audio devices used" % USB_AUDIO_DEVICES
			self.usb_audio_added += 1
			self.usb_audio_plugged.append(index)
			command = "device_add usb-audio,id=ceua%d,audiodev=usbsnd%d," \
				"bus=%s.0" % (index, index, USB_AUDIO_CONTROLLER)
		else:
			if not self.usb_audio_plugged:
				return "no usb audio device plugged in"
			index = self.usb_audio_plugged.pop()
			command = "device_del ceua%d" % index
		if not monitor_command(self.monitor_path, command):
			return "monitor command failed"
		return None

	def _send(self, command, settle=1.0):
		try:
			self.process.stdin.write(command.encode() + b"\n")
			self.process.stdin.flush()
		except OSError:
			return False
		time.sleep(settle)
		return True

	def _expected_kdl_reboot(self):
		"""Answers a kernel debugger entry the guest announced: waits for
		the prompt (the crash report is written before it), lets QEMU reset
		the machine on a guest reboot instead of exiting (-no-reboot is
		turned off for this one reboot through QMP's set-action), types
		"reboot" and, once the machine has reset, turns -no-reboot on
		again. The debugger's output goes to kdl-expected-<n>.log."""
		entry = {"answered": False, "error": None}
		self.expected_kdls.append(entry)
		try:
			deadline = time.time() + EXPECTED_KDL_PROMPT_TIMEOUT
			while time.time() < deadline:
				with self.lock:
					if "kdebug>" in self.expected_kdl_text:
						break
				time.sleep(0.2)
			else:
				entry["error"] = "no debugger prompt"
				return
			time.sleep(1)
			with self.lock:
				text = self.expected_kdl_text
			entry["message"] = next((m for m in EXPECTED_KDL_MESSAGES
				if m in text), None)
			if self.qmp_path is not None:
				entry["error"] = qmp_execute(self.qmp_path, "set-action",
					reboot="reset")
			# F-59: the second entry restarts with Alt+SysRq+B, the key the
			# debugger's closing lines name besides "reboot"
			if len(self.expected_kdls) == 2 and self.monitor_path is not None:
				entry["answer"] = "alt-sysrq-b"
				if entry["error"] is None and monitor_command(
						self.monitor_path, "sendkey alt-sysrq-b"):
					entry["answered"] = True
			else:
				entry["answer"] = "reboot"
				if entry["error"] is None and self._send("reboot", 1):
					entry["answered"] = True
			time.sleep(EXPECTED_KDL_QUIET_SECONDS)
			if self.qmp_path is not None:
				error = qmp_execute(self.qmp_path, "set-action",
					reboot="shutdown")
				if error and not entry["error"]:
					entry["error"] = "restoring -no-reboot: " + error
		finally:
			with self.lock:
				entry["text"] = self.expected_kdl_text
				self.expected_kdl_text = ""
			self.last_output = time.time()
			self.kdl_answering = False

	def _debug_session(self):
		"""Collects what is needed to debug a panic or hang without a
		second run: the current stack, all threads, the backtrace of every
		thread that is waiting or running (bounded), and the lock owners."""
		try:
			time.sleep(1)
			self._send("bt", 2)
			with self.lock:
				before = len(self.kdl_text)
			self._send("threads", 4)
			# The list of a full desktop is long and the serial line slow:
			# wait (up to 30 s) for the next prompt, so that every thread
			# is in it before choosing whom to trace.
			deadline = time.time() + 30
			while time.time() < deadline:
				with self.lock:
					text = self.kdl_text[before:]
				start = text.find("threads")
				if start >= 0 and "\nkdebug>" in text[start:]:
					break
				time.sleep(0.5)
			with self.lock:
				listing = self.kdl_text
			ids = []
			userWaits = []
			mutexes = []
			for line in listing.splitlines():
				fields = line.split()
				# "0xffff... <id> <state> [<wait type> <object>] ..." rows
				if len(fields) >= 3 and fields[0].startswith("0x") \
						and fields[1].isdigit():
					if fields[2] in ("waiting", "running", "ready"):
						# Threads blocked on user-level locks (pthread
						# mutexes, rw_locks: "waiting user") and running
						# ones are the usual suspects of a userland
						# deadlock; trace them before the long tail.
						if fields[2] == "running" or (len(fields) >= 4
								and fields[3] == "user"):
							userWaits.append(fields[1])
						else:
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
			ids = userWaits + ids
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
	if len(words) == 4 and words[0] == "audio_in":
		return all(re.fullmatch(r"\d{1,5}", w) is not None for w in words[1:]) \
			and int(words[1]) <= 20000 and int(words[2]) <= 20000 \
			and int(words[3]) <= 32767
	if len(words) == 2 and words[0] == "usb_audio":
		return words[1] in ("add", "remove")
	if words == ["kdl_reboot"]:
		return True
	if len(words) == 2 and words[0] == "screenshot":
		return re.fullmatch(r"[a-z0-9_]{1,40}", words[1]) is not None
	if len(words) == 3 and words[0] == "pointer_button":
		return words[1] in POINTER_DEVICES and words[2] in ("down", "up")
	if len(words) == 3 and words[0] == "pointer_rel":
		return all(re.fullmatch(r"-?\d{1,3}", w) is not None
			and abs(int(w)) <= POINTER_REL_MAX for w in words[1:])
	return False


def qmp_execute(path, command, **arguments):
	"""Runs one QMP command. Returns None, or what went wrong."""
	import socket
	try:
		with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as sock:
			sock.settimeout(5)
			sock.connect(path)
			stream = sock.makefile("rw")
			stream.readline()		# greeting
			for name, args in (("qmp_capabilities", {}),
					(command, arguments)):
				message = {"execute": name}
				if args:
					message["arguments"] = args
				stream.write(json.dumps(message) + "\n")
				stream.flush()
				while True:
					answer = json.loads(stream.readline())
					if "event" not in answer:
						break
				if "error" in answer:
					return "%s: %s" % (name, answer["error"].get("desc"))
		return None
	except (OSError, ValueError) as error:
		return "%s: %s" % (command, error)


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
			if words[0] in ("pointer", "pointer_button"):
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
			if words[0] == "pointer_button":
				execute("input-send-event", events=[
					{"type": "btn", "data": {"button": "left",
						"down": words[2] == "down"}}])
			elif words[0] == "pointer":
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


class HostLoadSampler(threading.Thread):
	"""Samples the host's one-minute load average every 10 s while the guest
	runs. QEMU without KVM runs the guest at the host's mercy: a run with
	the host's CPUs busy (a build, another QEMU) misses audio deadlines the
	guest would keep otherwise, and the timing checks of the Media Kit tests
	(media.click_free_volume, media_latency.*) are the first to fail. The
	summary shows the highest load next to the host's CPU count, so that
	such a failure can be told from a regression."""

	def __init__(self):
		threading.Thread.__init__(self, daemon=True)
		self.samples = []
		self.stop = threading.Event()

	def run(self):
		while True:
			try:
				self.samples.append(round(os.getloadavg()[0], 2))
			except OSError:
				return
			if self.stop.wait(10):
				return

	def result(self):
		self.stop.set()
		if not self.samples:
			return None
		return {"cpus": os.cpu_count(), "max": max(self.samples),
			"mean": round(sum(self.samples) / len(self.samples), 2),
			"samples": len(self.samples)}


def boot(args, results_dir, report, image=None, extra_drives=(), tag="",
		menu_script=None):
	"""Boots the test image (or \a image: another disk, booted as it is)
	and collects its serial output. A \a tag names a further boot of the
	same run (--installer-upgrade): its logs are serial-<tag>.log, its
	report is report["boots"][tag]. \a extra_drives are QEMU options for
	more drives; \a menu_script makes a BootMenuDriver play the boot
	menu."""
	bootReport = {}
	if tag:
		report.setdefault("boots", {})[tag] = bootReport
		# its own directory: recordings and scratch disks are per boot
		results_dir = os.path.join(results_dir, tag)
		os.makedirs(results_dir, exist_ok=True)
	else:
		report["boot"] = bootReport
	plainImage = image is not None
	if image is None:
		image = image_path(args)
	if not os.path.exists(image):
		bootReport.update({"status": "no-image", "image": image})
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
	if not plainImage and (args.boot_menu_timeout or args.only
			or args.other_systems or args.installer_upgrade):
		patched = os.path.join(results_dir, "patched.img")
		error = make_patched_image(args, image, patched)
		if error is not None:
			bootReport.update({"status": "image-setup-failed",
				"image": image, "error": error})
			log(error)
			return False
		image = patched

	otherMedia = None
	if args.other_systems and not tag:
		try:
			otherMedia = make_other_systems_media(results_dir)
		except (OSError, subprocess.CalledProcessError) as error:
			bootReport.update({"status": "image-setup-failed",
				"image": image,
				"error": "other systems media: %s (needs nasm and mtools)"
					% error})
			log(bootReport["error"])
			return False

	decoy = None
	if args.two_installs:
		decoy = os.path.join(results_dir, "decoy.img")
		if not make_decoy(image, decoy):
			bootReport.update({"status": "no-bfs-volume", "image": image})
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
		# The second card: capture from the harness, output to a raw file
		# (see CAPTURE_RATE).
		"-audiodev", "sdl,id=snd1,in.frequency=%d,in.channels=2,"
			"in.format=s16,in.buffer-length=%d,out.frequency=%d,"
			"out.channels=2,out.format=s16,out.buffer-length=%d"
			% (CAPTURE_RATE, CAPTURE_PERIOD_FRAMES * 1000000 // CAPTURE_RATE,
				CAPTURE_RATE,
				CAPTURE_PERIOD_FRAMES * 1000000 // CAPTURE_RATE),
		"-device", "virtio-sound-pci,audiodev=snd1",
		"-device", "qemu-xhci,id=%s" % USB_AUDIO_CONTROLLER,
	] + [arg for n in range(USB_AUDIO_DEVICES) for arg in (
		"-audiodev", "wav,id=usbsnd%d,path=%s,out.frequency=48000,"
			"out.channels=2,out.format=s16"
			% (n, os.path.join(results_dir, "usb-%d.wav" % n)))] + [
		"-nic", "none",
		# QEMU's educational PCI device (MMIO, MSI, DMA): lkpi_pcitest
		# tests the LinuxKPI core against it (linuxkpi suite, F-26;
		# check_linuxkpi()). A 24-bit DMA mask instead of 28 puts nearly
		# all guest memory above it, so the DMA API must bounce.
		"-device", "edu,dma_mask=0xffffff",
		# QEMU's PCI test device: fbsd_wlantest, the net80211 test
		# driver of harness images, attaches to it (wlan_reload suite)
		"-device", "pci-testdev",
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
	if otherMedia is not None:
		qemu += other_systems_drives(args, otherMedia)
	qemu += list(extra_drives)

	bootReport.update({"command": " ".join(qemu), "image": image})
	if decoy is not None:
		bootReport["volume_name"] = bfs_volume_name(image)
	log("booting: " + " ".join(qemu))

	feed = os.path.join(results_dir, "capture-in.raw")
	feeder = AudioFeeder(feed)
	environment = dict(os.environ, SDL_AUDIODRIVER="disk",
		SDL_DISKAUDIOFILEIN=feed,
		SDL_DISKAUDIOFILE=os.path.join(results_dir, "sdl-out.raw"))

	loadSampler = HostLoadSampler()
	loadSampler.start()
	started = time.time()
	qemu_stderr = open(os.path.join(results_dir, "qemu.stderr.log"), "wb")
	process = subprocess.Popen(qemu, stdin=subprocess.PIPE,
		stdout=subprocess.PIPE, stderr=qemu_stderr, env=environment)
	feeder.pid = process.pid
	feeder.start()
	reader = SerialReader(process, serial_log, monitor_path, qmp_path,
		feeder)
	menuDriver = None
	if menu_script is not None or (args.other_systems and not tag):
		menuDriver = BootMenuDriver(menu_script or args.firmware,
			monitor_path)
		reader.menu_driver = menuDriver
		menuDriver.start()
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
		# The boots after an announced debugger entry (crashlog_test) are
		# slow in QEMU (up to two minutes to the runner, with long quiet
		# stretches while the boot volume is read); give them more time
		# before calling it a hang.
		stall = args.stall if not reader.expected_kdls \
			else max(args.stall, EXPECTED_KDL_STALL)
		if stalled_at is None and silent > stall \
				and not reader.finished.is_set():
			# No serial output for a long time: press Alt+SysRq+D, Haiku's
			# emergency key for the kernel debugger, so that the session
			# above records where every thread is stuck.
			stalled_at = elapsed
			log("no serial output for %.0fs: entering the kernel debugger"
				% silent)
			bootReport["stall_after_seconds"] = round(elapsed - silent, 1)
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
	feeder.stop()
	os.remove(feed)
	bootReport["capture_feed"] = {
		"most_read_seconds": round(feeder.most_read / (CAPTURE_RATE * 4), 1),
		"tones_written": feeder.changes, "error": feeder.error}
	for copy in (decoy, patched):
		# Full copies of the image, only needed while the guest runs.
		if copy is not None:
			os.remove(copy)
	bootReport["screenshots"] = convert_screenshots(shots
		+ reader.screenshots)
	if reader.kdl.is_set():
		# The whole debugger session (every backtrace); summary.md only
		# shows the start of it.
		with open(os.path.join(results_dir, "kdl.log"), "w") as kdlFile:
			kdlFile.write(reader.kdl_text.replace("\r", ""))
		bootReport["kdl_log"] = "kdl.log"
	for path in (monitor_path, qmp_path):
		if os.path.exists(path):
			os.unlink(path)
	expected = []
	for index, entry in enumerate(reader.expected_kdls):
		name = "kdl-expected-%d.log" % (index + 1)
		with open(os.path.join(results_dir, name), "w") as kdlFile:
			kdlFile.write(entry.get("text", "").replace("\r", ""))
		expected.append({"answered": entry["answered"],
			"error": entry["error"], "message": entry.get("message"),
			"answer": entry.get("answer"), "log": name})
	bootReport["expected_kdl"] = expected
	bootReport["expected_kdl_unanswered"] = reader.kdl_expected
	bootReport["status"] = status
	bootReport["seconds"] = round(time.time() - started, 1)
	bootReport["host_load"] = loadSampler.result()
	bootReport["runner_finished"] = reader.finished.is_set()
	bootReport["host_requests"] = reader.host_requests
	if menuDriver is not None:
		bootReport["boot_menu"] = menuDriver.summary()
	if otherMedia is not None:
		for path in otherMedia.values():
			os.remove(path)
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

	in_expected_kdl = False
	kept = []		# the text without the announced debugger entries
	for line in text.splitlines():
		line = line.rstrip("\r")
		if line.startswith(EXPECTED_KDL_MARKER):
			in_expected_kdl = True
		elif line.startswith("CE-HARNESS START"):
			in_expected_kdl = False
		if not in_expected_kdl:
			kept.append(line)
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
			# In an announced debugger entry, only its own panic and the
			# debugger's welcome are expected.
			if not in_expected_kdl \
					or not (any(m in line for m in EXPECTED_KDL_MESSAGES)
						or "Welcome to Kernel Debugging Land" in line):
				problems.append(line)

	settle_media_changed(text, tests)

	# Keep the debugger output (backtrace) if there was a crash (not an
	# announced debugger entry).
	text_kept = "\n".join(kept)
	kdl_index = text_kept.find("Welcome to Kernel Debugging Land")
	if kdl_index >= 0:
		start = text_kept.rfind("\n", 0, max(0, kdl_index - 2000))
		report["kernel_debugger"] = text_kept[start:start + 8000]

	for driver in WATCHED_DRIVERS:
		marker = "\x1b[33m" + driver + ":"
		errors = [line.replace("\x1b[33m", "").replace("\x1b[0m", "")
			for line in text.splitlines()
			if marker in line and not line.startswith("KERN: ")]
		tests.append({"name": "driver.%s.errors" % driver,
			"pass": len(errors) == 0,
			"detail": "%d error line(s)%s" % (len(errors),
				": " + errors[0].strip() if errors else "")})

	# Which test programs ran, and for how long: runner.sh prints
	# "CE-INFO running <name>" and "CE-INFO <name> exited with <status>";
	# the host time comes from the time-stamped copy of the serial log.
	timing = []
	timed = {}
	try:
		with open(serial_log.replace(".log", ".timed.log"), "rb") as timedFile:
			timed_text = timedFile.read().decode("latin-1")
	except OSError:
		timed_text = ""
	for line in timed_text.splitlines():
		match = re.match(r"^\[\s*([0-9.]+)\] (CE-INFO|CE-HARNESS) (.*)$",
			line.rstrip("\r"))
		if not match:
			continue
		stamp, kind, rest = float(match.group(1)), match.group(2), \
			match.group(3)
		if kind == "CE-HARNESS" and rest.startswith("START"):
			# the first boot's (crashlog_test reboots the guest; the
			# runner's time then spans every boot)
			timed.setdefault("runner_start", stamp)
		elif kind == "CE-HARNESS" and rest.startswith("END"):
			timed["runner_end"] = stamp
		elif rest.startswith("running "):
			timing.append({"test": rest.split()[1], "start": stamp})
		else:
			exited = re.match(r"^(\S+) exited with (-?\d+)", rest)
			if exited and timing and timing[-1]["test"] == exited.group(1):
				timing[-1]["seconds"] = round(stamp - timing[-1]["start"], 1)
				timing[-1]["status"] = int(exited.group(2))
	report["timing"] = {"tests": timing,
		"boot_to_runner": timed.get("runner_start"),
		"runner_seconds": round(timed["runner_end"] - timed["runner_start"], 1)
			if "runner_start" in timed and "runner_end" in timed else None}

	# Every test program that should have run did, to the end: all of them
	# in a full run, the --only selection in a partial one. A program that
	# is missing from the image or never returns would otherwise only show
	# as fewer checks.
	expected = report.get("selected_tests") or report.get("installed_tests")
	if expected:
		finished = [t["test"] for t in timing if "seconds" in t]
		missing = [name for name in expected if name not in finished]
		extra = [name for name in finished if name not in expected]
		tests.append({"name": "harness.tests_ran",
			"pass": not missing and not extra,
			"detail": "%d of %d test programs ran to the end%s%s" % (
				len(expected) - len(missing), len(expected),
				"; missing: " + ", ".join(missing) if missing else "",
				"; not selected but ran: " + ", ".join(extra)
					if extra else "")})

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


def check_linuxkpi(results_dir, report):
	"""QEMU's edu device masks DMA addresses to its DMA mask and prints
	"EDU: clamping DMA <address> to <address>!" on its standard output (the
	serial pipe, so serial.log) when an address does not fit (the harness sets it to 24
	bits). lkpi_pcitest maps buffers above that mask on purpose (linuxkpi_test): LinuxKPI's DMA
	API has to bounce them, and a clamp means it handed the device an
	address it cannot reach. The guest-side check that the data arrived
	intact is linuxkpi.edu_dma_streaming_roundtrip."""
	clamps = []
	try:
		with open(os.path.join(results_dir, "serial.log"),
				errors="replace") as serial:
			clamps = [line.strip() for line in serial
				if "EDU: clamping" in line]
	except OSError:
		pass
	report.setdefault("host_checks", []).append({
		"name": "linuxkpi.edu_no_clamp",
		"pass": len(clamps) == 0,
		"detail": "%d DMA address(es) clamped by the device%s" % (len(clamps),
			": " + clamps[0] if clamps else "")})


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
	the Renga package repository is configured, ahead of Haiku's and
	HaikuPorts', as runner.sh reports."""
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
		"pass": "Renga" in names and "HaikuPorts" in names,
		"detail": "configured: %s (want Renga next to HaikuPorts)" % (
			" ".join(names) if names else "not reported")})

	# Renga's repository wins over HaikuPorts and Haiku for a package they
	# both offer (haiku_devel, Renga builds of ports): the package solver
	# prefers the lower configured priority.
	priorities = repository_priorities(text)
	renga = priorities.get("Renga")
	haikuports = priorities.get("HaikuPorts")
	haiku = priorities.get("Haiku")
	checks.append({"name": "image.repository_priority",
		"pass": renga is not None and haikuports is not None
			and renga < haikuports and (haiku is None or renga < haiku),
		"detail": "priorities: %s (want Renga lower, i.e. better, than "
			"HaikuPorts and Haiku)" % (", ".join("%s %d" % item
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
		if r["request"].split()[0] in ("pointer", "pointer_rel",
			"pointer_button")]
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
			# A tone that is none of the phases' (the Media Kit tests play
			# on this card later) is "other", not the nearest phase tone.
			# A gap in a phase tone still leaves about half its energy.
			if purity < 0.3:
				best = "other"
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
		if all(hz is None or hz == "other" for hz in segment["key"]):
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


def read_raw_audio(path, rate=CAPTURE_RATE, channels=2):
	"""Raw s16le audio as written by SDL's disk driver: (rate, channels,
	samples per channel)."""
	with open(path, "rb") as rawFile:
		payload = rawFile.read()
	frame_count = len(payload) // (2 * channels)
	values = struct.unpack("<%dh" % (frame_count * channels),
		payload[:frame_count * channels * 2])
	return rate, channels, [values[c::channels] for c in range(channels)]


# How far a tone may be off its frequency in a card's recording: a stream
# that reaches a card through a mixer sink is resampled to that card's own
# pace (clock-drift correction, F-31), and QEMU's second card (SDL's disk
# driver, one sleep per period) runs about 0.5-0.8 % slow, so its file,
# read at the nominal 48 kHz, has the tones that much higher. Upwards
# only (and 0.2 % down): the candidates are 50 Hz apart, under 2 % at the
# top, so a band both ways would let a tone take its neighbour's name.
TONE_SPREAD = 0.01


def tone_windows(samples, rate, candidates, window, spread=0.0):
	"""Per window of one channel: (dominant candidate Hz or None for
	silence or "other" for anything else, purity, RMS). With \a spread, a
	candidate also matches up to that fraction above it (and a fifth of
	it below; the best of a few frequencies). Uses numpy when it is there (the Media Kit
	phases add a minute or more of tones)."""
	offsets = [0.0]
	if spread > 0:
		steps = 5
		offsets = [spread * k / steps for k in range(-1, steps + 1)]
	results = []
	try:
		import numpy
		data = numpy.asarray(samples, dtype=numpy.float64)
		count = len(data) // window
		if count == 0:
			return results
		blocks = data[:count * window].reshape(count, window)
		energy = (blocks * blocks).sum(axis=1)
		n = numpy.arange(window)
		powers = []
		for hz in candidates:
			best_power = None
			for offset in offsets:
				phasor = numpy.exp(-2j * numpy.pi * hz * (1 + offset) * n
					/ rate)
				power = numpy.abs(blocks @ phasor) ** 2
				best_power = power if best_power is None \
					else numpy.maximum(best_power, power)
			powers.append(best_power)
		powers = numpy.array(powers)
		best = powers.argmax(axis=0)
		for i in range(count):
			rms = math.sqrt(energy[i] / window)
			if rms < 100:
				results.append((None, 0.0, rms))
				continue
			purity = 2 * powers[best[i], i] / (window * energy[i])
			results.append((candidates[best[i]] if purity >= 0.5
				else "other", float(purity), rms))
		return results
	except ImportError:
		pass
	for start in range(0, len(samples) - window + 1, window):
		chunk = samples[start:start + window]
		energy = sum(x * x for x in chunk)
		rms = math.sqrt(energy / window)
		if rms < 100:
			results.append((None, 0.0, rms))
			continue
		def power(hz):
			return max(goertzel_power(chunk, rate, hz * (1 + offset))
				for offset in offsets)
		best = max(candidates, key=power)
		purity = 2 * power(best) / (window * energy)
		results.append((best if purity >= 0.5 else "other", purity, rms))
	return results


def media_segments(path, reader, frequencies=None, spread=0.0):
	"""Segments of equal (left, right) tones in a recording, with their
	length, per-channel RMS (interior windows) and start window."""
	if frequencies is None:
		frequencies = MEDIA_FREQUENCIES
	rate, channels, samples = reader(path)
	window = rate // 10
	left = tone_windows(samples[0], rate, frequencies, window, spread)
	right = tone_windows(samples[1] if channels > 1 else samples[0], rate,
		frequencies, window, spread)
	segments = []
	for index, (l, r) in enumerate(zip(left, right)):
		key = (l[0], r[0])
		if segments and segments[-1]["key"] == key:
			segments[-1]["windows"].append((l, r))
		else:
			segments.append({"key": key, "start": index, "windows": [(l, r)]})
	for segment in segments:
		windows = segment["windows"]
		interior = windows[1:-1] if len(windows) > 2 else windows
		segment["seconds"] = len(windows) / 10.0
		segment["left_rms"] = sum(w[0][2] for w in interior) / len(interior)
		segment["right_rms"] = sum(w[1][2] for w in interior) / len(interior)
	return rate, samples, segments


def analyze_media_audio(results_dir, report):
	"""The Media Kit phases in what the two cards played (MEDIA_PHASES):
	per-application volume and mute, routing to the second card and back,
	settings remembered across application and media service restarts, and
	the default output switch. Also that moving a stream to and from the
	second card did not click there: no step between two samples larger
	than the tone itself makes (sdl-out.raw only carries the Media Kit
	phases; the driver tests play silence on that card)."""
	checks = []
	report.setdefault("media_audio", {})["checks"] = checks
	recordings = {}
	for name, reader in (("audio.wav", read_wav),
			("sdl-out.raw", read_raw_audio)):
		try:
			recordings[name] = media_segments(os.path.join(results_dir, name),
				reader, spread=TONE_SPREAD if name == "sdl-out.raw" else 0.0)
		except (OSError, ValueError, struct.error) as error:
			checks.append({"name": "media_audio.recording", "pass": False,
				"detail": "%s: %s" % (name, error)})
			return
	report["media_audio"]["segments"] = {name: [{"start": s["start"] / 10.0,
			"seconds": s["seconds"], "left_hz": s["key"][0],
			"right_hz": s["key"][1], "left_rms": round(s["left_rms"]),
			"right_rms": round(s["right_rms"])}
		for s in segments if s["seconds"] >= 0.3
			and s["key"] != (None, None)]
		for name, (_, _, segments) in recordings.items()}

	def longest(recording, left, right):
		matching = [s for s in recordings[recording][2]
			if s["key"] == (left, right)]
		return max(matching, key=lambda s: s["seconds"], default=None)

	reference = longest(*MEDIA_REFERENCE)
	for name, recording, left, right, seconds, level in MEDIA_PHASES:
		segment = longest(recording, left, right)
		passed = segment is not None and segment["seconds"] >= seconds
		detail = "%s L %s / R %s Hz: %s" % (recording, left or "-",
			right or "-", "%.1fs" % segment["seconds"] if segment
				else "not found")
		if passed and level is not None and reference is not None:
			ratio = segment["left_rms"] / reference["left_rms"]
			passed = abs(ratio - level) <= 0.12 * level
			detail += ", left level %.3f of the reference (want %.3f)" % (
				ratio, level)
		checks.append({"name": name, "pass": passed, "detail": detail})

	# Click check around the moves to and from the second card.
	rate, samples, segments = recordings["sdl-out.raw"]
	first = longest("sdl-out.raw", None, 2700)
	last = longest("sdl-out.raw", None, 2750)
	if first is None or last is None:
		checks.append({"name": "media_audio.route_click_free", "pass": False,
			"detail": "the route phases are not in sdl-out.raw"})
		return
	window = rate // 10
	start = max(0, (first["start"] - 3) * window)
	end = min(len(samples[1]), (last["start"] + len(last["windows"]) + 3)
		* window)
	region = samples[1][start:end]
	peak = max((abs(x) for x in region), default=0)
	step, where = max(((abs(region[i] - region[i - 1]), i)
		for i in range(1, len(region))), default=(0, 0))
	bound = 2 * math.pi * 2750 / rate * peak * 1.15 + 60
	detail = "largest step %d around the moves (%.1f s), a 2750 Hz tone " \
		"at peak %d makes at most %d" % (step, len(region) / rate, peak, bound)
	if step > bound:
		# Say where and what it is: a run of zeros (the card had no buffer
		# for a period) or a jump in the tone (a buffer dropped or repeated).
		zeros = 0
		for x in region[where:where + rate]:
			if x != 0:
				break
			zeros += 1
		detail += "; at %.3f s in sdl-out.raw (%.3f s after the first " \
			"move), %s" % ((start + where) / rate,
			(start + where) / rate - first["start"] / 10.0,
			"followed by %d zero samples" % zeros if zeros > 8
				else "a jump in the tone")
	checks.append({"name": "media_audio.route_click_free",
		"pass": len(region) > 0 and step <= bound, "detail": detail})


def analyze_sound_audio(results_dir, report):
	"""The media_sound_test phases (SOUND_PHASES) in what the two virtio
	cards played: what the Audio preferences' master volume, mute, output
	device menu and per-application output menu changed. A phase that must
	be absent may not show for 0.3 s or more."""
	checks = []
	report.setdefault("sound_audio", {})["checks"] = checks
	recordings = {}
	for name, reader in (("audio.wav", read_wav),
			("sdl-out.raw", read_raw_audio)):
		try:
			recordings[name] = media_segments(os.path.join(results_dir, name),
				reader, SOUND_FREQUENCIES,
				spread=TONE_SPREAD if name == "sdl-out.raw" else 0.0)
		except (OSError, ValueError, struct.error) as error:
			checks.append({"name": "sound_audio.recording", "pass": False,
				"detail": "%s: %s" % (name, error)})
			return

	def longest(recording, left, right):
		matching = [s for s in recordings[recording][2]
			if s["key"] == (left, right)]
		return max(matching, key=lambda s: s["seconds"], default=None)

	reference = None
	for name, recording, left, right, seconds, level, present \
			in SOUND_PHASES:
		segment = longest(recording, left, right)
		where = "%s L %s / R %s Hz" % (recording, left or "-", right or "-")
		if not present:
			absent = segment is None or segment["seconds"] < 0.3
			checks.append({"name": name, "pass": absent,
				"detail": "%s: %s" % (where, "absent, as it should be"
					if absent else "%.1fs" % segment["seconds"])})
			continue
		passed = segment is not None and segment["seconds"] >= seconds
		detail = "%s: %s" % (where, "%.1fs" % segment["seconds"]
			if segment else "not found")
		if reference is None and segment is not None:
			reference = segment
		if passed and level is not None and reference is not None:
			ratio = segment["left_rms"] / reference["left_rms"]
			passed = abs(ratio - level) <= 0.08 * level
			detail += ", left level %.3f of the first phase (want %.3f)" % (
				ratio, level)
		checks.append({"name": name, "pass": passed, "detail": detail})


def analyze_usb_audio(results_dir, report):
	"""The usb_audio_test phases in what QEMU's usb-audio devices played
	(USB_AUDIO_PHASES): each phase in one piece, as long as it was played,
	at the level the feature unit's volume calls for (QEMU's own mapping,
	qemu_usb_audio_gain()), and the muted phase not at all."""
	checks = []
	report.setdefault("usb_audio_out", {})["checks"] = checks
	recordings = {}
	for name in sorted({phase[1] for phase in USB_AUDIO_PHASES}):
		path = os.path.join(results_dir, name)
		try:
			recordings[name] = media_segments(path, read_wav,
				USB_AUDIO_FREQUENCIES)
		except (OSError, ValueError, struct.error) as error:
			checks.append({"name": "usb_audio_out.recording", "pass": False,
				"detail": "%s: %s" % (name, error)})
			return
	report["usb_audio_out"]["segments"] = {name: [{
			"start": s["start"] / 10.0, "seconds": s["seconds"],
			"left_hz": s["key"][0], "right_hz": s["key"][1],
			"left_rms": round(s["left_rms"]),
			"right_rms": round(s["right_rms"])}
		for s in segments if s["seconds"] >= 0.2
			and s["key"] != (None, None)]
		for name, (_, _, segments) in recordings.items()}

	def matching(recording, left, right):
		return [s for s in recordings[recording][2]
			if s["key"] == (left, right) and s["seconds"] >= 0.2]

	reference = None
	for name, recording, left, right, seconds, level, present \
			in USB_AUDIO_PHASES:
		pieces = matching(recording, left, right)
		if not present:
			checks.append({"name": name, "pass": not pieces,
				"detail": "%s L %d / R %d Hz: %s" % (recording, left, right,
					"%d piece(s), %.1fs" % (len(pieces),
						sum(p["seconds"] for p in pieces)) if pieces
					else "absent, as it should be")})
			continue
		segment = max(pieces, key=lambda p: p["seconds"], default=None)
		passed = segment is not None and segment["seconds"] >= seconds \
			and len(pieces) == 1
		detail = "%s L %d / R %d Hz: %s" % (recording, left, right,
			"%.1fs in %d piece(s)" % (segment["seconds"], len(pieces))
				if segment else "not found")
		if segment is not None and reference is None:
			reference = segment
		if passed and level is not None and reference is not None:
			ratios = [segment[side] / reference[side]
				for side in ("left_rms", "right_rms")]
			passed = all(abs(r - level) <= 0.05 * level for r in ratios)
			detail += ", level %.3f / %.3f of the first phase (want %.3f)" \
				% (ratios[0], ratios[1], level)
		checks.append({"name": name, "pass": passed, "detail": detail})


def analyze_media_usb_audio(results_dir, report):
	"""The media_usb_test phases (MEDIA_USB_PHASES) in what the cards
	played: a stream routed to the hot-plugged device, the device as the
	default output, both streams falling back to the first virtio card when
	it is pulled out, and returning when it is plugged in again."""
	checks = []
	report.setdefault("media_usb_audio", {})["checks"] = checks
	plugged = sorted(glob.glob(os.path.join(results_dir, "usb-*.wav")),
		key=lambda path: int(re.search(r"usb-(\d+)\.wav$", path).group(1)))
	files = {"audio.wav": os.path.join(results_dir, "audio.wav")}
	if len(plugged) >= 2:
		files[MEDIA_USB_FIRST] = plugged[-2]
		files[MEDIA_USB_SECOND] = plugged[-1]
	report["media_usb_audio"]["files"] = {name: os.path.basename(path)
		for name, path in files.items()}
	recordings = {}
	for name in sorted({phase[1] for phase in MEDIA_USB_PHASES}):
		if name not in files:
			checks.append({"name": "media_usb_audio.recording", "pass": False,
				"detail": "%d usb-<n>.wav file(s), the test plugs in twice"
					% len(plugged)})
			return
		try:
			recordings[name] = media_segments(files[name], read_wav,
				MEDIA_USB_FREQUENCIES)
		except (OSError, ValueError, struct.error) as error:
			checks.append({"name": "media_usb_audio.recording", "pass": False,
				"detail": "%s: %s" % (name, error)})
			return
	for name, recording, left, right, seconds in MEDIA_USB_PHASES:
		matching = [s for s in recordings[recording][2]
			if s["key"] == (left, right)]
		segment = max(matching, key=lambda s: s["seconds"], default=None)
		checks.append({"name": name,
			"pass": segment is not None and segment["seconds"] >= seconds,
			"detail": "%s L %s / R %s Hz: %s (want %.1fs)" % (
				os.path.basename(files[recording]),
				left or "-", right or "-", "%.1fs" % segment["seconds"]
					if segment else "not found", seconds)})


# media_routing_test (F-58, OBS Studio's O-2): A set to "None (loopback
# only)" plays this tone (left) on no card while B plays its tone (right)
# on the default output.
ROUTING_SILENT_TONE = 5250
ROUTING_AUDIBLE_TONE = 5350
# F-58 batch 2 (O-18): D, loopback-only by team for now from before its
# stream started, plays this tone (left) on no card.
ROUTING_TRANSIENT_TONE = 5700
# The Output menus with "None (loopback only)": the Audio preferences'
# Applications page and the Media preferences.
ROUTING_SCREENSHOTS = ["routing_loopback_only", "routing_loopback_only_media"]
# CodyCam previewing the virtual camera (F-58, OBS Studio's O-12).
VCAM_SCREENSHOTS = ["virtual_camera_codycam"]
# media_routing_test (F-58, OBS Studio's O-4): application A routed by name
# to "QEMU USB Audio" before it is plugged in plays this tone (left) once
# the device is there.
ROUTING_USB_TONE = 5150
ROUTING_USB_SECONDS = 1.5


def analyze_routing_audio(results_dir, report):
	"""media_routing_test's route by name: some usb-<n>.wav carries A's
	ROUTING_USB_TONE on the left for ROUTING_USB_SECONDS or longer (the
	stream reached the device it was routed to before it existed)."""
	checks = report.setdefault("host_checks", [])
	best = (0.0, None)
	for path in sorted(glob.glob(os.path.join(results_dir, "usb-*.wav"))):
		try:
			_, _, segments = media_segments(path, read_wav,
				[ROUTING_USB_TONE, 5300])
		except (OSError, ValueError, struct.error):
			continue
		for segment in segments:
			if segment["key"][0] == ROUTING_USB_TONE \
					and segment["seconds"] > best[0]:
				best = (segment["seconds"], os.path.basename(path))
	checks.append({"name": "media_routing_audio.route_to_usb",
		"pass": best[0] >= ROUTING_USB_SECONDS,
		"detail": "%d Hz on the left: %s (want %.1fs)" % (ROUTING_USB_TONE,
			"%.1fs in %s" % best if best[1] else "in no usb-<n>.wav",
			ROUTING_USB_SECONDS)})


def analyze_loopback_only_audio(results_dir, report):
	"""media_routing_test's "None (loopback only)" phase: B's
	ROUTING_AUDIBLE_TONE on the right of audio.wav (the phase happened)
	and A's ROUTING_SILENT_TONE on the left of neither card."""
	checks = report.setdefault("host_checks", [])
	audible = 0.0
	heard = []
	for name, reader in (("audio.wav", read_wav),
			("sdl-out.raw", read_raw_audio)):
		try:
			_, _, segments = media_segments(os.path.join(results_dir, name),
				reader, [ROUTING_SILENT_TONE, ROUTING_AUDIBLE_TONE],
				spread=TONE_SPREAD if name == "sdl-out.raw" else 0.0)
		except (OSError, ValueError, struct.error):
			continue
		for segment in segments:
			if name == "audio.wav" \
					and segment["key"][1] == ROUTING_AUDIBLE_TONE:
				audible = max(audible, segment["seconds"])
			if segment["key"][0] == ROUTING_SILENT_TONE \
					and segment["seconds"] >= 0.3:
				heard.append("%.1fs in %s" % (segment["seconds"], name))
	checks.append({"name": "media_routing_audio.loopback_only_silent",
		"pass": audible >= 1.5 and not heard,
		"detail": "B's %d Hz %.1fs on the default output (want 1.5s); A's "
			"%d Hz: %s" % (ROUTING_AUDIBLE_TONE, audible,
			ROUTING_SILENT_TONE, ", ".join(heard) if heard
				else "on no card")})
	# F-58 batch 2 (O-18): D's tone, loopback-only by team from before its
	# stream connected, on no card at all (not even its first buffers).
	heard = []
	read = []
	for name, reader in (("audio.wav", read_wav),
			("sdl-out.raw", read_raw_audio)):
		try:
			_, _, segments = media_segments(os.path.join(results_dir, name),
				reader, [ROUTING_TRANSIENT_TONE, ROUTING_AUDIBLE_TONE],
				spread=TONE_SPREAD if name == "sdl-out.raw" else 0.0)
		except (OSError, ValueError, struct.error):
			continue
		read.append(name)
		for segment in segments:
			if segment["key"][0] == ROUTING_TRANSIENT_TONE \
					and segment["seconds"] >= 0.2:
				heard.append("%.1fs in %s" % (segment["seconds"], name))
	checks.append({"name": "media_routing_audio.transient_silent",
		"pass": bool(read) and not heard,
		"detail": "D's %d Hz (loopback only by team before it started): %s"
			% (ROUTING_TRANSIENT_TONE, ", ".join(heard) if heard
				else "on no card (%s read)" % ", ".join(read) if read
				else "no recording read")})


def zero_runs(samples, start, end, minimum):
	"""Runs of more than \a minimum zero samples in samples[start:end]:
	(count, longest)."""
	count = longest = run = 0
	for x in samples[start:end]:
		if x == 0:
			run += 1
			if run == minimum + 1:
				count += 1
			longest = max(longest, run)
		else:
			run = 0
	return count, longest


def analyze_latency_audio(results_dir, report):
	"""sound_latency_test's tone in low-latency mode (LATENCY_TONE) on the
	default output (whichever card that is at the end of the Media Kit
	tests): every piece of it at least LATENCY_TONE_SECONDS long may have
	at most one gap (a run of more than LATENCY_MAX_ZERO_RUN zero samples:
	a period the card did not get in time is played as silence), of at
	most LATENCY_MAX_GAP samples. QEMU without KVM runs the guest slowly
	and its audio timer ticks every 10 ms, hence one gap is tolerated; the
	guest test applies the same rule to the mix."""
	files = [os.path.join(results_dir, "audio.wav"),
		os.path.join(results_dir, "sdl-out.raw")] + sorted(
		glob.glob(os.path.join(results_dir, "usb-*.wav")))
	found = []
	for path in files:
		if not os.path.exists(path):
			continue
		reader = read_raw_audio if path.endswith(".raw") else read_wav
		try:
			rate, samples, segments = media_segments(path, reader,
				list(LATENCY_TONE) + [2000])
		except (OSError, ValueError, struct.error):
			continue
		# Pieces of the tone a window or two apart are one: a dropped
		# period makes its 100 ms window look like something else.
		spans = []
		for segment in segments:
			if segment["key"] != LATENCY_TONE:
				continue
			first = segment["start"]
			last = segment["start"] + len(segment["windows"])
			if spans and first - spans[-1][1] <= 2:
				spans[-1][1] = last
			else:
				spans.append([first, last])
		window = rate // 10
		for first, last in spans:
			seconds = (last - first) / 10.0
			if seconds < LATENCY_TONE_SECONDS:
				continue
			# the interior: the first and last window can be partial
			gaps, longest = zero_runs(samples[0], (first + 1) * window,
				(last - 1) * window, LATENCY_MAX_ZERO_RUN)
			found.append((os.path.basename(path), seconds, gaps, longest))
	if not found:
		passed = False
		detail = "no %d/%d Hz tone of %.1f s or more on any card" % (
			LATENCY_TONE + (LATENCY_TONE_SECONDS,))
	else:
		passed = all(gaps <= 1 and longest <= LATENCY_MAX_GAP
			for _, _, gaps, longest in found)
		detail = "; ".join("%s: %.1f s of the %d/%d Hz tone, %d gap(s), " \
			"longest run of zeros %d samples" % ((name, seconds)
				+ LATENCY_TONE + (gaps, longest))
			for name, seconds, gaps, longest in found) \
			+ " (at most one gap of %d)" % LATENCY_MAX_GAP
	report.setdefault("host_checks", []).append({
		"name": "media_latency.device_continuous", "pass": passed,
		"detail": detail})


# Screenshots crashlog_test asks for on images with System logs and a
# desktop (F-51).
CRASHLOG_SCREENSHOTS = ["crash_notification", "systemlogs_crash",
	"systemlogs_live", "systemlogs_previous"]
# Kernel debugger entries crashlog_test makes: the panic, then the user's.
CRASHLOG_DEBUGGER_ENTRIES = 2


# The kernel debugger's closing lines after a crash report (F-59).
KDL_KEEP_HINT = "To keep this report, restart with \"reboot\""


def check_kdl_hint(results_dir, report):
	"""crashlog.debugger_hint: every announced debugger entry ended with
	the lines that say how to keep the crash report (F-59), and the one
	answered with Alt+SysRq+B restarted (the next runner start is counted
	by crashlog.debugger_reboots)."""
	entries = report.get("boot", {}).get("expected_kdl", [])

	def text(entry):
		try:
			with open(os.path.join(results_dir, entry.get("log", ""))) as log:
				return log.read()
		except OSError:
			return ""

	hinted = [e for e in entries if KDL_KEEP_HINT in text(e)]
	keys = [e for e in entries if e.get("answer") == "alt-sysrq-b"]
	report.setdefault("host_checks", []).append({
		"name": "crashlog.debugger_hint",
		"pass": len(entries) > 0 and len(hinted) == len(entries)
			and len(keys) == 1 and keys[0]["answered"],
		"detail": "%d of %d debugger entries say how to keep the report; "
			"%d answered with Alt+SysRq+B" % (len(hinted), len(entries),
			len(keys))})


def check_crashlog(results_dir, report):
	"""The crashlog suite's debugger entries (F-51): each was announced,
	answered with "reboot" and followed by a new boot of the guest runner.
	Without this, a reboot that never came back would only show as fewer
	crashlog checks."""
	boot = report.get("boot", {})
	entries = boot.get("expected_kdl", [])
	try:
		with open(os.path.join(results_dir, "serial.log"), "rb") as logFile:
			text = logFile.read().decode("latin-1")
	except OSError:
		text = ""
	starts = len(re.findall(r"^CE-HARNESS START", text, re.MULTILINE))
	answered = [e for e in entries if e["answered"] and not e["error"]]
	messages = [e.get("message") for e in entries]
	report.setdefault("host_checks", []).append({
		"name": "crashlog.debugger_reboots",
		"pass": len(answered) == CRASHLOG_DEBUGGER_ENTRIES
			and len(entries) == CRASHLOG_DEBUGGER_ENTRIES
			and boot.get("expected_kdl_unanswered", 0) == 0
			and None not in messages
			and starts == CRASHLOG_DEBUGGER_ENTRIES + 1,
		"detail": "%d of %d announced debugger entries answered with reboot "
			"(%s), %d runner starts (want %d)%s" % (len(answered),
			CRASHLOG_DEBUGGER_ENTRIES, ", ".join(str(m) for m in messages)
				or "none", starts, CRASHLOG_DEBUGGER_ENTRIES + 1,
			"".join("; %s" % e["error"] for e in entries if e["error"]))})


# Screenshots media_ui_test asks for (CE-HOST screenshot): the
# per-application audio controls (F-31/F-32).
MEDIA_UI_SCREENSHOTS = ["volume_popup", "media_preferences",
	"media_preferences_advanced", "soundrecorder_application"]


def check_screenshots(results_dir, report, names, check):
	"""Every screenshot was saved and shows something: not one flat
	colour (a screen that never came up, or a failed screendump)."""
	missing = []
	flat = []
	for name in names:
		path = os.path.join(results_dir, "screen-%s.png" % name)
		if not os.path.exists(path):
			path = path[:-4] + ".ppm"
		if not os.path.exists(path):
			missing.append(name)
			continue
		try:
			from PIL import Image
			colours = Image.open(path).convert("RGB").getcolors(16)
			if colours is not None and len(colours) < 4:
				flat.append(name)
		except ImportError:
			pass
		except OSError:
			flat.append(name)
	report.setdefault("host_checks", []).append({"name": check,
		"pass": not missing and not flat,
		"detail": "%d of %d saved%s%s" % (len(names) - len(missing),
			len(names), "; missing: " + ", ".join(missing) if missing else "",
			"; blank: " + ", ".join(flat) if flat else "")})


# Screenshot hw_cursor_test asks for (F-59): a 300x300 drag bitmap over the
# 256x256 limit of app_server's stand-in hardware cursor, which the software
# cursor shows instead.
HW_CURSOR_SCREENSHOTS = ["hw_cursor_software_drag", "input_drag_setting"]


def check_hw_cursor_drag(results_dir, report):
	"""hw_cursor.software_drag_drawn: when the hardware cursor cannot take a
	drag image, the software cursor shows it. The test drags an opaque red
	300x300 bitmap; the screenshot must have a red area at least 250 by 250
	pixels (rows of 250 or more red pixels in 250 or more rows), which no
	window of the desktop has."""
	path = os.path.join(results_dir, "screen-hw_cursor_software_drag.png")
	if not os.path.exists(path):
		path = path[:-4] + ".ppm"
	check = {"name": "hw_cursor.software_drag_drawn", "pass": False}
	try:
		from PIL import Image
		picture = Image.open(path).convert("RGB")
		width, height = picture.size
		pixels = picture.load()
		rows = 0
		for y in range(height):
			run = longest = 0
			for x in range(width):
				r, g, b = pixels[x, y]
				if r >= 200 and g <= 60 and b <= 60:
					run += 1
					longest = max(longest, run)
				else:
					run = 0
			if longest >= 250:
				rows += 1
		check["pass"] = rows >= 250
		check["detail"] = "%d rows with 250 or more red pixels in a row " \
			"(want 250 or more)" % rows
	except ImportError:
		check["pass"] = True
		check["detail"] = "not checked (no Pillow)"
	except OSError as error:
		check["detail"] = "no screenshot: %s" % error
	report.setdefault("host_checks", []).append(check)


# #pragma mark - report


def write_report(results_dir, report):
	checks = list(report.get("tests", []))
	checks += report.get("audio", {}).get("checks", [])
	checks += report.get("media_audio", {}).get("checks", [])
	checks += report.get("usb_audio_out", {}).get("checks", [])
	checks += report.get("media_usb_audio", {}).get("checks", [])
	checks += report.get("sound_audio", {}).get("checks", [])
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
	# A run with --only is never a pass: it did not run everything, so it
	# cannot stand in for a full run (default, --smp 2, UEFI) before a
	# commit. Its verdict says PARTIAL; ce-test.py exits with 2.
	partial = bool(report.get("selected_tests"))
	report["verdict"]["partial"] = partial
	if partial:
		report["verdict"]["selection_passed"] = report["verdict"]["pass"]
		report["verdict"]["pass"] = False

	with open(os.path.join(results_dir, "results.json"), "w") as jsonFile:
		json.dump(report, jsonFile, indent=2, default=str)

	lines = ["# Renga test run %s" % report["started"], ""]
	verdict = report["verdict"]
	if verdict["partial"]:
		lines.append("**Verdict: PARTIAL, %s** (%d checks, %d failed). Only "
			"%s ran (--only); this run does not count as a test pass." % (
			"selection passed" if verdict["selection_passed"]
				else "selection FAILED", verdict["checks"], verdict["failed"],
			", ".join(report["selected_tests"])))
	else:
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
	if report.get("other_systems"):
		lines[-1] += (" Other systems' disk, a USB drive, an empty CD "
			"drive%s were attached and started from the boot menu." % (
			" and a floppy" if report.get("firmware") == "bios" else ""))
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
		load = boot.get("host_load")
		if load:
			lines.append("- host load while the guest ran: highest %s, mean "
				"%s (one-minute load average, %s host CPUs%s)" % (load["max"],
				load["mean"], load["cpus"],
				"; busier than the guest needs, timing checks may suffer"
				if load["cpus"] and load["max"] > load["cpus"] + 0.5 else ""))
		if boot.get("screenshots"):
			lines.append("- screenshots: " + ", ".join(boot["screenshots"]))
		if boot.get("kdl_log"):
			lines.append("- full kernel debugger session: %s"
				% boot["kdl_log"])
		lines.append("")

	timing = report.get("timing")
	if timing and timing.get("tests"):
		lines.append("## Timing")
		lines.append("- boot to runner start: %ss; runner: %ss" % (
			timing.get("boot_to_runner"), timing.get("runner_seconds")))
		lines.append("")
		lines.append("| Test program | Seconds | Exit status |")
		lines.append("| --- | --- | --- |")
		for entry in sorted(timing["tests"],
				key=lambda e: -e.get("seconds", 1e9)):
			lines.append("| %s | %s | %s |" % (entry["test"],
				entry.get("seconds", "did not finish"),
				entry.get("status", "-")))
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
	parser.add_argument("--other-systems", action="store_true",
		help="attach a disk with other systems' loaders and boot records, "
			"a slow USB drive with a removable medium's EFI loader, an empty "
			"CD drive and (BIOS) a slow bootable floppy; show the boot menu "
			"(3 s timeout; on UEFI with Windows Boot Manager as the default), "
			"start each system from it with the keyboard and check what "
			"they report on the serial port (see README)")
	parser.add_argument("--installer-upgrade", action="store_true",
		help="build a %d MB image, attach two copies of it made to look like "
			"an older Haiku installation (older system package, a user "
			"package, an older state), let installer_test upgrade, replace "
			"and erase them with the Installer's command line, then boot the "
			"upgraded copy and, through the boot menu, its old package state "
			"(hard disk, BIOS or UEFI; see README)" % INSTALLER_IMAGE_SIZE)
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
	parser.add_argument("--only", default="", metavar="TESTS",
		help="quick check while developing: boot a copy of the image that "
			"runs only these test programs (comma-separated, e.g. "
			"epoll,signalfd); host-side checks of the other programs are "
			"skipped. The verdict is PARTIAL and the exit status 2, never a "
			"pass: commits still need full runs")
	parser.add_argument("--list-tests", action="store_true",
		help="list the test programs harness images carry and exit")
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
	if args.profile is None:
		args.profile = "minimum-anyboot" \
			if args.firmware == "uefi" or args.media == "cdrom" \
			else "minimum-raw"
	available = installed_tests(args.profile,
		final=not (args.other_systems or args.installer_upgrade))
	if args.list_tests:
		print("\n".join(available))
		return 0
	try:
		args.only = selected_tests(args.only, available) if args.only \
			else []
	except ValueError as error:
		parser.error(str(error))
	if args.two_installs and (args.media == "cdrom"
			or args.firmware != "uefi"):
		parser.error("--two-installs tests the EFI boot loader's choice "
			"between hard disks; it needs --firmware uefi and --media disk")
	if args.installer_upgrade and (args.media != "disk"
			or args.disk_bus != "ide" or args.other_systems
			or args.two_installs):
		parser.error("--installer-upgrade boots from an IDE hard disk")
	if args.other_systems and (args.media != "disk" or args.disk_bus != "ide"
			or args.two_installs or args.boot_menu_timeout):
		parser.error("--other-systems boots from an IDE hard disk and sets "
			"the boot menu timeout itself")
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
		"other_systems": args.other_systems,
		"installer_upgrade": args.installer_upgrade,
		"boot_menu_timeout": args.boot_menu_timeout,
		"pointer_devices": args.pointer_devices,
		"usb_controller": args.usb_controller,
		"compile_only": args.compile_only,
		"installed_tests": available,
		"selected_tests": args.only}

	ok = True
	if not args.no_build:
		ok = build(args, results_dir, report)
	if ok and not args.compile_only:
		disks = None
		if args.installer_upgrade:
			disks = make_installer_disks(args, results_dir, report)
		extra = installer_drives(disks) if disks else []
		if args.profile.startswith("renga-") and not disks \
				and not args.other_systems \
				and "installer_ui_test" in (args.only or available):
			target = make_installer_ui_target(args, results_dir)
			if target is not None:
				extra += ["-drive", "file=%s,format=raw,if=none,id=cetarget"
					% target, "-device", "ide-hd,drive=cetarget,bus=ide.1,"
					"unit=0"]
		boot(args, results_dir, report, extra_drives=extra)
		if disks:
			run_installer_boots(args, results_dir, report, disks)
		parse_serial(results_dir, report)
		parse_qemu_log(results_dir, report)
		check_scratch_disks(results_dir, report)
		check_pointer_devices(args, results_dir, report)
		check_nvme_namespaces(results_dir, report)
		check_image_identity(args, results_dir, report)
		check_branding(args, results_dir, report)
		check_two_installs(args, results_dir, report)
		check_installer_boots(args, results_dir, report)
		if "linuxkpi_test" in (args.only or available):
			check_linuxkpi(results_dir, report)
		if "crashlog_test" in (args.only or available) \
				and not args.installer_upgrade:
			check_crashlog(results_dir, report)
			check_kdl_hint(results_dir, report)
			if args.profile.startswith("renga-"):
				check_screenshots(results_dir, report, CRASHLOG_SCREENSHOTS,
					"crashlog.screenshots")
		if args.other_systems:
			check_other_systems(args, results_dir, report)
		else:
			check_boot_menu(args, results_dir, report)
		if not args.only or "vsnd_test" in args.only:
			analyze_audio(results_dir, report, VIRTIO_SOUND_PHASES)
		if "usb_audio_test" in (args.only or available):
			analyze_usb_audio(results_dir, report)
		if "media_test" in (args.only or available):
			analyze_media_audio(results_dir, report)
		if "media_routing_test" in (args.only or available):
			analyze_routing_audio(results_dir, report)
			analyze_loopback_only_audio(results_dir, report)
			check_screenshots(results_dir, report, ROUTING_SCREENSHOTS,
				"media_routing.screenshots")
		if "media_sound_test" in (args.only or available):
			analyze_sound_audio(results_dir, report)
			check_screenshots(results_dir, report, SOUND_SCREENSHOTS,
				"sound_settings.screenshots")
		if "media_ui_test" in (args.only or available):
			check_screenshots(results_dir, report, MEDIA_UI_SCREENSHOTS,
				"media_ui.screenshots")
		if "media_usb_test" in (args.only or available):
			analyze_media_usb_audio(results_dir, report)
			check_screenshots(results_dir, report, MEDIA_USB_SCREENSHOTS,
				"media_usb.screenshots")
		if "media_devnotify_test" in (args.only or available):
			check_screenshots(results_dir, report, DEVNOTIFY_SCREENSHOTS,
				"devnotify.screenshots")
		if "tracker_menu_test" in (args.only or available):
			check_screenshots(results_dir, report, TRACKER_MENU_SCREENSHOTS,
				"tracker_menu.screenshots")
		if "media_vcam_test" in (args.only or available):
			check_screenshots(results_dir, report, VCAM_SCREENSHOTS,
				"virtual_camera.screenshots")
		if "media_video_test" in (args.only or available):
			check_screenshots(results_dir, report, CODYCAM_SCREENSHOTS,
				"codycam.screenshots")
		if "screen_capture_test" in (args.only or available):
			check_screenshots(results_dir, report, CAPTURE_SCREENSHOTS,
				"capture.screenshots")
		if "sound_latency_test" in (args.only or available):
			analyze_latency_audio(results_dir, report)
			check_screenshots(results_dir, report, LATENCY_SCREENSHOTS,
				"media_latency.screenshots")
		if "media_stream_test" in (args.only or available):
			check_screenshots(results_dir, report, STREAM_SCREENSHOTS,
				"media_stream.screenshots")
		if "media_html5_test" in (args.only or available):
			check_screenshots(results_dir, report, HTML5_SCREENSHOTS,
				"html5_media.screenshots")
		if "installer_ui_test" in (args.only or available):
			check_screenshots(results_dir, report, INSTALLER_SCREENSHOTS,
				"installer_ui.screenshots")
		if "bootmenu_ui_test" in (args.only or available):
			check_screenshots(results_dir, report, BOOTMENU_SCREENSHOTS,
				"bootmenu.screenshots")
		if "hw_cursor_test" in (args.only or available) \
				and "usb" in args.pointer_devices:
			check_screenshots(results_dir, report, HW_CURSOR_SCREENSHOTS,
				"hw_cursor.screenshots")
			check_hw_cursor_drag(results_dir, report)
		if "linuxkpi_ui_test" in (args.only or available):
			check_screenshots(results_dir, report, LINUXKPI_SCREENSHOTS,
				"linuxkpi_ui.screenshots")
		if "wayland_test" in (args.only or available):
			check_screenshots(results_dir, report, WAYLAND_SCREENSHOTS
				+ WAYLAND_LINUX_SCREENSHOTS, "wayland.screenshots")

	summary, passed = write_report(results_dir, report)
	print(summary)
	log("results in " + results_dir)
	if report["verdict"].get("partial"):
		return 2 if report["verdict"]["selection_passed"] else 1
	return 0 if passed else 1


if __name__ == "__main__":
	sys.exit(main())
