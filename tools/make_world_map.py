#!/usr/bin/env python3
"""Builds res/worldmap.bin, the map behind the log book's world view, from Natural Earth (public domain).

    pip install numpy mapbox_earcut
    make_world_map.py FOLDER_OF_GEOJSON res/worldmap.bin

The folder holds these files from github.com/nvkelso/natural-earth-vector/tree/master/geojson:
ne_{50m,10m}_{land,lakes,coastline,admin_0_boundary_lines_land}, ne_50m_admin_0_countries and
ne_10m_populated_places_simple.

The file, little-endian, is "WMP1", then the coarse level for the whole world and the fine level for zooming in,
each of them:
    land, lakes:     u32 batches, then for each: u16 verts, u32 indices, i16 lon/lat pairs, u16 triangle indices
    coast, borders:  u32 runs, then for each: u16 points, i16 lon/lat pairs
and after both levels the labels, countries and then towns, biggest first: u32 count, then for each: i16 lon, i16 lat, u8 kind (0 country,
1 town), u8 rank (lower is bigger), u8 length, the name in ASCII or Latin-1.
Longitudes are scaled so that 180 degrees is 32767, and latitudes so that 90 degrees is. Polygons are cut into
triangles here so the program needn't, and everything is grouped by area so it can skip what's off the screen.
"""
import json
import os
import struct
import sys
import unicodedata

import mapbox_earcut as earcut
import numpy as np

LEVELS = [("50m", 30, 0.0), ("10m", 10, 0.012)]  # data, grouping cell in degrees, simplifying tolerance in degrees
RUN_POINTS = 128
MAX_TOWN_RANK = 7


def load(folder, name):
    with open(os.path.join(folder, f"ne_{name}.geojson"), encoding="utf-8") as f:
        return json.load(f)["features"]


def simplify_open(p, tol):
    """Douglas-Peucker on an array of points, keeping both ends."""
    n = len(p)
    if tol <= 0 or n < 3:
        return p
    keep = np.zeros(n, bool)
    keep[0] = keep[-1] = True
    stack = [(0, n - 1)]
    while stack:
        a, b = stack.pop()
        if b - a < 2:
            continue
        d = p[b] - p[a]
        rel = p[a + 1 : b] - p[a]
        length = np.hypot(*d)
        dist = np.abs(d[0] * rel[:, 1] - d[1] * rel[:, 0]) / length if length > 0 else np.hypot(rel[:, 0], rel[:, 1])
        i = int(np.argmax(dist))
        if dist[i] > tol:
            m = a + 1 + i
            keep[m] = True
            stack += [(a, m), (m, b)]
    return p[keep]


def simplify_ring(p, tol):
    """The same for a closed ring without its repeated last point. None if nothing would be left of it."""
    if tol <= 0:
        return p
    if len(p) < 4:
        return p
    far = int(np.argmax(np.hypot(*(p - p[0]).T)))
    if far == 0:
        return None
    a = simplify_open(p[: far + 1], tol)
    b = simplify_open(np.vstack((p[far:], p[:1])), tol)
    out = np.vstack((a[:-1], b[:-1]))
    return out if len(out) >= 3 else None


def quantise(p):
    q = np.empty(p.shape, np.int16)
    q[:, 0] = np.round(np.clip(p[:, 0], -180, 180) * 32767 / 180)
    q[:, 1] = np.round(np.clip(p[:, 1], -90, 90) * 32767 / 90)
    return q


def polygons(features):
    for f in features:
        g = f["geometry"]
        if g["type"] == "Polygon":
            yield g["coordinates"]
        elif g["type"] == "MultiPolygon":
            yield from g["coordinates"]


def lines(features):
    for f in features:
        g = f["geometry"]
        if g["type"] == "LineString":
            yield g["coordinates"]
        elif g["type"] == "MultiLineString":
            yield from g["coordinates"]


def mesh(features, cell, tol):
    """Triangles from every polygon, grouped by the cell each one's middle is in."""
    groups = {}
    for poly in polygons(features):
        rings, ends, total = [], [], 0
        for i, ring in enumerate(poly):
            r = np.array(ring, float)[:, :2]
            if len(r) > 1 and (r[0] == r[-1]).all():
                r = r[:-1]
            r = simplify_ring(r, tol)
            if r is None or len(r) < 3:
                if i == 0:
                    break  # the outline has gone, so its holes go too
                continue
            rings.append(r)
            total += len(r)
            ends.append(total)
        if not rings:
            continue
        pts = np.vstack(rings)
        tris = earcut.triangulate_float64(pts, np.array(ends, np.uint32)).reshape(-1, 3)
        q = quantise(pts)
        mid = pts[tris].mean(axis=1)
        cx, cy = (mid[:, 0] // cell).astype(int), (mid[:, 1] // cell).astype(int)
        for key in set(zip(cx.tolist(), cy.tolist())):
            sel = tris[(cx == key[0]) & (cy == key[1])]
            groups.setdefault(key, []).append((q, sel))
    out = bytearray(struct.pack("<I", 0))
    batches = verts_total = tris_total = 0

    def flush(vs, idx):
        nonlocal batches, verts_total, tris_total, out
        if not idx:
            return
        out += struct.pack("<HI", len(vs), len(idx)) + np.array(vs, np.int16).tobytes() + np.array(idx, np.uint16).tobytes()
        batches += 1
        verts_total += len(vs)
        tris_total += len(idx) // 3

    for parts in groups.values():
        vs, idx, seen = [], [], {}
        for q, sel in parts:
            for tri in sel.tolist():
                if len(vs) > 65000:
                    flush(vs, idx)
                    vs, idx, seen = [], [], {}
                for v in tri:
                    k = (int(q[v, 0]), int(q[v, 1]))
                    i = seen.get(k)
                    if i is None:
                        i = seen[k] = len(vs)
                        vs.append(k)
                    idx.append(i)
        flush(vs, idx)
    out[:4] = struct.pack("<I", batches)
    return bytes(out), verts_total, tris_total


def runs(features, tol):
    out = bytearray(struct.pack("<I", 0))
    count = total = 0
    for line in lines(features):
        q = quantise(simplify_open(np.array(line, float)[:, :2], tol))
        q = q[np.r_[True, (q[1:] != q[:-1]).any(axis=1)]]
        if len(q) < 2:
            continue
        for s in range(0, len(q) - 1, RUN_POINTS - 1):
            part = q[s : s + RUN_POINTS]
            out += struct.pack("<H", len(part)) + part.tobytes()
            count += 1
            total += len(part)
    out[:4] = struct.pack("<I", count)
    return bytes(out), total


def latin1(name):
    """The fonts stop at Latin-1, so a letter outside it loses its accent."""
    try:
        return name.encode("latin-1")
    except UnicodeEncodeError:
        plain = unicodedata.normalize("NFKD", name)
        return plain.encode("ascii", "ignore")


def labels(folder):
    rows = []
    for f in load(folder, "50m_admin_0_countries"):
        p = f["properties"]
        rows.append((p["LABELRANK"], 0, p["LABEL_X"], p["LABEL_Y"], latin1(p["NAME"])))
    for f in load(folder, "10m_populated_places_simple"):
        p = f["properties"]
        if p["scalerank"] <= MAX_TOWN_RANK:
            rows.append((p["scalerank"], 1, p["longitude"], p["latitude"], latin1(p["nameascii"] or p["name"])))
    rows = [r for r in rows if r[4]]
    rows.sort(key=lambda r: (r[1], r[0]))
    out = bytearray(struct.pack("<I", len(rows)))
    for rank, kind, lon, lat, name in rows:
        name = name[:255]
        out += struct.pack("<hhBBB", round(lon * 32767 / 180), round(lat * 32767 / 90), kind, rank, len(name)) + name
    return bytes(out), len(rows)


def main():
    folder, target = sys.argv[1], sys.argv[2]
    out = bytearray(b"WMP1")
    for name, cell, tol in LEVELS:
        for layer in ("land", "lakes"):
            data, v, t = mesh(load(folder, f"{name}_{layer}"), cell, tol)
            out += data
            print(f"{name} {layer}: {v} vertices, {t} triangles, {len(data)} bytes")
        for layer in ("coastline", "admin_0_boundary_lines_land"):
            data, n = runs(load(folder, f"{name}_{layer}"), tol)
            out += data
            print(f"{name} {layer}: {n} points, {len(data)} bytes")
    data, n = labels(folder)
    out += data
    print(f"{n} labels, {len(data)} bytes")
    with open(target, "wb") as f:
        f.write(out)
    print(f"{target}: {len(out)} bytes")


if __name__ == "__main__":
    main()
