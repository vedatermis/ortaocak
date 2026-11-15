"""Zone 1 props: imports the generated source meshes and turns them into game-ready props.

Usage (headless):
  blender --background --factory-startup --python tools/blender/gen_props.py -- [--only name,name] [--no-preview]

Sources: tools/blender/source_meshes/props/<src>.glb (Tripo H3.1 output, project-owned: one mesh, one
material, embedded 4096^2 Color / NormalGL / ORM images, normalised to ~1 unit, centred on its bounds,
arbitrary facing). Everything a prop needs is data in PROPS below: source, facing fix, real-world
size, ground offset, triangle budget, texture size, footprint, emissive regions, LOD.

Per prop the script
  1. imports the source, joins it, rotates it so its front faces -Y (engine +Z) and scales it to its
     real-world size (uniform from one axis, or per axis for the fence section);
  2. cleans it: welds coincident vertices, reports open edges ("holes"), makes face winding consistent
     on closed shells (flips counted), deletes small shells that float free of everything else;
  3. grounds it: origin below the centre of its base, base on Z = 0, then sunk by `sink` metres so an
     uneven base never shows a gap on sloped ground;
  4. decimates to the triangle budget (Blender collapse; UVs interpolated) and, for `lod`, adds a
     second, coarser copy named `lod1` (the full one is `lod0`; the client picks by distance);
  5. downsamples the embedded maps (box filter: colour in linear light, normals as vectors, ORM
     linear) to `tex` and writes assets_src/props/<name>.png, <name>_n.png, <name>_orm.png
     (R occlusion, G roughness, B metallic: Tripo's ORM image already has that layout), plus
     <name>_e.png when an emissive mask is derived (see EMISSIVE);
  6. exports assets_src/props/<name>.glb (one material: base colour, normal map, ORM wired to both
     occlusion and metallic-roughness, optional emissive texture + KHR_materials_emissive_strength);
  7. measures the result and writes assets_src/props/props_meta.json (bounds, triangles, footprint
     polygon in engine coordinates) for gen_terrain.py, and preview renders next to a 1.8 m marker.

Emissive (EMISSIVE in the table): a texel glows when (a) it belongs to a face inside one of the prop's
region boxes (final metres, Blender axes) and (b) its colour/roughness passes the rule (`warm`: warm,
saturated, bright base colour = lantern glass / candle; `glass`: smooth (ORM roughness < 0.45) and not
dark = window panes; `ember`: orange-red, saturated = the Ant Tasi crack). The emissive PNG is
base colour x mask (sRGB), the strength goes to KHR_materials_emissive_strength.

Conventions as everywhere (docs/ASSET_PIPELINE.md): Blender Z up, front -Y, left +X; engine x = -X,
z = -Y. Deterministic: fixed operations, own PNG writer, no time.
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

SOURCE_DIR = os.path.join(vc.HERE, "source_meshes", "props")
OUT_DIR = os.path.join(vc.SRC, "props")
META_PATH = os.path.join(OUT_DIR, "props_meta.json")

# rot_z: degrees about Z that turn the source's front to -Y. size: (axis, metres) for a uniform
# scale, or ("xyz", (X, Y, Z)) for per-axis. sink: metres below Z = 0 after grounding. tris: budget of
# lod0; lod: triangles of the far LOD (None = no LOD). tex: texture size. foot: footprint for the walk
# mask: ("hull", z_max) = convex hull of everything below z_max, ("disc", z0, z1) = disc around the
# origin covering the vertices between z0 and z1 (posts, trunks). emissive: (rule, strength, mask
# size, [regions]) with regions as ((x0, y0, z0), (x1, y1, z1)) in final metres.
PROPS = {
    "house_a": dict(src="house_a", rot_z=-90.0, size=("z", 8.0), sink=0.18, tris=14000, lod=2600, tex=2048,
                    foot=("hull", 2.2)),
    "cottage": dict(src="cottage", rot_z=180.0, size=("x", 6.9), sink=0.18, tris=14000, lod=2600, tex=2048,
                    foot=("hull", 2.2)),
    "well": dict(src="well", rot_z=0.0, size=("z", 2.9), sink=0.08, tris=7000, lod=None, tex=1024,
                 foot=("hull", 1.0)),
    "market_stall": dict(src="market_stall", rot_z=-90.0, size=("x", 3.4), sink=0.05, tris=9000, lod=None,
                         tex=1024, foot=("hull", 1.0)),
    "ant_tasi": dict(src="ant_tasi", rot_z=-90.0, size=("z", 4.9), sink=0.10, tris=11600, lod=None, tex=2048,
                     foot=("hull", 1.0)),
    "watchtower": dict(src="watchtower", rot_z=0.0, size=("z", 8.8), sink=0.15, tris=9000, lod=2000, tex=1024,
                       foot=("hull", 1.6)),
    "fence": dict(src="fence", rot_z=90.0, size=("xyz", (4.0, 0.30, 1.25)), sink=0.06, tris=2400, lod=None,
                  tex=512, foot=("hull", 2.2)),
    "lamp_post": dict(src="lamp_post", rot_z=180.0, size=("z", 3.2), sink=0.06, tris=3000, lod=None, tex=512,
                      foot=("disc", 0.0, 0.6)),
    "goods": dict(src="goods", rot_z=0.0, size=("x", 1.8), sink=0.04, tris=5000, lod=None, tex=1024,
                  foot=("hull", 2.2)),
    "signpost": dict(src="signpost", rot_z=0.0, size=("z", 2.6), sink=0.08, tris=3000, lod=None, tex=512,
                     foot=("disc", 0.6, 1.4)),
    "boulder": dict(src="boulder", rot_z=0.0, size=("x", 2.1), sink=0.25, tris=3000, lod=None, tex=1024,
                    foot=("hull", 2.2)),
    "tree_oak": dict(src="oak", rot_z=0.0, size=("z", 9.0), sink=0.20, tris=12000, lod=1600, tex=2048,
                     foot=("disc", 1.0, 2.0)),
    "tree_pine": dict(src="pine", rot_z=0.0, size=("z", 12.0), sink=0.20, tris=10000, lod=1200, tex=2048,
                      foot=("disc", 1.0, 2.0)),
}

# Emissive masks: strength (KHR_materials_emissive_strength), mask size, parts = (rule, weight, warmth,
# regions). Regions were read off the processed props (final metres, Blender axes: front -Y).
WARM_LIGHT = (1.0, 0.55, 0.22)  # linear candle colour the window glow is pulled towards
EMISSIVE = {
    # Wall lantern beside the door; window panes (smooth glass in the ORM) below the eaves, dimmer.
    "house_a": dict(strength=3.0, size=1024, parts=[
        ("warm", 1.0, 0.2, [((1.3, -4.2, 1.2), (3.2, -2.8, 2.7))]),
        ("glass", 0.22, 0.85, [((-3.2, -4.2, 0.6), (3.2, 4.2, 5.4))]),
    ]),
    "lamp_post": dict(strength=4.0, size=512, parts=[
        ("warm", 1.0, 0.2, [((-0.6, -1.9, 1.4), (0.6, -0.9, 2.6))]),
    ]),
    # The ember-lit crack at the top of the oath stone (docs/LORE.md).
    "ant_tasi": dict(strength=1.4, size=1024, parts=[
        ("ember", 1.0, 0.1, [((-0.7, -0.8, 2.8), (0.7, 0.5, 5.0))]),
    ]),
    # The two lanterns hanging at the stall's outer posts.
    "market_stall": dict(strength=3.0, size=1024, parts=[
        ("warm", 1.0, 0.25, [((1.3, -1.4, 1.6), (2.0, 1.4, 2.9)), ((-2.0, -1.4, 1.6), (-1.3, 1.4, 2.9))]),
    ]),
    # The brazier on the lookout platform.
    "watchtower": dict(strength=3.0, size=1024, parts=[
        ("ember", 1.0, 0.1, [((1.1, -1.1, 4.5), (2.3, 0.3, 6.0))]),
    ]),
}

FLOAT_EPS = 1e-4  # weld distance (m)


# ---------------------------------------------------------------------------------------------
# Mesh processing

def import_source(src):
    before = set(bpy.data.objects)
    bpy.ops.import_scene.gltf(filepath=os.path.join(SOURCE_DIR, f"{src}.glb"))
    objs = [o for o in bpy.data.objects if o not in before and o.type == 'MESH']
    for o in objs:
        o.data.transform(o.matrix_world)
        o.parent = None
        o.matrix_world = Matrix.Identity(4)
    obj = objs[0]
    if len(objs) > 1:
        with bpy.context.temp_override(active_object=obj, selected_editable_objects=objs):
            bpy.ops.object.join()
    for o in list(bpy.data.objects):
        if o is not obj and o.type != 'MESH':
            bpy.data.objects.remove(o)
    return obj


def verts_np(obj):
    co = np.empty(len(obj.data.vertices) * 3, np.float64)
    obj.data.vertices.foreach_get("co", co)
    return co.reshape(-1, 3)


def place(obj, cfg):
    me = obj.data
    me.transform(Matrix.Rotation(math.radians(cfg["rot_z"]), 4, 'Z'))
    v = verts_np(obj)
    ext = v.max(0) - v.min(0)
    kind, val = cfg["size"]
    if kind == "xyz":
        s = np.array(val) / ext
    else:
        k = "xyz".index(kind)
        s = np.full(3, val / ext[k])
    me.transform(Matrix.Diagonal((s[0], s[1], s[2], 1.0)))
    return s


def clean(obj, name):
    """Weld, fix winding on closed shells, drop small floating shells. Returns a report dict."""
    me = obj.data
    bm = bmesh.new()
    bm.from_mesh(me)
    n0 = len(bm.verts)
    bmesh.ops.remove_doubles(bm, verts=bm.verts, dist=FLOAT_EPS)
    welded = n0 - len(bm.verts)
    bm.faces.ensure_lookup_table()
    # Shells (connected face sets).
    seen = set()
    shells = []
    for f in bm.faces:
        if f.index in seen:
            continue
        stack, shell = [f], []
        seen.add(f.index)
        while stack:
            g = stack.pop()
            shell.append(g)
            for e in g.edges:
                for h in e.link_faces:
                    if h.index not in seen:
                        seen.add(h.index)
                        stack.append(h)
        shells.append(shell)
    total = len(bm.faces)

    def bbox(shell):
        pts = np.array([v.co[:] for f in shell for v in f.verts])
        return pts.min(0), pts.max(0)

    boxes = [bbox(s) for s in shells]
    flipped = 0
    removed = []
    big = max(len(s) for s in shells)
    for i, shell in enumerate(shells):
        lo, hi = boxes[i]
        closed = all(len(e.link_faces) == 2 for f in shell for e in f.edges)
        if closed:
            before = [f.normal.copy() for f in shell]
            bmesh.ops.recalc_face_normals(bm, faces=shell)
            flipped += sum(1 for f, n in zip(shell, before) if f.normal.dot(n) < 0.0)
        # Floating: a small shell (< 0.5 % of the faces) whose box does not touch any other shell's
        # box (2 cm tolerance) and that is above the ground.
        if len(shell) < 0.005 * total and len(shell) < big:
            touching = False
            for j, (lo2, hi2) in enumerate(boxes):
                if j != i and np.all(lo <= hi2 + 0.02) and np.all(lo2 <= hi + 0.02):
                    touching = True
                    break
            if not touching and lo[2] > -0.5:
                removed.append(shell)
    removed_faces = sum(len(s) for s in removed)
    if removed:
        bmesh.ops.delete(bm, geom=[f for s in removed for f in s], context='FACES')
    loose = [v for v in bm.verts if not v.link_faces]
    bmesh.ops.delete(bm, geom=loose, context='VERTS')
    open_edges = sum(1 for e in bm.edges if len(e.link_faces) == 1)
    nonmanifold = sum(1 for e in bm.edges if len(e.link_faces) > 2)
    bm.to_mesh(me)
    bm.free()
    me.update()
    return dict(shells=len(shells), welded=welded, flipped=flipped, floating_removed=len(removed),
                floating_faces=removed_faces, open_edges=open_edges, nonmanifold_edges=nonmanifold)


def ground(obj, cfg):
    """Origin below the centre of the base, base at Z = -sink. Returns base stats."""
    v = verts_np(obj)
    zmin = v[:, 2].min()
    h = v[:, 2].max() - zmin
    base = v[v[:, 2] < zmin + max(0.02 * h, 0.05)]
    cx = 0.5 * (base[:, 0].min() + base[:, 0].max())
    cy = 0.5 * (base[:, 1].min() + base[:, 1].max())
    # Flatness of the base: spread of the lowest 5 % band.
    band = v[v[:, 2] < zmin + 0.05 * h]
    obj.data.transform(Matrix.Translation((-cx, -cy, -zmin - cfg["sink"])))
    return dict(base_spread=round(float(np.percentile(band[:, 2], 50) - zmin), 3), base_w=round(float(np.ptp(base[:, 0])), 3),
                base_d=round(float(np.ptp(base[:, 1])), 3))


def decimate(obj, tris):
    obj.data.calc_loop_triangles()
    n = len(obj.data.loop_triangles)
    if n > tris:
        mod = obj.modifiers.new("decimate", 'DECIMATE')
        mod.decimate_type = 'COLLAPSE'
        mod.ratio = tris / n
        mod.use_collapse_triangulate = True
        with bpy.context.temp_override(object=obj, active_object=obj):
            bpy.ops.object.modifier_apply(modifier=mod.name)
    obj.data.calc_loop_triangles()
    return len(obj.data.loop_triangles)


def triangulate(obj):
    bm = bmesh.new()
    bm.from_mesh(obj.data)
    bmesh.ops.triangulate(bm, faces=bm.faces)
    bm.to_mesh(obj.data)
    bm.free()


# ---------------------------------------------------------------------------------------------
# Textures

def source_images(obj):
    """(color, normal, orm) Blender images of the imported material."""
    mat = obj.data.materials[0]
    col = nrm = orm = None
    for n in mat.node_tree.nodes:
        if n.type != 'TEX_IMAGE' or n.image is None:
            continue
        targets = [l.to_node for out in n.outputs for l in out.links]
        kinds = {t.type for t in targets}
        if 'NORMAL_MAP' in kinds:
            nrm = n.image
        elif 'SEPARATE_COLOR' in kinds or 'SEPRGB' in kinds:
            orm = n.image
        elif 'BSDF_PRINCIPLED' in kinds:
            col = n.image
    if col is None or nrm is None or orm is None:
        raise SystemExit(f"{obj.name}: expected Color, NormalGL and ORM images")
    return col, nrm, orm


def pixels(img):
    w, h = img.size
    px = np.empty(w * h * 4, np.float32)
    img.pixels.foreach_get(px)
    return px.reshape(h, w, 4)[::-1, :, :3].copy()  # top row first


def srgb_to_lin(c):
    return np.where(c <= 0.04045, c / 12.92, ((c + 0.055) / 1.055) ** 2.4)


def lin_to_srgb(c):
    c = np.clip(c, 0.0, 1.0)
    return np.where(c <= 0.0031308, c * 12.92, 1.055 * np.power(c, 1.0 / 2.4) - 0.055)


def box_down(a, size):
    f = a.shape[0] // size
    if f <= 1:
        return a
    h, w, c = a.shape
    return a.reshape(h // f, f, w // f, f, c).mean(axis=(1, 3), dtype=np.float64).astype(np.float32)


def down_color(srgb, size):
    return lin_to_srgb(box_down(srgb_to_lin(srgb), size))


def down_normal(rgb, size):
    xy = rgb[..., :2] * 2.0 - 1.0
    z = np.sqrt(np.clip(1.0 - (xy ** 2).sum(-1, keepdims=True), 0.0, 1.0))
    v = box_down(np.concatenate([xy, z], -1), size)
    v /= np.maximum(np.linalg.norm(v, axis=-1, keepdims=True), 1e-6)
    return v * 0.5 + 0.5


def rasterize_faces(obj, faces, size):
    """Boolean UV-space mask (top row first) of the given polygons' UV triangles."""
    me = obj.data
    uv = me.uv_layers.active.data
    mask = np.zeros((size, size), bool)
    for p in faces:
        idx = list(p.loop_indices)
        pts = np.array([uv[i].uv[:] for i in idx], np.float64)
        pts[:, 0] *= size
        pts[:, 1] = (1.0 - pts[:, 1]) * size
        for k in range(1, len(pts) - 1):
            a, b, c = pts[0], pts[k], pts[k + 1]
            x0, y0 = np.floor(np.minimum(np.minimum(a, b), c)).astype(int) - 1
            x1, y1 = np.ceil(np.maximum(np.maximum(a, b), c)).astype(int) + 1
            x0, y0 = max(x0, 0), max(y0, 0)
            x1, y1 = min(x1, size), min(y1, size)
            if x1 <= x0 or y1 <= y0:
                continue
            gx, gy = np.meshgrid(np.arange(x0, x1) + 0.5, np.arange(y0, y1) + 0.5)
            d = (b[1] - c[1]) * (a[0] - c[0]) + (c[0] - b[0]) * (a[1] - c[1])
            if abs(d) < 1e-12:
                continue
            l1 = ((b[1] - c[1]) * (gx - c[0]) + (c[0] - b[0]) * (gy - c[1])) / d
            l2 = ((c[1] - a[1]) * (gx - c[0]) + (a[0] - c[0]) * (gy - c[1])) / d
            l3 = 1.0 - l1 - l2
            # Half a texel of slack so thin islands are not missed.
            tol = 0.5 / max(np.linalg.norm(b - a), np.linalg.norm(c - a), 1.0)
            inside = (l1 >= -tol) & (l2 >= -tol) & (l3 >= -tol)
            mask[y0:y1, x0:x1] |= inside
    return mask


def emissive_rule(rule, srgb, orm):
    r, g, b = srgb[..., 0], srgb[..., 1], srgb[..., 2]
    mx = np.maximum(np.maximum(r, g), b)
    mn = np.minimum(np.minimum(r, g), b)
    sat = (mx - mn) / np.maximum(mx, 1e-4)
    # Hue in degrees (0 red, 60 yellow).
    hue = np.degrees(np.arctan2(np.sqrt(3.0) * (g - b), 2 * r - g - b)) % 360.0
    if rule == "warm":
        return (hue >= 15.0) & (hue <= 62.0) & (sat >= 0.30) & (mx >= 0.50)
    if rule == "glass":
        return (orm[..., 1] < 0.45) & (mx >= 0.18) & (mx <= 0.85)
    if rule == "ember":
        return ((hue <= 42.0) | (hue >= 345.0)) & (sat >= 0.45) & (mx >= 0.30)
    raise SystemExit(f"unknown emissive rule {rule}")


def write_textures(obj, name, cfg):
    col_img, nrm_img, orm_img = source_images(obj)
    size = cfg["tex"]
    col_full = pixels(col_img)
    orm_full = pixels(orm_img)
    col = down_color(col_full, size)
    paths = dict(color=os.path.join(OUT_DIR, f"{name}.png"), normal=os.path.join(OUT_DIR, f"{name}_n.png"),
                 orm=os.path.join(OUT_DIR, f"{name}_orm.png"))
    vc.write_png(paths["color"], vc.to_u8(col))
    vc.write_png(paths["normal"], vc.to_u8(down_normal(pixels(nrm_img), size)))
    vc.write_png(paths["orm"], vc.to_u8(box_down(orm_full, size)))
    info = dict(tex=size)
    em = EMISSIVE.get(name)
    if em is not None:
        esize = em["size"]
        ecol = down_color(col_full, esize)
        eorm = box_down(orm_full, esize)
        lin = srgb_to_lin(ecol)
        emis_lin = np.zeros_like(lin)
        texels, nfaces = 0, 0
        for rule, weight, warmth, regions in em["parts"]:
            faces = [p for p in obj.data.polygons
                     if any(all(lo[k] <= p.center[k] <= hi[k] for k in range(3)) for lo, hi in regions)]
            region = rasterize_faces(obj, faces, esize)
            mask = region & emissive_rule(rule, ecol, eorm)
            # Soften by one texel so BC compression and mips do not leave a hard rim.
            soft = mask.astype(np.float32)
            pad = np.pad(soft, 1, mode='edge')
            soft = np.maximum(soft, 0.5 * np.maximum.reduce([pad[:-2, 1:-1], pad[2:, 1:-1], pad[1:-1, :-2],
                                                             pad[1:-1, 2:]]))
            soft *= region
            # Glow colour: the base colour pulled towards warm candle light by `warmth` (window
            # panes are grey in the base colour; their glow is the light behind them).
            bright = np.clip(lin.max(-1, keepdims=True), 0.25, 1.0)
            glow = lin * (1.0 - warmth) + np.array(WARM_LIGHT, np.float32) * bright * warmth
            emis_lin = np.maximum(emis_lin, glow * (soft * weight)[..., None])
            texels += int(mask.sum())
            nfaces += len(faces)
        paths["emissive"] = os.path.join(OUT_DIR, f"{name}_e.png")
        vc.write_png(paths["emissive"], vc.to_u8(lin_to_srgb(emis_lin)))
        info.update(emissive_strength=em["strength"], emissive_size=esize, emissive_texels=texels,
                    emissive_faces=nfaces)
    for img in (col_img, nrm_img, orm_img):
        bpy.data.images.remove(img)
    return paths, info


def build_material(name, paths, strength):
    from io_scene_gltf2.blender.com.material_helpers import create_settings_group, get_gltf_node_name
    mat = bpy.data.materials.new(name)
    mat.use_nodes = True
    mat.use_backface_culling = True
    nt = mat.node_tree
    bsdf = nt.nodes.get("Principled BSDF")
    bsdf.inputs["Metallic"].default_value = 1.0   # ORM.b decides (0 for nearly everything)
    bsdf.inputs["Roughness"].default_value = 1.0  # ORM.g decides

    def tex(path, non_color):
        n = nt.nodes.new("ShaderNodeTexImage")
        n.image = bpy.data.images.load(path, check_existing=False)
        n.image.colorspace_settings.name = 'Non-Color' if non_color else 'sRGB'
        return n

    c = tex(paths["color"], False)
    nt.links.new(c.outputs["Color"], bsdf.inputs["Base Color"])
    n = tex(paths["normal"], True)
    nm = nt.nodes.new("ShaderNodeNormalMap")
    nt.links.new(n.outputs["Color"], nm.inputs["Color"])
    nt.links.new(nm.outputs["Normal"], bsdf.inputs["Normal"])
    o = tex(paths["orm"], True)
    sep = nt.nodes.new("ShaderNodeSeparateColor")
    nt.links.new(o.outputs["Color"], sep.inputs["Color"])
    nt.links.new(sep.outputs["Green"], bsdf.inputs["Roughness"])
    nt.links.new(sep.outputs["Blue"], bsdf.inputs["Metallic"])
    group_name = get_gltf_node_name()
    group = bpy.data.node_groups.get(group_name) or create_settings_group(group_name)
    gn = nt.nodes.new("ShaderNodeGroup")
    gn.node_tree = group
    nt.links.new(sep.outputs["Red"], gn.inputs["Occlusion"])
    if "emissive" in paths:
        e = tex(paths["emissive"], False)
        nt.links.new(e.outputs["Color"], bsdf.inputs["Emission Color"])
        bsdf.inputs["Emission Strength"].default_value = strength
    return mat


# ---------------------------------------------------------------------------------------------
# Footprint (engine coordinates: x = -X, z = -Y)

def convex_hull(pts):
    pts = sorted(set((round(float(x), 4), round(float(y), 4)) for x, y in pts))
    if len(pts) < 3:
        return pts

    def cross(o, a, b):
        return (a[0] - o[0]) * (b[1] - o[1]) - (a[1] - o[1]) * (b[0] - o[0])

    lower, upper = [], []
    for p in pts:
        while len(lower) >= 2 and cross(lower[-2], lower[-1], p) <= 0:
            lower.pop()
        lower.append(p)
    for p in reversed(pts):
        while len(upper) >= 2 and cross(upper[-2], upper[-1], p) <= 0:
            upper.pop()
        upper.append(p)
    return lower[:-1] + upper[:-1]


def footprint(v, cfg):
    kind = cfg["foot"][0]
    eng = np.stack([-v[:, 0], -v[:, 1]], 1)
    if kind == "hull":
        sel = eng[v[:, 2] < cfg["foot"][1]]
        return dict(kind="hull", points=[[round(x, 3), round(z, 3)] for x, z in convex_hull(sel)])
    z0, z1 = cfg["foot"][1], cfg["foot"][2]
    sel = eng[(v[:, 2] >= z0) & (v[:, 2] <= z1)]
    r = float(np.percentile(np.hypot(sel[:, 0], sel[:, 1]), 95))
    return dict(kind="disc", radius=round(r, 3))


# ---------------------------------------------------------------------------------------------

def build(name, cfg):
    vc.reset_scene()
    obj = import_source(cfg["src"])
    obj.data.calc_loop_triangles()
    src_tris = len(obj.data.loop_triangles)
    scale = place(obj, cfg)
    report = clean(obj, name)
    base = ground(obj, cfg)
    paths, tex_info = write_textures(obj, name, cfg)
    em = EMISSIVE.get(name)
    mat = build_material(name, paths, em["strength"] if em else 0.0)
    obj.data.materials.clear()
    obj.data.materials.append(mat)
    for p in obj.data.polygons:
        p.material_index = 0
    triangulate(obj)
    full = obj
    lod1 = None
    if cfg["lod"]:
        lod1 = full.copy()
        lod1.data = full.data.copy()
        vc.link(lod1)
    tris0 = decimate(full, cfg["tris"])
    full.name = "lod0" if lod1 else name
    full.data.name = full.name
    objs = [full]
    tris1 = 0
    if lod1:
        tris1 = decimate(lod1, cfg["lod"])
        lod1.name = "lod1"
        lod1.data.name = "lod1"
        objs.append(lod1)
    for o in objs:
        o.data.shade_smooth()
    vc.export_glb(os.path.join(OUT_DIR, f"{name}.glb"), objs, textures=True)
    v = verts_np(full)
    lo, hi = v.min(0), v.max(0)
    meta = dict(source=f"{cfg['src']}.glb", source_tris=src_tris, tris=tris0, lod_tris=tris1,
                scale=[round(float(s), 4) for s in scale], rot_z=cfg["rot_z"], sink=cfg["sink"],
                # Engine-space bounds (x = -X, y = Z, z = -Y).
                bounds_min=[round(float(-hi[0]), 3), round(float(lo[2]), 3), round(float(-hi[1]), 3)],
                bounds_max=[round(float(-lo[0]), 3), round(float(hi[2]), 3), round(float(-lo[1]), 3)],
                footprint=footprint(v, cfg), **report, **base, **tex_info)
    print(f"PROP {name}: {src_tris} -> {tris0} tris (lod1 {tris1}), size {np.round(hi - lo, 2)} m, "
          f"shells {report['shells']}, welded {report['welded']}, flipped {report['flipped']}, floating "
          f"{report['floating_removed']} ({report['floating_faces']} faces), open edges {report['open_edges']}, "
          f"base spread {base['base_spread']:.3f} m, tex {cfg['tex']}"
          + (f", emissive {tex_info['emissive_texels']} texels on {tex_info['emissive_faces']} faces"
             if 'emissive_texels' in tex_info else ""))
    return objs, meta


def preview(name, objs, meta):
    """Three-quarter view of lod0 next to a 1.8 m marker (and the far LOD beside it)."""
    cam = vc.setup_preview((480, 480), sun_dir=(-0.6, -1.0, 1.3))
    ground_mat = vc.material("preview_ground", "#8a8476")
    bpy.ops.mesh.primitive_plane_add(size=200, location=(0, 0, 0))
    bpy.context.active_object.data.materials.append(ground_mat)
    lo, hi = np.array(meta["bounds_min"]), np.array(meta["bounds_max"])
    w = float(max(hi[0] - lo[0], hi[2] - lo[2]))
    bpy.ops.mesh.primitive_cylinder_add(radius=0.12, depth=1.8, location=(w * 0.5 + 0.6, 0.0, 0.9))
    bpy.context.active_object.data.materials.append(vc.material("preview_marker", "#3c8cff"))
    if len(objs) > 1:
        objs[1].location = (-(w + 1.0), 0, 0)
    size = max(w, float(hi[1] - lo[1]))
    d = size * 1.7 + 2.0
    tgt = (-(w + 1.0) * 0.5 if len(objs) > 1 else 0.0, 0.0, float(hi[1]) * 0.45)
    vc.aim_camera(cam, (tgt[0] + d * 0.62, -d * 0.8, tgt[2] + d * 0.38), tgt, lens=50 if len(objs) == 1 else 40)
    return vc.render_to(os.path.join(vc.PREVIEWS, "props", f"{name}.png"))


def main():
    args = vc.script_args()
    only = None
    if "--only" in args:
        only = set(args[args.index("--only") + 1].split(","))
    meta_all = {}
    if os.path.exists(META_PATH):
        with open(META_PATH, "r", encoding="utf-8") as f:
            meta_all = json.load(f)
    shots = []
    for name, cfg in PROPS.items():
        if only and name not in only:
            continue
        objs, meta = build(name, cfg)
        meta_all[name] = meta
        if "--no-preview" not in args:
            shots.append(preview(name, objs, meta))
    meta_all = {k: meta_all[k] for k in PROPS if k in meta_all}
    with open(META_PATH, "w", encoding="utf-8", newline="\n") as f:
        json.dump(meta_all, f, indent=1, sort_keys=True)
        f.write("\n")
    if shots and not only:
        vc.compose_sheet(shots, os.path.join(vc.PREVIEWS, "props", "props_sheet.png"), 5)


main()
