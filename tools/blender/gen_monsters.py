"""Humanoid monsters: Tripo H3.1 meshes rigged to the players' skeleton (59 bones: 53 UAL + 6
helpers) with the players' clips, through the character pipeline (gen_character.py: mesh clean-up,
fit, bone heat bind, weight clean-up, deformation into the shared A-pose rest).

Usage (headless):
  blender --background --factory-startup --python tools/blender/gen_monsters.py -- [--only ash_thrall] [--no-preview]
  blender ... gen_monsters.py -- --prepare <raw dir> [--only <names>]   (once per new raw source)
  blender ... gen_monsters.py -- --measure <dir> [--only <names>]        (scratch renders for the fit)
  blender ... gen_monsters.py -- --only slag_walker --debug <dir>        (+ weight renders in <dir>)

Sources: tools/blender/source_meshes/monsters/<name>.glb. The raw Tripo output (~1.45M triangles,
~44 MB each, external_sources/monsters_a/, not committed) is reduced once by `--prepare` exactly like
the weapons (gen_weapons.prepare: welded, collapse-decimated to PREP_TRIS, the raw file's three
embedded 2048^2 images copied byte for byte); the A-pose concept image goes next to it as
<name>_apose.png (half size).

Outputs per monster (model name = gamedata/monsters.toml `model`):
  assets_src/monsters/<name>.glb          skinned mesh "body", the players' 59-bone skeleton, MONSTER_CLIPS
  assets_src/monsters/<name>.png          base colour (sRGB)            } `texture`^2, from the 2048^2
  assets_src/monsters/<name>_n.png        normal map (OpenGL +Y)        } source maps
  assets_src/monsters/<name>_orm.png      R occlusion, G roughness, B metallic
  assets_src/monsters/<name>_e.png        emissive (glowing cracks / eyes / crystal), sRGB
  assets_src/monsters/<name>.anims.toml   clip loop flags and move speeds for assetc
  assets_src/monsters/monsters_meta.json  height, triangles, maps, walk / run stride speeds per monster
  assets_src/previews/monsters/<name>_*.png
Deterministic: same inputs -> same bytes.
"""

import json
import math
import os
import sys

import bmesh
import bpy
import numpy as np
from mathutils import Matrix, Vector

sys.dont_write_bytecode = True  # keep tools/blender free of __pycache__
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import vo_common as vc  # noqa: E402
import vo_ual  # noqa: E402
import gen_character as gc  # noqa: E402  (its main() does not run on import)
import gen_props as gp  # noqa: E402
import gen_weapons as gw  # noqa: E402

SOURCE_DIR = os.path.join(vc.HERE, "source_meshes", "monsters")
OUT_DIR = os.path.join(vc.SRC, "monsters")
PREVIEW_DIR = os.path.join(vc.PREVIEWS, "monsters")
META_PATH = os.path.join(OUT_DIR, "monsters_meta.json")

PREP_TRIS = 50000  # triangles of the committed reduced sources
REF_FACTOR = 2     # concept images are stored at half size

# The clips a monster needs (docs/PROTOCOL_COMBAT.md §1.3): locomotion, two attacks, the hit
# reaction, death and the simple spell set. Order = vo_ual.CLIPS order.
MONSTER_CLIP_NAMES = ("idle", "walk", "run", "punch_jab", "punch_cross", "spell_enter", "spell_idle",
                      "spell_shoot", "spell_exit", "hit_chest", "death")
MONSTER_CLIPS = [c for c in vo_ual.CLIPS if c[0] in MONSTER_CLIP_NAMES]

COMMON = dict(gc.COMMON, texture=1024, max_tris=30000)

# Per monster: the gen_character.LOOKS fields (fit, weight rules; see the comment there) plus
#   emissive  glow map: texels whose base colour passes the `rule` (hue / saturation / value window)
#             get colour x (1 - tint_mix) + tint x tint_mix, times `strength` in the material
MONSTERS = []
MONSTER = {}


# ---------------------------------------------------------------------------------------------
# --prepare: raw Tripo glb -> reduced committed source (geometry decimated, images byte-identical)

def prepare(raw_dir, names):
    for name in names:
        raw_path = os.path.join(raw_dir, f"{name}_tripo.glb")
        vc.reset_scene()
        obj = gw.import_glb(raw_path)
        obj.data.calc_loop_triangles()
        n0 = len(obj.data.loop_triangles)
        bm = bmesh.new()
        bm.from_mesh(obj.data)
        bmesh.ops.remove_doubles(bm, verts=bm.verts, dist=gp.FLOAT_EPS * 0.01)  # raw units (1 unit tall)
        bm.to_mesh(obj.data)
        bm.free()
        n1 = gp.decimate(obj, PREP_TRIS)
        gp.triangulate(obj)
        raw_gltf, raw_bin = gw._read_glb(raw_path)
        verts, idx = gw._mesh_arrays(obj)
        out = os.path.join(SOURCE_DIR, f"{name}.glb")
        gw.write_glb(out, verts, idx, raw_gltf, raw_bin)
        ref = os.path.join(raw_dir, f"{name}_apose.png")
        if os.path.exists(ref):
            img = vc.read_image(ref)
            h, w = img.shape[:2]
            img = img[:h - h % REF_FACTOR, :w - w % REF_FACTOR, :3]
            small = gp.down_color(np.ascontiguousarray(img), img.shape[0] // REF_FACTOR) if img.shape[0] == img.shape[1] \
                else gp.lin_to_srgb(gp.srgb_to_lin(img).reshape(img.shape[0] // REF_FACTOR, REF_FACTOR,
                                                               img.shape[1] // REF_FACTOR, REF_FACTOR, 3).mean(axis=(1, 3)))
            vc.write_png(os.path.join(SOURCE_DIR, f"{name}_apose.png"), vc.to_u8(small))
        print(f"PREPARE {name}: {n0} -> {n1} tris, {len(verts)} verts, {os.path.getsize(out) / 1e6:.2f} MB")


# ---------------------------------------------------------------------------------------------
# --measure: the normalised mesh (no decimation) in orthographic views with a 10 cm grid

def measure(R, out_dir):
    vc.reset_scene()
    Rm = dict(R, max_tris=None)
    body, _ = gc.load_source(Rm)
    V = np.array([v.co for v in body.data.vertices])
    print(f"MEASURE {R['look']}: bounds {np.round(V.min(0), 3)} .. {np.round(V.max(0), 3)}")
    cam = vc.setup_preview((900, 1200), background="#9a9a9a", sun_dir=(-0.5, -1.0, 0.8), strength=3.0)
    sc = bpy.context.scene
    sc.render.engine = 'BLENDER_WORKBENCH'
    sc.display.shading.light = 'STUDIO'
    sc.display.shading.color_type = 'TEXTURE'
    H = R["height"]
    grid_mat = vc.material("grid", "#202020")
    lines = []
    for k in range(0, int(H * 10) + 3):  # horizontal lines every 10 cm (front and side planes)
        for plane in ("front", "side"):
            bpy.ops.mesh.primitive_cube_add(size=1)
            ln = bpy.context.active_object
            if plane == "front":
                ln.location = (0, 1.0, k * 0.1)
                ln.scale = (2.4, 0.001, 0.002 if k % 5 else 0.005)
            else:
                ln.location = (-1.0, 0, k * 0.1)
                ln.scale = (0.001, 2.4, 0.002 if k % 5 else 0.005)
            ln.data.materials.append(grid_mat)
            lines.append(ln)
    for k in range(-12, 13):  # vertical lines
        for plane in ("front", "side"):
            bpy.ops.mesh.primitive_cube_add(size=1)
            ln = bpy.context.active_object
            if plane == "front":
                ln.location = (k * 0.1, 1.0, H / 2 + 0.1)
                ln.scale = (0.002 if k % 5 else 0.005, 0.001, H + 0.4)
            else:
                ln.location = (-1.0, k * 0.1, H / 2 + 0.1)
                ln.scale = (0.001, 0.002 if k % 5 else 0.005, H + 0.4)
            ln.data.materials.append(grid_mat)
    for vname, loc in (("front", (0, -6, H / 2)), ("side", (6, 0, H / 2))):
        vc.aim_camera(cam, loc, (0, 0, H / 2), ortho_scale=max(H, 1.6) + 0.3)
        sc.render.filepath = os.path.join(out_dir, f"{R['look']}_{vname}.png")
        bpy.ops.render.render(write_still=True)


def main():
    args = vc.script_args()
    names = [M["look"] for M in MONSTERS] or ["ash_thrall", "slag_walker", "ember_warden"]
    if "--only" in args:
        names = args[args.index("--only") + 1].split(",")
    if "--prepare" in args:
        prepare(args[args.index("--prepare") + 1], names)
        return
    if "--measure" in args:
        out = vc.ensure_dir(args[args.index("--measure") + 1])
        for n in names:
            measure(MONSTER[n], out)
        return


if __name__ == "__main__":
    main()
