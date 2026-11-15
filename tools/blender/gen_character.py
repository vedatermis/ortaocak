"""Character import-rig-export: textured source meshes (three looks per race), each rigged to the
shared UAL skeleton.

Usage (headless):
  blender --background --factory-startup --python tools/blender/gen_character.py -- --look aydor_1 [--no-preview]
  blender ... gen_character.py -- --race kargan [--no-preview]     (every look of the race)

Inputs:
  tools/blender/source_meshes/<look>_tripo.glb   textured mesh (one material, one base-colour texture),
                                                  no armature, any scale/orientation (fixed by LOOKS)
  tools/blender/third_party/quaternius_ual/       skeleton + clips (vo_ual.py)
Outputs (look = <race>_<n>, n = 0, 1, 2):
  assets_src/characters/<look>.glb               skinned mesh "body" (one always-visible look), 53-bone
                                                  UAL skeleton, 26 clips; references <look>.png
  assets_src/characters/<look>.png               base-colour texture (downscaled, deterministic PNG)
  assets_src/characters/<look>.anims.toml        clip settings for assetc (loop, move_speed)
  assets_src/characters/<race>.glb / .anims.toml LEGACY: byte copies of look 0 (texture <race>_0.png) for
                                                  clients that still load the un-suffixed names; drop
                                                  them once every client loads <race>_<n>
  assets_src/previews/characters/<look>_*.png

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

# Per look (output name <race>_<n>): source mesh, how to stand it up (rot_z about Z so it faces -Y),
# target height, skeleton scale (of the 1.83 m UAL mannequin; matches the mesh's hip height because
# the leg lengths stay the library's), texture size, skeleton fit (left-side joint positions measured
# on the normalised mesh: x left(+), y back(+)/front(-), z up), extra posture for the clips, and the
# weight clean-up rules (see clean_weights):
#   torso_column  ((z_max, half_width), ...): no arm weights inside this column (tunic, skirt, mantle)
#   shoulder_cap  max upper-arm share of cloth/fur above the shoulder joint
#   regions       rigid hair/beard: vertices inside box ((x0, x1), (y0, y1), (z0, z1)) take weights
#                 interpolated over z from `chain` ((z, {bone: w}), ...) instead of the heat weights,
#                 so braids and beards ride on head/neck/chest and never swing through the body
#   back          (y_min, z_min, z_max): no arm weights behind the arms (mantle backs hanging there)
# Optional: max_tris (denser sources are decimated to this budget), bind_scales (heat solver scales
# to try), proxy_voxel (voxel size of the fallback bind proxy). Adding or replacing a look is a
# change to this table plus a --look step in build_all.ps1 and an entry in the content tests' kLooks.
# Sources may be PBR (base colour + normal + metallic-roughness): only the image feeding Base Color
# is exported (downscaled to `texture`), the others stay in the source file.
COMMON = dict(rot_z=-90.0, texture=1024, lean=(), shoulder_cap=0.35, regions=(), back=None, max_tris=13000)
KARGAN_LEAN = (("spine_02", 4.0), ("neck_01", -4.0))
LOOKS = [
    dict(COMMON, look="aydor_0", race="aydor", source="aydor_0_tripo.glb", height=1.77, scale=0.967,
         fit=dict(shoulder=(0.200, 0.03, 1.360), elbow=(0.374, 0.02, 1.154), wrist=(0.488, -0.02, 0.956),
                  hip=(0.100, 0.0, 0.900), knee=(0.150, -0.01, 0.480), ankle=(0.190, 0.03, 0.100)),
         torso_column=((1.10, 0.30), (1.22, 0.25))),
    # Woman, auburn braid over the left shoulder lying on the chest, green cloak.
    dict(COMMON, look="aydor_1", race="aydor", source="aydor_1_tripo.glb", height=1.70, scale=0.929,
         fit=dict(shoulder=(0.190, 0.02, 1.300), elbow=(0.344, 0.01, 1.100), wrist=(0.436, -0.03, 0.986),
                  hip=(0.095, 0.0, 0.865), knee=(0.145, -0.01, 0.460), ankle=(0.195, 0.04, 0.100)),
         torso_column=((1.06, 0.29), (1.17, 0.24)),
         regions=(dict(box=((0.02, 0.135), (-0.21, -0.07), (1.18, 1.52)),
                       chain=((1.50, {"Head": 0.4, "neck_01": 0.3, "spine_03": 0.3}), (1.36, {"spine_03": 1.0}))),)),
    # Grey-bearded veteran, dark red cloak, broad shoulders.
    dict(COMMON, look="aydor_2", race="aydor", source="aydor_2_tripo.glb", height=1.77, scale=0.967,
         fit=dict(shoulder=(0.235, 0.03, 1.340), elbow=(0.415, 0.0, 1.095), wrist=(0.520, -0.07, 0.910),
                  hip=(0.110, 0.0, 0.900), knee=(0.180, -0.01, 0.485), ankle=(0.220, 0.03, 0.120)),
         torso_column=((1.10, 0.33), (1.22, 0.28))),
    dict(COMMON, look="kargan_0", race="kargan", source="kargan_0_tripo.glb", height=1.93, scale=1.055,
         fit=dict(shoulder=(0.350, 0.02, 1.440), elbow=(0.500, 0.0, 1.250), wrist=(0.630, -0.06, 1.000),
                  hip=(0.130, 0.0, 0.930), knee=(0.210, -0.01, 0.480), ankle=(0.235, 0.035, 0.110)),
         lean=KARGAN_LEAN, torso_column=((1.15, 0.43), (1.30, 0.35))),
    # Woman, long braid hanging down the back, pale fur mantle.
    dict(COMMON, look="kargan_1", race="kargan", source="kargan_1_tripo.glb", height=1.86, scale=1.017,
         fit=dict(shoulder=(0.220, 0.0, 1.380), elbow=(0.385, -0.03, 1.185), wrist=(0.575, -0.10, 0.965),
                  hip=(0.120, 0.0, 0.896), knee=(0.200, -0.01, 0.460), ankle=(0.255, 0.02, 0.110)),
         lean=KARGAN_LEAN, torso_column=((1.12, 0.36), (1.26, 0.30)),
         regions=(dict(box=((-0.36, -0.08), (0.165, 0.42), (0.78, 1.64)),
                       chain=((1.62, {"Head": 0.5, "neck_01": 0.5}), (1.42, {"spine_03": 1.0}))),)),
    # Bald elder, long grey braided beard down the chest, dark fur mantle.
    dict(COMMON, look="kargan_2", race="kargan", source="kargan_2_tripo.glb", height=1.93, scale=1.055,
         fit=dict(shoulder=(0.290, 0.02, 1.450), elbow=(0.450, -0.02, 1.185), wrist=(0.610, -0.12, 0.955),
                  hip=(0.130, 0.0, 0.930), knee=(0.220, -0.01, 0.485), ankle=(0.255, 0.03, 0.110)),
         lean=KARGAN_LEAN, torso_column=((1.15, 0.42), (1.30, 0.36)),
         regions=(dict(box=((-0.15, 0.15), (-0.46, -0.275), (1.11, 1.60)),
                       chain=((1.58, {"Head": 1.0}), (1.42, {"Head": 0.5, "neck_01": 0.2, "spine_03": 0.3}),
                              (1.15, {"spine_03": 0.8, "neck_01": 0.2}))),)),
]
LOOK = {L["look"]: L for L in LOOKS}

FINGER_PREFIX = ("thumb_", "index_", "middle_", "ring_", "pinky_")


# ---------------------------------------------------------------------------------------------
# Source mesh

def load_source(R):
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
    # Dissolving can merge triangles into polygons with collinear corners; triangulate again so the
    # sliver test below sees every triangle the exporter would write.
    bmesh.ops.triangulate(bm, faces=[f for f in bm.faces if len(f.verts) > 3])
    _drop_slivers(bm)
    bmesh.ops.recalc_face_normals(bm, faces=bm.faces)
    bm.to_mesh(me)
    bm.free()
    tris = len(me.polygons)
    if R.get("max_tris") and tris > R["max_tris"]:
        # Denser sources (next mesh generation: 30-40k triangles) are reduced to the budget by edge
        # collapse (UVs are interpolated; deterministic).
        bpy.context.view_layer.objects.active = obj
        dec = obj.modifiers.new("Decimate", 'DECIMATE')
        dec.decimate_type = 'COLLAPSE'
        dec.ratio = R["max_tris"] / tris
        dec.use_collapse_triangulate = True
        bpy.ops.object.modifier_apply(modifier=dec.name)
        remove_slivers(me)
        print(f"STATS decimated {tris} -> {len(me.polygons)} triangles")
    me.shade_smooth()
    obj.name = "body"
    me.name = "body"
    return obj


def _drop_slivers(bm):
    """Slivers below the engine's degenerate-triangle threshold (1e-7 m^2, with margin) go, and the
    vertices they leave unused (vertex groups live in the bmesh deform layer and survive)."""
    small = [f for f in bm.faces if f.calc_area() < 4e-7]
    bmesh.ops.delete(bm, geom=small, context='FACES')
    loose = [v for v in bm.verts if not v.link_faces]
    bmesh.ops.delete(bm, geom=loose, context='VERTS')
    return len(small)


def remove_slivers(me):
    bm = bmesh.new()
    bm.from_mesh(me)
    n = _drop_slivers(bm)
    if n:
        bm.to_mesh(me)
        print(f"BIND rest pose: {n} sliver triangles removed")
    bm.free()


def base_color_image(obj):
    """The image that feeds the Principled BSDF's Base Color (searched upstream through any mixing
    nodes). PBR sources also carry normal / metallic-roughness / occlusion images: those are left
    alone (not exported yet; see docs/ASSET_PIPELINE.md)."""
    found = []
    for m in obj.data.materials:
        if m is None or m.node_tree is None:
            continue
        for bsdf in (n for n in m.node_tree.nodes if n.type == 'BSDF_PRINCIPLED'):
            todo = [l.from_node for l in bsdf.inputs["Base Color"].links]
            seen = set()
            while todo:
                n = todo.pop()
                if n.name in seen:
                    continue
                seen.add(n.name)
                if n.type == 'TEX_IMAGE' and n.image is not None:
                    found.append(n.image)
                    break
                for inp in n.inputs:
                    todo.extend(l.from_node for l in inp.links)
    if not found:
        raise SystemExit(f"{obj.name}: no base-colour texture linked to the Principled BSDF")
    others = sorted({i.name for i in bpy.data.images} - {found[0].name})
    if others:
        print(f"TEXTURE base colour {found[0].name} {tuple(found[0].size)}; not exported: {others}")
    return found[0]


def bake_texture(obj, look, size):
    """Downscales the source base-colour texture to size^2, writes it next to the character glb and
    rebuilds the material around it (base colour texture only, white factor)."""
    src_img = base_color_image(obj)
    img = src_img.copy()
    img.scale(size, size)
    px = np.empty(size * size * 4, np.float32)
    img.pixels.foreach_get(px)
    rgb = vc.to_u8(px.reshape(size, size, 4)[::-1, :, :3])  # Blender stores the bottom row first
    out = os.path.join(vc.SRC, "characters", f"{look}.png")
    vc.write_png(out, rgb)
    bpy.data.images.remove(img)
    tex = bpy.data.images.load(out, check_existing=False)
    tex.name = look
    mat = bpy.data.materials.new(f"{look}_skin")
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
        foot = arm.pose.bones[f"foot_{side}"].matrix.to_3x3()
        pose_to(arm, f"thigh_{side}", m(T["knee"]) - m(T["hip"]))
        pose_to(arm, f"calf_{side}", m(T["ankle"]) - m(T["knee"]))
        level_foot(arm, side, foot)


def level_foot(arm, side, rot3):
    """Gives the foot bone the world rotation rot3 at its current place (the soles stand flat in the
    source mesh and in the rest pose, whatever the leg angles in between)."""
    pb = arm.pose.bones[f"foot_{side}"]
    pb.matrix = Matrix.Translation(pb.matrix.translation) @ rot3.to_4x4()
    bpy.context.view_layer.update()


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


def chain_weights(chain, z):
    """Weights from a region chain ((z, {bone: w}), ... from top to bottom), linear over z, clamped."""
    if z >= chain[0][0]:
        return dict(chain[0][1])
    for (z0, w0), (z1, w1) in zip(chain, chain[1:]):
        if z >= z1:
            t = (z0 - z) / (z0 - z1)
            out = {}
            for n in sorted(set(w0) | set(w1)):
                out[n] = w0.get(n, 0.0) * (1 - t) + w1.get(n, 0.0) * t
            return {n: x for n, x in out.items() if x > 0.0}
    return dict(chain[-1][1])


def no_arms(co, R):
    """True where arm weights are not allowed: inside the torso/skirt column or behind the arms."""
    column = next((hw for z_max, hw in R["torso_column"] if co.z < z_max), 0.0)
    back = R["back"]
    return abs(co.x) < column or bool(back and co.y > back[0] and back[1] < co.z < back[2])


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
        region = next((rg for rg in R["regions"] if all(lo <= c <= hi for c, (lo, hi) in zip(co, rg["box"]))), None)
        if region is not None:
            w = chain_weights(region["chain"], co.z)
        elif no_arms(co, R):
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
            co = verts[vi].co
            if no_arms(co, R):
                # Inside the torso column (pouches, belt pieces, cloth shells next to the hands): the
                # nearest weighted vertex that has non-arm weights, arm weights dropped.
                for _, k, _ in kd.find_n(co, 64):
                    cand = {n: x for n, x in per_vertex[weighted[k]].items() if not n.startswith(ARM_CHAIN)}
                    if cand:
                        per_vertex[vi] = cand
                        break
            if not per_vertex[vi]:
                per_vertex[vi] = dict(per_vertex[weighted[kd.find(co)[1]]])
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


def heat_bind(obj, fitarm, R):
    """Parents obj to fitarm with automatic (bone heat) weights. The solver's tolerances are absolute,
    so whether it converges depends on the scale: try a few and keep the first that weights at least
    85% of the vertices. Returns that scale, or None (obj left unparented, no groups)."""
    for bind_scale in R.get("bind_scales", (1.0, 10.0, 3.0, 30.0)):
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
        print(f"BIND {obj.name} scale {bind_scale}: {weighted}/{len(obj.data.vertices)} vertices weighted")
        if weighted >= 0.85 * len(obj.data.vertices):
            return bind_scale
        for m in [m for m in obj.modifiers if m.type == 'ARMATURE']:
            obj.modifiers.remove(m)
        obj.vertex_groups.clear()
        obj.parent = None
    return None


def make_proxy(obj, voxel):
    """Watertight stand-in for weighting: voxel remesh of a copy of obj (same pose and place)."""
    proxy = obj.copy()
    proxy.data = obj.data.copy()
    proxy.name = "bind_proxy"
    bpy.context.scene.collection.objects.link(proxy)
    proxy.data.materials.clear()
    bpy.ops.object.select_all(action='DESELECT')
    bpy.context.view_layer.objects.active = proxy
    proxy.select_set(True)
    rm = proxy.modifiers.new("Remesh", 'REMESH')
    rm.mode = 'VOXEL'
    rm.voxel_size = voxel
    rm.adaptivity = 0.0
    bpy.ops.object.modifier_apply(modifier=rm.name)
    # Only the largest connected shell: a shell that no bone can "see" makes the heat system singular
    # (all weights fail); the parts dropped here take the weights of the nearest proxy surface.
    bm = bmesh.new()
    bm.from_mesh(proxy.data)
    bm.verts.ensure_lookup_table()
    comp = [-1] * len(bm.verts)
    sizes = []
    for v in bm.verts:
        if comp[v.index] >= 0:
            continue
        c = len(sizes)
        comp[v.index] = c
        stack, n = [v], 0
        while stack:
            u = stack.pop()
            n += 1
            for e in u.link_edges:
                o = e.other_vert(u)
                if comp[o.index] < 0:
                    comp[o.index] = c
                    stack.append(o)
        sizes.append(n)
    keep = max(range(len(sizes)), key=lambda c: (sizes[c], -c))
    bmesh.ops.delete(bm, geom=[v for v in bm.verts if comp[v.index] != keep], context='VERTS')
    bm.to_mesh(proxy.data)
    bm.free()
    print(f"BIND proxy: voxel {voxel} m, {len(sizes)} shells, kept {len(proxy.data.vertices)} vertices")
    return proxy


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
    used_scale = heat_bind(obj, fitarm, R)
    if used_scale is None:
        # The solver fails on some meshes at every scale (many intersecting shells): bind a watertight
        # voxel-remeshed proxy instead and transfer its weights to the mesh (nearest surface point).
        proxy = make_proxy(obj, R.get("proxy_voxel", 0.012))
        used_scale = heat_bind(proxy, fitarm, R)
        if used_scale is None:
            raise SystemExit(f"{R['look']}: bone heat weighting failed at every bind scale (proxy too)")
        used_scale = f"proxy@{used_scale}"
        for g in proxy.vertex_groups:
            obj.vertex_groups.new(name=g.name)
        bpy.ops.object.select_all(action='DESELECT')
        bpy.context.view_layer.objects.active = obj
        obj.select_set(True)
        dt = obj.modifiers.new("WeightTransfer", 'DATA_TRANSFER')
        dt.object = proxy
        dt.use_vert_data = True
        dt.data_types_verts = {'VGROUP_WEIGHTS'}
        dt.vert_mapping = 'POLYINTERP_NEAREST'
        dt.layers_vgroup_select_src = 'ALL'
        dt.layers_vgroup_select_dst = 'NAME'
        bpy.ops.object.modifier_apply(modifier=dt.name)
        bpy.data.objects.remove(proxy, do_unlink=True)
        obj.parent = fitarm
        obj.matrix_parent_inverse = Matrix.Identity(4)
        mod = obj.modifiers.new("Armature", 'ARMATURE')
        mod.object = fitarm
    missing = clean_weights(obj, R)
    # Back to the shared rest pose: point every fitted bone along its rest direction in `arm`.
    for side in ("l", "r"):
        for b in (f"upperarm_{side}", f"lowerarm_{side}", f"thigh_{side}", f"calf_{side}"):
            rb = arm.data.bones[b]
            pose_to(fitarm, b, rb.tail_local - rb.head_local)
        level_foot(fitarm, side, arm.data.bones[f"foot_{side}"].matrix_local.to_3x3())
    bpy.ops.object.select_all(action='DESELECT')
    bpy.context.view_layer.objects.active = obj
    obj.select_set(True)
    mod = next(m for m in obj.modifiers if m.type == 'ARMATURE')
    bpy.ops.object.modifier_apply(modifier=mod.name)
    remove_slivers(obj.data)  # moving into the rest pose can squash a few tiny triangles
    obj.parent = None
    bpy.data.objects.remove(fitarm, do_unlink=True)
    obj.parent = arm
    mod = obj.modifiers.new("Armature", 'ARMATURE')
    mod.object = arm
    return missing, used_scale


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

def build_look(R, preview):
    look, race = R["look"], R["race"]
    vc.reset_scene()
    arm, actions, settings = vo_ual.load_skeleton(R["scale"], lean=R["lean"], fit=R["fit"])
    reset_pose(arm)
    body = load_source(R)
    tex_path = bake_texture(body, look, R["texture"])
    missing, bind_scale = rig(body, arm, R)
    reset_pose(arm)
    chars = os.path.join(vc.SRC, "characters")
    out_glb = os.path.join(chars, f"{look}.glb")
    vc.export_glb(out_glb, [arm, body], textures=True)
    toml = os.path.join(chars, f"{look}.anims.toml")
    with open(toml, "w", newline="\n") as f:
        f.write(f"# Generated by tools/blender/gen_character.py --look {look}. Clip settings for assetc.\n")
        f.write("# Clips: Quaternius Universal Animation Library (CC0), see tools/blender/vo_ual.py. move_speed =\n")
        f.write("# the clip's own planted-foot speed (the client scales playback to the real ground speed).\n")
        for name, src, loop in vo_ual.CLIPS:
            lp, sp = settings[name]
            f.write(f'\n[[clip]]\nname = "{name}"\nloop = {"true" if lp else "false"}\nmove_speed = {sp}\n')
    if look == f"{race}_0":
        # LEGACY un-suffixed names for clients that do not load <race>_<n> yet: byte copies (the glb
        # keeps referencing <race>_0.png, so the converted meshes are identical too). Remove once
        # every client loads looks by index.
        for src, dst in ((out_glb, f"{race}.glb"), (toml, f"{race}.anims.toml")):
            with open(src, "rb") as fi, open(os.path.join(chars, dst), "wb") as fo:
                fo.write(fi.read())
    V = np.array([v.co for v in body.data.vertices])
    print("STATS", look, "bones", len(arm.data.bones), "tris", vc.triangle_count(body), "verts", len(V),
          "height", round(float(V[:, 2].max()), 3), "bottom", round(float(V[:, 2].min()), 3),
          "unweighted", missing, "bind_scale", bind_scale,
          "texture", os.path.basename(tex_path))
    slide_report(look, arm, actions, settings)
    if preview:
        render_previews(look, R, arm, actions, vc.ensure_dir(os.path.join(vc.PREVIEWS, "characters")))


def main():
    args = vc.script_args()
    preview = "--no-preview" not in args
    if "--look" in args:
        names = args[args.index("--look") + 1].split(",")
    else:
        race = args[args.index("--race") + 1] if "--race" in args else "aydor"
        names = [L["look"] for L in LOOKS if L["race"] == race]
    for name in names:
        build_look(LOOK[name], preview)


main()
