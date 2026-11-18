"""M15 armour item icons (the inventory style of gen_shop_icons.py / gen_weapons.py: a procedural mesh
rendered with Cycles at 512^2, box-filtered 4x on premultiplied linear colour, soft dark halo, stable writer).

The five armour slots get one neutral, light steel-and-leather icon each; gamedata/items.toml tints them
per armour line and tier (`tint`, docs/PROTOCOL_LEVELS.md §4), so one light base reads as iron, leather
or cloth until real per-item art exists:
  assets_src/ui/items/armor_head.png    open helm with a nasal and a rim
  assets_src/ui/items/armor_chest.png   cuirass: chest plate with a neck opening and shoulder rolls
  assets_src/ui/items/armor_legs.png    greaves: two tapered leg plates with knee cops
  assets_src/ui/items/armor_hands.png   gauntlet: cuff, back plate, four fingers and a thumb
  assets_src/ui/items/armor_feet.png    sabaton: a boot with a toe cap

Usage (headless):
  blender --background --factory-startup --python tools/blender/gen_armor_icons.py
Deterministic.
"""

import math
import os
import sys

import bpy

sys.dont_write_bytecode = True
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import vo_common as vc  # noqa: E402
import gen_weapons as gw  # noqa: E402
import gen_loot as gl  # noqa: E402

ICON_DIR = os.path.join(vc.SRC, "ui", "items")


def material(name, c0, c1, rough, metal, scale=12.0):
    mat, g, b = gl.new_material(name)
    o = g.coords("Object")
    n = g.noise(o, scale, 4.0, 0.6)
    gl.set_bsdf(g, b, g.mix(g.ramp(n, 0.3, 0.7), gl.lin(c0), gl.lin(c1)), rough, metal, g.bump(n, 0.3, 0.01))
    return mat


STEEL = ("#e9edf2", "#b9c0ca")
STRAP = ("#d8cbb4", "#a8977a")


def add(kind, mat, smooth=True, **kw):
    getattr(bpy.ops.mesh, f"primitive_{kind}_add")(**kw)
    o = bpy.context.active_object
    o.data.materials.append(mat)
    if smooth:
        bpy.ops.object.shade_smooth()
    return o


def subsurf(o, levels=2):
    m = o.modifiers.new("s", "SUBSURF")
    m.levels = m.render_levels = levels
    return o


def head(steel, strap):
    parts = []
    dome = add("uv_sphere", steel, radius=0.5, segments=48, ring_count=24, location=(0, 0, 0))
    dome.scale = (1.0, 1.08, 1.12)
    for v in dome.data.vertices:  # cut the lower part off: an open helm
        if v.co.z < -0.12:
            v.co.z = -0.12 + (v.co.z + 0.12) * 0.15
    parts.append(dome)
    rim = add("torus", strap, major_radius=0.5, minor_radius=0.045, location=(0, 0, -0.13))
    rim.scale = (1.0, 1.08, 1.0)
    parts.append(rim)
    nasal = add("cube", steel, smooth=False, size=1, location=(0, -0.55, -0.12))
    nasal.scale = (0.07, 0.04, 0.42)
    parts.append(nasal)
    crest = add("cube", strap, smooth=False, size=1, location=(0, 0, 0.5))
    crest.scale = (0.06, 0.8, 0.12)
    subsurf(crest, 1)
    parts.append(crest)
    return parts


def chest(steel, strap):
    parts = []
    body = add("cylinder", steel, vertices=48, radius=0.5, depth=1.1, location=(0, 0, 0))
    body.scale = (1.0, 0.55, 1.0)
    for v in body.data.vertices:  # waist narrower than the chest, the front bulging
        if v.co.z < 0:
            v.co.x *= 0.82
        if v.co.y < 0:
            v.co.y *= 1.0 + 0.25 * (1.0 - abs(v.co.z) / 0.55)
    subsurf(body)
    parts.append(body)
    neck = add("torus", strap, major_radius=0.2, minor_radius=0.05, location=(0, -0.02, 0.55))
    neck.scale = (1.0, 0.7, 1.0)
    parts.append(neck)
    for side in (-1, 1):
        sh = add("uv_sphere", steel, radius=0.22, location=(side * 0.48, 0, 0.42))
        sh.scale = (1.0, 0.9, 0.7)
        parts.append(sh)
    belt = add("torus", strap, major_radius=0.43, minor_radius=0.05, location=(0, -0.02, -0.42))
    belt.scale = (1.0, 0.62, 1.0)
    parts.append(belt)
    return parts


def legs(steel, strap):
    parts = []
    for side in (-1, 1):
        leg = add("cone", steel, vertices=40, radius1=0.15, radius2=0.22, depth=1.3, location=(side * 0.22, 0, 0))
        leg.scale = (1.0, 0.8, 1.0)
        parts.append(leg)
        knee = add("uv_sphere", steel, radius=0.13, location=(side * 0.22, -0.13, 0.05))
        parts.append(knee)
        strap1 = add("torus", strap, major_radius=0.2, minor_radius=0.03, location=(side * 0.22, 0, 0.45))
        strap1.scale = (1.0, 0.8, 1.0)
        parts.append(strap1)
    belt = add("cube", strap, smooth=False, size=1, location=(0, 0, 0.66))
    belt.scale = (0.85, 0.3, 0.12)
    subsurf(belt, 1)
    parts.append(belt)
    return parts


def hands(steel, strap):
    parts = []
    cuff = add("cone", steel, vertices=40, radius1=0.3, radius2=0.22, depth=0.45, location=(0, 0, -0.45))
    cuff.scale = (1.0, 0.6, 1.0)
    parts.append(cuff)
    back = add("cube", steel, smooth=False, size=1, location=(0, 0, 0.0))
    back.scale = (0.42, 0.18, 0.5)
    subsurf(back, 2)
    parts.append(back)
    for k in range(4):
        f = add("cylinder", steel, vertices=20, radius=0.055, depth=0.42, location=(-0.15 + 0.1 * k, 0, 0.45))
        parts.append(f)
    thumb = add("cylinder", steel, vertices=20, radius=0.06, depth=0.32, location=(-0.3, -0.02, 0.1),
                rotation=(0, math.radians(-50), 0))
    parts.append(thumb)
    band = add("torus", strap, major_radius=0.24, minor_radius=0.035, location=(0, 0, -0.22))
    band.scale = (1.0, 0.6, 1.0)
    parts.append(band)
    return parts


def feet(steel, strap):
    parts = []
    shaft = add("cylinder", steel, vertices=40, radius=0.2, depth=0.75, location=(0, 0.1, 0.15))
    shaft.scale = (1.0, 0.9, 1.0)
    parts.append(shaft)
    foot = add("cube", steel, smooth=False, size=1, location=(0, -0.18, -0.32))
    foot.scale = (0.38, 0.75, 0.22)
    subsurf(foot, 2)
    parts.append(foot)
    toe = add("uv_sphere", steel, radius=0.2, location=(0, -0.48, -0.3))
    toe.scale = (0.95, 1.1, 0.7)
    parts.append(toe)
    cuff = add("torus", strap, major_radius=0.21, minor_radius=0.045, location=(0, 0.1, 0.52))
    parts.append(cuff)
    sole = add("cube", strap, smooth=False, size=1, location=(0, -0.2, -0.45))
    sole.scale = (0.4, 0.95, 0.06)
    parts.append(sole)
    return parts


ICONS = {"armor_head": (head, (18.0, 10.0, -20.0)), "armor_chest": (chest, (12.0, 8.0, -18.0)),
         "armor_legs": (legs, (10.0, 6.0, -16.0)), "armor_hands": (hands, (14.0, 20.0, -24.0)),
         "armor_feet": (feet, (14.0, 30.0, -30.0))}


def render(icon):
    vc.reset_scene()
    builder, (rx, ry, rz) = ICONS[icon]
    steel = material(f"{icon}_steel", STEEL[0], STEEL[1], 0.35, 0.85)
    strap = material(f"{icon}_strap", STRAP[0], STRAP[1], 0.8, 0.0, 30.0)
    parts = builder(steel, strap)
    bpy.ops.object.select_all(action='DESELECT')
    for p in parts:
        p.select_set(True)
    bpy.context.view_layer.objects.active = parts[0]
    bpy.ops.object.convert(target='MESH')
    bpy.ops.object.join()
    obj = bpy.context.active_object
    obj.matrix_world = gl.display(rx, ry, rz)
    big = gw.ICON_SIZE * gw.ICON_SUPERSAMPLE
    cam = gw.setup_cycles((big, big), 48, True, world="#9a9ca0", world_strength=0.8)
    bpy.context.view_layer.update()
    gw.fit_camera(cam, [obj], 0.08)
    rgba = gw.render_rgba(os.path.join(ICON_DIR, f"_{icon}.render.png"))
    out = gw.add_shadow(gw.box_down_premul(rgba, gw.ICON_SUPERSAMPLE))
    path = os.path.join(ICON_DIR, f"{icon}.png")
    gw.write_png_stable_rgba(path, vc.to_u8(out))
    print(f"ICON {icon}: {out.shape[1]}x{out.shape[0]}, coverage {float(out[..., 3].mean()):.2f}")


def main():
    for icon in ICONS:
        render(icon)


if __name__ == "__main__":
    main()
