#!/usr/bin/env python3
#
# Copyright 2026, Renga contributors.
# Distributed under the terms of the MIT License.
#
"""Writes the Renga start button, the Deskbar's menu button (roadmap F-41;
maintainer's choice 2026-11-26: option C, "leaf pinwheel", reworked to match
the boot animation), as HVIF icons:

    3rdparty/ce/tools/make_start_button_icon.py --rdef \\
        > src/apps/deskbar/start_button.rdef
    3rdparty/ce/tools/make_start_button_icon.py --preview <directory>

Design. The three leaves are the boot splash's (make_splash_renga.py,
boot_splash_render.cpp): its colours (green, orange, gold; a dark leaf
under a lighter upper fold, no outline and no midrib), its leaf profile
(half width 0.23 x length x sin(pi s)^0.85) and its curl (a bend of the
midrib growing with the square of the distance from the middle). They turn
around a soft point of warm light, the splash's sunfleck, as a pinwheel.
Option C's outlined, gradient-filled leaves are gone; the shape stays.

Four icons, all square (the Deskbar draws them at the panel's height, so
they scale with the font size and HiDPI):

  light, dark          for light and dark menu colours: on a light panel a
                       faint dark rim keeps the gold leaf apart from the
                       grey; on a dark panel the light is a little brighter
  light_lit, dark_lit  the same with the light grown and brighter: the
                       subtle hover and press state

The resource IDs are the ones of src/apps/deskbar/icons.h.
"""

import math
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import hvif

# make_splash_renga.py LEAF_COLORS: (light fold, dark body)
LEAVES = [((126, 190, 74), (78, 140, 46)),
	((240, 128, 40), (196, 84, 22)),
	((246, 196, 64), (206, 150, 30))]
WARM = (255, 243, 196)		# the splash's light (LIGHT in the designs)
GOLD = (246, 196, 64)

RESOURCES = [("light", 30), ("dark", 31), ("light_lit", 32), ("dark_lit", 33)]
CENTRE = (35.4, 36.4)
	# the pinwheel is not symmetric about its light: this centres its
	# leaves in the 64 x 64 grid


def leaf_outline(cx, cy, length, angle, curl, scale=1.0, shift=0.0,
		steps=14):
	"""The splash leaf as a polygon: centre, length, angle (degrees),
	curl; scale and shift (across, in units of the half width) give the
	lighter upper fold."""
	c = math.cos(math.radians(angle))
	s = math.sin(math.radians(angle))
	halfWidth = length * 0.23
	upper = []
	lower = []
	for i in range(steps + 1):
		t = i / float(steps)
		u = (t - 0.5) * length
		half = halfWidth * scale * math.sin(math.pi * t) ** 0.85
		middle = curl * length * (t - 0.5) ** 2 - shift * halfWidth
		upper.append((u, middle - half))
		lower.append((u, middle + half))
	points = upper + lower[::-1][1:-1]
	return [(cx + u * c - v * s, cy + u * s + v * c) for u, v in points]


def build(variant, lit):
	icon = hvif.Icon()
	dark = variant == "dark"
	rim = None if dark else ((40, 46, 30, 120), 1.6)
	length = 31.0
	for index, (light, body) in enumerate(LEAVES):
		# leaves point out from the light, turned 28 degrees for the
		# pinwheel's sense of motion (the gust of the animation)
		direction = -90 + 120 * index
		radians = math.radians(direction)
		distance = length * 0.5 + 1.5
		cx = CENTRE[0] + distance * math.cos(radians)
		cy = CENTRE[1] + distance * math.sin(radians)
		angle = direction + 28
		icon.polygon(leaf_outline(cx, cy, length, angle, 0.10),
			body + (255,), outline=rim)
		icon.polygon(leaf_outline(cx, cy, length, angle, 0.10, scale=0.8,
			shift=0.16), light + (255,))
	radius = 11.0 if lit else 8.5
	centreAlpha = 255 if lit or dark else 235
	icon.polygon(hvif.circle(CENTRE[0], CENTRE[1], radius, 28),
		hvif.radial(CENTRE, radius, [(0, (255, 255, 255, centreAlpha)),
			(110, WARM + (220 if lit else 190,)),
			(255, GOLD + (0,))]))
	return icon


def main():
	if len(sys.argv) >= 2 and sys.argv[1] == "--rdef":
		print("/* Renga start button (batch 13): written by\n"
			"   3rdparty/ce/tools/make_start_button_icon.py --rdef */\n")
		print('#include "icons.h"\n')
		for index, (name, number) in enumerate(RESOURCES):
			variant, _, lit = name.partition("_")
			data = build(variant, bool(lit)).flat()
			hexdata = data.hex().upper()
			print("resource(%d, \"start_button_%s\") vector_icon {"
				% (number, name))
			for i in range(0, len(hexdata), 64):
				print("\t$\"%s\"" % hexdata[i:i + 64])
			print("};" if index == len(RESOURCES) - 1 else "};\n")
		return
	if len(sys.argv) == 3 and sys.argv[1] == "--preview":
		from PIL import Image
		out = sys.argv[2]
		os.makedirs(out, exist_ok=True)
		strip = Image.new("RGBA", (4 * 140, 2 * 140), (0, 0, 0, 0))
		for row, (variant, background) in enumerate((("light",
				(216, 216, 216, 255)), ("dark", (43, 43, 43, 255)))):
			for column, (lit, size) in enumerate(((False, 128),
					(True, 128), (False, 22), (False, 44))):
				tile = build(variant, lit).preview(size, background)
				tile.save(os.path.join(out, "%s%s-%d.png" % (variant,
					"_lit" if lit else "", size)))
				strip.paste(tile, (column * 140 + 6, row * 140 + 6))
		strip.save(os.path.join(out, "overview.png"))
		return
	sys.exit(__doc__)


if __name__ == "__main__":
	main()
