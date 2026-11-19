"""Forged weapons (M17): fourteen procedural weapon models with inventory icons, original designs built
from parametric parts (no generated or downloaded meshes).

Usage (headless):
  blender --background --factory-startup --python tools/blender/gen_forged_weapons.py -- [--only a,b] [--no-preview]

Every model is authored in the GRIP FRAME of docs/ASSET_PIPELINE.md §4b (Blender axes: business end up +Z,
edge / front towards -Y, the point the hand closes around at the origin; shields: face towards -Y, top up,
origin at the handle bar behind the boss), so the existing grips of gamedata/attachments.toml place it on
all six looks without new rows. Materials are flat PBR colours (assetc bakes them into vertex colours and
splits submeshes by roughness / metallic); crystals and orbs are emissive. Output:
  assets_src/weapons/<model>.glb, assets_src/ui/items/<model>.png (128x128 icon, gen_weapons.render_icon),
  assets_src/previews/weapons/<model>.png and forged_sheet.png, assets_src/weapons/forged_meta.json.
Deterministic.
"""

import json
import math
import os
import sys

import bpy
import numpy as np
from mathutils import Matrix

sys.dont_write_bytecode = True
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import vo_common as vc  # noqa: E402
import gen_weapons as gw  # noqa: E402

OUT_DIR = os.path.join(vc.SRC, "weapons")
META_PATH = os.path.join(OUT_DIR, "forged_meta.json")

# name: (hex sRGB, roughness, metallic, emission strength)
MATS = {
    "steel": ("#c4cad3", 0.28, 1.0, 0.0),
    "dark": ("#5d636c", 0.45, 1.0, 0.0),
    "gold": ("#d8b05a", 0.30, 1.0, 0.0),
    "bronze": ("#b07a46", 0.38, 1.0, 0.0),
    "leather": ("#5b3b26", 0.72, 0.0, 0.0),
    "wood": ("#7b5434", 0.74, 0.0, 0.0),
    "darkwood": ("#3f2b1f", 0.70, 0.0, 0.0),
    "red": ("#8e2b22", 0.55, 0.0, 0.0),
    "blue": ("#2e416d", 0.55, 0.0, 0.0),
    "bone": ("#ddd2bb", 0.6, 0.0, 0.0),
    "crystal": ("#b892ff", 0.15, 0.0, 2.2),
    "orb": ("#ffb15a", 0.2, 0.0, 2.6),
}


def make_material(name):
    hexstr, rough, metal, emit = MATS[name]
    mat = vc.material(f"fw_{name}", hexstr, rough)
    bsdf = mat.node_tree.nodes.get("Principled BSDF")
    bsdf.inputs["Metallic"].default_value = metal
    if emit > 0.0:
        r, g, b = vc.hex_linear(hexstr)
        bsdf.inputs["Emission Color"].default_value = (r, g, b, 1.0)
        bsdf.inputs["Emission Strength"].default_value = emit
    return mat


class Builder:
    """Vertices + faces (CCW from outside) + a material name per face."""

    def __init__(self):
        self.V, self.F, self.M = [], [], []

    def add(self, V, F, mats):
        base = len(self.V)
        self.V += [tuple(float(x) for x in v) for v in V]
        for f, m in zip(F, mats):
            self.F.append(tuple(base + i for i in f))
            self.M.append(m)

    def loft(self, rings, mat_fn, cap0=True, cap1=True, cap_mat=None):
        """rings: list of (n, 3) arrays ordered along the part; mat_fn(segment, k) -> material."""
        n = len(rings[0])
        V = [p for r in rings for p in r]
        F, M = [], []
        for s in range(len(rings) - 1):
            for k in range(n):
                k2 = (k + 1) % n
                F.append((s * n + k, s * n + k2, (s + 1) * n + k2, (s + 1) * n + k))
                M.append(mat_fn(s, k))
        if cap0:
            F.append(tuple(reversed(range(n))))
            M.append(cap_mat or mat_fn(0, 0))
        if cap1:
            last = (len(rings) - 1) * n
            F.append(tuple(last + k for k in range(n)))
            M.append(cap_mat or mat_fn(len(rings) - 2, 0))
        self.add(V, F, M)

    def lathe(self, profile, sides, mat_fn, z_axis=(0.0, 0.0, 0.0), cap0=True, cap1=True, phase=0.0):
        """profile: [(radius, z)] bottom -> top around the Z axis through z_axis."""
        rings = []
        for r, z in profile:
            rings.append(np.array([(z_axis[0] + r * math.cos(2 * math.pi * k / sides + phase),
                                    z_axis[1] + r * math.sin(2 * math.pi * k / sides + phase), z) for k in range(sides)]))
        self.loft(rings, mat_fn if callable(mat_fn) else (lambda s, k, m=mat_fn: m), cap0, cap1)

    def box(self, c, size, mat, rot=None):
        sx, sy, sz = (x * 0.5 for x in size)
        P = np.array([(x, y, z) for z in (-sz, sz) for (x, y) in ((-sx, -sy), (sx, -sy), (sx, sy), (-sx, sy))])
        if rot is not None:
            P = P @ np.asarray(rot).T
        P = P + np.asarray(c)
        F = [(0, 3, 2, 1), (4, 5, 6, 7), (0, 1, 5, 4), (1, 2, 6, 5), (2, 3, 7, 6), (3, 0, 4, 7)]
        self.add(P, F, [mat] * 6)

    def ellipsoid(self, c, radii, mat, seg=12, rings=8, shape=None):
        V = []
        for i in range(1, rings):
            th = math.pi * i / rings
            for j in range(seg):
                ph = 2 * math.pi * j / seg
                u = np.array([math.sin(th) * math.cos(ph), math.sin(th) * math.sin(ph), math.cos(th)])
                s = 1.0 if shape is None else shape(u)
                V.append(np.asarray(c) + u * np.asarray(radii) * s)
        top = len(V)
        V.append(np.asarray(c) + np.array([0, 0, radii[2]]) * (1.0 if shape is None else shape(np.array([0, 0, 1.0]))))
        bot = len(V)
        V.append(np.asarray(c) - np.array([0, 0, radii[2]]) * (1.0 if shape is None else shape(np.array([0, 0, -1.0]))))
        F = []
        for j in range(seg):
            j2 = (j + 1) % seg
            F.append((top, j, j2))
            F.append((bot, (rings - 2) * seg + j2, (rings - 2) * seg + j))
        for i in range(rings - 2):
            for j in range(seg):
                j2 = (j + 1) % seg
                F.append((i * seg + j, (i + 1) * seg + j, (i + 1) * seg + j2, i * seg + j2))
        self.add(V, F, [mat] * len(F))

    def prism(self, outline, x0, x1, mat, edge_thin=None, rim_mat=None):
        """Extrudes a CCW outline in the (y, z) plane (seen from +X) between x = x0 and x1; edge_thin(y, z)
        -> 0..1 scales the thickness (1 = full, 0.1 = a sharp edge)."""
        n = len(outline)
        c = 0.5 * (x0 + x1)
        h = 0.5 * (x1 - x0)
        V = []
        for side in (1.0, -1.0):
            for (y, z) in outline:
                k = 1.0 if edge_thin is None else edge_thin(y, z)
                V.append((c + side * h * k, y, z))
        F, M = [], []
        # Side faces: +X face is the outline as seen from +X (CCW), -X reversed.
        F.append(tuple(range(n)))
        M.append(mat)
        F.append(tuple(n + k for k in reversed(range(n))))
        M.append(mat)
        for k in range(n):
            k2 = (k + 1) % n
            F.append((k2, k, n + k, n + k2))
            M.append(rim_mat or mat)
        # Fan-triangulate the caps (outlines here are star-shaped around their first point's centroid).
        F2, M2 = [], []
        for f, m in zip(F, M):
            if len(f) > 4:
                cpos = np.mean([V[i] for i in f], axis=0)
                ci = len(V)
                V.append(tuple(cpos))
                for a in range(len(f)):
                    F2.append((ci, f[a], f[(a + 1) % len(f)]))
                    M2.append(m)
            else:
                F2.append(f)
                M2.append(m)
        self.add(V, F2, M2)

    def build(self, name):
        mats = sorted(set(self.M))
        objs = [make_material(m) for m in mats]
        obj = vc.mesh_object(name, self.V, self.F, [mats.index(m) for m in self.M], objs, smooth=True, sharp_angle=40.0)
        import bmesh
        bm = bmesh.new()
        bm.from_mesh(obj.data)
        bmesh.ops.remove_doubles(bm, verts=bm.verts, dist=1e-5)
        bmesh.ops.recalc_face_normals(bm, faces=bm.faces)
        bmesh.ops.triangulate(bm, faces=bm.faces)
        bm.to_mesh(obj.data)
        bm.free()
        obj.data.shade_smooth()
        obj.data.set_sharp_from_angle(angle=math.radians(40.0))
        return obj


# ---------------------------------------------------------------------------------------------
# Parts

def blade(b, z0, z1, width, thick, curve=None, single=False, steps=14, fuller=0.0, mat="steel", tip=0.12,
          wave=None):
    """Blade from z0 to z1 (edge towards -Y): width(t) / thick(t) over t = 0..1, centre line offset
    curve(t) in Y (+Y = backwards), the last `tip` fraction tapering to a point. fuller: fraction of the
    length with a dark fuller down the middle of each flat."""
    rings = []
    for i in range(steps + 1):
        t = i / steps
        z = z0 + (z1 - z0) * t
        w = width(t)
        th = thick(t)
        if t > 1.0 - tip:
            q = (1.0 - t) / tip
            w *= max(q, 0.02) ** 0.8
            th *= max(q, 0.05)
        yc = curve(t) if curve else 0.0
        if wave:
            yc += wave(t)
        if single:  # thick spine at +Y, edge at -Y
            pts = [(0.0, -w / 2), (th * 0.3, -w / 4), (th / 2, w * 0.15), (th / 2, w / 2), (-th / 2, w / 2),
                   (-th / 2, w * 0.15), (-th * 0.3, -w / 4)]
        else:
            pts = [(0.0, -w / 2), (th * 0.35, -w / 4), (th / 2, 0.0), (th * 0.35, w / 4), (0.0, w / 2),
                   (-th * 0.35, w / 4), (-th / 2, 0.0), (-th * 0.35, -w / 4)]
        rings.append(np.array([(x, y + yc, z) for x, y in pts]))
    n = len(rings[0])

    def m(s, k):
        t = s / steps
        if fuller > 0 and t < fuller and not single and k in (1, 2, 5, 6):
            return "dark"
        return mat

    b.loft(rings, m, cap0=True, cap1=True)


def grip(b, z0, z1, r, wrap="leather", ring="gold", bands=4, sides=10):
    prof = []
    for i in range(bands * 2 + 1):
        z = z0 + (z1 - z0) * i / (bands * 2)
        prof.append((r * (1.0 + 0.08 * math.sin(math.pi * i / (bands * 2))), z))
    b.lathe(prof, sides, lambda s, k: wrap)
    for i in (0, bands * 2):
        z = z0 + (z1 - z0) * i / (bands * 2)
        b.lathe([(r * 1.0, z - 0.008), (r * 1.25, z - 0.004), (r * 1.25, z + 0.004), (r * 1.0, z + 0.008)], sides,
                lambda s, k, rr=ring: rr)


def shaft(b, z0, z1, r0, r1, mat="wood", sides=10, bands=(), band_mat="dark"):
    b.lathe([(r0, z0), (r1, z1)], sides, lambda s, k: mat)
    for z in bands:
        t = (z - z0) / (z1 - z0)
        r = r0 + (r1 - r0) * t
        b.lathe([(r, z - 0.012), (r * 1.25, z - 0.008), (r * 1.25, z + 0.008), (r, z + 0.012)], sides,
                lambda s, k: band_mat)


def guard(b, z, half_span, depth, height, mat="gold", up=0.0):
    """Cross guard along Y (edge direction) with the quillon tips raised by `up`."""
    segs = 6
    for i in range(segs):
        for side in (-1, 1):
            t0, t1 = i / segs, (i + 1) / segs
            y0, y1 = side * half_span * t0, side * half_span * t1
            zc = z + up * ((t0 + t1) / 2) ** 2
            hh = height * (1.0 - 0.35 * (t0 + t1) / 2)
            b.box((0.0, (y0 + y1) / 2, zc), (depth, abs(y1 - y0) + 0.002, hh), mat)


def spike(b, base, tip, r, mat="steel", sides=6):
    base, tip = np.asarray(base, float), np.asarray(tip, float)
    d = tip - base
    L = np.linalg.norm(d)
    a = d / L
    ref = np.array([0, 0, 1.0]) if abs(a[2]) < 0.9 else np.array([1.0, 0, 0])
    s = np.cross(a, ref)
    s /= np.linalg.norm(s)
    f = np.cross(s, a)
    ring = [base + r * (math.cos(2 * math.pi * k / sides) * s + math.sin(2 * math.pi * k / sides) * f) for k in range(sides)]
    V = ring + [tip, base]
    F = [(k, (k + 1) % sides, sides) for k in range(sides)] + [((k + 1) % sides, k, sides + 1) for k in range(sides)]
    b.add(V, F, [mat] * len(F))


def torus(b, c, R, r, mat, axis="y", seg=20, sides=8):
    V = []
    for i in range(seg):
        u = 2 * math.pi * i / seg
        for j in range(sides):
            v = 2 * math.pi * j / sides
            p = np.array([(R + r * math.cos(v)) * math.cos(u), (R + r * math.cos(v)) * math.sin(u), r * math.sin(v)])
            if axis == "y":  # ring in the X-Z plane (facing -Y)
                p = np.array([p[0], p[2], p[1]])
            V.append(np.asarray(c) + p)
    F = []
    for i in range(seg):
        i2 = (i + 1) % seg
        for j in range(sides):
            j2 = (j + 1) % sides
            F.append((i * sides + j, i2 * sides + j, i2 * sides + j2, i * sides + j2))
    b.add(V, F, [mat] * len(F))


# ---------------------------------------------------------------------------------------------
# Models (lengths in metres; z = 0 is the grip point)

def sword_ridge(b):
    grip(b, -0.10, 0.075, 0.016)
    b.ellipsoid((0, 0, -0.125), (0.026, 0.026, 0.032), "gold", 8, 6, shape=lambda u: 1.0 + 0.25 * abs(u[2]))
    guard(b, 0.09, 0.11, 0.03, 0.024, "gold", up=0.03)
    blade(b, 0.10, 0.83, lambda t: 0.056 - 0.012 * t, lambda t: 0.009, fuller=0.75)


def sword_curved(b):
    grip(b, -0.095, 0.07, 0.015, wrap="darkwood", ring="bronze")
    b.ellipsoid((0, 0.012, -0.115), (0.02, 0.03, 0.024), "bronze", 8, 6)
    b.ellipsoid((0, 0, 0.085), (0.034, 0.05, 0.012), "bronze", 10, 6)
    blade(b, 0.093, 0.78, lambda t: 0.04 + 0.022 * math.sin(math.pi * min(t * 1.15, 1.0)) ** 2,
          lambda t: 0.009 - 0.003 * t, curve=lambda t: 0.13 * t * t, single=True, tip=0.16)


def sword_broad(b):
    grip(b, -0.30, 0.10, 0.019, bands=6)
    b.ellipsoid((0, 0, -0.345), (0.034, 0.034, 0.04), "dark", 8, 6, shape=lambda u: 1.0 + 0.3 * abs(u[2]))
    guard(b, 0.125, 0.17, 0.036, 0.03, "dark", up=-0.04)
    b.box((0, 0, 0.125), (0.05, 0.06, 0.05), "gold")
    blade(b, 0.14, 1.15, lambda t: 0.075 - 0.02 * t, lambda t: 0.012, fuller=0.6, steps=16)


def dagger_fang(b):
    grip(b, -0.075, 0.05, 0.013, wrap="bone", ring="gold", bands=3)
    torus(b, (0, 0, -0.1), 0.02, 0.005, "gold", axis="y", seg=14, sides=6)
    guard(b, 0.06, 0.05, 0.022, 0.016, "gold", up=0.02)
    blade(b, 0.068, 0.33, lambda t: 0.034 - 0.01 * t, lambda t: 0.007,
          wave=lambda t: 0.008 * math.sin(t * 3.0 * math.pi) * (1 - t), tip=0.2)


def dagger_twin(b):
    grip(b, -0.08, 0.045, 0.014, wrap="leather", ring="steel", bands=3)
    b.box((0, 0.005, -0.09), (0.024, 0.03, 0.02), "steel")
    b.box((0, -0.018, 0.055), (0.024, 0.07, 0.014), "steel")
    blade(b, 0.06, 0.34, lambda t: 0.035 + 0.008 * math.sin(math.pi * t), lambda t: 0.008, curve=lambda t: -0.06 * t * t,
          single=True, tip=0.22)


def mace_flanged(b):
    shaft(b, -0.12, 0.40, 0.014, 0.016, "dark", sides=10)
    grip(b, -0.11, 0.07, 0.017, wrap="leather", ring="steel", bands=3)
    b.ellipsoid((0, 0, -0.13), (0.022, 0.022, 0.018), "steel", 8, 6)
    b.lathe([(0.018, 0.38), (0.026, 0.40), (0.026, 0.56), (0.012, 0.6)], 10, lambda s, k: "steel")
    for k in range(7):
        a = 2 * math.pi * k / 7
        outline = [(0.02, 0.385), (0.055, 0.42), (0.06, 0.5), (0.05, 0.56), (0.02, 0.585)]
        rot = np.array([[math.cos(a), -math.sin(a), 0], [math.sin(a), math.cos(a), 0], [0, 0, 1]])
        sub = Builder()
        sub.prism([(y, z) for y, z in outline] + [(0.0, 0.585), (0.0, 0.385)], -0.005, 0.005, "steel")
        V = np.array(sub.V)
        V = np.stack([V[:, 1], V[:, 0], V[:, 2]], 1)  # outline's y -> radial (x)
        V = V @ rot.T
        b.add(V, [tuple(reversed(f)) for f in sub.F], sub.M)
    spike(b, (0, 0, 0.59), (0, 0, 0.64), 0.012)


def mace_spiked(b):
    shaft(b, -0.13, 0.43, 0.015, 0.017, "wood", sides=10, bands=(0.25, 0.4))
    grip(b, -0.11, 0.07, 0.018, wrap="leather", ring="dark", bands=3)
    b.ellipsoid((0, 0, -0.13), (0.02, 0.02, 0.02), "dark", 8, 6)
    c = np.array([0, 0, 0.5])
    b.ellipsoid(c, (0.06, 0.06, 0.06), "dark", 14, 10)
    golden = math.pi * (3 - math.sqrt(5))
    for i in range(14):
        zz = 1 - 2 * (i + 0.5) / 14
        r = math.sqrt(1 - zz * zz)
        d = np.array([r * math.cos(golden * i), r * math.sin(golden * i), zz])
        spike(b, c + d * 0.05, c + d * 0.1, 0.012, "steel")


def hammer_war(b):
    shaft(b, -0.43, 0.86, 0.017, 0.019, "darkwood", sides=10, bands=(-0.3, 0.6, 0.8))
    grip(b, -0.14, 0.14, 0.021, wrap="leather", ring="steel", bands=5)
    b.box((0, -0.06, 0.82), (0.07, 0.12, 0.08), "steel")
    b.box((0, -0.125, 0.82), (0.085, 0.02, 0.095), "dark")
    spike(b, (0, 0.03, 0.82), (0, 0.17, 0.79), 0.022, "steel", sides=4)
    spike(b, (0, 0, 0.86), (0, 0, 0.97), 0.016, "steel", sides=4)
    b.box((0, 0, 0.82), (0.05, 0.05, 0.1), "dark")


def axe_bearded(b):
    shaft(b, -0.43, 0.92, 0.017, 0.02, "wood", sides=10, bands=(0.85,))
    grip(b, -0.12, 0.16, 0.021, wrap="leather", ring="bronze", bands=5)
    outline = [(-0.02, 0.92), (-0.08, 0.95), (-0.17, 1.01), (-0.24, 1.0), (-0.255, 0.9), (-0.235, 0.78),
               (-0.2, 0.7), (-0.15, 0.71), (-0.1, 0.8), (-0.05, 0.83), (-0.02, 0.83)]
    edge = lambda y, z: 0.15 if y < -0.22 else (1.0 if y > -0.12 else 1.0 - 0.85 * (-0.12 - y) / 0.1)
    b.prism(outline, -0.012, 0.012, "steel", edge_thin=edge, rim_mat="steel")
    b.box((0, 0.02, 0.875), (0.04, 0.06, 0.1), "dark")
    b.box((0, 0.06, 0.875), (0.035, 0.03, 0.05), "dark")


def halberd(b):
    shaft(b, -0.60, 1.30, 0.016, 0.018, "darkwood", sides=10, bands=(-0.55, 1.0, 1.26))
    grip(b, -0.12, 0.14, 0.02, wrap="leather", ring="steel", bands=5)
    blade(b, 1.30, 1.58, lambda t: 0.05 - 0.01 * t, lambda t: 0.012, tip=0.35)
    outline = [(-0.02, 1.27), (-0.05, 1.30), (-0.14, 1.33), (-0.19, 1.29), (-0.2, 1.18), (-0.17, 1.1), (-0.05, 1.14),
               (-0.02, 1.15)]
    edge = lambda y, z: 0.15 if y < -0.17 else 1.0
    b.prism(outline, -0.01, 0.01, "steel", edge_thin=edge)
    spike(b, (0, 0.015, 1.22), (0, 0.14, 1.27), 0.018, "steel", sides=4)
    b.lathe([(0.022, 1.1), (0.024, 1.3)], 10, lambda s, k: "dark")


def staff_ritual(b):
    shaft(b, -0.71, 0.88, 0.018, 0.022, "wood", sides=10, bands=(-0.6, 0.3, 0.75))
    for z in (-0.06, 0.02, 0.1):
        b.lathe([(0.022, z - 0.015), (0.026, z), (0.022, z + 0.015)], 10, lambda s, k: "red")
    torus(b, (0, 0, 1.0), 0.1, 0.012, "bronze", axis="y", seg=24, sides=8)
    b.lathe([(0.02, 0.86), (0.035, 0.9), (0.02, 0.95)], 10, lambda s, k: "bronze")
    b.ellipsoid((0, 0, 1.0), (0.045, 0.045, 0.045), "orb", 14, 10)
    for k, a in enumerate((-70.0, 0.0, 70.0)):
        x = 0.1 * math.sin(math.radians(a)) * 0.95
        z = 1.0 - 0.1 * math.cos(math.radians(a)) * 0.95
        b.box((x, 0, z - 0.03), (0.004, 0.004, 0.05), "dark")
        b.ellipsoid((x, 0, z - 0.065), (0.014, 0.014, 0.018), "gold", 8, 6)
    spike(b, (0, 0, 1.1), (0, 0, 1.17), 0.012, "bronze")


def staff_crystal(b):
    shaft(b, -0.72, 0.84, 0.017, 0.021, "darkwood", sides=10, bands=(-0.6, 0.5))
    grip(b, -0.1, 0.12, 0.022, wrap="blue", ring="steel", bands=4)
    for k in range(4):
        a = 2 * math.pi * k / 4 + math.pi / 4
        d = np.array([math.cos(a), math.sin(a), 0])
        spike(b, np.array([0, 0, 0.84]) + d * 0.012, np.array([0, 0, 1.02]) + d * 0.06, 0.009, "steel", sides=5)
    rng = vc.Rng(17)
    for k in range(5):
        a = 2 * math.pi * k / 5
        tilt = 0.0 if k == 0 else 0.35
        d = np.array([math.sin(tilt) * math.cos(a), math.sin(tilt) * math.sin(a), math.cos(tilt)])
        base = np.array([0, 0, 0.9])
        L = 0.17 if k == 0 else 0.1 + 0.03 * rng.uniform()
        r = 0.022 if k == 0 else 0.014
        spike(b, base + d * 0.02, base + d * L, r, "crystal", sides=6)
        spike(b, base + d * 0.02, base - d * 0.02, r, "crystal", sides=6)


def shield_tower(b):
    W, H, R = 0.62, 1.06, 0.7
    rows, cols = 12, 10
    V, M = [], []
    for i in range(rows + 1):
        z = -H * 0.45 + H * i / rows
        for j in range(cols + 1):
            x = -W / 2 + W * j / cols
            # Rounded top corners: pull the top row in.
            if i == rows:
                x *= 0.86
            ang = x / R
            # Curved around a vertical axis: the middle bulges forwards (-Y), the edges stay at y = -0.035.
            y = -0.035 - R * (math.cos(ang) - math.cos(W / 2 / R))
            V.append((R * math.sin(ang), y, z))
    F = []
    for i in range(rows):
        for j in range(cols):
            a = i * (cols + 1) + j
            F.append((a, a + cols + 1, a + cols + 2, a + 1))
            jj = j / cols
            M.append("steel" if abs(jj - 0.5) < 0.12 or i in (rows // 2, rows // 2 + 1) else "red")
    front = np.array(V)
    back = front + np.array([0, 0.025, 0])
    n = len(V)
    allV = list(front) + list(back)
    F2 = list(F) + [tuple(n + k for k in reversed(f)) for f in F]
    M2 = list(M) + ["darkwood"] * len(F)
    # Edges.
    loop = [i * (cols + 1) for i in range(rows + 1)] + [rows * (cols + 1) + j for j in range(1, cols + 1)] + \
        [i * (cols + 1) + cols for i in range(rows - 1, -1, -1)] + [j for j in range(cols - 1, 0, -1)]
    for k in range(len(loop)):
        a, c = loop[k], loop[(k + 1) % len(loop)]
        F2.append((c, a, n + a, n + c))
        M2.append("dark")
    b.add(allV, F2, M2)
    b.ellipsoid((0, -0.06, 0.08), (0.075, 0.035, 0.075), "steel", 14, 8)
    b.box((0, 0.0, 0.0), (0.03, 0.03, 0.16), "dark")  # the handle bar behind the boss
    for zz in (-0.08, 0.08):
        b.box((0, 0.0, zz), (0.03, 0.06, 0.02), "dark")


def shield_buckler(b):
    rad = 0.21
    prof = [(rad, 0.0), (rad * 0.98, -0.012), (rad * 0.8, -0.03), (rad * 0.5, -0.045), (0.07, -0.052), (0.0, -0.055)]
    rings = []
    sides = 24
    for r, y in prof:
        rings.append(np.array([(r * math.cos(2 * math.pi * k / sides), y - 0.03, r * math.sin(2 * math.pi * k / sides))
                               for k in range(sides)]))
    b.loft(rings, lambda s, k: "bronze" if s == 0 else ("wood" if s < 3 else "steel"), cap0=True, cap1=False)
    b.ellipsoid((0, -0.09, 0), (0.06, 0.04, 0.06), "steel", 14, 8)
    torus(b, (0, -0.03, 0), rad, 0.01, "dark", axis="y", seg=28, sides=6)
    for k in range(8):
        a = 2 * math.pi * k / 8
        b.ellipsoid((0.15 * math.cos(a), -0.068, 0.15 * math.sin(a)), (0.008, 0.006, 0.008), "gold", 6, 4)
    b.box((0, 0.0, 0.0), (0.03, 0.03, 0.12), "dark")
    for zz in (-0.06, 0.06):
        b.box((0, -0.015, zz), (0.03, 0.03, 0.02), "dark")


# name: (builder, grip, icon (yaw, tilt, roll) as gen_weapons, expected length along +Z / shield width)
FORGED = {
    "sword_ridge": (sword_ridge, "sword", (70.0, -12.0, 45.0)),
    "sword_curved": (sword_curved, "sword", (80.0, -12.0, 45.0)),
    "sword_broad": (sword_broad, "axe2h", (70.0, -12.0, 45.0)),
    "dagger_fang": (dagger_fang, "dagger", (70.0, -12.0, 45.0)),
    "dagger_twin": (dagger_twin, "dagger", (80.0, -12.0, 45.0)),
    "mace_flanged": (mace_flanged, "mace", (25.0, -12.0, 45.0)),
    "mace_spiked": (mace_spiked, "mace", (25.0, -12.0, 45.0)),
    "hammer_war": (hammer_war, "axe2h", (80.0, -10.0, 45.0)),
    "axe_bearded": (axe_bearded, "axe2h", (80.0, -10.0, 45.0)),
    "halberd": (halberd, "axe2h", (80.0, -10.0, 45.0)),
    "staff_ritual": (staff_ritual, "staff", (20.0, -10.0, 45.0)),
    "staff_crystal": (staff_crystal, "staff", (20.0, -10.0, 45.0)),
    "shield_tower": (shield_tower, "shield", (-28.0, -10.0, 0.0)),
    "shield_buckler": (shield_buckler, "shield", (-28.0, -10.0, 0.0)),
}


def build(name):
    vc.reset_scene()
    fn, grip_kind, icon = FORGED[name]
    b = Builder()
    fn(b)
    obj = b.build(name)
    obj.data.name = name
    vc.export_glb(os.path.join(OUT_DIR, f"{name}.glb"), [obj])
    v = gw.verts_np(obj)
    lo, hi = v.min(0), v.max(0)
    obj.data.calc_loop_triangles()
    tris = len(obj.data.loop_triangles)
    meta = dict(tris=tris, grip=grip_kind, materials=sorted(set(b.M)),
                bounds_min=[round(float(-hi[0]), 3), round(float(lo[2]), 3), round(float(-hi[1]), 3)],
                bounds_max=[round(float(-lo[0]), 3), round(float(hi[2]), 3), round(float(-lo[1]), 3)])
    print(f"FORGED {name}: {tris} tris, grip {grip_kind}, size {np.round(hi - lo, 3)}, below grip {-lo[2]:.3f}, above {hi[2]:.3f}")
    return obj, meta, dict(grip=grip_kind, icon=icon, icon_name=name)


def main():
    args = vc.script_args()
    names = list(FORGED)
    if "--only" in args:
        names = [n for n in names if n in set(args[args.index("--only") + 1].split(","))]
    meta_all = {}
    if os.path.exists(META_PATH):
        with open(META_PATH, "r", encoding="utf-8") as f:
            meta_all = json.load(f)
    shots = []
    for name in names:
        obj, meta, cfg = build(name)
        meta_all[name] = meta
        icon = gw.render_icon(name, obj, cfg)
        if "--no-preview" not in args:
            shots.append(gw.render_preview(name, obj, cfg, icon))
    with open(META_PATH, "w", encoding="utf-8", newline="\n") as f:
        json.dump({k: meta_all[k] for k in FORGED if k in meta_all}, f, indent=1, sort_keys=True)
        f.write("\n")
    if len(shots) == len(FORGED):
        vc.compose_sheet(shots, os.path.join(gw.PREVIEW_DIR, "forged_sheet.png"), 7, stable=True)


if __name__ == "__main__":
    main()
