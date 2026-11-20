"""M18 "Kuşanma" accessory icons (rings, earrings, necklaces, belts) in the inventory style of the other item
icons (gen_loot.render_icon_obj: a procedural mesh rendered with Cycles at 512^2, box-filtered 4x on
premultiplied linear colour, soft dark halo, stable writer).

The base materials are light and fairly neutral (pale steel, pale gold, light leather, a pale gem): the game
multiplies every icon by the item's `tint`, so one icon serves several tiers.

  assets_src/ui/items/acc_ring_band.png        a plain wide metal band with two engraved grooves
  assets_src/ui/items/acc_ring_signet.png      a ring with a raised square bezel holding a cut stone
  assets_src/ui/items/acc_earring_hoop.png     a small hoop with an ear wire and a bead
  assets_src/ui/items/acc_earring_drop.png     an ear loop, a short link and a hanging tear-drop gem
  assets_src/ui/items/acc_necklace_chain.png   a chain of small links with a round pendant and stone
  assets_src/ui/items/acc_necklace_torc.png    a rigid, twisted open collar with knobbed ends
  assets_src/ui/items/acc_belt_leather.png     a loosely closed leather strap with a buckle and rivets
  assets_src/ui/items/acc_belt_plated.png      a strap covered with overlapping metal plates and a clasp

Everything is procedural (Blender primitives, curves and procedural shaders); no downloads.

Usage (headless only; never the live Blender session):
  blender --background --factory-startup --python tools/blender/gen_accessory_icons.py -- [--only ring_band,...]
Deterministic (an existing icon within 2/255 is kept by the stable writer).
"""

import math
import os
import sys

import bpy
from mathutils import Matrix, Vector

sys.dont_write_bytecode = True
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import vo_common as vc  # noqa: E402
import gen_loot as gl  # noqa: E402


# ---------------------------------------------------------------------------------------------
# Materials

def metal(name, c0, c1, rough=0.28):
    """Polished metal with soft tarnish (two colours mixed by noise) and a fine brushed bump."""
    mat, g, b = gl.new_material(name)
    o = g.coords("Object")
    n = g.noise(o, 6.0, 4.0, 0.55)
    col = g.mix(g.ramp(n, 0.3, 0.7), gl.lin(c0), gl.lin(c1))
    gl.set_bsdf(g, b, col, g.ramp(n, 0.2, 0.8, rough - 0.08, rough + 0.1), 1.0,
                g.bump(g.noise(o, 60.0, 2.0, 0.5), 0.12, 0.01))
    return mat


def gem(name, colour, glow):
    mat, g, b = gl.new_material(name)
    o = g.coords("Object")
    n = g.noise(o, 4.0, 3.0, 0.5)
    col = g.mix(g.ramp(n, 0.3, 0.7), gl.lin(colour), (1.0, 1.0, 1.0))
    gl.set_bsdf(g, b, col, 0.08, 0.0, None, gl.lin(glow), 0.35)
    b.inputs["Coat Weight"].default_value = 1.0
    b.inputs["Specular IOR Level"].default_value = 0.8
    return mat


def leather(name):
    mat, g, b = gl.new_material(name)
    o = g.coords("Object")
    n = g.noise(o, 5.0, 5.0, 0.6)
    grain = g.noise(o, 70.0, 2.0, 0.5)
    col = g.mix(g.ramp(n, 0.25, 0.75), gl.lin("#a0784e"), gl.lin("#d2ad80"))
    col = g.mix(g.ramp(grain, 0.6, 0.8, 0.0, 0.35), col, gl.lin("#5a3e26"))
    gl.set_bsdf(g, b, col, 0.7, 0.0, g.bump(g.math('ADD', grain, g.math('MULTIPLY', n, 0.5)), 0.35, 0.02))
    return mat


def steel():
    return metal("pale_steel", "#aeb4bc", "#e6e9ee")


def gold():
    return metal("pale_gold", "#dcc28a", "#fbeccb", 0.24)


# ---------------------------------------------------------------------------------------------
# Geometry helpers (everything ends up joined in one mesh object per icon)

def finish(obj, mat, smooth=True):
    obj.data.materials.clear()
    obj.data.materials.append(mat)
    if smooth and obj.type == 'MESH':
        for p in obj.data.polygons:
            p.use_smooth = True
    return obj


def torus(R, r, mat, loc=(0, 0, 0), rot=(0, 0, 0), scale=(1, 1, 1), major=48, minor=16):
    bpy.ops.mesh.primitive_torus_add(major_radius=R, minor_radius=r, major_segments=major, minor_segments=minor,
                                     location=loc, rotation=rot)
    o = bpy.context.active_object
    o.scale = scale
    bpy.ops.object.transform_apply(location=False, rotation=False, scale=True)
    return finish(o, mat)


def sphere(r, mat, loc=(0, 0, 0), scale=(1, 1, 1), rot=(0, 0, 0), seg=24, rings=12):
    bpy.ops.mesh.primitive_uv_sphere_add(radius=r, segments=seg, ring_count=rings, location=loc, rotation=rot)
    o = bpy.context.active_object
    o.scale = scale
    bpy.ops.object.transform_apply(location=False, rotation=False, scale=True)
    return finish(o, mat)


def box(size, mat, loc=(0, 0, 0), rot=(0, 0, 0), bevel=0.0):
    bpy.ops.mesh.primitive_cube_add(size=1.0, location=loc, rotation=rot)
    o = bpy.context.active_object
    o.scale = size
    bpy.ops.object.transform_apply(location=False, rotation=False, scale=True)
    if bevel > 0.0:
        m = o.modifiers.new("b", "BEVEL")
        m.width = bevel
        m.segments = 3
        apply_mods(o)
    return finish(o, mat, smooth=bevel > 0.0)


def cylinder(r, depth, mat, loc=(0, 0, 0), rot=(0, 0, 0), verts=32, bevel=0.0):
    bpy.ops.mesh.primitive_cylinder_add(vertices=verts, radius=r, depth=depth, location=loc, rotation=rot)
    o = bpy.context.active_object
    if bevel > 0.0:
        m = o.modifiers.new("b", "BEVEL")
        m.width = bevel
        m.segments = 3
        m.limit_method = 'ANGLE'
        apply_mods(o)
    return finish(o, mat)


def apply_mods(o):
    bpy.ops.object.select_all(action='DESELECT')
    bpy.context.view_layer.objects.active = o
    o.select_set(True)
    for m in list(o.modifiers):
        bpy.ops.object.modifier_apply(modifier=m.name)


def curve_tube(points, mat, bevel_depth, extrude=0.0, closed=False, twist_bevel=None, resolution=24):
    """A poly/NURBS path through `points` with a round (or flat when extrude > 0) cross-section, as a mesh."""
    cd = bpy.data.curves.new("c", 'CURVE')
    cd.dimensions = '3D'
    cd.resolution_u = resolution
    cd.bevel_depth = bevel_depth
    cd.bevel_resolution = 6
    cd.extrude = extrude
    cd.use_fill_caps = True
    cd.twist_mode = 'Z_UP'
    sp = cd.splines.new('NURBS')
    sp.points.add(len(points) - 1)
    for p, q in zip(sp.points, points):
        p.co = (q[0], q[1], q[2], 1.0)
    sp.order_u = 4
    sp.use_endpoint_u = not closed
    sp.use_cyclic_u = closed
    o = bpy.data.objects.new("c", cd)
    bpy.context.collection.objects.link(o)
    bpy.ops.object.select_all(action='DESELECT')
    bpy.context.view_layer.objects.active = o
    o.select_set(True)
    bpy.ops.object.convert(target='MESH')
    o = bpy.context.active_object
    return finish(o, mat)


def join(objs, name):
    bpy.ops.object.select_all(action='DESELECT')
    for o in objs:
        o.select_set(True)
    bpy.context.view_layer.objects.active = objs[0]
    bpy.ops.object.transform_apply(location=True, rotation=True, scale=True)
    bpy.ops.object.join()
    o = bpy.context.active_object
    o.name = name
    return o


def arc(R, a0, a1, n, z=0.0, zfn=None, ry=1.0):
    out = []
    for i in range(n):
        t = i / (n - 1)
        a = math.radians(a0 + (a1 - a0) * t)
        out.append((R * math.cos(a), R * ry * math.sin(a), z if zfn is None else zfn(t)))
    return out


# ---------------------------------------------------------------------------------------------
# Icons

def ring_band():
    vc.reset_scene()
    m = gold()
    band = torus(0.5, 0.075, m, scale=(1.0, 1.0, 2.6), major=64)
    # Two engraved grooves round the outside: thin darker rings pressed slightly in.
    dark = metal("groove", "#8a7650", "#b09868", 0.4)
    g1 = torus(0.567, 0.018, dark, loc=(0, 0, 0.09), major=64, minor=8)
    g2 = torus(0.567, 0.018, dark, loc=(0, 0, -0.09), major=64, minor=8)
    obj = join([band, g1, g2], "acc_ring_band")
    return gl.render_icon_obj(obj, "acc_ring_band", gl.display(10.0, 62.0, -18.0))


def ring_signet():
    vc.reset_scene()
    m = steel()
    # The hoop stands upright (axis along X); the bezel sits on top and faces up.
    hoop = torus(0.45, 0.07, m, rot=(0, math.radians(90), 0), scale=(1.5, 1.0, 1.0), major=64)
    sh = sphere(0.16, m, loc=(0, 0, 0.44), scale=(1.3, 1.8, 0.7))
    bezel = box((0.5, 0.5, 0.16), m, loc=(0, 0, 0.55), bevel=0.04)
    rim = box((0.4, 0.4, 0.06), metal("bezel_rim", "#c8ccd2", "#f2f4f6", 0.2), loc=(0, 0, 0.63), bevel=0.02)
    stone = sphere(0.19, gem("signet_gem", "#7fb6e8", "#1a3a5a"), loc=(0, 0, 0.64), scale=(1.0, 1.0, 0.55),
                   seg=8, rings=4)
    for p in stone.data.polygons:
        p.use_smooth = False
    obj = join([hoop, sh, bezel, rim, stone], "acc_ring_signet")
    return gl.render_icon_obj(obj, "acc_ring_signet", gl.display(25.0, 58.0, 0.0))


def earring_hoop():
    vc.reset_scene()
    m = gold()
    hoop = torus(0.42, 0.055, m, loc=(0, 0, -0.1), rot=(math.radians(90), 0, 0), major=64)
    # Ear wire: an arc rising from the top of the hoop and curling back.
    wire = curve_tube([(0.0, 0.0, 0.32), (0.05, 0.0, 0.5), (0.22, 0.0, 0.62), (0.36, 0.0, 0.52), (0.38, 0.0, 0.36)],
                      m, 0.02)
    bead = sphere(0.1, gem("hoop_bead", "#e8c9d6", "#3a1a2a"), loc=(0, 0, -0.54))
    cap = sphere(0.06, m, loc=(0, 0, 0.33))
    obj = join([hoop, wire, bead, cap], "acc_earring_hoop")
    return gl.render_icon_obj(obj, "acc_earring_hoop", gl.display(-20.0, 10.0, 8.0))


def earring_drop():
    vc.reset_scene()
    m = steel()
    loop = torus(0.17, 0.035, m, loc=(0, 0, 0.75), rot=(math.radians(90), 0, 0), major=40)
    link = torus(0.09, 0.025, m, loc=(0, 0, 0.5), rot=(0, 0, 0), scale=(1.0, 1.0, 1.6), major=24)
    link.rotation_euler = (0, math.radians(90), 0)
    cap = cylinder(0.11, 0.1, m, loc=(0, 0, 0.36), verts=24, bevel=0.02)
    drop = sphere(0.2, gem("drop_gem", "#9fd8c4", "#0f3a2c"), loc=(0, 0, 0.0), seg=12, rings=8)
    # Pull the top of the sphere into a tear point.
    for v in drop.data.vertices:
        if v.co.z > 0.0:
            k = v.co.z / 0.2
            v.co.x *= 1.0 - 0.75 * k
            v.co.y *= 1.0 - 0.75 * k
            v.co.z *= 1.6
    for p in drop.data.polygons:
        p.use_smooth = False
    obj = join([loop, link, cap, drop], "acc_earring_drop")
    return gl.render_icon_obj(obj, "acc_earring_drop", gl.display(-15.0, 12.0, 10.0))


def necklace_chain():
    vc.reset_scene()
    m = gold()
    parts = []
    # Links along a U (seen from the front): a hanging catenary-like curve in XZ.
    n = 30
    pts = []
    for i in range(n + 1):
        t = i / n
        a = math.radians(200 + 140 * t)
        pts.append(Vector((0.9 * math.cos(a), 0.0, 0.25 + 0.95 * math.sin(a))))
    for i in range(n):
        p0, p1 = pts[i], pts[i + 1]
        mid = (p0 + p1) * 0.5
        d = (p1 - p0).normalized()
        ang = math.atan2(d.z, d.x)
        o = torus(0.05, 0.016, m, loc=mid, scale=(1.6, 1.0, 1.0), major=16, minor=6)
        # Lay the link along the curve; alternate links turned 90 degrees about their axis.
        o.matrix_world = (Matrix.Translation(mid) @ Matrix.Rotation(-ang, 4, 'Y') @
                          Matrix.Rotation(math.radians(90 if i % 2 else 0), 4, 'X'))
        parts.append(o)
    low = pts[n // 2]
    bail = torus(0.06, 0.018, m, loc=low + Vector((0, 0, -0.07)), rot=(0, math.radians(90), 0), major=16, minor=6)
    pend = cylinder(0.22, 0.05, m, loc=low + Vector((0, 0, -0.33)), rot=(math.radians(90), 0, 0), verts=40, bevel=0.015)
    ring = torus(0.2, 0.025, metal("pend_rim", "#b39260", "#e6cf9c", 0.3), loc=low + Vector((0, -0.03, -0.33)),
                 rot=(math.radians(90), 0, 0), major=40, minor=8)
    stone = sphere(0.09, gem("pend_gem", "#e89a8a", "#3a120c"), loc=low + Vector((0, -0.05, -0.33)),
                   scale=(1.0, 0.55, 1.0), seg=10, rings=6)
    for p in stone.data.polygons:
        p.use_smooth = False
    obj = join(parts + [bail, pend, ring, stone], "acc_necklace_chain")
    return gl.render_icon_obj(obj, "acc_necklace_chain", gl.display(0.0, -8.0, 0.0))


def necklace_torc():
    vc.reset_scene()
    m = gold()
    # Rigid open collar: an ellipse of ~290 degrees in XY with the gap at the front (-Y).
    pts = arc(0.7, -55, 235, 24, ry=0.82)
    bar = curve_tube(pts, m, 0.07)
    # A spiral wire wound round the bar (twisted look).
    spiral = []
    for i in range(240):
        t = i / 239
        a = math.radians(-50 + 280 * t)
        c = Vector((0.7 * math.cos(a), 0.7 * 0.82 * math.sin(a), 0.0))
        out = Vector((math.cos(a), 0.82 * math.sin(a), 0.0)).normalized()
        ph = 2 * math.pi * 22 * t
        spiral.append(c + out * 0.07 * math.cos(ph) + Vector((0, 0, 0.07 * math.sin(ph))))
    wire = curve_tube(spiral, metal("torc_wire", "#b4945c", "#e2c995", 0.32), 0.018, resolution=4)
    ends = []
    for a in (-55, 235):
        r = math.radians(a)
        ends.append(sphere(0.13, m, loc=(0.7 * math.cos(r), 0.7 * 0.82 * math.sin(r), 0.0), scale=(1.0, 1.0, 0.9)))
    obj = join([bar, wire] + ends, "acc_necklace_torc")
    return gl.render_icon_obj(obj, "acc_necklace_torc", gl.display(0.0, 55.0, 0.0))


def belt_strap(mat, R=0.75, height=0.24, a0=-60.0, a1=250.0):
    """An upright flat strap round a loose ellipse (the belt seen as if lying closed)."""
    pts = arc(R, a0, a1, 28, ry=0.62)
    return curve_tube(pts, mat, 0.012, extrude=height)


def belt_leather():
    vc.reset_scene()
    lm = leather("belt_leather")
    m = steel()
    strap = belt_strap(lm)
    # Buckle at the front: a rounded rectangular frame standing upright, and its prong.
    a = math.radians(-75)
    bx, by = 0.75 * math.cos(a), 0.75 * 0.62 * math.sin(a)
    frame = torus(0.24, 0.045, m, loc=(bx, by - 0.02, 0), rot=(math.radians(90), math.radians(45), math.radians(15)),
                  major=4, minor=8)
    prong = box((0.05, 0.03, 0.36), m, loc=(bx, by - 0.03, 0.0), rot=(0, math.radians(90), math.radians(15)), bevel=0.01)
    # Rivets / holes along the strap.
    studs = []
    for k in range(7):
        ang = math.radians(-20 + 34 * k)
        p = Vector((0.75 * math.cos(ang), 0.75 * 0.62 * math.sin(ang), 0.0))
        out = Vector((math.cos(ang) * 0.62, math.sin(ang), 0.0)).normalized()
        studs.append(sphere(0.045, m, loc=p + out * 0.02, scale=(1, 1, 1), seg=12, rings=6))
    obj = join([strap, frame, prong] + studs, "acc_belt_leather")
    return gl.render_icon_obj(obj, "acc_belt_leather", gl.display(0.0, 32.0, 0.0))


def belt_plated():
    vc.reset_scene()
    lm = leather("plated_leather")
    m = steel()
    strap = belt_strap(lm, height=0.26)
    plates = []
    for k in range(11):
        ang = math.radians(-40 + 27 * k)
        p = Vector((0.75 * math.cos(ang), 0.75 * 0.62 * math.sin(ang), 0.0))
        tan = Vector((-math.sin(ang), 0.62 * math.cos(ang), 0.0)).normalized()
        out = Vector((tan.y, -tan.x, 0.0))
        rot = math.atan2(tan.y, tan.x)
        o = box((0.19, 0.04, 0.34), m, loc=p + out * 0.035, rot=(0, 0, rot), bevel=0.015)
        plates.append(o)
        plates.append(sphere(0.035, gold(), loc=p + out * 0.06, seg=10, rings=6))
    # A clasp: two round bosses at the front gap.
    clasp = []
    for a in (-60, -98):
        r = math.radians(a)
        clasp.append(cylinder(0.15, 0.06, gold(), loc=(0.75 * math.cos(r), 0.75 * 0.62 * math.sin(r) - 0.03, 0.0),
                              rot=(math.radians(90), 0, 0), verts=32, bevel=0.012))
    obj = join([strap] + plates + clasp, "acc_belt_plated")
    return gl.render_icon_obj(obj, "acc_belt_plated", gl.display(0.0, 32.0, 0.0))


ICONS = {
    "ring_band": ring_band,
    "ring_signet": ring_signet,
    "earring_hoop": earring_hoop,
    "earring_drop": earring_drop,
    "necklace_chain": necklace_chain,
    "necklace_torc": necklace_torc,
    "belt_leather": belt_leather,
    "belt_plated": belt_plated,
}


def main():
    argv = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
    only = None
    if "--only" in argv:
        only = set(argv[argv.index("--only") + 1].split(","))
    for key, fn in ICONS.items():
        if only is None or key in only:
            fn()


if __name__ == "__main__":
    main()
