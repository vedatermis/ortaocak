"""M13 review renders (not assets): per zone a top-down overview with the named markers, safe zones,
monster spawn circles (and aggro reach) drawn on it, and ground-level views of the homelands (town,
gate, hunting grounds, each new monster standing where it spawns). Reads only committed sources:
assets_src/terrain/<zone>/ (description, height, splat), the props, monsters and gamedata/monsters.toml.

Usage (headless, after gen_zones.py / gen_terrain.py):
  blender --background --factory-startup --python tools/blender/gen_zone_shots.py -- [--out docs/screenshots/m13] [--only 2]

Overview legend: yellow square = gate_*, green ring = arrive_*, blue square = spawn_<race>, pale green
circle = safe zone, red disc = monster wander circle (orange ring = + aggro radius); labels name the
markers and the monster template ids. Image: +X right, +Z (north) up, 2 px per metre.
"""

import math
import os
import sys
import tomllib

import bpy
import numpy as np

sys.dont_write_bytecode = True
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import vo_common as vc  # noqa: E402

M = 512

# A 5x7 bitmap font for the labels (rows top to bottom, '#' = ink).
FONT = {
    "A": ".###.#...##...#######...##...##...#", "B": "####.#...#####.#...##...##...#####.",
    "C": ".###.#...##....#....#....#...#.###.", "D": "####.#...##...##...##...##...#####.",
    "E": "######....####.#....#....#....#####", "F": "######....####.#....#....#....#....",
    "G": ".###.#...##....#.####...##...#.####", "H": "#...##...#######...##...##...##...#",
    "I": ".###...#....#....#....#....#...###.", "J": "..###...#....#....#....##..#.##...",
    "K": "#...##..#.#.#..##...#.#..#..#.#...#", "L": "#....#....#....#....#....#....#####",
    "M": "#...###.###.#.##...##...##...##...#", "N": "#...###..##.#.##..###...##...##...#",
    "O": ".###.#...##...##...##...##...#.###.", "P": "####.#...##...#####.#....#....#....",
    "R": "####.#...##...#####.#.#..#..#.#...#", "S": ".####....#.....###.....#....#####.",
    "T": "#####..#....#....#....#....#....#..", "U": "#...##...##...##...##...##...#.###.",
    "V": "#...##...##...##...##...#.#.#...#..", "W": "#...##...##...##.#.##.#.##.#..#.#.","Y": "#...##...#.#.#...#....#....#....#..",
    "Z": "#####....#...#...#...#...#....#####", "0": ".###.#...##..###.#.###..##...#.###.",
    "1": "..#...##....#....#....#....#...###.", "2": ".###.#...#....#...#...#...#...#####",
    "3": "#####...#...#.....#....##...#.###.", "4": "...#...##..#.#.#..######...#....#.",
    "5": "######....####.....#....##...#.###.", "6": "..##..#...#....####.#...##...#.###.",
    "7": "#####....#...#...#...#....#....#...", "8": ".###.#...##...#.###.#...##...#.###.",
    "9": ".###.#...##...#.####....#...#..##..", "_": "..............................#####",
    " ": "...................................", "-": "...............###.................",
}
FONT.update({k: "".join(rows) for k, rows in {
    "J": ("..###", "...#.", "...#.", "...#.", "#..#.", "#..#.", ".##.."),
    "S": (".####", "#....", "#....", ".###.", "....#", "....#", "####."),
    "W": ("#...#", "#...#", "#...#", "#.#.#", "#.#.#", "##.##", "#...#"),
    "3": ("#####", "...#.", "..#..", "...#.", "....#", "#...#", ".###."),
    "4": ("...#.", "..##.", ".#.#.", "#..#.", "#####", "...#.", "...#."),
}.items()})


def text(img, s, x, y, col, scale=2):
    """Draws `s` with its top-left at pixel (x, y) (with a dark outline for contrast)."""
    for pass_, c in ((0, (15, 15, 15)), (1, col)):
        cx = x
        for ch in s.upper():
            g = FONT.get(ch, FONT[" "])
            for r in range(7):
                for k in range(5):
                    if g[r * 5 + k] != "#":
                        continue
                    y0, x0 = y + r * scale, cx + k * scale
                    if pass_ == 0:
                        img[max(y0 - 1, 0):y0 + scale + 1, max(x0 - 1, 0):x0 + scale + 1] = c
                    else:
                        img[y0:y0 + scale, x0:x0 + scale] = c
            cx += 6 * scale


def load_zone(zone):
    d = os.path.join(vc.SRC, "terrain", f"zone{zone}")
    with open(os.path.join(d, f"zone{zone}.terrain.toml"), "rb") as f:
        desc = tomllib.load(f)
    img = bpy.data.images.load(os.path.join(d, desc["terrain"]["heightmap"]), check_existing=False)
    img.colorspace_settings.name = 'Non-Color'
    w, h = img.size
    px = np.empty(w * h * 4, np.float32)
    img.pixels.foreach_get(px)
    t = desc["terrain"]
    hm = px.reshape(h, w, 4)[::-1, :, 0].astype(np.float64)
    heights = t["height_min"] + hm * (t["height_max"] - t["height_min"])
    bpy.data.images.remove(img)
    splat = vc.read_image(os.path.join(d, t["splat"]))
    splat = np.round(splat * 255.0).astype(np.uint8)
    return desc, heights, splat


def bilinear(h, x, z):
    n = h.shape[0]
    fx, fz = np.clip(x, 0, n - 1.001), np.clip(z, 0, n - 1.001)
    i, j = int(fx), int(fz)
    tx, tz = fx - i, fz - j
    a = h[j, i] * (1 - tx) + h[j, i + 1] * tx
    b = h[j + 1, i] * (1 - tx) + h[j + 1, i + 1] * tx
    return a * (1 - tz) + b * tz


def build_scene(zone, desc, heights, splat):
    vc.reset_scene()
    R = 2048
    layers = []
    for L in desc["layer"]:
        img = vc.read_image(os.path.join(vc.SRC, *L["texture"].split("/")))[..., :3]
        px = int(L["tile_size"] * R / 512.0)
        idx = (np.arange(px) * img.shape[0] / px).astype(int)
        small = img[np.ix_(idx, idx)]
        reps = R // px + 1
        layers.append(np.tile(small, (reps, reps, 1))[:R, :R])
    up = (np.arange(R) * M / R).astype(int)
    w = splat[np.ix_(up, up)].astype(np.float32) / 255.0
    albedo = sum(layers[k] * w[..., k:k + 1] for k in range(4))
    path = os.path.join(vc.PREVIEWS, "terrain", f"_shots_albedo_{zone}.png")
    vc.write_png(path, vc.to_u8(albedo))
    step = 1
    h = heights[::step, ::step]
    n = h.shape[0]
    verts = [(-i * step, -j * step, h[j, i]) for j in range(n) for i in range(n)]
    faces = [(j * n + i, j * n + i + 1, j * n + i + n + 1, j * n + i + n) for j in range(n - 1) for i in range(n - 1)]
    me = bpy.data.meshes.new("terrain")
    me.from_pydata(verts, [], faces)
    uv = me.uv_layers.new(name="UVMap")
    vidx = np.zeros(len(me.loops), np.int64)
    me.loops.foreach_get("vertex_index", vidx)
    coords = np.zeros((len(me.loops), 2), np.float32)
    coords[:, 0] = (vidx % n) / (n - 1)
    coords[:, 1] = 1.0 - (vidx // n) / (n - 1)
    uv.data.foreach_set("uv", coords.ravel())
    me.shade_smooth()
    mat = bpy.data.materials.new("terrain")
    mat.use_nodes = True
    tex = mat.node_tree.nodes.new("ShaderNodeTexImage")
    tex.image = bpy.data.images.load(path)
    tex.image.pack()  # the temporary file goes below
    bsdf = mat.node_tree.nodes["Principled BSDF"]
    bsdf.inputs["Roughness"].default_value = 0.95
    mat.node_tree.links.new(tex.outputs["Color"], bsdf.inputs["Base Color"])
    me.materials.append(mat)
    vc.link(bpy.data.objects.new("terrain", me))
    protos, tinted = {}, {}
    for o in desc["object"]:
        pname = o["mesh"].split("/")[-1].replace(".vmesh", "")
        if pname not in protos:
            before = set(bpy.data.objects)
            bpy.ops.import_scene.gltf(filepath=os.path.join(vc.SRC, "props", f"{pname}.glb"))
            new = [ob for ob in bpy.data.objects if ob not in before and ob.type == 'MESH']
            keep = [ob for ob in new if not ob.name.startswith("lod1")]
            for ob in new:
                if ob not in keep:
                    bpy.data.objects.remove(ob)
            protos[pname] = keep[0]
            keep[0].hide_render = True
            keep[0].location = (0, 0, -1000)
        ob = protos[pname].copy()
        tint = tuple(o.get("tint", (255, 255, 255)))
        if tint != (255, 255, 255):
            key = (pname, tint)
            if key not in tinted:
                m2 = ob.data.materials[0].copy()
                bs = next(nd for nd in m2.node_tree.nodes if nd.type == 'BSDF_PRINCIPLED')
                mix = m2.node_tree.nodes.new("ShaderNodeMix")
                mix.data_type = 'RGBA'
                mix.blend_type = 'MULTIPLY'
                mix.inputs["Factor"].default_value = 1.0
                m2.node_tree.links.new(bs.inputs["Base Color"].links[0].from_socket, mix.inputs["A"])
                mix.inputs["B"].default_value = vc.hex_linear("#%02x%02x%02x" % tint) + (1.0,)
                m2.node_tree.links.new(mix.outputs["Result"], bs.inputs["Base Color"])
                d2 = ob.data.copy()
                d2.materials[0] = m2
                tinted[key] = d2
            ob.data = tinted[key]
        vc.link(ob)
        ob.hide_render = False
        ob.location = (-o["x"], -o["z"], bilinear(heights, o["x"], o["z"]) + o.get("y_offset", 0.0))
        ob.rotation_mode = 'XYZ'
        ob.rotation_euler = (0, 0, -o.get("yaw", 0.0))
        st = o.get("stretch", (1.0, 1.0, 1.0))
        sc = o.get("scale", 1.0)
        ob.scale = (sc * st[0], sc * st[2], sc * st[1])
    os.remove(path)


def place_veils(desc, heights):
    """The gate veils (effect meshes) in their gates, as the client will draw them."""
    for s in desc["spawn"]:
        if not s["name"].startswith("gate_"):
            continue
        gate = next(o for o in desc["object"] if o["mesh"].startswith("props/gate_") and
                    abs(o["x"] - s["x"]) < 0.05 and abs(o["z"] - s["z"]) < 0.05)
        look = gate["mesh"].split("gate_")[-1].replace(".vmesh", "")
        before = set(bpy.data.objects)
        bpy.ops.import_scene.gltf(filepath=os.path.join(vc.SRC, "effects", f"gate_veil_{look}.glb"))
        for ob in bpy.data.objects:
            if ob not in before and ob.type == 'MESH':
                ob.location = (-s["x"], -s["z"], bilinear(heights, s["x"], s["z"]) + gate.get("y_offset", 0.0))
                ob.rotation_mode = 'XYZ'
                ob.rotation_euler = (0, 0, -s["yaw"])
                m = ob.data.materials[0]
                m.surface_render_method = "BLENDED"
                bs = next(nd for nd in m.node_tree.nodes if nd.type == 'BSDF_PRINCIPLED')
                bs.inputs["Alpha"].default_value = 0.55


def place_monster(model, x, z, yaw, scale, heights, frame=12):
    before = set(bpy.data.objects)
    bpy.ops.import_scene.gltf(filepath=os.path.join(vc.SRC, "monsters", f"{model}.glb"))
    new = [ob for ob in bpy.data.objects if ob not in before]
    arm = next(ob for ob in new if ob.type == 'ARMATURE')
    act = bpy.data.actions.get("idle")
    if act is not None:
        arm.animation_data_create()
        arm.animation_data.action = act
        if hasattr(arm.animation_data, "action_slot") and act.slots:
            arm.animation_data.action_slot = act.slots[0]
    arm.location = (-x, -z, bilinear(heights, x, z))
    arm.rotation_mode = 'XYZ'
    arm.rotation_euler = (math.pi / 2, 0, -yaw) if abs(arm.rotation_euler[0]) > 1.0 else (0, 0, -yaw)
    arm.scale = (scale, scale, scale)
    bpy.context.scene.frame_set(frame)


def aim(cam, eye, target):
    """Engine (x, y, z) points -> Blender."""
    vc.aim_camera(cam, (-eye[0], -eye[2], eye[1]), (-target[0], -target[2], target[1]), lens=30)


def overview(zone, desc, out_path, monsters):
    src = os.path.join(vc.PREVIEWS, "terrain", f"zone{zone}_topdown.png")
    img = vc.to_u8(vc.read_image(src))[..., :3].copy()
    H, W = img.shape[:2]
    s = W / 512.0
    yy, xx = np.mgrid[0:H, 0:W]
    wx, wz = (xx + 0.5) / s, 512.0 - (yy + 0.5) / s

    def blend(mask, col, a):
        img[mask] = (img[mask] * (1 - a) + np.array(col) * a).astype(np.uint8)

    for sz in monsters["safe_zone"]:
        if sz["zone"] != zone:
            continue
        d = np.hypot(wx - sz["x"], wz - sz["z"])
        blend(d <= sz["radius"], (120, 230, 130), 0.18)
        blend(np.abs(d - sz["radius"]) < 0.9, (90, 230, 110), 0.9)
    tmpl = {m["id"]: m for m in monsters["monster"]}
    for sp in monsters["spawn"]:
        if sp["zone"] != zone:
            continue
        m = tmpl[sp["monster"]]
        d = np.hypot(wx - sp["x"], wz - sp["z"])
        blend(np.abs(d - (sp["radius"] + m["aggro_radius"])) < 0.7, (255, 150, 40), 0.85)
        blend(d <= sp["radius"], (230, 40, 40), 0.7)
    for mk in desc["spawn"]:
        px, py = int(mk["x"] * s), int((512.0 - mk["z"]) * s)
        n = mk["name"]
        col = (255, 210, 40) if n.startswith("gate") else (90, 240, 110) if n.startswith("arrive") else (70, 150, 255)
        if n.startswith("arrive"):
            d = np.hypot(xx - px, yy - py)
            img[(d > 4) & (d < 8)] = col
        else:
            img[py - 6:py + 7, px - 6:px + 7] = (15, 15, 15)
            img[py - 4:py + 5, px - 4:px + 5] = col
        # direction tick (yaw: engine forward = (sin, cos); image y down = -z)
        for k in range(10, 22):
            qx, qy = int(px + math.sin(mk["yaw"]) * k), int(py - math.cos(mk["yaw"]) * k)
            if 0 <= qx < W and 0 <= qy < H:
                img[max(qy - 1, 0):qy + 2, max(qx - 1, 0):qx + 2] = col
        if n.startswith("arrive"):  # arrival labels to the left, so they never sit on their gate's
            text(img, n, px - 12 * len(n) - 10, py - 7, col)
        else:
            text(img, n, px + 10, py - 22, col)
    # Labels for the spawn groups: template id at the first circle of each id cluster.
    for sp in monsters["spawn"]:
        if sp["zone"] != zone:
            continue
        px, py = int(sp["x"] * s), int((512.0 - sp["z"]) * s)
        text(img, str(sp["monster"]), px - 4, py - 7, (255, 235, 235), scale=1)
    text(img, f"ZONE {zone}", 12, 12, (255, 255, 255), scale=3)
    vc.write_png(out_path, img)


# Ground-level views per homeland: (file stem, eye (x, y above ground, z), target (x, y, z))
SHOTS = {
    2: [("town", (226.0, 9.0, 252.0), (258.0, 6.0, 304.0)),
        ("gate", (250.0, 3.5, 420.0), (256.0, 4.0, 440.0)),
        ("hunting", (232.0, 6.0, 190.0), (248.0, 2.0, 130.0))],
    3: [("town", (240.0, 8.0, 300.0), (258.0, 8.0, 360.0)),
        ("gate", (252.0, 4.5, 132.0), (256.0, 4.0, 100.0)),
        ("hunting", (222.0, 6.0, 222.0), (168.0, 2.0, 250.0))],
    1: [("gate_aydor", (262.0, 4.5, 190.0), (284.0, 3.0, 196.0)),
        ("gate_kargan", (255.0, 4.5, 330.0), (274.0, 3.0, 326.0))],
}
# A monster of each new kind at one of its spawn points, the camera 6 m off.
MONSTER_SHOTS = {2: (6, 8), 3: (10, 12)}


def render_zone(zone, out_dir, monsters):
    desc, heights, splat = load_zone(zone)
    overview(zone, desc, os.path.join(out_dir, f"map_zone{zone}_overview.png"), monsters)
    build_scene(zone, desc, heights, splat)
    place_veils(desc, heights)
    sky = {1: "#a9c3d6", 2: "#b9c6cf", 3: "#9aa6b2"}[zone]
    cam = vc.setup_preview((1280, 720), background=sky, sun_dir=(-0.4, 0.5, 1.0), strength=3.4)
    bpy.context.scene.world.node_tree.nodes["Background"].inputs["Strength"].default_value = 1.1
    tmpl = {m["id"]: m for m in monsters["monster"]}
    for stem, eye, tgt in SHOTS[zone]:
        e = (eye[0], bilinear(heights, eye[0], eye[2]) + eye[1], eye[2])
        t = (tgt[0], bilinear(heights, tgt[0], tgt[2]) + tgt[1], tgt[2])
        aim(cam, e, t)
        vc.render_to(os.path.join(out_dir, f"zone{zone}_{stem}.png"))
    for mid in MONSTER_SHOTS.get(zone, ()):
        sp = next(s for s in monsters["spawn"] if s["zone"] == zone and s["monster"] == mid)
        m = tmpl[mid + 1]  # the level-2 / level-4 variant of the kind
        x, z = sp["x"], sp["z"]
        place_monster(m["model"], x, z, math.radians(200.0), m["scale"], heights)
        gy = bilinear(heights, x, z)
        e = (x + 2.4, gy + 2.0, z - 5.2)
        aim(cam, e, (x, gy + 1.0, z))
        vc.render_to(os.path.join(out_dir, f"zone{zone}_monster_{m['model']}.png"))


def main():
    args = vc.script_args()
    out_dir = vc.ensure_dir(os.path.join(vc.REPO, *(args[args.index("--out") + 1] if "--out" in args else
                                                    "docs/screenshots/m13").split("/")))
    zones = [int(a) for a in args[args.index("--only") + 1].split(",")] if "--only" in args else [1, 2, 3]
    with open(os.path.join(vc.REPO, "gamedata", "monsters.toml"), "rb") as f:
        monsters = tomllib.load(f)
    for zone in zones:
        render_zone(zone, out_dir, monsters)


if __name__ == "__main__":
    main()
