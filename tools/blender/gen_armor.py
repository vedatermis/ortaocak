"""Wearable armour (M17, second pass): skinned armour pieces fitted to every character look.

Usage (headless, after gen_character.py):
  blender --background --factory-startup --python tools/blender/gen_armor.py -- [--only aydor_0,...]
          [--pieces heavy_a_chest,...] [--sets heavy_a,cloth_b] [--no-preview] [--no-icons]

Output, per look L (aydor_0..2, kargan_0..2), line (heavy, leather, cloth), shape (a = tiers 1-3, b = tiers
4-5 and the unique pieces) and slot (head, chest, legs, hands, feet):
  assets_src/armor/<L>/<line>_<shape>_<slot>.glb      skinned to the look's own skeleton (the character
                                                      glb's joint nodes and inverse bind matrices copied
                                                      verbatim, so the body's bone palette drives it)
  assets_src/armor/textures/<stem>_{c,n,orm}.png       tiling detail maps per material (armor_textures.py)
  assets_src/ui/items/armor_<line>_<slot>.png          15 inventory icons (shape a, rendered on aydor_0)
  assets_src/previews/armor/<L>.png                    every line/shape on the look, front / back / side and
                                                      in run / attack / sit / death poses
  assets_src/armor/armor_meta.json                     triangles per piece

How a piece is made (all in Blender axes, the look's bind pose; the character faces -Y):
  * PROXY VOLUMES instead of copies of the skin. The Tripo bodies carry deep relief (Kargan fur, scale
    skirts, straps, pouches); a surface copied from them comes out lumpy. Torso, limbs, hips and feet are
    LOFTS: rings around an axis (spine, a bone, the foot) whose radius per angle is fitted to the body
    region: per ring a robust radius per angular sector (a percentile, so tufts and buckles do not count),
    a low-order Fourier fit over the angle (a smooth, convex-ish section), smoothed along the axis, then
    raised as a whole until nearly all of the region's skin is inside, plus the line's margin. Where
    helper geometry (cloaks, fur mantles, beards, braids, aprons) lies over the region, the loft is
    tucked under it (pulled in, smoothly, at most a few cm), so those stay on top.
  * Shoulders, knees, elbows and heads are DOMES (star-shaped hulls around a centre, smoothed); hands are
    SHELLS of the hand skin whose source surface is smoothed first (fingers need the real topology).
  * Every vertex takes the bone weights of the nearest skin point of its region (barycentric), smoothed
    over the loft grid, so a piece deforms like the skin under it (no tearing between bones).
  * Detail is real geometry where it shapes the silhouette (plate lames, flutes, ridges, rolled edges,
    rivets, buckles, crests, cheek guards, flaps) and texture where it is fine: every surface carries UVs
    in metres (u around, v along) for its line's tiling detail maps (brushed steel, grained leather with
    stitched panels, woven cloth) and strips (rolled steel trim with rivets, stitched straps, embroidered
    bands) carry strip UVs. Vertex colours hold the light base colour, darker recesses and trims; the
    item `tint` multiplies everything per tier.

Deterministic: fixed operations, own glb writer (sorted JSON), Cycles with a fixed seed for renders.
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
from mathutils.bvhtree import BVHTree

sys.dont_write_bytecode = True
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import armor_textures as atex  # noqa: E402
import vo_common as vc  # noqa: E402

CHAR_DIR = os.path.join(vc.SRC, "characters")
OUT_DIR = os.path.join(vc.SRC, "armor")
TEX_DIR = os.path.join(OUT_DIR, "textures")
ICON_DIR = os.path.join(vc.SRC, "ui", "items")
PREVIEW_DIR = os.path.join(vc.PREVIEWS, "armor")
META_PATH = os.path.join(OUT_DIR, "armor_meta.json")

LOOKS = ("aydor_0", "aydor_1", "aydor_2", "kargan_0", "kargan_1", "kargan_2")
LINES = ("heavy", "leather", "cloth")
SHAPES = ("a", "b")
SLOTS = ("head", "chest", "legs", "hands", "feet")

# Bones whose skin stays on top of the armour (cloaks, fur mantles, tabard flaps, braids, beards).
HELPERS = ("cloth_back", "cloth_front", "mantle", "beard", "braid_l", "braid_r")
FINGERS = tuple(f"{f}_0{k}_{s}" for f in ("index", "middle", "ring", "pinky", "thumb") for k in (1, 2, 3)
                for s in ("l", "r"))
TORSO = ("pelvis", "spine_01", "spine_02", "spine_03", "clavicle_l", "clavicle_r")


def side_bones(*names):
    return tuple(f"{n}_{s}" for n in names for s in ("l", "r"))


ARMS = side_bones("upperarm", "lowerarm", "hand") + FINGERS
LEGS = side_bones("thigh", "calf", "foot", "ball")


# ---------------------------------------------------------------------------------------------
# glTF in / out

def read_glb(path):
    with open(path, "rb") as f:
        data = f.read()
    jlen = struct.unpack_from("<I", data, 12)[0]
    g = json.loads(data[20:20 + jlen].decode("utf-8"))
    off = 20 + jlen
    blen = struct.unpack_from("<I", data, off)[0]
    return g, data[off + 8:off + 8 + blen]


def accessor(g, binary, idx):
    acc = g["accessors"][idx]
    bv = g["bufferViews"][acc["bufferView"]]
    comp = {5126: "<f4", 5121: "u1", 5123: "<u2", 5125: "<u4"}[acc["componentType"]]
    n = {"SCALAR": 1, "VEC2": 2, "VEC3": 3, "VEC4": 4, "MAT4": 16}[acc["type"]]
    off = bv.get("byteOffset", 0) + acc.get("byteOffset", 0)
    stride = bv.get("byteStride", 0)
    itemsize = np.dtype(comp).itemsize * n
    if stride and stride != itemsize:
        raw = np.frombuffer(binary, np.uint8, count=stride * acc["count"], offset=off).reshape(-1, stride)[:, :itemsize]
        a = np.frombuffer(raw.tobytes(), comp).reshape(acc["count"], n)
    else:
        a = np.frombuffer(binary, comp, count=n * acc["count"], offset=off).reshape(acc["count"], n)
    if acc.get("normalized"):
        a = a.astype(np.float32) / {"u1": 255.0, "<u2": 65535.0}.get(comp, 1.0)
    return a


def gl_to_bl(a):
    a = np.asarray(a, np.float64)
    return np.stack([a[..., 0], -a[..., 2], a[..., 1]], -1)


def bl_to_gl(a):
    a = np.asarray(a, np.float64)
    return np.stack([a[..., 0], a[..., 2], -a[..., 1]], -1)


class Body:
    """The look's skinned body in Blender axes (bind pose): welded positions, triangles, normals,
    per-vertex bone weights (skin joint index -> weight) and the joint positions."""

    def __init__(self, look):
        self.look = look
        self.path = os.path.join(CHAR_DIR, f"{look}.glb")
        self.gltf, self.bin = read_glb(self.path)
        g = self.gltf
        skin = g["skins"][0]
        self.jnames = [g["nodes"][j]["name"] for j in skin["joints"]]
        self.jindex = {n: k for k, n in enumerate(self.jnames)}
        ibm = accessor(g, self.bin, skin["inverseBindMatrices"]).reshape(-1, 4, 4).transpose(0, 2, 1)
        self.joint = {n: gl_to_bl(np.linalg.inv(ibm[k])[:3, 3]) for k, n in enumerate(self.jnames)}
        prim = g["meshes"][0]["primitives"][0]
        at = prim["attributes"]
        P = gl_to_bl(accessor(g, self.bin, at["POSITION"]))
        J = accessor(g, self.bin, at["JOINTS_0"]).astype(np.int64)
        W = accessor(g, self.bin, at["WEIGHTS_0"]).astype(np.float64)
        idx = accessor(g, self.bin, prim["indices"]).reshape(-1, 3).astype(np.int64)
        # Weld the glTF vertices split at UV seams / normal creases (weights are equal there).
        key = np.round(P / 1e-5).astype(np.int64)
        _, first, inv = np.unique(key, axis=0, return_index=True, return_inverse=True)
        inv = inv.reshape(-1)
        self.P = P[first]
        self.J = J[first]
        self.W = W[first] / np.maximum(W[first].sum(1, keepdims=True), 1e-9)
        T = inv[idx]
        T = T[(T[:, 0] != T[:, 1]) & (T[:, 1] != T[:, 2]) & (T[:, 0] != T[:, 2])]
        self.T = T
        e1 = self.P[T[:, 1]] - self.P[T[:, 0]]
        e2 = self.P[T[:, 2]] - self.P[T[:, 0]]
        fn = np.cross(e1, e2)
        vn = np.zeros_like(self.P)
        for k in range(3):
            np.add.at(vn, T[:, k], fn)
        self.N = vn / np.maximum(np.linalg.norm(vn, axis=1, keepdims=True), 1e-12)
        self.height = float(self.P[:, 2].max())
        self.scale = self.height / 1.77
        self.kargan = look.startswith("kargan")
        self.floor = float(self.P[self.rw(side_bones("foot", "ball")) > 0.5][:, 2].min())
        self._bvh = {}

    def j(self, name):
        return np.array(self.joint[name], float)

    def rw(self, names):
        """Per-vertex summed weight of the given bones."""
        ids = [self.jindex[n] for n in names if n in self.jindex]
        m = np.isin(self.J, ids)
        return (self.W * m).sum(1)

    def mask(self, bones, thr=0.5, exclude=HELPERS, exclude_thr=0.3):
        m = self.rw(bones) >= thr
        if exclude:
            m &= self.rw(exclude) < exclude_thr
        return m

    def faces_of(self, names, thr=0.5, exclude=HELPERS, exclude_thr=0.3):
        ok = self.mask(names, thr, exclude, exclude_thr)
        return ok[self.T].sum(1) >= 2

    def helpers(self, names=HELPERS):
        return self.P[self.rw(names) >= 0.5]

    def bvh(self, fmask_key, fmask):
        if fmask_key not in self._bvh:
            tris = self.T[fmask]
            tree = BVHTree.FromPolygons([tuple(p) for p in self.P], [tuple(int(i) for i in t) for t in tris])
            self._bvh[fmask_key] = (tree, tris)
        return self._bvh[fmask_key]

    def weights_at(self, tree_tris, points):
        """Bone weights (list of {joint index: w}) interpolated at the nearest surface point."""
        tree, tris = tree_tris
        out = []
        for p in points:
            loc, _n, fi, _d = tree.find_nearest(Vector(p))
            t = tris[fi]
            a, b, c = self.P[t[0]], self.P[t[1]], self.P[t[2]]
            bc = barycentric(np.array(loc), a, b, c)
            acc = {}
            for k in range(3):
                for jj, ww in zip(self.J[t[k]], self.W[t[k]]):
                    if ww > 0:
                        acc[int(jj)] = acc.get(int(jj), 0.0) + bc[k] * ww
            out.append(acc)
        return out


def barycentric(p, a, b, c):
    v0, v1, v2 = b - a, c - a, p - a
    d00, d01, d11 = v0 @ v0, v0 @ v1, v1 @ v1
    d20, d21 = v2 @ v0, v2 @ v1
    den = d00 * d11 - d01 * d01
    if abs(den) < 1e-18:
        return np.array([1.0, 0.0, 0.0])
    v = (d11 * d20 - d01 * d21) / den
    w = (d00 * d21 - d01 * d20) / den
    bc = np.clip(np.array([1.0 - v - w, v, w]), 0.0, 1.0)
    return bc / bc.sum()


def top4(wd):
    items = sorted(((w, j) for j, w in wd.items() if w > 1e-4), reverse=True)[:4]
    s = sum(w for w, _ in items) or 1.0
    return [(j, w / s) for w, j in items]


# Materials: (texture stem or None, roughness factor, metallic factor, normal scale). Textured materials take
# roughness / metal from their ORM map (factors 1); the others are flat.
MATERIALS = {
    "steel": ("steel", 1.0, 1.0, 1.0),
    "trim": ("trim", 1.0, 1.0, 1.0),
    "leather": ("leather", 1.0, 1.0, 1.0),
    "strap": ("strap", 1.0, 1.0, 1.0),
    "cloth": ("cloth", 1.0, 1.0, 1.0),
    "band": ("band", 1.0, 1.0, 1.0),
    "gold": (None, 0.3, 1.0, 0.0),
    "iron": (None, 0.48, 1.0, 0.0),
    "gem": (None, 0.15, 0.0, 0.0),
}


def unorm(a, bits):
    m = (1 << bits) - 1
    return np.clip(np.round(np.asarray(a) * m), 0, m).astype("<u2" if bits == 16 else "u1")


def quant_weights(W):
    """Weights (n,4) -> u8 normalised that sum to exactly 255 per vertex (the largest takes the rest)."""
    q = np.clip(np.round(W * 255.0), 0, 255).astype(np.int64)
    big = np.argmax(W, 1)
    q[np.arange(len(q)), big] += 255 - q.sum(1)
    return np.clip(q, 0, 255).astype("u1")


def write_piece_glb(path, body, name, prims):
    """prims: list of (material name, positions (n,3) Blender, normals (n,3), uvs (n,2), colours (n,4) linear,
    joints (n,4) skin joint indices, weights (n,4), indices (m,)). The skeleton (joint nodes, skin, inverse
    bind matrices) is the character glb's, copied verbatim. Compact attributes: colours u16, weights u8
    (normalised), indices u16 where they fit."""
    src = body.gltf
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

    def add_acc(arr, comp, typ, target=None, minmax=False, normalized=False):
        off, ln = add(arr.tobytes())
        v = dict(buffer=0, byteOffset=off, byteLength=ln)
        if target:
            v["target"] = target
        views.append(v)
        a = dict(bufferView=len(views) - 1, componentType=comp, count=int(arr.shape[0]),
                 type={1: "SCALAR", 2: "VEC2", 3: "VEC3", 4: "VEC4", 16: "MAT4"}[1 if arr.ndim == 1 else arr.shape[1]])
        if normalized:
            a["normalized"] = True
        if minmax:
            a["min"] = [float(x) for x in arr.min(0)]
            a["max"] = [float(x) for x in arr.max(0)]
        accessors.append(a)
        return len(accessors) - 1

    # Inverse bind matrices: the original bytes.
    skin = src["skins"][0]
    ibm_acc = src["accessors"][skin["inverseBindMatrices"]]
    bv = src["bufferViews"][ibm_acc["bufferView"]]
    o = bv.get("byteOffset", 0) + ibm_acc.get("byteOffset", 0)
    off, ln = add(body.bin[o:o + 64 * ibm_acc["count"]])
    views.append(dict(buffer=0, byteOffset=off, byteLength=ln))
    accessors.append(dict(bufferView=len(views) - 1, componentType=5126, count=ibm_acc["count"], type="MAT4"))
    ibm_index = len(accessors) - 1

    images, textures, materials, primitives = [], [], [], []

    def tex_index(uri):
        if uri not in [im["uri"] for im in images]:
            images.append(dict(uri=uri))
            textures.append(dict(sampler=0, source=len(images) - 1))
        return [im["uri"] for im in images].index(uri)

    for mname, P, N, UV, C, Jt, Wt, I in prims:
        if mname not in [m["name"] for m in materials]:
            stem, r, m, ns = MATERIALS[mname]
            mat = dict(name=mname, pbrMetallicRoughness=dict(baseColorFactor=[1.0, 1.0, 1.0, 1.0],
                                                             metallicFactor=m, roughnessFactor=r))
            if stem:
                ti = lambda suffix, stem=stem: tex_index(f"../textures/{stem}{suffix}.png")  # noqa: E731
                mat["pbrMetallicRoughness"]["baseColorTexture"] = dict(index=ti("_c"))
                mat["pbrMetallicRoughness"]["metallicRoughnessTexture"] = dict(index=ti("_orm"))
                mat["occlusionTexture"] = dict(index=ti("_orm"))
                mat["normalTexture"] = dict(index=ti("_n"), scale=ns)
            materials.append(mat)
        mi = [m["name"] for m in materials].index(mname)
        attrs = dict(POSITION=add_acc(np.ascontiguousarray(bl_to_gl(P)).astype("<f4"), 5126, 3, 34962, True),
                     NORMAL=add_acc(np.ascontiguousarray(bl_to_gl(N)).astype("<f4"), 5126, 3, 34962),
                     TEXCOORD_0=add_acc(np.ascontiguousarray(UV).astype("<f4"), 5126, 2, 34962),
                     COLOR_0=add_acc(np.ascontiguousarray(unorm(C, 16)), 5123, 4, 34962, normalized=True),
                     JOINTS_0=add_acc(np.ascontiguousarray(Jt).astype("u1"), 5121, 4, 34962),
                     WEIGHTS_0=add_acc(np.ascontiguousarray(quant_weights(Wt)), 5121, 4, 34962, normalized=True))
        if int(I.max()) < 65535:
            ind = add_acc(np.ascontiguousarray(I).astype("<u2"), 5123, 1, 34963)
        else:
            ind = add_acc(np.ascontiguousarray(I).astype("<u4"), 5125, 1, 34963)
        primitives.append(dict(attributes=attrs, indices=ind, material=mi, mode=4))
    binary = b"".join(chunks)
    binary += b"\0" * ((-len(binary)) % 4)
    nodes = json.loads(json.dumps(src["nodes"]))
    mesh_node = next(i for i, n in enumerate(nodes) if "mesh" in n)
    nodes[mesh_node] = dict(name=name, mesh=0, skin=0)
    gltf = dict(asset=dict(version="2.0", generator="VedatOnline gen_armor.py"), scene=0,
                scenes=json.loads(json.dumps(src["scenes"])), nodes=nodes,
                skins=[dict(name=skin.get("name", "skin"), joints=list(skin["joints"]), inverseBindMatrices=ibm_index)],
                meshes=[dict(name=name, primitives=primitives)], materials=materials,
                accessors=accessors, bufferViews=views, buffers=[dict(byteLength=len(binary))])
    if images:
        gltf["images"] = images
        gltf["textures"] = textures
        # REPEAT: the detail maps tile (the client draws worn pieces with a wrap sampler).
        gltf["samplers"] = [dict(magFilter=9729, minFilter=9987, wrapS=10497, wrapT=10497)]
    js = json.dumps(gltf, separators=(",", ":"), sort_keys=True).encode("utf-8")
    js += b" " * ((-len(js)) % 4)
    out = struct.pack("<III", 0x46546C67, 2, 12 + 8 + len(js) + 8 + len(binary))
    out += struct.pack("<II", len(js), 0x4E4F534A) + js + struct.pack("<II", len(binary), 0x004E4942) + binary
    vc.ensure_dir(os.path.dirname(path))
    if os.path.exists(path):
        with open(path, "rb") as f:
            if f.read() == out:
                return
    with open(path, "wb") as f:
        f.write(out)


# ---------------------------------------------------------------------------------------------
# Geometry: a Part is vertices + faces + per-vertex weights / colour / uv + per-face material

class Part:
    def __init__(self, name, material):
        self.name = name
        self.material = material
        self.V = np.zeros((0, 3))
        self.F = []          # polygons (index tuples)
        self.Wt = []         # per vertex: list of (joint index, weight)
        self.C = None        # per vertex colour (linear rgb)
        self.UV = None       # per vertex uv (metres / tile); None = zeros
        self.uperiod = None  # u wraps with this period (lofts around an axis): corners are unwrapped
        self.fmat = None     # per face material name (None = self.material)
        self.rim = 0.0       # rim thickness (inwards); 0 = none
        self.smooth = True
        self.sharp = 50.0


def make_part(name, material, V, F, weights=None, C=None, UV=None, uperiod=None):
    p = Part(name, material)
    p.V = np.asarray(V, float)
    p.F = [tuple(int(i) for i in f) for f in F]
    p.Wt = weights if weights is not None else []
    p.C = None if C is None else np.asarray(C, float)
    p.UV = None if UV is None else np.asarray(UV, float)
    p.uperiod = uperiod
    return p


def bm_from(V, F):
    bm = bmesh.new()
    vs = [bm.verts.new(tuple(p)) for p in V]
    for f in F:
        try:
            bm.faces.new([vs[i] for i in f])
        except ValueError:
            pass
    bm.verts.ensure_lookup_table()
    bm.faces.ensure_lookup_table()
    return bm


def bm_arrays(bm):
    bm.verts.ensure_lookup_table()
    bm.verts.index_update()
    V = np.array([tuple(v.co) for v in bm.verts]) if bm.verts else np.zeros((0, 3))
    F = [tuple(v.index for v in f.verts) for f in bm.faces]
    return V, F


def keep_islands(bm, min_frac=0.06):
    """Removes connected face islands smaller than min_frac of the largest."""
    bm.faces.ensure_lookup_table()
    seen = set()
    islands = []
    for f in bm.faces:
        if f.index in seen:
            continue
        stack, isl = [f], []
        seen.add(f.index)
        while stack:
            x = stack.pop()
            isl.append(x)
            for e in x.edges:
                for y in e.link_faces:
                    if y.index not in seen:
                        seen.add(y.index)
                        stack.append(y)
        islands.append(isl)
    if not islands:
        return
    big = max(len(i) for i in islands)
    drop = [f for i in islands if len(i) < min_frac * big for f in i]
    bmesh.ops.delete(bm, geom=drop, context='FACES')
    loose = [v for v in bm.verts if not v.link_faces]
    bmesh.ops.delete(bm, geom=loose, context='VERTS')
    bm.faces.index_update()


def adjacency(V, F):
    n = len(V)
    nb = [set() for _ in range(n)]
    edge_count = {}
    for f in F:
        for k in range(len(f)):
            a, b = f[k], f[(k + 1) % len(f)]
            nb[a].add(b)
            nb[b].add(a)
            key = (min(a, b), max(a, b))
            edge_count[key] = edge_count.get(key, 0) + 1
    boundary = np.zeros(n, bool)
    for (a, b), c in edge_count.items():
        if c == 1:
            boundary[a] = boundary[b] = True
    return [np.array(sorted(s), np.int64) for s in nb], boundary


def vertex_normals(V, F):
    vn = np.zeros_like(V)
    for f in F:
        p = V[list(f)]
        n = np.zeros(3)
        for k in range(1, len(f) - 1):
            n += np.cross(p[k] - p[0], p[k + 1] - p[0])
        for i in f:
            vn[i] += n
    return vn / np.maximum(np.linalg.norm(vn, axis=1, keepdims=True), 1e-12)


def taubin(V, nb, boundary, iters, lam=0.5, mu=-0.53, pin=None, edge_scale=0.35):
    """Taubin smoothing (no shrinkage); boundary vertices move gently, `pin` not at all."""
    V = V.copy()
    if iters <= 0 or not len(nb):
        return V
    rows = np.concatenate([np.full(len(n), i) for i, n in enumerate(nb)])
    cols = np.concatenate(nb)
    deg = np.maximum(np.array([len(n) for n in nb], float), 1.0)[:, None]
    scale = np.where(boundary, edge_scale, 1.0)[:, None]
    if pin is not None:
        scale[pin] = 0.0
    for _ in range(iters):
        for f in (lam, mu):
            acc = np.zeros_like(V)
            np.add.at(acc, rows, V[cols])
            V += (acc / deg - V) * f * scale
    return V


def decimate(V, F, target_tris):
    """Blender collapse decimation of a triangle/quad mesh to about `target_tris` triangles."""
    me = bpy.data.meshes.new("dec")
    me.from_pydata([tuple(p) for p in V], [], [tuple(f) for f in F])
    obj = vc.link(bpy.data.objects.new("dec", me))
    tris = sum(len(f) - 2 for f in F)
    if tris > target_tris:
        mod = obj.modifiers.new("dec", 'DECIMATE')
        mod.ratio = target_tris / tris
        mod.use_collapse_triangulate = True
        with bpy.context.temp_override(object=obj, active_object=obj):
            bpy.ops.object.modifier_apply(modifier=mod.name)
    bm = bmesh.new()
    bm.from_mesh(obj.data)
    bmesh.ops.dissolve_degenerate(bm, dist=1e-5, edges=bm.edges)
    bmesh.ops.triangulate(bm, faces=bm.faces)
    small = [f for f in bm.faces if f.calc_area() < 4e-7]
    bmesh.ops.delete(bm, geom=small, context='FACES')
    loose = [v for v in bm.verts if not v.link_faces]
    bmesh.ops.delete(bm, geom=loose, context='VERTS')
    V2, F2 = bm_arrays(bm)
    bm.free()
    bpy.data.objects.remove(obj)
    bpy.data.meshes.remove(me)
    return V2, F2


def frame_from(axis, front=(0.0, -1.0, 0.0)):
    """(side, front, axis) orthonormal frame; axis is the hull's 'up'."""
    a = np.asarray(axis, float)
    a = a / np.linalg.norm(a)
    f = np.asarray(front, float) - a * np.dot(front, a)
    if np.linalg.norm(f) < 1e-6:
        f = np.array([1.0, 0.0, 0.0]) - a * a[0]
    f /= np.linalg.norm(f)
    s = np.cross(f, a)
    return s, f, a


def gauss1d(A, sigma, wrap=False):
    """Gaussian smoothing along axis 0 (edge padding, or periodic)."""
    A = np.asarray(A, float)
    if sigma <= 0 or len(A) < 2:
        return A.copy()
    r = int(math.ceil(sigma * 2.5))
    k = np.exp(-0.5 * (np.arange(-r, r + 1) / sigma) ** 2)
    k /= k.sum()
    if wrap:
        P = np.concatenate([A[-r:], A, A[:r]], 0) if r <= len(A) else np.tile(A, (3,) + (1,) * (A.ndim - 1))
    else:
        P = np.pad(A, [(r, r)] + [(0, 0)] * (A.ndim - 1), mode='edge')
    out = np.zeros_like(A)
    for i, w in enumerate(k):
        out += w * P[i:i + len(A)]
    return out


def angdiff(a, b):
    return np.abs((a - b + math.pi) % (2 * math.pi) - math.pi)


def fourier_basis(th, order):
    th = np.asarray(th, float)
    cols = [np.ones_like(th)]
    for k in range(1, order + 1):
        cols += [np.cos(k * th), np.sin(k * th)]
    return np.stack(cols, -1)


# ---------------------------------------------------------------------------------------------
# Lofts: rings around an axis, fitted to a body region

class Loft:
    """Radius grid R[i, k] at axis parameter ts[i] (fraction of L from c0) and angle 2 pi k / K (0 = the
    frame's front, towards its side); ring centres offset by (cx, cy) along (side, front)."""

    K = 64

    def __init__(self, c0, frame, L, ts, R, cx, cy):
        self.c0 = np.asarray(c0, float)
        self.s, self.f, self.a = frame
        self.L = float(L)
        self.ts = np.asarray(ts, float)
        self.R = np.asarray(R, float)
        self.cx = np.asarray(cx, float)
        self.cy = np.asarray(cy, float)

    def radius(self, T, TH):
        T = np.asarray(T, float)
        TH = np.asarray(TH, float)
        fi = np.interp(T, self.ts, np.arange(len(self.ts)))
        i0 = np.clip(np.floor(fi).astype(int), 0, len(self.ts) - 1)
        i1 = np.clip(i0 + 1, 0, len(self.ts) - 1)
        wi = fi - i0
        fk = (np.mod(TH, 2 * math.pi) / (2 * math.pi)) * self.K
        k0 = np.floor(fk).astype(int) % self.K
        k1 = (k0 + 1) % self.K
        wk = fk - np.floor(fk)
        r0 = self.R[i0, k0] * (1 - wk) + self.R[i0, k1] * wk
        r1 = self.R[i1, k0] * (1 - wk) + self.R[i1, k1] * wk
        return r0 * (1 - wi) + r1 * wi

    def centre(self, T):
        T = np.asarray(T, float)
        return (self.c0 + np.multiply.outer(T * self.L, self.a) + np.multiply.outer(np.interp(T, self.ts, self.cx), self.s)
                + np.multiply.outer(np.interp(T, self.ts, self.cy), self.f))

    def radial(self, TH):
        TH = np.asarray(TH, float)
        return np.multiply.outer(np.cos(TH), self.f) + np.multiply.outer(np.sin(TH), self.s)

    def points(self, T, TH, DR=0.0):
        T = np.asarray(T, float)
        TH = np.asarray(TH, float)
        r = self.radius(T, TH) + DR
        return self.centre(T) + self.radial(TH) * np.asarray(r)[..., None]

    def circumference(self, t):
        th = np.linspace(0, 2 * math.pi, 65)
        p = self.points(np.full(65, t), th)
        return float(np.linalg.norm(np.diff(p, axis=0), axis=1).sum())

    def tuck(self, hp, gap=0.006, max_pull=0.03, slab=0.03):
        """Pulls the loft in under helper geometry `hp` that lies over it (smoothly, at most max_pull)."""
        if hp is None or not len(hp):
            return
        rel = hp - self.c0
        tp = rel @ self.a
        D = np.zeros_like(self.R)
        thk = 2 * math.pi * np.arange(self.K) / self.K
        for i, t in enumerate(self.ts):
            m = np.abs(tp - t * self.L) < slab
            if not m.any():
                continue
            dx = rel[m] @ self.s - self.cx[i]
            dy = rel[m] @ self.f - self.cy[i]
            r = np.hypot(dx, dy)
            th = np.arctan2(dx, dy)
            for k in range(self.K):
                sel = angdiff(th, thk[k]) < 2.2 * math.pi / self.K
                if sel.any():
                    cap = float(r[sel].min()) - gap
                    D[i, k] = min(max(self.R[i, k] - cap, 0.0), max_pull)
        D = gauss1d(gauss1d(D, 1.0).T, 1.5, wrap=True).T
        self.R = self.R - D

    def contain(self, hp, gap=0.008, max_push=0.06, slab=0.03):
        """Pushes the loft out (smoothly, at most max_push) so that the points `hp` end up inside."""
        if hp is None or not len(hp):
            return
        rel = hp - self.c0
        tp = rel @ self.a
        D = np.zeros_like(self.R)
        thk = 2 * math.pi * np.arange(self.K) / self.K
        for i, t in enumerate(self.ts):
            m = np.abs(tp - t * self.L) < slab
            if not m.any():
                continue
            dx = rel[m] @ self.s - self.cx[i]
            dy = rel[m] @ self.f - self.cy[i]
            r = np.hypot(dx, dy)
            th = np.arctan2(dx, dy)
            for k in range(self.K):
                sel = angdiff(th, thk[k]) < 2.2 * math.pi / self.K
                if sel.any():
                    D[i, k] = min(max(float(r[sel].max()) + gap - self.R[i, k], 0.0), max_push)
        # Widen before smoothing so the peak survives (a gentle swell over the braid, not a spike).
        Dw = D.copy()
        for sh in (-2, -1, 1, 2):
            Dw = np.maximum(Dw, np.roll(D, sh, 1))
        Dw2 = Dw.copy()
        Dw2[1:] = np.maximum(Dw2[1:], Dw[:-1])
        Dw2[:-1] = np.maximum(Dw2[:-1], Dw[1:])
        self.R = self.R + gauss1d(gauss1d(Dw2, 1.0).T, 1.5, wrap=True).T


def fit_loft(pts, c0, c1, ts, *, order=3, pct=88.0, cover=0.96, margin=0.01, front=(0.0, -1.0, 0.0), slab=None,
             sm=1.2, centre=False, sectors=24, cover_pts=None, floor_r=0.0):
    """Fits a Loft around c0 -> c1 to the points `pts` (see the module docstring). `cover_pts` (default pts)
    are the points that must end up inside."""
    c0 = np.asarray(c0, float)
    c1 = np.asarray(c1, float)
    axis = c1 - c0
    L = float(np.linalg.norm(axis))
    s, f, a = frame_from(axis / L, front)
    ts = np.asarray(ts, float)
    n = len(ts)
    rel = pts - c0
    tp, xs, yf = rel @ a, rel @ s, rel @ f
    if slab is None:
        slab = max(0.018, 0.6 * L * (float(np.max(np.diff(ts))) if n > 1 else 0.1))
    sel = []
    cx, cy = np.zeros(n), np.zeros(n)
    for i, t in enumerate(ts):
        m = np.abs(tp - t * L) < slab
        if m.sum() < 16:
            m = np.abs(tp - t * L) < slab * 2.5
        sel.append(m)
        if centre and m.sum() >= 8:
            cx[i] = 0.5 * (np.percentile(xs[m], 4) + np.percentile(xs[m], 96))
            cy[i] = 0.5 * (np.percentile(yf[m], 4) + np.percentile(yf[m], 96))
    if centre:
        cx, cy = gauss1d(cx, 1.5), gauss1d(cy, 1.5)
    K = sectors
    thk = 2 * math.pi * np.arange(K) / K
    B = fourier_basis(thk, order)
    C = np.zeros((n, 2 * order + 1))
    have = np.zeros(n, bool)
    for i in range(n):
        m = sel[i]
        if m.sum() < 6:
            continue
        dx, dy = xs[m] - cx[i], yf[m] - cy[i]
        r, th = np.hypot(dx, dy), np.arctan2(dx, dy)
        Rk = np.full(K, np.nan)
        for k in range(K):
            d = angdiff(th, thk[k]) < 1.6 * math.pi / K
            if d.sum() >= 2:
                Rk[k] = np.percentile(r[d], pct)
        ok = ~np.isnan(Rk)
        if ok.sum() < 3:
            continue
        if not ok.all():
            idx = np.nonzero(ok)[0]
            Rk = np.interp(np.arange(K), idx, Rk[idx], period=K)
        C[i] = np.linalg.lstsq(B, Rk, rcond=None)[0]
        have[i] = True
    if not have.any():
        raise RuntimeError("fit_loft: no points")
    idx = np.nonzero(have)[0]
    for i in range(n):
        if not have[i]:
            C[i] = C[idx[np.argmin(np.abs(idx - i))]]
    C = gauss1d(C, sm)
    # Raise each ring until `cover` of the region's points near it are inside.
    cp = pts if cover_pts is None else cover_pts
    relc = cp - c0
    tpc, xsc, yfc = relc @ a, relc @ s, relc @ f
    up = np.zeros(n)
    cov = np.broadcast_to(np.asarray(cover, float), (n,))
    for i, t in enumerate(ts):
        m = np.abs(tpc - t * L) < slab
        if m.sum() < 4:
            continue
        dx, dy = xsc[m] - cx[i], yfc[m] - cy[i]
        r, th = np.hypot(dx, dy), np.arctan2(dx, dy)
        d = r - fourier_basis(th, order) @ C[i]
        up[i] = max(0.0, float(np.quantile(d, cov[i])))
    upm = up.copy()
    for i in range(n):
        upm[i] = up[max(0, i - 1):i + 2].max()
    C[:, 0] += gauss1d(upm, sm) + margin
    thK = 2 * math.pi * np.arange(Loft.K) / Loft.K
    R = np.maximum(C @ fourier_basis(thK, order).T, floor_r)
    return Loft(c0, (s, f, a), L, ts, R, cx, cy)


# ---------------------------------------------------------------------------------------------
# Weights

def dense_weights(body, wl, n):
    W = np.zeros((n, len(body.jnames)))
    for i, w in enumerate(wl):
        for j, x in (w.items() if isinstance(w, dict) else w):
            W[i, j] += x
    return W


def sparse_weights(W):
    out = []
    for row in W:
        nz = np.nonzero(row > 1e-4)[0]
        out.append(top4({int(j): float(row[j]) for j in nz}) if len(nz) else [(0, 1.0)])
    return out


def transfer(body, bones, V, thr=0.35, exclude=HELPERS):
    src = body.bvh(("tr",) + tuple(sorted(bones)) + (thr,), body.faces_of(bones, thr, exclude))
    return [top4(w) for w in body.weights_at(src, V)]


def grid_smooth_weights(body, wl, rows, cols, iters=2, wrap=True):
    """Weights of a rows x cols vertex grid (ring-major), smoothed over the grid neighbours."""
    W = dense_weights(body, wl, rows * cols).reshape(rows, cols, -1)
    for _ in range(iters):
        P = np.pad(W, ((1, 1), (0, 0), (0, 0)), mode='edge')
        if wrap:
            left, right = np.roll(W, 1, 1), np.roll(W, -1, 1)
        else:
            Q = np.pad(W, ((0, 0), (1, 1), (0, 0)), mode='edge')
            left, right = Q[:, :-2], Q[:, 2:]
        W = 0.4 * W + 0.15 * (P[:-2] + P[2:] + left + right)
    return sparse_weights(W.reshape(rows * cols, -1))


def rigid(body, bone, n):
    return [[(body.jindex[bone], 1.0)]] * n


def chain(body, V, links):
    """Weights interpolated over height between bones: links = [(z, bone), ...] top to bottom."""
    out = []
    for p in V:
        z = p[2]
        if z >= links[0][0]:
            out.append([(body.jindex[links[0][1]], 1.0)])
            continue
        if z <= links[-1][0]:
            out.append([(body.jindex[links[-1][1]], 1.0)])
            continue
        for (z0, b0), (z1, b1) in zip(links, links[1:]):
            if z1 <= z <= z0:
                t = (z0 - z) / max(z0 - z1, 1e-6)
                w = {body.jindex[b0]: 1.0 - t}
                w[body.jindex[b1]] = w.get(body.jindex[b1], 0.0) + t
                out.append(top4(w))
                break
    return out


# ---------------------------------------------------------------------------------------------
# Colours (sRGB hex, light: the item tint multiplies them per tier)

def lin(hexstr):
    return np.array(vc.hex_linear(hexstr))


PALETTE = {
    "heavy": dict(base="#d9dde2", recess="#474c54", trim="#cfd3d8", trim_b="#e8c27a", strap="#7a5c42",
                  buckle="#d9d9d9"),
    "leather": dict(base="#f4e6d2", recess="#7f654c", trim="#a88a6a", trim_b="#e3c58e", strap="#8f6d50",
                    buckle="#cfcfcf", stud="#bfc2c6"),
    "cloth": dict(base="#f6f2ea", recess="#cdc4b4", trim="#ffffff", trim_b="#ffffff", strap="#ffffff",
                  sash="#ffffff", lining="#d9cbb4", gold="#e9c46e"),
}


def shade(C, k, col):
    k = np.clip(np.asarray(k, float), 0.0, 1.0)[:, None]
    return C * (1 - k) + lin(col) * k


# ---------------------------------------------------------------------------------------------
# Loft parts and decorations

def stepped(u, steps, eps=0.004):
    """Ring parameters `u` (0..1) plus a pair of rings tightly around every step position (plate lames: the
    saw-tooth's step lands between two close rings, so it stays a crisp edge instead of a smeared slope)."""
    extra = [x for s in steps for x in (s - eps, s + eps) if eps < s < 1.0 - eps]
    v = np.unique(np.round(np.concatenate([np.asarray(u, float), extra]), 5))
    keep = [v[0]]
    for x in v[1:]:
        if x - keep[-1] > eps * 0.6:
            keep.append(x)
    return np.array(keep)


def lame_steps(t_from, t_to, per):
    """Step positions (multiples of `per`) inside [t_from, t_to]."""
    k0 = int(math.ceil(t_from / per))
    return [k * per for k in range(max(k0, 1), int(t_to / per) + 1)]


def loft_grid(lo, t0, t1, rows, cols, dr=None, tdist=None):
    """Vertices of the loft between t0 and t1: rows x cols (ring-major, wrapping columns). dr(T, TH) -> extra
    radius. tdist: a function u in [0,1] -> t fraction (uneven ring spacing), or an explicit array of u values
    (then `rows` is its length)."""
    if tdist is not None and not callable(tdist):
        u = np.asarray(tdist, float)
        rows = len(u)
    else:
        u = np.linspace(0.0, 1.0, rows)
        if tdist is not None:
            u = tdist(u)
    T = np.repeat(t0 + (t1 - t0) * u, cols)
    TH = np.tile(2 * math.pi * np.arange(cols) / cols, rows)
    DR = dr(T, TH) if dr is not None else 0.0
    return lo.points(T, TH, DR), T, TH


def grid_faces(rows, cols, wrap=True, base=0):
    F = []
    for r in range(rows - 1):
        for c in range(cols if wrap else cols - 1):
            c2 = (c + 1) % cols
            a, b = base + r * cols + c, base + r * cols + c2
            d, e = base + (r + 1) * cols + c, base + (r + 1) * cols + c2
            F.append((a, b, e, d))
    return F


def orient_faces(V, F, outward):
    """Winds every face so that its normal points along outward(centroids) (per face)."""
    V = np.asarray(V, float)
    cent = np.array([V[list(f)].mean(0) for f in F])
    nrm = np.array([np.cross(V[f[1]] - V[f[0]], V[f[2]] - V[f[0]]) for f in F])
    out = outward(cent)
    flip = np.einsum("ij,ij->i", nrm, out) < 0
    return [tuple(reversed(f)) if fl else tuple(f) for f, fl in zip(F, flip)]


def orient_loft(part, lo):
    def outward(cent):
        t = np.clip(((cent - lo.c0) @ lo.a) / lo.L, lo.ts[0], lo.ts[-1])
        return cent - lo.centre(t)
    part.F = orient_faces(part.V, part.F, outward)
    part.oriented = True
    return part


def loft_part(body, name, mat, lo, t0, t1, rows, cols, bones, tile, dr=None, tdist=None, wsmooth=2,
              weights=None, thr=0.35):
    """A loft surface with surface UVs (u around in tiles, v along in tiles) and skin weights."""
    V, T, TH = loft_grid(lo, t0, t1, rows, cols, dr, tdist)
    rows = len(V) // cols
    F = grid_faces(rows, cols)
    circ = lo.circumference(0.5 * (t0 + t1))
    n = max(1, int(round(circ / tile)))
    UV = np.stack([TH / (2 * math.pi) * n, T * lo.L / tile], -1)
    if weights is None:
        weights = grid_smooth_weights(body, transfer(body, bones, V, thr), rows, cols, wsmooth)
    p = make_part(name, mat, V, F, weights, UV=UV, uperiod=float(n))
    p.T, p.TH, p.rows, p.cols = T, TH, rows, cols
    return orient_loft(p, lo)


def ring_strip(body, name, mat, lo, t_c, width, lift, bones, tile, cols=36, rows=3, bulge=0.0, weights_from=None,
               colour=None):
    """A band around the loft at t_c, `width` metres along the axis, lifted off the surface; strip UVs
    (u along the band in tiles, v 0..1 across). bulge: extra lift in the middle (a rolled edge)."""
    dt = 0.5 * width / lo.L
    vv = np.linspace(0.0, 1.0, rows)
    T = np.repeat(t_c - dt + 2 * dt * vv, cols)
    TH = np.tile(2 * math.pi * np.arange(cols) / cols, rows)
    prof = np.repeat(np.sin(math.pi * vv) ** 0.8, cols)
    V = lo.points(T, TH, lift + bulge * prof)
    F = grid_faces(rows, cols)
    n = max(1, int(round(lo.circumference(t_c) / tile)))
    UV = np.stack([TH / (2 * math.pi) * n, np.repeat(vv, cols)], -1)
    W = grid_smooth_weights(body, transfer(body, bones, weights_from(V) if weights_from else V), rows, cols, 1)
    p = make_part(name, mat, V, F, W, UV=UV, uperiod=float(n))
    if colour is not None:
        p.C = np.tile(lin(colour), (len(V), 1))
    p.rim = 0.002
    return orient_loft(p, lo)


def long_strip(body, name, mat, lo, th_c, t0, t1, width, lift, bones, tile, rows=12, cols=3, colour=None,
               taper=None):
    """A strip along the loft from t0 to t1 at angle th_c, `width` metres across; strip UVs (u along in tiles,
    v 0..1 across). taper(u) scales the width along the strip."""
    uu = np.linspace(0.0, 1.0, rows)
    vv = np.linspace(-0.5, 0.5, cols)
    T = np.repeat(t0 + (t1 - t0) * uu, cols)
    Rc = lo.radius(T, np.full(len(T), th_c))
    wsc = np.repeat(taper(uu), cols) if taper else 1.0
    TH = th_c + np.tile(vv, rows) * width * wsc / np.maximum(Rc, 0.02)
    V = lo.points(T, TH, lift)
    F = grid_faces(rows, cols, wrap=False)
    UV = np.stack([T * lo.L / tile, np.tile(vv + 0.5, rows)], -1)
    W = grid_smooth_weights(body, transfer(body, bones, V), rows, cols, 1, wrap=False)
    p = make_part(name, mat, V, F, W, UV=UV)
    if colour is not None:
        p.C = np.tile(lin(colour), (len(V), 1))
    p.rim = 0.002
    return orient_loft(p, lo)


class MeshBox:
    """Small rigid or skinned solids (boxes, studs) collected into one part."""

    def __init__(self):
        self.V, self.F = [], []

    def box(self, c, size, rot=0.0, axes=None):
        sx, sy, sz = (x * 0.5 for x in size)
        if axes is None:
            cr, sr = math.cos(math.radians(rot)), math.sin(math.radians(rot))
            axes = (np.array([cr, 0, sr]), np.array([0, 1.0, 0]), np.array([-sr, 0, cr]))
        ax, ay, az = (np.asarray(x, float) for x in axes)
        b = len(self.V)
        for z in (-sz, sz):
            for (x, y) in ((-sx, -sy), (sx, -sy), (sx, sy), (-sx, sy)):
                self.V.append(np.asarray(c, float) + x * ax + y * ay + z * az)
        for f in ((0, 3, 2, 1), (4, 5, 6, 7), (0, 1, 5, 4), (1, 2, 6, 5), (2, 3, 7, 6), (3, 0, 4, 7)):
            self.F.append(tuple(b + i for i in f))

    def stud(self, c, n, size, height=None, sides=6):
        """A low dome (rivet head / stud) on a surface with normal n."""
        n = np.asarray(n, float) / np.linalg.norm(n)
        s, f, a = frame_from(n, (0.0, 0.0, 1.0) if abs(n[2]) < 0.9 else (0.0, -1.0, 0.0))
        h = size * 0.6 if height is None else height
        b = len(self.V)
        for k in range(sides):
            ang = 2 * math.pi * k / sides
            self.V.append(np.asarray(c) + (math.cos(ang) * f + math.sin(ang) * s) * size - a * 0.002)
        for k in range(sides):
            ang = 2 * math.pi * (k + 0.5) / sides
            self.V.append(np.asarray(c) + (math.cos(ang) * f + math.sin(ang) * s) * size * 0.6 + a * h * 0.75)
        self.V.append(np.asarray(c) + a * h)
        top = len(self.V) - 1
        for k in range(sides):
            k2 = (k + 1) % sides
            self.F.append((b + k, b + k2, b + sides + k))
            self.F.append((b + k2, b + sides + k2, b + sides + k))
            self.F.append((b + sides + k, b + sides + k2, top))

    def part(self, body, name, material, colour, bone=None, bones=None):
        V = np.array(self.V)
        W = rigid(body, bone, len(V)) if bone else transfer(body, bones, V)
        p = make_part(name, material, V, self.F, W)
        p.C = np.tile(lin(colour), (len(V), 1))
        p.smooth = False
        return p


def loft_studs(body, lo, coords, size, lift, bones, colour, material="iron", name="studs"):
    mb = MeshBox()
    for t, th in coords:
        p = lo.points(np.array([t]), np.array([th]), lift)[0]
        n = lo.radial(np.array([th]))[0]
        mb.stud(p, n, size)
    return mb.part(body, name, material, colour, bones=bones)


# ---------------------------------------------------------------------------------------------
# Domes: star-shaped hulls around a centre (pauldrons, cops, heads)

class Dome:
    """Radius grid R[e, k] over elevation rows (from the top down to the per-azimuth lower edge) and
    azimuth columns (0 = frame front)."""

    def __init__(self, c, frame, lower_edge, rows, cols, pts, margin, shape=None, cone=16.0, iters=3, floor=0.0):
        self.c = np.asarray(c, float)
        self.s, self.f, self.a = frame
        self.rows, self.cols = rows, cols
        self.lower = lower_edge
        dirs, EL, AZ = [], [], []
        for r in range(rows):
            t = (r + 1) / rows
            for k in range(cols):
                az = 2 * math.pi * k / cols
                el = math.radians(90.0 - t * (90.0 - lower_edge(az)))
                dirs.append(self.dir(el, az))
                EL.append(el)
                AZ.append(az)
        self.dirs = np.array(dirs)
        self.EL, self.AZ = np.array(EL), np.array(AZ)
        R = hull_radius(pts, self.c, self.dirs, cone, margin, floor)
        R = smooth_grid(R, rows, cols, iters)
        if shape:
            R = np.array([shape(az, math.degrees(el), r) for az, el, r in zip(self.AZ, self.EL, R)])
        self.R = R
        top = float(hull_radius(pts, self.c, self.a[None], cone, margin, floor)[0])
        self.top = shape(0.0, 90.0, top) if shape else top

    def dir(self, el, az):
        d = math.cos(el) * (math.cos(az) * self.f + math.sin(az) * self.s) + math.sin(el) * self.a
        return d / np.linalg.norm(d)

    def mesh(self, dr=None):
        """V, F, UV-ready (el, az) per vertex; capped at the top pole."""
        R = self.R + (dr(self.EL, self.AZ) if dr else 0.0)
        V = self.c + self.dirs * R[:, None]
        F = [tuple(reversed(q)) for q in grid_faces(self.rows, self.cols)]
        ti = len(V)
        topr = self.top + (dr(np.array([math.pi / 2]), np.array([0.0]))[0] if dr else 0.0)
        V = np.vstack([V, (self.c + self.a * topr)[None]])
        for k in range(self.cols):
            F.append((ti, (k + 1) % self.cols, k))
        EL = np.append(self.EL, math.pi / 2)
        AZ = np.append(self.AZ, 0.0)
        return V, F, EL, AZ

    def radius_at(self, el, az):
        low = math.radians(self.lower(az))
        t = (math.pi / 2 - el) / max(math.pi / 2 - low, 1e-6)
        fr = float(np.clip(t * self.rows - 1.0, 0.0, self.rows - 1.0))
        fk = (az % (2 * math.pi)) / (2 * math.pi) * self.cols
        r0, k0 = int(math.floor(fr)), int(math.floor(fk)) % self.cols
        r1, k1 = min(r0 + 1, self.rows - 1), (k0 + 1) % self.cols
        wr, wk = fr - math.floor(fr), fk - math.floor(fk)
        G = self.R.reshape(self.rows, self.cols)
        top = (G[r0, k0] * (1 - wk) + G[r0, k1] * wk) * (1 - wr) + (G[r1, k0] * (1 - wk) + G[r1, k1] * wk) * wr
        if t * self.rows < 1.0:  # between the top pole and the first row
            w = max(0.0, t * self.rows)
            top = self.top * (1 - w) + top * w
        return top

    def point(self, el, az, dr=0.0):
        """A point on the dome surface (interpolated radius) at elevation el, azimuth az (radians)."""
        d = self.dir(el, az)
        return self.c + d * (self.radius_at(el, az) + dr), d


def hull_radius(pts, c, dirs, cone_deg=14.0, margin=0.01, floor=0.0):
    """Per direction (unit vectors) the largest distance of `pts` from c inside a cone, + margin."""
    d = pts - c
    r = np.linalg.norm(d, axis=1)
    u = d / np.maximum(r[:, None], 1e-9)
    cosc = math.cos(math.radians(cone_deg))
    out = np.full(len(dirs), floor)
    dots = u @ np.asarray(dirs).T
    for i in range(len(dirs)):
        m = dots[:, i] > cosc
        if m.any():
            out[i] = max(floor, float(r[m].max()))
    return out + margin


def smooth_grid(R, rows, cols, iters=2, wrap=True):
    G = np.asarray(R, float).reshape(rows, cols)
    for _ in range(iters):
        P = np.pad(G, ((1, 1), (0, 0)), mode='edge')
        if wrap:
            left, right = np.roll(G, 1, 1), np.roll(G, -1, 1)
        else:
            Q = np.pad(G, ((0, 0), (1, 1)), mode='edge')
            left, right = Q[:, :-2], Q[:, 2:]
        G = 0.4 * G + 0.15 * (P[:-2] + P[2:] + left + right)
    return G.reshape(-1)


def dome_part(body, name, mat, dome, tile, weights=None, bones=None, bone=None, dr=None):
    V, F, EL, AZ = dome.mesh(dr)
    n = max(1, int(round(2 * math.pi * float(np.mean(dome.R)) / tile)))
    arc = (math.pi / 2 - EL) * float(np.mean(dome.R))
    UV = np.stack([AZ / (2 * math.pi) * n, arc / tile], -1)
    if weights is None:
        weights = rigid(body, bone, len(V)) if bone else transfer(body, bones, V)
    p = make_part(name, mat, V, F, weights, UV=UV, uperiod=float(n))
    p.EL, p.AZ = EL, AZ
    p.F = orient_faces(p.V, p.F, lambda cent: cent - dome.c)
    p.oriented = True
    return p


# ---------------------------------------------------------------------------------------------
# Shells (hands): the hand skin, smoothed, pushed out

def shell(body, name, material, bones, cuts, offset, src_smooth=8, smooth=6, thr=0.5, exclude=HELPERS,
          min_island=0.06, keep=0.5):
    """A surface layer over the body region `bones`, from a smoothed copy of the skin. cuts: list of
    (point, normal[, side]): geometry on the side the normal points to is removed."""
    fmask = body.faces_of(bones, thr, exclude)
    tris = body.T[fmask]
    used, inv = np.unique(tris, return_inverse=True)
    bm = bm_from(body.P[used], inv.reshape(-1, 3))
    for cut in cuts:
        p, n = cut[0], cut[1]
        side = cut[2] if len(cut) > 2 else 0.0
        if side:
            vs = [v for v in bm.verts if v.co.x * side > 0.0]
            vset = set(vs)
            geom = vs + [e for e in bm.edges if all(v in vset for v in e.verts)] + \
                [f for f in bm.faces if all(v in vset for v in f.verts)]
        else:
            geom = list(bm.verts) + list(bm.edges) + list(bm.faces)
        bmesh.ops.bisect_plane(bm, geom=geom, dist=1e-6, plane_co=tuple(p), plane_no=tuple(n), clear_outer=True)
    keep_islands(bm, min_island)
    bmesh.ops.remove_doubles(bm, verts=bm.verts, dist=0.002)
    bmesh.ops.dissolve_degenerate(bm, dist=1e-5, edges=bm.edges)
    V, F = bm_arrays(bm)
    bm.free()
    nb, boundary = adjacency(V, F)
    V0 = V.copy()
    Vs = taubin(V, nb, boundary, src_smooth, edge_scale=0.6)   # the smoothed source surface
    n0 = vertex_normals(Vs, F)
    V = Vs + n0 * offset
    V = taubin(V, nb, boundary, smooth)
    # Never closer than keep x offset to the raw skin point the vertex came from.
    s = np.einsum("ij,ij->i", V - V0, n0)
    V = V + n0 * np.maximum(offset * keep - s, 0.0)[:, None]
    V = taubin(V, nb, boundary, 2)
    part = Part(name, material)
    part.V, part.F = V, F
    return part


# ---------------------------------------------------------------------------------------------
# Line parameters

MARGIN = {"heavy": 0.016, "leather": 0.009, "cloth": 0.010}
SURF = {"heavy": "steel", "leather": "leather", "cloth": "cloth"}
EDGE = {"heavy": "trim", "leather": "strap", "cloth": "band"}
RIM = {"heavy": 0.005, "leather": 0.0035, "cloth": 0.0025}
TILE = atex.TILE


def sides():
    return (("l", 1.0), ("r", -1.0))


def ease_top(p):
    return lambda u: 1.0 - (1.0 - u) ** p


def colour_by(part, line, base=None):
    part.C = np.tile(lin(base or PALETTE[line]["base"]), (len(part.V), 1))
    return part


def saw(T, period, phase=0.0):
    return np.mod((T - phase) / period, 1.0)


# ---------------------------------------------------------------------------------------------
# Torso

def torso_points(body, z0, z1, neck=True):
    bones = TORSO + (("neck_01",) if neck else ())
    m = body.mask(bones, 0.45) & (body.rw(ARMS) < 0.25) & (body.rw(("Head",)) < 0.3)
    m &= (body.P[:, 2] > z0 - 0.08) & (body.P[:, 2] < z1 + 0.08)
    # The shoulder tips belong to the pauldrons: a cuirass fitted out to them is a flat-topped box.
    # Above the armpits the allowed width narrows towards the neck (sloped shoulders, a neck-sized top).
    ux = abs(body.j("upperarm_l")[0])
    zc = body.j("clavicle_l")[2] - 0.05 * body.scale
    k = np.clip((body.P[:, 2] - zc) / max(z1 - zc, 1e-3), 0.0, 1.0)
    m &= np.abs(body.P[:, 0]) < ux * (0.72 - 0.3 * k)
    return body.P[m]


def hanging_points(body, z0, z1):
    """Braids and beards (helper bones, or head skin below the chin) over the torso: the armour goes over
    them (a braid tucked into a breastplate reads better than one cut by it)."""
    neck = body.j("neck_01")
    m = (body.rw(("beard", "braid_l", "braid_r")) >= 0.3) | ((body.rw(("Head",)) >= 0.5) & (body.P[:, 2] < neck[2] - 0.02))
    m &= (body.P[:, 2] > z0) & (body.P[:, 2] < z1)
    return body.P[m]


def torso_loft(body, z0, z1, margin, pct=88.0, rows=24, order=4, tuck=True):
    pts = torso_points(body, z0, z1)
    yc = float(np.median(pts[:, 1]))
    ts = np.linspace(0.0, 1.0, rows)
    # The belly and belt (pouches, buckles) may show through a little; the chest and shoulders may not.
    cover = 0.86 + 0.11 * np.clip(ts / 0.35, 0.0, 1.0)
    lo = fit_loft(pts, (0.0, yc, z0), (0.0, yc, z1), ts, order=order, pct=pct, cover=cover, margin=margin,
                  centre=True, sm=1.0)
    # Chest and back plates of the body (narrow features the robust fit treats as outliers) stay inside.
    upper = pts[pts[:, 2] > z0 + 0.3 * (z1 - z0)]
    lo.contain(upper, gap=0.004, max_push=0.035)
    if tuck:
        lo.tuck(body.helpers(("mantle", "cloth_back", "cloth_front")), gap=0.006, max_pull=0.035, slab=0.03)
        lo.contain(hanging_points(body, z0, z1), gap=0.008, max_push=0.06)
    return lo


def arm_loft(body, sd, f0, f1, margin, pct=86.0, order=3, rows=10, extend=0.0):
    """Upper arm (f0..f1 of shoulder -> elbow) or forearm when f > 1 (1..2 = elbow -> wrist)."""
    sh, el, wr = body.j(f"upperarm_{sd}"), body.j(f"lowerarm_{sd}"), body.j(f"hand_{sd}")
    if f0 >= 1.0:
        a0, a1, bones = el, wr, (f"lowerarm_{sd}",)
        f0, f1 = f0 - 1.0, f1 - 1.0
    else:
        a0, a1, bones = sh, el, (f"upperarm_{sd}",)
    m = body.mask(bones, 0.5)
    pts = body.P[m]
    c0 = a0 + (a1 - a0) * f0
    c1 = a0 + (a1 - a0) * f1
    d = (c1 - c0) / np.linalg.norm(c1 - c0)
    c0 = c0 - d * extend
    ts = np.linspace(0.0, 1.0, rows)
    lo = fit_loft(pts, c0, c1, ts, order=order, pct=pct, cover=0.95, margin=margin, front=(0.0, 0.0, 1.0),
                  centre=True, sm=1.0)
    return lo, bones


def build_chest(body, line, shape):
    s = body.scale
    b = shape == "b"
    pel, neck = body.j("pelvis"), body.j("neck_01")
    waist = pel[2] + (0.05 if line == "heavy" else 0.03) * s
    top = neck[2] - 0.012 * s
    margin = MARGIN[line] * (1.15 if b and line == "heavy" else 1.0)
    lo = torso_loft(body, waist, top, margin)
    L = lo.L
    pal = PALETTE[line]
    parts = []
    bones = TORSO + ("neck_01",) + side_bones("upperarm")
    # The torso surface: plates (heavy), a stitched vest (leather), a robe body (cloth).
    tdist = ease_top(1.5)
    if line == "heavy":
        belly = (body.j("spine_02")[2] - waist) / L
        per = 0.055 * s / L

        def dr(T, TH, b=b):
            out = np.zeros_like(T)
            lame = T < belly
            out += np.where(lame, 0.011 * (1.0 - saw(T, per)), 0.0)                   # lames (overlap downward)
            front = np.cos(TH)
            out += 0.012 * np.clip(1.0 - T / 0.06, 0, 1)  # the lowest lame flares over the fauld
            out += 0.010 * np.clip(front, 0, 1) ** 3 * np.clip((T - belly) / 0.2, 0, 1) * (1 - T) * 2.2  # chest swell
            if b:  # a raised medial ridge and flutes radiating on the breastplate
                out += 0.007 * np.exp(-(np.sin(TH) / 0.05) ** 2) * (front > 0) * (T > belly)
                out += 0.003 * np.clip(front, 0, 1) * np.cos(TH * 14.0) ** 8 * (T > belly) * (1 - T)
            return out

        tl = stepped(ease_top(1.5)(np.linspace(0.0, 1.0, 17)), lame_steps(0.0, belly, per))
        p = loft_part(body, "cuirass", "steel", lo, 0.0, 1.0, 0, 38, bones, TILE["steel"], dr, tl)
        colour_by(p, line)
        groove = np.where(p.T < belly, np.clip((0.16 - saw(p.T, per)) / 0.08, 0, 1), 0.0)
        side_seam = np.exp(-((np.abs(np.sin(p.TH)) - 1.0) / 0.012) ** 2)   # breast / back plate join
        p.C = shade(p.C, 0.85 * groove + 0.6 * side_seam, pal["recess"])
        parts.append(p)
        trim_col = pal["trim_b"] if b else pal["trim"]
        parts.append(ring_strip(body, "hem", "trim", lo, 0.012, 0.026 * s, 0.004, bones, TILE["trim"],
                                bulge=0.004, colour=trim_col))
        parts.append(ring_strip(body, "neckroll", "trim", lo, 0.985, 0.022 * s, 0.004, bones, TILE["trim"],
                                bulge=0.004, colour=trim_col))
        if b:
            parts.append(ring_strip(body, "beltline", "trim", lo, belly + 0.01, 0.022 * s, 0.006, bones,
                                    TILE["trim"], bulge=0.003, colour=trim_col))
        # Side straps joining the plates.
        for sd, sg in sides():
            th = math.pi / 2 if sg < 0 else -math.pi / 2
            for tc in (0.3, 0.55):
                parts.append(long_strip(body, f"sidestrap_{sd}", "strap", lo, th, tc - 0.06, tc + 0.06, 0.03 * s, 0.006,
                                        bones, TILE["strap"], rows=3, colour=pal["strap"]))
        parts += pauldrons(body, line, shape)
        parts += rerebraces(body, line, shape)
        if b:
            parts.append(gorget(body, line, shape))
    elif line == "leather":
        def dr(T, TH):
            return 0.004 * np.clip(np.cos(TH), 0, 1) ** 3 * np.clip(T - 0.2, 0, 1)

        p = loft_part(body, "jerkin", "leather", lo, 0.0, 1.0, 20, 36, bones, TILE["leather"], dr, tdist)
        colour_by(p, line)
        front_seam = np.exp(-(np.abs(np.sin(p.TH)) / 0.03) ** 2) * (np.cos(p.TH) > 0)  # lacing down the front
        p.C = shade(p.C, 0.5 * front_seam, pal["recess"])
        parts.append(p)
        belt_t = 0.06
        parts.append(ring_strip(body, "belt", "strap", lo, belt_t, 0.05 * s, 0.006, bones, TILE["strap"], rows=3,
                                colour=pal["strap"]))
        parts.append(buckle(body, lo, belt_t, 0.0, 0.012, bones, pal["buckle"], s))
        parts.append(ring_strip(body, "collar", "strap", lo, 0.975, 0.03 * s, 0.004, bones, TILE["strap"], rows=3,
                                colour=pal["trim"]))
        # Lacing: short crossing laces down the front seam.
        mb = MeshBox()
        for k in range(6):
            t = 0.25 + 0.11 * k
            pt = lo.points(np.array([t]), np.array([0.0]), 0.004)[0]
            mb.box(pt, (0.03 * s, 0.006, 0.005), rot=25.0 if k % 2 else -25.0)
        parts.append(mb.part(body, "laces", "iron", "#3a2a1e", bones=bones))
        if b:
            # A bandolier from the right shoulder to the left hip, studs on the chest, one shoulder guard,
            # a high collar and leather sleeves to the elbow.
            parts.append(bandolier(body, lo, bones, pal, s))
            studs = [(t, th) for t in (0.45, 0.6, 0.75) for th in (0.55, 0.85, -0.55, -0.85)]
            parts.append(loft_studs(body, lo, studs, 0.008 * s, 0.003, bones, pal["stud"]))
            parts += pauldrons(body, line, shape)
            parts += rerebraces(body, line, shape)
            parts.append(gorget(body, line, shape))
        else:
            parts += pauldrons(body, line, shape)
            parts += rerebraces(body, line, shape)
    else:
        def dr(T, TH):
            # Soft vertical folds below the chest.
            return 0.004 * np.cos(TH * 9.0) ** 2 * np.clip(0.6 - T, 0, 1)

        p = loft_part(body, "robe", "cloth", lo, 0.0, 1.0, 20, 38, bones, TILE["cloth"], dr, tdist)
        colour_by(p, line)
        parts.append(p)
        parts.append(skirt(body, line, shape, lo, waist))
        sash_t = 0.07
        parts.append(ring_strip(body, "sash", "band", lo, sash_t, (0.075 if b else 0.06) * s, 0.008, bones,
                                TILE["band"] * (1.4 if b else 1.0), rows=4, bulge=0.004))
        parts.append(ring_strip(body, "collar", "band", lo, 0.975, 0.035 * s, 0.003, bones, TILE["band"] * 0.6, rows=3))
        parts += sleeves(body, line, shape)
        parts += pauldrons(body, line, shape)
        if b:
            # An embroidered stole down the front, from the collar over the sash.
            for sd, sg in sides():
                th = -0.32 * sg
                parts.append(long_strip(body, f"stole_{sd}", "band", lo, th, 0.12, 0.97, 0.05 * s, 0.006, bones,
                                        TILE["band"] * 0.7, rows=14))
    return parts


def buckle(body, lo, t, th, lift, bones, colour, s):
    mb = MeshBox()
    p = lo.points(np.array([t]), np.array([th]), lift)[0]
    n = lo.radial(np.array([th]))[0]
    up = lo.a
    side = np.cross(up, n)
    w, h, d = 0.05 * s, 0.045 * s, 0.006
    for (dx, dz, sx, sz) in ((-0.5, 0, 0.18, 1.0), (0.5, 0, 0.18, 1.0), (0, -0.5, 1.0, 0.18), (0, 0.5, 1.0, 0.18)):
        mb.box(p + side * dx * w * 0.82 + up * dz * h * 0.82, (w * sx, d, h * sz), axes=(side, n, up))
    mb.box(p + n * 0.002, (0.006, d, h * 0.9), axes=(side, n, up))  # the tongue
    return mb.part(body, "buckle", "iron", colour, bones=bones)


def bandolier(body, lo, bones, pal, s):
    """A strap across the chest and back from the right shoulder to the left hip (strip UVs)."""
    rows, cols = 20, 3
    width = 0.05 * s
    uu = np.linspace(0.0, 1.0, rows)
    out_V, out_UV = [], []
    for face in (0.0, math.pi):  # front and back halves
        T = 0.95 - 0.85 * uu
        TH = np.where(face == 0.0, -0.95 + 1.9 * uu, math.pi + 0.95 - 1.9 * uu)
        V = []
        UV = []
        for i in range(rows):
            for vv in np.linspace(-0.5, 0.5, cols):
                V.append(lo.points(np.array([T[i] + vv * width / lo.L]), np.array([TH[i]]), 0.007)[0])
                UV.append((uu[i] * 0.9 * lo.L / TILE["strap"] + face, vv + 0.5))
        out_V.append(np.array(V))
        out_UV.append(np.array(UV))
    V = np.vstack(out_V)
    UV = np.vstack(out_UV)
    F = grid_faces(rows, cols, wrap=False) + grid_faces(rows, cols, wrap=False, base=rows * cols)
    W = transfer(body, bones, V)
    p = make_part("bandolier", "strap", V, F, W, UV=UV)
    p.C = np.tile(lin(pal["strap"]), (len(V), 1))
    p.rim = 0.002
    return orient_loft(p, lo)


def pauldrons(body, line, shape, only=None):
    s = body.scale
    out = []
    b = shape == "b"
    pal = PALETTE[line]
    for sd, sg in sides():
        if only and sd != only:
            continue
        sh = body.j(f"upperarm_{sd}")
        cl = body.j(f"clavicle_{sd}")
        bones = (f"clavicle_{sd}", f"upperarm_{sd}")
        m = body.mask(bones, 0.3) & (np.linalg.norm(body.P - sh, axis=1) < 0.17 * s)
        pts = body.P[m]
        c = sh + np.array([-sg * 0.03 * s, 0.0, -0.03 * s])
        axis = np.array([sg * (0.8 if b else 0.6), 0.0, 1.0])
        frame = frame_from(axis)
        deep = {"heavy": 34.0 if b else 24.0, "leather": 22.0, "cloth": 16.0}[line]

        def low(az, sg=sg, deep=deep):
            o = -sg * math.sin(az)  # +1: away from the neck, down the arm
            return 2.0 - deep * max(0.0, o) ** 1.5 + 14.0 * max(0.0, -o) - 6.0 * abs(math.cos(az))

        def shp(az, el, r, b=b, line=line):
            e = max(0.0, math.sin(math.radians(el)))
            k = 1.0 - (0.10 if line != "heavy" else 0.05) * e ** 2  # a shell, not a ball
            if b and line == "heavy":  # a raised ridge from front to back over the shoulder
                k += 0.08 * math.exp(-(math.sin(az) ** 2) / 0.02) * e
            return r * k

        margin = MARGIN[line] + ((0.016 if b else 0.01) if line == "heavy" else 0.006) * s
        dome = Dome(c, frame, low, 8, 22, pts, margin, shp, cone=20.0, iters=4)
        mat = SURF[line]
        if line == "heavy":
            per = math.radians(14.0 if b else 18.0)

            def dr(EL, AZ, per=per):
                return 0.006 * (1.0 - np.mod((math.pi / 2 - EL) / per, 1.0)) * (EL < math.radians(55))
        elif line == "leather":
            def dr(EL, AZ):
                return 0.004 * (1.0 - np.mod((math.pi / 2 - EL) / math.radians(22.0), 1.0)) * (EL < math.radians(50))
        else:
            dr = None
        part = dome_part(body, f"pauldron_{sd}", mat, dome, TILE[SURF[line]], bones=bones + ("spine_03",), dr=dr)
        colour_by(part, line)
        if line != "cloth":
            per = math.radians(14.0 if b else 18.0) if line == "heavy" else math.radians(22.0)
            g = np.clip((0.16 - np.mod((math.pi / 2 - part.EL) / per, 1.0)) / 0.08, 0, 1) * (part.EL < math.radians(55))
            part.C = shade(part.C, 0.8 * g, pal["recess"])
        lowest = np.array([math.radians(low(a)) for a in part.AZ])
        edge = np.clip(1.0 - (part.EL - lowest) / math.radians(9.0), 0, 1)
        part.C = shade(part.C, edge, pal["lining"] if line == "cloth" else (pal["trim_b"] if b else pal["trim"]))
        part.rim = RIM[line]
        out.append(part)
        if line == "heavy":
            # Rivets along the lower edge.
            mb = MeshBox()
            for k in range(7):
                az = 2 * math.pi * (k + 0.5) / 7
                if abs(math.sin(az)) > 0.95:
                    continue
                el = math.radians(low(az) + 9.0)
                p, d = dome.point(el, az, 0.0)
                mb.stud(p, d, 0.007 * s)
            out.append(mb.part(body, f"rivets_{sd}", "iron" if not b else "gold",
                               pal["trim_b"] if b else pal["buckle"], bones=bones))
        del cl
    return out


def rerebraces(body, line, shape):
    """Upper arm covers below the pauldron: plates (heavy), leather sleeves, (cloth: sleeves())."""
    s = body.scale
    b = shape == "b"
    pal = PALETTE[line]
    out = []
    f1 = {("heavy", "a"): 0.62, ("heavy", "b"): 0.98, ("leather", "a"): 0.45, ("leather", "b"): 0.9}[(line, shape)]
    for sd, sg in sides():
        lo, bones = arm_loft(body, sd, 0.2, f1, MARGIN[line] * 0.8, rows=10)
        if line == "heavy":
            per = 0.045 * s / lo.L

            def dr(T, TH, per=per):
                return 0.005 * saw(T, per)
            p = loft_part(body, f"rerebrace_{sd}", "steel", lo, 0.0, 1.0, 10, 18, bones, TILE["steel"], dr)
            colour_by(p, line)
            p.C = shade(p.C, 0.8 * np.clip((saw(p.T, per) - 0.86) / 0.08, 0, 1), pal["recess"])
            out.append(p)
            out.append(ring_strip(body, f"rerebrace_edge_{sd}", "trim", lo, 0.97, 0.02 * s, 0.003, bones,
                                  TILE["trim"], cols=18, rows=3, bulge=0.003, colour=pal["trim_b"] if b else pal["trim"]))
            if b:
                out.append(couter(body, sd, line, shape))
        else:
            p = loft_part(body, f"sleeve_{sd}", "leather", lo, 0.0, 1.0, 8, 20, bones, TILE["leather"])
            colour_by(p, line)
            out.append(p)
            out.append(ring_strip(body, f"sleeve_edge_{sd}", "strap", lo, 0.96, 0.025 * s, 0.003, bones,
                                  TILE["strap"], cols=20, rows=3, colour=pal["trim"]))
    return out


def couter(body, sd, line, shape):
    s = body.scale
    el = body.j(f"lowerarm_{sd}")
    bones = (f"upperarm_{sd}", f"lowerarm_{sd}")
    m = body.mask(bones, 0.3) & (np.linalg.norm(body.P - el, axis=1) < 0.09 * s)
    pts = body.P[m]
    out_dir = np.array([0.0, 0.6, 0.0]) + 0.4 * (el - body.j(f"upperarm_{sd}")) / np.linalg.norm(el - body.j(f"upperarm_{sd}"))
    frame = frame_from(out_dir, (0.0, 0.0, 1.0))
    dome = Dome(el, frame, lambda az: 10.0, 5, 16, pts, MARGIN[line] + 0.01, None, cone=24.0, iters=3)
    p = dome_part(body, f"couter_{sd}", "steel", dome, TILE["steel"], bones=bones)
    colour_by(p, line)
    p.C = shade(p.C, np.clip(1.0 - (p.EL - math.radians(10.0)) / math.radians(14.0), 0, 1), PALETTE[line]["trim_b"])
    p.rim = RIM[line]
    return p


def gorget(body, line, shape):
    s = body.scale
    neck = body.j("neck_01")
    m = body.mask(("neck_01", "spine_03", "Head"), 0.3)
    near = np.hypot(body.P[:, 0] - neck[0], body.P[:, 1] - neck[1]) < 0.42 * abs(body.j("upperarm_l")[0])
    near &= np.abs(body.P[:, 2] - neck[2]) < 0.12 * s
    pts = body.P[m & near]
    if len(pts) < 60:  # a neck hidden under a fur mantle and a beard: fit the gorget around all of it
        pts = body.P[(body.rw(("neck_01", "spine_03", "Head")) >= 0.3) & near]
    if len(pts) < 60:  # nothing skinned to the neck there: a plain collar around the neck joint
        ang = np.linspace(0, 2 * math.pi, 48, endpoint=False)
        pts = np.array([neck + np.array([math.cos(a) * 0.075 * s, math.sin(a) * 0.075 * s, dz * s])
                        for dz in np.linspace(-0.08, 0.1, 10) for a in ang])
    c0 = neck + np.array([0, 0, -0.045 * s])
    c1 = neck + np.array([0, 0, 0.065 * s])
    lo = fit_loft(pts, c0, c1, np.linspace(0, 1, 6), order=2, pct=90.0, cover=0.95, margin=MARGIN[line] + 0.006,
                  centre=True)
    flare = 0.022 if line == "heavy" else 0.014
    lo.R = lo.R + (flare * (1.0 - lo.ts) ** 2)[:, None]
    mat = SURF[line]
    p = loft_part(body, "gorget", mat, lo, 0.0, 1.0, 6, 32, ("neck_01", "spine_03"), TILE[mat],
                  weights=None)
    p.Wt = chain(body, p.V, [(c1[2], "neck_01"), (c0[2], "spine_03")])
    colour_by(p, line)
    if line == "heavy":
        per = 1.0 / 3.0
        p.C = shade(p.C, 0.7 * np.clip((saw(p.T, per) - 0.86) / 0.08, 0, 1), PALETTE[line]["recess"])
        p.C = shade(p.C, np.clip(1.0 - p.T / 0.15, 0, 1), PALETTE[line]["trim_b"])
    p.rim = RIM[line]
    return p


def skirt(body, line, shape, lo_torso, waist):
    """Robe skirt from the waist to mid-thigh (a) or below the knee (b): a loft around both legs whose
    weights follow each leg (thigh, then calf below the knee) away from the centre line, so it swings
    with the legs and does not let the knees through in a run or a sit."""
    s = body.scale
    b = shape == "b"
    hip, knee = body.j("thigh_l"), body.j("calf_l")
    hem = hip[2] - (0.30 if not b else 0.62) * s
    m = (body.rw(("pelvis",) + side_bones("thigh", "calf")) >= 0.4) & (body.rw(HELPERS) < 0.6)
    m &= (body.P[:, 2] < waist + 0.03) & (body.P[:, 2] > hem - 0.06) & (np.abs(body.P[:, 0]) < 0.34 * s)
    pts = body.P[m]
    yc = float(np.median(pts[:, 1]))
    rows = 9 if not b else 13
    ts = np.linspace(0.0, 1.0, rows)
    lo = fit_loft(pts, (0.0, yc, waist + 0.02 * s), (0.0, yc, hem), ts, order=3, pct=94.0, cover=0.985,
                  margin=MARGIN[line] + 0.012, centre=True, sm=1.0)
    # Flare towards the hem (more on b), soft folds.
    flare = (0.025 + 0.11 * ts ** 1.6) * s * (1.0 if b else 0.75)
    lo.R = lo.R + flare[:, None]
    lo.R = np.maximum.accumulate(lo.R, axis=0)  # never narrower below
    thK = 2 * math.pi * np.arange(Loft.K) / Loft.K
    lo.R = lo.R + 0.008 * np.outer(ts, np.cos(thK * 8.0) ** 2)
    V, T, TH = loft_grid(lo, 0.0, 1.0, rows, 32)
    F = grid_faces(rows, 32)
    circ = lo.circumference(0.5)
    n = max(1, int(round(circ / TILE["cloth"])))
    UV = np.stack([TH / (2 * math.pi) * n, T * lo.L / TILE["cloth"]], -1)
    # Weights: pelvis at the waist -> each leg's skin below; the centre front / back blends both legs.
    wts = []
    legs = {sd: (f"thigh_{sd}", f"calf_{sd}") for sd, _ in sides()}
    tl = transfer(body, legs["l"], V, 0.4)
    tr = transfer(body, legs["r"], V, 0.4)
    J = len(body.jnames)
    pel = body.jindex["pelvis"]
    for i, p in enumerate(V):
        t = float(T[i])
        lw = float(np.clip(0.15 + 1.4 * t, 0.0, 0.92)) if t > 0.03 else 0.0
        side = float(np.clip(0.5 + p[0] / (0.12 * s), 0.0, 1.0))  # +X is the character's left
        w = np.zeros(J)
        w[pel] += 1.0 - lw
        for j, x in tl[i]:
            w[j] += lw * side * x
        for j, x in tr[i]:
            w[j] += lw * (1.0 - side) * x
        wts.append({int(j): float(w[j]) for j in np.nonzero(w > 1e-5)[0]})
    W = grid_smooth_weights(body, wts, rows, 32, 2)
    p = make_part("skirt", "cloth", V, F, W, UV=UV, uperiod=float(n))
    colour_by(p, line)
    p.C = shade(p.C, 0.25 * np.cos(TH * 8.0) ** 8 * T, PALETTE[line]["recess"])
    p.rim = RIM[line]
    p.T, p.TH = T, TH
    orient_loft(p, lo)
    hemband = ring_strip(body, "hemband", "band", lo, 1.0 - 0.6 * (0.05 * s) / lo.L, (0.06 if b else 0.045) * s,
                         0.003, ("pelvis",), TILE["band"] * (1.2 if b else 0.9), cols=32, rows=3)
    hemband.Wt = [W[int(np.argmin(np.linalg.norm(V - q, axis=1)))] for q in hemband.V]
    return merge_parts("skirt", [p, hemband])


def merge_parts(name, parts):
    """Concatenates parts (keeping per-face materials and per-part u periods are dropped: only for parts
    whose seams do not matter)."""
    out = Part(name, parts[0].material)
    V, F, W, C, UV, fm = [], [], [], [], [], []
    base = 0
    for p in parts:
        V.append(p.V)
        F += [tuple(i + base for i in f) for f in p.F]
        W += list(p.Wt)
        C.append(p.C if p.C is not None else np.ones((len(p.V), 3)))
        UV.append(p.UV if p.UV is not None else np.zeros((len(p.V), 2)))
        fm += [p.material] * len(p.F) if p.fmat is None else list(p.fmat)
        base += len(p.V)
    out.V, out.F, out.Wt, out.C, out.UV, out.fmat = np.vstack(V), F, W, np.vstack(C), np.vstack(UV), fm
    out.uperiod = parts[0].uperiod
    out.rim = parts[0].rim
    out.subparts = parts
    return out


def sleeves(body, line, shape):
    """Robe sleeves: upper arm to the elbow (a) or bell sleeves past the elbow (b), with a band at the cuff."""
    s = body.scale
    b = shape == "b"
    out = []
    for sd, _sg in sides():
        sh, el, wr = body.j(f"upperarm_{sd}"), body.j(f"lowerarm_{sd}"), body.j(f"hand_{sd}")
        bones = (f"upperarm_{sd}", f"lowerarm_{sd}")
        m = body.mask(bones, 0.4)
        pts = body.P[m]
        c0 = sh + (el - sh) * 0.15
        c1 = el + (wr - el) * (0.15 if not b else 0.55)
        rows = 10
        ts = np.linspace(0, 1, rows)
        lo = fit_loft(pts, c0, c1, ts, order=2, pct=88.0, cover=0.95, margin=MARGIN[line] + 0.004,
                      front=(0.0, 0.0, 1.0), centre=True)
        if b:
            lo.R = lo.R + (0.07 * s * np.clip((ts - 0.45) / 0.55, 0, 1) ** 1.5)[:, None]
        else:
            lo.R = lo.R + (0.012 * s * ts)[:, None]
        p = loft_part(body, f"sleeve_{sd}", "cloth", lo, 0.0, 1.0, rows, 24, bones, TILE["cloth"])
        colour_by(p, line)
        p.rim = RIM[line]
        out.append(p)
        out.append(ring_strip(body, f"cuff_{sd}", "band", lo, 0.96, (0.05 if b else 0.035) * s, 0.003, bones,
                              TILE["band"] * 0.8, cols=24, rows=3))
    return out


# ---------------------------------------------------------------------------------------------
# Legs

def skirt_hem(body):
    """Lowest height of non-helper skin between the legs above the knees (a scale skirt, a long coat), else
    the crotch."""
    s = body.scale
    hip, knee = body.j("thigh_l"), body.j("calf_l")
    m = (np.abs(body.P[:, 0]) < 0.025 * s) & (body.rw(HELPERS) < 0.3) & (body.P[:, 2] < hip[2]) & \
        (body.P[:, 2] > knee[2] + 0.1 * s)
    z = float(body.P[m][:, 2].min()) if m.any() else hip[2] - 0.08 * s
    return float(np.clip(z, hip[2] - 0.36 * s, hip[2] - 0.07 * s))


def leg_loft(body, sd, z_top, z_bot, margin, pct=86.0, order=3, rows=12, exclude_skirt=None):
    hp, kn, an = body.j(f"thigh_{sd}"), body.j(f"calf_{sd}"), body.j(f"foot_{sd}")
    bones = (f"thigh_{sd}", f"calf_{sd}")
    m = body.mask(bones, 0.45)
    if exclude_skirt is not None:
        m &= body.P[:, 2] < exclude_skirt
    pts = body.P[m]

    def along(z):
        # point on the leg's bone chain at height z
        if z >= kn[2]:
            t = (hp[2] - z) / (hp[2] - kn[2])
            return hp + (kn - hp) * t
        t = (kn[2] - z) / (kn[2] - an[2])
        return kn + (an - kn) * t

    c0, c1 = along(z_top), along(z_bot)
    c0[0] = c1[0] = 0.5 * (c0[0] + c1[0])
    ts = np.linspace(0, 1, rows)
    lo = fit_loft(pts, c0, c1, ts, order=order, pct=pct, cover=0.95, margin=margin, centre=True, sm=1.2)
    return lo, bones


def build_legs(body, line, shape):
    s = body.scale
    b = shape == "b"
    pal = PALETTE[line]
    pel, hip, knee, ankle = body.j("pelvis"), body.j("thigh_l"), body.j("calf_l"), body.j("foot_l")
    top = pel[2] + 0.085 * s  # tucked under the cuirass's flared lowest lame
    shem = skirt_hem(body)
    fhem = min(shem - 0.01 * s, hip[2] - {"heavy": 0.12, "leather": 0.15, "cloth": 0.06}[line] * s)
    if line == "cloth":
        fhem = hip[2] - 0.06 * s
    bottom = {"heavy": knee[2] - 0.2 * s, "leather": knee[2] - 0.24 * s, "cloth": ankle[2] + 0.07 * s}[line]
    parts = []
    for sd, sg in sides():
        z_top = min(fhem + 0.04 * s, hip[2] - 0.05 * s) if line != "cloth" else hip[2] - 0.02 * s
        margin = MARGIN[line] if line != "cloth" else 0.022 * s
        lo, bones = leg_loft(body, sd, z_top, bottom, margin, rows=16,
                             exclude_skirt=(shem + 0.02 * s) if line != "cloth" and shem < hip[2] - 0.1 * s else None)
        tk = (z_top - knee[2]) / (z_top - bottom)
        if line == "cloth":
            # Baggy trousers gathered at the ankle.
            def dr(T, TH, tk=tk):
                return (0.03 * s * np.sin(math.pi * np.clip(T / 0.95, 0, 1)) ** 0.8 + 0.004 * np.cos(TH * 6.0) ** 2) * \
                    (1.15 if b else 1.0)
            p = loft_part(body, f"trouser_{sd}", "cloth", lo, 0.0, 1.0, 16, 28, bones + ("pelvis",), TILE["cloth"], dr)
            colour_by(p, line)
            p.C = shade(p.C, 0.2 * np.cos(p.TH * 6.0) ** 8, pal["recess"])
            p.rim = RIM[line]
            parts.append(p)
            parts.append(ring_strip(body, f"anklecuff_{sd}", "band", lo, 0.97, (0.05 if b else 0.035) * s, 0.004, bones,
                                    TILE["band"] * 0.8, cols=28, rows=3))
            if b:  # embroidered side bands down the outer leg
                th = -math.pi / 2 * sg
                parts.append(long_strip(body, f"sideband_{sd}", "band", lo, th, 0.04, 0.9, 0.045 * s, 0.005, bones,
                                        TILE["band"] * 0.7, rows=12))
            continue
        if line == "heavy":
            per = 0.05 * s / lo.L

            def dr(T, TH, tk=tk, per=per):
                lame = (T > tk + 0.08) | (T < tk - 0.1)
                return 0.008 * saw(T, per) * lame + 0.006 * np.clip(np.cos(TH), 0, 1) ** 2
            steps = [x for x in lame_steps(0.0, 1.0, per) if x > tk + 0.08 or x < tk - 0.1]
            tl = stepped(np.linspace(0.0, 1.0, 12), steps)
            p = loft_part(body, f"leg_{sd}", "steel", lo, 0.0, 1.0, 0, 22, bones + ("pelvis",), TILE["steel"], dr, tl)
            colour_by(p, line)
            g = np.clip((0.16 - saw(p.T, per)) / 0.08, 0, 1) * ((p.T > tk + 0.08) | (p.T < tk - 0.1))
            p.C = shade(p.C, 0.8 * g, pal["recess"])
            p.rim = RIM[line]
            parts.append(p)
            parts.append(ring_strip(body, f"legedge_{sd}", "trim", lo, 0.975, 0.02 * s, 0.003, bones, TILE["trim"],
                                    cols=26, rows=3, bulge=0.003, colour=pal["trim_b"] if b else pal["trim"]))
            if b:  # a ridge down the front of the thigh plate
                parts.append(long_strip(body, f"legridge_{sd}", "trim", lo, 0.0, 0.05, tk - 0.12, 0.018 * s, 0.006,
                                        bones, TILE["trim"], rows=8, colour=pal["trim_b"]))
        else:
            p = loft_part(body, f"leg_{sd}", "leather", lo, 0.0, 1.0, 14, 24, bones + ("pelvis",), TILE["leather"])
            colour_by(p, line)
            p.rim = RIM[line]
            parts.append(p)
            for tc in ((0.25, 0.55, 0.85) if b else (0.3, 0.75)):
                parts.append(ring_strip(body, f"legstrap_{sd}", "strap", lo, tc, 0.03 * s, 0.005, bones, TILE["strap"],
                                        cols=24, rows=3, colour=pal["strap"]))
    parts.append(fauld(body, line, shape, top, fhem if line != "cloth" else hip[2] - 0.05 * s))
    if line == "heavy":
        parts += knee_cops(body, line, shape)
    elif line == "leather" and b:
        parts += knee_cops(body, line, shape)
    if line == "cloth":
        parts.append(waist_sash(body, line, shape, top - 0.02 * s))
    return parts


def fauld(body, line, shape, top, hem):
    """Hip cover from the waist over the seat (and over a scale skirt, down to its hem): plate lames (heavy),
    split leather flaps (leather), a wrapped cloth band (cloth). Weights from the skin under it, smoothed."""
    s = body.scale
    b = shape == "b"
    pal = PALETTE[line]
    m = (body.rw(("pelvis",) + side_bones("thigh")) >= 0.3) & (body.rw(HELPERS) < 0.3)
    m &= (body.P[:, 2] < top + 0.03) & (body.P[:, 2] > hem - 0.04) & (np.abs(body.P[:, 0]) < 0.36 * s)
    pts = body.P[m]
    yc = float(np.median(pts[:, 1]))
    rows = max(5, int(round((top - hem) / (0.022 * s))) + 1)
    ts = np.linspace(0, 1, rows)
    lo = fit_loft(pts, (0.0, yc, top), (0.0, yc, hem), ts, order=3, pct=90.0, cover=0.95,
                  margin=MARGIN[line] + 0.002, centre=True, sm=1.0)
    flare = (0.006 + (0.022 if b else 0.012) * ts ** 1.5) * s
    lo.R = np.maximum.accumulate(lo.R + flare[:, None], axis=0)
    lo.tuck(body.helpers(), gap=0.006, max_pull=0.03, slab=0.03)
    cols = 32
    bones = ("pelvis",) + side_bones("thigh")
    mat = SURF[line]
    if line == "heavy":
        per = 0.045 * s / lo.L

        def dr(T, TH):
            return 0.009 * saw(T, per)  # each lame's lower edge over the next one
        tl = stepped(np.linspace(0.0, 1.0, rows), lame_steps(0.0, 1.0, per))
        p = loft_part(body, "fauld", "steel", lo, 0.0, 1.0, 0, cols, bones, TILE["steel"], dr, tl, wsmooth=3)
        colour_by(p, line)
        p.C = shade(p.C, 0.85 * np.clip((0.16 - saw(p.T, per)) / 0.08, 0, 1), pal["recess"])
        p.rim = RIM[line]
        out = [p, ring_strip(body, "faulds_edge", "trim", lo, 0.975, 0.02 * s, 0.003, bones, TILE["trim"], cols=cols,
                             rows=3, bulge=0.003, colour=pal["trim_b"] if b else pal["trim"])]
        return merge_parts("fauld", out)
    if line == "leather":
        # Flaps: the loft cut into panels with gaps at the sides and between the legs.
        p = loft_part(body, "fauld", "leather", lo, 0.0, 1.0, rows, cols, bones, TILE["leather"], wsmooth=3)
        colour_by(p, line)
        nflap = 8 if b else 6
        keep = []
        for fi, f in enumerate(p.F):
            th = float(np.mean(p.TH[list(f)]))
            t = float(np.mean(p.T[list(f)]))
            ph = (th / (2 * math.pi) * nflap + 0.5) % 1.0
            if t > 0.3 and (ph < 0.06 or ph > 0.94):
                continue
            keep.append(f)
        p.F = keep
        p.C = shade(p.C, 0.4 * np.clip((p.T - 0.85) / 0.15, 0, 1), pal["recess"])
        p.rim = RIM[line]
        belt = ring_strip(body, "hipbelt", "strap", lo, 0.04, 0.04 * s, 0.005, bones, TILE["strap"], cols=cols, rows=3,
                          colour=pal["strap"])
        return merge_parts("fauld", [p, belt])
    p = loft_part(body, "fauld", "cloth", lo, 0.0, 1.0, rows, cols, bones, TILE["cloth"], wsmooth=3)
    colour_by(p, line)
    p.rim = RIM[line]
    return p


def waist_sash(body, line, shape, z):
    """Cloth trousers' sash: a band around the waist (and, b, two hanging ends)."""
    s = body.scale
    b = shape == "b"
    m = (np.abs(body.P[:, 2] - z) < 0.07 * s) & body.mask(TORSO + side_bones("thigh"), 0.4)
    pts = body.P[m]
    yc = float(np.median(pts[:, 1]))
    lo = fit_loft(pts, (0.0, yc, z - 0.04 * s), (0.0, yc, z + 0.04 * s), np.linspace(0, 1, 4), order=3, pct=94.0,
                  cover=0.98, margin=0.03 * s, centre=True)
    lo.tuck(body.helpers(), gap=0.006, max_pull=0.03)
    p = ring_strip(body, "sash", "band", lo, 0.5, 0.07 * s, 0.0, ("pelvis", "spine_01"), TILE["band"], cols=40, rows=4,
                   bulge=0.004)
    if not b:
        return p
    ends = []
    for k, th in enumerate((0.35, 0.6)):
        q = lo.points(np.array([0.5]), np.array([th]), 0.008)[0]
        n = lo.radial(np.array([th]))[0]
        rows = 8
        V, UV = [], []
        for i in range(rows):
            u = i / (rows - 1)
            for vv in (-0.5, 0.5):
                side = np.cross(np.array([0, 0, 1.0]), n)
                V.append(q + side * vv * 0.06 * s + np.array([0, 0, -0.26 * s * u]) + n * 0.01 * u)
                UV.append((u * 0.26 * s / (TILE["band"] * 0.8), vv + 0.5))
        F = grid_faces(rows, 2, wrap=False)
        W = transfer(body, ("pelvis",) + side_bones("thigh"), np.array(V))
        ends.append(make_part(f"sashend_{k}", "band", V, F, W, UV=UV))
    return merge_parts("sash", [p] + ends)


def knee_cops(body, line, shape):
    s = body.scale
    b = shape == "b"
    out = []
    for sd, sg in sides():
        kn = body.j(f"calf_{sd}")
        bones = (f"thigh_{sd}", f"calf_{sd}")
        m = body.mask(bones, 0.3) & (np.linalg.norm(body.P - kn, axis=1) < 0.12 * s)
        pts = body.P[m]
        c = kn + np.array([0, 0.02 * s, 0.0])
        frame = frame_from((0, -1, 0.15), (0, 0, 1))

        def shp(az, el, r, b=b, sg=sg):
            k = 1.0 + (0.16 if b else 0.07) * max(0.0, math.sin(math.radians(el))) ** 3
            if b:  # a side wing on the outer side
                k += 0.18 * max(0.0, -math.sin(az) * sg) ** 6 * max(0.0, math.cos(math.radians(el)))
            return r * k

        dome = Dome(c, frame, lambda az, b=b: 15.0 if not b else 2.0, 6, 20, pts, MARGIN[line] + 0.012, shp,
                    cone=20.0, iters=3)
        mat = SURF[line]
        part = dome_part(body, f"knee_{sd}", mat, dome, TILE[mat], bones=bones)
        colour_by(part, line)
        part.C = shade(part.C, np.clip(1.0 - (part.EL - math.radians(2.0 if b else 15.0)) / math.radians(12.0), 0, 1),
                       PALETTE[line]["trim_b"] if b else PALETTE[line]["trim"])
        part.rim = RIM[line]
        out.append(part)
        if line == "heavy":
            mb = MeshBox()
            p, d = dome.point(math.radians(90.0), 0.0, 0.0)
            mb.stud(p, d, 0.008 * s)
            out.append(mb.part(body, f"kneestud_{sd}", "gold" if b else "iron",
                               PALETTE[line]["trim_b"] if b else PALETTE[line]["buckle"], bones=bones))
    return out


# ---------------------------------------------------------------------------------------------
# Hands

def build_hands(body, line, shape):
    s = body.scale
    b = shape == "b"
    pal = PALETTE[line]
    parts = []
    # Glove: the hand skin (fingers and palm) from just above the wrist, smoothed; cloth: fingerless wraps.
    cuts = []
    for sd, sg in sides():
        el, wr = body.j(f"lowerarm_{sd}"), body.j(f"hand_{sd}")
        d = (wr - el) / np.linalg.norm(wr - el)
        cuts.append((wr - d * 0.03 * s, -d, sg))
        if line == "cloth":
            cuts.append((wr + d * 0.075 * s, d, sg))
    bones = side_bones("hand") + FINGERS + side_bones("lowerarm")
    mat = SURF[line]
    off = {"heavy": 0.006, "leather": 0.004, "cloth": 0.004}[line]
    p = shell(body, "glove", mat, bones, cuts, off, src_smooth=10, smooth=4, min_island=0.2)
    p.V, p.F = decimate(p.V, p.F, 1100 if line != "cloth" else 500)
    p.Wt = transfer(body, bones, p.V, 0.5)
    colour_by(p, line)
    # UVs: cylindrical around each hand's axis.
    UV = np.zeros((len(p.V), 2))
    for sd, sg in sides():
        wr, el = body.j(f"hand_{sd}"), body.j(f"lowerarm_{sd}")
        ax = (wr - el) / np.linalg.norm(wr - el)
        sfa = frame_from(ax, (0.0, 0.0, 1.0))
        m = p.V[:, 0] * sg > 0
        rel = p.V[m] - wr
        th = np.arctan2(rel @ sfa[0], rel @ sfa[1])
        UV[m, 0] = th / (2 * math.pi) * 2.0
        UV[m, 1] = (rel @ ax) / TILE[mat]
    p.UV, p.uperiod = UV, 2.0
    if line == "heavy":
        # Knuckle plates: darker grooves across the back of the hand.
        for sd, sg in sides():
            wr, el = body.j(f"hand_{sd}"), body.j(f"lowerarm_{sd}")
            ax = (wr - el) / np.linalg.norm(wr - el)
            m = p.V[:, 0] * sg > 0
            u = (p.V[m] - wr) @ ax
            g = np.clip((0.16 - np.mod(u / (0.025 * s), 1.0)) / 0.08, 0, 1) * (u > 0.0)
            C = p.C[m]
            p.C[m] = shade(C, 0.6 * g, pal["recess"])
    p.rim = RIM[line] * 0.6
    parts.append(p)
    # Vambrace / bracer / wrap on the forearm.
    for sd, sg in sides():
        f0 = {("heavy", "a"): 1.38, ("heavy", "b"): 1.12, ("leather", "a"): 1.5, ("leather", "b"): 1.2,
              ("cloth", "a"): 1.55, ("cloth", "b"): 1.25}[(line, shape)]
        lo, fb = arm_loft(body, sd, f0, 1.98, MARGIN[line] * 0.8 + 0.002, rows=10)
        if b and line != "cloth":
            lo.R = lo.R + (0.025 * s * np.clip(1.0 - lo.ts / 0.35, 0, 1) ** 2)[:, None]  # flared cuff
        if b and line == "cloth":
            lo.R = lo.R + (0.04 * s * np.clip(1.0 - lo.ts / 0.5, 0, 1) ** 1.5)[:, None]
        fbones = fb + (f"hand_{sd}",)
        if line == "heavy":
            def dr(T, TH):
                return 0.004 * np.exp(-((np.cos(TH) - 1.0) / 0.08) ** 2)  # a ridge along the top
            q = loft_part(body, f"vambrace_{sd}", "steel", lo, 0.0, 1.0, 10, 24, fbones, TILE["steel"], dr)
            colour_by(q, line)
            q.rim = RIM[line]
            parts.append(q)
            parts.append(ring_strip(body, f"vambrace_edge_{sd}", "trim", lo, 0.03, 0.02 * s, 0.003, fbones,
                                    TILE["trim"], cols=24, rows=3, bulge=0.003, colour=pal["trim_b"] if b else pal["trim"]))
        elif line == "leather":
            q = loft_part(body, f"bracer_{sd}", "leather", lo, 0.0, 1.0, 8, 20, fbones, TILE["leather"])
            colour_by(q, line)
            q.rim = RIM[line]
            parts.append(q)
            for tc in ((0.2, 0.5, 0.8) if b else (0.3, 0.7)):
                parts.append(ring_strip(body, f"bracer_strap_{sd}", "strap", lo, tc, 0.02 * s, 0.004, fbones,
                                        TILE["strap"], cols=20, rows=3, colour=pal["strap"]))
        else:
            def dr(T, TH):
                return 0.0025 * np.cos((T * lo.L / (0.018 * s) + TH / (2 * math.pi)) * 2 * math.pi) ** 2
            q = loft_part(body, f"wrap_{sd}", "cloth", lo, 0.0, 1.0, 12, 20, fbones, TILE["cloth"], dr)
            colour_by(q, line)
            q.C = shade(q.C, 0.25 * np.cos((q.T * lo.L / (0.018 * s) + q.TH / (2 * math.pi)) * 2 * math.pi) ** 8,
                        pal["recess"])
            q.rim = RIM[line]
            parts.append(q)
            parts.append(ring_strip(body, f"wrap_band_{sd}", "band", lo, 0.04, 0.04 * s if b else 0.03 * s, 0.003, fbones,
                                    TILE["band"] * 0.7, cols=20, rows=3))
    return parts


# ---------------------------------------------------------------------------------------------
# Feet

def foot_loft(body, sd, margin, pct=90.0, toe_extra=0.0, rows=14):
    """A loft along the foot from behind the heel to beyond the toes; rings in vertical planes."""
    s = body.scale
    an, ball = body.j(f"foot_{sd}"), body.j(f"ball_{sd}")
    bones = (f"foot_{sd}", f"ball_{sd}")
    m = body.mask(bones, 0.4) & (body.P[:, 2] < an[2] + 0.02 * s)
    pts = body.P[m]
    fwd = np.array([ball[0] - an[0], ball[1] - an[1], 0.0])
    fwd /= np.linalg.norm(fwd)
    rel = pts - an
    along = rel @ fwd
    h0 = float(np.percentile(along, 0.5)) - 0.004
    h1 = float(np.percentile(along, 99.5)) + 0.004 + toe_extra
    zc = body.floor + 0.045 * s
    c0 = np.array([an[0], an[1], zc]) + fwd * h0
    c1 = np.array([an[0], an[1], zc]) + fwd * h1
    ts = np.linspace(0, 1, rows)
    lo = fit_loft(pts, c0, c1, ts, order=3, pct=pct, cover=0.96, margin=margin, front=(0.0, 0.0, 1.0),
                  centre=True, sm=1.0)
    return lo, bones


def closed_loft_part(body, name, mat, lo, rows, cols, bones, tile, dr=None, floor=None, cap0=True, cap1=True,
                     tdist=None):
    """A loft closed at both ends with pole vertices (feet)."""
    p = loft_part(body, name, mat, lo, 0.0, 1.0, rows, cols, bones, tile, dr, tdist)
    V, F, UV = list(p.V), list(p.F), list(p.UV)
    W = list(p.Wt)
    T, TH = list(p.T), list(p.TH)
    for cap, r0, t in ((cap0, 0, 0.0), (cap1, rows - 1, 1.0)):
        if not cap:
            continue
        ring = np.arange(r0 * cols, r0 * cols + cols)
        c = p.V[ring].mean(0) + lo.a * (-1 if t == 0.0 else 1) * 0.35 * float(np.mean(lo.radius(np.full(cols, t),
                                                                                              2 * math.pi * np.arange(cols) / cols)))
        V.append(c)
        UV.append(p.UV[ring].mean(0))
        W.append(W[int(ring[0])])
        T.append(t)
        TH.append(0.0)
        pi = len(V) - 1
        for k in range(cols):
            a, b2 = int(ring[k]), int(ring[(k + 1) % cols])
            F.append((pi, b2, a) if t == 0.0 else (pi, a, b2))
    p.V, p.F, p.UV, p.Wt = np.array(V), F, np.array(UV), W
    p.T, p.TH = np.array(T), np.array(TH)
    orient_loft(p, lo)
    if floor is not None:
        p.V[:, 2] = np.maximum(p.V[:, 2], floor)
    return p


def build_feet(body, line, shape):
    s = body.scale
    b = shape == "b"
    pal = PALETTE[line]
    knee, ankle = body.j("calf_l"), body.j("foot_l")
    top = {("heavy", "a"): knee[2] - 0.15 * s, ("heavy", "b"): knee[2] - 0.05 * s,
           ("leather", "a"): ankle[2] + 0.13 * s, ("leather", "b"): knee[2] - 0.09 * s,
           ("cloth", "a"): ankle[2] + 0.035 * s, ("cloth", "b"): ankle[2] + 0.09 * s}[(line, shape)]
    floor = body.floor + 0.002
    parts = []
    mat = SURF[line]
    for sd, sg in sides():
        toe = {("heavy", "b"): 0.03, ("cloth", "b"): 0.05}.get((line, shape), 0.0) * s
        lo, fbones = foot_loft(body, sd, MARGIN[line] * 0.8 + 0.002, toe_extra=toe)
        if line == "heavy" or (line == "cloth" and b):
            # A pointed / upturned toe: narrow the last rings, lift the tip (cloth).
            k = np.clip((lo.ts - 0.7) / 0.3, 0, 1)
            lo.R = lo.R * (1.0 - (0.45 if b else 0.15) * k ** 2)[:, None]
        rows = 16
        bones = fbones + (f"calf_{sd}",)
        if line == "heavy":
            per = 0.04 * s / lo.L

            def dr(T, TH, per=per):
                return 0.004 * (1.0 - saw(T, per)) * (T > 0.4) * (T < 0.85)
        else:
            dr = None
        p = closed_loft_part(body, f"foot_{sd}", mat, lo, rows, 24, bones, TILE[mat], dr, floor)
        if line == "cloth" and b:
            tip = p.T > 0.8
            p.V[tip, 2] += (p.T[tip] - 0.8) ** 2 * 1.2 * s
        colour_by(p, line)
        sole = np.clip(1.0 - (p.V[:, 2] - floor) / (0.018 * s), 0.0, 1.0)
        p.C = shade(p.C, 0.85 * sole, pal["strap"] if line != "cloth" else pal["lining"])
        if line == "heavy":
            p.C = shade(p.C, 0.8 * np.clip((0.16 - saw(p.T, per)) / 0.08, 0, 1) * (p.T > 0.4) * (p.T < 0.85),
                        pal["recess"])
        p.rim = RIM[line]
        parts.append(p)
        # The shaft: from the ankle up to `top` (greave, boot leg, slipper collar).
        an, kn = body.j(f"foot_{sd}"), body.j(f"calf_{sd}")
        z0 = body.floor + 0.06 * s
        m = body.mask((f"calf_{sd}", f"foot_{sd}"), 0.45) & (body.P[:, 2] > z0 - 0.02) & (body.P[:, 2] < top + 0.04)
        pts = body.P[m]
        d = (kn - an) / np.linalg.norm(kn - an)
        c0 = an + d * ((z0 - an[2]) / d[2])
        c1 = an + d * ((top - an[2]) / d[2])
        rows2 = max(4, int(round((top - z0) / (0.03 * s))) + 1)
        ts = np.linspace(0, 1, rows2)
        slo = fit_loft(pts, c0, c1, ts, order=3, pct=88.0, cover=0.96, margin=MARGIN[line] * 0.8 + 0.003,
                       centre=True, sm=1.2)
        if line == "leather" and b:
            slo.R = slo.R + (0.018 * s * np.clip((ts - 0.75) / 0.25, 0, 1))[:, None]  # folded cuff flare
        sb = (f"calf_{sd}", f"foot_{sd}")
        if line == "heavy":
            per2 = 0.05 * s / slo.L

            def dr2(T, TH, per2=per2):
                return 0.004 * saw(T, per2) * (T < 0.3) + 0.004 * np.clip(np.cos(TH), 0, 1) ** 3
            q = loft_part(body, f"greave_{sd}", "steel", slo, 0.0, 1.0, rows2, 24, sb, TILE["steel"], dr2)
            colour_by(q, line)
            q.rim = RIM[line]
            parts.append(q)
            parts.append(ring_strip(body, f"greave_edge_{sd}", "trim", slo, 0.97, 0.02 * s, 0.003, sb, TILE["trim"],
                                    cols=24, rows=3, bulge=0.003, colour=pal["trim_b"] if b else pal["trim"]))
            if b:  # a raised front ridge on the greave
                parts.append(long_strip(body, f"greave_ridge_{sd}", "trim", slo, 0.0, 0.1, 0.95, 0.016 * s, 0.006, sb,
                                        TILE["trim"], rows=8, colour=pal["trim_b"]))
        elif line == "leather":
            q = loft_part(body, f"bootleg_{sd}", "leather", slo, 0.0, 1.0, rows2, 22, sb, TILE["leather"])
            colour_by(q, line)
            q.rim = RIM[line]
            parts.append(q)
            for tc in ((0.25, 0.6) if b else (0.45,)):
                parts.append(ring_strip(body, f"bootstrap_{sd}", "strap", slo, tc, 0.022 * s, 0.004, sb, TILE["strap"],
                                        cols=22, rows=3, colour=pal["strap"]))
                parts.append(buckle(body, slo, tc, -math.pi / 2 * sg, 0.008, sb, pal["buckle"], s * 0.5))
            if b:
                parts.append(ring_strip(body, f"bootcuff_{sd}", "strap", slo, 0.93, 0.05 * s, 0.006, sb, TILE["strap"],
                                        cols=22, rows=3, colour=pal["trim"]))
        else:
            q = loft_part(body, f"collar_{sd}", "cloth", slo, 0.0, 1.0, rows2, 22, sb, TILE["cloth"])
            colour_by(q, line)
            q.rim = RIM[line]
            parts.append(q)
            parts.append(ring_strip(body, f"slipper_band_{sd}", "band", slo, 0.9, 0.03 * s, 0.003, sb,
                                    TILE["band"] * 0.6, cols=22, rows=3))
    return parts


# ---------------------------------------------------------------------------------------------
# Head

def head_cloud(body):
    m = (body.rw(("Head",)) >= 0.5) & (body.rw(HELPERS) < 0.3)
    return body.P[m]


def fold(az):
    return abs((az + math.pi) % (2 * math.pi) - math.pi)  # 0 = front, pi = back


def head_shape(line, shape):
    """(lower edge elevation per azimuth, radius style, margin) of the head piece."""
    if line == "heavy" and shape == "a":
        return (lambda az: -10.0 - 40.0 * (1 - math.cos(az)) / 2,
                lambda az, el, r: r * (1.0 + 0.08 * max(0.0, math.sin(math.radians(el))) ** 3), 0.016)
    if line == "heavy":
        return (lambda az: -4.0 if fold(az) < math.radians(28) else -60.0 + 8.0 * math.cos(fold(az)),
                lambda az, el, r: r * (1.0 + 0.40 * max(0.0, math.sin(math.radians(el))) ** 5), 0.02)
    if line == "leather" and shape == "a":
        return (lambda az: 4.0 if fold(az) < math.radians(35) else (-48.0 if fold(az) < math.radians(120) else -32.0),
                lambda az, el, r: r * (1.0 + 0.03 * max(0.0, math.sin(math.radians(el))) ** 2), 0.012)
    if line == "leather":
        return (lambda az: 0.0 if fold(az) < math.radians(32) else -72.0 - 10.0 * (fold(az) / math.pi),
                lambda az, el, r: r * (1.0 + 0.12 * max(0.0, math.sin(math.radians(el))) ** 2), 0.03)
    tall = 0.18 if shape == "a" else 0.55
    power = 2 if shape == "a" else 3

    def turban(az, el, r):
        e = math.radians(el)
        return r * (1.0 + tall * max(0.0, math.sin(e)) ** power)

    return (lambda az: 6.0 - 26.0 * (fold(az) / math.pi), turban, 0.03 if shape == "a" else 0.032)


def build_head(body, line, shape):
    s = body.scale
    b = shape == "b"
    pal = PALETTE[line]
    pts = head_cloud(body)
    hj = body.j("Head")
    top = float(pts[:, 2].max())
    brow = hj[2] + 0.52 * (top - hj[2])
    c = np.array([hj[0], float(np.mean(pts[pts[:, 2] > brow][:, 1])), brow])
    frame = frame_from((0, 0, 1), (0, -1, 0))
    low, shp, margin = head_shape(line, shape)
    dome = Dome(c, frame, low, 12, 32, pts, margin, shp, cone=18.0, iters=4)
    mat = SURF[line]
    if line == "cloth":
        # Turban wraps: diagonal folds.
        def dr(EL, AZ):
            return 0.006 * np.abs(np.sin((EL * 5.0 + 0.8 * AZ)))
    elif line == "leather" and not b:
        def dr(EL, AZ):  # panel seams meeting at the crown
            return -0.0025 * np.exp(-(np.sin(AZ * 3.0) / 0.06) ** 2) * (EL > math.radians(5))
    else:
        dr = None
    part = dome_part(body, "head", mat, dome, TILE[mat], bone="Head", dr=dr)
    if line == "leather" and b:
        part.Wt = chain(body, part.V, [(hj[2] - 0.02, "Head"), (hj[2] - 0.12 * s, "neck_01"),
                                       (body.j("neck_01")[2] - 0.05 * s, "spine_03")])
    colour_by(part, line)
    lowest = np.array([math.radians(low(a)) for a in part.AZ])
    edge = np.clip(1.0 - (part.EL - lowest) / math.radians(8.0), 0, 1)
    if line == "heavy":
        part.C = shade(part.C, edge, pal["trim_b"] if b else pal["trim"])
        # A raised brow band (darker groove above it).
        g = np.exp(-((part.EL - lowest - math.radians(16.0)) / math.radians(2.5)) ** 2)
        part.C = shade(part.C, 0.6 * g, pal["recess"])
    elif line == "leather":
        part.C = shade(part.C, edge, pal["trim"])
        if not b:
            seam = np.exp(-(np.sin(part.AZ * 3.0) / 0.06) ** 2) * (part.EL > math.radians(5))
            part.C = shade(part.C, 0.6 * seam, pal["recess"])
    else:
        folds = np.abs(np.sin(part.EL * 5.0 + 0.8 * part.AZ))
        part.C = shade(part.C, 0.3 * (1 - folds) ** 4, pal["recess"])
    part.rim = RIM[line] + 0.002
    parts = [part]
    d_front = np.array([[0.0, -1.0, 0.0]])
    front = c + d_front[0] * float(hull_radius(pts, c, d_front, 18.0, margin)[0])
    if line == "heavy":
        mb = MeshBox()
        mb.box(front + np.array([0, -0.006, -0.035 * s]), (0.02 * s, 0.012, 0.09 * s))  # nasal guard
        parts.append(mb.part(body, "nasal", "gold" if b else "steel", pal["trim_b"] if b else pal["trim"], bone="Head"))
        if b:  # a swept fin crest from the brow over the crown to the nape
            n = 18
            V, F = [], []
            th = 0.005
            for i in range(n):
                t = i / (n - 1)
                ang = math.radians(50.0 - 160.0 * t)  # + = towards the front
                el, az = math.pi / 2 - abs(ang), (0.0 if ang > 0 else math.pi)
                p, dd = dome.point(el, az, -0.004)
                h = (0.01 + 0.032 * math.sin(math.pi * min(1.0, t * 1.15)) ** 0.8) * s
                h += 0.012 * s * max(0.0, (t - 0.75) / 0.25) ** 2  # a swept tail
                for side in (-1.0, 1.0):
                    V.append(p + np.array([side * th, 0, 0]))
                    V.append(p + dd * h + np.array([side * th * 0.5, 0, 0]))
            for i in range(n - 1):
                a, b2 = 4 * i, 4 * (i + 1)
                F.append((a + 0, a + 1, b2 + 1, b2 + 0))      # side towards -X
                F.append((a + 2, b2 + 2, b2 + 3, a + 3))      # side towards +X
                F.append((a + 1, a + 3, b2 + 3, b2 + 1))      # top
            F.append((0, 2, 3, 1))
            e = 4 * (n - 1)
            F.append((e, e + 1, e + 3, e + 2))
            fin = make_part("crest", "gold", np.array(V), F, rigid(body, "Head", len(V)))
            fin.C = np.tile(lin(pal["trim_b"]), (len(V), 1))
            fin.smooth = False
            parts.append(fin)
        if b:
            for sd, sg in sides():
                cg = MeshBox()
                az = math.radians(55.0) * (-sg)  # towards the character's side (frame side = -X)
                for k in range(3):
                    el = math.radians(-12.0 - 14.0 * k)
                    p, dd = dome.point(el, az, 0.004)
                    cg.box(p, (0.065 * s * (1.0 - 0.18 * k), 0.008, 0.05 * s),
                           axes=(np.cross(np.array([0, 0, 1.0]), dd) / max(np.linalg.norm(np.cross(np.array([0, 0, 1.0]), dd)), 1e-6),
                                 dd, np.array([0, 0, 1.0])))
                q = cg.part(body, f"cheek_{sd}", "steel", pal["base"], bone="Head")
                q.smooth = False
                parts.append(q)
        # Rivets around the brow band.
        rv = MeshBox()
        for k in range(14):
            az = 2 * math.pi * k / 14
            if fold(az) < math.radians(20):
                continue
            el = math.radians(low(az) + 9.0)
            p, dd = dome.point(el, az, 0.0)
            rv.stud(p, dd, 0.006 * s)
        parts.append(rv.part(body, "rivets", "gold" if b else "iron", pal["trim_b"] if b else pal["buckle"], bone="Head"))
    elif line == "leather" and not b:
        # Ear flaps tied under... a strap over the crown and a stud on top.
        mb = MeshBox()
        p, dd = dome.point(math.radians(89.0), 0.0, 0.0)
        mb.stud(p, dd, 0.012 * s)
        parts.append(mb.part(body, "crownstud", "iron", pal["stud"], bone="Head"))
    elif line == "leather":
        # The hood's brim strap.
        rv = MeshBox()
        for k in range(9):
            az = math.radians(-35.0 + 70.0 * k / 8)
            p, dd = dome.point(math.radians(3.0), az, 0.002)
            rv.stud(p, dd, 0.005 * s)
        parts.append(rv.part(body, "brimstuds", "iron", pal["stud"], bone="Head"))
    else:
        # A band around the lower edge of the turban; b: a jewel and a plume.
        band = []
        rows, cols = 3, 32
        V, UV = [], []
        for i in range(rows):
            for k in range(cols):
                az = 2 * math.pi * k / cols
                el = math.radians(low(az) + 2.0 + 10.0 * i / (rows - 1))
                p, dd = dome.point(el, az, 0.006)
                V.append(p)
                UV.append((k / cols * 6.0, i / (rows - 1)))
        F = [tuple(reversed(q)) for q in grid_faces(rows, cols)]
        bp = make_part("turban_band", "band", V, F, rigid(body, "Head", len(V)), UV=UV, uperiod=6.0)
        bp.C = np.tile(lin("#ffffff"), (len(V), 1))
        bp.rim = 0.002
        parts.append(bp)
        del band
        if b:
            nb = MeshBox()
            jewel = front + np.array([0, -0.01, 0.05 * s])
            nb.box(jewel, (0.03, 0.016, 0.036), rot=45.0)
            parts.append(nb.part(body, "jewel", "gem", "#d23a3a", bone="Head"))
            pl = MeshBox()
            for k in range(7):  # a plume rising from the jewel, bending back
                t = k / 6
                pl.box(jewel + np.array([0, 0.035 * t * t, 0.03 + 0.13 * t]) * s, (0.014, 0.01, 0.04))
            parts.append(pl.part(body, "plume", "gold", pal["gold"], bone="Head"))
    return parts


BUILDERS = {"head": build_head, "chest": build_chest, "legs": build_legs, "hands": build_hands, "feet": build_feet}


# ---------------------------------------------------------------------------------------------
# Assembly and export

def add_rim(part):
    """Gives the open edges a thickness: each boundary edge gets a quad going inwards by part.rim."""
    if part.rim <= 0.0:
        return part
    n = vertex_normals(part.V, part.F)
    edges = {}
    fm = part.fmat if part.fmat is not None else [part.material] * len(part.F)
    for fi, f in enumerate(part.F):
        for k in range(len(f)):
            a, b = f[k], f[(k + 1) % len(f)]
            edges.setdefault((min(a, b), max(a, b)), []).append((a, b, fi))
    V, C, W, UV = list(part.V), list(part.C), list(part.Wt), list(part.UV)
    inner = {}

    def inner_of(i):
        if i not in inner:
            inner[i] = len(V)
            V.append(part.V[i] - n[i] * part.rim)
            C.append(part.C[i] * 0.6)
            W.append(part.Wt[i])
            UV.append(part.UV[i] + np.array([0.0, 0.02]))
        return inner[i]

    F = list(part.F)
    FM = list(fm)
    for lst in edges.values():
        if len(lst) == 1:
            a, b, fi = lst[0]
            F.append((b, a, inner_of(a), inner_of(b)))
            FM.append(fm[fi])
    part.V, part.C, part.Wt, part.F, part.UV, part.fmat = np.array(V), np.array(C), W, F, np.array(UV), FM
    return part


def triangulate(F, FM):
    T, TM = [], []
    for f, m in zip(F, FM):
        for k in range(1, len(f) - 1):
            T.append((f[0], f[k], f[k + 1]))
            TM.append(m)
    return np.array(T, np.int64).reshape(-1, 3), TM


def corner_normals(V, T, sharp_deg, smooth=True):
    fn = np.cross(V[T[:, 1]] - V[T[:, 0]], V[T[:, 2]] - V[T[:, 0]])
    area = np.linalg.norm(fn, axis=1)
    fu = fn / np.maximum(area[:, None], 1e-15)
    if not smooth:
        return np.repeat(fu[:, None, :], 3, 1)
    # Smooth over welded positions (lofts and strips have no duplicated seam vertices, but rims do not
    # share their corner vertices with the surface either).
    key = np.round(V / 1e-5).astype(np.int64)
    _, wid = np.unique(key, axis=0, return_inverse=True)
    wid = wid.reshape(-1)
    inc = [[] for _ in range(int(wid.max()) + 1)]
    for t, tri in enumerate(T):
        for v in tri:
            inc[wid[v]].append(t)
    inc = [np.array(x, np.int64) for x in inc]
    cosl = math.cos(math.radians(sharp_deg))
    out = np.zeros((len(T), 3, 3))
    for t, tri in enumerate(T):
        for k, v in enumerate(tri):
            ids = inc[wid[v]]
            sel = ids[(fu[ids] @ fu[t]) >= cosl]
            acc = fn[sel].sum(0)
            nn = np.linalg.norm(acc)
            out[t, k] = acc / nn if nn > 1e-15 else fu[t]
    return out


def orient_outward(body, part):
    """Flips the part's faces if most of them face the body (the engine culls back faces). Returns the
    fraction of faces facing away from the body after the fix."""
    tree = body.bvh(("all",), np.ones(len(body.T), bool))[0]
    out = 0
    for f in part.F:
        p = part.V[list(f)]
        c = p.mean(0)
        n = np.cross(p[1] - p[0], p[2] - p[0])
        loc, nrm, _i, _d = tree.find_nearest(Vector(c))
        d = c - np.array(loc)
        if np.linalg.norm(d) < 1e-6:
            d = np.array(nrm)
        out += 1 if float(np.dot(n, d)) > 0.0 else 0
    frac = out / max(len(part.F), 1)
    if frac < 0.5:
        part.F = [tuple(reversed(f)) for f in part.F]
        frac = 1.0 - frac
    return frac


def flatten(parts):
    out = []
    for p in parts:
        out += p.subparts if getattr(p, "subparts", None) else [p]
    return out


def export_piece(body, line, shape, slot, parts, path):
    """Merges the parts per material into primitives with split corners and writes the glb."""
    groups = {}
    tris_total = 0
    for p in flatten(parts):
        if p.C is None:
            colour_by(p, line)
        if p.UV is None:
            p.UV = np.zeros((len(p.V), 2))
        # Lofts and domes are wound from their own axis / centre; boxes are closed and wound outwards.
        frac = orient_outward(body, p) if p.smooth and not getattr(p, "oriented", False) else 1.0
        if frac < 0.6:
            print(f"WARNING {body.look} {line}_{shape}_{slot}/{p.name}: only {frac:.0%} of the faces face outwards")
        add_rim(p)
        fm = p.fmat if p.fmat is not None else [p.material] * len(p.F)
        T, TM = triangulate(p.F, fm)
        if len(T) == 0:
            continue
        a = np.linalg.norm(np.cross(p.V[T[:, 1]] - p.V[T[:, 0]], p.V[T[:, 2]] - p.V[T[:, 0]]), axis=1) * 0.5
        keep = a > 2e-7
        T = T[keep]
        TM = [m for m, k in zip(TM, keep) if k]
        cn = corner_normals(p.V, T, p.sharp, p.smooth)
        corners = {}
        for t, tri in enumerate(T):
            g = groups.setdefault(TM[t], dict(P=[], N=[], UV=[], C=[], J=[], W=[], I=[]))
            uvs = p.UV[tri].copy()
            if p.uperiod:
                for k in (1, 2):
                    uvs[k, 0] += p.uperiod * round((uvs[0, 0] - uvs[k, 0]) / p.uperiod)
            for k, v in enumerate(tri):
                key = (TM[t], int(v), tuple(np.round(cn[t, k] * 1000).astype(int)), tuple(np.round(uvs[k] * 4096).astype(int)))
                idx = corners.get(key)
                if idx is None:
                    idx = corners[key] = len(g["P"])
                    g["P"].append(p.V[v])
                    g["N"].append(cn[t, k])
                    g["UV"].append(uvs[k])
                    g["C"].append(np.append(np.clip(p.C[v], 0.0, 1.0), 1.0))
                    w = list(p.Wt[v])[:4]
                    g["J"].append([j for j, _ in w] + [0] * (4 - len(w)))
                    g["W"].append([x for _, x in w] + [0.0] * (4 - len(w)))
                g["I"].append(idx)
        tris_total += len(T)
    prims = []
    for mname in sorted(groups):
        g = groups[mname]
        prims.append((mname, np.array(g["P"]), np.array(g["N"]), np.array(g["UV"]), np.array(g["C"]),
                      np.array(g["J"]), np.array(g["W"]), np.array(g["I"])))
    write_piece_glb(path, body, f"{line}_{shape}_{slot}", prims)
    return tris_total, prims


def piece_names():
    return [f"{l}_{sh}_{sl}" for l in LINES for sh in SHAPES for sl in SLOTS]


def build_look(look, only=None, sets=None):
    body = Body(look)
    meta = {}
    built = {}
    for line in LINES:
        for shape in SHAPES:
            if sets and f"{line}_{shape}" not in sets:
                continue
            for slot in SLOTS:
                name = f"{line}_{shape}_{slot}"
                if only and name not in only:
                    continue
                vc.reset_scene()
                parts = BUILDERS[slot](body, line, shape)
                tris, prims = export_piece(body, line, shape, slot, parts, os.path.join(OUT_DIR, look, f"{name}.glb"))
                meta[name] = dict(tris=int(tris), parts=len(flatten(parts)), materials=len(prims))
                built[name] = prims
                print(f"ARMOR {look} {name}: {tris} tris, {len(flatten(parts))} part(s), {len(prims)} material(s)")
    return body, meta, built


# ---------------------------------------------------------------------------------------------
# Previews and icons

def preview_material(mname):
    mat = bpy.data.materials.get(f"pv_{mname}")
    if mat is not None:
        return mat
    stem, r, m, ns = MATERIALS[mname]
    mat = bpy.data.materials.new(f"pv_{mname}")
    mat.use_nodes = True
    nt = mat.node_tree
    bsdf = nt.nodes.get("Principled BSDF")
    attr = nt.nodes.new("ShaderNodeAttribute")
    attr.attribute_name = "Col"
    if stem:
        def img(suffix, colour):
            node = nt.nodes.new("ShaderNodeTexImage")
            node.image = bpy.data.images.load(os.path.join(TEX_DIR, f"{stem}{suffix}.png"), check_existing=True)
            node.image.colorspace_settings.name = "sRGB" if colour else "Non-Color"
            return node
        cimg, nimg, oimg = img("_c", True), img("_n", False), img("_orm", False)
        mix = nt.nodes.new("ShaderNodeMix")
        mix.data_type = 'RGBA'
        mix.blend_type = 'MULTIPLY'
        mix.inputs["Factor"].default_value = 1.0
        nt.links.new(attr.outputs["Color"], mix.inputs[6])
        nt.links.new(cimg.outputs["Color"], mix.inputs[7])
        nt.links.new(mix.outputs[2], bsdf.inputs["Base Color"])
        nmap = nt.nodes.new("ShaderNodeNormalMap")
        nmap.inputs["Strength"].default_value = ns
        nt.links.new(nimg.outputs["Color"], nmap.inputs["Color"])
        nt.links.new(nmap.outputs["Normal"], bsdf.inputs["Normal"])
        sep = nt.nodes.new("ShaderNodeSeparateColor")
        nt.links.new(oimg.outputs["Color"], sep.inputs["Color"])
        nt.links.new(sep.outputs[1], bsdf.inputs["Roughness"])
        nt.links.new(sep.outputs[2], bsdf.inputs["Metallic"])
    else:
        nt.links.new(attr.outputs["Color"], bsdf.inputs["Base Color"])
        bsdf.inputs["Roughness"].default_value = r
        bsdf.inputs["Metallic"].default_value = m
    mat.use_backface_culling = True
    return mat


def piece_object(name, prims, body, arm=None, tint=(1.0, 1.0, 1.0)):
    """A Blender object of an exported piece (corners as exported), skinned to `arm` if given."""
    verts, faces, cols, mats, groups, uvs = [], [], [], [], [], []
    materials = []
    for mname, P, N, UV, C, J, W, I in prims:
        base = len(verts)
        verts += [tuple(p) for p in P]
        cols += [tuple(np.append(c[:3] * np.array(tint), 1.0)) for c in C]
        uvs += [tuple(u) for u in UV]
        mi = len(materials)
        materials.append(preview_material(mname))
        for t in I.reshape(-1, 3):
            faces.append(tuple(int(base + i) for i in t))
            mats.append(mi)
        groups += [list(zip(j, w)) for j, w in zip(J, W)]
    me = bpy.data.meshes.new(name)
    me.from_pydata(verts, [], faces)
    for m in materials:
        me.materials.append(m)
    me.polygons.foreach_set("material_index", mats)
    attr = me.color_attributes.new("Col", 'FLOAT_COLOR', 'POINT')
    for i, c in enumerate(cols):
        attr.data[i].color = c
    uvl = me.uv_layers.new(name="UVMap")
    lv = np.zeros(len(me.loops), np.int64)
    me.loops.foreach_get("vertex_index", lv)
    # glTF v runs down the image, Blender's up.
    uva = np.array(uvs)[lv]
    uva[:, 1] = 1.0 - uva[:, 1]
    uvl.data.foreach_set("uv", uva.reshape(-1))
    me.shade_smooth()
    obj = vc.link(bpy.data.objects.new(name, me))
    if arm is not None:
        vg = {}
        for vi, g in enumerate(groups):
            for j, w in g:
                if w <= 0:
                    continue
                bn = body.jnames[int(j)]
                if bn not in vg:
                    vg[bn] = obj.vertex_groups.new(name=bn)
                vg[bn].add([vi], float(w), 'REPLACE')
        obj.parent = arm
        mod = obj.modifiers.new("Armature", 'ARMATURE')
        mod.object = arm
    return obj


PREVIEW_POSES = (("front", None, 0.0), ("side", None, 0.0), ("back", None, 0.0), ("run", "run", 0.25),
                 ("attack", "sword_attack", 0.45), ("sit", "sit_ground_idle", 0.5), ("death", "death", 1.0))
# The tier tint each preview row is shown with (shape a: SaÄŸlam, shape b: Korlu; items.toml).
PREVIEW_TINT = {("heavy", "a"): (205, 208, 214), ("heavy", "b"): (242, 146, 96), ("leather", "a"): (170, 124, 82),
                ("leather", "b"): (186, 96, 60), ("cloth", "a"): (125, 145, 175), ("cloth", "b"): (205, 76, 66)}


def setup_studio(resolution, samples=16):
    import gen_attachments as ga
    cam = ga.setup_studio(resolution)
    bpy.context.scene.cycles.samples = samples
    return cam


def render_look_preview(look, body, built, quick=None):
    """quick: (path, resolution) - a larger sheet of fewer poses for iterating (not the committed preview)."""
    vc.reset_scene()
    bpy.ops.import_scene.gltf(filepath=body.path)
    arm = next(o for o in bpy.data.objects if o.type == 'ARMATURE')
    for o in list(bpy.data.objects):
        if o.type == 'MESH' and o.parent is None and o.name.startswith("Icosphere"):
            bpy.data.objects.remove(o)
    sc = bpy.context.scene
    cam = setup_studio(quick[1] if quick else (200, 280))
    poses = [p for p in PREVIEW_POSES if p[0] in ("front", "side", "back", "run", "sit")] if quick else PREVIEW_POSES
    H = body.height
    sets = {}
    for line in LINES:
        for shape in SHAPES:
            objs = []
            tint = tuple(vc.srgb_to_linear(c / 255.0) for c in PREVIEW_TINT[(line, shape)])
            for slot in SLOTS:
                name = f"{line}_{shape}_{slot}"
                if name in built:
                    objs.append(piece_object(f"pv_{name}", built[name], body, arm, tint))
            if objs:
                sets[(line, shape)] = objs
    tiles = []
    for key, objs in sets.items():
        for other in sets.values():
            for o in other:
                o.hide_render = other is not objs
        for pname, clip, at in poses:
            act = bpy.data.actions.get(clip) if clip else None
            if act is None:
                arm.animation_data_create()
                arm.animation_data.action = None
                for pb in arm.pose.bones:
                    pb.rotation_mode = 'QUATERNION'
                    pb.location = (0, 0, 0)
                    pb.rotation_quaternion = (1, 0, 0, 0)
                    pb.scale = (1, 1, 1)
                sc.frame_set(0)
            else:
                import vo_ual
                vo_ual.use_action(arm, act)
                sc.frame_set(int(round(act.frame_range[1] * at)))
            d = 3.6
            loc = {"front": (0, -d, H * 0.6), "side": (d, 0, H * 0.6), "back": (0, d, H * 0.6)}.get(
                pname, (d * 0.7, -d * 0.7, H * 0.65))
            tgt = (0, 0, H * (0.5 if pname != "sit" else 0.3))
            if pname == "death":
                tgt = (0, 0.4, 0.3)
                loc = (d * 0.8, -d * 0.4, 1.6)
            vc.aim_camera(cam, loc, tgt, lens=50 if not quick else 62)
            tiles.append(vc.render_to(os.path.join(PREVIEW_DIR, f"_tmp_{look}_{len(tiles)}.png")))
    dst = quick[0] if quick else os.path.join(PREVIEW_DIR, f"{look}.png")
    out = vc.compose_sheet(tiles, dst, len(poses), stable=not quick)
    vc.remove_files(tiles)
    print(f"PREVIEW {out}")


ICON_LOOK = "aydor_0"
# Per slot: which side to keep (hands / feet: the left one, x > 0), display rotation (yaw about Z, then
# tilt about X), in degrees.
ICON_VIEW = {"head": (None, -30.0, 8.0), "chest": (None, -25.0, 6.0), "legs": (None, -20.0, 4.0),
             "hands": (1.0, -70.0, 10.0), "feet": (1.0, -55.0, 12.0)}


def render_icons(body, built):
    """armor_<line>_<slot>.png, 128x128, from the shape-a pieces fitted to ICON_LOOK (the inventory icon
    style of gen_weapons.render_icon: Cycles 512^2 box-filtered 4x on premultiplied colour, soft halo)."""
    import gen_weapons as gw
    for line in LINES:
        for slot in SLOTS:
            name = f"{line}_a_{slot}"
            if name not in built:
                continue
            vc.reset_scene()
            obj = piece_object(f"icon_{name}", built[name], body)
            keep, yaw, tilt = ICON_VIEW[slot]
            if keep is not None:
                bm = bmesh.new()
                bm.from_mesh(obj.data)
                drop = [v for v in bm.verts if v.co.x * keep < 0.0]
                bmesh.ops.delete(bm, geom=drop, context='VERTS')
                bm.to_mesh(obj.data)
                bm.free()
            pts = np.array([v.co for v in obj.data.vertices])
            c = 0.5 * (pts.min(0) + pts.max(0))
            obj.data.transform(Matrix.Translation(Vector(-c)))
            obj.matrix_world = Matrix.Rotation(math.radians(tilt), 4, 'X') @ Matrix.Rotation(math.radians(yaw), 4, 'Z')
            big = gw.ICON_SIZE * gw.ICON_SUPERSAMPLE
            cam = gw.setup_cycles((big, big), 48, True, world="#9a9ca0", world_strength=0.8)
            bpy.context.view_layer.update()
            gw.fit_camera(cam, [obj], 0.06)
            rgba = gw.render_rgba(os.path.join(ICON_DIR, f"_armor_{line}_{slot}.render.png"))
            out = gw.add_shadow(gw.box_down_premul(rgba, gw.ICON_SUPERSAMPLE))
            path = os.path.join(ICON_DIR, f"armor_{line}_{slot}.png")
            gw.write_png_stable_rgba(path, vc.to_u8(out))
            print(f"ICON armor_{line}_{slot}: coverage {float(out[..., 3].mean()):.2f}")


def main():
    args = vc.script_args()
    looks = LOOKS
    if "--only" in args:
        looks = tuple(l for l in LOOKS if l in set(args[args.index("--only") + 1].split(",")))
    only = None
    if "--pieces" in args:
        only = set(args[args.index("--pieces") + 1].split(","))
    sets = None
    if "--sets" in args:
        sets = set(args[args.index("--sets") + 1].split(","))
    atex.write_all(TEX_DIR, vc.write_png)
    meta_all = {}
    if os.path.exists(META_PATH):
        with open(META_PATH, "r", encoding="utf-8") as f:
            meta_all = json.load(f)
    for look in looks:
        body, meta, built = build_look(look, only, sets)
        meta_all.setdefault(look, {}).update(meta)
        if "--quick" in args:
            qdir = args[args.index("--quick") + 1]
            render_look_preview(look, body, built, (os.path.join(qdir, f"{look}.png"), (330, 460)))
        elif "--no-preview" not in args:
            render_look_preview(look, body, built)
        if look == ICON_LOOK and "--no-icons" not in args and only is None and sets is None:
            render_icons(body, built)
    with open(META_PATH, "w", encoding="utf-8", newline="\n") as f:
        json.dump({k: dict(sorted(v.items())) for k, v in sorted(meta_all.items())}, f, indent=1, sort_keys=True)
        f.write("\n")


if __name__ == "__main__":
    main()
