"""M14 "Pazar": the six merchant NPCs (a potion seller and a smith per town), built on the player
looks' rigged sources (gen_character.py: same 59-bone skeleton, same clips) with a town palette
painted over the source maps and real outfit geometry modelled around the rest-pose body:

  potion seller   hood (open around the face) and a cowl over the shoulders, a long cloth apron with
                  a hem band, a belt with three potion flasks (red, blue, red; the liquid glows
                  faintly) and a satchel on the left hip with two more flask necks, a strap across
                  the chest
  smith           a heavy leather apron (bib, neck straps over the shoulders, rivets, a pocket),
                  leather gauntlets over the forearms, headwear per smith (`cap_style`), a belt with a
                  hammer on the right hip and tongs on the left (Kargan: a glowing ingot in the tongs)
  own features    each merchant also gets something of its own (`features`) so it does not read as
                  its base player look in an apron: see the table

| key                 | base look | town                    | palette | own features |
|---------------------|-----------|-------------------------|---------|--------------|
| npc_potion_esik     | aydor_1   | Ortaocak (Eşik)         | moss green, undyed linen, brown leather | sage sprig in the hood |
| npc_smith_esik      | kargan_0  | Ortaocak (Eşik)         | ochre, brown leather, ochre felt | tall felt börk, fleece brim |
| npc_potion_aydor    | aydor_2   | Tanyeri Burcu (Aydor)   | white, blue, gold | long pipe, gold clasp |
| npc_smith_aydor     | aydor_0   | Tanyeri Burcu (Aydor)   | blue, white, tan leather, brass | knotted bandana, drooping moustache |
| npc_potion_kargan   | kargan_1  | Demirağız (Kargan)      | charcoal, ember orange | draped capelet, iron circlet + ember bead |
| npc_smith_kargan    | kargan_2  | Demirağız (Kargan)      | black leather, iron, ember | studded skullcap, iron beard rings |

Usage (headless):
  blender --background --factory-startup --python tools/blender/gen_npcs.py -- [--only npc_smith_aydor,...] [--no-preview]

Outputs per key (assets_src/npcs/, compiled by assetc to assets/npcs/ like the monsters):
  <key>.glb          skinned mesh "body" (body + outfit, one material), 59-bone skeleton, NPC_CLIPS
  <key>.png          base colour 1024^2: the recoloured body atlas squeezed into the left 3/4, the
                     outfit swatches in the right strip
  <key>_n.png        normal map (OpenGL +Y), <key>_orm.png (R occlusion, G roughness, B metallic),
  <key>_e.png        emissive (potion liquids, the Kargan smith's hot ingot)
  <key>.anims.toml   clip loop flags for assetc
  npcs_meta.json     height, triangles, base look, outfit triangles per key
  assets_src/previews/npcs/<key>_{front,threequarter,back}.png, <key>_clips_sheet.png
Runtime: npcs/<key>.vmesh + npcs/<key>/<clip>.vanim (clip = idle, idle_talking, interact).
Deterministic: same inputs -> same bytes (previews through the stable writer).
"""

import json
import math
import os
import sys

import bmesh
import bpy
import numpy as np
from mathutils import Matrix, Vector
from mathutils.kdtree import KDTree

sys.dont_write_bytecode = True
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import vo_common as vc  # noqa: E402
import vo_ual  # noqa: E402
import gen_character as gc  # noqa: E402
import gen_props as gp  # noqa: E402

OUT_DIR = os.path.join(vc.SRC, "npcs")
PREVIEW_DIR = os.path.join(vc.PREVIEWS, "npcs")
META_PATH = os.path.join(OUT_DIR, "npcs_meta.json")

# Merchants stand at their stall: the idle, a talking idle (gestures while the shop is open) and the
# library's interact (a one-shot reach forward: handing over the goods).
NPC_CLIP_NAMES = ("idle", "idle_talking", "interact")
NPC_CLIPS = [c for c in vo_ual.CLIPS if c[0] in NPC_CLIP_NAMES]

TEX = 1024          # atlas size
BODY_W = 768        # the body's source atlas is squeezed into columns 0..767 (u 0..0.75)
STRIP_X0 = 776      # outfit swatches: columns 776..1019 (8 px gutter on both sides)
STRIP_W = TEX - 4 - STRIP_X0
MAX_TRIS = 24400    # body budget; the outfit adds 4.5-5.6k (total stays under 30k like the players)
SRC_SIZE = 2048     # source maps are recoloured at this size


# ---------------------------------------------------------------------------------------------
# Small helpers

def hexc(h):
    h = h.lstrip("#")
    return np.array([int(h[i:i + 2], 16) / 255.0 for i in (0, 2, 4)], np.float32)


def gmap(t, stops):
    """t (..) 0..1 -> sRGB colour through the hex stops (evenly spaced)."""
    cols = np.array([hexc(s) for s in stops])
    pos = np.linspace(0.0, 1.0, len(stops))
    t = np.clip(t, 0, 1)
    return np.stack([np.interp(t, pos, cols[:, k]) for k in range(3)], -1).astype(np.float32)


def vnoise(rng, cells, h, w):
    """Smooth value noise 0..1 of shape (h, w), `cells` lattice cells along the longer side."""
    n = max(h, w)
    lat = rng.random((cells + 2, cells + 2))
    x = np.linspace(0, cells, n)
    i0 = np.floor(x).astype(int)
    t = x - i0
    t = t * t * (3 - 2 * t)
    a, b = lat[np.ix_(i0, i0)], lat[np.ix_(i0, i0 + 1)]
    c, d = lat[np.ix_(i0 + 1, i0)], lat[np.ix_(i0 + 1, i0 + 1)]
    ty, tx = t[:, None], t[None, :]
    out = (a * (1 - tx) + b * tx) * (1 - ty) + (c * (1 - tx) + d * tx) * ty
    return out[:h, :w].astype(np.float32)


def fbm(rng, cells, h, w, octaves=4):
    tot, amp, norm = np.zeros((h, w), np.float32), 1.0, 0.0
    for _ in range(octaves):
        tot += amp * vnoise(rng, cells, h, w)
        norm += amp
        amp *= 0.5
        cells *= 2
    return tot / norm


def srgb_lin(c):
    return np.where(c <= 0.04045, c / 12.92, ((c + 0.055) / 1.055) ** 2.4)


def lin_srgb(c):
    c = np.clip(c, 0, 1)
    return np.where(c <= 0.0031308, c * 12.92, 1.055 * np.power(c, 1 / 2.4) - 0.055)


def hsv(c):
    r, g, b = c[..., 0], c[..., 1], c[..., 2]
    mx = np.maximum(np.maximum(r, g), b)
    mn = np.minimum(np.minimum(r, g), b)
    sat = (mx - mn) / np.maximum(mx, 1e-4)
    hue = np.degrees(np.arctan2(np.sqrt(3.0) * (g - b), 2 * r - g - b)) % 360.0
    return np.where(hue > 180.0, hue - 360.0, hue), sat, mx


# ---------------------------------------------------------------------------------------------
# The six NPCs (palettes are sRGB hex; body recolour stops dark -> light)

NPCS = [
    # Ortaocak's herb-wife: aydor_1 under a moss-green hood, undyed linen apron, brown leather.
    dict(key="npc_potion_esik", base="aydor_1", kind="potion", town="esik",
         body=dict(primary=("#14180d", "#3f4a29", "#7f8a5c"), secondary=("#17110b", "#4e3a28", "#9a8264"),
                   metal=("#140e09", "#47321f", "#806048"), metal_keep=0.0),
         hood="#5a6640", lining="#2c2a1c", trim="#8a6a3a", apron="#d8ccae", hem="#7a5a34",
         leather="#6b4a2c", bag="#7a5532", fitting="#b08a4a", fitting_metal=0.8,
         # a sprig of sage tucked into the hood over the right temple
         features=("sprig",), leaf="#5f7a3e"),
    # Ortaocak's smith: kargan_0 in ochre and brown, a tall ochre felt börk with a fat rolled fleece
    # brim, brown leather.
    dict(key="npc_smith_esik", base="kargan_0", kind="smith", town="esik",
         body=dict(primary=("#1c130a", "#5e4122", "#a8854e"), secondary=("#15110d", "#463c33", "#857a6e"),
                   metal=("#120c08", "#3c2a1a", "#6e523a"), metal_keep=0.0),
         cap="#7a5a30", cap_band="#cbbd98", apron="#6b4527", leather="#4a3220", fitting="#8a8a86",
         fitting_metal=0.85, gauntlet="#5a3c24", brim=True, cap_style="bork"),
    # Tanyeri Burcu's apothecary: aydor_2 in white and blue with gold trim.
    dict(key="npc_potion_aydor", base="aydor_2", kind="potion", town="aydor",
         body=dict(primary=("#0e1626", "#2b4872", "#7896c0"), secondary=("#2e2d2b", "#a6a196", "#e8e4da"),
                   metal=("#38352f", "#b7ad96", "#ebe3cf"), metal_keep=0.0),
         hood="#ece7da", lining="#2e4a74", trim="#d0a44c", apron="#f1ede3", hem="#34557f",
         leather="#b49a74", bag="#c9b896", fitting="#d8b060", fitting_metal=0.85,
         # a long-stemmed pipe in the mouth corner, a gold clasp with a blue stone under the chin
         features=("pipe", "brooch")),
    # Tanyeri Burcu's smith: aydor_0 in blue, a blue bandana knotted at the back with a white edge, a
    # drooping moustache, tan leather, brass.
    dict(key="npc_smith_aydor", base="aydor_0", kind="smith", town="aydor",
         body=dict(primary=("#0e1626", "#294470", "#6e8ebc"), secondary=("#262624", "#959086", "#dcd8ce"),
                   metal=("#1a120c", "#5a4028", "#9c7a56"), metal_keep=0.0),
         cap="#34557f", cap_band="#ebe6da", apron="#9a6c42", leather="#6e4c2c", fitting="#d4ac5c",
         fitting_metal=0.85, gauntlet="#a07a50", brim=True, cap_style="bandana", features=("moustache",),
         hair="#4a2c1a"),
    # Demirağız's ember-brewer: kargan_1 in charcoal with ember-orange trim.
    dict(key="npc_potion_kargan", base="kargan_1", kind="potion", town="kargan",
         body=dict(primary=("#0b0a0a", "#282422", "#57504a"), secondary=("#120705", "#55220f", "#a24c26"),
                   metal=("#0e0e0f", "#3a3a3e", "#6e6e74"), metal_keep=0.8),
         hood="#2c2724", lining="#5a1e0c", trim="#d8641e", apron="#3c332c", hem="#c4561a",
         leather="#2a2019", bag="#3a2a1e", fitting="#6a6a70", fitting_metal=0.9,
         # kargan_1's fur collar stands wide up to the chin: the ring cowl over it read as a square
         # mantle, so this one gets the draped capelet (drape_cowl).
         cowl="drape",
         # an iron circlet across the brow with a glowing ember bead
         features=("circlet",)),
    # Demirağız's master smith: kargan_2 in black leather and iron, an iron-studded skullcap, a hot
    # ingot in the tongs.
    dict(key="npc_smith_kargan", base="kargan_2", kind="smith", town="kargan",
         body=dict(primary=("#0d0c0b", "#312d29", "#665e56"), secondary=("#110906", "#462412", "#864826"),
                   metal=("#0e0e0f", "#3c3c40", "#727278"), metal_keep=0.85),
         cap="#231c17", cap_band="#4a4a50", apron="#2a201a", leather="#1e1712", fitting="#7c7c82",
         fitting_metal=0.9, gauntlet="#2e241c", brim=False, ingot=True,
         # iron rings clamped round his braided beard
         features=("beard_rings",)),
]
NPC = {N["key"]: N for N in NPCS}


# ---------------------------------------------------------------------------------------------
# Outfit geometry: closed shells with per-corner swatch UVs and a weight rule per vertex

class Outfit:
    def __init__(self):
        self.V = []      # positions
        self.F = []      # faces (vertex index tuples)
        self.FUV = []    # per face: list of (s, t) in 0..1 (swatch space)
        self.FS = []     # per face: swatch name
        self.W = []      # per vertex: None (nearest body weights) or {bone: w}
        self.parts = {}

    def _add_verts(self, pts, weight):
        i0 = len(self.V)
        for k, p in enumerate(pts):
            self.V.append(tuple(float(c) for c in p))
            self.W.append(weight(k) if callable(weight) else weight)
        return i0

    def face(self, idx, uvs, swatch):
        self.F.append(tuple(int(i) for i in idx))
        self.FUV.append([tuple(map(float, u)) for u in uvs])
        self.FS.append(swatch)

    def grid(self, P, UV, swatch, weight=None, wrap=False, flip=False):
        """Quad grid over P (rows, cols, 3); UV (rows, cols, 2). wrap: the columns close (cols -> 0).
        Returns the (rows, cols) index array."""
        R, C = P.shape[:2]
        i0 = self._add_verts(P.reshape(-1, 3), weight if not callable(weight) else
                             (lambda k: weight(k // C, k % C)))
        idx = i0 + np.arange(R * C).reshape(R, C)
        cols = C if wrap else C - 1
        for r in range(R - 1):
            for c in range(cols):
                c2 = (c + 1) % C
                q = [idx[r, c], idx[r, c2], idx[r + 1, c2], idx[r + 1, c]]
                u = [UV[r, c], UV[r, c2] if c2 else (1.0, UV[r, 0][1]),
                     UV[r + 1, c2] if c2 else (1.0, UV[r + 1, 0][1]), UV[r + 1, c]]
                if flip:
                    q, u = q[::-1], u[::-1]
                self.face(q, u, swatch)
        return idx

    def strip(self, a, b, ua, ub, swatch, closed=False):
        """Quads between two index rows a, b (same length) with UV rows ua, ub."""
        n = len(a)
        for k in range(n if closed else n - 1):
            k2 = (k + 1) % n
            self.face([a[k], a[k2], b[k2], b[k]], [ua[k], ua[k2], ub[k2], ub[k]], swatch)

    def fan(self, ring, centre, uv_ring, uv_c, swatch):
        n = len(ring)
        for k in range(n):
            k2 = (k + 1) % n
            self.face([ring[k], ring[k2], centre], [uv_ring[k], uv_ring[k2], uv_c], swatch)

    def slab(self, P, N, thick, swatch, weight=None, edge_swatch=None):
        """A sheet P (rows, cols, 3) with outward normals N, given `thick` behind it: front grid,
        back grid, closed by the four edges. UVs: s across the columns, t down the rows."""
        R, C = P.shape[:2]
        uv = np.stack(np.meshgrid(np.linspace(0, 1, C), np.linspace(0, 1, R)), -1)
        f = self.grid(P, uv, swatch, weight)
        b = self.grid(P - N * thick, uv, swatch, weight, flip=True)
        es = edge_swatch or swatch
        border = [(f[0, :], b[0, :], uv[0, :]), (f[:, -1], b[:, -1], uv[:, -1]),
                  (f[-1, ::-1], b[-1, ::-1], uv[-1, ::-1]), (f[::-1, 0], b[::-1, 0], uv[::-1, 0])]
        for fa, ba, ua in border:
            self.strip(fa, ba, ua, ua, es)

    def tube(self, P, N, B, w, h, swatch, weight=None, closed=False, t_scale=1.0):
        """A rectangular ribbon / tube along the path P (n, 3): width w along B, height h along N.
        UVs: s along the path (0..1), t across (the four sides at 0, 1/4, .. 1). t_scale unused
        (kept for call sites; the atlas swatches do not tile)."""
        n = len(P)
        corners = []
        for k in range(n):
            p, nn, bb = P[k], N[k], B[k]
            corners.append([p + bb * w / 2, p + bb * w / 2 + nn * h, p - bb * w / 2 + nn * h, p - bb * w / 2])
        C = np.array(corners)  # (n, 4, 3)
        Pc = np.concatenate([P, P[:1]]) if closed else P
        L = np.concatenate([[0], np.cumsum(np.linalg.norm(np.diff(Pc, axis=0), axis=1))])
        ss = L / max(L[-1], 1e-6)
        idx = self._add_verts(C.reshape(-1, 3), weight)
        I = idx + np.arange(n * 4).reshape(n, 4)
        tv = (0.0, 0.25, 0.5, 0.75, 1.0)
        for k in range(n if closed else n - 1):
            k2 = (k + 1) % n
            for j in range(4):
                j2 = (j + 1) % 4
                self.face([I[k, j], I[k, j2], I[k2, j2], I[k2, j]],
                          [(ss[k], tv[j]), (ss[k], tv[j + 1]), (ss[k + 1], tv[j + 1]), (ss[k + 1], tv[j])], swatch)
        if not closed:
            for k, rev in ((0, True), (n - 1, False)):
                q = list(I[k]) if not rev else list(I[k])[::-1]
                self.face(q, [(0, 0), (0.05, 0), (0.05, 1), (0, 1)], swatch)

    def lathe(self, base, axis, side, prof, sides, swatch, weight=None, phase=0.0):
        """Closed surface of revolution: prof [(radius, height), ...] bottom to top along `axis`
        from `base`; caps where the end radius is > 0."""
        axis = np.asarray(axis, float)
        axis /= np.linalg.norm(axis)
        e1 = np.asarray(side, float)
        e1 = e1 - axis * np.dot(e1, axis)
        e1 /= np.linalg.norm(e1)
        e2 = np.cross(axis, e1)
        rings, uvs = [], []
        L = np.concatenate([[0], np.cumsum([math.hypot(prof[k + 1][0] - prof[k][0], prof[k + 1][1] - prof[k][1])
                                            for k in range(len(prof) - 1)])])
        for (r, h), l in zip(prof, L / L[-1]):
            pts = []
            for k in range(sides):
                a = 2 * math.pi * k / sides + phase
                pts.append(np.asarray(base) + axis * h + r * (math.cos(a) * e1 + math.sin(a) * e2))
            rings.append(self._add_verts(pts, weight) + np.arange(sides))
            uvs.append([(k / sides, l) for k in range(sides + 1)])
        for k in range(len(rings) - 1):
            a, b = rings[k], rings[k + 1]
            for j in range(sides):
                j2 = (j + 1) % sides
                self.face([a[j], a[j2], b[j2], b[j]], [uvs[k][j], uvs[k][j + 1], uvs[k + 1][j + 1], uvs[k + 1][j]],
                          swatch)
        for ring, (r, h), top in ((rings[0], prof[0], False), (rings[-1], prof[-1], True)):
            if r <= 1e-6:
                continue
            c = self._add_verts([np.asarray(base) + axis * h], weight)
            ring_l = list(ring) if top else list(ring)[::-1]
            n = len(ring_l)
            for j in range(n):
                self.face([ring_l[j], ring_l[(j + 1) % n], c], [(0.5, 0.0), (0.5, 0.05), (0.52, 0.02)], swatch)

    def box(self, centre, axes, size, swatch, weight=None):
        """Box with edge axes (3 unit vectors) and size (3,)."""
        c = np.asarray(centre, float)
        ax = [np.asarray(a, float) for a in axes]
        h = [s / 2 for s in size]
        pts = []
        for sz in (-1, 1):
            for (sx, sy) in ((-1, -1), (1, -1), (1, 1), (-1, 1)):
                pts.append(c + ax[0] * sx * h[0] + ax[1] * sy * h[1] + ax[2] * sz * h[2])
        i = self._add_verts(pts, weight)
        uv = [(0, 0), (1, 0), (1, 1), (0, 1)]
        for f in ((0, 3, 2, 1), (4, 5, 6, 7), (0, 1, 5, 4), (1, 2, 6, 5), (2, 3, 7, 6), (3, 0, 4, 7)):
            self.face([i + k for k in f], uv, swatch)


# ---------------------------------------------------------------------------------------------
# Body sampling (rest pose)

def ray_extent(origins, dirs, pts, tol):
    """Per ray: the farthest point of `pts` within `tol` of the ray (forward side), as a distance
    from the origin; NaN where nothing is near. Origins/dirs (n, 3)."""
    out = np.full(len(origins), np.nan)
    if not len(pts):
        return out
    for s in range(0, len(origins), 64):
        o = origins[s:s + 64, None, :]
        d = dirs[s:s + 64, None, :]
        rel = pts[None, :, :] - o
        proj = (rel * d).sum(-1)
        perp2 = (rel * rel).sum(-1) - proj * proj
        ok = (proj > 0) & (perp2 < tol * tol)
        val = np.where(ok, proj, -np.inf).max(axis=1)
        out[s:s + 64] = np.where(np.isfinite(val), val, np.nan)
    return out


def fill_nan(a, axis_wrap=False):
    """NaNs replaced by the nearest valid value along each row (then column)."""
    a = a.copy()
    for arr in (a, a.T):
        for row in arr:
            ok = ~np.isnan(row)
            if not ok.any():
                continue
            if ok.all():
                continue
            xs = np.arange(len(row))
            row[~ok] = np.interp(xs[~ok], xs[ok], row[ok])
    a[np.isnan(a)] = np.nanmean(a) if np.isfinite(np.nanmean(a)) else 0.0
    return a


def blur_max(r, iters=2, wrap_cols=False):
    """Outward-only smoothing: r = max(r, mean of the 3x3 neighbourhood)."""
    for _ in range(iters):
        p = np.pad(r, 1, mode="edge")
        if wrap_cols:
            p[1:-1, 0], p[1:-1, -1] = r[:, -1], r[:, 0]
        m = sum(p[1 + dy:1 + dy + r.shape[0], 1 + dx:1 + dx + r.shape[1]] for dy in (-1, 0, 1) for dx in (-1, 0, 1)) / 9
        r = np.maximum(r, m)
    return r


def grid_normals(P, outward_hint):
    """Per-point normals of a (rows, cols, 3) grid, flipped to agree with outward_hint (rows, cols, 3)."""
    du = np.gradient(P, axis=1)
    dv = np.gradient(P, axis=0)
    n = np.cross(du, dv)
    n /= np.maximum(np.linalg.norm(n, axis=-1, keepdims=True), 1e-9)
    s = np.sign((n * outward_hint).sum(-1, keepdims=True))
    s[s == 0] = 1
    return n * s


class Body:
    """Measurements of the rest-pose body for fitting the outfit."""

    def __init__(self, body, arm, R):
        me = body.data
        self.V = np.array([v.co for v in me.vertices])
        self.s = gc.look_scale(R)
        f = R["fit"]
        self.R = R
        self.sh = np.array(f["shoulder"])
        self.hip = np.array(f["hip"])
        self.knee = np.array(f["knee"])
        self.ankle = np.array(f["ankle"])
        self.wrist = np.array(f["wrist"])
        self.belt_z = self.hip[2] + 0.17 * self.s
        self.neck_z = self.sh[2] + 0.10 * self.s
        self.torso_hw = R["torso_column"][0][1]
        self.bones = {b.name: (np.array(arm.matrix_world @ b.head_local), np.array(arm.matrix_world @ b.tail_local))
                      for b in arm.data.bones}
        # Braids / beards hang over the outfit: their vertices are left out of the sampling.
        keep = np.ones(len(self.V), bool)
        for rg in R["regions"]:
            if "capsule" in rg and any(b in ("braid_l", "braid_r", "beard") for _, w in rg["chain"] for b in w):
                p0, p1, rad = rg["capsule"]
                p0, p1 = np.array(p0), np.array(p1)
                d = p1 - p0
                t = np.clip(((self.V - p0) @ d) / (d @ d), 0, 1)
                dist = np.linalg.norm(self.V - (p0 + t[:, None] * d), axis=1)
                keep &= dist > rad + 0.025
        self.keep = keep
        # Head: everything above the neck within the head's width.
        hv = self.V[(self.V[:, 2] > self.neck_z + 0.02) & (np.abs(self.V[:, 0]) < 0.2) & keep]
        self.top_z = float(hv[:, 2].max())
        self.head_min = hv.min(0)
        self.head_max = hv.max(0)
        self.hc = np.array([0.0, (self.head_min[1] + self.head_max[1]) / 2,
                            self.top_z - 0.5 * (self.top_z - self.neck_z) * 0.82])
        self.chin_z = self.top_z - 0.80 * (self.top_z - self.neck_z)
        self.height = float(self.V[:, 2].max())

    def pts(self, xlim=None, zlim=None, keep_only=True):
        m = self.keep.copy() if keep_only else np.ones(len(self.V), bool)
        if xlim is not None:
            m &= (self.V[:, 0] >= xlim[0]) & (self.V[:, 0] <= xlim[1])
        if zlim is not None:
            m &= (self.V[:, 2] >= zlim[0]) & (self.V[:, 2] <= zlim[1])
        return self.V[m]


# ---------------------------------------------------------------------------------------------
# Outfit pieces

def apron(o, B, top_z, bib_hw, belt_hw, hem_z, hem_hw, offset, thick, swatch, extra_pts=None, rows=20, cols=11):
    """Bib from top_z to the belt (follows the chest), skirt from the belt to hem_z (hangs straight
    from the most forward point above). Returns the front grid (rows, cols, 3)."""
    zs = np.concatenate([np.linspace(top_z, B.belt_z, rows // 3 + 1)[:-1], np.linspace(B.belt_z, hem_z, rows - rows // 3)])
    s = np.linspace(-1, 1, cols)
    hw = np.where(zs >= B.belt_z, bib_hw + (belt_hw - bib_hw) * np.clip((top_z - zs) / max(top_z - B.belt_z, 1e-6), 0, 1) ** 2,
                  belt_hw + (hem_hw - belt_hw) * np.clip((B.belt_z - zs) / max(B.belt_z - hem_z, 1e-6), 0, 1))
    X = s[None, :] * hw[:, None]
    Z = np.repeat(zs[:, None], cols, 1)
    pts = B.pts(xlim=(-0.6, 0.6), zlim=(hem_z - 0.1, top_z + 0.1))
    pts = pts[np.abs(pts[:, 0]) < B.torso_hw + 0.08]
    if extra_pts is not None:
        pts = np.concatenate([pts, extra_pts])
    org = np.stack([X.ravel(), np.full(X.size, 0.6), Z.ravel()], 1)
    d = np.tile([0.0, -1.0, 0.0], (X.size, 1))
    r = ray_extent(org, d, pts, 0.03).reshape(X.shape)
    Yraw = fill_nan(0.6 - r)
    # Smooth outward (towards -Y) and hang below the belt; towards the hem the cloth settles back
    # part of the way onto the legs (never inside them), so it does not stand off like a board.
    Yraw = -blur_max(-Yraw, 2)
    Y = Yraw.copy()
    for i in range(1, len(zs)):
        if zs[i] < B.belt_z - 0.01:
            Y[i] = np.minimum(Y[i], Y[i - 1])
    k = (np.clip((B.belt_z - zs) / max(B.belt_z - hem_z, 1e-6), 0, 1) * 0.6)[:, None]
    Y = np.minimum(Y * (1 - k) + Yraw * k, Yraw)
    for _ in range(2):  # soften the creases the blend leaves between the legs
        p = np.pad(Y, 1, mode="edge")
        Y = np.minimum((p[1:-1, :-2] + p[1:-1, 1:-1] + p[1:-1, 2:] + p[:-2, 1:-1] + p[2:, 1:-1]) / 5, Yraw)
    # The sides wrap back a little (cloth tied round the body).
    Y = Y - offset + 0.035 * np.abs(s[None, :]) ** 3
    P = np.stack([X, Y, Z], -1)
    N = grid_normals(P, np.tile([0.0, -1.0, 0.0], P.shape[:2] + (1,)))
    wz = apron_weight(B)
    o.slab(P, N, thick, swatch, weight=lambda r, c: wz(P[r, c, 2]))
    return P, N, zs


def apron_weight(B):
    """Skirt below the belt on pelvis -> cloth_front (like the source looks' cloth panels); above:
    nearest body weights (None)."""
    drop = 0.10 * B.s

    def w(z):
        if z >= B.belt_z:
            return None
        t = np.clip((B.belt_z - z) / drop, 0, 1)
        return {"pelvis": 1 - t, "cloth_front": t} if t < 1 else {"cloth_front": 1.0}
    return w


def belt(o, B, z, h, swatch, extra_pts=None, offset=0.012, sides=32, thick=0.008):
    """A belt round the waist at height z over the body (and over extra_pts, e.g. the apron).
    Returns (angle -> (point, outward normal)) sampler."""
    a = np.linspace(0, 2 * math.pi, sides, endpoint=False)
    d = np.stack([np.sin(a), -np.cos(a), np.zeros_like(a)], 1)
    pts = B.pts(zlim=(z - h, z + h))
    pts = pts[np.abs(pts[:, 0]) < B.torso_hw + 0.06]
    if extra_pts is not None:
        e = extra_pts[np.abs(extra_pts[:, 2] - z) < h]
        pts = np.concatenate([pts, e])
    cy = float(np.median(pts[:, 1]))
    org = np.tile([0.0, cy, z], (sides, 1))
    r = ray_extent(org, d, pts, 0.03)
    r = fill_nan(r[None, :])[0]
    r = blur_max(r[None, :], 2, wrap_cols=True)[0] + offset
    P = org + d * r[:, None]
    up = np.tile([0.0, 0.0, 1.0], (sides, 1))
    # Tube: width along Z (up), height along the outward normal.
    o.tube(P, d, up, h, thick, swatch, weight=None, closed=True)

    def at(angle):
        k = (angle % (2 * math.pi)) / (2 * math.pi) * sides
        i0 = int(math.floor(k)) % sides
        i1 = (i0 + 1) % sides
        t = k - math.floor(k)
        p = P[i0] * (1 - t) + P[i1] * t + d[i0] * thick
        n = d[i0] * (1 - t) + d[i1] * t
        return p, n / np.linalg.norm(n)
    return at


def strap(o, B, keys, w, thick, offset, swatch, extra_pts=None, xlim=None, n=26):
    """A strap lying on the body along key rays [(origin, dir), ...] (interpolated, projected onto
    the surface). Weights: nearest body."""
    keys = [(np.array(a, float), np.array(b, float) / np.linalg.norm(b)) for a, b in keys]
    t = np.linspace(0, len(keys) - 1, n)
    org, dirs = [], []
    for tt in t:
        i = min(int(tt), len(keys) - 2)
        f = tt - i
        org.append(keys[i][0] * (1 - f) + keys[i + 1][0] * f)
        dd = keys[i][1] * (1 - f) + keys[i + 1][1] * f
        dirs.append(dd / np.linalg.norm(dd))
    org, dirs = np.array(org), np.array(dirs)
    pts = B.pts()
    if xlim is not None:
        pts = pts[(pts[:, 0] > xlim[0]) & (pts[:, 0] < xlim[1])]
    if extra_pts is not None:
        pts = np.concatenate([pts, extra_pts])
    r = ray_extent(org, dirs, pts, 0.03)
    r = fill_nan(r[None, :])[0]
    # Smooth outward along the path.
    for _ in range(3):
        p = np.pad(r, 1, mode="edge")
        r = np.maximum(r, (p[:-2] + p[1:-1] + p[2:]) / 3)
    P = org + dirs * (r + offset)[:, None]
    T = np.gradient(P, axis=0)
    T /= np.maximum(np.linalg.norm(T, axis=1, keepdims=True), 1e-9)
    Bn = np.cross(T, dirs)
    Bn /= np.maximum(np.linalg.norm(Bn, axis=1, keepdims=True), 1e-9)
    N = np.cross(Bn, T)
    o.tube(P, N, Bn, w, thick, swatch, weight=None, t_scale=max(1.0, len(P) / 8))
    return P


def hood(o, B, swatch, lining, rim, margin=0.024):
    """Hood open round the face: a spherical grid round the head centre with its pole pointing
    forward (the face opening is the polar cap that is left out), radius from the head's own
    extent + margin, closed at the back; inner layer 8 mm in; rim stitched at the opening."""
    hc = B.hc.copy()
    f = np.array([0.0, -1.0, -0.22])
    f /= np.linalg.norm(f)
    e1 = np.array([1.0, 0.0, 0.0])
    e2 = np.cross(f, e1)
    e2 /= np.linalg.norm(e2)
    if e2[2] < 0:
        e2 = -e2
    rows, cols = 12, 26
    psi = np.linspace(0, 2 * math.pi, cols, endpoint=False)
    aw, ah = math.radians(50), math.radians(60)
    alpha = 1.0 / np.sqrt((np.cos(psi) / aw) ** 2 + (np.sin(psi) / ah) ** 2)
    alpha = np.where(np.sin(psi) < 0, alpha * 0.92, alpha)  # a little lower under the chin
    t = np.linspace(0, 1, rows)
    phi = alpha[None, :] + (math.pi * 0.95 - alpha[None, :]) * t[:, None]
    D = (np.cos(phi)[..., None] * f + np.sin(phi)[..., None] * (np.cos(psi)[None, :, None] * e1 +
                                                               np.sin(psi)[None, :, None] * e2))
    pts = B.pts(zlim=(B.chin_z - 0.03, 9.0))
    pts = pts[(np.abs(pts[:, 0]) < 0.25) & (np.abs(pts[:, 1] - hc[1]) < 0.25)]
    org = np.tile(hc, (D.size // 3, 1))
    r = ray_extent(org, D.reshape(-1, 3), pts, 0.03).reshape(rows, cols)
    floor_z = B.chin_z - 0.02
    down = -D[..., 2]
    cap = np.where(down > 0.05, (hc[2] - floor_z) / np.maximum(down, 0.05), 0.3)
    r = np.where(np.isnan(r), np.minimum(cap, 0.22), np.minimum(r, np.maximum(cap, 0.10)))
    r = blur_max(r, 3, wrap_cols=True)
    # Looser at the back and over the crown (a soft hood, not a coif).
    r = r + margin + 0.015 * t[:, None] ** 1.5
    P = hc + D * r[..., None]
    Pin = hc + D * (r - 0.009)[..., None]
    # Below the chin the hood closes in round the neck (it would flare into a skirt otherwise).
    neck_r = 0.1 * B.s

    def neck_in(Q, inset):
        rel = Q[..., :2] - hc[:2]
        hr = np.maximum(np.linalg.norm(rel, axis=-1), 1e-6)
        zf = np.clip((Q[..., 2] - floor_z) / 0.07, 0, 1)
        lim = (neck_r - inset) + zf * 0.3
        k = np.minimum(1.0, lim / hr)
        Q = Q.copy()
        Q[..., :2] = hc[:2] + rel * k[..., None]
        return Q
    P = neck_in(P, 0.0)
    Pin = neck_in(Pin, 0.009)
    uv = np.stack(np.meshgrid(np.linspace(0, 1, cols + 1)[:-1], t), -1)
    wfn = lambda rr, cc: {"Head": 1.0} if P[rr, cc, 2] > B.chin_z + 0.02 else {"Head": 0.7, "neck_01": 0.3}  # noqa: E731
    out_i = o.grid(P, uv, swatch, wfn, wrap=True)
    in_i = o.grid(Pin, uv, lining, wfn, wrap=True, flip=True)
    rim_uv = [(k / cols, 0.0) for k in range(cols)]
    o.strip(list(out_i[0]), list(in_i[0]), rim_uv, [(u, 1.0) for u, _ in rim_uv], rim, closed=True)
    back = hc + (-f) * (r[-1].mean() + 0.01)
    c_out = o._add_verts([back], {"Head": 1.0})
    c_in = o._add_verts([hc + (-f) * (r[-1].mean() - 0.008)], {"Head": 1.0})
    o.fan(list(out_i[-1]), c_out, [tuple(uv[-1, k]) for k in range(cols)], (0.5, 1.0), swatch)
    o.fan(list(in_i[-1])[::-1], c_in, [tuple(uv[-1, k]) for k in range(cols)][::-1], (0.5, 1.0), lining)
    return P.reshape(-1, 3)


def cowl(o, B, swatch, extra_pts, z_top, z_bot, offset=0.02, rows=8, sides=30, hem=None):
    """A short cape over the shoulders: rings round the torso axis from z_top (the hood's lower
    edge) down to z_bot, each ray's radius the body's extent (arms left out: they would widen it
    into a bell) + offset, never narrowing downward."""
    zs = np.linspace(z_top, z_bot, rows)
    a = np.linspace(0, 2 * math.pi, sides, endpoint=False)
    d = np.stack([np.sin(a), -np.cos(a), np.zeros_like(a)], 1)
    pts = B.pts(zlim=(z_bot - 0.05, z_top + 0.02))
    pts = pts[np.abs(pts[:, 0]) < B.sh[0] + 0.03]
    # The hood is not sampled: its lower edge tucks into the cowl instead of the cowl wrapping it.
    cy = float(np.median(B.pts(zlim=(z_bot, B.sh[2]))[:, 1]))
    # The collar dips 5 cm at the front (under the chin) and rises at the back of the neck.
    front = np.clip(np.cos(a), 0, 1)
    org = np.concatenate([np.stack([np.zeros(sides), np.full(sides, cy),
                                    z - 0.05 * front * (1 - i / (rows - 1))], 1) for i, z in enumerate(zs)])
    D = np.tile(d, (rows, 1))
    r = ray_extent(org, D, pts, 0.035).reshape(rows, sides)
    r = fill_nan(r)
    r = blur_max(r, 2, wrap_cols=True)
    for i in range(1, rows):
        r[i] = np.maximum(r[i], r[i - 1])
    # A sloping mantle rather than a shelf: from the collar radius at the top out to the shoulders'
    # radius along an easing curve, never inside the body.
    tt = np.linspace(0, 1, rows)[:, None]
    ease = np.sin(0.5 * math.pi * tt) ** 0.8
    r = np.maximum(r[:1] + (r[-1:] - r[:1]) * ease, r)
    r = blur_max(r, 1, wrap_cols=True)
    # Never wider than the collar + 13 cm (scaled): over a wide fur collar or big pauldrons the
    # cowl stays a cowl (the fur shows under its hem) instead of becoming a square poncho.
    r = np.minimum(r, r[:1] + 0.13 * B.s * ease)
    r = r + offset + 0.025 * tt ** 2
    P = org.reshape(rows, sides, 3) + D.reshape(rows, sides, 3) * r[..., None]
    # Inner layer and a turned hem: the cape is 10 mm thick.
    Pin = org.reshape(rows, sides, 3) + D.reshape(rows, sides, 3) * (r - 0.010)[..., None]
    uv = np.stack(np.meshgrid(np.linspace(0, 1, sides + 1)[:-1], np.linspace(0, 1, rows)), -1)
    o_i = o.grid(P, uv, swatch, None, wrap=True)
    i_i = o.grid(Pin, uv, swatch, None, wrap=True, flip=True)
    ru = [(k / sides, 0.0) for k in range(sides)]
    o.strip(list(i_i[0]), list(o_i[0]), ru, ru, swatch, closed=True)
    o.strip(list(o_i[-1]), list(i_i[-1]), [(u, 1.0) for u, _ in ru], [(u, 1.0) for u, _ in ru], hem or swatch, closed=True)
    return P.reshape(-1, 3)


def drape_cowl(o, B, swatch, hood_pts, offset=0.026, rows=8, sides=32, hem=None, folds=8, x_reach=0.12):
    """A draped capelet for looks with a wide, high fur collar (kargan_1), where the ring cowl's
    vertical walls read as a square mantle. Per column round the torso axis a quarter-ellipse
    profile: from a narrow collar round the hood's neck (higher at the sides and back, dipping under
    the chin) out over the shoulder's slope, then down to the hem, which hangs lower at the front
    and back than over the arms. Shallow radial folds gathered at the collar deepen towards the hem
    and pull it down a little at each crest (a scalloped edge). Wherever the profile would cut the
    body (or the hood near the collar) it is pushed out and the push is smoothed over the
    neighbours, so it bulges over the fur instead of stepping. Past the shoulder joint only the
    pauldrons' tops count (above the joint, |x| < sh + x_reach); the arm below hangs out under the
    hem. Returns the outer grid points."""
    s = B.s
    chin, sh_z = B.chin_z, B.sh[2]
    a = np.linspace(0, 2 * math.pi, sides, endpoint=False)
    d = np.stack([np.sin(a), -np.cos(a), np.zeros_like(a)], 1)
    front = np.clip(np.cos(a), 0, 1) ** 1.5
    side = np.sin(a) ** 2
    # Folds: two periodic waves (irregular spacing), 0 in the troughs, 1 on the crests.
    fw = 0.6 * np.cos(folds * a + 0.4) + 0.4 * np.cos((folds - 3) * a + 1.3)
    f01 = (fw - fw.min()) / (fw.max() - fw.min())
    back = np.clip(-np.cos(a), 0, 1) ** 1.5
    z_c = chin + 0.070 * s - 0.095 * s * front + 0.035 * s * back
    z_h = sh_z - 0.120 * s + 0.115 * s * side ** 1.2 - 0.032 * s * f01
    cy = float(np.median(B.pts(zlim=(sh_z - 0.10 * s, sh_z))[:, 1]))
    body = B.pts(zlim=(z_h.min() - 0.05, z_c.max() + 0.03))
    # The torso and the fur over the shoulders; past the shoulder joint only what lies above it
    # (the pauldrons' tops: the cape rests on them, the arm hangs out under its hem).
    ax = np.abs(body[:, 0])
    body = body[(ax < B.sh[0] + 0.03) | ((ax < B.sh[0] + x_reach) & (body[:, 2] > sh_z - 0.01))]
    hood_low = hood_pts[hood_pts[:, 2] < z_c.max() + 0.03]
    pts = np.concatenate([body, hood_low])

    def need(z):
        """Per column: the body's (and the hood's) extent at heights z (sides,) + offset."""
        org = np.stack([np.zeros(sides), np.full(sides, cy), z], 1)
        r = ray_extent(org, d, pts, 0.03)
        return np.where(np.isnan(r), 0.0, r + offset)
    # Collar: just outside the hood's neck ring (and the neck); the hem: outside everything the
    # cape passes over, plus a little flare.
    r_c = blur_max(need(z_c)[None, :], 1, wrap_cols=True)[0]
    r_c = np.maximum(r_c, 0.09 * s)
    hs = np.linspace(0, 1, 16)
    r_h = np.max([need(z_c + (z_h - z_c) * h) for h in hs], axis=0)
    r_h = blur_max(r_h[None, :], 2, wrap_cols=True)[0] + 0.008 * s
    tt = np.linspace(0, 1, rows)[:, None]
    th = 0.5 * math.pi * tt
    R = r_c[None, :] + (r_h - r_c)[None, :] * (0.85 * np.sin(th) + 0.15 * tt)
    # Mostly a slope from the collar (the shoulder line), rounding over into the fall.
    Z = z_c[None, :] + (z_h - z_c)[None, :] * (0.5 * (1 - np.cos(th)) + 0.5 * tt)
    # Push out where the profile cuts the body; spread the push so it stays a smooth bulge.
    push = np.zeros_like(R)
    for i in range(rows):
        push[i] = np.maximum(need(Z[i]) - R[i], 0.0)
    push = blur_max(push, 3, wrap_cols=True)
    R = R + push
    for i in range(1, rows):  # never tucking back in towards the hem
        R[i] = np.maximum(R[i], R[i - 1] - 0.004)
    # The folds go on last (outward only), so the push does not fill their troughs.
    R = R + 0.060 * s * tt * f01[None, :] ** 1.6
    P = np.stack([d[None, :, 0] * R, cy + d[None, :, 1] * R, Z], -1)
    Nrm = grid_normals(P, np.broadcast_to(d[None, :, :], P.shape).copy())
    # Inner layer 10 mm in along the surface normal (the top of the dome is nearly horizontal).
    Pin = P - Nrm * 0.010
    uv = np.stack(np.meshgrid(np.linspace(0, 1, sides + 1)[:-1], np.linspace(0, 1, rows)), -1)
    o_i = o.grid(P, uv, swatch, None, wrap=True)
    i_i = o.grid(Pin, uv, swatch, None, wrap=True, flip=True)
    ru = [(k / sides, 0.0) for k in range(sides)]
    o.strip(list(i_i[0]), list(o_i[0]), ru, ru, swatch, closed=True)
    o.strip(list(o_i[-1]), list(i_i[-1]), [(u, 1.0) for u, _ in ru], [(u, 1.0) for u, _ in ru], hem or swatch, closed=True)
    return P.reshape(-1, 3)


def cap(o, B, swatch, band, brim, studs, fitting, style=None):
    """A cap on the crown: rings round the head axis from the brow up, closed at the top; a band
    round the brow; a brim or iron studs (skullcap). style: "soft" (a loose linen / felt bonnet),
    "skull" (tight; studs), "bork" (a tall felt cap rising well above the crown, its tip slumping
    forward, a thick rolled brim), "bandana" (a cloth tied tight over the skull, lower at the back,
    a knot and two tails at the back of the head)."""
    style = style or ("skull" if studs else "soft")
    hc = B.hc
    s = B.s
    brow = B.top_z - 0.09 * B.s
    sides = 32
    a = np.linspace(0, 2 * math.pi, sides, endpoint=False)
    d = np.stack([np.sin(a), -np.cos(a), np.zeros_like(a)], 1)
    back = np.clip(-np.cos(a), 0, 1)
    tall = 0.055 * s if style == "bork" else 0.0  # the test allows the merchant 10 cm over the look
    nz = 12 if style == "bork" else 9
    zs = np.linspace(brow, B.top_z - 0.004 + tall, nz)
    t = np.linspace(0, 1, len(zs))[:, None]
    # The bandana's lower edge drops at the back of the head (over the occiput).
    drop = (-0.035 * s * back[None, :] * (1 - t)) if style == "bandana" else np.zeros((len(zs), sides))
    pts = B.pts(zlim=(brow - 0.06, 9.0))
    pts = pts[(np.abs(pts[:, 0]) < 0.2) & (np.abs(pts[:, 1] - hc[1]) < 0.2)]
    Z = zs[:, None] + drop
    org = np.stack([np.full(Z.size, hc[0]), np.full(Z.size, hc[1]), Z.ravel()], 1)
    r = ray_extent(org, np.tile(d, (len(zs), 1)), pts, 0.025).reshape(len(zs), sides)
    if style == "bork":
        # Above the scalp nothing is hit: the felt wall rises from the brow ring, tapering a little.
        # The wall bulges a little (soft felt) and rounds over into a dome at the top.
        r0 = blur_max(fill_nan(r[:1]), 2, wrap_cols=True)[0]
        dome = np.sqrt(np.clip(1.0 - (np.clip(t - 0.55, 0, 1) / 0.45) ** 2, 0.06, 1.0))
        wall = r0[None, :] * (1.0 + 0.09 * np.sin(np.pi * np.clip(t * 1.1, 0, 1))) * dome
        r = np.where(np.isnan(r), 0.0, r)
        r = blur_max(r, 2, wrap_cols=True)
        r = np.maximum(r, wall) + 0.014 * dome
    elif style == "bandana":
        # Tight over the skull: smoothed round each ring only (a 3x3 blur would square the crown off).
        r = fill_nan(r)
        r = np.concatenate([blur_max(r[i:i + 1], 2, wrap_cols=True) for i in range(len(zs))])
        r = r + 0.009
    else:
        r = fill_nan(r)
        r = blur_max(r, 2, wrap_cols=True)
        # Soft caps (linen / felt) puff out over the crown like a loose bonnet; skullcap and bandana
        # are tight.
        soft = 1.0 if style == "soft" else 0.0
        off = 0.009 if style == "bandana" else 0.012
        r = r + off + soft * 0.016 * np.sin(np.pi * np.clip(t * 0.85, 0, 1))
        r[-1] = np.maximum(r[-1] * (0.75 if soft else 1.0), 0.02)
    D3 = np.tile(d, (len(zs), 1)).reshape(len(zs), sides, 3)
    P = org.reshape(len(zs), sides, 3) + D3 * r[..., None]
    if style == "bork":
        # The tip slumps forward (felt), the top is a soft dome.
        P[:, :, 1] -= 0.02 * s * t ** 2.2
        P[:, :, 2] += 0.010 * t ** 0.7
    elif style == "bandana":
        P[:, :, 2] += 0.008 * t ** 0.7
    else:
        soft = 1.0 if style == "soft" else 0.0
        P[:, :, 2] += (0.012 + soft * 0.022) * t ** 0.7  # crown above the scalp
    Pin = P - D3 * 0.007
    Pin[:, :, 2] -= 0.006
    uv = np.stack(np.meshgrid(np.linspace(0, 1, sides + 1)[:-1], np.linspace(0, 1, len(zs))), -1)
    W = {"Head": 1.0}
    o_i = o.grid(P, uv, swatch, W, wrap=True)
    i_i = o.grid(Pin, uv, swatch, W, wrap=True, flip=True)
    ru = [(k / sides, 0.0) for k in range(sides)]
    o.strip(list(i_i[0]), list(o_i[0]), ru, ru, swatch, closed=True)
    top = o._add_verts([P[-1].mean(0) + [0, 0, 0.012]], W)
    top_in = o._add_verts([Pin[-1].mean(0) + [0, 0, 0.004]], W)
    o.fan(list(o_i[-1]), top, [tuple(uv[-1, k]) for k in range(sides)], (0.5, 1.0), swatch)
    o.fan(list(i_i[-1])[::-1], top_in, [tuple(uv[-1, k]) for k in range(sides)][::-1], (0.5, 1.0), swatch)
    # Band round the brow (a tube just outside the cap's lowest rows).
    # Felt caps get a thick rolled band (the börk a fat rolled brim), the skullcap a flat iron-grey
    # one, the bandana a thin rolled edge.
    nb = d
    if style == "bork":
        pb = P[0] + nb * 0.006
        o.tube(pb - [0, 0, 0.022], nb, np.tile([0, 0, 1.0], (sides, 1)), 0.05, 0.02, band, weight=W, closed=True)
    elif style == "bandana":
        pb = P[0] + nb * 0.002
        o.tube(pb - [0, 0, 0.004], nb, np.tile([0, 0, 1.0], (sides, 1)), 0.016, 0.006, band, weight=W, closed=True)
        # The knot at the back of the head and two tails hanging from it.
        kb = sides // 2
        kp = P[1, kb] + d[kb] * 0.012
        o.lathe(kp - d[kb] * 0.012, d[kb], [1.0, 0, 0], [(0.0, 0.0), (0.018, 0.004), (0.022, 0.016), (0.016, 0.028),
                                                         (0.0, 0.032)], 10, swatch, W)
        for sgn in (-1, 1):
            side_v = np.array([sgn * 1.0, 0, 0])
            L, wd = 0.15 * s, 0.038 * s
            rows = 5
            ts = np.linspace(0, 1, rows)
            ctr = kp[None, :] + d[kb][None, :] * (0.012 + 0.03 * ts[:, None]) + \
                side_v[None, :] * (0.012 + 0.03 * ts[:, None]) - np.array([0, 0, 1.0])[None, :] * L * ts[:, None]
            wid = wd * (1.0 - 0.3 * ts)
            Pt = np.stack([ctr - side_v * (wid[:, None] / 2), ctr + side_v * (wid[:, None] / 2)], 1)
            Nt = np.broadcast_to(d[kb], Pt.shape).copy()
            o.slab(Pt, Nt, 0.005, swatch, weight=W)
    else:
        pb = P[1] + nb * 0.004
        o.tube(pb - [0, 0, 0.012], nb, np.tile([0, 0, 1.0], (sides, 1)), 0.03 if studs else 0.026,
               0.006 if studs else 0.009, band, weight=W, closed=True)
    if brim:
        # A short brim all round, longer at the front: a flat ring sloping down 15 degrees.
        ext = 0.03 + 0.03 * np.clip(-np.cos(a), 0, 1)
        inner = P[0] + nb * 0.002
        outer = inner + nb * ext[:, None] + np.array([0, 0, -0.012])
        ring_in = o._add_verts(inner, W) + np.arange(sides)
        ring_out = o._add_verts(outer, W) + np.arange(sides)
        ring_in_b = o._add_verts(inner - [0, 0, 0.007], W) + np.arange(sides)
        ring_out_b = o._add_verts(outer - [0, 0, 0.007], W) + np.arange(sides)
        ru0 = [(k / sides, 0.0) for k in range(sides)]
        ru1 = [(k / sides, 1.0) for k in range(sides)]
        o.strip(list(ring_out), list(ring_in), ru1, ru0, swatch, closed=True)
        o.strip(list(ring_in_b), list(ring_out_b), ru0, ru1, swatch, closed=True)
        o.strip(list(ring_out_b), list(ring_out), ru1, ru1, swatch, closed=True)
        o.strip(list(ring_in), list(ring_in_b), ru0, ru0, swatch, closed=True)
    if studs:
        for zz in (2, 5):
            for k in range(0, sides, 4):
                p = P[zz, k] + d[k] * 0.003
                up = np.array([0, 0, 1.0])
                side = np.cross(up, d[k])
                o.box(p, (side, up, d[k]), (0.014, 0.014, 0.010), fitting, W)
    return P.reshape(-1, 3)


# ---------------------------------------------------------------------------------------------
# Individual features (each merchant's own: moustache, pipe, herb sprig, brooch, circlet, beard rings)

def head_pts(B, z0):
    pts = B.pts(zlim=(z0, 9.0))
    return pts[(np.abs(pts[:, 0]) < 0.2) & (np.abs(pts[:, 1] - B.hc[1]) < 0.25)]


def face_y(B, pts, x, z):
    """y of the face surface (front = -Y) at (x, z): the farthest head point along a ray from the
    head's centre plane forward."""
    x, z = np.atleast_1d(x).astype(float), np.atleast_1d(z).astype(float)
    org = np.stack([x, np.full(len(x), B.hc[1]), z], 1)
    d = np.tile([0.0, -1.0, 0.0], (len(x), 1))
    r = ray_extent(org, d, pts, 0.008)
    r2 = ray_extent(org, d, pts, 0.016)
    r = np.where(np.isnan(r), r2, r)
    r = fill_nan(r[None, :])[0]
    return B.hc[1] - r


def nose_tip(B, pts):
    """The most forward point of the face between the chin and the head's centre."""
    m = (np.abs(pts[:, 0]) < 0.02) & (pts[:, 2] > B.chin_z + 0.03) & (pts[:, 2] < B.hc[2])
    q = pts[m]
    return q[np.argmin(q[:, 1])]


def taper_tube(o, P, rad, swatch, weight, sides=8, squash=1.0, ref=(0.0, -1.0, 0.0)):
    """A round tube along P (n, 3) with radius rad (n,), squashed along `ref` (the face normal) by
    `squash`; closed with a fan at both ends. UVs: s round, t along."""
    n = len(P)
    T = np.gradient(P, axis=0)
    T /= np.maximum(np.linalg.norm(T, axis=1, keepdims=True), 1e-9)
    ref = np.asarray(ref, float)
    Nn = ref[None, :] - T * (T @ ref)[:, None]
    Nn /= np.maximum(np.linalg.norm(Nn, axis=1, keepdims=True), 1e-9)
    Bn = np.cross(T, Nn)
    a = np.linspace(0, 2 * math.pi, sides, endpoint=False)
    G = P[:, None, :] + rad[:, None, None] * (np.cos(a)[None, :, None] * Nn[:, None, :] * squash +
                                              np.sin(a)[None, :, None] * Bn[:, None, :])
    uv = np.stack(np.meshgrid(np.linspace(0, 1, sides + 1)[:-1], np.linspace(0, 1, n)), -1)
    idx = o.grid(G, uv, swatch, weight, wrap=True)
    for row, end in ((0, P[0] - T[0] * rad[0] * 0.5), (n - 1, P[-1] + T[-1] * rad[-1] * 0.5)):
        c = o._add_verts([end], weight)
        o.fan(list(idx[row]), c, [tuple(uv[row, k]) for k in range(sides)], (0.5, float(row > 0)), swatch)


def moustache(o, B, swatch):
    """A full moustache over the upper lip whose ends droop past the mouth corners, following the
    face's surface; rigid on Head."""
    s = B.s
    pts = head_pts(B, B.chin_z - 0.03)
    nose = nose_tip(B, pts)
    lip = nose[2] - 0.020 * s
    u = np.linspace(-1, 1, 19)
    x = 0.052 * s * u
    droop = 0.046 * s * np.clip((np.abs(u) - 0.45) / 0.55, 0, 1) ** 1.3
    z = lip - 0.004 * s * np.abs(u) - droop
    y = face_y(B, pts, x, z)
    for _ in range(2):
        p = np.pad(y, 1, mode="edge")
        y = np.minimum(y, (p[:-2] + p[1:-1] + p[2:]) / 3)
    rad = 0.0105 * s * (1.0 - 0.6 * np.abs(u) ** 2.2) * (1.0 - 0.3 * np.exp(-(u / 0.1) ** 2)) + 0.0012
    P = np.stack([x, y - 0.6 * rad, z], 1)
    taper_tube(o, P, rad, swatch, {"Head": 1.0}, sides=8, squash=0.7)
    return nose


def pipe(o, B, wood, ring):
    """A long-stemmed pipe held in the right mouth corner, the bowl hanging forward and down."""
    s = B.s
    pts = head_pts(B, B.chin_z - 0.03)
    nose = nose_tip(B, pts)
    mz = nose[2] - 0.034 * s
    x0 = -0.022 * s  # the merchant's right (+X is his left: the mesh faces -Y)
    y0 = float(face_y(B, pts, x0, mz)[0]) + 0.006
    p0 = np.array([x0, y0, mz])
    c1, c2, c3 = p0 + [-0.012, -0.05, -0.006], p0 + [-0.035, -0.10, -0.03], p0 + [-0.045, -0.125, -0.05]
    tt = np.linspace(0, 1, 12)[:, None]
    P = (1 - tt) ** 3 * p0 + 3 * (1 - tt) ** 2 * tt * c1 + 3 * (1 - tt) * tt ** 2 * c2 + tt ** 3 * c3
    W = {"Head": 1.0}
    taper_tube(o, P, np.linspace(0.0040, 0.0052, len(P)), wood, W, sides=8, ref=(0.0, 0.0, 1.0))
    up = np.array([0.0, 0.0, 1.0])
    base = c3 - up * 0.008
    o.lathe(base, up, [1.0, 0, 0], [(0.0, 0.0), (0.011, 0.001), (0.0155, 0.014), (0.0165, 0.036), (0.0125, 0.037),
                                    (0.0115, 0.02), (0.0, 0.018)], 12, wood, W)
    o.lathe(base + up * 0.030, up, [1.0, 0, 0], [(0.0168, 0.0), (0.0172, 0.004), (0.0168, 0.008)], 12, ring, W)


def herb_sprig(o, B, hood_pts, leaf, tie):
    """A bunch of herb stems tucked into the hood over the right temple, leaves along each stem."""
    s = B.s
    hc = B.hc
    dirn = np.array([-0.72, -0.30, 0.62])
    dirn /= np.linalg.norm(dirn)
    r = ray_extent(hc[None, :], dirn[None, :], hood_pts, 0.03)[0]
    if np.isnan(r):
        r = 0.13
    base = hc + dirn * (r - 0.006)
    W = {"Head": 1.0}
    rng = np.random.default_rng(7)
    for k, (ang, L) in enumerate(((-0.35, 0.115), (0.0, 0.13), (0.4, 0.11))):
        sd = np.array([-0.75 + 0.3 * ang, 0.45 + 0.3 * ang, 0.85])
        sd /= np.linalg.norm(sd)
        L *= s
        side = np.cross(sd, dirn)
        side /= np.linalg.norm(side)
        o.box(base + sd * L / 2, (side, np.cross(sd, side), sd), (0.004, 0.004, L), leaf, W)
        for j in range(5):
            f = 0.3 + 0.15 * j
            p = base + sd * L * f
            for sg in (-1, 1):
                lv = sd * 0.6 + side * sg
                lv /= np.linalg.norm(lv)
                ln = (0.034 - 0.003 * j) * s * (0.9 + 0.2 * rng.random())
                nrm = np.cross(lv, sd)
                nrm /= np.linalg.norm(nrm)
                o.box(p + lv * ln / 2, (np.cross(nrm, lv), nrm, lv), (0.014 * s, 0.003, ln), leaf, W)
        # A bud cluster at the tip.
        o.box(base + sd * (L + 0.008), (side, np.cross(sd, side), sd), (0.012, 0.012, 0.018), leaf, W)
    o.box(base + np.array([0, 0, 0.02]), (np.array([1.0, 0, 0]), np.array([0, 1.0, 0]), np.array([0, 0, 1.0])),
          (0.022, 0.022, 0.016), tie, W)


def brooch(o, B, cowl_pts, swatch, inner):
    """A round clasp on the cowl's front just under the chin."""
    q = cowl_pts[np.abs(cowl_pts[:, 0]) < 0.03]
    q = q[q[:, 2] > q[:, 2].max() - 0.07]
    p = q[np.argmin(q[:, 1])]
    ax = np.array([0.0, -1.0, 0.18])
    ax /= np.linalg.norm(ax)
    base = p + ax * 0.002
    o.lathe(base, ax, [1.0, 0, 0], [(0.0, 0.0), (0.024, 0.0), (0.026, 0.005), (0.02, 0.009), (0.0, 0.009)], 16,
            swatch, None)
    o.lathe(base + ax * 0.008, ax, [1.0, 0, 0], [(0.0, 0.0), (0.009, 0.0), (0.008, 0.006), (0.0, 0.008)], 10, inner,
            None)


def circlet(o, B, metal, bead):
    """A thin iron circlet across the brow (its sides run under the hood) with an ember bead over the
    middle of the forehead."""
    s = B.s
    pts = head_pts(B, B.chin_z)
    z = B.top_z - 0.085 * s
    a = np.radians(np.linspace(-95, 95, 19))
    d = np.stack([np.sin(a), -np.cos(a), np.zeros_like(a)], 1)
    org = np.tile([0.0, B.hc[1], z], (len(a), 1))
    r = ray_extent(org, d, pts, 0.012)
    r = fill_nan(r[None, :])[0]
    r = blur_max(r[None, :], 2)[0] + 0.004
    zz = z - 0.012 * s * np.cos(a) ** 4  # dipping a little at the middle of the brow
    P = org + d * r[:, None]
    P[:, 2] = zz
    W = {"Head": 1.0}
    o.tube(P, d, np.tile([0.0, 0.0, 1.0], (len(a), 1)), 0.009, 0.004, metal, weight=W)
    c = P[len(a) // 2] + np.array([0, -0.006, -0.004])
    o.lathe(c - np.array([0, 0, 0.011]), [0, 0, 1.0], [1.0, 0, 0], [(0.0, 0.0), (0.006, 0.002), (0.010, 0.008),
                                                                     (0.011, 0.012), (0.008, 0.018), (0.0, 0.022)],
            10, bead, W)
    o.box(c + np.array([0, 0.004, 0.0]), (np.array([1.0, 0, 0]), np.array([0, 1.0, 0]), np.array([0, 0, 1.0])),
          (0.03, 0.006, 0.026), metal, W)


def beard_rings(o, B, swatch, ts=(0.42, 0.62, 0.80)):
    """Iron rings clamped round a braided beard (the look's beard capsule region), weighted like the
    beard under them."""
    for rg in B.R["regions"]:
        if "capsule" not in rg or not any("beard" in w for _, w in rg["chain"]):
            continue
        p0, p1, rad = np.array(rg["capsule"][0]), np.array(rg["capsule"][1]), rg["capsule"][2]
        ax = p1 - p0
        L = np.linalg.norm(ax)
        ax /= L
        e1, e2 = vc.ring_frame(ax)
        rel = B.V - p0
        tp = rel @ ax / L
        perp = np.linalg.norm(rel - np.outer(rel @ ax, ax), axis=1)
        sides = 16
        a = np.linspace(0, 2 * math.pi, sides, endpoint=False)
        dirs = np.cos(a)[:, None] * e1 + np.sin(a)[:, None] * e2
        for t in ts:
            near = B.V[(np.abs(tp - t) < 0.04) & (perp < rad + 0.03) & ~B.keep]
            c = p0 + ax * L * t
            if len(near) < 8:
                continue
            # The ring's centre: the braid's own centre at this height.
            c = c + (near.mean(0) - c) - ax * ((near.mean(0) - c) @ ax)
            r = ray_extent(np.tile(c, (sides, 1)), dirs, near, 0.012)
            r = fill_nan(r[None, :])[0]
            r = blur_max(r[None, :], 2, wrap_cols=True)[0] + 0.003
            P = c + dirs * r[:, None]
            o.tube(P, dirs, np.tile(ax, (sides, 1)), 0.032, 0.010, swatch, weight=None, closed=True)
        return


def sleeve(o, B, side, swatch, cuff_swatch, t0=0.34, t1=1.03, offset=0.012, flare=0.014):
    """A leather gauntlet round the forearm (lowerarm bone, t along it), flared at the elbow end."""
    e, w = B.bones[f"lowerarm_{side}"]
    ax = w - e
    L = np.linalg.norm(ax)
    ax /= L
    s1, s2 = vc.ring_frame(ax)
    ts = np.linspace(t0, t1, 8)
    sides = 18
    a = np.linspace(0, 2 * math.pi, sides, endpoint=False)
    dirs = np.cos(a)[:, None] * s1 + np.sin(a)[:, None] * s2
    rel = B.V - e
    tp = rel @ ax / L
    perp = np.linalg.norm(rel - np.outer(rel @ ax, ax), axis=1)
    near = B.V[(tp > t0 - 0.12) & (tp < t1 + 0.04) & (perp < 0.16)]
    org = np.concatenate([np.tile(e + ax * L * t, (sides, 1)) for t in ts])
    r = ray_extent(org, np.tile(dirs, (len(ts), 1)), near, 0.03).reshape(len(ts), sides)
    r = fill_nan(r)
    r = blur_max(r, 2, wrap_cols=True)
    # Never narrower towards the elbow (a stiff cuff), flared at the elbow end.
    for i in range(len(ts) - 2, -1, -1):
        r[i] = np.maximum(r[i], r[i + 1] * 0.97)
    r = r + offset + flare * np.clip(1 - (ts - t0) / 0.18, 0, 1)[:, None]
    P = org.reshape(len(ts), sides, 3) + np.tile(dirs, (len(ts), 1)).reshape(len(ts), sides, 3) * r[..., None]
    Pin = org.reshape(len(ts), sides, 3) + np.tile(dirs, (len(ts), 1)).reshape(len(ts), sides, 3) * (r - 0.008)[..., None]
    uv = np.stack(np.meshgrid(np.linspace(0, 1, sides + 1)[:-1], np.linspace(0, 1, len(ts))), -1)
    o_i = o.grid(P, uv, swatch, None, wrap=True)
    i_i = o.grid(Pin, uv, swatch, None, wrap=True, flip=True)
    ru = [(k / sides, 0.0) for k in range(sides)]
    o.strip(list(i_i[0]), list(o_i[0]), ru, ru, cuff_swatch, closed=True)
    o.strip(list(o_i[-1]), list(i_i[-1]), ru, ru, swatch, closed=True)
    # A cuff band at the elbow end.
    pb = P[1] + np.tile(dirs, (1, 1)) * 0.004
    o.tube(pb, dirs, np.tile(ax, (sides, 1)), 0.03, 0.006, cuff_swatch, weight=None, closed=True, t_scale=2.0)


def flask(o, p, n, up, colour_swatch, cork, size=1.0, weight=None):
    """A small potion bottle standing on `p` (its base), axis `up`."""
    s = size
    prof = [(0.0, 0.0), (0.026 * s, 0.002), (0.034 * s, 0.018 * s), (0.035 * s, 0.045 * s), (0.026 * s, 0.07 * s),
            (0.011 * s, 0.085 * s), (0.010 * s, 0.105 * s), (0.014 * s, 0.11 * s), (0.0, 0.112 * s)]
    o.lathe(p, up, n, prof, 12, colour_swatch, weight)
    o.lathe(np.asarray(p) + np.asarray(up) * 0.104 * s, up, n, [(0.0, 0.0), (0.0095 * s, 0.001), (0.011 * s, 0.024 * s),
                                                              (0.0, 0.026 * s)], 8, cork, weight)


def hammer(o, p, n, t, swatch_wood, swatch_iron, weight, size=1.0):
    """Hammer hanging in a belt loop at p: head above the loop, handle down (a little forward)."""
    up = np.array([0.0, 0.0, 1.0])
    ax = up * 0.97 + n * 0.12
    ax /= np.linalg.norm(ax)
    head_c = p + n * 0.035 + ax * 0.05 * size
    o.box(head_c, (t, n, ax), (0.15 * size, 0.05 * size, 0.055 * size), swatch_iron, weight)
    o.box(head_c + t * 0.085 * size, (t, n, ax), (0.03 * size, 0.06 * size, 0.065 * size), swatch_iron, weight)
    o.lathe(head_c - ax * 0.36 * size, ax, t, [(0.0, 0.0), (0.017, 0.004), (0.016, 0.33 * size), (0.014, 0.345 * size),
                                               (0.0, 0.35 * size)], 8, swatch_wood, weight)
    # The loop it hangs in.
    o.box(p + n * 0.012, (t, n, up), (0.05, 0.016, 0.05), "strap", weight)


def tongs(o, p, n, t, swatch_iron, weight, ingot_swatch=None):
    up = np.array([0.0, 0.0, 1.0])
    base = p + n * 0.03
    for sgn in (-1, 1):
        a = up + t * 0.07 * sgn
        a /= np.linalg.norm(a)
        c = base - a * 0.2 + up * 0.03
        o.box(c, (t, n, a), (0.012, 0.012, 0.44), swatch_iron, weight)
    o.box(base + up * 0.02, (t, n, up), (0.03, 0.03, 0.03), swatch_iron, weight)
    if ingot_swatch:
        o.box(base - up * 0.40, (t, n, up), (0.07, 0.035, 0.035), ingot_swatch, weight)
    o.box(p + n * 0.012, (t, n, up), (0.05, 0.016, 0.05), "strap", weight)


def satchel(o, p, n, t, swatch, flap, strap_sw, fitting, red, blue, cork, weight):
    up = np.array([0.0, 0.0, 1.0])
    c = p + n * 0.055 - up * 0.13
    o.box(c, (t, n, up), (0.22, 0.09, 0.19), swatch, weight)
    o.box(c + up * 0.07 + n * 0.006, (t, n, up), (0.226, 0.1, 0.07), flap, weight)
    o.box(c + up * 0.04 - n * -0.052, (t, n, up), (0.03, 0.006, 0.05), fitting, weight)
    for k, sw in ((-1, red), (1, blue)):
        flask(o, c + t * (0.06 * k) + up * 0.05 - n * 0.01, n, up, sw, cork, size=0.85, weight=weight)
    # Loops onto the belt.
    for k in (-1, 1):
        o.box(p + t * 0.07 * k + n * 0.02 - up * 0.02, (t, n, up), (0.03, 0.012, 0.09), strap_sw, weight)


# ---------------------------------------------------------------------------------------------
# Swatches (texture strip) and painters

SWATCH_ROWS = [  # name, height (px)
    ("apron", 150), ("hood", 110), ("lining", 32), ("trim", 30), ("strap", 36), ("bag", 72), ("flap", 44),
    ("gauntlet", 76), ("cuff", 28), ("wood", 26), ("iron", 26), ("fitting", 26),
    ("potion_red", 56), ("potion_blue", 56), ("cork", 20), ("ember", 20), ("leaf", 24), ("hair", 40),
]


def swatch_rects():
    out, y = {}, 4
    for name, h in SWATCH_ROWS:
        out[name] = (STRIP_X0, y, STRIP_W, h)
        y += h + 6
    assert y <= TEX, y
    return out


def paint_swatches(N, rng):
    """Paints every swatch: base colour (sRGB), height (mm-ish), roughness, metallic, emissive
    (linear). Returns dict name -> (rgb, height, rough, metal, emis)."""
    out = {}
    W = STRIP_W
    pot = N["kind"] == "potion"

    def cloth(h, col, weave=1.0, rough=0.88, hem=None, hem_w=0.16, stitch=None):
        g = fbm(rng, 8, h, W, 4)
        fine = vnoise(rng, 120, h, W)
        yy, xx = np.mgrid[0:h, 0:W]
        thread = (np.sin(xx * 1.7) * np.sin(yy * 1.7)) * 0.5 + 0.5
        t = 0.55 + 0.25 * (g - 0.5) + 0.1 * (fine - 0.5) + 0.06 * (thread - 0.5) * weave
        base = hexc(col)
        rgb = np.clip(base[None, None, :] * (0.78 + 0.45 * t[..., None]), 0, 1)
        hgt = 0.4 * thread * weave + 0.6 * g
        if hem is not None:
            band = (yy > h * (1 - hem_w)).astype(np.float32)
            rgb = rgb * (1 - band[..., None]) + np.clip(hexc(hem) * (0.8 + 0.4 * t[..., None]), 0, 1) * band[..., None]
            hgt = hgt + band * 0.5
            seam = (np.abs(yy - h * (1 - hem_w)) < 1.2).astype(np.float32)
            rgb = rgb * (1 - 0.35 * seam[..., None])
        if stitch is not None:
            st = ((np.abs(yy - h * 0.06) < 1.0) | (np.abs(yy - h * 0.94) < 1.0)) & ((xx // 4) % 2 == 0)
            rgb = np.where(st[..., None], hexc(stitch), rgb)
        return rgb, hgt, np.full((h, W), rough, np.float32), np.zeros((h, W), np.float32), np.zeros((h, W, 3), np.float32)

    def leather(h, col, rough=0.62, rivets=0, stitch=True):
        g = fbm(rng, 6, h, W, 5)
        crack = vnoise(rng, 70, h, W)
        yy, xx = np.mgrid[0:h, 0:W]
        t = 0.5 + 0.35 * (g - 0.5) + 0.12 * (crack - 0.5)
        base = hexc(col)
        rgb = np.clip(base[None, None, :] * (0.7 + 0.6 * t[..., None]), 0, 1)
        # Worn lighter edges near the top / bottom of the piece.
        edge = np.clip(1 - np.minimum(yy, h - 1 - yy) / 6.0, 0, 1)
        rgb = rgb * (1 + 0.25 * edge[..., None])
        hgt = 0.7 * g + 0.25 * crack
        rough_a = rough + 0.15 * (g - 0.5)
        metal = np.zeros((h, W), np.float32)
        if stitch:
            st = ((np.abs(yy - 4) < 0.9) | (np.abs(yy - (h - 5)) < 0.9)) & ((xx // 3) % 2 == 0)
            rgb = np.where(st[..., None], np.clip(hexc(col) * 1.9, 0, 1), rgb)
            hgt = hgt - st * 0.4
        if rivets:
            for k in range(rivets):
                cx, cy = (k + 0.5) * W / rivets, h * 0.5
                m = (xx - cx) ** 2 + (yy - cy) ** 2 < 9
                rgb = np.where(m[..., None], hexc(N["fitting"]), rgb)
                metal = np.where(m, N["fitting_metal"], metal)
                hgt = hgt + m * 1.0
        return np.clip(rgb, 0, 1), hgt, np.clip(rough_a, 0, 1).astype(np.float32), metal, np.zeros((h, W, 3), np.float32)

    def metal_sw(h, col, m, rough=0.4):
        g = fbm(rng, 10, h, W, 4)
        yy, xx = np.mgrid[0:h, 0:W]
        brushed = vnoise(rng, 200, h, W)
        t = 0.5 + 0.3 * (g - 0.5) + 0.15 * (brushed - 0.5)
        rgb = np.clip(hexc(col) * (0.75 + 0.5 * t[..., None]), 0, 1)
        return rgb, 0.3 * g, np.full((h, W), rough, np.float32) + 0.2 * (g - 0.5), np.full((h, W), m, np.float32), \
            np.zeros((h, W, 3), np.float32)

    def wood(h):
        yy, xx = np.mgrid[0:h, 0:W]
        g = fbm(rng, 6, h, W, 3)
        grain = np.sin(yy * 0.9 + 6 * g) * 0.5 + 0.5
        rgb = gmap(0.35 + 0.35 * grain + 0.2 * (g - 0.5), ("#2a1a0e", "#5a3c22", "#8a6440"))
        return rgb, grain * 0.6, np.full((h, W), 0.75, np.float32), np.zeros((h, W), np.float32), np.zeros((h, W, 3), np.float32)

    def potion(h, liquid, glow):
        """Bottle swatch: t (rows) runs up the profile: liquid in the lower 62 %, glass above."""
        yy, xx = np.mgrid[0:h, 0:W]
        t = yy / (h - 1)
        g = fbm(rng, 6, h, W, 3)
        liq = (t < 0.62).astype(np.float32)
        swirl = np.sin(xx * 0.12 + 5 * g) * 0.5 + 0.5
        lc = hexc(liquid) * (0.75 + 0.5 * (0.6 * swirl + 0.4 * g))[..., None]
        gc_ = np.array([0.72, 0.78, 0.76], np.float32) * (0.85 + 0.2 * g)[..., None]
        rgb = lc * liq[..., None] + gc_ * (1 - liq[..., None])
        # A brighter meniscus line, glass highlight streak.
        men = (np.abs(t - 0.62) < 0.03).astype(np.float32)
        rgb = rgb + men[..., None] * 0.2
        hi = ((xx % (W // 3)) < 6).astype(np.float32) * (t > 0.1)
        rgb = rgb + hi[..., None] * 0.18
        rough = np.where(liq > 0, 0.18, 0.08).astype(np.float32)
        emis = srgb_lin(hexc(glow))[None, None, :] * (liq * (0.22 + 0.2 * swirl))[..., None]
        return np.clip(rgb, 0, 1), 0.1 * g, rough, np.zeros((h, W), np.float32), emis.astype(np.float32)

    def ember(h):
        g = fbm(rng, 10, h, W, 4)
        rgb = gmap(g, ("#7a1e06", "#e05a12", "#ffc060"))
        emis = srgb_lin(rgb) * (0.6 + 0.6 * g)[..., None]
        return rgb, 0.2 * g, np.full((h, W), 0.6, np.float32), np.zeros((h, W), np.float32), emis.astype(np.float32)

    rows = dict(SWATCH_ROWS)
    if pot:
        out["apron"] = cloth(rows["apron"], N["apron"], hem=N["hem"], stitch=N["trim"])
        out["hood"] = cloth(rows["hood"], N["hood"], weave=1.2, rough=0.92)
        out["lining"] = cloth(rows["lining"], N["lining"], rough=0.9)
        out["trim"] = cloth(rows["trim"], N["trim"], weave=0.6, rough=0.7)
        out["bag"] = leather(rows["bag"], N["bag"])
        out["flap"] = leather(rows["flap"], N["bag"], rivets=0)
        out["gauntlet"] = leather(rows["gauntlet"], N["leather"])
        out["cuff"] = leather(rows["cuff"], N["leather"])
    else:
        out["apron"] = leather(rows["apron"], N["apron"], rough=0.58, rivets=0)
        out["hood"] = cloth(rows["hood"], N["cap"], weave=1.3, rough=0.92)   # the cap
        out["lining"] = cloth(rows["lining"], N["cap"], rough=0.92)
        out["trim"] = cloth(rows["trim"], N["cap_band"], weave=0.8, rough=0.8)
        out["bag"] = leather(rows["bag"], N["leather"])
        out["flap"] = leather(rows["flap"], N["leather"])
        out["gauntlet"] = leather(rows["gauntlet"], N["gauntlet"], rough=0.66)
        out["cuff"] = leather(rows["cuff"], N["leather"], rivets=8, stitch=False)
    out["strap"] = leather(rows["strap"], N["leather"], rivets=0)
    out["wood"] = wood(rows["wood"])
    out["iron"] = metal_sw(rows["iron"], "#5a5a5e", 0.9, 0.45)
    out["fitting"] = metal_sw(rows["fitting"], N["fitting"], N["fitting_metal"], 0.35)
    out["potion_red"] = potion(rows["potion_red"], "#c21a1a", "#ff3a28")
    out["potion_blue"] = potion(rows["potion_blue"], "#1f4fd0", "#3a7aff")
    out["cork"] = wood(rows["cork"])
    out["ember"] = ember(rows["ember"])

    def leafy(h, col):
        g = fbm(rng, 10, h, W, 3)
        yy, xx = np.mgrid[0:h, 0:W]
        vein = (np.abs(yy - h / 2) < 1.0).astype(np.float32)
        rgb = np.clip(hexc(col) * (0.75 + 0.5 * g[..., None]) * (1 + 0.25 * vein[..., None]), 0, 1)
        return rgb, 0.3 * g, np.full((h, W), 0.7, np.float32), np.zeros((h, W), np.float32), np.zeros((h, W, 3), np.float32)

    def hairy(h, col):
        """Hair strands run along the tube (s = columns): streaks vary across the rows."""
        g = fbm(rng, 6, h, W, 3)
        streak = vnoise(rng, 90, h, W)[:, :1] * np.ones((1, W), np.float32)
        streak2 = vnoise(rng, 40, h, W)[:, :1] * np.ones((1, W), np.float32)
        t = 0.45 + 0.4 * (streak - 0.5) + 0.25 * (streak2 - 0.5) + 0.1 * (g - 0.5)
        rgb = np.clip(hexc(col) * (0.6 + 0.8 * t[..., None]), 0, 1)
        return rgb, 0.6 * streak + 0.2 * g, np.full((h, W), 0.7, np.float32), np.zeros((h, W), np.float32), \
            np.zeros((h, W, 3), np.float32)
    out["leaf"] = leafy(rows["leaf"], N.get("leaf", "#55703a"))
    out["hair"] = hairy(rows["hair"], N.get("hair", "#2a1d14"))
    return out


# ---------------------------------------------------------------------------------------------
# Body recolour (the source maps at SRC_SIZE, in UV space; head, hands and braids/beards kept)

def protected_mask(body, B, size):
    """UV-space mask of faces that keep their colours: the head (above the neck), the hands
    (beyond the wrists) and braids / beards."""
    me = body.data
    V = B.V
    wr = B.wrist
    el = np.array(B.R["fit"]["elbow"])
    fa = (wr - el) / np.linalg.norm(wr - el)
    m = np.abs(V[:, 0])
    mirrored = np.stack([m, V[:, 1], V[:, 2]], 1)
    hand = (((mirrored - wr) @ fa) > -0.01) & (np.linalg.norm(mirrored - wr, axis=1) < 0.25)
    head = V[:, 2] > B.neck_z + 0.01
    keep = hand | head | ~B.keep
    faces = [p for p in me.polygons if all(keep[i] for i in p.vertices)]
    return gp.rasterize_faces(body, faces, size)


def recolor(c, orm, prot, N, rng):
    """Town palette over the body: luminance through gradient maps chosen per texel class (metal,
    saturated cloth, plain cloth / leather); skin keeps its colour (Kargan skin by hue)."""
    P = N["body"]
    hue, sat, mx = hsv(c)
    lum = c @ np.array([0.2126, 0.7152, 0.0722], np.float32)
    metal = np.clip((orm[..., 2] - 0.35) / 0.3, 0, 1)
    skin = np.zeros_like(lum)
    if N["base"].startswith("kargan"):
        skin = ((hue > 55) & (hue < 160) & (sat > 0.07) & (metal < 0.5)).astype(np.float32)
    sat_cls = np.clip((sat - 0.22) / 0.15, 0, 1) * (1 - metal)
    t = np.clip(lum * 1.55, 0, 1)
    g = fbm(rng, 24, c.shape[0], c.shape[1], 3)
    t = np.clip(t + 0.06 * (g - 0.5), 0, 1)
    prim = gmap(t, P["primary"])
    sec = gmap(t, P["secondary"])
    met = gmap(np.clip(lum * 1.3, 0, 1), P["metal"])
    out = prim * sat_cls[..., None] + sec * ((1 - sat_cls) * (1 - metal))[..., None] + met * metal[..., None]
    keep = np.clip(prot.astype(np.float32) + skin, 0, 1)[..., None]
    out = c * keep + out * (1 - keep)
    # Converted metal becomes dyed leather / cloth (not metallic) unless the town keeps iron.
    mk = P["metal_keep"]
    orm2 = orm.copy()
    conv = metal * (1 - keep[..., 0])
    orm2[..., 2] = orm[..., 2] * (1 - conv) + orm[..., 2] * mk * conv
    orm2[..., 1] = orm[..., 1] * (1 - conv) + (0.55 + 0.25 * (1 - mk)) * conv
    return np.clip(out, 0, 1).astype(np.float32), orm2


def squeeze(a, width):
    """(n, n, c) -> (n, width, c): columns resampled by area (box filter over fractional texels)."""
    n = a.shape[1]
    edges = np.linspace(0, n, width + 1)
    cs = np.concatenate([np.zeros_like(a[:, :1]), np.cumsum(a, axis=1)], axis=1)

    def at(x):
        i = np.floor(x).astype(int)
        f = (x - i)[None, :, None]
        i1 = np.minimum(i + 1, n)
        return cs[:, i] + (cs[:, i1] - cs[:, i]) * f
    return (at(edges[1:]) - at(edges[:-1])) / (edges[1:] - edges[:-1])[None, :, None]


def compose_maps(N, c, orm, nrm, rng, used):
    """Atlas: the body in columns 0..BODY_W-1 (with a gutter), the swatches in the strip."""
    k = c.shape[0] // TEX
    lin = srgb_lin(c.astype(np.float64))
    lin = lin.reshape(TEX, k, TEX, k, 3).mean(axis=(1, 3))
    col = np.zeros((TEX, TEX, 3))
    col[:, :BODY_W] = squeeze(lin, BODY_W)
    nn = nrm.astype(np.float64) * 2 - 1
    nn = nn.reshape(TEX, k, TEX, k, 3).mean(axis=(1, 3))
    nb = np.zeros((TEX, TEX, 3))
    nb[:] = (0, 0, 1)
    nb[:, :BODY_W] = squeeze(nn, BODY_W)
    o = orm.astype(np.float64).reshape(TEX, k, TEX, k, 3).mean(axis=(1, 3))
    ob = np.zeros((TEX, TEX, 3))
    ob[:] = (1.0, 0.8, 0.0)
    ob[:, :BODY_W] = squeeze(o, BODY_W)
    em = np.zeros((TEX, TEX, 3))
    # Gutter: replicate the last body column.
    for a in (col, nb, ob):
        a[:, BODY_W:STRIP_X0] = a[:, BODY_W - 1:BODY_W]
    rects = swatch_rects()
    sw = paint_swatches(N, rng)
    for name, (x, y, w, h) in rects.items():
        rgb, hgt, rough, metal, emis = sw[name]
        col[y:y + h, x:x + w] = srgb_lin(rgb)
        gy, gx = np.gradient(hgt.astype(np.float64))
        nmap = np.stack([-gx * 0.6, gy * 0.6, np.ones_like(gx)], -1)
        nmap /= np.linalg.norm(nmap, axis=-1, keepdims=True)
        nb[y:y + h, x:x + w] = nmap
        ob[y:y + h, x:x + w, 0] = 1.0
        ob[y:y + h, x:x + w, 1] = np.clip(rough, 0.05, 1)
        ob[y:y + h, x:x + w, 2] = metal
        em[y:y + h, x:x + w] = emis if name in used else 0.0
        # Bleed 3 px around the swatch so mips and filtering keep its colour.
        for a, src in ((col, col[y:y + h, x:x + w]), (nb, nb[y:y + h, x:x + w]), (ob, ob[y:y + h, x:x + w]),
                       (em, em[y:y + h, x:x + w])):
            a[max(y - 3, 0):y, x:x + w] = src[:1]
            a[y + h:y + h + 3, x:x + w] = src[-1:]
            a[max(y - 3, 0):y + h + 3, max(x - 3, 0):x] = a[max(y - 3, 0):y + h + 3, x:x + 1]
            a[max(y - 3, 0):y + h + 3, x + w:x + w + 3] = a[max(y - 3, 0):y + h + 3, x + w - 1:x + w]
    nb /= np.maximum(np.linalg.norm(nb, axis=-1, keepdims=True), 1e-8)
    key = N["key"]
    paths = dict(color=os.path.join(OUT_DIR, f"{key}.png"), normal=os.path.join(OUT_DIR, f"{key}_n.png"),
                 orm=os.path.join(OUT_DIR, f"{key}_orm.png"))
    vc.write_png(paths["color"], vc.to_u8(lin_srgb(col)), optimize=True)
    vc.write_png(paths["normal"], vc.to_u8(nb * 0.5 + 0.5), optimize=True)
    vc.write_png(paths["orm"], vc.to_u8(ob), optimize=True)
    em_path = os.path.join(OUT_DIR, f"{key}_e.png")
    if em.max() > 0.01:
        paths["emissive"] = em_path
        vc.write_png(paths["emissive"], vc.to_u8(lin_srgb(em)), optimize=True)
    elif os.path.exists(em_path):
        os.remove(em_path)  # nothing glows on this merchant (any more)
    return paths, rects, float((vc.to_u8(lin_srgb(em))[..., 0] > 60).mean())


# ---------------------------------------------------------------------------------------------
# Build

def body_weights(body):
    names = [g.name for g in body.vertex_groups]
    W = np.zeros((len(body.data.vertices), len(names)), np.float32)
    for v in body.data.vertices:
        for g in v.groups:
            W[v.index, g.group] = g.weight
    return names, W


def outfit_for(N, B):
    """Models the outfit around the rest-pose body; returns the Outfit."""
    o = Outfit()
    s = B.s
    sh_z, belt_z = B.sh[2], B.belt_z
    knee_z = B.knee[2]
    thw = B.torso_hw
    if N["kind"] == "potion":
        hood_pts = hood(o, B, "hood", "lining", "trim")
        cowl_top = B.chin_z + 0.01
        cowl_bot = sh_z - 0.10 * s
        if N.get("cowl") == "drape":
            cowl_pts = drape_cowl(o, B, "hood", hood_pts, hem="trim")
        else:
            cowl_pts = cowl(o, B, "hood", hood_pts, cowl_top, cowl_bot, hem="trim")
        feats = N.get("features", ())
        if "sprig" in feats:
            herb_sprig(o, B, hood_pts, "leaf", "trim")
        if "pipe" in feats:
            pipe(o, B, "wood", "fitting")
        if "brooch" in feats:
            brooch(o, B, cowl_pts, "fitting", "potion_blue")
        if "circlet" in feats:
            circlet(o, B, "iron", "ember")
        top = sh_z - 0.10 * s
        P, Nn, zs = apron(o, B, top, 0.45 * thw, 0.72 * thw, knee_z - 0.22 * s, 0.92 * thw, 0.03, 0.007, "apron")
        ap_pts = P.reshape(-1, 3)
        at = belt(o, B, belt_z, 0.045, "strap", extra_pts=ap_pts, offset=0.010)
        # Three flasks on the belt, front right; the satchel on the left hip.
        for k, (ang, sw) in enumerate(((-22, "potion_red"), (-40, "potion_blue"), (-58, "potion_red"))):
            p, n = at(math.radians(ang))
            t = np.cross([0, 0, 1.0], n)
            base = p + n * 0.038 - np.array([0, 0, 0.115])
            flask(o, base, n, np.array([0, 0, 1.0]), sw, "cork", size=1.0 if k != 1 else 0.9, weight={"pelvis": 1.0})
            o.box(p + n * 0.02 - np.array([0, 0, 0.035]), (t, n, np.array([0, 0, 1.0])), (0.05, 0.012, 0.022), "strap",
                  {"pelvis": 1.0})
        p, n = at(math.radians(78))
        t = np.cross([0, 0, 1.0], n)
        satchel(o, p, n, t, "bag", "flap", "strap", "fitting", "potion_red", "potion_blue", "cork", {"pelvis": 1.0})
        # Shoulder strap of the satchel: from the bag up across the chest under the cowl, and down
        # the back.
        c = np.array([0.0, 0.0, 0.0])
        keys = []
        for (ang, z) in ((70, belt_z + 0.06), (35, belt_z + 0.15), (0, (belt_z + sh_z) / 2 + 0.04),
                         (-30, sh_z - 0.08 * s)):
            a = math.radians(ang)
            keys.append(((0.0, 0.0, z), (math.sin(a), -math.cos(a), 0.0)))
        # (The draped capelet hangs lower: the strap's ends go under it instead of over its hem.)
        drape = N.get("cowl") == "drape"
        strap(o, B, keys, 0.04, 0.006, 0.006, "strap", extra_pts=ap_pts if drape else np.concatenate([ap_pts, cowl_pts]),
              xlim=(-B.torso_hw - 0.05, B.torso_hw + 0.05))
        keys = []
        for (ang, z) in ((105, belt_z + 0.06), (145, belt_z + 0.15), (180, (belt_z + sh_z) / 2 + 0.04),
                         (210, sh_z - 0.08 * s)):
            a = math.radians(ang)
            keys.append(((0.0, 0.0, z), (math.sin(a), -math.cos(a), 0.0)))
        strap(o, B, keys, 0.04, 0.006, 0.006, "strap", extra_pts=None if drape else cowl_pts,
              xlim=(-B.torso_hw - 0.05, B.torso_hw + 0.05))
    else:
        cap(o, B, "hood", "trim", False, N["town"] == "kargan", "fitting", style=N.get("cap_style"))
        feats = N.get("features", ())
        if "moustache" in feats:
            moustache(o, B, "hair")
        if "beard_rings" in feats:
            beard_rings(o, B, "fitting")
        top = sh_z - 0.02 * s
        P, Nn, zs = apron(o, B, top, 0.55 * thw, 0.88 * thw, knee_z + 0.04 * s, 1.0 * thw, 0.032, 0.012, "apron")
        ap_pts = P.reshape(-1, 3)
        # Rivets at the bib's top corners and a pocket on the skirt.
        for sx in (-1, 1):
            i = 1
            cpt = P[i, 1 if sx < 0 else -2] + Nn[i, 1 if sx < 0 else -2] * 0.004
            nrm = Nn[i, 1 if sx < 0 else -2]
            up = np.array([0, 0, 1.0])
            side = np.cross(up, nrm)
            o.box(cpt, (side, up, nrm), (0.02, 0.02, 0.012), "fitting", None)
        r0 = int(len(zs) * 0.5)
        pc = P[r0:r0 + 5, 4:11].mean(axis=(0, 1)) + Nn[r0 + 2, 7] * 0.008
        up = np.array([0, 0, 1.0])
        nrm = Nn[r0 + 2, 7]
        side = np.cross(up, nrm)
        o.box(pc, (side, up, nrm), (0.2 * s, 0.13 * s, 0.012), "flap", apron_weight(B)(pc[2]))
        # Neck straps from the bib corners over the shoulders down the back to the belt.
        for sx in (-1, 1):
            x = sx * 0.13 * s
            keys = []
            for ang in (-35, 0, 45, 90, 135, 180):
                a = math.radians(ang)
                keys.append(((x * 0.8, 0.0, sh_z - 0.14 * s), (sx * 0.12, -math.cos(a), math.sin(a))))
            keys.append(((x * 0.6, 0.0, (sh_z + belt_z) / 2), (sx * 0.08, 1.0, -0.15)))
            keys.append(((x * 0.5, 0.0, belt_z + 0.03), (0.0, 1.0, 0.0)))
            pts_x = (x - 0.08, x + 0.08) if sx > 0 else (x - 0.08, x + 0.08)
            strap(o, B, keys, 0.035, 0.006, 0.008, "strap", extra_pts=ap_pts, xlim=pts_x)
        at = belt(o, B, belt_z, 0.05, "strap", extra_pts=ap_pts, offset=0.012)
        p, n = at(math.radians(-38))
        t = np.cross([0, 0, 1.0], n)
        hammer(o, p, n, t, "wood", "iron", {"pelvis": 1.0}, size=1.0 * s)
        p, n = at(math.radians(42))
        t = np.cross([0, 0, 1.0], n)
        tongs(o, p, n, t, "iron", {"pelvis": 1.0}, "ember" if N.get("ingot") else None)
        for side in ("l", "r"):
            sleeve(o, B, side, "gauntlet", "cuff")
    return o


def outfit_object(o, N, B, rects, body_names, body_W, body):
    """Mesh object of the outfit: UVs into the swatch strip, weights (rules or nearest body), split
    normals kept in a corner attribute for the join."""
    me = bpy.data.meshes.new("outfit")
    me.from_pydata(o.V, [], o.F)
    me.validate(clean_customdata=False)
    uvl = me.uv_layers.new(name="UVMap")
    li = 0
    uv_flat = []
    for fi, poly in enumerate(me.polygons):
        x, y, w, h = rects[o.FS[fi]]
        for k in range(poly.loop_total):
            s_, t_ = o.FUV[fi][k]
            s_ = min(max(s_, 0.0), 1.0)
            t_ = min(max(t_, 0.0), 1.0)
            u = (x + 1.5 + s_ * (w - 3)) / TEX
            v = 1.0 - (y + 1.5 + t_ * (h - 3)) / TEX
            uv_flat += [u, v]
    uvl.data.foreach_set("uv", uv_flat)
    obj = vc.link(bpy.data.objects.new("outfit", me))
    # Weights.
    kd = KDTree(len(B.V))
    for i, p in enumerate(B.V):
        kd.insert(p, i)
    kd.balance()
    groups = {n: obj.vertex_groups.new(name=n) for n in body_names}
    for vi, rule in enumerate(o.W):
        p = me.vertices[vi].co
        if rule is None:
            near = kd.find_n(p, 6)
            acc = np.zeros(len(body_names))
            tot = 0.0
            for co, idx, dist in near:
                wgt = 1.0 / max(dist, 0.004) ** 2
                acc += body_W[idx] * wgt
                tot += wgt
            acc /= max(tot, 1e-9)
            order = np.argsort(-acc)[:4]
            top = acc[order]
            top = top / max(top.sum(), 1e-9)
            w = {body_names[j]: float(x) for j, x in zip(order, top) if x > 1e-4}
        else:
            w = rule
        tot = sum(w.values())
        for n, x in w.items():
            groups[n].add([vi], x / tot, 'REPLACE')
    # Clean-up: weld, consistent outward winding per closed shell, triangulate, drop slivers.
    bm = bmesh.new()
    bm.from_mesh(me)
    bmesh.ops.remove_doubles(bm, verts=bm.verts, dist=1e-6)
    bmesh.ops.recalc_face_normals(bm, faces=bm.faces)
    bmesh.ops.triangulate(bm, faces=bm.faces)
    small = [f for f in bm.faces if f.calc_area() < 4e-7]
    bmesh.ops.delete(bm, geom=small, context='FACES')
    loose = [v for v in bm.verts if not v.link_faces]
    bmesh.ops.delete(bm, geom=loose, context='VERTS')
    bm.to_mesh(me)
    bm.free()
    me.shade_smooth()
    me.set_sharp_from_angle(angle=math.radians(38.0))
    me.update()
    return obj


def store_corner_normals(obj):
    me = obj.data
    n = np.empty(len(me.loops) * 3, np.float32)
    me.corner_normals.foreach_get("vector", n)
    a = me.attributes.new("keep_n", 'FLOAT_VECTOR', 'CORNER')
    a.data.foreach_set("vector", n)


def build_npc(N, preview):
    key = N["key"]
    R = dict(gc.LOOK[N["base"]], max_tris=MAX_TRIS)
    vc.reset_scene()
    s = gc.look_scale(R)
    arm, actions, settings = vo_ual.load_skeleton(s, lean=R["lean"], fit=R["fit"], helpers=gc.default_helpers(R),
                                                  sit=R["sit"])
    gc.reset_pose(arm)
    body, (src_tris, src_verts) = gc.load_source(R)
    imgs = gc.source_images(body)
    metal = gc.vertex_metal(body, imgs["orm"])

    def get(img):
        a = np.ascontiguousarray(gc.image_array(img)[:, :, :3], dtype=np.float32)
        f = a.shape[0] // SRC_SIZE
        return a.reshape(SRC_SIZE, f, SRC_SIZE, f, 3).mean(axis=(1, 3)).astype(np.float32) if f > 1 else a
    c, orm, nrm = get(imgs["color"]), get(imgs["orm"]), get(imgs["normal"])
    for img in list(bpy.data.images):
        bpy.data.images.remove(img)
    missing, bind_scale, summary = gc.rig(body, arm, R, metal)
    gc.reset_pose(arm)
    B = Body(body, arm, R)
    rng = np.random.default_rng(sum(ord(ch) * (k + 1) for k, ch in enumerate(key)))
    prot = protected_mask(body, B, SRC_SIZE)
    c2, orm2 = recolor(c, orm, prot, N, rng)
    body_tris = vc.triangle_count(body)
    # Body UVs into the left 3/4 of the atlas.
    me = body.data
    uv = np.empty(len(me.loops) * 2, np.float32)
    me.uv_layers.active.data.foreach_get("uv", uv)
    uv = uv.reshape(-1, 2)
    print(f"STATS {key} source UV range u {uv[:, 0].min():.4f}..{uv[:, 0].max():.4f} v {uv[:, 1].min():.4f}..{uv[:, 1].max():.4f}")
    uv[:, 0] = np.clip(uv[:, 0], 0.0, 1.0) * (BODY_W / TEX)
    me.uv_layers.active.data.foreach_set("uv", uv.ravel())
    # Outfit.
    o = outfit_for(N, B)
    rects = swatch_rects()
    names, W = body_weights(body)
    out = outfit_object(o, N, B, rects, names, W, body)
    paths, rects, em_share = compose_maps(N, c2, orm2, nrm, rng, set(o.FS))
    del c, orm, nrm, c2, orm2
    outfit_tris = vc.triangle_count(out)
    # Join (body first), restoring every corner's normal (body: the source normals; outfit: its own).
    store_corner_normals(body)
    store_corner_normals(out)
    if out.data.uv_layers.active.name != me.uv_layers.active.name:
        out.data.uv_layers.active.name = me.uv_layers.active.name
    bpy.ops.object.select_all(action='DESELECT')
    out.select_set(True)
    body.select_set(True)
    bpy.context.view_layer.objects.active = body
    bpy.ops.object.join()
    me = body.data
    kn = np.empty(len(me.loops) * 3, np.float32)
    me.attributes["keep_n"].data.foreach_get("vector", kn)
    me.attributes.remove(me.attributes["keep_n"])
    me.normals_split_custom_set(kn.reshape(-1, 3).tolist())
    mat = gp.build_material(f"{key}_skin", paths, 1.0 if "emissive" in paths else 0.0)
    me.materials.clear()
    me.materials.append(mat)
    me.polygons.foreach_set("material_index", [0] * len(me.polygons))
    for m in list(bpy.data.materials):
        if m is not mat and m.users == 0:
            bpy.data.materials.remove(m)
    for clip, act in list(actions.items()):
        if clip not in NPC_CLIP_NAMES:
            bpy.data.actions.remove(act)
            del actions[clip]
    gc.reset_pose(arm)
    vc.ensure_dir(OUT_DIR)
    vc.export_glb(os.path.join(OUT_DIR, f"{key}.glb"), [arm, body], textures=True)
    with open(os.path.join(OUT_DIR, f"{key}.anims.toml"), "w", newline="\n") as f:
        f.write(f"# Generated by tools/blender/gen_npcs.py ({key}). Clip settings for assetc.\n")
        f.write("# Clips: Quaternius Universal Animation Library (CC0, see tools/blender/vo_ual.py), fitted to\n")
        f.write(f"# the {N['base']} look this merchant is built on. Merchants stand still: move_speed 0.\n")
        for clip, src, loop in NPC_CLIPS:
            lp, sp = settings[clip]
            f.write(f'\n[[clip]]\nname = "{clip}"\nloop = {"true" if lp else "false"}\nmove_speed = {sp}\n')
    V = np.array([v.co for v in me.vertices])
    meta = dict(base=N["base"], kind=N["kind"], town=N["town"], height=round(float(V[:, 2].max()), 3),
                bottom=round(float(V[:, 2].min()), 3), tris=vc.triangle_count(body), body_tris=body_tris,
                outfit_tris=outfit_tris, verts=len(V), bones=len(arm.data.bones), texture=TEX,
                emissive=bool("emissive" in paths), emissive_share=round(em_share, 4),
                clips=[c[0] for c in NPC_CLIPS], skeleton_scale=round(s, 4), unweighted=missing,
                bind_scale=str(bind_scale))
    print("STATS", key, "base", N["base"], "tris", meta["tris"], "(body", body_tris, "outfit", outfit_tris, ") verts",
          len(V), "height", meta["height"], "bottom", meta["bottom"], "emissive_share", meta["emissive_share"])
    if preview:
        render_previews(N, R, arm, actions)
    return meta


def render_previews(N, R, arm, actions):
    key, H = N["key"], R["height"]
    out_dir = vc.ensure_dir(PREVIEW_DIR)
    sc = bpy.context.scene
    cam = gc.setup_studio((384, 512))
    ground_mat = vc.material("preview_ground", "#77746d")
    bpy.ops.mesh.primitive_plane_add(size=12, location=(0, 0, 0))
    bpy.context.active_object.data.materials.append(ground_mat)
    dist = 4.8 * H / 1.85
    tgt = (0, 0, H * 0.52)
    made = set()
    vo_ual.use_action(arm, actions["idle"])
    sc.frame_set(0)
    for vname, loc in {"front": (0, -dist, H * 0.55), "threequarter": (dist * 0.7, -dist * 0.7, H * 0.7),
                       "back": (0, dist, H * 0.6)}.items():
        vc.aim_camera(cam, loc, tgt, lens=58)
        vc.render_to(os.path.join(out_dir, f"{key}_{vname}.png"), stable=True)
        made.add(f"{key}_{vname}.png")
    sc.render.resolution_x, sc.render.resolution_y = 192, 256
    tiles = []
    for clip in NPC_CLIP_NAMES:
        act = actions[clip]
        vo_ual.use_action(arm, act)
        frames = int(round(act.frame_range[1]))
        for k in range(6):
            sc.frame_set(round(k * frames / 5))
            vc.aim_camera(cam, (dist * 0.72, -dist * 0.68, H * 0.62), (0, 0, H * 0.48), lens=64)
            tiles.append(vc.render_to(os.path.join(out_dir, f"_tmp_{key}_{clip}{k}.png")))
    vc.compose_sheet(tiles, os.path.join(out_dir, f"{key}_clips_sheet.png"), 6, stable=True)
    made.add(f"{key}_clips_sheet.png")
    vc.remove_files(tiles)
    gc.reset_pose(arm)
    sc.frame_set(0)
    vc.remove_files(sorted(os.path.join(out_dir, f) for f in os.listdir(out_dir)
                           if f.startswith(f"{key}_") and f not in made))


def main():
    args = vc.script_args()
    names = [N["key"] for N in NPCS]
    if "--only" in args:
        names = args[args.index("--only") + 1].split(",")
    preview = "--no-preview" not in args
    meta_all = {}
    if os.path.exists(META_PATH):
        with open(META_PATH, "r", encoding="utf-8") as f:
            meta_all = json.load(f)
    for n in names:
        meta_all[n] = build_npc(NPC[n], preview)
    meta_all = {k: meta_all[k] for k in NPC if k in meta_all}
    vc.ensure_dir(OUT_DIR)
    with open(META_PATH, "w", encoding="utf-8", newline="\n") as f:
        json.dump(meta_all, f, indent=1, sort_keys=True)
        f.write("\n")


if __name__ == "__main__":
    main()
