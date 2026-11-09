#!/usr/bin/env python3
"""Renders the Renga boot splash: the wordmark and its gust animation.

Usage:
  make_splash_renga.py <Lato-Light.ttf> <haiku top> [generate_boot_screen]

Writes:
  data/artwork/boot_splash/splash_logo-renga.png
      Frame 0, the logo the boot loader draws. If the path of Haiku's
      generate_boot_screen tool is given, it is also converted into
      headers/private/kernel/boot/images-renga.h together with the stage
      icons (otherwise run the tool by hand, see below).
  headers/private/kernel/boot/splash_animation-renga.h
      Frames 1-7 for the kernel, one per boot stage. Each is stored as the
      rectangle that changed since the previous frame (24-bit BGR, the
      blitter's order, zlib-compressed), so the loader decompresses only a
      few small rectangles instead of seven full logos.

Converting frame 0 by hand:
  generate_boot_screen data/artwork/boot_splash/splash_logo-renga.png 50 50 \
      data/artwork/boot_splash/splash_icons.png 50 50 \
      headers/private/kernel/boot/images-renga.h

Design: "RENGA" in spaced Lato Light (SIL Open Font License) with a fading
hairline, and three leaves drawn from scratch (not the HAIKU Leaf
trademark) in spring green, maple orange and ginkgo gold. During boot a
gust of wind (thin streaks above and below the wordmark) lifts the leaves,
turns them and sets them down a little further on, one step per boot stage,
so the animation shows real boot progress and needs no timer. Canvas
372 x 96, as before, so the splash layout is unchanged.
"""

import math, sys
from PIL import Image, ImageDraw, ImageFont, ImageFilter

W, H = 372, 96
S = 4  # supersampling
LIGHT = None	# set from the command line

# Leaf palette: spring green, maple orange, ginkgo gold.
LEAVES = [((126, 190, 74), (78, 140, 46)),
          ((240, 128, 40), (196, 84, 22)),
          ((246, 196, 64), (206, 150, 30))]


def ease(t):
    return t * t * (3 - 2 * t)


def leaf_polygon(cx, cy, length, width, angle, curl):
    """An almond leaf with a slight curl, as a polygon (supersampled)."""
    points = []
    steps = 40
    for side in (1, -1):
        rng = range(steps + 1) if side == 1 else range(steps, -1, -1)
        for i in rng:
            u = i / steps                       # 0 = stem, 1 = tip
            x = (u - 0.5) * length
            half = width / 2 * math.sin(math.pi * u) ** 0.85
            y = side * half + curl * length * (u - 0.5) ** 2
            points.append((x, y))
    a = math.radians(angle)
    return [(cx + x * math.cos(a) - y * math.sin(a),
             cy + x * math.sin(a) + y * math.cos(a)) for x, y in points]


def draw_leaf(draw, cx, cy, length, width, angle, curl, colors, alpha=255):
    light, dark = colors
    poly = leaf_polygon(cx, cy, length, width, angle, curl)
    draw.polygon(poly, fill=dark + (alpha,))
    # lighter upper half: a simple fold shading
    inner = leaf_polygon(cx, cy, length * 0.96, width * 0.80, angle,
        curl - 0.02)
    a = math.radians(angle)
    ox, oy = -math.sin(a) * width * 0.08, math.cos(a) * width * 0.08
    draw.polygon([(x - ox, y - oy) for x, y in inner], fill=light + (alpha,))
    # midrib and stem
    a = math.radians(angle)
    x0 = cx - 0.62 * length / 2 * math.cos(a)
    y0 = cy - 0.62 * length / 2 * math.sin(a)
    x1 = cx + 0.42 * length / 2 * math.cos(a)
    y1 = cy + 0.42 * length / 2 * math.sin(a)
    draw.line([(x0 - 0.2 * length * math.cos(a), y0 - 0.2 * length * math.sin(a)),
               (x1, y1)], fill=dark + (alpha,), width=max(1, int(0.045 * width)))


def frame(t):
    """t in [0, 1]: 0 = calm (loader), 1 = settled after the gust."""
    w, h = W * S, H * S
    img = Image.new("RGBA", (w, h), (0, 0, 0, 255))
    over = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    d = ImageDraw.Draw(over)

    # Wordmark: "RENGA", wide tracking, light weight.
    font = ImageFont.truetype(LIGHT, 44 * S)
    text = "RENGA"
    tracking = 15 * S
    widths = [d.textlength(c, font=font) for c in text]
    total = sum(widths) + tracking * (len(text) - 1)
    x = 136 * S
    ascent, _ = font.getmetrics()
    y = 47 * S - ascent * 0.62
    for c, cw in zip(text, widths):
        d.text((x, y), c, font=font, fill=(244, 244, 244, 255))
        x += cw + tracking
    text_left = 136 * S
    text_right = text_left + total

    # Wind: three thin streaks that sweep across with the gust and fade.
    g = ease(min(1.0, t * 1.15))
    for i, (yy, length, delay) in enumerate([(14, 150, 0.0), (86, 120, 0.12),
                                             (22, 80, 0.25)]):
        p = max(0.0, min(1.0, (g - delay) / (1 - delay + 1e-6)))
        if p <= 0 or p >= 1:
            continue
        head = (-40 + p * (W + 120)) * S
        alpha = int(215 * math.sin(math.pi * p))
        pts = []
        for k in range(30):
            u = k / 29
            px = head - length * S * (1 - u)
            py = yy * S + math.sin(u * math.pi * 1.4 + i) * 3 * S * (1 - u)
            pts.append((px, py))
        for k in range(len(pts) - 1):
            a = int(alpha * (k / (len(pts) - 1)))
            d.line([pts[k], pts[k + 1]], fill=(206, 216, 226, a),
                   width=int(1.6 * S))

    # Leaves: rest to the left of the wordmark; the gust lifts them and
    # carries them a little to the right, where they settle in a new pose.
    rest = [(34, 60, 40, -38, 0.10), (62, 40, 34, 14, -0.08),
            (78, 66, 30, 58, 0.12)]
    settled = [(48, 56, 40, -12, 0.06), (88, 34, 34, 36, -0.05),
               (104, 64, 30, 92, 0.10)]
    for i, (r, s) in enumerate(zip(rest, settled)):
        p = max(0.0, min(1.0, (t - 0.08 * i) / (1 - 0.16)))
        e = ease(p)
        cx = r[0] + (s[0] - r[0]) * e
        cy = r[1] + (s[1] - r[1]) * e - 14 * math.sin(math.pi * p)  # lift
        ang = r[3] + (s[3] - r[3]) * e + 25 * math.sin(math.pi * p * 2)  # flutter
        curl = r[4] + (s[4] - r[4]) * e
        length = r[2] * (1 - 0.12 * math.sin(math.pi * p))  # foreshortening
        draw_leaf(d, cx * S, cy * S, length * S, length * 0.46 * S, ang,
                  curl, LEAVES[i])

    # Hairline under the wordmark, fading out at both ends.
    ry = 74 * S
    for px in range(int(text_left), int(text_right)):
        u = (px - text_left) / (text_right - text_left)
        a = int(140 * min(1, min(u, 1 - u) / 0.2))
        d.line([(px, ry), (px, ry + S - 1)], fill=(180, 180, 180, a))

    img.alpha_composite(over)
    return img.convert("RGB").resize((W, H), Image.LANCZOS)




FRAME_COUNT = 8		# frame 0 = loader, frames 1-7 = boot stages 0-6


def changed_rect(previous, current):
    """Bounding box (x, y, w, h) of the pixels that differ, or None."""
    from PIL import ImageChops
    box = ImageChops.difference(previous, current).getbbox()
    if box is None:
        return None
    left, top, right, bottom = box
    return left, top, right - left, bottom - top


def bgr_bytes(image):
    r, g, b = image.split()
    return Image.merge("RGB", (b, g, r)).tobytes()


def c_array(name, data):
    lines = ["static const uint8 %s[] = {" % name]
    for i in range(0, len(data), 16):
        lines.append("\t" + ", ".join(str(b) for b in data[i:i + 16]) + ",")
    lines.append("};")
    return "\n".join(lines)


def write_animation_header(frames, path):
    import zlib
    rects = []
    arrays = []
    for k in range(1, len(frames)):
        rect = changed_rect(frames[k - 1], frames[k])
        if rect is None:
            rects.append((0, 0, 0, 0, None))
            continue
        x, y, w, h = rect
        pixels = bgr_bytes(frames[k].crop((x, y, x + w, y + h)))
        name = "kSplashAnimationFrame%d" % k
        arrays.append(c_array(name, zlib.compress(pixels, 9)))
        rects.append((x, y, w, h, name))

    out = []
    out.append("// Generated by 3rdparty/ce/artwork/make_splash_renga.py;"
        " do not edit.")
    out.append("#ifndef _KERNEL_BOOT_SPLASH_ANIMATION_RENGA_H")
    out.append("#define _KERNEL_BOOT_SPLASH_ANIMATION_RENGA_H")
    out.append("")
    out.append("#define SPLASH_ANIMATION 1")
    out.append("")
    out.append("struct splash_animation_frame {")
    out.append("\tuint16 x, y, width, height;	// within the logo")
    out.append("};")
    out.append("")
    out.append("// Frame n + 1 is shown when boot stage n starts.")
    out.append("static const int32 kSplashAnimationFrameCount = %d;"
        % len(rects))
    out.append("static const splash_animation_frame kSplashAnimationFrames[] = {")
    for x, y, w, h, _ in rects:
        out.append("\t{ %d, %d, %d, %d }," % (x, y, w, h))
    out.append("};")
    out.append("")
    out.append("#ifndef __BOOTSPLASH_KERNEL__")
    out.append("// Compressed pixel data (24-bit BGR), for the boot loader.")
    for a in arrays:
        out.append(a)
    out.append("static const uint8* const kSplashAnimationData[] = {")
    for *_, name in rects:
        out.append("\t%s," % (name if name else "NULL"))
    out.append("};")
    out.append("static const uint32 kSplashAnimationDataSize[] = {")
    for *_, name in rects:
        out.append("\tsizeof(%s)," % name if name else "\t0,")
    out.append("};")
    out.append("#endif")
    out.append("")
    out.append("#endif	// _KERNEL_BOOT_SPLASH_ANIMATION_RENGA_H")
    with open(path, "w") as f:
        f.write("\n".join(out) + "\n")


if __name__ == "__main__":
    import os
    import subprocess
    globals()["LIGHT"] = sys.argv[1]
    top = sys.argv[2]
    tool = sys.argv[3] if len(sys.argv) > 3 else None

    frames = [frame(i / (FRAME_COUNT - 1)) for i in range(FRAME_COUNT)]
    logo = os.path.join(top, "data/artwork/boot_splash/splash_logo-renga.png")
    frames[0].save(logo)
    write_animation_header(frames, os.path.join(top,
        "headers/private/kernel/boot/splash_animation-renga.h"))
    if tool:
        subprocess.check_call([tool, logo, "50", "50",
            os.path.join(top, "data/artwork/boot_splash/splash_icons.png"),
            "50", "50",
            os.path.join(top, "headers/private/kernel/boot/images-renga.h")])
