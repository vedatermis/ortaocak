"""Character import-rig-export: a textured source mesh per race, rigged to the shared UAL skeleton.

Usage (headless):
  blender --background --factory-startup --python tools/blender/gen_character.py -- --race aydor [--no-preview]

Inputs:
  tools/blender/source_meshes/<race>_tripo.glb   textured mesh (one material, one base-colour texture),
                                                  no armature, any scale/orientation (fixed by RACES)
  tools/blender/third_party/quaternius_ual/       skeleton + clips (vo_ual.py)
Outputs:
  assets_src/characters/<race>.glb               skinned mesh "body" (one always-visible look), 53-bone
                                                  UAL skeleton, 26 clips; references <race>.png
  assets_src/characters/<race>.png               base-colour texture (downscaled, deterministic PNG)
  assets_src/characters/<race>.anims.toml        clip settings for assetc (loop, move_speed)
  assets_src/previews/characters/<race>_*.png

Steps: normalise the mesh (face -Y, left +X, feet on Z = 0, target height, merge doubles, drop loose
geometry, outward normals); load the UAL skeleton scaled to the race and fit its joints to the mesh
(vo_ual.fit_joints, translations only); pose a copy of it to the mesh's own arm/leg angles, bind
there with automatic (bone heat) weights, clean the weights (fingers merged into rigid hands, at most
4 influences, normalised), deform the mesh into the shared A-pose rest and skin it to the clean
skeleton. Deterministic: same inputs -> same bytes.
"""

import math
import os
import sys

import bpy
import bmesh
import numpy as np
from mathutils import Matrix, Vector

sys.dont_write_bytecode = True  # keep tools/blender free of __pycache__
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import vo_common as vc  # noqa: E402
import vo_ual  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))

# Per race: source mesh, how to stand it up, target height, texture size, skeleton fit (left-side
# joint positions measured on the normalised mesh: x left(+), y back(+)/front(-), z up), extra
# posture for the clips.
RACES = {
    "aydor": dict(
        source="aydor_tripo.glb", rot_z=-90.0, height=1.77, scale=0.967, texture=1024,
        fit=dict(shoulder=(0.200, 0.03, 1.360), elbow=(0.374, 0.02, 1.154), wrist=(0.488, -0.02, 0.956),
                 hip=(0.100, 0.0, 0.900), knee=(0.150, -0.01, 0.480), ankle=(0.190, 0.03, 0.100)),
        lean=(), torso_column=((1.10, 0.30), (1.22, 0.25)), shoulder_cap=0.35,
    ),
    "kargan": dict(
        source="kargan_tripo.glb", rot_z=-90.0, height=1.93, scale=1.055, texture=1024,
        fit=dict(shoulder=(0.350, 0.02, 1.440), elbow=(0.500, 0.0, 1.250), wrist=(0.630, -0.06, 1.000),
                 hip=(0.130, 0.0, 0.930), knee=(0.210, -0.01, 0.480), ankle=(0.235, 0.035, 0.110)),
        lean=(("spine_02", 4.0), ("neck_01", -4.0)), torso_column=((1.15, 0.43), (1.30, 0.35)), shoulder_cap=0.35,
    ),
}

FINGER_PREFIX = ("thumb_", "index_", "middle_", "ring_", "pinky_")


# ---------------------------------------------------------------------------------------------
# Source mesh

def load_source(race, R):
    path = os.path.join(HERE, "source_meshes", R["source"])
    before = set(bpy.data.objects)
    bpy.ops.import_scene.gltf(filepath=path)
    new = [o for o in bpy.data.objects if o not in before]
    meshes = [o for o in new if o.type == 'MESH']
    for o in new:
        if o.type != 'MESH':
            bpy.data.objects.remove(o, do_unlink=True)
    bpy.ops.object.select_all(action='DESELECT')
    for o in meshes:
        o.select_set(True)
    bpy.context.view_layer.objects.active = meshes[0]
    if len(meshes) > 1:
        bpy.ops.object.join()
    obj = bpy.context.view_layer.objects.active
    obj.parent = None
    bpy.ops.object.transform_apply(location=True, rotation=True, scale=True)
    me = obj.data
    me.transform(Matrix.Rotation(math.radians(R["rot_z"]), 4, 'Z'))
    V = np.array([v.co for v in me.vertices])
    me.transform(Matrix.Scale(R["height"] / float(V[:, 2].max() - V[:, 2].min()), 4))
    V = np.array([v.co for v in me.vertices])
    me.transform(Matrix.Translation(Vector((-(V[:, 0].max() + V[:, 0].min()) / 2,
                                            -(V[:, 1].max() + V[:, 1].min()) / 2, -V[:, 2].min()))))
    # Clean-up that keeps the UVs (they live on the loops).
    bm = bmesh.new()
    bm.from_mesh(me)
    bmesh.ops.remove_doubles(bm, verts=bm.verts, dist=1e-5)
    bmesh.ops.triangulate(bm, faces=bm.faces)
    bmesh.ops.dissolve_degenerate(bm, edges=bm.edges, dist=2e-4)
    # Slivers below the engine's degenerate-triangle threshold (1e-7 m^2, with margin) go.
    small = [f for f in bm.faces if f.calc_area() < 4e-7]
    bmesh.ops.delete(bm, geom=small, context='FACES')
    loose = [v for v in bm.verts if not v.link_faces]
    bmesh.ops.delete(bm, geom=loose, context='VERTS')
    bmesh.ops.recalc_face_normals(bm, faces=bm.faces)
    bm.to_mesh(me)
    bm.free()
    me.shade_smooth()
    obj.name = "body"
    me.name = "body"
    return obj


def bake_texture(obj, race, size):
    """Downscales the source base-colour texture to size^2, writes it next to the character glb and
    rebuilds the material around it (base colour texture only, white factor)."""
    src_img = None
    for m in obj.data.materials:
        for n in m.node_tree.nodes:
            if n.type == 'TEX_IMAGE' and n.image is not None:
                src_img = n.image
    img = src_img.copy()
    img.scale(size, size)
    px = np.empty(size * size * 4, np.float32)
    img.pixels.foreach_get(px)
    rgb = vc.to_u8(px.reshape(size, size, 4)[::-1, :, :3])  # Blender stores the bottom row first
    out = os.path.join(vc.SRC, "characters", f"{race}.png")
    vc.write_png(out, rgb)
    bpy.data.images.remove(img)
    tex = bpy.data.images.load(out, check_existing=False)
    tex.name = race
    mat = bpy.data.materials.new(f"{race}_skin")
    mat.use_nodes = True
    nt = mat.node_tree
    bsdf = nt.nodes.get("Principled BSDF")
    bsdf.inputs["Roughness"].default_value = 0.9
    bsdf.inputs["Metallic"].default_value = 0.0
    tn = nt.nodes.new("ShaderNodeTexImage")
    tn.image = tex
    nt.links.new(tn.outputs["Color"], bsdf.inputs["Base Color"])
    mat.use_backface_culling = True
    obj.data.materials.clear()
    obj.data.materials.append(mat)
    for p in obj.data.polygons:
        p.material_index = 0
    for m in list(bpy.data.materials):
        if m is not mat and m.users == 0:
            bpy.data.materials.remove(m)
    return out


# ---------------------------------------------------------------------------------------------
# Rigging

def pose_to(arm, bone, target_dir):
    """Rotates a pose bone about its head so it points along target_dir (armature space)."""
    pb = arm.pose.bones[bone]
    M = pb.matrix.copy()
    head = M.translation.copy()
    cur = (pb.tail - pb.head).normalized()
    R = cur.rotation_difference(Vector(target_dir).normalized()).to_matrix().to_4x4()
    pb.matrix = Matrix.Translation(head) @ R @ Matrix.Translation(-head) @ M
    bpy.context.view_layer.update()


def match_pose(arm, fit):
    """Poses `arm` like the source mesh (arm and leg angles from the fit targets)."""
    T = {k: Vector(v) for k, v in fit.items()}
    for side, sgn in (("l", 1.0), ("r", -1.0)):
        m = lambda v: Vector((sgn * v.x, v.y, v.z))  # noqa: E731
        pose_to(arm, f"upperarm_{side}", m(T["elbow"]) - m(T["shoulder"]))
        pose_to(arm, f"lowerarm_{side}", m(T["wrist"]) - m(T["elbow"]))
        pose_to(arm, f"thigh_{side}", m(T["knee"]) - m(T["hip"]))
        pose_to(arm, f"calf_{side}", m(T["ankle"]) - m(T["knee"]))


def reset_pose(arm):
    if arm.animation_data:
        arm.animation_data.action = None
    for pb in arm.pose.bones:
        pb.rotation_mode = 'QUATERNION'
        pb.rotation_quaternion = (1.0, 0.0, 0.0, 0.0)
        pb.location = (0.0, 0.0, 0.0)
        pb.scale = (1.0, 1.0, 1.0)
    bpy.context.view_layer.update()


ARM_CHAIN = ("upperarm_", "lowerarm_", "hand_")


def clean_weights(obj, R):
    """Fingers into rigid hands, root never weighted; no arm weights inside the torso/skirt column
    (half width per height band, torso_column) (cloth next to the hanging hands must not follow them); shoulder cloth/fur above
    the shoulder joint follows the upper arm by at most shoulder_cap; <= 4 influences, normalised."""
    vg = obj.vertex_groups
    names = {g.index: g.name for g in vg}
    per_vertex = []
    for v in obj.data.vertices:
        w = {}
        for g in v.groups:
            n = names[g.group]
            if g.weight <= 0.0:
                continue
            if n.startswith(FINGER_PREFIX):
                n = "hand_" + n[-1]
            if n == "root":
                n = "pelvis"
            w[n] = w.get(n, 0.0) + g.weight
        co = obj.data.vertices[len(per_vertex)].co
        column = next((hw for z_max, hw in R["torso_column"] if co.z < z_max), 0.0)
        if abs(co.x) < column:
            w = {n: x for n, x in w.items() if not n.startswith(ARM_CHAIN)}
        elif co.z > R["fit"]["shoulder"][2] - 0.02:
            ua = sum(x for n, x in w.items() if n.startswith("upperarm_"))
            total = sum(w.values())
            if total > 0 and ua / total > R["shoulder_cap"]:
                k = R["shoulder_cap"] * (total - ua) / ((1 - R["shoulder_cap"]) * ua) if total > ua else 1.0
                side = "l" if co.x > 0 else "r"
                if total <= ua:
                    w[f"clavicle_{side}"] = w.get(f"clavicle_{side}", 0.0) + ua * (1 - R["shoulder_cap"])
                    k = R["shoulder_cap"]
                w = {n: (x * k if n.startswith("upperarm_") else x) for n, x in w.items()}
        per_vertex.append(w)
    for g in list(vg):
        vg.remove(g)
    # Vertices the heat solver could not reach (separate shells): weights of the nearest weighted
    # vertex in the source pose.
    from mathutils.kdtree import KDTree
    verts = obj.data.vertices
    weighted = [i for i, w in enumerate(per_vertex) if w]
    kd = KDTree(len(weighted))
    for k, i in enumerate(weighted):
        kd.insert(verts[i].co, k)
    kd.balance()
    missing = 0
    for vi, w in enumerate(per_vertex):
        if not w:
            missing += 1
            per_vertex[vi] = dict(per_vertex[weighted[kd.find(verts[vi].co)[1]]])
    groups = {}
    for vi, w in enumerate(per_vertex):
        top = sorted(w.items(), key=lambda kv: (-kv[1], kv[0]))[:4]
        total = sum(x for _, x in top)
        for n, x in top:
            g = groups.get(n)
            if g is None:
                g = groups[n] = vg.new(name=n)
            g.add([vi], x / total, 'REPLACE')
    return missing


def rig(obj, arm, R):
    fit = R["fit"]
    """Binds obj (in the source pose) to arm (shared rest pose); returns the number of vertices the
    heat weighting left unweighted."""
    fitarm = arm.copy()
    fitarm.data = arm.data.copy()
    fitarm.animation_data_clear()
    bpy.context.scene.collection.objects.link(fitarm)
    reset_pose(fitarm)
    match_pose(fitarm, fit)
    bpy.ops.object.select_all(action='DESELECT')
    bpy.context.view_layer.objects.active = fitarm
    fitarm.select_set(True)
    bpy.ops.object.mode_set(mode='POSE')
    bpy.ops.pose.select_all(action='SELECT')
    bpy.ops.pose.armature_apply(selected=False)
    bpy.ops.object.mode_set(mode='OBJECT')
    # Bone heat weights in the source pose.
    bpy.ops.object.select_all(action='DESELECT')
    obj.select_set(True)
    fitarm.select_set(True)
    bpy.context.view_layer.objects.active = fitarm
    # Bone heat's solver tolerances are absolute, so whether it converges depends on the scale: try
    # a few and keep the first that weights at least 85% of the vertices.
    for bind_scale in (1.0, 10.0, 3.0, 30.0):
        fitarm.scale = obj.scale = (bind_scale,) * 3
        bpy.context.view_layer.update()
        bpy.ops.object.select_all(action='DESELECT')
        obj.select_set(True)
        fitarm.select_set(True)
        bpy.context.view_layer.objects.active = fitarm
        bpy.ops.object.parent_set(type='ARMATURE_AUTO')
        fitarm.scale = obj.scale = (1.0, 1.0, 1.0)
        obj.matrix_parent_inverse = Matrix.Identity(4)
        bpy.context.view_layer.update()
        weighted = sum(1 for v in obj.data.vertices if any(g.weight > 0 for g in v.groups))
        print(f"BIND scale {bind_scale}: {weighted}/{len(obj.data.vertices)} vertices weighted")
        if weighted >= 0.85 * len(obj.data.vertices):
            break
        for m in [m for m in obj.modifiers if m.type == 'ARMATURE']:
            obj.modifiers.remove(m)
        obj.vertex_groups.clear()
        obj.parent = None
    missing = clean_weights(obj, R)
    # Back to the shared rest pose: point every fitted bone along its rest direction in `arm`.
    for side in ("l", "r"):
        for b in (f"upperarm_{side}", f"lowerarm_{side}", f"thigh_{side}", f"calf_{side}"):
            rb = arm.data.bones[b]
            pose_to(fitarm, b, rb.tail_local - rb.head_local)
    bpy.ops.object.select_all(action='DESELECT')
    bpy.context.view_layer.objects.active = obj
    obj.select_set(True)
    mod = next(m for m in obj.modifiers if m.type == 'ARMATURE')
    bpy.ops.object.modifier_apply(modifier=mod.name)
    obj.parent = None
    bpy.data.objects.remove(fitarm, do_unlink=True)
    obj.parent = arm
    mod = obj.modifiers.new("Armature", 'ARMATURE')
    mod.object = arm
    return missing


# ---------------------------------------------------------------------------------------------
# Checks and previews

def slide_report(race, arm, actions, settings):
    sc = bpy.context.scene
    rest = {s: arm.data.bones[f"ball_{s}"].head_local.z for s in ("l", "r")}
    for clip in ("walk", "run"):
        act = actions[clip]
        arm.animation_data.action = act
        n = int(round(act.frame_range[1]))
        v = settings[clip][1] / vo_ual.FPS
        worst, planted = 0.0, 0
        for s in ("l", "r"):
            prev = None
            for f in range(n + 1):
                sc.frame_set(f)
                p = arm.pose.bones[f"ball_{s}"].head.copy()
                if prev is not None and abs(prev.z - rest[s]) <= 0.002 and abs(p.z - rest[s]) <= 0.002:
                    planted += 1
                    worst = max(worst, abs((p.y - prev.y) - v) + abs(p.x - prev.x))
                prev = p
        print(f"SLIDE {race} {clip}: {planted} planted frame pairs, max slide {worst * 1000:.2f} mm/frame "
              f"(move speed {settings[clip][1]} m/s)")
    reset_pose(arm)


def render_previews(race, R, arm, actions, out_dir):
    sc = bpy.context.scene
    cam = vc.setup_preview((384, 512), sun_dir=(-0.5, -1.0, 1.2))
    ground_mat = vc.material("preview_ground", "#9a9384")
    bpy.ops.mesh.primitive_plane_add(size=12, location=(0, 0, 0))
    bpy.context.active_object.data.materials.append(ground_mat)
    line_mat = vc.material("preview_line", "#5d574c")
    for k in range(-12, 13):
        bpy.ops.mesh.primitive_plane_add(size=1, location=(0, k * 0.5, 0.001))
        ln = bpy.context.active_object
        ln.scale = (6, 0.008, 1)
        ln.data.materials.append(line_mat)
    H = R["height"]
    dist = 4.6
    tgt = (0, 0, H * 0.52)
    reset_pose(arm)
    views = {"front": (0, -dist, H * 0.55), "side": (dist, 0, H * 0.55), "back": (0, dist, H * 0.6),
             "threequarter": (dist * 0.7, -dist * 0.7, H * 0.7)}
    for vname, loc in views.items():
        vc.aim_camera(cam, loc, tgt, lens=58)
        vc.render_to(os.path.join(out_dir, f"{race}_bind_{vname}.png"))
    # Head close-ups in the idle pose.
    arm.animation_data.action = actions["idle"]
    sc.frame_set(0)
    hz = (arm.matrix_world @ arm.pose.bones["Head"].head).z + 0.08 * R["scale"]
    tiles = []
    for k, (loc, lens) in enumerate((((0.0, -1.3, hz + 0.03), 85), ((0.85, -1.05, hz + 0.08), 70),
                                     ((1.3, 0.0, hz + 0.03), 85), ((0.0, 1.3, hz + 0.05), 85))):
        vc.aim_camera(cam, loc, (0, 0, hz - 0.02), lens=lens)
        tiles.append(vc.render_to(os.path.join(out_dir, f"_tmp_{race}_head{k}.png")))
    vc.compose_sheet(tiles, os.path.join(out_dir, f"{race}_head.png"), 4)
    vc.remove_files(tiles)
    sheet_views = {"": (dist, 0.0, H * 0.5), "_3q": (dist * 0.75, -dist * 0.65, H * 0.62),
                   "_front": (0.0, -dist, H * 0.55), "_back": (0.0, dist, H * 0.6)}
    views_per_clip = {"idle": ("", "_front"), "walk": ("", "_3q", "_back"), "run": ("", "_3q", "_back"),
                      "sword_attack": ("_3q",), "sit_idle": ("_3q",)}
    for clip, wanted in views_per_clip.items():
        act = actions[clip]
        arm.animation_data.action = act
        frames = int(round(act.frame_range[1]))
        for suffix, loc in sheet_views.items():
            if suffix not in wanted:
                continue
            tiles = []
            for k in range(6):
                sc.frame_set(round(k * frames / 6))
                vc.aim_camera(cam, loc, (0, 0, H * 0.48), lens=58)
                tiles.append(vc.render_to(os.path.join(out_dir, f"_tmp_{race}_{clip}{suffix}{k}.png")))
            vc.compose_sheet(tiles, os.path.join(out_dir, f"{race}_{clip}_sheet{suffix}.png"), 6)
            vc.remove_files(tiles)
    # Every clip at its middle frame.
    sc.render.resolution_x, sc.render.resolution_y = 192, 256
    tiles = []
    for name, src, loop in vo_ual.CLIPS:
        act = actions[name]
        arm.animation_data.action = act
        sc.frame_set(int(round(act.frame_range[1] * 0.5)))
        vc.aim_camera(cam, (dist * 0.75, -dist * 0.65, H * 0.62), (0, 0, H * 0.45), lens=50)
        tiles.append(vc.render_to(os.path.join(out_dir, f"_tmp_{race}_clip_{name}.png")))
    vc.compose_sheet(tiles, os.path.join(out_dir, f"{race}_clips_sheet.png"), 9)
    vc.remove_files(tiles)
    reset_pose(arm)
    sc.frame_set(0)


# ---------------------------------------------------------------------------------------------

def main():
    args = vc.script_args()
    race = args[args.index("--race") + 1] if "--race" in args else "aydor"
    preview = "--no-preview" not in args
    R = RACES[race]
    vc.reset_scene()
    arm, actions, settings = vo_ual.load_skeleton(R["scale"], lean=R["lean"], fit=R["fit"])
    reset_pose(arm)
    body = load_source(race, R)
    tex_path = bake_texture(body, race, R["texture"])
    missing = rig(body, arm, R)
    reset_pose(arm)
    out_glb = os.path.join(vc.SRC, "characters", f"{race}.glb")
    vc.export_glb(out_glb, [arm, body], textures=True)
    with open(os.path.join(vc.SRC, "characters", f"{race}.anims.toml"), "w", newline="\n") as f:
        f.write(f"# Generated by tools/blender/gen_character.py --race {race}. Clip settings for assetc.\n")
        f.write("# Clips: Quaternius Universal Animation Library (CC0), see tools/blender/vo_ual.py. move_speed =\n")
        f.write("# the clip's own planted-foot speed (the client scales playback to the real ground speed).\n")
        for name, src, loop in vo_ual.CLIPS:
            lp, sp = settings[name]
            f.write(f'\n[[clip]]\nname = "{name}"\nloop = {"true" if lp else "false"}\nmove_speed = {sp}\n')
    V = np.array([v.co for v in body.data.vertices])
    print("STATS", race, "bones", len(arm.data.bones), "tris", vc.triangle_count(body), "verts", len(V),
          "height", round(float(V[:, 2].max()), 3), "unweighted", missing, "texture", os.path.basename(tex_path))
    slide_report(race, arm, actions, settings)
    if preview:
        render_previews(race, R, arm, actions, vc.ensure_dir(os.path.join(vc.PREVIEWS, "characters")))


main()
