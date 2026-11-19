"""M16 "Tav" upgrade stone icons (the inventory style of gen_loot.py's material icons: a procedural mesh
rendered with Cycles at 512^2, box-filtered 4x on premultiplied linear colour, soft dark halo, stable writer).

Three grades, three silhouettes and colours, so they read apart at bag size (docs/PROTOCOL_UPGRADE.md §2.1):
  assets_src/ui/items/temper_stone_raw.png     Ham Tav Taşı: a rough, chipped grey-brown whetstone bar
  assets_src/ui/items/temper_stone_pure.png    Saf Tav Taşı: a pale, six-sided honed stone with cold blue veins
  assets_src/ui/items/temper_stone_master.png  Usta Tav Taşı: a dark rounded stone, cracked open on an ember core

Usage (headless only; never the live Blender session):
  blender --background --factory-startup --python tools/blender/gen_upgrade_icons.py
Deterministic.
"""

import math
import os
import sys

import bpy

sys.dont_write_bytecode = True
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import vo_common as vc  # noqa: E402
import gen_loot as gl  # noqa: E402


def bevel(obj, width, segments=3):
    m = obj.modifiers.new("b", "BEVEL")
    m.width = width
    m.segments = segments
    m.limit_method = 'ANGLE'
    return obj


def displace(obj, scale, strength):
    tex = bpy.data.textures.new(f"{obj.name}_n", type='CLOUDS')
    tex.noise_scale = scale
    tex.noise_depth = 2
    m = obj.modifiers.new("d", "DISPLACE")
    m.texture = tex
    m.strength = strength
    m.mid_level = 0.5
    m.texture_coords = 'OBJECT'
    return obj


def apply_all(obj):
    bpy.context.view_layer.objects.active = obj
    obj.select_set(True)
    for m in list(obj.modifiers):
        bpy.ops.object.modifier_apply(modifier=m.name)


def stone_raw():
    """A whetstone bar with chipped corners: dull, grainy, a pale worn stripe where blades were drawn."""
    vc.reset_scene()
    bpy.ops.mesh.primitive_cube_add(size=1.0)
    obj = bpy.context.active_object
    obj.name = "temper_stone_raw"
    obj.scale = (1.0, 0.36, 0.26)
    bpy.ops.object.transform_apply(scale=True)
    m = obj.modifiers.new("s", "SUBSURF")
    m.levels = m.render_levels = 3
    bevel(obj, 0.05)
    displace(obj, 0.35, 0.06)
    apply_all(obj)
    bpy.ops.object.shade_smooth()
    mat, g, b = gl.new_material("raw_stone")
    o = g.coords("Object")
    n = g.noise(o, 9.0, 6.0, 0.65)
    grain = g.noise(o, 45.0, 2.0, 0.5)
    col = g.mix(g.ramp(n, 0.3, 0.7), gl.lin("#5a5048"), gl.lin("#8a7e70"))
    col = g.mix(g.ramp(grain, 0.55, 0.75), col, gl.lin("#3a332d"))
    # The worn stripe along the top.
    zz = g.xyz(o)[2]
    stripe = g.ramp(zz, 0.08, 0.13)
    col = g.mix(g.math('MULTIPLY', stripe, 0.55), col, gl.lin("#c9bba4"))
    gl.set_bsdf(g, b, col, 0.92, 0.0, g.bump(g.math('ADD', n, g.math('MULTIPLY', grain, 0.3)), 0.6, 0.03))
    obj.data.materials.append(mat)
    return gl.render_icon_obj(obj, "temper_stone_raw", gl.display(-28.0, 32.0, 18.0))


def stone_pure():
    """A six-sided honed stone, pale and smooth, cold blue veins glowing faintly."""
    vc.reset_scene()
    bpy.ops.mesh.primitive_cylinder_add(vertices=6, radius=0.5, depth=1.0)
    obj = bpy.context.active_object
    obj.name = "temper_stone_pure"
    obj.scale = (0.62, 0.62, 1.0)
    bpy.ops.object.transform_apply(scale=True)
    # Taper the ends a little: a honed crystal-like stone.
    for v in obj.data.vertices:
        if abs(v.co.z) > 0.4:
            v.co.x *= 0.8
            v.co.y *= 0.8
    bevel(obj, 0.06, 4)
    apply_all(obj)
    mat, g, b = gl.new_material("pure_stone")
    o = g.coords("Object")
    n = g.noise(o, 5.0, 5.0, 0.55)
    vo = g.voronoi(g.warp(o, 2.0, 0.6), 3.2, 'DISTANCE_TO_EDGE')
    vein = g.ramp(vo["Distance"], 0.035, 0.006)
    col = g.mix(g.ramp(n, 0.35, 0.7), gl.lin("#c7ccd3"), gl.lin("#eef1f4"))
    col = g.mix(vein, col, gl.lin("#5fb8ff"))
    emit = g.mix(vein, (0.0, 0.0, 0.0), (0.18, 0.55, 1.0))
    gl.set_bsdf(g, b, col, 0.32, 0.0, g.bump(n, 0.25, 0.02), emit, 1.6)
    b.inputs["Coat Weight"].default_value = 0.4
    obj.data.materials.append(mat)
    return gl.render_icon_obj(obj, "temper_stone_pure", gl.display(18.0, 38.0, -24.0))


def stone_master():
    """A heavy dark stone, rounded, split by glowing amber cracks round an ember core."""
    vc.reset_scene()
    obj = gl.convex_rock("temper_stone_master", 30, (0.62, 0.5, 0.48), 21, 0.14)
    m = obj.modifiers.new("s", "SUBSURF")
    m.levels = m.render_levels = 2
    apply_all(obj)
    bpy.ops.object.shade_smooth()
    mat, g, b = gl.new_material("master_stone")
    o = g.coords("Object")
    n = g.noise(o, 6.0, 6.0, 0.6)
    vo = g.voronoi(g.warp(o, 2.2, 0.55), 2.4, 'DISTANCE_TO_EDGE')
    crack = g.ramp(vo["Distance"], 0.05, 0.01)
    crack = g.math('MULTIPLY', crack, g.ramp(g.noise(o, 1.6, 2.0), 0.35, 0.5))
    col = g.mix(g.ramp(n, 0.3, 0.7), gl.lin("#15110f"), gl.lin("#3b302a"))
    col = g.mix(crack, col, gl.lin("#ffb340"))
    emit = g.mix(crack, (0.0, 0.0, 0.0), (1.0, 0.52, 0.10))
    gl.set_bsdf(g, b, col, g.ramp(n, 0.3, 0.7, 0.45, 0.75), 0.15, g.bump(g.math('ADD', n, g.math('MULTIPLY', crack, -0.6)), 0.5, 0.04),
                emit, 2.4)
    obj.data.materials.append(mat)
    return gl.render_icon_obj(obj, "temper_stone_master", gl.display(-12.0, 24.0, 30.0))


def main():
    for f in (stone_raw, stone_pure, stone_master):
        f()


if __name__ == "__main__":
    main()
