"""M12 "Yetenek Yolları" 2D art: 24 branch skill icons, the skill bar / skill tree UI pieces and the
buff / debuff status icons. Same painter as the four M9 icons (`gen_effects.py`: signed distance
fields, 4x supersampled, box-filtered on premultiplied linear colour), so the new icons sit next to
the old ones as one set.

Usage (headless):
  blender --background --factory-startup --python tools/blender/gen_skill_art.py -- [--sheets <dir>]
          [--only icons|status|ui] [--no-preview]

Outputs (all straight sRGB alpha PNGs; assetc writes the premultiplied RGBA8 .vtex next to each copy):
  assets_src/ui/skills/<id>.png      64x64 skill icons (rounded badge in the branch colour, gold rim)
  assets_src/ui/status/<id>.png      48x48 buff / debuff icons (read at 24 px: mip 1 is an exact 2x box)
  assets_src/ui/skillbar/<id>.png    bar slot frames, overlays, key badge, lock, rank pips, tree
                                     connectors, branch headers, skill point marker
Contact sheets (not assets; default <repo>/build/art_sheets/m12, git-ignored):
  m12_icons.png    all 28 skill icons per class: 64 px and 48 px icons in bar slots
  m12_status.png   status icons at 24 px (and 48 px) on the HUD colour
  m12_ui.png       every UI piece at 1x and 2x on the panel colour, plus a mock bar and a mock tree
Deterministic: no time, no randomness.
"""

import math
import os
import sys

import numpy as np

sys.dont_write_bytecode = True
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import vo_common as vc  # noqa: E402
import gen_effects as ge  # noqa: E402  (the M9 icon painter)
from gen_effects import (sd_round_rect, sd_circle, sd_stroke, sd_polygon, arc_points, taper,  # noqa: E402
                         hexc, lerp_field, _lin, _srgb)

SKILL_DIR = os.path.join(vc.SRC, "ui", "skills")
STATUS_DIR = os.path.join(vc.SRC, "ui", "status")
BAR_DIR = os.path.join(vc.SRC, "ui", "skillbar")
DEFAULT_SHEETS = os.path.join(vc.REPO, "build", "art_sheets", "m12")

SS = 4
INK = "#0b0c10"   # outline colour of every symbol (same as the M9 icons)
GOLD_RIM = "#c9a052"


# ---------------------------------------------------------------------------------------------
# Canvas of any size. Coordinates in `unit` pixels (icons: unit = size, so 0..1; UI art: unit = 1,
# so pixels), y down. Same compositing as gen_effects.Canvas.

class Canvas:
    def __init__(self, w, h=None, unit=None):
        h = w if h is None else h
        self.w, self.h = w, h
        self.unit = float(w if unit is None else unit)
        self.n = w * SS  # gen_effects.badge uses (n, n): only square canvases call it
        cx = (np.arange(w * SS) + 0.5) / (SS * self.unit)
        cy = (np.arange(h * SS) + 0.5) / (SS * self.unit)
        self.x, self.y = np.meshgrid(cx, cy)
        self.px = 1.0 / (SS * self.unit)
        self.rgba = np.zeros((h * SS, w * SS, 4))

    def cover(self, sdf, soft=1.0):
        return np.clip(0.5 - sdf / (self.px * soft), 0.0, 1.0)

    def over(self, alpha, color):
        col = _lin(color)
        if col.ndim == 1:
            col = np.broadcast_to(col, self.rgba.shape[:2] + (3,))
        a = alpha[..., None]
        self.rgba[..., :3] = col * a + self.rgba[..., :3] * (1.0 - a)
        self.rgba[..., 3:] = a + self.rgba[..., 3:] * (1.0 - a)

    def result(self):
        pm = self.rgba.reshape(self.h, SS, self.w, SS, 4).mean(axis=(1, 3))
        a = pm[..., 3:4]
        col = np.where(a > 1e-6, pm[..., :3] / np.maximum(a, 1e-6), 0.0)
        return np.concatenate([_srgb(col), a], -1)


def fill(cv, sdf, color, alpha=1.0, soft=1.0):
    cv.over(cv.cover(sdf, soft) * alpha, color if not isinstance(color, str) else hexc(color))


def symbol(cv, sdf, color=None, width=0.035, shade=None, outline=INK):
    """Dark outline `width` wide, then the fill (a hex colour or an (h, w, 3) sRGB field)."""
    cv.over(cv.cover(sdf - width), hexc(outline))
    cv.over(cv.cover(sdf), shade if shade is not None else hexc(color))


def glow(cv, cx, cy, r, color, strength=0.5):
    g = np.clip(1.0 - np.hypot(cv.x - cx, cv.y - cy) / r, 0, 1) ** 2
    cv.over(g * strength, hexc(color))


# ---------------------------------------------------------------------------------------------
# Shape helpers (icon units)

def rot(x, y, cx, cy, deg):
    """Coordinates rotated by -deg about (cx, cy): evaluate a shape defined upright, drawn rotated by deg."""
    a = math.radians(deg)
    c, s = math.cos(a), math.sin(a)
    dx, dy = x - cx, y - cy
    return cx + dx * c + dy * s, cy - dx * s + dy * c


def union(*ds):
    out = ds[0]
    for d in ds[1:]:
        out = np.minimum(out, d)
    return out


def sub(a, b):
    return np.maximum(a, -b)


def sd_ellipse(x, y, cx, cy, rx, ry):
    return (np.hypot((x - cx) / rx, (y - cy) / ry) - 1.0) * min(rx, ry)


def sd_ring(x, y, cx, cy, r, w):
    return np.abs(np.hypot(x - cx, y - cy) - r) - w


def star_pts(cx, cy, r_out, r_in, n, rot_deg=-90.0):
    pts = []
    for k in range(2 * n):
        a = math.radians(rot_deg + 180.0 * k / n)
        r = r_out if k % 2 == 0 else r_in
        pts.append((cx + r * math.cos(a), cy + r * math.sin(a)))
    return pts


def sd_star(x, y, cx, cy, r_out, r_in, n, rot_deg=-90.0):
    return sd_polygon(x, y, star_pts(cx, cy, r_out, r_in, n, rot_deg))


def sd_flame(x, y, cx, cy, h, w, lean=0.0, wave=0.0):
    """Flame tongue standing on (cx, cy) (its round base), tip h above; lean shifts the tip in x."""
    n = 18
    pts, ws = [], []
    for k in range(n):
        t = k / (n - 1)               # 0 base -> 1 tip
        px = cx + lean * t * t + wave * math.sin(t * math.pi * 2.0) * t
        py = cy - h * t
        pts.append((px, py))
        ws.append(w * (1.0 - t) ** 0.8 * (0.75 + 0.25 * math.sin(math.pi * min(1.0, t * 1.6))) + 0.004)
    return union(sd_stroke(x, y, pts, ws), sd_circle(x, y, cx, cy + w * 0.15, w * 0.95))


def sd_drop(x, y, cx, cy, r, h):
    """Teardrop: circle of radius r at (cx, cy), tip h above the centre."""
    tip = (cx, cy - h)
    ang = math.asin(min(0.999, r / h))
    tx, ty = r * math.cos(ang), r * math.sin(ang)
    tri = sd_polygon(x, y, [tip, (cx + tx, cy - ty), (cx - tx, cy - ty)])
    return union(sd_circle(x, y, cx, cy, r), tri)


def sd_heart(x, y, cx, cy, s):
    l = sd_circle(x, y, cx - 0.5 * s, cy - 0.25 * s, 0.56 * s)
    r = sd_circle(x, y, cx + 0.5 * s, cy - 0.25 * s, 0.56 * s)
    tri = sd_polygon(x, y, [(cx - 1.02 * s, cy - 0.05 * s), (cx + 1.02 * s, cy - 0.05 * s), (cx, cy + 1.05 * s)])
    return union(l, r, tri)


def sd_kite(x, y, cx, cy, w, h):
    """Heater shield centred on (cx, cy): flat top at cy - h, straight sides to just above the
    centre, then curving into the point at cy + h."""
    y0 = cy - 0.15 * h
    pts = [(cx - w, cy - h), (cx + w, cy - h)]
    n = 12
    for k in range(0, n + 1):
        t = k / n
        pts.append((cx + w * (1 - t ** 2.0), y0 + (cy + h - y0) * t))
    for k in range(n - 1, -1, -1):
        t = k / n
        pts.append((cx - w * (1 - t ** 2.0), y0 + (cy + h - y0) * t))
    return sd_polygon(x, y, pts) - 0.008


def sd_arrow_up(x, y, cx, cy, w, h):
    """Upward arrow: head on top, shaft below; (cx, cy) = centre."""
    head = sd_polygon(x, y, [(cx, cy - h * 0.5), (cx + w * 0.5, cy - h * 0.05), (cx - w * 0.5, cy - h * 0.05)])
    shaft = sd_round_rect(x, y, cx, cy + h * 0.2, w * 0.17, h * 0.28, 0.004)
    return union(head, shaft)


def sd_plus(x, y, cx, cy, s, t):
    return union(sd_round_rect(x, y, cx, cy, t, s, t * 0.4), sd_round_rect(x, y, cx, cy, s, t, t * 0.4))


def sd_snowflake(x, y, cx, cy, r, w):
    d = np.full(x.shape, 1e9)
    for k in range(6):
        a = math.radians(k * 60 - 90)
        ca, sa = math.cos(a), math.sin(a)
        d = np.minimum(d, sd_stroke(x, y, [(cx, cy), (cx + ca * r, cy + sa * r)], [w, w]))
        bx, by = cx + ca * r * 0.58, cy + sa * r * 0.58
        for side in (-1, 1):
            b = a + side * math.radians(48)
            d = np.minimum(d, sd_stroke(x, y, [(bx, by), (bx + math.cos(b) * r * 0.34, by + math.sin(b) * r * 0.34)],
                                        [w * 0.8, w * 0.8]))
    return d


def blade_shade(x, y, hx, hy, tx, ty, light="#f4f6fa", dark="#8d97a6"):
    """Two-tone steel across a blade from (hx, hy) to (tx, ty) (as agir_darbe)."""
    L = math.hypot(tx - hx, ty - hy)
    u = ((x - hx) * (tx - hx) + (y - hy) * (ty - hy)) / (L * L)
    side = ((x - hx) * (hy - ty) - (y - hy) * (hx - tx)) / L
    return lerp_field(np.where(side > 0.0, 0.0, 0.55) + u * 0.2, light, dark)


def sd_blade(x, y, hx, hy, tx, ty, w0, w1=None):
    """Straight double-edged blade from the hilt (hx, hy) to the point (tx, ty): half-width w0 at
    the hilt, w1 at 80 % of the length, then the point."""
    w1 = w0 * 0.85 if w1 is None else w1
    L = math.hypot(tx - hx, ty - hy)
    ux, uy = (tx - hx) / L, (ty - hy) / L
    nx, ny = -uy, ux
    mx, my = hx + ux * L * 0.78, hy + uy * L * 0.78
    pts = [(hx + nx * w0, hy + ny * w0), (mx + nx * w1, my + ny * w1), (tx, ty),
           (mx - nx * w1, my - ny * w1), (hx - nx * w0, hy - ny * w0)]
    return sd_polygon(x, y, pts)


def draw_sword(cv, hx, hy, tx, ty, w=0.05, guard=0.12, grip=0.15, steel=("#f4f6fa", "#8d97a6"),
               hilt=("#e8c060", "#8a5a1c"), outline=0.035):
    """Blade from the hilt (hx, hy) to the point, cross guard and grip behind the hilt."""
    x, y = cv.x, cv.y
    L = math.hypot(tx - hx, ty - hy)
    ux, uy = (tx - hx) / L, (ty - hy) / L
    nx, ny = -uy, ux
    blade = sd_blade(x, y, hx, hy, tx, ty, w)
    g = sd_stroke(x, y, [(hx - nx * guard, hy - ny * guard), (hx + nx * guard, hy + ny * guard)], [0.03, 0.03])
    gx, gy = hx - ux * grip, hy - uy * grip
    gr = sd_stroke(x, y, [(hx, hy), (gx, gy)], [0.026, 0.024])
    pm = sd_circle(x, y, gx - ux * 0.02, gy - uy * 0.02, 0.036)
    h = union(g, gr, pm)
    cv.over(cv.cover(union(blade, h) - outline), hexc(INK))
    cv.over(cv.cover(blade), blade_shade(x, y, hx, hy, tx, ty, *steel))
    cv.over(cv.cover(h), lerp_field((y - min(hy, gy)) / 0.25, *hilt))


def draw_dagger(cv, hx, hy, tx, ty, w=0.045, guard=0.08, grip=0.11, steel=("#f4f6fa", "#8d97a6"),
                hilt=("#9a8ab0", "#3a2c4a"), outline=0.035):
    draw_sword(cv, hx, hy, tx, ty, w=w, guard=guard, grip=grip, steel=steel, hilt=hilt, outline=outline)


def slash(cv, pts_fn, n, wmax, c0, c1, outline=0.035):
    """A tapered cut along pts_fn(t) (as cifte_kesik): bright edge -> colour."""
    pts = [pts_fn(k / (n - 1)) for k in range(n)]
    d = sd_stroke(cv.x, cv.y, pts, taper(n, 0.004, wmax, 0.004))
    cv.over(cv.cover(d - outline), hexc(INK))
    g = np.clip(1.0 - np.abs(d) / wmax, 0, 1)
    cv.over(cv.cover(d), lerp_field(1.0 - g, c0, c1))
    return d


# ---------------------------------------------------------------------------------------------
# Branch palettes: badge (top, bottom). The rim stays gold for the whole set (as M9).

BRANCH = {
    "hucum": ("#8c1c22", "#2a0609"),   # Savaşçı · Hücum: warm red
    "siper": ("#4e5c6c", "#121820"),   # Savaşçı · Siper: blue-grey (bronze symbols)
    "pusu": ("#4a2a62", "#10081a"),    # Haydut · Pusu: dark violet (steel symbols)
    "zehir": ("#58731a", "#141e05"),   # Haydut · Zehir: sickly green
    "koz": ("#a04a10", "#2a0e03"),     # Büyücü · Köz: ember orange
    "kiragi": ("#5f93bc", "#0f2338"),  # Büyücü · Kırağı: pale frost blue
    "sifa": ("#9c8538", "#2c2408"),    # Şifacı · Şifa: pale gold
    "yargi": ("#8f897c", "#1c1a17"),   # Şifacı · Yargı: ivory stone (white / amber symbols)
}


def badge(cv, branch):
    top, bottom = BRANCH[branch]
    ge.badge(cv, top, bottom, GOLD_RIM)


# ---- Savaşçı · Hücum ------------------------------------------------------------------------

def icon_yarma(cv):
    """Cleave around self: a nearly closed sweeping arc round a broad axe."""
    badge(cv, "hucum")
    x, y = cv.x, cv.y
    cx, cy = 0.5, 0.52
    pts = arc_points(cx, cy, 0.335, 160, 160 + 300, 40)
    arc = sd_stroke(x, y, pts, [0.006 + 0.062 * (k / 39) ** 1.2 for k in range(40)])
    arc = sub(arc, sd_circle(x, y, cx + 0.03, cy - 0.02, 0.27))
    t = np.clip((np.arctan2(y - cy, x - cx) % (2 * math.pi)) / (2 * math.pi), 0, 1)
    symbol(cv, arc, shade=lerp_field(np.abs(t - 0.4), "#fff6c8", "#ff7a28"), width=0.03)
    # Arrow head at the leading end (angle 460 = 100 deg: bottom), pointing along the sweep.
    a = math.radians(100)
    ex, ey = cx + 0.335 * math.cos(a), cy + 0.335 * math.sin(a)
    tx, ty = -math.sin(a), math.cos(a)     # tangent (increasing angle)
    nx, ny = math.cos(a), math.sin(a)
    head = sd_polygon(x, y, [(ex + tx * 0.11, ey + ty * 0.11), (ex + nx * 0.1, ey + ny * 0.1),
                             (ex - nx * 0.1, ey - ny * 0.1)])
    symbol(cv, head, "#ffb050", width=0.03)
    # Axe: haft from lower right to upper left of the centre, bearded head at the top.
    hx0, hy0, hx1, hy1 = 0.64, 0.74, 0.40, 0.30
    haft = sd_stroke(x, y, [(hx0, hy0), (hx1, hy1)], [0.028, 0.024])
    hb = sd_polygon(x, y, [(0.37, 0.25), (0.47, 0.30), (0.45, 0.40), (0.34, 0.50), (0.26, 0.47),
                           (0.24, 0.33), (0.30, 0.25)])
    cv.over(cv.cover(union(haft, hb) - 0.035), hexc(INK))
    cv.over(cv.cover(haft), lerp_field((y - 0.3) / 0.45, "#c08a48", "#5a3416"))
    cv.over(cv.cover(hb), lerp_field((x - 0.24) / 0.24, "#f6f8fb", "#8a94a4"))
    edge = sd_stroke(x, y, [(0.30, 0.25), (0.24, 0.33), (0.26, 0.47)], [0.012, 0.012])
    cv.over(cv.cover(np.maximum(edge, hb)), hexc("#ffffff"))


def icon_kan_ofkesi(cv):
    """Rage: a big blood drop with an upward arrow, a glow of fury behind."""
    badge(cv, "hucum")
    x, y = cv.x, cv.y
    glow(cv, 0.5, 0.6, 0.45, "#ff5030", 0.45)
    drop = sd_drop(x, y, 0.5, 0.6, 0.255, 0.48)
    r = np.hypot(x - 0.42, y - 0.5) / 0.4
    symbol(cv, drop, shade=lerp_field(r, "#ff5a48", "#7a0a0c"), width=0.035)
    hl = sd_ellipse(x, y, 0.4, 0.5, 0.045, 0.085)
    cv.over(cv.cover(hl) * 0.7, hexc("#ffd0c0"))
    arrow = sd_arrow_up(x, y, 0.535, 0.62, 0.26, 0.34)
    symbol(cv, arrow, shade=lerp_field((y - 0.45) / 0.35, "#fff4c0", "#f0b040"), width=0.03)


def icon_sarsinti(cv):
    """Stunning blow: a war hammer striking a jagged starburst, two dizzy stars above."""
    badge(cv, "hucum")
    x, y = cv.x, cv.y
    burst = sd_star(x, y, 0.42, 0.62, 0.31, 0.15, 9, -80)
    r = np.hypot(x - 0.42, y - 0.62) / 0.31
    symbol(cv, burst, shade=lerp_field(r, "#fffbe0", "#ff9a30"), width=0.03)
    # Hammer: handle from the upper right, head across it near the burst.
    rx, ry = rot(x, y, 0.56, 0.44, -40)
    handle = sd_stroke(rx, ry, [(0.56, 0.44), (0.56, 0.05)], [0.03, 0.026])
    head = sd_round_rect(rx, ry, 0.56, 0.47, 0.16, 0.085, 0.02)
    cv.over(cv.cover(union(handle, head) - 0.035), hexc(INK))
    cv.over(cv.cover(handle), lerp_field((ry - 0.05) / 0.4, "#b07a40", "#5a3416"))
    cv.over(cv.cover(head), lerp_field((ry - 0.39) / 0.16, "#f2f4f8", "#6d7686"))
    band = sd_round_rect(rx, ry, 0.56, 0.47, 0.04, 0.09, 0.01)
    cv.over(cv.cover(np.maximum(band, head)), hexc("#c9a052"))
    for (sx, sy, s) in ((0.2, 0.24, 0.07), (0.34, 0.14, 0.05)):
        symbol(cv, sd_star(x, y, sx, sy, s, s * 0.42, 4), "#ffe27a", width=0.022)


# ---- Savaşçı · Siper ------------------------------------------------------------------------

def _shield(cv, cx, cy, w, h, face=("#9fb0c4", "#3a4656"), rim="#d09a50", boss=True):
    x, y = cv.x, cv.y
    s = sd_kite(x, y, cx, cy, w, h)
    cv.over(cv.cover(s - 0.035), hexc(INK))
    cv.over(cv.cover(s), hexc(rim))
    inner = sd_kite(x, y, cx, cy + 0.006, w - 0.03, h - 0.035)
    cv.over(cv.cover(inner), lerp_field((x - cx + w) / (2 * w) * 0.6 + (y - cy + h * 0.45) / (2 * h) * 0.6,
                                        *face))
    if boss:
        b = sd_circle(x, y, cx, cy + h * 0.1, w * 0.28)
        cv.over(cv.cover(b - 0.012), hexc(INK))
        cv.over(cv.cover(b), lerp_field(np.hypot(x - cx + 0.01, y - cy - h * 0.1 + 0.01) / (w * 0.3), "#ffe0a0", "#9a6a28"))


def icon_kalkan_duvari(cv):
    """Shield wall: three overlapping shields, the middle one in front, an up chevron above."""
    badge(cv, "siper")
    _shield(cv, 0.27, 0.53, 0.17, 0.25, boss=False)
    _shield(cv, 0.73, 0.53, 0.17, 0.25, boss=False)
    _shield(cv, 0.5, 0.56, 0.2, 0.28)
    x, y = cv.x, cv.y
    chev = sd_stroke(x, y, [(0.38, 0.2), (0.5, 0.11), (0.62, 0.2)], [0.03, 0.03, 0.03])
    symbol(cv, chev, "#f0d080", width=0.028)


def icon_meydan_okuma(cv):
    """Taunting shout: a curved war horn blowing, three sound waves out of the bell."""
    badge(cv, "siper")
    x, y = cv.x, cv.y
    n = 24
    pts = []
    p0, p1, p2 = (0.16, 0.84), (0.2, 0.42), (0.55, 0.42)   # quadratic Bezier: mouthpiece -> bell
    for k in range(n):
        t = k / (n - 1)
        pts.append(((1 - t) ** 2 * p0[0] + 2 * (1 - t) * t * p1[0] + t * t * p2[0],
                    (1 - t) ** 2 * p0[1] + 2 * (1 - t) * t * p1[1] + t * t * p2[1]))
    ws = [0.02 + 0.09 * (k / (n - 1)) ** 1.8 for k in range(n)]
    horn = sd_stroke(x, y, pts, ws)
    bx, by = pts[-1]
    bell = sd_ellipse(x, y, bx, by, 0.045, 0.125)
    whole = union(horn, bell)
    cv.over(cv.cover(whole - 0.035), hexc(INK))
    cv.over(cv.cover(horn), lerp_field((x - 0.17) / 0.6, "#f2e2b8", "#c8903e"))
    for t0 in (0.4, 0.75):  # two dark bronze bands across the horn
        bi = int(t0 * (n - 1))
        (ax, ay), (bx2, by2) = pts[bi - 1], pts[bi + 1]
        tx, ty = bx2 - ax, by2 - ay
        L = math.hypot(tx, ty)
        along = np.abs(((x - pts[bi][0]) * tx + (y - pts[bi][1]) * ty) / L) - 0.012
        cv.over(cv.cover(np.maximum(along, horn)), hexc("#7a5420"))
    cv.over(cv.cover(bell), hexc("#3a2a14"))
    cv.over(cv.cover(bell + 0.018), hexc("#120c06"))
    # Sound waves out of the bell (up and right).
    for k, r in enumerate((0.13, 0.22, 0.31)):
        wave = sd_stroke(x, y, arc_points(bx + 0.02, by, r, -55, 55, 16), [0.024 - k * 0.002] * 16)
        symbol(cv, wave, "#e8eef8", width=0.025)


def icon_direncli_soluk(cv):
    """Steady breath (self heal over time): three calm breath streams curling at their ends, a
    small green cross."""
    badge(cv, "siper")
    x, y = cv.x, cv.y
    d = np.full(x.shape, 1e9)
    for k, (y0, x1, r) in enumerate(((0.36, 0.66, 0.085), (0.54, 0.74, 0.075), (0.72, 0.6, 0.065))):
        n = 18
        line = [(0.14 + (x1 - 0.14) * i / (n - 1), y0 + 0.018 * math.sin(i * 0.7 + k)) for i in range(n)]
        # Curl: a spiral at the end turning up and back.
        cx, cy = x1, y0 - r
        curl = []
        for i in range(1, 22):
            t = i / 21
            a = math.radians(90 - 300 * t)
            rr = r * (1 - 0.55 * t)
            curl.append((cx + rr * math.cos(a), cy + rr * math.sin(a)))
        pts = line + curl
        ws = [0.02] * len(line) + [0.02 * (1 - 0.5 * i / 21) for i in range(1, 22)]
        d = np.minimum(d, sd_stroke(x, y, pts, ws))
    symbol(cv, d, shade=lerp_field((x - 0.14) / 0.7, "#ffffff", "#a8d4ee"), width=0.028)
    symbol(cv, sd_plus(x, y, 0.8, 0.2, 0.085, 0.03), "#7ee09a", width=0.028)


# ---- Haydut · Pusu --------------------------------------------------------------------------

def icon_sirt_hanceri(cv):
    """Heavy backstab: a broad dagger plunging point-down into a dark wound splash."""
    badge(cv, "pusu")
    x, y = cv.x, cv.y
    splash = sd_star(x, y, 0.5, 0.82, 0.17, 0.07, 7, -90)
    splash = union(splash, sd_ellipse(x, y, 0.5, 0.84, 0.2, 0.05))
    symbol(cv, splash, shade=lerp_field(np.hypot(x - 0.5, y - 0.82) / 0.2, "#ff6a6a", "#7a1020"), width=0.028)
    for sx in (0.24, 0.76):
        streak = sd_stroke(x, y, [(sx, 0.16), (sx, 0.52)], [0.004, 0.018])
        symbol(cv, streak, "#c8a8f0", width=0.022)
    draw_dagger(cv, 0.5, 0.33, 0.5, 0.84, w=0.075, guard=0.14, grip=0.17,
                hilt=("#c8a8e8", "#4a2c6a"))


def icon_golge_adimi(cv):
    """Shadow step: three fading violet chevrons leaping up-right into a bright target ring."""
    badge(cv, "pusu")
    x, y = cv.x, cv.y
    ring = sd_ring(x, y, 0.72, 0.29, 0.13, 0.03)
    symbol(cv, ring, shade=lerp_field(np.hypot(x - 0.72, y - 0.29) / 0.16, "#ffffff", "#d6a8ff"), width=0.028)
    symbol(cv, sd_circle(x, y, 0.72, 0.29, 0.035), "#ffffff", width=0.022)
    for k, (cx, cy, s) in enumerate(((0.23, 0.78, 0.85), (0.37, 0.64, 1.0), (0.51, 0.5, 1.15))):
        rx, ry = rot(x, y, cx, cy, -45)
        ch = sd_polygon(rx, ry, [(cx - 0.12 * s, cy - 0.13 * s), (cx + 0.07 * s, cy), (cx - 0.12 * s, cy + 0.13 * s),
                                 (cx - 0.06 * s, cy), ])
        col = ["#4a3070", "#7a52b0", "#c49cff"][k]
        col2 = ["#22143a", "#3e2468", "#7040c0"][k]
        symbol(cv, ch, shade=lerp_field((rx - cx + 0.12) / 0.2, col2, col), width=0.03)


def icon_seri_eller(cv):
    """Quick hands (attack speed): three daggers spinning as a pinwheel, motion arcs between."""
    badge(cv, "pusu")
    x, y = cv.x, cv.y
    cx, cy = 0.5, 0.5
    for k in range(3):
        a0 = -90 + k * 120 + 50
        arc = sd_stroke(x, y, arc_points(cx, cy, 0.36, a0, a0 + 55, 12), taper(12, 0.004, 0.022, 0.004))
        symbol(cv, arc, "#d8c0ff", width=0.022)
    for k in range(3):
        a = math.radians(-90 + k * 120)
        ca, sa = math.cos(a), math.sin(a)
        draw_dagger(cv, cx + ca * 0.1, cy + sa * 0.1, cx + ca * 0.42, cy + sa * 0.42, w=0.04, guard=0.06, grip=0.07,
                    hilt=("#c8a8e8", "#4a2c6a"), outline=0.03)
    symbol(cv, sd_circle(x, y, cx, cy, 0.05), "#e8c060", width=0.025)


# ---- Haydut · Zehir -------------------------------------------------------------------------

def icon_zehirli_kesik(cv):
    """Poisoned cut: one long curved slash dripping three green drops."""
    badge(cv, "zehir")
    x, y = cv.x, cv.y

    def p(t):
        bow = 0.08 * math.sin(math.pi * t)
        return (0.84 - 0.66 * t - bow, 0.14 + 0.6 * t - bow)

    slash(cv, p, 30, 0.07, "#ffffff", "#c6f060")
    for (dx, dy, r) in ((0.36, 0.78, 0.05), (0.56, 0.7, 0.042), (0.72, 0.56, 0.034)):
        dr = sd_drop(x, y, dx, dy, r, r * 2.1)
        symbol(cv, dr, shade=lerp_field(np.hypot(x - dx + r * 0.3, y - dy + r * 0.3) / (r * 1.6), "#e8ff9a", "#4e8a10"),
               width=0.026)


def icon_sis_bombasi(cv):
    """Fog bomb: a round iron bomb with a sparking fuse sinking into a sickly fog cloud."""
    badge(cv, "zehir")
    x, y = cv.x, cv.y
    bomb = sd_circle(x, y, 0.45, 0.42, 0.2)
    neck = sd_round_rect(*rot(x, y, 0.58, 0.26, 40), 0.58, 0.26, 0.05, 0.035, 0.01)
    cv.over(cv.cover(union(bomb, neck) - 0.035), hexc(INK))
    cv.over(cv.cover(bomb), lerp_field(np.hypot(x - 0.38, y - 0.35) / 0.28, "#8a94a0", "#20242a"))
    cv.over(cv.cover(neck), hexc("#5a6068"))
    fuse = sd_stroke(x, y, [(0.6, 0.24), (0.66, 0.17), (0.72, 0.16)], [0.014, 0.012, 0.01])
    symbol(cv, fuse, "#c8a060", width=0.02)
    symbol(cv, sd_star(x, y, 0.76, 0.14, 0.075, 0.03, 6), "#fff0a0", width=0.02)
    hl = sd_ellipse(x, y, 0.38, 0.33, 0.05, 0.03)
    cv.over(cv.cover(hl) * 0.8, hexc("#d8e0e8"))
    cloud = union(sd_circle(x, y, 0.2, 0.74, 0.12), sd_circle(x, y, 0.36, 0.68, 0.14), sd_circle(x, y, 0.56, 0.7, 0.15),
                  sd_circle(x, y, 0.76, 0.73, 0.12), sd_round_rect(x, y, 0.5, 0.8, 0.36, 0.07, 0.06))
    cv.over(cv.cover(cloud - 0.035), hexc(INK))
    cv.over(cv.cover(cloud), lerp_field((y - 0.56) / 0.3, "#e4f0b0", "#7a9a3a"))
    cv.over(cv.cover(sd_circle(x, y, 0.36, 0.66, 0.06)) * 0.5, hexc("#ffffff"))


def icon_kacamak(cv):
    """Evasion: a figure leaping sideways out of the way, two fading after-images behind it."""
    badge(cv, "zehir")
    x, y = cv.x, cv.y

    def figure(cx):
        rx, ry = rot(x, y, cx, 0.6, 18)  # leaning into the leap
        head = sd_circle(rx, ry, cx + 0.03, 0.27, 0.085)
        body = sd_stroke(rx, ry, [(cx + 0.02, 0.38), (cx - 0.01, 0.6)], [0.065, 0.055])
        legs = union(sd_stroke(rx, ry, [(cx - 0.01, 0.6), (cx - 0.08, 0.72), (cx - 0.2, 0.8)], [0.04, 0.035, 0.03]),
                     sd_stroke(rx, ry, [(cx - 0.01, 0.6), (cx + 0.1, 0.7), (cx + 0.08, 0.86)], [0.04, 0.035, 0.03]))
        arms = union(sd_stroke(rx, ry, [(cx + 0.02, 0.42), (cx + 0.15, 0.5), (cx + 0.22, 0.44)], [0.032, 0.028, 0.025]),
                     sd_stroke(rx, ry, [(cx + 0.02, 0.42), (cx - 0.1, 0.45), (cx - 0.18, 0.38)], [0.032, 0.028, 0.025]))
        return union(head, body, legs, arms)

    for k, (cx, a) in enumerate(((0.3, 0.3), (0.44, 0.55))):
        d = figure(cx)
        cv.over(cv.cover(d) * a, hexc(["#4e7016", "#7aa428"][k]))
    d = figure(0.6)
    symbol(cv, d, shade=lerp_field((y - 0.15) / 0.75, "#f4ffd0", "#9ad040"), width=0.034)
    for (yy, x0, x1) in ((0.22, 0.12, 0.3), (0.86, 0.1, 0.3)):
        line = sd_stroke(x, y, [(x0, yy), (x1, yy)], [0.004, 0.016])
        symbol(cv, line, "#e8ffb0", width=0.02)


# ---- Büyücü · Köz ---------------------------------------------------------------------------

def _fire(cv, d, cx, top, h, width=0.032):
    symbol(cv, d, shade=lerp_field((cv.y - top) / h, "#ffe070", "#c02a08"), width=width)


def icon_yakici_iz(cv):
    """Searing trail (burn over time): three flames along a scorched streak, growing as they go."""
    badge(cv, "koz")
    x, y = cv.x, cv.y
    streak = sd_stroke(x, y, [(0.1, 0.86), (0.5, 0.74), (0.9, 0.6)], [0.012, 0.03, 0.045])
    symbol(cv, streak, shade=lerp_field((x - 0.1) / 0.8, "#5a1a06", "#ff7a20"), width=0.025)
    for (fx, fy, h, w) in ((0.22, 0.82, 0.2, 0.06), (0.47, 0.75, 0.32, 0.085), (0.74, 0.66, 0.46, 0.11)):
        f = sd_flame(x, y, fx, fy, h, w, lean=0.03, wave=0.015)
        _fire(cv, f, fx, fy - h, h + w)
        core = sd_flame(x, y, fx, fy + w * 0.1, h * 0.55, w * 0.45, lean=0.015)
        cv.over(cv.cover(core), hexc("#fff8d8"))


def icon_alev_halkasi(cv):
    """Ring of flame: a burning ring on the ground seen from the side, flames standing round a dark
    caster in the middle (back flames behind it, front flames in front)."""
    badge(cv, "koz")
    x, y = cv.x, cv.y
    cx, cy, ex, ey = 0.5, 0.68, 0.37, 0.15
    ring = np.abs(sd_ellipse(x, y, cx, cy, ex, ey)) - 0.03
    symbol(cv, ring, shade=lerp_field((y - cy + ey) / (2 * ey), "#ffd060", "#c02a08"), width=0.03)

    def tongues(front):
        d = np.full(x.shape, 1e9)
        for k in range(10):
            a = math.radians(k * 36 + 18)
            px, py = cx + ex * math.cos(a), cy + ey * math.sin(a)
            if (py > cy) != front:
                continue
            h = (0.2 if front else 0.15) + 0.06 * (k % 2)
            d = np.minimum(d, sd_flame(x, y, px, py, h, 0.05 if front else 0.042, lean=0.02 * math.cos(a)))
        return d

    back = tongues(False)
    _fire(cv, back, cx, cy - ey - 0.22, 0.3)
    # Caster: a small dark figure standing in the ring.
    fig = union(sd_circle(x, y, cx, 0.4, 0.055),
                sd_polygon(x, y, [(cx - 0.1, 0.7), (cx + 0.1, 0.7), (cx + 0.05, 0.46), (cx - 0.05, 0.46)]))
    symbol(cv, fig, "#2a0e06", width=0.025, outline="#ffb050")
    front = tongues(True)
    _fire(cv, front, cx, cy + ey - 0.26, 0.32)


def icon_kor_yagmuru(cv):
    """Rain of embers: five glowing comets falling to the lower left onto a burning ground line."""
    badge(cv, "koz")
    x, y = cv.x, cv.y
    ground = sd_stroke(x, y, [(0.12, 0.84), (0.88, 0.84)], [0.02, 0.02])
    symbol(cv, ground, "#ff8a20", width=0.025)
    ux, uy = -0.5, 0.866  # falling direction (down-left)
    for (hx, hy, r, L) in ((0.3, 0.7, 0.06, 0.3), (0.52, 0.56, 0.07, 0.42), (0.74, 0.44, 0.06, 0.34),
                           (0.42, 0.3, 0.045, 0.24), (0.8, 0.74, 0.045, 0.22)):
        tx, ty = hx - ux * L, hy - uy * L
        n = 12
        pts = [(tx + (hx - tx) * k / (n - 1), ty + (hy - ty) * k / (n - 1)) for k in range(n)]
        c = union(sd_stroke(x, y, pts, [0.004 + r * 0.9 * (k / (n - 1)) ** 1.5 for k in range(n)]),
                  sd_circle(x, y, hx, hy, r))
        s = np.clip(((x - tx) * (hx - tx) + (y - ty) * (hy - ty)) / (L * L), 0, 1)
        symbol(cv, c, shade=lerp_field(1 - s, "#ffe880", "#b0200c"), width=0.028)
        cv.over(cv.cover(sd_circle(x, y, hx + 0.008, hy - 0.008, r * 0.5)), hexc("#fffbe6"))


# ---- Büyücü · Kırağı ------------------------------------------------------------------------

def icon_kiragi_oku(cv):
    """Frost bolt: a faceted ice spear flying up-right, a trail of frost motes."""
    badge(cv, "kiragi")
    x, y = cv.x, cv.y
    for (px, py, s) in ((0.16, 0.8, 0.035), (0.25, 0.86, 0.025), (0.12, 0.66, 0.025), (0.3, 0.74, 0.03)):
        symbol(cv, sd_star(x, y, px, py, s, s * 0.4, 4, -45), "#e8f8ff", width=0.02)
    rx, ry = rot(x, y, 0.5, 0.5, 45)
    # Upright spear in rotated space: tip at the top.
    spear = sd_polygon(rx, ry, [(0.5, 0.06), (0.585, 0.3), (0.56, 0.78), (0.5, 0.86), (0.44, 0.78), (0.415, 0.3)])
    cv.over(cv.cover(spear - 0.035), hexc(INK))
    cv.over(cv.cover(spear), lerp_field(np.where(rx > 0.5, 0.15, 0.6) + (ry - 0.06) * 0.3, "#ffffff", "#5ab0e8"))
    facet = sd_stroke(rx, ry, [(0.5, 0.08), (0.5, 0.84)], [0.006, 0.006])
    cv.over(cv.cover(np.maximum(facet, spear)) * 0.6, hexc("#2a78b8"))
    for side in (-1, 1):
        sh = sd_polygon(rx, ry, [(0.5 + side * 0.04, 0.5), (0.5 + side * 0.16, 0.66), (0.5 + side * 0.05, 0.62)])
        symbol(cv, sh, "#c8ecff", width=0.025)


def icon_buz_kabugu(cv):
    """Ice shell: a faceted hexagonal ice crystal dome with light facets and a crack."""
    badge(cv, "kiragi")
    x, y = cv.x, cv.y
    cx, cy, r = 0.5, 0.5, 0.36
    hexpts = [(cx + r * math.cos(math.radians(60 * k - 90)), cy + r * math.sin(math.radians(60 * k - 90))) for k in range(6)]
    hx = sd_polygon(x, y, hexpts)
    cv.over(cv.cover(hx - 0.035), hexc(INK))
    shades = ["#ffffff", "#d4f0ff", "#8ccbf0", "#4a94cc", "#78b8e4", "#c0e6ff"]
    for k in range(6):
        tri = sd_polygon(x, y, [(cx, cy), hexpts[k], hexpts[(k + 1) % 6]])
        cv.over(cv.cover(np.maximum(tri, hx)), hexc(shades[k]))
    inner = [(cx + r * 0.45 * math.cos(math.radians(60 * k - 60)), cy + r * 0.45 * math.sin(math.radians(60 * k - 60)))
             for k in range(6)]
    ih = sd_polygon(x, y, inner)
    cv.over(cv.cover(ih), lerp_field(np.hypot(x - cx + 0.03, y - cy + 0.03) / 0.2, "#ffffff", "#a8dcff"))
    for k in range(6):
        e = sd_stroke(x, y, [(cx, cy), hexpts[k]], [0.006, 0.006])
        cv.over(cv.cover(np.maximum(e, hx)) * 0.55, hexc("#1a4a78"))
    cv.over(cv.cover(np.abs(ih) - 0.006) * 0.6, hexc("#1a4a78"))
    crack = sd_stroke(x, y, [(0.62, 0.26), (0.58, 0.36), (0.66, 0.42), (0.6, 0.5)], [0.007] * 4)
    cv.over(cv.cover(np.maximum(crack, hx)), hexc("#ffffff"))


def icon_donduran_nefes(cv):
    """Freezing breath: a cone of frost spreading to the right over a snowflake, ice spikes rising
    from the ground (rooted)."""
    badge(cv, "kiragi")
    x, y = cv.x, cv.y
    n = 14
    top = [(0.14 + 0.74 * k / (n - 1), 0.42 - 0.28 * (k / (n - 1)) ** 0.9 + 0.02 * math.sin(k * 1.3)) for k in range(n)]
    bot = [(0.14 + 0.74 * k / (n - 1), 0.46 + 0.22 * (k / (n - 1)) ** 0.9 - 0.02 * math.sin(k * 1.1)) for k in range(n)]
    tipx = 0.88
    edge = [(tipx + 0.03 * math.cos(math.radians(a)), 0.4 + 0.27 * math.sin(math.radians(a))) for a in range(-80, 81, 20)]
    cone = sd_polygon(x, y, top + edge + bot[::-1])
    symbol(cv, cone, shade=lerp_field((x - 0.14) / 0.75, "#ffffff", "#5aaee0"), width=0.032)
    flake = sd_snowflake(x, y, 0.6, 0.41, 0.15, 0.016)
    symbol(cv, flake, "#ffffff", width=0.02)
    for (sx, h, w) in ((0.36, 0.13, 0.05), (0.52, 0.18, 0.06), (0.68, 0.13, 0.05), (0.82, 0.1, 0.04)):
        sp = sd_polygon(x, y, [(sx - w, 0.88), (sx, 0.88 - h), (sx + w, 0.88)])
        symbol(cv, sp, shade=lerp_field((0.88 - y) / h, "#3a8ac8", "#e8f8ff"), width=0.026)


# ---- Şifacı · Şifa --------------------------------------------------------------------------

def icon_yenileyen_isik(cv):
    """Renewing light (heal over time): a two-leaf sprout in a soft disc of light, rising motes."""
    badge(cv, "sifa")
    x, y = cv.x, cv.y
    glow(cv, 0.5, 0.48, 0.42, "#fff6c8", 0.6)
    disc = sd_ring(x, y, 0.5, 0.5, 0.3, 0.02)
    symbol(cv, disc, "#fff0b8", width=0.022)
    stem = sd_stroke(x, y, [(0.5, 0.82), (0.5, 0.6), (0.52, 0.44)], [0.026, 0.022, 0.018])
    left = sd_polygon(*rot(x, y, 0.5, 0.5, 0), [(0.5, 0.5), (0.4, 0.38), (0.24, 0.36), (0.3, 0.5), (0.42, 0.56)])
    left = sd_ellipse(*rot(x, y, 0.36, 0.46, -30), 0.36, 0.46, 0.15, 0.07)
    right = sd_ellipse(*rot(x, y, 0.65, 0.36, 35), 0.65, 0.36, 0.16, 0.075)
    whole = union(stem, left, right)
    cv.over(cv.cover(whole - 0.035), hexc(INK))
    cv.over(cv.cover(stem), hexc("#5aa040"))
    cv.over(cv.cover(left), lerp_field((x - 0.22) / 0.3, "#c8f080", "#3a8a2a"))
    cv.over(cv.cover(right), lerp_field((0.8 - x) / 0.3, "#d8ff90", "#3a8a2a"))
    for (mx, my, s) in ((0.24, 0.24, 0.04), (0.8, 0.6, 0.035), (0.2, 0.68, 0.03)):
        symbol(cv, sd_plus(x, y, mx, my, s, s * 0.35), "#fffbe0", width=0.02)


def icon_kutlu_siper(cv):
    """Blessed guard (defense and max hp): a pale gold shield bearing a heart, a halo above."""
    badge(cv, "sifa")
    x, y = cv.x, cv.y
    halo = sd_ellipse(x, y, 0.5, 0.17, 0.2, 0.06)
    halo = np.abs(halo) - 0.016
    symbol(cv, halo, "#fff2a0", width=0.024)
    _shield(cv, 0.5, 0.56, 0.25, 0.3, face=("#fff8e0", "#c8a860"), rim="#e8c060", boss=False)
    heart = sd_heart(x, y, 0.5, 0.56, 0.1)
    symbol(cv, heart, shade=lerp_field(np.hypot(x - 0.46, y - 0.5) / 0.18, "#ff8a8a", "#a01828"), width=0.025)


def icon_halka_sifasi(cv):
    """Ring of healing: a glowing ring on the ground, crosses of light rising from it."""
    badge(cv, "sifa")
    x, y = cv.x, cv.y
    glow(cv, 0.5, 0.74, 0.4, "#fff8c0", 0.5)
    ring = np.abs(sd_ellipse(x, y, 0.5, 0.74, 0.34, 0.12)) - 0.028
    symbol(cv, ring, shade=lerp_field((y - 0.62) / 0.24, "#fffbe0", "#e0b040"), width=0.028)
    for (px, py, s) in ((0.5, 0.36, 0.13), (0.24, 0.5, 0.08), (0.76, 0.5, 0.08)):
        p = sd_plus(x, y, px, py, s, s * 0.34)
        symbol(cv, p, shade=lerp_field((y - py + s) / (2 * s), "#ffffff", "#9ae08a"), width=0.028)
    for (px, py) in ((0.5, 0.56), (0.36, 0.66), (0.64, 0.66)):
        symbol(cv, sd_circle(x, y, px, py, 0.018), "#fffbe0", width=0.012)


# ---- Şifacı · Yargı -------------------------------------------------------------------------

def icon_isik_carpmasi(cv):
    """Light strike: a lance of light falling from above into a four-pointed burst."""
    badge(cv, "yargi")
    x, y = cv.x, cv.y
    glow(cv, 0.5, 0.7, 0.36, "#ffd070", 0.6)
    beam = sd_polygon(x, y, [(0.47, 0.06), (0.53, 0.06), (0.585, 0.66), (0.415, 0.66)])
    symbol(cv, beam, shade=lerp_field((y - 0.06) / 0.6, "#ffe9a8", "#ffffff"), width=0.03)
    burst = sd_star(x, y, 0.5, 0.7, 0.3, 0.075, 4, -90)
    small = sd_star(x, y, 0.5, 0.7, 0.19, 0.06, 4, -45)
    symbol(cv, union(burst, small), shade=lerp_field(np.hypot(x - 0.5, y - 0.7) / 0.3, "#ffffff", "#f0a020"), width=0.03)


def icon_baglayan_soz(cv):
    """Binding word: two interlocked chain links across an amber rune circle."""
    badge(cv, "yargi")
    x, y = cv.x, cv.y
    rune = sd_ring(x, y, 0.5, 0.5, 0.33, 0.022)
    glyph = union(sd_stroke(x, y, [(0.5, 0.22), (0.5, 0.78)], [0.02, 0.02]),
                  sd_stroke(x, y, [(0.5, 0.36), (0.66, 0.26)], [0.018, 0.018]),
                  sd_stroke(x, y, [(0.5, 0.64), (0.34, 0.74)], [0.018, 0.018]))
    glow(cv, 0.5, 0.5, 0.4, "#ffb040", 0.45)
    symbol(cv, union(rune, glyph), "#ffc860", width=0.024)
    for (cx, cy, k) in ((0.38, 0.6, 0), (0.62, 0.4, 1)):
        rx, ry = rot(x, y, cx, cy, -45)
        link = np.abs(sd_round_rect(rx, ry, cx, cy, 0.17, 0.085, 0.085)) - 0.032
        cv.over(cv.cover(link - 0.035), hexc(INK))
        cv.over(cv.cover(link), lerp_field((ry - cy + 0.1) / 0.2, "#ffffff", "#9aa0aa"))
    # Re-draw the first link's crossing part on top so they interlock.
    cx, cy = 0.38, 0.6
    rx, ry = rot(x, y, cx, cy, -45)
    link = np.abs(sd_round_rect(rx, ry, cx, cy, 0.17, 0.085, 0.085)) - 0.032
    clip = sd_circle(x, y, 0.53, 0.5, 0.07)
    part = np.maximum(link, clip)
    cv.over(cv.cover(part - 0.03) * cv.cover(clip - 0.03), hexc(INK))
    cv.over(cv.cover(part), lerp_field((ry - cy + 0.1) / 0.2, "#ffffff", "#9aa0aa"))


def icon_ofkeli_dua(cv):
    """Wrathful prayer (attack up): a sword raised point-up through a radiant halo."""
    badge(cv, "yargi")
    x, y = cv.x, cv.y
    rays = np.full(x.shape, 1e9)
    for k in range(8):
        a = math.radians(k * 45 - 90 + 22.5)
        ca, sa = math.cos(a), math.sin(a)
        rays = np.minimum(rays, sd_polygon(x, y, [(0.5 + ca * 0.2 - sa * 0.03, 0.4 + sa * 0.2 + ca * 0.03),
                                                  (0.5 + ca * 0.38, 0.4 + sa * 0.38),
                                                  (0.5 + ca * 0.2 + sa * 0.03, 0.4 + sa * 0.2 - ca * 0.03)]))
    symbol(cv, rays, "#ffd27a", width=0.025)
    halo = sd_ring(x, y, 0.5, 0.4, 0.2, 0.03)
    symbol(cv, halo, shade=lerp_field(np.abs(x - 0.5) / 0.25, "#ffffff", "#f0b040"), width=0.028)
    draw_sword(cv, 0.5, 0.72, 0.5, 0.1, w=0.05, guard=0.13, grip=0.14, hilt=("#ffe08a", "#9a6a20"))


SKILLS = {
    # Savaşçı · Hücum / Siper
    "yarma": ("hucum", icon_yarma),
    "kan_ofkesi": ("hucum", icon_kan_ofkesi),
    "sarsinti": ("hucum", icon_sarsinti),
    "kalkan_duvari": ("siper", icon_kalkan_duvari),
    "meydan_okuma": ("siper", icon_meydan_okuma),
    "direncli_soluk": ("siper", icon_direncli_soluk),
    # Haydut · Pusu / Zehir
    "sirt_hanceri": ("pusu", icon_sirt_hanceri),
    "golge_adimi": ("pusu", icon_golge_adimi),
    "seri_eller": ("pusu", icon_seri_eller),
    "zehirli_kesik": ("zehir", icon_zehirli_kesik),
    "sis_bombasi": ("zehir", icon_sis_bombasi),
    "kacamak": ("zehir", icon_kacamak),
    # Büyücü · Köz / Kırağı
    "yakici_iz": ("koz", icon_yakici_iz),
    "alev_halkasi": ("koz", icon_alev_halkasi),
    "kor_yagmuru": ("koz", icon_kor_yagmuru),
    "kiragi_oku": ("kiragi", icon_kiragi_oku),
    "buz_kabugu": ("kiragi", icon_buz_kabugu),
    "donduran_nefes": ("kiragi", icon_donduran_nefes),
    # Şifacı · Şifa / Yargı
    "yenileyen_isik": ("sifa", icon_yenileyen_isik),
    "kutlu_siper": ("sifa", icon_kutlu_siper),
    "halka_sifasi": ("sifa", icon_halka_sifasi),
    "isik_carpmasi": ("yargi", icon_isik_carpmasi),
    "baglayan_soz": ("yargi", icon_baglayan_soz),
    "ofkeli_dua": ("yargi", icon_ofkeli_dua),
}

# Contact sheet rows: per class the M9 icon first, then its two branches.
CLASS_ROWS = [
    ("agir_darbe", ["yarma", "kan_ofkesi", "sarsinti", "kalkan_duvari", "meydan_okuma", "direncli_soluk"]),
    ("cifte_kesik", ["sirt_hanceri", "golge_adimi", "seri_eller", "zehirli_kesik", "sis_bombasi", "kacamak"]),
    ("koz_oku", ["yakici_iz", "alev_halkasi", "kor_yagmuru", "kiragi_oku", "buz_kabugu", "donduran_nefes"]),
    ("soluk_gunes", ["yenileyen_isik", "kutlu_siper", "halka_sifasi", "isik_carpmasi", "baglayan_soz", "ofkeli_dua"]),
]


def make_icons():
    out = {}
    for name, (_, painter) in SKILLS.items():
        cv = Canvas(64)
        painter(cv)
        rgba = cv.result()
        vc.write_png(os.path.join(SKILL_DIR, f"{name}.png"), vc.to_u8(rgba))
        out[name] = rgba
        print(f"SKILL icon {name}: 64x64, coverage {float(rgba[..., 3].mean()):.2f}")
    return out


# ---------------------------------------------------------------------------------------------
# Status icons (48x48, read at 24): a round plate, gold rim for buffs, red rim for debuffs.

STATUS_SIZE = 48
BUFF_PLATE = ("#2e3a2a", "#11160f", "#d9ae4a")     # top, bottom, rim
DEBUFF_PLATE = ("#3e1a1a", "#160808", "#c8402c")


def plate(cv, debuff):
    top, bottom, rim = DEBUFF_PLATE if debuff else BUFF_PLATE
    x, y = cv.x, cv.y
    outer = sd_round_rect(x, y, 0.5, 0.5, 0.48, 0.48, 0.16)
    cv.over(cv.cover(outer), hexc(INK))
    cv.over(cv.cover(outer + 0.035), hexc(rim))
    inner = outer + 0.075
    cv.over(cv.cover(inner), lerp_field((y - 0.1) / 0.8, top, bottom))


def corner_arrow(cv, up=True):
    """Small green up / red down arrow in the bottom-right corner."""
    x, y = cv.x, cv.y
    if up:
        a = sd_polygon(x, y, [(0.78, 0.56), (0.95, 0.78), (0.85, 0.78), (0.85, 0.95), (0.71, 0.95), (0.71, 0.78), (0.61, 0.78)])
        symbol(cv, a, shade=lerp_field((y - 0.56) / 0.4, "#d8ff9a", "#3aa020"), width=0.05)
    else:
        a = sd_polygon(x, y, [(0.78, 0.96), (0.95, 0.74), (0.85, 0.74), (0.85, 0.57), (0.71, 0.57), (0.71, 0.74), (0.61, 0.74)])
        symbol(cv, a, shade=lerp_field((y - 0.56) / 0.4, "#ff9a8a", "#b01810"), width=0.05)


W_S = 0.055  # status outline width (thicker: they are read at 24 px)


def st_attack_up(cv):
    plate(cv, False)
    draw_sword(cv, 0.3, 0.72, 0.66, 0.2, w=0.075, guard=0.14, grip=0.12, outline=W_S)
    corner_arrow(cv)


def st_defense_up(cv):
    plate(cv, False)
    x, y = cv.x, cv.y
    s = sd_kite(x, y, 0.44, 0.45, 0.25, 0.32)
    symbol(cv, s, "#d09a50", width=W_S)
    cv.over(cv.cover(sd_kite(x, y, 0.44, 0.455, 0.19, 0.25)), lerp_field((y - 0.2) / 0.6, "#c4d2e2", "#4a5868"))
    corner_arrow(cv)


def st_attack_speed_up(cv):
    plate(cv, False)
    x, y = cv.x, cv.y
    for k, yy in enumerate((0.3, 0.46, 0.62)):
        line = sd_stroke(x, y, [(0.1 + 0.06 * k, yy), (0.34 + 0.06 * k, yy)], [0.03, 0.03])
        symbol(cv, line, "#e8e0ff", width=0.04)
    draw_dagger(cv, 0.38, 0.62, 0.78, 0.24, w=0.07, guard=0.12, grip=0.1, outline=W_S, hilt=("#c8a8e8", "#4a2c6a"))
    corner_arrow(cv)


def st_dodge_up(cv):
    plate(cv, False)
    x, y = cv.x, cv.y
    n = 20
    pts = [(0.62 - 0.36 * math.sin(math.pi * k / (n - 1)), 0.86 - 0.62 * k / (n - 1)) for k in range(n)]
    ex, ey = pts[-1]
    sw = union(sd_stroke(x, y, pts, [0.06] * n), sd_polygon(x, y, [(ex + 0.02, ey - 0.15), (ex + 0.15, ey + 0.05), (ex - 0.11, ey + 0.04)]))
    symbol(cv, sw, shade=lerp_field((0.9 - y) / 0.75, "#a8e040", "#ffffff"), width=W_S)
    corner_arrow(cv)


def st_max_hp_up(cv):
    plate(cv, False)
    x, y = cv.x, cv.y
    h = sd_heart(x, y, 0.44, 0.48, 0.2)
    symbol(cv, h, shade=lerp_field(np.hypot(x - 0.36, y - 0.36) / 0.4, "#ff9a9a", "#a01020"), width=W_S)
    corner_arrow(cv)


def st_heal_over_time(cv):
    plate(cv, False)
    x, y = cv.x, cv.y
    arc = sd_stroke(x, y, arc_points(0.5, 0.5, 0.31, -60, 220, 24), [0.045] * 24)
    a = math.radians(220)
    ex, ey = 0.5 + 0.31 * math.cos(a), 0.5 + 0.31 * math.sin(a)
    head = sd_polygon(x, y, [(ex - 0.13, ey - 0.02), (ex + 0.1, ey - 0.1), (ex + 0.04, ey + 0.13)])
    symbol(cv, union(arc, head), "#bff0a0", width=W_S)
    symbol(cv, sd_plus(x, y, 0.5, 0.5, 0.17, 0.065), "#5ad070", width=W_S)


def st_shield(cv):
    plate(cv, False)
    x, y = cv.x, cv.y
    b = sd_circle(x, y, 0.5, 0.52, 0.32)
    symbol(cv, b, shade=lerp_field(np.hypot(x - 0.42, y - 0.42) / 0.42, "#e8f8ff", "#3a8ad0"), width=W_S)
    cv.over(cv.cover(np.abs(sd_circle(x, y, 0.5, 0.52, 0.24)) - 0.02) * 0.6, hexc("#ffffff"))
    cv.over(cv.cover(sd_ellipse(*rot(x, y, 0.38, 0.38, -40), 0.38, 0.38, 0.09, 0.05)), hexc("#ffffff"))


def st_burn(cv):
    plate(cv, True)
    x, y = cv.x, cv.y
    f = sd_flame(x, y, 0.5, 0.74, 0.58, 0.2, lean=0.04, wave=0.03)
    symbol(cv, f, shade=lerp_field((y - 0.16) / 0.7, "#ffe070", "#c02a08"), width=W_S)
    cv.over(cv.cover(sd_flame(x, y, 0.5, 0.76, 0.3, 0.09, lean=0.02)), hexc("#fff8d8"))


def st_poison(cv):
    plate(cv, True)
    x, y = cv.x, cv.y
    d = sd_drop(x, y, 0.46, 0.6, 0.24, 0.48)
    symbol(cv, d, shade=lerp_field(np.hypot(x - 0.38, y - 0.5) / 0.35, "#e8ff9a", "#3e7a08"), width=W_S)
    for (bx, by, r) in ((0.78, 0.3, 0.07), (0.74, 0.12, 0.045)):
        symbol(cv, sd_ring(x, y, bx, by, r, 0.022), "#c8f070", width=0.035)


def st_slow(cv):
    plate(cv, True)
    x, y = cv.x, cv.y
    glass = union(sd_polygon(x, y, [(0.26, 0.16), (0.74, 0.16), (0.53, 0.5), (0.74, 0.84), (0.26, 0.84), (0.47, 0.5)]))
    symbol(cv, glass, "#cfe4f4", width=W_S)
    sand = union(sd_polygon(x, y, [(0.34, 0.26), (0.66, 0.26), (0.5, 0.46)]),
                 sd_polygon(x, y, [(0.5, 0.6), (0.68, 0.8), (0.32, 0.8)]))
    cv.over(cv.cover(sand), hexc("#5a9ad8"))
    caps = union(sd_round_rect(x, y, 0.5, 0.14, 0.3, 0.045, 0.02), sd_round_rect(x, y, 0.5, 0.86, 0.3, 0.045, 0.02))
    symbol(cv, caps, "#c8a060", width=0.04)


def st_stun(cv):
    plate(cv, True)
    x, y = cv.x, cv.y
    orbit = np.abs(sd_ellipse(x, y, 0.5, 0.56, 0.33, 0.13)) - 0.022
    symbol(cv, orbit, "#f0c040", width=0.04)
    for (sx, sy, s) in ((0.5, 0.36, 0.2), (0.22, 0.6, 0.13), (0.78, 0.62, 0.13)):
        symbol(cv, sd_star(x, y, sx, sy, s, s * 0.42, 4), "#fff3a0", width=W_S)


def st_root(cv):
    plate(cv, True)
    x, y = cv.x, cv.y
    ground = sd_round_rect(x, y, 0.5, 0.82, 0.38, 0.05, 0.02)
    spikes = union(*[sd_polygon(x, y, [(sx - w, 0.8), (sx, 0.8 - h), (sx + w, 0.8)])
                     for (sx, h, w) in ((0.24, 0.42, 0.1), (0.5, 0.62, 0.12), (0.76, 0.42, 0.1))])
    symbol(cv, union(ground, spikes), shade=lerp_field((0.85 - y) / 0.6, "#3a7ac0", "#ffffff"), width=W_S)


def st_weakened(cv):
    plate(cv, True)
    x, y = cv.x, cv.y
    # A sword snapped in two: the blade's tip half drops away.
    draw_sword(cv, 0.24, 0.78, 0.5, 0.5, w=0.075, guard=0.13, grip=0.12, outline=W_S, steel=("#d0d4da", "#6a727e"))
    tip = sd_polygon(*rot(x, y, 0.64, 0.32, 30), [(0.58, 0.42), (0.7, 0.36), (0.78, 0.14), (0.62, 0.3)])
    symbol(cv, tip, "#a8aeb8", width=W_S)
    corner_arrow(cv, up=False)


def st_taunted(cv):
    plate(cv, True)
    x, y = cv.x, cv.y
    cross = union(np.abs(sd_circle(x, y, 0.5, 0.5, 0.3)) - 0.035,
                  sd_round_rect(x, y, 0.5, 0.13, 0.03, 0.08, 0.01), sd_round_rect(x, y, 0.5, 0.87, 0.03, 0.08, 0.01),
                  sd_round_rect(x, y, 0.13, 0.5, 0.08, 0.03, 0.01), sd_round_rect(x, y, 0.87, 0.5, 0.08, 0.03, 0.01))
    symbol(cv, cross, "#ff7a5a", width=0.045)
    bang = union(sd_polygon(x, y, [(0.43, 0.3), (0.57, 0.3), (0.53, 0.58), (0.47, 0.58)]), sd_circle(x, y, 0.5, 0.68, 0.055))
    symbol(cv, bang, "#fff0d0", width=W_S)


STATUS = {
    "attack_up": st_attack_up,
    "defense_up": st_defense_up,
    "attack_speed_up": st_attack_speed_up,
    "dodge_up": st_dodge_up,
    "max_hp_up": st_max_hp_up,
    "heal_over_time": st_heal_over_time,
    "shield": st_shield,
    "burn": st_burn,
    "poison": st_poison,
    "slow": st_slow,
    "stun": st_stun,
    "root": st_root,
    "weakened": st_weakened,
    "taunted": st_taunted,
}


def make_status():
    out = {}
    for name, painter in STATUS.items():
        cv = Canvas(STATUS_SIZE)
        painter(cv)
        rgba = cv.result()
        vc.write_png(os.path.join(STATUS_DIR, f"{name}.png"), vc.to_u8(rgba))
        out[name] = rgba
        print(f"ICON status {name}: {STATUS_SIZE}x{STATUS_SIZE}")
    return out


# ---------------------------------------------------------------------------------------------
# Skill bar / tree UI pieces (pixel coordinates). Palette from client/ui/Theme.h.

PANEL_TOP, PANEL_BOTTOM = "#2b333d", "#1a1f26"
BORDER = "#7a6236"
ORNAMENT = "#c9a247"
ACCENT_BRIGHT = "#f5d27a"
INSET = "#11151a"

SLOT = 64
WIN = (6.0, 6.0, 58.0, 58.0)   # icon window inside a slot (x0, y0, x1, y1): icon = slot * 52 / 64
WIN_R = 7.0


def win_sdf(cv, grow=0.0):
    x0, y0, x1, y1 = WIN
    return sd_round_rect(cv.x, cv.y, (x0 + x1) / 2, (y0 + y1) / 2, (x1 - x0) / 2 + grow, (y1 - y0) / 2 + grow, WIN_R + grow)


def slot_frame(cv, state):
    """The ring around the icon window (transparent window, drawn over the icon)."""
    x, y = cv.x, cv.y
    outer = sd_round_rect(x, y, 32, 32, 31.5, 31.5, 9)
    window = win_sdf(cv)
    ring = np.maximum(outer, -window)
    if state == "hover":
        g = np.clip(1.0 - np.abs(window) / 7.0, 0, 1) ** 2
        cv.over(g * 0.55 * cv.cover(-window + 0.5), hexc("#ffe08a"))  # warm glow into the icon edge
    cv.over(cv.cover(ring), hexc("#07090c"))
    body = np.maximum(outer + 1.2, -(window - 1.2))
    t = (y - 1) / 62
    if state == "pressed":
        col = lerp_field(t, "#3a3020", "#6a5530")
        hl = "#8a6c34"
    elif state == "hover":
        col = lerp_field(t, "#f5d27a", "#a07a30")
        hl = "#fff2c0"
    elif state == "empty":
        col = lerp_field(t, "#5a5040", "#2e2a22")
        hl = "#6e6450"
    else:
        col = lerp_field(t, "#d9ae4a", "#7a6236")
        hl = "#f5d27a"
    cv.over(cv.cover(body), col)
    # Bevel: a light line on the outer top edge, a dark line inside next to the window.
    top_edge = np.maximum(np.abs(outer + 1.8) - 0.6, y - 20)
    cv.over(cv.cover(top_edge) * 0.8, hexc(hl))
    inner_line = np.abs(window - 1.6) - 0.6
    cv.over(cv.cover(inner_line) * 0.9, hexc("#1a1406"))
    # Corner studs (an original touch): small diamonds at the four corners.
    for (cx, cy) in ((5.5, 5.5), (58.5, 5.5), (5.5, 58.5), (58.5, 58.5)):
        d = sd_polygon(x, y, [(cx, cy - 3.2), (cx + 3.2, cy), (cx, cy + 3.2), (cx - 3.2, cy)])
        cv.over(cv.cover(d + 0.0), hexc("#07090c"))
        cv.over(cv.cover(d + 1.0), hexc(hl if state != "pressed" else "#b08a40"))
    if state == "pressed":
        # Inner shadow from the top into the window.
        sh = np.clip(1.0 - (y - WIN[1]) / 10.0, 0, 1) * cv.cover(window)
        cv.over(sh * 0.55, hexc("#000000"))


def slot_back(cv, empty=False):
    """The recessed well behind the icon (also the empty slot's face)."""
    x, y = cv.x, cv.y
    outer = sd_round_rect(x, y, 32, 32, 31.5, 31.5, 9)
    cv.over(cv.cover(outer), lerp_field((y - 2) / 60, "#0a0d11", "#171c22"))
    w = win_sdf(cv)
    sh = np.clip(1.0 - (y - WIN[1]) / 8.0, 0, 1) * cv.cover(w)
    cv.over(sh * 0.6, hexc("#000000"))
    if empty:
        d = sd_polygon(x, y, [(32, 22), (42, 32), (32, 42), (22, 32)])
        cv.over(cv.cover(np.abs(d) - 0.8) * 0.45, hexc("#7a6236"))
        cv.over(cv.cover(sd_circle(x, y, 32, 32, 2.2)) * 0.45, hexc("#7a6236"))


def ui_cooldown_shade(cv):
    """Window-shaped dark veil: the client masks it with its clockwise sweep (or draws it whole)."""
    cv.over(cv.cover(win_sdf(cv)) * 0.66, hexc("#05070a"))


def ui_nomana(cv):
    """Window-shaped cold tint for "not enough mana" (draw over the icon, under the frame)."""
    x, y = cv.x, cv.y
    w = win_sdf(cv)
    cv.over(cv.cover(w) * 0.42, hexc("#14246a"))
    edge = np.clip(1.0 - np.abs(w) / 4.0, 0, 1) * cv.cover(w)
    cv.over(edge * 0.6, hexc("#4a7aff"))


def ui_ready_flash(cv):
    """A soft gold-white glow hugging the window edge (flash it for ~0.25 s when a cooldown ends)."""
    w = win_sdf(cv)
    g = np.clip(1.0 - np.abs(w) / 6.0, 0, 1) ** 1.5
    cv.over(g * 0.9, hexc("#fff0b0"))


def ui_lock_overlay(cv):
    """Locked skill: window veil plus a padlock (drawn over the icon, under the frame)."""
    cv.over(cv.cover(win_sdf(cv)) * 0.62, hexc("#06080b"))
    padlock(cv, 32, 34, 1.0)


def padlock(cv, cx, cy, s):
    x, y = cv.x, cv.y
    shackle = np.abs(sd_round_rect(x, y, cx, cy - 9 * s, 7.5 * s, 9 * s, 7.5 * s)) - 2.4 * s
    shackle = np.maximum(shackle, y - (cy - 2 * s))
    body = sd_round_rect(x, y, cx, cy + 4 * s, 12 * s, 9.5 * s, 2.5 * s)
    cv.over(cv.cover(union(shackle, body) - 1.6 * s), hexc(INK))
    cv.over(cv.cover(shackle), lerp_field((x - cx + 8 * s) / (16 * s), "#c8ccd4", "#5a606a"))
    cv.over(cv.cover(body), lerp_field((y - cy + 6 * s) / (20 * s), "#8a7444", "#3a2c14"))
    cv.over(cv.cover(np.abs(body + 1.4 * s) - 0.6 * s) * 0.8, hexc(ORNAMENT))
    hole = union(sd_circle(x, y, cx, cy + 2.5 * s, 2.4 * s),
                 sd_polygon(x, y, [(cx - 1.4 * s, cy + 3 * s), (cx + 1.4 * s, cy + 3 * s), (cx + 2 * s, cy + 8.5 * s),
                                   (cx - 2 * s, cy + 8.5 * s)]))
    cv.over(cv.cover(hole), hexc("#0a0806"))


def ui_lock_icon(cv):
    padlock(cv, 16, 17, 0.62)


def ui_key_badge(cv):
    """Plate for the key number (the client draws the digit, Theme text colour, centred)."""
    x, y = cv.x, cv.y
    o = sd_round_rect(x, y, 12, 12, 11.5, 11.5, 5)
    cv.over(cv.cover(o), hexc("#07090c"))
    cv.over(cv.cover(o + 1.0), lerp_field((y - 1) / 22, "#9c7d3c", "#5a4620"))
    cv.over(cv.cover(o + 2.2), lerp_field((y - 2) / 20, "#262019", "#0e0c09"))


def rank_pip(cv, on):
    x, y = cv.x, cv.y
    d = sd_polygon(x, y, [(8, 1), (15, 8), (8, 15), (1, 8)])
    cv.over(cv.cover(d), hexc("#07090c"))
    if on:
        cv.over(cv.cover(d + 1.3), lerp_field((y - 2) / 12, "#fff0b0", "#c98a1e"))
        cv.over(cv.cover(sd_polygon(x, y, [(8, 4), (11, 7), (8, 8.5), (5, 7)])) * 0.8, hexc("#ffffff"))
    else:
        cv.over(cv.cover(d + 1.3), hexc("#3c372c"))
        cv.over(cv.cover(d + 3.0), hexc("#15130f"))


def rank_strip(n_on, total=3):
    """total pips 16 px apart with 2 px gaps: (16 + 2) * total - 2 wide."""
    w = 18 * total - 2
    cv = Canvas(w, 16, unit=1)
    for k in range(total):
        sub_cv = Canvas(16, 16, unit=1)
        rank_pip(sub_cv, k < n_on)
        cv.rgba[:, k * 18 * SS:(k * 18 + 16) * SS] = sub_cv.rgba
    return cv


def connector(cv, segs, lit):
    """Tree connector pieces: line segments in pixel coordinates (16 px wide track)."""
    x, y = cv.x, cv.y
    d = np.full(x.shape, 1e9)
    for (a, b) in segs:
        d = np.minimum(d, sd_stroke(x, y, [a, b], [3.0, 3.0]))
    if lit:
        g = np.clip(1.0 - d / 5.0, 0, 1) ** 2 * (d > 0)
        cv.over(g * 0.55, hexc("#ffcc50"))
        cv.over(cv.cover(d), hexc("#3a2a08"))
        cv.over(cv.cover(d + 1.0), lerp_field(np.zeros_like(x), "#f5d27a", "#f5d27a"))
        core = np.full(x.shape, 1e9)
        for (a, b) in segs:
            core = np.minimum(core, sd_stroke(x, y, [a, b], [0.9, 0.9]))
        cv.over(cv.cover(core), hexc("#fffbe6"))
    else:
        cv.over(cv.cover(d), hexc("#07090c"))
        cv.over(cv.cover(d + 1.0), hexc("#66583c"))
        core = np.full(x.shape, 1e9)
        for (a, b) in segs:
            core = np.minimum(core, sd_stroke(x, y, [a, b], [0.6, 0.6]))
        cv.over(cv.cover(core) * 0.7, hexc("#2e281c"))


def connector_pieces():
    """name -> (w, h, segments). Lines run through the track centre (x or y = 8) to the edges
    (they extend past the edge so the caps are cut flat and pieces tile seamlessly)."""
    return {
        "tree_v": (16, 32, [((8, -4), (8, 36))]),
        "tree_h": (32, 16, [((-4, 8), (36, 8))]),
        "tree_elbow_se": (16, 16, [((8, 20), (8, 8)), ((8, 8), (20, 8))]),   # joins bottom and right
        "tree_elbow_sw": (16, 16, [((8, 20), (8, 8)), ((8, 8), (-4, 8))]),
        "tree_elbow_ne": (16, 16, [((8, -4), (8, 8)), ((8, 8), (20, 8))]),
        "tree_elbow_nw": (16, 16, [((8, -4), (8, 8)), ((8, 8), (-4, 8))]),
        "tree_tee_s": (16, 16, [((-4, 8), (20, 8)), ((8, 8), (8, 20))]),     # left, right and down
    }


def branch_emblem(cv, key, cx, cy, s):
    """Small emblem (s = radius in px) for the branch headers."""
    x, y = cv.x, cv.y
    u = lambda v: v * s  # noqa: E731
    w = max(1.2, s * 0.09)
    if key == "hucum":
        d = union(sd_blade(x, y, cx, cy + u(0.35), cx, cy - u(0.85), u(0.16)),
                  sd_stroke(x, y, [(cx - u(0.4), cy + u(0.35)), (cx + u(0.4), cy + u(0.35))], [u(0.09)] * 2),
                  sd_stroke(x, y, [(cx, cy + u(0.35)), (cx, cy + u(0.8))], [u(0.08)] * 2))
        symbol(cv, d, "#f4f6fa", width=w)
    elif key == "siper":
        d = sd_kite(x, y, cx, cy - u(0.05), u(0.55), u(0.62))
        symbol(cv, d, "#d09a50", width=w)
        cv.over(cv.cover(sd_kite(x, y, cx, cy - u(0.04), u(0.38), u(0.45))), hexc("#8a9cb0"))
    elif key == "pusu":
        moon = sub(sd_circle(x, y, cx - u(0.1), cy, u(0.62)), sd_circle(x, y, cx + u(0.15), cy - u(0.12), u(0.55)))
        symbol(cv, moon, "#c8a8f0", width=w)
        d = sd_blade(x, y, cx + u(0.2), cy - u(0.3), cx + u(0.2), cy + u(0.8), u(0.13))
        symbol(cv, d, "#f4f6fa", width=w)
    elif key == "zehir":
        d = sd_drop(x, y, cx, cy + u(0.22), u(0.5), u(1.0))
        symbol(cv, d, shade=lerp_field((y - cy + s) / (2 * s), "#e8ff9a", "#4e8a10"), width=w)
    elif key == "koz":
        d = sd_flame(x, y, cx, cy + u(0.55), u(1.25), u(0.42), lean=u(0.08), wave=u(0.05))
        symbol(cv, d, shade=lerp_field((y - cy + s) / (2 * s), "#ffe070", "#d0300a"), width=w)
    elif key == "kiragi":
        d = sd_snowflake(x, y, cx, cy, u(0.85), u(0.09))
        symbol(cv, d, "#eaf8ff", width=w)
    elif key == "sifa":
        d = sd_plus(x, y, cx, cy, u(0.75), u(0.26))
        symbol(cv, d, shade=lerp_field((y - cy + s) / (2 * s), "#ffffff", "#9ae08a"), width=w)
    elif key == "yargi":
        d = sd_star(x, y, cx, cy, u(0.95), u(0.25), 4)
        symbol(cv, d, shade=lerp_field(np.hypot(x - cx, y - cy) / s, "#ffffff", "#f0a020"), width=w)


def ui_branch_header(cv, key):
    """192x48 plaque: a medallion in the branch colour with its emblem on the left; the client
    writes the branch name on the plate right of it (x >= 52, vertically centred)."""
    x, y = cv.x, cv.y
    plate_ = sd_round_rect(x, y, 112, 24, 79, 15, 5)
    cv.over(cv.cover(plate_ + 0.0), hexc("#07090c"))
    cv.over(cv.cover(plate_ + 1.0), hexc(BORDER))
    cv.over(cv.cover(plate_ + 2.0), lerp_field((y - 10) / 28, PANEL_TOP, PANEL_BOTTOM))
    # Branch-colour stripe along the bottom and a fading gold rule on top.
    top, bottom = BRANCH[key]
    stripe = sd_round_rect(x, y, 116, 34.5, 72, 1.6, 1.2)
    cv.over(cv.cover(stripe) * np.clip(1.2 - (x - 50) / 150, 0, 1), hexc(top))
    rule = sd_round_rect(x, y, 116, 13, 72, 0.6, 0.5)
    cv.over(cv.cover(rule) * np.clip(1.0 - (x - 50) / 150, 0, 1) * 0.7, hexc(ORNAMENT))
    # Tail ornament on the right: a small diamond.
    d = sd_polygon(x, y, [(186, 24), (190.5, 19.5), (195, 24), (190.5, 28.5)])
    cv.over(cv.cover(d - 0.0), hexc("#07090c"))
    cv.over(cv.cover(d + 1.0), hexc(ORNAMENT))
    # Medallion.
    m = sd_circle(x, y, 24, 24, 22.5)
    cv.over(cv.cover(m), hexc("#07090c"))
    cv.over(cv.cover(m + 1.4), lerp_field((y - 2) / 44, ACCENT_BRIGHT, "#7a5a20"))
    inner = m + 3.6
    grad = np.clip((y - 4) / 40, 0, 1)[..., None]
    col = hexc(top) * (1 - grad) + hexc(bottom) * grad
    lift = np.clip(1.0 - np.hypot(x - 24, y - 20) / 20, 0, 1)[..., None] ** 2 * 0.3
    cv.over(cv.cover(inner), col + (1 - col) * lift)
    branch_emblem(cv, key, 24, 24, 13.5)


def ui_skill_point_marker(cv, star=True):
    """Gold disc like the M11 unspent-points marker (FFE08A -> C98A1E, 5A3A08 rim); with a dark
    four-pointed star (no number) or blank for a number drawn on it."""
    x, y = cv.x, cv.y
    g = np.clip(1.0 - (np.hypot(x - 16, y - 16) - 9) / 7, 0, 1) ** 2
    cv.over(g * 0.35, hexc("#f5c84c"))
    d = sd_circle(x, y, 16, 16, 10.5)
    cv.over(cv.cover(d), hexc("#5a3a08"))
    cv.over(cv.cover(d + 1.2), lerp_field((y - 6) / 20, "#ffe08a", "#c98a1e"))
    cv.over(cv.cover(np.abs(d + 3.0) - 0.4) * 0.5, hexc("#fff4c8"))
    if star:
        s = sd_star(x, y, 16, 16, 6.8, 2.0, 4)
        cv.over(cv.cover(s), hexc("#2a1a02"))


UI_PIECES = {}


def make_ui():
    out = {}

    def save(name, cv):
        rgba = cv.result()
        vc.write_png(os.path.join(BAR_DIR, f"{name}.png"), vc.to_u8(rgba))
        out[name] = rgba
        print(f"ICON ui {name}: {rgba.shape[1]}x{rgba.shape[0]}")

    for state in ("normal", "hover", "pressed"):
        cv = Canvas(SLOT, unit=1)
        slot_frame(cv, state)
        save(f"slot_frame_{state}", cv)
    cv = Canvas(SLOT, unit=1)
    slot_back(cv)
    save("slot_back", cv)
    cv = Canvas(SLOT, unit=1)
    slot_back(cv, empty=True)
    slot_frame(cv, "empty")
    save("slot_empty", cv)
    for name, fn in (("slot_cooldown", ui_cooldown_shade), ("slot_nomana", ui_nomana),
                     ("slot_ready_flash", ui_ready_flash), ("slot_locked", ui_lock_overlay)):
        cv = Canvas(SLOT, unit=1)
        fn(cv)
        save(name, cv)
    cv = Canvas(32, unit=1)
    ui_lock_icon(cv)
    save("lock_icon", cv)
    cv = Canvas(24, unit=1)
    ui_key_badge(cv)
    save("key_badge", cv)
    for on in (True, False):
        cv = Canvas(16, unit=1)
        rank_pip(cv, on)
        save(f"rank_pip_{'on' if on else 'off'}", cv)
    for n in range(4):
        save(f"rank_pips_{n}", rank_strip(n))
    for name, (w, h, segs) in connector_pieces().items():
        for lit in (True, False):
            cv = Canvas(w, h, unit=1)
            connector(cv, segs, lit)
            save(f"{name}_{'lit' if lit else 'unlit'}", cv)
    for key in BRANCH:
        cv = Canvas(192, 48, unit=1)
        ui_branch_header(cv, key)
        save(f"branch_{key}", cv)
    cv = Canvas(32, unit=1)
    ui_skill_point_marker(cv, True)
    save("skill_point_marker", cv)
    cv = Canvas(32, unit=1)
    ui_skill_point_marker(cv, False)
    save("skill_point_marker_blank", cv)
    return out


# ---------------------------------------------------------------------------------------------
# Contact sheets (outside the asset tree)

UI_DARK = (22, 24, 30)


class Sheet:
    """Linear premultiplied sheet; place straight-alpha RGBA images at integer positions."""

    def __init__(self, w, h, bg=UI_DARK):
        self.img = np.zeros((h, w, 3))
        self.img[:] = _lin(np.array(bg) / 255.0)

    def rect(self, x0, y0, w, h, color):
        self.img[y0:y0 + h, x0:x0 + w] = _lin(np.array(color) / 255.0)

    def place(self, rgba, x0, y0, size=None):
        if size is not None and (size, size) != rgba.shape[:2]:
            pm = resize(rgba, size, size)
        else:
            pm = np.concatenate([_lin(rgba[..., :3]) * rgba[..., 3:], rgba[..., 3:]], -1)
        h, w = pm.shape[:2]
        region = self.img[y0:y0 + h, x0:x0 + w]
        region[:] = pm[..., :3] + region * (1 - pm[..., 3:])

    def save(self, path):
        vc.write_png(path, vc.to_u8(_srgb(self.img)))
        print(f"SHEET {path}")


def resize(rgba, w, h):
    """Area-weighted resample of straight RGBA on premultiplied linear colour (like a GPU mip +
    bilinear, close enough for a contact sheet); returns premultiplied linear RGBA."""
    pm = np.concatenate([_lin(rgba[..., :3]) * rgba[..., 3:], rgba[..., 3:]], -1)
    H, W = pm.shape[:2]

    def axis_matrix(n_out, n_in):
        m = np.zeros((n_out, n_in))
        scale = n_in / n_out
        for i in range(n_out):
            a, b = i * scale, (i + 1) * scale
            for j in range(int(math.floor(a)), min(n_in, int(math.ceil(b)))):
                m[i, j] = min(b, j + 1) - max(a, j)
            m[i] /= m[i].sum()
        return m

    my, mx = axis_matrix(h, H), axis_matrix(w, W)
    return np.einsum("ij,jkc,lk->ilc", my, pm, mx)


def load_png(path):
    return vc.read_image(path)


def icon_with_slot(sheet, ui, icon, x0, y0, icon_px):
    """Slot art scaled so its window holds the icon at icon_px."""
    slot_px = int(round(icon_px * 64 / 52))
    off = (slot_px - icon_px) // 2
    sheet.place(ui["slot_back"], x0, y0, slot_px)
    sheet.place(icon, x0 + off, y0 + off, icon_px)
    sheet.place(ui["slot_frame_normal"], x0, y0, slot_px)
    return slot_px


def sheet_icons(icons, ui, path):
    pad = 14
    cols = 7
    w64 = int(round(64 * 64 / 52))
    w48 = int(round(48 * 64 / 52))
    row_h = pad + w64 + 6 + w48
    W = max(pad + cols * (w64 + pad), pad + 24 * 36 + pad)
    H = pad + len(CLASS_ROWS) * (row_h + pad) + 64 + pad
    sh = Sheet(W, H)
    for r, (old, new) in enumerate(CLASS_ROWS):
        y0 = pad + r * (row_h + pad)
        for c, name in enumerate([old] + new):
            x0 = pad + c * (w64 + pad)
            icon_with_slot(sh, ui, icons[name], x0, y0, 64)
            icon_with_slot(sh, ui, icons[name], x0 + (w64 - w48) // 2, y0 + w64 + 6, 48)
    # Bottom strip: every icon raw at 32 px (the smallest size a tooltip would use).
    y0 = H - 64 - pad + 16
    for k, name in enumerate([n for _, row in CLASS_ROWS for n in row]):
        sh.place(icons[name], pad + k * 36, y0, 32)
    sh.save(path)


def sheet_status(status, path):
    pad = 10
    names = list(status)
    W = pad + len(names) * (48 + pad)
    H = pad + 48 + pad + 24 + pad + 24 + pad
    sh = Sheet(W, H)
    for k, n in enumerate(names):
        x0 = pad + k * (48 + pad)
        sh.place(status[n], x0, pad, 48)
        sh.place(status[n], x0 + 12, pad + 48 + pad, 24)
    # A mock buff row as under a portrait: 24 px with 2 px gaps on the panel colour.
    sh.rect(pad - 4, pad * 3 + 72 - 4, len(names) * 26 + 6, 32, (43, 51, 61))
    for k, n in enumerate(names):
        sh.place(status[n], pad + k * 26, pad * 3 + 72, 24)
    sh.save(path)


def sheet_ui(ui, icons, path):
    W, H = 1000, 520
    sh = Sheet(W, H, bg=(26, 31, 38))
    x, y = 12, 12
    order = ["slot_back", "slot_frame_normal", "slot_frame_hover", "slot_frame_pressed", "slot_empty",
             "slot_cooldown", "slot_nomana", "slot_ready_flash", "slot_locked", "lock_icon", "key_badge",
             "skill_point_marker", "skill_point_marker_blank", "rank_pip_on", "rank_pip_off"]
    for n in order:
        img = ui[n]
        h, w = img.shape[:2]
        sh.place(img, x, y)
        x += w + 10
    # 2x of the slot states with an icon (bar mock) -------------------------------------------
    y = 96
    x = 12
    demo = icons["yarma"]
    states = [("normal", None), ("hover", None), ("pressed", None), ("nomana", "slot_nomana"),
              ("cooldown", "slot_cooldown"), ("locked", "slot_locked"), ("ready", "slot_ready_flash")]
    for k, (st, overlay) in enumerate(states):
        x0 = x + k * 90
        sh.place(ui["slot_back"], x0, y)
        sh.place(demo, x0 + 6, y + 6, 52)
        if overlay is not None:
            sh.place(ui[overlay], x0, y)
        frame = {"hover": "slot_frame_hover", "pressed": "slot_frame_pressed"}.get(st, "slot_frame_normal")
        sh.place(ui[frame], x0, y)
        sh.place(ui["key_badge"], x0 - 4, y - 4)
        sh.place(ui["rank_pips_2"], x0 + 6, y + 68)
    sh.place(ui["slot_empty"], x + 7 * 90, y)
    # A mock skill bar: 8 slots at 48 px icons.
    y = 196
    bar = [icons[n] for n in ("agir_darbe", "yarma", "kan_ofkesi", "sarsinti", "kalkan_duvari", "meydan_okuma",
                              "direncli_soluk")]
    sh.rect(8, y - 6, 8 * 64 + 8, 72, (43, 51, 61))
    for k in range(8):
        x0 = 12 + k * 64
        if k < len(bar):
            icon_with_slot(sh, ui, bar[k], x0, y, 48)
        else:
            sh.place(ui["slot_empty"], x0, y, 59)
        sh.place(ui["key_badge"], x0 - 2, y - 2, 18)
    # Branch headers --------------------------------------------------------------------------
    y = 290
    for k, key in enumerate(BRANCH):
        sh.place(ui[f"branch_{key}"], 12 + (k % 4) * 200, y + (k // 4) * 56)
    # Connectors at 1x and a mock tree --------------------------------------------------------
    y = 410
    x = 12
    for lit in ("lit", "unlit"):
        for n in ("tree_v", "tree_h", "tree_elbow_se", "tree_elbow_sw", "tree_elbow_ne", "tree_elbow_nw", "tree_tee_s"):
            img = ui[f"{n}_{lit}"]
            sh.place(img, x, y)
            x += img.shape[1] + 6
        x += 20
    # Mock tree: a learned node on top, a tee down to two children; the left path is lit (learned),
    # the right child is locked.
    tx, ty = 880, 300
    sh.place(icons["kalkan_duvari"], tx, ty, 40)
    sh.place(ui["rank_pips_3"], tx - 5, ty + 42)
    sh.place(ui["tree_v_lit"], tx + 12, ty + 58)
    sh.place(ui["tree_tee_s_lit"], tx + 12, ty + 90)
    sh.place(ui["tree_h_lit"], tx - 20, ty + 90)
    sh.place(ui["tree_elbow_se_lit"], tx - 36, ty + 90)
    sh.place(ui["tree_v_lit"], tx - 36, ty + 106)
    sh.place(ui["tree_h_unlit"], tx + 28, ty + 90)
    sh.place(ui["tree_elbow_sw_unlit"], tx + 60, ty + 90)
    sh.place(ui["tree_v_unlit"], tx + 60, ty + 106)
    sh.place(icons["meydan_okuma"], tx - 48, ty + 138, 40)
    sh.place(icons["direncli_soluk"], tx + 48, ty + 138, 40)
    sh.place(ui["slot_locked"], tx + 45, ty + 135, 46)
    sh.place(ui["skill_point_marker"], tx + 30, ty - 12, 24)
    sh.save(path)


def main():
    args = vc.script_args()
    sheets = DEFAULT_SHEETS
    if "--sheets" in args:
        sheets = args[args.index("--sheets") + 1]
    only = args[args.index("--only") + 1] if "--only" in args else None
    icons, status, ui = {}, {}, {}
    if only in (None, "icons"):
        icons = make_icons()
    if only in (None, "status"):
        status = make_status()
    if only in (None, "ui"):
        ui = make_ui()
    if "--no-preview" in args:
        return
    vc.ensure_dir(sheets)
    if not icons:
        icons = {n: load_png(os.path.join(SKILL_DIR, f"{n}.png")) for n in SKILLS}
    for old, _ in CLASS_ROWS:
        icons[old] = load_png(os.path.join(SKILL_DIR, f"{old}.png"))
    if not ui:
        ui = {os.path.splitext(f)[0]: load_png(os.path.join(BAR_DIR, f)) for f in sorted(os.listdir(BAR_DIR))}
    if not status:
        status = {n: load_png(os.path.join(STATUS_DIR, f"{n}.png")) for n in STATUS}
    sheet_icons(icons, ui, os.path.join(sheets, "m12_icons.png"))
    # 3x nearest-neighbour zoom of the new icons (pixel inspection).
    names = [n for _, row in CLASS_ROWS for n in row]
    z = Sheet(8 + 8 * 200, 8 + 3 * 200)
    for k, n in enumerate(names):
        big = np.repeat(np.repeat(icons[n], 3, 0), 3, 1)
        z.place(big, 8 + (k % 8) * 200, 8 + (k // 8) * 200)
    z.save(os.path.join(sheets, "m12_icons_zoom.png"))
    sheet_status(status, os.path.join(sheets, "m12_status.png"))
    sheet_ui(ui, icons, os.path.join(sheets, "m12_ui.png"))


if __name__ == "__main__":
    main()
