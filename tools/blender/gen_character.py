"""Character import-rig-export: semi-realistic PBR source meshes (three looks per race), each rigged
to the shared UAL skeleton plus six helper bones.

Usage (headless):
  blender --background --factory-startup --python tools/blender/gen_character.py -- --look aydor_1 [--no-preview]
  blender ... gen_character.py -- --race kargan [--no-preview]     (every look of the race)
  blender ... gen_character.py -- --look aydor_0 --debug <dir>     (+ weight / joint debug renders in <dir>)

Inputs:
  tools/blender/source_meshes/<look>_tripo.glb   one fused mesh (~38k triangles), one PBR material with
                                                  4096^2 Color / NormalGL / ORM images, no armature
  tools/blender/third_party/quaternius_ual/       skeleton + clips (vo_ual.py)
Outputs (look = <race>_<n>, n = 0, 1, 2):
  assets_src/characters/<look>.glb               skinned mesh "body", 59-bone skeleton (53 UAL + 6
                                                  helpers), 29 clips; one material (see EXPORT_PBR_MAPS)
  assets_src/characters/<look>.png               base colour, 2048^2 sRGB
  assets_src/characters/<look>_n.png             tangent-space normal map, 2048^2, OpenGL (+Y) convention
  assets_src/characters/<look>_orm.png           R occlusion, G roughness, B metallic, 2048^2 linear
  assets_src/characters/<look>.anims.toml        clip settings for assetc (loop, move_speed)
  assets_src/characters/<race>.glb / .anims.toml LEGACY: byte copies of look 0 for clients that still
                                                  load the un-suffixed names
  assets_src/previews/characters/<look>_*.png

Steps: normalise the mesh (face -Y, left +X, feet on Z = 0, target height, merge the UV-seam
splits, decimate to max_tris with the head and hands protected, keep the source's vertex normals,
which the normal map was baked against); load the UAL skeleton scaled to the look and fit its
joints and leg proportions to the mesh (vo_ual); pose a copy of it to the mesh's own arm/leg angles,
bind there with bone heat weights, clean the weights (rigid armour, cloth panels and braids on
helper bones, belts and pouches on the pelvis, see clean_weights), deform the mesh into the shared
A-pose rest and skin it. Deterministic: same inputs -> same bytes.
"""

import math
import os
import sys

import bpy
import bmesh
import numpy as np
from mathutils import Matrix, Vector
from mathutils.kdtree import KDTree

sys.dont_write_bytecode = True  # keep tools/blender free of __pycache__
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import vo_common as vc  # noqa: E402
import vo_ual  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))

# Normal and ORM maps in the exported material. The PNGs are always written next to the glb. True:
# the glb's material references all three (baseColorTexture <look>.png, normalTexture <look>_n.png,
# metallicRoughnessTexture and occlusionTexture <look>_orm.png, factors 1). The current assetc only
# reads baseColorTexture and ignores the other two (checked: no error, same .vmesh); the renderer
# branch's assetc picks them up. False: base colour only in the glb (the fallback if an assetc ever
# rejected the extra textures).
EXPORT_PBR_MAPS = True

UAL_HIP = 0.932  # hip joint height of the library mannequin at scale 1 (m)

# Per look (output name <race>_<n>): source mesh, how to stand it up (rot_z about Z so it faces -Y),
# target height, texture size, skeleton fit (left-side joint positions measured on the normalised
# mesh: x left(+), y back(+)/front(-), z up; the skeleton scale is hip height / UAL_HIP, the thigh
# takes the hip-knee distance and the calf the rest of the leg), extra posture for the clips, and
# the weight rules (see clean_weights):
#   torso_column  ((z_max, half_width), ...): no arm weights inside this column (tunic, skirt, mantle)
#   shoulder_cap  max upper-arm share of cloth/fur above the shoulder joint
#   regions       weights from a chain instead of the heat solver for vertices inside a box
#                 ((x0, x1), (y0, y1), (z0, z1)) or a capsule ((p0, p1, radius)): `chain` ((z, {bone: w}),
#                 ...) interpolated over z; `feather` (m) blends into the heat weights at the edges;
#                 `metal_max` keeps metal (ORM blue) vertices out. Cloth panels hang from the pelvis on
#                 cloth_front / cloth_back, braids and beards from the head on braid_l / braid_r /
#                 beard, the fur mantle's back on mantle (helper bones, vo_ual.HELPER_MOTION).
#   rigid         rigid pieces (pauldrons, pouches): box (+ optional bone) -> one weight set for the
#                 whole piece (its dominant bone, or a fixed blend of the top two), blended at the seam
#   helpers       rest positions (head, tail) of the helper bones, overriding default_helpers()
#   sit           ground-sit tuning (sit_z: hip height above the ground at scale 1)
# Optional: max_tris (decimation budget), bind_scales (heat solver scales to try), proxy_voxel.
# Adding or replacing a look is a change to this table plus a --look step in build_all.ps1 and an
# entry in the content tests' kLooks.
COMMON = dict(rot_z=-90.0, texture=2048, lean=(), shoulder_cap=0.35, regions=(), rigid=(), back=None,
              max_tris=30000, helpers={}, sit={})
KARGAN_LEAN = (("spine_02", 4.0), ("neck_01", -4.0))


def _panel(helper, x, y, z_top, z_bottom, drop=0.10, feather=0.07, **kw):
    """A hanging cloth panel: pelvis at the belt, the helper bone from `drop` below it."""
    return dict(box=(x, y, (z_bottom, z_top)), feather=feather,
                chain=((z_top, {"pelvis": 1.0}), (z_top - drop, {helper: 1.0})), **kw)


LOOKS = [
    # Knight: plate pauldrons, couters and vambraces over mail, greaves, blue-grey surcoat with a
    # sun emblem (front and back panels to the knee), belt pouch on his right hip.
    dict(COMMON, look="aydor_0", race="aydor", source="aydor_0_tripo.glb", height=1.82,
         fit=dict(shoulder=(0.190, 0.03, 1.450), elbow=(0.405, 0.0, 1.240), wrist=(0.530, -0.09, 1.070),
                  hip=(0.095, 0.0, 0.930), knee=(0.170, 0.0, 0.510), ankle=(0.205, 0.04, 0.100)),
         torso_column=((1.20, 0.27), (1.30, 0.22)),
         regions=(_panel("cloth_front", (-0.17, 0.17), (-0.40, -0.03), 1.12, 0.52),
                  _panel("cloth_back", (-0.17, 0.17), (0.04, 0.40), 1.12, 0.52)),
         rigid=(dict(box=((-0.27, -0.09), (-0.25, 0.02), (0.93, 1.14)), bone="pelvis"),)),
    # Woman knight: same kit, braid down her back.
    dict(COMMON, look="aydor_1", race="aydor", source="aydor_1_tripo.glb", height=1.72,
         fit=dict(shoulder=(0.168, 0.02, 1.370), elbow=(0.371, 0.0, 1.195), wrist=(0.500, -0.09, 1.010),
                  hip=(0.090, 0.0, 0.877), knee=(0.180, 0.0, 0.490), ankle=(0.215, 0.035, 0.100)),
         torso_column=((1.12, 0.25), (1.22, 0.21)),
         regions=(_panel("cloth_front", (-0.16, 0.16), (-0.40, -0.03), 1.08, 0.50),
                  _panel("cloth_back", (-0.16, 0.16), (0.04, 0.40), 1.08, 0.50),
                  # braid down the back, a little left of the spine (centre line from the mesh)
                  dict(capsule=((0.015, 0.10, 1.66), (0.078, 0.125, 1.28), 0.03), feather=0.012, metal_max=0.3,
                       chain=((1.62, {"Head": 1.0}), (1.52, {"braid_l": 1.0})))),
         rigid=(dict(box=((-0.26, -0.08), (-0.24, 0.02), (0.90, 1.12)), bone="pelvis"),),
         helpers=dict(braid_l=((0.015, 0.10, 1.64), (0.078, 0.125, 1.28)))),
    # Grey-bearded veteran: dark red surcoat, mail coif collar, plate pauldrons and greaves.
    dict(COMMON, look="aydor_2", race="aydor", source="aydor_2_tripo.glb", height=1.80,
         fit=dict(shoulder=(0.190, 0.03, 1.440), elbow=(0.415, -0.01, 1.240), wrist=(0.535, -0.10, 1.040),
                  hip=(0.100, 0.0, 0.920), knee=(0.187, 0.0, 0.510), ankle=(0.230, 0.03, 0.100)),
         torso_column=((1.20, 0.28), (1.30, 0.23)),
         regions=(_panel("cloth_front", (-0.21, 0.21), (-0.40, -0.03), 1.11, 0.52),
                  _panel("cloth_back", (-0.21, 0.21), (0.04, 0.40), 1.11, 0.52)),
         rigid=(dict(box=((-0.28, -0.09), (-0.25, 0.02), (0.93, 1.13)), bone="pelvis"),)),
    # Orc-kin warrior: scale armour, iron pauldrons, fur mantle, two braids on the chest, loincloth.
    dict(COMMON, look="kargan_0", race="kargan", source="kargan_0_tripo.glb", height=2.00,
         fit=dict(shoulder=(0.300, 0.03, 1.550), elbow=(0.475, 0.0, 1.380), wrist=(0.653, -0.09, 1.145),
                  hip=(0.120, 0.0, 1.020), knee=(0.230, -0.01, 0.580), ankle=(0.260, 0.03, 0.115)),
         lean=KARGAN_LEAN, torso_column=((1.25, 0.36), (1.42, 0.30)),
         regions=(_panel("cloth_front", (-0.13, 0.13), (-0.45, -0.17), 1.17, 0.40, drop=0.12, feather=0.03),
                  dict(box=((-0.45, 0.45), (0.08, 0.40), (1.28, 1.95)), feather=0.04,
                       chain=((1.75, {"spine_03": 0.6, "neck_01": 0.4}), (1.55, {"spine_03": 0.5, "mantle": 0.5}),
                              (1.35, {"mantle": 1.0}))),
                  # chest braids (centre lines ~2.5 cm inside their front surface)
                  dict(capsule=((0.12, -0.14, 1.72), (0.12, -0.255, 1.55), 0.035), feather=0.012,
                       chain=((1.72, {"Head": 1.0}), (1.62, {"braid_l": 1.0}))),
                  dict(capsule=((0.12, -0.255, 1.55), (0.12, -0.255, 1.34), 0.035), feather=0.012,
                       chain=((1.72, {"Head": 1.0}), (1.62, {"braid_l": 1.0}))),
                  dict(capsule=((-0.12, -0.14, 1.72), (-0.12, -0.255, 1.55), 0.035), feather=0.012,
                       chain=((1.72, {"Head": 1.0}), (1.62, {"braid_r": 1.0}))),
                  dict(capsule=((-0.12, -0.255, 1.55), (-0.12, -0.255, 1.34), 0.035), feather=0.012,
                       chain=((1.72, {"Head": 1.0}), (1.62, {"braid_r": 1.0})))),
         rigid=(dict(box=((0.24, 0.52), (-0.17, 0.17), (1.46, 1.72))),
                dict(box=((-0.52, -0.24), (-0.17, 0.17), (1.46, 1.72))),
                dict(box=((0.15, 0.33), (-0.30, 0.02), (1.00, 1.28)), bone="pelvis")),
         helpers=dict(braid_l=((0.12, -0.14, 1.72), (0.12, -0.255, 1.34)),
                      braid_r=((-0.12, -0.14, 1.72), (-0.12, -0.255, 1.34)))),
    # Orc-kin woman: scale and leather, fur collar, loincloth, long braid down her back.
    dict(COMMON, look="kargan_1", race="kargan", source="kargan_1_tripo.glb", height=1.90,
         fit=dict(shoulder=(0.230, 0.015, 1.480), elbow=(0.423, 0.0, 1.305), wrist=(0.592, -0.09, 1.082),
                  hip=(0.110, 0.0, 0.970), knee=(0.200, 0.015, 0.550), ankle=(0.254, 0.046, 0.100)),
         lean=KARGAN_LEAN, torso_column=((1.20, 0.32), (1.36, 0.26)),
         regions=(_panel("cloth_front", (-0.15, 0.15), (-0.40, -0.12), 1.12, 0.40, drop=0.12, feather=0.03),
                  # braid from the crown down the back over her right shoulder blade (centre line
                  # from the mesh, ~2.5 cm inside its outer surface)
                  dict(capsule=((-0.035, 0.10, 1.86), (-0.10, 0.175, 1.70), 0.04), feather=0.012,
                       chain=((1.84, {"Head": 1.0}), (1.72, {"braid_l": 1.0}))),
                  dict(capsule=((-0.10, 0.175, 1.70), (-0.17, 0.20, 1.50), 0.04), feather=0.012,
                       chain=((1.84, {"Head": 1.0}), (1.72, {"braid_l": 1.0}))),
                  dict(capsule=((-0.17, 0.20, 1.50), (-0.195, 0.19, 1.29), 0.04), feather=0.012,
                       chain=((1.84, {"Head": 1.0}), (1.72, {"braid_l": 1.0})))),
         rigid=(dict(box=((0.24, 0.42), (-0.14, 0.14), (1.38, 1.56))),
                dict(box=((-0.42, -0.24), (-0.14, 0.14), (1.38, 1.56))),
                dict(box=((0.10, 0.31), (-0.28, 0.02), (0.97, 1.20)), bone="pelvis")),
         helpers=dict(braid_l=((-0.04, 0.11, 1.84), (-0.19, 0.19, 1.30)))),
    # Elder smith: bald, braided grey beard, leather apron over a tunic, fur mantle, one carved
    # pauldron on the left shoulder.
    dict(COMMON, look="kargan_2", race="kargan", source="kargan_2_tripo.glb", height=1.98,
         fit=dict(shoulder=(0.300, 0.02, 1.520), elbow=(0.464, -0.01, 1.317), wrist=(0.664, -0.13, 1.085),
                  hip=(0.130, 0.0, 1.010), knee=(0.240, 0.0, 0.580), ankle=(0.288, 0.03, 0.110)),
         lean=KARGAN_LEAN, torso_column=((1.25, 0.36), (1.40, 0.30)),
         regions=(_panel("cloth_front", (-0.25, 0.28), (-0.45, -0.16), 1.09, 0.50, feather=0.04),
                  _panel("cloth_back", (-0.25, 0.25), (0.12, 0.45), 1.05, 0.50, feather=0.04),
                  dict(box=((-0.50, 0.50), (0.10, 0.40), (1.35, 1.85)), feather=0.04,
                       chain=((1.75, {"spine_03": 0.6, "neck_01": 0.4}), (1.60, {"spine_03": 0.5, "mantle": 0.5}),
                              (1.40, {"mantle": 1.0}))),
                  # braided beard lying on the apron bib (centre line ~3 cm inside its front)
                  dict(capsule=((0.0, -0.18, 1.72), (0.0, -0.29, 1.34), 0.045), feather=0.015,
                       chain=((1.74, {"Head": 1.0}), (1.62, {"beard": 1.0})))),
         rigid=(dict(box=((0.22, 0.50), (-0.18, 0.20), (1.42, 1.74))),
                dict(box=((0.18, 0.36), (-0.30, 0.0), (0.92, 1.15)), bone="pelvis")),
         helpers=dict(beard=((0.0, -0.18, 1.72), (0.0, -0.29, 1.34)))),
]
LOOK = {L["look"]: L for L in LOOKS}

FINGER_PREFIX = ("thumb_", "index_", "middle_", "ring_", "pinky_")
ARM_CHAIN = ("upperarm_", "lowerarm_", "hand_")
# Limb segments whose armour is rigid (bracers / vambraces on the forearm, greaves / boot shafts on
# the shin): (bone, t0, t1, radius at scale 1) with t along the bone in the bind pose.
RIGID_SEGMENTS = (("lowerarm", 0.15, 0.95, 0.15), ("calf", 0.12, 0.88, 0.16))
METAL = 0.5            # ORM blue above this = metal
ISLAND_MAX = 0.42      # m at scale 1: larger metal islands (mail, trims) bend with the body
ISLAND_MIN_VERTS = 25


def look_scale(R):
    return R["fit"]["hip"][2] / UAL_HIP


def default_helpers(R):
    """Rest positions (head, tail) of the helper bones, from the fit; overridden per look."""
    s = look_scale(R)
    f = R["fit"]
    hip_z, sh_z = f["hip"][2], f["shoulder"][2]
    belt = hip_z + 0.17 * s
    head_z = R["height"] - 0.17 * s
    out = {
        "cloth_front": ((0.0, -0.15 * s, belt), (0.0, -0.17 * s, belt - 0.45 * s)),
        "cloth_back": ((0.0, 0.15 * s, belt), (0.0, 0.17 * s, belt - 0.45 * s)),
        "mantle": ((0.0, 0.14 * s, sh_z + 0.05 * s), (0.0, 0.18 * s, sh_z - 0.25 * s)),
        "braid_l": ((0.07 * s, 0.06 * s, head_z), (0.07 * s, 0.10 * s, head_z - 0.25 * s)),
        "braid_r": ((-0.07 * s, 0.06 * s, head_z), (-0.07 * s, 0.10 * s, head_z - 0.25 * s)),
        "beard": ((0.0, -0.10 * s, head_z - 0.04 * s), (0.0, -0.12 * s, head_z - 0.20 * s)),
    }
    out.update(R["helpers"])
    return out


# ---------------------------------------------------------------------------------------------
# Source mesh and textures

def source_images(obj):
    """{'color', 'normal', 'orm'} images of the source's Principled BSDF (Base Color; Normal Map
    node -> Normal; Separate Color -> Roughness / Metallic)."""
    def upstream(sock, kind):
        todo = [l.from_node for l in sock.links]
        seen = set()
        while todo:
            n = todo.pop()
            if n.name in seen:
                continue
            seen.add(n.name)
            if n.type == 'TEX_IMAGE' and n.image is not None:
                return n.image
            for inp in n.inputs:
                todo.extend(l.from_node for l in inp.links)
        raise SystemExit(f"{obj.name}: no {kind} image linked to the Principled BSDF")

    mat = obj.data.materials[0]
    bsdf = next(n for n in mat.node_tree.nodes if n.type == 'BSDF_PRINCIPLED')
    return dict(color=upstream(bsdf.inputs["Base Color"], "base colour"),
                normal=upstream(bsdf.inputs["Normal"], "normal map"),
                orm=upstream(bsdf.inputs["Roughness"], "ORM"))


def image_array(img):
    """Float RGBA pixels, top row first (Blender stores the bottom row first)."""
    w, h = img.size
    px = np.empty(w * h * 4, np.float32)
    img.pixels.foreach_get(px)
    return px.reshape(h, w, 4)[::-1]


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
    src_tris, src_verts = len(me.polygons), len(me.vertices)
    # The source's vertex normals (smooth, no hard edges; they differ from Blender's own by ~10 deg
    # on average and the normal map was baked against them): kept as a vertex attribute and
    # restored as custom normals after rigging.
    cn = np.empty(len(me.loops) * 3, np.float32)
    me.corner_normals.foreach_get("vector", cn)
    cv = np.empty(len(me.loops), np.int64)
    me.loops.foreach_get("vertex_index", cv)
    vn = np.zeros((len(me.vertices), 3))
    np.add.at(vn, cv, cn.reshape(-1, 3))
    rot = Matrix.Rotation(math.radians(R["rot_z"]), 4, 'Z')
    vn = vn @ np.array(rot.to_3x3()).T
    vn /= np.maximum(np.linalg.norm(vn, axis=1, keepdims=True), 1e-12)
    attr = me.attributes.new("src_n", 'FLOAT_VECTOR', 'POINT')
    attr.data.foreach_set("vector", vn.astype(np.float32).ravel())
    if me.attributes.get("custom_normal"):
        me.attributes.remove(me.attributes["custom_normal"])
    me.transform(rot)
    V = np.array([v.co for v in me.vertices])
    me.transform(Matrix.Scale(R["height"] / float(V[:, 2].max() - V[:, 2].min()), 4))
    V = np.array([v.co for v in me.vertices])
    me.transform(Matrix.Translation(Vector((-(V[:, 0].max() + V[:, 0].min()) / 2,
                                            -(V[:, 1].max() + V[:, 1].min()) / 2, -V[:, 2].min()))))
    # Clean-up that keeps the UVs (they live on the loops): the UV-seam splits are merged (one
    # fused shell), degenerate bits dissolved.
    bm = bmesh.new()
    bm.from_mesh(me)
    bmesh.ops.remove_doubles(bm, verts=bm.verts, dist=1e-5)
    bmesh.ops.triangulate(bm, faces=bm.faces)
    bmesh.ops.dissolve_degenerate(bm, edges=bm.edges, dist=2e-4)
    bmesh.ops.triangulate(bm, faces=[f for f in bm.faces if len(f.verts) > 3])
    _drop_slivers(bm)
    bmesh.ops.recalc_face_normals(bm, faces=bm.faces)
    bm.to_mesh(me)
    bm.free()
    merged_verts = len(me.vertices)
    tris = len(me.polygons)
    if R.get("max_tris") and tris > R["max_tris"]:
        # Edge-collapse decimation (quadric error; UVs and the normal attribute are interpolated,
        # seams kept by the collapse's loop-attribute check) with the head and hands protected.
        protect = obj.vertex_groups.new(name="protect")
        f = R["fit"]
        neck_z = f["shoulder"][2] + 0.10 * look_scale(R)
        el, wr = Vector(f["elbow"]), Vector(f["wrist"])
        fa = (wr - el).normalized()
        keep = []
        for v in me.vertices:
            c = v.co
            m = Vector((abs(c.x), c.y, c.z))
            if c.z > neck_z or (m - wr).dot(fa) > -0.02:
                keep.append(v.index)
        protect.add(keep, 1.0, 'REPLACE')
        bpy.context.view_layer.objects.active = obj
        dec = obj.modifiers.new("Decimate", 'DECIMATE')
        dec.decimate_type = 'COLLAPSE'
        dec.ratio = R["max_tris"] / tris
        dec.use_collapse_triangulate = True
        dec.vertex_group = protect.name
        dec.invert_vertex_group = True
        dec.vertex_group_factor = 10.0
        bpy.ops.object.modifier_apply(modifier=dec.name)
        obj.vertex_groups.remove(obj.vertex_groups["protect"])
        remove_slivers(me)
        print(f"STATS decimated {tris} -> {len(me.polygons)} triangles ({len(keep)} protected vertices)")
    me.shade_smooth()
    obj.name = "body"
    me.name = "body"
    print(f"STATS source {R['source']}: {src_tris} triangles, {src_verts} vertices ({merged_verts} after merging "
          f"UV-seam splits) -> {len(me.polygons)} triangles, {len(me.vertices)} vertices")
    return obj, (src_tris, src_verts)


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


def vertex_metal(obj, orm):
    """Per-vertex metalness (ORM blue at the vertex's UVs, averaged over its corners)."""
    me = obj.data
    a = image_array(orm)
    h, w = a.shape[:2]
    uv = np.empty(len(me.loops) * 2, np.float32)
    me.uv_layers.active.data.foreach_get("uv", uv)
    uv = uv.reshape(-1, 2)
    cv = np.empty(len(me.loops), np.int64)
    me.loops.foreach_get("vertex_index", cv)
    xi = np.clip(((uv[:, 0] % 1.0) * w).astype(np.int64), 0, w - 1)
    yi = np.clip(((1.0 - uv[:, 1] % 1.0) * h).astype(np.int64), 0, h - 1)
    m = np.zeros(len(me.vertices))
    c = np.zeros(len(me.vertices))
    np.add.at(m, cv, a[yi, xi, 2])
    np.add.at(c, cv, 1.0)
    return m / np.maximum(c, 1.0)


def _srgb_to_linear(c):
    return np.where(c <= 0.04045, c / 12.92, ((c + 0.055) / 1.055) ** 2.4)


def _linear_to_srgb(c):
    c = np.clip(c, 0.0, 1.0)
    return np.where(c <= 0.0031308, c * 12.92, 1.055 * np.power(c, 1.0 / 2.4) - 0.055)


def _box_down(a, size):
    """Area average down to size x size (integer factor)."""
    h, w = a.shape[:2]
    k = h // size
    return a.reshape(size, k, size, k, a.shape[2]).mean(axis=(1, 3))


def bake_textures(obj, look, size):
    """Downscales the source's three 4096^2 maps to size^2 (base colour averaged in linear light,
    normals averaged as vectors and renormalised, ORM plain average), writes them next to the
    character glb and rebuilds the material around them. Returns (paths, per-vertex metalness)."""
    imgs = source_images(obj)
    for key, img in imgs.items():
        if tuple(img.size) != tuple(imgs["color"].size):
            raise SystemExit(f"{look}: source {key} map is {tuple(img.size)}, base colour {tuple(imgs['color'].size)}")
    metal = vertex_metal(obj, imgs["orm"])
    chars = os.path.join(vc.SRC, "characters")
    paths = dict(color=os.path.join(chars, f"{look}.png"), normal=os.path.join(chars, f"{look}_n.png"),
                 orm=os.path.join(chars, f"{look}_orm.png"))
    a = image_array(imgs["color"])[:, :, :3]
    rgb = _linear_to_srgb(_box_down(_srgb_to_linear(a.astype(np.float64)), size))
    vc.write_png(paths["color"], vc.to_u8(rgb), optimize=True)
    a = image_array(imgs["normal"])[:, :, :3].astype(np.float64) * 2.0 - 1.0
    n = _box_down(a, size)
    n /= np.maximum(np.linalg.norm(n, axis=2, keepdims=True), 1e-8)
    vc.write_png(paths["normal"], vc.to_u8(n * 0.5 + 0.5), optimize=True)
    a = image_array(imgs["orm"])[:, :, :3].astype(np.float64)
    vc.write_png(paths["orm"], vc.to_u8(_box_down(a, size)), optimize=True)
    del a, n, rgb
    for img in list(bpy.data.images):
        bpy.data.images.remove(img)
    mat = bpy.data.materials.new(f"{look}_skin")
    mat.use_nodes = True
    nt = mat.node_tree
    bsdf = nt.nodes.get("Principled BSDF")

    def tex(path, name, color):
        img = bpy.data.images.load(path, check_existing=False)
        img.name = name
        img.colorspace_settings.name = 'sRGB' if color else 'Non-Color'
        node = nt.nodes.new("ShaderNodeTexImage")
        node.image = img
        return node

    base = tex(paths["color"], look, True)
    nt.links.new(base.outputs["Color"], bsdf.inputs["Base Color"])
    if EXPORT_PBR_MAPS:
        nm = tex(paths["normal"], f"{look}_n", False)
        nmap = nt.nodes.new("ShaderNodeNormalMap")
        nmap.space = 'TANGENT'
        nmap.inputs["Strength"].default_value = 1.0
        nt.links.new(nm.outputs["Color"], nmap.inputs["Color"])
        nt.links.new(nmap.outputs["Normal"], bsdf.inputs["Normal"])
        orm = tex(paths["orm"], f"{look}_orm", False)
        sep = nt.nodes.new("ShaderNodeSeparateColor")
        nt.links.new(orm.outputs["Color"], sep.inputs["Color"])
        nt.links.new(sep.outputs["Green"], bsdf.inputs["Roughness"])
        nt.links.new(sep.outputs["Blue"], bsdf.inputs["Metallic"])
        # Occlusion: the glTF exporter reads it from a "glTF Material Output" group node.
        grp = bpy.data.node_groups.get("glTF Material Output")
        if grp is None:
            grp = bpy.data.node_groups.new("glTF Material Output", "ShaderNodeTree")
            grp.interface.new_socket("Occlusion", in_out='INPUT', socket_type='NodeSocketFloat')
        gn = nt.nodes.new("ShaderNodeGroup")
        gn.node_tree = grp
        nt.links.new(sep.outputs["Red"], gn.inputs["Occlusion"])
    else:
        bsdf.inputs["Roughness"].default_value = 0.9
        bsdf.inputs["Metallic"].default_value = 0.0
    mat.use_backface_culling = True
    obj.data.materials.clear()
    obj.data.materials.append(mat)
    for p in obj.data.polygons:
        p.material_index = 0
    for m in list(bpy.data.materials):
        if m is not mat and m.users == 0:
            bpy.data.materials.remove(m)
    return paths, metal


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


def region_factor(rg, co):
    """1 inside the region, 0 outside, linear over `feather` at the edges."""
    fe = rg.get("feather", 0.0)
    if "box" in rg:
        d = min(min(c - lo, hi - c) for c, (lo, hi) in zip(co, rg["box"]))
    else:
        p0, p1, r = rg["capsule"]
        a, b = Vector(p0), Vector(p1)
        ab = b - a
        t = max(0.0, min(1.0, (co - a).dot(ab) / ab.length_squared))
        d = r - (co - (a + ab * t)).length
    if d <= 0.0:
        return 0.0
    return 1.0 if fe <= 0.0 or d >= fe else d / fe


def no_arms(co, R):
    """True where arm weights are not allowed: inside the torso/skirt column or behind the arms."""
    column = next((hw for z_max, hw in R["torso_column"] if co.z < z_max), 0.0)
    back = R["back"]
    return abs(co.x) < column or bool(back and co.y > back[0] and back[1] < co.z < back[2])


def _mix(a, b, t):
    """(1 - t) * a + t * b for weight dicts."""
    out = {}
    for n in set(a) | set(b):
        x = a.get(n, 0.0) * (1.0 - t) + b.get(n, 0.0) * t
        if x > 1e-6:
            out[n] = x
    return out


def _norm(w):
    s = sum(w.values())
    return {n: x / s for n, x in w.items()} if s > 0 else w


def _neighbours(me):
    nb = [[] for _ in me.vertices]
    for e in me.edges:
        a, b = e.vertices
        nb[a].append(b)
        nb[b].append(a)
    return nb


def clean_weights(obj, R, fitarm, metal):
    """Weight clean-up after the heat bind (mesh and fitarm in the source pose), in this order:
    1. fingers into rigid hands, root never weighted; vertices the solver missed take the weights of
       the nearest weighted vertex (counted and reported);
    2. no arm weights inside the torso/skirt column; shoulder cloth/fur follows the upper arm by at
       most shoulder_cap;
    3. above the hips (belt, pouches, buckles) thigh weights move to the pelvis;
    4. forearm and shin armour (RIGID_SEGMENTS) rigid on lowerarm / calf, blended at the joints;
    5. regions: cloth panels, mantle, braids and beards from their chains (helper bones);
    6. rigid pieces: metal islands (ORM metallic, eroded so thin trims and mail drop out, at most
       ISLAND_MAX across) and the look's rigid boxes take one weight set for the whole piece (the
       dominant bone if it has >= 60 %, else a fixed blend of the top two), the two vertex rings
       around each piece blend into it;
    7. at most 4 influences, normalised.
    Returns (unreached vertex count, rigid piece summary)."""
    s = look_scale(R)
    me = obj.data
    vg = obj.vertex_groups
    names = {g.index: g.name for g in vg}
    verts = me.vertices
    co = [v.co.copy() for v in verts]
    W = []
    for v in verts:
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
        W.append(w)
    for g in list(vg):
        vg.remove(g)
    # 1. unreached vertices
    weighted = [i for i, w in enumerate(W) if w]
    kd = KDTree(len(weighted))
    for k, i in enumerate(weighted):
        kd.insert(co[i], k)
    kd.balance()
    missing = 0
    for vi, w in enumerate(W):
        if w:
            continue
        missing += 1
        if no_arms(co[vi], R):
            for _, k, _ in kd.find_n(co[vi], 64):
                cand = {n: x for n, x in W[weighted[k]].items() if not n.startswith(ARM_CHAIN)}
                if cand:
                    W[vi] = dict(cand)
                    break
        if not W[vi]:
            W[vi] = dict(W[weighted[kd.find(co[vi])[1]]])
    # 2. torso column, shoulder cap
    sh_z = R["fit"]["shoulder"][2]
    for vi, w in enumerate(W):
        c = co[vi]
        if no_arms(c, R):
            nw = {n: x for n, x in w.items() if not n.startswith(ARM_CHAIN)}
            W[vi] = nw if nw else {"spine_03" if c.z > R["fit"]["hip"][2] + 0.25 * s else "pelvis": 1.0}
        elif c.z > sh_z - 0.02:
            ua = sum(x for n, x in w.items() if n.startswith("upperarm_"))
            total = sum(w.values())
            cap = R["shoulder_cap"]
            if total > 0 and ua / total > cap:
                side = "l" if c.x > 0 else "r"
                if total <= ua:
                    w = {f"clavicle_{side}": ua * (1 - cap), **{n: x * cap for n, x in w.items()}}
                else:
                    k = cap * (total - ua) / ((1 - cap) * ua)
                    w = {n: (x * k if n.startswith("upperarm_") else x) for n, x in w.items()}
                W[vi] = w
    # 3. belt line: no thigh weights above the hips
    hip_x, hip_z = R["fit"]["hip"][0], R["fit"]["hip"][2]
    for vi, w in enumerate(W):
        c = co[vi]
        if abs(c.x) > hip_x + 0.16 * s:
            continue
        t = (c.z - (hip_z + 0.02 * s)) / (0.08 * s)
        if t <= 0.0:
            continue
        t = min(1.0, t)
        th = sum(x for n, x in w.items() if n.startswith("thigh_"))
        if th > 0.0:
            w = {n: (x * (1 - t) if n.startswith("thigh_") else x) for n, x in w.items()}
            w["pelvis"] = w.get("pelvis", 0.0) + th * t
            W[vi] = w
    # 4. rigid forearm / shin armour
    segs = []
    for bone, t0, t1, rad in RIGID_SEGMENTS:
        for side in ("l", "r"):
            pb = fitarm.pose.bones[f"{bone}_{side}"]
            segs.append((f"{bone}_{side}", pb.head.copy(), pb.tail.copy(), t0, t1, rad * s))
    for vi, w in enumerate(W):
        c = co[vi]
        top = max(w.items(), key=lambda kv: (kv[1], kv[0]))[0]
        for bone, a, b, t0, t1, rad in segs:
            ab = b - a
            t = (c - a).dot(ab) / ab.length_squared
            if t < t0 - 0.06 or t > t1 + 0.06:
                continue
            if (c - (a + ab * t)).length > rad:
                continue
            side = bone[-1]
            chain = (f"upperarm_{side}", f"lowerarm_{side}", f"hand_{side}") if bone.startswith("lowerarm") else \
                (f"thigh_{side}", f"calf_{side}", f"foot_{side}")
            if top not in chain:
                continue
            f = min(1.0, max(0.0, (t - (t0 - 0.06)) / 0.06)) * min(1.0, max(0.0, ((t1 + 0.06) - t) / 0.06))
            W[vi] = _mix(w, {bone: 1.0}, f)
            break
    # 5. regions
    for vi in range(len(W)):
        c = co[vi]
        for rg in R["regions"]:
            if rg.get("metal_max") is not None and metal[vi] > rg["metal_max"]:
                continue
            f = region_factor(rg, c)
            if f > 0.0:
                W[vi] = _mix(W[vi], chain_weights(rg["chain"], c.z), f)
                break
    # 6. rigid pieces
    nb = _neighbours(me)
    nv = len(W)
    is_metal = metal > METAL
    core = is_metal.copy()
    for _ in range(2):  # erosion: thin trims and studs vanish
        core = np.array([core[i] and all(core[j] for j in nb[i]) for i in range(nv)])
    island = np.full(nv, -1)
    pieces = []
    for i in range(nv):
        if not core[i] or island[i] >= 0:
            continue
        k = len(pieces)
        island[i] = k
        stack, members = [i], []
        while stack:
            u = stack.pop()
            members.append(u)
            for j in nb[u]:
                if core[j] and island[j] < 0:
                    island[j] = k
                    stack.append(j)
        pieces.append(members)
    # regrow two rings into the metal around each core (breadth first, lowest piece wins ties)
    frontier = [i for i in range(nv) if island[i] >= 0]
    for _ in range(2):
        nxt = []
        for u in frontier:
            for j in nb[u]:
                if is_metal[j] and island[j] < 0:
                    island[j] = island[u]
                    pieces[island[u]].append(j)
                    nxt.append(j)
        frontier = nxt
    rigid = []
    for k, members in enumerate(pieces):
        P = np.array([tuple(co[i]) for i in members])
        ext = float(np.linalg.norm(P.max(0) - P.min(0)))
        if len(members) >= ISLAND_MIN_VERTS and ext <= ISLAND_MAX * s:
            rigid.append(("metal", sorted(members), None))
    for rb in R["rigid"]:
        members = [i for i in range(nv) if all(lo <= x <= hi for x, (lo, hi) in zip(co[i], rb["box"]))]
        if members:
            rigid.append(("box", members, rb.get("bone")))
    # Metal on the thighs is mail skirt or surcoat trim: it has to bend with the hips, not ride on
    # one thigh.
    keep = []
    for kind, members, bone in rigid:
        if kind == "metal":
            acc = {}
            for i in members:
                for n, x in _norm(W[i]).items():
                    acc[n] = acc.get(n, 0.0) + x
            top2 = [n for n, _ in sorted(acc.items(), key=lambda kv: (-kv[1], kv[0]))[:2]]
            if any(n.startswith("thigh_") for n in top2):
                continue
        keep.append((kind, members, bone))
    rigid = keep
    owner = np.full(nv, -1)
    piece_w = []
    summary = []
    for k, (kind, members, bone) in enumerate(rigid):
        if bone:
            pw = {bone: 1.0}
        else:
            acc = {}
            for i in members:
                for n, x in _norm(W[i]).items():
                    acc[n] = acc.get(n, 0.0) + x
            ctr = Vector(np.array([tuple(co[i]) for i in members]).mean(0))
            if abs(ctr.x) < 0.08 * s:
                # Centred pieces (gorget, buckle, emblem) stay symmetric: no left/right bones.
                central = {n: x for n, x in acc.items() if not n.endswith(("_l", "_r"))}
                acc = central or acc
            top = sorted(acc.items(), key=lambda kv: (-kv[1], kv[0]))
            tot = sum(acc.values())
            side = "l" if ctr.x > 0 else "r"
            if top[0][0] in (f"clavicle_{side}", f"upperarm_{side}") and ctr.z > sh_z - 0.15 * s \
                    and abs(ctr.x) > R["fit"]["shoulder"][0] - 0.06 * s:
                # Pauldrons: half shoulder girdle, half upper arm on both sides alike, so a raised
                # arm lifts the plate instead of sliding under it.
                pw = {f"clavicle_{side}": 0.5, f"upperarm_{side}": 0.5}
            elif top[0][1] >= 0.6 * tot or len(top) == 1:
                pw = {top[0][0]: 1.0}
            else:
                pw = _norm(dict(top[:2]))
        piece_w.append(pw)
        for i in members:
            W[i] = dict(pw)
            owner[i] = k
        P = np.array([tuple(co[i]) for i in members])
        summary.append((kind, len(members), tuple(np.round(P.mean(0), 2)), {n: round(x, 2) for n, x in pw.items()}))
    # seams: the two rings around each piece blend into it
    ring = [i for i in range(nv) if owner[i] >= 0]
    seen = owner >= 0
    for share in (0.5, 0.25):
        nxt = []
        for u in ring:
            for j in nb[u]:
                if not seen[j]:
                    seen[j] = True
                    W[j] = _mix(W[j], piece_w[owner[u]], share)
                    owner[j] = owner[u]
                    nxt.append(j)
        ring = nxt
    # 7. top 4, normalised
    groups = {}
    final = []
    for vi, w in enumerate(W):
        top = sorted(w.items(), key=lambda kv: (-kv[1], kv[0]))[:4]
        total = sum(x for _, x in top)
        top = [(n, x / total) for n, x in top]
        final.append(dict(top))
        for n, x in top:
            g = groups.get(n)
            if g is None:
                g = groups[n] = vg.new(name=n)
            g.add([vi], x, 'REPLACE')
    return missing, summary, final


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
    print(f"BIND proxy: voxel {voxel} m, {len(proxy.data.vertices)} vertices")
    return proxy


def rig(obj, arm, R, metal):
    """Binds obj (in the source pose) to arm (shared rest pose); returns (unreached vertices, bind
    scale, rigid piece summary, final weights)."""
    fit = R["fit"]
    fitarm = arm.copy()
    fitarm.data = arm.data.copy()
    fitarm.animation_data_clear()
    bpy.context.scene.collection.objects.link(fitarm)
    for name in vo_ual.HELPERS:  # helper bones get weights only from the look's regions
        fitarm.data.bones[name].use_deform = False
    reset_pose(fitarm)
    match_pose(fitarm, fit)
    bpy.ops.object.select_all(action='DESELECT')
    bpy.context.view_layer.objects.active = fitarm
    fitarm.select_set(True)
    bpy.ops.object.mode_set(mode='POSE')
    bpy.ops.pose.select_all(action='SELECT')
    bpy.ops.pose.armature_apply(selected=False)
    bpy.ops.object.mode_set(mode='OBJECT')
    used_scale = heat_bind(obj, fitarm, R)
    if used_scale is None:
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
    missing, summary, weights = clean_weights(obj, R, fitarm, metal)
    for name in vo_ual.HELPERS:
        fitarm.data.bones[name].use_deform = True
    # Back to the shared rest pose: point every fitted bone along its rest direction in `arm`.
    for side in ("l", "r"):
        for b in (f"upperarm_{side}", f"lowerarm_{side}", f"thigh_{side}", f"calf_{side}"):
            rb = arm.data.bones[b]
            pose_to(fitarm, b, rb.tail_local - rb.head_local)
        level_foot(fitarm, side, arm.data.bones[f"foot_{side}"].matrix_local.to_3x3())
    # The source normals follow the same deformation (linear blend of the bone rotations).
    rots = {pb.name: np.array((pb.matrix @ fitarm.data.bones[pb.name].matrix_local.inverted()).to_3x3())
            for pb in fitarm.pose.bones}
    me = obj.data
    src_n = np.empty(len(me.vertices) * 3, np.float32)
    me.attributes["src_n"].data.foreach_get("vector", src_n)
    src_n = src_n.reshape(-1, 3).astype(np.float64)
    for vi, w in enumerate(weights):
        m = sum(rots[n] * x for n, x in w.items())
        src_n[vi] = m @ src_n[vi]
    src_n /= np.maximum(np.linalg.norm(src_n, axis=1, keepdims=True), 1e-12)
    me.attributes["src_n"].data.foreach_set("vector", src_n.astype(np.float32).ravel())
    bpy.ops.object.select_all(action='DESELECT')
    bpy.context.view_layer.objects.active = obj
    obj.select_set(True)
    mod = next(m for m in obj.modifiers if m.type == 'ARMATURE')
    bpy.ops.object.modifier_apply(modifier=mod.name)
    remove_slivers(me)  # moving into the rest pose can squash a few tiny triangles
    n = np.empty(len(me.vertices) * 3, np.float32)
    me.attributes["src_n"].data.foreach_get("vector", n)
    me.normals_split_custom_set_from_vertices(n.reshape(-1, 3).tolist())
    me.attributes.remove(me.attributes["src_n"])
    obj.parent = None
    bpy.data.objects.remove(fitarm, do_unlink=True)
    obj.parent = arm
    mod = obj.modifiers.new("Armature", 'ARMATURE')
    mod.object = arm
    return missing, used_scale, summary


# ---------------------------------------------------------------------------------------------
# Checks and previews

def slide_report(race, arm, actions, settings):
    sc = bpy.context.scene
    rest = {s: arm.data.bones[f"ball_{s}"].head_local.z for s in ("l", "r")}
    for clip in ("walk", "run"):
        act = actions[clip]
        vo_ual.use_action(arm, act)
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


def setup_studio(resolution):
    """Neutral studio: mid-grey world, warm key, cool fill and a rim light (EEVEE)."""
    cam = vc.setup_preview(resolution, background="#7d7f82", sun_dir=(-0.6, -1.0, 1.1), strength=2.6)
    sc = bpy.context.scene
    # Deterministic renders of glossy metal: no screen-space ray tracing (its GPU noise differed
    # between runs), fixed sample count, hard-edged sun shadows.
    sc.eevee.use_raytracing = False
    sc.eevee.taa_render_samples = 16
    sc.world.node_tree.nodes["Background"].inputs["Strength"].default_value = 0.55
    for name, direction, energy, col in (("Fill", (1.0, -0.6, 0.4), 0.9, (0.85, 0.9, 1.0)),
                                         ("Rim", (0.2, 1.0, 0.8), 2.0, (1.0, 1.0, 1.0))):
        lt = bpy.data.lights.new(name, 'SUN')
        lt.energy = energy
        lt.color = col
        lt.angle = math.radians(10)
        lo = vc.link(bpy.data.objects.new(name, lt))
        lo.rotation_euler = Vector(direction).normalized().to_track_quat('Z', 'Y').to_euler()
    return cam


def render_previews(look, R, arm, actions, out_dir):
    # This look's previews are regenerated from scratch (no stale sheets of dropped views/clips).
    vc.remove_files(sorted(os.path.join(out_dir, f) for f in os.listdir(out_dir) if f.startswith(f"{look}_")))
    sc = bpy.context.scene
    cam = setup_studio((384, 512))
    ground_mat = vc.material("preview_ground", "#77746d")
    bpy.ops.mesh.primitive_plane_add(size=12, location=(0, 0, 0))
    bpy.context.active_object.data.materials.append(ground_mat)
    line_mat = vc.material("preview_line", "#55524b")
    for k in range(-12, 13):
        bpy.ops.mesh.primitive_plane_add(size=1, location=(0, k * 0.5, 0.001))
        ln = bpy.context.active_object
        ln.scale = (6, 0.008, 1)
        ln.data.materials.append(line_mat)
    H = R["height"]
    dist = 4.8
    tgt = (0, 0, H * 0.52)
    reset_pose(arm)
    views = {"front": (0, -dist, H * 0.55), "side": (dist, 0, H * 0.55), "back": (0, dist, H * 0.6),
             "threequarter": (dist * 0.7, -dist * 0.7, H * 0.7)}
    for vname, loc in views.items():
        vc.aim_camera(cam, loc, tgt, lens=58)
        vc.render_to(os.path.join(out_dir, f"{look}_bind_{vname}.png"))
    # Head close-ups in the idle pose.
    vo_ual.use_action(arm, actions["idle"])
    sc.frame_set(0)
    hz = (arm.matrix_world @ arm.pose.bones["Head"].head).z + 0.08 * look_scale(R)
    tiles = []
    for k, (loc, lens) in enumerate((((0.0, -1.3, hz + 0.03), 85), ((0.85, -1.05, hz + 0.08), 70),
                                     ((1.3, 0.0, hz + 0.03), 85), ((0.0, 1.3, hz + 0.05), 85))):
        vc.aim_camera(cam, loc, (0, 0, hz - 0.02), lens=lens)
        tiles.append(vc.render_to(os.path.join(out_dir, f"_tmp_{look}_head{k}.png")))
    vc.compose_sheet(tiles, os.path.join(out_dir, f"{look}_head.png"), 4)
    vc.remove_files(tiles)
    # Contact sheets (6 frames over the clip): side and three-quarter.
    sc.render.resolution_x, sc.render.resolution_y = 256, 340
    sheet_views = {"": (dist, 0.0, H * 0.5), "_3q": (dist * 0.75, -dist * 0.65, H * 0.62)}
    for clip in ("walk", "run", "sword_attack", "death", "roll", "sit_ground_enter", "sit_ground_idle"):
        act = actions[clip]
        vo_ual.use_action(arm, act)
        frames = int(round(act.frame_range[1]))
        for suffix, loc in sheet_views.items():
            tiles = []
            for k in range(6):
                sc.frame_set(round(k * frames / 6) if clip != "sit_ground_enter" else round(k * frames / 5))
                vc.aim_camera(cam, loc, (0, 0, H * 0.45), lens=55)
                tiles.append(vc.render_to(os.path.join(out_dir, f"_tmp_{look}_{clip}{suffix}{k}.png")))
            vc.compose_sheet(tiles, os.path.join(out_dir, f"{look}_{clip}_sheet{suffix}.png"), 6)
            vc.remove_files(tiles)
    # Every clip at its middle frame.
    sc.render.resolution_x, sc.render.resolution_y = 160, 213
    tiles = []
    for name, src, loop in vo_ual.CLIPS:
        act = actions[name]
        vo_ual.use_action(arm, act)
        sc.frame_set(int(round(act.frame_range[1] * 0.5)))
        vc.aim_camera(cam, (dist * 0.75, -dist * 0.65, H * 0.62), (0, 0, H * 0.45), lens=50)
        tiles.append(vc.render_to(os.path.join(out_dir, f"_tmp_{look}_clip_{name}.png")))
    vc.compose_sheet(tiles, os.path.join(out_dir, f"{look}_clips_sheet.png"), 10)
    vc.remove_files(tiles)
    reset_pose(arm)
    sc.frame_set(0)


BONE_COLOURS = {}


def debug_renders(look, R, arm, body, weights, out_dir):
    """Scratch renders (not committed): dominant bone per vertex (bind pose, four views) and the
    rest skeleton's joints over the mesh."""
    me = body.data
    names = sorted({n for w in weights for n in w})
    rng = np.random.default_rng(7)
    pal = {n: rng.uniform(0.15, 1.0, 3) for n in names}
    for n, c in (("pelvis", (0.9, 0.9, 0.9)), ("cloth_front", (1, 0, 1)), ("cloth_back", (0, 1, 1)),
                 ("mantle", (1, 1, 0)), ("braid_l", (1, 0.4, 0)), ("braid_r", (0, 0.4, 1)), ("beard", (0.4, 1, 0.4))):
        pal[n] = np.array(c)
    col = me.color_attributes.new("dbg", 'BYTE_COLOR', 'POINT')
    for vi, w in enumerate(weights):
        c = sum(pal[n] * x for n, x in w.items())
        col.data[vi].color = (*np.clip(c, 0, 1), 1.0)
    sc = bpy.context.scene
    eng = sc.render.engine
    sc.render.engine = 'BLENDER_WORKBENCH'
    sc.display.shading.light = 'STUDIO'
    sc.display.shading.color_type = 'VERTEX'
    sc.render.resolution_x, sc.render.resolution_y = 600, 800
    cam = bpy.data.objects.new("dbgcam", bpy.data.cameras.new("dbgcam"))
    vc.link(cam)
    sc.camera = cam
    H = R["height"]
    reset_pose(arm)
    tiles = []
    for k, loc in enumerate(((0, -4.5, H * 0.55), (4.5, 0, H * 0.55), (0, 4.5, H * 0.55), (3.2, -3.2, H * 0.7))):
        vc.aim_camera(cam, loc, (0, 0, H * 0.5), lens=55)
        p = os.path.join(out_dir, f"_dbg{k}.png")
        sc.render.filepath = p
        bpy.ops.render.render(write_still=True)
        tiles.append(p)
    vc.compose_sheet(tiles, os.path.join(out_dir, f"{look}_weights.png"), 4)
    vc.remove_files(tiles)
    for clip, frames in (("walk", (4, 12)), ("run", (5, 12)), ("sword_attack", (14, 22)), ("sit_ground_idle", (0,)),
                         ("death", (40,)), ("roll", (15,)), ("crouch_walk", (10,))):
        tiles = []
        vo_ual.use_action(arm, vo_ual_actions[clip])
        for f in frames:
            sc.frame_set(f)
            for loc in ((4.5, 0, H * 0.5), (3.2, -3.2, H * 0.6)):
                vc.aim_camera(cam, loc, (0, 0, H * 0.45), lens=55)
                p = os.path.join(out_dir, f"_dbg_{clip}{f}{loc[1]}.png")
                sc.render.filepath = p
                bpy.ops.render.render(write_still=True)
                tiles.append(p)
        vc.compose_sheet(tiles, os.path.join(out_dir, f"{look}_w_{clip}.png"), len(tiles))
        vc.remove_files(tiles)
    reset_pose(arm)
    me.color_attributes.remove(col)
    bpy.data.objects.remove(cam, do_unlink=True)
    sc.render.engine = eng


# ---------------------------------------------------------------------------------------------

vo_ual_actions = {}


def build_look(R, preview, debug_dir=None):
    look, race = R["look"], R["race"]
    vc.reset_scene()
    s = look_scale(R)
    arm, actions, settings = vo_ual.load_skeleton(s, lean=R["lean"], fit=R["fit"], helpers=default_helpers(R),
                                                  sit=R["sit"])
    vo_ual_actions.clear()
    vo_ual_actions.update(actions)
    reset_pose(arm)
    body, (src_tris, src_verts) = load_source(R)
    paths, metal = bake_textures(body, look, R["texture"])
    missing, bind_scale, summary = rig(body, arm, R, metal)
    for kind, n, ctr, w in summary:
        print(f"RIGID {look} {kind}: {n} vertices at {ctr} -> {w}")
    reset_pose(arm)
    chars = os.path.join(vc.SRC, "characters")
    out_glb = os.path.join(chars, f"{look}.glb")
    vc.export_glb(out_glb, [arm, body], textures=True)
    toml = os.path.join(chars, f"{look}.anims.toml")
    with open(toml, "w", newline="\n") as f:
        f.write(f"# Generated by tools/blender/gen_character.py --look {look}. Clip settings for assetc.\n")
        f.write("# Clips: Quaternius Universal Animation Library (CC0, see tools/blender/vo_ual.py) and the\n")
        f.write("# procedural sit_ground_* clips. move_speed = the clip's own planted-foot speed (the client\n")
        f.write("# scales playback to the real ground speed).\n")
        for name, src, loop in vo_ual.CLIPS:
            lp, sp = settings[name]
            f.write(f'\n[[clip]]\nname = "{name}"\nloop = {"true" if lp else "false"}\nmove_speed = {sp}\n')
    if look == f"{race}_0":
        # LEGACY un-suffixed names for clients that do not load <race>_<n> yet: byte copies (the glb
        # keeps referencing <race>_0*.png, so the converted meshes are identical too). Remove once
        # every client loads looks by index.
        for src, dst in ((out_glb, f"{race}.glb"), (toml, f"{race}.anims.toml")):
            with open(src, "rb") as fi, open(os.path.join(chars, dst), "wb") as fo:
                fo.write(fi.read())
    V = np.array([v.co for v in body.data.vertices])
    print("STATS", look, "bones", len(arm.data.bones), "tris", vc.triangle_count(body), "verts", len(V),
          "source_tris", src_tris, "source_verts", src_verts,
          "height", round(float(V[:, 2].max()), 3), "bottom", round(float(V[:, 2].min()), 3),
          "unweighted", missing, "bind_scale", bind_scale, "scale", round(s, 4),
          "textures", ", ".join(os.path.basename(p) for p in paths.values()))
    slide_report(look, arm, actions, settings)
    if debug_dir:
        weights = [{g.group: g.weight for g in v.groups} for v in body.data.vertices]
        names = {g.index: g.name for g in body.vertex_groups}
        weights = [{names[k]: x for k, x in w.items()} for w in weights]
        debug_renders(look, R, arm, body, weights, vc.ensure_dir(debug_dir))
    if preview:
        render_previews(look, R, arm, actions, vc.ensure_dir(os.path.join(vc.PREVIEWS, "characters")))


def main():
    args = vc.script_args()
    preview = "--no-preview" not in args
    debug_dir = args[args.index("--debug") + 1] if "--debug" in args else None
    if "--look" in args:
        names = args[args.index("--look") + 1].split(",")
    else:
        race = args[args.index("--race") + 1] if "--race" in args else "aydor"
        names = [L["look"] for L in LOOKS if L["race"] == race]
    for name in names:
        build_look(LOOK[name], preview, debug_dir)


main()
