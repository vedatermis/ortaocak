"""Shared helpers for the VedatOnline Blender generators (run headless by Blender's bundled Python).

Conventions (docs/ASSET_PIPELINE.md):
  * Blender space: Z up, the asset faces -Y (Blender "front" view), +X is the asset's left side.
    The glTF exporter (+Y up) and assetc (X mirror) turn that into engine space (+Z forward, +X right).
  * Metres, origin at the ground below the asset's centre.
  * Colours are written as sRGB hex here and converted to linear for Blender/glTF.
  * Everything is deterministic: no time, no unseeded randomness; PNGs are written by write_png().
"""

import math
import os
import struct
import sys
import zlib

import bpy
import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.normpath(os.path.join(HERE, "..", ".."))
SRC = os.path.join(REPO, "assets_src")
PREVIEWS = os.path.join(SRC, "previews")


def script_args():
    """Arguments after `--` on the Blender command line."""
    argv = sys.argv
    return argv[argv.index("--") + 1:] if "--" in argv else []


def ensure_dir(path):
    os.makedirs(path, exist_ok=True)
    return path


# ---------------------------------------------------------------------------------------------
# Deterministic PNG writer (8-bit gray/RGB/RGBA or 16-bit gray), no metadata chunks, filtered rows.

def _filter_rows(rows, bpp):
    """PNG row filtering (lossless): per row the filter type (None, Sub, Up, Average, Paeth) with the
    smallest sum of |signed residuals| (the usual libpng heuristic; ties -> lowest type). Returns the
    filtered scanlines with their filter-type byte. Deterministic."""
    x = rows.astype(np.int16)
    a = np.zeros_like(x)
    a[:, bpp:] = x[:, :-bpp]
    b = np.zeros_like(x)
    b[1:] = x[:-1]
    c = np.zeros_like(x)
    c[1:, bpp:] = x[:-1, :-bpp]
    p = a + b - c
    pa, pb, pc = np.abs(p - a), np.abs(p - b), np.abs(p - c)
    paeth = np.where((pa <= pb) & (pa <= pc), a, np.where(pb <= pc, b, c))
    cands = np.stack([x, x - a, x - b, x - ((a + b) >> 1), x - paeth]) & 0xFF
    signed = np.where(cands > 127, cands - 256, cands)
    choice = np.argmin(np.abs(signed).sum(axis=2), axis=0)
    out = np.empty((rows.shape[0], rows.shape[1] + 1), np.uint8)
    out[:, 0] = choice
    out[:, 1:] = cands[choice, np.arange(rows.shape[0])]
    return out


def _filter_all(rows, bpp, kind):
    """Every row with the same PNG filter type (1 Sub, 2 Up, 4 Paeth)."""
    x = rows.astype(np.int16)
    a = np.zeros_like(x)
    a[:, bpp:] = x[:, :-bpp]
    b = np.zeros_like(x)
    b[1:] = x[:-1]
    if kind == 1:
        r = x - a
    elif kind == 2:
        r = x - b
    else:
        c = np.zeros_like(x)
        c[1:, bpp:] = x[:-1, :-bpp]
        p = a + b - c
        pa, pb, pc = np.abs(p - a), np.abs(p - b), np.abs(p - c)
        r = x - np.where((pa <= pb) & (pa <= pc), a, np.where(pb <= pc, b, c))
    out = np.empty((rows.shape[0], rows.shape[1] + 1), np.uint8)
    out[:, 0] = kind
    out[:, 1:] = r & 0xFF
    return out


def write_png(path, array, optimize=False):
    """optimize: also try a few whole-image filter choices and zlib strategies and keep the smallest
    stream (lossless, deterministic, slower; for the large character maps)."""
    a = np.asarray(array)
    if a.ndim == 2:
        a = a[:, :, None]
    h, w, c = a.shape
    if a.dtype == np.uint16:
        if c != 1:
            raise ValueError("16-bit PNG must be grayscale")
        bit_depth, color_type = 16, 0
        raw_rows = np.ascontiguousarray(a[:, :, 0].astype(">u2")).view(np.uint8).reshape(h, w * 2)
    else:
        a = a.astype(np.uint8)
        bit_depth = 8
        color_type = {1: 0, 2: 4, 3: 2, 4: 6}[c]
        raw_rows = a.reshape(h, w * c)
    bpp = (2 if bit_depth == 16 else 1) * c
    rows = np.ascontiguousarray(raw_rows)
    data = zlib.compress(_filter_rows(rows, bpp).tobytes(), 9)
    if optimize:
        for kind in (1, 2, 4):
            raw = _filter_all(rows, bpp, kind).tobytes()
            for strategy in (zlib.Z_DEFAULT_STRATEGY, zlib.Z_FILTERED):
                co = zlib.compressobj(9, zlib.DEFLATED, 15, 9, strategy)
                cand = co.compress(raw) + co.flush()
                if len(cand) < len(data):
                    data = cand

    def chunk(tag, data):
        return struct.pack(">I", len(data)) + tag + data + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)

    png = b"\x89PNG\r\n\x1a\n"
    png += chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, bit_depth, color_type, 0, 0, 0))
    png += chunk(b"IDAT", data)
    png += chunk(b"IEND", b"")
    ensure_dir(os.path.dirname(path))
    with open(path, "wb") as f:
        f.write(png)


def read_image(path):
    """Loads an image through Blender; returns float32 RGBA array, top row first."""
    img = bpy.data.images.load(path, check_existing=False)
    w, h = img.size
    px = np.empty(w * h * 4, np.float32)
    img.pixels.foreach_get(px)
    bpy.data.images.remove(img)
    return px.reshape(h, w, 4)[::-1].copy()


def to_u8(rgba_float):
    return (np.clip(rgba_float, 0.0, 1.0) * 255.0 + 0.5).astype(np.uint8)


# ---------------------------------------------------------------------------------------------
# Colours and materials

def srgb_to_linear(c):
    return c / 12.92 if c <= 0.04045 else ((c + 0.055) / 1.055) ** 2.4


def hex_linear(hexstr):
    h = hexstr.lstrip("#")
    return tuple(srgb_to_linear(int(h[i:i + 2], 16) / 255.0) for i in (0, 2, 4))


def material(name, hexstr, roughness=0.85):
    """Flat-coloured Principled material (exported as glTF baseColorFactor; assetc bakes it into
    the vertex colours)."""
    mat = bpy.data.materials.get(name)
    if mat is None:
        mat = bpy.data.materials.new(name)
        mat.use_nodes = True
    bsdf = mat.node_tree.nodes.get("Principled BSDF")
    r, g, b = hex_linear(hexstr)
    bsdf.inputs["Base Color"].default_value = (r, g, b, 1.0)
    bsdf.inputs["Roughness"].default_value = roughness
    bsdf.inputs["Metallic"].default_value = 0.0
    mat.diffuse_color = (r, g, b, 1.0)
    # The engine culls back faces; previews do too, so inverted faces show up as holes.
    mat.use_backface_culling = True
    return mat


# ---------------------------------------------------------------------------------------------
# Scene / objects

def reset_scene():
    bpy.ops.wm.read_factory_settings(use_empty=True)
    sc = bpy.context.scene
    sc.render.fps = 30
    sc.unit_settings.system = 'METRIC'
    return sc


def link(obj):
    bpy.context.scene.collection.objects.link(obj)
    return obj


def mesh_object(name, verts, faces, face_mats, materials, smooth=False, sharp_angle=None, recalc=False):
    """Creates a mesh object from Python lists. face_mats: material index per face. Faces must be
    wound counter-clockwise seen from outside (Blender convention); `recalc` lets Blender fix
    closed meshes instead."""
    me = bpy.data.meshes.new(name)
    me.from_pydata([tuple(v) for v in verts], [], [tuple(f) for f in faces])
    me.validate(clean_customdata=False)
    for m in materials:
        me.materials.append(m)
    if face_mats is not None:
        me.polygons.foreach_set("material_index", list(face_mats))
    me.update()
    obj = link(bpy.data.objects.new(name, me))
    if recalc:
        import bmesh
        bm = bmesh.new()
        bm.from_mesh(me)
        bmesh.ops.recalc_face_normals(bm, faces=bm.faces)
        bm.to_mesh(me)
        bm.free()
    if smooth:
        me.shade_smooth()
        if sharp_angle is not None:
            me.set_sharp_from_angle(angle=math.radians(sharp_angle))
    else:
        me.shade_flat()
    me.update()
    return obj


def triangle_count(obj):
    return sum(len(p.vertices) - 2 for p in obj.data.polygons)


def export_glb(path, objects, vertex_colors=False, textures=False):
    """Exports exactly `objects` (plus nothing else) as a binary glTF with fixed options.
    vertex_colors: also export each mesh's active colour attribute as COLOR_0 (assetc multiplies it
    with the material's base colour). textures: reference image files by relative URI (they must lie
    under assets_src; assetc rejects embedded images) instead of exporting no images."""
    ensure_dir(os.path.dirname(path))
    bpy.ops.object.select_all(action='DESELECT')
    for o in objects:
        o.select_set(True)
    bpy.ops.export_scene.gltf(
        filepath=path,
        export_format='GLB',
        use_selection=True,
        export_yup=True,
        export_apply=False,
        export_texcoords=True,
        export_normals=True,
        export_tangents=False,
        export_materials='EXPORT',
        export_vertex_color='ACTIVE' if vertex_colors else 'NONE',
        export_all_vertex_colors=not vertex_colors,
        export_cameras=False,
        export_lights=False,
        export_extras=False,
        export_skins=True,
        export_influence_nb=4,
        export_def_bones=False,
        export_leaf_bone=False,
        export_animations=True,
        export_animation_mode='ACTIONS',
        export_force_sampling=True,
        export_frame_step=1,
        # Drops keys equal to both neighbours (linear: the curves sample identically in assetc) and
        # constant channels; halves a character's file size.
        export_optimize_animation_size=True,
        export_reset_pose_bones=True,
        export_rest_position_armature=True,
        export_morph=False,
        export_image_format='AUTO' if textures else 'NONE',
        export_keep_originals=textures,
    )


# ---------------------------------------------------------------------------------------------
# Preview rendering (EEVEE, Standard view transform so the palette shows as authored)

def setup_preview(resolution=(512, 512), background="#8fa7b8", sun_dir=(0.6, 0.9, 1.4), strength=3.2):
    sc = bpy.context.scene
    sc.render.engine = 'BLENDER_EEVEE'
    sc.render.resolution_x, sc.render.resolution_y = resolution
    sc.render.resolution_percentage = 100
    sc.render.film_transparent = False
    sc.render.dither_intensity = 0.0  # no dither noise: previews compress ~2x better
    sc.render.image_settings.file_format = 'PNG'
    sc.render.image_settings.color_mode = 'RGBA'
    sc.view_settings.view_transform = 'Standard'
    sc.view_settings.look = 'None'
    world = bpy.data.worlds.new("PreviewWorld")
    world.use_nodes = True
    bg = world.node_tree.nodes.get("Background")
    r, g, b = hex_linear(background)
    bg.inputs["Color"].default_value = (r, g, b, 1.0)
    bg.inputs["Strength"].default_value = 0.9
    sc.world = world
    sun = bpy.data.lights.new("PreviewSun", 'SUN')
    sun.energy = strength
    sun.angle = math.radians(8)
    so = link(bpy.data.objects.new("PreviewSun", sun))
    d = np.array(sun_dir, float)
    d /= np.linalg.norm(d)
    # Sun points along its local -Z; aim it from direction d towards the origin.
    from mathutils import Vector
    so.rotation_euler = Vector(d).to_track_quat('Z', 'Y').to_euler()
    cam = bpy.data.cameras.new("PreviewCam")
    co = link(bpy.data.objects.new("PreviewCam", cam))
    sc.camera = co
    return co


def aim_camera(cam_obj, location, target, ortho_scale=None, lens=50.0):
    from mathutils import Vector
    cam_obj.location = Vector(location)
    direction = Vector(target) - Vector(location)
    cam_obj.rotation_euler = direction.to_track_quat('-Z', 'Y').to_euler()
    if ortho_scale is not None:
        cam_obj.data.type = 'ORTHO'
        cam_obj.data.ortho_scale = ortho_scale
    else:
        cam_obj.data.type = 'PERSP'
        cam_obj.data.lens = lens
    cam_obj.data.clip_start = 0.05
    cam_obj.data.clip_end = 5000.0


def render_to(path):
    sc = bpy.context.scene
    ensure_dir(os.path.dirname(path))
    sc.render.filepath = path
    bpy.ops.render.render(write_still=True)
    # Re-encode with the deterministic writer (Blender's PNG output carries metadata that changes
    # between runs; the pixels do not).
    write_png(path, to_u8(read_image(path))[:, :, :3])
    return path


def compose_sheet(paths, out_path, columns, labels=None, pad=4, background=(40, 44, 52)):
    """Tiles rendered PNGs into one sheet (deterministic writer)."""
    tiles = [to_u8(read_image(p))[:, :, :3] for p in paths]
    th, tw = tiles[0].shape[:2]
    rows = (len(tiles) + columns - 1) // columns
    sheet = np.zeros((rows * (th + pad) + pad, columns * (tw + pad) + pad, 3), np.uint8)
    sheet[:] = background
    for i, t in enumerate(tiles):
        r, c = divmod(i, columns)
        y, x = pad + r * (th + pad), pad + c * (tw + pad)
        sheet[y:y + th, x:x + tw] = t
    write_png(out_path, sheet)
    return out_path


def remove_files(paths):
    for p in paths:
        try:
            os.remove(p)
        except OSError:
            pass


# ---------------------------------------------------------------------------------------------
# Geometry helpers

def ring_frame(direction, forward=(0.0, -1.0, 0.0)):
    """Orthonormal (side, front) axes perpendicular to `direction`, with `front` as close to
    `forward` as possible."""
    d = np.asarray(direction, float)
    d = d / np.linalg.norm(d)
    f = None
    for ref in (forward, (0.0, -1.0, 0.0), (1.0, 0.0, 0.0)):
        cand = np.asarray(ref, float)
        cand = cand - d * np.dot(cand, d)
        if np.linalg.norm(cand) > 1e-3:
            f = cand / np.linalg.norm(cand)
            break
    s = np.cross(f, d)
    s /= np.linalg.norm(s)
    return s, f


class MeshBuilder:
    """Accumulates vertices/faces/material indices/bone weights for one mesh object."""

    def __init__(self):
        self.verts = []
        self.faces = []
        self.mats = []
        self.weights = []  # per vertex: dict bone -> weight

    def add_vertex(self, p, weights):
        self.verts.append(tuple(float(x) for x in p))
        self.weights.append(dict(weights))
        return len(self.verts) - 1

    def add_face(self, idx, mat):
        self.faces.append(tuple(idx))
        self.mats.append(mat)

    def loft(self, rings, mat_per_segment, cap_start=True, cap_end=True, cap_mats=None):
        """rings: list of (points[n], weights_dict); consecutive rings are joined with quads."""
        n = len(rings[0][0])
        idx = []
        for pts, w in rings:
            idx.append([self.add_vertex(p, w) for p in pts])
        for r in range(len(rings) - 1):
            m = mat_per_segment[r] if isinstance(mat_per_segment, (list, tuple)) else mat_per_segment
            for k in range(n):
                k2 = (k + 1) % n
                self.add_face((idx[r][k], idx[r][k2], idx[r + 1][k2], idx[r + 1][k]), m)
        cm = cap_mats or (None, None)
        if cap_start:
            m0 = cm[0] if cm[0] is not None else (mat_per_segment[0] if isinstance(mat_per_segment, (list, tuple)) else mat_per_segment)
            self.add_face(tuple(reversed(idx[0])), m0)
        if cap_end:
            m1 = cm[1] if cm[1] is not None else (mat_per_segment[-1] if isinstance(mat_per_segment, (list, tuple)) else mat_per_segment)
            self.add_face(tuple(idx[-1]), m1)
        return idx

    def ellipsoid(self, center, radii, segments, rings, mat, weights, shape=None, mat_fn=None):
        """Low-poly UV ellipsoid. shape(p_unit) -> scale factors (sx, sy, sz) for local tweaks;
        mat_fn(p_unit) -> material index per face (by face centre)."""
        c = np.asarray(center, float)
        top = None
        ring_idx = []
        pts_unit = []
        for i in range(1, rings):
            theta = math.pi * i / rings
            row = []
            for j in range(segments):
                phi = 2 * math.pi * j / segments
                u = np.array([math.sin(theta) * math.cos(phi), math.sin(theta) * math.sin(phi), math.cos(theta)])
                s = np.ones(3) if shape is None else np.asarray(shape(u), float)
                p = c + u * np.asarray(radii) * s
                row.append(self.add_vertex(p, weights))
                pts_unit.append(u)
            ring_idx.append(row)
        top_u = np.array([0, 0, 1.0])
        bot_u = np.array([0, 0, -1.0])
        st = np.ones(3) if shape is None else np.asarray(shape(top_u), float)
        sb = np.ones(3) if shape is None else np.asarray(shape(bot_u), float)
        top = self.add_vertex(c + top_u * np.asarray(radii) * st, weights)
        bot = self.add_vertex(c + bot_u * np.asarray(radii) * sb, weights)

        def face_mat(ids):
            if mat_fn is None:
                return mat
            cen = np.mean([np.asarray(self.verts[i]) for i in ids], axis=0) - c
            return mat_fn(cen / np.asarray(radii))

        for j in range(segments):
            j2 = (j + 1) % segments
            f = (top, ring_idx[0][j], ring_idx[0][j2])
            self.add_face(f, face_mat(f))
            f = (bot, ring_idx[-1][j2], ring_idx[-1][j])
            self.add_face(f, face_mat(f))
        for i in range(len(ring_idx) - 1):
            for j in range(segments):
                j2 = (j + 1) % segments
                f = (ring_idx[i][j], ring_idx[i + 1][j], ring_idx[i + 1][j2], ring_idx[i][j2])
                self.add_face(f, face_mat(f))

    def box(self, center, size, mat, weights, rot_z=0.0, taper_top=1.0):
        cx, cy, cz = center
        sx, sy, sz = (s * 0.5 for s in size)
        cr, sr = math.cos(rot_z), math.sin(rot_z)
        ids = []
        for z in (-sz, sz):
            k = taper_top if z > 0 else 1.0
            for (x, y) in ((-sx, -sy), (sx, -sy), (sx, sy), (-sx, sy)):
                x2, y2 = x * k, y * k
                ids.append(self.add_vertex((cx + x2 * cr - y2 * sr, cy + x2 * sr + y2 * cr, cz + z), weights))
        b = ids
        for f in ((b[0], b[3], b[2], b[1]), (b[4], b[5], b[6], b[7]), (b[0], b[1], b[5], b[4]),
                  (b[1], b[2], b[6], b[5]), (b[2], b[3], b[7], b[6]), (b[3], b[0], b[4], b[7])):
            self.add_face(f, mat)

    def mark(self):
        return len(self.verts)

    def transform_since(self, start, location=(0, 0, 0), rot_z=0.0, rot_x=0.0, rot_y=0.0, scale=1.0):
        """Applies scale, then rotations X, Y, Z (radians), then translation to vertices added since
        `start` (rotations keep the winding; negative scales are not allowed)."""
        from mathutils import Matrix, Vector
        m = (Matrix.Translation(Vector(location)) @ Matrix.Rotation(rot_z, 4, 'Z') @ Matrix.Rotation(rot_y, 4, 'Y')
             @ Matrix.Rotation(rot_x, 4, 'X') @ Matrix.Scale(scale, 4))
        for i in range(start, len(self.verts)):
            self.verts[i] = tuple(m @ Vector(self.verts[i]))

    def cylinder(self, base, r_bottom, r_top, height, sides, mat, weights=None, cap=True, phase=0.0, ry_scale=1.0):
        """Vertical (tapered) cylinder standing on `base`."""
        rings = []
        for z, rr in ((0.0, r_bottom), (height, r_top)):
            pts = []
            for k in range(sides):
                th = 2 * math.pi * k / sides + phase
                pts.append((base[0] + rr * math.cos(th), base[1] + rr * ry_scale * math.sin(th), base[2] + z))
            rings.append((pts, weights or {}))
        self.loft(rings, mat, cap_start=cap, cap_end=cap)

    def profile(self, base, radii_heights, sides, mats, cap=(True, True), phase=0.0):
        """Lathe: list of (radius, z) from bottom to top around a vertical axis at `base`."""
        rings = []
        for rr, z in radii_heights:
            pts = [(base[0] + rr * math.cos(2 * math.pi * k / sides + phase),
                    base[1] + rr * math.sin(2 * math.pi * k / sides + phase), base[2] + z) for k in range(sides)]
            rings.append((pts, {}))
        self.loft(rings, mats, cap_start=cap[0], cap_end=cap[1])

    def build(self, name, materials, smooth=False, sharp_angle=None, armature=None):
        obj = mesh_object(name, self.verts, self.faces, self.mats, materials, smooth, sharp_angle)
        if armature is not None:
            groups = {}
            for vi, w in enumerate(self.weights):
                total = sum(w.values())
                for bone, val in w.items():
                    if val <= 0.0:
                        continue
                    g = groups.get(bone)
                    if g is None:
                        g = groups[bone] = obj.vertex_groups.new(name=bone)
                    g.add([vi], val / total, 'REPLACE')
            obj.parent = armature
            mod = obj.modifiers.new("Armature", 'ARMATURE')
            mod.object = armature
        return obj


class Rng:
    """Small deterministic PRNG (numpy Generator with a fixed seed)."""

    def __init__(self, seed):
        self.g = np.random.default_rng(seed)

    def uniform(self, a=0.0, b=1.0):
        return float(self.g.uniform(a, b))

    def choice(self, seq):
        return seq[int(self.g.integers(0, len(seq)))]
