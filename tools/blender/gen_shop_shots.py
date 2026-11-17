"""M14 "Pazar" review renders (not assets) into docs/screenshots/m14/ (prefix art_):

  art_npcs_lineup.png        the six merchants in their idle pose between two player looks
                             (aydor_0, kargan_0), front view, labelled
  art_npcs_<town>.png        per town: potion seller, smith and a player of the matching race,
                             front and three-quarter rows
  art_npcs_closeup.png       upper bodies of the six, three-quarter view
  art_npcs_talk.png          idle_talking and interact frames of two merchants
  art_stalls.png             the four new stall props with their merchant at the suggested stand
                             point, plus Ortaocak's existing market stall with the Eşik merchants
  art_markers.png            a merchant with the overhead marker billboard at the suggested height
  art_icons_real.png         the potion icons and shop glyphs at their real slot sizes (1:1 pixels)
  art_icons_enlarged.png     the same sheet 3x enlarged (nearest), to judge the small sizes

Reads only committed sources (assets_src/npcs, characters, props, ui). Cycles (CPU, fixed seed).

Usage (headless, after gen_npcs.py, gen_shop_props.py, gen_shop_icons.py):
  blender --background --factory-startup --python tools/blender/gen_shop_shots.py -- [--out docs/screenshots/m14] [--only lineup,stalls,...]
"""

import math
import os
import sys

import bpy
import numpy as np
from mathutils import Matrix, Vector

sys.dont_write_bytecode = True
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import vo_common as vc  # noqa: E402
import gen_character as gc  # noqa: E402
import gen_skill_art as sa  # noqa: E402
import gen_shop_icons as si  # noqa: E402
from gen_zone_shots import text  # noqa: E402

OUT = os.path.join(vc.REPO, "docs", "screenshots", "m14")
TOWNS = {"esik": ("npc_potion_esik", "npc_smith_esik", "kargan_1"),
         "aydor": ("npc_potion_aydor", "npc_smith_aydor", "aydor_0"),
         "kargan": ("npc_potion_kargan", "npc_smith_kargan", "kargan_0")}


def place_root(obj, x, y, yaw):
    """The glTF importer puts a correction rotation on the armature; keep it and add our own."""
    obj.matrix_world = Matrix.Translation((x, y, 0.0)) @ Matrix.Rotation(math.radians(yaw), 4, 'Z') @ obj.matrix_world


def load(path, x=0.0, y=0.0, yaw=0.0, clip="idle", frame=0):
    before = set(bpy.data.objects)
    acts_before = set(bpy.data.actions)
    bpy.ops.import_scene.gltf(filepath=path)
    new = [o for o in bpy.data.objects if o not in before]
    roots = [o for o in new if o.parent is None]
    arm = next((o for o in new if o.type == 'ARMATURE'), None)
    if arm is not None:
        acts = [a for a in bpy.data.actions if a not in acts_before]
        pick = [a for a in acts if a.name.split("_armature")[0].split(".")[0] == clip] or \
               [a for a in acts if a.name.startswith(clip)]
        if arm.animation_data is None:
            arm.animation_data_create()
        if pick:
            arm.animation_data.action = pick[0]
            if hasattr(arm.animation_data, "action_slot") and len(pick[0].slots):
                arm.animation_data.action_slot = pick[0].slots[0]
    for r in roots:
        place_root(r, x, y, yaw)
    bpy.context.scene.frame_set(frame)
    return new


def studio(res, ground="#77746d"):
    vc.reset_scene()
    cam = gc.setup_studio(res)
    bpy.ops.mesh.primitive_plane_add(size=60, location=(0, 0, 0))
    bpy.context.active_object.data.materials.append(vc.material("ground", ground))
    sc = bpy.context.scene
    sc.cycles.samples = 48
    return cam


def label(path, labels):
    img = vc.to_u8(vc.read_image(path))[:, :, :3].copy()
    for (s, x, y) in labels:
        text(img, s, x, y, (240, 236, 220), 2)
    vc.write_png(path, img)


def project(cam, p):
    """Pixel position of world point p in the current render."""
    from bpy_extras.object_utils import world_to_camera_view
    sc = bpy.context.scene
    co = world_to_camera_view(sc, cam, Vector(p))
    return int(co.x * sc.render.resolution_x), int((1 - co.y) * sc.render.resolution_y)


def npc_path(key):
    return os.path.join(vc.SRC, "npcs", f"{key}.glb")


def char_path(look):
    return os.path.join(vc.SRC, "characters", f"{look}.glb")


def shot_lineup(out):
    cam = studio((1800, 600))
    row = [("aydor_0", char_path("aydor_0"))] + [(k, npc_path(k)) for k in
                                                 ("npc_potion_esik", "npc_smith_esik", "npc_potion_aydor",
                                                  "npc_smith_aydor", "npc_potion_kargan", "npc_smith_kargan")] + \
          [("kargan_0", char_path("kargan_0"))]
    xs = np.linspace(-6.3, 6.3, len(row))
    for (name, p), x in zip(row, xs):
        load(p, x=float(x))
    vc.aim_camera(cam, (0, -19.0, 1.5), (0, 0, 1.0), lens=50)
    path = os.path.join(out, "art_npcs_lineup.png")
    vc.render_to(path)
    labels = []
    for (name, p), x in zip(row, xs):
        px, py = project(cam, (float(x), 0, 0.0))
        nm = name.replace("npc_", "").replace("_", " ")
        labels.append((nm, px - len(nm) * 6, py + 12))
    label(path, labels)


def shot_town(out, town):
    potion, smith, player = TOWNS[town]
    cam = studio((1200, 760))
    for k, (p, x) in enumerate(((char_path(player), -1.6), (npc_path(potion), 0.0), (npc_path(smith), 1.7))):
        load(p, x=x)
    tiles = []
    for k, loc in enumerate(((0, -7.6, 1.3), (5.2, -5.6, 2.2))):
        vc.aim_camera(cam, loc, (0, 0, 1.0), lens=50)
        tiles.append(vc.render_to(os.path.join(out, f"_tmp_{town}{k}.png")))
    path = os.path.join(out, f"art_npcs_{town}.png")
    vc.compose_sheet(tiles, path, 1)
    vc.remove_files(tiles)
    label(path, [(f"{player} (player)", 20, 16), (f"{potion}   {smith}", 20, 36)])


def shot_closeup(out):
    cam = studio((420, 520))
    tiles = []
    for key in ("npc_potion_esik", "npc_smith_esik", "npc_potion_aydor", "npc_smith_aydor", "npc_potion_kargan",
                "npc_smith_kargan"):
        new = load(npc_path(key))
        top = max((o.matrix_world @ Vector(c)).z for o in new if o.type == 'MESH' for c in o.bound_box)
        vc.aim_camera(cam, (1.25, -1.7, top - 0.15), (0, 0, top - 0.42), lens=50)
        tiles.append(vc.render_to(os.path.join(out, f"_tmp_close_{key}.png")))
        for o in new:
            bpy.data.objects.remove(o, do_unlink=True)
    path = os.path.join(out, "art_npcs_closeup.png")
    vc.compose_sheet(tiles, path, 6)
    vc.remove_files(tiles)


def shot_talk(out):
    cam = studio((300, 400))
    tiles = []
    for key in ("npc_potion_aydor", "npc_smith_kargan"):
        for clip, frames in (("idle_talking", (0, 22, 44, 66)), ("interact", (0, 20, 32, 50))):
            for f in frames:
                new = load(npc_path(key), clip=clip, frame=f)
                top = max((o.matrix_world @ Vector(c)).z for o in new if o.type == 'MESH' for c in o.bound_box)
                vc.aim_camera(cam, (2.8, -3.6, top * 0.7), (0, 0, top * 0.5), lens=50)
                tiles.append(vc.render_to(os.path.join(out, f"_tmp_talk_{key}_{clip}{f}.png")))
                for o in new:
                    bpy.data.objects.remove(o, do_unlink=True)
    path = os.path.join(out, "art_npcs_talk.png")
    vc.compose_sheet(tiles, path, 8)
    vc.remove_files(tiles)


def shot_stalls(out):
    import json
    with open(os.path.join(vc.SRC, "props", "shop_props_meta.json"), encoding="utf-8") as f:
        meta = json.load(f)
    cam = studio((960, 640), ground="#8a8476")
    tiles = []
    combos = [("aydor_potion_stall", "npc_potion_aydor", None), ("aydor_smithy", "npc_smith_aydor", None),
              ("kargan_potion_stall", "npc_potion_kargan", None), ("kargan_smithy", "npc_smith_kargan", "kargan_forge"),
              ("market_stall", "npc_potion_esik", None)]
    for prop, npc, extra in combos:
        new = load(os.path.join(vc.SRC, "props", f"{prop}.glb"))
        if extra:
            new += load(os.path.join(vc.SRC, "props", f"{extra}.glb"), x=-3.2, y=0.6)
        stand = meta.get(prop, {}).get("stand", [0.0, -0.15])
        new += load(npc_path(npc), x=-stand[0], y=-stand[1])
        if prop == "market_stall":
            new += load(npc_path("npc_smith_esik"), x=2.6, y=-0.6, yaw=-25.0)
        vc.aim_camera(cam, (4.0, -6.6, 3.0), (-0.3 if extra else 0.0, 0, 1.0), lens=40 if extra else 45)
        tiles.append(vc.render_to(os.path.join(out, f"_tmp_stall_{prop}.png")))
        for o in new:
            bpy.data.objects.remove(o, do_unlink=True)
    path = os.path.join(out, "art_stalls.png")
    vc.compose_sheet(tiles, path, 2)
    vc.remove_files(tiles)


MARKER_ABOVE = 0.45   # metres between the head top and the marker's lower tip (suggested)
MARKER_SIZE = 0.55    # metres, the billboard's side


def marker_plane(name, png, centre):
    me = bpy.data.meshes.new(name)
    h = MARKER_SIZE / 2
    me.from_pydata([(-h, 0, -h), (h, 0, -h), (h, 0, h), (-h, 0, h)], [], [(0, 1, 2, 3)])
    uv = me.uv_layers.new(name="UVMap")
    for k, (u, v) in enumerate(((0, 0), (1, 0), (1, 1), (0, 1))):
        uv.data[k].uv = (u, v)
    obj = vc.link(bpy.data.objects.new(name, me))
    obj.location = centre
    mat = bpy.data.materials.new(name)
    mat.use_nodes = True
    nt = mat.node_tree
    bsdf = nt.nodes["Principled BSDF"]
    tex = nt.nodes.new("ShaderNodeTexImage")
    tex.image = bpy.data.images.load(png, check_existing=False)
    nt.links.new(tex.outputs["Color"], bsdf.inputs["Emission Color"])
    nt.links.new(tex.outputs["Color"], bsdf.inputs["Base Color"])
    nt.links.new(tex.outputs["Alpha"], bsdf.inputs["Alpha"])
    bsdf.inputs["Emission Strength"].default_value = 0.6
    me.materials.append(mat)
    return obj


def shot_markers(out):
    cam = studio((1100, 700))
    tiles = []
    for k, (key, marker) in enumerate((("npc_potion_kargan", "marker_potion"), ("npc_smith_aydor", "marker_smith"))):
        new = load(npc_path(key), x=-1.0 + 2.0 * k)
        top = max((o.matrix_world @ Vector(c)).z for o in new if o.type == 'MESH' for c in o.bound_box)
        # The plaque's pin tip is at 0.96 of the image height from the top: the billboard centre sits
        # MARKER_ABOVE + 0.46 * MARKER_SIZE above the head top.
        marker_plane(marker, os.path.join(vc.SRC, "ui", "shop", f"{marker}.png"),
                     (-1.0 + 2.0 * k, 0.0, top + MARKER_ABOVE + 0.46 * MARKER_SIZE))
    load(char_path("aydor_1"), x=0.0, y=-2.2, yaw=180.0)
    vc.aim_camera(cam, (1.5, -9.5, 2.6), (0, 0, 1.4), lens=50)
    vc.render_to(os.path.join(out, "art_markers.png"))


def shot_icons(out):
    items = {n: vc.read_image(os.path.join(vc.SRC, "ui", "items", f"{n}.png")) for n in si.POTIONS}
    ui = {n: vc.read_image(os.path.join(vc.SRC, "ui", "shop", f"{n}.png")) for n in si.UI}
    real = os.path.join(out, "art_icons_real.png")
    si.sheet(items, ui, real)
    img = vc.to_u8(vc.read_image(real))[:, :, :3]
    # The 72/64/48/40 slot rows and the small glyph columns, 3x nearest.
    crop = img[150:520, 0:900]
    big = np.repeat(np.repeat(crop, 3, 0), 3, 1)
    vc.write_png(os.path.join(out, "art_icons_enlarged.png"), big)


SHOTS = {"lineup": shot_lineup, "esik": lambda o: shot_town(o, "esik"), "aydor": lambda o: shot_town(o, "aydor"),
         "kargan": lambda o: shot_town(o, "kargan"), "closeup": shot_closeup, "talk": shot_talk,
         "stalls": shot_stalls, "markers": shot_markers, "icons": shot_icons}


def main():
    args = vc.script_args()
    out = vc.ensure_dir(args[args.index("--out") + 1] if "--out" in args else OUT)
    only = args[args.index("--only") + 1].split(",") if "--only" in args else list(SHOTS)
    for name in only:
        SHOTS[name](out)
        print(f"SHOT {name}")


if __name__ == "__main__":
    main()
