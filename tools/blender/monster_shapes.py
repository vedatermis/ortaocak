"""Silhouette pieces for the M13 homeland monsters (gen_monsters.py `shapes`): rest-pose proportion
changes baked into the skeleton, and added rigid / skinned geometry (straw tufts, a broken spear and
banner, burnt plate shards, ice crystals, icicles, a frost crown, a frozen cloak) textured from a small
swatch strip at the right edge of the monster's own 1024^2 atlas (the body's maps are squeezed into
columns 0..BODY_W-1, like the merchants' atlases in gen_npcs.py).

Imported by gen_monsters.py (no main). Deterministic: every random choice comes from a seeded rng.
"""

import math

import bmesh
import bpy
import numpy as np
from mathutils import Vector
from mathutils.kdtree import KDTree

TEX = 1024
BODY_W = 896          # body atlas columns 0..895 (u 0..0.875)
STRIP_X0 = 904        # swatches: columns 904..1019 (8 px gutter left, 4 px right)
STRIP_W = TEX - 4 - STRIP_X0
SWATCH_H = 160
TORSO = ("spine_03", "spine_02", "spine_01", "pelvis", "neck_01")


# ---------------------------------------------------------------------------------------------
# Atlas: squeeze the body maps, paint the swatches

def squeeze(a, width=BODY_W):
    """(n, n, c) -> (n, n, c): columns resampled into 0..width-1 (area-weighted linear), the rest
    filled with the last body column (bled over by the swatches afterwards)."""
    n = a.shape[1]
    x = (np.arange(width) + 0.5) * n / width - 0.5
    x0 = np.clip(np.floor(x).astype(int), 0, n - 1)
    x1 = np.clip(x0 + 1, 0, n - 1)
    f = (x - np.floor(x))[None, :, None]
    # A light 3-tap box first so the 1.14x minification does not alias.
    k = np.concatenate([a[:, :1], a[:, :-1]], 1) * 0.25 + a * 0.5 + np.concatenate([a[:, 1:], a[:, -1:]], 1) * 0.25
    out = np.empty_like(a)
    out[:, :width] = k[:, x0] * (1 - f) + k[:, x1] * f
    out[:, width:] = out[:, width - 1:width]
    return out


def swatch_rects(names):
    return {nm: (STRIP_X0, 4 + k * SWATCH_H, STRIP_W, SWATCH_H - 4) for k, nm in enumerate(names)}


def _hex(h):
    return np.array([int(h.lstrip("#")[i:i + 2], 16) / 255.0 for i in (0, 2, 4)], np.float32)


def _ramp(t, stops):
    pos = [p for p, _ in stops]
    cols = np.array([_hex(h) for _, h in stops])
    return np.stack([np.interp(np.clip(t, 0, 1), pos, cols[:, k]) for k in range(3)], -1).astype(np.float32)


def _vnoise(rng, cy, cx, h, w):
    g = rng.random((cy + 1, cx + 1)).astype(np.float32)
    y = np.linspace(0, cy, h, endpoint=False)
    x = np.linspace(0, cx, w, endpoint=False)
    y0, x0 = y.astype(int), x.astype(int)
    fy, fx = (y - y0)[:, None], (x - x0)[None, :]
    fy, fx = fy * fy * (3 - 2 * fy), fx * fx * (3 - 2 * fx)
    a, b = g[y0][:, x0], g[y0][:, x0 + 1]
    c, d = g[y0 + 1][:, x0], g[y0 + 1][:, x0 + 1]
    return (a * (1 - fx) + b * fx) * (1 - fy) + (c * (1 - fx) + d * fx) * fy


def _fbm(rng, cy, cx, h, w, octaves=3):
    tot, amp, nrm = np.zeros((h, w), np.float32), 1.0, 0.0
    for _ in range(octaves):
        tot += amp * _vnoise(rng, cy, cx, h, w)
        nrm += amp
        amp *= 0.5
        cy, cx = cy * 2, cx * 2
    return tot / nrm


def paint_swatch(kind, h, w, rng):
    """One swatch: (colour sRGB (h, w, 3), height (h, w), roughness, metallic, emissive sRGB (h, w, 3)).
    s runs across the columns (around a spike / across a sheet), t down the rows (base -> tip /
    top -> hem)."""
    t = np.linspace(0, 1, h, dtype=np.float32)[:, None] * np.ones((1, w), np.float32)
    s = np.linspace(0, 1, w, dtype=np.float32)[None, :] * np.ones((h, 1), np.float32)
    em = np.zeros((h, w, 3), np.float32)
    metal = np.zeros((h, w), np.float32)
    if kind == "straw":
        # Dry straw fibres along t, darker at the base, charred tips with a gold ember at the very end.
        fib = _fbm(rng, 4, 40, h, w, 2)
        streak = np.clip(np.sin(s * 70.0 + fib * 9.0) * 0.5 + 0.5, 0, 1)
        lum = np.clip(0.35 + 0.45 * streak + 0.25 * (fib - 0.5) + 0.25 * t, 0, 1)
        col = _ramp(lum, [(0.0, "#3b2814"), (0.45, "#8a6630"), (0.8, "#c9a35a"), (1.0, "#e6cf8f")])
        char = np.clip((t - 0.72) / 0.18, 0, 1)[..., None]
        col = col * (1 - char) + _hex("#17110c") * char
        glow = np.clip((t - 0.9) / 0.06, 0, 1) * np.clip((fib - 0.35) / 0.15, 0, 1)
        col = col * (1 - glow[..., None]) + _hex("#ffb23a") * glow[..., None]
        em = _hex("#ffa030")[None, None] * glow[..., None]
        height = streak * 0.6 + fib * 0.4
        rough = np.full((h, w), 0.86, np.float32)
    elif kind == "twine":
        fib = _fbm(rng, 6, 6, h, w, 3)
        braid = np.clip(np.sin((s * 8 + t * 14) * math.pi) * 0.5 + 0.5, 0, 1)
        col = _ramp(np.clip(0.3 + 0.4 * braid + 0.3 * (fib - 0.5), 0, 1), [(0, "#2a1d10"), (0.6, "#5a4122"), (1, "#7e6237")])
        height = braid
        rough = np.full((h, w), 0.9, np.float32)
    elif kind == "char_wood":
        # Charred spear shaft: black alligator char along the grain, ember glowing in the deepest cracks.
        grain = _fbm(rng, 3, 30, h, w, 3)
        cell = _fbm(rng, 10, 6, h, w, 3)
        crack = np.clip(1.0 - np.abs(cell - 0.5) * 18.0, 0, 1)
        lum = np.clip(0.25 + 0.5 * grain, 0, 1)
        col = _ramp(lum, [(0, "#0d0a08"), (0.6, "#2b1e15"), (1.0, "#4b3624")])
        ember = crack * np.clip((grain - 0.45) / 0.15, 0, 1)
        col = col * (1 - ember[..., None]) + _hex("#ff6418") * ember[..., None]
        em = _hex("#ff5a10")[None, None] * ember[..., None]
        height = grain * 0.5 - crack * 0.5
        rough = np.full((h, w), 0.92, np.float32)
    elif kind == "burnt_iron":
        n1 = _fbm(rng, 8, 8, h, w, 4)
        rust = np.clip((_fbm(rng, 5, 5, h, w, 3) - 0.5) / 0.12, 0, 1)
        col = _ramp(np.clip(n1, 0, 1), [(0, "#121110"), (0.5, "#2b2826"), (1, "#4e4844")])
        col = col * (1 - 0.7 * rust[..., None]) + _hex("#4a2a18") * 0.7 * rust[..., None]
        edge = np.clip((t - 0.85) / 0.1, 0, 1)  # the torn edge glows faintly
        col = col * (1 - edge[..., None] * 0.6) + _hex("#ff6418") * edge[..., None] * 0.6
        em = _hex("#ff5a10")[None, None] * (edge * 0.8)[..., None]
        height = n1
        rough = (0.55 + 0.35 * rust).astype(np.float32)
        metal = (0.85 * (1 - rust)).astype(np.float32)
    elif kind == "rag":
        # A burnt war banner: dark red cloth, soot from the top, charred black edges toward the hem.
        weave = np.clip(np.sin(s * 220) * np.sin(t * 220) * 0.5 + 0.5, 0, 1)
        soot = _fbm(rng, 6, 4, h, w, 4)
        col = _ramp(np.clip(0.55 + 0.4 * (soot - 0.5) + 0.1 * weave, 0, 1),
                    [(0, "#160c0a"), (0.5, "#4a1712"), (1, "#7a2a1c")])
        burn = np.clip((t + 0.35 * (soot - 0.5) - 0.62) / 0.2, 0, 1)
        col = col * (1 - burn[..., None]) + _hex("#120d0b") * burn[..., None]
        glow = np.clip(1.0 - np.abs(t + 0.35 * (soot - 0.5) - 0.86) * 40.0, 0, 1)
        col = col * (1 - glow[..., None]) + _hex("#ff7020") * glow[..., None]
        em = _hex("#ff5a10")[None, None] * glow[..., None]
        height = weave * 0.3 + soot * 0.5
        rough = np.full((h, w), 0.95, np.float32)
    elif kind == "ice":
        # Ice crystal: deep blue at the root, pale and clear toward the tip, a cold glowing core line.
        n1 = _fbm(rng, 6, 3, h, w, 4)
        facet = np.abs(((s * 6.0) % 1.0) - 0.5) * 2.0
        lum = np.clip(0.25 + 0.65 * t + 0.25 * (n1 - 0.5) + 0.1 * facet, 0, 1)
        col = _ramp(lum, [(0, "#1d3a58"), (0.45, "#5f9cc4"), (0.8, "#bfe3f4"), (1, "#eef8fc")])
        core = np.clip(1.0 - np.abs(((s * 6.0 + 0.5) % 1.0) - 0.5) * 9.0, 0, 1) * np.clip((0.75 - t) / 0.3, 0, 1)
        col = col * (1 - 0.5 * core[..., None]) + _hex("#7fdcff") * 0.5 * core[..., None]
        em = _hex("#68ccff")[None, None] * (core * 0.6)[..., None]
        height = facet * 0.4 + n1 * 0.4
        rough = (0.12 + 0.2 * n1).astype(np.float32)
    elif kind == "frost_rock":
        n1 = _fbm(rng, 10, 10, h, w, 4)
        frost = np.clip((_fbm(rng, 6, 6, h, w, 3) - 0.45) / 0.15, 0, 1)
        col = _ramp(np.clip(n1 * 1.2, 0, 1), [(0, "#0f1216"), (0.5, "#2c3540"), (1, "#56636f")])
        col = col * (1 - 0.8 * frost[..., None]) + _hex("#d3e3ef") * 0.8 * frost[..., None]
        height = n1 + frost * 0.3
        rough = (0.85 - 0.4 * frost).astype(np.float32)
    elif kind == "frost_cloak":
        # Frozen cloak: grey-blue wool gone stiff, white rime from the hem up, ice glaze in streaks.
        weave = np.clip(np.sin(s * 180) * np.sin(t * 180) * 0.5 + 0.5, 0, 1)
        n1 = _fbm(rng, 6, 8, h, w, 4)
        col = _ramp(np.clip(0.45 + 0.4 * (n1 - 0.5) + 0.1 * weave, 0, 1),
                    [(0, "#1a222b"), (0.5, "#3c4a58"), (1, "#647789")])
        rime = np.clip((t * 0.9 + 0.5 * (n1 - 0.5) - 0.35) / 0.3, 0, 1)
        col = col * (1 - 0.75 * rime[..., None]) + _hex("#dbe8f2") * 0.75 * rime[..., None]
        height = weave * 0.3 + n1 * 0.4
        rough = (0.9 - 0.45 * rime).astype(np.float32)
    else:
        raise ValueError(kind)
    return col.astype(np.float32), height.astype(np.float32), rough, metal, em.astype(np.float32)


def height_normal(hgt, strength=3.0):
    gy, gx = np.gradient(hgt)
    n = np.stack([-gx * strength, gy * strength, np.ones_like(hgt)], -1)
    n /= np.linalg.norm(n, axis=-1, keepdims=True)
    return n * 0.5 + 0.5


def paint_strip(maps, names, rng):
    """maps: dict color / normal / orm / emissive (1024, 1024, 3) sRGB-ish 0..1 arrays (already
    squeezed). Paints the swatches (3 px bleed around each)."""
    rects = swatch_rects(names)
    for nm, (x, y, w, h) in rects.items():
        col, hgt, rough, metal, em = paint_swatch(nm, h, w, rng)
        nrm = height_normal(hgt)
        orm = np.stack([np.ones_like(rough), rough, metal], -1)
        for key, arr in (("color", col), ("normal", nrm), ("orm", orm), ("emissive", em)):
            big = np.pad(arr, ((3, 3), (3, 3), (0, 0)), mode="edge")
            maps[key][y - 3:y + h + 3, x - 3:x + w + 3] = big
    return rects


# ---------------------------------------------------------------------------------------------
# Rest-pose proportion changes

def reshape(arm, body, pose):
    """pose: {bone: (euler degrees XYZ in the bone's local frame, uniform scale)}. Poses the bones,
    bakes the deformation into the body (normals rotated by the blended bone rotations) and the pose
    into the armature's rest. The clips are bone-local, so they play on the new proportions."""
    import gen_character as gc
    gc.reset_pose(arm)
    for bn, (rot, scl) in pose.items():
        pb = arm.pose.bones[bn]
        pb.rotation_mode = 'XYZ'
        pb.rotation_euler = [math.radians(a) for a in rot]
        pb.scale = (scl, scl, scl)
    bpy.context.view_layer.update()
    me = body.data
    rots = {}
    for pb in arm.pose.bones:
        m = np.array((pb.matrix @ arm.data.bones[pb.name].matrix_local.inverted()).to_3x3())
        rots[pb.name] = m
    names = {g.index: g.name for g in body.vertex_groups}
    nv = len(me.vertices)
    vm = np.zeros((nv, 3, 3))
    for v in me.vertices:
        tot = 0.0
        for g in v.groups:
            nm = names[g.group]
            if nm in rots and g.weight > 0:
                vm[v.index] += rots[nm] * g.weight
                tot += g.weight
        if tot > 0:
            vm[v.index] /= tot
        else:
            vm[v.index] = np.eye(3)
    cn = np.empty(len(me.loops) * 3, np.float32)
    me.corner_normals.foreach_get("vector", cn)
    cv = np.empty(len(me.loops), np.int64)
    me.loops.foreach_get("vertex_index", cv)
    cn = cn.reshape(-1, 3).astype(np.float64)
    cn = np.einsum("nij,nj->ni", np.linalg.inv(vm[cv]).transpose(0, 2, 1), cn)
    cn /= np.maximum(np.linalg.norm(cn, axis=1, keepdims=True), 1e-12)
    bpy.ops.object.select_all(action='DESELECT')
    bpy.context.view_layer.objects.active = body
    body.select_set(True)
    mod = next(m for m in body.modifiers if m.type == 'ARMATURE')
    bpy.ops.object.modifier_apply(modifier=mod.name)
    me.normals_split_custom_set(cn.tolist())
    bpy.ops.object.select_all(action='DESELECT')
    bpy.context.view_layer.objects.active = arm
    arm.select_set(True)
    bpy.ops.object.mode_set(mode='POSE')
    bpy.ops.pose.select_all(action='SELECT')
    bpy.ops.pose.armature_apply(selected=False)
    bpy.ops.object.mode_set(mode='OBJECT')
    gc.reset_pose(arm)
    mod = body.modifiers.new("Armature", 'ARMATURE')
    mod.object = arm


# ---------------------------------------------------------------------------------------------
# Geometry

def _unit(v):
    v = np.asarray(v, float)
    return v / max(np.linalg.norm(v), 1e-9)


def _frame(axis, hint=(0.0, 0.0, 1.0)):
    a = _unit(axis)
    h = np.asarray(hint, float)
    if abs(np.dot(a, _unit(h))) > 0.95:
        h = np.array([1.0, 0.0, 0.0]) if abs(a[0]) < 0.9 else np.array([0.0, 1.0, 0.0])
    e1 = _unit(h - a * np.dot(a, h))
    return a, e1, np.cross(a, e1)


class Body:
    """The rigged body in the rest pose: vertices, weights, bone heads, a surface probe."""

    def __init__(self, body, arm):
        me = body.data
        self.V = np.array([v.co for v in me.vertices])
        self.names = [g.name for g in body.vertex_groups]
        self.W = np.zeros((len(self.V), len(self.names)), np.float32)
        for v in me.vertices:
            for g in v.groups:
                self.W[v.index, g.group] = g.weight
        self.kd = KDTree(len(self.V))
        for i, p in enumerate(self.V):
            self.kd.insert(p, i)
        self.kd.balance()
        self.head = {b.name: np.array(b.head_local) for b in arm.data.bones}
        self.tail = {b.name: np.array(b.tail_local) for b in arm.data.bones}
        self.top = float(self.V[:, 2].max())

    def near(self, p, n=8, only=None):
        """Skin weights at point p (inverse-distance blend of the n nearest body vertices, top 4);
        `only` keeps just those bones (a torso piece must not follow the arms)."""
        acc = np.zeros(len(self.names))
        tot = 0.0
        for co, idx, dist in self.kd.find_n(Vector(p), n):
            w = 1.0 / max(dist, 0.004) ** 2
            acc += self.W[idx] * w
            tot += w
        acc /= max(tot, 1e-9)
        if only:
            acc *= np.array([nm in only for nm in self.names], float)
            if acc.sum() < 1e-6:
                return {only[0]: 1.0}
        order = np.argsort(-acc)[:4]
        top = acc[order] / max(acc[order].sum(), 1e-9)
        return {self.names[j]: float(x) for j, x in zip(order, top) if x > 1e-3}

    def surf(self, origin, direction, rad=0.04, bones=None):
        """The body surface along the ray origin + k * direction (outermost vertex in a cylinder);
        bones: only vertices mostly weighted to those bones (the hanging arms are not the hips)."""
        o = np.asarray(origin, float)
        d = _unit(direction)
        rel = self.V - o
        k = rel @ d
        perp = np.linalg.norm(rel - np.outer(k, d), axis=1)
        sel = (perp < rad) & (k > 0)
        if bones:
            sel &= self.W[:, [self.names.index(b) for b in bones if b in self.names]].sum(axis=1) > 0.5
        if not sel.any():
            return o
        return o + d * float(k[sel].max())


class Parts:
    def __init__(self):
        self.V, self.F, self.FUV, self.FS, self.W = [], [], [], [], []

    def _verts(self, pts, w):
        i0 = len(self.V)
        for p in pts:
            self.V.append(tuple(float(c) for c in p))
            self.W.append(w)
        return i0

    def face(self, idx, uvs, sw):
        self.F.append(tuple(int(i) for i in idx))
        self.FUV.append([tuple(map(float, u)) for u in uvs])
        self.FS.append(sw)

    def lathe(self, base, axis, prof, sw, w, sides=6, hint=(0, 0, 1), squash=1.0, bend=(0, 0, 0), twist=0.0,
              jag=None, rng=None):
        """Closed tapered shell along `axis` from `base`: prof [(t 0..1, radius)], length = |axis|.
        A ring with radius 0 is a single tip vertex. squash flattens the e2 direction (blades,
        shards); bend adds bend * t^2 (a curve); jag (with rng) varies the last ring's length."""
        L = float(np.linalg.norm(axis))
        a, e1, e2 = _frame(axis, hint)
        bend = np.asarray(bend, float)
        rings = []
        for k, (t, r) in enumerate(prof):
            c = np.asarray(base, float) + a * L * t + bend * t * t
            if r <= 1e-6:
                rings.append(("tip", self._verts([c], w), t))
                continue
            pts = []
            for j in range(sides):
                ang = 2 * math.pi * j / sides + twist * t
                off = r * (math.cos(ang) * e1 + squash * math.sin(ang) * e2)
                extra = a * L * jag * (rng.random() - 0.3) if (jag and rng is not None and k == len(prof) - 1) else 0.0
                pts.append(c + off + extra)
            rings.append(("ring", self._verts(pts, w), t))
        for (ka, ia, ta), (kb, ib, tb) in zip(rings[:-1], rings[1:]):
            for j in range(sides):
                j2 = (j + 1) % sides
                s0, s1 = j / sides, (j + 1) / sides
                if ka == "ring" and kb == "ring":
                    self.face([ia + j, ia + j2, ib + j2, ib + j], [(s0, ta), (s1, ta), (s1, tb), (s0, tb)], sw)
                elif ka == "ring":
                    self.face([ia + j, ia + j2, ib], [(s0, ta), (s1, ta), ((s0 + s1) / 2, tb)], sw)
                elif kb == "ring":
                    self.face([ia, ib + j2, ib + j], [((s0 + s1) / 2, ta), (s1, tb), (s0, tb)], sw)
        for kind, i0, t in (rings[0], rings[-1]):
            if kind != "ring":
                continue
            first = i0 == rings[0][1]
            c = self._verts([np.mean([self.V[i0 + j] for j in range(sides)], axis=0)], w)
            for j in range(sides):
                j2 = (j + 1) % sides
                q = [i0 + j2, i0 + j, c] if first else [i0 + j, i0 + j2, c]
                self.face(q, [(0.5, t), (0.52, t), (0.51, t)], sw)

    def spike(self, base, direction, length, radius, sw, w, sides=5, **kw):
        prof = kw.pop("prof", [(0.0, radius), (0.45, radius * 0.75), (1.0, 0.0)])
        self.lathe(base, _unit(direction) * length, prof, sw, w, sides=sides, **kw)

    def crystal(self, base, direction, length, radius, sw, w, sides=6, **kw):
        """Ice / crystal prism: a short flare at the root, straight sides, a pointed cap."""
        prof = [(0.0, radius * 0.7), (0.12, radius), (0.72, radius * 0.95), (1.0, 0.0)]
        self.lathe(base, _unit(direction) * length, prof, sw, w, sides=sides, **kw)

    def sheet(self, P, N, thick, sw, wfn):
        """Cloth / banner sheet P (rows, cols, 3) with outward normals N, `thick` behind; s across
        the columns, t down the rows; wfn(row, col) -> weights."""
        R, C = P.shape[:2]
        idx_f = np.zeros((R, C), int)
        idx_b = np.zeros((R, C), int)
        for r in range(R):
            for c in range(C):
                idx_f[r, c] = self._verts([P[r, c]], wfn(r, c))
                idx_b[r, c] = self._verts([P[r, c] - N[r, c] * thick], wfn(r, c))
        uv = lambda r, c: (c / (C - 1), r / (R - 1))  # noqa: E731
        for r in range(R - 1):
            for c in range(C - 1):
                q = [(r, c), (r, c + 1), (r + 1, c + 1), (r + 1, c)]
                self.face([idx_f[a] for a in q], [uv(*a) for a in q], sw)
                self.face([idx_b[a] for a in q[::-1]], [uv(*a) for a in q[::-1]], sw)
        border = [[(0, c) for c in range(C)], [(r, C - 1) for r in range(R)],
                  [(R - 1, c) for c in range(C - 1, -1, -1)], [(r, 0) for r in range(R - 1, -1, -1)]]
        for edge in border:
            for a, b in zip(edge[:-1], edge[1:]):
                self.face([idx_f[a], idx_b[a], idx_b[b], idx_f[b]], [uv(*a), uv(*a), uv(*b), uv(*b)], sw)


def build_object(o, rects, body_names, uv_name, max_z=None):
    """Mesh object of the parts: UVs into the swatch strip, the parts' weights, closed shells with
    outward winding, faceted shading (sharp edges over 38 deg). max_z clamps the tips (the monster's
    authored height is the top of the body or its tallest piece)."""
    V = np.array(o.V)
    if max_z is not None:
        V[:, 2] = np.minimum(V[:, 2], max_z)
    me = bpy.data.meshes.new("shapes")
    me.from_pydata(V.tolist(), [], o.F)
    me.validate(clean_customdata=False)
    uvl = me.uv_layers.new(name=uv_name)
    flat = []
    for fi, poly in enumerate(me.polygons):
        x, y, w, h = rects[o.FS[fi]]
        for k in range(poly.loop_total):
            s_, t_ = o.FUV[fi][k]
            s_, t_ = min(max(s_, 0.0), 1.0), min(max(t_, 0.0), 1.0)
            flat += [(x + 1.5 + s_ * (w - 3)) / TEX, 1.0 - (y + 1.5 + t_ * (h - 3)) / TEX]
    uvl.data.foreach_set("uv", flat)
    obj = bpy.data.objects.new("shapes", me)
    bpy.context.scene.collection.objects.link(obj)
    groups = {n: obj.vertex_groups.new(name=n) for n in body_names}
    for vi, w in enumerate(o.W):
        tot = sum(w.values())
        for n, x in w.items():
            groups[n].add([vi], x / tot, 'REPLACE')
    bm = bmesh.new()
    bm.from_mesh(me)
    bmesh.ops.remove_doubles(bm, verts=bm.verts, dist=1e-6)
    bmesh.ops.recalc_face_normals(bm, faces=bm.faces)
    bmesh.ops.triangulate(bm, faces=bm.faces)
    small = [f for f in bm.faces if f.calc_area() < 2e-7]
    bmesh.ops.delete(bm, geom=small, context='FACES')
    loose = [v for v in bm.verts if not v.link_faces]
    bmesh.ops.delete(bm, geom=loose, context='VERTS')
    bm.to_mesh(me)
    bm.free()
    me.shade_smooth()
    me.set_sharp_from_angle(angle=math.radians(38.0))
    me.update()
    return obj


def join_into(body, extra):
    """Joins `extra` into `body`, keeping every corner normal (the body's custom source normals and
    the pieces' own faceted normals)."""
    for ob in (body, extra):
        me = ob.data
        n = np.empty(len(me.loops) * 3, np.float32)
        me.corner_normals.foreach_get("vector", n)
        a = me.attributes.new("keep_n", 'FLOAT_VECTOR', 'CORNER')
        a.data.foreach_set("vector", n)
    bpy.ops.object.select_all(action='DESELECT')
    extra.select_set(True)
    body.select_set(True)
    bpy.context.view_layer.objects.active = body
    bpy.ops.object.join()
    me = body.data
    kn = np.empty(len(me.loops) * 3, np.float32)
    me.attributes["keep_n"].data.foreach_get("vector", kn)
    me.attributes.remove(me.attributes["keep_n"])
    me.normals_split_custom_set(kn.reshape(-1, 3).tolist())
    me.polygons.foreach_set("material_index", [0] * len(me.polygons))


# ---------------------------------------------------------------------------------------------
# The four monsters

def _bone_dir(B, name):
    return _unit(B.tail[name] - B.head[name])


def stubble_parts(B, H, rng):
    """Anız Yanığı: a scarecrow of burnt stubble - a fan of straw bursting from the hunched upper
    back, straw tufts out of the shoulders and elbows, a ragged straw skirt, twine bands."""
    o = Parts()
    sp3, neck = B.head["spine_03"], B.head["neck_01"]
    # Back sheaf: ~26 stalks fanning up and back from between the shoulder blades.
    root = B.surf(sp3 + np.array([0, 0, 0.04]), (0, 1, 0)) - np.array([0, 0.02, 0])
    w = B.near(root, only=TORSO)
    for k in range(26):
        ang = math.radians(-70 + 140 * (k + rng.random() * 0.6) / 26)
        lift = 0.55 + 0.35 * rng.random()
        d = np.array([math.sin(ang) * 0.8, 0.55 + 0.25 * rng.random(), lift + 0.4 * math.cos(ang)])
        L = (0.30 + 0.18 * rng.random()) * (0.75 + 0.35 * math.cos(ang))
        b = root + np.array([math.sin(ang) * 0.05, 0, 0.03 * rng.random()])
        o.spike(b, d, L, 0.011 + 0.004 * rng.random(), "straw", w, sides=4, bend=(0, 0.04, -0.03), twist=0.5)
    o.lathe(root + np.array([0, 0.015, -0.03]), np.array([0, 0.0, 0.07]), [(0, 0.05), (1, 0.05)], "twine", w,
            sides=8, hint=(0, 1, 0), squash=0.6)
    for side, sx in (("l", 1), ("r", -1)):
        # Shoulder tufts: straw bursting out sideways and up.
        sh = B.head[f"upperarm_{side}"]
        top = B.surf(sh, (0.3 * sx, 0, 1))
        w = B.near(top)
        for k in range(9):
            d = np.array([sx * (0.6 + 0.6 * rng.random()), (rng.random() - 0.5) * 0.8, 0.5 + 0.7 * rng.random()])
            o.spike(top + (rng.random(3) - 0.5) * 0.04, d, 0.13 + 0.10 * rng.random(), 0.009, "straw", w, sides=4)
        # Elbow tufts: hanging back and out.
        el = B.head[f"lowerarm_{side}"]
        pe = B.surf(el, (0.3 * sx, 0.9, -0.2))
        w = B.near(pe)
        for k in range(6):
            d = np.array([sx * 0.4 * rng.random(), 0.7 + 0.3 * rng.random(), -0.5 - 0.5 * rng.random()])
            o.spike(pe + (rng.random(3) - 0.5) * 0.03, d, 0.10 + 0.08 * rng.random(), 0.008, "straw", w, sides=4)
        # Wrist band of twine with straw spilling out over the hand.
        wr = B.head[f"hand_{side}"]
        fa = _bone_dir(B, f"lowerarm_{side}")
        w = B.near(wr - fa * 0.05)
        o.lathe(wr - fa * 0.09, fa * 0.035, [(0, 0.045), (1, 0.045)], "twine", w, sides=8, hint=(0, 0, 1))
        for k in range(7):
            ang = 2 * math.pi * k / 7
            a_, e1, e2 = _frame(fa)
            d = fa * 0.9 + 0.5 * (math.cos(ang) * e1 + math.sin(ang) * e2)
            o.spike(wr - fa * 0.07 + 0.035 * (math.cos(ang) * e1 + math.sin(ang) * e2), d,
                    0.07 + 0.05 * rng.random(), 0.007, "straw", w, sides=4)
    # Straw skirt: a ring of stalks from the waist, splayed out, to the knees.
    pel = B.head["pelvis"]
    z0 = pel[2] + 0.04
    for k in range(34):
        ang = 2 * math.pi * (k + 0.5 * rng.random()) / 34
        d0 = np.array([math.sin(ang), -math.cos(ang), 0.0])
        p = B.surf(np.array([0, 0, z0]), d0, rad=0.05, bones=("pelvis", "spine_01", "thigh_l", "thigh_r")) - d0 * 0.01
        w = B.near(p + np.array([0, 0, 0.05]), only=("pelvis", "spine_01"))
        d = d0 * (0.45 + 0.2 * rng.random()) + np.array([0, 0, -1.0])
        o.spike(p, d, 0.26 + 0.12 * rng.random(), 0.012, "straw", w, sides=4)
    return o


def burnt_parts(B, H, rng):
    """Yanık Nöbetçi: the burnt border soldier - a broken spear strapped across the back carrying a
    torn, burnt banner; jagged split plates on the shoulders; broken arrow shafts in chest and back."""
    o = Parts()
    sp3, sp1 = B.head["spine_03"], B.head["spine_01"]
    wb = B.near(B.surf(sp3, (0, 1, 0)), only=TORSO)
    # Spear: from behind the right hip up past the left shoulder, broken (jagged) at the top.
    back_lo = B.surf(sp1 + np.array([-0.14, 0, -0.05]), (0, 1, 0)) + np.array([0, 0.07, 0])
    back_hi = B.surf(sp3 + np.array([0.18, 0, 0.10]), (0, 1, 0)) + np.array([0, 0.07, 0])
    d = _unit(back_hi - back_lo)
    a = back_lo - d * 0.30
    top_z = H - 0.005
    L = (top_z - a[2]) / d[2]
    o.lathe(a, d * L, [(0, 0.022), (0.5, 0.021), (0.97, 0.019), (1.0, 0.012)], "char_wood", wb, sides=7, jag=0.03,
            rng=rng, hint=(0, 1, 0))
    # A splinter beside the break.
    o.spike(a + d * (L - 0.02) + np.array([0.012, 0, 0]), d + np.array([0.2, 0, 0]), 0.09, 0.008, "char_wood", wb,
            sides=4)
    # Banner: a cross bar near the top, the torn burnt cloth hanging from it behind the back.
    bar_c = a + d * (L - 0.16) + np.array([0, 0.03, 0])
    bx = np.array([1.0, 0, 0])
    o.lathe(bar_c - bx * 0.24, bx * 0.48, [(0, 0.012), (1, 0.012)], "char_wood", wb, sides=6, hint=(0, 0, 1))
    R, C = 9, 8
    P = np.zeros((R, C, 3))
    N = np.zeros((R, C, 3))
    hem = [0.42, 0.55, 0.34, 0.60, 0.48, 0.30, 0.52, 0.40]
    for c in range(C):
        x = -0.22 + 0.44 * c / (C - 1)
        for r in range(R):
            t = r / (R - 1)
            drop = hem[c] * t
            sway = 0.02 * math.sin(c * 1.7 + t * 3) + 0.06 * t
            P[r, c] = bar_c + np.array([x, 0.035 + sway, -0.015 - drop])
            N[r, c] = (0, 1, 0)
    o.sheet(P, N, 0.012, "rag", lambda r, c: wb)
    # Split plates on the shoulders: 4 jagged shards each, fanning up and out.
    for side, sx in (("l", 1), ("r", -1)):
        sh = B.head[f"upperarm_{side}"]
        top = B.surf(sh + np.array([0, 0, 0.02]), (0.5 * sx, 0, 1)) - np.array([0, 0, 0.015])
        w = B.near(top)
        for k in range(4):
            ang = math.radians(-50 + 33 * k)
            d = np.array([sx * (0.75 + 0.2 * rng.random()), math.sin(ang) * 0.6, 0.8 + 0.3 * rng.random()])
            base = top + np.array([0, math.sin(ang) * 0.07, 0])
            o.spike(base, d, 0.13 + 0.08 * rng.random() + 0.04 * (k in (1, 2)), 0.05, "burnt_iron", w, sides=4,
                    squash=0.15, hint=(0, 1, 0), prof=[(0, 0.05), (0.5, 0.032), (1, 0.0)])
        # A torn plate on the outer forearm, sticking back.
        el = B.head[f"lowerarm_{side}"]
        fa = _bone_dir(B, f"lowerarm_{side}")
        p = B.surf(el + fa * 0.08, (sx, 0.3, 0.2))
        o.spike(p, np.array([sx * 0.5, 0.8, 0.3]), 0.14, 0.04, "burnt_iron", B.near(p), sides=4, squash=0.15,
                hint=(0, 0, 1), prof=[(0, 0.04), (0.5, 0.03), (1, 0.0)])
    # Broken arrows: two in the chest, one in the back, one in the left thigh.
    chest = B.head["spine_03"]
    for (dx, dz, dirn, L) in ((0.08, -0.02, (0.25, -1, 0.15), 0.26), (-0.06, -0.12, (-0.3, -1, -0.05), 0.20),
                              (-0.12, 0.05, (-0.2, 1, 0.25), 0.24)):
        dd = _unit(dirn)
        p = B.surf(chest + np.array([dx, 0, dz]), (0, np.sign(dd[1]), 0)) - dd * 0.03
        w = B.near(p, only=TORSO)
        o.lathe(p, dd * L, [(0, 0.007), (0.95, 0.007), (1, 0.004)], "char_wood", w, sides=5, jag=0.05, rng=rng,
                hint=(0, 0, 1))
    return o


def rime_parts(B, H, rng):
    """Kırağı Kayası: frost-bound basalt grown over with ice - a crusted hump of rimed rock on the
    back, ice-crystal clusters jutting from the shoulders, back and forearms."""
    o = Parts()
    sp3, sp2 = B.head["spine_03"], B.head["spine_02"]
    # Crusted hump: a lumpy flattened dome over the upper back.
    c = B.surf(sp2 + np.array([0, 0, 0.12]), (0, 1, 0)) - np.array([0, 0.10, 0])
    w = B.near(c + np.array([0, 0.05, 0]), only=TORSO)
    prof = [(0, 0.30), (0.25, 0.33), (0.55, 0.28), (0.8, 0.17), (1.0, 0.0)]
    o.lathe(c, np.array([0, 0.30, 0.10]), prof, "frost_rock", w, sides=10, hint=(0, 0, 1), squash=0.85)
    hump_top = c + np.array([0, 0.22, 0.12])
    # Back crystals out of the hump.
    for k in range(7):
        ang = math.radians(-60 + 120 * k / 6 + 8 * (rng.random() - 0.5))
        d = np.array([math.sin(ang) * 0.9, 0.55, 0.75 + 0.2 * rng.random()])
        b = hump_top + np.array([math.sin(ang) * 0.17, -0.04, -0.05 * abs(math.sin(ang))])
        o.crystal(b, d, 0.30 + 0.22 * rng.random() * (1 - abs(math.sin(ang)) * 0.4), 0.05 + 0.02 * rng.random(),
                  "ice", w, sides=6, twist=0.4)
    for side, sx in (("l", 1), ("r", -1)):
        # Shoulder clusters: big crystals jutting out and up.
        sh = B.head[f"upperarm_{side}"]
        top = B.surf(sh, (0.6 * sx, 0, 1)) - np.array([0, 0, 0.05])
        w = B.near(top)
        for k in range(5):
            d = np.array([sx * (0.8 + 0.5 * rng.random()), (rng.random() - 0.5) * 0.9, 0.5 + 0.6 * rng.random()])
            o.crystal(top + (rng.random(3) - 0.5) * np.array([0.10, 0.14, 0.04]), d, 0.26 + 0.20 * rng.random(),
                      0.045 + 0.02 * rng.random(), "ice", w, sides=6, twist=0.3)
        # Forearm crystals: a row along the outer forearm, pointing out and back.
        el = B.head[f"lowerarm_{side}"]
        fa = _bone_dir(B, f"lowerarm_{side}")
        L = float(np.linalg.norm(B.head[f"hand_{side}"] - el))
        for k in range(3):
            q = el + fa * L * (0.2 + 0.25 * k)
            p = B.surf(q, (sx, 0.4, 0.1)) - np.array([sx * 0.02, 0, 0])
            o.crystal(p, np.array([sx * 0.8, 0.6, 0.35 - 0.15 * k]), 0.16 + 0.07 * rng.random(), 0.035, "ice",
                      B.near(p), sides=6)
    return o


def frozen_parts(B, H, rng):
    """Donuk Ocakçı: a hearth-guard frozen where he stood - a crown of ice spikes and two ice horns
    sweeping out from the temples, icicles hanging from the pauldrons, shards on the shoulders, a
    stiff frozen cloak with an icicle hem."""
    o = Parts()
    hd = B.head["Head"]
    head_top = B.top
    wh = B.near(hd + np.array([0, 0, 0.12]))
    wh = {"Head": 1.0}
    # Crown: a ring of spikes around the skull, splayed out, the tips up to the authored height.
    zc = head_top - 0.10
    ctr = np.array([hd[0], hd[1] + 0.01, zc])
    for k in range(9):
        ang = math.radians(-105 + 210 * k / 8)
        d0 = np.array([math.sin(ang), -math.cos(ang), 0.0])
        p = B.surf(ctr, d0, rad=0.03) - d0 * 0.012
        d = d0 * 0.55 + np.array([0, 0, 1.0])
        L = 0.11 + 0.07 * (1 - abs(math.sin(ang * 0.5))) if abs(ang) < 1.0 else 0.10
        o.crystal(p, d, L, 0.018, "ice", wh, sides=5)
    # Horns: from the temples out sideways, curving up.
    for sx in (1, -1):
        p = B.surf(np.array([hd[0], hd[1], zc - 0.02]), (sx, -0.2, 0.2), rad=0.03) - np.array([sx * 0.015, 0, 0])
        o.lathe(p, np.array([sx * 0.30, 0.05, 0.02]), [(0, 0.04), (0.35, 0.034), (0.7, 0.022), (1, 0.0)], "ice", wh,
                sides=7, bend=(0, 0.06, 0.20), hint=(0, 0, 1))
    for side, sx in (("l", 1), ("r", -1)):
        sh = B.head[f"upperarm_{side}"]
        # Shoulder shards: ice jutting up and out over the pauldron.
        top = B.surf(sh + np.array([0, 0, 0.03]), (0.5 * sx, 0, 1)) - np.array([0, 0, 0.03])
        w = B.near(top)
        for k in range(4):
            d = np.array([sx * (0.7 + 0.4 * rng.random()), (k - 1.5) * 0.25, 0.9])
            o.crystal(top + np.array([0, (k - 1.5) * 0.06, 0]), d, 0.16 + 0.1 * rng.random(), 0.035, "ice", w, sides=5)
        # Icicles under the pauldron rim.
        rim = B.surf(sh + np.array([0, 0, -0.06]), (sx, 0, -0.15))
        w = B.near(rim)
        for k in range(6):
            p = rim + np.array([-sx * 0.02, -0.10 + 0.04 * k, 0.0])
            o.spike(p, (0, 0, -1), 0.09 + 0.12 * rng.random(), 0.016, "ice", w, sides=5,
                    prof=[(0, 0.016), (0.3, 0.012), (1, 0.0)])
    # Frozen cloak: a short stiff cape from the shoulders down the back to the hips (longer ones go
    # through the ground in the death clip), flaring a little, an icicle hem.
    sh_z = B.head["upperarm_l"][2]
    R, C = 10, 13
    P = np.zeros((R, C, 3))
    N = np.zeros((R, C, 3))
    z0, z1 = sh_z + 0.02, B.head["pelvis"][2] + 0.02
    hw0 = abs(B.head["upperarm_l"][0]) * 0.95
    jag = np.array([0.0 if c % 2 else 0.08 + 0.05 * rng.random() for c in range(C)])
    yb = np.array([[B.surf(np.array([(-1 + 2 * c / (C - 1)) * hw0 * 0.8, 0, z0 + (z1 - z0) * r / (R - 1)]),
                           (0, 1, 0), rad=0.06)[1] for c in range(C)] for r in range(R)])
    for r in range(1, R):  # stiff: tucks in slowly under the mantle
        yb[r] = np.maximum(np.maximum(yb[r], 0.08), yb[r - 1] - 0.05)
    for r in range(R):
        t = r / (R - 1)
        z = z0 + (z1 - z0) * t
        hw = hw0 * (1 + 0.25 * t)
        for c in range(C):
            u = -1 + 2 * c / (C - 1)
            y = yb[r, c] + 0.025 + 0.015 * math.cos(u * math.pi * 3) * t - 0.03 * u * u * (1 - t)
            P[r, c] = (u * hw, y, z - jag[c] * (t ** 6))
            N[r, c] = _unit(np.array([u * 0.4, 1.0, 0.1]))
    wrows = [[B.near(B.surf(np.array([P[r, c][0] * 0.8, 0.0, P[r, c][2]]), (0, 1, 0), rad=0.06), n=16, only=TORSO)
              for c in range(C)] for r in range(R)]

    def cloak_w(r, c):  # the torso's own weights under each point: the cape follows the back
        return wrows[r][c]
    o.sheet(P, N, 0.016, "frost_cloak", cloak_w)
    # Icicles along the hem.
    for c in range(0, C, 2):
        p = P[-1, c] - np.array([0, 0.008, 0])
        o.spike(p, (0, 0.1, -1), 0.06 + 0.07 * rng.random(), 0.013, "ice", cloak_w(R - 1, c), sides=5,
                prof=[(0, 0.013), (0.3, 0.010), (1, 0.0)])
    return o


SHAPES = {
    "stubble": dict(swatches=("straw", "twine"), parts=stubble_parts),
    "burnt": dict(swatches=("char_wood", "burnt_iron", "rag"), parts=burnt_parts),
    "rime": dict(swatches=("ice", "frost_rock"), parts=rime_parts),
    "frozen": dict(swatches=("ice", "frost_cloak"), parts=frozen_parts),
}
