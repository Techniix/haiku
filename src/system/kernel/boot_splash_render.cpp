/*
 * Copyright 2026, Haiku Community Edition contributors.
 * Distributed under the terms of the MIT License.
 */

/*!	The Renga boot splash renderer: one frame of the animation band from the
	assets in boot/splash_player_assets-renga.h. See
	3rdparty/ce/artwork/make_splash_renga.py for the design and timeline.

	Integer arithmetic only (16.16 fixed point and table lookups): it runs in
	the kernel, which must not touch the FPU, and it is also compiled on the
	host so that the boot loader's static logo is exactly this renderer's
	first frame.

	Composition order, back to front: black, glow under the letters, the
	letters (silver, warmed by the light), the hairline, wind wisps, motes,
	leaves. Every layer is a straightforward alpha blend into the BGR band.
*/


#include "boot_splash_render.h"

#include <string.h>


typedef int64_t fixed;	// 16.16
#define FIXED_ONE		((fixed)1 << 16)

// timeline (ms)
static const int32_t kGustStart = 400;
static const int32_t kGustLength = 2800;
static const int32_t kLeafStart = 650;
static const int32_t kLeafStagger = 220;
static const int32_t kLeafFlight = 1550;
static const int32_t kSnapStart = 2350;
static const int32_t kSnapLength = 550;


static inline fixed
fsin(int32_t index)
{
	return kSplashSine[index & 1023];
}


static inline fixed
fcos(int32_t index)
{
	return kSplashSine[(index + 256) & 1023];
}


static inline int32_t
clamp255(int64_t value)
{
	return value < 0 ? 0 : (value > 255 ? 255 : (int32_t)value);
}


//!	Blends "color" (BGR) over the pixel with alpha 0..255.
static inline void
blend(uint8_t* pixel, int32_t b, int32_t g, int32_t r, int32_t alpha)
{
	if (alpha <= 0)
		return;
	if (alpha >= 255) {
		pixel[0] = b;
		pixel[1] = g;
		pixel[2] = r;
		return;
	}
	pixel[0] += ((b - pixel[0]) * alpha) / 255;
	pixel[1] += ((g - pixel[1]) * alpha) / 255;
	pixel[2] += ((r - pixel[2]) * alpha) / 255;
}


//!	Gust strength 0..255: sin(pi p) over the gust.
static int32_t
gust_strength(int32_t timeMs)
{
	int32_t p = timeMs - kGustStart;
	if (p <= 0 || p >= kGustLength)
		return 0;
	return clamp255((fsin(p * 512 / kGustLength) * 255) >> 16);
}


//!	ease-out-back for p in [0, 1] (16.16), slight overshoot.
static fixed
ease_out_back(fixed p)
{
	const fixed k = (fixed)(1.6 * 65536);
	fixed t = p - FIXED_ONE;
	fixed t2 = (t * t) >> 16;
	fixed t3 = (t2 * t) >> 16;
	return FIXED_ONE + (((k + FIXED_ONE) * t3) >> 16) + ((k * t2) >> 16);
}


//!	Wordmark x (band pixels) at this time.
static int32_t
text_x(int32_t timeMs, bool settled)
{
	if (settled)
		return SPLASH_TEXT_CENTER_X;
	int32_t p = timeMs - kSnapStart;
	if (p <= 0)
		return SPLASH_TEXT_START_X;
	if (p >= kSnapLength)
		return SPLASH_TEXT_CENTER_X;
	fixed e = ease_out_back(((fixed)p << 16) / kSnapLength);
	return SPLASH_TEXT_START_X + (int32_t)(((SPLASH_TEXT_CENTER_X
		- SPLASH_TEXT_START_X) * e) >> 16);
}


// #pragma mark - dappled light


struct Fleck {
	int32_t	x, y, rx, ry;	// pixels, relative to the light box
};

// fraction x, fraction y, radius fraction, speed (rad/s * 1000), phase
static const int32_t kFlecks[5][5] = {
	{ 150, 400, 220, 550, 0 }, { 450, 700, 180, 750, 1700 },
	{ 750, 300, 250, 450, 3100 }, { 950, 600, 160, 900, 4400 },
	{ 300, 100, 140, 1100, 2200 },
};


static void
compute_flecks(int32_t timeMs, int32_t width, int32_t height, int32_t gust,
	Fleck* flecks)
{
	// Branches sway faster while the gust blows.
	int64_t sway = 1000 + 2500 * gust / 255;		// x1000
	for (int i = 0; i < 5; i++) {
		// angle in radians x 1e6 -> table index (1024 per 2 pi)
		int64_t radians = (int64_t)timeMs * kFlecks[i][3] * sway / 1000
			+ (int64_t)kFlecks[i][4] * 1000;
		int32_t a = (int32_t)(radians * 1024 / 6283185);
		int32_t b = (int32_t)(radians * 7 / 10 * 1024 / 6283185
			+ kFlecks[i][4] * 1300 / 1000 * 1024 / 6283);
		flecks[i].x = (int32_t)((kFlecks[i][0] * width
			+ ((100 * width * fsin(a)) >> 16)) / 1000);
		flecks[i].y = (int32_t)((kFlecks[i][1] * height
			+ ((250 * height * fsin(b)) >> 16)) / 1000);
		int32_t r = (int32_t)(kFlecks[i][2] * width / 1000
			* (850 + ((150 * fsin(timeMs * 1300 / 6283 + i * 163)) >> 16))
			/ 1000);
		flecks[i].rx = r < 4 ? 4 : r;
		flecks[i].ry = flecks[i].rx * 7 / 10;
	}
}


//!	Light 0..255 at (x, y) in the light box.
static inline int32_t
light_at(const Fleck* flecks, int32_t x, int32_t y)
{
	int32_t sum = 0;
	for (int i = 0; i < 5; i++) {
		// map onto the 64 x 64 blob (radius 32)
		int32_t u = 32 + (x - flecks[i].x) * 32 / flecks[i].rx;
		int32_t v = 32 + (y - flecks[i].y) * 32 / flecks[i].ry;
		if (u < 0 || u >= SPLASH_BLOB_SIZE || v < 0 || v >= SPLASH_BLOB_SIZE)
			continue;
		sum += kSplashLightBlob[v * SPLASH_BLOB_SIZE + u];
	}
	return sum > 255 ? 255 : sum;
}


static void
render_wordmark(uint8_t* bgr, int32_t bytesPerRow, int32_t timeMs,
	bool settled)
{
	const int32_t pad = SPLASH_GLOW_PAD;
	const int32_t boxWidth = SPLASH_TEXT_WIDTH + 2 * pad;
	const int32_t boxHeight = SPLASH_TEXT_HEIGHT + 2 * pad;
	const int32_t x0 = text_x(timeMs, settled) - pad;
	const int32_t y0 = SPLASH_TEXT_Y - pad;

	Fleck flecks[5];
	compute_flecks(settled ? 0 : timeMs, boxWidth, boxHeight,
		settled ? 0 : gust_strength(timeMs), flecks);

	for (int32_t y = 0; y < boxHeight; y++) {
		int32_t by = y0 + y;
		if (by < 0 || by >= SPLASH_BAND_HEIGHT)
			continue;
		uint8_t* row = bgr + by * bytesPerRow;
		for (int32_t x = 0; x < boxWidth; x++) {
			int32_t bx = x0 + x;
			if (bx < 0 || bx >= SPLASH_BAND_WIDTH)
				continue;
			int32_t glow = kSplashGlowMask[y * boxWidth + x];
			int32_t tx = x - pad, ty = y - pad;
			int32_t mask = (tx >= 0 && tx < SPLASH_TEXT_WIDTH && ty >= 0
					&& ty < SPLASH_TEXT_HEIGHT)
				? kSplashTextMask[ty * SPLASH_TEXT_WIDTH + tx] : 0;
			if (glow == 0 && mask == 0)
				continue;

			int32_t light = light_at(flecks, x, y);
			uint8_t* pixel = row + bx * 3;
			// warm glow where the light lands near the letters
			blend(pixel, 160, 226, 255, glow * light / 255 * 55 / 100);
			if (mask != 0) {
				// silver in the shade, warm white in the light
				int32_t b = 130 + (214 - 130) * light / 255;
				int32_t g = 122 + (244 - 122) * light / 255;
				int32_t r = 118 + (255 - 118) * light / 255;
				blend(pixel, b, g, r, mask);
			}
		}
	}

	// hairline under the wordmark, fading out at both ends
	int32_t hx = x0 + pad;
	uint8_t* row = bgr + SPLASH_HAIRLINE_Y * bytesPerRow;
	for (int32_t x = 0; x < SPLASH_TEXT_WIDTH; x++) {
		int32_t bx = hx + x;
		if (bx < 0 || bx >= SPLASH_BAND_WIDTH)
			continue;
		int32_t edge = x < SPLASH_TEXT_WIDTH - x ? x : SPLASH_TEXT_WIDTH - x;
		int32_t alpha = 130 * edge * 5 / SPLASH_TEXT_WIDTH;
		blend(row + bx * 3, 180, 180, 180, alpha > 130 ? 130 : alpha);
	}
}


// #pragma mark - wind


static void
render_wind(uint8_t* bgr, int32_t bytesPerRow, int32_t timeMs)
{
	int32_t gust = gust_strength(timeMs);
	if (gust == 0)
		return;

	// wisps: pre-blurred strokes sweeping left to right, each at its speed
	for (int32_t i = 0; i < SPLASH_WISP_COUNT; i++) {
		const splash_wisp& wisp = kSplashWisps[i];
		int32_t head = -260 + (timeMs - kGustStart) * wisp.speed / 1000;
		int32_t left = head - wisp.width;
		int32_t top = wisp.y - wisp.height / 2
			+ (int32_t)((3 * fsin(timeMs * 3 * 1024 / 6283 + i * 200)) >> 16);
		int32_t alpha = wisp.alpha * gust / 255;
		for (int32_t y = 0; y < wisp.height; y++) {
			int32_t by = top + y;
			if (by < 0 || by >= SPLASH_BAND_HEIGHT)
				continue;
			uint8_t* row = bgr + by * bytesPerRow;
			const uint8_t* sprite = wisp.sprite + y * wisp.width;
			for (int32_t x = 0; x < wisp.width; x++) {
				int32_t bx = left + x;
				if (bx < 0 || bx >= SPLASH_BAND_WIDTH || sprite[x] == 0)
					continue;
				blend(row + bx * 3, 246, 228, 214, sprite[x] * alpha / 255);
			}
		}
	}

	// motes: pollen carried along by the gust
	static const int32_t kDot[3][3] = { { 64, 160, 64 }, { 160, 255, 160 },
		{ 64, 160, 64 } };
	int32_t moteAlpha = 150 * gust / 255;
	for (int32_t i = 0; i < SPLASH_MOTE_COUNT; i++) {
		const int16_t* mote = &kSplashMotes[i * 4];
		int32_t drift = (timeMs - kGustStart) * 230
			* (600 + mote[2] * 300 / 256) / 1000 / 1000;
		int32_t span = SPLASH_BAND_WIDTH + 200;
		int32_t px = ((mote[0] + drift) % span + span) % span - 100;
		int32_t py = mote[1] + (int32_t)((6 * fsin(timeMs * 2 * 1024 / 6283
			+ mote[3])) >> 16);
		for (int32_t dy = -1; dy <= 1; dy++) {
			int32_t by = py + dy;
			if (by < 0 || by >= SPLASH_BAND_HEIGHT)
				continue;
			for (int32_t dx = -1; dx <= 1; dx++) {
				int32_t bx = px + dx;
				if (bx < 0 || bx >= SPLASH_BAND_WIDTH)
					continue;
				blend(bgr + by * bytesPerRow + bx * 3, 230, 250, 255,
					kDot[dy + 1][dx + 1] * moteAlpha / 255);
			}
		}
	}
}


// #pragma mark - leaves


static void
render_leaf(uint8_t* bgr, int32_t bytesPerRow, fixed cx, fixed cy,
	int32_t length, fixed widthScale, int32_t angle, fixed curl,
	const int32_t* colors, int32_t fade)
{
	const fixed len = (fixed)length << 16;
	const fixed halfWidth = ((fixed)length * 46 / 100 * widthScale) >> 1;
		// 16.16: half of length * 0.46 * scale
	const fixed c = fcos(angle), s = fsin(angle);

	int32_t extent = length / 2 + 4;
	int32_t left = (int32_t)(cx >> 16) - extent;
	int32_t top = (int32_t)(cy >> 16) - extent;
	for (int32_t y = top; y <= top + 2 * extent; y++) {
		if (y < 0 || y >= SPLASH_BAND_HEIGHT)
			continue;
		uint8_t* row = bgr + y * bytesPerRow;
		for (int32_t x = left; x <= left + 2 * extent; x++) {
			if (x < 0 || x >= SPLASH_BAND_WIDTH)
				continue;
			fixed dx = ((fixed)x << 16) + FIXED_ONE / 2 - cx;
			fixed dy = ((fixed)y << 16) + FIXED_ONE / 2 - cy;
			fixed u = (dx * c + dy * s) >> 16;		// along the leaf
			fixed v = (-dx * s + dy * c) >> 16;		// across
			fixed along = (u << 16) / len + FIXED_ONE / 2;	// 0..1
			if (along <= 0 || along >= FIXED_ONE)
				continue;
			int32_t index = (int32_t)(along >> 8);	// 0..256
			fixed half = (halfWidth * kSplashLeafProfile[index]) >> 16;
			fixed centered = along - FIXED_ONE / 2;
			fixed curlOffset = (((curl * length) * ((centered * centered)
				>> 16)) >> 16);
			fixed distance = v - curlOffset;
			if (distance < 0)
				distance = -distance;

			// coverage with one pixel of antialiasing
			fixed coverage = half - distance + FIXED_ONE / 2;
			if (coverage <= 0)
				continue;
			int32_t alpha = coverage >= FIXED_ONE ? 255
				: (int32_t)(coverage * 255 >> 16);

			// lighter upper fold
			fixed innerHalf = half * 8 / 10;
			fixed innerDistance = v - curlOffset + halfWidth * 16 / 100;
			if (innerDistance < 0)
				innerDistance = -innerDistance;
			fixed innerCoverage = innerHalf - innerDistance + FIXED_ONE / 2;
			int32_t inner = innerCoverage <= 0 ? 0 : (innerCoverage
				>= FIXED_ONE ? 255 : (int32_t)(innerCoverage * 255 >> 16));

			uint8_t* pixel = row + x * 3;
			alpha = alpha * fade / 255;
			// record: x, y, length, angle, curl, light BGR (5-7),
			// dark BGR (8-10)
			blend(pixel, colors[8], colors[9], colors[10], alpha);
			blend(pixel, colors[5], colors[6], colors[7],
				inner * alpha / 255);
		}
	}
}


static void
render_leaves(uint8_t* bgr, int32_t bytesPerRow, int32_t timeMs)
{
	for (int32_t i = 0; i < SPLASH_LEAF_COUNT; i++) {
		const int32_t* leaf = &kSplashLeaves[i * 11];
		int32_t start = kLeafStart + kLeafStagger * i;
		int32_t elapsed = timeMs - start;

		fixed cx = (fixed)leaf[0] << 16, cy = (fixed)leaf[1] << 16;
		int32_t angle = leaf[3];
		fixed widthScale = FIXED_ONE;
		int32_t fade = 255;
		if (elapsed > 0) {
			if (elapsed >= kLeafFlight)
				continue;	// gone
			fixed p = ((fixed)elapsed << 16) / kLeafFlight;
			// accelerating flight to beyond the right edge
			fixed eased = (p * p >> 16) * 6 / 10 + p * 4 / 10;
			fixed target = (fixed)(SPLASH_BAND_WIDTH + 80) << 16;
			cx += ((target - cx) * eased) >> 16;
			int32_t arc = (int32_t)(p * 512 >> 16);		// pi p
			cy -= 24 * fsin(arc);
			cy += 7 * fsin((int32_t)(p * 11 * 1024 / 6283 >> 16) * 1
				+ (int32_t)((p * 1793) >> 16) + i * 163);
			angle += (int32_t)((p * 1536) >> 16) * ((i & 1) ? -1 : 1);
				// one and a half turns
			fixed tumble = fcos((int32_t)((p * 1467) >> 16) + i * 163);
			widthScale = tumble < 0 ? -tumble : tumble;
			if (widthScale < FIXED_ONE * 15 / 100)
				widthScale = FIXED_ONE * 15 / 100;
			// fade out over the last 40 pixels of the band
			int32_t x = (int32_t)(cx >> 16);
			if (x > SPLASH_BAND_WIDTH - 40)
				fade = clamp255((SPLASH_BAND_WIDTH - x) * 255 / 40);
		}
		render_leaf(bgr, bytesPerRow, cx, cy, leaf[2], widthScale, angle,
			leaf[4], leaf, fade);
	}
}


// #pragma mark -


void
splash_render(uint8_t* bgr, int32_t bytesPerRow, int32_t timeMs, bool settled)
{
	for (int32_t y = 0; y < SPLASH_BAND_HEIGHT; y++)
		memset(bgr + y * bytesPerRow, 0, SPLASH_BAND_WIDTH * 3);

	render_wordmark(bgr, bytesPerRow, timeMs, settled);
	if (settled)
		return;
	render_wind(bgr, bytesPerRow, timeMs);
	render_leaves(bgr, bytesPerRow, timeMs);
}
