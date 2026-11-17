"""M14 "Pazar" icons and shop UI art.

Item icons (the inventory / skill-bar style of the weapon and loot icons: procedural mesh rendered
with Cycles at 512^2, box-filtered 4x on premultiplied linear colour, soft dark halo, stable
writer; gen_weapons.render_icon / gen_loot.render_icon_obj):
  assets_src/ui/items/potion_hp_small.png    round-bellied flask, short neck, cork: red
  assets_src/ui/items/potion_hp_medium.png   tall shouldered bottle, long neck, wax-sealed stopper,
                                             cord and a paper tag: red
  assets_src/ui/items/potion_mp_small.png    as hp_small, blue
  assets_src/ui/items/potion_mp_medium.png   as hp_medium, blue
  All four are framed by the same reference box (the medium bottle's), so the small flask is drawn
  smaller in the slot, not just a different shape.

Shop UI art (the HUD glyphs' SDF painter and ink outline, gen_hud_art.py / gen_skill_art.py):
  assets_src/ui/shop/akce.png            64^2  one Akçe coin (price columns, the purse line)
  assets_src/ui/shop/tab_al.png          64^2  "Al" (buy) tab glyph: a potion dropping into an open palm
  assets_src/ui/shop/tab_sat.png         64^2  "Sat" (sell) tab glyph: an open palm holding out a coin (you are paid)
  assets_src/ui/shop/marker_potion.png   128^2 overhead merchant marker: flask on a round plaque
  assets_src/ui/shop/marker_smith.png    128^2 overhead merchant marker: hammer on anvil on a plaque
assetc compiles every PNG directly in ui/shop/ to a premultiplied RGBA8 .vtex (the UI-icon rule).

Contact sheet (not an asset; default build/art_sheets/m14): shop_icons.png.

Usage (headless):
  blender --background --factory-startup --python tools/blender/gen_shop_icons.py -- [--only items|ui] [--sheets <dir>]
Deterministic.
"""

import math
import os
import sys

import bmesh
import bpy
import numpy as np
from mathutils import Matrix

sys.dont_write_bytecode = True
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import vo_common as vc  # noqa: E402
import gen_props as gp  # noqa: E402
import gen_weapons as gw  # noqa: E402
import gen_loot as gl  # noqa: E402
import gen_skill_art as sa  # noqa: E402
from gen_effects import sd_round_rect, sd_circle, sd_stroke, sd_polygon, hexc, lerp_field  # noqa: E402
from gen_skill_art import Canvas, union, sub, sd_ellipse, rot, INK  # noqa: E402

ICON_DIR = os.path.join(vc.SRC, "ui", "items")
SHOP_DIR = os.path.join(vc.SRC, "ui", "shop")
DEFAULT_SHEETS = os.path.join(vc.REPO, "build", "art_sheets", "m14")

POTIONS = {
    # icon: (liquid colour, glow colour, size)
    "potion_hp_small": ("#d01a1e", "#ff3020", "small"),
    "potion_hp_medium": ("#c0141a", "#ff2a1a", "medium"),
    "potion_mp_small": ("#1d5ce0", "#3a86ff", "small"),
    "potion_mp_medium": ("#1848c8", "#2f74ff", "medium"),
}

# Bottle profiles (radius, height) bottom to top, in units (the medium bottle is 1.0 tall).
SMALL = [(0.0, 0.0), (0.17, 0.005), (0.3, 0.065), (0.36, 0.18), (0.355, 0.29), (0.29, 0.39), (0.16, 0.46),
         (0.09, 0.49), (0.085, 0.6), (0.105, 0.62), (0.105, 0.645), (0.0, 0.65)]
MEDIUM = [(0.0, 0.0), (0.23, 0.005), (0.29, 0.04), (0.3, 0.3), (0.29, 0.44), (0.225, 0.52), (0.11, 0.58),
          (0.075, 0.62), (0.07, 0.8), (0.1, 0.83), (0.1, 0.86), (0.0, 0.865)]


def lathe_obj(name, prof, sides=48, z0=0.0):
    bm = bmesh.new()
    rings = []
    for (r, h) in prof:
        if r <= 1e-6:
            rings.append([bm.verts.new((0.0, 0.0, z0 + h))])
            continue
        rings.append([bm.verts.new((r * math.cos(2 * math.pi * k / sides), r * math.sin(2 * math.pi * k / sides),
                                    z0 + h)) for k in range(sides)])
    for a, b in zip(rings, rings[1:]):
        if len(a) == 1:
            for k in range(sides):
                bm.faces.new((a[0], b[(k + 1) % sides], b[k]))
        elif len(b) == 1:
            for k in range(sides):
                bm.faces.new((a[k], a[(k + 1) % sides], b[0]))
        else:
            for k in range(sides):
                bm.faces.new((a[k], a[(k + 1) % sides], b[(k + 1) % sides], b[k]))
    bmesh.ops.recalc_face_normals(bm, faces=bm.faces)
    me = bpy.data.meshes.new(name)
    bm.to_mesh(me)
    bm.free()
    me.shade_smooth()
    obj = vc.link(bpy.data.objects.new(name, me))
    return obj


def torus_obj(name, R, r, z, sides=32, ring=8):
    bm = bmesh.new()
    vs = []
    for i in range(sides):
        a = 2 * math.pi * i / sides
        row = []
        for j in range(ring):
            b = 2 * math.pi * j / ring
            rr = R + r * math.cos(b)
            row.append(bm.verts.new((rr * math.cos(a), rr * math.sin(a), z + r * math.sin(b))))
        vs.append(row)
    for i in range(sides):
        for j in range(ring):
            bm.faces.new((vs[i][j], vs[(i + 1) % sides][j], vs[(i + 1) % sides][(j + 1) % ring], vs[i][(j + 1) % ring]))
    bmesh.ops.recalc_face_normals(bm, faces=bm.faces)
    me = bpy.data.meshes.new(name)
    bm.to_mesh(me)
    bm.free()
    me.shade_smooth()
    return vc.link(bpy.data.objects.new(name, me))


def glass_material(name, liquid, glow, level):
    """Liquid below `level` (object Z): saturated colour with a faint inner glow and swirls; clear
    pale glass above; a bright rim of the liquid at the level."""
    mat, g, b = gl.new_material(name)
    o = g.coords("Object")
    z = g.xyz(o)[2]
    n = g.noise(o, 6.0, 3.0, 0.5)
    is_liq = g.ramp(z, level + 0.006, level - 0.006)
    depth = g.ramp(z, 0.0, level)
    liq_col = g.mix(g.math("MULTIPLY", depth, 0.22), gl.lin(liquid), gl.lin("#ffffff"))
    liq_col = g.mix(g.ramp(n, 0.3, 0.7, 0.0, 0.35), liq_col, gl.lin("#000000"))
    glass_col = gl.lin("#c9d8dc")
    col = g.mix(is_liq, glass_col, liq_col)
    gl.set_bsdf(g, b, col, g.mix(is_liq, 0.06, 0.12), 0.0)
    b.inputs["Coat Weight"].default_value = 0.6
    b.inputs["Coat Roughness"].default_value = 0.05
    em = g.mix(g.math('MULTIPLY', is_liq, g.ramp(n, 0.2, 0.8, 0.35, 0.8)), gl.lin("#000000"), gl.lin(glow))
    g.put(b.inputs["Emission Color"], em)
    b.inputs["Emission Strength"].default_value = 1.3
    return mat


def plain_material(name, c0, c1, rough=0.8, metal=0.0, scale=14.0):
    mat, g, b = gl.new_material(name)
    o = g.coords("Object")
    n = g.noise(o, scale, 4.0, 0.6)
    gl.set_bsdf(g, b, g.mix(g.ramp(n, 0.3, 0.7), gl.lin(c0), gl.lin(c1)), rough, metal, g.bump(n, 0.4, 0.01))
    return mat


def potion_icon(icon, liquid, glow, size):
    vc.reset_scene()
    parts = []
    prof = SMALL if size == "small" else MEDIUM
    level = 0.33 if size == "small" else 0.5
    body = lathe_obj("bottle", prof)
    body.data.materials.append(glass_material(f"{icon}_glass", liquid, glow, level))
    parts.append(body)
    top = prof[-1][1]
    if size == "small":
        cork = lathe_obj("cork", [(0.0, 0.0), (0.07, 0.0), (0.085, 0.12), (0.08, 0.13), (0.0, 0.135)], 24, z0=top - 0.06)
        cork.data.materials.append(plain_material("cork", "#9a7448", "#6a4c2c"))
        parts.append(cork)
        cord = torus_obj("cord", 0.092, 0.018, 0.5)
        cord.data.materials.append(plain_material("cord", "#c8ac78", "#8a7048", rough=0.9, scale=40.0))
        parts.append(cord)
    else:
        # Wax-sealed stopper in the liquid's colour (dark), a cord round the neck and a paper tag.
        wax_c = "#5a0c0c" if "hp" in icon else "#0c1e5a"
        wax = lathe_obj("wax", [(0.0, 0.0), (0.1, 0.0), (0.125, 0.04), (0.12, 0.1), (0.09, 0.13), (0.0, 0.14)], 24,
                        z0=top - 0.04)
        wax.data.materials.append(plain_material("wax", wax_c, "#2a0606" if "hp" in icon else "#060c2a", rough=0.35,
                                                 scale=8.0))
        parts.append(wax)
        cord = torus_obj("cord", 0.08, 0.02, 0.66)
        cord.data.materials.append(plain_material("cord", "#c8ac78", "#8a7048", rough=0.9, scale=40.0))
        parts.append(cord)
        # The tag: a thin card hanging from the cord at the front-right, a little turned.
        bpy.ops.mesh.primitive_cube_add(size=1)
        tag = bpy.context.active_object
        tag.scale = (0.17, 0.012, 0.21)
        tag.rotation_euler = (math.radians(4), 0, math.radians(-14))
        tag.location = (0.2, -0.17, 0.5)
        bpy.ops.object.transform_apply(location=True, rotation=True, scale=True)
        tag.data.materials.append(plain_material("tag", "#efe2c0", "#cdb98e", rough=0.9, scale=30.0))
        parts.append(tag)
        bpy.ops.mesh.primitive_cylinder_add(radius=0.006, depth=0.14, location=(0.15, -0.12, 0.64),
                                            rotation=(0.0, math.radians(50), math.radians(-30)))
        string = bpy.context.active_object
        string.data.materials.append(plain_material("cord2", "#c8ac78", "#8a7048", rough=0.9))
        parts.append(string)
    bpy.ops.object.select_all(action='DESELECT')
    for p in parts:
        p.select_set(True)
    bpy.context.view_layer.objects.active = body
    bpy.ops.object.join()
    obj = body
    # The same frame for every potion: the medium bottle's bounds (a hidden reference box).
    ref = lathe_obj("ref", [(0.0, 0.0), (0.31, 0.0), (0.31, 0.97), (0.0, 0.97)], 8)
    ref.hide_render = True
    disp = gl.display(20.0, 14.0, -12.0)
    big = gw.ICON_SIZE * gw.ICON_SUPERSAMPLE
    cam = gw.setup_cycles((big, big), 48, True, world="#9a9ca0", world_strength=0.7)
    obj.matrix_world = disp
    ref.matrix_world = disp
    bpy.context.view_layer.update()
    gw.fit_camera(cam, [ref], 0.02)
    # Keep the bottle's base at the bottom of the frame: shift the camera so the shorter flask sits
    # on the same baseline (it reads as "smaller", not "floating").
    rgba = gw.render_rgba(os.path.join(ICON_DIR, f"_{icon}.render.png"))
    out = gw.add_shadow(gw.box_down_premul(rgba, gw.ICON_SUPERSAMPLE))
    path = os.path.join(ICON_DIR, f"{icon}.png")
    gw.write_png_stable_rgba(path, vc.to_u8(out))
    print(f"ICON {icon}: {out.shape[1]}x{out.shape[0]}, coverage {float(out[..., 3].mean()):.2f}")
    return out


# ---------------------------------------------------------------------------------------------
# Shop UI art (SDF painter)

OUT = 0.045
GOLD = ("#fff0b8", "#c98a2a")
BRONZE = ("#ffd890", "#9a5a1c")
IVORY = ("#fbf3dc", "#c9b88e")
LEATHER = ("#b8834a", "#5a3416")
STEEL = ("#f4f6fa", "#8d97a6")


def shade_y(cv, y0, y1, c):
    return lerp_field((cv.y - y0) / max(y1 - y0, 1e-6), *c)


def ink(cv, d, w=OUT):
    cv.over(cv.cover(d - w), hexc(INK))


def paint(cv, d, shade):
    cv.over(cv.cover(d), shade)


def coin(cv, cx, cy, r, w=OUT):
    """An Akçe: a gold disc with a raised rim and a stamped square hole edged by a diamond (reads
    as a coin down to 16 px)."""
    x, y = cv.x, cv.y
    disc = sd_circle(x, y, cx, cy, r)
    ink(cv, disc, w)
    paint(cv, disc, lerp_field(np.hypot(x - cx + r * 0.35, y - cy + r * 0.35) / (r * 1.6), *BRONZE))
    rim = np.abs(sd_circle(x, y, cx, cy, r * 0.8)) - r * 0.05
    cv.over(cv.cover(rim) * 0.8, hexc("#7a4612"))
    diamond = sd_polygon(x, y, [(cx, cy - r * 0.5), (cx + r * 0.5, cy), (cx, cy + r * 0.5), (cx - r * 0.5, cy)])
    cv.over(cv.cover(np.abs(diamond) - r * 0.05) * 0.85, hexc("#7a4612"))
    hole = sd_round_rect(x, y, cx, cy, r * 0.17, r * 0.17, r * 0.03)
    cv.over(cv.cover(hole + r * 0.03), hexc("#7a4612"))
    cv.over(cv.cover(hole), hexc("#2a1404"))
    hl = sd_ellipse(x, y, cx - r * 0.45, cy - r * 0.5, r * 0.18, r * 0.1)
    cv.over(cv.cover(hl) * 0.6, hexc("#ffffff"))


def glyph_akce(cv):
    coin(cv, 0.5, 0.5, 0.42)


def _palm(cv):
    """An open right palm seen from the front, fingers together, a blue sleeve below."""
    x, y = cv.x, cv.y
    palm = union(sd_ellipse(x, y, 0.42, 0.66, 0.24, 0.13),
                 sd_stroke(x, y, [(0.18, 0.62), (0.13, 0.52)], [0.05, 0.045]),   # thumb
                 sd_round_rect(x, y, 0.42, 0.8, 0.13, 0.1, 0.05))                 # wrist / heel
    fingers = union(*[sd_stroke(x, y, [(0.3 + 0.085 * k, 0.6), (0.33 + 0.09 * k, 0.55 - 0.012 * k)], [0.04, 0.036])
                      for k in range(4)])
    sleeve = sd_round_rect(x, y, 0.42, 0.93, 0.17, 0.07, 0.03)
    ink(cv, union(palm, fingers, sleeve))
    paint(cv, sleeve, shade_y(cv, 0.86, 1.0, ("#6a8ab8", "#2a3a5a")))
    paint(cv, union(palm, fingers), shade_y(cv, 0.45, 0.9, ("#f2d0a8", "#a8704a")))


def _arrow(cv, pts, w, head, color):
    """A thick arrow along pts with a triangular head at the last point."""
    x, y = cv.x, cv.y
    shaft = sd_stroke(x, y, pts, [w] * len(pts))
    (ax, ay), (bx, by) = pts[-2], pts[-1]
    d = np.array([bx - ax, by - ay])
    d /= np.linalg.norm(d)
    nx, ny = -d[1], d[0]
    tip = (bx + d[0] * head, by + d[1] * head)
    tri = sd_polygon(x, y, [tip, (bx + nx * head * 0.95, by + ny * head * 0.95), (bx - nx * head * 0.95, by - ny * head * 0.95)])
    a = union(shaft, tri)
    ink(cv, a, 0.035)
    paint(cv, a, shade_y(cv, 0.05, 0.6, color))


def glyph_tab_al(cv):
    """Al (buy): the goods come to you - a red potion flask dropping into an open palm, a green
    arrow down."""
    x, y = cv.x, cv.y
    _palm(cv)
    body = sd_circle(x, y, 0.42, 0.4, 0.14)
    neck = sd_round_rect(x, y, 0.42, 0.22, 0.045, 0.07, 0.015)
    lip = sd_round_rect(x, y, 0.42, 0.15, 0.065, 0.022, 0.012)
    bottle = union(body, neck, lip)
    ink(cv, bottle, 0.035)
    paint(cv, bottle, hexc("#dfeaec"))
    liquid = np.maximum(sd_circle(x, y, 0.42, 0.4, 0.115), -(y - 0.37))
    paint(cv, liquid, lerp_field(np.hypot(x - 0.39, y - 0.44) / 0.15, "#ff6a5a", "#a00c10"))
    hl = sd_ellipse(x, y, 0.37, 0.36, 0.022, 0.04)
    cv.over(cv.cover(hl) * 0.85, hexc("#ffffff"))
    _arrow(cv, [(0.84, 0.1), (0.84, 0.4)], 0.035, 0.09, ("#c8ffd8", "#2a9a52"))


def glyph_tab_sat(cv):
    """Sat (sell): the coin comes to you - an open palm holding out a coin, an amber arrow up."""
    _palm(cv)
    coin(cv, 0.42, 0.4, 0.17, 0.03)
    _arrow(cv, [(0.84, 0.46), (0.84, 0.16)], 0.035, 0.09, ("#ffe6a8", "#d0781a"))


def _plaque(cv, ring_col):
    """Round plaque with a gold rim and a short pin below (an overhead marker)."""
    x, y = cv.x, cv.y
    pin = sd_polygon(x, y, [(0.42, 0.78), (0.58, 0.78), (0.5, 0.96)])
    disc = sd_circle(x, y, 0.5, 0.44, 0.38)
    shape = union(disc, pin)
    ink(cv, shape, 0.03)
    paint(cv, shape, shade_y(cv, 0.05, 0.95, GOLD))
    inner = sd_circle(x, y, 0.5, 0.44, 0.31)
    cv.over(cv.cover(inner + 0.012), hexc("#3a2408"))
    paint(cv, inner, lerp_field(np.hypot(x - 0.45, y - 0.36) / 0.38, *ring_col))


def glyph_marker_potion(cv):
    _plaque(cv, ("#3a4a5e", "#151a24"))
    x, y = cv.x, cv.y
    body = sd_circle(x, y, 0.5, 0.52, 0.17)
    neck = sd_round_rect(x, y, 0.5, 0.3, 0.055, 0.08, 0.02)
    lip = sd_round_rect(x, y, 0.5, 0.215, 0.08, 0.025, 0.015)
    bottle = union(body, neck, lip)
    ink(cv, bottle, 0.028)
    paint(cv, bottle, hexc("#dfeaec"))
    liquid = np.maximum(sd_circle(x, y, 0.5, 0.52, 0.145), -(y - 0.5))
    paint(cv, liquid, lerp_field(np.hypot(x - 0.46, y - 0.56) / 0.17, "#ff6a5a", "#a00c10"))
    cork = sd_round_rect(x, y, 0.5, 0.18, 0.045, 0.035, 0.012)
    ink(cv, cork, 0.02)
    paint(cv, cork, shade_y(cv, 0.14, 0.22, ("#c89a60", "#6a4420")))
    hl = sd_ellipse(x, y, 0.44, 0.47, 0.03, 0.05)
    cv.over(cv.cover(hl) * 0.85, hexc("#ffffff"))
    for (bx, by, r) in ((0.55, 0.6, 0.018), (0.48, 0.64, 0.012)):
        cv.over(cv.cover(sd_circle(x, y, bx, by, r)) * 0.6, hexc("#ffd0c0"))


def glyph_marker_smith(cv):
    _plaque(cv, ("#3a4a5e", "#151a24"))
    x, y = cv.x, cv.y
    # Anvil.
    top = sd_round_rect(x, y, 0.49, 0.56, 0.17, 0.035, 0.01)
    horn = sd_polygon(x, y, [(0.66, 0.53), (0.78, 0.565), (0.66, 0.595)])
    waist = sd_polygon(x, y, [(0.42, 0.595), (0.56, 0.595), (0.53, 0.66), (0.45, 0.66)])
    foot = sd_round_rect(x, y, 0.49, 0.68, 0.12, 0.025, 0.01)
    anvil = union(top, horn, waist, foot)
    ink(cv, anvil, 0.028)
    paint(cv, anvil, shade_y(cv, 0.5, 0.72, ("#c8ced8", "#4a525e")))
    # Hammer above, tilted.
    rx, ry = rot(x, y, 0.47, 0.34, -32)
    head = sd_round_rect(rx, ry, 0.47, 0.25, 0.12, 0.05, 0.015)
    handle = sd_round_rect(rx, ry, 0.47, 0.39, 0.025, 0.12, 0.012)
    ink(cv, union(head, handle), 0.028)
    paint(cv, handle, shade_y(cv, 0.25, 0.55, ("#d8a868", "#6a4420")))
    paint(cv, head, shade_y(cv, 0.15, 0.4, STEEL))
    # Sparks.
    for (sx, sy, r) in ((0.64, 0.44, 0.02), (0.69, 0.4, 0.014), (0.6, 0.39, 0.012)):
        cv.over(cv.cover(sd_circle(x, y, sx, sy, r)), hexc("#ffc040"))


UI = {
    "akce": (glyph_akce, 64), "tab_al": (glyph_tab_al, 64), "tab_sat": (glyph_tab_sat, 64),
    "marker_potion": (glyph_marker_potion, 128), "marker_smith": (glyph_marker_smith, 128),
}


def make_ui():
    vc.ensure_dir(SHOP_DIR)
    out = {}
    for name, (fn, size) in UI.items():
        cv = Canvas(size)
        fn(cv)
        rgba = cv.result()
        vc.write_png(os.path.join(SHOP_DIR, f"{name}.png"), vc.to_u8(rgba))
        out[name] = rgba
        print(f"ICON shop {name}: {size}x{size}")
    return out


def sheet(items, ui, path):
    """Item icons at 128 (raw), 64, 48 and 72 (48 at 150 %) in the slot art; the shop glyphs at 64,
    40, 32, 24, 16 (the coin) and in the slot art; the markers at 128 / 64 / 40 on a sky-ish and a
    dark background."""
    bar_dir = os.path.join(vc.SRC, "ui", "skillbar")
    back = sa.load_png(os.path.join(bar_dir, "slot_back.png"))
    frame = sa.load_png(os.path.join(bar_dir, "slot_frame_normal.png"))
    pad = 14
    W, H = 900, 640
    sh = sa.Sheet(W, H, bg=(26, 31, 38))
    names = list(items)
    x0, y = pad, pad
    for k, n in enumerate(names):
        sh.place(items[n], x0 + k * (128 + pad), y)
    y += 128 + pad
    for size in (72, 64, 48, 40):
        win = int(round(size * 52 / 64))
        off = (size - win) // 2
        for k, n in enumerate(names):
            xx = x0 + k * (128 + pad)
            sh.place(back, xx, y, size)
            sh.place(items[n], xx + off, y + off, win)
            sh.place(frame, xx, y, size)
        y += size + pad
    # Shop glyphs.
    gx = x0 + 4 * (128 + pad) + pad
    yy = pad
    for n in ("akce", "tab_al", "tab_sat"):
        xx = gx
        for size in (64, 40, 32, 24, 16):
            sh.place(ui[n], xx, yy, size)
            xx += size + 8
        win = 40
        sh.place(back, xx, yy, 48)
        sh.place(ui[n], xx + 4, yy + 4, win)
        sh.place(frame, xx, yy, 48)
        yy += 64 + pad
    # Markers on two backgrounds.
    for bg_k, bg in enumerate(((108, 132, 160), (40, 44, 52))):
        bx = gx + bg_k * 140
        sh.rect(bx, yy, 132, 250, bg)
        sh.place(ui["marker_potion"], bx + 2, yy + 2, 128)
        sh.place(ui["marker_smith"], bx + 2, yy + 132, 64)
        sh.place(ui["marker_potion"], bx + 70, yy + 140, 40)
        sh.place(ui["marker_smith"], bx + 70, yy + 190, 40)
    sh.save(path)


def main():
    args = vc.script_args()
    only = set(args[args.index("--only") + 1].split(",")) if "--only" in args else {"items", "ui"}
    sheets = args[args.index("--sheets") + 1] if "--sheets" in args else DEFAULT_SHEETS
    items = {}
    if "items" in only:
        for icon, (liquid, glow, size) in POTIONS.items():
            items[icon] = potion_icon(icon, liquid, glow, size)
    ui = make_ui() if "ui" in only else {}
    if "--no-preview" in args or not items or not ui:
        return
    vc.ensure_dir(sheets)
    sheet(items, ui, os.path.join(sheets, "shop_icons.png"))


if __name__ == "__main__":
    main()
