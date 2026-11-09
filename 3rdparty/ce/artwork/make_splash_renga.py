#!/usr/bin/env python3
"""Builds the Renga boot splash: assets for the kernel splash player and the
static logo the boot loader draws.

Usage:
  make_splash_renga.py <Lato-Light.ttf> <haiku top> <generate_boot_screen>
      [--preview out.gif]

What it does:
 1. Renders the building blocks of the animation with Pillow and writes them
    to headers/private/kernel/boot/splash_player_assets-renga.h: the letter
    mask, a pre-blurred glow mask, a soft light blob (sunflecks), five
    pre-blurred wind wisps, the motes, the leaves' parameters, colours and
    timeline, plus integer sine and leaf-profile tables (kernel code cannot
    use floating point).
 2. Builds src/system/kernel/boot_splash_render.cpp for the host (the very
    renderer the kernel runs) and renders frame 0 with it. That frame,
    cropped to the 372 x 96 logo, becomes
    data/artwork/boot_splash/splash_logo-renga.png and, through Haiku's
    generate_boot_screen tool, headers/private/kernel/boot/images-renga.h.
    So the logo the loader draws is pixel-identical to the player's first
    frame: no jump when the kernel takes over.
 3. With --preview, renders the whole animation with the same host build as
    a GIF, which is how the kernel output is reviewed.

Design ("Renga", leaves drawn from scratch, not the HAIKU Leaf trademark;
Lato Light under the SIL Open Font License):
 - 0.4 s: an airy gust arrives: wide, soft, blurred wisps in layers moving
   at different speeds, and pollen-like motes.
 - The three leaves (green, orange, gold) are lifted one after another,
   tumble through the letters and leave the band on the right.
 - 2.35 s: "RENGA" glides from its place beside the leaves to the centre,
   with a slight overshoot.
 - Throughout: dappled light. Soft warm sunflecks drift over the letters as
   if overhead branches were swaying (faster while the gust blows), with a
   gentle glow where they land.
"""

import math
import os
import random
import subprocess
import sys
import tempfile

from PIL import Image, ImageDraw, ImageFilter, ImageFont


LOGO_W, LOGO_H = 372, 96
BAND_W, BAND_H = 640, 96
LOGO_LEFT = (BAND_W - LOGO_W) // 2
S = 4

LEAF_COLORS = [((126, 190, 74), (78, 140, 46)),
               ((240, 128, 40), (196, 84, 22)),
               ((246, 196, 64), (206, 150, 30))]
# rest position in logo coordinates: x, y, length, angle (deg), curl
LEAF_REST = [(34, 60, 40, -38, 0.10), (62, 40, 34, 14, -0.08),
             (78, 66, 30, 58, 0.12)]

TEXT_LEFT_IN_LOGO = 136


def text_mask(font_path):
    """The wordmark as rendered in the logo, as (mask, x, y) in logo
    coordinates."""
    font = ImageFont.truetype(font_path, 44 * S)
    img = Image.new("L", (LOGO_W * S, LOGO_H * S), 0)
    d = ImageDraw.Draw(img)
    text, tracking = "RENGA", 15 * S
    x = TEXT_LEFT_IN_LOGO * S
    ascent, _ = font.getmetrics()
    y = 47 * S - ascent * 0.62
    for c in text:
        d.text((x, y), c, font=font, fill=255)
        x += d.textlength(c, font=font) + tracking
    img = img.resize((LOGO_W, LOGO_H), Image.LANCZOS)
    box = img.getbbox()
    return img.crop(box), box[0], box[1]


def blob(size=64):
    img = Image.new("L", (size, size), 0)
    c = (size - 1) / 2
    for y in range(size):
        for x in range(size):
            r = math.hypot(x - c, y - c) / (size / 2)
            img.putpixel((x, y), int(255 * math.exp(-4.0 * r * r)) if r < 1
                else 0)
    return img


# wisps: band y, amplitude, speed (px/s), stroke width, alpha
WISPS = [(19, 5, 520, 6, 95), (42, 7, 430, 9, 70), (62, 6, 600, 5, 105),
         (80, 4, 360, 7, 60), (32, 3, 680, 4, 115)]
WISP_LEN = 240
WISP_PAD = 14


def wisp_sprite(index, amp, width):
    h = int(2 * amp + width + 2 * WISP_PAD + 12)
    w = WISP_LEN + 2 * WISP_PAD
    img = Image.new("L", (w * S, h * S), 0)
    d = ImageDraw.Draw(img)
    pts = []
    for k in range(60):
        u = k / 59
        px = WISP_PAD + WISP_LEN * u
        py = h / 2 + amp * math.sin(u * 5 + index) - 8 * u * u + 4
        pts.append((px * S, py * S))
    for k in range(len(pts) - 1):
        a = int(255 * math.sin(math.pi * k / (len(pts) - 1)))
        d.line([pts[k], pts[k + 1]], fill=a, width=width * S)
    img = img.filter(ImageFilter.GaussianBlur(5 * S))
    return img.resize((w, h), Image.LANCZOS)


def c_bytes(name, data, const="static const uint8_t"):
    lines = ["%s %s[] = {" % (const, name)]
    for i in range(0, len(data), 20):
        lines.append("\t" + ", ".join(str(b) for b in data[i:i + 20]) + ",")
    lines.append("};")
    return "\n".join(lines)


def c_ints(name, values, ctype="int32_t"):
    lines = ["static const %s %s[] = {" % (ctype, name)]
    for i in range(0, len(values), 8):
        lines.append("\t" + ", ".join(str(v) for v in values[i:i + 8]) + ",")
    lines.append("};")
    return "\n".join(lines)


def write_assets(font_path, path):
    mask, mx, my = text_mask(font_path)
    glow_pad = 12
    glow = Image.new("L", (mask.width + 2 * glow_pad,
        mask.height + 2 * glow_pad), 0)
    glow.paste(mask, (glow_pad, glow_pad))
    glow = glow.filter(ImageFilter.GaussianBlur(4))

    light_blob = blob(64)

    out = []
    out.append("// Generated by 3rdparty/ce/artwork/make_splash_renga.py; "
        "do not edit.")
    out.append("// Assets for the Renga boot splash player "
        "(src/system/kernel/boot_splash_render.cpp).")
    out.append("#ifndef _BOOT_SPLASH_PLAYER_ASSETS_RENGA_H")
    out.append("#define _BOOT_SPLASH_PLAYER_ASSETS_RENGA_H")
    out.append("")
    out.append("#include <stdint.h>")
    out.append("")
    out.append("// The animation band: the logo, centred, extended sideways "
        "so leaves can leave.")
    out.append("#define SPLASH_BAND_WIDTH %d" % BAND_W)
    out.append("#define SPLASH_BAND_HEIGHT %d" % BAND_H)
    out.append("#define SPLASH_LOGO_LEFT %d" % LOGO_LEFT)
    out.append("#define SPLASH_LOGO_WIDTH %d" % LOGO_W)
    out.append("")
    out.append("// Wordmark: alpha mask, its place at the start (beside the "
        "leaves) and after the snap (centred).")
    out.append("#define SPLASH_TEXT_WIDTH %d" % mask.width)
    out.append("#define SPLASH_TEXT_HEIGHT %d" % mask.height)
    out.append("#define SPLASH_TEXT_START_X %d" % (LOGO_LEFT + mx))
    out.append("#define SPLASH_TEXT_CENTER_X %d" % ((BAND_W - mask.width) // 2))
    out.append("#define SPLASH_TEXT_Y %d" % my)
    out.append("#define SPLASH_HAIRLINE_Y %d" % (my + mask.height + 6))
    out.append(c_bytes("kSplashTextMask", mask.tobytes()))
    out.append("#define SPLASH_GLOW_PAD %d" % glow_pad)
    out.append(c_bytes("kSplashGlowMask", glow.tobytes()))
    out.append("")
    out.append("#define SPLASH_BLOB_SIZE 64")
    out.append(c_bytes("kSplashLightBlob", light_blob.tobytes()))
    out.append("")

    out.append("// Wind wisps: band y (centre), speed px/s, alpha, sprite "
        "size; sprites are pre-blurred alpha.")
    out.append("struct splash_wisp { int16_t y; int16_t speed; int16_t "
        "alpha; int16_t width; int16_t height; const uint8_t* sprite; };")
    names = []
    for i, (y, amp, speed, width, alpha) in enumerate(WISPS):
        sprite = wisp_sprite(i, amp, width)
        name = "kSplashWispSprite%d" % i
        out.append(c_bytes(name, sprite.tobytes()))
        names.append((y, speed, alpha, sprite.width, sprite.height, name))
    out.append("static const splash_wisp kSplashWisps[] = {")
    for y, speed, alpha, w, h, name in names:
        out.append("\t{ %d, %d, %d, %d, %d, %s }," % (y, speed, alpha, w, h,
            name))
    out.append("};")
    out.append("#define SPLASH_WISP_COUNT %d" % len(names))
    out.append("")

    random.seed(7)
    motes = []
    for _ in range(46):
        motes += [int(random.uniform(-200, BAND_W)),
            int(random.uniform(8, BAND_H - 8)),
            int(random.uniform(0.6, 1.6) * 256),
            int(random.uniform(0, 1024))]
    out.append("// Motes: x, y, size (8.8), phase (sine table index)")
    out.append(c_ints("kSplashMotes", motes, "int16_t"))
    out.append("#define SPLASH_MOTE_COUNT %d" % (len(motes) // 4))
    out.append("")

    out.append("// Leaves at rest (band coordinates): x, y, length, angle "
        "(sine table units), curl (16.16), light BGR, dark BGR")
    leaves = []
    for (x, y, length, angle, curl), (light, dark) in zip(LEAF_REST,
            LEAF_COLORS):
        leaves += [LOGO_LEFT + x, y, length, int(angle / 360 * 1024),
            int(curl * 65536), light[2], light[1], light[0], dark[2],
            dark[1], dark[0]]
    out.append(c_ints("kSplashLeaves", leaves))
    out.append("#define SPLASH_LEAF_COUNT %d" % len(LEAF_REST))
    out.append("")

    out.append("// sin(2 pi i / 1024) in 16.16")
    out.append(c_ints("kSplashSine", [int(round(math.sin(2 * math.pi * i
        / 1024) * 65536)) for i in range(1024)]))
    out.append("// Leaf half-width profile: sin(pi s)^0.85 for s = i / 256, "
        "16.16")
    out.append(c_ints("kSplashLeafProfile", [int(round(math.sin(math.pi * i
        / 256) ** 0.85 * 65536)) for i in range(257)]))
    out.append("")
    out.append("#endif	// _BOOT_SPLASH_PLAYER_ASSETS_RENGA_H")

    with open(path, "w") as f:
        f.write("\n".join(out) + "\n")


HOST_MAIN = r'''
#include <stdio.h>
#include <stdlib.h>
#include "boot_splash_render.h"

// Renders frames for the generator: argv = out_dir first_ms step_ms count
int main(int argc, char** argv)
{
	const char* dir = argv[1];
	int first = atoi(argv[2]), step = atoi(argv[3]), count = atoi(argv[4]);
	static uint8_t band[SPLASH_BAND_WIDTH * SPLASH_BAND_HEIGHT * 3];
	for (int i = 0; i < count; i++) {
		splash_render(band, SPLASH_BAND_WIDTH * 3, first + i * step, false);
		char path[512];
		snprintf(path, sizeof(path), "%s/f%04d.bgr", dir, i);
		FILE* f = fopen(path, "wb");
		fwrite(band, 1, sizeof(band), f);
		fclose(f);
	}
	splash_render(band, SPLASH_BAND_WIDTH * 3, 0, true);
	char path[512];
	snprintf(path, sizeof(path), "%s/settled.bgr", dir);
	FILE* f = fopen(path, "wb");
	fwrite(band, 1, sizeof(band), f);
	fclose(f);
	return 0;
}
'''


def render_host(top, first_ms, step_ms, count):
    """Builds the kernel renderer for the host and renders frames."""
    tmp = tempfile.mkdtemp(prefix="renga-splash-")
    main = os.path.join(tmp, "main.cpp")
    with open(main, "w") as f:
        f.write(HOST_MAIN)
    exe = os.path.join(tmp, "render")
    subprocess.check_call(["g++", "-O2", "-std=c++17", "-Wall", "-Werror",
        "-I", os.path.join(top, "src/system/kernel"),
        "-I", os.path.join(top, "headers/private/kernel"),
        main, os.path.join(top, "src/system/kernel/boot_splash_render.cpp"),
        "-o", exe])
    subprocess.check_call([exe, tmp, str(first_ms), str(step_ms), str(count)])

    def load(name):
        data = open(os.path.join(tmp, name), "rb").read()
        b, g, r = Image.frombytes("RGB", (BAND_W, BAND_H), data).split()
        return Image.merge("RGB", (r, g, b))

    frames = [load("f%04d.bgr" % i) for i in range(count)]
    return frames, load("settled.bgr")


if __name__ == "__main__":
    font_path, top, tool = sys.argv[1], sys.argv[2], sys.argv[3]
    preview = None
    if "--preview" in sys.argv:
        preview = sys.argv[sys.argv.index("--preview") + 1]

    write_assets(font_path, os.path.join(top,
        "headers/private/kernel/boot/splash_player_assets-renga.h"))

    count = 160 if preview else 1
    frames, settled = render_host(top, 0, 40, count)
    logo = frames[0].crop((LOGO_LEFT, 0, LOGO_LEFT + LOGO_W, LOGO_H))
    logo_path = os.path.join(top,
        "data/artwork/boot_splash/splash_logo-renga.png")
    logo.save(logo_path)
    subprocess.check_call([tool, logo_path, "50", "50",
        os.path.join(top, "data/artwork/boot_splash/splash_icons.png"),
        "50", "50",
        os.path.join(top, "headers/private/kernel/boot/images-renga.h")])

    if preview:
        frames[0].save(preview, save_all=True, append_images=frames[1:]
            + [settled] * 10, duration=40, loop=0)
