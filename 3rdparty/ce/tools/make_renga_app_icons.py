#!/usr/bin/env python3
#
# Copyright 2026, Renga contributors.
# Distributed under the terms of the MIT License.
#
"""Writes the HVIF icons of Renga programs that had none (batch 4 icon
pass), in the style of the other Renga icons (make_session_bus_icon.py):
Haiku's three-quarter view, gradients, dark outlines, no mascots or
trademarks.

    3rdparty/ce/tools/make_renga_app_icons.py          # writes every rdef
    3rdparty/ce/tools/make_renga_app_icons.py --preview sheet.png

Icons and where they go:
    SystemLogs           src/apps/systemlogs/SystemLogsIcon.rdef
                         a log page with lines and a magnifier
    CrashReportNotifier  src/apps/systemlogs/CrashReportNotifierIcon.rdef
                         a log page with a red warning triangle
    BootMenu             src/preferences/bootmenu/BootMenuIcon.rdef
                         a screen with a menu list and a selection bar
    ScreenRecording      src/preferences/screenrecording/ScreenRecordingIcon.rdef
                         a screen with a red record dot
    WaylandCompositor    src/servers/wayland/WaylandCompositorIcon.rdef
                         two overlapping windows, the front one green
    ControlPanel         src/apps/controlpanel/ControlPanelIcon.rdef
                         (batch 5) a window with a sidebar and two
                         switches, one on (green), one off
    RengaDoctor          src/apps/rengadoctor/RengaDoctorIcon.rdef
                         (batch 5) a report page with a green cross
    Welcome              src/apps/welcome/WelcomeIcon.rdef
                         (batch 5) a screen with a gold star
    RenguinManager       src/apps/renguinmanager/RenguinManagerIcon.rdef
                         (batch 5) a terminal window with a green prompt
                         and a gold gear
"""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import hvif

TOP = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..",
	".."))

GREEN = ((126, 190, 74, 255), (78, 140, 46, 255))
PAPER = ((250, 248, 240, 255), (196, 192, 180, 255))
GREY = ((180, 186, 196, 255), (110, 118, 130, 255))
SCREEN = ((90, 140, 210, 255), (40, 78, 150, 255))
RED = ((240, 70, 60, 255), (170, 30, 24, 255))
GOLD = ((246, 196, 64, 255), (206, 150, 30, 255))
LINE = ((120, 124, 130, 255), (120, 124, 130, 255))
OUTLINE = (30, 32, 26, 255)


def skew(points):
	return [(x + (40 - y) * 0.12, y) for x, y in points]


def fill(colors, p0=(0, 0), p1=(64, 64)):
	return hvif.gradient(p0, p1, [(0, colors[0]), (255, colors[1])])


def rect(x0, y0, x1, y1, r=3):
	if r == 0:
		return [(x0, y0), (x1, y0), (x1, y1), (x0, y1)]
	points = []
	points += hvif.arc_points(x1 - r, y0 + r, r, -90, 0, 4)
	points += hvif.arc_points(x1 - r, y1 - r, r, 0, 90, 4)
	points += hvif.arc_points(x0 + r, y1 - r, r, 90, 180, 4)
	points += hvif.arc_points(x0 + r, y0 + r, r, 180, 270, 4)
	return points


def page(icon):
	icon.polygon(skew(rect(10, 4, 48, 58, 3)), fill(PAPER, (10, 4),
		(48, 58)), outline=(OUTLINE, 2))
	for y in (14, 22, 30, 38, 46):
		icon.polygon(skew(rect(16, y, 42 if y % 16 else 36, y + 2, 0)),
			fill(LINE))


def screen(icon):
	icon.polygon(skew(rect(4, 8, 60, 46, 3)), fill(GREY, (4, 8), (60, 46)),
		outline=(OUTLINE, 2))
	icon.polygon(skew(rect(8, 12, 56, 42, 1)), fill(SCREEN, (8, 12),
		(56, 42)))
	icon.polygon(skew([(26, 46), (38, 46), (40, 54), (24, 54)]),
		fill(GREY), outline=(OUTLINE, 2))
	icon.polygon(skew(rect(16, 54, 48, 58, 2)), fill(GREY),
		outline=(OUTLINE, 2))


def system_logs():
	icon = hvif.Icon()
	page(icon)
	icon.polygon(skew(hvif.circle(44, 42, 10)), fill(GREEN, (34, 32),
		(54, 52)), outline=(OUTLINE, 2))
	icon.polygon(skew(hvif.circle(44, 42, 6)), fill(PAPER))
	icon.polygon(skew([(50, 49), (54, 46), (62, 56), (58, 60)]),
		fill(GOLD), outline=(OUTLINE, 2))
	return icon


def crash_report():
	icon = hvif.Icon()
	page(icon)
	icon.polygon(skew([(44, 30), (60, 58), (28, 58)]), fill(RED, (28, 30),
		(60, 58)), outline=(OUTLINE, 2))
	icon.polygon(skew(rect(42.5, 38, 45.5, 50, 1)), fill(PAPER))
	icon.polygon(skew(hvif.circle(44, 54, 1.8)), fill(PAPER))
	return icon


def boot_menu():
	icon = hvif.Icon()
	screen(icon)
	for y in (17, 25, 33):
		icon.polygon(skew(rect(14, y, 50, y + 5, 1)), fill(
			GREEN if y == 25 else ((150, 180, 226, 255), (120, 150, 200,
				255))))
	return icon


def screen_recording():
	icon = hvif.Icon()
	screen(icon)
	icon.polygon(skew(hvif.circle(32, 27, 9)), fill(RED, (23, 18), (41, 36)),
		outline=(OUTLINE, 2))
	return icon


def wayland_compositor():
	icon = hvif.Icon()
	icon.polygon(skew(rect(4, 6, 42, 38, 3)), fill(GREY, (4, 6), (42, 38)),
		outline=(OUTLINE, 2))
	icon.polygon(skew(rect(4, 6, 42, 13, 3)), fill(GOLD))
	icon.polygon(skew(rect(20, 24, 60, 58, 3)), fill(PAPER, (20, 24),
		(60, 58)), outline=(OUTLINE, 2))
	icon.polygon(skew(rect(20, 24, 60, 31, 3)), fill(GREEN))
	return icon


def control_panel():
	icon = hvif.Icon()
	icon.polygon(skew(rect(4, 6, 60, 58, 3)), fill(PAPER, (4, 6), (60, 58)),
		outline=(OUTLINE, 2))
	icon.polygon(skew(rect(4, 6, 60, 14, 3)), fill(GOLD))
	icon.polygon(skew(rect(8, 18, 22, 54, 1)), fill(SCREEN, (8, 18),
		(22, 54)))
	for y in (22, 30, 38):
		icon.polygon(skew(rect(10, y, 20, y + 3, 0)), fill(PAPER))
	# two switches: on (green, knob right), off (grey, knob left)
	icon.polygon(skew(rect(28, 22, 54, 32, 5)), fill(GREEN),
		outline=(OUTLINE, 2))
	icon.polygon(skew(hvif.circle(48, 27, 4)), fill(PAPER),
		outline=(OUTLINE, 1))
	icon.polygon(skew(rect(28, 40, 54, 50, 5)), fill(GREY),
		outline=(OUTLINE, 2))
	icon.polygon(skew(hvif.circle(34, 45, 4)), fill(PAPER),
		outline=(OUTLINE, 1))
	return icon


def renga_doctor():
	icon = hvif.Icon()
	page(icon)
	icon.polygon(skew(hvif.circle(42, 44, 13)), fill(GREEN, (29, 31),
		(55, 57)), outline=(OUTLINE, 2))
	icon.polygon(skew(rect(39, 36, 45, 52, 1)), fill(PAPER))
	icon.polygon(skew(rect(34, 41, 50, 47, 1)), fill(PAPER))
	return icon


def welcome():
	icon = hvif.Icon()
	screen(icon)
	import math
	star = []
	for i in range(10):
		radius = 13 if i % 2 == 0 else 5.5
		angle = math.radians(-90 + i * 36)
		star.append((32 + radius * math.cos(angle), 27 + radius
			* math.sin(angle)))
	icon.polygon(skew(star), fill(GOLD, (19, 14), (45, 40)),
		outline=(OUTLINE, 2))
	return icon


def renguin_manager():
	import math
	icon = hvif.Icon()
	icon.polygon(skew(rect(4, 8, 52, 50, 3)), fill(((70, 74, 82, 255),
		(30, 32, 38, 255)), (4, 8), (52, 50)), outline=(OUTLINE, 2))
	icon.polygon(skew(rect(4, 8, 52, 15, 3)), fill(GREY))
	icon.polygon(skew([(10, 22), (18, 27), (10, 32), (10, 29), (14, 27),
		(10, 25)]), fill(GREEN))
	icon.polygon(skew(rect(20, 31, 30, 33, 0)), fill(GREEN))
	teeth = []
	for i in range(16):
		radius = 12 if i % 2 == 0 else 9
		angle = math.radians(i * 22.5)
		teeth.append((46 + radius * math.cos(angle), 46 + radius
			* math.sin(angle)))
	icon.polygon(skew(teeth), fill(GOLD, (34, 34), (58, 58)),
		outline=(OUTLINE, 2))
	icon.polygon(skew(hvif.circle(46, 46, 4)), fill(PAPER),
		outline=(OUTLINE, 1))
	return icon


ICONS = [
	("SystemLogs", system_logs,
		"src/apps/systemlogs/SystemLogsIcon.rdef"),
	("CrashReportNotifier", crash_report,
		"src/apps/systemlogs/CrashReportNotifierIcon.rdef"),
	("BootMenu", boot_menu, "src/preferences/bootmenu/BootMenuIcon.rdef"),
	("ScreenRecording", screen_recording,
		"src/preferences/screenrecording/ScreenRecordingIcon.rdef"),
	("WaylandCompositor", wayland_compositor,
		"src/servers/wayland/WaylandCompositorIcon.rdef"),
	("ControlPanel", control_panel,
		"src/apps/controlpanel/ControlPanelIcon.rdef"),
	("RengaDoctor", renga_doctor,
		"src/apps/rengadoctor/RengaDoctorIcon.rdef"),
	("Welcome", welcome, "src/apps/welcome/WelcomeIcon.rdef"),
	("RenguinManager", renguin_manager,
		"src/apps/renguinmanager/RenguinManagerIcon.rdef"),
]


def rdef(name, icon):
	hexdata = icon.flat().hex().upper()
	lines = ["/*", " * Copyright 2026, Renga contributors.",
		" * Distributed under the terms of the MIT License.", " *",
		" * %s's icon. Generated by" % name,
		" * 3rdparty/ce/tools/make_renga_app_icons.py; see there.",
		" */", "", "resource vector_icon {"]
	for i in range(0, len(hexdata), 64):
		lines.append("\t$\"%s\"" % hexdata[i:i + 64])
	lines.append("};")
	return "\n".join(lines) + "\n"


if __name__ == "__main__":
	if len(sys.argv) == 3 and sys.argv[1] == "--preview":
		from PIL import Image
		sheet = Image.new("RGBA", (len(ICONS) * 72 + 8, 72 + 40),
			(216, 216, 216, 255))
		for i, (name, build, path) in enumerate(ICONS):
			icon = build()
			sheet.paste(icon.preview(64, (216, 216, 216, 255)),
				(8 + i * 72, 4))
			sheet.paste(icon.preview(16, (216, 216, 216, 255)),
				(8 + i * 72, 76))
			sheet.paste(icon.preview(32, (216, 216, 216, 255)),
				(30 + i * 72, 76))
		sheet.save(sys.argv[2])
	else:
		for name, build, path in ICONS:
			with open(os.path.join(TOP, path), "w") as output:
				output.write(rdef(name, build()))
			print(path)
