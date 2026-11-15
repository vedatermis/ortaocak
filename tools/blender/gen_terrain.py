"""Zone 1 terrain (512 x 512 m): heightmap, splat, walk mask, spawn points and prop placements.

Usage (headless; needs the prop .glb files from gen_props.py for the preview only):
  blender --background --factory-startup --python tools/blender/gen_terrain.py [-- --no-preview]

Outputs (assets_src/terrain/zone1/):
  zone1_height.png        513x513 16-bit grayscale; pixel (col i, row j) = sample at x = i m, z = j m
  zone1_splat.png         512x512 RGBA8 layer weights (grass, dirt road, rock, dry grass)
  zone1_walk.png          512x512 8-bit: 255 walkable, 0 blocked (1 m cells)
  zone1.terrain.toml      description for assetc (layers, spawns, objects)
Previews (assets_src/previews/terrain/): zone1_topdown.png, zone1_oblique.png, zone1_village.png,
zone1_walkmask.png.

All coordinates here are ENGINE coordinates (x right, z forward, y up). For the Blender preview an
engine point (x, y, z) is placed at Blender (-x, -z, y) and an engine yaw becomes Blender rot_z = -yaw.
"""

import math
import os
import sys

import numpy as np

sys.dont_write_bytecode = True  # keep tools/blender free of __pycache__
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import vo_common as vc  # noqa: E402

SIZE = 512.0
N = 513                 # height samples per side (1 m spacing)
M = 512                 # mask / splat cells per side
CENTER = np.array([256.0, 256.0])
SEED = 5150
MARGIN = 6.0            # outer blocked margin (m)
MAX_SLOPE = math.tan(math.radians(36.0))
CHAR_RADIUS = 0.35      # footprints are grown by this much in the walk mask

# Prop footprints in the prop's local engine frame: (half width x, half depth z), or radius.
# `blocks` False = decorative, walkable through (none at the moment).
FOOTPRINT = {
    "tree_oak": ("circle", 0.45), "tree_pine": ("circle", 0.35), "rock_a": ("circle", 1.0),
    "rock_b": ("circle", 1.35), "fence": ("rect", 1.0, 0.08), "well": ("circle", 1.1),
    "market_stall": ("rect", 1.4, 0.85), "lamp_post": ("circle", 0.2), "crate": ("rect", 0.42, 0.42),
    "barrel": ("circle", 0.45), "signpost": ("circle", 0.22), "house": ("rect", 3.3, 2.6),
    "watchtower": ("rect", 1.75, 1.75), "ant_tasi": ("rect", 1.8, 1.2),
}
# The Ant Taşı (docs/LORE.md) stands in the village plaza between the two spawns, off the road, its
# Aydor face (front, +Z) turned towards the Aydor spawn and its Kargan face towards the Kargan spawn.
ANT_TASI = (249.0, 263.5)
# Spawns: on the plateau, 40-60 m apart, each within 40 m of the stone and at least SPAWN_WALL_CLEAR
# from every house wall (the follow camera must not end up inside a house).
SPAWNS = [("spawn_aydor", 238.0, 271.0, math.radians(135.0)),
          ("spawn_kargan", 277.0, 247.0, math.atan2(ANT_TASI[0] - 277.0, ANT_TASI[1] - 247.0))]
SPAWN_WALL_CLEAR = 8.0
HOUSE_HALF = (3.15, 2.45)   # house walls incl. the stone plinth (gen_props.house), local x / z

ROADS = [
    [(256, 4), (250, 50), (263, 110), (253, 185), (256, 256), (262, 322), (249, 392), (259, 452), (256, 508)],
    [(256, 256), (312, 262), (372, 248), (432, 268), (508, 262)],
]
ROAD_HALF = 3.2

# ---------------------------------------------------------------------------------------------
# Noise

def value_noise(rng, cells, n=N):
    lat = rng.random((cells + 2, cells + 2))
    x = np.linspace(0, cells, n)
    i0 = np.floor(x).astype(int)
    t = x - i0
    t = t * t * (3 - 2 * t)
    a = lat[np.ix_(i0, i0)]
    b = lat[np.ix_(i0, i0 + 1)]
    c = lat[np.ix_(i0 + 1, i0)]
    d = lat[np.ix_(i0 + 1, i0 + 1)]
    ty, tx = t[:, None], t[None, :]
    return (a * (1 - tx) + b * tx) * (1 - ty) + (c * (1 - tx) + d * tx) * ty


def fbm(rng, cells, octaves, gain=0.5, n=N):
    tot, amp, norm = np.zeros((n, n)), 1.0, 0.0
    for _ in range(octaves):
        tot += amp * value_noise(rng, cells, n)
        norm += amp
        amp *= gain
        cells *= 2
    return tot / norm


def smoothstep(e0, e1, x):
    t = np.clip((x - e0) / (e1 - e0), 0.0, 1.0)
    return t * t * (3 - 2 * t)


def seg_distance(px, pz, a, b):
    ax, az = a
    bx, bz = b
    dx, dz = bx - ax, bz - az
    t = np.clip(((px - ax) * dx + (pz - az) * dz) / (dx * dx + dz * dz), 0.0, 1.0)
    return np.hypot(px - (ax + t * dx), pz - (az + t * dz))


def road_points(poly, step=1.0):
    """Densified, Catmull-Rom smoothed road centreline."""
    p = np.array(poly, float)
    pts = []
    for i in range(len(p) - 1):
        p0 = p[max(i - 1, 0)]
        p1, p2 = p[i], p[i + 1]
        p3 = p[min(i + 2, len(p) - 1)]
        n = max(2, int(np.linalg.norm(p2 - p1) / step))
        for k in range(n):
            t = k / n
            t2, t3 = t * t, t * t * t
            pts.append(0.5 * ((2 * p1) + (-p0 + p2) * t + (2 * p0 - 5 * p1 + 4 * p2 - p3) * t2 +
                              (-p0 + 3 * p1 - 3 * p2 + p3) * t3))
    pts.append(p[-1])
    return np.array(pts)


def distance_to_polyline(px, pz, pts):
    d = np.full(px.shape, 1e9)
    for i in range(len(pts) - 1):
        d = np.minimum(d, seg_distance(px, pz, pts[i], pts[i + 1]))
    return d


# ---------------------------------------------------------------------------------------------
# Terrain fields

def build_fields():
    rng = np.random.default_rng(SEED)
    jj, ii = np.mgrid[0:N, 0:N].astype(float)   # j = z index (rows), i = x index (cols)
    x, z = ii * (SIZE / (N - 1)), jj * (SIZE / (N - 1))
    roads = [road_points(r, 2.0) for r in ROADS]
    d_road = np.minimum.reduce([distance_to_polyline(x, z, r) for r in roads])

    hills = fbm(rng, 3, 5)
    detail = fbm(rng, 24, 3)
    rolling = fbm(rng, 9, 3)
    h = 16.0 + 26.0 * (hills - 0.5) + 9.0 * (rolling - 0.5) + 1.6 * (detail - 0.5)
    # Village plateau: flat-ish at 20 m within ~60 m of the centre.
    d_c = np.hypot(x - CENTER[0], z - CENTER[1])
    w_plateau = smoothstep(95.0, 58.0, d_c)
    h = h * (1 - w_plateau) + (20.0 + 0.35 * (detail - 0.5)) * w_plateau
    # Border ridges: steep, high, impassable; road valleys cut passes through them.
    e = np.minimum.reduce([x, z, SIZE - x, SIZE - z])
    ridge_noise = fbm(rng, 6, 4)
    ridged = 1.0 - np.abs(fbm(rng, 10, 4) * 2.0 - 1.0)   # sharp crests
    wobble = fbm(rng, 8, 3)
    e_mod = e + 34.0 * (wobble - 0.5)                    # wavy inner edge of the mountains
    pass_cut = smoothstep(30.0, 10.0, d_road)
    profile = smoothstep(78.0, 14.0, e_mod) ** 1.3
    ridge = (26.0 + 34.0 * ridge_noise + 22.0 * ridged ** 2) * profile * (1.0 - 0.92 * pass_cut)
    h = h + ridge
    # Roads: follow a smoothed version of the ground (gentle grades), blended over 3..10 m.
    from_road = h.copy()
    k = 15
    blur = from_road
    for _ in range(3):  # cheap box blur
        acc = np.zeros_like(blur)
        for dj in (-k, 0, k):
            for di in (-k, 0, k):
                acc += np.roll(np.roll(blur, dj, 0), di, 1)
        blur = acc / 9.0
    w_road = smoothstep(10.0, ROAD_HALF, d_road)
    h = h * (1 - w_road) + (blur - 0.15) * w_road
    # Plaza in the centre (slightly dished, dirt).
    plaza = smoothstep(19.0, 14.0, d_c)
    h = h * (1 - plaza) + 19.95 * plaza
    return dict(h=h, x=x, z=z, d_road=d_road, d_c=d_c, e=e, roads=roads, rng=rng, plaza=plaza)


def bilinear(h, x, z):
    fx, fz = np.clip(x, 0, SIZE) / (SIZE / (N - 1)), np.clip(z, 0, SIZE) / (SIZE / (N - 1))
    i = min(int(fx), N - 2)
    j = min(int(fz), N - 2)
    tx, tz = fx - i, fz - j
    a = h[j, i] * (1 - tx) + h[j, i + 1] * tx
    b = h[j + 1, i] * (1 - tx) + h[j + 1, i + 1] * tx
    return a * (1 - tz) + b * tz


def cell_fields(F):
    """Per 1 m cell (centre) values: slope, road distance, etc."""
    h = F["h"]
    gz, gx = np.gradient(h, SIZE / (N - 1))
    slope = np.hypot(gx, gz)
    # Cell value = max over its four corners (conservative).
    s = np.maximum.reduce([slope[:-1, :-1], slope[1:, :-1], slope[:-1, 1:], slope[1:, 1:]])
    c = lambda a: 0.25 * (a[:-1, :-1] + a[1:, :-1] + a[:-1, 1:] + a[1:, 1:])  # noqa: E731
    return dict(slope=s, d_road=c(F["d_road"]), d_c=c(F["d_c"]), e=c(F["e"]), h=c(h), plaza=c(F["plaza"]))


# ---------------------------------------------------------------------------------------------
# Placement

class Placer:
    def __init__(self, F, C):
        self.F, self.C = F, C
        self.objects = []  # dict(mesh, x, z, yaw, scale, y_offset, blocks)

    def radius(self, name, scale):
        fp = FOOTPRINT[name]
        return (fp[1] if fp[0] == "circle" else math.hypot(fp[1], fp[2])) * scale

    def free(self, name, x, z, scale, clearance=0.6, road_clear=1.0, spawn_clear=4.0):
        if not (MARGIN < x < SIZE - MARGIN and MARGIN < z < SIZE - MARGIN):
            return False
        r = self.radius(name, scale)
        if float(distance_to_polyline(np.array([x]), np.array([z]), np.vstack(self.F["roads"][0:1]))[0]) < r + ROAD_HALF + road_clear:
            return False
        if float(distance_to_polyline(np.array([x]), np.array([z]), self.F["roads"][1])[0]) < r + ROAD_HALF + road_clear:
            return False
        for (_, sx, sz, _) in SPAWNS:
            if math.hypot(x - sx, z - sz) < r + spawn_clear:
                return False
        for o in self.objects:
            if math.hypot(x - o["x"], z - o["z"]) < r + self.radius(o["mesh"], o["scale"]) + clearance:
                return False
        return True

    def add(self, name, x, z, yaw, scale=1.0, y_offset=0.0, blocks=True, **kw):
        if not self.free(name, x, z, scale, **kw):
            return False
        self.objects.append(dict(mesh=name, x=float(x), z=float(z), yaw=float(yaw), scale=float(scale),
                                 y_offset=float(y_offset), blocks=blocks))
        return True

    def slope_at(self, x, z):
        i, j = int(np.clip(x, 0, M - 1)), int(np.clip(z, 0, M - 1))
        return float(self.C["slope"][j, i])


def facing(x, z, tx, tz):
    """Yaw that turns the prop's front (+Z) towards (tx, tz)."""
    return math.atan2(tx - x, tz - z)


def place_all(F, C):
    P = Placer(F, C)
    rng = np.random.default_rng(SEED + 1)
    cx, cz = CENTER
    # The oath stone first: everything else keeps its clearance from it.
    sx, sz = SPAWNS[0][1], SPAWNS[0][2]
    if not P.add("ant_tasi", ANT_TASI[0], ANT_TASI[1], facing(ANT_TASI[0], ANT_TASI[1], sx, sz), road_clear=1.5,
                 spawn_clear=8.0, clearance=2.0):
        raise SystemExit("ant_tasi: placement is not free")
    # Well in the plaza, off the road.
    P.add("well", cx - 10.0, cz - 8.0, 0.4)
    # Market stalls around the plaza facing the centre.
    for ang in (205.0, 232.0, 318.0):
        a = math.radians(ang)
        x, z = cx + 21.0 * math.cos(a), cz + 21.0 * math.sin(a)
        if P.add("market_stall", x, z, facing(x, z, cx, cz)):
            for k in range(3):  # crates / barrels behind each stall
                b = a + rng.uniform(-0.25, 0.25)
                rr = 24.5 + rng.uniform(0, 1.5)
                P.add("crate" if k % 2 == 0 else "barrel", cx + rr * math.cos(b), cz + rr * math.sin(b),
                      rng.uniform(0, 6.28), clearance=0.3)
    # Houses on a ring, facing the plaza.
    for ang in range(0, 360, 20):
        a = math.radians(ang + rng.uniform(-5, 5))
        rr = rng.uniform(37.0, 46.0)
        x, z = cx + rr * math.cos(a), cz + rr * math.sin(a)
        yaw = facing(x, z, cx, cz) + rng.uniform(-0.15, 0.15)
        if P.add("house", x, z, yaw, road_clear=2.5, clearance=3.0):
            for k in range(2):
                b = a + rng.uniform(-0.12, 0.12)
                P.add("barrel" if k else "crate", cx + (rr - 4.5) * math.cos(b) + rng.uniform(-2, 2),
                      cz + (rr - 4.5) * math.sin(b) + rng.uniform(-2, 2), rng.uniform(0, 6.28), clearance=0.4)
    # Lamp posts along the roads near the village, alternating sides.
    for road in F["roads"]:
        acc, side = 0.0, 1
        for i in range(1, len(road)):
            p0, p1 = road[i - 1], road[i]
            seg = np.linalg.norm(p1 - p0)
            acc += seg
            if acc < 16.0:
                continue
            acc = 0.0
            if np.hypot(*(p1 - CENTER)) > 110:
                continue
            d = (p1 - p0) / seg
            nrm = np.array([d[1], -d[0]]) * side
            q = p1 + nrm * (ROAD_HALF + 1.6)
            if P.add("lamp_post", q[0], q[1], facing(q[0], q[1], p1[0], p1[1]), road_clear=-ROAD_HALF + 1.0,
                     clearance=0.5):
                side = -side
    # Signposts at the junction and near the passes.
    for (x, z, tx, tz) in ((cx + 6.5, cz - 6.0, cx, cz - 20), (256 + 9.0, 78.0, 256, 60), (252 - 9.0, 432.0, 252, 450),
                           (420.0, 258.0, 440, 268)):
        P.add("signpost", x, z, facing(x, z, tx, tz), road_clear=0.2, spawn_clear=3.0)
    # Watchtowers guarding the three passes.
    for (x, z) in ((268.0, 66.0), (240.0, 442.0), (446.0, 282.0)):
        P.add("watchtower", x, z, facing(x, z, cx, cz), road_clear=1.5)
    # Fenced paddock west of the village.
    fx0, fz0, fw, fd = 172.0, 296.0, 26.0, 18.0
    corners = [(fx0, fz0), (fx0 + fw, fz0), (fx0 + fw, fz0 + fd), (fx0, fz0 + fd), (fx0, fz0)]
    for k in range(4):
        (ax, az), (bx, bz) = corners[k], corners[k + 1]
        L = math.hypot(bx - ax, bz - az)
        n = int(round(L / 2.0))
        yaw = math.atan2(bx - ax, bz - az) - math.pi / 2  # fence runs along its local X
        for s in range(n):
            if k == 1 and s in (n // 2 - 1, n // 2):  # gate gap on the east side
                continue
            t = (s + 0.5) / n
            P.add("fence", ax + (bx - ax) * t, az + (bz - az) * t, yaw, clearance=-0.5)
    # Groves of trees away from the village and the roads.
    groves = 0
    tries = 0
    while groves < 26 and tries < 4000:
        tries += 1
        gx, gz = rng.uniform(40, SIZE - 40), rng.uniform(40, SIZE - 40)
        if math.hypot(gx - cx, gz - cz) < 80:
            continue
        i, j = int(gx), int(gz)
        if C["slope"][j, i] > 0.45 or C["d_road"][j, i] < 14:
            continue
        groves += 1
        pine_bias = (C["h"][j, i] - 14.0) / 25.0
        count = int(rng.integers(7, 18))
        for _ in range(count * 3):
            if count <= 0:
                break
            x, z = gx + rng.normal(0, 11), gz + rng.normal(0, 11)
            if not (MARGIN + 2 < x < SIZE - MARGIN - 2 and MARGIN + 2 < z < SIZE - MARGIN - 2):
                continue
            if P.slope_at(x, z) > 0.6:
                continue
            kind = "tree_pine" if rng.uniform() < 0.35 + pine_bias else "tree_oak"
            if P.add(kind, x, z, rng.uniform(0, 6.283), scale=rng.uniform(0.8, 1.25), y_offset=-0.15,
                     clearance=1.2, road_clear=3.0):
                count -= 1
    # Scattered single trees.
    for _ in range(140):
        x, z = rng.uniform(MARGIN + 4, SIZE - MARGIN - 4), rng.uniform(MARGIN + 4, SIZE - MARGIN - 4)
        if math.hypot(x - cx, z - cz) < 62 or P.slope_at(x, z) > 0.55:
            continue
        P.add("tree_oak" if rng.uniform() < 0.6 else "tree_pine", x, z, rng.uniform(0, 6.283),
              scale=rng.uniform(0.75, 1.15), y_offset=-0.15, clearance=1.5, road_clear=3.0)
    # Rocks on slopes (and a few in the meadows).
    placed = 0
    for _ in range(3000):
        if placed >= 110:
            break
        x, z = rng.uniform(MARGIN + 3, SIZE - MARGIN - 3), rng.uniform(MARGIN + 3, SIZE - MARGIN - 3)
        s = P.slope_at(x, z)
        if math.hypot(x - cx, z - cz) < 60:
            continue
        if s < 0.3 and rng.uniform() > 0.08:
            continue
        name = "rock_b" if rng.uniform() < 0.4 else "rock_a"
        if P.add(name, x, z, rng.uniform(0, 6.283), scale=rng.uniform(0.7, 1.5), y_offset=-0.35, clearance=0.8,
                 road_clear=2.0):
            placed += 1
    return P.objects


# ---------------------------------------------------------------------------------------------
# Maps

def footprint_cells(o):
    fp = FOOTPRINT[o["mesh"]]
    s = o["scale"]
    if fp[0] == "circle":
        hx = hz = fp[1] * s + CHAR_RADIUS
    else:
        hx, hz = fp[1] * s + CHAR_RADIUS, fp[2] * s + CHAR_RADIUS
    r = math.hypot(hx, hz) + 1
    cells = [(int(o["z"]), int(o["x"]))]  # the cell under the origin is always blocked
    c, sn = math.cos(o["yaw"]), math.sin(o["yaw"])
    for j in range(int(o["z"] - r), int(o["z"] + r) + 1):
        for i in range(int(o["x"] - r), int(o["x"] + r) + 1):
            if not (0 <= i < M and 0 <= j < M):
                continue
            dx, dz = i + 0.5 - o["x"], j + 0.5 - o["z"]
            # Into the prop's local frame (local +Z = forward(yaw) = (sin, cos), local +X = (cos, -sin)).
            lx = dx * c - dz * sn
            lz = dx * sn + dz * c
            if fp[0] == "circle":
                if math.hypot(lx, lz) <= hx:
                    cells.append((j, i))
            elif abs(lx) <= hx and abs(lz) <= hz:
                cells.append((j, i))
    return cells


def build_maps(F, C, objects):
    slope = C["slope"]
    walk = np.ones((M, M), bool)
    walk &= slope <= MAX_SLOPE
    walk &= C["e"] > MARGIN
    for o in objects:
        if o["blocks"]:
            for (j, i) in footprint_cells(o):
                walk[j, i] = False
    # Splat weights.
    rng = np.random.default_rng(SEED + 2)
    dry_noise = fbm(rng, 5, 4, n=M)
    road = smoothstep(ROAD_HALF + 1.2, ROAD_HALF - 0.8, C["d_road"])
    road = np.maximum(road, C["plaza"] * 0.95)
    rock = smoothstep(0.42, 0.75, slope)
    rock = np.maximum(rock, smoothstep(44.0, 58.0, C["h"]) * 0.8)
    dry = smoothstep(0.5, 0.68, dry_noise) * (1 - rock)
    dry = np.maximum(dry, smoothstep(30.0, 42.0, C["h"]) * 0.7 * (1 - rock))
    grass = np.clip(1.0 - np.maximum.reduce([road, rock, dry]), 0, 1)
    w = np.stack([grass * (1 - road), road, rock * (1 - road), dry * (1 - road)], axis=-1)
    w /= np.maximum(w.sum(axis=-1, keepdims=True), 1e-6)
    splat = np.round(w * 255).astype(np.int32)
    splat[..., 0] += 255 - splat.sum(axis=-1)  # exact 255 total
    splat = np.clip(splat, 0, 255).astype(np.uint8)
    return walk, splat


def write_outputs(F, objects, walk, splat):
    out = vc.ensure_dir(os.path.join(vc.SRC, "terrain", "zone1"))
    h = F["h"]
    hmin, hmax = math.floor(h.min()) - 1.0, math.ceil(h.max()) + 1.0
    h16 = np.round((h - hmin) / (hmax - hmin) * 65535.0).astype(np.uint16)
    vc.write_png(os.path.join(out, "zone1_height.png"), h16)
    vc.write_png(os.path.join(out, "zone1_splat.png"), splat)
    vc.write_png(os.path.join(out, "zone1_walk.png"), np.where(walk, 255, 0).astype(np.uint8))
    lines = [
        "# Zone 1 terrain description for assetc. Generated by tools/blender/gen_terrain.py; do not edit.",
        "# Coordinates are engine space (x right, z forward, metres); y comes from the heightmap.",
        "[terrain]",
        'heightmap = "zone1_height.png"',
        f"world_size = {SIZE:.1f}",
        f"height_min = {hmin:.1f}",
        f"height_max = {hmax:.1f}",
        'splat = "zone1_splat.png"',
        'walk_mask = "zone1_walk.png"',
        "",
    ]
    for tex, tile in (("grass", 6.0), ("dirt", 4.0), ("rock", 8.0), ("drygrass", 6.0)):
        lines += ["[[layer]]", f'texture = "textures/terrain/{tex}.png"', f"tile_size = {tile:.1f}", ""]
    for (name, x, z, yaw) in SPAWNS:
        lines += ["[[spawn]]", f'name = "{name}"', f"x = {x:.2f}", f"z = {z:.2f}", f"yaw = {yaw:.4f}", ""]
    for o in objects:
        lines += ["[[object]]", f'mesh = "props/{o["mesh"]}.vmesh"', f"x = {o['x']:.3f}", f"z = {o['z']:.3f}",
                  f"yaw = {o['yaw']:.4f}", f"scale = {o['scale']:.3f}", f"y_offset = {o['y_offset']:.3f}",
                  f"blocks = {'true' if o['blocks'] else 'false'}", ""]
    with open(os.path.join(out, "zone1.terrain.toml"), "w", newline="\n") as f:
        f.write("\n".join(lines))
    return hmin, hmax


# ---------------------------------------------------------------------------------------------
# Previews

def walkmask_preview(F, C, walk, objects):
    h = C["h"]
    shade = (h - h.min()) / (h.max() - h.min())
    gz, gx = np.gradient(h)
    light = np.clip(0.75 + 0.6 * (-gx * 0.6 - gz * 0.4), 0.3, 1.2)
    img = np.stack([shade * 0.5 + 0.35] * 3, axis=-1) * light[..., None]
    img[~walk] = img[~walk] * 0.45 + np.array([0.75, 0.12, 0.1]) * 0.55
    img = np.clip(img * 255, 0, 255).astype(np.uint8)
    for (name, x, z, _) in SPAWNS:
        col = (60, 140, 255) if "aydor" in name else (255, 200, 40)
        img[int(z) - 3:int(z) + 4, int(x) - 3:int(x) + 4] = col
    for o in objects:
        img[int(o["z"]), int(o["x"])] = (20, 20, 20)
    vc.write_png(os.path.join(vc.PREVIEWS, "terrain", "zone1_walkmask.png"), img)


def blender_preview(F, splat, objects):
    import bpy
    vc.reset_scene()
    # Albedo: layer textures tiled at their tile size, blended by the splat map (4 px per metre).
    R = 2048
    layers = []
    for tex, tile in (("grass", 6.0), ("dirt", 4.0), ("rock", 8.0), ("drygrass", 6.0)):
        img = vc.read_image(os.path.join(vc.SRC, "textures", "terrain", f"{tex}.png"))[..., :3]
        px = int(tile * R / SIZE)
        idx = (np.arange(px) * img.shape[0] / px).astype(int)
        small = img[np.ix_(idx, idx)]
        reps = R // px + 1
        layers.append(np.tile(small, (reps, reps, 1))[:R, :R])
    up = (np.arange(R) * M / R).astype(int)
    w = splat[np.ix_(up, up)].astype(np.float32) / 255.0
    albedo = sum(layers[k] * w[..., k:k + 1] for k in range(4))
    path = os.path.join(vc.PREVIEWS, "terrain", "_albedo_tmp.png")
    vc.write_png(path, vc.to_u8(albedo))
    # Terrain mesh at 2 m resolution. Engine (x, y, z) -> Blender (-x, -z, y).
    step = 2
    h = F["h"][::step, ::step]
    n = h.shape[0]
    verts, faces, uvs = [], [], []
    for j in range(n):
        for i in range(n):
            x, z = i * step, j * step
            verts.append((-x, -z, h[j, i]))
    for j in range(n - 1):
        for i in range(n - 1):
            a = j * n + i
            # Blender (x,y) = (-x_e, -z_e) keeps orientation (a 180-degree turn), so CCW stays CCW.
            faces.append((a, a + 1, a + n + 1, a + n))
    me = bpy.data.meshes.new("terrain")
    me.from_pydata(verts, [], faces)
    uv = me.uv_layers.new(name="UVMap")
    coords = np.zeros((len(me.loops), 2), np.float32)
    vidx = np.zeros(len(me.loops), np.int64)
    me.loops.foreach_get("vertex_index", vidx)
    vi, vj = vidx % n, vidx // n
    coords[:, 0] = vi / (n - 1)
    coords[:, 1] = 1.0 - vj / (n - 1)  # image row 0 (z = 0) at the top of the texture
    uv.data.foreach_set("uv", coords.ravel())
    me.shade_smooth()
    mat = bpy.data.materials.new("terrain")
    mat.use_nodes = True
    nt = mat.node_tree
    tex = nt.nodes.new("ShaderNodeTexImage")
    tex.image = bpy.data.images.load(path)
    bsdf = nt.nodes["Principled BSDF"]
    bsdf.inputs["Roughness"].default_value = 0.95
    nt.links.new(tex.outputs["Color"], bsdf.inputs["Base Color"])
    me.materials.append(mat)
    vc.link(bpy.data.objects.new("terrain", me))
    # Props: import each glb once and instance it.
    protos = {}
    for name in sorted({o["mesh"] for o in objects}):
        before = set(bpy.data.objects)
        bpy.ops.import_scene.gltf(filepath=os.path.join(vc.SRC, "props", f"{name}.glb"))
        new = [ob for ob in bpy.data.objects if ob not in before and ob.type == 'MESH']
        protos[name] = new[0]
        new[0].hide_render = True
        new[0].location = (0, 0, -1000)
    for o in objects:
        ob = protos[o["mesh"]].copy()
        vc.link(ob)
        ob.hide_render = False
        y = bilinear(F["h"], o["x"], o["z"]) + o["y_offset"]
        ob.location = (-o["x"], -o["z"], y)
        ob.rotation_mode = 'XYZ'
        ob.rotation_euler = (0, 0, -o["yaw"])
        ob.scale = (o["scale"],) * 3
    # Spawn markers (tall coloured pillars, preview only).
    for (name, x, z, _) in SPAWNS:
        bpy.ops.mesh.primitive_cylinder_add(radius=0.6, depth=5.0, location=(-x, -z, bilinear(F["h"], x, z) + 2.5))
        m = vc.material(f"marker_{name}", "#3c8cff" if "aydor" in name else "#ffc828")
        bpy.context.active_object.data.materials.append(m)
    cam = vc.setup_preview((1024, 1024), background="#a9c3d6", sun_dir=(-0.4, 0.5, 1.0), strength=3.4)
    bpy.context.scene.world.node_tree.nodes["Background"].inputs["Strength"].default_value = 1.1
    vc.aim_camera(cam, (-256, -256, 400), (-256, -256.001, 0), ortho_scale=512)
    cam.rotation_euler = (0, 0, math.pi)  # engine +Z (Blender -Y) up in the image, +X right
    vc.render_to(os.path.join(vc.PREVIEWS, "terrain", "zone1_topdown.png"))
    bpy.context.scene.render.resolution_x, bpy.context.scene.render.resolution_y = 1280, 720
    vc.aim_camera(cam, (-90, 80, 190), (-250, -240, 10), lens=28)
    vc.render_to(os.path.join(vc.PREVIEWS, "terrain", "zone1_oblique.png"))
    vc.aim_camera(cam, (-215, -175, 62), (-262, -258, 18), lens=30)
    vc.render_to(os.path.join(vc.PREVIEWS, "terrain", "zone1_village.png"))
    os.remove(path)


def wall_distance(px, pz, o):
    """Distance from (px, pz) to a house's walls (its rotated rectangle; 0 inside)."""
    dx, dz = px - o["x"], pz - o["z"]
    c, s = math.cos(o["yaw"]), math.sin(o["yaw"])
    lx, lz = dx * c - dz * s, dx * s + dz * c
    ex, ez = max(abs(lx) - HOUSE_HALF[0] * o["scale"], 0.0), max(abs(lz) - HOUSE_HALF[1] * o["scale"], 0.0)
    return math.hypot(ex, ez)


def check_layout(F, objects, walk):
    """Hard checks on the village layout (the content tests repeat them on the built terrain)."""
    stone = [o for o in objects if o["mesh"] == "ant_tasi"]
    if len(stone) != 1:
        raise SystemExit("exactly one ant_tasi expected")
    st = stone[0]
    if walk[int(st["z"]), int(st["x"])]:
        raise SystemExit("ant_tasi: its cell is walkable")
    for k in range(32):  # walkable ring 3.5 m out (plinth 1.8 x 1.2 m + character radius)
        a = 2 * math.pi * k / 32
        x, z = st["x"] + 3.5 * math.cos(a), st["z"] + 3.5 * math.sin(a)
        if not walk[int(z), int(x)]:
            raise SystemExit(f"ant_tasi: ground around it is blocked at ({x:.1f}, {z:.1f})")
    ys = [bilinear(F["h"], st["x"] + dx, st["z"] + dz) for dx in (-1.8, 0, 1.8) for dz in (-1.8, 0, 1.8)]
    houses = [o for o in objects if o["mesh"] == "house"]
    for (name, x, z, _) in SPAWNS:
        wall = min(wall_distance(x, z, o) for o in houses)
        d_stone = math.hypot(x - st["x"], z - st["z"])
        print(f"SPAWN {name}: nearest house wall {wall:.1f} m, Ant Tasi {d_stone:.1f} m")
        if wall < SPAWN_WALL_CLEAR:
            raise SystemExit(f"{name}: house wall {wall:.1f} m away (< {SPAWN_WALL_CLEAR})")
        if d_stone > 40.0:
            raise SystemExit(f"{name}: {d_stone:.1f} m from the Ant Tasi")
    apart = math.hypot(SPAWNS[0][1] - SPAWNS[1][1], SPAWNS[0][2] - SPAWNS[1][2])
    print(f"SPAWN apart {apart:.1f} m; Ant Tasi at ({st['x']}, {st['z']}) yaw {st['yaw']:.3f}, "
          f"ground {min(ys):.2f}..{max(ys):.2f} m")
    if not 40.0 <= apart <= 60.0:
        raise SystemExit(f"spawns {apart:.1f} m apart")


def main():
    args = vc.script_args()
    F = build_fields()
    C = cell_fields(F)
    objects = place_all(F, C)
    walk, splat = build_maps(F, C, objects)
    for (name, x, z, _) in SPAWNS:
        if not walk[int(z), int(x)]:
            raise SystemExit(f"{name} is not walkable")
    check_layout(F, objects, walk)
    hmin, hmax = write_outputs(F, objects, walk, splat)
    counts = {}
    for o in objects:
        counts[o["mesh"]] = counts.get(o["mesh"], 0) + 1
    print(f"TERRAIN heights [{F['h'].min():.2f}, {F['h'].max():.2f}] stored range [{hmin}, {hmax}], "
          f"walkable {100.0 * walk.mean():.1f}%, objects {len(objects)} {counts}")
    for (name, x, z, yaw) in SPAWNS:
        print(f"SPAWN {name} x={x} z={z} y={bilinear(F['h'], x, z):.2f} yaw={yaw:.3f}")
    walkmask_preview(F, C, walk, objects)
    if "--no-preview" not in args:
        blender_preview(F, splat, objects)


main()
