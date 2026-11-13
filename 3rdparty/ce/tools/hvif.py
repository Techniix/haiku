#
# Copyright 2026, Renga contributors.
# Distributed under the terms of the MIT License.
#
"""A small writer for HVIF, Haiku's vector icon format (the flat format of
src/libs/icon/flat_icon: FlatIconFormat.h, FlatIconImporter.cpp), for icons
made of polygons: solid colours or linear gradients, optionally outlined
with HVIF's stroke transformer. Used by make_network_icons.py and
make_linuxkpi_manager_icon.py; Icon-O-Matic opens the result.

    icon = Icon()
    icon.polygon(points, (r, g, b, a))                       # solid
    icon.polygon(points, gradient((x0, y0), (x1, y1),
        [(0, rgba), (255, rgba)]), outline=((r, g, b, a), 3))
    data = icon.flat()
    rdef_array(1, "name", data)

Coordinates are on HVIF's 64 x 64 grid. preview() draws an icon with PIL
(an approximation, for checking the shapes by eye).
"""

import math
import struct


def coord(value):
	if value == int(value) and -32 <= value < 96:
		return bytes([int(value) + 32])
	encoded = int(round((value + 128.0) * 102.0))
	assert 0 <= encoded < 32768
	return bytes([0x80 | (encoded >> 8), encoded & 0xff])


def float24(value):
	"""1 sign bit, 6 exponent bits (bias 32), 17 mantissa bits."""
	if value == 0:
		return b"\0\0\0"
	bits = struct.unpack(">I", struct.pack(">f", value))[0]
	sign = bits >> 31
	exponent = ((bits >> 23) & 0xff) - 127 + 32
	assert 0 < exponent < 64, value
	packed = (sign << 23) | (exponent << 17) | ((bits & 0x7fffff) >> 6)
	return bytes([packed >> 16, (packed >> 8) & 0xff, packed & 0xff])


def round_coord(value):
	# keep simple integers simple (one byte each)
	r = round(value)
	return r if abs(r - value) < 0.05 else round(value * 4) / 4


class Gradient:
	"""A linear gradient from p0 (stop offset 0) to p1 (offset 255)."""

	def __init__(self, p0, p1, stops):
		self.p0 = p0
		self.p1 = p1
		self.stops = stops

	def matrix(self):
		# gradient space runs from -64 to 64 along x
		dx = (self.p1[0] - self.p0[0]) / 128.0
		dy = (self.p1[1] - self.p0[1]) / 128.0
		return (dx, dy, -dy, dx, (self.p0[0] + self.p1[0]) / 2.0,
			(self.p0[1] + self.p1[1]) / 2.0)

	def key(self):
		return ("g", self.p0, self.p1, tuple(self.stops))

	def flat(self):
		opaque = all(rgba[3] == 0xff for _, rgba in self.stops)
		flags = (1 << 1) | ((1 << 2) if opaque else 0)
		data = bytes([2, 0, flags, len(self.stops)])
		for value in self.matrix():
			data += float24(value)
		for offset, rgba in self.stops:
			data += bytes([offset]) + bytes(rgba[:3] if opaque else rgba)
		return data

	def colour_at(self, x, y):
		vx, vy = self.p1[0] - self.p0[0], self.p1[1] - self.p0[1]
		length = vx * vx + vy * vy
		t = ((x - self.p0[0]) * vx + (y - self.p0[1]) * vy) / length
		t = min(max(t, 0.0), 1.0) * 255
		stops = self.stops
		if t <= stops[0][0]:
			return stops[0][1]
		for (o0, c0), (o1, c1) in zip(stops, stops[1:]):
			if t <= o1:
				f = (t - o0) / max(o1 - o0, 1)
				return tuple(int(c0[i] + (c1[i] - c0[i]) * f) for i in range(4))
		return stops[-1][1]


def gradient(p0, p1, stops):
	return Gradient(p0, p1, stops)


class Icon:
	def __init__(self):
		self.styles = []
		self.paths = []
		self.shapes = []

	def style(self, fill):
		key = fill.key() if isinstance(fill, Gradient) else fill
		for index, existing in enumerate(self.styles):
			existingKey = existing.key() if isinstance(existing, Gradient) \
				else existing
			if existingKey == key:
				return index
		self.styles.append(fill)
		return len(self.styles) - 1

	def polygon(self, points, fill, outline=None):
		"""outline: (colour, width) drawn under the fill, so that half of
		its width shows outside."""
		self.paths.append([(round_coord(x), round_coord(y))
			for x, y in points])
		path = len(self.paths) - 1
		if outline is not None:
			self.shapes.append((self.style(outline[0]), path, outline[1]))
		self.shapes.append((self.style(fill), path, None))

	def flat(self):
		data = b"ncif"
		assert len(self.styles) < 256 and len(self.paths) < 256
		data += bytes([len(self.styles)])
		for fill in self.styles:
			if isinstance(fill, Gradient):
				data += fill.flat()
			elif fill[3] == 0xff:
				data += bytes([3, fill[0], fill[1], fill[2]])
			else:
				data += bytes([1]) + bytes(fill)
		data += bytes([len(self.paths)])
		for points in self.paths:
			assert len(points) < 256
			data += bytes([(1 << 1) | (1 << 3), len(points)])	# closed, lines
			for x, y in points:
				data += coord(x) + coord(y)
		data += bytes([len(self.shapes)])
		for style, path, stroke in self.shapes:
			data += bytes([10, style, 1, path])
			if stroke is None:
				data += bytes([0])
			else:
				# a stroke transformer: width, round join and cap, miter 4
				data += bytes([1 << 4, 1, 23, int(stroke) + 128,
					(2 << 4) | 2, 4])
		return data

	def preview(self, size, background):
		from PIL import Image, ImageDraw
		scale = size / 64.0
		tile = Image.new("RGBA", (size, size), background)
		for style, path, stroke in self.shapes:
			fill = self.styles[style]
			points = [(x * scale, y * scale) for x, y in self.paths[path]]
			mask = Image.new("L", (size, size), 0)
			draw = ImageDraw.Draw(mask)
			if stroke is None:
				draw.polygon(points, fill=255)
			else:
				draw.line(points + points[:1], fill=255,
					width=max(1, int(round(stroke * scale))), joint="curve")
			layer = Image.new("RGBA", (size, size), (0, 0, 0, 0))
			if isinstance(fill, Gradient):
				pixels = layer.load()
				for y in range(size):
					for x in range(size):
						pixels[x, y] = fill.colour_at((x + 0.5) / scale,
							(y + 0.5) / scale)
			else:
				layer.paste(fill, (0, 0, size, size))
			alpha = Image.new("RGBA", (size, size), (0, 0, 0, 0))
			alpha.paste(layer, (0, 0), mask)
			tile = Image.alpha_composite(tile, alpha)
		return tile


def arc_points(cx, cy, radius, start, end, steps):
	return [(cx + radius * math.cos(math.radians(start + (end - start) * i
		/ steps)), cy + radius * math.sin(math.radians(start + (end - start)
		* i / steps))) for i in range(steps + 1)]


def circle(cx, cy, r, steps=24):
	return arc_points(cx, cy, r, 0, 360, steps)[:-1]


def rdef_array(number, name, data):
	hexdata = data.hex().upper()
	lines = ["resource(%d, \"%s\") #'VICN' array {" % (number, name)]
	for i in range(0, len(hexdata), 64):
		lines.append("\t$\"%s\"" % hexdata[i:i + 64])
	lines.append("};")
	return "\n".join(lines)
