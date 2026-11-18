"""M12 "Yetenek YollarÄ±" effect meshes: small, low-poly, emissive static meshes in the same style as
the M9 ember bolt (`gen_effects.py`): one material, a tiny base-colour gradient and an emissive
gradient (UV V along the gradient), strength 2-4. Drawn by the client with
`WorldRenderer::addEffectMesh` (opaque, main pass, no shadow; its tint multiplies the BASE colour
only, the emissive is not tinted - so every tintable effect also comes pre-coloured, plus a
greyscale `_neutral` variant for when the renderer starts tinting the emissive too).

Usage (headless):
  blender --background --factory-startup --python tools/blender/gen_skill_effects.py -- [--sheets <dir>]
          [--no-preview]

Outputs: assets_src/effects/<name>.glb + <name>.png (base) + <name>_e.png (emissive). The manifest
(skills, scale, tint, duration, attach point) is docs/SKILL_ART.md Â§3. Contact sheet (not an asset):
<sheets>/m12_effects.png (default <repo>/build/art_sheets/m12, git-ignored).

Blender space as everywhere: Z up, -Y = engine +Z (forward). Projectiles point along engine +Z with
the origin at their widest ring; ground effects have their origin on the ground at the centre.
Deterministic.
"""

import math
import os
import sys

import bpy
import bmesh
import numpy as np

sys.dont_write_bytecode = True
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import vo_common as vc  # noqa: E402
from gen_effects import hexc, _lin, _srgb  # noqa: E402

EFFECTS_DIR = os.path.join(vc.SRC, "effects")
DEFAULT_SHEETS = os.path.join(vc.REPO, "build", "art_sheets", "m12")


# ---------------------------------------------------------------------------------------------
# Gradient textures (row 0 = V 0 in glTF = the "start" of the gradient)

def gradient(stops, n):
    s = (np.arange(n) + 0.5) / n
    col = np.zeros((n, 3))
    for (s0, c0), (s1, c1) in zip(stops, stops[1:]):
        m = (s >= s0) & (s <= s1)
        t = ((s[m] - s0) / max(s1 - s0, 1e-6))[:, None]
        col[m] = _srgb(_lin(hexc(c0)) * (1 - t) + _lin(hexc(c1)) * t)
    col[s < stops[0][0]] = hexc(stops[0][1])
    col[s > stops[-1][0]] = hexc(stops[-1][1])
    return col


def write_maps(name, columns, h=64, w=16, base_scale=0.6):
    """columns: list of stop lists, one per vertical band of the texture (U split evenly). The base
    colour is the emissive darkened in linear light. Returns (base path, emissive path)."""
    bands = [gradient(st, h) for st in columns]
    per = w // len(bands)
    emis = np.zeros((h, w, 3))
    for k, b in enumerate(bands):
        emis[:, k * per:(k + 1) * per] = b[:, None, :]
    base = _srgb(_lin(emis) * base_scale)
    pb = os.path.join(EFFECTS_DIR, f"{name}.png")
    pe = os.path.join(EFFECTS_DIR, f"{name}_e.png")
    vc.write_png(pb, vc.to_u8(base))
    vc.write_png(pe, vc.to_u8(emis))
    return pb, pe


def band_u(k, n):
    """U at the centre of band k of n."""
    return (k + 0.5) / n


def emissive_material(name, paths, strength):
    mat = bpy.data.materials.new(name)
    mat.use_nodes = True
    mat.use_backface_culling = True
    nt = mat.node_tree
    bsdf = nt.nodes.get("Principled BSDF")
    bsdf.inputs["Roughness"].default_value = 1.0
    bsdf.inputs["Metallic"].default_value = 0.0

    def tex(path):
        n = nt.nodes.new("ShaderNodeTexImage")
        n.image = bpy.data.images.load(path, check_existing=False)
        n.image.colorspace_settings.name = 'sRGB'
        n.interpolation = 'Linear'
        n.extension = 'EXTEND'
        return n

    nt.links.new(tex(paths[0]).outputs["Color"], bsdf.inputs["Base Color"])
    nt.links.new(tex(paths[1]).outputs["Color"], bsdf.inputs["Emission Color"])
    bsdf.inputs["Emission Strength"].default_value = strength
    return mat


# ---------------------------------------------------------------------------------------------
# Geometry builder: every face gets its own vertices (flat shading, per-corner UVs).

class Geo:
    def __init__(self):
        self.verts, self.faces, self.uvs = [], [], []

    def face(self, pts, uvs, double=False):
        i0 = len(self.verts)
        self.verts.extend(tuple(float(c) for c in p) for p in pts)
        self.faces.append(tuple(range(i0, i0 + len(pts))))
        self.uvs.append([(float(u), 1.0 - float(v)) for (u, v) in uvs])  # v: 0 = image row 0
        if double:
            self.face(pts[::-1], uvs[::-1])

    def tris(self):
        return sum(len(f) - 2 for f in self.faces)

    def build(self, name, smooth=False):
        me = bpy.data.meshes.new(name)
        me.from_pydata(self.verts, [], self.faces)
        uvl = me.uv_layers.new(name="UVMap")
        for poly in me.polygons:
            for k, li in enumerate(poly.loop_indices):
                uvl.data[li].uv = self.uvs[poly.index][k]
        me.update()
        if smooth:
            me.shade_smooth()
        else:
            me.shade_flat()
        obj = bpy.data.objects.new(name, me)
        bpy.context.scene.collection.objects.link(obj)
        return obj


def lathe(g, profile, sides, axis="y", twist=0.0, wave=0.0, u=0.5, phase=0.0, flip=False):
    """profile: list of (s, along, radius); s -> V. axis 'y': along = Blender y; 'z': along = z.
    Faces wind outward (`flip` reverses)."""
    rings = []
    for (s, along, r) in profile:
        ring = []
        for j in range(sides):
            a = 2 * math.pi * j / sides + twist * s + phase
            rr = r * (1.0 + wave * math.sin(3 * a + 5 * s))
            c, sn = rr * math.cos(a), rr * math.sin(a)
            ring.append((c, along, sn) if axis == "y" else (c, sn, along))
        rings.append((s, ring))
    for (s0, A), (s1, B) in zip(rings, rings[1:]):
        for j in range(sides):
            j2 = (j + 1) % sides
            pts = [A[j], A[j2], B[j2], B[j]]
            uv = [(u, s0), (u, s0), (u, s1), (u, s1)]
            if axis == "y":
                pts, uv = pts[::-1], uv[::-1]
            if flip:
                pts, uv = pts[::-1], uv[::-1]
            # Drop degenerate corners at the poles.
            keep = [k for k in range(4) if k == 0 or np.linalg.norm(np.subtract(pts[k], pts[k - 1])) > 1e-7]
            if len(keep) == 4 and np.linalg.norm(np.subtract(pts[3], pts[0])) < 1e-7:
                keep = keep[:3]
            if len(keep) >= 3:
                g.face([pts[k] for k in keep], [uv[k] for k in keep])


# ---------------------------------------------------------------------------------------------
# Palettes (emissive gradients, start -> end)

FROST = [(0.0, "#ffffff"), (0.2, "#d8f4ff"), (0.5, "#7cc8f4"), (0.8, "#2a78c8"), (1.0, "#123a78")]
EMBER = [(0.0, "#fff8e0"), (0.2, "#ffd25a"), (0.5, "#ff8a1e"), (0.8, "#d0380c"), (1.0, "#701404")]
LIGHT = [(0.0, "#ffffff"), (0.25, "#fff4d0"), (0.6, "#ffd070"), (1.0, "#8a5a10")]
SHADOW = [(0.0, "#e0b8ff"), (0.3, "#9a5ae0"), (0.7, "#3a1a68"), (1.0, "#0a0414")]

RING_TINTS = {   # (band stops inner -> outer, teeth stops base -> tip, strength)
    "flame": ([(0.0, "#401004"), (0.5, "#ff9a30"), (0.75, "#fff0a0"), (1.0, "#c03008")],
              [(0.0, "#fff0a0"), (0.4, "#ff8a20"), (1.0, "#801804")], 3.0),
    "frost": ([(0.0, "#0a2040"), (0.5, "#6ac0f0"), (0.75, "#ffffff"), (1.0, "#2a70c0")],
              [(0.0, "#ffffff"), (0.4, "#8ad4ff"), (1.0, "#1a4a90")], 2.5),
    "heal": ([(0.0, "#2a2408"), (0.5, "#e8d070"), (0.75, "#fffbe0"), (1.0, "#8ac060")],
             [(0.0, "#fffbe0"), (0.4, "#f0d880"), (1.0, "#5a9a40")], 2.5),
    "fog": ([(0.0, "#141a0c"), (0.5, "#7a9050"), (0.75, "#c0d090"), (1.0, "#4a5a30")],
            [(0.0, "#c0d090"), (0.5, "#7a9050"), (1.0, "#2a3418")], 1.4),
    "neutral": ([(0.0, "#101010"), (0.5, "#a0a0a0"), (0.75, "#ffffff"), (1.0, "#606060")],
                [(0.0, "#ffffff"), (0.4, "#b0b0b0"), (1.0, "#303030")], 2.5),
}

AURA_TINTS = {   # mote stops centre -> tip, strength
    "gold": ([(0.0, "#fffbe0"), (0.4, "#ffd860"), (1.0, "#8a5a10")], 3.0),      # attack up, prayer
    "red": ([(0.0, "#fff0e0"), (0.4, "#ff5a30"), (1.0, "#701008")], 3.0),       # rage
    "steel": ([(0.0, "#ffffff"), (0.4, "#a8c4e0"), (1.0, "#304a68")], 2.5),     # defense, shield wall
    "green": ([(0.0, "#f4ffe0"), (0.4, "#a8e040"), (1.0, "#2a5a10")], 2.5),     # haste, dodge
    "neutral": ([(0.0, "#ffffff"), (0.4, "#c0c0c0"), (1.0, "#404040")], 2.5),
}


# ---------------------------------------------------------------------------------------------
# Effects

def fx_frost_bolt():
    """KÄ±raÄŸÄ± Oku: a faceted ice spear, 0.55 m, nose along engine +Z, three swept-back shards."""
    g = Geo()
    L = 0.55
    nose = -0.2 * L
    prof = [(0.0, nose, 0.0), (0.2, nose + 0.2 * L, 0.06), (0.45, nose + 0.45 * L, 0.045), (1.0, nose + L, 0.0)]
    lathe(g, prof, 6, axis="y", phase=math.radians(30))
    for k in range(3):
        a = math.radians(90 + k * 120)
        ca, sa = math.cos(a), math.sin(a)
        r0, r1 = 0.035, 0.085
        y0, y1, y2 = nose + 0.3 * L, nose + 0.55 * L, nose + 0.42 * L
        base1 = (ca * r0 - sa * 0.012, y0, sa * r0 + ca * 0.012)
        base2 = (ca * r0 + sa * 0.012, y0, sa * r0 - ca * 0.012)
        root = (ca * r0 * 0.6, y1, sa * r0 * 0.6)
        tip = (ca * r1, y2, sa * r1)
        for tri in ((base1, tip, root), (base2, root, tip), (base1, base2, tip)):
            g.face(tri, [(0.5, 0.3), (0.5, 0.55), (0.5, 0.45)], double=True)
    obj = g.build("frost_bolt")
    paths = write_maps("frost_bolt", [FROST])
    return obj, paths, 3.0, g.tris()


def fx_ember_mote():
    """KÃ¶z YaÄŸmuru element: a small falling ember streak, 0.32 m, nose along engine +Z."""
    g = Geo()
    L, R = 0.32, 0.045
    nose = -0.24 * L
    prof = []
    for s in (0.0, 0.06, 0.14, 0.24, 0.4, 0.6, 0.8, 1.0):
        r = R * math.sin(math.acos(1.0 - s / 0.24)) if s < 0.24 else R * (1.0 - (s - 0.24) / 0.76) ** 1.3
        prof.append((s, nose + s * L, r))
    lathe(g, prof, 6, axis="y", twist=1.4, wave=0.15)
    obj = g.build("ember_mote", smooth=True)
    paths = write_maps("ember_mote", [EMBER])
    return obj, paths, 4.0, g.tris()


def fx_light_strike():
    """IÅŸÄ±k Ã‡arpmasÄ±: a 4 m lance of light standing on the target, a four-fin burst and a ground star."""
    g = Geo()
    prof = [(0.05, 0.0, 0.2), (0.35, 1.2, 0.13), (0.7, 2.8, 0.08), (1.0, 4.0, 0.03)]
    lathe(g, prof, 6, axis="z", u=band_u(0, 2))
    for k in range(4):
        a = math.radians(k * 45)
        ca, sa = math.cos(a), math.sin(a)
        P = lambda d, z: (ca * d, sa * d, z)  # noqa: E731
        g.face([P(-0.75, 0.12), P(0, -0.04), P(0.75, 0.12), P(0, 0.7)],
               [(band_u(1, 2), 0.9), (band_u(1, 2), 0.5), (band_u(1, 2), 0.9), (band_u(1, 2), 0.05)], double=True)
    star = []
    for k in range(16):
        a = math.radians(k * 22.5)
        r = 0.9 if k % 4 == 0 else (0.5 if k % 2 == 0 else 0.22)
        star.append((r * math.cos(a), r * math.sin(a), 0.015))
    for k in range(16):
        g.face([(0, 0, 0.015), star[k], star[(k + 1) % 16]],
               [(band_u(1, 2), 0.0), (band_u(1, 2), 0.95), (band_u(1, 2), 0.95)])
    obj = g.build("light_strike")
    paths = write_maps("light_strike", [LIGHT, [(0.0, "#ffffff"), (0.4, "#fff0b0"), (1.0, "#e09020")]])
    return obj, paths, 4.0, g.tris()


def ring_geo():
    g = Geo()
    n, r0, r1, z = 48, 0.86, 1.0, 0.02
    for k in range(n):
        a0, a1 = 2 * math.pi * k / n, 2 * math.pi * (k + 1) / n
        p = lambda r, a: (r * math.cos(a), r * math.sin(a), z)  # noqa: E731
        g.face([p(r0, a0), p(r1, a0), p(r1, a1), p(r0, a1)],
               [(band_u(0, 2), 0.0), (band_u(0, 2), 1.0), (band_u(0, 2), 1.0), (band_u(0, 2), 0.0)])
    for k in range(24):
        a = 2 * math.pi * (k + 0.5) / 24
        ca, sa = math.cos(a), math.sin(a)
        tx, ty = -sa, ca
        h = 0.2 if k % 2 == 0 else 0.12
        w = 0.09
        c = (0.96 * ca, 0.96 * sa)
        lean = 0.04
        g.face([(c[0] - tx * w, c[1] - ty * w, z), (c[0] + tx * w, c[1] + ty * w, z),
                (c[0] + ca * lean, c[1] + sa * lean, z + h)],
               [(band_u(1, 2), 0.0), (band_u(1, 2), 0.0), (band_u(1, 2), 1.0)], double=True)
    return g


def fx_ground_rings():
    out = []
    for tint, (band, teeth, strength) in RING_TINTS.items():
        g = ring_geo()
        name = f"ground_ring_{tint}"
        obj = g.build(name)
        paths = write_maps(name, [band, teeth], h=32, w=8)
        out.append((obj, paths, strength, g.tris()))
    return out


def bubble_geo():
    """Geodesic lattice: the edges of a 2x subdivided icosahedron as thin triangular struts,
    radius 1 m, centre 1 m above the origin."""
    bm = bmesh.new()
    bmesh.ops.create_icosphere(bm, subdivisions=2, radius=1.0)
    edges = [(tuple(e.verts[0].co), tuple(e.verts[1].co)) for e in bm.edges]
    bm.free()
    edges.sort()
    g = Geo()
    w = 0.016
    for (a, b) in edges:
        A, B = np.array(a), np.array(b)
        d = B - A
        d /= np.linalg.norm(d)
        out = (A + B) / 2
        out /= np.linalg.norm(out)
        side = np.cross(d, out)
        corners = [out * w, side * w * 0.9 - out * w * 0.5, -side * w * 0.9 - out * w * 0.5]
        va = [(A + c) + np.array([0, 0, 1.0]) for c in corners]
        vb = [(B + c) + np.array([0, 0, 1.0]) for c in corners]
        for k in range(3):
            k2 = (k + 1) % 3
            za = (va[k][2] + vb[k][2]) / 2
            v = float(np.clip(1.0 - za / 2.0, 0, 1))
            pts = [va[k], va[k2], vb[k2], vb[k]]
            # Winding: outward normal.
            n_ = np.cross(np.subtract(pts[1], pts[0]), np.subtract(pts[2], pts[0]))
            mid = np.mean(pts, axis=0) - (A + B) / 2 - np.array([0, 0, 1.0])
            if np.dot(n_, mid) < 0:
                pts = pts[::-1]
            g.face(pts, [(0.5, v)] * 4)
    return g


# Buz Kabuğu ice shell: texture atlas of triangular ice plates, FROST_COLS variants x 3 frost rows
# (0 clear, 1 medium, 2 frosted: the band near the feet), FROST_CELL px per cell. Every facet of the
# shell maps onto the plate triangle FROST_TRI of one cell (cell-local, image rows downwards).
FROST_COLS, FROST_ROWS, FROST_CELL = 4, 3, 128
FROST_TRI = ((0.07, 0.11), (0.93, 0.11), (0.5, 0.89))


def _hash01(*k):
    """Deterministic pseudo-random number in [0, 1) from integers."""
    h = 2166136261
    for x in k:
        h = ((h ^ (int(x) & 0xFFFFFFFF)) * 16777619) & 0xFFFFFFFF
    h ^= h >> 13
    h = (h * 0x5BD1E995) & 0xFFFFFFFF
    h ^= h >> 15
    return h / 4294967296.0


def frost_shell_geo():
    """A closed, faceted ice shell: a geodesic sphere of 320 triangular plates with each corner pushed
    in or out a little so the plates sit at slightly different angles, radius ~1 m, centre 1 m above
    the origin (the feet). Double-sided: first every plate wound inwards (the far wall, seen through
    the near one), then every plate wound outwards, so the draw order is back to front for a
    translucent draw that does not write depth."""
    bm = bmesh.new()
    bmesh.ops.create_icosphere(bm, subdivisions=3, radius=1.0)   # bmesh: 3 -> 320 faces
    bm.verts.ensure_lookup_table()
    key = {}
    for v in bm.verts:   # sort key independent of bmesh's vertex order
        key[v.index] = tuple(round(c, 5) for c in v.co)
    order = sorted(key, key=lambda i: key[i])
    rank = {i: r for r, i in enumerate(order)}
    pos = {}
    for v in bm.verts:
        p = np.array(v.co, float)
        p /= np.linalg.norm(p)
        r = 1.0 + 0.035 * (2.0 * _hash01(rank[v.index], 7) - 1.0)
        pos[v.index] = p * r
    faces = []
    for f in bm.faces:
        idx = [v.index for v in f.verts]
        faces.append(idx)
    bm.free()
    faces.sort(key=lambda f: tuple(sorted(rank[i] for i in f)))
    centre = np.array([0.0, 0.0, 1.0])
    plates = []
    for k, f in enumerate(faces):
        P = [pos[i] for i in f]
        n = np.cross(P[1] - P[0], P[2] - P[0])
        if np.dot(n, np.mean(P, axis=0)) < 0:      # outward winding
            P = P[::-1]
        zc = float(np.mean(P, axis=0)[2])           # -1 (feet) .. 1 (top)
        row = 2 if zc < -0.6 else (1 if zc < -0.25 else 0)
        if row == 0 and _hash01(k, 3) < 0.12:        # a few frosted plates higher up
            row = 1
        col = int(_hash01(k, 5) * FROST_COLS)
        rot = int(_hash01(k, 11) * 3)
        cu, cv = col / FROST_COLS, row / FROST_ROWS
        uv = [(cu + t[0] / FROST_COLS, cv + t[1] / FROST_ROWS) for t in FROST_TRI]
        uv = uv[rot:] + uv[:rot]
        plates.append(([p + centre for p in P], uv))
    g = Geo()
    for P, uv in plates:
        g.face(P[::-1], uv[::-1])
    for P, uv in plates:
        g.face(P, uv)
    return g


def _value_noise(h, w, freq, rng):
    """Smooth value noise in [0, 1] over an h x w image, `freq` lattice cells across the width."""
    gh, gw = int(math.ceil(freq * h / w)) + 2, freq + 2
    lat = rng.random_sample((gh, gw))
    y = (np.arange(h) + 0.5) / w * freq
    x = (np.arange(w) + 0.5) / w * freq
    y0, x0 = np.floor(y).astype(int), np.floor(x).astype(int)
    ty, tx = y - y0, x - x0
    ty, tx = ty * ty * (3 - 2 * ty), tx * tx * (3 - 2 * tx)
    a = lat[y0][:, x0]
    b = lat[y0][:, x0 + 1]
    c = lat[y0 + 1][:, x0]
    d = lat[y0 + 1][:, x0 + 1]
    top = a + (b - a) * tx[None, :]
    bot = c + (d - c) * tx[None, :]
    return top + (bot - top) * ty[:, None]


def _seg_dist(px, py, a, b):
    ab = np.subtract(b, a)
    t = np.clip(((px - a[0]) * ab[0] + (py - a[1]) * ab[1]) / max(float(np.dot(ab, ab)), 1e-9), 0, 1)
    return np.hypot(px - (a[0] + t * ab[0]), py - (a[1] + t * ab[1]))


def frost_shell_maps(name):
    """Base colour RGBA (alpha = translucency, the client multiplies its draw opacity by it) and
    emissive maps of the ice plates. Per plate: a thin bright rim along its edges (the seams between
    plates), feathery frost growing in from the edges, a few branching cracks, faint inner sparkle.
    Alpha ~0.15-0.3 over the clear plates, 0.6-0.85 on rims and cracks, higher frost on the lower
    rows. Returns (base path, emissive path)."""
    C = FROST_CELL
    W, H = C * FROST_COLS, C * FROST_ROWS
    alpha = np.zeros((H, W))
    white = np.zeros((H, W))      # 0 = clear ice tint, 1 = white frost
    glow = np.zeros((H, W))       # emissive weight
    yy, xx = np.mgrid[0:C, 0:C].astype(float) + 0.5
    T = np.array(FROST_TRI) * C
    # Barycentrics of the cell's pixels in the plate triangle.
    (x0, y0), (x1, y1), (x2, y2) = T
    det = (y1 - y2) * (x0 - x2) + (x2 - x1) * (y0 - y2)
    l0 = ((y1 - y2) * (xx - x2) + (x2 - x1) * (yy - y2)) / det
    l1 = ((y2 - y0) * (xx - x2) + (x0 - x2) * (yy - y2)) / det
    l2 = 1.0 - l0 - l1
    height = abs(det) / max(np.hypot(x1 - x0, y1 - y0), 1e-6)   # ~ triangle height in px
    dpx = np.clip(np.minimum(np.minimum(l0, l1), l2), 0.0, None) * height  # px to the nearest edge
    de = [np.clip(l, 0.0, None) * height for l in (l0, l1, l2)]  # px to each edge
    inside = np.minimum(np.minimum(l0, l1), l2) > -0.02
    for row in range(FROST_ROWS):
        for col in range(FROST_COLS):
            rng = np.random.RandomState(9100 + row * 17 + col)
            n1 = _value_noise(C, C, 6, rng)
            n2 = _value_noise(C, C, 14, rng)
            n3 = _value_noise(C, C, 32, rng)
            fbm = 0.5 * n1 + 0.3 * n2 + 0.2 * n3
            ridged = (1.0 - np.abs(2.0 * n2 - 1.0)) ** 6 * 0.6 + (1.0 - np.abs(2.0 * n3 - 1.0)) ** 8 * 0.5
            # Seam rim: a crisp line and a softer halo inside it, one strength per edge (the
            # facets rotate, so strong and faint seams mix into irregular larger plates).
            ew = [(1.0, 0.45, 0.2), (0.9, 0.2, 0.55), (0.25, 1.0, 0.35), (0.5, 0.3, 0.95)][col]
            rim = np.zeros((C, C))
            for w_, d_ in zip(ew, de):
                line = np.exp(-(d_ / 1.4) ** 2) + 0.3 * np.exp(-d_ / 4.0)
                rim = np.maximum(rim, w_ * line * (0.75 + 0.25 * n2))
            rim = np.clip(rim, 0.0, 1.0)
            # Frost feathers growing in from the edges (further on the frosted rows).
            reach = (10.0, 15.0, 28.0)[row]
            feather = np.clip(ridged * 1.6 + fbm * 0.35 - 0.25, 0, 1) * np.exp(-dpx / reach)
            feather *= np.maximum.reduce([w_ * np.exp(-d_ / reach) for w_, d_ in zip(ew, de)]) / np.maximum(
                np.exp(-dpx / reach), 1e-6) * 0.5 + 0.5   # more frost along the strong seams
            # Cracks: a few branching polylines from a random point near the plate's middle.
            crack = np.zeros((C, C))
            ncr = (0, 1, 2)[row] + int(rng.randint(0, 3))
            cx, cy = T[:, 0].mean(), T[:, 1].mean()
            for _ in range(ncr):
                # From a point on an edge, across the plate towards (roughly) its middle.
                e = rng.randint(0, 3)
                t = rng.uniform(0.2, 0.8)
                p = T[e] * (1 - t) + T[(e + 1) % 3] * t
                ang = math.atan2(cy - p[1], cx - p[0]) + rng.uniform(-0.5, 0.5)
                width = rng.uniform(0.9, 1.5)
                for seg in range(rng.randint(2, 5)):
                    ang += rng.uniform(-0.5, 0.5)
                    q = p + rng.uniform(10, 22) * np.array([math.cos(ang), math.sin(ang)])
                    crack = np.maximum(crack, np.exp(-(_seg_dist(xx, yy, p, q) / width) ** 2))
                    if rng.random_sample() < 0.45:   # a side branch
                        b = ang + rng.choice([-1, 1]) * rng.uniform(0.6, 1.2)
                        q2 = p + rng.uniform(5, 10) * np.array([math.cos(b), math.sin(b)])
                        crack = np.maximum(crack, 0.8 * np.exp(-(_seg_dist(xx, yy, p, q2) / (width * 0.8)) ** 2))
                    p = q
            crack *= inside
            sparkle = np.clip((n3 - 0.72) * 4.0, 0, 1) * 0.5
            base_a = (0.15, 0.22, 0.38)[row] + 0.08 * fbm + (0.0, 0.05, 0.18)[row] * n1
            a = base_a + 0.12 * sparkle
            a = np.maximum(a, 0.80 * rim)
            a = np.maximum(a, 0.8 * crack)
            a = a + (0.30, 0.35, 0.45)[row] * feather
            a = np.clip(a, 0.0, 0.85)
            wt = np.clip(np.maximum.reduce([rim, crack, feather * 1.2, sparkle]) + (0.0, 0.15, 0.4)[row] * fbm, 0, 1)
            gl = np.clip(0.9 * rim + 0.8 * crack + 0.35 * feather + 0.5 * sparkle, 0, 1)
            sl = (slice(row * C, (row + 1) * C), slice(col * C, (col + 1) * C))
            alpha[sl], white[sl], glow[sl] = a, wt, gl
    tint_lo, tint_hi = np.array(vc.hex_linear("#9fd4ec")), np.array(vc.hex_linear("#f2fbff"))
    base = tint_lo * (1 - white[..., None]) + tint_hi * white[..., None]
    e_lo, e_hi = np.array(vc.hex_linear("#0a2230")), np.array(vc.hex_linear("#6cc4ec"))
    emis = e_lo * (1 - glow[..., None]) + e_hi * glow[..., None]
    pb, pe = os.path.join(EFFECTS_DIR, f"{name}.png"), os.path.join(EFFECTS_DIR, f"{name}_e.png")
    rgba = np.concatenate([vc.to_u8(_srgb(base)), np.round(alpha * 255.0).astype(np.uint8)[..., None]], -1)
    vc.write_png(pb, rgba)
    vc.write_png(pe, vc.to_u8(_srgb(emis)))
    return pb, pe


def fx_shield_bubbles():
    out = []
    for tint, stops, strength in (
            ("gold", [(0.0, "#fffbe0"), (0.5, "#ffd060"), (1.0, "#a06a10")], 2.5),
            ("frost", None, 1.0),
            ("neutral", [(0.0, "#ffffff"), (0.5, "#c0c0c0"), (1.0, "#505050")], 2.5)):
        if stops is None:   # Buz Kabuğu: a translucent ice shell, not a lattice
            g = frost_shell_geo()
            name = f"shield_bubble_{tint}"
            obj = g.build(name)
            paths = frost_shell_maps(name)
            out.append((obj, paths, strength, g.tris()))
            continue
        g = bubble_geo()
        name = f"shield_bubble_{tint}"
        obj = g.build(name)
        paths = write_maps(name, [stops], h=32, w=4)
        out.append((obj, paths, strength, g.tris()))
    return out


def aura_geo():
    g = Geo()
    n = 32
    for k in range(n):
        a0, a1 = 2 * math.pi * k / n, 2 * math.pi * (k + 1) / n
        p = lambda r, a: (r * math.cos(a), r * math.sin(a), 0.02)  # noqa: E731
        g.face([p(0.5, a0), p(0.56, a0), p(0.56, a1), p(0.5, a1)], [(0.5, 0.35)] * 4)
    for k in range(12):
        a = math.radians(k * 30)
        ca, sa = math.cos(a), math.sin(a)
        cx, cy = 0.55 * ca, 0.55 * sa
        z0 = 0.12 + ((k * 0.37) % 1.0) * 1.2
        h, w = 0.26 + 0.06 * (k % 3), 0.045
        for (ux, uy) in ((-sa, ca), (ca, sa)):
            pts = [(cx, cy, z0), (cx + ux * w, cy + uy * w, z0 + h * 0.45), (cx, cy, z0 + h),
                   (cx - ux * w, cy - uy * w, z0 + h * 0.45)]
            g.face(pts, [(0.5, 0.9), (0.5, 0.15), (0.5, 0.9), (0.5, 0.15)], double=True)
    return g


def fx_auras():
    out = []
    for tint, (stops, strength) in AURA_TINTS.items():
        g = aura_geo()
        name = f"buff_aura_{tint}"
        obj = g.build(name)
        paths = write_maps(name, [stops], h=32, w=4)
        out.append((obj, paths, strength, g.tris()))
    return out


def fx_tick_wisps():
    out = []
    for tint, stops, strength in (
            ("burn", EMBER, 3.0),
            ("poison", [(0.0, "#f4ffc0"), (0.25, "#c0f040"), (0.6, "#4a9a10"), (1.0, "#1a3a04")], 2.5)):
        g = Geo()
        H, R = 0.3, 0.07
        prof = []
        for s in (0.0, 0.08, 0.2, 0.35, 0.55, 0.75, 1.0):   # s = 0 bottom (round) -> 1 tip (top)
            r = R * math.sin(math.acos(1.0 - s / 0.3)) if s < 0.3 else R * (1.0 - (s - 0.3) / 0.7) ** 1.2
            prof.append((s, s * H, r))
        lathe(g, prof, 6, axis="z", twist=2.0, wave=0.18)
        name = f"tick_wisp_{tint}"
        obj = g.build(name, smooth=True)
        paths = write_maps(name, [stops], h=32, w=4)
        out.append((obj, paths, strength, g.tris()))
    return out


def fx_stun_marker():
    """Three four-pointed stars (crossed, double-sided) on a thin ring, origin at the ring centre."""
    g = Geo()
    for k in range(3):
        a = math.radians(k * 120 + 90)
        cx, cy = 0.2 * math.cos(a), 0.2 * math.sin(a)
        for (ux, uy) in ((-math.sin(a), math.cos(a)), (math.cos(a), math.sin(a))):
            pts = []
            for j in range(8):
                b = math.radians(j * 45 + 90)
                r = 0.075 if j % 2 == 0 else 0.026
                h, v = r * math.cos(b), r * math.sin(b)
                pts.append((cx + ux * h, cy + uy * h, v))
            for j in range(8):
                g.face([(cx, cy, 0.0), pts[j], pts[(j + 1) % 8]], [(0.5, 0.0), (0.5, 0.8), (0.5, 0.8)], double=True)
    n = 24
    for k in range(n):
        a0, a1 = 2 * math.pi * k / n, 2 * math.pi * (k + 1) / n
        p = lambda r, a: (r * math.cos(a), r * math.sin(a), 0.0)  # noqa: E731
        g.face([p(0.19, a0), p(0.215, a0), p(0.215, a1), p(0.19, a1)], [(0.5, 0.45)] * 4, double=True)
    obj = g.build("stun_marker")
    paths = write_maps("stun_marker", [[(0.0, "#ffffff"), (0.4, "#fff090"), (1.0, "#e0a020")]], h=32, w=4)
    return obj, paths, 3.0, g.tris()


def fx_root_spikes():
    """Donduran Nefes root: seven ice spikes round the feet, leaning out a little."""
    g = Geo()
    for k in range(7):
        a = math.radians(k * (360 / 7) + 10)
        ca, sa = math.cos(a), math.sin(a)
        r = 0.34 + 0.04 * (k % 2)
        cx, cy = r * ca, r * sa
        h = 0.26 + 0.09 * ((k * 3) % 4) / 3
        bw = 0.06
        tip = (cx + ca * 0.08, cy + sa * 0.08, h)
        base = []
        for j in range(4):
            b = math.radians(j * 90 + 45 + k * 13)
            base.append((cx + bw * math.cos(b), cy + bw * math.sin(b), -0.03))
        for j in range(4):
            g.face([base[j], base[(j + 1) % 4], tip], [(0.5, 0.95), (0.5, 0.95), (0.5, 0.05)])
    obj = g.build("root_spikes")
    paths = write_maps("root_spikes", [FROST], h=32, w=4, base_scale=0.9)
    return obj, paths, 1.6, g.tris()


def fx_leap_trail():
    """GÃ¶lge AdÄ±mÄ± trail: a crossed ribbon 1 m long, origin at its head (the arriving end), the tail
    along engine -Z; scale engine Z to the leap length."""
    g = Geo()
    n = 8
    for plane in (0, 1):
        for k in range(n):
            s0, s1 = k / n, (k + 1) / n
            w0 = 0.32 * (1 - s0) ** 0.7 + 0.01
            w1 = 0.32 * (1 - s1) ** 0.7 + 0.01
            wob0, wob1 = 0.03 * math.sin(s0 * 9), 0.03 * math.sin(s1 * 9)
            if plane == 0:  # horizontal
                pts = [(-w0 + wob0, s0, 0), (w0 + wob0, s0, 0), (w1 + wob1, s1, 0), (-w1 + wob1, s1, 0)]
            else:           # vertical
                pts = [(wob0, s0, -w0), (wob0, s0, w0), (wob1, s1, w1), (wob1, s1, -w1)]
            g.face(pts, [(0.0, s0), (1.0, s0), (1.0, s1), (0.0, s1)], double=True)
    obj = g.build("leap_trail")
    # U across the ribbon: bright violet edges, dark core; V along: fade towards the tail.
    h, w = 32, 16
    u = (np.arange(w) + 0.5) / w
    v = (np.arange(h) + 0.5) / h
    edge = np.abs(u - 0.5) * 2.0
    lo, hi = _lin(hexc("#140828")), _lin(hexc("#c890ff"))
    k = (edge[None, :] ** 3)[..., None] * (1.0 - v[:, None, None]) ** 1.5
    emis = _srgb(lo * (1 - k) + hi * k)
    base = _srgb(_lin(emis) * 0.5)
    pb, pe = os.path.join(EFFECTS_DIR, "leap_trail.png"), os.path.join(EFFECTS_DIR, "leap_trail_e.png")
    vc.write_png(pb, vc.to_u8(base))
    vc.write_png(pe, vc.to_u8(emis))
    return obj, (pb, pe), 3.0, g.tris()


def fx_fog_cloud():
    """Sis BombasÄ± cloud: seven lumpy half-buried puffs, lit (no emissive), grey-green base colour:
    the renderer's tint works on it directly. Radius ~1 m, origin on the ground at the centre."""
    mb = vc.MeshBuilder()
    puffs = [(0.0, 0.0, 0.1, 0.55), (0.55, 0.15, 0.0, 0.42), (-0.5, 0.25, 0.0, 0.45), (0.2, -0.55, 0.0, 0.44),
             (-0.3, -0.45, 0.0, 0.4), (0.15, 0.6, 0.0, 0.38), (-0.65, -0.1, -0.05, 0.36)]
    for (x, y, z, r) in puffs:
        mb.ellipsoid((x, y, z), (r, r, r * 0.6), 8, 5, 0, {},
                     shape=lambda u: (1.0 + 0.08 * math.sin(5 * u[0] + 3 * u[1]),) * 3)
    obj = mb.build("fog_cloud", [vc.material("fog_cloud", "#c4ceb0", roughness=1.0)], smooth=True)
    return obj, None, 0.0, vc.triangle_count(obj)


# ---------------------------------------------------------------------------------------------

def export(obj, paths, strength):
    if paths is not None:
        obj.data.materials.append(emissive_material(obj.name, paths, strength))
        vc.export_glb(os.path.join(EFFECTS_DIR, f"{obj.name}.glb"), [obj], textures=True)
    else:
        vc.export_glb(os.path.join(EFFECTS_DIR, f"{obj.name}.glb"), [obj])


def preview_sheet(objs, sheet_path):
    """EEVEE render of each effect on a dark ground, a grey 1.8 m figure for scale where it belongs."""
    sc = bpy.context.scene
    cam = vc.setup_preview((400, 300), background="#14161b", sun_dir=(-0.5, -1.0, 0.8), strength=1.5)
    try:
        sc.eevee.use_bloom = True
    except AttributeError:
        pass
    ground_mat = vc.material("pv_ground", "#2a2e36", roughness=1.0)
    bpy.ops.mesh.primitive_plane_add(size=12, location=(0, 0, 0))
    ground = bpy.context.active_object
    ground.data.materials.append(ground_mat)
    fig_mat = vc.material("pv_figure", "#6a6e76")
    bpy.ops.mesh.primitive_cylinder_add(vertices=12, radius=0.22, depth=1.4, location=(0, 0, 0.7))
    body = bpy.context.active_object
    bpy.ops.mesh.primitive_uv_sphere_add(segments=12, ring_count=8, radius=0.14, location=(0, 0, 1.6))
    head = bpy.context.active_object
    for o in (body, head):
        o.data.materials.append(fig_mat)
    # Label: white emissive text in front of the camera.
    curve = bpy.data.curves.new("pv_label", 'FONT')
    curve.size = 0.11
    label = bpy.data.objects.new("pv_label", curve)
    sc.collection.objects.link(label)
    label.parent = cam
    label.location = (-0.98, 0.62, -2.5)
    tm = bpy.data.materials.new("pv_text")
    tm.use_nodes = True
    tb = tm.node_tree.nodes.get("Principled BSDF")
    tb.inputs["Emission Color"].default_value = (1, 1, 1, 1)
    tb.inputs["Emission Strength"].default_value = 1.0
    tb.inputs["Base Color"].default_value = (0, 0, 0, 1)
    curve.materials.append(tm)
    tiles = []
    for obj, kind in objs:
        for o, _ in objs:
            o.hide_render = o is not obj
        figure = kind in ("ground", "body", "head")
        body.hide_render = head.hide_render = not figure
        ground.hide_render = kind == "projectile"
        if kind == "projectile":
            obj.location = (0, 0, 0)
            vc.aim_camera(cam, (0.75, -0.55, 0.35), (0, 0.02, 0), lens=50)
        elif kind == "head":
            obj.location = (0, 0, 2.0)
            vc.aim_camera(cam, (1.4, -1.9, 2.3), (0, 0, 1.6), lens=50)
        elif kind == "trail":
            obj.location = (0, 0, 0.9)
            obj.scale = (1, 3.0, 1)
            vc.aim_camera(cam, (3.2, -2.2, 2.2), (0, 1.3, 0.7), lens=40)
        elif kind == "tall":
            vc.aim_camera(cam, (4.2, -5.0, 2.6), (0, 0, 1.4), lens=35)
        else:
            vc.aim_camera(cam, (2.3, -3.1, 2.1), (0, 0, 0.7), lens=40)
        hw = 2.5 * 18.0 / cam.data.lens  # half the view width at the label's distance
        label.location = (-0.92 * hw, 0.62 * hw, -2.5)
        curve.size = 0.11 * hw / 0.9
        curve.body = obj.name
        tmp = os.path.join(os.path.dirname(sheet_path), f"_fx_{obj.name}.png")
        vc.render_to(tmp)
        tiles.append(tmp)
    vc.compose_sheet(tiles, sheet_path, columns=5)
    vc.remove_files(tiles)
    print(f"SHEET {sheet_path}")


def main():
    args = vc.script_args()
    sheets = args[args.index("--sheets") + 1] if "--sheets" in args else DEFAULT_SHEETS
    vc.reset_scene()
    made = []  # (obj, preview kind)

    def add(result, kind):
        obj, paths, strength, tris = result
        export(obj, paths, strength)
        V = np.array([v.co for v in obj.data.vertices])
        size = V.max(axis=0) - V.min(axis=0)
        print(f"EFFECT {obj.name}: {tris} triangles, size {size[0]:.2f} x {size[1]:.2f} x {size[2]:.2f} m (Blender xyz)")
        made.append((obj, kind))

    add(fx_frost_bolt(), "projectile")
    add(fx_ember_mote(), "projectile")
    add(fx_light_strike(), "tall")
    for r in fx_ground_rings():
        add(r, "ground")
    for r in fx_shield_bubbles():
        add(r, "body")
    for r in fx_auras():
        add(r, "body")
    for r in fx_tick_wisps():
        add(r, "projectile")
    add(fx_stun_marker(), "head")
    add(fx_root_spikes(), "ground")
    add(fx_leap_trail(), "trail")
    add(fx_fog_cloud(), "ground")
    if "--no-preview" not in args:
        vc.ensure_dir(sheets)
        preview_sheet(made, os.path.join(sheets, "m12_effects.png"))


if __name__ == "__main__":
    main()
