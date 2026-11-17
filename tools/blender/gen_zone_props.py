"""M13 "Üç Yurt" props: the gates between the zones (three looks), the settlement props of the two
homelands (Aydor: white-stone keep tower, curtain wall, house, sun banner; Kargan: rock-cut doorway,
forge, brazier, iron-bound palisade, stone hut) and the gate veils (effect meshes). Fully procedural:
geometry from boxes / wedges / lathes, materials from Blender procedural shader nodes (3D, in the
prop's own metres, so they run on across UV seams), baked with Cycles (CPU, fixed seed) into one
texture set per prop: base colour, normal (tangent space, OpenGL +Y), ORM (R ambient occlusion
with a ground plane, G roughness, B metallic from the materials' metal flags) and, where something
glows, an emissive map. The output has the zone-1 props' format (docs/ASSET_PIPELINE.md §4).

Usage (headless):
  blender --background --factory-startup --python tools/blender/gen_zone_props.py -- [--only gate_aydor,...] [--no-preview]

Outputs: assets_src/props/<name>.glb + <name>.png, _n.png, _orm.png (+ _e.png),
assets_src/props/zone_props_meta.json (bounds, triangles, footprint in engine axes: read by
gen_terrain.py / gen_zones.py; gates have a "multi" footprint, one hull per pillar),
assets_src/effects/gate_veil_<look>.glb + .png / _e.png, previews/props/<name>.png,
previews/props/m13_props_sheet.png. Blender conventions as everywhere: front -Y, left +X, Z up,
origin on the ground below the centre. Baked maps go through vo_common.write_png_stable (the path
tracer's AO may differ by a stray 1/255 between runs; within 2/255 the file is kept).
"""

import json
import math
import os
import sys

import bmesh
import bpy
import numpy as np

sys.dont_write_bytecode = True
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import vo_common as vc  # noqa: E402
import gen_props as gp  # noqa: E402  (main() does not run on import)

OUT_DIR = os.path.join(vc.SRC, "props")
FX_DIR = os.path.join(vc.SRC, "effects")
META_PATH = os.path.join(OUT_DIR, "zone_props_meta.json")


# ---------------------------------------------------------------------------------------------
# Procedural materials (Cycles nodes, object-space coordinates = metres in the prop)

class NT:
    """Small node-tree helper."""

    def __init__(self, mat):
        self.nt = mat.node_tree
        self.bsdf = self.nt.nodes["Principled BSDF"]
        tc = self.nt.nodes.new("ShaderNodeTexCoord")
        self.obj = tc.outputs["Object"]

    def n(self, kind, **props):
        node = self.nt.nodes.new(kind)
        for k, v in props.items():
            setattr(node, k, v)
        return node

    def link(self, a, b):
        self.nt.links.new(a, b)

    def scaled(self, scale):
        m = self.n("ShaderNodeMapping")
        m.inputs["Scale"].default_value = scale
        self.link(self.obj, m.inputs["Vector"])
        return m.outputs["Vector"]

    def noise(self, vec, scale, detail=4.0, rough=0.55):
        t = self.n("ShaderNodeTexNoise")
        t.inputs["Scale"].default_value = scale
        t.inputs["Detail"].default_value = detail
        t.inputs["Roughness"].default_value = rough
        self.link(vec, t.inputs["Vector"])
        return t.outputs["Fac"]

    def math(self, op, a, b=None, clamp=False):
        m = self.n("ShaderNodeMath", operation=op, use_clamp=clamp)
        for k, v in enumerate((a, b)):
            if v is None:
                continue
            if isinstance(v, (int, float)):
                m.inputs[k].default_value = v
            else:
                self.link(v, m.inputs[k])
        return m.outputs[0]

    def mix(self, fac, a, b):
        m = self.n("ShaderNodeMix", data_type='RGBA')
        for sock, v in ((m.inputs["Factor"], fac), (m.inputs[6], a), (m.inputs[7], b)):
            if isinstance(v, (int, float)):
                sock.default_value = v
            elif isinstance(v, tuple):
                sock.default_value = v
            else:
                self.link(v, sock)
        return m.outputs[2]

    def ramp(self, fac, lo, hi, a, b):
        """Linear map fac lo..hi -> 0..1 (clamped)."""
        m = self.n("ShaderNodeMapRange", clamp=True)
        self.link(fac, m.inputs["Value"])
        m.inputs["From Min"].default_value = lo
        m.inputs["From Max"].default_value = hi
        m.inputs["To Min"].default_value = a
        m.inputs["To Max"].default_value = b
        return m.outputs["Result"]

    def bump(self, height, strength, distance=0.02):
        b = self.n("ShaderNodeBump")
        b.inputs["Strength"].default_value = strength
        b.inputs["Distance"].default_value = distance
        self.link(height, b.inputs["Height"])
        self.link(b.outputs["Normal"], self.bsdf.inputs["Normal"])

    def z(self):
        s = self.n("ShaderNodeSeparateXYZ")
        self.link(self.obj, s.inputs["Vector"])
        return s.outputs["Z"]


def col(hexstr):
    return vc.hex_linear(hexstr) + (1.0,)


METAL = {}   # material name -> metallic (0..1); everything else is 0
EMIT = set()  # material names that glow


def new_mat(name):
    mat = bpy.data.materials.new(name)
    mat.use_nodes = True
    t = NT(mat)
    img = t.n("ShaderNodeTexImage")
    img.name = "bake_target"
    return mat, t


def stone_mat(name, c1, c2, mortar, course=0.45, block=0.95, rough=0.86, grime="#6a6358", grime_h=1.0, bump=0.8,
              rubble=0.0):
    """Coursed masonry in 3D: rows `course` m high (object Z), blocks `block` m long along X + Y
    (so walls facing either axis get joints), every other row offset half a block; mortar joints,
    a tone per block (white noise of the block index), fine noise, darker towards the ground.
    `rubble` warps the joints (irregular dark stone)."""
    mat, t = new_mat(name)
    sep = t.n("ShaderNodeSeparateXYZ")
    vec = t.obj
    if rubble:
        warp = t.n("ShaderNodeTexNoise")
        warp.inputs["Scale"].default_value = 1.6
        t.link(t.obj, warp.inputs["Vector"])
        off = t.n("ShaderNodeVectorMath", operation='SCALE')
        t.link(warp.outputs["Color"], off.inputs[0])
        off.inputs["Scale"].default_value = rubble
        add = t.n("ShaderNodeVectorMath", operation='ADD')
        t.link(t.obj, add.inputs[0])
        t.link(off.outputs[0], add.inputs[1])
        vec = add.outputs[0]
    t.link(vec, sep.inputs["Vector"])
    zr = t.math('DIVIDE', sep.outputs["Z"], course)
    row = t.math('FLOOR', zr)
    u = t.math('ADD', t.math('DIVIDE', t.math('ADD', sep.outputs["X"], sep.outputs["Y"]), block),
               t.math('MULTIPLY', row, 0.5))
    col_i = t.math('FLOOR', u)
    jv = t.math('ABSOLUTE', t.math('SUBTRACT', t.math('FRACT', u), 0.5))       # 0.5 at a vertical joint
    jh = t.math('ABSOLUTE', t.math('SUBTRACT', t.math('FRACT', zr), 0.5))      # 0.5 at a bed joint
    jw = t.math('MAXIMUM', t.math('MULTIPLY', jv, block), t.math('MULTIPLY', jh, course))  # m to the centre
    edge = t.math('MINIMUM', t.math('SUBTRACT', t.math('MULTIPLY', 0.5, block), t.math('MULTIPLY', jv, block)),
                  t.math('SUBTRACT', t.math('MULTIPLY', 0.5, course), t.math('MULTIPLY', jh, course)))
    comb = t.n("ShaderNodeCombineXYZ")
    t.link(col_i, comb.inputs["X"])
    t.link(row, comb.inputs["Y"])
    wn = t.n("ShaderNodeTexWhiteNoise", noise_dimensions='3D')
    t.link(comb.outputs["Vector"], wn.inputs["Vector"])
    fine = t.noise(t.obj, 9.0, 6.0, 0.6)
    tone = t.mix(wn.outputs["Value"], col(c1), col(c2))
    tone = t.mix(t.ramp(fine, 0.3, 0.75, 0.0, 0.35), tone, col(c2))
    joint = t.ramp(edge, 0.012, 0.035, 1.0, 0.0)
    c = t.mix(joint, tone, col(mortar))
    low = t.ramp(t.z(), 0.0, grime_h, 0.55, 0.0)
    c = t.mix(t.math('MULTIPLY', low, t.ramp(t.noise(t.obj, 2.2), 0.3, 0.7, 0.4, 1.0)), c, col(grime))
    t.link(c, t.bsdf.inputs["Base Color"])
    t.bsdf.inputs["Roughness"].default_value = rough
    h = t.math('ADD', t.ramp(edge, 0.0, 0.05, 0.0, 1.0), t.math('MULTIPLY', fine, 0.25))
    t.bump(h, bump, 0.03)
    del jw
    return mat


def rough_mat(name, c1, c2, scale=1.4, rough=0.9, bump=1.0, metal=0.0, detail=6.0, stretch=(1, 1, 1), lines=None):
    """Noise-toned surface (rock, turf, wood grain with `stretch`, iron with rust when metal)."""
    mat, t = new_mat(name)
    v = t.scaled(stretch)
    big = t.noise(v, scale, detail, 0.6)
    fine = t.noise(v, scale * 7.0, 4.0, 0.5)
    c = t.mix(t.ramp(big, 0.35, 0.68, 0.0, 1.0), col(c1), col(c2))
    c = t.mix(t.ramp(fine, 0.2, 0.8, 0.0, 0.25), c, col(c2))
    h = t.math('ADD', big, t.math('MULTIPLY', fine, 0.4))
    if lines:  # planks / tile rows: bands along object Z (or X) every `lines` m
        w = t.n("ShaderNodeTexWave", wave_type='BANDS', bands_direction='Z', wave_profile='SAW')
        w.inputs["Scale"].default_value = 1.0 / lines
        w.inputs["Distortion"].default_value = 0.6
        t.link(t.obj, w.inputs["Vector"])
        band = t.ramp(w.outputs["Fac"], 0.0, 0.12, 1.0, 0.0)
        c = t.mix(t.math('MULTIPLY', band, 0.7), c, col("#1c1612"))
        h = t.math('SUBTRACT', h, t.math('MULTIPLY', band, 0.8))
    t.link(c, t.bsdf.inputs["Base Color"])
    t.bsdf.inputs["Roughness"].default_value = rough
    t.bsdf.inputs["Metallic"].default_value = metal
    if metal:
        METAL[name] = metal
    t.bump(h, bump, 0.02)
    return mat


def flat_mat(name, c, rough=0.8, metal=0.0, emit=None, noise=0.15):
    mat, t = new_mat(name)
    fine = t.noise(t.obj, 6.0, 4.0, 0.5)
    cc = t.mix(t.ramp(fine, 0.2, 0.8, -noise, noise), col(c), col("#000000"))
    t.link(cc, t.bsdf.inputs["Base Color"])
    t.bsdf.inputs["Roughness"].default_value = rough
    t.bsdf.inputs["Metallic"].default_value = metal
    if metal:
        METAL[name] = metal
    if emit:
        t.bsdf.inputs["Emission Color"].default_value = col(emit)
        t.bsdf.inputs["Emission Strength"].default_value = 1.0
        EMIT.add(name)
    t.bump(fine, 0.15, 0.01)
    return mat


def coal_mat(name, glow="#ff7a26"):
    """Glowing coals: dark crust, hot cell centres (Voronoi) emit."""
    mat, t = new_mat(name)
    v = t.scaled((9.0, 9.0, 9.0))
    vor = t.n("ShaderNodeTexVoronoi", feature='DISTANCE_TO_EDGE')
    t.link(v, vor.inputs["Vector"])
    hot = t.ramp(vor.outputs["Distance"], 0.05, 0.25, 0.0, 1.0)
    t.link(t.mix(hot, col("#1a1210"), col("#5a2a14")), t.bsdf.inputs["Base Color"])
    em = t.mix(hot, col("#000000"), col(glow))
    t.link(em, t.bsdf.inputs["Emission Color"])
    t.bsdf.inputs["Emission Strength"].default_value = 1.0
    t.bsdf.inputs["Roughness"].default_value = 0.9
    t.bump(vor.outputs["Distance"], 0.6, 0.02)
    EMIT.add(name)
    return mat


def materials():
    M = {}
    M["stone_white"] = stone_mat("stone_white", "#e6dfd0", "#cdc4b1", "#9e9582", grime="#8a8170")
    M["stone_white_big"] = stone_mat("stone_white_big", "#e2dbcb", "#c9c0ac", "#9e9582", course=0.6, block=1.3,
                                     grime="#8a8170")
    M["plaster"] = rough_mat("plaster", "#e8e1d3", "#d4cab6", scale=1.2, rough=0.92, bump=0.25)
    M["stone_dark"] = stone_mat("stone_dark", "#555049", "#3a3633", "#1f1c1a", course=0.55, block=1.1, rough=0.9,
                                grime="#211d1a", bump=1.2, rubble=0.12)
    M["stone_grey"] = stone_mat("stone_grey", "#a39c90", "#867f74", "#5a544b", course=0.5, block=1.05,
                                grime="#5b5a46", rubble=0.05)
    M["rock"] = rough_mat("rock", "#45403d", "#2e2a28", scale=0.9, rough=0.92, bump=1.6, detail=8.0)
    M["wood"] = rough_mat("wood", "#7a5a3c", "#57402b", scale=3.0, rough=0.82, bump=0.5, stretch=(9.0, 9.0, 0.9))
    M["wood_dark"] = rough_mat("wood_dark", "#4a3627", "#2f2219", scale=3.0, rough=0.85, bump=0.5,
                               stretch=(9.0, 9.0, 0.9), lines=0.32)
    M["iron"] = rough_mat("iron", "#4a4a4e", "#5e4030", scale=2.5, rough=0.55, bump=0.3, metal=0.85)
    M["gold"] = flat_mat("gold", "#dcb466", rough=0.45, metal=0.5, noise=0.08)
    M["tiles"] = rough_mat("tiles", "#a5533a", "#7d3a28", scale=2.0, rough=0.78, bump=0.8, lines=0.28)
    M["turf"] = rough_mat("turf", "#5b5a40", "#3c3b2b", scale=2.5, rough=0.95, bump=1.2)
    M["cloth_blue"] = rough_mat("cloth_blue", "#3d5878", "#324a66", scale=4.0, rough=0.9, bump=0.2, stretch=(1, 1, 3))
    M["cloth_sun"] = flat_mat("cloth_sun", "#efe4c2", rough=0.85)
    M["dark"] = flat_mat("dark", "#141210", rough=0.95, noise=0.05)
    M["glass_warm"] = flat_mat("glass_warm", "#e8b060", rough=0.3, emit="#e89a48", noise=0.05)
    M["coal"] = coal_mat("coal")
    M["rune_ember"] = flat_mat("rune_ember", "#ff8a3a", rough=0.6, emit="#ff7a24", noise=0.05)
    M["rune_pale"] = flat_mat("rune_pale", "#cfe6ef", rough=0.6, emit="#a8d8f0", noise=0.05)
    M["rune_sun"] = flat_mat("rune_sun", "#fff0c0", rough=0.5, emit="#ffd890", noise=0.05)
    return M


# ---------------------------------------------------------------------------------------------
# Geometry

class G:
    """Polygon soup with material names; every part a closed shell (normals recalculated)."""

    def __init__(self, seed=1):
        self.verts, self.faces, self.mats = [], [], []
        self.rng = np.random.default_rng(seed)

    def hexa(self, c, mat):
        """Eight corners: bottom ring (4) then top ring (4), same order."""
        i = len(self.verts)
        self.verts += [tuple(float(x) for x in p) for p in c]
        for f in ((0, 3, 2, 1), (4, 5, 6, 7), (0, 1, 5, 4), (1, 2, 6, 5), (2, 3, 7, 6), (3, 0, 4, 7)):
            self.faces.append(tuple(i + k for k in f))
            self.mats.append(mat)

    def box(self, center, size, mat, rz=0.0, rx=0.0, ry=0.0, taper=1.0, jitter=0.0):
        cx, cy, cz = center
        sx, sy, sz = (s * 0.5 for s in size)
        pts = []
        for z in (-sz, sz):
            k = taper if z > 0 else 1.0
            for (x, y) in ((-sx, -sy), (sx, -sy), (sx, sy), (-sx, sy)):
                p = np.array([x * k, y * k, z])
                if jitter:
                    p = p + self.rng.uniform(-jitter, jitter, 3)
                pts.append(rot(p, rx, ry, rz) + np.array([cx, cy, cz]))
        self.hexa(pts, mat)

    def lathe(self, base, prof, sides, mat, phase=0.0, scale_y=1.0):
        """prof: [(radius, z), ...] bottom to top; caps where the radius is > 0."""
        i0 = len(self.verts)
        rings = []
        for (r, z) in prof:
            ring = []
            for k in range(sides):
                a = 2 * math.pi * k / sides + phase
                ring.append(len(self.verts))
                self.verts.append((base[0] + r * math.cos(a), base[1] + r * scale_y * math.sin(a), base[2] + z))
            rings.append(ring)
        for a, b in zip(rings, rings[1:]):
            for k in range(sides):
                k2 = (k + 1) % sides
                self.faces.append((a[k], a[k2], b[k2], b[k]))
                self.mats.append(mat)
        self.faces.append(tuple(reversed(rings[0])))
        self.mats.append(mat)
        self.faces.append(tuple(rings[-1]))
        self.mats.append(mat)
        return i0

    def rock(self, center, size, mat, rough=0.18, rings=7, segs=12, flat_bottom=True):
        """An irregular rock mass: a superellipsoid-ish blob of `size` (x, y, z) with radial jitter,
        its bottom flattened (it stands on / sinks into the ground)."""
        cx, cy, cz = center
        sx, sy, sz = (s * 0.5 for s in size)
        rows = []
        for i in range(1, rings):
            th = math.pi * i / rings
            row = []
            for j in range(segs):
                ph = 2 * math.pi * j / segs
                d = np.array([math.sin(th) * math.cos(ph), math.sin(th) * math.sin(ph), math.cos(th)])
                d = np.sign(d) * np.abs(d) ** 0.6  # boxier than a sphere
                k = 1.0 + self.rng.uniform(-rough, rough)
                p = np.array([cx + d[0] * sx * k, cy + d[1] * sy * k, cz + d[2] * sz * k])
                if flat_bottom:
                    p[2] = max(p[2], cz - sz)
                row.append(len(self.verts))
                self.verts.append(tuple(p))
            rows.append(row)
        top = len(self.verts)
        self.verts.append((cx, cy, cz + sz * (1 + self.rng.uniform(-rough, rough) * 0.5)))
        bot = len(self.verts)
        self.verts.append((cx, cy, cz - sz))
        for j in range(segs):
            j2 = (j + 1) % segs
            self.faces += [(top, rows[0][j], rows[0][j2]), (bot, rows[-1][j2], rows[-1][j])]
            self.mats += [mat, mat]
        for a, b in zip(rows, rows[1:]):
            for j in range(segs):
                j2 = (j + 1) % segs
                self.faces.append((a[j], b[j], b[j2], a[j2]))
                self.mats.append(mat)

    def cyl(self, base, r0, r1, h, sides, mat, phase=0.0):
        return self.lathe(base, [(r0, 0.0), (r1, h)], sides, mat, phase)

    def disc_y(self, center, r, depth, sides, mat, rays=0, ray_len=0.0):
        """A flat disc facing -Y (a sun emblem), optionally with triangular rays."""
        cx, cy, cz = center
        # Outline: a circle, or a star (rays) alternating the disc radius and the ray tips.
        if rays:
            pts = []
            for k in range(rays * 2):
                a = math.pi * k / rays
                rr = r + ray_len if k % 2 else r
                pts.append((rr * math.cos(a), rr * math.sin(a)))
        else:
            pts = [(r * math.cos(2 * math.pi * k / sides), r * math.sin(2 * math.pi * k / sides)) for k in range(sides)]
        n = len(pts)
        i0 = len(self.verts)
        for yy in (cy - depth / 2, cy + depth / 2):
            for (x, z) in pts:
                self.verts.append((cx + x, yy, cz + z))
        f, b = list(range(i0, i0 + n)), list(range(i0 + n, i0 + 2 * n))
        self.faces.append(tuple(f))
        self.faces.append(tuple(reversed(b)))
        self.mats += [mat, mat]
        for k in range(n):
            k2 = (k + 1) % n
            self.faces.append((f[k], b[k], b[k2], f[k2]))
            self.mats.append(mat)

    def wedge(self, center, r_in, r_out, a0, a1, depth, mat):
        """Arch voussoir in the XZ plane round `center`, from angle a0 to a1 (radians), Y +-depth/2."""
        cx, cy, cz = center
        pts = []
        for yy in (cy - depth / 2, cy + depth / 2):
            for (r, a) in ((r_in, a0), (r_out, a0), (r_out, a1), (r_in, a1)):
                pts.append((cx + r * math.cos(a), yy, cz + r * math.sin(a)))
        # reorder to bottom/top rings of hexa: front face ring (y-) then back ring (y+)
        self.hexa(pts, mat)

    def build(self, name, M, sharp=40.0):
        names = sorted(set(self.mats))
        me = bpy.data.meshes.new(name)
        me.from_pydata(self.verts, [], self.faces)
        me.validate()
        for n in names:
            me.materials.append(M[n])
        me.polygons.foreach_set("material_index", [names.index(m) for m in self.mats])
        obj = vc.link(bpy.data.objects.new(name, me))
        bm = bmesh.new()
        bm.from_mesh(me)
        bmesh.ops.remove_doubles(bm, verts=bm.verts, dist=1e-5)
        bmesh.ops.recalc_face_normals(bm, faces=bm.faces)
        bmesh.ops.triangulate(bm, faces=[f for f in bm.faces if len(f.verts) > 4])
        bm.to_mesh(me)
        bm.free()
        me.shade_smooth()
        me.set_sharp_from_angle(angle=math.radians(sharp))
        me.update()
        return obj


def rot(p, rx=0.0, ry=0.0, rz=0.0):
    x, y, z = p
    if rx:
        c, s = math.cos(rx), math.sin(rx)
        y, z = y * c - z * s, y * s + z * c
    if ry:
        c, s = math.cos(ry), math.sin(ry)
        x, z = x * c + z * s, -x * s + z * c
    if rz:
        c, s = math.cos(rz), math.sin(rz)
        x, y = x * c - y * s, x * s + y * c
    return np.array([x, y, z])


# ---------------------------------------------------------------------------------------------
# Props

GATE_W, GATE_P, GATE_D, GATE_HS = 3.4, 1.0, 1.4, 3.3   # opening width, pillar width, depth, spring height
GATE_LOOKS = {
    # stone, trim, glow; Aydor: white ashlar, gold pale-sun emblems front and back, two sun banners,
    # merlons. Kargan: dark stone, iron bands, iron spikes, braziers on the pillars, ember runes.
    # Eşik: weathered grey (the Ant Taşı's stone), pale rune inlays (the oath's third line).
    "aydor": dict(stone="stone_white", rune="rune_sun"),
    "kargan": dict(stone="stone_dark", rune="rune_ember"),
    "esik": dict(stone="stone_grey", rune="rune_pale"),
}


def gate(look):
    L = GATE_LOOKS[look]
    g = G(seed={"aydor": 11, "kargan": 12, "esik": 13}[look])
    st = L["stone"]
    W, P, D, Hs = GATE_W, GATE_P, GATE_D, GATE_HS
    xo = W / 2 + P / 2
    taper = 0.94 if look == "kargan" else 1.0
    for sx in (-1, 1):
        g.box((sx * xo, 0, 0.25), (P + 0.34, D + 0.34, 0.62), st)                       # plinth
        g.box((sx * xo, 0, 0.5 + (Hs - 0.5) / 2), (P, D, Hs - 0.5), st, taper=taper)     # shaft
        g.box((sx * xo, 0, Hs + 0.06), (P + 0.22, D + 0.2, 0.24), st)                    # impost
    c = (0.0, 0.0, Hs + 0.18)
    r_in, r_out = W / 2, W / 2 + P
    n = 11
    z_top = c[2] + r_out + 0.25
    for k in range(n):
        a0, a1 = math.pi * k / n + 0.006, math.pi * (k + 1) / n - 0.006
        rr = r_out + (0.12 if k == n // 2 else 0.0)
        g.wedge(c, r_in, rr, a0, a1, D + (0.12 if k == n // 2 else 0.0), st)
        # spandrel above the voussoir, up to the cornice (only beyond the keystone's reach)
        x0, x1 = r_out * math.cos(a0), r_out * math.cos(a1)
        z0, z1 = c[2] + r_out * math.sin(a0), c[2] + r_out * math.sin(a1)
        if min(z0, z1) < z_top - 0.05:
            pts = [(x0, -D / 2 + 0.04, z0), (x1, -D / 2 + 0.04, z1), (x1, D / 2 - 0.04, z1), (x0, D / 2 - 0.04, z0),
                   (x0, -D / 2 + 0.04, z_top), (x1, -D / 2 + 0.04, z_top), (x1, D / 2 - 0.04, z_top),
                   (x0, D / 2 - 0.04, z_top)]
            g.hexa(pts, st)
    for sx in (-1, 1):  # the spandrel blocks above the imposts out to the pillars' outer faces
        g.box((sx * (W / 2 + P * 0.5), 0, (Hs + 0.18 + z_top) / 2), (P * 0.98, D - 0.08, z_top - Hs - 0.18), st)
    g.box((0, 0, z_top + 0.16), (W + 2 * P + 0.5, D + 0.36, 0.32), st)                    # cornice
    g.box((0, 0, z_top + 0.46), (W + 2 * P + 0.2, D + 0.16, 0.3), st)
    top = z_top + 0.61
    # The keystone's rune (front and back; the Aydor gate carries its sun emblem there instead).
    for yy in ((-1, 1) if look != "aydor" else ()):
        g.box((0, yy * (D / 2 + 0.07), c[2] + r_out - 0.45), (0.22, 0.04, 0.5), L["rune"])
    if look == "aydor":
        for k in range(4):  # merlons
            x = -2.3 + k * (4.6 / 3)
            g.box((x, 0, top + 0.4), (0.7, D + 0.1, 0.8), st)
        for yy in (-1, 1):  # gold pale-sun emblems over the keystone (front and back)
            g.disc_y((0, yy * (D / 2 + 0.12), z_top - 0.05), 0.36, 0.08, 16, "gold", rays=12, ray_len=0.3)
            g.disc_y((0, yy * (D / 2 + 0.17), z_top - 0.05), 0.2, 0.04, 12, "rune_sun")
        for sx in (-1, 1):  # sun banners on the pillars' fronts
            g.box((sx * xo, -D / 2 - 0.08, 3.05), (0.9, 0.08, 0.08), "gold")
            g.box((sx * xo, -D / 2 - 0.06, 2.05), (0.78, 0.035, 1.9), "cloth_blue")
            g.disc_y((sx * xo, -D / 2 - 0.09, 2.45), 0.2, 0.025, 14, "cloth_sun", rays=10, ray_len=0.12)
    elif look == "kargan":
        for sx in (-1, 1):
            for zb in (0.95, 2.45):  # iron bands round the pillars
                g.box((sx * xo, 0, zb), (P * (1.0 - 0.06 * zb / Hs) + 0.07, D * (1.0 - 0.06 * zb / Hs) + 0.07, 0.13), "iron")
            # braziers on the pillars
            g.lathe((sx * xo, 0, top), [(0.16, 0.0), (0.12, 0.25), (0.42, 0.55), (0.48, 0.62), (0.4, 0.66)], 12, "iron")
            g.cyl((sx * xo, 0, top + 0.5), 0.38, 0.3, 0.14, 12, "coal")
        for k in range(5):  # iron spikes along the cornice
            x = -1.6 + k * 0.8
            g.cyl((x, 0, top - 0.02), 0.07, 0.0, 0.62, 6, "iron")
        for sx in (-1, 1):  # ember runes cut into the pillar fronts
            for k in range(3):
                g.box((sx * xo, -D / 2 - 0.035 + 0.07 * (k == 9), 1.2 + 0.42 * k), (0.05 + 0.05 * (k % 2), 0.03, 0.3),
                      "rune_ember")
    else:
        for sx in (-1, 1):  # pale rune inlays down the pillar fronts and backs
            for yy in (-1, 1):
                for k in range(4):
                    g.box((sx * xo + 0.12 * ((k % 2) * 2 - 1), yy * (D / 2 + 0.015), 0.95 + 0.5 * k),
                          (0.08, 0.03, 0.32), "rune_pale")
        for k in range(3):  # a few fallen-off blocks lying by the pillars
            sx = (-1, 1, 1)[k]
            g.box((sx * (xo + 1.0 + 0.2 * k), -0.6 + 0.9 * k, 0.18), (0.6, 0.45, 0.4), st, rz=0.5 * k + 0.3, jitter=0.04)
    obj = g.build(f"gate_{look}", MATS)
    return obj


def aydor_wall():
    g = G(21)
    L, T, H = 6.0, 1.4, 4.0
    g.box((0, 0, 0.3), (L + 0.02, T + 0.3, 0.7), "stone_white_big")
    g.box((0, 0, 0.6 + (H - 0.6) / 2), (L, T, H - 0.6), "stone_white", taper=0.985)
    g.box((0, 0, H + 0.1), (L + 0.02, T + 0.16, 0.2), "stone_white_big")
    for k in range(4):  # merlons on the outer (+Y) side, a low parapet on the inner side
        g.box((-2.25 + 1.5 * k, T / 2 - 0.3, H + 0.2 + 0.45), (0.85, 0.55, 0.9), "stone_white")
    g.box((0, -T / 2 + 0.2, H + 0.2 + 0.25), (L, 0.36, 0.5), "stone_white")
    for sx in (-1, 1):  # flat buttresses on the outside
        g.box((sx * 1.5, T / 2 + 0.22, 1.4), (0.8, 0.5, 2.8), "stone_white_big", taper=0.85)
    return g.build("aydor_wall", MATS)


def aydor_tower():
    g = G(22)
    S, H = 6.0, 12.6
    g.box((0, 0, 0.95), (S + 0.9, S + 0.9, 1.9), "stone_white_big", taper=0.9)
    g.box((0, 0, 1.6 + (H - 1.6) / 2), (S, S, H - 1.6), "stone_white", taper=0.97)
    g.box((0, 0, H + 0.2), (S + 0.6, S + 0.6, 0.45), "stone_white_big")
    top = H + 0.42
    for side in range(4):
        for k in range(4):
            u = -2.5 + k * (5.0 / 3)
            p = rot(np.array([u, -(S + 0.6) / 2 + 0.25, top + 0.55]), rz=side * math.pi / 2)
            g.box(tuple(p), (0.8, 0.5, 1.1), "stone_white", rz=side * math.pi / 2)
    for side in range(4):  # arrow slits and the lit upper windows
        for (u, z) in ((-1.4, 5.0), (1.4, 5.0), (0.0, 8.0)):
            p = rot(np.array([u, -S / 2 * 0.985 - 0.01, z]), rz=side * math.pi / 2)
            g.box(tuple(p), (0.18, 0.08, 1.0), "dark", rz=side * math.pi / 2)
        for u in (-1.3, 1.3):
            p = rot(np.array([u, -S / 2 * 0.975 - 0.02, 10.6]), rz=side * math.pi / 2)
            g.box(tuple(p), (0.42, 0.08, 0.9), "glass_warm" if side != 2 else "dark", rz=side * math.pi / 2)
    # Door (front), stone frame.
    g.box((0, -S / 2 - 0.45, 1.45), (2.4, 1.1, 2.9), "stone_white_big")       # porch
    g.box((0, -S / 2 - 1.02, 1.25), (1.4, 0.1, 2.3), "wood_dark")
    g.box((0, -S / 2 - 1.02, 2.55), (1.8, 0.16, 0.3), "stone_white")
    # The great pale-sun banner down the front, and a flag on the roof.
    g.box((0, -S / 2 - 0.2, 11.35), (2.2, 0.12, 0.12), "gold")
    g.box((0, -S / 2 - 0.12, 8.6), (1.9, 0.05, 5.4), "cloth_blue")
    g.disc_y((0, -S / 2 - 0.16, 9.9), 0.5, 0.04, 18, "cloth_sun", rays=14, ray_len=0.3)
    g.cyl((1.8, 1.8, top), 0.08, 0.06, 3.4, 8, "wood")
    g.box((1.8, 1.8 - 0.75, top + 2.85), (0.03, 1.4, 0.8), "cloth_blue")
    return g.build("aydor_tower", MATS)


def aydor_house():
    g = G(23)
    Lx, Ly, H = 6.4, 5.2, 3.2
    g.box((0, 0, 0.25), (Lx + 0.24, Ly + 0.24, 0.62), "stone_white_big")
    g.box((0, 0, 0.5 + (H - 0.5) / 2), (Lx, Ly, H - 0.5), "plaster")
    pitch = math.radians(30.0)
    half = Ly / 2 + 0.35
    rise = math.tan(pitch) * half
    for sy in (-1, 1):  # gable roof, ridge along X, terracotta
        g.box((0, sy * half / 2, H + rise / 2 + 0.02), (Lx + 0.6, half / math.cos(pitch) + 0.05, 0.16), "tiles",
              rx=-sy * pitch)
    for sx in (-1, 1):  # gable ends
        x = sx * (Lx / 2 - 0.1)
        i = len(g.verts)
        for xx in (x - 0.1, x + 0.1):
            g.verts += [(xx, -Ly / 2, H), (xx, Ly / 2, H), (xx, 0.0, H + rise - 0.1)]
        g.faces += [(i, i + 1, i + 2), (i + 5, i + 4, i + 3), (i, i + 3, i + 4, i + 1), (i + 1, i + 4, i + 5, i + 2),
                    (i + 2, i + 5, i + 3, i)]
        g.mats += ["plaster"] * 5
    g.box((0, 0, H + rise + 0.05), (Lx + 0.7, 0.3, 0.16), "tiles")  # ridge
    # Door and windows (front -Y), shutters, one side window each side.
    g.box((0.9, -Ly / 2 - 0.06, 1.15), (1.1, 0.12, 2.1), "wood_dark")
    g.box((0.9, -Ly / 2 - 0.1, 2.3), (1.5, 0.2, 0.22), "stone_white_big")
    for (x, y, rz) in ((-1.6, -Ly / 2 - 0.05, 0.0), (Lx / 2 + 0.05, 0.6, math.pi / 2), (-Lx / 2 - 0.05, -0.6, math.pi / 2)):
        g.box((x, y, 1.9), (0.8, 0.08, 0.9), "glass_warm", rz=rz)
        for s in (-1, 1):
            p = rot(np.array([s * 0.62, 0.0, 0.0]), rz=rz)
            g.box((x + p[0], y + p[1] - 0.02 * (rz == 0), 1.9), (0.42, 0.06, 1.0), "wood", rz=rz)
    g.box((-2.3, 1.2, H + rise * 0.55 + 0.6), (0.6, 0.6, 1.7), "stone_white")  # chimney
    return g.build("aydor_house", MATS)


def aydor_banner():
    g = G(24)
    g.box((0, 0, 0.18), (0.62, 0.62, 0.46), "stone_white_big")
    g.cyl((0, 0, 0.3), 0.075, 0.06, 4.3, 10, "wood")
    g.box((0, -0.06, 4.25), (1.25, 0.07, 0.07), "gold")
    g.lathe((0, 0, 4.58), [(0.0, 0.0), (0.08, 0.04), (0.11, 0.12), (0.06, 0.2), (0.0, 0.24)], 10, "gold")
    g.box((0, -0.06, 3.15), (1.0, 0.04, 2.15), "cloth_blue")
    for yy in (-1, 1):
        g.disc_y((0, -0.06 + yy * 0.03, 3.55), 0.24, 0.02, 16, "cloth_sun", rays=12, ray_len=0.14)
    # swallow-tail: two dark notches at the hem read as the cut
    return g.build("aydor_banner", MATS)


def kargan_door():
    g = G(31)
    # Rock mass behind (sinks into the cliff), jambs and a heavy lintel, the door set back.
    g.rock((0, 1.3, 2.9), (6.8, 2.8, 5.8), "rock", rough=0.12, rings=8, segs=16)
    g.rock((-2.75, 0.25, 1.8), (1.6, 1.8, 3.6), "rock", rough=0.15)
    g.rock((2.75, 0.25, 1.8), (1.6, 1.8, 3.6), "rock", rough=0.15)
    for sx in (-1, 1):
        g.box((sx * 1.55, -0.25, 1.85), (0.8, 0.9, 3.7), "stone_dark")
    g.box((0, -0.3, 4.1), (4.3, 1.0, 1.0), "stone_dark")
    g.box((0, -0.82, 4.1), (2.6, 0.06, 0.18), "rune_ember")
    g.box((0, 0.25, 1.75), (2.3, 0.2, 3.5), "wood_dark")
    for zb in (0.7, 1.75, 2.8):
        g.box((0, 0.13, zb), (2.32, 0.06, 0.14), "iron")
    g.cyl((0.75, 0.08, 1.55), 0.09, 0.09, 0.08, 8, "iron")
    g.box((0, -0.9, 0.06), (3.8, 1.3, 0.32), "stone_dark")  # step
    for sx in (-1, 1):  # wall braziers either side
        g.lathe((sx * 2.45, -0.75, 2.6), [(0.08, 0.0), (0.06, 0.15), (0.26, 0.35), (0.3, 0.42), (0.24, 0.45)], 10, "iron")
        g.cyl((sx * 2.45, -0.75, 2.92), 0.22, 0.17, 0.1, 10, "coal")
    return g.build("kargan_door", MATS)


def kargan_forge():
    g = G(32)
    g.box((0, 0, 0.45), (2.5, 1.7, 1.0), "stone_dark")
    g.box((0, 0, 0.98), (1.7, 1.0, 0.12), "coal")
    for sx in (-1, 1):  # back posts (stone) and front posts (iron)
        g.box((sx * 1.05, 0.6, 1.9), (0.36, 0.36, 2.0), "stone_dark")
        g.cyl((sx * 1.05, -0.65, 0.95), 0.06, 0.06, 1.55, 8, "iron")
    g.box((0, 0, 3.05), (2.6, 1.8, 0.7), "stone_dark", taper=0.45)
    g.box((0, 0.1, 3.9), (0.6, 0.6, 1.3), "stone_dark")
    # Anvil on a stump, quench trough, a few iron bars.
    g.cyl((1.95, -0.35, 0.0), 0.32, 0.3, 0.55, 12, "wood")
    g.box((1.95, -0.35, 0.65), (0.26, 0.5, 0.2), "iron")
    g.box((1.95, -0.35, 0.83), (0.3, 0.75, 0.16), "iron", taper=0.85)
    g.box((-1.95, -0.2, 0.28), (0.6, 1.3, 0.56), "wood_dark")
    g.box((-1.95, -0.2, 0.55), (0.5, 1.2, 0.02), "dark")
    for k in range(3):
        g.box((1.4 - 0.1 * k, 0.75, 0.45), (0.05, 0.05, 0.9), "iron", rx=0.25, ry=0.15 * k)
    return g.build("kargan_forge", MATS)


def kargan_brazier():
    g = G(33)
    g.cyl((0, 0, 0.0), 0.42, 0.36, 0.3, 10, "stone_dark")
    g.cyl((0, 0, 0.28), 0.09, 0.07, 0.7, 8, "iron")
    for k in range(3):
        a = 2 * math.pi * k / 3
        g.box((0.17 * math.cos(a), 0.17 * math.sin(a), 0.62), (0.05, 0.05, 0.62), "iron", rz=a, ry=0.0,
              rx=0.0)
    g.lathe((0, 0, 0.92), [(0.12, 0.0), (0.2, 0.08), (0.46, 0.32), (0.52, 0.4), (0.44, 0.43)], 12, "iron")
    g.lathe((0, 0, 1.18), [(0.44, 0.0), (0.42, 0.1), (0.28, 0.2), (0.0, 0.24)], 12, "coal")
    return g.build("kargan_brazier", MATS)


def kargan_palisade():
    g = G(34)
    n = 12
    for k in range(n):
        x = -2.0 + (k + 0.5) * 4.0 / n
        h = 2.65 + 0.3 * g.rng.uniform()
        r = 0.16 + 0.02 * g.rng.uniform()
        g.lathe((x, 0, 0), [(r, 0.0), (r * 0.95, h), (0.0, h + 0.45)], 7, "wood", phase=g.rng.uniform(0, 1))
    for zb in (0.75, 2.05):
        for yy in (-1, 1):
            g.box((0, yy * 0.17, zb), (4.0, 0.05, 0.16), "iron")
    return g.build("kargan_palisade", MATS)


def kargan_hut():
    g = G(35)
    R = 3.0
    g.lathe((0, 0, 0), [(R + 0.15, 0.0), (R + 0.1, 0.5), (R, 2.3), (R * 0.97, 2.4)], 20, "stone_dark")
    g.lathe((0, 0, 2.2), [(R + 0.45, 0.0), (R + 0.35, 0.22), (1.0, 2.2), (0.45, 2.75), (0.0, 2.85)], 20, "turf")
    g.cyl((0, 0, 4.7), 0.35, 0.3, 0.7, 8, "stone_dark")
    # Door with a stone frame and iron bands (front -Y), two small shuttered windows.
    g.box((0, -R - 0.05, 1.05), (1.2, 0.3, 2.1), "wood_dark")
    for zb in (0.5, 1.5):
        g.box((0, -R - 0.21, zb), (1.22, 0.04, 0.12), "iron")
    for sx in (-1, 1):
        g.box((sx * 0.8, -R - 0.12, 1.15), (0.36, 0.4, 2.3), "stone_dark")
    g.box((0, -R - 0.15, 2.4), (2.0, 0.5, 0.4), "stone_dark")
    for a in (math.radians(-50), math.radians(-130)):
        x, y = (R + 0.02) * math.cos(a), (R + 0.02) * math.sin(a)
        g.box((x, y, 1.5), (0.5, 0.1, 0.5), "glass_warm", rz=a + math.pi / 2)
    return g.build("kargan_hut", MATS)


# name -> (builder, texture size, sink (m), footprint ("hull", z_max) / ("disc", z0, z1) / ("pillars",),
# emissive strength (None: none))
PROPS = {
    "gate_aydor": (lambda: gate("aydor"), 1024, 0.12, ("pillars",), 2.0),
    "gate_kargan": (lambda: gate("kargan"), 1024, 0.12, ("pillars",), 3.0),
    "gate_esik": (lambda: gate("esik"), 1024, 0.12, ("pillars",), 2.0),
    "aydor_wall": (aydor_wall, 1024, 0.15, ("hull", 2.2), None),
    "aydor_tower": (aydor_tower, 1024, 0.2, ("hull", 2.2), 2.5),
    "aydor_house": (aydor_house, 1024, 0.15, ("hull", 2.2), 2.0),
    "aydor_banner": (aydor_banner, 512, 0.08, ("disc", 0.0, 0.45), None),
    "kargan_door": (kargan_door, 1024, 0.15, ("hull", 2.2), 3.0),
    "kargan_forge": (kargan_forge, 1024, 0.1, ("hull", 2.2), 3.0),
    "kargan_brazier": (kargan_brazier, 512, 0.06, ("disc", 0.0, 0.3), 3.0),
    "kargan_palisade": (kargan_palisade, 512, 0.25, ("hull", 2.2), None),
    "kargan_hut": (kargan_hut, 1024, 0.15, ("hull", 2.2), 2.0),
}


# ---------------------------------------------------------------------------------------------
# Bake

def setup_bake():
    sc = bpy.context.scene
    sc.render.engine = 'CYCLES'
    sc.cycles.device = 'CPU'
    sc.cycles.seed = 0
    sc.cycles.use_denoising = False
    sc.render.bake.margin = 8
    sc.render.bake.use_clear = True


def new_image(name, size):
    img = bpy.data.images.new(name, size, size, alpha=False, float_buffer=True)
    img.colorspace_settings.name = 'Non-Color'
    return img


def bake(obj, kind, img, samples, **kw):
    for m in obj.data.materials:
        n = m.node_tree.nodes["bake_target"]
        n.image = img
        m.node_tree.nodes.active = n
    bpy.context.scene.cycles.samples = samples
    bpy.ops.object.select_all(action='DESELECT')
    obj.select_set(True)
    bpy.context.view_layer.objects.active = obj
    bpy.ops.object.bake(type=kind, **kw)
    w, h = img.size
    px = np.empty(w * h * 4, np.float32)
    img.pixels.foreach_get(px)
    return px.reshape(h, w, 4)[::-1, :, :3].copy()


def dilate(mask, n):
    m = mask.copy()
    for _ in range(n):
        p = np.pad(m, 1)
        m = m | p[:-2, 1:-1] | p[2:, 1:-1] | p[1:-1, :-2] | p[1:-1, 2:]
    return m


def bake_prop(name, obj, size, strength):
    # UVs: smart projection, islands scaled to one texel density.
    bpy.ops.object.select_all(action='DESELECT')
    obj.select_set(True)
    bpy.context.view_layer.objects.active = obj
    obj.data.uv_layers.new(name="UVMap")
    bpy.ops.object.mode_set(mode='EDIT')
    bpy.ops.mesh.select_all(action='SELECT')
    bpy.ops.uv.smart_project(angle_limit=math.radians(60.0), island_margin=0.006, area_weight=0.0,
                             correct_aspect=True, scale_to_bounds=False)
    bpy.ops.object.mode_set(mode='OBJECT')
    # A ground plane for the AO (props stand on the ground).
    bpy.ops.mesh.primitive_plane_add(size=60, location=(0, 0, 0))
    ground = bpy.context.active_object
    ground.data.materials.append(bpy.data.materials.new("bake_ground"))
    setup_bake()
    colour = bake(obj, 'DIFFUSE', new_image("c", size), 4, pass_filter={'COLOR'})
    normal = bake(obj, 'NORMAL', new_image("n", size), 4, normal_space='TANGENT')
    rough = bake(obj, 'ROUGHNESS', new_image("r", size), 4)
    ao = bake(obj, 'AO', new_image("a", size), 48)
    emis = None
    if strength:
        emis = bake(obj, 'EMIT', new_image("e", size), 4)
    bpy.data.objects.remove(ground)
    # Metallic from the materials' flags, rasterised in UV space (dilated over the bake margin).
    metal = np.zeros((size, size), np.float32)
    for mi, m in enumerate(obj.data.materials):
        if m.name in METAL:
            faces = [p for p in obj.data.polygons if p.material_index == mi]
            mask = dilate(gp.rasterize_faces(obj, faces, size), 4)
            metal = np.maximum(metal, mask * METAL[m.name])
    ao = np.clip(0.25 + 0.75 * ao[..., 0], 0.0, 1.0)  # soften: ambient light is not all sky
    orm = np.stack([ao, rough[..., 0], metal], -1)
    paths = dict(color=os.path.join(OUT_DIR, f"{name}.png"), normal=os.path.join(OUT_DIR, f"{name}_n.png"),
                 orm=os.path.join(OUT_DIR, f"{name}_orm.png"))
    vc.write_png_stable(paths["color"], vc.to_u8(gp.lin_to_srgb(colour)))
    vc.write_png_stable(paths["normal"], vc.to_u8(normal))
    vc.write_png_stable(paths["orm"], vc.to_u8(orm))
    lit = 0.0
    if emis is not None:
        paths["emissive"] = os.path.join(OUT_DIR, f"{name}_e.png")
        e8 = vc.to_u8(gp.lin_to_srgb(emis))
        vc.write_png_stable(paths["emissive"], e8)
        lit = float(((e8[..., 0] > 40) | (e8[..., 1] > 40)).mean())
    for img in list(bpy.data.images):
        if img.name in ("c", "n", "r", "a", "e"):
            bpy.data.images.remove(img)
    return paths, lit


def footprint(v, spec):
    eng = np.stack([-v[:, 0], -v[:, 1]], 1)
    if spec[0] == "hull":
        sel = eng[v[:, 2] < spec[1]]
        return dict(kind="hull", points=[[round(x, 3), round(z, 3)] for x, z in gp.convex_hull(sel)])
    if spec[0] == "disc":
        sel = eng[(v[:, 2] >= spec[1]) & (v[:, 2] <= spec[2])]
        return dict(kind="disc", radius=round(float(np.max(np.hypot(sel[:, 0], sel[:, 1]))), 3))
    # Gate: one hull per pillar (|x| beyond the opening), below head height.
    parts = []
    for sx in (-1, 1):
        sel = eng[(v[:, 2] < 2.2) & (eng[:, 0] * sx > GATE_W / 2 - 0.05) & (np.abs(eng[:, 1]) < 1.2)]
        parts.append([[round(x, 3), round(z, 3)] for x, z in gp.convex_hull(sel)])
    return dict(kind="multi", parts=parts)


def build(name, spec):
    builder, size, sink, foot, strength = spec
    vc.reset_scene()
    global MATS
    METAL.clear()
    EMIT.clear()
    MATS = materials()
    obj = builder()
    obj.data.transform(__import__("mathutils").Matrix.Translation((0, 0, -sink)))
    tris = vc.triangle_count(obj)
    paths, lit = bake_prop(name, obj, size, strength)
    mat = gp.build_material(name, paths, strength or 0.0)
    obj.data.materials.clear()
    obj.data.materials.append(mat)
    obj.data.polygons.foreach_set("material_index", [0] * len(obj.data.polygons))
    obj.name = obj.data.name = name
    vc.export_glb(os.path.join(OUT_DIR, f"{name}.glb"), [obj], textures=True)
    v = gp.verts_np(obj)
    lo, hi = v.min(0), v.max(0)
    meta = dict(source="procedural (gen_zone_props.py)", tris=tris, lod_tris=0, sink=sink, tex=size,
                bounds_min=[round(float(-hi[0]), 3), round(float(lo[2]), 3), round(float(-hi[1]), 3)],
                bounds_max=[round(float(-lo[0]), 3), round(float(hi[2]), 3), round(float(-lo[1]), 3)],
                footprint=footprint(v, foot), emissive_strength=strength or 0.0, emissive_lit=round(lit, 4))
    print(f"PROP {name}: {tris} tris, size {np.round(hi - lo, 2)} m, tex {size}, emissive "
          f"{'-' if not strength else f'{strength} ({100 * lit:.2f} % lit)'}")
    return obj, meta


# ---------------------------------------------------------------------------------------------
# Gate veils (effect meshes, docs/WORLD_ZONES_ART.md): the glowing film in the opening.

VEILS = {"aydor": ("#fff2c4", "#f2c766"), "kargan": ("#ffb070", "#e0541c"), "esik": ("#e4f4ff", "#7fb8de")}


def veil(look):
    """A double-sided arch-shaped sheet filling the gate opening (origin on the ground at the
    opening's centre, in the plane z = 0 of the gate, engine +Z = the gate's front). UV v runs up the
    sheet, u across; the emissive map is bright at the edges and fades upward and in the middle, so
    drawn translucent (the client's effect opacity) it reads as a soft veil."""
    W, Hs = GATE_W - 0.06, GATE_HS + 0.18
    r = W / 2
    top = Hs + r
    cols, rows = 12, 16
    verts, faces, uvs = [], [], []

    def outline_x(z):  # half width of the opening at height z
        if z <= Hs:
            return r
        return math.sqrt(max(r * r - (z - Hs) ** 2, 0.0))

    for j in range(rows + 1):
        z = top * j / rows
        hw = max(outline_x(z), 0.02)
        for i in range(cols + 1):
            u = i / cols
            verts.append((hw * (2 * u - 1), 0.0, z))
            uvs.append((u, z / top))
    for j in range(rows):
        for i in range(cols):
            a = j * (cols + 1) + i
            faces.append((a, a + 1, a + cols + 2, a + cols + 1))
    nv = len(verts)
    verts += verts[:]
    uvs += uvs[:]
    faces += [tuple(reversed([k + nv for k in f])) for f in faces]
    me = bpy.data.meshes.new(f"gate_veil_{look}")
    me.from_pydata(verts, [], faces)
    uvl = me.uv_layers.new(name="UVMap")
    for poly in me.polygons:
        for li in poly.loop_indices:
            vi = me.loops[li].vertex_index
            uvl.data[li].uv = (uvs[vi][0], 1.0 - uvs[vi][1])
    me.shade_smooth()
    obj = vc.link(bpy.data.objects.new(f"gate_veil_{look}", me))
    # Maps 64 x 128: v = row (0 = bottom of the sheet: image row 0 is V 0 here, see uv flip above).
    hi, lo = np.array(vc.hex_linear(VEILS[look][0])), np.array(vc.hex_linear(VEILS[look][1]))
    h, w = 128, 64
    v = (np.arange(h) + 0.5)[:, None] / h
    u = (np.arange(w) + 0.5)[None, :] / w
    edge = np.clip(1.0 - np.abs(2 * u - 1) / 0.18 + (1 - np.abs(2 * u - 1)) * 0.0, 0.0, 1.0)
    side = np.abs(2 * u - 1)
    glow = 0.35 + 0.65 * side ** 3                     # brighter towards the jambs
    glow = glow * (1.0 - 0.55 * v)                       # fades upward
    glow = glow * (0.85 + 0.15 * np.sin(v * 40.0 + u * 6.0) ** 2)  # faint ripples
    glow = np.maximum(glow, edge * 0.0)
    emis = lo[None, None, :] * (1 - glow[..., None]) + hi[None, None, :] * glow[..., None]
    emis = emis * (0.35 + 0.65 * glow[..., None])
    base = emis * 0.4
    pb, pe = os.path.join(FX_DIR, f"gate_veil_{look}.png"), os.path.join(FX_DIR, f"gate_veil_{look}_e.png")
    vc.write_png(pb, vc.to_u8(gp.lin_to_srgb(base)))
    vc.write_png(pe, vc.to_u8(gp.lin_to_srgb(emis)))
    mat = bpy.data.materials.new(f"gate_veil_{look}")
    mat.use_nodes = True
    nt = mat.node_tree
    bsdf = nt.nodes["Principled BSDF"]
    for path, sock in ((pb, "Base Color"), (pe, "Emission Color")):
        n = nt.nodes.new("ShaderNodeTexImage")
        n.image = bpy.data.images.load(path, check_existing=False)
        n.extension = 'EXTEND'
        nt.links.new(n.outputs["Color"], bsdf.inputs[sock])
    bsdf.inputs["Emission Strength"].default_value = 2.0
    bsdf.inputs["Roughness"].default_value = 1.0
    me.materials.append(mat)
    vc.export_glb(os.path.join(FX_DIR, f"gate_veil_{look}.glb"), [obj], textures=True)
    print(f"EFFECT gate_veil_{look}: {vc.triangle_count(obj)} tris")
    return obj


def main():
    args = vc.script_args()
    only = set(args[args.index("--only") + 1].split(",")) if "--only" in args else None
    meta_all = {}
    if os.path.exists(META_PATH):
        with open(META_PATH, "r", encoding="utf-8") as f:
            meta_all = json.load(f)
    shots = []
    for name, spec in PROPS.items():
        if only and name not in only:
            continue
        obj, meta = build(name, spec)
        meta_all[name] = meta
        if "--no-preview" not in args:
            shots.append(gp.preview(name, [obj], meta))
    meta_all = {k: meta_all[k] for k in PROPS if k in meta_all}
    with open(META_PATH, "w", encoding="utf-8", newline="\n") as f:
        json.dump(meta_all, f, indent=1, sort_keys=True)
        f.write("\n")
    if not only or "veils" in only:
        for look in VEILS:
            vc.reset_scene()
            veil(look)
    if shots and not only:
        vc.compose_sheet(shots, os.path.join(vc.PREVIEWS, "props", "m13_props_sheet.png"), 4)


MATS = {}

if __name__ == "__main__":
    main()
