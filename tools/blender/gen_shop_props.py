"""M14 "Pazar" props: a potion stall and a smithy corner for each homeland (Ortaocak already has
its market stall). Built and baked exactly like the M13 settlement props (gen_zone_props.py:
procedural geometry from boxes / lathes, Cycles-baked base colour, normal, ORM and emissive maps,
one material, the zone props' format), in the homeland styles:

  aydor_potion_stall    white-stone counter with an oak top, shelves of red and blue potions, a
                        blue-and-cream striped awning on gold-capped posts, a pale-sun disc
  aydor_smithy          anvil on a white-stone block, stone quench trough, an oak tool and weapon
                        rack under a small blue lean-to, a coal bucket
  kargan_potion_stall   dark-stone counter, iron-banded shelves, potions in iron cages, bundles of
                        roots and herbs on a beam, a hide awning with an ember-red edge, a lantern
  kargan_smithy         great anvil on a rock stump, iron quench trough, an iron weapon rack (axes,
                        spears, a round shield), a grindstone, a coal bucket

Usage (headless):
  blender --background --factory-startup --python tools/blender/gen_shop_props.py -- [--only aydor_smithy,...] [--no-preview]

Outputs: assets_src/props/<name>.glb + <name>.png, _n.png, _orm.png, _e.png;
assets_src/props/shop_props_meta.json (bounds, triangles, footprint in engine axes, the merchant's
stand point); previews/props/<name>.png, previews/props/m14_props_sheet.png. Front = -Y (the
customer's side, engine +Z), the merchant stands behind the counter at `stand` (engine x, z).
"""

import json
import math
import os
import sys

import numpy as np

sys.dont_write_bytecode = True
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import vo_common as vc  # noqa: E402
import gen_props as gp  # noqa: E402
import gen_zone_props as zp  # noqa: E402
from gen_zone_props import G  # noqa: E402

META_PATH = os.path.join(zp.OUT_DIR, "shop_props_meta.json")


def extra_materials():
    M = zp.MATS
    M["glass_red"] = zp.flat_mat("glass_red", "#a4161a", rough=0.15, emit="#7a1010", noise=0.06)
    M["glass_blue"] = zp.flat_mat("glass_blue", "#1c3ca8", rough=0.15, emit="#14286e", noise=0.06)
    M["water"] = zp.flat_mat("water", "#1b262c", rough=0.08, noise=0.04)
    M["hide"] = zp.rough_mat("hide", "#5e4432", "#3a281c", scale=2.2, rough=0.86, bump=0.6)
    M["cloth_ember"] = zp.flat_mat("cloth_ember", "#a8401a", rough=0.85)
    M["herb"] = zp.rough_mat("herb", "#56602e", "#2c3418", scale=7.0, rough=0.95, bump=1.2)
    M["root"] = zp.rough_mat("root", "#6a4a30", "#3a2618", scale=8.0, rough=0.95, bump=1.0)
    M["steel"] = zp.rough_mat("steel", "#a4aab2", "#7c828c", scale=3.0, rough=0.32, bump=0.15, metal=0.9)
    M["cork"] = zp.rough_mat("cork", "#9a7650", "#6e5234", scale=12.0, rough=0.9, bump=0.4)


def bottle(g, x, y, z, glass, s=1.0):
    g.lathe((x, y, z), [(0.0, 0.0), (0.05 * s, 0.003), (0.06 * s, 0.05 * s), (0.055 * s, 0.11 * s), (0.02 * s, 0.15 * s),
                        (0.018 * s, 0.19 * s), (0.0, 0.195 * s)], 10, glass)
    g.cyl((x, y, z + 0.185 * s), 0.017 * s, 0.019 * s, 0.04 * s, 6, "cork")


def flask_round(g, x, y, z, glass, s=1.0):
    g.lathe((x, y, z), [(0.0, 0.0), (0.04 * s, 0.006), (0.07 * s, 0.05 * s), (0.065 * s, 0.1 * s), (0.025 * s, 0.13 * s),
                        (0.016 * s, 0.17 * s), (0.0, 0.175 * s)], 10, glass)
    g.cyl((x, y, z + 0.165 * s), 0.015 * s, 0.017 * s, 0.035 * s, 6, "cork")


def bottle_row(g, x0, x1, y, z, n, pattern, s=1.0):
    for k in range(n):
        x = x0 + (x1 - x0) * (k + 0.5) / n
        glass = pattern[k % len(pattern)]
        (bottle if k % 3 != 1 else flask_round)(g, x + g.rng.uniform(-0.015, 0.015), y + g.rng.uniform(-0.02, 0.02), z,
                                                glass, s * g.rng.uniform(0.85, 1.1))


def anvil(g, x, y, z, s=1.0, mat="iron"):
    """Anvil standing at z (its base), horn towards +X."""
    g.box((x, y, z + 0.05 * s), (0.36 * s, 0.26 * s, 0.1 * s), mat, taper=0.8)
    g.box((x, y, z + 0.17 * s), (0.18 * s, 0.14 * s, 0.16 * s), mat, taper=1.25)
    g.box((x, y, z + 0.32 * s), (0.5 * s, 0.17 * s, 0.13 * s), mat)
    # Horn: a cone along +X, built as a lathe about Z then turned (rot about Y by -90 deg).
    i0 = len(g.verts)
    g.lathe((0, 0, 0), [(0.075 * s, 0.0), (0.05 * s, 0.12 * s), (0.0, 0.26 * s)], 10, mat)
    for i in range(i0, len(g.verts)):
        p = zp.rot(np.array(g.verts[i]), ry=math.radians(90.0))
        g.verts[i] = (x + 0.25 * s + p[0], y + p[1], z + 0.34 * s + p[2])
    g.box((x - 0.29 * s, y, z + 0.33 * s), (0.1 * s, 0.13 * s, 0.11 * s), mat, taper=0.7)


def hammer(g, x, y, z, ang=0.0, s=1.0, head="iron", handle="wood"):
    """Hammer hanging head-up from a peg at (x, y, z)."""
    g.box((x, y, z), (0.16 * s, 0.06 * s, 0.07 * s), head, rz=ang)
    g.cyl((x, y, z - 0.42 * s), 0.018 * s, 0.02 * s, 0.4 * s, 6, handle)


def sword(g, x, y, z, ang=0.0, guard="gold"):
    """A sword standing point-down (leaning in a rack), hilt at z."""
    g.box((x, y, z - 0.47), (0.05, 0.012, 0.8), "steel", ry=ang)
    g.box((x, y, z - 0.05), (0.2, 0.035, 0.035), guard, ry=ang)
    g.cyl((x, y, z - 0.03), 0.017, 0.017, 0.15, 6, "wood_dark")
    g.cyl((x, y, z + 0.12), 0.028, 0.0, 0.05, 6, guard)


def awning_stripes(g, x0, x1, y0, y1, z0, z1, n, mats, depth=0.04):
    """A sloped canopy from (y0, z0) (front) to (y1, z1) (back) made of n stripes along X."""
    pitch = math.atan2(z1 - z0, y1 - y0)
    L = math.hypot(z1 - z0, y1 - y0)
    for k in range(n):
        xa = x0 + (x1 - x0) * k / n
        xb = x0 + (x1 - x0) * (k + 1) / n
        g.box(((xa + xb) / 2, (y0 + y1) / 2, (z0 + z1) / 2), (xb - xa + 0.002, L, depth), mats[k % len(mats)],
              rx=pitch)
    # Valance: a short hanging flap along the front edge, tabs alternating.
    for k in range(n):
        xa = x0 + (x1 - x0) * k / n
        xb = x0 + (x1 - x0) * (k + 1) / n
        g.box(((xa + xb) / 2, y0 - 0.01, z0 - 0.13), (xb - xa - 0.01, 0.025, 0.26), mats[k % len(mats)])


def aydor_potion_stall():
    g = G(41)
    g.box((0, -0.45, 0.45), (2.6, 0.62, 0.9), "stone_white")                 # counter
    g.box((0, -0.45, 0.935), (2.76, 0.74, 0.07), "wood")                      # counter top
    g.box((0, -0.45, 0.06), (2.7, 0.7, 0.14), "stone_white_big")               # plinth
    bottle_row(g, -1.15, -0.2, -0.5, 0.97, 7, ["glass_red", "glass_red", "glass_blue"])
    bottle_row(g, 0.35, 1.2, -0.42, 0.97, 6, ["glass_blue", "glass_red"])
    g.box((0.08, -0.5, 1.0), (0.32, 0.22, 0.06), "wood_dark")                  # a small tray
    for k in range(3):
        flask_round(g, -0.02 + 0.1 * k, -0.5, 1.03, "glass_red" if k != 1 else "glass_blue", 0.7)
    # Back shelves (the merchant stands between counter and shelves).
    for sx in (-1, 1):
        g.box((sx * 1.15, 0.62, 1.0), (0.09, 0.3, 2.0), "wood")
    for zz in (0.62, 1.08, 1.54):
        g.box((0, 0.62, zz), (2.4, 0.32, 0.05), "wood")
        bottle_row(g, -1.05, 1.05, 0.62, zz + 0.025, 11, ["glass_red", "glass_blue", "glass_red", "glass_blue", "glass_blue"])
    g.box((0, 0.79, 1.0), (2.4, 0.03, 2.0), "wood_dark")                      # back board
    # Awning: four posts with gold caps, striped canopy sloping to the front, pale-sun disc.
    for (x, y, h) in ((-1.4, -0.86, 2.35), (1.4, -0.86, 2.35), (-1.4, 0.86, 2.62), (1.4, 0.86, 2.62)):
        g.cyl((x, y, 0.0), 0.06, 0.05, h, 8, "wood")
        g.lathe((x, y, h), [(0.0, 0.0), (0.07, 0.01), (0.08, 0.06), (0.0, 0.14)], 8, "gold")
    awning_stripes(g, -1.55, 1.55, -1.0, 0.95, 2.42, 2.7, 8, ["cloth_blue", "cloth_sun"])
    g.disc_y((0, -1.05, 2.22), 0.16, 0.03, 14, "gold", rays=12, ray_len=0.1)
    # Crates by the counter's end.
    g.box((1.72, -0.35, 0.22), (0.48, 0.44, 0.44), "wood_dark", rz=0.2)
    g.box((1.7, -0.35, 0.56), (0.36, 0.34, 0.24), "wood", rz=-0.15)
    return g.build("aydor_potion_stall", zp.MATS)


def aydor_smithy():
    g = G(42)
    g.box((0.0, -0.25, 0.3), (0.62, 0.5, 0.6), "stone_white_big")             # anvil block
    anvil(g, 0.0, -0.25, 0.6)
    # Quench trough: stone walls round a water surface.
    tx, ty = -1.25, -0.15
    g.box((tx, ty, 0.06), (0.72, 1.2, 0.12), "stone_white_big")
    for (dx, dy, sx, sy) in ((0, -0.55, 0.72, 0.1), (0, 0.55, 0.72, 0.1), (-0.31, 0, 0.1, 1.2), (0.31, 0, 0.1, 1.2)):
        g.box((tx + dx, ty + dy, 0.36), (sx, sy, 0.6), "stone_white")
    g.box((tx, ty, 0.5), (0.54, 1.02, 0.04), "water")
    # Rack against the back: oak frame, a tool board with hammers and tongs, swords leaning.
    rx, ry = 0.75, 0.75
    for sx in (-1, 1):
        g.box((rx + sx * 0.8, ry, 0.95), (0.09, 0.09, 1.9), "wood")
    for zz in (0.25, 1.05, 1.82):
        g.box((rx, ry, zz), (1.7, 0.08, 0.07), "wood")
    g.box((rx - 0.35, ry + 0.06, 1.42), (0.8, 0.04, 0.68), "plaster")           # tool board (limewashed)
    for k, x in enumerate((-0.62, -0.42, -0.22)):
        hammer(g, rx + x, ry - 0.02, 1.68, s=0.8 + 0.1 * (k % 2))
    for k in range(3):
        sword(g, rx + 0.15 + 0.2 * k, ry - 0.08, 1.02, ang=0.12)
    g.box((rx + 0.35, ry - 0.08, 0.25), (0.75, 0.05, 0.05), "gold")
    # Small blue lean-to over the rack.
    for sx in (-1, 1):
        g.cyl((rx + sx * 0.95, -0.15, 0.0), 0.05, 0.045, 2.3, 8, "wood")
        g.lathe((rx + sx * 0.95, -0.15, 2.3), [(0.0, 0.0), (0.06, 0.01), (0.07, 0.05), (0.0, 0.12)], 8, "gold")
    awning_stripes(g, rx - 1.1, rx + 1.1, -0.3, 0.95, 2.3, 2.55, 6, ["cloth_blue", "cloth_blue", "cloth_sun"])
    # Coal bucket and a stack of bars.
    g.lathe((1.65, -0.55, 0.0), [(0.0, 0.0), (0.2, 0.0), (0.23, 0.36), (0.21, 0.38)], 10, "iron")
    g.cyl((1.65, -0.55, 0.3), 0.2, 0.18, 0.07, 10, "coal")
    # A low oak pallet with a stack of bar stock, and a finished blade leaning on the anvil block.
    g.box((-0.55, -0.72, 0.06), (0.7, 0.32, 0.12), "wood")
    for k in range(5):
        g.box((-0.55, -0.82 + 0.05 * k, 0.15 + 0.045 * (k % 2)), (0.66, 0.04, 0.04), "steel")
    g.box((0.36, -0.52, 0.42), (0.05, 0.012, 0.8), "steel", ry=-0.35)
    g.box((0.5, -0.52, 0.8), (0.17, 0.03, 0.03), "gold", ry=-0.35)
    return g.build("aydor_smithy", zp.MATS)


def kargan_potion_stall():
    g = G(43)
    g.rock((0, -0.45, 0.35), (2.7, 0.8, 0.75), "stone_dark", rough=0.06, rings=6, segs=14)
    g.box((0, -0.45, 0.82), (2.8, 0.82, 0.16), "stone_dark")                   # slab top
    for sx in (-1, 0, 1):
        g.box((sx * 1.1, -0.86, 0.6), (0.06, 0.04, 0.5), "iron")
    # Iron cages of potions on the counter.
    for cx in (-0.85, 0.15, 0.95):
        g.box((cx, -0.45, 0.91), (0.6, 0.34, 0.03), "iron")
        g.box((cx, -0.45, 1.24), (0.6, 0.34, 0.03), "iron")
        for (dx, dy) in ((-0.28, -0.15), (0.28, -0.15), (-0.28, 0.15), (0.28, 0.15)):
            g.box((cx + dx, -0.45 + dy, 1.08), (0.025, 0.025, 0.34), "iron")
        bottle_row(g, cx - 0.24, cx + 0.24, -0.45, 0.925, 4, ["glass_red", "glass_blue"] if cx < 0.5 else ["glass_blue"], 0.95)
    # Back shelves: dark wood with iron bands.
    for sx in (-1, 1):
        g.box((sx * 1.2, 0.62, 1.05), (0.14, 0.28, 2.1), "wood_dark")
    for zz in (0.55, 1.05, 1.55):
        g.box((0, 0.62, zz), (2.5, 0.34, 0.07), "wood_dark")
        g.box((0, 0.46, zz + 0.02), (2.5, 0.02, 0.05), "iron")
        bottle_row(g, -1.05, 1.05, 0.62, zz + 0.035, 9, ["glass_red", "glass_red", "glass_blue"], 1.05)
    g.box((0, 0.8, 1.05), (2.5, 0.05, 2.1), "stone_dark")
    # Beam with hanging roots and herb bundles.
    g.box((0, -0.05, 2.15), (2.9, 0.1, 0.1), "wood_dark")
    for k in range(9):
        x = -1.25 + k * 0.31
        mat = "herb" if k % 2 else "root"
        g.cyl((x, -0.05, 1.68 + 0.04 * (k % 3)), 0.012, 0.075, 0.42, 6, mat)
        g.box((x, -0.05, 2.07), (0.02, 0.02, 0.1), "iron")
    # Lantern on the right post.
    g.cyl((1.3, -0.85, 1.75), 0.0, 0.09, 0.05, 6, "iron")
    g.box((1.3, -0.85, 1.6), (0.13, 0.13, 0.2), "glass_warm")
    g.cyl((1.3, -0.85, 1.48), 0.1, 0.08, 0.03, 6, "iron")
    # Hide awning on two heavy posts and the back wall, ember-red edge.
    for sx in (-1, 1):
        g.cyl((sx * 1.45, -0.85, 0.0), 0.09, 0.07, 2.4, 7, "wood_dark")
        g.box((sx * 1.45, -0.85, 2.0), (0.22, 0.22, 0.08), "iron")
    awning_stripes(g, -1.6, 1.6, -1.05, 0.95, 2.4, 2.75, 3, ["hide"], depth=0.05)
    g.box((0, -1.07, 2.21), (3.2, 0.035, 0.07), "cloth_ember")
    # A small brazier at the counter's left end.
    g.cyl((-1.75, -0.4, 0.0), 0.2, 0.17, 0.22, 8, "stone_dark")
    g.lathe((-1.75, -0.4, 0.2), [(0.08, 0.0), (0.12, 0.12), (0.3, 0.32), (0.34, 0.38), (0.28, 0.4)], 10, "iron")
    g.lathe((-1.75, -0.4, 0.5), [(0.28, 0.0), (0.26, 0.06), (0.15, 0.12), (0.0, 0.15)], 10, "coal")
    return g.build("kargan_potion_stall", zp.MATS)


def kargan_smithy():
    g = G(44)
    g.rock((0.0, -0.25, 0.32), (0.85, 0.7, 0.7), "rock", rough=0.1, rings=6, segs=12)
    anvil(g, 0.0, -0.25, 0.62, s=1.25)
    # Iron quench trough on stone feet.
    tx, ty = -1.35, -0.1
    for dy in (-0.45, 0.45):
        g.box((tx, ty + dy, 0.12), (0.7, 0.22, 0.24), "stone_dark")
    g.box((tx, ty, 0.28), (0.74, 1.24, 0.08), "iron")
    for (dx, dy, sx, sy) in ((0, -0.6, 0.74, 0.06), (0, 0.6, 0.74, 0.06), (-0.34, 0, 0.06, 1.24), (0.34, 0, 0.06, 1.24)):
        g.box((tx + dx, ty + dy, 0.5), (sx, sy, 0.48), "iron")
    g.box((tx, ty, 0.66), (0.62, 1.12, 0.04), "water")
    # Weapon rack: iron frame, axes, spears, a round shield.
    rx, ry = 0.85, 0.8
    for sx in (-1, 1):
        g.box((rx + sx * 0.85, ry, 1.0), (0.1, 0.1, 2.0), "iron")
    for zz in (0.3, 1.25, 1.95):
        g.box((rx, ry, zz), (1.8, 0.08, 0.08), "iron")
    for k in range(3):  # spears
        x = rx - 0.65 + 0.18 * k
        g.cyl((x, ry - 0.08, 0.05), 0.022, 0.02, 2.0, 6, "wood_dark")
        g.cyl((x, ry - 0.08, 2.05), 0.04, 0.0, 0.22, 4, "steel")
    for k in range(2):  # axes hanging head-up
        x = rx + 0.0 + 0.3 * k
        g.cyl((x, ry - 0.1, 0.55), 0.024, 0.024, 0.85, 6, "wood_dark")
        g.box((x + 0.1, ry - 0.1, 1.32), (0.22, 0.03, 0.2), "steel", taper=1.0)
        g.box((x + 0.2, ry - 0.1, 1.32), (0.04, 0.04, 0.3), "steel")
    g.disc_y((rx + 0.62, ry - 0.12, 1.0), 0.32, 0.06, 18, "wood_dark")         # shield
    g.disc_y((rx + 0.62, ry - 0.16, 1.0), 0.09, 0.04, 10, "iron")
    for k in range(4):
        a = k * math.pi / 2 + math.pi / 4
        g.box((rx + 0.62 + 0.2 * math.cos(a), ry - 0.155, 1.0 + 0.2 * math.sin(a)), (0.05, 0.03, 0.05), "iron")
    # Grindstone in a wooden frame (wheel facing the front).
    gx, gy = 1.75, -0.45
    for sx in (-1, 1):
        g.box((gx + sx * 0.2, gy, 0.35), (0.07, 0.4, 0.7), "wood_dark")
    g.box((gx, gy, 0.66), (0.48, 0.05, 0.05), "iron", rz=math.pi / 2)
    g.disc_y((gx, gy, 0.66), 0.3, 0.12, 18, "stone_grey")
    # Coal bucket.
    g.lathe((-0.6, -0.75, 0.0), [(0.0, 0.0), (0.2, 0.0), (0.24, 0.38), (0.22, 0.4)], 10, "iron")
    g.cyl((-0.6, -0.75, 0.32), 0.21, 0.18, 0.07, 10, "coal")
    return g.build("kargan_smithy", zp.MATS)


# name -> (builder, texture size, sink, footprint, emissive strength, merchant stand point (Blender x, y))
PROPS = {
    "aydor_potion_stall": (aydor_potion_stall, 1024, 0.05, ("hull", 2.2), 2.0, (0.0, 0.15)),
    "aydor_smithy": (aydor_smithy, 1024, 0.05, ("hull", 2.2), 3.0, (-0.6, 0.35)),
    "kargan_potion_stall": (kargan_potion_stall, 1024, 0.05, ("hull", 2.2), 2.5, (0.0, 0.15)),
    "kargan_smithy": (kargan_smithy, 1024, 0.05, ("hull", 2.2), 3.0, (-0.55, 0.3)),
}


def build(name, spec):
    builder, size, sink, foot, strength, stand = spec

    def wrapped():
        extra_materials()
        return builder()
    obj, meta = zp.build(name, (wrapped, size, sink, foot, strength))
    meta["source"] = "procedural (gen_shop_props.py)"
    # Where the merchant NPC stands (engine x = -Blender x, z = -Blender y), facing engine +Z like
    # the prop: behind the counter (stalls) or behind the anvil (smithies), so the counter / anvil
    # is between the merchant and the customer.
    meta["stand"] = [round(-stand[0], 3), round(-stand[1], 3)]
    return obj, meta


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
    if shots and not only:
        vc.compose_sheet(shots, os.path.join(vc.PREVIEWS, "props", "m14_props_sheet.png"), 2)


if __name__ == "__main__":
    main()
