"""Silhouette pieces for the eight M15 monsters (gen_monsters.py `shapes`), on top of monster_shapes.py:
new swatch painters (registered in monster_shapes.EXTRA_SWATCHES), extra primitives (box, flat prism
from a 2D outline, closed skirt, tube along a polyline) and one parts function per monster.

  Tanyeri (burnt straw):        scorched_reaper (straw hat, arm scythe, straw back cape)
                                threshing_brute (crossed sheaves on the back, rope, bound fists)
  Demirağız (frost and iron):   glacier_bellowsman (hunched, frosted bellows on the back)
                                anvil_breaker (hammer-head right hand, iron slabs, ice ridge)
  Ortaocak (ash, slag, ember):  soot_stalker (crouched, snout, claws, quills, tail)
                                slag_bulwark (walking wall of slag slabs with glowing seams)
                                ember_warlock (hooded robe, ember core, cinders round the hands)
                                hearth_lord (boss: burning hearth-grate crown, chest grate, flues)

Every piece is opaque (no alpha): the monsters draw in the opaque mesh pass. Skin rules (fix1 lessons):
pieces near a limb follow that limb rigidly (one bone); skirts and capes blend the thighs by side;
torso pieces only take torso bones. Imported by gen_monsters.py (no main). Deterministic.
"""

import math

import numpy as np

import monster_shapes as ms
from monster_shapes import Parts, TORSO, _fbm, _frame, _hex, _ramp, _unit

LEGS_UP = ("pelvis", "thigh_l", "thigh_r")


# ---------------------------------------------------------------------------------------------
# Swatches: painter(h, w, rng, t, s) -> (colour sRGB, height, roughness, metallic, emissive sRGB).
# t runs down the rows (base -> tip / top -> hem); prisms put their cap in t 0..0.85 and their edge
# band (the slab's thickness) in t 0.95..0.99, so the last rows are the "edge" colour.

def _edge(t):
    return np.clip((t - 0.9) / 0.03, 0, 1)


def sw_char_straw(h, w, rng, t, s):
    """Straw scorched in the field fire: smoky brown fibres, black char toward the tips, a thin gold
    ember line where it still smoulders."""
    fib = _fbm(rng, 4, 40, h, w, 2)
    streak = np.clip(np.sin(s * 70.0 + fib * 9.0) * 0.5 + 0.5, 0, 1)
    lum = np.clip(0.3 + 0.4 * streak + 0.25 * (fib - 0.5) - 0.2 * t, 0, 1)
    col = _ramp(lum, [(0.0, "#140e09"), (0.45, "#3e2a17"), (0.8, "#6e5230"), (1.0, "#93744a")])
    char = np.clip((t - 0.55) / 0.3, 0, 1) * np.clip((fib + 0.3), 0, 1)
    col = col * (1 - char[..., None]) + _hex("#0d0a08") * char[..., None]
    glow = np.clip(1.0 - np.abs(t + 0.25 * (fib - 0.5) - 0.9) * 40.0, 0, 1) * np.clip((fib - 0.4) / 0.15, 0, 1)
    col = col * (1 - glow[..., None]) + _hex("#ffb23a") * glow[..., None]
    em = _hex("#ffa030")[None, None] * glow[..., None]
    return col, streak * 0.6 + fib * 0.4, np.full((h, w), 0.9, np.float32), np.zeros((h, w), np.float32), em


def sw_hat_straw(h, w, rng, t, s):
    """The reaper's hat: woven dry straw (lighter than the char), scorched toward the crown, a gold
    ember ring smouldering near the top."""
    fib = _fbm(rng, 4, 40, h, w, 2)
    weave = np.clip(np.sin(s * 90.0 + fib * 6.0) * np.sin(t * 60.0) * 0.5 + 0.5, 0, 1)
    lum = np.clip(0.45 + 0.35 * weave + 0.2 * (fib - 0.5) - 0.15 * t, 0, 1)
    col = _ramp(lum, [(0.0, "#2e2012"), (0.45, "#6e522d"), (0.8, "#a48452"), (1.0, "#c4a670")])
    char = np.clip((t - 0.62) / 0.25, 0, 1) * np.clip(fib + 0.4, 0, 1)
    col = col * (1 - char[..., None]) + _hex("#120d09") * char[..., None]
    glow = np.clip(1.0 - np.abs(t + 0.2 * (fib - 0.5) - 0.86) * 40.0, 0, 1) * np.clip((fib - 0.4) / 0.15, 0, 1)
    col = col * (1 - glow[..., None]) + _hex("#ffb23a") * glow[..., None]
    em = _hex("#ffa030")[None, None] * glow[..., None]
    return col, weave * 0.6 + fib * 0.4, np.full((h, w), 0.9, np.float32), np.zeros((h, w), np.float32), em


def sw_scythe(h, w, rng, t, s):
    n1 = _fbm(rng, 6, 10, h, w, 4)
    rust = np.clip((_fbm(rng, 4, 6, h, w, 3) - 0.52) / 0.1, 0, 1)
    col = _ramp(n1, [(0, "#0e0d0c"), (0.5, "#262321"), (1, "#3d3833")])
    col = col * (1 - 0.6 * rust[..., None]) + _hex("#4d2c14") * 0.6 * rust[..., None]
    e = _edge(t)
    col = col * (1 - e[..., None]) + _hex("#c9c4b8") * e[..., None]       # honed edge
    ember = np.clip((n1 - 0.72) / 0.06, 0, 1) * (1 - e) * (1 - rust)
    col = col * (1 - ember[..., None]) + _hex("#ffb23a") * ember[..., None]
    em = _hex("#ffa030")[None, None] * (ember * 0.9)[..., None]
    rough = (0.55 - 0.35 * e + 0.3 * rust).astype(np.float32)
    metal = (0.9 * (1 - rust)).astype(np.float32)
    return col, n1 * 0.6, rough, metal, em


def sw_rope(h, w, rng, t, s):
    fib = _fbm(rng, 6, 6, h, w, 3)
    braid = np.clip(np.sin((s * 6 + t * 22) * math.pi) * 0.5 + 0.5, 0, 1)
    soot = np.clip((_fbm(rng, 4, 4, h, w, 3) - 0.45) / 0.2, 0, 1)
    col = _ramp(np.clip(0.3 + 0.45 * braid + 0.3 * (fib - 0.5), 0, 1), [(0, "#2d2216"), (0.6, "#6e5634"), (1, "#9a7f50")])
    col = col * (1 - 0.6 * soot[..., None]) + _hex("#1a140f") * 0.6 * soot[..., None]
    return col, braid, np.full((h, w), 0.92, np.float32), np.zeros((h, w), np.float32), np.zeros((h, w, 3), np.float32)


def sw_chalk(h, w, rng, t, s):
    """Threshing stone: chalky ochre stone, soot from the edges."""
    n1 = _fbm(rng, 8, 8, h, w, 4)
    col = _ramp(n1, [(0, "#5a4a38"), (0.5, "#9c8a70"), (1, "#cfc3aa")])
    soot = np.clip((_fbm(rng, 5, 5, h, w, 3) - 0.5) / 0.15, 0, 1)
    col = col * (1 - 0.7 * soot[..., None]) + _hex("#1e1913") * 0.7 * soot[..., None]
    return col, n1, np.full((h, w), 0.9, np.float32), np.zeros((h, w), np.float32), np.zeros((h, w, 3), np.float32)


def sw_frost_wood(h, w, rng, t, s):
    grain = _fbm(rng, 3, 26, h, w, 3)
    rime = np.clip((_fbm(rng, 18, 14, h, w, 3) - 0.55) / 0.14, 0, 1) * np.clip((grain - 0.35) / 0.2, 0, 1)
    col = _ramp(grain, [(0, "#2a2018"), (0.6, "#4e3e2e"), (1, "#6d5a44")])
    col = col * (1 - 0.45 * rime[..., None]) + _hex("#d6e4ee") * 0.45 * rime[..., None]
    e = _edge(t)
    col = col * (1 - 0.6 * e[..., None]) + _hex("#e8f2f8") * 0.6 * e[..., None]
    return col, grain * 0.6 + rime * 0.3, (0.85 - 0.4 * rime).astype(np.float32), np.zeros((h, w), np.float32), \
        np.zeros((h, w, 3), np.float32)


def sw_frost_leather(h, w, rng, t, s):
    pleat = np.abs(((t * 9.0) % 1.0) - 0.5) * 2.0
    n1 = _fbm(rng, 6, 6, h, w, 4)
    col = _ramp(np.clip(0.3 + 0.5 * pleat + 0.3 * (n1 - 0.5), 0, 1), [(0, "#1a120c"), (0.6, "#3e2b1d"), (1, "#5e4530")])
    rime = np.clip((pleat - 0.75) / 0.2, 0, 1) * np.clip((n1 - 0.3) / 0.3, 0, 1)
    col = col * (1 - 0.8 * rime[..., None]) + _hex("#dfe9f0") * 0.8 * rime[..., None]
    return col, pleat, (0.75 - 0.3 * rime).astype(np.float32), np.zeros((h, w), np.float32), np.zeros((h, w, 3), np.float32)


def sw_frost_iron(h, w, rng, t, s):
    n1 = _fbm(rng, 8, 8, h, w, 4)
    frost = np.clip((_fbm(rng, 16, 16, h, w, 3) - 0.55) / 0.15, 0, 1)
    col = _ramp(n1, [(0, "#16191d"), (0.5, "#323840"), (1, "#566069")])
    col = col * (1 - 0.45 * frost[..., None]) + _hex("#d8e6f0") * 0.45 * frost[..., None]
    e = _edge(t)
    glow = e * np.clip((n1 - 0.4) / 0.2, 0, 1)
    col = col * (1 - glow[..., None]) + _hex("#7fd8ff") * glow[..., None]
    em = _hex("#60c8ff")[None, None] * (glow * 0.7)[..., None]
    return col, n1 + 0.3 * frost, (0.5 + 0.3 * frost).astype(np.float32), (0.8 * (1 - frost)).astype(np.float32), em


def sw_soot(h, w, rng, t, s):
    fib = _fbm(rng, 4, 40, h, w, 2)
    streak = np.clip(np.sin(s * 60.0 + fib * 8.0) * 0.5 + 0.5, 0, 1)
    col = _ramp(np.clip(0.2 + 0.5 * streak * (1 - t * 0.6), 0, 1), [(0, "#050505"), (0.6, "#171615"), (1, "#34312e")])
    glow = np.clip((t - 0.93) / 0.04, 0, 1) * np.clip((fib - 0.45) / 0.1, 0, 1)
    col = col * (1 - glow[..., None]) + _hex("#ff5a1a") * glow[..., None]
    em = _hex("#ff4a10")[None, None] * (glow * 0.8)[..., None]
    return col, streak, np.full((h, w), 0.95, np.float32), np.zeros((h, w), np.float32), em


def sw_claw(h, w, rng, t, s):
    n1 = _fbm(rng, 4, 8, h, w, 3)
    col = _ramp(np.clip(0.2 + 0.6 * t + 0.2 * (n1 - 0.5), 0, 1), [(0, "#0b0908"), (0.6, "#2a2522"), (1, "#8a8278")])
    return col, n1 * 0.3, (0.3 + 0.2 * n1).astype(np.float32), np.zeros((h, w), np.float32), np.zeros((h, w, 3), np.float32)


def sw_slag_plate(h, w, rng, t, s):
    """Cooled slag: dark glassy-rough stone, a faint crack net, the slab edges (thickness) glowing."""
    n1 = _fbm(rng, 10, 10, h, w, 4)
    col = _ramp(n1, [(0, "#0f0d0c"), (0.5, "#2b2622"), (1, "#4e453d")])
    cell = _fbm(rng, 7, 7, h, w, 3)
    crack = np.clip(1.0 - np.abs(cell - 0.5) * 30.0, 0, 1) * (t < 0.88)
    # The seam glows only in a thin line through the middle of the slab's edge, broken up by crust.
    e = np.clip(1.0 - np.abs(t - 0.97) * 120.0, 0, 1) * np.clip((n1 - 0.3) / 0.15, 0, 1)
    glow = np.maximum(crack * 0.6, e * 0.85)
    col = col * (1 - glow[..., None]) + _hex("#f06a1c") * glow[..., None]
    em = _hex("#ff5a10")[None, None] * glow[..., None]
    return col, n1 - crack * 0.4, (0.8 - 0.2 * n1).astype(np.float32), np.zeros((h, w), np.float32), em


def sw_ash_robe(h, w, rng, t, s):
    weave = np.clip(np.sin(s * 240) * np.sin(t * 240) * 0.5 + 0.5, 0, 1)
    n1 = _fbm(rng, 5, 8, h, w, 4)
    col = _ramp(np.clip(0.4 + 0.4 * (n1 - 0.5) + 0.1 * weave, 0, 1), [(0, "#0b0908"), (0.5, "#221a17"), (1, "#3c2e29")])
    ash = np.clip((n1 - 0.62) / 0.1, 0, 1) * (1 - t)
    col = col * (1 - 0.5 * ash[..., None]) + _hex("#7a716a") * 0.5 * ash[..., None]
    burn = np.clip((t + 0.3 * (n1 - 0.5) - 0.8) / 0.08, 0, 1)
    col = col * (1 - burn[..., None]) + _hex("#0a0807") * burn[..., None]
    glow = np.clip(1.0 - np.abs(t + 0.3 * (n1 - 0.5) - 0.8) * 45.0, 0, 1)
    col = col * (1 - glow[..., None]) + _hex("#ff6a1c") * glow[..., None]
    em = _hex("#ff5010")[None, None] * glow[..., None]
    return col, weave * 0.3 + n1 * 0.4, np.full((h, w), 0.95, np.float32), np.zeros((h, w), np.float32), em


def sw_ember_core(h, w, rng, t, s):
    n1 = _fbm(rng, 5, 5, h, w, 4)
    col = _ramp(np.clip(0.55 + 0.6 * (n1 - 0.5), 0, 1), [(0, "#c8380a"), (0.5, "#ff8a1e"), (1, "#ffe08a")])
    em = col * 1.0
    return col, n1, np.full((h, w), 0.4, np.float32), np.zeros((h, w), np.float32), em


def sw_cinder(h, w, rng, t, s):
    n1 = _fbm(rng, 6, 6, h, w, 4)
    crust = np.clip((n1 - 0.58) / 0.1, 0, 1)
    hot = _ramp(np.clip(n1 * 1.4, 0, 1), [(0, "#ff4a0c"), (1, "#ffc24a")])
    col = hot * (1 - crust[..., None]) + _hex("#1c1310") * crust[..., None]
    em = hot * (1 - crust[..., None])
    return col, crust, np.full((h, w), 0.7, np.float32), np.zeros((h, w), np.float32), em


def sw_grate_iron(h, w, rng, t, s):
    n1 = _fbm(rng, 8, 8, h, w, 4)
    col = _ramp(n1, [(0, "#0c0b0a"), (0.5, "#221e1b"), (1, "#3d3631")])
    heat = np.clip((t - 0.6) / 0.35, 0, 1) ** 1.5
    hot = _ramp(heat, [(0, "#3a1408"), (0.5, "#c0400c"), (1, "#ff9a30")])
    col = col * (1 - heat[..., None]) + hot * heat[..., None]
    em = hot * (heat * 0.9)[..., None]
    return col, n1, (0.6 - 0.2 * heat).astype(np.float32), (0.8 * (1 - heat)).astype(np.float32), em


def sw_flame(h, w, rng, t, s):
    """Flame tongues: white-yellow at the root, orange, deep red at the tip; all emissive."""
    n1 = _fbm(rng, 4, 8, h, w, 3)
    col = _ramp(np.clip(t + 0.2 * (n1 - 0.5), 0, 1), [(0, "#fff0b0"), (0.35, "#ffb030"), (0.75, "#ff5a10"), (1, "#a01c06")])
    return col, n1 * 0.2, np.full((h, w), 0.6, np.float32), np.zeros((h, w), np.float32), col * 1.0


def sw_coal(h, w, rng, t, s):
    n1 = _fbm(rng, 10, 10, h, w, 4)
    cell = _fbm(rng, 8, 8, h, w, 3)
    crack = np.clip(1.0 - np.abs(cell - 0.5) * 9.0, 0, 1)
    col = _ramp(n1, [(0, "#090807"), (1, "#2a2420")])
    hot = _ramp(np.clip(n1 * 1.3, 0, 1), [(0, "#ff5a10"), (1, "#ffc040")])
    col = col * (1 - crack[..., None]) + hot * crack[..., None]
    return col, n1 - crack * 0.5, np.full((h, w), 0.85, np.float32), np.zeros((h, w), np.float32), hot * crack[..., None]


ms.EXTRA_SWATCHES.update(char_straw=sw_char_straw, hat_straw=sw_hat_straw, scythe=sw_scythe, rope=sw_rope, chalk=sw_chalk, frost_wood=sw_frost_wood,
                         frost_leather=sw_frost_leather, frost_iron=sw_frost_iron, soot=sw_soot, claw=sw_claw,
                         slag_plate=sw_slag_plate, ash_robe=sw_ash_robe, ember_core=sw_ember_core, cinder=sw_cinder,
                         grate_iron=sw_grate_iron, flame=sw_flame, coal=sw_coal)


# ---------------------------------------------------------------------------------------------
# Primitives

class P15(Parts):
    def box(self, c, ax, ay, hs, sw, w, jit=0.0, rng=None, taper=1.0):
        """Box at c, axes ax, ay (az = ax x ay), half sizes hs; `taper` scales the +az face."""
        ax = _unit(ax)
        ay = _unit(np.asarray(ay, float) - ax * np.dot(ay, ax))
        az = np.cross(ax, ay)
        pts = []
        for k in (-1, 1):
            for j in (-1, 1):
                for i in (-1, 1):
                    f = taper if k > 0 else 1.0
                    p = np.asarray(c, float) + ax * hs[0] * i * f + ay * hs[1] * j * f + az * hs[2] * k
                    if jit and rng is not None:
                        p = p + (rng.random(3) - 0.5) * 2 * jit
                    pts.append(p)
        i0 = self._verts(pts, w)
        uv = [(0, 0), (1, 0), (1, 0.85), (0, 0.85)]
        for q in ((0, 1, 3, 2), (4, 5, 7, 6), (0, 1, 5, 4), (2, 3, 7, 6), (0, 2, 6, 4), (1, 3, 7, 5)):
            self.face([i0 + a for a in q], uv, sw)

    def prism(self, c, ex, ey, pts2d, thick, sw, w, back=0.5):
        """Flat slab: outline pts2d [(x, y)] in the plane c + x ex + y ey, `thick` along ex x ey
        (`back` of it behind the plane). Cap UVs from the outline's bounds (t 0..0.85), the rim in
        the swatch's edge band (t 0.95..0.99)."""
        ex = _unit(ex)
        ey = _unit(np.asarray(ey, float) - ex * np.dot(ey, ex))
        nz = np.cross(ex, ey)
        P = np.asarray(pts2d, float)
        lo, hi = P.min(0), P.max(0)
        uv = [((x - lo[0]) / max(hi[0] - lo[0], 1e-6), 0.85 * (y - lo[1]) / max(hi[1] - lo[1], 1e-6)) for x, y in P]
        base = [np.asarray(c, float) + ex * x + ey * y for x, y in P]
        f = self._verts([p + nz * thick * (1 - back) for p in base], w)
        b = self._verts([p - nz * thick * back for p in base], w)
        n = len(P)
        self.face([f + i for i in range(n)], uv, sw)
        self.face([b + i for i in range(n)][::-1], uv[::-1], sw)
        for i in range(n):
            i2 = (i + 1) % n
            self.face([f + i, b + i, b + i2, f + i2], [(uv[i][0], 0.95), (uv[i][0], 0.99), (uv[i2][0], 0.99),
                                                       (uv[i2][0], 0.95)], sw)

    def skirt(self, P, N, thick, sw, wfn):
        """Closed sheet: P (rows, cols, 3) with the columns wrapping round, N outward normals."""
        R, C = P.shape[:2]
        f = np.zeros((R, C), int)
        b = np.zeros((R, C), int)
        for r in range(R):
            for c in range(C):
                f[r, c] = self._verts([P[r, c]], wfn(r, c))
                b[r, c] = self._verts([P[r, c] - N[r, c] * thick], wfn(r, c))
        for r in range(R - 1):
            for c in range(C):
                c2 = (c + 1) % C
                u0, u1, t0, t1 = c / C, (c + 1) / C, r / (R - 1), (r + 1) / (R - 1)
                self.face([f[r, c], f[r, c2], f[r + 1, c2], f[r + 1, c]], [(u0, t0), (u1, t0), (u1, t1), (u0, t1)], sw)
                self.face([b[r + 1, c], b[r + 1, c2], b[r, c2], b[r, c]], [(u0, t1), (u1, t1), (u1, t0), (u0, t0)], sw)
        for r in (0, R - 1):
            t = r / (R - 1)
            for c in range(C):
                c2 = (c + 1) % C
                self.face([f[r, c], b[r, c], b[r, c2], f[r, c2]], [(c / C, t), (c / C, t), ((c + 1) / C, t),
                                                                   ((c + 1) / C, t)], sw)

    def tube(self, pts, r, sw, wfn, sides=6, closed=False, hint=(0, 0, 1)):
        """Round tube along a polyline (closed: a ring); wfn(k) -> weights of point k; hint must not be
        parallel to the tangent (for a ring: the ring's axis)."""
        pts = [np.asarray(p, float) for p in pts]
        n = len(pts)
        rings = []
        for k in range(n):
            a = pts[(k + 1) % n] - pts[k - 1] if closed else pts[min(k + 1, n - 1)] - pts[max(k - 1, 0)]
            _, e1, e2 = _frame(a, hint)
            rk = r(k) if callable(r) else r
            rings.append(self._verts([pts[k] + rk * (math.cos(2 * math.pi * j / sides) * e1 +
                                                     math.sin(2 * math.pi * j / sides) * e2) for j in range(sides)],
                                     wfn(k)))
        segs = n if closed else n - 1
        for k in range(segs):
            ia, ib = rings[k], rings[(k + 1) % n]
            t0, t1 = k / segs, (k + 1) / segs
            for j in range(sides):
                j2 = (j + 1) % sides
                self.face([ia + j, ia + j2, ib + j2, ib + j], [(j / sides, t0), ((j + 1) / sides, t0),
                                                               ((j + 1) / sides, t1), (j / sides, t1)], sw)
        if not closed:
            for i0, first in ((rings[0], True), (rings[-1], False)):
                c = self._verts([np.mean([self.V[i0 + j] for j in range(sides)], axis=0)], wfn(0 if first else n - 1))
                for j in range(sides):
                    j2 = (j + 1) % sides
                    q = [i0 + j2, i0 + j, c] if first else [i0 + j, i0 + j2, c]
                    self.face(q, [(0.5, 0.0), (0.52, 0.0), (0.51, 0.0)], sw)

    def ball(self, c, r, sw, w, sides=8, axis=(0, 0, 1), squash=1.0, hint=(1, 0, 0), rings=5):
        a = _unit(axis)
        prof = [(0.0, 0.0)] + [(k / (rings + 1), r * math.sin(math.pi * k / (rings + 1))) for k in range(1, rings + 1)] \
            + [(1.0, 0.0)]
        self.lathe(np.asarray(c, float) - a * r, a * 2 * r, prof, sw, w, sides=sides, hint=hint, squash=squash)

    def band(self, B, bone, t, pad, sw, height=0.05, sides=10, w=None):
        """A ring (short closed cylinder) round a limb bone at fraction t, `pad` outside its skin."""
        p, d, r = limb_section(B, bone, t)
        self.lathe(p - d * height / 2, d * height, [(0, r + pad), (1, r + pad)], sw, w or {bone: 1.0}, sides=sides)
        return p, d, r


def cloud(B, bone, thr=0.5):
    return B.V[B.W[:, B.names.index(bone)] > thr]


def head_info(B):
    V = cloud(B, "Head")
    return V.mean(0), float(V[:, 2].max()), V


def bone_dir(B, name):
    return _unit(B.tail[name] - B.head[name])


def limb_section(B, bone, t):
    a, b = B.head[bone], B.tail[bone]
    d = _unit(b - a)
    p = a + (b - a) * t
    V = cloud(B, bone, 0.4)
    rel = V - p
    k = rel @ d
    perp = np.linalg.norm(rel - np.outer(k, d), axis=1)
    sel = np.abs(k) < 0.04
    r = float(np.percentile(perp[sel], 90)) if sel.sum() > 5 else 0.06
    return p, d, r


def side_weights(x, scale=1.2):
    """Left / right thigh shares for a point at x (+X = the creature's left)."""
    wl = float(np.clip(0.5 + x * scale, 0, 1))
    return wl, 1.0 - wl


# ---------------------------------------------------------------------------------------------
# Tanyeri

def reaper_parts(B, H, rng):
    """Kavruk Tırpancı: a wide burnt straw hat, a scythe blade grown out of the right fist, twine at the
    wrist, a short cape of straw down the back."""
    o = P15()
    hc, htop, _ = head_info(B)
    wh = {"Head": 1.0}
    base = np.array([hc[0], hc[1] + 0.01, htop - 0.13])
    i0 = len(o.V)
    sides = 20
    # Tipped back against the stoop (in the clips the head pitches forward), so the cone reads as a hat.
    o.lathe(base, _unit(np.array([0.0, 0.22, 1.0])) * 0.24, [(0, 0.40), (0.07, 0.37), (0.4, 0.16), (1.0, 0.0)],
            "hat_straw", wh, sides=sides, hint=(1, 0, 0))
    for j in range(sides):  # ragged, slightly drooping brim
        v = np.array(o.V[i0 + j])
        o.V[i0 + j] = tuple(base + (v - base) * (0.92 + 0.14 * rng.random()) + np.array([0, 0, -0.02 - 0.025 * rng.random()]))
    # Scythe blade: a crescent from the right fist, curving forward and out.
    hand = B.head["hand_r"]
    fa = bone_dir(B, "lowerarm_r")
    out = _unit(np.array([-0.45, -0.9, 0.0]) - fa * np.dot(np.array([-0.45, -0.9, 0.0]), fa))
    # A sickle arc (radius Rr, sweep Th) so that the tip hooks round instead of reaching far out (the
    # death clip throws the arm overhead: the content test's reach limit).
    Rr, Th = 0.26, math.radians(100)
    outer, inner = [], []
    for k in range(15):
        u = k / 14
        th = u * Th
        cx, cy = Rr * math.sin(th) + 0.06 * u, Rr * (1 - math.cos(th))
        nx, ny = -math.sin(th), math.cos(th)  # toward the arc's centre
        wd = 0.085 * (1 - u) ** 0.7 + 0.006
        outer.append((cx - nx * wd * 0.3, cy - ny * wd * 0.3))
        inner.append((cx + nx * wd * 0.7, cy + ny * wd * 0.7))
    o.prism(hand + fa * 0.04, fa, out, outer + inner[::-1][1:-1], 0.014, "scythe", {"hand_r": 1.0})
    # A charred wooden collar and twine where the blade meets the fist.
    o.lathe(hand - fa * 0.02, fa * 0.07, [(0, 0.052), (1, 0.05)], "rope", {"hand_r": 1.0}, sides=10)
    o.band(B, "lowerarm_l", 0.85, 0.012, "rope")
    # Straw cape: stalks from the shoulder blades down the back (torso only: the arms stay clear).
    sp3 = B.head["spine_03"]
    for k in range(30):
        x = (k / 29 - 0.5) * 0.30
        z = sp3[2] + 0.12 - 0.05 * rng.random()
        p = B.surf(np.array([x, 0.0, z]), (0, 1, 0), rad=0.04, bones=TORSO) - np.array([0, 0.01, 0])
        d = np.array([x * 0.8, 0.25 + 0.1 * rng.random(), -1.0])
        o.spike(p, d, 0.36 + 0.14 * rng.random(), 0.013, "char_straw", B.near(p, only=TORSO), sides=4)
    return o


def brute_parts(B, H, rng):
    """Harman Devi: two burnt sheaves crossed on the back, a rope bandolier and rope bands on the limbs,
    fists bound into rope-and-iron flail heads."""
    o = P15()
    sp3, sp2 = B.head["spine_03"], B.head["spine_02"]
    c = B.surf(sp2 + np.array([0, 0, 0.16]), (0, 1, 0), bones=TORSO) + np.array([0, 0.13, 0])
    wb = B.near(c - np.array([0, 0.13, 0]), only=TORSO)
    for sx in (1, -1):
        d = _unit(np.array([0.70 * sx, 0.15, 1.0]))
        Ls = 1.15
        a = c - d * Ls * 0.45
        o.lathe(a, d * Ls, [(0, 0.10), (0.3, 0.125), (0.5, 0.10), (0.7, 0.125), (1, 0.115)], "char_straw", wb, sides=10,
                hint=(0, 1, 0), twist=0.4)
        for tt in (0.47, 0.2):
            o.lathe(a + d * Ls * (tt - 0.03), d * Ls * 0.06, [(0, 0.115), (1, 0.115)], "rope", wb, sides=10,
                    hint=(0, 1, 0))
        _, e1, e2 = _frame(d, (0, 1, 0))
        for k in range(16):  # splayed ears at the top, stubble at the bottom
            ang = 2 * math.pi * k / 16
            rad = math.cos(ang) * e1 + math.sin(ang) * e2
            o.spike(a + d * Ls + rad * 0.07, d + rad * 0.6, 0.26 + 0.12 * rng.random(), 0.02, "straw", wb, sides=4)
            if k % 2 == 0:
                o.spike(a + rad * 0.07, -d * 0.6 + rad * 0.6, 0.12 + 0.05 * rng.random(), 0.016, "straw", wb, sides=4)
    # Bandolier: a rope from the right shoulder to the left hip.
    ctr = (sp3 + B.head["spine_01"]) / 2
    nrm = _unit(np.array([0.55, 0.0, 1.0]))
    _, e1, e2 = _frame(nrm, (0, 1, 0))
    pts, ws = [], []
    for k in range(24):
        ang = 2 * math.pi * k / 24
        dd = math.cos(ang) * e1 + math.sin(ang) * e2
        p = B.surf(ctr, dd, rad=0.06, bones=TORSO) + dd * 0.025
        pts.append(p)
        ws.append(B.near(p, only=TORSO))
    o.tube(pts, 0.03, "rope", lambda k: ws[k], sides=6, closed=True, hint=nrm)
    for side in ("l", "r"):
        # Flail fists: a lumpy ball of rope round the fist, iron studs, a band at the wrist.
        hand = B.head[f"hand_{side}"]
        fa = bone_dir(B, f"lowerarm_{side}")
        cc = hand + fa * 0.12
        o.ball(cc, 0.21, "rope", {f"hand_{side}": 1.0}, sides=12, axis=fa, rings=7)
        _, e1, e2 = _frame(fa)
        for k in range(10):
            ang = 2 * math.pi * k / 10 + 0.3
            dd = fa * (0.6 if k % 2 else -0.1) + math.cos(ang) * e1 + math.sin(ang) * e2
            o.spike(cc + _unit(dd) * 0.17, dd, 0.11, 0.035, "burnt_iron", {f"hand_{side}": 1.0}, sides=4)
        o.band(B, f"lowerarm_{side}", 0.75, 0.02, "rope", height=0.07)
        o.band(B, f"upperarm_{side}", 0.5, 0.02, "rope", height=0.06)
        o.band(B, f"thigh_{side}", 0.35, 0.02, "rope", height=0.06)
        p, d, r = limb_section(B, f"thigh_{side}", 0.35)
        _, e1, e2 = _frame(d)
        for k in range(5):  # straw tufts stuck under the thigh rope
            ang = 2 * math.pi * k / 5 + 0.5
            dd = math.cos(ang) * e1 + math.sin(ang) * e2
            o.spike(p + dd * (r + 0.02), dd * 0.5 + d * 0.9, 0.12, 0.012, "straw", {f"thigh_{side}": 1.0}, sides=4)
    return o


# ---------------------------------------------------------------------------------------------
# Demirağız

def bellows_parts(B, H, rng):
    """Buzul Körükçü: a big frosted forge bellows strapped to the hunched back (two boards, pleated
    leather, an iron nozzle, handles over the shoulders), frost crust on the shoulders, icicles."""
    o = P15()
    add_bellows(o, B, rng, 1.2)
    frost_trim(o, B, rng, beard_icicles=6, beard_len=0.06)
    return o


def add_bellows(o, B, rng, k_size, x_off=0.0):
    sp1, sp2, sp3, nk = (B.head[b] for b in ("spine_01", "spine_02", "spine_03", "neck_01"))
    up = _unit(nk - sp1)
    ex = np.array([1.0, 0, 0])
    up = _unit(up - ex * np.dot(up, ex))
    back = np.cross(up, ex)  # (spine up) x (+X) = toward the back
    if back[1] < 0:
        back = -back
    c0 = B.surf((sp2 + sp3) / 2 + ex * x_off, back, rad=0.06, bones=TORSO)
    wb = {"spine_03": 0.7, "spine_02": 0.3}
    K = k_size
    outline = []
    for k in range(18):
        a = 2 * math.pi * k / 18
        x, y = 0.25 * math.sin(a), 0.25 * math.cos(a) + 0.06
        if math.cos(a) < 0:
            f = 1.0 + math.cos(a)  # 1 at the sides, 0 at the bottom
            x = 0.25 * math.sin(a) * (0.25 + 0.75 * f)
            y = 0.06 + math.cos(a) * 0.46
        outline.append((x * K, y * K))
    d_in, d_out, tb = 0.05, 0.05 + 0.20 * K, 0.035 * K
    o.prism(c0 + back * d_in, ex, up, outline, tb, "frost_wood", wb, back=0.0)
    o.prism(c0 + back * d_out, ex, up, outline, tb, "frost_wood", wb, back=1.0)
    # Pleated leather between the boards: three rings, the middle one pulled in.
    rings = []
    mid = (d_in + d_out) / 2
    for dd, sc in ((d_in + tb, 0.94), (mid, 0.80), (d_out - tb, 0.94)):
        rings.append([c0 + back * dd + ex * x * sc + up * (0.06 * K + (y - 0.06 * K) * sc) for x, y in outline])
    P = np.array(rings)
    N = np.zeros_like(P)
    for r in range(3):
        for c in range(len(outline)):
            N[r, c] = _unit(ex * outline[c][0] + up * (outline[c][1] - 0.06 * K))
    o.skirt(P, N, 0.01, "frost_leather", lambda r, c: wb)
    # Nozzle at the bottom point, handles up over the shoulders.
    tip = c0 + back * mid + up * (0.06 - 0.46) * K
    o.lathe(tip + up * 0.06 * K, (-up * 0.26 + ex * 0.06) * K, [(0, 0.05 * K), (0.6, 0.032 * K), (1, 0.022 * K)],
            "frost_iron", wb, sides=8)
    for dd in (d_in + tb / 2, d_out - tb / 2):
        top = c0 + back * dd + up * 0.30 * K
        o.lathe(top, (up * 0.30 + back * 0.04) * K, [(0, 0.026 * K), (0.85, 0.024 * K), (0.9, 0.04 * K), (1, 0.03 * K)],
                "frost_wood", wb, sides=6)
    # Icicles under the boards' lower rims.
    for k in range(7):
        x = (-0.15 + 0.3 * k / 6) * K
        y = (0.06 - 0.46 * (1 - abs(x / K) / 0.25) * 0.8) * K
        for dd in (d_in + tb / 2, d_out - tb / 2):
            p = c0 + back * dd + ex * x + up * (y + 0.01)
            o.spike(p, (0, 0.05, -1), (0.06 + 0.08 * rng.random()) * K, 0.012 * K, "ice", wb, sides=5,
                    prof=[(0, 0.012 * K), (0.3, 0.009 * K), (1, 0.0)])


def frost_trim(o, B, rng, beard_icicles, beard_len):
    """Frost crust on the shoulders, icicles off the forearms and the beard."""
    for side, sx in (("l", 1), ("r", -1)):
        sh = B.head[f"upperarm_{side}"]
        top = B.surf(sh + np.array([0, 0, 0.02]), (0.4 * sx, 0, 1)) - np.array([0, 0, 0.03])
        w = B.near(top)
        for k in range(3):
            p = top + np.array([sx * 0.03 * k, (k - 1) * 0.06, 0])
            o.lathe(p, np.array([sx * 0.04, 0, 0.09]), [(0, 0.07), (0.5, 0.06), (1, 0.0)], "frost_rock", w, sides=7,
                    squash=0.8)
        for k in range(4):
            q = B.head[f"lowerarm_{side}"] + bone_dir(B, f"lowerarm_{side}") * (0.05 + 0.06 * k)
            p = B.surf(q, (0, 0.3, -1), rad=0.05, bones=(f"lowerarm_{side}",))
            o.spike(p, (0, 0, -1), 0.07 + 0.06 * rng.random(), 0.013, "ice", {f"lowerarm_{side}": 1.0}, sides=5,
                    prof=[(0, 0.013), (0.3, 0.01), (1, 0.0)])
    if "beard" in B.names:
        V = cloud(B, "beard", 0.6)
        if len(V):
            low = V[V[:, 2] < V[:, 2].min() + 0.06 + 0.4 * beard_len]
            for k in range(min(beard_icicles, len(low))):
                p = low[int(rng.integers(len(low)))]
                r = 0.011 + 0.04 * beard_len
                o.spike(p + np.array([0, -0.005, 0.01]), (0, -0.1, -1), beard_len * (1 + 0.8 * rng.random()), r, "ice",
                        {"beard": 1.0}, sides=5, prof=[(0, r), (0.3, r * 0.75), (1, 0.0)])


def anvil_parts(B, H, rng):
    """Örs Kıran: the right fist sunk in a huge iron hammer head, an iron gauntlet with ice knuckles on
    the left, iron slabs on the shoulders, a ridge of ice shards down the back."""
    o = P15()
    hand = B.head["hand_r"]
    fa = bone_dir(B, "lowerarm_r")
    cross = _unit(np.cross(fa, np.array([0, 0, 1.0])))
    wr = {"hand_r": 1.0}
    hc = hand + fa * 0.11
    o.box(hc, cross, np.cross(fa, cross), (0.25, 0.13, 0.15), "frost_iron", wr)
    for sgn in (1, -1):  # bevelled striking faces
        o.box(hc + cross * sgn * 0.27, cross, np.cross(fa, cross), (0.03, 0.115, 0.135), "frost_iron", wr)
    o.lathe(hand - fa * 0.08, fa * 0.12, [(0, 0.075), (1, 0.09)], "frost_iron", wr, sides=8)
    for k in range(4):  # ice grown over the head
        p = hc + cross * (-0.15 + 0.1 * k) + fa * -0.12
        o.crystal(p, -fa + cross * (0.3 * (k - 1.5)), 0.12 + 0.06 * rng.random(), 0.03, "ice", wr, sides=5)
    # Left gauntlet with ice knuckle spikes.
    hand = B.head["hand_l"]
    fa = bone_dir(B, "lowerarm_l")
    cross = _unit(np.cross(fa, np.array([0, 0, 1.0])))
    wl = {"hand_l": 1.0}
    o.box(hand + fa * 0.07, cross, np.cross(fa, cross), (0.09, 0.09, 0.11), "frost_iron", wl)
    for k in range(3):
        p = hand + fa * 0.17 + cross * (-0.05 + 0.05 * k)
        o.crystal(p, fa + cross * 0.1 * (k - 1), 0.10, 0.022, "ice", wl, sides=5)
    # Shoulder slabs.
    for side, sx in (("l", 1), ("r", -1)):
        sh = B.head[f"upperarm_{side}"]
        top = B.surf(sh + np.array([0, 0, 0.03]), (0.45 * sx, 0, 1)) + np.array([0, 0, 0.02])
        w = {f"clavicle_{side}": 0.5, f"upperarm_{side}": 0.5}
        o.box(top, np.array([1.0, 0, -0.35 * sx]), np.array([0, 1.0, 0]), (0.17, 0.2, 0.045), "frost_iron", w,
              jit=0.01, rng=rng)
        for k in range(3):
            o.crystal(top + np.array([sx * 0.05, (k - 1) * 0.09, 0.03]), np.array([sx * 0.6, (k - 1) * 0.3, 1.0]),
                      0.18 + 0.08 * rng.random(), 0.035, "ice", w, sides=6)
    # Ice ridge down the spine.
    for k in range(6):
        z = B.head["spine_03"][2] + 0.18 - 0.11 * k
        p = B.surf(np.array([0, 0, z]), (0, 1, 0), rad=0.05, bones=TORSO) - np.array([0, 0.03, 0])
        o.crystal(p, np.array([0, 1.0, 0.8 - 0.1 * k]), 0.20 - 0.02 * k + 0.04 * rng.random(), 0.045, "ice",
                  B.near(p, only=TORSO), sides=6, twist=0.3)
    return o


# ---------------------------------------------------------------------------------------------
# Ortaocak

def stalker_parts(B, H, rng):
    """İs Avcısı: a long soot snout, swept-back horns, quills along the spine, hooked claws, a tail."""
    o = P15()
    hc, htop, HV = head_info(B)
    wh = {"Head": 1.0}
    hd = B.head["Head"]
    fwd = _unit(np.array([0, -1.0, -0.15]))
    face = B.surf(np.array([hc[0], hc[1], hc[2] - 0.01]), fwd, rad=0.03, bones=("Head",))
    o.lathe(face - fwd * 0.06, fwd * 0.27, [(0, 0.07), (0.4, 0.05), (0.8, 0.025), (1, 0.0)], "soot", wh, sides=7,
            squash=0.7, hint=(1, 0, 0), bend=(0, 0, -0.03))
    for sx in (1, -1):
        p = B.surf(hc + np.array([0, 0, 0.04]), (sx, 0.5, 0.6), rad=0.03, bones=("Head",)) - np.array([0, 0.01, 0])
        o.lathe(p, np.array([sx * 0.10, 0.34, 0.02]), [(0, 0.035), (0.5, 0.025), (1, 0.0)], "claw", wh, sides=6,
                bend=(sx * 0.02, 0.0, 0.05), hint=(0, 0, 1))
        # Ember eyes either side of the snout's root.
        eye = face - fwd * 0.01 + np.array([sx * 0.045, 0, 0.035])
        o.ball(eye, 0.018, "cinder", wh, sides=6, rings=3)
    # Quills from the nape to the small of the back.
    nk, sp1 = B.head["neck_01"], B.head["spine_01"]
    for k in range(11):
        q = nk + (sp1 - nk) * (k / 10)
        p = B.surf(q, (0, 1, 0.3), rad=0.05, bones=TORSO) - np.array([0, 0.015, 0])
        for sx in ((0,) if k % 2 else (1, -1)):
            d = np.array([sx * 0.35, 0.9, 0.55])
            o.spike(p + np.array([sx * 0.04, 0, 0]), d, 0.16 + 0.12 * math.sin(math.pi * k / 10) + 0.04 * rng.random(),
                    0.018, "soot", B.near(p, only=TORSO), sides=4)
    # Claws: four per hand, out of the fingers' ends, hooked down.
    for side in ("l", "r"):
        hand = B.head[f"hand_{side}"]
        fa = bone_dir(B, f"lowerarm_{side}")
        _, e1, e2 = _frame(fa, (0, -1, 0))
        for k in range(4):
            p = hand + fa * 0.13 + e1 * (-0.045 + 0.03 * k) * 0.8
            o.lathe(p, fa * 0.17, [(0, 0.014), (0.5, 0.01), (1, 0.0)], "claw", {f"hand_{side}": 1.0}, sides=5,
                    bend=e2 * 0.05, hint=(0, 0, 1))
    # Tail: off the pelvis, up and back, curling.
    pel = B.head["pelvis"]
    root = B.surf(pel + np.array([0, 0, 0.08]), (0, 1, 0), rad=0.05, bones=("pelvis", "spine_01")) - np.array([0, 0.03, 0])
    o.lathe(root, np.array([0, 0.48, 0.12]), [(0, 0.06), (0.4, 0.045), (0.8, 0.025), (1, 0.0)], "soot",
            {"pelvis": 1.0}, sides=7, bend=(0, -0.05, 0.22), hint=(1, 0, 0))
    return o


def bulwark_parts(B, H, rng):
    """Cüruf Zırhlısı: a walking wall of cooled slag slabs with glowing seams - stacked chest and back
    slabs, a rampart of slabs over the shoulders, tower slabs on the forearms, slabs on thighs and shins."""
    o = P15()

    def slab_outline(wx, wy, n=7, jag=0.18):
        pts = []
        for k in range(n):
            a = 2 * math.pi * (k + 0.5) / n
            r = 1.0 - jag * rng.random()
            pts.append((wx * r * math.cos(a) * 1.25, wy * r * math.sin(a) * 1.25))
        return [(max(-wx, min(wx, x)), max(-wy, min(wy, y))) for x, y in pts]

    sp3, sp2, sp1 = B.head["spine_03"], B.head["spine_02"], B.head["spine_01"]
    ex = np.array([1.0, 0, 0])
    # Chest and belly: three slabs, top one widest.
    for k, (q, wx, wy) in enumerate(((sp3 + np.array([0, 0, 0.08]), 0.27, 0.15), (sp2 + np.array([0, 0, 0.02]), 0.23, 0.12),
                                     (sp1 + np.array([0, 0, -0.02]), 0.20, 0.10))):
        p = B.surf(q, (0, -1, 0), rad=0.06, bones=TORSO) - np.array([0, 0.03, 0])
        tilt = _unit(np.array([0, -0.25, 1.0]))
        o.prism(p, ex, tilt, slab_outline(wx, wy), 0.07, "slag_plate", B.near(p, only=TORSO))
    # Back: two slabs and the rampart rising over the shoulders behind the head.
    for q, wx, wy in ((sp2 + np.array([0, 0, 0.0]), 0.25, 0.13), (sp3 + np.array([0, 0, 0.06]), 0.30, 0.16)):
        p = B.surf(q, (0, 1, 0), rad=0.06, bones=TORSO) + np.array([0, 0.03, 0])
        o.prism(p, ex, _unit(np.array([0, 0.2, 1.0])), slab_outline(wx, wy), 0.08, "slag_plate", B.near(p, only=TORSO))
    p = B.surf(sp3 + np.array([0, 0, 0.12]), (0, 1, 0), rad=0.06, bones=TORSO) + np.array([0, 0.12, 0.26])
    wr = {"spine_03": 1.0}
    rampart = [(-0.34, -0.22), (0.34, -0.22), (0.36, 0.10), (0.24, 0.16), (0.20, 0.06), (0.06, 0.20), (-0.04, 0.12),
               (-0.16, 0.22), (-0.24, 0.08), (-0.36, 0.12)]
    o.prism(p, ex, _unit(np.array([0, 0.15, 1.0])), rampart, 0.09, "slag_plate", wr)
    for side, sx in (("l", 1), ("r", -1)):
        # Shoulders: two stacked slabs, the upper one tipped out.
        sh = B.head[f"upperarm_{side}"]
        top = B.surf(sh + np.array([0, 0, 0.03]), (0.4 * sx, 0, 1)) + np.array([0, 0, 0.02])
        w = {f"clavicle_{side}": 0.5, f"upperarm_{side}": 0.5}
        o.prism(top, np.array([1.0, 0, -0.45 * sx]), np.array([0, 1.0, 0]), slab_outline(0.17, 0.19), 0.07,
                "slag_plate", w)
        o.prism(top + np.array([sx * 0.06, 0, 0.08]), np.array([1.0, 0, -0.9 * sx]), np.array([0, 1.0, 0]),
                slab_outline(0.13, 0.16), 0.06, "slag_plate", w)
        # Forearm tower slab on the outer side.
        fa = bone_dir(B, f"lowerarm_{side}")
        el = B.head[f"lowerarm_{side}"]
        L = float(np.linalg.norm(B.head[f"hand_{side}"] - el))
        outd = _unit(np.cross(fa, np.array([0, 1.0, 0])) * sx)
        if outd[0] * sx < 0:
            outd = -outd
        p = B.surf(el + fa * L * 0.55, outd, rad=0.05, bones=(f"lowerarm_{side}",)) + outd * 0.02
        o.prism(p, fa, np.array([0, 1.0, 0]), slab_outline(L * 0.62, 0.15), 0.06, "slag_plate", {f"lowerarm_{side}": 1.0},
                back=0.0)
        # Thigh and shin slabs (front), each on its own bone.
        for bone, t, wx, wy in ((f"thigh_{side}", 0.4, 0.13, 0.17), (f"calf_{side}", 0.45, 0.10, 0.16)):
            pp, d, r = limb_section(B, bone, t)
            front = B.surf(pp, (0, -1, 0), rad=0.05, bones=(bone,)) - np.array([0, 0.015, 0])
            o.prism(front, ex, -d, slab_outline(wx, wy), 0.05, "slag_plate", {bone: 1.0})
    return o


def warlock_parts(B, H, rng):
    """Kor Büyücüsü: a pointed hood, a long robe from the waist with a burnt glowing hem, an ember core
    caged in the chest, cinders circling both hands."""
    o = P15()
    hc, htop, HV = head_info(B)
    # Hood: an open shell round the back and sides of the head, rising to a point behind.
    rh = float(np.percentile(np.linalg.norm(HV[:, :2] - hc[:2], axis=1), 92)) + 0.035
    zc = hc[2]
    R, C = 8, 13
    P = np.zeros((R, C, 3))
    N = np.zeros((R, C, 3))
    tipp = np.array([hc[0], hc[1] + 0.30, htop + 0.22])
    for r in range(R):
        el = math.radians(-35 + 125 * r / (R - 1))
        for c in range(C):
            az = math.radians(-125 + 250 * c / (C - 1))  # 0 = straight back (+y)
            d = np.array([math.sin(az) * math.cos(el), math.cos(az) * math.cos(el), math.sin(el)])
            rr = rh * (1.0 + 0.25 * max(0.0, -math.sin(el)))
            p = np.array([hc[0], hc[1], zc + 0.02]) + d * rr * np.array([1.0, 1.0, 1.25])
            f = max(0.0, (r - (R - 4)) / 3.0) ** 1.5  # the top rows pulled toward the point
            P[r, c] = p * (1 - f) + tipp * f
            N[r, c] = d
    wh = lambda r, c: {"Head": 1.0} if r > 1 else {"Head": 0.6, "neck_01": 0.4}  # noqa: E731
    o.sheet(P, N, 0.015, "ash_robe", wh)
    # Robe: from the belt to mid-shin, flaring, a ragged hem.
    pel = B.head["pelvis"]
    z0 = pel[2] + 0.10
    knee = B.head["calf_l"][2]
    z1 = knee - 0.62 * (knee - B.head["foot_l"][2])
    R, C = 9, 24
    P = np.zeros((R, C, 3))
    N = np.zeros((R, C, 3))
    hem = [0.05 * rng.random() for _ in range(C)]
    for r in range(R):
        t = r / (R - 1)
        z = z0 + (z1 - z0) * t
        for c in range(C):
            a = 2 * math.pi * c / C
            d = np.array([math.sin(a), -math.cos(a), 0.0])
            s = B.surf(np.array([0, 0, z]), d, rad=0.06, bones=("pelvis", "spine_01", "thigh_l", "thigh_r", "calf_l",
                                                                "calf_r", "cloth_front", "cloth_back"))
            rad = max(float(np.linalg.norm((s - np.array([0, 0, z]))[:2])), 0.12)
            rad = rad + 0.03 + 0.10 * t * t
            P[r, c] = np.array([0, 0.01, z - hem[c] * t ** 4]) + d * rad
            N[r, c] = d

    def robe_w(r, c):
        t = r / (R - 1)
        a = 2 * math.pi * c / C
        wl, wr_ = side_weights(math.sin(a), 1.4)
        legs = min(1.0, t * 1.3) * 0.85
        w = {"pelvis": 1.0 - legs, "thigh_l": legs * wl, "thigh_r": legs * wr_}
        back = max(0.0, math.cos(a + math.pi))  # +y side (behind): follows the calves a little (run kick)
        if back > 0 and t > 0.5:
            k = 0.3 * back * (t - 0.5) * 2
            w["calf_l"], w["calf_r"] = k * wl, k * wr_
        return {n: v for n, v in w.items() if v > 1e-3}
    o.skirt(P, N, 0.014, "ash_robe", robe_w)
    # Ember core in the chest with four iron ribs over it.
    sp3 = B.head["spine_03"]
    ch = B.surf(sp3 + np.array([0, 0, 0.0]), (0, -1, 0), rad=0.05, bones=TORSO)
    wc = B.near(ch, only=TORSO)
    o.ball(ch + np.array([0, 0.0, 0]), 0.115, "ember_core", wc, sides=12, axis=(0, -1, 0), rings=6, hint=(0, 0, 1))
    for k in range(4):
        x = -0.075 + 0.05 * k
        pts = [ch + np.array([x, -0.10 * math.sin(math.pi * j / 6) - 0.012, 0.15 - 0.05 * j]) for j in range(7)]
        o.tube(pts, 0.011, "grate_iron", lambda j: wc, sides=5, hint=(1, 0, 0))
    # Cinders round the hands.
    for side in ("l", "r"):
        hand = B.head[f"hand_{side}"]
        fa = bone_dir(B, f"lowerarm_{side}")
        _, e1, e2 = _frame(fa)
        for k in range(8):
            ang = 2 * math.pi * k / 8 + rng.random() * 0.4
            p = hand + fa * (0.06 + 0.08 * rng.random()) + (math.cos(ang) * e1 + math.sin(ang) * e2) * (0.14 + 0.05 * rng.random())
            d = _unit(rng.random(3) - 0.5)
            o.crystal(p - d * 0.04, d, 0.08 + 0.04 * rng.random(), 0.026, "cinder", {f"hand_{side}": 1.0}, sides=4)
    return o


def hearth_parts(B, H, rng):
    """Ocak Efendisi (boss): a burning hearth grate for a crown (iron band, bars, coals, flames), a
    glowing grate in the chest, two flue pipes off the shoulders, ember shards on the forearms."""
    o = P15()
    hc, htop, HV = head_info(B)
    wh = {"Head": 1.0}
    rh = float(np.percentile(np.linalg.norm(HV[:, :2] - hc[:2], axis=1), 90)) + 0.03
    zb = htop - 0.09
    c = np.array([hc[0], hc[1], zb])
    o.lathe(c, np.array([0, 0, 0.09]), [(0, rh), (1, rh + 0.02)], "grate_iron", wh, sides=12)
    nb = 10
    tops = []
    for k in range(nb):
        a = 2 * math.pi * (k + 0.5) / nb
        d = np.array([math.cos(a), math.sin(a), 0])
        b0 = c + d * (rh + 0.02) + np.array([0, 0, 0.07])
        b1 = b0 + d * 0.07 + np.array([0, 0, 0.30 + (0.07 if k % 2 else 0.0)])
        o.lathe(b0, b1 - b0, [(0, 0.018), (0.9, 0.016), (1, 0.0 if k % 2 else 0.016)], "grate_iron", wh, sides=5)
        tops.append(b0 + (b1 - b0) * 0.72)
    o.tube(tops, 0.014, "grate_iron", lambda k: wh, sides=5, closed=True, hint=(0, 0, 1))
    for k in range(9):  # coals heaped on the grate floor
        a = 2 * math.pi * k / 9
        p = c + np.array([0, 0, 0.11]) + np.array([math.cos(a), math.sin(a), 0]) * rh * 0.55 * (0.5 + 0.5 * rng.random())
        o.ball(p, 0.05 + 0.02 * rng.random(), "coal", wh, sides=6, rings=3, axis=_unit(rng.random(3) - 0.5))
    o.ball(c + np.array([0, 0, 0.12]), 0.07, "coal", wh, sides=7, rings=3)
    for k in range(9):  # flames
        a = 2 * math.pi * k / 9 + 0.2
        p = c + np.array([0, 0, 0.12]) + np.array([math.cos(a), math.sin(a), 0]) * rh * 0.45
        L = 0.28 + 0.2 * rng.random() + (0.15 if k % 3 == 0 else 0)
        o.lathe(p, np.array([0.04 * math.cos(a), 0.04 * math.sin(a), L]), [(0, 0.05), (0.35, 0.045), (1, 0.0)], "flame",
                wh, sides=5, twist=1.6, bend=(0.04 * math.sin(a), -0.04 * math.cos(a), 0))
    o.lathe(c + np.array([0, 0, 0.12]), np.array([0, 0.02, 0.62]), [(0, 0.08), (0.3, 0.07), (1, 0.0)], "flame", wh,
            sides=6, twist=2.0)
    # Chest grate: bars over a glowing coal bed.
    sp3 = B.head["spine_03"]
    ch = B.surf(sp3 + np.array([0, 0, 0.02]), (0, -1, 0), rad=0.05, bones=TORSO)
    wc = B.near(ch, only=TORSO)
    o.box(ch + np.array([0, -0.005, 0]), (1, 0, 0), (0, 0, 1), (0.13, 0.12, 0.03), "coal", wc)
    for k in range(5):
        x = -0.11 + 0.055 * k
        o.lathe(ch + np.array([x, -0.04, -0.14]), np.array([0, 0, 0.28]), [(0, 0.014), (1, 0.014)], "grate_iron", wc,
                sides=5)
    for z in (-0.12, 0.12):
        o.lathe(ch + np.array([-0.15, -0.045, z]), np.array([0.30, 0, 0]), [(0, 0.017), (1, 0.017)], "grate_iron", wc,
                sides=5)
    # Flues off the shoulders (between neck and shoulder joint: the arms stay clear).
    for side, sx in (("l", 1), ("r", -1)):
        q = (B.head["neck_01"] + B.head[f"upperarm_{side}"]) / 2 + np.array([0, 0.04, 0])
        p = B.surf(q, (0, 0.3, 1), rad=0.05, bones=TORSO + (f"clavicle_{side}",)) - np.array([0, 0, 0.04])
        w = {"spine_03": 0.6, f"clavicle_{side}": 0.4}
        o.lathe(p, np.array([sx * 0.08, 0.05, 0.42]), [(0, 0.07), (0.85, 0.06), (0.88, 0.085), (1, 0.08)], "grate_iron",
                w, sides=8)
        o.lathe(p + np.array([sx * 0.08, 0.05, 0.40]), np.array([sx * 0.02, 0.01, 0.22]), [(0, 0.06), (1, 0.0)], "flame",
                w, sides=6, twist=1.5)
        # Ember shards along the outer forearm.
        fa = bone_dir(B, f"lowerarm_{side}")
        el = B.head[f"lowerarm_{side}"]
        for k in range(3):
            pp = B.surf(el + fa * (0.08 + 0.08 * k), (sx, 0.4, 0.2), rad=0.05, bones=(f"lowerarm_{side}",))
            o.crystal(pp - np.array([sx * 0.015, 0, 0]), np.array([sx * 0.7, 0.6, 0.3]), 0.14 + 0.05 * rng.random(), 0.03,
                      "cinder", {f"lowerarm_{side}": 1.0}, sides=5)
    return o


# ---------------------------------------------------------------------------------------------
# M15 zone bosses (scope addition): each the master of its zone's family, a crown silhouette.

def stubble_lord_parts(B, H, rng):
    """Anız Ağası (Tanyeri boss): a crown of charred sheaves round the helm, a straw mantle over the
    back and pauldrons, straw bursting from the vambraces, a rope girdle with hanging ears."""
    o = P15()
    hc, htop, HV = head_info(B)
    wh = {"Head": 1.0}
    rh = float(np.percentile(np.linalg.norm(HV[:, :2] - hc[:2], axis=1), 88)) + 0.015
    zc = htop - 0.10
    o.lathe(np.array([hc[0], hc[1], zc - 0.03]), np.array([0, 0, 0.06]), [(0, rh), (1, rh + 0.01)], "rope", wh, sides=12)
    for k in range(11):  # sheaves splayed up and out like a crown, each bursting into ears
        a = 2 * math.pi * (k + 0.5) / 11
        d0 = np.array([math.cos(a), math.sin(a), 0])
        base = np.array([hc[0], hc[1], zc]) + d0 * (rh + 0.01)
        d = _unit(d0 * 0.7 + np.array([0, 0, 1.0]))
        L = 0.22 + (0.08 if k % 2 == 0 else 0.0)
        o.lathe(base, d * L, [(0, 0.022), (0.5, 0.026), (1, 0.03)], "rope", wh, sides=6, twist=0.5)
        _, e1, e2 = _frame(d)
        for j in range(6):
            ang = 2 * math.pi * j / 6
            rad = math.cos(ang) * e1 + math.sin(ang) * e2
            o.spike(base + d * L * 0.9 + rad * 0.018, d + rad * 0.45, 0.13 + 0.07 * rng.random(), 0.011, "char_straw",
                    wh, sides=4)
    # Straw mantle: stalks from a collar line round the back of the neck, hanging down the back.
    nk, sp3 = B.head["neck_01"], B.head["spine_03"]
    for k in range(40):
        x = (k / 39 - 0.5) * 0.46
        z = (nk[2] + sp3[2]) / 2 + 0.02 - 0.12 * (abs(x) / 0.23) ** 2
        p = B.surf(np.array([x, 0.0, z]), (0, 1, 0.3), rad=0.04, bones=TORSO) - np.array([0, 0.01, 0])
        d = np.array([x * 1.2, 0.35 + 0.1 * rng.random(), -1.0])
        o.spike(p, d, 0.55 + 0.2 * rng.random(), 0.017, "char_straw", B.near(p, only=TORSO), sides=4)
    for side, sx in (("l", 1), ("r", -1)):
        # Straw over the pauldrons, fanning out (shoulder blend like the pauldron itself).
        sh = B.head[f"upperarm_{side}"]
        top = B.surf(sh + np.array([0, 0, 0.04]), (0.4 * sx, 0, 1))
        w = {f"clavicle_{side}": 0.5, f"upperarm_{side}": 0.5}
        for k in range(12):
            d = np.array([sx * (0.7 + 0.5 * rng.random()), (rng.random() - 0.5) * 1.0, -0.2 + 0.6 * rng.random()])
            o.spike(top + (rng.random(3) - 0.5) * np.array([0.08, 0.14, 0.03]), d, 0.20 + 0.12 * rng.random(), 0.014,
                    "char_straw", w, sides=4)
        p, d, r = o.band(B, f"lowerarm_{side}", 0.3, 0.025, "rope", height=0.06)
        _, e1, e2 = _frame(d)
        for k in range(7):
            ang = 2 * math.pi * k / 7
            dd = math.cos(ang) * e1 + math.sin(ang) * e2
            o.spike(p + dd * (r + 0.02), -d * 0.8 + dd * 0.6, 0.13 + 0.05 * rng.random(), 0.012, "straw",
                    {f"lowerarm_{side}": 1.0}, sides=4)
    return o


def bellows_khan_parts(B, H, rng):
    """Körük Hanı (Demirağız boss): a tall frost-iron crown of spikes, a great bellows on the back, a
    beard grown into long icicles, ice shards on the pauldrons."""
    o = P15()
    hc, htop, HV = head_info(B)
    wh = {"Head": 1.0}
    rh = float(np.percentile(np.linalg.norm(HV[:, :2] - hc[:2], axis=1), 88)) + 0.02
    zc = htop - 0.07
    o.lathe(np.array([hc[0], hc[1], zc - 0.04]), np.array([0, 0, 0.10]), [(0, rh), (1, rh + 0.03)], "frost_iron", wh,
            sides=10)
    for k in range(10):
        a = 2 * math.pi * (k + 0.5) / 10
        d0 = np.array([math.cos(a), math.sin(a), 0])
        base = np.array([hc[0], hc[1], zc + 0.04]) + d0 * (rh + 0.01)
        front = -d0[1]  # tallest at the front
        L = 0.22 + 0.16 * max(0.0, front) + (0.06 if k % 2 else 0.0)
        if k % 2:
            o.crystal(base, d0 * 0.2 + np.array([0, 0, 1.0]), L, 0.035, "ice", wh, sides=6, twist=0.3)
        else:
            o.spike(base, d0 * 0.15 + np.array([0, 0, 1.0]), L * 0.8, 0.03, "frost_iron", wh, sides=4)
    add_bellows(o, B, rng, 1.3)
    frost_trim(o, B, rng, beard_icicles=14, beard_len=0.16)
    for side, sx in (("l", 1), ("r", -1)):
        sh = B.head[f"upperarm_{side}"]
        top = B.surf(sh + np.array([0, 0, 0.03]), (0.5 * sx, 0, 1)) - np.array([0, 0, 0.02])
        w = B.near(top)
        for k in range(5):
            d = np.array([sx * (0.6 + 0.4 * rng.random()), (k - 2) * 0.25, 1.0])
            o.crystal(top + np.array([0, (k - 2) * 0.05, 0]), d, 0.20 + 0.12 * rng.random(), 0.04, "ice", w, sides=6)
    return o


def slag_matron_parts(B, H, rng):
    """Cüruf Anası (Ortaocak boss): three tiers of cooled-slag skirt with glowing hems (skinned to the
    thighs by side, the lowest tier's back to the calves too), a molten crown of slag spikes, slag
    plates on the shoulders, molten drips at the hands."""
    o = P15()
    pel = B.head["pelvis"]
    knee = B.head["calf_l"][2]
    ankle = B.head["foot_l"][2]
    tiers = ((pel[2] + 0.14, pel[2] - 0.12, 0.05, 0.10), (pel[2] + 0.02, knee + 0.02, 0.10, 0.10),
             (pel[2] - 0.12, ankle + 0.17, 0.15, 0.08))
    for ti, (z0, z1, pad, flare) in enumerate(tiers):
        R, C = 6, 22
        P = np.zeros((R, C, 3))
        N = np.zeros((R, C, 3))
        hem = [0.06 * rng.random() for _ in range(C)]
        for r in range(R):
            t = r / (R - 1)
            z = z0 + (z1 - z0) * t
            for c in range(C):
                a = 2 * math.pi * (c + 0.5 * ti) / C
                d = np.array([math.sin(a), -math.cos(a), 0.0])
                s = B.surf(np.array([0, 0, z]), d, rad=0.07, bones=("pelvis", "spine_01", "thigh_l", "thigh_r", "calf_l",
                                                                    "calf_r", "cloth_front", "cloth_back"))
                rad = max(float(np.linalg.norm(s[:2])), 0.13) + pad + flare * t
                P[r, c] = np.array([0, 0.01, z - hem[c] * t ** 3]) + d * rad
                N[r, c] = d

        def wfn(r, c, ti=ti, R=R, C=C, z0=z0, z1=z1):
            t = r / (R - 1)
            z = z0 + (z1 - z0) * t
            a = 2 * math.pi * (c + 0.5 * ti) / C
            wl, wr_ = side_weights(math.sin(a), 1.4)
            depth = float(np.clip((pel[2] - z) / max(pel[2] - knee, 0.1), 0, 1))  # 0 at the hip, 1 at the knee
            legs = 0.15 + 0.7 * depth
            w = {"pelvis": 1.0 - legs, "thigh_l": legs * wl, "thigh_r": legs * wr_}
            back = max(0.0, math.cos(a + math.pi))
            if ti == 2 and back > 0 and z < knee + 0.1:
                k = 0.35 * back * min(1.0, (knee + 0.1 - z) / 0.3)
                w["calf_l"], w["calf_r"] = k * wl, k * wr_
            return {n: v for n, v in w.items() if v > 1e-3}
        o.skirt(P, N, 0.03, "slag_plate", wfn)
    # Molten crown.
    hc, htop, HV = head_info(B)
    wh = {"Head": 1.0}
    rh = float(np.percentile(np.linalg.norm(HV[:, :2] - hc[:2], axis=1), 88)) + 0.02
    zc = htop - 0.08
    o.lathe(np.array([hc[0], hc[1], zc - 0.03]), np.array([0, 0, 0.07]), [(0, rh), (1, rh + 0.02)], "slag_plate", wh,
            sides=10)
    for k in range(11):
        a = 2 * math.pi * (k + 0.5) / 11
        d0 = np.array([math.cos(a), math.sin(a), 0])
        base = np.array([hc[0], hc[1], zc + 0.03]) + d0 * (rh + 0.01)
        L = 0.24 + 0.12 * (k % 2) + 0.06 * rng.random()
        o.spike(base, d0 * 0.45 + np.array([0, 0, 1.0]), L, 0.035, "slag_plate", wh, sides=4, hint=(0, 0, 1))
        o.ball(base + d0 * 0.02 + np.array([0, 0, 0.02]), 0.03, "cinder", wh, sides=6, rings=3)
    o.lathe(np.array([hc[0], hc[1], zc + 0.04]), np.array([0, 0, 0.30]), [(0, 0.06), (0.3, 0.05), (1, 0.0)], "flame", wh,
            sides=6, twist=1.8)
    for side, sx in (("l", 1), ("r", -1)):
        sh = B.head[f"upperarm_{side}"]
        top = B.surf(sh + np.array([0, 0, 0.03]), (0.4 * sx, 0, 1)) + np.array([0, 0, 0.01])
        w = {f"clavicle_{side}": 0.5, f"upperarm_{side}": 0.5}
        o.prism(top, np.array([1.0, 0, -0.5 * sx]), np.array([0, 1.0, 0]),
                [(-0.13, -0.15), (0.12, -0.17), (0.16, 0.02), (0.11, 0.16), (-0.12, 0.14), (-0.16, 0.0)], 0.06,
                "slag_plate", w)
        hand = B.head[f"hand_{side}"]
        fa = bone_dir(B, f"lowerarm_{side}")
        for k in range(4):  # molten drips off the fingers
            p = hand + fa * (0.08 + 0.03 * k) + (rng.random(3) - 0.5) * 0.06
            o.lathe(p, np.array([0, 0, -0.08 - 0.05 * rng.random()]), [(0, 0.022), (0.7, 0.016), (1, 0.0)], "cinder",
                    {f"hand_{side}": 1.0}, sides=5)
    return o


SHAPES = {
    "reaper": dict(swatches=("char_straw", "scythe", "rope", "hat_straw"), parts=reaper_parts),
    "brute": dict(swatches=("straw", "rope", "burnt_iron", "char_straw"), parts=brute_parts),
    "bellows": dict(swatches=("frost_wood", "frost_leather", "frost_iron", "ice", "frost_rock"), parts=bellows_parts),
    "anvil": dict(swatches=("frost_iron", "ice"), parts=anvil_parts),
    "stalker": dict(swatches=("soot", "claw", "cinder"), parts=stalker_parts),
    "bulwark": dict(swatches=("slag_plate",), parts=bulwark_parts),
    "warlock": dict(swatches=("ash_robe", "ember_core", "grate_iron", "cinder"), parts=warlock_parts),
    "hearth": dict(swatches=("grate_iron", "coal", "flame", "cinder"), parts=hearth_parts),
    "stubble_lord": dict(swatches=("char_straw", "straw", "rope"), parts=stubble_lord_parts),
    "bellows_khan": dict(swatches=("frost_wood", "frost_leather", "frost_iron", "ice", "frost_rock"),
                         parts=bellows_khan_parts),
    "slag_matron": dict(swatches=("slag_plate", "cinder", "flame"), parts=slag_matron_parts),
}
ms.SHAPES.update(SHAPES)
