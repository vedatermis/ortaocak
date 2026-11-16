"""Combat presentation assets: the Mage's ember bolt projectile and the four skill icons.

Usage (headless):
  blender --background --factory-startup --python tools/blender/gen_effects.py -- [--no-preview]

Outputs:
  assets_src/effects/ember_bolt.glb      projectile mesh (~200 triangles), pointing along engine +Z
                                         (Blender -Y), origin at its centre; base colour + emissive
  assets_src/effects/ember_bolt.png      16x64 base colour gradient along the bolt (V = 0 nose, 1 tail)
  assets_src/effects/ember_bolt_e.png    16x64 emissive gradient (white-yellow nose -> deep red tail)
  assets_src/ui/skills/<id>.png          64x64 skill icons, straight sRGB alpha (assetc writes the
                                         premultiplied RGBA8 .vtex next to the copy, like ui/items)
  assets_src/previews/effects/skills.png icons at 64 and 48 px on the dark UI colour, on a light tile
  assets_src/previews/effects/ember_bolt.png
Icons are painted procedurally (signed distance fields, 4x supersampled, box-filtered on
premultiplied linear colour): a rounded badge in the class colour with a bold, outlined symbol, so
they read at 48 px on a dark UI. Deterministic.
"""

import math
import os
import sys

import bpy
import numpy as np
from mathutils import Vector

sys.dont_write_bytecode = True
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import vo_common as vc  # noqa: E402

EFFECTS_DIR = os.path.join(vc.SRC, "effects")
SKILL_DIR = os.path.join(vc.SRC, "ui", "skills")
PREVIEW_DIR = os.path.join(vc.PREVIEWS, "effects")

ICON = 64
SS = 4
UI_DARK = (22, 24, 30)  # behind the previews: the HUD's dark panel colour


# ---------------------------------------------------------------------------------------------
# Small SDF painter (coordinates in icon units: 0..1, y down)

def _grid(n):
    c = (np.arange(n) + 0.5) / n
    return np.meshgrid(c, c)


def _lin(c):
    c = np.asarray(c, np.float64)
    return np.where(c <= 0.04045, c / 12.92, ((c + 0.055) / 1.055) ** 2.4)


def _srgb(c):
    c = np.clip(c, 0.0, 1.0)
    return np.where(c <= 0.0031308, c * 12.92, 1.055 * np.power(c, 1.0 / 2.4) - 0.055)


def hexc(h):
    h = h.lstrip("#")
    return np.array([int(h[i:i + 2], 16) / 255.0 for i in (0, 2, 4)])


class Canvas:
    """Premultiplied linear RGBA at ICON * SS, composited front to back with `over`."""

    def __init__(self):
        self.n = ICON * SS
        self.x, self.y = _grid(self.n)
        self.px = 1.0 / self.n
        self.rgba = np.zeros((self.n, self.n, 4))

    def cover(self, sdf, soft=1.0):
        """Coverage from a signed distance (negative inside), anti-aliased over `soft` pixels."""
        return np.clip(0.5 - sdf / (self.px * soft), 0.0, 1.0)

    def over(self, alpha, color):
        """color: sRGB (3,) or an (n, n, 3) sRGB array."""
        col = _lin(color)
        if col.ndim == 1:
            col = np.broadcast_to(col, (self.n, self.n, 3))
        a = alpha[..., None]
        self.rgba[..., :3] = col * a + self.rgba[..., :3] * (1.0 - a)
        self.rgba[..., 3:] = a + self.rgba[..., 3:] * (1.0 - a)

    def result(self):
        """Straight sRGB RGBA float at ICON^2 (box filter on premultiplied linear colour)."""
        f = SS
        pm = self.rgba.reshape(ICON, f, ICON, f, 4).mean(axis=(1, 3))
        a = pm[..., 3:4]
        col = np.where(a > 1e-6, pm[..., :3] / np.maximum(a, 1e-6), 0.0)
        return np.concatenate([_srgb(col), a], -1)


def sd_round_rect(x, y, cx, cy, hw, hh, r):
    qx = np.abs(x - cx) - hw + r
    qy = np.abs(y - cy) - hh + r
    return np.hypot(np.maximum(qx, 0), np.maximum(qy, 0)) + np.minimum(np.maximum(qx, qy), 0) - r


def sd_circle(x, y, cx, cy, r):
    return np.hypot(x - cx, y - cy) - r


def sd_stroke(x, y, pts, widths):
    """Union of tapered capsules along a polyline: half-width interpolated per segment."""
    d = np.full(x.shape, 1e9)
    for (ax, ay), (bx, by), wa, wb in zip(pts, pts[1:], widths, widths[1:]):
        abx, aby = bx - ax, by - ay
        L2 = abx * abx + aby * aby
        t = np.clip(((x - ax) * abx + (y - ay) * aby) / max(L2, 1e-12), 0.0, 1.0)
        dist = np.hypot(x - (ax + abx * t), y - (ay + aby * t)) - (wa + (wb - wa) * t)
        d = np.minimum(d, dist)
    return d


def sd_polygon(x, y, pts):
    """Exact signed distance to a simple polygon (negative inside)."""
    P = np.asarray(pts, float)
    d = np.full(x.shape, 1e9)
    s = np.ones(x.shape)
    n = len(P)
    for i in range(n):
        ax, ay = P[i - 1]
        bx, by = P[i]
        ex, ey = bx - ax, by - ay
        wx, wy = x - ax, y - ay
        t = np.clip((wx * ex + wy * ey) / (ex * ex + ey * ey), 0, 1)
        d = np.minimum(d, np.hypot(wx - ex * t, wy - ey * t))
        c1 = y >= ay
        c2 = y < by
        c3 = ex * wy > ey * wx
        flip = (c1 & c2 & c3) | (~c1 & ~c2 & ~c3)
        s = np.where(flip, -s, s)
    return s * d


def arc_points(cx, cy, r, a0, a1, n=24):
    return [(cx + r * math.cos(math.radians(a0 + (a1 - a0) * k / (n - 1))),
             cy + r * math.sin(math.radians(a0 + (a1 - a0) * k / (n - 1)))) for k in range(n)]


def taper(n, w0, wmid, w1):
    """Half-widths along n points: w0 -> wmid (middle) -> w1, smooth."""
    out = []
    for k in range(n):
        t = k / (n - 1)
        out.append(w0 + (wmid - w0) * math.sin(math.pi * min(t, 0.5)) if t <= 0.5
                   else w1 + (wmid - w1) * math.sin(math.pi * max(t, 0.5)))
    return out


# ---------------------------------------------------------------------------------------------
# Icons

def badge(cv, top, bottom, rim):
    """Rounded square in the class colour: vertical gradient, 1.5 px dark border, a lighter rim."""
    x, y = cv.x, cv.y
    outer = sd_round_rect(x, y, 0.5, 0.5, 0.485, 0.485, 0.12)
    grad = np.clip((y - 0.04) / 0.92, 0, 1)[..., None]
    # Soft radial lift behind the symbol.
    glow = np.clip(1.0 - np.hypot(x - 0.5, y - 0.45) / 0.55, 0, 1)[..., None] ** 2 * 0.35
    col = hexc(top) * (1 - grad) + hexc(bottom) * grad
    col = col + (1 - col) * glow * 0.4
    cv.over(cv.cover(outer), np.full((cv.n, cv.n, 3), hexc("#0b0c10")))
    inner = sd_round_rect(x, y, 0.5, 0.5, 0.485 - 0.025, 0.485 - 0.025, 0.10)
    cv.over(cv.cover(inner), hexc(rim))
    inner2 = sd_round_rect(x, y, 0.5, 0.5, 0.485 - 0.045, 0.485 - 0.045, 0.085)
    cv.over(cv.cover(inner2), col)


def symbol(cv, sdf, fill, outline="#0b0c10", width=0.035, shade=None):
    """A symbol with a dark outline `width` wide (icon units) and an optional gradient fill."""
    cv.over(cv.cover(sdf - width), hexc(outline))
    a = cv.cover(sdf)
    if shade is not None:
        cv.over(a, shade)
    else:
        cv.over(a, hexc(fill))


def lerp_field(t, c0, c1):
    t = np.clip(t, 0, 1)[..., None]
    return hexc(c0) * (1 - t) + hexc(c1) * t


def icon_agir_darbe(cv):
    """Heavy blow: a broad sword cutting down-right, a heavy strike arc sweeping behind it."""
    badge(cv, "#7a1c18", "#2c0907", "#c9a052")
    x, y = cv.x, cv.y
    # Strike arc: a wide crescent from the top right round the left down to the bottom.
    pts = arc_points(0.53, 0.55, 0.33, -70, 150, 32)
    arc = sd_stroke(x, y, pts, taper(32, 0.006, 0.075, 0.006))
    # Cut the inner side so it reads as a crescent swoosh.
    arc = np.maximum(arc, -sd_circle(x, y, 0.60, 0.50, 0.25))
    t = np.clip((np.arctan2(y - 0.55, x - 0.53) + 1.2) / 3.8, 0, 1)
    symbol(cv, arc, None, width=0.03, shade=lerp_field(t, "#fff6c8", "#ff8a2a"))
    # Sword: blade from the hilt (lower left) to the tip (upper right).
    hx, hy, tx, ty = 0.27, 0.74, 0.80, 0.21
    blade = sd_polygon(x, y, [(hx + 0.055, hy - 0.002), (tx - 0.02, ty - 0.03), (tx + 0.025, ty + 0.02),
                              (hx + 0.06, hy + 0.06), (hx + 0.002, hy + 0.055)])
    guard = sd_stroke(x, y, [(0.19, 0.64), (0.37, 0.82)], [0.032, 0.032])
    grip = sd_stroke(x, y, [(0.24, 0.77), (0.13, 0.88)], [0.03, 0.026])
    pommel = sd_circle(x, y, 0.115, 0.895, 0.042)
    hilt = np.minimum(np.minimum(guard, grip), pommel)
    whole = np.minimum(blade, hilt)
    cv.over(cv.cover(whole - 0.035), hexc("#0b0c10"))
    u = ((x - hx) * (tx - hx) + (y - hy) * (ty - hy)) / ((tx - hx) ** 2 + (ty - hy) ** 2)
    side = ((x - hx) * (hy - ty) - (y - hy) * (hx - tx)) / math.hypot(tx - hx, ty - hy)
    steel = lerp_field(np.where(side > 0.0, 0.0, 0.55) + u * 0.2, "#f4f6fa", "#8d97a6")
    cv.over(cv.cover(blade), steel)
    cv.over(cv.cover(hilt), lerp_field(y - 0.6, "#e8c060", "#8a5a1c"))


def icon_cifte_kesik(cv):
    """Twin slash: two curved cuts crossing in an X, bright edges on a dark green badge."""
    badge(cv, "#1d5a43", "#06201a", "#c9a052")
    x, y = cv.x, cv.y
    cuts = []
    for sx in (1.0, -1.0):
        pts = []
        n = 28
        for k in range(n):
            t = k / (n - 1)
            px = 0.5 + sx * (-0.34 + 0.68 * t)
            py = 0.16 + 0.68 * t
            bow = 0.07 * math.sin(math.pi * t)
            pts.append((px + sx * bow, py - bow))
        cuts.append(sd_stroke(x, y, pts, taper(n, 0.004, 0.062, 0.004)))
    for k, c in enumerate(cuts):
        cv.over(cv.cover(c - 0.035), hexc("#0b0c10"))
        g = np.clip(1.0 - np.abs(c) / 0.06, 0, 1)
        shade = lerp_field(1.0 - g, "#ffffff", "#7cf0c6")
        cv.over(cv.cover(c), shade)


def icon_koz_oku(cv):
    """Ember bolt: a flaming teardrop flying up-right with a tapering trail and two sparks."""
    badge(cv, "#2a2366", "#0c0a26", "#c9a052")
    x, y = cv.x, cv.y
    hx, hy = 0.66, 0.34          # head centre
    tx, ty = 0.15, 0.85          # trail end
    ux, uy = (hx - tx), (hy - ty)
    L = math.hypot(ux, uy)
    ux, uy = ux / L, uy / L
    # Flame: union of a head circle and a trail that narrows, with a wavy edge.
    n = 30
    trail = [(tx + (hx - tx) * k / (n - 1) + 0.018 * math.sin(k * 0.8) * -uy,
              ty + (hy - ty) * k / (n - 1) + 0.018 * math.sin(k * 0.8) * ux) for k in range(n)]
    widths = [0.02 + 0.13 * (k / (n - 1)) ** 1.4 for k in range(n)]
    flame = np.minimum(sd_stroke(x, y, trail, widths), sd_circle(x, y, hx, hy, 0.16))
    core_tr = [(tx + 0.25 * (hx - tx) + (hx - tx) * 0.75 * k / (n - 1), ty + 0.25 * (hy - ty) + (hy - ty) * 0.75 * k / (n - 1))
               for k in range(n)]
    core = np.minimum(sd_stroke(x, y, core_tr, [0.006 + 0.07 * (k / (n - 1)) ** 1.5 for k in range(n)]),
                      sd_circle(x, y, hx + 0.015, hy - 0.015, 0.09))
    s = np.clip(((x - tx) * ux + (y - ty) * uy) / L, 0, 1)
    symbol(cv, flame, None, width=0.035, shade=lerp_field(1.0 - s, "#ff9a2a", "#b0200c"))
    cv.over(cv.cover(core), lerp_field(1.0 - s, "#fffbe6", "#ffd040"))
    for (sx, sy, r) in ((0.86, 0.15, 0.028), (0.88, 0.45, 0.022), (0.50, 0.17, 0.02)):
        sp = sd_circle(x, y, sx, sy, r)
        symbol(cv, sp, "#ffd060", width=0.022)


def icon_soluk_gunes(cv):
    """Pale sun (healing): a pale disk with long and short rays and a soft halo."""
    badge(cv, "#2d5f74", "#0d2230", "#c9a052")
    x, y = cv.x, cv.y
    cx, cy = 0.5, 0.5
    halo = np.clip(1.0 - np.hypot(x - cx, y - cy) / 0.42, 0, 1) ** 2
    cv.over(halo * 0.45, hexc("#e8fff6"))
    rays = np.full(x.shape, 1e9)
    for k in range(12):
        a = math.radians(k * 30 - 90)
        long = k % 2 == 0
        r0, r1 = 0.205, (0.43 if long else 0.33)
        w = 0.055 if long else 0.04
        ca, sa = math.cos(a), math.sin(a)
        p = [(cx + ca * r0 - sa * w, cy + sa * r0 + ca * w), (cx + ca * r1, cy + sa * r1),
             (cx + ca * r0 + sa * w, cy + sa * r0 - ca * w)]
        rays = np.minimum(rays, sd_polygon(x, y, p))
    symbol(cv, rays, "#f7efc4", width=0.03)
    disk = sd_circle(x, y, cx, cy, 0.19)
    r = np.hypot(x - cx + 0.05, y - cy + 0.05) / 0.26
    symbol(cv, disk, None, width=0.035, shade=lerp_field(r, "#fffef6", "#e8dc9a"))
    # A small healing cross in the disk.
    cross = np.minimum(sd_round_rect(x, y, cx, cy, 0.03, 0.10, 0.012), sd_round_rect(x, y, cx, cy, 0.10, 0.03, 0.012))
    cv.over(cv.cover(cross), hexc("#5fbf8a"))


SKILLS = {
    # id (gamedata skill icon name): painter
    "agir_darbe": icon_agir_darbe,
    "cifte_kesik": icon_cifte_kesik,
    "koz_oku": icon_koz_oku,
    "soluk_gunes": icon_soluk_gunes,
}


def make_icons():
    out = {}
    for name, painter in SKILLS.items():
        cv = Canvas()
        painter(cv)
        rgba = cv.result()
        vc.write_png(os.path.join(SKILL_DIR, f"{name}.png"), vc.to_u8(rgba))
        out[name] = rgba
        print(f"SKILL icon {name}: {ICON}x{ICON}, coverage {float(rgba[..., 3].mean()):.2f}")
    return out


def _resize(rgba, size):
    """Bilinear resample of straight RGBA (preview only), on premultiplied colour."""
    h = rgba.shape[0]
    c = (np.arange(size) + 0.5) * h / size - 0.5
    i0 = np.clip(np.floor(c).astype(int), 0, h - 1)
    i1 = np.clip(i0 + 1, 0, h - 1)
    f = np.clip(c - i0, 0, 1)
    pm = np.concatenate([_lin(rgba[..., :3]) * rgba[..., 3:], rgba[..., 3:]], -1)
    rows = pm[i0] * (1 - f)[:, None, None] + pm[i1] * f[:, None, None]
    out = rows[:, i0] * (1 - f)[None, :, None] + rows[:, i1] * f[None, :, None]
    return out


def icons_preview(icons):
    """Each icon at 64 and 48 px over the dark UI colour, then at 64 px over a light tile."""
    pad = 12
    cols = len(icons)
    W = pad + cols * (64 + pad)
    H = pad + (64 + pad) * 2 + 48 + pad
    sheet = np.zeros((H, W, 3))
    sheet[:] = _lin(np.array(UI_DARK) / 255.0)
    for k, rgba in enumerate(icons.values()):
        x0 = pad + k * (64 + pad)
        for (y0, size, bg) in ((pad, 64, None), (pad + 64 + pad, 48, None), (pad + 2 * (64 + pad), 64, (0.62, 0.64, 0.66))):
            pm = _resize(rgba, size) if size != ICON else np.concatenate([_lin(rgba[..., :3]) * rgba[..., 3:], rgba[..., 3:]], -1)
            if bg is not None:
                H2 = min(size, H - y0)
                sheet[y0:y0 + H2, x0:x0 + size] = _lin(np.array(bg))
            region = sheet[y0:y0 + size, x0:x0 + size]
            hh = region.shape[0]
            region[:] = pm[:hh, :, :3] + region * (1 - pm[:hh, :, 3:])
    vc.write_png(os.path.join(PREVIEW_DIR, "skills.png"), vc.to_u8(_srgb(sheet)))


# ---------------------------------------------------------------------------------------------
# Ember bolt

BOLT_LEN = 0.50   # m, nose to tail tip
BOLT_R = 0.065    # m, widest radius
BOLT_SEG = 10     # around
BOLT_STRENGTH = 4.0


def bolt_profile():
    """(s, radius) along the bolt from the nose (s = 0) to the tail (s = 1): a rounded head that
    tapers into a long flame tail."""
    rows = []
    for s in (0.0, 0.03, 0.08, 0.15, 0.24, 0.34, 0.46, 0.58, 0.70, 0.82, 0.92, 1.0):
        if s < 0.24:
            r = BOLT_R * math.sin(math.acos(1.0 - s / 0.24))
        else:
            r = BOLT_R * (1.0 - (s - 0.24) / 0.76) ** 1.3
        rows.append((s, r))
    return rows


def make_bolt():
    """Lathed flame body with a twist and small waves (reads as a flickering flame when it spins),
    plus three short fins near the tail. Blender: nose towards -Y (engine +Z), origin at the
    widest ring."""
    verts, faces, uvs = [], [], []
    prof = bolt_profile()
    nose_y = -BOLT_LEN * 0.24
    ring_idx = []
    for k, (s, r) in enumerate(prof):
        yy = nose_y + s * BOLT_LEN
        if r < 1e-6:
            ring_idx.append([len(verts)])
            verts.append((0.0, yy, 0.0))
            uvs.append([(0.5, s)])
            continue
        twist = s * 1.6
        idx = []
        for j in range(BOLT_SEG):
            a = 2 * math.pi * j / BOLT_SEG + twist
            wave = 1.0 + (0.18 * math.sin(3 * a + 5 * s) if s > 0.25 else 0.0)
            idx.append(len(verts))
            verts.append((r * wave * math.cos(a), yy, r * wave * math.sin(a)))
        ring_idx.append(idx)
    for k in range(len(ring_idx) - 1):
        A, B = ring_idx[k], ring_idx[k + 1]
        if len(A) == 1:
            for j in range(BOLT_SEG):
                faces.append((A[0], B[(j + 1) % BOLT_SEG], B[j]))
        elif len(B) == 1:
            for j in range(BOLT_SEG):
                faces.append((A[j], A[(j + 1) % BOLT_SEG], B[0]))
        else:
            for j in range(BOLT_SEG):
                faces.append((A[j], A[(j + 1) % BOLT_SEG], B[(j + 1) % BOLT_SEG], B[j]))
    obj = vc.mesh_object("ember_bolt", verts, faces, None, [], smooth=True, recalc=True)
    me = obj.data
    # Triangulate, UV = (around, along) with V from the vertex position along the bolt.
    import bmesh
    bm = bmesh.new()
    bm.from_mesh(me)
    bmesh.ops.triangulate(bm, faces=bm.faces)
    uvl = bm.loops.layers.uv.new("UVMap")
    for f in bm.faces:
        for lp in f.loops:
            s = (lp.vert.co.y - nose_y) / BOLT_LEN
            lp[uvl].uv = (0.5, 1.0 - min(max(s, 0.0), 1.0))  # glTF V = s (image row 0 = nose)
    bm.to_mesh(me)
    bm.free()
    me.shade_smooth()
    return obj


def bolt_textures():
    """16x64 gradients, row 0 = nose. Base colour: warm; emissive: white-yellow -> orange -> red."""
    h, w = 64, 16
    s = (np.arange(h) + 0.5) / h
    stops = [(0.0, "#fff8e0"), (0.18, "#ffd25a"), (0.45, "#ff8a1e"), (0.75, "#e0400e"), (1.0, "#801606")]
    col = np.zeros((h, 3))
    for (s0, c0), (s1, c1) in zip(stops, stops[1:]):
        m = (s >= s0) & (s <= s1)
        t = ((s[m] - s0) / (s1 - s0))[:, None]
        col[m] = _srgb(_lin(hexc(c0)) * (1 - t) + _lin(hexc(c1)) * t)
    emis = np.repeat(col[:, None, :], w, 1)
    base = np.repeat(_srgb(_lin(col) * 0.6)[:, None, :], w, 1)
    paths = dict(color=os.path.join(EFFECTS_DIR, "ember_bolt.png"), emissive=os.path.join(EFFECTS_DIR, "ember_bolt_e.png"))
    vc.write_png(paths["color"], vc.to_u8(base))
    vc.write_png(paths["emissive"], vc.to_u8(emis))
    return paths


def bolt_material(paths):
    mat = bpy.data.materials.new("ember_bolt")
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
        return n

    nt.links.new(tex(paths["color"]).outputs["Color"], bsdf.inputs["Base Color"])
    nt.links.new(tex(paths["emissive"]).outputs["Color"], bsdf.inputs["Emission Color"])
    bsdf.inputs["Emission Strength"].default_value = BOLT_STRENGTH
    return mat


def bolt_preview(obj):
    sc = bpy.context.scene
    cam = vc.setup_preview((640, 360), background="#1a1c22", sun_dir=(-0.5, -1.0, 0.8), strength=1.0)
    sc.render.engine = 'CYCLES'
    sc.cycles.device = 'CPU'
    sc.cycles.samples = 32
    sc.cycles.seed = 0
    sc.cycles.use_denoising = False
    sc.render.threads_mode = 'FIXED'
    sc.render.threads = 8
    vc.aim_camera(cam, (0.55, -0.35, 0.30), (0.0, 0.02, 0.0), lens=50)
    vc.render_to(os.path.join(PREVIEW_DIR, "ember_bolt.png"), stable=True)


def main():
    args = vc.script_args()
    vc.reset_scene()
    icons = make_icons()
    paths = bolt_textures()
    obj = make_bolt()
    obj.data.materials.append(bolt_material(paths))
    tris = vc.triangle_count(obj)
    V = np.array([v.co for v in obj.data.vertices])
    vc.export_glb(os.path.join(EFFECTS_DIR, "ember_bolt.glb"), [obj], textures=True)
    print(f"EFFECT ember_bolt: {tris} triangles, {len(V)} vertices, length {float(V[:, 1].max() - V[:, 1].min()):.3f} m, "
          f"nose at Blender y {float(V[:, 1].min()):.3f} (engine +Z)")
    if "--no-preview" not in args:
        icons_preview(icons)
        bolt_preview(obj)


if __name__ == "__main__":
    main()
