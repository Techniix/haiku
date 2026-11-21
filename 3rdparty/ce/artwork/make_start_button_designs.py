#!/usr/bin/env python3
"""Draws the design options for Renga's Deskbar start button (roadmap F-41,
"Start button"), for the maintainer to choose from before anything is
implemented.

Usage:
  make_start_button_designs.py <output directory>

The maintainer's brief (2026-11-11): a Renga emblem drawn as a vector icon,
leaf and light motif from the boot splash, without the wordmark, in light
and dark variants, scaling to the panel size and HiDPI, with a subtle hover
and press animation later. The leaves and their colours are the boot
splash's (make_splash_renga.py, LEAF_COLORS): green, orange and gold.

For each option this writes:
  <name>-light.svg, <name>-dark.svg   the emblem for light and dark panels
                                       (Icon-O-Matic imports SVG; the HVIF
                                       is made from the chosen one)
  <name>-light-<size>.png, <name>-dark-<size>.png
                                       renderings at 16, 32 and 128 px
and one overview, start-button-options.png: every option on a light and a
dark Deskbar strip at the sizes the Deskbar uses (16 to 32 px, also 2x) and
large.

Needs rsvg-convert (librsvg) and Pillow.
"""

import math
import os
import shutil
import subprocess
import sys
import tempfile

from PIL import Image, ImageDraw, ImageFont


GREEN = ("#7ebe4a", "#4e8c2e")
ORANGE = ("#f08028", "#c45416")
GOLD = ("#f6c440", "#ce961e")
LIGHT = "#fff3c4"

SIZES = (16, 32, 128)
	# written next to the SVGs; the overview also uses 20, 24 and 48


def leaf(gid, x, y, length, width, angle, colours, outline, rib=True):
	"""A leaf from (x, y) pointing at angle (degrees, 0 = up), with a
	gradient from its light to its dark colour and a midrib."""
	light, dark = colours
	w = width / 2.0
	path = ("M0,0 C%.2f,%.2f %.2f,%.2f 0,%.2f C%.2f,%.2f %.2f,%.2f 0,0 Z"
		% (w * 1.25, -length * 0.25, w * 0.9, -length * 0.8, -length,
			-w * 0.9, -length * 0.8, -w * 1.25, -length * 0.25))
	out = ['<linearGradient id="%s" x1="0" y1="1" x2="0.4" y2="0" '
		'gradientUnits="objectBoundingBox">'
		'<stop offset="0" stop-color="%s"/><stop offset="1" '
		'stop-color="%s"/></linearGradient>' % (gid, dark, light)]
	out.append('<g transform="translate(%.2f %.2f) rotate(%.2f)">'
		% (x, y, angle))
	stroke = (' stroke="%s" stroke-opacity="%.2f" stroke-width="%.2f"'
		% outline) if outline else ''
	out.append('<path d="%s" fill="url(#%s)"%s/>' % (path, gid, stroke))
	if rib:
		out.append('<path d="M0,%.2f L0,%.2f" stroke="%s" '
			'stroke-opacity="0.55" stroke-width="%.2f" '
			'stroke-linecap="round"/>' % (-length * 0.08, -length * 0.82,
				light, max(0.8, width * 0.06)))
	out.append('</g>')
	return out


def svg(body, defs=""):
	return ('<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 64 64" '
		'width="64" height="64"><defs>%s</defs>%s</svg>\n' % (defs,
			"".join(body)))


def outline_for(variant):
	# light panels: a thin dark edge keeps the leaves apart from the grey;
	# dark panels: a faint light edge
	return ("#24301c", 0.45, 1.1) if variant == "light" else \
		("#fff6d8", 0.30, 0.9)


def option_trio(variant):
	"""A: the splash's three leaves, fanned from one stem."""
	o = outline_for(variant)
	body = []
	body += leaf("a1", 32, 56, 34, 15, -38, ORANGE, o)
	body += leaf("a3", 32, 56, 34, 15, 38, GOLD, o)
	body += leaf("a2", 32, 57, 44, 18, 0, GREEN, o)
	return svg(body)


def option_sunleaf(variant):
	"""B: one green leaf crossing a disc of light."""
	o = outline_for(variant)
	glow_inner = LIGHT if variant == "dark" else "#ffe58a"
	defs = ('<radialGradient id="sun" cx="0.5" cy="0.5" r="0.5">'
		'<stop offset="0" stop-color="%s"/><stop offset="0.55" '
		'stop-color="%s"/><stop offset="1" stop-color="%s" '
		'stop-opacity="0"/></radialGradient>' % (glow_inner, GOLD[0],
			GOLD[1]))
	body = ['<circle cx="38" cy="25" r="21" fill="url(#sun)"/>']
	body += leaf("b1", 12, 56, 50, 21, 42, GREEN, o)
	return svg(body, defs)


def option_pinwheel(variant):
	"""C: three leaves turning around a point of light."""
	o = outline_for(variant)
	body = []
	for i, (angle, colours) in enumerate(((0, GREEN), (120, ORANGE),
			(240, GOLD))):
		a = math.radians(angle)
		x = 32 + 4 * math.sin(a)
		y = 33 - 4 * math.cos(a)
		body += leaf("c%d" % i, x, y, 27, 15, angle + 18, colours, o)
	defs = ('<radialGradient id="spark" cx="0.5" cy="0.5" r="0.5">'
		'<stop offset="0" stop-color="#ffffff"/><stop offset="0.5" '
		'stop-color="%s"/><stop offset="1" stop-color="%s" '
		'stop-opacity="0"/></radialGradient>' % (LIGHT, GOLD[0]))
	body.append('<circle cx="32" cy="33" r="7" fill="url(#spark)"/>')
	return svg(body, defs)


def option_badge(variant):
	"""D: a rounded badge with an upright leaf whose midrib runs on as a ray
	of light."""
	if variant == "light":
		fill, edge = "#f4f0e2", "#b9b096"
	else:
		fill, edge = "#22302a", "#4b6352"
	defs = ('<linearGradient id="ray" x1="0" y1="1" x2="0" y2="0">'
		'<stop offset="0" stop-color="%s" stop-opacity="0.9"/>'
		'<stop offset="1" stop-color="%s" stop-opacity="0"/>'
		'</linearGradient>' % (GOLD[0], LIGHT))
	body = ['<rect x="4" y="4" width="56" height="56" rx="15" fill="%s" '
		'stroke="%s" stroke-width="1.5"/>' % (fill, edge)]
	body += leaf("d1", 32, 53, 38, 22, 8, GREEN, outline_for(variant),
		rib=False)
	# along the leaf's midrib (8 degrees) and on past its tip
	body.append('<path d="M32.4,50 L38.6,7.5" stroke="url(#ray)" '
		'stroke-width="2.4" stroke-linecap="round"/>')
	body.append('<circle cx="38.8" cy="7.2" r="2.6" fill="%s"/>' % LIGHT)
	return svg(body, defs)


OPTIONS = [
	("A-three-leaves", "A  Three leaves", option_trio),
	("B-leaf-and-sun", "B  Leaf and sun", option_sunleaf),
	("C-leaf-pinwheel", "C  Leaf pinwheel", option_pinwheel),
	("D-badge-ray", "D  Badge with a ray of light", option_badge),
]


def render(svg_path, size, png_path):
	subprocess.check_call(["rsvg-convert", "-w", str(size), "-h", str(size),
		"-o", png_path, svg_path])


def main():
	if len(sys.argv) != 2:
		sys.exit(__doc__)
	out = sys.argv[1]
	os.makedirs(out, exist_ok=True)
	scratch = tempfile.mkdtemp(prefix="start-button-")

	renders = {}
	for name, title, make in OPTIONS:
		for variant in ("light", "dark"):
			svg_path = os.path.join(out, "%s-%s.svg" % (name, variant))
			with open(svg_path, "w") as f:
				f.write(make(variant))
			for size in SIZES + (20, 24, 48):
				folder = out if size in SIZES else scratch
				png = os.path.join(folder, "%s-%s-%d.png" % (name, variant,
					size))
				render(svg_path, size, png)
				renders[(name, variant, size)] = png

	# The overview: per option a row with a light and a dark Deskbar strip
	# (the emblem at 16, 20, 24, 32 px and at 2x), and the 128 px emblems.
	try:
		font = ImageFont.truetype(
			"/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf", 15)
		small = ImageFont.truetype(
			"/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf", 11)
	except OSError:
		font = small = ImageFont.load_default()
	row_h = 168
	width = 1180
	sheet = Image.new("RGB", (width, 40 + row_h * len(OPTIONS)), "#ececec")
	d = ImageDraw.Draw(sheet)
	d.text((16, 12), "Renga start button: design options (Deskbar, light "
		"and dark panels)", fill="#202020", font=font)
	for row, (name, title, make) in enumerate(OPTIONS):
		top = 40 + row * row_h
		d.text((16, top + 6), title, fill="#202020", font=font)
		for column, (variant, panel, text) in enumerate((("light",
				"#d8d8d8", "#202020"), ("dark", "#2b2d30", "#e8e8e8"))):
			left = 16 + column * 330
			strip_top = top + 34
			d.rectangle((left, strip_top, left + 310, strip_top + 110),
				fill=panel)
			x = left + 10
			for size in (16, 20, 24, 32, 48):
				icon = Image.open(renders[(name, variant, size)]).convert(
					"RGBA")
				sheet.paste(icon, (x, strip_top + 40 - size // 2), icon)
				d.text((x, strip_top + 78), "%d" % size, fill=text,
					font=small)
				x += size + 14
			d.text((left + 10, strip_top + 94), "%s panel" % variant,
				fill=text, font=small)
		for column, (variant, panel) in enumerate((("light", "#d8d8d8"),
				("dark", "#2b2d30"))):
			left = 690 + column * 240
			d.rectangle((left, top + 4, left + 224, top + 160), fill=panel)
			icon = Image.open(renders[(name, variant, 128)]).convert("RGBA")
			sheet.paste(icon, (left + 48, top + 18), icon)
	sheet.save(os.path.join(out, "start-button-options.png"))
	shutil.rmtree(scratch)


if __name__ == "__main__":
	main()
