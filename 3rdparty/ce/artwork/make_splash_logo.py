#!/usr/bin/env python3
"""Renders the Haiku Community Edition boot splash wordmarks.

Usage: make_splash_logo.py <Lato directory> [output directory]

The Lato directory must hold Lato-Light.ttf and Lato-Regular.ttf (SIL Open
Font License, https://www.latofonts.com). Writes, into the output directory
(default: data/artwork/boot_splash):

  splash_logo-ce.png          "HAIKU" | "COMMUNITY / EDITION" lockup with
                              three marks in the colours of the lit stage
                              icons (the default splash)
  splash_logo-ce-neutral.png  "COMMUNITY EDITION" only (name-neutral)

Convert each into its boot header with Haiku's own tool (build it with
jam -q '<build>generate_boot_screen'):

  T=generated.x86_64/objects/linux/x86_64/release/tools/generate_boot_screen
  $T data/artwork/boot_splash/splash_logo-ce.png 50 50 \\
     data/artwork/boot_splash/splash_icons.png 50 50 \\
     headers/private/kernel/boot/images-ce.h
  $T data/artwork/boot_splash/splash_logo-ce-neutral.png 50 50 \\
     data/artwork/boot_splash/splash_icons.png 50 50 \\
     headers/private/kernel/boot/images-ce-neutral.h

headers/private/kernel/boot/images.h selects images-ce.h for non-official
builds, or images-ce-neutral.h with HAIKU_CE_NEUTRAL_SPLASH defined.

Trademarks: both designs are original artwork; neither uses or imitates the
official HAIKU logo (no leaves, a different typeface). The default design
does use the word "Haiku". Haiku, Inc.'s distribution guidelines
(https://www.haiku-os.org/community/guidelines_creating_haiku_distribution)
ask distributions not to use "Haiku" in their name or logo without
permission, so ask Haiku, Inc. before distributing images with it, or build
with HAIKU_CE_NEUTRAL_SPLASH.

Both canvases are 372 x 96, the size of the official logo, so the layout of
the splash screen is unchanged. Everything is drawn at 4x and downsampled
for clean anti-aliasing on the black splash background.
"""

import os
import sys

from PIL import Image, ImageDraw, ImageFont

W, H, S = 372, 96, 4
GREEN, ORANGE, GOLD = (108, 178, 72), (238, 132, 44), (240, 196, 72)


def spaced_width(d, text, font, tracking):
	return sum(d.textlength(c, font=font) for c in text) \
		+ tracking * (len(text) - 1)


def draw_spaced(d, x, y, text, font, tracking, fill):
	for c in text:
		d.text((x, y), c, font=font, fill=fill)
		x += d.textlength(c, font=font) + tracking


def fade_rule(d, left, right, y, peak=150):
	"""A one-pixel rule that fades out over the outer 18% at both ends."""
	for px in range(int(left), int(right)):
		t = (px - left) / (right - left)
		v = int(peak * max(0.0, min(1.0, min(t, 1 - t) / 0.18)))
		d.line([(px, y), (px, y + S - 1)], fill=(v, v, v))


def marks(d, x, y, size, gap, radius):
	for i, colour in enumerate((GREEN, ORANGE, GOLD)):
		x0 = x + i * (size + gap) * S
		d.rounded_rectangle([x0, y, x0 + size * S, y + size * S],
			radius=radius * S, fill=colour)


def canvas():
	image = Image.new("RGB", (W * S, H * S), (0, 0, 0))
	return image, ImageDraw.Draw(image)


def haiku_lockup(light, regular):
	image, d = canvas()
	big = ImageFont.truetype(light, 44 * S)
	small = ImageFont.truetype(regular, 13 * S)
	wide = spaced_width(d, "HAIKU", big, 16 * S)
	narrow = max(spaced_width(d, "COMMUNITY", small, 3.2 * S),
		spaced_width(d, "EDITION", small, 3.2 * S))
	gap = 18 * S
	x = (W * S - (wide + 2 * gap + S + narrow)) / 2
	draw_spaced(d, x, 20 * S, "HAIKU", big, 16 * S, (245, 245, 245))
	divider = x + wide + gap
	for py in range(28 * S, 66 * S):
		d.line([(divider, py), (divider + S - 1, py)], fill=(120, 120, 120))
	x = divider + S + gap
	draw_spaced(d, x, 29 * S, "COMMUNITY", small, 3.2 * S, (215, 215, 215))
	draw_spaced(d, x, 48 * S, "EDITION", small, 3.2 * S, (215, 215, 215))
	marks(d, (W * S - (3 * 6 + 2 * 4) * S) / 2, 80 * S, 6, 4, 1.5)
	return image.resize((W, H), Image.LANCZOS)


def neutral(light):
	image, d = canvas()
	font = ImageFont.truetype(light, 25 * S)
	text = "COMMUNITY EDITION"
	tracking = 5.2 * S
	total = spaced_width(d, text, font, tracking)
	x = (W * S - total) / 2
	ascent, _ = font.getmetrics()
	draw_spaced(d, x, 36 * S - ascent // 2, text, font, tracking,
		(242, 242, 242))
	fade_rule(d, x - 12 * S, x + total + 12 * S, 62 * S)
	marks(d, (W * S - (3 * 7 + 2 * 5) * S) / 2, 72 * S, 7, 5, 2)
	return image.resize((W, H), Image.LANCZOS)


def main():
	if len(sys.argv) < 2:
		sys.exit(__doc__)
	fonts = sys.argv[1]
	output = sys.argv[2] if len(sys.argv) > 2 else "data/artwork/boot_splash"
	light = os.path.join(fonts, "Lato-Light.ttf")
	regular = os.path.join(fonts, "Lato-Regular.ttf")
	haiku_lockup(light, regular).save(
		os.path.join(output, "splash_logo-ce.png"))
	neutral(light).save(os.path.join(output, "splash_logo-ce-neutral.png"))


if __name__ == "__main__":
	main()
