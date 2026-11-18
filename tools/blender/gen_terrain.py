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

import json
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

# Prop footprints come from the real meshes: gen_props.py writes assets_src/props/props_meta.json with,
# per prop, the convex hull (engine local x, z) of everything below about head height, or a disc for
# posts and tree trunks (canopies do not block). The walk mask blocks every cell whose centre lies
# within CHAR_RADIUS of a placed footprint (stretch and yaw applied).
with open(os.path.join(vc.SRC, "props", "props_meta.json"), "r", encoding="utf-8") as _f:
    PROP_META = json.load(_f)
# M13 props (gates, homeland settlement props) are procedural (gen_zone_props.py) and keep their own
# meta file; a gate's footprint is "multi" (one hull per pillar: the opening stays walkable).
_ZONE_META = os.path.join(vc.SRC, "props", "zone_props_meta.json")
if os.path.exists(_ZONE_META):
    with open(_ZONE_META, "r", encoding="utf-8") as _f:
        PROP_META.update(json.load(_f))
# M14 stall props (gen_shop_props.py): the homelands' potion stalls and smithies (gen_zones.py places them).
_SHOP_META = os.path.join(vc.SRC, "props", "shop_props_meta.json")
if os.path.exists(_SHOP_META):
    with open(_SHOP_META, "r", encoding="utf-8") as _f:
        PROP_META.update(json.load(_f))
HOUSES = ("house_a", "cottage")
WALLED = HOUSES + ("watchtower",)  # spawns keep SPAWN_WALL_CLEAR from these
TREES = ("tree_oak", "tree_pine")
# Zone 1 terrain layers in splat order: base colour, normal, ORM (gen_textures.py), tile size (m).
LAYERS = (("grass", 2.6), ("dirt", 3.0), ("rock", 7.0), ("drygrass", 3.6))
# Scripted UI flows (tools/dev, client/automation/flows) walk these straight lines / areas: nothing
# that blocks may stand on them. world_tour: from the west house to the north-east hill and up it.
CLEAR_PATHS = [((226.0, 255.5), (334.0, 338.0), 3.5)]
CLEAR_AREAS = [((334.0, 338.0), 26.0)]
# The west house (polish / world_tour flows: front wall facing +X at x ~ 220.2, z 254.6 .. 261.6).
WEST_HOUSE = (216.4, 258.1)
# The Ant Taşı (docs/LORE.md) stands in the village plaza between the two spawns, off the road, its
# Aydor face (front, +Z) turned towards the Aydor spawn and its Kargan face towards the Kargan spawn.
ANT_TASI = (249.0, 263.5)
# Spawns: on the plateau, 40-60 m apart, each within 40 m of the stone and at least SPAWN_WALL_CLEAR
# from every house wall (the follow camera must not end up inside a house).
SPAWNS = [("spawn_aydor", 238.0, 271.0, math.radians(135.0)),
          ("spawn_kargan", 277.0, 247.0, math.atan2(ANT_TASI[0] - 277.0, ANT_TASI[1] - 247.0))]
SPAWN_WALL_CLEAR = 8.0
# M13 "Üç Yurt": the gates to the two homelands and where a player arriving through them appears
# (docs/WORLD_ZONES_ART.md). Aydor gate south-south-east of the village just outside the house ring,
# facing the plaza: the straight walk in passes between two houses and stays 5 m beyond every
# monster's reach (the south road runs through the south pack, so the gate does not face it); its
# arrival point lies inside the village's safe ground. Kargan gate north, east of the north road and
# well west of the line world_tour walks, facing the road 12 m away. A gate's yaw turns its front
# (the approach side) towards its arrival point; an arrival point stands 8 m in front of its gate,
# facing away from it. They are written after SPAWNS and placed after everything else (props in the
# way are dropped), so every other placement keeps its exact position.
GATE_MARKERS = [  # (gate marker, arrival marker, gate prop, gate x, z, arrival distance[, target])
    ("gate_aydor", "arrive_aydor", "gate_aydor", 287.0, 213.0, 8.0, "plaza"),
    ("gate_kargan", "arrive_kargan", "gate_kargan", 274.0, 326.0, 8.0),
]

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

def local_polygon(name):
    """Footprint of a prop in its local engine frame: polygon points (N x 2) or a disc radius."""
    fp = PROP_META[name]["footprint"]
    if fp["kind"] == "disc":
        return None, float(fp["radius"])
    if fp["kind"] == "multi":  # all parts' points (radius, ground offset); distances go per part
        return np.array([p for part in fp["parts"] for p in part], float), None
    return np.array(fp["points"], float), None


def placed_polygon(o, points=None):
    """World x/z polygon (or centre + radius) of a placement's footprint (or of `points`, local)."""
    poly, r = local_polygon(o["mesh"]) if points is None else (np.array(points, float), None)
    sx, sz = o["scale"] * o["stretch"][0], o["scale"] * o["stretch"][2]
    if poly is None:
        return None, r * o["scale"] * max(o["stretch"][0], o["stretch"][2])
    c, s = math.cos(o["yaw"]), math.sin(o["yaw"])
    lx, lz = poly[:, 0] * sx, poly[:, 1] * sz
    # local +X -> world (cos, -sin), local +Z -> world (sin, cos)  (RotY(yaw), forward(yaw) = (sin, cos))
    wx = o["x"] + lx * c + lz * s
    wz = o["z"] - lx * s + lz * c
    return np.stack([wx, wz], 1), None


def poly_distance(px, pz, poly):
    """Distance from points to a convex polygon (0 inside). px, pz arrays."""
    px, pz = np.asarray(px, float), np.asarray(pz, float)
    d = np.full(px.shape, 1e9)
    inside = np.ones(px.shape, bool)
    n = len(poly)
    area = 0.0
    for k in range(n):
        a, b = poly[k], poly[(k + 1) % n]
        area += a[0] * b[1] - b[0] * a[1]
    sign = 1.0 if area > 0 else -1.0
    for k in range(n):
        a, b = poly[k], poly[(k + 1) % n]
        d = np.minimum(d, seg_distance(px, pz, a, b))
        cross = (b[0] - a[0]) * (pz - a[1]) - (b[1] - a[1]) * (px - a[0])
        inside &= cross * sign >= 0.0
    return np.where(inside, 0.0, d)


def footprint_distance(px, pz, o):
    fp = PROP_META[o["mesh"]]["footprint"]
    if fp["kind"] == "multi":
        return np.minimum.reduce([poly_distance(px, pz, placed_polygon(o, part)[0]) for part in fp["parts"]])
    poly, r = placed_polygon(o)
    if poly is None:
        return np.maximum(np.hypot(np.asarray(px) - o["x"], np.asarray(pz) - o["z"]) - r, 0.0)
    return poly_distance(px, pz, poly)


def prop_radius(name, scale, stretch=(1.0, 1.0, 1.0)):
    poly, r = local_polygon(name)
    s = scale * max(stretch[0], stretch[2])
    if poly is None:
        return r * s
    return float(np.max(np.hypot(poly[:, 0], poly[:, 1]))) * s


def slot_areas(zone, margin):
    """M15: the hunting slots of `zone` (gamedata/monsters.toml [[slot]]) as keep-out discs (area + margin): the
    slots stand on open ground (no prop, no tree shade over the monsters) and every home stays walkable."""
    import tomllib
    with open(os.path.join(vc.REPO, "gamedata", "monsters.toml"), "rb") as f:
        data = tomllib.load(f)
    out = []
    for s in data.get("slot", []):
        if s["zone"] != zone:
            continue
        far = max((math.hypot(dx, dz) for dx, dz in s["homes"]), default=0.0) if "homes" in s else \
            (s.get("ring", 0.0) if s["count"] > 1 else 0.0)
        out.append(((s["x"], s["z"]), far + s.get("wander", 1.0) + margin))
    return out


SLOT_MARGIN = 8.0  # M15: metres of open ground round a slot's area (zone 1 here, zones 2-3 in gen_zones.py)


class Placer:
    slot_keep = None  # zone 1's slot discs, loaded on first use

    def __init__(self, F, C):
        self.F, self.C = F, C
        self.objects = []  # dict(mesh, x, z, yaw, scale, stretch, tint, y_offset, blocks)

    def free(self, name, x, z, scale, clearance=0.6, road_clear=1.0, spawn_clear=4.0, stretch=(1, 1, 1),
             blocks=True):
        if not (MARGIN < x < SIZE - MARGIN and MARGIN < z < SIZE - MARGIN):
            return False
        r = prop_radius(name, scale, stretch)
        for road in self.F["roads"]:
            if float(distance_to_polyline(np.array([x]), np.array([z]), road)[0]) < r + ROAD_HALF + road_clear:
                return False
        for (_, sx, sz, _) in SPAWNS:
            if math.hypot(x - sx, z - sz) < r + spawn_clear:
                return False
        if blocks:
            for (a, b, half) in CLEAR_PATHS:
                if float(seg_distance(np.array([x]), np.array([z]), a, b)[0]) < r + half:
                    return False
            for (c, rad) in CLEAR_AREAS:
                if math.hypot(x - c[0], z - c[1]) < r + rad:
                    return False
        if Placer.slot_keep is None:
            Placer.slot_keep = slot_areas(1, SLOT_MARGIN)
        if name in ("tree_oak", "tree_pine", "boulder"):  # only the natural scatter; houses and fences stay
            for (c, rad) in Placer.slot_keep:
                if math.hypot(x - c[0], z - c[1]) < r + rad:
                    return False

        for o in self.objects:
            if math.hypot(x - o["x"], z - o["z"]) < r + prop_radius(o["mesh"], o["scale"], o["stretch"]) + clearance:
                return False
        return True

    def ground_offset(self, name, x, z, yaw, scale, stretch, extra=0.0):
        """y_offset that puts the lowest point of the footprint on the ground (the prop's own sink
        then hides the rest), minus `extra`."""
        o = dict(mesh=name, x=x, z=z, yaw=yaw, scale=scale, stretch=stretch)
        poly, r = placed_polygon(o)
        if poly is None:
            pts = [(x + r * math.cos(a), z + r * math.sin(a)) for a in np.linspace(0, 2 * math.pi, 8, endpoint=False)]
        else:
            pts = list(poly)
        h0 = bilinear(self.F["h"], x, z)
        hmin = min(bilinear(self.F["h"], px, pz) for px, pz in pts)
        return min(hmin - h0, 0.0) - extra

    def add(self, name, x, z, yaw, scale=1.0, stretch=(1.0, 1.0, 1.0), tint=(255, 255, 255), extra_sink=0.0,
            blocks=True, y_offset=None, **kw):
        if not self.free(name, x, z, scale, stretch=stretch, blocks=blocks, **kw):
            return False
        if y_offset is None:
            y_offset = self.ground_offset(name, x, z, yaw, scale, stretch, extra_sink)
        self.objects.append(dict(mesh=name, x=float(x), z=float(z), yaw=float(yaw), scale=float(scale),
                                 stretch=tuple(float(s) for s in stretch), tint=tuple(int(t) for t in tint),
                                 y_offset=float(y_offset), blocks=blocks))
        return True

    def slope_at(self, x, z):
        i, j = int(np.clip(x, 0, M - 1)), int(np.clip(z, 0, M - 1))
        return float(self.C["slope"][j, i])


def facing(x, z, tx, tz):
    """Yaw that turns the prop's front (+Z) towards (tx, tz)."""
    return math.atan2(tx - x, tz - z)


# Roadside props placed with their lantern over the road (road_clear < 0) and how far their footprint stays
# from any road centre line: lamp posts off the road surface, braziers (wide fire bowls) also clear of
# the corridor walked routes take round bends (fix1: a walk down the Demirağız road stuck on one).
ROADSIDE = {"lamp_post": ROAD_HALF + 0.3, "kargan_brazier": ROAD_HALF + 2.3}


def road_footprint_distance(o, roads):
    """Smallest distance from any road centre line (points every <= 2 m) to the placement's footprint."""
    return min(float(np.min(footprint_distance(r[:, 0], r[:, 1], o))) for r in roads)


def clear_roadsides(P, roads, centre, plaza_r):
    """fix1: lamp posts / braziers whose footprint reaches into a road (on a bend, or where two roads
    meet) are pushed out of it, away from the nearest centre-line point, if the new spot is free of the
    other placements; otherwise dropped. Runs after everything else is placed, so no other placement
    moves. Plaza decoration (within plaza_r of the centre) is left alone."""
    kept = []
    for o in P.objects:
        if o["mesh"] not in ROADSIDE or math.hypot(o["x"] - centre[0], o["z"] - centre[1]) < plaza_r:
            kept.append(o)
            continue
        d = road_footprint_distance(o, roads)
        if d >= ROADSIDE[o["mesh"]]:
            kept.append(o)
            continue
        pts = np.concatenate(roads)
        k = int(np.argmin(np.hypot(pts[:, 0] - o["x"], pts[:, 1] - o["z"])))
        away = np.array([o["x"], o["z"]]) - pts[k]
        away /= max(float(np.linalg.norm(away)), 1e-6)
        moved = None
        for step in np.arange(0.25, 4.01, 0.25):
            q = dict(o, x=float(o["x"] + away[0] * step), z=float(o["z"] + away[1] * step))
            if road_footprint_distance(q, roads) < ROADSIDE[o["mesh"]]:
                continue
            r = prop_radius(q["mesh"], q["scale"], q["stretch"])
            if any(p is not o and math.hypot(q["x"] - p["x"], q["z"] - p["z"]) <
                   r + prop_radius(p["mesh"], p["scale"], p["stretch"]) + 0.3 for p in P.objects):
                continue
            q["y_offset"] = P.ground_offset(q["mesh"], q["x"], q["z"], q["yaw"], q["scale"], q["stretch"])
            moved = q
            break
        print(f"ROADSIDE {o['mesh']} ({o['x']:.2f}, {o['z']:.2f}) {d:.2f} m from a road centre: " +
              (f"moved to ({moved['x']:.2f}, {moved['z']:.2f})" if moved else "dropped"))
        if moved:
            kept.append(moved)
    P.objects = kept
    return kept


# House variants: (tint sRGB, stretch x/y/z). Variant B of each kind is a little wider or taller and
# warmer/greyer in colour; stretch stays within +-8 %.
HOUSE_VARIANTS = {
    "house_a": [((255, 255, 255), (1.0, 1.0, 1.0)), ((238, 228, 212), (1.06, 0.95, 1.04))],
    "cottage": [((255, 255, 255), (1.0, 1.0, 1.0)), ((228, 222, 214), (0.95, 1.07, 1.05))],
}

# Village houses: (kind, angle deg around the plaza centre, ring radius, variant). The door faces the
# plaza. Gaps for the three roads (90, 270 and 0 degrees) and around both spawns' views.
VILLAGE = [
    ("cottage", 22.0, 45.0, 0), ("house_a", 40.0, 39.0, 1), ("cottage", 252.0, 58.0, 1),
    ("house_a", 112.0, 40.0, 0), ("cottage", 134.0, 44.0, 0), ("house_a", 154.0, 41.0, 1),
    ("cottage", 200.0, 41.0, 1), ("house_a", 222.0, 40.0, 0), ("cottage", 244.0, 43.0, 0),
    ("house_a", 296.0, 40.0, 1), ("cottage", 318.0, 43.0, 1), ("house_a", 340.0, 41.0, 0),
]


def place_all(F, C):
    P = Placer(F, C)
    rng = np.random.default_rng(SEED + 1)
    cx, cz = CENTER
    # The oath stone first: everything else keeps its clearance from it.
    sx, sz = SPAWNS[0][1], SPAWNS[0][2]
    if not P.add("ant_tasi", ANT_TASI[0], ANT_TASI[1], facing(ANT_TASI[0], ANT_TASI[1], sx, sz), road_clear=1.5,
                 spawn_clear=8.0, clearance=2.5):
        raise SystemExit("ant_tasi: placement is not free")
    # The west house of the flows, exactly: front wall (+Z local) facing +X.
    if not P.add("house_a", WEST_HOUSE[0], WEST_HOUSE[1], math.pi / 2, road_clear=2.5, clearance=3.0):
        raise SystemExit("west house: placement is not free")
    # Houses on a ring around the plaza, doors towards it.
    for kind, ang, rr, var in VILLAGE:
        a = math.radians(ang)
        x, z = cx + rr * math.cos(a), cz + rr * math.sin(a)
        tint, stretch = HOUSE_VARIANTS[kind][var]
        yaw = facing(x, z, cx, cz) + rng.uniform(-0.12, 0.12)
        if not P.add(kind, x, z, yaw, stretch=stretch, tint=tint, road_clear=2.5, clearance=3.0):
            # Try a little further out before giving up on the slot.
            x, z = cx + (rr + 4.0) * math.cos(a), cz + (rr + 4.0) * math.sin(a)
            if not P.add(kind, x, z, yaw, stretch=stretch, tint=tint, road_clear=2.5, clearance=3.0):
                print(f"TERRAIN village slot {kind} at {ang:.0f} deg / {rr:.0f} m is not free; skipped")
    houses = [o for o in P.objects if o["mesh"] in HOUSES]
    # Goods beside the houses' doors (front-left or front-right corner).
    for k, h in enumerate(houses):
        side = -1.0 if k % 2 else 1.0
        fwd = (math.sin(h["yaw"]), math.cos(h["yaw"]))
        right = (math.cos(h["yaw"]), -math.sin(h["yaw"]))
        d = PROP_META[h["mesh"]]["bounds_max"][2] * h["scale"] * h["stretch"][2] + 1.6
        w = PROP_META[h["mesh"]]["bounds_max"][0] * h["scale"] * h["stretch"][0] - 0.6
        x = h["x"] + fwd[0] * d + right[0] * w * side
        z = h["z"] + fwd[1] * d + right[1] * w * side
        if k % 3 != 2:
            P.add("goods", x, z, h["yaw"] + rng.uniform(-0.6, 0.6), scale=rng.uniform(0.85, 1.05), clearance=0.4,
                  road_clear=0.8)
    # Well in the plaza, off the road.
    P.add("well", cx - 10.0, cz - 8.0, 0.4, clearance=1.0)
    # Market stalls around the plaza facing the centre, goods behind each.
    for ang in (205.0, 232.0, 318.0):
        a = math.radians(ang)
        x, z = cx + 21.0 * math.cos(a), cz + 21.0 * math.sin(a)
        if P.add("market_stall", x, z, facing(x, z, cx, cz), clearance=1.0):
            for k in range(2):
                b = a + (k - 0.5) * 0.16
                rr = 24.8 + rng.uniform(0, 0.8)
                P.add("goods", cx + rr * math.cos(b), cz + rr * math.sin(b), facing(x, z, cx, cz) + rng.uniform(-0.8, 0.8),
                      scale=rng.uniform(0.8, 1.0), clearance=0.3)
    # Signpost at the crossroads (its arms towards the roads), and near the three passes.
    for (x, z, tx, tz) in ((cx + 6.6, cz - 6.2, cx, cz - 20), (256 + 9.0, 78.0, 256, 60),
                           (252 - 9.0, 432.0, 252, 450), (420.0, 258.0, 440, 268)):
        P.add("signpost", x, z, facing(x, z, tx, tz), road_clear=0.2, spawn_clear=3.0, clearance=0.4)
    # Lamp posts along the roads near the village, alternating sides, lanterns over the road.
    for road in F["roads"]:
        acc, side = 0.0, 1
        for i in range(1, len(road)):
            p0, p1 = road[i - 1], road[i]
            seg = np.linalg.norm(p1 - p0)
            acc += seg
            if acc < 18.0:
                continue
            acc = 0.0
            if np.hypot(*(p1 - CENTER)) > 115 or np.hypot(*(p1 - CENTER)) < 22:
                continue
            d = (p1 - p0) / seg
            nrm = np.array([d[1], -d[0]]) * side
            q = p1 + nrm * (ROAD_HALF + 1.3)
            if P.add("lamp_post", q[0], q[1], facing(q[0], q[1], p1[0], p1[1]), road_clear=-ROAD_HALF + 0.6,
                     clearance=0.5):
                side = -side
    # Watchtowers guarding the three passes.
    for (x, z) in ((268.0, 66.0), (240.0, 442.0), (446.0, 282.0)):
        P.add("watchtower", x, z, facing(x, z, cx, cz), road_clear=1.5)
    # Fences: yards behind some houses (an L of two or three sections) and the road edges just outside
    # the village. Sections are 4 m (gen_props fence) along their local X.
    def fence_run(ax, az, bx, bz, gaps=()):
        L = math.hypot(bx - ax, bz - az)
        n = max(1, int(round(L / 4.0)))
        yaw = math.atan2(bx - ax, bz - az) - math.pi / 2  # fence runs along its local X
        for s in range(n):
            if s in gaps:
                continue
            t0, t1 = s / n, (s + 1) / n
            x0, z0 = ax + (bx - ax) * t0, az + (bz - az) * t0
            x1, z1 = ax + (bx - ax) * t1, az + (bz - az) * t1
            # Skip sections whose ends differ too much in height (the rigid rail would float).
            if abs(bilinear(F["h"], x0, z0) - bilinear(F["h"], x1, z1)) > 0.45:
                continue
            P.add("fence", 0.5 * (x0 + x1), 0.5 * (z0 + z1), yaw, stretch=(L / n / 4.0, 1.0, 1.0), clearance=-0.6,
                  road_clear=0.4)
    for k, h in enumerate(houses):
        if k % 2:
            continue
        fwd = (math.sin(h["yaw"]), math.cos(h["yaw"]))
        right = (math.cos(h["yaw"]), -math.sin(h["yaw"]))
        back = -PROP_META[h["mesh"]]["bounds_min"][2] * h["stretch"][2] - 0.4
        half = PROP_META[h["mesh"]]["bounds_max"][0] * h["stretch"][0] + 2.5
        depth = 9.0
        p = lambda u, v: (h["x"] + right[0] * u - fwd[0] * v, h["z"] + right[1] * u - fwd[1] * v)  # noqa: E731
        c0, c1, c2, c3 = p(-half, back), p(-half, back + depth), p(half, back + depth), p(half, back)
        fence_run(*c0, *c1)
        fence_run(*c1, *c2, gaps=(1,))
        fence_run(*c2, *c3)
    for road in F["roads"]:
        for side in (1.0, -1.0):
            run = []
            for i in range(1, len(road)):
                p = road[i]
                dc = float(np.hypot(*(p - CENTER)))
                if 70.0 < dc < 105.0 and (i // 6) % 3 != 2:
                    d = road[i] - road[i - 1]
                    d /= np.linalg.norm(d)
                    run.append(p + np.array([d[1], -d[0]]) * side * (ROAD_HALF + 2.2))
                elif len(run) > 1:
                    for k in range(0, len(run) - 4, 4):
                        fence_run(*run[k], *run[k + 4])
                    run = []
                else:
                    run = []
    # Groves of oaks and pines at the plateau edges and on the hills; three sizes.
    sizes = (0.78, 1.0, 1.18)
    groves = 0
    tries = 0
    while groves < 30 and tries < 5000:
        tries += 1
        gx, gz = rng.uniform(40, SIZE - 40), rng.uniform(40, SIZE - 40)
        if math.hypot(gx - cx, gz - cz) < 72:
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
            if P.slope_at(x, z) > 0.6 or math.hypot(x - cx, z - cz) < 60:
                continue
            kind = "tree_pine" if rng.uniform() < 0.35 + pine_bias else "tree_oak"
            s = sizes[int(rng.integers(0, 3))] * rng.uniform(0.94, 1.06)
            if P.add(kind, x, z, rng.uniform(0, 6.283), scale=s, extra_sink=0.05, clearance=1.4, road_clear=3.0):
                count -= 1
    # Scattered single trees.
    for _ in range(140):
        x, z = rng.uniform(MARGIN + 4, SIZE - MARGIN - 4), rng.uniform(MARGIN + 4, SIZE - MARGIN - 4)
        if math.hypot(x - cx, z - cz) < 62 or P.slope_at(x, z) > 0.55:
            continue
        s = sizes[int(rng.integers(0, 3))] * rng.uniform(0.94, 1.06)
        P.add("tree_oak" if rng.uniform() < 0.6 else "tree_pine", x, z, rng.uniform(0, 6.283), scale=s,
              extra_sink=0.05, clearance=1.6, road_clear=3.0)
    # Boulders on slopes (and a few in the meadows), random yaw and size, sunk into the slope.
    placed = 0
    for _ in range(3000):
        if placed >= 95:
            break
        x, z = rng.uniform(MARGIN + 3, SIZE - MARGIN - 3), rng.uniform(MARGIN + 3, SIZE - MARGIN - 3)
        s = P.slope_at(x, z)
        if math.hypot(x - cx, z - cz) < 60:
            continue
        if s < 0.3 and rng.uniform() > 0.08:
            continue
        sc = rng.uniform(0.55, 1.5)
        if P.add("boulder", x, z, rng.uniform(0, 6.283), scale=sc, extra_sink=0.12 * sc, clearance=0.8,
                 road_clear=2.0):
            placed += 1
    return clear_roadsides(P, F["roads"], CENTER, 22.0)


# Placements that may give way to a gate or an arrival point (decoration); anything else in the way
# is an error (move the marker instead).
YIELDING = ("tree_oak", "tree_pine", "boulder", "fence", "goods", "lamp_post", "signpost")


def nearest_road_point(F, x, z):
    best, bd = None, 1e9
    for road in F["roads"]:
        d = np.hypot(road[:, 0] - x, road[:, 1] - z)
        k = int(np.argmin(d))
        if d[k] < bd:
            best, bd = road[k], float(d[k])
    return (float(best[0]), float(best[1])), bd


def gate_layout(F, gate_markers, target):
    """Per gate: dict(gate=(name, x, z, yaw), arrive=(name, x, z, yaw), mesh, road=(x, z)). The
    arrival point lies `dist` m from the gate towards `target` (None: the nearest road point, so the
    approach from the road runs straight through the opening); the gate's front faces it. A marker
    tuple may carry its own target (7th item: "plaza" = the village centre, the walk ends at the
    plaza's dirt edge 17 m before it)."""
    out = []
    for (gname, aname, mesh, gx, gz, dist, *own) in gate_markers:
        road, _ = nearest_road_point(F, gx, gz)
        tx, tz = road if target is None else target
        if own and own[0] == "plaza":
            tx, tz = CENTER
        dx, dz = tx - gx, tz - gz
        n = math.hypot(dx, dz)
        if own and own[0] == "plaza":
            road = (gx + dx / n * (n - 17.0), gz + dz / n * (n - 17.0))
        ax, az = gx + dx / n * dist, gz + dz / n * dist
        yaw = facing(gx, gz, ax, az)
        out.append(dict(gate=(gname, gx, gz, yaw), arrive=(aname, round(ax, 2), round(az, 2), yaw), mesh=mesh,
                        road=road))
    return out


def gate_keepout(g):
    """Keep-out shapes around a gate: (kind, data, clearance) for clear_for_gates."""
    (_, gx, gz, _), (_, ax, az, _) = g["gate"], g["arrive"]
    return [("disc", (gx, gz), 6.5), ("disc", (ax, az), 4.5), ("seg", ((gx, gz), (ax, az)), 2.5),
            ("seg", ((gx, gz), g["road"]), 1.2)]


def clear_for_gates(objects, gates, yielding=YIELDING):
    """Drops the placements whose footprint comes into a gate's keep-out shapes."""
    keep = []
    for o in objects:
        hit = False
        for g in gates:
            for kind, data, clear in gate_keepout(g):
                if kind == "disc":
                    d = float(np.asarray(footprint_distance(np.array([data[0]]), np.array([data[1]]), o)).ravel()[0])
                else:
                    (ax, az), (bx, bz) = data
                    ts = np.linspace(0.0, 1.0, max(2, int(math.hypot(bx - ax, bz - az) / 0.5)))
                    d = float(np.min(footprint_distance(ax + ts * (bx - ax), az + ts * (bz - az), o)))
                hit = hit or d < clear
        if hit:
            if o["mesh"] not in yielding:
                raise SystemExit(f"a gate: {o['mesh']} at ({o['x']:.1f}, {o['z']:.1f}) is in the way")
            continue
        keep.append(o)
    return keep


def add_gates(P, gates):
    """Gate props at the gate markers (only once gen_zone_props.py has built them): not a blocking
    placement (the opening is walkable), the pillars block the walk mask (`mask`)."""
    for g in gates:
        name, x, z, yaw = g["gate"]
        if g["mesh"] not in PROP_META:
            print(f"TERRAIN {g['mesh']}: not built yet, {name} has no gate prop")
            continue
        y = P.ground_offset(g["mesh"], x, z, yaw, 1.0, (1.0, 1.0, 1.0))
        P.objects.append(dict(mesh=g["mesh"], x=float(x), z=float(z), yaw=float(yaw), scale=1.0,
                              stretch=(1.0, 1.0, 1.0), tint=(255, 255, 255), y_offset=float(y), blocks=False,
                              mask=True))


def walkable_line(walk, a, b, step=0.25):
    """True if every mask cell on the straight line a-b is walkable."""
    n = max(2, int(math.hypot(b[0] - a[0], b[1] - a[1]) / step))
    for t in np.linspace(0.0, 1.0, n):
        x, z = a[0] + t * (b[0] - a[0]), a[1] + t * (b[1] - a[1])
        if not walk[int(z), int(x)]:
            return False
    return True


def check_gates(walk, gates, size=SIZE):
    """Arrival points: the 3 m around them walkable. Gates: the opening (+-0.5 m across, +-2 m along)
    walkable, a straight walkable path to the arrival point and to the nearest road point."""
    for g in gates:
        gname, gx, gz, yaw = g["gate"]
        aname, ax, az, _ = g["arrive"]
        for dx in (-3.0, -1.5, 0.0, 1.5, 3.0):
            for dz in (-3.0, -1.5, 0.0, 1.5, 3.0):
                if not walk[int(az + dz), int(ax + dx)]:
                    raise SystemExit(f"{aname}: ({ax + dx:.1f}, {az + dz:.1f}) is blocked")
        fwd = (math.sin(yaw), math.cos(yaw))
        side = (math.cos(yaw), -math.sin(yaw))
        for u in (-0.5, 0.0, 0.5):
            for v in (-2.0, -1.0, 0.0, 1.0, 2.0):
                x, z = gx + side[0] * u + fwd[0] * v, gz + side[1] * u + fwd[1] * v
                if not walk[int(z), int(x)]:
                    raise SystemExit(f"{gname}: opening ({x:.1f}, {z:.1f}) is blocked")
        if not walkable_line(walk, (gx, gz), (ax, az)):
            raise SystemExit(f"{gname}: no straight walkable path to {aname}")
        if not walkable_line(walk, (gx, gz), g["road"]):
            raise SystemExit(f"{gname}: no straight walkable path to the road at {g['road']}")
        d_road = math.hypot(g["road"][0] - gx, g["road"][1] - gz)
        print(f"SPAWN {gname} ({gx}, {gz}) yaw {yaw:.3f}, {aname} ({ax}, {az}), road {d_road:.1f} m")


# ---------------------------------------------------------------------------------------------
# Maps

def footprint_cells(o):
    """Mask cells (j, i) whose centre lies within CHAR_RADIUS of the placed footprint."""
    r = prop_radius(o["mesh"], o["scale"], o["stretch"]) + CHAR_RADIUS + 1.0
    i0, i1 = max(int(o["x"] - r), 0), min(int(o["x"] + r) + 1, M - 1)
    j0, j1 = max(int(o["z"] - r), 0), min(int(o["z"] + r) + 1, M - 1)
    jj, ii = np.mgrid[j0:j1 + 1, i0:i1 + 1]
    d = footprint_distance(ii + 0.5, jj + 0.5, o)
    sel = d <= CHAR_RADIUS
    # The cell under the origin is always blocked (not for a gate: its origin is the opening).
    cells = [] if PROP_META[o["mesh"]]["footprint"]["kind"] == "multi" else [(int(o["z"]), int(o["x"]))]
    cells += list(zip(jj[sel].tolist(), ii[sel].tolist()))
    return cells


def build_maps(F, C, objects):
    slope = C["slope"]
    walk = np.ones((M, M), bool)
    walk &= slope <= MAX_SLOPE
    walk &= C["e"] > MARGIN
    for o in objects:
        # `mask`: blocks the walk mask without being a blocking placement (gates: the pillars block,
        # the opening between them stays walkable).
        if o.get("mask", o["blocks"]):
            for (j, i) in footprint_cells(o):
                walk[j, i] = False
    # Splat weights.
    rng = np.random.default_rng(SEED + 2)
    dry_noise = fbm(rng, 5, 4, n=M)
    # Ragged edges: the road and plaza borders wander by a metre or two (worn verges, not a ruler).
    edge = fbm(rng, 48, 3, n=M) - 0.5
    road = smoothstep(ROAD_HALF + 1.3, ROAD_HALF - 0.9, C["d_road"] + 2.2 * edge)
    road = np.maximum(road, smoothstep(19.5, 14.5, C["d_c"] + 6.0 * edge) * 0.95)
    rock = smoothstep(0.42, 0.75, slope)
    rock = np.maximum(rock, smoothstep(44.0, 58.0, C["h"]) * 0.8)
    dry = smoothstep(0.55, 0.74, dry_noise) * 0.85 * (1 - rock)
    dry = np.maximum(dry, smoothstep(34.0, 50.0, C["h"]) * 0.6 * (1 - rock))
    grass = np.clip(1.0 - np.maximum.reduce([road, rock, dry]), 0, 1)
    w = np.stack([grass * (1 - road), road, rock * (1 - road), dry * (1 - road)], axis=-1)
    w /= np.maximum(w.sum(axis=-1, keepdims=True), 1e-6)
    splat = np.round(w * 255).astype(np.int32)
    splat[..., 0] += 255 - splat.sum(axis=-1)  # exact 255 total
    splat = np.clip(splat, 0, 255).astype(np.uint8)
    return walk, splat


def write_outputs(F, objects, walk, splat, markers=SPAWNS):
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
    for tex, tile in LAYERS:
        lines += ["[[layer]]", f'texture = "textures/terrain/{tex}.png"', f'normal = "textures/terrain/{tex}_n.png"',
                  f'orm = "textures/terrain/{tex}_orm.png"', f"tile_size = {tile:.1f}", ""]
    for (name, x, z, yaw) in markers:
        lines += ["[[spawn]]", f'name = "{name}"', f"x = {x:.2f}", f"z = {z:.2f}", f"yaw = {yaw:.4f}", ""]
    for o in objects:
        lines += ["[[object]]", f'mesh = "props/{o["mesh"]}.vmesh"', f"x = {o['x']:.3f}", f"z = {o['z']:.3f}",
                  f"yaw = {o['yaw']:.4f}", f"scale = {o['scale']:.3f}", f"y_offset = {o['y_offset']:.3f}"]
        if tuple(o["stretch"]) != (1.0, 1.0, 1.0):
            lines += ["stretch = [{:.3f}, {:.3f}, {:.3f}]".format(*o["stretch"])]
        if tuple(o["tint"]) != (255, 255, 255):
            lines += ["tint = [{}, {}, {}]".format(*o["tint"])]
        lines += [f"blocks = {'true' if o['blocks'] else 'false'}", ""]
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
    for tex, tile in LAYERS:
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
        keep = [ob for ob in new if not ob.name.startswith("lod1")]  # the full mesh (lod0), not the far LOD
        for ob in new:
            if ob not in keep:
                bpy.data.objects.remove(ob)
        new = keep
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
        # Engine local (x, y, z) stretch -> Blender local (X, Z, Y) after the glTF import.
        ob.scale = (o["scale"] * o["stretch"][0], o["scale"] * o["stretch"][2], o["scale"] * o["stretch"][1])
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
    """Distance from (px, pz) to a building's footprint (0 inside)."""
    return float(footprint_distance(np.array([px]), np.array([pz]), o)[0])


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
    houses = [o for o in objects if o["mesh"] in WALLED]
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
    gates = gate_layout(F, GATE_MARKERS, None)
    P = Placer(F, C)
    P.objects = clear_for_gates(objects, gates)
    add_gates(P, gates)
    objects = P.objects
    markers = list(SPAWNS) + [m for g in gates for m in (g["gate"], g["arrive"])]
    # Round as written to the description, so the walk mask matches the compiled placements exactly.
    for o in objects:
        for k, nd in (("x", 3), ("z", 3), ("yaw", 4), ("scale", 3), ("y_offset", 3)):
            o[k] = round(o[k], nd)
        o["stretch"] = tuple(round(s, 3) for s in o["stretch"])
    walk, splat = build_maps(F, C, objects)
    for (name, x, z, _) in SPAWNS:
        if not walk[int(z), int(x)]:
            raise SystemExit(f"{name} is not walkable")
    check_layout(F, objects, walk)
    check_gates(walk, gates)
    hmin, hmax = write_outputs(F, objects, walk, splat, markers)
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


if __name__ == "__main__":  # gen_zones.py imports the field, placement and map helpers
    main()
