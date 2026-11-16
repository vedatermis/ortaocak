"""Loot (M10 "Ganimet"): the loot bag world prop and the loot item icons.

Usage (headless):
  blender --background --factory-startup --python tools/blender/gen_loot.py -- [--only bag,icons] [--no-preview]

Outputs:
  assets_src/props/loot_bag.glb          tied sack lying on the ground (~1350 triangles), origin at the
                                         ground centre, one PBR material (base colour, normal, ORM,
                                         emissive: embers in the opening and the clasp gem)
  assets_src/props/loot_bag{,_n,_orm,_e}.png   512^2 maps painted procedurally in UV space
  assets_src/ui/items/<icon>.png         128x128 inventory icons, straight sRGB alpha, rendered like the
                                         weapon icons (gen_weapons.render_icon: Cycles CPU, fixed seed,
                                         4x supersampled, soft dark halo):
      materials  ash_dust, slag_shard, ember_core   procedural meshes + procedural shaders
      currency   coin                               a stack of worn bronze / silver coins
      variants   dagger_thrall, sword_charred, mace_slag, shield_warden
                 the committed weapon meshes (assets_src/weapons/<src>.glb) re-rendered in their source
                 icon pose with a re-worked shader (charred / slag / ash / ember emblem)
  assets_src/previews/loot/loot_bag.png  the bag from three angles on a grass plane (the third one at
                                         game-camera distance)
  assets_src/previews/loot/icons.png     every item icon (old and new) at 64 and 48 px on the HUD colour

Everything is procedural (numpy with fixed seeds, Blender procedural textures); no downloads, no
generated images. Deterministic: own PNG writer; Cycles icon renders go through the weapons' stable
writer (an existing icon within 2/255 is kept).
"""

import math
import os
import sys

import bmesh
import bpy
import numpy as np
from mathutils import Matrix, Vector

sys.dont_write_bytecode = True
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import vo_common as vc  # noqa: E402
import gen_props as gp  # noqa: E402  (PBR material, colour helpers)
import gen_weapons as gw  # noqa: E402  (icon renderer, weapon table)
import gen_effects as ge  # noqa: E402  (SDF painter for the shield emblem)

PROP_DIR = os.path.join(vc.SRC, "props")
ICON_DIR = os.path.join(vc.SRC, "ui", "items")
WEAPON_DIR = os.path.join(vc.SRC, "weapons")
PREVIEW_DIR = os.path.join(vc.PREVIEWS, "loot")

TEX = 512
UI_DARK = (22, 24, 30)
SLOT = (35, 38, 43)

# =============================================================================================
# Numpy painting helpers

def smoothstep(a, b, x):
    t = np.clip((x - a) / (b - a), 0.0, 1.0)
    return t * t * (3.0 - 2.0 * t)


def hexs(h):
    """sRGB triple 0..1 from a hex string."""
    return ge.hexc(h)


def vnoise(u, v, fu, fv, seed):
    """Value noise periodic in u (period 1, fu cells) and v (period 1, fv cells)."""
    rng = np.random.RandomState(seed)
    g = rng.rand(fv, fu)
    x = u * fu
    y = v * fv
    x0 = np.floor(x).astype(np.int64)
    y0 = np.floor(y).astype(np.int64)
    fx = x - x0
    fy = y - y0
    fx = fx * fx * (3 - 2 * fx)
    fy = fy * fy * (3 - 2 * fy)
    x0m, x1m = x0 % fu, (x0 + 1) % fu
    y0m, y1m = y0 % fv, (y0 + 1) % fv
    a = g[y0m, x0m] * (1 - fx) + g[y0m, x1m] * fx
    b = g[y1m, x0m] * (1 - fx) + g[y1m, x1m] * fx
    return a * (1 - fy) + b * fy


def fbm(u, v, fu, fv, octaves, seed, gain=0.5):
    out = np.zeros_like(u)
    amp, tot = 1.0, 0.0
    for o in range(octaves):
        out += amp * vnoise(u, v, fu * 2 ** o, fv * 2 ** o, seed + 101 * o)
        tot += amp
        amp *= gain
    return out / tot


def voronoi(px, py, seeds):
    """F1, F2 distances and the nearest seed index for points (px, py) (brute force)."""
    d = np.hypot(px[..., None] - seeds[:, 0], py[..., None] - seeds[:, 1])
    idx = np.argsort(d, axis=-1)
    f1 = np.take_along_axis(d, idx[..., :1], -1)[..., 0]
    f2 = np.take_along_axis(d, idx[..., 1:2], -1)[..., 0]
    return f1, f2, idx[..., 0]


def normal_from_height(h, dx, dy, wrap_x):
    """OpenGL tangent-space normal map (+X = +U, +Y = +V) from a height field in mm; dx / dy: mm
    per texel along U (per row, broadcast) and along V. Rows run top (V = 1) to bottom."""
    if wrap_x:
        gx = (np.roll(h, -1, 1) - np.roll(h, 1, 1)) / (2.0 * dx)
    else:
        p = np.pad(h, ((0, 0), (1, 1)), mode='edge')
        gx = (p[:, 2:] - p[:, :-2]) / (2.0 * dx)
    p = np.pad(h, ((1, 1), (0, 0)), mode='edge')
    gy = -(p[2:, :] - p[:-2, :]) / (2.0 * dy)  # row + 1 = lower V
    n = np.stack([-gx, -gy, np.ones_like(h)], -1)
    n /= np.linalg.norm(n, axis=-1, keepdims=True)
    return n * 0.5 + 0.5


# =============================================================================================
# Loot bag geometry

NA = 24                 # segments around the sack
# Profile (radius, height) from the bottom pole up the outside, over the ruffled rim, down the inside
# of the opening to the ember bed (an inner pole). Metres, before the lumps / ellipse / slump.
PROFILE = [
    (0.000, 0.000), (0.100, 0.000), (0.170, 0.008), (0.207, 0.030), (0.222, 0.064), (0.219, 0.102),
    (0.203, 0.146), (0.174, 0.186), (0.134, 0.220), (0.092, 0.246), (0.060, 0.262),
    (0.046, 0.276),   # 11: neck, under the cord
    (0.050, 0.292), (0.068, 0.312), (0.088, 0.333), (0.098, 0.348),   # 15: rim
    (0.084, 0.352), (0.066, 0.336), (0.045, 0.320), (0.022, 0.313), (0.000, 0.311),
]
NECK = 11
RIM = 15
COAL_FROM = 17          # rings from here to the inner pole carry the ember bed
CROWN_ENV = {12: 0.25, 13: 0.55, 14: 0.85, 15: 1.0, 16: 1.0, 17: 0.75, 18: 0.45, 19: 0.2}
ELLIPSE_Y = 0.86
CORD_R = 0.011
SINK = 0.005

# Atlas regions (Blender UV, V up): body V 0.25..1 (U around, V down the profile), cord V 0.12..0.24,
# metal V 0.01..0.09 (U < 0.85), gem U 0.9..1, V 0..0.1.
BODY_V0, BODY_V1 = 0.25, 1.0
CORD_V0, CORD_V1 = 0.12, 0.24
METAL_U1, METAL_V0, METAL_V1 = 0.85, 0.01, 0.09
GEM_UV = (0.95, 0.05)


def profile_s():
    """Arc-length parameter of every profile ring, 0 (bottom pole) .. 1 (inner pole)."""
    p = np.array(PROFILE)
    seg = np.hypot(np.diff(p[:, 0]), np.diff(p[:, 1]))
    s = np.concatenate([[0.0], np.cumsum(seg)])
    return s / s[-1], float(s[-1])


def body_point(k, j):
    r, z = PROFILE[k]
    th = 2.0 * math.pi * j / NA
    # Contents pushing the cloth out: a few broad lumps on the body, strongest at mid height.
    if 1 <= k <= 10:
        env = math.sin(math.pi * min(z / 0.262, 1.0)) ** 0.8
        lump = 0.08 * math.sin(3 * th + 1.1) + 0.05 * math.sin(5 * th + 2.3) + 0.03 * math.sin(2 * th + 0.4)
        for (tc, zc, a) in ((4.3, 0.12, 0.07), (1.2, 0.08, 0.06), (2.6, 0.16, 0.05), (5.6, 0.06, 0.05)):
            dth = math.atan2(math.sin(th - tc), math.cos(th - tc))
            lump += a * math.exp(-(dth / 0.45) ** 2 - ((z - zc) / 0.05) ** 2)
        r *= 1.0 + lump * env
    # Gathered, ruffled crown above the tie.
    e = CROWN_ENV.get(k, 0.0)
    if e:
        r *= 1.0 + 0.22 * e * math.sin(6 * th + 0.7) + 0.06 * e * math.sin(11 * th)
        z += 0.010 * e * math.cos(6 * th + 0.7)
    x, y = r * math.cos(th), r * math.sin(th) * ELLIPSE_Y
    # Slumped: the top leans to +X / -Y (the bottom stays flat on the ground).
    q = (z / 0.35) ** 2
    return (x + 0.030 * q, y - 0.012 * q, z)


class MeshBuilder:
    """Vertices + faces with one UV per face corner (Blender loop order of from_pydata)."""

    def __init__(self):
        self.verts, self.faces, self.uvs = [], [], []

    def v(self, p):
        self.verts.append(tuple(float(c) for c in p))
        return len(self.verts) - 1

    def f(self, idx, uv):
        self.faces.append(tuple(idx))
        self.uvs.append([tuple(map(float, t)) for t in uv])

    def tube(self, path, radius, sides, closed, v0, v1, caps=True, out=None):
        """Tube along `path` (list of points); U along the path, V across in [v0, v1]."""
        P = [np.array(p, float) for p in path]
        n = len(P)
        rings = []
        for i in range(n):
            if closed:
                d = P[(i + 1) % n] - P[i - 1]
            else:
                d = P[min(i + 1, n - 1)] - P[max(i - 1, 0)]
            d /= np.linalg.norm(d)
            ref = np.array(out[i] if out is not None else (0.0, 0.0, 1.0), float)
            a = ref - d * np.dot(ref, d)
            if np.linalg.norm(a) < 1e-6:
                a = np.cross(d, (1.0, 0.0, 0.0))
            a /= np.linalg.norm(a)
            b = np.cross(d, a)
            r = radius[i] if hasattr(radius, "__len__") else radius
            rings.append([self.v(P[i] + r * (math.cos(2 * math.pi * k / sides) * a + math.sin(2 * math.pi * k / sides) * b))
                          for k in range(sides)])
        segs = n if closed else n - 1
        for i in range(segs):
            A, B = rings[i], rings[(i + 1) % n]
            ua, ub = i / segs, (i + 1) / segs
            for k in range(sides):
                k1 = (k + 1) % sides
                va, vb = v0 + (v1 - v0) * k / sides, v0 + (v1 - v0) * (k + 1) / sides
                self.f((A[k], A[k1], B[k1], B[k]), ((ua, va), (ua, vb), (ub, vb), (ub, va)))
        if caps and not closed:
            for ring, rev in ((rings[0], True), (rings[-1], False)):
                idx = list(reversed(ring)) if rev else ring
                c = 0.5 * (v0 + v1)
                self.f(idx, [(0.0, c)] * len(idx))
        return rings

    def lathe(self, prof, sides, frame, origin, uv_fn):
        """Closed solid of revolution about frame axis 2; prof: (r, h) with r = 0 at both ends."""
        a, b, ax = (np.array(x, float) for x in frame)
        o = np.array(origin, float)
        rings = []
        for (r, h) in prof:
            if r < 1e-9:
                rings.append([self.v(o + ax * h)])
            else:
                rings.append([self.v(o + ax * h + r * (math.cos(2 * math.pi * k / sides) * a +
                                                         math.sin(2 * math.pi * k / sides) * b))
                              for k in range(sides)])
        for i in range(len(rings) - 1):
            A, B = rings[i], rings[i + 1]
            for k in range(sides):
                k1 = (k + 1) % sides
                if len(A) == 1:
                    idx = (A[0], B[k1], B[k])
                elif len(B) == 1:
                    idx = (A[k], A[k1], B[0])
                else:
                    idx = (A[k], A[k1], B[k1], B[k])
                self.f(idx, [uv_fn(self.verts[q]) for q in idx])

    def to_object(self, name, mats=()):
        me = bpy.data.meshes.new(name)
        me.from_pydata(self.verts, [], self.faces)
        uvl = me.uv_layers.new(name="UVMap")
        flat = [c for face in self.uvs for t in face for c in t]
        assert len(flat) == 2 * len(me.loops), (len(flat), len(me.loops))
        uvl.data.foreach_set("uv", flat)
        for m in mats:
            me.materials.append(m)
        bm = bmesh.new()
        bm.from_mesh(me)
        bmesh.ops.recalc_face_normals(bm, faces=bm.faces)
        bm.to_mesh(me)
        bm.free()
        me.validate(clean_customdata=False)
        me.update()
        return vc.link(bpy.data.objects.new(name, me))


def build_bag_mesh():
    s_ring, _ = profile_s()
    mb = MeshBuilder()
    nr = len(PROFILE)
    rings = []
    for k in range(nr):
        if PROFILE[k][0] < 1e-9:
            z = PROFILE[k][1]
            rings.append([mb.v((0.030 * (z / 0.35) ** 2, -0.012 * (z / 0.35) ** 2, z))])
        else:
            rings.append([mb.v(body_point(k, j)) for j in range(NA)])

    def V(k):
        return BODY_V1 - (BODY_V1 - BODY_V0) * s_ring[k]

    for k in range(nr - 1):
        A, B = rings[k], rings[k + 1]
        for j in range(NA):
            j1 = (j + 1) % NA
            u0, u1 = j / NA, (j + 1) / NA
            if len(A) == 1:
                mb.f((A[0], B[j1], B[j]), (((u0 + u1) / 2, V(k)), (u1, V(k + 1)), (u0, V(k + 1))))
            elif len(B) == 1:
                mb.f((A[j], A[j1], B[0]), ((u0, V(k)), (u1, V(k)), ((u0 + u1) / 2, V(k + 1))))
            else:
                mb.f((A[j], A[j1], B[j1], B[j]), ((u0, V(k)), (u1, V(k)), (u1, V(k + 1)), (u0, V(k + 1))))
    body_faces = len(mb.faces)

    # Cord round the neck: the neck ring pushed out by the cord radius.
    neck = np.array([mb.verts[q] for q in rings[NECK]])
    c = neck.mean(0)
    path, outs = [], []
    for p in neck:
        d = p - c
        d[2] = 0.0
        d /= np.linalg.norm(d)
        path.append(p + d * CORD_R * 0.75 + np.array((0, 0, 0.002)))
        outs.append(d)
    mb.tube(path, CORD_R, 5, True, CORD_V0, CORD_V1, out=outs)

    # Knot + clasp at the front (-Y): column j = 18 (theta = -90 degrees).
    jf = 18
    front = np.array(outs[jf])
    knot = np.array(path[jf]) + front * 0.006
    # Two loose cord ends hanging down the front of the sack, following the cloth.
    col = [np.array(mb.verts[rings[k][jf]]) for k in range(4, NECK + 1)]
    for side, spread in ((-1.0, 0.022), (1.0, 0.030)):
        pts = []
        for t in np.linspace(0.0, 1.0, 5):
            z = knot[2] - 0.10 * t - 0.012 * t * t
            # Surface point of the front column at height z (rings are ordered by height here).
            zs = np.array([p[2] for p in col])
            i = int(np.clip(np.searchsorted(zs, z), 1, len(col) - 1))
            w = (z - zs[i - 1]) / max(zs[i] - zs[i - 1], 1e-6)
            sp = col[i - 1] * (1 - w) + col[i] * w
            radial = sp.copy()
            radial[2] = 0.0
            radial /= np.linalg.norm(radial)
            p = sp + radial * (CORD_R * 0.9) + np.array((side * spread * t, 0.0, 0.0))
            if t == 0.0:
                p = knot + np.array((side * 0.006, 0.0, -0.004))
            pts.append(p)
        mb.tube(pts, [CORD_R * 0.9, CORD_R * 0.85, CORD_R * 0.8, CORD_R * 0.75, CORD_R * 0.6], 5, False,
                CORD_V0, CORD_V1, out=[front] * len(pts))

    # Clasp: a bronze disc on the knot, facing out (and a little up), with a glowing gem.
    fwd = front * math.cos(math.radians(25)) + np.array((0, 0, 1.0)) * math.sin(math.radians(25))
    fwd /= np.linalg.norm(fwd)
    a = np.cross(fwd, (0.0, 0.0, 1.0))
    a /= np.linalg.norm(a)
    b = np.cross(fwd, a)
    centre = knot + front * 0.010

    def metal_uv(p, scale=0.03, u0=0.05, w=0.35):
        q = np.array(p) - centre
        return (u0 + w * (0.5 + 0.5 * np.clip(np.dot(q, a) / scale, -1, 1)),
                METAL_V0 + (METAL_V1 - METAL_V0) * (0.5 + 0.5 * np.clip(np.dot(q, b) / scale, -1, 1)))

    clasp = [(0.0, -0.003), (0.020, -0.003), (0.022, 0.0), (0.020, 0.003), (0.0, 0.003)]
    mb.lathe(clasp, 8, (a, b, fwd), centre, metal_uv)
    gem = [(0.0, 0.0), (0.0085, 0.0045), (0.0, 0.0105)]
    mb.lathe(gem, 4, (a, b, fwd), centre + fwd * 0.002, lambda p: GEM_UV)

    # Two coins spilled on the ground in front (one lying flat, one leaning on it).
    coin = [(0.0, 0.0), (0.02, 0.0), (0.02, 0.004), (0.0, 0.004)]
    for k, (pos, tilt, u0) in enumerate((((0.10, -0.245, 0.0), 0.0, 0.45), ((0.165, -0.215, 0.007), 18.0, 0.62))):
        rot = Matrix.Rotation(math.radians(tilt), 3, 'X') @ Matrix.Rotation(math.radians(30 * k + 10), 3, 'Z')
        fa, fb, fz = (np.array(rot @ Vector(e)) for e in ((1, 0, 0), (0, 1, 0), (0, 0, 1)))
        centre_c = np.array(pos)
        mb.lathe(coin, 10, (fa, fb, fz), centre_c,
                 lambda p, cc=centre_c, fa=fa, fb=fb, u0=u0: (u0 + 0.07 * (0.5 + 0.5 * np.dot(np.array(p) - cc, fa) / 0.02),
                                                             METAL_V0 + 0.08 * (0.5 + 0.5 * np.dot(np.array(p) - cc, fb) / 0.02)))

    obj = mb.to_object("loot_bag")
    me = obj.data
    # Origin: ground point below the centre of the sack's footprint (the coins excluded); sunk a few mm.
    co = np.array([v.co for v in me.vertices])
    body = co[:len([q for r in rings for q in r])]
    lo, hi = body.min(0), body.max(0)
    me.transform(Matrix.Translation(Vector((-(lo[0] + hi[0]) / 2, -(lo[1] + hi[1]) / 2, -lo[2] - SINK))))
    me.shade_smooth()
    me.set_sharp_from_angle(angle=math.radians(50))
    me.update()
    return obj, s_ring, body_faces


# =============================================================================================
# Loot bag textures (painted in UV space)

def bag_textures(s_ring, length):
    H = W = TEX
    U = (np.arange(W) + 0.5) / W
    Vrow = 1.0 - (np.arange(H) + 0.5) / H
    uu, vv = np.meshgrid(U, Vrow)
    col = np.zeros((H, W, 3))
    col[:] = hexs("#2a2018")
    rough = np.full((H, W), 0.92)
    metal = np.zeros((H, W))
    ao = np.ones((H, W))
    emis = np.zeros((H, W, 3))
    nrm = np.zeros((H, W, 3))
    nrm[:] = (0.5, 0.5, 1.0)

    # ---- body (canvas sack) ------------------------------------------------------------------
    rows = np.where((Vrow >= BODY_V0 - 2.0 / H) & (Vrow <= BODY_V1))[0]
    s = np.clip((BODY_V1 - Vrow[rows]) / (BODY_V1 - BODY_V0), 0, 1)
    su, ss = np.meshgrid(U, s)
    # Radius at each row (for the physical texel size along U) by interpolating the profile.
    prof_r = np.array([p[0] for p in PROFILE])
    r_row = np.maximum(np.interp(s, s_ring, prof_r), 0.008)
    du = 2.0 * math.pi * r_row * 1000.0 / W            # mm per texel along U (per row)
    dv = length * 1000.0 / ((BODY_V1 - BODY_V0) * H)  # mm per texel along V
    s_neck, s_rim, s_coal = s_ring[NECK], s_ring[RIM], s_ring[COAL_FROM]

    n1 = fbm(su, ss, 8, 6, 5, 11)
    n2 = fbm(su, ss, 24, 16, 3, 23)
    weave = vnoise(su, ss, 320, 240, 37) - 0.5
    h = 0.12 * weave
    # Broad folds; vertical wrinkles gathered under the tie (stronger towards the neck).
    phase = 2 * math.pi * (18 * su) + 3.0 * fbm(su, ss, 6, 4, 2, 41)
    gather = smoothstep(s_neck - 0.25, s_neck - 0.01, ss) * (ss < s_neck + 0.01)
    h += np.sin(phase) * (0.6 + 3.2 * gather)
    h += 2.0 * (fbm(su, ss, 5, 4, 3, 53) - 0.5)
    crown = (ss > s_neck + 0.01) & (ss < s_rim + 0.05)
    h += np.where(crown, 1.4 * np.sin(2 * math.pi * 14 * su + 2.0 * n1), 0.0)
    # Four vertical seams with stitches, from the bottom to the neck.
    seam_u = np.abs(((su * 4.0) % 1.0) - 0.5) * 2 * math.pi * r_row[:, None] * 1000 / 4  # mm from the seam
    on_body = (ss > 0.04) & (ss < s_neck - 0.01)
    groove = np.exp(-(seam_u / 1.6) ** 2) * on_body
    h -= 1.2 * groove
    stitch_v = (ss * length * 1000.0 / 9.0) % 1.0          # one stitch every 9 mm
    stitch = (np.abs(seam_u) < 3.2) & (stitch_v < 0.55) & on_body
    h += 0.7 * stitch
    # A square leather patch on the front-left side.
    pu = np.abs(su - 0.70) * 2 * math.pi * r_row[:, None]
    pv = np.abs(ss - 0.22) * length
    patch_d = np.maximum(pu - 0.055, pv - 0.045)            # m, < 0 inside
    patch = patch_d < 0
    patch_edge = (patch_d > -0.006) & (patch_d < -0.002) & ((((su * 2 * math.pi * r_row[:, None] + ss * length) * 1000 / 7.0) % 1.0) < 0.5)
    h += np.where(patch, 0.8, 0.0) + 0.6 * patch_edge

    canvas = hexs("#5c4630") * (0.75 + 0.5 * n1[..., None]) * (0.92 + 0.16 * n2[..., None])
    worn = smoothstep(0.55, 0.75, n1)[..., None] * smoothstep(0.08, 0.3, ss)[..., None] * (ss < s_neck)[..., None]
    c = canvas * (1 - 0.35 * worn) + hexs("#7a6247") * 0.35 * worn
    dirt = (1.0 - smoothstep(0.02, 0.16, ss))[..., None]
    c = c * (1 - 0.6 * dirt) + hexs("#231a12") * 0.6 * dirt
    c = np.where(patch[..., None], hexs("#5e3f26") * (0.8 + 0.4 * n2[..., None]), c)
    c = np.where(patch_edge[..., None] | stitch[..., None], hexs("#a08a62"), c)
    c = c * (1 - 0.5 * groove[..., None])
    # Under the tie and inside the opening: dark; the inner wall warms towards the embers.
    inside = smoothstep(s_rim, s_rim + 0.03, ss)
    c = c * (1 - 0.75 * inside[..., None]) + hexs("#140d09") * 0.75 * inside[..., None]
    a = 1.0 - 0.55 * np.exp(-((ss - s_neck) / 0.025) ** 2)
    a *= 1.0 - 0.6 * inside
    a *= 1.0 - 0.25 * (1.0 - smoothstep(0.0, 0.06, ss))
    a *= 1.0 - 0.12 * np.clip(-np.sin(phase), 0, 1) * gather
    a *= 1.0 - 0.3 * groove

    # Ember bed: coals in the bottom of the opening (polar coordinates on the inner disc).
    coal = ss > s_coal - 0.004
    rho = np.clip((1.0 - ss) / (1.0 - s_coal), 0, 1.2)
    ang = 2 * math.pi * su
    px, py = rho * np.cos(ang) * 3.2, rho * np.sin(ang) * 3.2
    rng = np.random.RandomState(7)
    g = np.array([(i, j) for i in range(-4, 4) for j in range(-4, 4)], float) + rng.rand(64, 2)
    heat = 0.35 + 0.65 * rng.rand(64)
    cr = np.where(coal)
    f1, f2, ix = voronoi(px[cr], py[cr], g)
    inner = smoothstep(0.04, 0.22, f2 - f1)
    glow = inner * heat[ix] * smoothstep(1.15, 0.85, rho[cr])
    t = np.clip(glow, 0, 1)[:, None]
    ember_col = hexs("#7a1606") * (1 - t) + hexs("#ff8a26") * t
    ember_col = np.where(t > 0.75, hexs("#ffb84a") * 1.0, ember_col)
    em_body = np.zeros(ss.shape + (3,))
    em_body[cr] = ember_col * t
    cbody = c.copy()
    cbody[cr] = hexs("#1a100a") * (1 - t) + hexs("#9a4a1a") * t
    hb = h.copy()
    hb[cr] = 1.5 * inner - 0.8
    rough_b = np.full(ss.shape, 0.93)
    rough_b[cr] = 0.85
    # Warm spill light on the inner wall just above the coals.
    spill = smoothstep(s_rim + 0.02, s_coal, ss) * (1 - coal)
    em_body += hexs("#5a1a06")[None, None] * spill[..., None] * 0.6

    col[rows] = cbody
    rough[rows] = rough_b
    ao[rows] = a
    emis[rows] = em_body
    nrm[rows] = normal_from_height(hb, du[:, None], dv, True)

    # ---- cord ---------------------------------------------------------------------------------
    rows = np.where((Vrow >= CORD_V0 - 2.0 / H) & (Vrow <= CORD_V1 + 2.0 / H))[0]
    cv_ = np.clip((Vrow[rows] - CORD_V0) / (CORD_V1 - CORD_V0), 0, 1)
    cu, cvv = np.meshgrid(U, cv_)
    tw = 2 * math.pi * (40 * cu + 3 * cvv)
    strand = np.abs(np.sin(tw))
    hc = 0.6 * strand + 0.1 * (vnoise(cu, cvv, 200, 20, 61) - 0.5)
    nc = fbm(cu, cvv, 16, 2, 3, 71)
    col[rows] = hexs("#7d6544") * (0.65 + 0.35 * strand[..., None]) * (0.85 + 0.3 * nc[..., None])
    rough[rows] = 0.9
    ao[rows] = 0.75 + 0.25 * strand
    nrm[rows] = normal_from_height(hc, 0.75, 0.6, True)

    # ---- metal (clasp, coins) and gem ---------------------------------------------------------
    rows = np.where(Vrow <= 0.105)[0]
    mu, mv = uu[rows], vv[rows]
    nm = fbm(mu, mv, 40, 8, 4, 83)
    bronze = hexs("#b8844a") * (0.8 + 0.4 * nm[..., None])
    tarnish = smoothstep(0.55, 0.75, fbm(mu, mv, 20, 4, 3, 89))[..., None]
    bronze = bronze * (1 - 0.5 * tarnish) + hexs("#3d4a3a") * 0.5 * tarnish
    gem = (mu >= 0.9)
    col[rows] = np.where(gem[..., None], hexs("#ff9a3a"), bronze)
    rough[rows] = np.where(gem, 0.3, 0.38 + 0.3 * nm)
    metal[rows] = np.where(gem, 0.0, 1.0)
    ao[rows] = 1.0
    emis[rows] = np.where(gem[..., None], hexs("#ff8a2c"), 0.0)
    nrm[rows] = normal_from_height(0.15 * (vnoise(mu, mv, 120, 24, 97) - 0.5), 0.5, 0.5, False)

    paths = dict(color=os.path.join(PROP_DIR, "loot_bag.png"), normal=os.path.join(PROP_DIR, "loot_bag_n.png"),
                 orm=os.path.join(PROP_DIR, "loot_bag_orm.png"), emissive=os.path.join(PROP_DIR, "loot_bag_e.png"))
    vc.write_png(paths["color"], vc.to_u8(np.clip(col, 0, 1)))
    vc.write_png(paths["normal"], vc.to_u8(nrm))
    vc.write_png(paths["orm"], vc.to_u8(np.stack([ao, rough, metal], -1)))
    vc.write_png(paths["emissive"], vc.to_u8(np.clip(emis, 0, 1)))
    lit = float(((emis[..., 0] > 40 / 255) | (emis[..., 1] > 40 / 255)).mean())
    return paths, lit


EMISSIVE_STRENGTH = 3.0


def build_bag():
    vc.reset_scene()
    obj, s_ring, _ = build_bag_mesh()
    _, length = profile_s()
    paths, lit = bag_textures(s_ring, length)
    mat = gp.build_material("loot_bag", paths, EMISSIVE_STRENGTH)
    obj.data.materials.append(mat)
    vc.export_glb(os.path.join(PROP_DIR, "loot_bag.glb"), [obj], textures=True)
    tris = vc.triangle_count(obj)
    v = np.array([q.co for q in obj.data.vertices])
    lo, hi = v.min(0), v.max(0)
    print(f"PROP loot_bag: {tris} triangles, {len(v)} vertices, size {np.round(hi - lo, 3)} m (x, y, z), "
          f"base z {lo[2]:.3f}, emissive texels {lit * 100:.2f} %")
    return obj


def bag_preview(obj):
    sc = bpy.context.scene
    cam = vc.setup_preview((512, 512), background="#9aa8b4", sun_dir=(0.5, -0.8, 1.0), strength=2.6)
    sc.eevee.taa_render_samples = 32
    # Grass ground: the terrain's grass layer tiled at its in-game size (2.6 m).
    bpy.ops.mesh.primitive_plane_add(size=40, location=(0, 0, 0))
    ground = bpy.context.active_object
    gm = bpy.data.materials.new("ground")
    gm.use_nodes = True
    nt = gm.node_tree
    tc = nt.nodes.new("ShaderNodeTexCoord")
    mp = nt.nodes.new("ShaderNodeMapping")
    mp.inputs["Scale"].default_value = (1 / 2.6, 1 / 2.6, 1)
    tx = nt.nodes.new("ShaderNodeTexImage")
    tx.image = bpy.data.images.load(os.path.join(vc.SRC, "textures", "terrain", "grass.png"), check_existing=False)
    nt.links.new(tc.outputs["Object"], mp.inputs["Vector"])
    nt.links.new(mp.outputs["Vector"], tx.inputs["Vector"])
    bs = nt.nodes.get("Principled BSDF")
    nt.links.new(tx.outputs["Color"], bs.inputs["Base Color"])
    bs.inputs["Roughness"].default_value = 0.95
    ground.data.materials.append(gm)
    tiles = []
    for k, (loc, tgt, lens) in enumerate((((0.75, -1.05, 0.62), (0.0, 0.0, 0.15), 50),
                                          ((-0.95, 0.55, 0.85), (0.0, 0.0, 0.14), 50),
                                          ((4.2, -6.0, 3.6), (0.0, 0.0, 0.15), 50))):
        vc.aim_camera(cam, loc, tgt, lens=lens)
        tiles.append(vc.render_to(os.path.join(PREVIEW_DIR, f"_tmp_bag_{k}.png")))
    vc.compose_sheet(tiles, os.path.join(PREVIEW_DIR, "loot_bag.png"), 3, stable=True)
    vc.remove_files(tiles)


# =============================================================================================
# Icons: shader-graph helpers

class G:
    """Small helper to build Cycles node graphs (constants or sockets as inputs)."""

    def __init__(self, mat):
        self.nt = mat.node_tree

    def node(self, kind, **props):
        n = self.nt.nodes.new(kind)
        for k, v in props.items():
            setattr(n, k, v)
        return n

    def put(self, sock, x):
        if isinstance(x, bpy.types.NodeSocket):
            self.nt.links.new(x, sock)
        elif x is not None:
            if sock.type == 'RGBA' and not hasattr(x, "__len__"):
                x = (x, x, x, 1.0)
            elif sock.type == 'RGBA' and len(x) == 3:
                x = (*x, 1.0)
            sock.default_value = x

    def math(self, op, a, b=0.0, clamp=False):
        n = self.node("ShaderNodeMath", operation=op, use_clamp=clamp)
        self.put(n.inputs[0], a)
        self.put(n.inputs[1], b)
        return n.outputs[0]

    def mix(self, fac, a, b, blend='MIX'):
        n = self.node("ShaderNodeMix", data_type='RGBA', blend_type=blend, clamp_factor=True)
        self.put(n.inputs["Factor"], fac)
        self.put([s for s in n.inputs if s.name == "A" and s.type == 'RGBA'][0], a)
        self.put([s for s in n.inputs if s.name == "B" and s.type == 'RGBA'][0], b)
        return [s for s in n.outputs if s.type == 'RGBA'][0]

    def ramp(self, x, a, b, c=0.0, d=1.0):
        n = self.node("ShaderNodeMapRange", interpolation_type='SMOOTHSTEP', clamp=True)
        self.put(n.inputs["Value"], x)
        n.inputs["From Min"].default_value = a
        n.inputs["From Max"].default_value = b
        n.inputs["To Min"].default_value = c
        n.inputs["To Max"].default_value = d
        return n.outputs["Result"]

    def coords(self, kind="Object"):
        return self.node("ShaderNodeTexCoord").outputs[kind]

    def xyz(self, vec):
        n = self.node("ShaderNodeSeparateXYZ")
        self.put(n.inputs[0], vec)
        return n.outputs

    def noise(self, vec, scale, detail=4.0, rough=0.55):
        n = self.node("ShaderNodeTexNoise")
        self.put(n.inputs["Vector"], vec)
        n.inputs["Scale"].default_value = scale
        n.inputs["Detail"].default_value = detail
        n.inputs["Roughness"].default_value = rough
        return n.outputs["Fac"]

    def voronoi(self, vec, scale, feature='F1', rand=1.0):
        n = self.node("ShaderNodeTexVoronoi", feature=feature)
        self.put(n.inputs["Vector"], vec)
        n.inputs["Scale"].default_value = scale
        n.inputs["Randomness"].default_value = rand
        return n.outputs

    def warp(self, vec, scale, amount):
        """vec + (noise colour - 0.5) * amount: breaks up the regular Voronoi cells."""
        n = self.node("ShaderNodeTexNoise")
        self.put(n.inputs["Vector"], vec)
        n.inputs["Scale"].default_value = scale
        n.inputs["Detail"].default_value = 3.0
        sub = self.node("ShaderNodeVectorMath", operation='SUBTRACT')
        self.put(sub.inputs[0], n.outputs["Color"])
        sub.inputs[1].default_value = (0.5, 0.5, 0.5)
        sc = self.node("ShaderNodeVectorMath", operation='SCALE')
        self.put(sc.inputs[0], sub.outputs[0])
        sc.inputs["Scale"].default_value = amount
        add = self.node("ShaderNodeVectorMath", operation='ADD')
        self.put(add.inputs[0], vec)
        self.put(add.inputs[1], sc.outputs[0])
        return add.outputs[0]

    def hsv(self, color, sat=1.0, val=1.0, hue=0.5):
        n = self.node("ShaderNodeHueSaturation")
        self.put(n.inputs["Color"], color)
        n.inputs["Hue"].default_value = hue
        n.inputs["Saturation"].default_value = sat
        n.inputs["Value"].default_value = val
        return n.outputs["Color"]

    def tex(self, path, non_color):
        n = self.node("ShaderNodeTexImage")
        n.image = bpy.data.images.load(path, check_existing=False)
        n.image.colorspace_settings.name = 'Non-Color' if non_color else 'sRGB'
        return n.outputs["Color"]

    def bump(self, height, strength, distance, normal=None):
        n = self.node("ShaderNodeBump")
        self.put(n.inputs["Height"], height)
        n.inputs["Strength"].default_value = strength
        n.inputs["Distance"].default_value = distance
        if normal is not None:
            self.put(n.inputs["Normal"], normal)
        return n.outputs["Normal"]


def lin(hexstr):
    return vc.hex_linear(hexstr)


def new_material(name):
    mat = bpy.data.materials.new(name)
    mat.use_nodes = True
    g = G(mat)
    return mat, g, mat.node_tree.nodes.get("Principled BSDF")


def set_bsdf(g, bsdf, color=None, rough=None, metal=None, normal=None, emit=None, emit_strength=None):
    g.put(bsdf.inputs["Base Color"], color)
    g.put(bsdf.inputs["Roughness"], rough)
    g.put(bsdf.inputs["Metallic"], metal)
    if normal is not None:
        g.put(bsdf.inputs["Normal"], normal)
    if emit is not None:
        g.put(bsdf.inputs["Emission Color"], emit)
        g.put(bsdf.inputs["Emission Strength"], emit_strength if emit_strength is not None else 1.0)


def weapon_maps(g, name):
    base = os.path.join(WEAPON_DIR, name)
    color = g.tex(base + ".png", False)
    orm = g.node("ShaderNodeSeparateColor")
    g.put(orm.inputs["Color"], g.tex(base + "_orm.png", True))
    nm = g.node("ShaderNodeNormalMap")
    g.put(nm.inputs["Color"], g.tex(base + "_n.png", True))
    return color, orm.outputs["Red"], orm.outputs["Green"], orm.outputs["Blue"], nm.outputs["Normal"]


# ---------------------------------------------------------------------------------------------
# Weapon measurements (grip frame, Blender axes: length along +Z)

def width_profile(v, step=0.005):
    z0, z1 = v[:, 2].min(), v[:, 2].max()
    zs = np.arange(z0, z1, step)
    w = []
    for z in zs:
        sl = v[np.abs(v[:, 2] - z) < step]
        w.append(0.0 if len(sl) == 0 else float(max(np.ptp(sl[:, 0]), np.ptp(sl[:, 1]))))
    return zs, np.array(w)


def guard_top(v):
    """Top of the cross-guard: the widest slab in the lower 45 % of the length, then up until the
    width drops under 60 % of it."""
    zs, w = width_profile(v)
    n = len(zs)
    lowpart = slice(0, int(n * 0.45))
    i = int(np.argmax(w[lowpart]))
    gw_ = w[i]
    while i < n - 1 and w[i] > 0.6 * gw_:
        i += 1
    return float(zs[i])


def head_bottom(v):
    """Bottom of a mace head: first slab above the grip wider than twice the handle."""
    zs, w = width_profile(v)
    handle = float(np.median(w[(zs > -0.05) & (zs < 0.1)]))
    for z, ww in zip(zs, w):
        if z > 0.1 and ww > 2.0 * handle:
            return float(z)
    return 0.3


# ---------------------------------------------------------------------------------------------
# Variant materials

ORANGE = (1.0, 0.36, 0.06)


def blade_profile(v, z_guard, step=0.01):
    """Blade half-width along its edge direction (the main axis of the blade's XY spread, which is
    not axis-aligned in the source) from the guard to the tip, normalised: (z0, z1, (dx, dy),
    centre, max half-width, [(t, w / w_max)])."""
    z1 = float(v[:, 2].max())
    z0 = z_guard + 0.005
    bl = v[(v[:, 2] > z0) & (v[:, 2] < z1 - 0.05)]
    xy = bl[:, :2] - bl[:, :2].mean(0)
    evals, evecs = np.linalg.eigh(xy.T @ xy)
    d = evecs[:, int(np.argmax(evals))]
    proj = v[:, 0] * d[0] + v[:, 1] * d[1]
    pts = []
    for z in np.arange(z0, z1, step):
        m = np.abs(v[:, 2] - z) < step * 0.6
        if m.any():
            pts.append((float(z), float(proj[m].min()), float(proj[m].max())))
    pc = float(np.median([0.5 * (a + b) for _, a, b in pts]))
    hw = [(z, max(b - pc, pc - a)) for z, a, b in pts]
    wmax = max(w for _, w in hw)
    curve = [((z - z0) / (z1 - z0), w / wmax) for z, w in hw] + [(1.0, 0.0)]
    return z0, z1, (float(d[0]), float(d[1])), pc, wmax, curve


def mat_sword_charred(z_guard, v):
    mat, g, bsdf = new_material("sword_charred")
    color, ao, rough, metal, normal = weapon_maps(g, "sword_soldier")
    obj = g.coords("Object")
    z = g.xyz(obj)[2]
    blade = g.ramp(z, z_guard, z_guard + 0.02)
    n = g.noise(obj, 18.0, 6.0, 0.6)
    char = g.mix(n, lin("#141110"), lin("#3b2a1f"))
    # Keep a little of the forged texture in the charred steel.
    char = g.mix(0.25, char, g.hsv(color, 0.3, 0.35), 'MULTIPLY')
    col = g.mix(blade, g.hsv(color, 0.7, 0.55), char)
    # Glowing cutting edge: grazing facets of the bevels, plus fine heat cracks in the blade.
    # Edge distance: |y - centre| over the measured half-width at this height (a float curve).
    z0, z1, (dx, dy), pc, wmax, curve = blade_profile(v, z_guard)
    fc = g.node("ShaderNodeFloatCurve")
    cm = fc.mapping
    cm.clip_min_y, cm.clip_max_y = 0.0, 1.0
    pts = cm.curves[0].points
    pts[0].location = curve[0]
    pts[1].location = curve[-1]
    for t, w in curve[1:-1]:
        pts.new(float(np.clip(t, 0.0, 1.0)), float(w))
    cm.update()
    g.put(fc.inputs["Value"], g.math('DIVIDE', g.math('SUBTRACT', z, z0), z1 - z0))
    half = g.math('MAXIMUM', g.math('MULTIPLY', fc.outputs["Value"], wmax), 0.002)
    xyz = g.xyz(obj)
    proj = g.math('ADD', g.math('MULTIPLY', xyz[0], dx), g.math('MULTIPLY', xyz[1], dy))
    across = g.math('DIVIDE', g.math('ABSOLUTE', g.math('SUBTRACT', proj, pc)), half)
    edge = g.math('MULTIPLY', g.ramp(across, 0.72, 0.95), blade)
    vo = g.voronoi(g.warp(obj, 40.0, 0.01), 60.0, 'DISTANCE_TO_EDGE')
    crack = g.math('MULTIPLY', g.ramp(vo["Distance"], 0.05, 0.0), g.ramp(n, 0.45, 0.6))
    heat = g.math('MULTIPLY', blade, g.math('ADD', edge, g.math('MULTIPLY', crack, 0.6)), clamp=True)
    col = g.mix(edge, col, lin("#c84a14"))
    emit = g.mix(heat, (0, 0, 0), (1.0, 0.30, 0.05))
    set_bsdf(g, bsdf, col, g.mix(blade, rough, 0.75), g.math('MULTIPLY', metal, g.math('SUBTRACT', 1.0, g.math('MULTIPLY', blade, 0.85))),
             normal, emit, 1.5)
    return mat


def mat_dagger_thrall(z_guard):
    mat, g, bsdf = new_material("dagger_thrall")
    color, ao, rough, metal, normal = weapon_maps(g, "dagger_hunter")
    obj = g.coords("Object")
    z = g.xyz(obj)[2]
    blade = g.ramp(z, z_guard, z_guard + 0.015)
    n = g.noise(obj, 30.0, 5.0, 0.6)
    ash = g.mix(n, lin("#cfc6b8"), lin("#857a6e"))
    ash = g.mix(0.35, ash, g.hsv(color, 0.0, 1.3), 'MULTIPLY')
    wrap = g.hsv(color, 0.1, 0.5)                    # ashen grey cloth, blackened fittings
    col = g.mix(blade, wrap, ash)
    vo = g.voronoi(obj, 38.0, 'DISTANCE_TO_EDGE')
    vein = g.math('MULTIPLY', g.ramp(vo["Distance"], 0.07, 0.0), g.ramp(g.noise(obj, 9.0, 2.0), 0.42, 0.55))
    heat = g.math('MULTIPLY', vein, blade, clamp=True)
    col = g.mix(heat, col, lin("#5a0a04"))
    emit = g.mix(heat, (0, 0, 0), (1.0, 0.12, 0.03))
    set_bsdf(g, bsdf, col, g.mix(blade, rough, 0.85), g.math('MULTIPLY', metal, g.math('SUBTRACT', 1.0, blade)), normal, emit, 1.6)
    return mat


def mat_mace_slag(z_head):
    mat, g, bsdf = new_material("mace_slag")
    color, ao, rough, metal, normal = weapon_maps(g, "mace_sun")
    obj = g.coords("Object")
    z = g.xyz(obj)[2]
    head = g.ramp(z, z_head - 0.015, z_head + 0.005)
    n = g.noise(obj, 22.0, 6.0, 0.65)
    rock = g.mix(n, lin("#151210"), lin("#3e352e"))
    shaft = g.hsv(color, 0.6, 0.45)
    col = g.mix(head, shaft, rock)
    vo = g.voronoi(g.warp(obj, 30.0, 0.04), 26.0, 'DISTANCE_TO_EDGE')
    crack = g.ramp(vo["Distance"], 0.05, 0.008)
    heat = g.math('MULTIPLY', crack, head, clamp=True)
    col = g.mix(heat, col, lin("#d8501a"))
    emit = g.mix(heat, (0, 0, 0), (1.0, 0.30, 0.05))
    crust = g.bump(g.math('ADD', n, g.math('MULTIPLY', crack, -0.6)), 0.8, 0.01, normal)
    set_bsdf(g, bsdf, col, g.mix(head, rough, 0.85), g.math('MULTIPLY', metal, g.math('SUBTRACT', 1.0, head)), None, emit, 1.2)
    # The crust bump only on the head: blend the bumped normal with the map's by the head mask.
    mixn = g.node("ShaderNodeMix", data_type='VECTOR', clamp_factor=True)
    g.put(mixn.inputs["Factor"], head)
    g.put([s for s in mixn.inputs if s.name == "A" and s.type == 'VECTOR'][0], normal)
    g.put([s for s in mixn.inputs if s.name == "B" and s.type == 'VECTOR'][0], crust)
    g.put(bsdf.inputs["Normal"], [s for s in mixn.outputs if s.type == 'VECTOR'][0])
    return mat


def emblem_image():
    """Ember emblem for the warden shield, painted with the skill-icon SDF painter: a ring with
    eight flame tongues and four inner sparks (white = emblem; coverage in all channels)."""
    cv = ge.Canvas()
    x, y = cv.x, cv.y
    cx = cy = 0.5
    ring = np.abs(ge.sd_circle(x, y, cx, cy, 0.29)) - 0.03
    tongues = np.full(x.shape, 1e9)
    for k in range(8):
        a = math.radians(k * 45 + 22.5)
        ca, sa = math.cos(a), math.sin(a)
        r0, r1, w = 0.27, 0.43 if k % 2 == 0 else 0.38, 0.06
        # A flame: base on the ring, tip bent slightly clockwise.
        tip = (cx + ca * r1 - sa * 0.03, cy + sa * r1 + ca * 0.03)
        p = [(cx + ca * r0 - sa * w, cy + sa * r0 + ca * w), tip, (cx + ca * r0 + sa * w, cy + sa * r0 - ca * w)]
        tongues = np.minimum(tongues, ge.sd_polygon(x, y, p))
    sparks = np.full(x.shape, 1e9)
    for k in range(4):
        a = math.radians(k * 90 + 45)
        sparks = np.minimum(sparks, ge.sd_circle(x, y, cx + 0.2 * math.cos(a), cy + 0.2 * math.sin(a), 0.028))
    d = np.minimum(np.minimum(ring, tongues), sparks)
    cv.over(cv.cover(d), np.array((1.0, 1.0, 1.0)))
    m = cv.rgba[..., 3].reshape(ge.ICON, ge.SS, ge.ICON, ge.SS).mean(axis=(1, 3))
    n = m.shape[0]
    # Through a temporary PNG (loaded and packed, then deleted): generated images are not reliably
    # seen by a background Cycles render.
    tmp = os.path.join(ICON_DIR, "_warden_emblem.tmp.png")
    vc.write_png(tmp, vc.to_u8(np.repeat(m[:, :, None], 3, 2)))
    img = bpy.data.images.load(tmp, check_existing=False)
    img.colorspace_settings.name = 'Non-Color'
    img.pack()
    os.remove(tmp)
    return img


def mat_shield_warden():
    mat, g, bsdf = new_material("shield_warden")
    color, ao, rough, metal, normal = weapon_maps(g, "shield_oak")
    obj = g.coords("Object")
    p = g.xyz(obj)
    # Emblem projected on the face (-Y) through the object's X / Z: 0.66 m across.
    comb = g.node("ShaderNodeCombineXYZ")
    g.put(comb.inputs[0], g.math('ADD', g.math('DIVIDE', p[0], 0.66), 0.5))
    g.put(comb.inputs[1], g.math('ADD', g.math('DIVIDE', p[2], 0.66), 0.5))
    tx = g.node("ShaderNodeTexImage", extension='CLIP', interpolation='Linear')
    tx.image = emblem_image()
    g.put(tx.inputs["Vector"], comb.outputs[0])
    nrm_obj = g.xyz(g.coords("Normal"))
    front = g.ramp(g.math('MULTIPLY', nrm_obj[1], -1.0), 0.3, 0.6)
    wood = g.ramp(metal, 0.5, 0.3)                    # planks (not the iron rim / boss)
    em = g.math('MULTIPLY', g.math('MULTIPLY', g.xyz(tx.outputs["Color"])[0], front), wood, clamp=True)
    planks = g.hsv(color, 0.25, 0.40)
    planks = g.mix(0.5, planks, lin("#3a2a26"), 'MULTIPLY')
    iron = g.hsv(color, 0.3, 0.8)
    col = g.mix(wood, iron, planks)
    col = g.mix(em, col, lin("#e8702a"))
    emit = g.mix(em, (0, 0, 0), (1.0, 0.30, 0.05))
    set_bsdf(g, bsdf, col, rough, metal, normal, emit, 1.4)
    return mat


VARIANTS = {
    # icon: (source weapon, material builder kind)
    "dagger_thrall": ("dagger_hunter", "dagger"),
    "sword_charred": ("sword_soldier", "sword"),
    "mace_slag": ("mace_sun", "mace"),
    "shield_warden": ("shield_oak", "shield"),
}


def render_icon_obj(obj, icon_name, display, samples=48):
    """gen_weapons.render_icon for any object: Cycles at 512^2, 4x box filter, dark halo, stable write."""
    big = gw.ICON_SIZE * gw.ICON_SUPERSAMPLE
    cam = gw.setup_cycles((big, big), samples, True, world="#9a9ca0", world_strength=0.7)
    obj.matrix_world = display
    bpy.context.view_layer.update()
    gw.fit_camera(cam, [obj], 0.04)
    rgba = gw.render_rgba(os.path.join(ICON_DIR, f"_{icon_name}.render.png"))
    icon = gw.add_shadow(gw.box_down_premul(rgba, gw.ICON_SUPERSAMPLE))
    path = os.path.join(ICON_DIR, f"{icon_name}.png")
    gw.write_png_stable_rgba(path, vc.to_u8(icon))
    a = icon[..., 3]
    print(f"ICON {icon_name}: {icon.shape[1]}x{icon.shape[0]}, coverage {float(a.mean()):.2f}")
    return path


def make_variant(icon, src, kind):
    vc.reset_scene()
    obj = gw.import_glb(os.path.join(WEAPON_DIR, f"{src}.glb"))
    v = gw.verts_np(obj)
    if kind == "sword":
        mat = mat_sword_charred(guard_top(v), v)
    elif kind == "dagger":
        mat = mat_dagger_thrall(guard_top(v))
    elif kind == "mace":
        mat = mat_mace_slag(head_bottom(v))
    else:
        mat = mat_shield_warden()
    obj.data.materials.clear()
    obj.data.materials.append(mat)
    cfg = gw.WEAPONS[src]
    return render_icon_obj(obj, icon, gw.display_matrix(cfg))


# ---------------------------------------------------------------------------------------------
# Material and currency icons: procedural meshes

def display(yaw, tilt, roll=0.0):
    """Turn about Z (yaw), tip the top towards the camera (-Y) by `tilt`, roll in the picture."""
    return (Matrix.Rotation(math.radians(roll), 4, 'Y') @ Matrix.Rotation(math.radians(tilt), 4, 'X')
            @ Matrix.Rotation(math.radians(yaw), 4, 'Z'))


def grid_heap(R, H, rings, segs, height_fn):
    """Polar grid heap (closed underneath by a flat disc)."""
    verts, faces = [], []
    top = [0]
    verts.append((0.0, 0.0, height_fn(0.0, 0.0)))
    ring_idx = [[0]]
    for i in range(1, rings + 1):
        r = R * i / rings
        idx = []
        for j in range(segs):
            th = 2 * math.pi * j / segs
            idx.append(len(verts))
            verts.append((r * math.cos(th), r * math.sin(th), height_fn(r / R, th)))
        ring_idx.append(idx)
    for i in range(rings):
        A, B = ring_idx[i], ring_idx[i + 1]
        for j in range(segs):
            j1 = (j + 1) % segs
            if len(A) == 1:
                faces.append((A[0], B[j], B[j1]))
            else:
                faces.append((A[j], B[j], B[j1], A[j1]))
    faces.append(tuple(reversed(ring_idx[-1])))
    return verts, faces


def icon_ash_dust():
    vc.reset_scene()
    rng = np.random.RandomState(31)
    bumps = [(rng.uniform(-0.6, 0.6), rng.uniform(-0.6, 0.6), rng.uniform(0.08, 0.2), rng.uniform(0.04, 0.09)) for _ in range(14)]

    def hfn(r, th):
        x, y = r * math.cos(th), r * math.sin(th)
        h = 0.62 * max(0.0, 1.0 - r * r) ** 1.1
        for (bx, by, br, bh) in bumps:
            h += bh * math.exp(-((x - bx) ** 2 + (y - by) ** 2) / (br * br)) * (1 - r)
        h += 0.02 * math.sin(7 * th + 3 * r) * r
        return h

    verts, faces = grid_heap(1.0, 0.4, 22, 56, hfn)
    obj = vc.mesh_object("ash_dust", verts, faces, None, [], smooth=True, recalc=True)
    # Charred chunks half buried in the ash.
    chunks = []
    for k, (cx, cy, sz) in enumerate(((-0.42, -0.28, 0.16), (0.30, -0.42, 0.12), (0.05, 0.05, 0.13),
                                      (0.55, 0.10, 0.10), (-0.20, 0.35, 0.11), (-0.62, 0.05, 0.08))):
        ch = convex_rock(f"chunk{k}", 12, (sz * 1.3, sz, sz * 0.8), 40 + k, 0.3)
        r = math.hypot(cx, cy)
        ch.location = (cx, cy, hfn(r, math.atan2(cy, cx)) - sz * 0.35)
        ch.rotation_euler = (0.3 * k, 0.5 * k, 1.1 * k)
        chunks.append(ch)
    mat, g, bsdf = new_material("ash")
    o = g.coords("Object")
    n = g.noise(o, 5.0, 8.0, 0.65)
    fine = g.noise(o, 90.0, 2.0, 0.5)
    col = g.mix(n, lin("#6a6460"), lin("#c2bcb4"))
    col = g.mix(g.ramp(fine, 0.6, 0.8), col, lin("#2f2b28"))
    vo = g.voronoi(o, 6.0, 'F1')
    cell = g.ramp(vo["Distance"], 0.30, 0.12)
    pick = g.ramp(g.xyz(vo["Color"])[0], 0.62, 0.68)
    up = g.ramp(g.xyz(g.coords("Normal"))[2], 0.2, 0.6)
    ember = g.math('MULTIPLY', g.math('MULTIPLY', cell, pick), up, clamp=True)
    col = g.mix(ember, col, lin("#ff7a28"))
    emit = g.mix(ember, (0, 0, 0), ORANGE)
    bump = g.bump(g.math('ADD', n, g.math('MULTIPLY', fine, 0.4)), 0.5, 0.05)
    set_bsdf(g, bsdf, col, 1.0, 0.0, bump, emit, 1.5)
    obj.data.materials.append(mat)
    cm, g2, b2 = new_material("charcoal")
    o2 = g2.coords("Object")
    n2 = g2.noise(o2, 14.0, 4.0, 0.6)
    vo2 = g2.voronoi(g2.warp(o2, 8.0, 0.1), 9.0, 'DISTANCE_TO_EDGE')
    glow = g2.math('MULTIPLY', g2.ramp(vo2["Distance"], 0.06, 0.01), g2.ramp(n2, 0.45, 0.55))
    set_bsdf(g2, b2, g2.mix(glow, g2.mix(n2, lin("#0c0a09"), lin("#2a2420")), lin("#c84a14")), 0.6, 0.0, None,
             g2.mix(glow, (0, 0, 0), (1.0, 0.30, 0.05)), 1.3)
    for ch in chunks:
        ch.data.materials.append(cm)
    with bpy.context.temp_override(active_object=obj, selected_editable_objects=[obj] + chunks):
        bpy.ops.object.join()
    return render_icon_obj(obj, "ash_dust", display(20.0, 26.0))


def convex_rock(name, n_pts, scale, seed, jag=0.18):
    rng = np.random.RandomState(seed)
    pts = rng.normal(size=(n_pts, 3))
    pts /= np.linalg.norm(pts, axis=1, keepdims=True)
    pts *= (1.0 - jag + jag * 2 * rng.rand(n_pts, 1))
    pts *= np.array(scale)
    bm = bmesh.new()
    for p in pts:
        bm.verts.new(p)
    bmesh.ops.convex_hull(bm, input=bm.verts)
    loose = [v for v in bm.verts if not v.link_faces]
    bmesh.ops.delete(bm, geom=loose, context='VERTS')
    me = bpy.data.meshes.new(name)
    bm.to_mesh(me)
    bm.free()
    obj = vc.link(bpy.data.objects.new(name, me))
    me.shade_flat()
    return obj


def icon_slag_shard():
    vc.reset_scene()
    obj = convex_rock("slag_shard", 26, (1.0, 0.36, 0.30), 5, 0.35)
    # Bend it into a shard: a sharp tip at +X, a broken, wider base at -X.
    me = obj.data
    for v in me.vertices:
        x = v.co.x
        t = (x + 1.0) / 2.0
        v.co.y *= 1.25 - 0.85 * t
        v.co.z *= 1.25 - 0.85 * t
        v.co.z += 0.12 * math.sin(2.5 * x)
    mat, g, bsdf = new_material("slag")
    o = g.coords("Object")
    n = g.noise(o, 3.5, 6.0, 0.6)
    col = g.mix(n, lin("#0e0c0b"), lin("#332a24"))
    vo = g.voronoi(g.warp(o, 2.5, 0.5), 2.6, 'DISTANCE_TO_EDGE')
    crack = g.ramp(vo["Distance"], 0.045, 0.008)
    crack = g.math('MULTIPLY', crack, g.ramp(g.noise(o, 1.8, 2.0), 0.4, 0.5))
    col = g.mix(crack, col, lin("#d8501a"))
    emit = g.mix(crack, (0, 0, 0), (1.0, 0.30, 0.05))
    bump = g.bump(g.math('ADD', n, g.math('MULTIPLY', crack, -0.5)), 0.5, 0.05)
    set_bsdf(g, bsdf, col, g.ramp(n, 0.3, 0.7, 0.6, 0.9), 0.0, bump, emit, 1.3)
    me.materials.append(mat)
    return render_icon_obj(obj, "slag_shard", display(-15.0, 25.0, 35.0))


def crystal(mb_verts, mb_faces, base, axis, length, radius, sides=6, tip=0.32, twist=0.0):
    axis = np.array(axis, float)
    axis /= np.linalg.norm(axis)
    a = np.cross(axis, (0.31, 0.7, 0.2))
    a /= np.linalg.norm(a)
    b = np.cross(axis, a)
    base = np.array(base, float)
    rings = []
    for (h, r) in ((0.0, radius * 0.75), (length * (1 - tip), radius)):
        idx = []
        for k in range(sides):
            th = 2 * math.pi * k / sides + twist
            idx.append(len(mb_verts))
            mb_verts.append(tuple(base + axis * h + r * (math.cos(th) * a + math.sin(th) * b)))
        rings.append(idx)
    top = len(mb_verts)
    mb_verts.append(tuple(base + axis * length))
    bot = len(mb_verts)
    mb_verts.append(tuple(base - axis * radius * 0.2))
    A, B = rings
    for k in range(sides):
        k1 = (k + 1) % sides
        mb_faces.append((A[k], A[k1], B[k1], B[k]))
        mb_faces.append((B[k], B[k1], top))
        mb_faces.append((A[k1], A[k], bot))


def icon_ember_core():
    vc.reset_scene()
    verts, faces = [], []
    crystal(verts, faces, (0.0, 0.0, 0.05), (0.08, -0.05, 1.0), 1.25, 0.24)
    crystal(verts, faces, (0.12, 0.05, 0.0), (0.85, 0.1, 0.75), 0.72, 0.15, twist=0.4)
    crystal(verts, faces, (-0.12, 0.02, 0.0), (-0.9, -0.15, 0.6), 0.62, 0.13, twist=0.9)
    crystal(verts, faces, (0.0, -0.1, 0.0), (0.2, -1.0, 0.45), 0.45, 0.10, twist=0.2)
    cry = vc.mesh_object("ember_core", verts, faces, None, [], smooth=False, recalc=True)
    rock = convex_rock("cradle", 18, (0.55, 0.45, 0.22), 9, 0.2)
    rock.location = (0.0, 0.0, -0.02)
    mat, g, bsdf = new_material("ember_crystal")
    lw = g.node("ShaderNodeLayerWeight")
    lw.inputs["Blend"].default_value = 0.45
    face = lw.outputs["Facing"]
    o = g.coords("Object")
    zz = g.xyz(o)[2]
    core = g.mix(g.ramp(face, 0.1, 0.8), (1.0, 0.55, 0.12), (0.8, 0.10, 0.01))
    core = g.mix(g.ramp(zz, 0.3, 1.3), core, (1.0, 0.32, 0.04))
    set_bsdf(g, bsdf, lin("#a8300c"), 0.15, 0.0, None, core, 0.85)
    bsdf.inputs["Coat Weight"].default_value = 0.6
    cry.data.materials.append(mat)
    rm, g2, b2 = new_material("cradle")
    o2 = g2.coords("Object")
    n2 = g2.noise(o2, 5.0, 6.0, 0.6)
    vo = g2.voronoi(o2, 4.0, 'DISTANCE_TO_EDGE')
    crack = g2.ramp(vo["Distance"], 0.05, 0.01)
    set_bsdf(g2, b2, g2.mix(crack, g2.mix(n2, lin("#110e0c"), lin("#3a302a")), lin("#ff7a20")), 0.75, 0.0,
             None, g2.mix(crack, (0, 0, 0), ORANGE), 2.0)
    rock.data.materials.append(rm)
    # One object for the framing (the renderer frames every vertex of `obj`).
    with bpy.context.temp_override(active_object=cry, selected_editable_objects=[cry, rock]):
        bpy.ops.object.join()
    return render_icon_obj(cry, "ember_core", display(25.0, 18.0, -8.0))


COIN_PROFILE = [(0.0, 0.02), (0.78, 0.02), (0.84, 0.0), (1.0, 0.0), (1.0, 0.15), (0.84, 0.15), (0.78, 0.13),
                (0.0, 0.13)]   # radius 1: recessed faces inside a raised rim


def icon_coin():
    vc.reset_scene()
    rng = np.random.RandomState(13)
    mb = MeshBuilder()
    mats = []

    def coin(centre, rot, mat_idx):
        R = np.array(rot)
        ax = [R @ np.array(e, float) for e in ((1, 0, 0), (0, 1, 0), (0, 0, 1))]
        c = np.array(centre, float)
        n0 = len(mb.faces)
        mb.lathe(COIN_PROFILE, 40, ax, c,
                 lambda p: (0.5 + 0.5 * float(np.dot(np.array(p) - c, ax[0])), 0.5 + 0.5 * float(np.dot(np.array(p) - c, ax[1]))))
        mats.extend([mat_idx] * (len(mb.faces) - n0))

    z = 0.0
    for k in range(5):
        off = rng.uniform(-0.08, 0.08, 2)
        tilt = (Matrix.Rotation(math.radians(rng.uniform(-4, 4)), 3, 'X') @ Matrix.Rotation(math.radians(rng.uniform(-4, 4)), 3, 'Y')
                @ Matrix.Rotation(math.radians(rng.uniform(0, 360)), 3, 'Z'))
        coin((off[0] + 0.35, off[1] + 0.1, z), tilt, 1 if k == 2 else 0)
        z += 0.155
    coin((-0.95, -0.45, 0.0), Matrix.Rotation(math.radians(4), 3, 'Y'), 1)
    coin((-0.55, -1.25, 0.0), Matrix.Rotation(math.radians(-3), 3, 'X'), 0)
    bronze, gb, bb = new_material("bronze")
    silver, gs, bs = new_material("silver")
    for (g, b, c0, c1) in ((gb, bb, "#9a6026", "#e8b070"), (gs, bs, "#8a8c90", "#e4e6ea")):
        uv = g.coords("UV")
        o = g.coords("Object")
        n = g.noise(o, 3.0, 3.0, 0.5)
        wear = g.noise(o, 12.0, 2.0, 0.5)
        col = g.mix(g.ramp(n, 0.25, 0.75), lin(c0), lin(c1))
        col = g.mix(g.ramp(wear, 0.7, 0.85, 0.0, 0.6), col, lin("#4a3a2a"))
        # Stamp: a ring of relief and a raised centre (distance from the UV centre of each coin).
        dv = g.node("ShaderNodeVectorMath", operation='DISTANCE')
        g.put(dv.inputs[0], uv)
        dv.inputs[1].default_value = (0.5, 0.5, 0.0)
        d = dv.outputs["Value"]
        ring = g.ramp(g.math('ABSOLUTE', g.math('SUBTRACT', d, 0.27)), 0.035, 0.015)
        boss = g.ramp(d, 0.14, 0.10)
        bump = g.bump(g.math('ADD', ring, boss), 0.5, 0.02)
        set_bsdf(g, b, col, g.ramp(wear, 0.3, 0.8, 0.3, 0.55), 1.0, bump)
    obj = mb.to_object("coin", [bronze, silver])
    obj.data.polygons.foreach_set("material_index", mats)
    obj.data.shade_smooth()
    obj.data.set_sharp_from_angle(angle=math.radians(35))
    obj.data.update()
    return render_icon_obj(obj, "coin", display(15.0, 40.0))


MATERIAL_ICONS = {"ash_dust": icon_ash_dust, "slag_shard": icon_slag_shard, "ember_core": icon_ember_core,
                  "coin": icon_coin}


# ---------------------------------------------------------------------------------------------
# Icon sheet preview

def area_matrix(n_in, n_out):
    W = np.zeros((n_out, n_in))
    f = n_in / n_out
    for i in range(n_out):
        a, b = i * f, (i + 1) * f
        for j in range(int(math.floor(a)), int(math.ceil(b))):
            W[i, j] = max(0.0, min(b, j + 1) - max(a, j))
    return W / W.sum(1, keepdims=True)


def icons_sheet():
    old = ["axe_hearth", "dagger_hunter", "mace_sun", "shield_oak", "staff_ember", "staff_frost", "sword_keep", "sword_soldier"]
    new = ["ash_dust", "dagger_thrall", "mace_slag", "shield_warden", "slag_shard", "ember_core", "coin", "sword_charred"]
    pad = 10
    cols = len(old)
    W = pad + cols * (64 + pad)
    H = pad + 2 * (64 + pad) + 2 * (48 + pad) + pad
    sheet = np.zeros((H, W, 3))
    sheet[:] = gp.srgb_to_lin(np.array(UI_DARK) / 255.0)
    slot = gp.srgb_to_lin(np.array(SLOT) / 255.0)
    y = pad
    for size in (64, 48):
        Wm = area_matrix(gw.ICON_SIZE, size)
        for names in (old, new):
            for k, name in enumerate(names):
                img = vc.read_image(os.path.join(ICON_DIR, f"{name}.png"))
                pm = np.concatenate([gp.srgb_to_lin(img[..., :3]) * img[..., 3:], img[..., 3:]], -1)
                small = np.einsum("ij,jkc,lk->ilc", Wm, pm, Wm)
                x = pad + k * (64 + pad) + (64 - size) // 2
                sheet[y - 1:y + size + 1, x - 1:x + size + 1] = slot
                reg = sheet[y:y + size, x:x + size]
                reg[:] = small[..., :3] + reg * (1 - small[..., 3:])
            y += size + pad
        y += pad
    vc.write_png(os.path.join(PREVIEW_DIR, "icons.png"), vc.to_u8(gp.lin_to_srgb(sheet)))


# ---------------------------------------------------------------------------------------------

def main():
    args = vc.script_args()
    only = set(args[args.index("--only") + 1].split(",")) if "--only" in args else {"bag", "icons"}
    preview = "--no-preview" not in args
    if "bag" in only:
        obj = build_bag()
        if preview:
            bag_preview(obj)
    if "icons" in only:
        for name, fn in MATERIAL_ICONS.items():
            fn()
        for icon, (src, kind) in VARIANTS.items():
            make_variant(icon, src, kind)
        if preview:
            icons_sheet()


if __name__ == "__main__":
    main()
