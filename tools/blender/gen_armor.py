"""Wearable armour (M17): skinned armour pieces fitted to every character look.

Usage (headless, after gen_character.py):
  blender --background --factory-startup --python tools/blender/gen_armor.py -- [--only aydor_0,...]
          [--pieces heavy_a_chest,...] [--no-preview] [--no-icons]

Output, per look L (aydor_0..2, kargan_0..2), line (heavy, leather, cloth), shape (a = tiers 1-3, b = tiers
4-5 and the unique pieces) and slot (head, chest, legs, hands, feet):
  assets_src/armor/<L>/<line>_<shape>_<slot>.glb      skinned to the look's own skeleton (the character
                                                      glb's joint nodes and inverse bind matrices copied
                                                      verbatim, so the body's bone palette drives it)
  assets_src/ui/items/armor_<line>_<slot>.png          15 inventory icons (shape a, rendered on aydor_0)
  assets_src/previews/armor/<L>.png                    every line/shape on the look, front / back / side and
                                                      in run / attack / sit / death poses
  assets_src/armor/armor_meta.json                     triangles per piece

How a piece is made (all in Blender axes, the look's bind pose; the character faces -Y):
  * SHELLS (chest, legs, hands, feet): the body triangles whose bone weights fall in the slot's region
    (helper bones such as cloaks, mantles, braids and tabard flaps excluded, so they stay on top), welded,
    cut with planes through joints (clean straight edges), pushed out along the normals by the line's
    offset, Taubin-smoothed (the Tripo body's folds and buckles melt into a plate / leather / cloth
    surface) and pushed out again wherever smoothing brought it closer than the offset to the body
    region it came from. Each shell vertex then takes the bone weights of the nearest point of that
    region (barycentric), so the shell deforms exactly like the skin under it: no poke-through in any
    clip by construction, and nothing of the body has to be hidden. Heavy plates get overlapping lames
    (a saw-tooth displacement along the limb), cloth gets soft folds.
  * PARTS added to a shell (pauldrons, knee cops, toe caps, cuffs, collars, skirts, sleeves) are
    star-shaped hulls / lofts fitted to the body vertices around a joint, weighted like the nearest skin
    of a given bone region (skirts: blended pelvis -> thigh by side and height).
  * HEAD pieces are rigid to the Head bone: a star-shaped hull of the head (hair, hood) above the brow
    plus a margin, opened at the face, shaped per line (helm with nasal / conical helm with cheek guards
    and aventail; leather cap with ear flaps / hood; turban / tall turban with a jewel and plume).
  * A rim (solidify, rim only) gives every open edge its thickness; colours are vertex colours (trim
    bands along the edges, lame grooves, seams), materials are flat PBR factors (metal / leather / cloth),
    so assetc bakes them into the vertex colours. The item `tint` multiplies everything per tier.

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
import vo_common as vc  # noqa: E402

CHAR_DIR = os.path.join(vc.SRC, "characters")
OUT_DIR = os.path.join(vc.SRC, "armor")
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
    if acc.get("normalized") and comp == "u1":
        a = a.astype(np.float32) / 255.0
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
        # Kargan bodies carry deep relief (scale skirts, fur, straps): shells sit further out.
        self.bulk = 1.3 if look.startswith("kargan") else 1.0
        self._bvh = {}

    def j(self, name):
        return np.array(self.joint[name], float)

    def rw(self, names):
        """Per-vertex summed weight of the given bones."""
        ids = [self.jindex[n] for n in names if n in self.jindex]
        m = np.isin(self.J, ids)
        return (self.W * m).sum(1)

    def faces_of(self, names, thr=0.5, exclude=HELPERS, exclude_thr=0.3):
        w = self.rw(names)
        ok = w >= thr
        if exclude:
            ok &= self.rw(exclude) < exclude_thr
        return ok[self.T].sum(1) >= 2

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

    def nearest(self, tree_tris, p):
        tree, _ = tree_tris
        loc, n, _fi, d = tree.find_nearest(Vector(p))
        return np.array(loc), np.array(n), d


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


# Materials: (name, roughness, metallic). Colours are per vertex.
MATERIALS = {
    "metal": (0.36, 1.0),
    "dark_metal": (0.5, 1.0),
    "leather": (0.66, 0.0),
    "cloth": (0.88, 0.0),
    "gem": (0.18, 0.0),
}


def write_piece_glb(path, body, name, prims):
    """prims: list of (material name, positions (n,3) Blender, normals (n,3), colours (n,4) linear,
    joints (n,4) skin joint indices, weights (n,4), indices (m,)). The skeleton (joint nodes, skin, inverse
    bind matrices) is the character glb's, copied verbatim."""
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

    materials, primitives = [], []
    for mname, P, N, C, Jt, Wt, I in prims:
        if mname not in [m["name"] for m in materials]:
            r, m = MATERIALS[mname]
            materials.append(dict(name=mname, pbrMetallicRoughness=dict(baseColorFactor=[1.0, 1.0, 1.0, 1.0],
                                                                        metallicFactor=m, roughnessFactor=r)))
        mi = [m["name"] for m in materials].index(mname)
        attrs = dict(POSITION=add_acc(np.ascontiguousarray(bl_to_gl(P)).astype("<f4"), 5126, 3, 34962, True),
                     NORMAL=add_acc(np.ascontiguousarray(bl_to_gl(N)).astype("<f4"), 5126, 3, 34962),
                     COLOR_0=add_acc(np.ascontiguousarray(C).astype("<f4"), 5126, 4, 34962),
                     JOINTS_0=add_acc(np.ascontiguousarray(Jt).astype("u1"), 5121, 4, 34962),
                     WEIGHTS_0=add_acc(np.ascontiguousarray(Wt).astype("<f4"), 5126, 4, 34962))
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
# Geometry: a Part is vertices + faces + per-vertex weights + per-vertex colour + per-face material

class Part:
    def __init__(self, name, material):
        self.name = name
        self.material = material
        self.V = np.zeros((0, 3))
        self.F = []          # polygons (index tuples), CCW seen from outside
        self.Wt = []         # per vertex: list of (joint index, weight)
        self.C = None        # per vertex colour (linear rgb)
        self.fmat = None     # per face material name (None = self.material)
        self.rim = 0.0       # rim thickness (inwards); 0 = none
        self.smooth = True
        self.sharp = 50.0


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


def taubin(V, nb, boundary, iters, lam=0.5, mu=-0.53, pin=None):
    """Taubin smoothing (no shrinkage); boundary vertices move gently, `pin` not at all."""
    V = V.copy()
    rows = np.concatenate([np.full(len(n), i) for i, n in enumerate(nb)]) if len(nb) else np.zeros(0, np.int64)
    cols = np.concatenate(nb) if len(nb) else np.zeros(0, np.int64)
    deg = np.maximum(np.array([len(n) for n in nb], float), 1.0)[:, None]
    scale = np.where(boundary, 0.35, 1.0)[:, None]
    if pin is not None:
        scale[pin] = 0.0
    for _ in range(iters):
        for f in (lam, mu):
            acc = np.zeros_like(V)
            np.add.at(acc, rows, V[cols])
            V += (acc / deg - V) * f * scale
    return V


def boundary_distance(V, F, nb, boundary):
    """Approximate geodesic distance of each vertex to the open boundary (Dijkstra over edges)."""
    import heapq
    dist = np.full(len(V), np.inf)
    heap = []
    for i in np.nonzero(boundary)[0]:
        dist[i] = 0.0
        heap.append((0.0, int(i)))
    heapq.heapify(heap)
    while heap:
        d, i = heapq.heappop(heap)
        if d > dist[i]:
            continue
        for j in nb[i]:
            nd = d + float(np.linalg.norm(V[j] - V[i]))
            if nd < dist[j]:
                dist[j] = nd
                heapq.heappush(heap, (nd, int(j)))
    return dist


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


# ---------------------------------------------------------------------------------------------
# Shells

def shell(body, name, material, bones, cuts, offset, smooth=6, thr=0.5, exclude=HELPERS, push_bones=None,
          min_island=0.06, extra_mask=None, keep=None):
    """A surface layer over the body region `bones`. cuts: list of (point, normal): geometry on the side
    the normal points to is removed. Returns a Part with transferred weights."""
    if keep is None:
        keep = 0.4 / body.bulk ** 2 if material == "metal" else 0.55
    offset *= body.bulk
    fmask = body.faces_of(bones, thr, exclude)
    if extra_mask is not None:
        fmask &= extra_mask(body)
    tris = body.T[fmask]
    used, inv = np.unique(tris, return_inverse=True)
    bm = bm_from(body.P[used], inv.reshape(-1, 3))
    for cut in cuts:
        p, n = cut[0], cut[1]
        side = cut[2] if len(cut) > 2 else 0.0  # +1 / -1: only the geometry on that side of x = 0
        if side:
            vs = [v for v in bm.verts if v.co.x * side > 0.0]
            if len(cut) > 3:  # (origin, direction, radius): only geometry near that ray (the limb, not the torso)
                o, d, rad = np.asarray(cut[3][0], float), np.asarray(cut[3][1], float), cut[3][2]
                d = d / np.linalg.norm(d)

                def near(co, o=o, d=d, rad=rad):
                    r = np.asarray(co) - o
                    t = float(r @ d)
                    return t > -0.02 and float(np.linalg.norm(r - t * d)) < rad

                vs = [v for v in vs if near(v.co)]
            vset = set(vs)
            geom = vs + [e for e in bm.edges if all(v in vset for v in e.verts)] + \
                [f for f in bm.faces if all(v in vset for v in f.verts)]
        else:
            geom = list(bm.verts) + list(bm.edges) + list(bm.faces)
        bmesh.ops.bisect_plane(bm, geom=geom, dist=1e-6, plane_co=tuple(p), plane_no=tuple(n), clear_outer=True)
    keep_islands(bm, min_island)
    # Merge the slivers the cuts leave next to existing vertices.
    bmesh.ops.remove_doubles(bm, verts=bm.verts, dist=0.002)
    bmesh.ops.dissolve_degenerate(bm, dist=1e-5, edges=bm.edges)
    V, F = bm_arrays(bm)
    bm.free()
    nb, boundary = adjacency(V, F)
    src = body.bvh(("shell",) + tuple(sorted(push_bones or bones)), body.faces_of(push_bones or bones, 0.35, exclude))
    n0 = vertex_normals(V, F)
    V0 = V.copy()
    V = V + n0 * offset
    for _ in range(4):
        V = taubin(V, nb, boundary, int(smooth * body.bulk ** 3))
        # Never below the skin point a vertex came from (thin layers such as tabards, belts and pouches
        # would otherwise be swallowed: their nearest surface is their own back), nor closer than that
        # to the region's surface.
        s = np.einsum("ij,ij->i", V - V0, n0)
        V = V + n0 * np.maximum(offset * keep - s, 0.0)[:, None]
        V = push_out(body, src, V, offset * keep)
    V = taubin(V, nb, boundary, max(2, smooth // 3))  # a last light pass: no creases from the constraints
    part = Part(name, material)
    part.V, part.F = V, F
    part.src = src
    return part


def push_out(body, src, V, minimum):
    out = V.copy()
    for i, p in enumerate(V):
        loc, n, _d = body.nearest(src, p)
        s = float(np.dot(p - loc, n))
        if s < minimum:
            out[i] = p + n * (minimum - s)
    return out


def finish_shell(body, part, tris, transfer=None):
    """Decimates, assigns transferred weights (from the shell's own source region)."""
    part.V, part.F = decimate(part.V, part.F, tris)
    part.Wt = [top4(w) for w in body.weights_at(transfer or part.src, part.V)]
    return part


# ---------------------------------------------------------------------------------------------
# Hulls: star-shaped surfaces around a centre, fitted to the body vertices

def frame_from(axis, front=(0.0, -1.0, 0.0)):
    """(side, front, axis) orthonormal frame; axis is the hull's 'up'."""
    a = np.asarray(axis, float)
    a /= np.linalg.norm(a)
    f = np.asarray(front, float) - a * np.dot(front, a)
    if np.linalg.norm(f) < 1e-6:
        f = np.array([1.0, 0.0, 0.0]) - a * a[0]
    f /= np.linalg.norm(f)
    s = np.cross(f, a)
    return s, f, a


def hull_radius(pts, c, dirs, cone_deg=14.0, margin=0.01, floor=0.0):
    """Per direction (unit vectors) the largest distance of `pts` from c inside a cone, + margin."""
    d = pts - c
    r = np.linalg.norm(d, axis=1)
    u = d / np.maximum(r[:, None], 1e-9)
    cosc = math.cos(math.radians(cone_deg))
    out = np.full(len(dirs), floor)
    for i, dv in enumerate(dirs):
        m = (u @ dv) > cosc
        if m.any():
            out[i] = max(floor, float(r[m].max()))
    return out + margin


def smooth_grid(R, rows, cols, iters=2, wrap=True):
    G = R.reshape(rows, cols)
    for _ in range(iters):
        P = np.pad(G, ((1, 1), (0, 0)), mode='edge')
        if wrap:
            left, right = np.roll(G, 1, 1), np.roll(G, -1, 1)
        else:
            Q = np.pad(G, ((0, 0), (1, 1)), mode='edge')
            left, right = Q[:, :-2], Q[:, 2:]
        G = 0.4 * G + 0.15 * (P[:-2] + P[2:] + left + right)
    return G.reshape(-1)


def grid_faces(rows, cols, wrap=True, base=0):
    F = []
    for r in range(rows - 1):
        for c in range(cols if wrap else cols - 1):
            c2 = (c + 1) % cols
            a, b = base + r * cols + c, base + r * cols + c2
            d, e = base + (r + 1) * cols + c, base + (r + 1) * cols + c2
            F.append((a, d, e, b))
    return F


def dome(c, frame, elev_rows, cols, lower_edge, pts, margin, shape=None, cone=16.0, floor=0.0, cap=True):
    """Star-shaped dome around c: rows from the top (elevation ~ +90) down to lower_edge(azimuth) degrees;
    azimuth 0 = frame front. shape(az, el, r) -> r (style). Returns V, F (outward winding)."""
    s, f, a = frame
    V = []
    dirs = []
    for r in range(elev_rows):
        t = (r + 1) / elev_rows
        for k in range(cols):
            az = 2 * math.pi * k / cols
            low = lower_edge(az)
            el = math.radians(90.0 - t * (90.0 - low))
            d = math.cos(el) * (math.cos(az) * f + math.sin(az) * s) + math.sin(el) * a
            dirs.append(d / np.linalg.norm(d))
    dirs = np.array(dirs)
    R = hull_radius(pts, c, dirs, cone, margin, floor)
    R = smooth_grid(R, elev_rows, cols, 2)
    for i, d in enumerate(dirs):
        r_, k = divmod(i, cols)
        az = 2 * math.pi * k / cols
        el = math.degrees(math.asin(max(-1.0, min(1.0, float(np.dot(d, a))))))
        rr = shape(az, el, R[i]) if shape else R[i]
        V.append(c + d * rr)
    V = np.array(V)
    F = grid_faces(elev_rows, cols)
    # The rows run from the top downwards; the outward winding of (a, d, e, b) is reversed here.
    F = [tuple(reversed(q)) for q in F]
    if cap:
        top = c + a * (shape(0.0, 90.0, float(hull_radius(pts, c, a[None], cone, margin, floor)[0]))
                       if shape else float(hull_radius(pts, c, a[None], cone, margin, floor)[0]))
        ti = len(V)
        V = np.vstack([V, top[None]])
        for k in range(cols):
            k2 = (k + 1) % cols
            F.append((ti, k2, k))
    return V, F


def loft(rings, close=True):
    """rings: list of (n,3) arrays (same n) from one end to the other; quads between them."""
    n = len(rings[0])
    V = np.vstack(rings)
    F = grid_faces(len(rings), n, wrap=close)
    return V, F


def tube_rings(c0, c1, front, ts, cols, pts, margin, radius_fn=None, cone_len=0.04, floor=0.0):
    """Rings around the segment c0->c1 at fractions ts; radius per (ring, angle) = largest distance of
    pts (within cone_len along the axis) in that angular sector + margin, smoothed."""
    axis = c1 - c0
    L = np.linalg.norm(axis)
    s, f, a = frame_from(axis / L, front)
    rel = pts - c0
    t_p = rel @ a
    rad = rel - np.outer(t_p, a)
    ang = np.arctan2(rad @ s, rad @ f)
    rr = np.linalg.norm(rad, axis=1)
    R = np.zeros((len(ts), cols))
    for i, t in enumerate(ts):
        m = np.abs(t_p - t * L) < cone_len
        for k in range(cols):
            az = 2 * math.pi * k / cols
            dang = np.abs((ang - az + math.pi) % (2 * math.pi) - math.pi)
            mm = m & (dang < math.radians(24))
            R[i, k] = max(floor, float(rr[mm].max()) if mm.any() else floor)
    R = smooth_grid(R.reshape(-1) + margin, len(ts), cols, 2).reshape(len(ts), cols)
    rings = []
    for i, t in enumerate(ts):
        ring = []
        for k in range(cols):
            az = 2 * math.pi * k / cols
            r = R[i, k] if radius_fn is None else radius_fn(t, az, R[i, k])
            ring.append(c0 + a * (t * L) + r * (math.cos(az) * f + math.sin(az) * s))
        rings.append(np.array(ring))
    return rings, (s, f, a)


def make_part(name, material, V, F, weights, colour=None):
    p = Part(name, material)
    p.V = np.asarray(V, float)
    p.F = [tuple(int(i) for i in f) for f in F]
    p.Wt = weights
    return p


def rigid(body, bone, n):
    return [[(body.jindex[bone], 1.0)]] * n


def transfer(body, bones, V, thr=0.35, exclude=HELPERS):
    src = body.bvh(("tr",) + tuple(sorted(bones)), body.faces_of(bones, thr, exclude))
    return [top4(w) for w in body.weights_at(src, V)]


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
    "heavy": dict(base="#dfe3e8", groove="#7b828c", edge="#9aa0a8", trim_b="#e9c77e", strap="#6e5a46"),
    "leather": dict(base="#e8d9c2", groove="#9a8063", edge="#8a6f52", trim_b="#d9c08a", strap="#6f5944",
                    metal="#d4d6d8"),
    "cloth": dict(base="#f3efe6", groove="#cfc6b6", edge="#b8a684", trim_b="#ecc76c", strap="#a8463c",
                  metal="#e2c27a"),
}


def colour_part(part, line, shape, base=None, trim=None, trim_w=0.022):
    """Base colour with a trim band along the open edges (geodesic distance)."""
    pal = PALETTE[line]
    nb, boundary = adjacency(part.V, part.F)
    dist = boundary_distance(part.V, part.F, nb, boundary)
    c = np.tile(lin(base or pal["base"]), (len(part.V), 1))
    t = lin(trim or (pal["trim_b"] if shape == "b" else pal["edge"]))
    band = np.clip(1.0 - (dist - trim_w) / 0.006, 0.0, 1.0) if trim_w > 0 else np.zeros(len(dist))
    c = c * (1 - band[:, None]) + t * band[:, None]
    part.C = c
    return part


def lames(part, origin, axis, period, amp, u_from=None, u_to=None, line="heavy", groove=None):
    """Overlapping plates along `axis` (each plate's lower edge over the next): saw-tooth displacement
    along the vertex normals plus a dark groove where a plate ends."""
    axis = np.asarray(axis, float) / np.linalg.norm(axis)
    u = (part.V - origin) @ axis
    m = np.ones(len(u), bool)
    if u_from is not None:
        m &= u >= u_from
    if u_to is not None:
        m &= u <= u_to
    fr = (u / period) % 1.0
    n = vertex_normals(part.V, part.F)
    d = amp * (1.0 - fr) * m
    part.V = part.V + n * d[:, None]
    if part.C is not None:
        g = lin(groove or PALETTE[line]["groove"])
        k = np.clip((0.14 - fr) / 0.07, 0.0, 1.0) * m
        part.C = part.C * (1 - k[:, None]) + g * k[:, None]


def stripes(part, fn, col, width):
    """Colour band where fn(V) is within `width` of an integer."""
    v = fn(part.V)
    k = np.clip(1.0 - np.abs(v - np.round(v)) / width, 0.0, 1.0)
    k = np.clip(k * 2.0, 0.0, 1.0)
    part.C = part.C * (1 - k[:, None]) + lin(col) * k[:, None]


# ---------------------------------------------------------------------------------------------
# Line parameters

OFFSET = {"heavy": 0.020, "leather": 0.012, "cloth": 0.011}
SMOOTH = {"heavy": 24, "leather": 16, "cloth": 14}
RIM = {"heavy": 0.006, "leather": 0.004, "cloth": 0.003}
MAT = {"heavy": "metal", "leather": "leather", "cloth": "cloth"}
TRIS = {"head": 1400, "chest": 3200, "legs": 2600, "hands": 1300, "feet": 1300}


def sides():
    return (("l", 1.0), ("r", -1.0))


def head_cloud(body):
    m = (body.rw(("Head",)) >= 0.5) & (body.rw(HELPERS) < 0.3)
    return body.P[m]


# ---------------------------------------------------------------------------------------------
# Slots

def build_chest(body, line, shape):
    s = body.scale
    pel, neck = body.j("pelvis"), body.j("neck_01")
    waist = pel[2] + (0.06 if line != "leather" else 0.02) * s
    frac = {("heavy", "a"): 0.5, ("heavy", "b"): 0.62, ("leather", "a"): 0.16, ("leather", "b"): 0.32,
            ("cloth", "a"): 0.92, ("cloth", "b"): 1.0}[(line, shape)]
    cuts = [((0, 0, waist), (0, 0, -1)), ((0, 0, neck[2] + 0.025 * s), (0, 0, 1))]
    for sd, _sg in sides():
        sh, el = body.j(f"upperarm_{sd}"), body.j(f"lowerarm_{sd}")
        cuts.append((sh + (el - sh) * frac, (el - sh) / np.linalg.norm(el - sh), _sg, (sh, el - sh, 0.10 * s * body.bulk)))
    bones = TORSO + side_bones("upperarm") + ("neck_01",)
    p = shell(body, "chest", MAT[line], bones + ("cloth_front", "cloth_back"), cuts,
              OFFSET[line] * (1.15 if shape == "b" else 1.0), SMOOTH[line], exclude=("mantle", "beard", "braid_l", "braid_r"))
    p = finish_shell(body, p, TRIS["chest"])
    colour_part(p, line, shape)
    parts = [p]
    if line == "heavy":
        lames(p, np.array([0, 0, waist]), (0, 0, 1), 0.052 * s, 0.007, 0.0, (body.j("spine_02")[2] - waist) + 0.01)
        for sd, _sg in sides():
            sh, el = body.j(f"upperarm_{sd}"), body.j(f"lowerarm_{sd}")
            lames(p, el, sh - el, 0.045 * s, 0.006, 0.0, np.linalg.norm(el - sh) * 0.75)
        parts += pauldrons(body, line, shape)
        if shape == "b":
            parts.append(gorget(body, line, shape))
    elif line == "leather":
        parts.append(belt(body, line, shape, pel[2] + 0.035 * s))
        stripes(p, lambda V: V[:, 0] / 0.09, PALETTE[line]["groove"], 0.05)
        if shape == "b":
            # A bandolier from the right shoulder to the left hip, one shoulder pad, a high collar.
            stripes(p, lambda V: ((V[:, 2] - waist) - (V[:, 0] + 0.2) * 0.9) / 0.5, PALETTE[line]["strap"], 0.07)
            parts += pauldrons(body, line, shape, only="l")
            parts.append(gorget(body, line, shape))
    else:
        stripes(p, lambda V: np.arctan2(V[:, 0], -V[:, 1] + 0.001) / 0.35, PALETTE[line]["groove"], 0.25)
        parts.append(skirt(body, line, shape, waist))
        parts.append(belt(body, line, shape, waist + 0.01 * s, sash=True))
        if shape == "b":
            parts += sleeves(body, line, shape)
            pass
    return parts


def pauldrons(body, line, shape, only=None):
    s = body.scale
    out = []
    for sd, sg in sides():
        if only and sd != only:
            continue
        sh = body.j(f"upperarm_{sd}")
        bones = (f"clavicle_{sd}", f"upperarm_{sd}")
        m = (body.rw(bones) >= 0.3) & (body.rw(HELPERS) < 0.3) & (np.linalg.norm(body.P - sh, axis=1) < 0.16 * s)
        pts = body.P[m]
        c = sh + np.array([-sg * 0.035 * s, 0.0, -0.035 * s])
        big = shape == "b"
        axis = np.array([sg * (0.75 if big else 0.6), 0.0, 1.0])
        frame = frame_from(axis)

        def low(az, big=big):
            return -18.0 if big else -8.0

        def shp(az, el, r, big=big):
            k = 1.0 + (0.10 if big else 0.04) * max(0.0, math.sin(math.radians(el))) ** 2
            if big:  # a raised ridge from front to back over the shoulder
                k += 0.07 * math.exp(-(math.sin(az) ** 2) / 0.05) * max(0.0, math.sin(math.radians(el)))
            return r * k

        margin = OFFSET[line] + (0.022 if big else 0.012) * s
        V, F = dome(c, frame, 7, 20, low, pts, margin, shp, cone=18.0)
        part = make_part(f"pauldron_{sd}", MAT[line], V, F, [])
        part.V, part.F = decimate(part.V, part.F, 500)
        part.Wt = transfer(body, bones + ("spine_03",), part.V)
        colour_part(part, line, shape, trim_w=0.016)
        if line == "heavy":
            lames(part, c, -axis, 0.04 * s, 0.006, -0.2, 0.2)
        part.rim = RIM[line]
        out.append(part)
    return out


def gorget(body, line, shape):
    s = body.scale
    neck = body.j("neck_01")
    m = (body.rw(("neck_01", "spine_03", "Head")) >= 0.3) & (body.rw(HELPERS) < 0.3)
    pts = body.P[m]
    c0 = neck + np.array([0, 0, -0.035 * s])
    c1 = neck + np.array([0, 0, 0.06 * s])
    flare = 0.035 if line == "heavy" else 0.02

    def rad(t, az, r):
        return r + flare * (1.0 - t) ** 2

    rings, _ = tube_rings(c0, c1, (0, -1, 0), np.linspace(0, 1, 5), 20, pts, OFFSET[line] + 0.006, rad, 0.03)
    V, F = loft(rings)
    part = make_part("gorget", MAT[line], V, F, [])
    part.Wt = chain(body, part.V, [(c1[2], "neck_01"), (c0[2], "spine_03")])
    colour_part(part, line, shape, trim_w=0.012)
    part.rim = RIM[line]
    return part


def belt(body, line, shape, z, sash=False):
    s = body.scale
    m = (np.abs(body.P[:, 2] - z) < 0.06 * s) & (body.rw(TORSO + side_bones("thigh")) >= 0.5) & (body.rw(HELPERS) < 0.3)
    m &= np.abs(body.P[:, 0]) < 0.3 * s
    pts = body.P[m]
    c0 = np.array([0.0, float(np.mean(pts[:, 1])), z - 0.03 * s])
    c1 = c0 + np.array([0, 0, 0.06 * s if not sash else 0.09 * s])
    rings, _ = tube_rings(c0, c1, (0, -1, 0), np.linspace(0, 1, 3), 28, pts, OFFSET[line] * 1.6 + 0.006, None, 0.06)
    V, F = loft(rings)
    part = make_part("belt", "leather" if not sash else "cloth", V, F, [])
    part.Wt = transfer(body, ("pelvis", "spine_01"), part.V)
    part.C = np.tile(lin(PALETTE[line]["strap"]), (len(V), 1))
    part.rim = 0.004
    return part


def skirt(body, line, shape, waist):
    """Robe skirt from the waist down to mid-thigh (a) or below the knee (b): rings fitted around both
    legs, flaring; weights blend pelvis -> thighs by side and height (cloth that swings with the legs)."""
    s = body.scale
    hip = body.j("thigh_l")
    hem = hip[2] - (0.28 if shape == "a" else 0.55) * s
    m = (body.rw(("pelvis", "cloth_front", "cloth_back") + side_bones("thigh", "calf")) >= 0.3) & (body.P[:, 2] < waist + 0.04) & (body.P[:, 2] > hem - 0.05)
    m &= np.abs(body.P[:, 0]) < 0.32 * s
    pts = body.P[m]
    zs = np.linspace(waist + 0.03 * s, hem, 9 if shape == "a" else 13)
    cols = 32
    centre = np.array([0.0, float(np.mean(pts[:, 1])), 0.0])
    rings = []
    for i, z in enumerate(zs):
        t = i / (len(zs) - 1)
        band = pts[np.abs(pts[:, 2] - z) < 0.05 * s]
        if len(band) < 10:
            band = pts[np.abs(pts[:, 2] - z) < 0.12 * s]
        rel = band[:, :2] - centre[:2]
        ring = []
        for k in range(cols):
            az = 2 * math.pi * k / cols
            d = np.array([math.sin(az), -math.cos(az)])
            proj = rel @ d
            perp = np.abs(rel @ np.array([-d[1], d[0]]))
            sel = perp < 0.06 * s
            r = float(proj[sel].max()) if sel.any() else 0.12 * s
            r = max(r, 0.08 * s) + OFFSET[line] + 0.01 + (0.03 + 0.10 * t * t) * s * (1.0 if shape == "b" else 0.8)
            r += 0.008 * t * math.sin(az * 8)  # soft vertical folds
            ring.append(np.array([centre[0] + d[0] * r, centre[1] + d[1] * r, z]))
        rings.append(np.array(ring))
    V = np.vstack(rings)
    F = [tuple(reversed(f)) for f in grid_faces(len(rings), cols)]
    part = make_part("skirt", "cloth", V, F, [])
    wts = []
    for p in part.V:
        t = float(np.clip((waist - p[2]) / max(waist - hem, 1e-3), 0.0, 1.0))
        side = float(np.clip(0.5 + p[0] / (0.16 * s), 0.0, 1.0))  # +X is the character's left
        lw = min(0.9, 0.15 + 1.1 * t) if t > 0.02 else 0.0
        w = {body.jindex["pelvis"]: 1.0 - lw, body.jindex["thigh_l"]: lw * side, body.jindex["thigh_r"]: lw * (1 - side)}
        wts.append(top4(w))
    part.Wt = wts
    colour_part(part, line, shape, trim_w=0.03)
    stripes(part, lambda V: np.arctan2(V[:, 0], -V[:, 1]) / (2 * math.pi / 8), PALETTE[line]["groove"], 0.12)
    part.rim = RIM[line]
    return part


def sleeves(body, line, shape):
    s = body.scale
    out = []
    for sd, _sg in sides():
        el, wr = body.j(f"lowerarm_{sd}"), body.j(f"hand_{sd}")
        bones = (f"lowerarm_{sd}", f"upperarm_{sd}")
        m = (body.rw(bones) >= 0.3) & (np.linalg.norm(body.P - (el + wr) / 2, axis=1) < 0.25 * s)
        pts = body.P[m]
        c0 = el + (el - wr) * 0.12
        c1 = el + (wr - el) * 0.62

        def rad(t, az, r):
            return r + 0.012 + 0.05 * t * t * s

        rings, _ = tube_rings(c0, c1, (0, -1, 0), np.linspace(0, 1, 6), 16, pts, OFFSET[line], rad, 0.04)
        V, F = loft(rings)
        part = make_part(f"sleeve_{sd}", "cloth", V, F, [])
        part.Wt = transfer(body, bones, part.V)
        colour_part(part, line, shape, trim_w=0.025)
        part.rim = RIM[line]
        out.append(part)
    return out


def capelet(body, line, shape):
    s = body.scale
    neck, sp3 = body.j("neck_01"), body.j("spine_03")
    bones = ("spine_03", "clavicle_l", "clavicle_r", "neck_01", "upperarm_l", "upperarm_r", "spine_02")
    m = (body.rw(bones) >= 0.3) & (body.rw(HELPERS) < 0.6) & (body.P[:, 2] > sp3[2] - 0.2 * s)
    m &= (body.rw(("Head",)) < 0.3) & (np.abs(body.P[:, 0]) < abs(body.j("upperarm_l")[0]) + 0.05 * s)
    pts = body.P[m]
    c0 = neck + np.array([0, 0, 0.03 * s])
    c1 = neck - np.array([0, 0, 0.15 * s])

    def rad(t, az, r):
        return r + 0.01 + 0.035 * t

    rings, _ = tube_rings(c0, c1, (0, -1, 0), np.linspace(0, 1, 6), 28, pts, OFFSET[line] + 0.02, rad, 0.035)
    V, F = loft(rings)
    part = make_part("capelet", "cloth", V, F, [])
    part.Wt = transfer(body, bones, part.V)
    colour_part(part, line, shape, trim_w=0.03)
    part.rim = RIM[line]
    return part


def build_legs(body, line, shape):
    s = body.scale
    pel = body.j("pelvis")
    knee = body.j("calf_l")
    ankle = body.j("foot_l")
    top = pel[2] + 0.08 * s
    bottom = {"heavy": knee[2] - 0.22 * s, "leather": knee[2] - 0.26 * s, "cloth": ankle[2] + 0.07 * s}[line]
    off = OFFSET[line] * body.bulk * (1.0 if line != "cloth" else 1.8)
    parts = []
    # Legs are hull tubes (thigh, shin) fitted around each leg's own skin, not shells: the bodies'
    # trousers carry deep folds, wraps and fur that a surface layer would copy as lumps.
    for sd, _sg in sides():
        hp, kn, an = body.j(f"thigh_{sd}"), body.j(f"calf_{sd}"), body.j(f"foot_{sd}")
        bones = (f"thigh_{sd}", f"calf_{sd}")
        m = (body.rw(bones) >= 0.4) & (body.rw(HELPERS) < 0.3)
        pts = body.P[m]
        up = (hp - kn) / np.linalg.norm(hp - kn)
        baggy = line == "cloth"

        def thigh_r(t, az, r, baggy=baggy):
            return r + (0.025 * math.sin(math.pi * t) if baggy else 0.0)

        rings, _ = tube_rings(hp + up * 0.07 * s, kn - up * 0.04 * s, (0, -1, 0), np.linspace(0, 1, 9), 22, pts,
                              off, thigh_r, 0.035)
        V, F = loft(rings)
        F = [tuple(reversed(f)) for f in F]
        t1 = make_part(f"thigh_{sd}", MAT[line], V, F, transfer(body, bones + ("pelvis",), V))
        colour_part(t1, line, shape, trim_w=0.018)
        down = (an - kn) / np.linalg.norm(an - kn)
        L2 = (kn[2] - bottom) / max(-down[2], 0.3)

        def shin_r(t, az, r, baggy=baggy):
            return r + (0.03 * math.sin(math.pi * min(t * 1.2, 1.0)) if baggy else 0.004 * (1 - t))

        rings, _ = tube_rings(kn - down * 0.05 * s, kn + down * L2, (0, -1, 0), np.linspace(0, 1, 8), 22, pts,
                              off * (0.9 if line != "cloth" else 1.0), shin_r, 0.035)
        V, F = loft(rings)
        F = [tuple(reversed(f)) for f in F]
        t2 = make_part(f"shin_{sd}", MAT[line], V, F, transfer(body, bones, V))
        colour_part(t2, line, shape, trim_w=0.018)
        for p in (t1, t2):
            p.rim = RIM[line]
            if line == "heavy":
                lames(p, kn, up, 0.05 * s, 0.005, -0.5, 0.5)
            elif line == "leather":
                stripes(p, lambda V, kn=kn: (V[:, 2] - kn[2]) / (0.16 * s), PALETTE[line]["strap"], 0.07)
            else:
                stripes(p, lambda V, kn=kn: np.arctan2(V[:, 0] - kn[0], -V[:, 1] + kn[1]) / 0.6,
                        PALETTE[line]["groove"], 0.2)
        parts += [t1, t2]
    # The seat and hips: a short fauld (heavy), leather flaps, or a wrapped cloth band.
    parts.append(fauld(body, line, shape, top))
    if line == "heavy":
        parts += knee_cops(body, line, shape)
    elif line == "leather":
        if shape == "b":
            parts += knee_cops(body, line, shape)
    else:
        parts += ankle_cuffs(body, line, shape)
        if shape == "b":
            parts.append(belt(body, line, shape, top - 0.03 * s, sash=True))
    return parts


def fauld(body, line, shape, top):
    """Hip cover from the waist to just below the hip joints, around both legs (skirt weights)."""
    s = body.scale
    hip = body.j("thigh_l")
    hem = hip[2] - {"heavy": 0.13, "leather": 0.16, "cloth": 0.08}[line] * s
    m = (body.rw(("pelvis", "cloth_front", "cloth_back") + side_bones("thigh")) >= 0.3) & (body.P[:, 2] < top + 0.03) & (body.P[:, 2] > hem - 0.05)
    m &= (np.abs(body.P[:, 0]) < 0.3 * s) & (body.rw(HELPERS) < 0.3)
    pts = body.P[m]
    zs = np.linspace(top, hem, 6)
    cols = 32
    centre = np.array([0.0, float(np.mean(pts[:, 1])), 0.0])
    off = OFFSET[line] * body.bulk * (1.0 if line != "cloth" else 1.8)
    rings = []
    for i, z in enumerate(zs):
        t = i / (len(zs) - 1)
        band = pts[np.abs(pts[:, 2] - z) < 0.05 * s]
        if len(band) < 10:
            band = pts[np.abs(pts[:, 2] - z) < 0.12 * s]
        rel = band[:, :2] - centre[:2]
        ring = []
        for k in range(cols):
            az = 2 * math.pi * k / cols
            d = np.array([math.sin(az), -math.cos(az)])
            sel = np.abs(rel @ np.array([-d[1], d[0]])) < 0.06 * s
            r = float((rel @ d)[sel].max()) if sel.any() else 0.12 * s
            r = max(r, 0.08 * s) + off + 0.006 + 0.025 * t * s
            ring.append(np.array([centre[0] + d[0] * r, centre[1] + d[1] * r, z]))
        rings.append(np.array(ring))
    V = np.vstack(rings)
    F = [tuple(reversed(f)) for f in grid_faces(len(rings), cols)]
    wts = []
    for p in V:
        t = float(np.clip((top - p[2]) / max(top - hem, 1e-3), 0.0, 1.0))
        side = float(np.clip(0.5 + p[0] / (0.16 * s), 0.0, 1.0))
        lw = 0.55 * t * t
        wts.append(top4({body.jindex["pelvis"]: 1.0 - lw, body.jindex["thigh_l"]: lw * side,
                         body.jindex["thigh_r"]: lw * (1 - side)}))
    part = make_part("fauld", MAT[line], V, F, wts)
    colour_part(part, line, shape, trim_w=0.02)
    if line == "heavy":
        lames(part, np.array([0, 0, hem]), (0, 0, 1), 0.045 * s, 0.006, 0.0, top - hem)
    elif line == "leather":
        stripes(part, lambda V: np.arctan2(V[:, 0], -V[:, 1]) / (2 * math.pi / 10), PALETTE[line]["strap"], 0.1)
    part.rim = RIM[line]
    return part


def knee_cops(body, line, shape):
    s = body.scale
    out = []
    for sd, _sg in sides():
        kn = body.j(f"calf_{sd}")
        bones = (f"thigh_{sd}", f"calf_{sd}")
        m = (body.rw(bones) >= 0.3) & (np.linalg.norm(body.P - kn, axis=1) < 0.13 * s) & (body.rw(HELPERS) < 0.3)
        pts = body.P[m]
        c = kn + np.array([0, 0.02 * s, 0.0])
        frame = frame_from((0, -1, 0.15), (0, 0, 1))
        big = shape == "b"

        def shp(az, el, r, big=big):
            return r * (1.0 + (0.18 if big else 0.08) * max(0.0, math.sin(math.radians(el))) ** 3)

        V, F = dome(c, frame, 5, 16, lambda az, big=big: 15.0 if not big else 0.0, pts,
                    OFFSET[line] + 0.012, shp, cone=20.0)
        part = make_part(f"knee_{sd}", MAT[line], V, F, [])
        part.Wt = transfer(body, bones, part.V)
        colour_part(part, line, shape, trim_w=0.012)
        part.rim = RIM[line]
        out.append(part)
    return out


def ankle_cuffs(body, line, shape):
    s = body.scale
    out = []
    for sd, _sg in sides():
        kn, an = body.j(f"calf_{sd}"), body.j(f"foot_{sd}")
        m = (body.rw((f"calf_{sd}",)) >= 0.4) & (np.linalg.norm(body.P - an, axis=1) < 0.25 * s)
        pts = body.P[m]
        c0 = an + (kn - an) * 0.12
        c1 = an + (kn - an) * 0.3
        rings, _ = tube_rings(c0, c1, (0, -1, 0), np.linspace(0, 1, 3), 16, pts, OFFSET[line] * 2.0 + 0.004, None, 0.04)
        V, F = loft(rings)
        part = make_part(f"cuff_{sd}", "cloth", V, F, [])
        part.Wt = transfer(body, (f"calf_{sd}",), part.V)
        part.C = np.tile(lin(PALETTE[line]["edge"] if shape == "a" else PALETTE[line]["trim_b"]), (len(V), 1))
        part.rim = RIM[line]
        out.append(part)
    return out


def build_hands(body, line, shape):
    s = body.scale
    frac = {("heavy", "a"): 0.35, ("heavy", "b"): 0.12, ("leather", "a"): 0.62, ("leather", "b"): 0.2,
            ("cloth", "a"): 0.7, ("cloth", "b"): 0.3}[(line, shape)]
    cuts = []
    for sd, _sg in sides():
        el, wr = body.j(f"lowerarm_{sd}"), body.j(f"hand_{sd}")
        d = (wr - el) / np.linalg.norm(wr - el)
        cuts.append((el + (wr - el) * frac, -d, _sg))
    bones = side_bones("hand", "lowerarm") + FINGERS
    p = shell(body, "hands", MAT[line], bones, cuts, OFFSET[line] * 0.55, SMOOTH[line] - 2, min_island=0.2)
    p = finish_shell(body, p, TRIS["hands"])
    colour_part(p, line, shape, trim_w=0.015)
    parts = [p]
    for sd, _sg in sides():
        el, wr = body.j(f"lowerarm_{sd}"), body.j(f"hand_{sd}")
        d = (wr - el) / np.linalg.norm(wr - el)
        if line == "heavy":
            lames(p, wr, el - wr, 0.04 * s, 0.004, 0.0, np.linalg.norm(wr - el) * (1 - frac) - 0.02)
        elif line == "leather":
            stripes(p, lambda V, wr=wr, d=d: ((V - wr) @ d) / 0.05, PALETTE[line]["strap"], 0.1)
        else:
            stripes(p, lambda V, wr=wr, d=d: ((V - wr) @ d + 0.4 * V[:, 1]) / 0.035, PALETTE[line]["groove"], 0.15)
    if shape == "b":
        parts += gauntlet_cuffs(body, line, shape, frac)
    return parts


def gauntlet_cuffs(body, line, shape, frac):
    out = []
    for sd, _sg in sides():
        el, wr = body.j(f"lowerarm_{sd}"), body.j(f"hand_{sd}")
        bones = (f"lowerarm_{sd}",)
        m = (body.rw(bones) >= 0.4) & (body.rw(HELPERS) < 0.3)
        pts = body.P[m]
        c0 = el + (wr - el) * (frac + 0.03)
        c1 = el + (wr - el) * max(frac - 0.08, 0.0)
        flare = 0.03 if line == "heavy" else 0.02

        def rad(t, az, r, flare=flare):
            return r + flare * t

        rings, _ = tube_rings(c0, c1, (0, -1, 0), np.linspace(0, 1, 3), 16, pts, OFFSET[line] * 0.55 + 0.006, rad, 0.035)
        V, F = loft(rings)
        part = make_part(f"cuff_{sd}", MAT[line], V, F, [])
        part.Wt = transfer(body, bones, part.V)
        part.C = np.tile(lin(PALETTE[line]["trim_b"]), (len(V), 1))
        part.rim = RIM[line]
        out.append(part)
    return out


def build_feet(body, line, shape):
    s = body.scale
    knee, ankle = body.j("calf_l"), body.j("foot_l")
    top = {("heavy", "a"): knee[2] - 0.16 * s, ("heavy", "b"): knee[2] - 0.06 * s,
           ("leather", "a"): ankle[2] + 0.12 * s, ("leather", "b"): knee[2] - 0.1 * s,
           ("cloth", "a"): ankle[2] + 0.03 * s, ("cloth", "b"): ankle[2] + 0.10 * s}[(line, shape)]
    cuts = [((0, 0, top), (0, 0, 1))]
    bones = side_bones("foot", "ball", "calf")
    p = shell(body, "feet", MAT[line], bones, cuts, OFFSET[line] * 0.8, SMOOTH[line] - 2, min_island=0.2)
    floor = float(body.P[body.rw(side_bones("foot", "ball")) > 0.5][:, 2].min()) + 0.002
    p.V[:, 2] = np.maximum(p.V[:, 2], floor)
    p = finish_shell(body, p, TRIS["feet"])
    p.V[:, 2] = np.maximum(p.V[:, 2], floor)
    colour_part(p, line, shape, trim_w=0.02)
    k = np.clip(1.0 - (p.V[:, 2] - floor) / 0.02, 0.0, 1.0)  # dark sole
    p.C = p.C * (1 - k[:, None]) + lin(PALETTE[line]["strap"]) * k[:, None]
    parts = [p]
    if line == "heavy":
        for sd, _sg in sides():
            an, kn = body.j(f"foot_{sd}"), body.j(f"calf_{sd}")
            lames(p, an, kn - an, 0.05 * s, 0.005, 0.03, (top - an[2]))
        parts += toe_caps(body, line, shape, floor)
    elif line == "leather":
        stripes(p, lambda V: (V[:, 2] - ankle[2]) / (0.07 * s), PALETTE[line]["strap"], 0.08)
        if shape == "b":
            parts += boot_cuffs(body, line, shape, top)
    elif shape == "b":
        parts += toe_caps(body, line, shape, floor)
    return parts


def toe_caps(body, line, shape, floor):
    out = []
    for sd, _sg in sides():
        ball = body.j(f"ball_{sd}")
        bones = (f"ball_{sd}", f"foot_{sd}")
        m = (body.rw(bones) >= 0.5) & (body.P[:, 1] < ball[1] + 0.03)
        pts = body.P[m]
        c = ball + np.array([0, 0.01, 0.0])
        c[2] = floor + 0.01
        frame = frame_from((0, -1, 0.5), (0, 0, 1))
        pointed = shape == "b"

        def shp(az, el, r, pointed=pointed):
            return r * (1.0 + (0.35 if pointed else 0.05) * max(0.0, math.sin(math.radians(el))) ** 4)

        V, F = dome(c, frame, 5, 16, lambda az: -5.0, pts, OFFSET[line] * 0.8 + 0.008, shp, cone=22.0)
        V[:, 2] = np.maximum(V[:, 2], floor)
        if line == "cloth":  # the curled slipper toe
            tip = V[:, 1] < c[1] - 0.04
            V[tip, 2] += (c[1] - 0.04 - V[tip, 1]) * 1.2
        part = make_part(f"toe_{sd}", MAT[line], V, F, [])
        part.Wt = transfer(body, bones, part.V)
        colour_part(part, line, shape, trim_w=0.01)
        part.rim = RIM[line]
        out.append(part)
    return out


def boot_cuffs(body, line, shape, top):
    s = body.scale
    out = []
    for sd, _sg in sides():
        kn = body.j(f"calf_{sd}")
        m = (body.rw((f"calf_{sd}",)) >= 0.4) & (np.abs(body.P[:, 2] - top) < 0.1)
        pts = body.P[m]
        c0 = np.array([kn[0], kn[1], top - 0.07 * s])
        c1 = np.array([kn[0], kn[1], top + 0.015 * s])

        def rad(t, az, r):
            return r + 0.012 * t

        rings, _ = tube_rings(c0, c1, (0, -1, 0), np.linspace(0, 1, 3), 16, pts, OFFSET[line] + 0.008, rad, 0.05)
        V, F = loft(rings)
        part = make_part(f"bootcuff_{sd}", "leather", V, F, [])
        part.Wt = transfer(body, (f"calf_{sd}",), part.V)
        part.C = np.tile(lin(PALETTE[line]["edge"]), (len(V), 1))
        part.rim = RIM[line]
        out.append(part)
    return out


def head_shape(line, shape):
    """(lower edge elevation per azimuth, radius style, margin) of the head piece."""
    def fold(az):
        return abs((az + math.pi) % (2 * math.pi) - math.pi)  # 0 = front, pi = back

    if line == "heavy" and shape == "a":
        return (lambda az: -8.0 - 45.0 * (1 - math.cos(az)) / 2,
                lambda az, el, r: r * (1.0 + 0.10 * max(0.0, math.sin(math.radians(el))) ** 3), 0.016)
    if line == "heavy":
        return (lambda az: -6.0 if fold(az) < math.radians(30) else -62.0 + 8.0 * math.cos(fold(az)),
                lambda az, el, r: r * (1.0 + 0.42 * max(0.0, math.sin(math.radians(el))) ** 5), 0.02)
    if line == "leather" and shape == "a":
        return (lambda az: 4.0 if fold(az) < math.radians(35) else (-48.0 if fold(az) < math.radians(120) else -32.0),
                lambda az, el, r: r, 0.012)
    if line == "leather":
        return (lambda az: 0.0 if fold(az) < math.radians(32) else -72.0 - 10.0 * (fold(az) / math.pi),
                lambda az, el, r: r * (1.0 + 0.10 * max(0.0, math.sin(math.radians(el))) ** 2), 0.03)
    tall = 0.18 if shape == "a" else 0.55
    power = 2 if shape == "a" else 3

    def turban(az, el, r):
        e = math.radians(el)
        return r * (1.0 + tall * max(0.0, math.sin(e)) ** power + 0.05 * abs(math.sin(5.0 * e + 0.8 * az)))

    return (lambda az: 6.0 - 26.0 * (fold(az) / math.pi), turban, 0.03 if shape == "a" else 0.032)


def build_head(body, line, shape):
    s = body.scale
    pts = head_cloud(body)
    hj = body.j("Head")
    top = float(pts[:, 2].max())
    brow = hj[2] + 0.52 * (top - hj[2])
    c = np.array([hj[0], float(np.mean(pts[pts[:, 2] > brow][:, 1])), brow])
    frame = frame_from((0, 0, 1), (0, -1, 0))
    low, shp, margin = head_shape(line, shape)
    V, F = dome(c, frame, 10, 28, low, pts, margin, shp, cone=18.0)
    part = make_part("head", MAT[line], V, F, [])
    if line == "leather" and shape == "b":
        part.Wt = chain(body, part.V, [(hj[2] - 0.02, "Head"), (hj[2] - 0.12 * s, "neck_01"),
                                       (body.j("neck_01")[2] - 0.05 * s, "spine_03")])
    else:
        part.Wt = rigid(body, "Head", len(V))
    colour_part(part, line, shape, trim_w=0.02 if line != "cloth" else 0.0)
    if line == "cloth":
        stripes(part, lambda V: ((V[:, 2] - brow) + 0.015 * np.arctan2(V[:, 0], -V[:, 1] + 1e-6)) / 0.032,
                PALETTE[line]["groove"], 0.18)
    part.rim = RIM[line] + 0.002
    parts = [part]
    d_front = np.array([[0.0, -1.0, 0.0]])
    front = c + d_front[0] * float(hull_radius(pts, c, d_front, 18.0, margin)[0])
    if line == "heavy":
        nb = MeshBox()
        nb.box(front + np.array([0, -0.004, -0.035 * s]), (0.018 * s, 0.012, 0.085 * s))  # nasal guard
        if shape == "b":  # crest along the top, front to back
            for k in range(7):
                t = k / 6
                ang = math.radians(-60 + 120 * t)
                d = np.array([0.0, -math.sin(ang), math.cos(ang)])
                r = shp(0.0, math.degrees(math.asin(d[2])), float(hull_radius(pts, c, d[None], 18.0, margin)[0]))
                nb.box(c + d * (r + 0.012), (0.01, 0.03, 0.03 * (1.4 - abs(t - 0.5))))
        parts.append(nb.part(body, "nasal", MAT[line], "Head", PALETTE[line]["trim_b"] if shape == "b" else PALETTE[line]["edge"]))
    elif line == "cloth" and shape == "b":
        nb = MeshBox()
        jewel = front + np.array([0, -0.006, 0.05 * s])
        nb.box(jewel, (0.028, 0.016, 0.034), rot=45.0)
        for k in range(6):  # a plume rising from the jewel, bending back
            t = k / 5
            nb.box(jewel + np.array([0, 0.03 * t * t, 0.03 + 0.12 * t]) * s, (0.012, 0.01, 0.04))
        parts.append(nb.part(body, "jewel", "gem", "Head", "#d23a3a"))
    return parts


class MeshBox:
    """A few boxes (rigid parts)."""

    def __init__(self):
        self.V, self.F = [], []

    def box(self, c, size, rot=0.0):
        sx, sy, sz = (x * 0.5 for x in size)
        cr, sr = math.cos(math.radians(rot)), math.sin(math.radians(rot))
        b = len(self.V)
        for z in (-sz, sz):
            for (x, y) in ((-sx, -sy), (sx, -sy), (sx, sy), (-sx, sy)):
                xr, zr = x * cr - z * sr, x * sr + z * cr
                self.V.append(np.asarray(c, float) + np.array([xr, y, zr]))
        for f in ((0, 3, 2, 1), (4, 5, 6, 7), (0, 1, 5, 4), (1, 2, 6, 5), (2, 3, 7, 6), (3, 0, 4, 7)):
            self.F.append(tuple(b + i for i in f))

    def part(self, body, name, material, bone, colour):
        p = make_part(name, material, np.array(self.V), self.F, rigid(body, bone, len(self.V)))
        p.C = np.tile(lin(colour), (len(self.V), 1))
        p.smooth = False
        return p


BUILDERS = {"head": build_head, "chest": build_chest, "legs": build_legs, "hands": build_hands, "feet": build_feet}


# ---------------------------------------------------------------------------------------------
# Assembly and export

def add_rim(part):
    """Gives the open edges a thickness: each boundary edge gets a quad going inwards by part.rim."""
    if part.rim <= 0.0:
        return part
    n = vertex_normals(part.V, part.F)
    edges = {}
    for f in part.F:
        for k in range(len(f)):
            a, b = f[k], f[(k + 1) % len(f)]
            edges.setdefault((min(a, b), max(a, b)), []).append((a, b))
    V, C, W = list(part.V), list(part.C), list(part.Wt)
    inner = {}

    def inner_of(i):
        if i not in inner:
            inner[i] = len(V)
            V.append(part.V[i] - n[i] * part.rim)
            C.append(part.C[i] * 0.7)
            W.append(part.Wt[i])
        return inner[i]

    F = list(part.F)
    for lst in edges.values():
        if len(lst) == 1:
            a, b = lst[0]
            F.append((b, a, inner_of(a), inner_of(b)))
    part.V, part.C, part.Wt, part.F = np.array(V), np.array(C), W, F
    return part


def triangulate(F):
    T = []
    for f in F:
        for k in range(1, len(f) - 1):
            T.append((f[0], f[k], f[k + 1]))
    return np.array(T, np.int64).reshape(-1, 3)


def corner_normals(V, T, sharp_deg, smooth=True):
    fn = np.cross(V[T[:, 1]] - V[T[:, 0]], V[T[:, 2]] - V[T[:, 0]])
    area = np.linalg.norm(fn, axis=1)
    fu = fn / np.maximum(area[:, None], 1e-15)
    if not smooth:
        return np.repeat(fu[:, None, :], 3, 1)
    inc = [[] for _ in range(len(V))]
    for t, tri in enumerate(T):
        for v in tri:
            inc[v].append(t)
    inc = [np.array(x, np.int64) for x in inc]
    cosl = math.cos(math.radians(sharp_deg))
    out = np.zeros((len(T), 3, 3))
    for t, tri in enumerate(T):
        for k, v in enumerate(tri):
            ids = inc[v]
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


def export_piece(body, line, shape, slot, parts, path):
    """Merges the parts per material into primitives with split corners and writes the glb."""
    groups = {}
    tris_total = 0
    for p in parts:
        if p.C is None:
            p.C = np.tile(lin(PALETTE[line]["base"]), (len(p.V), 1))
        frac = orient_outward(body, p) if p.smooth else 1.0  # (boxes are closed and wound outwards)
        if frac < 0.8:
            print(f"WARNING {body.look} {line}_{shape}_{slot}/{p.name}: only {frac:.0%} of the faces face outwards")
        add_rim(p)
        T = triangulate(p.F)
        if len(T) == 0:
            continue
        a = np.linalg.norm(np.cross(p.V[T[:, 1]] - p.V[T[:, 0]], p.V[T[:, 2]] - p.V[T[:, 0]]), axis=1) * 0.5
        T = T[a > 2e-7]
        cn = corner_normals(p.V, T, p.sharp, p.smooth)
        g = groups.setdefault(p.material, dict(P=[], N=[], C=[], J=[], W=[], I=[], n=0))
        corners = {}
        P, N, C, J, W, I = [], [], [], [], [], []
        for t, tri in enumerate(T):
            for k, v in enumerate(tri):
                key = (int(v), tuple(np.round(cn[t, k] * 1000).astype(int)))
                idx = corners.get(key)
                if idx is None:
                    idx = corners[key] = len(P)
                    P.append(p.V[v])
                    N.append(cn[t, k])
                    C.append(np.append(np.clip(p.C[v], 0.0, 1.0), 1.0))
                    w = list(p.Wt[v])[:4]
                    J.append([j for j, _ in w] + [0] * (4 - len(w)))
                    W.append([x for _, x in w] + [0.0] * (4 - len(w)))
                I.append(g["n"] + idx)
        g["n"] += len(P)
        for key, arr in (("P", P), ("N", N), ("C", C), ("J", J), ("W", W)):
            g[key].append(np.array(arr))
        g["I"].append(np.array(I))
        tris_total += len(T)
    prims = []
    for mname in sorted(groups):
        g = groups[mname]
        prims.append((mname, np.vstack(g["P"]), np.vstack(g["N"]), np.vstack(g["C"]), np.vstack(g["J"]),
                      np.vstack(g["W"]), np.concatenate(g["I"])))
    write_piece_glb(path, body, f"{line}_{shape}_{slot}", prims)
    return tris_total, prims


def piece_names():
    return [f"{l}_{sh}_{sl}" for l in LINES for sh in SHAPES for sl in SLOTS]


def build_look(look, only=None):
    body = Body(look)
    meta = {}
    built = {}
    for line in LINES:
        for shape in SHAPES:
            for slot in SLOTS:
                name = f"{line}_{shape}_{slot}"
                if only and name not in only:
                    continue
                vc.reset_scene()
                parts = BUILDERS[slot](body, line, shape)
                tris, prims = export_piece(body, line, shape, slot, parts, os.path.join(OUT_DIR, look, f"{name}.glb"))
                meta[name] = dict(tris=int(tris), parts=len(parts))
                built[name] = prims
                print(f"ARMOR {look} {name}: {tris} tris, {len(parts)} part(s)")
    return body, meta, built


# ---------------------------------------------------------------------------------------------
# Previews and icons

def preview_material(mname):
    mat = bpy.data.materials.get(f"pv_{mname}")
    if mat is not None:
        return mat
    mat = bpy.data.materials.new(f"pv_{mname}")
    mat.use_nodes = True
    nt = mat.node_tree
    bsdf = nt.nodes.get("Principled BSDF")
    attr = nt.nodes.new("ShaderNodeAttribute")
    attr.attribute_name = "Col"
    nt.links.new(attr.outputs["Color"], bsdf.inputs["Base Color"])
    r, m = MATERIALS[mname]
    bsdf.inputs["Roughness"].default_value = r
    bsdf.inputs["Metallic"].default_value = m
    mat.use_backface_culling = True
    return mat


def piece_object(name, prims, body, arm=None, tint=(1.0, 1.0, 1.0)):
    """A Blender object of an exported piece (corners as exported), skinned to `arm` if given."""
    verts, faces, cols, mats, groups = [], [], [], [], []
    materials = []
    for mname, P, N, C, J, W, I in prims:
        base = len(verts)
        verts += [tuple(p) for p in P]
        cols += [tuple(np.append(c[:3] * np.array(tint), 1.0)) for c in C]
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


def setup_studio(resolution, samples=16):
    import gen_attachments as ga
    cam = ga.setup_studio(resolution)
    bpy.context.scene.cycles.samples = samples
    return cam


def render_look_preview(look, body, built):
    vc.reset_scene()
    bpy.ops.import_scene.gltf(filepath=body.path)
    arm = next(o for o in bpy.data.objects if o.type == 'ARMATURE')
    for o in list(bpy.data.objects):
        if o.type == 'MESH' and o.parent is None and o.name.startswith("Icosphere"):
            bpy.data.objects.remove(o)
    sc = bpy.context.scene
    cam = setup_studio((200, 280))
    H = body.height
    sets = {}
    for line in LINES:
        for shape in SHAPES:
            objs = []
            for slot in SLOTS:
                name = f"{line}_{shape}_{slot}"
                if name in built:
                    objs.append(piece_object(f"pv_{name}", built[name], body, arm))
            sets[(line, shape)] = objs
    tiles = []
    for key, objs in sets.items():
        for other in sets.values():
            for o in other:
                o.hide_render = other is not objs
        for pname, clip, at in PREVIEW_POSES:
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
            vc.aim_camera(cam, loc, tgt, lens=50)
            tiles.append(vc.render_to(os.path.join(PREVIEW_DIR, f"_tmp_{look}_{len(tiles)}.png")))
    out = vc.compose_sheet(tiles, os.path.join(PREVIEW_DIR, f"{look}.png"), len(PREVIEW_POSES), stable=True)
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
    meta_all = {}
    if os.path.exists(META_PATH):
        with open(META_PATH, "r", encoding="utf-8") as f:
            meta_all = json.load(f)
    for look in looks:
        body, meta, built = build_look(look, only)
        meta_all.setdefault(look, {}).update(meta)
        if "--no-preview" not in args:
            render_look_preview(look, body, built)
        if look == ICON_LOOK and "--no-icons" not in args and only is None:
            render_icons(body, built)
    with open(META_PATH, "w", encoding="utf-8", newline="\n") as f:
        json.dump({k: dict(sorted(v.items())) for k, v in sorted(meta_all.items())}, f, indent=1, sort_keys=True)
        f.write("\n")


if __name__ == "__main__":
    main()
