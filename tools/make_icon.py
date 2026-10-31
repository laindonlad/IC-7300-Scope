#!/usr/bin/env python3
"""Draws res/ic7300console.ico: the app's tuning knob, at every size Windows asks for.

Only needed to change the icon, as the result is checked in. Standard library only.
    python tools/make_icon.py [output.ico]
"""
import math
import os
import struct
import sys
import zlib

SIZES = [16, 20, 24, 32, 40, 48, 64, 128, 256]
LIGHT = -0.75 * math.pi  # from the top left, as the knob in the app is lit


def lit(t):
    return 0.5 + 0.5 * math.cos(t - LIGHT)


def over(dst, rgb, a):
    """Paints rgb at opacity a over dst, an (r, g, b, a) with straight alpha."""
    r, g, b, da = dst
    out = a + da * (1 - a)
    if out <= 0:
        return (0.0, 0.0, 0.0, 0.0)
    mix = lambda s, d: (s * a + d * da * (1 - a)) / out
    return (mix(rgb[0], r), mix(rgb[1], g), mix(rgb[2], b), out)


def shade(x, y, size):
    """The colour at x, y, each -1 to 1 from the centre. Small icons drop detail that would only blur."""
    d = math.hypot(x, y)
    t = math.atan2(y, x)
    px = 2.0 / size
    edge = lambda r: max(0.0, min(1.0, (r - d) / px + 0.5))  # soft edge of a disc of radius r
    c = (0.0, 0.0, 0.0, 0.0)

    # The rubber grip, with flutes on anything big enough to show them.
    skirt = 0.97
    if edge(skirt) > 0:
        v = 26 + 40 * lit(t)
        if size >= 32 and d > 0.70:
            flutes = 32 if size >= 64 else 20
            ridge = math.cos(t * flutes)
            v = v * 0.28 if ridge > 0.35 else v * 1.0 + (34 * lit(t) if ridge < -0.55 else 0)
        c = over(c, (v, v, v + 2), edge(skirt))

    # The shoulder, a bright ring facing the light, then the cap's dark surround.
    shoulder = 0.70
    if edge(shoulder) > 0:
        v = 24 + 110 * lit(t)
        c = over(c, (v, v, v), edge(shoulder))
    surround = 0.655
    if edge(surround) > 0:
        v = 88 - 62 * lit(t)
        c = over(c, (v, v, v), edge(surround))

    # The spun aluminium cap, whose grain shows bright wedges opposite each other.
    cap = 0.615
    if edge(cap) > 0:
        sheen = abs(math.cos(t - LIGHT)) ** 7
        second = abs(math.cos(t - LIGHT - 1.1)) ** 18
        v = min(255, 126 + 112 * sheen + 34 * second)
        c = over(c, (v, v, v + 2), edge(cap))

    # The finger dimple, a hollow, so it's lit the opposite way. It sits up and to the right of centre.
    dx, dy = x - 0.20, y + 0.25
    dd = math.hypot(dx, dy)
    well = 0.19 if size >= 32 else 0.24
    a = max(0.0, min(1.0, (well - dd) / px + 0.5))
    if a > 0:
        dt = math.atan2(dy, dx)
        v = 118 - 92 * lit(dt) if dd > well * 0.72 else 52 + 40 * (1 - lit(dt))
        c = over(c, (v, v, v), a)
    return c


def render(size):
    ss = 4 if size <= 128 else 3
    n = size * ss
    px = bytearray()
    cells = [[0.0, 0.0, 0.0, 0.0] for _ in range(size * size)]
    for sy in range(n):
        y = (sy + 0.5) / n * 2 - 1
        for sx in range(n):
            x = (sx + 0.5) / n * 2 - 1
            if x * x + y * y > 1.04:
                continue
            r, g, b, a = shade(x, y, size)
            cell = cells[(sy // ss) * size + sx // ss]
            cell[0] += r * a
            cell[1] += g * a
            cell[2] += b * a
            cell[3] += a
    count = ss * ss
    for r, g, b, a in cells:
        if a <= 0:
            px += bytes(4)
        else:
            px += bytes((min(255, int(r / a + 0.5)), min(255, int(g / a + 0.5)), min(255, int(b / a + 0.5)), min(255, int(a * 255 / count + 0.5))))
    return bytes(px)


def png(size, rgba):
    def chunk(kind, data):
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data))

    rows = b"".join(b"\0" + rgba[y * size * 4 : (y + 1) * size * 4] for y in range(size))
    head = struct.pack(">IIBBBBB", size, size, 8, 6, 0, 0, 0)
    return b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", head) + chunk(b"IDAT", zlib.compress(rows, 9)) + chunk(b"IEND", b"")


def dib(size, rgba):
    """The classic format, which small icons should use: every Windows component can read it."""
    rows = [rgba[y * size * 4 : (y + 1) * size * 4] for y in range(size)]
    xor = bytearray()
    for row in reversed(rows):
        for i in range(0, len(row), 4):
            xor += bytes((row[i + 2], row[i + 1], row[i], row[i + 3]))
    mask_stride = ((size + 31) // 32) * 4
    mask = bytearray()
    for row in reversed(rows):
        line = bytearray(mask_stride)
        for x in range(size):
            if row[x * 4 + 3] == 0:
                line[x // 8] |= 0x80 >> (x % 8)
        mask += line
    head = struct.pack("<IiiHHIIiiII", 40, size, size * 2, 1, 32, 0, len(xor) + len(mask), 0, 0, 0, 0)
    return head + bytes(xor) + bytes(mask)


def main():
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    out = sys.argv[1] if len(sys.argv) > 1 else os.path.join(root, "res", "ic7300console.ico")
    images = []
    for size in SIZES:
        rgba = render(size)
        images.append((size, png(size, rgba) if size == 256 else dib(size, rgba)))
        print("drew", size)
    offset = 6 + 16 * len(images)
    table, blobs = b"", b""
    for size, data in images:
        side = 0 if size == 256 else size  # 0 means 256 here
        table += struct.pack("<BBBBHHII", side, side, 0, 0, 1, 32, len(data), offset)
        blobs += data
        offset += len(data)
    with open(out, "wb") as f:
        f.write(struct.pack("<HHH", 0, 1, len(images)) + table + blobs)
    print("wrote", out, offset, "bytes")


if __name__ == "__main__":
    main()
