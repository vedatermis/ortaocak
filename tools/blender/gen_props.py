"""Static low-poly props for zone 1.

Usage (headless):
  blender --background --factory-startup --python tools/blender/gen_props.py -- [--only name,name] [--no-preview]

Outputs assets_src/props/<name>.glb and assets_src/previews/props/<name>.png (+ props_sheet.png).
Each prop stands on Z = 0 with its origin at the centre of its footprint and its front facing -Y
(engine +Z). Footprints used by the terrain generator live in vo_common.PROP_INFO.
"""

import math
import os
import sys

import bpy
import numpy as np

sys.dont_write_bytecode = True  # keep tools/blender free of __pycache__
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import vo_common as vc  # noqa: E402

PAL = dict(
    bark="#5b3f2a", bark_dark="#47301f", leaf="#4f7f3a", leaf_light="#6b9a45", leaf_dark="#3b6630",
    pine="#2f5a3c", pine_light="#3e6e48", rock="#8a8780", rock_dark="#6d6a64", moss="#6f8a4a",
    wood="#9a6b42", wood_dark="#6e4a2c", plank="#b3844f", stone="#a39d92", stone_dark="#7f796f",
    plaster="#e3d6bb", roof="#9c4a32", roof_dark="#7a3826", iron="#3c3d40", glow="#ffd27a",
    cloth_a="#c4423a", cloth_b="#efe4c8", apple="#c8382f", pear="#b7b046", window="#2d3440", rope="#c7b08a",
    straw="#d2b260",
    # Ant Taşı: weathered grey oath stone, darker plinth, two kinds of marks, ember-lit crack.
    oath="#8e8b85", oath_light="#a29e96", oath_dark="#6b6862", plinth="#7d786f", plinth_dark="#645f57",
    script="#4b4844", rune="#2e2b29", crack="#3a2c26", ember="#c8622a", ember_hot="#f09a3c",
)


class Prop:
    def __init__(self):
        self.mb = vc.MeshBuilder()
        self.names = []

    def m(self, key):
        if key not in self.names:
            self.names.append(key)
        return self.names.index(key)

    def build(self, name):
        mats = [vc.material(f"prop_{k}", PAL[k]) for k in self.names]
        return self.mb.build(name, mats, smooth=False)


def lumpy(p, center, radii, mat, seed, segments=8, rings=6, amount=0.18):
    rng = vc.Rng(seed)
    table = [rng.uniform(-1, 1) for _ in range(97)]

    def shape(u):
        k = int((u[0] * 7.1 + u[1] * 13.7 + u[2] * 5.3 + 3) * 11) % 97
        s = 1.0 + amount * table[k]
        return (s, s, s)

    p.mb.ellipsoid(center, radii, segments, rings, mat, {}, shape=shape)


def plank_box(p, center, size, mat, rot_z=0.0, rot_x=0.0, rot_y=0.0):
    s = p.mb.mark()
    p.mb.box((0, 0, 0), size, mat, {})
    p.mb.transform_since(s, center, rot_z=rot_z, rot_x=rot_x, rot_y=rot_y)


# ---------------------------------------------------------------------------------------------

def tree_oak(p):
    p.mb.profile((0, 0, 0), [(0.34, 0.0), (0.26, 0.35), (0.22, 1.6), (0.20, 2.4)], 7, p.m("bark"))
    # Two branches.
    for a, h in ((0.6, 2.0), (3.4, 2.2)):
        plank_box(p, (math.cos(a) * 0.35, math.sin(a) * 0.35, h + 0.25), (0.14, 0.14, 0.9), p.m("bark_dark"),
                  rot_z=a, rot_y=0.6)
    blobs = [((0, 0, 3.4), (1.7, 1.6, 1.3), "leaf"), ((0.9, 0.4, 2.9), (1.1, 1.0, 0.9), "leaf_dark"),
             ((-0.9, -0.3, 3.0), (1.2, 1.1, 0.95), "leaf_light"), ((0.2, -0.9, 3.0), (1.0, 0.9, 0.85), "leaf"),
             ((-0.3, 0.8, 3.9), (1.0, 0.95, 0.8), "leaf_light")]
    for i, (c, rad, col) in enumerate(blobs):
        lumpy(p, c, rad, p.m(col), seed=300 + i, segments=9, rings=6, amount=0.12)


def tree_pine(p):
    p.mb.profile((0, 0, 0), [(0.22, 0.0), (0.17, 0.3), (0.12, 1.4)], 6, p.m("bark"))
    tiers = [(0.9, 1.75, 2.3, "pine"), (2.2, 1.35, 2.1, "pine_light"), (3.4, 1.0, 1.9, "pine"),
             (4.5, 0.62, 1.6, "pine_light")]
    for i, (z, r, h, col) in enumerate(tiers):
        p.mb.profile((0, 0, 0), [(r, z), (r * 0.82, z - 0.12), (0.02, z + h)], 9, p.m(col),
                     cap=(False, True), phase=i * 0.35)
        # Underside of each tier.
        p.mb.profile((0, 0, 0), [(r * 0.82, z - 0.12), (0.15, z + 0.05)], 9, p.m("pine"), cap=(False, False),
                     phase=i * 0.35)


def rock_a(p):
    lumpy(p, (0, 0, 0.45), (1.0, 0.8, 0.7), p.m("rock"), seed=41, segments=8, rings=5, amount=0.22)
    lumpy(p, (0.6, 0.35, 0.25), (0.55, 0.5, 0.42), p.m("rock_dark"), seed=42, segments=7, rings=4, amount=0.2)
    lumpy(p, (-0.2, -0.1, 0.95), (0.5, 0.45, 0.18), p.m("moss"), seed=43, segments=7, rings=4, amount=0.15)


def rock_b(p):
    lumpy(p, (0, 0, 0.9), (1.3, 1.1, 1.25), p.m("rock_dark"), seed=51, segments=9, rings=6, amount=0.2)
    lumpy(p, (-0.9, -0.6, 0.35), (0.7, 0.6, 0.5), p.m("rock"), seed=52, segments=7, rings=4, amount=0.22)
    lumpy(p, (0.8, 0.7, 0.3), (0.5, 0.5, 0.4), p.m("rock"), seed=53, segments=7, rings=4, amount=0.22)


def fence(p):
    # 2.0 m segment along X, centred; posts at both ends.
    for x in (-0.95, 0.95):
        p.mb.cylinder((x, 0, 0), 0.07, 0.06, 1.1, 6, p.m("wood_dark"))
        p.mb.cylinder((x, 0, 1.1), 0.06, 0.01, 0.1, 6, p.m("wood_dark"))
    for z, tilt in ((0.45, 0.02), (0.85, -0.015)):
        plank_box(p, (0, -0.06, z), (2.0, 0.05, 0.12), p.m("plank"), rot_y=tilt)


def well(p):
    sides = 12
    ring = [(1.0, 0.0), (1.0, 0.8), (0.95, 0.88)]
    p.mb.profile((0, 0, 0), ring, sides, p.m("stone"), cap=(False, False))
    p.mb.profile((0, 0, 0), [(0.95, 0.88), (0.72, 0.88)], sides, p.m("stone_dark"), cap=(False, False))
    # Inner wall facing inwards: lathe downward.
    p.mb.profile((0, 0, 0), [(0.72, 0.88), (0.72, 0.3)], sides, p.m("stone_dark"), cap=(False, False))
    p.mb.cylinder((0, 0, 0.28), 0.72, 0.72, 0.02, sides, p.m("window"))  # dark water surface
    # Stone bands.
    p.mb.profile((0, 0, 0), [(1.03, 0.36), (1.03, 0.46)], sides, p.m("stone_dark"), cap=(False, False), phase=0.2)
    for x in (-0.85, 0.85):
        p.mb.cylinder((x, 0, 0.6), 0.08, 0.07, 1.75, 6, p.m("wood_dark"))
    plank_box(p, (0, 0, 2.15), (1.9, 0.1, 0.1), p.m("wood"))
    plank_box(p, (0, 0, 2.15), (0.1, 0.1, 0.1), p.m("rope"))
    # Little gable roof.
    for sgn in (1, -1):
        plank_box(p, (0, sgn * 0.40, 2.48), (2.3, 1.0, 0.06), p.m("roof"), rot_x=-sgn * 0.6)
    plank_box(p, (0, 0, 2.75), (2.35, 0.1, 0.1), p.m("roof_dark"))
    # Bucket.
    p.mb.profile((0.35, -0.15, 0.88), [(0.14, 0.0), (0.17, 0.24)], 8, p.m("wood"))
    p.mb.profile((0.35, -0.15, 0.88), [(0.175, 0.05), (0.175, 0.09)], 8, p.m("iron"), cap=(False, False))


def market_stall(p):
    w, d = 2.6, 1.5
    for x in (-w / 2, w / 2):
        for y in (-d / 2, d / 2):
            h = 2.3 if y < 0 else 2.6
            p.mb.cylinder((x, y, 0), 0.06, 0.05, h, 6, p.m("wood_dark"))
    plank_box(p, (0, -d / 2 + 0.25, 0.85), (w, 0.6, 0.08), p.m("plank"))
    plank_box(p, (0, -d / 2 + 0.25, 0.42), (w - 0.1, 0.55, 0.8), p.m("wood"))
    # Striped awning sloping to the front.
    n = 8
    for i in range(n):
        x = -w / 2 - 0.15 + (w + 0.3) * (i + 0.5) / n
        plank_box(p, (x, 0.0, 2.45), ((w + 0.3) / n, d + 0.55, 0.05), p.m("cloth_a" if i % 2 == 0 else "cloth_b"),
                  rot_x=-0.2)
    # Produce baskets.
    for i, (x, col) in enumerate(((-0.8, "apple"), (0.0, "pear"), (0.8, "apple"))):
        p.mb.profile((x, -d / 2 + 0.25, 0.89), [(0.22, 0.0), (0.28, 0.16)], 8, p.m("straw"))
        for k in range(5):
            a = k * 1.3 + i
            p.mb.ellipsoid((x + 0.12 * math.cos(a), -d / 2 + 0.25 + 0.1 * math.sin(a), 1.08 + 0.03 * (k % 2)),
                           (0.07, 0.07, 0.07), 6, 4, p.m(col), {})


def lamp_post(p):
    p.mb.profile((0, 0, 0), [(0.2, 0.0), (0.2, 0.25), (0.08, 0.35), (0.06, 3.0), (0.08, 3.1)], 6, p.m("iron"))
    plank_box(p, (0.0, -0.3, 3.0), (0.06, 0.6, 0.06), p.m("iron"))
    # Lantern hanging in front.
    p.mb.box((0, -0.55, 2.6), (0.22, 0.22, 0.32), p.m("glow"), {})
    p.mb.profile((0, -0.55, 2.76), [(0.19, 0.0), (0.02, 0.16)], 4, p.m("iron"), phase=math.pi / 4)
    p.mb.box((0, -0.55, 2.42), (0.26, 0.26, 0.04), p.m("iron"), {})
    plank_box(p, (0, -0.55, 2.94), (0.02, 0.02, 0.12), p.m("iron"))


def crate(p):
    plank_box(p, (0, 0, 0.4), (0.8, 0.8, 0.8), p.m("plank"))
    for sgn in (1, -1):  # dark edge battens on front/back
        for z in (0.06, 0.74):
            plank_box(p, (0, sgn * 0.41, z), (0.82, 0.03, 0.1), p.m("wood_dark"))
        plank_box(p, (0, sgn * 0.41, 0.4), (1.0, 0.03, 0.09), p.m("wood_dark"), rot_y=math.atan2(0.68, 0.68))


def barrel(p):
    prof = [(0.36, 0.0), (0.42, 0.25), (0.45, 0.5), (0.42, 0.75), (0.36, 1.0)]
    p.mb.profile((0, 0, 0), prof, 12, [p.m("wood"), p.m("wood"), p.m("wood"), p.m("wood")])
    for z, rr in ((0.12, 0.40), (0.86, 0.40), (0.42, 0.455)):
        p.mb.profile((0, 0, 0), [(rr + 0.012, z), (rr + 0.012, z + 0.06)], 12, p.m("iron"), cap=(False, False))


def signpost(p):
    p.mb.cylinder((0, 0, 0), 0.09, 0.07, 2.4, 6, p.m("wood_dark"))
    p.mb.cylinder((0, 0, 2.4), 0.08, 0.01, 0.12, 6, p.m("wood_dark"))
    for z, yaw, ln in ((2.0, 0.35, 1.1), (1.6, 2.6, 0.95)):
        s = p.mb.mark()
        p.mb.box((ln / 2, 0, 0), (ln, 0.05, 0.24), p.m("plank"), {})
        tip = p.mb.mark()
        p.mb.box((ln + 0.08, 0, 0), (0.16, 0.05, 0.17), p.m("plank"), {}, rot_z=0.0, taper_top=1.0)
        p.mb.transform_since(s, (0, 0, z), rot_z=yaw)
        del tip
    p.mb.box((0, 0, 0.12), (0.4, 0.4, 0.24), p.m("stone"), {})


def house(p):
    W, D, H = 6.0, 4.6, 3.0
    plank_box(p, (0, 0, 0.25), (W + 0.3, D + 0.3, 0.5), p.m("stone"))  # stone plinth
    plank_box(p, (0, 0, 0.5 + H / 2), (W, D, H), p.m("plaster"))
    # Timber frame.
    for x in (-W / 2, -W / 6, W / 6, W / 2):
        for y in (-D / 2, D / 2):
            plank_box(p, (x, y, 0.5 + H / 2), (0.16, 0.16, H), p.m("wood_dark"))
    for y in (-D / 2, D / 2):
        plank_box(p, (0, y, 0.5 + H - 0.08), (W + 0.1, 0.16, 0.16), p.m("wood_dark"))
        plank_box(p, (0, y, 0.58), (W + 0.1, 0.16, 0.16), p.m("wood_dark"))
    for x in (-W / 2, W / 2):
        plank_box(p, (x, 0, 0.5 + H - 0.08), (0.16, D + 0.1, 0.16), p.m("wood_dark"))
    # Gable walls (triangular) on the X ends: lofted prism.
    gh = 2.0
    for sgn in (1, -1):
        x = sgn * W / 2
        s = p.mb.mark()
        a = p.mb.add_vertex((x, -D / 2, 0.5 + H), {})
        b = p.mb.add_vertex((x, D / 2, 0.5 + H), {})
        c = p.mb.add_vertex((x, 0, 0.5 + H + gh), {})
        p.mb.add_face((a, b, c) if sgn > 0 else (b, a, c), p.m("plaster"))
        del s
    # Roof slabs with overhang.
    slope = math.atan2(gh, D / 2)
    run = math.hypot(gh, D / 2) + 0.55
    for sgn in (1, -1):
        cy = sgn * (D / 4 + 0.12)
        plank_box(p, (0, cy, 0.5 + H + gh / 2 + 0.1), (W + 0.9, run, 0.14), p.m("roof"), rot_x=-sgn * slope)
    plank_box(p, (0, 0, 0.5 + H + gh + 0.08), (W + 0.95, 0.24, 0.2), p.m("roof_dark"))
    # Door and windows on the front (-Y).
    plank_box(p, (0.6, -D / 2 - 0.04, 0.5 + 1.0), (1.0, 0.1, 2.0), p.m("wood"))
    plank_box(p, (0.6, -D / 2 - 0.06, 0.5 + 1.0), (0.08, 0.1, 2.0), p.m("wood_dark"))
    for x in (-1.7, 2.1):
        plank_box(p, (x, -D / 2 - 0.03, 0.5 + 1.7), (0.8, 0.08, 0.7), p.m("window"))
        plank_box(p, (x, -D / 2 - 0.06, 0.5 + 1.3), (1.0, 0.12, 0.1), p.m("wood_dark"))
    for y in (-0.8, 0.9):
        plank_box(p, (W / 2 + 0.03, y, 0.5 + 1.7), (0.08, 0.7, 0.7), p.m("window"))
    # Chimney.
    plank_box(p, (-W / 2 + 0.9, D / 4, 0.5 + H + 1.6), (0.6, 0.6, 2.6), p.m("stone_dark"))


def watchtower(p):
    base, top, h = 1.5, 1.1, 5.5
    for sx in (1, -1):
        for sy in (1, -1):
            s = p.mb.mark()
            p.mb.cylinder((0, 0, 0), 0.12, 0.1, math.hypot(h, base - top), 6, p.m("wood_dark"))
            tilt = math.atan2(base - top, h)
            p.mb.transform_since(s, (sx * base, sy * base, 0), rot_y=-sx * tilt * 0.0, rot_x=0.0)
            # Lean the leg inwards by shearing the top ring.
            for i in range(s, len(p.mb.verts)):
                x, y, z = p.mb.verts[i]
                k = z / h
                p.mb.verts[i] = (x - sx * (base - top) * k, y - sy * (base - top) * k, z)
    # Cross braces on two sides.
    for sgn in (1, -1):
        L = math.hypot(2 * (base + top) / 2, 2.6)
        plank_box(p, (0, sgn * (base + top) / 2 * 1.0, 2.0), (L, 0.08, 0.14), p.m("wood"), rot_y=0.9)
        plank_box(p, (sgn * (base + top) / 2, 0, 2.0), (0.08, L, 0.14), p.m("wood"), rot_x=0.9)
    plank_box(p, (0, 0, h), (2 * top + 0.6, 2 * top + 0.6, 0.18), p.m("plank"))
    # Railing.
    for sgn in (1, -1):
        plank_box(p, (0, sgn * (top + 0.25), h + 0.55), (2 * top + 0.6, 0.08, 0.1), p.m("wood"))
        plank_box(p, (sgn * (top + 0.25), 0, h + 0.55), (0.08, 2 * top + 0.6, 0.1), p.m("wood"))
        for k in (-1, 0, 1):
            plank_box(p, (k * (top + 0.25), sgn * (top + 0.25), h + 0.3), (0.08, 0.08, 0.6), p.m("wood_dark"))
            plank_box(p, (sgn * (top + 0.25), k * (top + 0.25), h + 0.3), (0.08, 0.08, 0.6), p.m("wood_dark"))
    # Roof posts and pyramid roof.
    for sx in (1, -1):
        for sy in (1, -1):
            p.mb.cylinder((sx * top, sy * top, h), 0.07, 0.07, 1.9, 5, p.m("wood_dark"))
    p.mb.profile((0, 0, h + 1.9), [(2.0, 0.0), (0.05, 1.3)], 4, p.m("roof"), phase=math.pi / 4)
    # Ladder on the front.
    for x in (-0.3, 0.3):
        plank_box(p, (x, -base - 0.15, h / 2), (0.07, 0.07, h + 0.2), p.m("wood"), rot_x=0.07)
    for k in range(12):
        z = 0.35 + k * 0.45
        plank_box(p, (0, -base - 0.15 + 0.07 * (z - h / 2) / (h / 2) * 0.5, z), (0.62, 0.05, 0.05), p.m("plank"))


def loft_rings(p, rings, side_mat, top_mat, bottom=True):
    """Closed loft through rings of 3D points (each counter-clockwise seen from above, bottom to top)
    with a material per side: side_mat(ring, k) -> palette key; flat caps."""
    idx = [[p.mb.add_vertex(pt, {}) for pt in ring] for ring in rings]
    n = len(rings[0])
    for r in range(len(rings) - 1):
        for k in range(n):
            k2 = (k + 1) % n
            p.mb.add_face((idx[r][k], idx[r][k2], idx[r + 1][k2], idx[r + 1][k]), p.m(side_mat(r, k)))
    if bottom:
        p.mb.add_face(tuple(reversed(idx[0])), p.m(top_mat))
    p.mb.add_face(tuple(idx[-1]), p.m(top_mat))


def ant_tasi(p):
    """The Ant Taşı (docs/LORE.md): the cracked oath stone of Ortaocak. 4.5 m tall and 2.5 m wide on a
    two-step plinth, split by a vertical crack from the top down to a third of its height with a
    faint ember glow deep inside (vertex colour only). Front (-Y, engine +Z): neat written lines
    (Aydor); back: deep notch columns (Kargan). Abstract marks only, no readable text."""
    rng = vc.Rng(4500)
    table = [rng.uniform(-1.0, 1.0) for _ in range(211)]

    def nz(i, k, amp):
        return amp * table[(i * 37 + k * 11 + 5) % 211]

    # Plinth: two chamfered steps.
    for z0, z1, hx, hy, top in ((0.0, 0.22, 1.8, 1.2, "plinth"), (0.22, 0.42, 1.5, 0.95, "plinth")):
        c = 0.18
        ring = [(-hx + c, -hy), (hx - c, -hy), (hx, -hy + c), (hx, hy - c), (hx - c, hy), (-hx + c, hy),
                (-hx, hy - c), (-hx, -hy + c)]
        rings = [[(x + nz(j, k, 0.02), y + nz(j + 3, k, 0.02), z) for k, (x, y) in enumerate(ring)]
                 for j, z in enumerate((z0, z1))]
        loft_rings(p, rings, lambda r, k: "plinth_dark" if k % 2 else "plinth", top, bottom=False)
    P = 0.42                      # plinth top
    SH = 4.35                     # stone height above the plinth (~4.5 m with the raised top corners)
    ZC = P + SH / 3.0             # bottom of the crack
    TOP = P + SH

    def W(z):                     # half width (a slight belly)
        t = (z - P) / SH
        return 1.22 - 0.2 * t + 0.07 * math.sin(math.pi * min(max(t, 0.0), 1.0))

    def T(z):                     # half thickness
        return 0.46 - 0.12 * (z - P) / SH

    def gap(z):
        return 0.0 if z <= ZC else 0.30 * ((z - ZC) / (TOP - ZC)) ** 1.2

    def cx(z):                    # crack centre line (a slight zig-zag)
        u = max(0.0, z - ZC)
        return 0.06 * math.sin(2.7 * u) + 0.025 * math.sin(7.3 * u + 1.0)

    # The stone is built as two halves meeting at the crack line; below the crack bottom they touch
    # (gap 0), so the uncracked third reads as one block. Right half section (CCW from above): front
    # face from the crack outwards, outer side, back face, then the crack wall (dark rim, ember
    # middle).
    def half_ring(z, j, sgn, shrink=1.0, lift=0.0):
        w, t = W(z) * shrink, T(z) * shrink
        xi = cx(z) * sgn + gap(z) / 2.0      # inner face, in the half's own (+x) frame
        pts = [(xi, -t), (xi + (w - xi) * 0.45, -t), (0.85 * w, -t), (w, -0.55 * t), (w, 0.55 * t),
               (0.85 * w, t), (xi + (w - xi) * 0.45, t), (xi, t), (xi + 0.025, 0.35 * t), (xi + 0.025, -0.35 * t)]
        # (the ember strip is recessed 2.5 cm: keeps the cap polygons free of collinear points)
        out = []
        for k, (x, y) in enumerate(pts):
            fb = k in (1, 2, 5, 6)
            inner = k in (0, 7, 8, 9)
            amp = 0.0 if inner else (0.006 if fb else 0.05)
            xx = x + nz(j + (0 if sgn > 0 else 50), k, amp)
            yy = y + nz(j + (13 if sgn > 0 else 63), k, 0.004 if fb or inner else 0.025)
            zz = z + lift * (x / max(w, 1e-3))
            out.append((sgn * xx, yy, zz))
        return out if sgn > 0 else [out[0]] + out[:0:-1]  # mirrored: reverse to stay CCW

    def crack_mat(r, k, ring_z):
        if k == 8:
            return "ember" if ring_z[r] < TOP - 0.6 else "crack"
        if k in (7, 9):
            return "crack"
        return "oath_dark" if k in (2, 3, 4) else "oath"

    for sgn, top_z, lift in ((1.0, TOP, 0.10), (-1.0, TOP - 0.28, 0.16)):
        zs = [P - 0.02, P + 0.6, P + 1.1, ZC] + [ZC + (top_z - 0.35 - ZC) * i / 8.0 for i in range(1, 9)]
        rings = [half_ring(z, j, sgn) for j, z in enumerate(zs)]
        rings.append(half_ring(top_z - 0.2, 12, sgn, shrink=0.95, lift=lift))
        rings.append(half_ring(top_z - 0.06, 13, sgn, shrink=0.84, lift=lift * 1.3))
        rings.append(half_ring(top_z, 14, sgn, shrink=0.62, lift=lift * 1.5))
        ring_z = zs + [top_z - 0.2, top_z - 0.06, top_z]
        if sgn < 0:  # mirrored, reversed section: its side k is side 9 - k of the right half
            loft_rings(p, rings, lambda r, k: crack_mat(r, 9 - k, ring_z), "oath_light")
        else:
            loft_rings(p, rings, lambda r, k: crack_mat(r, k, ring_z), "oath_light")
    # Glow deep in the crack: a thin ember sheet in the middle, hottest low down.
    for z0, z1, mat in ((ZC + 0.05, ZC + 0.9, "ember_hot"), (ZC + 0.9, TOP - 0.7, "ember")):
        zm = (z0 + z1) / 2
        s = p.mb.mark()
        p.mb.box((0, 0, 0), (0.03, 0.5 * T(zm), z1 - z0), p.m(mat), {})
        p.mb.transform_since(s, (cx(zm), 0.0, zm))

    # Marks: thin plates just proud of the face, wound to face outwards.
    def plate(x0, x1, z0, z1, front, mat):
        def y_at(z):
            return -(T(z) + 0.016) if front else (T(z) + 0.016)
        quad = [(x0, y_at(z0), z0), (x1, y_at(z0), z0), (x1, y_at(z1), z1), (x0, y_at(z1), z1)]
        ids = [p.mb.add_vertex(q, {}) for q in quad]
        p.mb.add_face(tuple(ids) if front else tuple(reversed(ids)), p.m(mat))

    def clear_of_crack(x0, x1, z, margin):
        if z < ZC - 0.05:
            return True
        c, g = cx(z), gap(z) / 2.0 + margin
        return x1 < c - g or x0 > c + g

    # Aydor: neat ruled lines of "words" (dashes of varied length), left-aligned, even spacing,
    # between a ruled border above and below (broken by the crack).
    for zr in (TOP - 0.58, P + 0.3):
        w = W(zr) * 0.78
        if zr < ZC:
            plate(-w, w, zr, zr + 0.025, True, "script")
        else:
            c, g = cx(zr), gap(zr) / 2.0 + 0.05
            plate(-w, c - g, zr, zr + 0.025, True, "script")
            plate(c + g, w, zr, zr + 0.025, True, "script")
    row = 0
    z = TOP - 0.75
    while z > P + 0.45:
        w = W(z) * 0.72
        x = -w
        while True:
            ln = 0.07 + 0.26 * (0.5 + 0.5 * table[(row * 17 + int((x + 2) * 31)) % 211])
            if x + ln > w:
                break
            if clear_of_crack(x, x + ln, z, 0.05):
                plate(x, x + ln, z, z + 0.045, True, "script")
            x += ln + 0.07
        row += 1
        z -= 0.2 if row % 6 else 0.34   # paragraph gap every six lines
    # Kargan: deep notch columns cut from the bottom up, wide bars and paired notches.
    col_x = [-0.86, -0.56, -0.26, 0.10, 0.40, 0.70]
    for ci, x in enumerate(col_x):
        z = P + 0.4 + 0.08 * (ci % 2)
        k = 0
        while z < TOP - 0.9 - 0.25 * (ci % 3):
            v = table[(ci * 29 + k * 7) % 211]
            if v > 0.25:      # wide bar
                x0, x1, h = x - 0.1, x + 0.1, 0.07
                if clear_of_crack(x0, x1, z, 0.06):
                    plate(x0, x1, z, z + h, False, "rune")
            elif v > -0.45:   # paired notches
                h = 0.09
                for dx in (-0.075, 0.035):
                    if clear_of_crack(x + dx, x + dx + 0.04, z, 0.06):
                        plate(x + dx, x + dx + 0.04, z, z + h, False, "rune")
            else:             # tall stroke
                h = 0.2
                if clear_of_crack(x - 0.022, x + 0.022, z, 0.06):
                    plate(x - 0.022, x + 0.022, z, z + h, False, "rune")
            z += h + 0.09
            k += 1
    # Two fallen chips on the plinth below the crack.
    lumpy(p, (0.55, -0.75, P + 0.07), (0.16, 0.12, 0.09), p.m("oath_dark"), seed=4501, segments=6, rings=4)
    lumpy(p, (-0.9, 0.7, P + 0.06), (0.12, 0.1, 0.07), p.m("oath"), seed=4502, segments=6, rings=4)


PROPS = dict(tree_oak=tree_oak, tree_pine=tree_pine, rock_a=rock_a, rock_b=rock_b, fence=fence, well=well,
             market_stall=market_stall, lamp_post=lamp_post, crate=crate, barrel=barrel, signpost=signpost,
             house=house, watchtower=watchtower, ant_tasi=ant_tasi)


def preview(objs, out_dir):
    cam = vc.setup_preview((320, 320), sun_dir=(-0.6, -1.0, 1.3))
    ground = vc.material("preview_ground", "#9a9384")
    bpy.ops.mesh.primitive_plane_add(size=60, location=(0, 0, 0))
    bpy.context.active_object.data.materials.append(ground)
    paths = []
    for o in objs:
        for other in objs:
            other.hide_render = other is not o
        lo = np.min([o.matrix_world @ v.co for v in o.data.vertices], axis=0)
        hi = np.max([o.matrix_world @ v.co for v in o.data.vertices], axis=0)
        c = (lo + hi) / 2
        size = float(np.max(hi - lo))
        d = size * 1.9 + 1.0
        vc.aim_camera(cam, (c[0] + d * 0.62, c[1] - d * 0.78, c[2] + d * 0.45), tuple(c), lens=50)
        paths.append(vc.render_to(os.path.join(out_dir, f"{o.name}.png")))
    vc.compose_sheet(paths, os.path.join(out_dir, "props_sheet.png"), 5)
    # The oath stone: both inscribed faces and the crack, next to a 1.8 m marker post for scale.
    stone = next((o for o in objs if o.name == "ant_tasi"), None)
    if stone is not None:
        for other in objs:
            other.hide_render = other is not stone
        bpy.ops.mesh.primitive_cylinder_add(radius=0.12, depth=1.8, location=(2.4, -1.6, 0.9))
        bpy.context.active_object.data.materials.append(vc.material("preview_marker", "#3c8cff"))
        bpy.context.scene.render.resolution_x = bpy.context.scene.render.resolution_y = 640
        for view, loc in (("front", (2.2, -9.5, 2.6)), ("back", (-2.2, 9.5, 2.6)), ("top", (3.0, -4.0, 8.5))):
            vc.aim_camera(cam, loc, (0.0, 0.0, 2.4 if view != "top" else 3.6), lens=40)
            vc.render_to(os.path.join(out_dir, f"ant_tasi_{view}.png"))


def main():
    args = vc.script_args()
    only = None
    if "--only" in args:
        only = set(args[args.index("--only") + 1].split(","))
    vc.reset_scene()
    objs = []
    for name, fn in PROPS.items():
        if only and name not in only:
            continue
        p = Prop()
        fn(p)
        obj = p.build(name)
        objs.append(obj)
        vc.export_glb(os.path.join(vc.SRC, "props", f"{name}.glb"), [obj])
        lo = np.min([v.co for v in obj.data.vertices], axis=0)
        hi = np.max([v.co for v in obj.data.vertices], axis=0)
        print(f"PROP {name}: {vc.triangle_count(obj)} tris, size {hi - lo}")
    if "--no-preview" not in args:
        # Spread them out so they don't overlap in the renders (only one is visible at a time).
        preview(objs, vc.ensure_dir(os.path.join(vc.PREVIEWS, "props")))


main()
