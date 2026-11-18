"""M13 "Üç Yurt": the two homeland zones (512 x 512 m each), built like zone 1 (gen_terrain.py, whose
noise, road, placement, footprint and map helpers are imported).

  zone 2  Tanyeri Burcu sınır boyu - the Aydor border march: a broad golden plain at ~12 m inside a
          ring of pale chalk escarpments, the white-stone keep town Tanyeri Burcu on a low terrace,
          fenced fields around it, scorched fields (the hunting grounds) further out, the road north
          up a cut in the escarpment to the gate to Eşik.
  zone 3  Demirağız geçidi - the Kargan pass: a narrow valley between dark snow-capped mountains,
          rising gently to the north where the hearth-town sits against the cliff of Demirağız
          (doors cut into the rock), two side basins (hunting grounds), the road south down a gorge
          to the gate to Eşik.

Usage (headless; needs the props from gen_props.py and gen_zone_props.py):
  blender --background --factory-startup --python tools/blender/gen_zones.py -- [--only zone2] [--no-preview]

Outputs per zone (assets_src/terrain/<zone>/): <zone>_height.png (513^2, 16-bit), <zone>_splat.png,
<zone>_walk.png, <zone>.terrain.toml (layers, the named markers of docs/WORLD_ZONES_ART.md, objects);
previews in assets_src/previews/terrain/<zone>_{walkmask,topdown,oblique,town}.png.
Markers: spawn_<race> (new characters), gate_esik (the gate's opening; yaw = the gate's front, towards
the approach), arrive_esik (8 m in front of the gate, facing away from it). Deterministic.
"""

import math
import os
import sys

import numpy as np

sys.dont_write_bytecode = True  # keep tools/blender free of __pycache__
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import vo_common as vc  # noqa: E402
import gen_terrain as gt  # noqa: E402  (its main() does not run on import)

SIZE, N, M = gt.SIZE, gt.N, gt.M
MARGIN = gt.MARGIN
smoothstep, fbm = gt.smoothstep, gt.fbm
HOUSES = ("house_a", "cottage", "aydor_house", "kargan_hut")


def grid():
    jj, ii = np.mgrid[0:N, 0:N].astype(float)
    return ii * (SIZE / (N - 1)), jj * (SIZE / (N - 1))


def road_distance(x, z, roads):
    return np.minimum.reduce([gt.distance_to_polyline(x, z, r) for r in roads])


def grade_roads(h, d_road, half, base=None):
    """Roads follow a smoothed version of the ground (gentle grades), blended over half..10 m (as
    zone 1)."""
    k = 15
    blur = (h if base is None else base).copy()
    for _ in range(3):
        acc = np.zeros_like(blur)
        for dj in (-k, 0, k):
            for di in (-k, 0, k):
                acc += np.roll(np.roll(blur, dj, 0), di, 1)
        blur = acc / 9.0
    w = smoothstep(10.0, half, d_road)
    return h * (1 - w) + (blur - 0.15) * w


# ---------------------------------------------------------------------------------------------
# Zone 2: Aydor border march (Tanyeri Burcu)

Z2_CENTER = (250.0, 296.0)          # plaza of the keep town
Z2_GATE = (256.0, 440.0)            # gate to Eşik, in the cut of the northern escarpment
Z2_ROADS = [
    # north road: town -> the cut -> the gate (it ends at the gate; the cut closes behind it)
    [(250, 296), (253, 336), (262, 378), (259, 412), (256, 440)],
    # south road through the fields to the scorched land
    [(250, 296), (245, 252), (253, 196), (241, 140), (248, 92)],
    # east-west lane
    [(112, 272), (178, 290), (250, 296), (322, 292), (398, 278)],
]
Z2_KNOLLS = [(150.0, 190.0, 28.0, 5.0), (370.0, 170.0, 34.0, 6.0), (400.0, 360.0, 30.0, 4.5),
             (120.0, 380.0, 26.0, 4.0), (300.0, 120.0, 22.0, 3.0)]


def fields_zone2():
    rng = np.random.default_rng(7310)
    x, z = grid()
    roads = [gt.road_points(r, 2.0) for r in Z2_ROADS]
    d_road = road_distance(x, z, roads)
    d_north = gt.distance_to_polyline(x, z, roads[0])
    broad = fbm(rng, 3, 4)
    swell = fbm(rng, 9, 3)
    detail = fbm(rng, 24, 3)
    h = 12.0 + 8.0 * (broad - 0.5) + 2.6 * (swell - 0.5) + 0.7 * (detail - 0.5)
    for (kx, kz, kr, kh) in Z2_KNOLLS:  # low barrow-like knolls in the plain
        h += kh * np.exp(-((x - kx) ** 2 + (z - kz) ** 2) / (kr * kr))
    # The town's terrace: flat at 14 m within ~55 m of the plaza.
    d_c = np.hypot(x - Z2_CENTER[0], z - Z2_CENTER[1])
    w = smoothstep(95.0, 58.0, d_c)
    h = h * (1 - w) + (14.0 + 0.3 * (detail - 0.5)) * w
    # Chalk escarpments all round: a steep step with a second, higher step behind (mesa edges), the
    # inner edge wavy. The north road's cut runs through to the gate and closes right behind it.
    e = np.minimum.reduce([x, z, SIZE - x, SIZE - z])
    wob = fbm(rng, 7, 3)
    jag = fbm(rng, 18, 3)
    e_mod = e + 44.0 * (wob - 0.5) + 16.0 * (jag - 0.5)
    step1 = smoothstep(66.0, 44.0, e_mod)
    step2 = smoothstep(30.0, 16.0, e_mod + 26.0 * (fbm(rng, 5, 3) - 0.5))
    crest = fbm(rng, 10, 3)
    cliff = (22.0 + 8.0 * crest) * step1 + (12.0 + 6.0 * crest) * step2
    cut = smoothstep(24.0, 8.0, d_north) * smoothstep(Z2_GATE[1] + 26.0, Z2_GATE[1] + 10.0, z)
    h = h + cliff * (1.0 - 0.97 * cut)
    h = grade_roads(h, d_road, 3.2)
    plaza = smoothstep(18.0, 13.0, d_c)
    h = h * (1 - plaza) + 13.95 * plaza
    return dict(h=h, x=x, z=z, d_road=d_road, d_c=d_c, e=e, roads=roads, rng=rng, plaza=plaza)


# Fenced fields (centre x, z, size along / across, yaw of the long side): irrigated plots south of
# the town, furrows of bare earth in them (splat).
Z2_FIELDS = [(206.0, 236.0, 34.0, 22.0, 0.10), (292.0, 238.0, 36.0, 22.0, -0.08), (196.0, 338.0, 30.0, 20.0, 0.20),
             (306.0, 344.0, 32.0, 20.0, -0.15), (296.0, 196.0, 30.0, 18.0, 0.05)]
# Scorched fields (the hunting grounds): centres and radii of the burnt patches.
Z2_SCORCH = [(250.0, 130.0, 62.0), (150.0, 210.0, 48.0), (362.0, 200.0, 52.0), (126.0, 318.0, 40.0),
             (394.0, 338.0, 40.0)]


def splat_zone2(F, C):
    rng = np.random.default_rng(7312)
    edge = fbm(rng, 48, 3, n=M) - 0.5
    patchy = fbm(rng, 6, 4, n=M)
    jj, ii = np.mgrid[0:M, 0:M] + 0.5
    road = smoothstep(gt.ROAD_HALF + 1.3, gt.ROAD_HALF - 0.9, C["d_road"] + 2.2 * edge)
    road = np.maximum(road, smoothstep(18.5, 13.5, C["d_c"] + 5.0 * edge) * 0.95)
    # Furrows: inside each field, stripes of bare earth along its long side.
    for (fx, fz, a, b, yaw) in Z2_FIELDS:
        c, s = math.cos(yaw), math.sin(yaw)
        u = (ii - fx) * c - (jj - fz) * s
        v = (ii - fx) * s + (jj - fz) * c
        inside = (np.abs(u) < a / 2 - 1.0) & (np.abs(v) < b / 2 - 1.0)
        stripes = (np.sin(v * math.pi / 1.4) > 0.1).astype(float)
        road = np.maximum(road, np.where(inside, 0.45 * stripes, 0.0))
    rock = smoothstep(0.42, 0.72, C["slope"])
    rock = np.maximum(rock, smoothstep(30.0, 40.0, C["h"]) * 0.75)
    scorch = np.zeros((M, M))
    ragged = fbm(rng, 16, 3, n=M) - 0.5
    for k, (sx, sz, r) in enumerate(Z2_SCORCH):
        a = 0.7 * k  # each patch stretched along its own direction (burnt along the wind)
        u = (ii - sx) * math.cos(a) + (jj - sz) * math.sin(a)
        v = -(ii - sx) * math.sin(a) + (jj - sz) * math.cos(a)
        d = np.hypot(u / 1.25, v * 1.2) + 34.0 * (patchy - 0.5) + 22.0 * ragged + 3.0 * edge
        scorch = np.maximum(scorch, smoothstep(r, r * 0.6, d))
    scorch *= (1 - rock)
    grass = np.clip(1.0 - np.maximum.reduce([road, rock, scorch]), 0, 1)
    return np.stack([grass * (1 - road), road, rock * (1 - road), scorch * (1 - road)], axis=-1)


# ---------------------------------------------------------------------------------------------
# Zone 3: Kargan pass below Demirağız

Z3_CENTER = (256.0, 344.0)          # plaza of the hearth-town
Z3_GATE = (256.0, 100.0)            # gate to Eşik, at the foot of the southern gorge
Z3_CLIFF_Z = 392.0                  # the rock face of Demirağız north of the town
Z3_SPINE = [(256, 70), (256, 104), (250, 160), (263, 222), (252, 288), (256, 344), (256, 400)]
# M15: the west and east basins widened, the small south-west one grown into a basin for the Örs Kıran
# slots, and a new south-east basin for the Körük Hanı (docs/DEVELOPMENT_PLAN.md §35.3).
Z3_BASINS = [(160.0, 245.0, 46.0), (352.0, 200.0, 46.0), (176.0, 148.0, 38.0), (335.0, 132.0, 30.0)]
Z3_ROADS = [
    [(256, 344), (252, 292), (262, 226), (250, 162), (256, 100)],
    [(254, 262), (214, 254), (176, 248)],
    [(259, 210), (300, 204), (338, 202)],
]


def z3_halfwidth(z):
    """Valley floor half-width along the spine: a gorge at the gate, wide in the middle, the town
    terrace, closed at the cliff."""
    pts = [(70.0, 0.0), (84.0, 12.0), (96.0, 20.0), (116.0, 24.0), (138.0, 30.0), (170.0, 46.0), (230.0, 54.0), (290.0, 50.0),
           (330.0, 58.0), (370.0, 56.0), (Z3_CLIFF_Z, 34.0), (Z3_CLIFF_Z + 14.0, 0.0)]
    zs, ws = zip(*pts)
    return np.interp(z, zs, ws)


def z3_floor(z):
    return 30.0 + 0.045 * (np.clip(z, 90.0, 400.0) - 90.0)


Z3_MAX_STEP = 2.6   # m per m (~69 deg): still a cliff, no longer a vertical seam


def relax_steps(h, max_slope, iters=200):
    """Pulls cells whose slope exceeds max_slope towards their 4-neighbour mean until none does (or
    `iters` passes); the rest of the heightfield is untouched."""
    for _ in range(iters):
        gz, gx = np.gradient(h, SIZE / (N - 1))
        steep = np.hypot(gx, gz) > max_slope
        if not steep.any():
            break
        p = np.pad(h, 1, mode="edge")
        avg = 0.25 * (p[:-2, 1:-1] + p[2:, 1:-1] + p[1:-1, :-2] + p[1:-1, 2:])
        grow = steep | np.roll(steep, 1, 0) | np.roll(steep, -1, 0) | np.roll(steep, 1, 1) | np.roll(steep, -1, 1)
        h = np.where(grow, 0.4 * h + 0.6 * avg, h)
    return h


def fields_zone3():
    rng = np.random.default_rng(9420)
    x, z = grid()
    roads = [gt.road_points(r, 2.0) for r in Z3_ROADS]
    d_road = road_distance(x, z, roads)
    spine = gt.road_points(Z3_SPINE, 2.0)
    d_sp = gt.distance_to_polyline(x, z, spine)
    wob = fbm(rng, 9, 3)
    jag = fbm(rng, 16, 3)
    hw = z3_halfwidth(z) + 18.0 * (wob - 0.5) * smoothstep(130.0, 170.0, z)
    d_rim = d_sp + 20.0 * (jag - 0.5) * smoothstep(120.0, 160.0, z)
    inside = smoothstep(hw + 30.0, hw, d_rim)
    for (bx, bz, br) in Z3_BASINS:
        db = np.hypot(x - bx, z - bz) + 18.0 * (wob - 0.5) + 16.0 * (jag - 0.5)
        inside = np.maximum(inside, smoothstep(br + 28.0, br - 4.0, db))
    floor = z3_floor(z)
    detail = fbm(rng, 24, 3)
    roll = fbm(rng, 10, 3)
    floor_h = floor + 3.2 * (roll - 0.5) + 0.6 * (detail - 0.5)
    mass = fbm(rng, 4, 4)
    ridged = 1.0 - np.abs(fbm(rng, 5, 5) * 2.0 - 1.0)
    peaks = 40.0 + 64.0 * mass + 44.0 * ridged ** 2
    h = floor_h + peaks * (1.0 - inside) ** 1.25
    # Broken rock on the slopes: crags and gullies (no smooth ramps), none on the valley floor.
    crag = fbm(rng, 36, 3)
    gully = 1.0 - np.abs(fbm(rng, 14, 3) * 2.0 - 1.0)
    h += (9.0 * (crag - 0.5) - 7.0 * gully ** 3) * smoothstep(0.85, 0.35, inside)
    # fix1: the Demirağız face north of the town rose ~95 m within 6 m (up to 20 m per metre), a hard
    # seam with a flat shelf on top; relax every step steeper than Z3_MAX_STEP towards its neighbours.
    h = relax_steps(h, Z3_MAX_STEP)
    # The town terrace: flat within ~45 m of the plaza (the cliff stays north of it).
    d_c = np.hypot(x - Z3_CENTER[0], z - Z3_CENTER[1])
    w = smoothstep(62.0, 40.0, d_c) * smoothstep(Z3_CLIFF_Z - 2.0, Z3_CLIFF_Z - 18.0, z)
    h = h * (1 - w) + (z3_floor(Z3_CENTER[1]) + 0.25 * (detail - 0.5)) * w
    h = grade_roads(h, d_road, 3.0, base=floor_h)
    plaza = smoothstep(15.0, 11.0, d_c)
    h = h * (1 - plaza) + (z3_floor(Z3_CENTER[1]) - 0.05) * plaza
    e = np.minimum.reduce([x, z, SIZE - x, SIZE - z])
    return dict(h=h, x=x, z=z, d_road=d_road, d_c=d_c, e=e, roads=roads, rng=rng, plaza=plaza, inside=inside)


def splat_zone3(F, C):
    rng = np.random.default_rng(9422)
    edge = fbm(rng, 48, 3, n=M) - 0.5
    patchy = fbm(rng, 7, 4, n=M)
    road = smoothstep(3.0 + 1.3, 3.0 - 0.9, C["d_road"] + 2.0 * edge)
    road = np.maximum(road, smoothstep(15.0, 11.0, C["d_c"] + 4.0 * edge) * 0.95)
    rock = smoothstep(0.38, 0.65, C["slope"])
    floor = z3_floor(np.arange(M)[:, None] + 0.5)
    above = C["h"] - floor
    # Snow on the high ground (above ~60 m over the valley floor), steep faces bare; drifts in the
    # valley (low-frequency noise); scree (rock) along the foot of the slopes.
    snow = smoothstep(52.0, 72.0, above + 24.0 * (patchy - 0.5)) * (1.0 - smoothstep(0.55, 1.0, C["slope"]))
    snow = np.maximum(snow, smoothstep(0.68, 0.80, patchy) * 0.7 * (1 - rock))
    ins = F["inside"]
    ins = 0.25 * (ins[:-1, :-1] + ins[1:, :-1] + ins[:-1, 1:] + ins[1:, 1:])
    rock = np.maximum(rock, smoothstep(0.95, 0.75, ins) * smoothstep(0.2, 0.5, ins) * 0.6)
    rock = rock * (1 - snow)
    tundra = np.clip(1.0 - np.maximum.reduce([road, rock, snow]), 0, 1)
    return np.stack([tundra * (1 - road), road, rock * (1 - road), snow * (1 - road)], axis=-1)


# ---------------------------------------------------------------------------------------------
# Placement

def have(name):
    return name in gt.PROP_META


def monster_circles(zone, margin=2.5):
    """Keep-out discs round the zone's monster spawn circles (gamedata/monsters.toml): nothing is
    placed where a monster wanders (the content tests require every wander circle walkable)."""
    import tomllib
    with open(os.path.join(vc.REPO, "gamedata", "monsters.toml"), "rb") as f:
        data = tomllib.load(f)
    out = [((s["x"], s["z"]), s["radius"] + margin) for s in data.get("spawn", []) if s["zone"] == zone]
    # M15: hunting slots - their area (farthest home + wander; the ring, or the explicit homes) stays free.
    for s in data.get("slot", []):
        if s["zone"] != zone:
            continue
        far = max((math.hypot(dx, dz) for dx, dz in s["homes"]), default=0.0) if "homes" in s else \
            (s.get("ring", 0.0) if s["count"] > 1 else 0.0)
        out.append(((s["x"], s["z"]), far + s.get("wander", 1.0) + margin))
    return out


class ZPlacer(gt.Placer):
    """gt.Placer with this zone's markers and keep-out areas."""

    def __init__(self, F, C, markers, keep_out, keep_paths=()):
        super().__init__(F, C)
        self.markers = markers
        self.keep_out = keep_out        # [((x, z), radius)]: nothing at all (markers, plaza centre)
        self.keep_paths = keep_paths    # [((x0, z0), (x1, z1), half)]: no blocking props
        self.fields = ()                # [(x, z, along, across, yaw)]: only fences

    def free(self, name, x, z, scale, clearance=0.6, road_clear=1.0, spawn_clear=4.0, stretch=(1, 1, 1),
             blocks=True):
        if not (MARGIN + 1 < x < SIZE - MARGIN - 1 and MARGIN + 1 < z < SIZE - MARGIN - 1):
            return False
        r = gt.prop_radius(name, scale, stretch)
        for road in self.F["roads"]:
            if float(gt.distance_to_polyline(np.array([x]), np.array([z]), road)[0]) < r + gt.ROAD_HALF + road_clear:
                return False
        for (_, sx, sz, _) in self.markers:
            if math.hypot(x - sx, z - sz) < r + spawn_clear:
                return False
        for (c, rad) in self.keep_out:
            if math.hypot(x - c[0], z - c[1]) < r + rad:
                return False
        if name != "fence":  # the fields' furrows stay clear (their fences stand on the edges)
            for (fx, fz, a, b, yaw) in self.fields:
                u = (x - fx) * math.cos(yaw) - (z - fz) * math.sin(yaw)
                v = (x - fx) * math.sin(yaw) + (z - fz) * math.cos(yaw)
                pad = 0.0 if name == "aydor_wall" else r
                if abs(u) < a / 2 + pad and abs(v) < b / 2 + pad:
                    return False
        if blocks:
            for (a, b, half) in self.keep_paths:
                if float(gt.seg_distance(np.array([x]), np.array([z]), a, b)[0]) < r + half:
                    return False
        for o in self.objects:
            if math.hypot(x - o["x"], z - o["z"]) < r + gt.prop_radius(o["mesh"], o["scale"], o["stretch"]) + clearance:
                return False
        return True


def ring_run(P, mesh, cx, cz, radius, a0, a1, section, gaps=(), tint=(255, 255, 255), facing_in=True, **kw):
    """Wall / palisade sections (length `section` along local X) on an arc around (cx, cz) from a0 to
    a1 (degrees), skipping arcs in `gaps` ((a, half-width deg), ...)."""
    if not have(mesh):
        return 0
    n = max(1, int(round(math.radians(a1 - a0) * radius / section)))
    placed = 0
    for k in range(n):
        a = math.radians(a0 + (a1 - a0) * (k + 0.5) / n)
        deg = math.degrees(a) % 360.0
        if any(abs((deg - g + 180.0) % 360.0 - 180.0) < hw for g, hw in gaps):
            continue
        x, z = cx + radius * math.cos(a), cz + radius * math.sin(a)
        yaw = gt.facing(x, z, cx, cz) if facing_in else gt.facing(cx, cz, x, z) - math.pi
        if P.add(mesh, x, z, yaw, tint=tint, clearance=-0.8, **kw):
            placed += 1
    return placed


def fence_run(P, F, ax, az, bx, bz, gaps=(), tint=(255, 255, 255), mesh="fence", length=4.0, max_step=0.45):
    """As gen_terrain's fence runs: sections fitted to the run, skipped where the ends differ in height."""
    L = math.hypot(bx - ax, bz - az)
    n = max(1, int(round(L / length)))
    yaw = math.atan2(bx - ax, bz - az) - math.pi / 2
    for s in range(n):
        if s in gaps:
            continue
        t0, t1 = s / n, (s + 1) / n
        x0, z0 = ax + (bx - ax) * t0, az + (bz - az) * t0
        x1, z1 = ax + (bx - ax) * t1, az + (bz - az) * t1
        if abs(gt.bilinear(F["h"], x0, z0) - gt.bilinear(F["h"], x1, z1)) > max_step:
            continue
        P.add(mesh, 0.5 * (x0 + x1), 0.5 * (z0 + z1), yaw, stretch=(L / n / length, 1.0, 1.0), tint=tint,
              clearance=-0.6, road_clear=0.4)


def lamps_along(P, F, mesh, centre, r0, r1, every, tint=(255, 255, 255), offset=1.3):
    if not have(mesh):
        return
    for road in F["roads"]:
        acc, side = 0.0, 1
        for i in range(1, len(road)):
            p0, p1 = road[i - 1], road[i]
            seg = np.linalg.norm(p1 - p0)
            acc += seg
            if acc < every:
                continue
            acc = 0.0
            dc = float(np.hypot(*(p1 - np.array(centre))))
            if dc > r1 or dc < r0:
                continue
            d = (p1 - p0) / seg
            q = p1 + np.array([d[1], -d[0]]) * side * (gt.ROAD_HALF + offset)
            if P.add(mesh, q[0], q[1], gt.facing(q[0], q[1], p1[0], p1[1]), tint=tint,
                     road_clear=-gt.ROAD_HALF + 0.6, clearance=0.5):
                side = -side


def scatter(P, rng, kinds, count, area, tries=4000, slope_max=0.5, sizes=(0.78, 1.0, 1.18), tint=None,
            keep_from=None, **kw):
    """`count` placements of `kinds` ((mesh, weight), ...) in area(x, z) -> bool."""
    placed = 0
    for _ in range(tries):
        if placed >= count:
            break
        x, z = rng.uniform(MARGIN + 4, SIZE - MARGIN - 4), rng.uniform(MARGIN + 4, SIZE - MARGIN - 4)
        r = rng.uniform()
        acc, kind = 0.0, kinds[-1][0]
        for k, wgt in kinds:
            acc += wgt
            if r < acc:
                kind = k
                break
        s = sizes[int(rng.integers(0, len(sizes)))] * rng.uniform(0.94, 1.06)
        yaw = rng.uniform(0, 6.283)
        if not area(x, z) or P.slope_at(x, z) > slope_max:
            continue
        t = tint(kind, rng) if tint else (255, 255, 255)
        if P.add(kind, x, z, yaw, scale=s, tint=t, **kw):
            placed += 1
    return placed


# M14 "Pazar" (docs/SHOP_ART.md §2): each homeland town's potion stall and smithy, facing the race spawn,
# on the walkable ring between the plaza and the houses. The merchants stand beside them (their spots in
# gamedata/npcs.toml: ~4 m right of the prop's centre, 0.6 m forward, the prop's yaw; the server needs 1 m
# of walkable ground round a merchant, which the stall's own footprint takes behind its counter).
# Placed last: nothing after them moves.
SHOP_STALLS = {
    "zone2": (("aydor_potion_stall", 234.0, 281.0), ("aydor_smithy", 258.0, 273.0)),
    "zone3": (("kargan_potion_stall", 240.5, 336.0), ("kargan_smithy", 276.5, 346.0)),
}


def stand_point(o):
    """World (x, z) of a stall's merchant `stand` (shop_props_meta.json, the prop's own frame)."""
    lx, lz = gt.PROP_META[o["mesh"]]["stand"]
    c, s = math.cos(o["yaw"]), math.sin(o["yaw"])
    return o["x"] + lx * c + lz * s, o["z"] - lx * s + lz * c


def check_merchants(walk, objects):
    """fix1: each merchant stands at its stall's `stand` (behind the counter, inside the stall's blocked
    footprint); the server's spot rule (shared/game/Npcs.cpp checkNpcSpot) wants room for a customer:
    one of 16 points 2.5 m round the spot walkable with 0.5 m all round it. Prints the spots for
    gamedata/npcs.toml."""
    W = lambda x, z: bool(walk[int(z), int(x)])  # noqa: E731
    for o in objects:
        if "stand" not in gt.PROP_META.get(o["mesh"], {}):
            continue
        x, z = stand_point(o)
        ok = any(W(cx, cz) and all(W(cx + 0.5 * math.sin(b * math.pi / 4), cz + 0.5 * math.cos(b * math.pi / 4))
                                   for b in range(8))
                 for a in range(16) for cx, cz in [(x + 2.5 * math.sin(a * math.pi / 8), z + 2.5 * math.cos(a * math.pi / 8))])
        print(f"MERCHANT {o['mesh']}: x = {x:.2f} z = {z:.2f} yaw = {o['yaw']:.4f}")
        if not ok:
            raise SystemExit(f"{o['mesh']}: no room for a customer 2.5 m round its stand ({x:.2f}, {z:.2f})")


def check_road_paths(walk, F, Z, objects, gates):
    """fix1: the roads stay open - every centre-line point outside the plaza and away from the gate, and
    1.5 m to either side of it, is walkable (a brazier stood on the Demirağız road)."""
    cx, cz = Z["center"]
    bad = []
    for road in F["roads"]:
        for k in range(1, len(road)):
            p, d = road[k], road[k] - road[k - 1]
            d = d / max(float(np.linalg.norm(d)), 1e-6)
            if math.hypot(p[0] - cx, p[1] - cz) < 18.0:
                continue
            if any(math.hypot(p[0] - g["gate"][1], p[1] - g["gate"][2]) < 6.0 for g in gates):
                continue
            for off in (-1.5, 0.0, 1.5):
                x, z = p[0] + d[1] * off, p[1] - d[0] * off
                if not walk[int(z), int(x)]:
                    near = min(objects, key=lambda o: math.hypot(o["x"] - x, o["z"] - z))
                    bad.append(f"({x:.1f}, {z:.1f}) near {near['mesh']} ({near['x']:.1f}, {near['z']:.1f})")
    if bad:
        raise SystemExit("roads blocked at " + "; ".join(bad[:12]))
    print(f"ROADS {Z['id']}: open")


def place_shop_stalls(P, zone, markers):
    spawn = next(m for m in markers if m[0].startswith("spawn_"))
    for (name, x, z) in SHOP_STALLS[zone]:
        if not have(name):
            continue
        yaw = round(gt.facing(x, z, spawn[1], spawn[2]), 4)
        if not P.add(name, x, z, yaw, clearance=0.8, road_clear=0.6):
            raise SystemExit(f"{name}: placement at ({x}, {z}) is not free")
        print(f"STALL {name} x={x:.2f} z={z:.2f} yaw={yaw:.4f}")


def place_zone2(F, C, markers, gates):
    keep = [((m[1], m[2]), 5.0) for m in markers] + [(Z2_CENTER, 9.0)] + monster_circles(2)
    paths = [(g["gate"][1:3], g["arrive"][1:3], 3.0) for g in gates]
    P = ZPlacer(F, C, markers, keep, paths)
    P.fields = Z2_FIELDS
    rng = np.random.default_rng(7311)
    cx, cz = Z2_CENTER
    # Tanyeri Burcu: the keep tower on the terrace north-east of the plaza, its door to the plaza.
    tx, tz = cx + 21.0, cz + 15.0
    if have("aydor_tower"):
        if not P.add("aydor_tower", tx, tz, gt.facing(tx, tz, cx, cz), road_clear=2.0, clearance=2.0):
            raise SystemExit("aydor_tower: placement is not free")
    else:
        P.add("watchtower", tx, tz, gt.facing(tx, tz, cx, cz), road_clear=2.0, clearance=2.0)
    # Houses on two rings around the plaza, doors to it (gaps for the three roads: 90 north, 270
    # south, 0 / 180 east-west).
    house = "aydor_house" if have("aydor_house") else "cottage"
    slots = [(20.0, 31.0), (152.0, 30.0), (128.0, 34.0), (202.0, 31.0), (226.0, 33.0), (248.0, 34.0),
             (292.0, 33.0), (316.0, 31.0), (338.0, 33.0), (110.0, 40.0), (62.0, 40.0), (160.0, 44.0), (196.0, 45.0),
             (300.0, 46.0), (238.0, 46.0)]
    tints = [(255, 255, 255), (246, 240, 228), (236, 232, 226)]
    for k, (ang, rr) in enumerate(slots):
        a = math.radians(ang)
        x, z = cx + rr * math.cos(a), cz + rr * math.sin(a)
        yaw = gt.facing(x, z, cx, cz) + rng.uniform(-0.1, 0.1)
        for extra in (0.0, 3.0, 6.0):
            xx, zz = cx + (rr + extra) * math.cos(a), cz + (rr + extra) * math.sin(a)
            if P.add(house, xx, zz, yaw, tint=tints[k % 3], road_clear=2.5, clearance=2.5):
                break
    # White walls: an arc of curtain wall around the town (gaps where the roads leave), the keep
    # tower stands inside it.
    gaps = ((90.0, 9.0), (270.0, 9.0), (0.0, 9.0), (180.0, 9.0), (97.0, 9.0), (266.0, 8.0), (357.0, 8.0))
    ring_run(P, "aydor_wall", cx, cz, 58.0, 0.0, 360.0, 6.0, gaps=gaps, road_clear=1.0, facing_in=True)
    # Banners: around the plaza and either side of the road gaps.
    if have("aydor_banner"):
        for ang in (35.0, 145.0, 215.0, 325.0):
            a = math.radians(ang)
            x, z = cx + 15.5 * math.cos(a), cz + 15.5 * math.sin(a)
            P.add("aydor_banner", x, z, gt.facing(x, z, cx, cz), clearance=0.4, road_clear=0.6, spawn_clear=3.0)
        for road in F["roads"]:
            for i in range(1, len(road)):
                p = road[i]
                if abs(float(np.hypot(*(p - np.array(Z2_CENTER)))) - 66.0) < 1.0:
                    d = road[i] - road[i - 1]
                    d /= np.linalg.norm(d)
                    for side in (1.0, -1.0):
                        q = p + np.array([d[1], -d[0]]) * side * (gt.ROAD_HALF + 1.4)
                        P.add("aydor_banner", q[0], q[1], gt.facing(q[0], q[1], p[0], p[1]), clearance=0.4,
                              road_clear=-gt.ROAD_HALF + 0.8)
                    break
    # The well and two market stalls in the plaza, goods by the stalls and some doors.
    P.add("well", cx - 8.0, cz - 9.0, 0.3, clearance=1.0)
    for ang in (200.0, 330.0):
        a = math.radians(ang)
        x, z = cx + 19.0 * math.cos(a), cz + 19.0 * math.sin(a)
        if P.add("market_stall", x, z, gt.facing(x, z, cx, cz), clearance=1.0):
            b = a + 0.12
            P.add("goods", cx + 23.0 * math.cos(b), cz + 23.0 * math.sin(b), rng.uniform(0, 6.28), clearance=0.3)
    houses = [o for o in P.objects if o["mesh"] in HOUSES]
    for k, h in enumerate(houses):
        if k % 2:
            continue
        fwd = (math.sin(h["yaw"]), math.cos(h["yaw"]))
        right = (math.cos(h["yaw"]), -math.sin(h["yaw"]))
        d = gt.PROP_META[h["mesh"]]["bounds_max"][2] + 1.5
        w = gt.PROP_META[h["mesh"]]["bounds_max"][0] - 0.5
        P.add("goods", h["x"] + fwd[0] * d + right[0] * w, h["z"] + fwd[1] * d + right[1] * w,
              h["yaw"] + rng.uniform(-0.5, 0.5), scale=rng.uniform(0.85, 1.0), clearance=0.4, road_clear=0.8)
    # Fields: fenced plots (gap on the side towards the town).
    for (fx, fz, a, b, yaw) in Z2_FIELDS:
        c, s = math.cos(yaw), math.sin(yaw)
        corner = lambda u, v: (fx + u * c + v * s, fz - u * s + v * c)  # noqa: E731
        p = [corner(-a / 2, -b / 2), corner(a / 2, -b / 2), corner(a / 2, b / 2), corner(-a / 2, b / 2)]
        for k in range(4):
            fence_run(P, F, *p[k], *p[(k + 1) % 4], gaps=(1,) if k in (0, 2) else (), tint=(232, 222, 205))
    # Lamp posts along the roads near the town; signposts at the crossroads and by the cut.
    lamps_along(P, F, "lamp_post", Z2_CENTER, 20.0, 110.0, 20.0)
    for (x, z, ttx, ttz) in ((cx + 6.4, cz - 7.0, cx, cz - 20.0), (262.0 + 6.5, 412.0, 259.0, 430.0),
                             (247.0 + 6.0, 150.0, 241.0, 130.0)):
        P.add("signpost", x, z, gt.facing(x, z, ttx, ttz), road_clear=0.2, spawn_clear=3.0, clearance=0.4)
    # Rows of pines along the roads outside the walls (the plain's cypress lines), oaks by the fields.
    for road in F["roads"]:
        for i in range(0, len(road), 6):
            p = road[i]
            dc = float(np.hypot(*(p - np.array(Z2_CENTER))))
            if not 72.0 < dc < 150.0 or i == 0:
                continue
            d = road[min(i + 1, len(road) - 1)] - road[i - 1]
            d /= np.linalg.norm(d)
            for side in (1.0, -1.0):
                q = p + np.array([d[1], -d[0]]) * side * (gt.ROAD_HALF + 4.5)
                P.add("tree_pine", q[0], q[1], rng.uniform(0, 6.28), scale=rng.uniform(0.72, 0.86),
                      stretch=(0.93, 1.0, 0.93), extra_sink=0.05, clearance=0.8, road_clear=2.5)
    in_plain = lambda x, z: math.hypot(x - cx, z - cz) > 70.0 and gt.bilinear(F["h"], x, z) < 26.0  # noqa: E731

    def scorched(x, z):
        return any(math.hypot(x - sx, z - sz) < r * 0.85 for sx, sz, r in Z2_SCORCH)
    # Scorched fields: burnt trees (dark), boulders, broken wall stubs.
    scatter(P, rng, (("tree_oak", 0.75), ("tree_pine", 0.25)), 70, scorched,
            tint=lambda k, r: (int(r.integers(58, 76)),) * 3, extra_sink=0.05, clearance=2.0, road_clear=3.0)
    if have("aydor_wall"):
        scatter(P, rng, (("aydor_wall", 1.0),), 7, scorched, sizes=(1.0,), tint=lambda k, r: (150, 140, 128),
                clearance=3.0, road_clear=3.0, slope_max=0.15, extra_sink=0.6)
    # Green oaks and pines in the unburnt plain, a few groves on the knolls.
    scatter(P, rng, (("tree_oak", 0.7), ("tree_pine", 0.3)), 130,
            lambda x, z: in_plain(x, z) and not scorched(x, z), extra_sink=0.05, clearance=1.6, road_clear=3.0)
    # Boulders: chalk-pale ones on the escarpment slopes and a few in the plain.
    scatter(P, rng, (("boulder", 1.0),), 80, lambda x, z: math.hypot(x - cx, z - cz) > 70.0 and P.slope_at(x, z) > 0.28,
            slope_max=0.9, sizes=(0.6, 0.9, 1.3), tint=lambda k, r: (236, 230, 216), extra_sink=0.15, clearance=0.8,
            road_clear=2.0)
    scatter(P, rng, (("boulder", 1.0),), 25, in_plain, sizes=(0.55, 0.8, 1.1), tint=lambda k, r: (236, 230, 216),
            extra_sink=0.1, clearance=0.8, road_clear=2.0)
    gt.clear_roadsides(P, F["roads"], Z2_CENTER, 18.0)
    place_shop_stalls(P, "zone2", markers)
    return P.objects


def place_zone3(F, C, markers, gates):
    keep = [((m[1], m[2]), 5.0) for m in markers] + [(Z3_CENTER, 8.0)] + monster_circles(3)
    paths = [(g["gate"][1:3], g["arrive"][1:3], 3.0) for g in gates]
    P = ZPlacer(F, C, markers, keep, paths)
    rng = np.random.default_rng(9421)
    cx, cz = Z3_CENTER
    # Rock-cut doorways at the foot of the Demirağız face, facing south over the town: find the
    # first steep cell north of the terrace along each column.
    if have("kargan_door"):
        for x in (214.0, 236.0, 262.0, 284.0, 300.0):
            z = cz + 10.0
            while z < SIZE - 20 and P.slope_at(x, z) < 0.9:
                z += 0.5
            door_z = z - 0.8  # the rock mass behind the frame runs into the cliff
            P.add("kargan_door", x, door_z, math.pi, clearance=1.0, road_clear=1.0, extra_sink=0.1)
    hut = "kargan_hut" if have("kargan_hut") else "cottage"
    slots = [(200.0, 27.0), (222.0, 31.0), (246.0, 30.0), (294.0, 30.0), (318.0, 28.0), (340.0, 31.0),
             (160.0, 30.0), (20.0, 30.0), (140.0, 38.0), (40.0, 37.0), (205.0, 42.0), (330.0, 42.0)]
    for k, (ang, rr) in enumerate(slots):
        a = math.radians(ang)
        for extra in (0.0, 3.0, 6.0):
            x, z = cx + (rr + extra) * math.cos(a), cz + (rr + extra) * math.sin(a)
            if z > Z3_CLIFF_Z - 14.0:
                continue
            if P.add(hut, x, z, gt.facing(x, z, cx, cz) + rng.uniform(-0.12, 0.12), road_clear=2.5, clearance=2.5,
                     tint=(255, 255, 255) if k % 2 else (232, 230, 228)):
                break
    # Forges by the plaza; braziers round it and along the road into the town.
    if have("kargan_forge"):
        for ang in (150.0, 30.0):
            a = math.radians(ang)
            x, z = cx + 17.0 * math.cos(a), cz + 17.0 * math.sin(a)
            P.add("kargan_forge", x, z, gt.facing(x, z, cx, cz), clearance=1.0, road_clear=1.0)
    if have("kargan_brazier"):
        for ang in (75.0, 105.0, 210.0, 330.0, 255.0, 285.0):
            a = math.radians(ang)
            x, z = cx + 12.5 * math.cos(a), cz + 12.5 * math.sin(a)
            P.add("kargan_brazier", x, z, 0.0, clearance=0.5, road_clear=0.3, spawn_clear=3.0)
        lamps_along(P, F, "kargan_brazier", Z3_CENTER, 18.0, 120.0, 22.0, offset=1.6)
    # Iron-bound palisade round the town (open to the cliff and where the road leaves).
    ring_run(P, "kargan_palisade", cx, cz, 50.0, 160.0, 380.0, 4.0, gaps=((270.0, 8.0), (265.0, 8.0)), road_clear=1.0)
    P.add("goods", cx - 9.0, cz + 8.5, 0.4, clearance=0.6)
    P.add("goods", cx + 10.0, cz + 7.0, 2.2, clearance=0.6)
    for (x, z, ttx, ttz) in ((cx + 5.5, cz - 13.0, cx, cz - 30.0), (256.0 + 6.0, 130.0, 256.0, 110.0),
                             (254.0 - 1.0, 268.0, 214.0, 254.0)):
        P.add("signpost", x, z, gt.facing(x, z, ttx, ttz), road_clear=0.2, spawn_clear=3.0, clearance=0.4,
              tint=(170, 165, 160))
    on_floor = lambda x, z: F["inside"][int(z), int(x)] > 0.6 and math.hypot(x - cx, z - cz) > 58.0  # noqa: E731
    # Sparse frosted pines in the valley and basins; many dark boulders; pines thinning upwards.
    scatter(P, rng, (("tree_pine", 1.0),), 120, on_floor, sizes=(0.7, 0.85, 1.0), slope_max=0.45,
            tint=lambda k, r: (int(r.integers(176, 205)), int(r.integers(190, 212)), 214), extra_sink=0.05,
            clearance=1.8, road_clear=3.0)
    scatter(P, rng, (("boulder", 1.0),), 130, lambda x, z: math.hypot(x - cx, z - cz) > 50.0,
            slope_max=0.95, sizes=(0.6, 0.9, 1.3, 1.7), tint=lambda k, r: (118, 116, 118), extra_sink=0.15,
            clearance=0.8, road_clear=2.0)
    gt.clear_roadsides(P, F["roads"], Z3_CENTER, 16.0)
    place_shop_stalls(P, "zone3", markers)
    return P.objects


# ---------------------------------------------------------------------------------------------
# Zones

ZONES = {
    "zone2": dict(
        id=2, fields=fields_zone2, splat=splat_zone2, place=place_zone2, center=Z2_CENTER, seed=7310,
        layers=(("steppe", 3.4), ("chalk_road", 3.0), ("chalk_rock", 7.0), ("scorched", 3.2)),
        spawns=[("spawn_aydor", Z2_CENTER[0] - 6.0, Z2_CENTER[1] - 9.5, math.radians(20.0))],
        gates=[("gate_esik", "arrive_esik", "gate_esik", Z2_GATE[0], Z2_GATE[1], 8.0)],
        gate_target=(256.0, 300.0)),
    "zone3": dict(
        id=3, fields=fields_zone3, splat=splat_zone3, place=place_zone3, center=Z3_CENTER, seed=9420,
        layers=(("ash_tundra", 3.0), ("gravel", 3.0), ("basalt", 7.0), ("snow", 4.0)),
        spawns=[("spawn_kargan", Z3_CENTER[0] + 5.0, Z3_CENTER[1] - 9.0, math.radians(200.0))],
        gates=[("gate_esik", "arrive_esik", "gate_esik", Z3_GATE[0], Z3_GATE[1], 8.0)],
        gate_target=(256.0, 300.0)),
}


def build_maps(F, C, objects, splat_fn, seed):
    walk = np.ones((M, M), bool)
    walk &= C["slope"] <= gt.MAX_SLOPE
    walk &= C["e"] > MARGIN
    for o in objects:
        if o.get("mask", o["blocks"]):
            for (j, i) in gt.footprint_cells(o):
                walk[j, i] = False
    w = splat_fn(F, C)
    w /= np.maximum(w.sum(axis=-1, keepdims=True), 1e-6)
    splat = np.round(w * 255).astype(np.int32)
    splat[..., 0] += 255 - splat.sum(axis=-1)
    return walk, np.clip(splat, 0, 255).astype(np.uint8)


def write_outputs(name, Z, F, objects, walk, splat, markers):
    out = vc.ensure_dir(os.path.join(vc.SRC, "terrain", name))
    h = F["h"]
    hmin, hmax = math.floor(h.min()) - 1.0, math.ceil(h.max()) + 1.0
    vc.write_png(os.path.join(out, f"{name}_height.png"), np.round((h - hmin) / (hmax - hmin) * 65535.0).astype(np.uint16))
    vc.write_png(os.path.join(out, f"{name}_splat.png"), splat)
    vc.write_png(os.path.join(out, f"{name}_walk.png"), np.where(walk, 255, 0).astype(np.uint8))
    lines = [
        f"# Zone {Z['id']} terrain description for assetc. Generated by tools/blender/gen_zones.py; do not edit.",
        "# Coordinates are engine space (x right, z forward, metres); y comes from the heightmap.",
        "[terrain]", f'heightmap = "{name}_height.png"', f"world_size = {SIZE:.1f}", f"height_min = {hmin:.1f}",
        f"height_max = {hmax:.1f}", f'splat = "{name}_splat.png"', f'walk_mask = "{name}_walk.png"', ""]
    for tex, tile in Z["layers"]:
        lines += ["[[layer]]", f'texture = "textures/terrain/{tex}.png"', f'normal = "textures/terrain/{tex}_n.png"',
                  f'orm = "textures/terrain/{tex}_orm.png"', f"tile_size = {tile:.1f}", ""]
    for (mname, x, z, yaw) in markers:
        lines += ["[[spawn]]", f'name = "{mname}"', f"x = {x:.2f}", f"z = {z:.2f}", f"yaw = {yaw:.4f}", ""]
    for o in objects:
        lines += ["[[object]]", f'mesh = "props/{o["mesh"]}.vmesh"', f"x = {o['x']:.3f}", f"z = {o['z']:.3f}",
                  f"yaw = {o['yaw']:.4f}", f"scale = {o['scale']:.3f}", f"y_offset = {o['y_offset']:.3f}"]
        if tuple(o["stretch"]) != (1.0, 1.0, 1.0):
            lines += ["stretch = [{:.3f}, {:.3f}, {:.3f}]".format(*o["stretch"])]
        if tuple(o["tint"]) != (255, 255, 255):
            lines += ["tint = [{}, {}, {}]".format(*o["tint"])]
        lines += [f"blocks = {'true' if o['blocks'] else 'false'}", ""]
    with open(os.path.join(out, f"{name}.terrain.toml"), "w", newline="\n") as f:
        f.write("\n".join(lines))
    return hmin, hmax


def walkmask_preview(name, C, walk, objects, markers):
    h = C["h"]
    shade = (h - h.min()) / (h.max() - h.min())
    gz, gx = np.gradient(h)
    light = np.clip(0.75 + 0.6 * (-gx * 0.6 - gz * 0.4), 0.3, 1.2)
    img = np.stack([shade * 0.5 + 0.35] * 3, axis=-1) * light[..., None]
    img[~walk] = img[~walk] * 0.45 + np.array([0.75, 0.12, 0.1]) * 0.55
    img = np.clip(img * 255, 0, 255).astype(np.uint8)
    for o in objects:
        img[int(o["z"]), int(o["x"])] = (20, 20, 20)
    for (mname, x, z, _) in markers:
        col = (60, 140, 255) if mname.startswith("spawn") else (255, 200, 40) if mname.startswith("gate") else (80, 230, 90)
        img[int(z) - 2:int(z) + 3, int(x) - 2:int(x) + 3] = col
    vc.write_png(os.path.join(vc.PREVIEWS, "terrain", f"{name}_walkmask.png"), img)


def check_spawns(walk, F, spawns, objects):
    for (name, x, z, _) in spawns:
        for dx in (-3.0, -1.5, 0.0, 1.5, 3.0):
            for dz in (-3.0, -1.5, 0.0, 1.5, 3.0):
                if not walk[int(z + dz), int(x + dx)]:
                    raise SystemExit(f"{name}: ({x + dx:.1f}, {z + dz:.1f}) is blocked")
        walls = [o for o in objects if o["mesh"] in HOUSES + ("aydor_tower", "aydor_wall", "kargan_door", "watchtower")]
        wall = min((float(gt.footprint_distance(np.array([x]), np.array([z]), o)[0]) for o in walls), default=99.0)
        print(f"SPAWN {name} x={x:.2f} z={z:.2f} y={gt.bilinear(F['h'], x, z):.2f}, nearest wall {wall:.1f} m")
        if wall < 8.0:
            raise SystemExit(f"{name}: a wall {wall:.1f} m away (< 8)")


def build_zone(name, preview):
    Z = ZONES[name]
    F = Z["fields"]()
    C = gt.cell_fields(F)
    gates = gt.gate_layout(F, Z["gates"], Z["gate_target"])
    for g in gates:  # the gates of the homelands stand on their road: the arrival point follows it
        g["arrive"] = arrival_on_road(F, g)
    markers = list(Z["spawns"]) + [m for g in gates for m in (g["gate"], g["arrive"])]
    objects = Z["place"](F, C, markers, gates)
    P = gt.Placer(F, C)
    P.objects = gt.clear_for_gates(objects, gates)
    gt.add_gates(P, gates)
    objects = P.objects
    for o in objects:
        for k, nd in (("x", 3), ("z", 3), ("yaw", 4), ("scale", 3), ("y_offset", 3)):
            o[k] = round(o[k], nd)
        o["stretch"] = tuple(round(s, 3) for s in o["stretch"])
    walk, splat = build_maps(F, C, objects, Z["splat"], Z["seed"])
    walkmask_preview(name, C, walk, objects, markers)
    check_spawns(walk, F, Z["spawns"], objects)
    gt.check_gates(walk, gates)
    check_merchants(walk, objects)
    check_road_paths(walk, F, Z, objects, gates)
    hmin, hmax = write_outputs(name, Z, F, objects, walk, splat, markers)
    counts = {}
    for o in objects:
        counts[o["mesh"]] = counts.get(o["mesh"], 0) + 1
    print(f"TERRAIN {name}: heights [{F['h'].min():.2f}, {F['h'].max():.2f}] stored [{hmin}, {hmax}], walkable "
          f"{100.0 * walk.mean():.1f}%, objects {len(objects)} {counts}")
    walkmask_preview(name, C, walk, objects, markers)
    if preview:
        blender_preview(name, Z, F, splat, objects, markers)


def arrival_on_road(F, g):
    """The arrival point 8 m from the gate along its road (the homeland gates stand at a road's end),
    facing away from the gate."""
    name, gx, gz, _ = g["gate"]
    aname = g["arrive"][0]
    best = None
    for road in F["roads"]:
        d = np.hypot(road[:, 0] - gx, road[:, 1] - gz)
        if d.min() > 1.5:
            continue
        k = int(np.argmin(np.abs(d - 8.0)))
        best = road[k]
    if best is None:
        raise SystemExit(f"{name}: not at a road's end")
    ax, az = round(float(best[0]), 2), round(float(best[1]), 2)
    yaw = gt.facing(gx, gz, ax, az)
    g["gate"] = (name, gx, gz, yaw)
    return (aname, ax, az, yaw)


def blender_preview(name, Z, F, splat, objects, markers):
    import bpy
    vc.reset_scene()
    R = 2048
    layers = []
    for tex, tile in Z["layers"]:
        img = vc.read_image(os.path.join(vc.SRC, "textures", "terrain", f"{tex}.png"))[..., :3]
        px = int(tile * R / SIZE)
        idx = (np.arange(px) * img.shape[0] / px).astype(int)
        small = img[np.ix_(idx, idx)]
        reps = R // px + 1
        layers.append(np.tile(small, (reps, reps, 1))[:R, :R])
    up = (np.arange(R) * M / R).astype(int)
    w = splat[np.ix_(up, up)].astype(np.float32) / 255.0
    albedo = sum(layers[k] * w[..., k:k + 1] for k in range(4))
    path = os.path.join(vc.PREVIEWS, "terrain", f"_albedo_{name}_tmp.png")
    vc.write_png(path, vc.to_u8(albedo))
    step = 2
    h = F["h"][::step, ::step]
    n = h.shape[0]
    verts = [(-i * step, -j * step, h[j, i]) for j in range(n) for i in range(n)]
    faces = [(j * n + i, j * n + i + 1, j * n + i + n + 1, j * n + i + n) for j in range(n - 1) for i in range(n - 1)]
    me = bpy.data.meshes.new("terrain")
    me.from_pydata(verts, [], faces)
    uv = me.uv_layers.new(name="UVMap")
    vidx = np.zeros(len(me.loops), np.int64)
    me.loops.foreach_get("vertex_index", vidx)
    coords = np.zeros((len(me.loops), 2), np.float32)
    coords[:, 0] = (vidx % n) / (n - 1)
    coords[:, 1] = 1.0 - (vidx // n) / (n - 1)
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
    protos = {}
    for pname in sorted({o["mesh"] for o in objects}):
        before = set(bpy.data.objects)
        bpy.ops.import_scene.gltf(filepath=os.path.join(vc.SRC, "props", f"{pname}.glb"))
        new = [ob for ob in bpy.data.objects if ob not in before and ob.type == 'MESH']
        keep = [ob for ob in new if not ob.name.startswith("lod1")]
        for ob in new:
            if ob not in keep:
                bpy.data.objects.remove(ob)
        protos[pname] = keep[0]
        keep[0].hide_render = True
        keep[0].location = (0, 0, -1000)
    tinted = {}
    for o in objects:
        ob = protos[o["mesh"]].copy()
        if tuple(o["tint"]) != (255, 255, 255):
            key = (o["mesh"], tuple(o["tint"]))
            if key not in tinted:
                m2 = ob.data.materials[0].copy()
                bs = next(nd for nd in m2.node_tree.nodes if nd.type == 'BSDF_PRINCIPLED')
                mix = m2.node_tree.nodes.new("ShaderNodeMix")
                mix.data_type = 'RGBA'
                mix.blend_type = 'MULTIPLY'
                mix.inputs["Factor"].default_value = 1.0
                src = bs.inputs["Base Color"].links[0].from_socket
                m2.node_tree.links.new(src, mix.inputs["A"])
                mix.inputs["B"].default_value = tuple(vc.hex_linear("#%02x%02x%02x" % o["tint"])) + (1.0,)
                m2.node_tree.links.new(mix.outputs["Result"], bs.inputs["Base Color"])
                d2 = ob.data.copy()
                d2.materials[0] = m2
                tinted[key] = d2
            ob.data = tinted[key]
        vc.link(ob)
        ob.hide_render = False
        ob.location = (-o["x"], -o["z"], gt.bilinear(F["h"], o["x"], o["z"]) + o["y_offset"])
        ob.rotation_mode = 'XYZ'
        ob.rotation_euler = (0, 0, -o["yaw"])
        ob.scale = (o["scale"] * o["stretch"][0], o["scale"] * o["stretch"][2], o["scale"] * o["stretch"][1])
    for (mname, x, z, _) in markers:
        bpy.ops.mesh.primitive_cylinder_add(radius=0.5, depth=4.0, location=(-x, -z, gt.bilinear(F["h"], x, z) + 2.0))
        col = "#3c8cff" if mname.startswith("spawn") else "#ffc828" if mname.startswith("gate") else "#50e65a"
        bpy.context.active_object.data.materials.append(vc.material(f"marker_{mname}", col))
    sky = "#b9c6cf" if name == "zone2" else "#9aa6b2"
    cam = vc.setup_preview((1024, 1024), background=sky, sun_dir=(-0.4, 0.5, 1.0), strength=3.4)
    bpy.context.scene.world.node_tree.nodes["Background"].inputs["Strength"].default_value = 1.1
    vc.aim_camera(cam, (-256, -256, 600), (-256, -256.001, 0), ortho_scale=512)
    cam.rotation_euler = (0, 0, math.pi)
    vc.render_to(os.path.join(vc.PREVIEWS, "terrain", f"{name}_topdown.png"))
    bpy.context.scene.render.resolution_x, bpy.context.scene.render.resolution_y = 1280, 720
    cx, cz = Z["center"]
    hc = gt.bilinear(F["h"], cx, cz)
    # Oblique: from the south over the hunting grounds towards the town (zone 3: up the valley).
    vc.aim_camera(cam, (-cx - 40, -cz + 230, hc + 120), (-cx, -cz, hc), lens=28)
    vc.render_to(os.path.join(vc.PREVIEWS, "terrain", f"{name}_oblique.png"))
    vc.aim_camera(cam, (-cx + 30, -cz + 62, hc + 30), (-cx, -cz - 6, hc + 2), lens=30)
    vc.render_to(os.path.join(vc.PREVIEWS, "terrain", f"{name}_town.png"))
    os.remove(path)


def main():
    args = vc.script_args()
    names = list(ZONES)
    if "--only" in args:
        names = args[args.index("--only") + 1].split(",")
    for name in names:
        build_zone(name, "--no-preview" not in args)


if __name__ == "__main__":
    main()
