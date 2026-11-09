#!/usr/bin/env python3
"""Builds the Renga wordmark images AboutSystem and Installer show, from the
boot splash logo.

Usage:
  make_app_logos_renga.py <haiku top>

Official Haiku builds show the HAIKU logo in AboutSystem and the Installer;
that logo is a Haiku, Inc. trademark, reserved for the official
distribution. Renga shows its own wordmark instead, the one the boot splash
ends on: data/artwork/boot_splash/splash_logo-renga.png (made by
make_splash_renga.py, leaves drawn from scratch, Lato Light under the SIL
Open Font License).

The splash logo is opaque, drawn on black. The apps draw their logo over the
document background, light or dark depending on the user's colours, and pick
"logo.png" or "logo_dark.png" for it like the official build does. So for
each size this writes two transparent PNGs:

 - dark background ("-dark"): the splash logo un-premultiplied from black
   (alpha = the brightest channel; the leaves, which are opaque, are made
   opaque again), so it looks the same on any dark colour;
 - light background: the same, but the neutral parts (the letters and the
   hairline, which are light grey on the splash) in dark grey; the coloured
   leaves keep their colours.

Outputs (3rdparty/ce/artwork/):
  renga_logo_about.png, renga_logo_about-dark.png          AboutSystem
  renga_logo_installer.png, renga_logo_installer-dark.png  Installer
"""

import os
import sys

from PIL import Image


# name: width; the height follows from the logo's aspect ratio (372 x 96).
# About the size of the official logos they replace (158 x 51 and 207 x 68).
SIZES = {"about": 186, "installer": 232}
LIGHT_BACKGROUND_INK = (51, 51, 51)
# Pixels whose un-premultiplied colour spreads less than this over the
# channels are neutral (letters, hairline, their glow).
NEUTRAL_SPREAD = 60
# The leaves are opaque: their darkest shade (78, 140, 46) has 140 in its
# brightest channel. Coloured pixels at least that bright are opaque, dimmer
# ones (the anti-aliased edges) proportionally transparent.
LEAF_OPAQUE = 140
# The brightest the letters get on the splash (light grey).
LETTERS_BRIGHTEST = 224


def transparent(logo, ink=None):
    out = Image.new("RGBA", logo.size)
    pixels = []
    data = logo.get_flattened_data() \
        if hasattr(logo, "get_flattened_data") else logo.getdata()
    for r, g, b in data:
        alpha = max(r, g, b)
        if alpha == 0:
            pixels.append((0, 0, 0, 0))
            continue
        color = tuple(min(255, c * 255 // alpha) for c in (r, g, b))
        if max(color) - min(color) < NEUTRAL_SPREAD:
            if ink is not None:
                # Dark ink at the letters' brightest (224) is fully opaque.
                alpha = min(255, alpha * 255 // LETTERS_BRIGHTEST)
                color = ink
        else:
            alpha = min(255, alpha * 255 // LEAF_OPAQUE)
            color = tuple(min(255, c * 255 // alpha) for c in (r, g, b))
        pixels.append(color + (alpha,))
    out.putdata(pixels)
    return out


def main():
    top = sys.argv[1]
    logo = Image.open(os.path.join(top,
        "data/artwork/boot_splash/splash_logo-renga.png")).convert("RGB")
    out_dir = os.path.join(top, "3rdparty/ce/artwork")
    for name, width in SIZES.items():
        height = round(logo.height * width / logo.width)
        scaled = logo.resize((width, height), Image.LANCZOS)
        transparent(scaled, LIGHT_BACKGROUND_INK).save(
            os.path.join(out_dir, "renga_logo_%s.png" % name), optimize=True)
        transparent(scaled).save(
            os.path.join(out_dir, "renga_logo_%s-dark.png" % name),
            optimize=True)


if __name__ == "__main__":
    main()
