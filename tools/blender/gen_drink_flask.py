"""fix1: the small potion flask a character holds while its `drink` clip plays (the client hides the
main-hand weapon for the clip and draws this at the main hand's "dagger" grip; docs/CLIENT_RENDERING.md).

Usage (headless):
  blender --background --factory-startup --python tools/blender/gen_drink_flask.py

Output: assets_src/props/drink_flask.glb (flat materials: red glass, a darker liquid band, cork, a twine
cord; ~200 triangles). Grip frame as the weapons (gamedata/attachments.toml): origin where the hand
closes (round the flask's belly), engine +Y along the flask towards its cork, +Z its front. Original
shape, deterministic.
"""

import math
import os
import sys

sys.dont_write_bytecode = True
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import vo_common as vc  # noqa: E402

SIDES = 12
# (radius, z) from the bottom up, metres; z = 0 is the grip (Blender +Z = engine +Y).
GLASS = [(0.0, -0.052), (0.026, -0.050), (0.036, -0.036), (0.038, -0.012), (0.034, 0.012), (0.022, 0.030),
         (0.012, 0.042), (0.011, 0.068), (0.014, 0.072)]
CORK = [(0.0, 0.066), (0.0115, 0.066), (0.013, 0.090), (0.0, 0.092)]
CORD = [(0.0128, 0.052), (0.0128, 0.058)]


def lathe(prof, mat, verts, faces, mats, cap_top=True, cap_bottom=True):
    base = len(verts)
    rings = []
    for (r, z) in prof:
        ring = []
        for k in range(SIDES):
            a = 2.0 * math.pi * k / SIDES
            ring.append(len(verts))
            verts.append((r * math.cos(a), r * math.sin(a), z))
        rings.append(ring)
    for ra, rb in zip(rings, rings[1:]):
        for k in range(SIDES):
            k2 = (k + 1) % SIDES
            faces.append((ra[k], ra[k2], rb[k2], rb[k]))
            mats.append(mat)
    if cap_bottom and prof[0][0] > 0:
        faces.append(tuple(reversed(rings[0])))
        mats.append(mat)
    if cap_top and prof[-1][0] > 0:
        faces.append(tuple(rings[-1]))
        mats.append(mat)
    return base


def main():
    vc.reset_scene()
    glass = vc.material("flask_glass", "#b51f2a", roughness=0.18)
    cork = vc.material("flask_cork", "#8a6440", roughness=0.9)
    cord = vc.material("flask_cord", "#d8c9a0", roughness=0.95)
    verts, faces, mats = [], [], []
    lathe(GLASS, 0, verts, faces, mats)
    lathe(CORK, 1, verts, faces, mats)
    lathe(CORD, 2, verts, faces, mats)
    obj = vc.mesh_object("drink_flask", verts, faces, mats, [glass, cork, cord], smooth=True, sharp_angle=50.0,
                         recalc=True)
    out = os.path.join(vc.SRC, "props", "drink_flask.glb")
    vc.export_glb(out, [obj])
    print(f"PROP drink_flask: {vc.triangle_count(obj)} tris -> {out}")


if __name__ == "__main__":
    main()
