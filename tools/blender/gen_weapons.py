"""Weapons (M8 "Kusan"): turns the generated weapon meshes into game-ready weapons and item icons.

Usage (headless):
  blender --background --factory-startup --python tools/blender/gen_weapons.py -- [--only a,b] [--no-preview] [--no-icons]
  blender ... gen_weapons.py -- --prepare <raw dir>     (once per new raw source, see below)

Sources: tools/blender/source_meshes/weapons/<name>.glb. The raw Tripo H3.1 output (≈1M triangles,
27-46 MB each, external_sources/weapons_a/, not committed) is reduced once by `--prepare`: welded and
collapse-decimated to PREP_TRIS triangles, written by write_glb() below with the raw file's three
embedded images (Color JPEG, ORM JPEG, NormalGL PNG, 2048^2) copied byte for byte, so the committed
source loses geometry detail only (the final meshes have 1.4-4k triangles) and no texture
information. The concept image goes next to it as <name>_ref.png (downscaled to 512 px).

Per weapon (data in WEAPONS) the build
  1. imports the reduced source and puts it into the GRIP FRAME (docs/ASSET_PIPELINE.md "Weapons"):
     in Blender axes the weapon stands upright with its business end (blade tip, head, crystal) up
     (+Z), its edge / front facing -Y and the point the hand closes around at the origin; shields:
     face (boss) towards -Y, top up, origin at the centre of the hand grip behind the boss. After
     the glTF export and the assetc X mirror that is engine +Y = along the weapon, +Z = edge / face,
     origin = grip;
  2. scales it to its real length (`size`), cleans it like the props (weld, consistent winding on
     closed shells, floating specks removed);
  3. downsamples the maps to `tex` (colour in linear light, normals as vectors, ORM linear) and,
     for the two staffs, writes an emissive map for the crystal (colour rule inside a region);
  4. decimates to `tris` (Blender collapse, UVs interpolated), marks edges sharper than SHARP_DEG
     sharp (blade bevels), exports assets_src/weapons/<name>.glb (one material: base colour, normal,
     ORM as metallic-roughness and occlusion, optional emissive);
  5. renders the 128x128 inventory icon assets_src/ui/items/<icon>.png (see render_icon) and a
     preview assets_src/previews/weapons/<name>.png (front / side / icon angle next to a 1 m bar);
  6. writes assets_src/weapons/weapons_meta.json (triangles, bounds in engine axes, grip, sizes).

Attachment data and the in-hand previews come from gen_attachments.py (it reads these outputs).
Deterministic: fixed operations, own PNG and GLB writers, Cycles with a fixed seed for the renders
(written through vo_common.write_png_stable).
"""

import json
import math
import os
import struct
import sys

import bmesh
import bpy
import numpy as np
from mathutils import Matrix, Vector

sys.dont_write_bytecode = True  # keep tools/blender free of __pycache__
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import vo_common as vc  # noqa: E402
import gen_props as gp  # noqa: E402  (mesh clean-up and texture helpers; its main() does not run)

SOURCE_DIR = os.path.join(vc.HERE, "source_meshes", "weapons")
OUT_DIR = os.path.join(vc.SRC, "weapons")
ICON_DIR = os.path.join(vc.SRC, "ui", "items")
PREVIEW_DIR = os.path.join(vc.PREVIEWS, "weapons")
META_PATH = os.path.join(OUT_DIR, "weapons_meta.json")

PREP_TRIS = 40000  # triangles of the committed reduced sources
REF_SIZE = 512     # concept images are stored at this width
SHARP_DEG = 38.0   # edges with a larger dihedral angle get split normals
ICON_SIZE = 128
ICON_SUPERSAMPLE = 4


# ---------------------------------------------------------------------------------------------
# --prepare: raw Tripo glb -> reduced committed source (geometry decimated, images byte-identical)

def _read_glb(path):
    with open(path, "rb") as f:
        data = f.read()
    magic, _version, _length = struct.unpack_from("<III", data, 0)
    if magic != 0x46546C67:
        raise SystemExit(f"{path}: not a binary glTF")
    jlen, jtype = struct.unpack_from("<II", data, 12)
    gltf = json.loads(data[20:20 + jlen].decode("utf-8"))
    off = 20 + jlen
    blen, _btype = struct.unpack_from("<II", data, off)
    return gltf, data[off + 8:off + 8 + blen]


def _mesh_arrays(obj):
    """Unique (position, normal, uv) corners of the triangulated mesh in glTF axes (+Y up), sorted
    (deterministic), and the triangle index list."""
    me = obj.data
    me.calc_loop_triangles()
    tris = np.empty(len(me.loop_triangles) * 3, np.int64)
    me.loop_triangles.foreach_get("loops", tris)
    loop_vert = np.empty(len(me.loops), np.int64)
    me.loops.foreach_get("vertex_index", loop_vert)
    co = np.empty(len(me.vertices) * 3, np.float32)
    me.vertices.foreach_get("co", co)
    co = co.reshape(-1, 3)
    nrm = np.empty(len(me.loops) * 3, np.float32)
    me.corner_normals.foreach_get("vector", nrm)
    nrm = nrm.reshape(-1, 3)
    uv = np.empty(len(me.loops) * 2, np.float32)
    me.uv_layers.active.data.foreach_get("uv", uv)
    uv = uv.reshape(-1, 2)
    p = co[loop_vert[tris]]
    n = nrm[tris]
    t = uv[tris]
    # Blender (Z up) -> glTF (Y up): (x, y, z) -> (x, z, -y); glTF V runs down the image.
    corners = np.concatenate([p[:, [0, 2, 1]] * np.array([1, 1, -1], np.float32),
                              n[:, [0, 2, 1]] * np.array([1, 1, -1], np.float32),
                              np.stack([t[:, 0], 1.0 - t[:, 1]], 1)], 1).astype(np.float32)
    keys = np.ascontiguousarray(corners).view(np.dtype((np.void, corners.dtype.itemsize * 8))).ravel()
    _, first, inverse = np.unique(keys, return_index=True, return_inverse=True)
    verts = corners[first]
    return verts, inverse.astype(np.uint32)


def write_glb(path, verts, indices, raw_gltf, raw_bin):
    """Minimal deterministic glb: one node, one primitive, the raw file's material/textures/images
    (image bytes copied from its buffer)."""
    chunks = []

    def add(data):
        off = sum(len(c) for c in chunks)
        pad = (-off) % 4
        if pad:
            chunks.append(b"\0" * pad)
            off += pad
        chunks.append(data)
        return off, len(data)

    views, accessors = [], []
    off, ln = add(indices.astype("<u4").tobytes())
    views.append(dict(buffer=0, byteOffset=off, byteLength=ln, target=34963))
    accessors.append(dict(bufferView=0, componentType=5125, count=int(len(indices)), type="SCALAR",
                          min=[int(indices.min())], max=[int(indices.max())]))
    for k, (lo, hi, typ) in enumerate(((0, 3, "VEC3"), (3, 6, "VEC3"), (6, 8, "VEC2"))):
        a = np.ascontiguousarray(verts[:, lo:hi]).astype("<f4")
        off, ln = add(a.tobytes())
        views.append(dict(buffer=0, byteOffset=off, byteLength=ln, target=34962))
        acc = dict(bufferView=len(views) - 1, componentType=5126, count=int(len(a)), type=typ)
        if k == 0:
            acc.update(min=[float(x) for x in a.min(0)], max=[float(x) for x in a.max(0)])
        accessors.append(acc)
    images = []
    for im in raw_gltf["images"]:
        bv = raw_gltf["bufferViews"][im["bufferView"]]
        data = raw_bin[bv.get("byteOffset", 0):bv.get("byteOffset", 0) + bv["byteLength"]]
        off, ln = add(data)
        views.append(dict(buffer=0, byteOffset=off, byteLength=ln))
        images.append(dict(name=im.get("name", ""), mimeType=im["mimeType"], bufferView=len(views) - 1))
    binary = b"".join(chunks)
    binary += b"\0" * ((-len(binary)) % 4)
    gltf = dict(asset=dict(version="2.0", generator="VedatOnline gen_weapons.py --prepare (from Tripo output)"),
                scene=0, scenes=[dict(nodes=[0])], nodes=[dict(name="weapon", mesh=0)],
                meshes=[dict(name="weapon", primitives=[dict(attributes=dict(POSITION=1, NORMAL=2, TEXCOORD_0=3),
                                                              indices=0, material=0, mode=4)])],
                materials=raw_gltf["materials"], textures=raw_gltf["textures"], images=images,
                accessors=accessors, bufferViews=views, buffers=[dict(byteLength=len(binary))])
    js = json.dumps(gltf, separators=(",", ":"), sort_keys=True).encode("utf-8")
    js += b" " * ((-len(js)) % 4)
    out = struct.pack("<III", 0x46546C67, 2, 12 + 8 + len(js) + 8 + len(binary))
    out += struct.pack("<II", len(js), 0x4E4F534A) + js + struct.pack("<II", len(binary), 0x004E4942) + binary
    vc.ensure_dir(os.path.dirname(path))
    with open(path, "wb") as f:
        f.write(out)


def import_glb(path):
    before = set(bpy.data.objects)
    bpy.ops.import_scene.gltf(filepath=path)
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
        if o is not obj:
            bpy.data.objects.remove(o)
    return obj


def prepare(raw_dir, names):
    for name in names:
        src = WEAPONS[name]["src"]
        raw_path = os.path.join(raw_dir, f"{src}.glb")
        vc.reset_scene()
        obj = import_glb(raw_path)
        obj.data.calc_loop_triangles()
        n0 = len(obj.data.loop_triangles)
        bm = bmesh.new()
        bm.from_mesh(obj.data)
        bmesh.ops.remove_doubles(bm, verts=bm.verts, dist=gp.FLOAT_EPS * 0.01)  # raw units (≈1 m long)
        bm.to_mesh(obj.data)
        bm.free()
        n1 = gp.decimate(obj, PREP_TRIS)
        gp.triangulate(obj)
        raw_gltf, raw_bin = _read_glb(raw_path)
        verts, idx = _mesh_arrays(obj)
        out = os.path.join(SOURCE_DIR, f"{src}.glb")
        write_glb(out, verts, idx, raw_gltf, raw_bin)
        ref = os.path.join(raw_dir, "refs", f"{src}.png")
        if os.path.exists(ref):
            img = vc.read_image(ref)
            f = max(1, int(round(max(img.shape[:2]) / REF_SIZE)))
            small = gp.down_color(np.ascontiguousarray(img[:, :, :3]), img.shape[0] // f)
            vc.write_png(os.path.join(SOURCE_DIR, f"{src}_ref.png"), vc.to_u8(small))
        print(f"PREPARE {name}: {n0} -> {n1} tris, {len(verts)} verts, {os.path.getsize(out) / 1e6:.2f} MB")


# Per weapon (model name = icon name in gamedata/items.toml):
#   src       reduced source in source_meshes/weapons/   (all sources: ≈1 unit long along +Z, tip up)
#   up/front  source axes that become the grip frame's length (+Z Blender, engine +Y) and edge / face
#             (-Y Blender, engine +Z); `roll` degrees about the length afterwards
#   size      metres along the length (shield: diameter)
#   grip      gamedata/items.toml grip kind
#   grip_at   weapons: fraction of the length from the butt where the hand closes (middle of the
#             hand on the handle wrap); shields: metres from the back-most point at the disc centre
#             (the handle bar) to the grip origin (negative = towards the face)
#   tris      triangle budget; tex: map size (power of two)
#   icon      (yaw about the length, tilt towards the camera, roll in the picture) degrees
#   emissive  crystal glow: faces above z_min (m above the grip) whose colour passes `rule`
WEAPONS = {
    "sword_soldier": dict(src="sword_soldier", up="+z", front="-y", size=0.95, grip="sword", grip_at=0.175,
                          tris=2000, tex=512, icon=(70.0, -12.0, 45.0)),
    "shield_oak": dict(src="shield_oak", up="+z", front="+x", size=0.70, grip="shield", grip_at=-0.012,
                       tris=3000, tex=1024, icon=(-28.0, -10.0, 0.0)),
    "dagger_hunter": dict(src="dagger_hunter", up="+z", front="-y", size=0.45, grip="dagger", grip_at=0.22,
                          tris=1500, tex=512, icon=(70.0, -12.0, 45.0), handle_span=0.03),
    "staff_ember": dict(src="staff_ember", up="+z", front="-y", size=1.70, grip="staff", grip_at=0.42,
                        tris=3000, tex=1024, icon=(60.0, -10.0, 45.0),
                        emissive=dict(rule="ember", z_min=0.72, size=1024, strength=2.0,
                                      tint=(1.0, 0.42, 0.12), tint_mix=0.3)),
    "mace_sun": dict(src="mace_sun", up="+z", front="+x", size=0.60, grip="mace", grip_at=0.18,
                     tris=3000, tex=512, icon=(25.0, -12.0, 45.0)),
    "axe_hearth": dict(src="axe_hearth", up="+z", front="-y", size=1.40, grip="axe2h", grip_at=0.32,
                       tris=3000, tex=1024, icon=(80.0, -10.0, 45.0)),
    "sword_keep": dict(src="sword_keep", up="+z", front="-y", size=1.00, grip="sword", grip_at=0.19,
                       tris=2400, tex=512, icon=(70.0, -12.0, 45.0)),
    "staff_frost": dict(src="staff_frost", up="+z", front="-y", size=1.70, grip="staff", grip_at=0.42,
                        tris=3000, tex=1024, icon=(60.0, -10.0, 45.0),
                        emissive=dict(rule="frost", z_min=0.70, size=1024, strength=1.2,
                                      tint=(0.55, 0.8, 1.0), tint_mix=0.3)),
}
for _n, _c in WEAPONS.items():
    _c.setdefault("icon_name", _n)


# ---------------------------------------------------------------------------------------------
# Build: reduced source -> grip frame -> game mesh + maps

AXES = {"+x": (1, 0, 0), "-x": (-1, 0, 0), "+y": (0, 1, 0), "-y": (0, -1, 0), "+z": (0, 0, 1), "-z": (0, 0, -1)}


def verts_np(obj):
    return gp.verts_np(obj)


def orient(obj, cfg):
    """Source axes -> grip frame axes (Blender): cfg up -> +Z, cfg front -> -Y, then `roll` degrees
    about Z; uniform scale to `size` metres along Z (shields: the larger of the X / Z extents)."""
    u = np.array(AXES[cfg["up"]], float)
    f = np.array(AXES[cfg["front"]], float)
    rot = np.stack([np.cross(u, f), -f, u])
    m = Matrix([list(rot[0]) + [0.0], list(rot[1]) + [0.0], list(rot[2]) + [0.0], [0.0, 0.0, 0.0, 1.0]])
    m = Matrix.Rotation(math.radians(cfg.get("roll", 0.0)), 4, 'Z') @ m
    obj.data.transform(m)
    v = verts_np(obj)
    ext = v.max(0) - v.min(0)
    length = max(ext[0], ext[2]) if cfg["grip"] == "shield" else ext[2]
    s = cfg["size"] / length
    obj.data.transform(Matrix.Scale(s, 4))
    return s


def grip_point(obj, cfg):
    """Blender-space point that becomes the origin. Weapons: on the handle at `grip_at` (fraction of
    the length from the butt), x/y = centre of the handle's cross-section there. Shields: centre of
    the disc in X/Z, `grip_at` metres behind (+Y) the back face at the centre."""
    v = verts_np(obj)
    lo, hi = v.min(0), v.max(0)
    if cfg["grip"] == "shield":
        cx, cz = 0.5 * (lo[0] + hi[0]), 0.5 * (lo[2] + hi[2])
        near = v[(np.abs(v[:, 0] - cx) < 0.05) & (np.abs(v[:, 2] - cz) < 0.05)]
        return np.array([cx, float(np.percentile(near[:, 1], 98)) + cfg["grip_at"], cz])
    z = lo[2] + cfg["grip_at"] * (hi[2] - lo[2])
    slab = v[np.abs(v[:, 2] - z) < 0.015]
    c = 0.5 * (slab.min(0) + slab.max(0))
    return np.array([c[0], c[1], z])


def slab_centre(v, z, half=0.012):
    slab = v[np.abs(v[:, 2] - z) < half]
    return 0.5 * (slab.min(0) + slab.max(0))


def align_handle(obj, cfg):
    """Turns the weapon so the handle axis (centres of the handle cross-sections `handle_span`
    metres below and above the grip) is exactly +Z: the hand closes around that line, so it becomes
    the grip frame's length axis (a blade leaning off the handle, as on the dagger, keeps its lean).
    Returns the correction in degrees."""
    v = verts_np(obj)
    lo, hi = v[:, 2].min(), v[:, 2].max()
    zg = lo + cfg["grip_at"] * (hi - lo)
    span = cfg.get("handle_span", 0.05)
    a = slab_centre(v, zg - span)
    b = slab_centre(v, zg + span)
    axis = Vector(b - a).normalized()
    q = axis.rotation_difference(Vector((0.0, 0.0, 1.0)))
    obj.data.transform(q.to_matrix().to_4x4())
    return math.degrees(q.angle)


def sharpen(obj, degrees):
    """Smooth shading with edges sharper than `degrees` split (glTF export splits the normals)."""
    me = obj.data
    me.shade_smooth()
    me.set_sharp_from_angle(angle=math.radians(degrees))


def emissive_rule(rule, srgb, orm):
    r, g, b = srgb[..., 0], srgb[..., 1], srgb[..., 2]
    mx = np.maximum(np.maximum(r, g), b)
    mn = np.minimum(np.minimum(r, g), b)
    sat = (mx - mn) / np.maximum(mx, 1e-4)
    hue = np.degrees(np.arctan2(np.sqrt(3.0) * (g - b), 2 * r - g - b)) % 360.0
    if rule == "frost":  # pale ice blue crystal
        return (hue >= 175.0) & (hue <= 235.0) & (sat >= 0.12) & (mx >= 0.45)
    return gp.emissive_rule(rule, srgb, orm)


def write_textures(obj, name, cfg):
    col_img, nrm_img, orm_img = gp.source_images(obj)
    size = cfg["tex"]
    col_full = gp.pixels(col_img)
    orm_full = gp.pixels(orm_img)
    paths = dict(color=os.path.join(OUT_DIR, f"{name}.png"), normal=os.path.join(OUT_DIR, f"{name}_n.png"),
                 orm=os.path.join(OUT_DIR, f"{name}_orm.png"))
    vc.write_png(paths["color"], vc.to_u8(gp.down_color(col_full, size)))
    vc.write_png(paths["normal"], vc.to_u8(gp.down_normal(gp.pixels(nrm_img), size)))
    # Tripo's ORM image is wired to metallic-roughness only; its R channel is the occlusion.
    vc.write_png(paths["orm"], vc.to_u8(gp.box_down(orm_full, size)))
    info = dict(tex=size)
    em = cfg.get("emissive")
    if em is not None:
        esize = em["size"]
        ecol = gp.down_color(col_full, esize)
        eorm = gp.box_down(orm_full, esize)
        lin = gp.srgb_to_lin(ecol)
        faces = [p for p in obj.data.polygons if p.center[2] >= em["z_min"]]
        region = gp.rasterize_faces(obj, faces, esize)
        mask = region & emissive_rule(em["rule"], ecol, eorm)
        soft = mask.astype(np.float32)
        pad = np.pad(soft, 1, mode='edge')
        soft = np.maximum(soft, 0.5 * np.maximum.reduce([pad[:-2, 1:-1], pad[2:, 1:-1], pad[1:-1, :-2],
                                                         pad[1:-1, 2:]]))
        soft *= region
        glow = lin * (1.0 - em["tint_mix"]) + np.array(em["tint"], np.float32) * em["tint_mix"]
        emis = glow * soft[..., None]
        paths["emissive"] = os.path.join(OUT_DIR, f"{name}_e.png")
        vc.write_png(paths["emissive"], vc.to_u8(gp.lin_to_srgb(emis)))
        info.update(emissive_strength=em["strength"], emissive_texels=int(mask.sum()), emissive_faces=len(faces))
    for img in (col_img, nrm_img, orm_img):
        bpy.data.images.remove(img)
    return paths, info


def build(name, cfg):
    vc.reset_scene()
    obj = import_glb(os.path.join(SOURCE_DIR, f"{cfg['src']}.glb"))
    obj.data.calc_loop_triangles()
    src_tris = len(obj.data.loop_triangles)
    scale = orient(obj, cfg)
    report = gp.clean(obj, name)
    tilt = 0.0 if cfg["grip"] == "shield" else align_handle(obj, cfg)
    g = grip_point(obj, cfg)
    obj.data.transform(Matrix.Translation(Vector((-g[0], -g[1], -g[2]))))
    paths, tex_info = write_textures(obj, name, cfg)
    em = cfg.get("emissive")
    mat = gp.build_material(name, paths, em["strength"] if em else 0.0)
    obj.data.materials.clear()
    obj.data.materials.append(mat)
    for p in obj.data.polygons:
        p.material_index = 0
    gp.triangulate(obj)
    tris = gp.decimate(obj, cfg["tris"])
    obj.name = name
    obj.data.name = name
    sharpen(obj, SHARP_DEG)
    vc.export_glb(os.path.join(OUT_DIR, f"{name}.glb"), [obj], textures=True)
    v = verts_np(obj)
    lo, hi = v.min(0), v.max(0)
    meta = dict(source=f"{cfg['src']}.glb", source_tris=src_tris, tris=tris, grip=cfg["grip"],
                scale=round(float(scale), 5), grip_at=cfg["grip_at"], handle_tilt_deg=round(tilt, 2),
                # Engine axes (x = -X, y = Z, z = -Y), metres from the grip point.
                bounds_min=[round(float(-hi[0]), 3), round(float(lo[2]), 3), round(float(-hi[1]), 3)],
                bounds_max=[round(float(-lo[0]), 3), round(float(hi[2]), 3), round(float(-lo[1]), 3)],
                **{k: report[k] for k in ("shells", "welded", "flipped", "floating_removed", "open_edges")},
                **tex_info)
    print(f"WEAPON {name}: {src_tris} -> {tris} tris, size {np.round(hi - lo, 3)} m, grip {cfg['grip']} "
          f"at {cfg['grip_at']} (handle tilt {tilt:.1f} deg), below grip {-lo[2]:.3f} m, above {hi[2]:.3f} m, shells {report['shells']}, "
          f"flipped {report['flipped']}, floating {report['floating_removed']}, open edges {report['open_edges']}, "
          f"tex {cfg['tex']}" + (f", emissive {tex_info['emissive_texels']} texels" if em else ""))
    return obj, meta


# ---------------------------------------------------------------------------------------------
# Renders: icons and previews (Cycles on the CPU, fixed seed, stable writer)

def setup_cycles(resolution, samples, transparent, world="#2a2c30", world_strength=0.45):
    cam = vc.setup_preview(resolution, background=world, sun_dir=(-0.5, -1.0, 1.2), strength=2.4)
    sc = bpy.context.scene
    sc.render.engine = 'CYCLES'
    sc.cycles.device = 'CPU'
    sc.cycles.samples = samples
    sc.cycles.use_adaptive_sampling = False
    sc.cycles.seed = 0
    sc.cycles.use_animated_seed = False
    sc.cycles.use_denoising = False
    sc.cycles.filter_width = 1.5
    sc.cycles.max_bounces = 4
    sc.render.threads_mode = 'FIXED'
    sc.render.threads = 8
    sc.render.film_transparent = transparent
    sc.world.node_tree.nodes["Background"].inputs["Strength"].default_value = world_strength
    for nm, direction, energy, col in (("Fill", (1.0, -0.7, 0.3), 0.8, (0.8, 0.86, 1.0)),
                                       ("Rim", (0.3, 1.0, 0.9), 2.2, (1.0, 0.95, 0.88))):
        lt = bpy.data.lights.new(nm, 'SUN')
        lt.energy = energy
        lt.color = col
        lt.angle = math.radians(10)
        lt.use_shadow = False
        lo = vc.link(bpy.data.objects.new(nm, lt))
        lo.rotation_euler = Vector(direction).normalized().to_track_quat('Z', 'Y').to_euler()
    return cam


def render_rgba(path_tmp):
    sc = bpy.context.scene
    sc.render.filepath = path_tmp
    bpy.ops.render.render(write_still=True)
    img = vc.read_image(path_tmp)
    os.remove(path_tmp)
    return img


def write_png_stable_rgba(path, rgba, tolerance=2):
    """vo_common.write_png_stable for RGBA images (keeps an existing file within `tolerance`)."""
    if os.path.exists(path):
        old = vc.to_u8(vc.read_image(path))
        if old.shape == rgba.shape and int(np.abs(old.astype(np.int16) - rgba.astype(np.int16)).max()) <= tolerance:
            return False
    vc.write_png(path, rgba)
    return True


def display_matrix(cfg):
    """Icon pose of a grip-frame weapon: yaw about its length (show the flat / the face), tilt
    towards the camera, then roll so the length runs bottom-left -> top-right (shields stay up)."""
    yaw, tilt, roll = cfg["icon"]
    return (Matrix.Rotation(math.radians(roll), 4, 'Y') @ Matrix.Rotation(math.radians(tilt), 4, 'X')
            @ Matrix.Rotation(math.radians(yaw), 4, 'Z'))


def fit_camera(cam, objs, margin):
    """Orthographic camera on -Y looking +Y, framing every vertex with `margin` (fraction)."""
    pts = np.concatenate([np.array([o.matrix_world @ v.co for v in o.data.vertices]) for o in objs])
    lo, hi = pts.min(0), pts.max(0)
    c = 0.5 * (lo + hi)
    ext = max(hi[0] - lo[0], hi[2] - lo[2]) * (1.0 + 2.0 * margin)
    vc.aim_camera(cam, (c[0], lo[1] - 5.0, c[2]), (c[0], 0.0, c[2]), ortho_scale=ext)
    cam.data.clip_end = 20.0


def box_down_premul(rgba, f):
    """Box filter by an integer factor on linear, premultiplied colour; returns straight sRGB RGBA."""
    lin = gp.srgb_to_lin(rgba[..., :3]) * rgba[..., 3:4]
    h, w = rgba.shape[:2]
    pm = np.concatenate([lin, rgba[..., 3:4]], -1).reshape(h // f, f, w // f, f, 4).mean(axis=(1, 3))
    a = pm[..., 3:4]
    col = np.where(a > 1e-6, pm[..., :3] / np.maximum(a, 1e-6), 0.0)
    return np.concatenate([gp.lin_to_srgb(col), a], -1)


def add_shadow(rgba):
    """Soft dark halo behind the weapon (reads on any slot background): the alpha blurred twice by a
    3x3 box, shifted one texel down-right, 60 % black, composited under the icon."""
    a = rgba[..., 3]
    s = a
    for _ in range(2):
        p = np.pad(s, 1, mode='constant')
        s = sum(p[dy:dy + s.shape[0], dx:dx + s.shape[1]] for dy in range(3) for dx in range(3)) / 9.0
    s = np.roll(np.roll(s, 1, 0), 1, 1) * 0.6
    out_a = a + s * (1.0 - a)
    col = np.where(out_a[..., None] > 1e-6,
                   (gp.srgb_to_lin(rgba[..., :3]) * a[..., None]) / np.maximum(out_a[..., None], 1e-6), 0.0)
    return np.concatenate([gp.lin_to_srgb(col), out_a[..., None]], -1)


def render_icon(name, obj, cfg):
    sc = bpy.context.scene
    big = ICON_SIZE * ICON_SUPERSAMPLE
    cam = setup_cycles((big, big), 48, True, world="#9a9ca0", world_strength=0.7)
    obj.matrix_world = display_matrix(cfg)
    bpy.context.view_layer.update()
    fit_camera(cam, [obj], 0.04)
    rgba = render_rgba(os.path.join(ICON_DIR, f"_{name}.render.png"))
    icon = add_shadow(box_down_premul(rgba, ICON_SUPERSAMPLE))
    path = os.path.join(ICON_DIR, f"{cfg['icon_name']}.png")
    vc.ensure_dir(ICON_DIR)
    write_png_stable_rgba(path, vc.to_u8(icon))
    obj.matrix_world = Matrix.Identity(4)
    for o in list(bpy.data.objects):
        if o is not obj:
            bpy.data.objects.remove(o)
    sc.render.film_transparent = False
    return path


def render_preview(name, obj, cfg, icon_path):
    """Front (edge / face) and side views on a 10 cm grid with the grip origin marked, plus the icon."""
    cam = setup_cycles((256, 512), 32, False)
    v = verts_np(obj)
    lo, hi = v.min(0), v.max(0)
    line = vc.material("grid", "#3a3d43")
    for k in range(int(math.floor(lo[2] * 10)) - 1, int(math.ceil(hi[2] * 10)) + 2):
        bpy.ops.mesh.primitive_plane_add(size=1, location=(0, 0.6, k * 0.1))
        g = bpy.context.active_object
        g.scale = (4, 0.0015 if k else 0.004, 1)
        g.rotation_euler = (math.pi / 2, 0, 0)
        g.data.materials.append(vc.material("grid0", "#c03030") if k == 0 else line)
    tiles = []
    obj.rotation_mode = 'XYZ'
    width = float(max(hi[0] - lo[0], hi[1] - lo[1]))
    for k, rot in enumerate((0.0, 90.0)):
        obj.rotation_euler = (0, 0, math.radians(rot))
        c = 0.5 * (lo[2] + hi[2])
        vc.aim_camera(cam, (0, -4, c), (0, 0, c), ortho_scale=max(hi[2] - lo[2], 2.0 * width) * 1.08)
        tiles.append(vc.render_to(os.path.join(PREVIEW_DIR, f"_tmp_{name}_{k}.png")))
    obj.rotation_euler = (0, 0, 0)
    # The icon over the slot colour, scaled up 2x (nearest) into a 256 x 512 tile.
    icon = vc.read_image(icon_path)
    bg = np.array(vc.hex_linear("#23262b"))
    lin = gp.srgb_to_lin(icon[..., :3]) * icon[..., 3:4] + bg * (1.0 - icon[..., 3:4])
    tile = np.zeros((512, 256, 3), np.float32)
    tile[:] = gp.lin_to_srgb(bg)
    up = np.repeat(np.repeat(gp.lin_to_srgb(lin), 2, 0), 2, 1)
    tile[128:384, 0:256] = up
    tmp = os.path.join(PREVIEW_DIR, f"_tmp_{name}_icon.png")
    vc.write_png(tmp, vc.to_u8(tile))
    tiles.append(tmp)
    out = vc.compose_sheet(tiles, os.path.join(PREVIEW_DIR, f"{name}.png"), 3, stable=True)
    vc.remove_files(tiles)
    return out


def main():
    args = vc.script_args()
    names = list(WEAPONS)
    if "--only" in args:
        names = [n for n in names if n in set(args[args.index("--only") + 1].split(","))]
    if "--prepare" in args:
        prepare(args[args.index("--prepare") + 1], names)
        return
    meta_all = {}
    if os.path.exists(META_PATH):
        with open(META_PATH, "r", encoding="utf-8") as f:
            meta_all = json.load(f)
    shots = []
    for name in names:
        cfg = WEAPONS[name]
        obj, meta = build(name, cfg)
        meta_all[name] = meta
        if "--no-icons" not in args:
            icon = render_icon(name, obj, cfg)
            if "--no-preview" not in args:
                shots.append(render_preview(name, obj, cfg, icon))
    meta_all = {k: meta_all[k] for k in WEAPONS if k in meta_all}
    with open(META_PATH, "w", encoding="utf-8", newline="\n") as f:
        json.dump(meta_all, f, indent=1, sort_keys=True)
        f.write("\n")
    if len(shots) == len(WEAPONS):
        vc.compose_sheet(shots, os.path.join(PREVIEW_DIR, "weapons_sheet.png"), 4, stable=True)


if __name__ == "__main__":
    main()
