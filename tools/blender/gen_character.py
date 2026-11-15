"""Rigged low-poly humanoid generator for both races, with idle/walk/run clips.

Usage (headless):
  blender --background --factory-startup --python tools/blender/gen_character.py -- --race aydor [--no-preview]

Outputs:
  assets_src/characters/<race>.glb          skinned mesh: body + hair_0..3 + face_0..3, 22-bone skeleton,
                                            animations idle / walk / run (30 fps, in place)
  assets_src/characters/<race>.anims.toml   clip settings for assetc (loop, move_speed)
  assets_src/previews/characters/<race>_*.png

Blender space: Z up, the character faces -Y, its left side is +X, feet on Z = 0.
"""

import math
import os
import sys

import bpy
import numpy as np
from mathutils import Euler, Quaternion, Vector

sys.dont_write_bytecode = True  # keep tools/blender free of __pycache__
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import vo_common as vc  # noqa: E402

FPS = 30
BONES = ["root", "pelvis", "spine", "chest", "neck", "head",
         "clavicle_l", "upperarm_l", "forearm_l", "hand_l",
         "clavicle_r", "upperarm_r", "forearm_r", "hand_r",
         "thigh_l", "shin_l", "foot_l", "toe_l",
         "thigh_r", "shin_r", "foot_r", "toe_r"]

# ---------------------------------------------------------------------------------------------
# Race definitions. All lengths in metres. "f" = forward (= -Y in Blender).

RACES = {
    "aydor": dict(
        seed=1101,
        height=1.75,
        pelvis_z=0.97, spine_z=1.08, chest_z=1.24, neck_z=1.47, head_z=1.555,
        hip=(0.092, 0.93), knee_z=0.505, knee_f=0.012, ankle_z=0.085,
        heel_f=-0.055, ball_f=0.115, toe_f=0.185, foot_w=0.048,
        clav_in=0.025, shoulder=(0.172, 1.425), arm_angle=40.0,
        upperarm=0.285, forearm=0.255, hand=0.165, elbow_bend=8.0,
        head_c=(0.0, 0.004, 1.652), head_r=(0.094, 0.108, 0.118),
        torso=[  # z, rx (side), ry (front-back), f offset
            (0.84, 0.07, 0.06, 0.0), (0.885, 0.135, 0.092, 0.0), (0.95, 0.156, 0.100, 0.0),
            (1.02, 0.150, 0.097, 0.0), (1.10, 0.138, 0.093, 0.004), (1.20, 0.153, 0.100, 0.008),
            (1.30, 0.170, 0.108, 0.010), (1.38, 0.176, 0.104, 0.006), (1.435, 0.162, 0.090, 0.0),
            (1.475, 0.080, 0.068, 0.0), (1.50, 0.054, 0.054, 0.004), (1.60, 0.050, 0.050, 0.008)],
        leg=[  # s along hip(0)->knee(1)->ankle(2), radius side, radius front
            (-0.08, 0.080, 0.080), (0.0, 0.085, 0.086), (0.30, 0.077, 0.079), (0.72, 0.062, 0.064),
            (0.92, 0.056, 0.058), (1.0, 0.055, 0.057), (1.10, 0.056, 0.060), (1.38, 0.058, 0.064),
            (1.72, 0.046, 0.050), (1.92, 0.042, 0.046), (2.08, 0.040, 0.044)],
        arm=[  # s along shoulder(0)->elbow(1)->wrist(2), radius side, radius front
            (-0.15, 0.052, 0.054), (0.0, 0.055, 0.057), (0.40, 0.050, 0.051), (0.88, 0.043, 0.044),
            (1.0, 0.041, 0.042), (1.12, 0.042, 0.043), (1.50, 0.040, 0.040), (1.90, 0.033, 0.034),
            (2.02, 0.031, 0.033)],
        walk_frames=24, run_frames=20, idle_frames=60,
    ),
    "kargan": dict(
        seed=2207,
        height=1.95,
        pelvis_z=1.02, spine_z=1.14, chest_z=1.32, neck_z=1.615, head_z=1.70,
        hip=(0.128, 0.975), knee_z=0.545, knee_f=0.016, ankle_z=0.095,
        heel_f=-0.068, ball_f=0.140, toe_f=0.222, foot_w=0.064,
        clav_in=0.04, shoulder=(0.285, 1.565), arm_angle=42.0,
        upperarm=0.330, forearm=0.305, hand=0.195, elbow_bend=10.0,
        head_c=(0.0, 0.012, 1.808), head_r=(0.100, 0.112, 0.122),
        torso=[
            (0.88, 0.09, 0.08, 0.0), (0.93, 0.180, 0.125, 0.0), (1.00, 0.205, 0.140, 0.0),
            (1.10, 0.212, 0.150, 0.012), (1.20, 0.220, 0.152, 0.016), (1.32, 0.248, 0.155, 0.014),
            (1.44, 0.282, 0.152, 0.010), (1.53, 0.300, 0.138, 0.004), (1.585, 0.250, 0.115, 0.0),
            (1.625, 0.120, 0.095, 0.004), (1.66, 0.078, 0.078, 0.010), (1.76, 0.072, 0.072, 0.016)],
        leg=[
            (-0.08, 0.108, 0.108), (0.0, 0.112, 0.114), (0.30, 0.104, 0.108), (0.72, 0.084, 0.088),
            (0.92, 0.076, 0.080), (1.0, 0.075, 0.079), (1.10, 0.078, 0.084), (1.38, 0.082, 0.090),
            (1.72, 0.066, 0.070), (1.92, 0.060, 0.064), (2.08, 0.058, 0.062)],
        arm=[
            (-0.15, 0.080, 0.082), (0.0, 0.084, 0.086), (0.40, 0.078, 0.080), (0.88, 0.066, 0.068),
            (1.0, 0.064, 0.066), (1.12, 0.068, 0.070), (1.45, 0.080, 0.084), (1.82, 0.066, 0.068),
            (2.02, 0.054, 0.058)],
        walk_frames=26, run_frames=21, idle_frames=60,
    ),
}

WALK_SPEED = 2.0
RUN_SPEED = 5.5

# Gait shapes (fractions of a cycle; lengths in metres; pitches in radians). Stride = speed x cycle
# time; the planted foot moves backwards by stance x stride at exactly the clip's move speed.
GAITS = {
    "walk": dict(stance=0.60, lift=0.085, heel_pitch=0.28, toe_pitch=0.45, heel_frac=0.10, toe_frac=0.16,
                 skew=0.9, drop=-0.030, bob=0.014, low=0.0, offset=0.03),
    "run": dict(stance=0.30, lift=0.30, heel_pitch=0.10, toe_pitch=0.60, heel_frac=0.04, toe_frac=0.12,
                skew=0.6, drop=-0.060, bob=0.030, low=0.15, offset=-0.10),
}


def bf(y):
    """Forward coordinate -> Blender Y."""
    return -y


# ---------------------------------------------------------------------------------------------
# Skeleton

def joint_positions(r):
    """Joint world positions (Blender space) for the left side; right side mirrors X."""
    P = {}
    P["root"] = ((0, 0, 0), (0, 0, 0.12))
    P["pelvis"] = ((0, 0, r["pelvis_z"]), (0, 0, r["spine_z"]))
    P["spine"] = ((0, 0, r["spine_z"]), (0, bf(0.006), r["chest_z"]))
    P["chest"] = ((0, bf(0.006), r["chest_z"]), (0, 0, r["neck_z"]))
    P["neck"] = ((0, 0, r["neck_z"]), (0, bf(0.008), r["head_z"]))
    P["head"] = ((0, bf(0.008), r["head_z"]), (0, bf(0.008), r["height"]))
    sx, sz = r["shoulder"]
    a = math.radians(r["arm_angle"])
    d1 = np.array([math.sin(a), 0.0, -math.cos(a)])
    elbow = np.array([sx, 0.0, sz]) + d1 * r["upperarm"]
    eb = math.radians(r["elbow_bend"])
    d2 = d1 * math.cos(eb) + np.array([0.0, -1.0, 0.0]) * math.sin(eb)
    wrist = elbow + d2 * r["forearm"]
    hand_end = wrist + d2 * r["hand"]
    P["clavicle_l"] = ((r["clav_in"], 0.0, sz + 0.005), (sx, 0.0, sz))
    P["upperarm_l"] = ((sx, 0.0, sz), tuple(elbow))
    P["forearm_l"] = (tuple(elbow), tuple(wrist))
    P["hand_l"] = (tuple(wrist), tuple(hand_end))
    hx, hz = r["hip"]
    P["thigh_l"] = ((hx, 0.0, hz), (hx, bf(r["knee_f"]), r["knee_z"]))
    P["shin_l"] = ((hx, bf(r["knee_f"]), r["knee_z"]), (hx, 0.0, r["ankle_z"]))
    P["foot_l"] = ((hx, 0.0, r["ankle_z"]), (hx, bf(r["ball_f"]), 0.028))
    P["toe_l"] = ((hx, bf(r["ball_f"]), 0.028), (hx, bf(r["toe_f"]), 0.026))
    for name in list(P):
        if name.endswith("_l"):
            h, t = P[name]
            P[name[:-2] + "_r"] = ((-h[0], h[1], h[2]), (-t[0], t[1], t[2]))
    return P


PARENTS = {"root": None, "pelvis": "root", "spine": "pelvis", "chest": "spine", "neck": "chest", "head": "neck"}
for _s in ("l", "r"):
    PARENTS.update({f"clavicle_{_s}": "chest", f"upperarm_{_s}": f"clavicle_{_s}", f"forearm_{_s}": f"upperarm_{_s}",
                    f"hand_{_s}": f"forearm_{_s}", f"thigh_{_s}": "pelvis", f"shin_{_s}": f"thigh_{_s}",
                    f"foot_{_s}": f"shin_{_s}", f"toe_{_s}": f"foot_{_s}"})


def build_armature(r):
    P = joint_positions(r)
    arm = bpy.data.armatures.new("Armature")
    obj = vc.link(bpy.data.objects.new("Armature", arm))
    bpy.context.view_layer.objects.active = obj
    bpy.ops.object.mode_set(mode='EDIT')
    for name in BONES:
        eb = arm.edit_bones.new(name)
        eb.head, eb.tail = Vector(P[name][0]), Vector(P[name][1])
        # Roll: local Z faces forward for vertical/limb bones, up for feet/toes and clavicles get
        # forward too. Then a positive rotation about local X swings a bone forward (flexion).
        if name.startswith(("foot", "toe")):
            eb.align_roll(Vector((0, 0, 1)))
        else:
            eb.align_roll(Vector((0, -1, 0)))
    for name in BONES:
        if PARENTS[name]:
            arm.edit_bones[name].parent = arm.edit_bones[PARENTS[name]]
            arm.edit_bones[name].use_connect = False
    bpy.ops.object.mode_set(mode='OBJECT')
    for pb in obj.pose.bones:
        pb.rotation_mode = 'QUATERNION'
    return obj, P


# ---------------------------------------------------------------------------------------------
# Body mesh helpers

def chain_point(joints, s):
    n = len(joints) - 1
    i = int(min(max(math.floor(s), 0), n - 1))
    t = s - i
    a, b = np.asarray(joints[i], float), np.asarray(joints[i + 1], float)
    return a + (b - a) * t, (b - a) / np.linalg.norm(b - a)


def chain_weights(bones, s, blend=0.16, pre=None):
    """Weights for parameter s along a chain of bones (bone i spans s in [i, i+1])."""
    n = len(bones)
    i = int(min(max(math.floor(s), 0), n - 1))
    t = s - i
    w = {bones[i]: 1.0}
    if t < blend:
        prev = bones[i - 1] if i > 0 else pre
        if prev is not None:
            k = 0.5 * (1.0 - t / blend) if t >= 0 else 0.5 + 0.5 * min(1.0, -t / blend)
            w = {bones[i]: 1.0 - k, prev: k}
    if t > 1.0 - blend and i + 1 < n:
        k = 0.5 * (1.0 - (1.0 - t) / blend)
        w = {bones[i]: 1.0 - k, bones[i + 1]: k}
    return w


def ring_points(center, direction, r_side, r_front, sides, forward=(0, -1, 0), phase=0.0):
    s_ax, f_ax = vc.ring_frame(direction, forward)
    pts = []
    for k in range(sides):
        th = 2 * math.pi * k / sides + phase
        pts.append(np.asarray(center) + s_ax * (r_side * math.cos(th)) + f_ax * (r_front * math.sin(th)))
    return pts


def limb(mb, joints, bones, profile, mats, sides, pre=None, blend=0.16, forward=(0, -1, 0), cap=(True, True)):
    rings = []
    for (s, rs, rf) in profile:
        c, d = chain_point(joints, s)
        rings.append((ring_points(c, d, rs, rf, sides, forward), chain_weights(bones, s, blend, pre)))
    mb.loft(rings, mats, cap_start=cap[0], cap_end=cap[1])


def torso_weight(z, r, x=0.0):
    zs = [r["pelvis_z"], r["spine_z"], r["chest_z"], r["neck_z"], r["head_z"]]
    bones = ["pelvis", "spine", "chest", "neck"]
    if z <= zs[0]:
        return {"pelvis": 1.0}
    if z >= zs[4]:
        return {"neck": 0.5, "head": 0.5}
    for i in range(4):
        if zs[i] <= z <= zs[i + 1]:
            s = i + (z - zs[i]) / (zs[i + 1] - zs[i])
            w = chain_weights(bones, s, 0.22, None)
            break
    # Shoulder flesh follows the clavicles a little.
    sx = r["shoulder"][0]
    if z > r["chest_z"] + 0.08 and abs(x) > 0.55 * sx:
        k = min(1.0, (abs(x) - 0.55 * sx) / (0.45 * sx)) * 0.45
        side = "clavicle_l" if x > 0 else "clavicle_r"
        w = {b: v * (1 - k) for b, v in w.items()}
        w[side] = w.get(side, 0) + k
    return w


def torso(mb, r, mat_fn, sides=14):
    """Torso loft along Z with elliptical sections; per-vertex weights from height (and side)."""
    rings_pts = []
    for (z, rx, ry, fo) in r["torso"]:
        row = []
        for k in range(sides):
            th = 2 * math.pi * k / sides
            x, y = rx * math.cos(th), bf(fo) + ry * math.sin(th)  # sin>0 -> +Y = back
            row.append((x, y, z))
        rings_pts.append(row)
    idx = []
    for row in rings_pts:
        idx.append([mb.add_vertex(p, torso_weight(p[2], r, p[0])) for p in row])
    for i in range(len(idx) - 1):
        for k in range(sides):
            k2 = (k + 1) % sides
            f = (idx[i][k], idx[i][k2], idx[i + 1][k2], idx[i + 1][k])
            cen = np.mean([mb.verts[j] for j in f], axis=0)
            mb.add_face(f, mat_fn(cen))
    mb.add_face(tuple(reversed(idx[0])), mat_fn(np.array([0, 0, r["torso"][0][0]])))
    mb.add_face(tuple(idx[-1]), mat_fn(np.array([0, 0, r["torso"][-1][0]])))


def skirt(mb, r, sections, mat, sides=16, thickness=0.012, side_bias=0.85):
    """Open flared tube with an inner wall (no caps), weighted pelvis + thighs by side so the hem
    follows the legs (keeps thighs from poking through when they swing)."""
    hx = r["hip"][0]

    def weights(p, zfrac):
        k = side_bias * zfrac ** 0.8
        wl = k * min(1.0, max(0.0, 0.5 + p[0] / (0.45 * hx)))
        return {"pelvis": 1.0 - k, "thigh_l": wl, "thigh_r": k - wl}

    z0, z1 = sections[0][0], sections[-1][0]
    outer, inner = [], []
    for (z, rx, ry, fo) in sections:
        zfrac = (z0 - z) / (z0 - z1)
        o, i = [], []
        for k in range(sides):
            th = 2 * math.pi * k / sides
            for lst, s in ((o, 1.0), (i, 1.0 - thickness / max(rx, ry))):
                p = (rx * s * math.cos(th), bf(fo) + ry * s * math.sin(th), z)
                lst.append(mb.add_vertex(p, weights(p, zfrac)))
        outer.append(o)
        inner.append(i)
    n = len(sections)
    for a in range(n - 1):
        for k in range(sides):
            k2 = (k + 1) % sides
            mb.add_face((outer[a][k2], outer[a][k], outer[a + 1][k], outer[a + 1][k2]), mat)  # down the outside
            mb.add_face((inner[a][k], inner[a][k2], inner[a + 1][k2], inner[a + 1][k]), mat)
    for k in range(sides):  # hem
        k2 = (k + 1) % sides
        mb.add_face((outer[-1][k2], outer[-1][k], inner[-1][k], inner[-1][k2]), mat)


def foot(mb, r, side, mats):
    """Boot/shoe: loft from heel to toe with a flat sole."""
    sgn = 1 if side == "l" else -1
    hx = r["hip"][0] * sgn
    w = r["foot_w"]
    az = r["ankle_z"]
    sections = [  # f, half width, height, weights
        (r["heel_f"] - 0.012, w * 0.80, az * 0.95, {f"foot_{side}": 1.0}),
        (r["heel_f"] + 0.01, w * 0.95, az * 1.25, {f"foot_{side}": 1.0}),
        (0.03, w * 1.02, az * 1.15, {f"foot_{side}": 1.0}),
        (r["ball_f"] - 0.02, w * 1.08, az * 0.72, {f"foot_{side}": 0.75, f"toe_{side}": 0.25}),
        (r["ball_f"] + 0.03, w * 1.02, az * 0.55, {f"toe_{side}": 1.0}),
        (r["toe_f"] + 0.008, w * 0.70, az * 0.40, {f"toe_{side}": 1.0}),
    ]
    rings = []
    for (f, hw, h, wts) in sections:
        # Profile (lateral u, up v), counter-clockwise seen from the toe (the loft runs towards -Y),
        # which makes the faces point outwards.
        prof = [(-hw, 0.0), (hw, 0.0), (hw, 0.55 * h), (0.62 * hw, h), (-0.62 * hw, h), (-hw, 0.55 * h)]
        rings.append(([(hx + u, bf(f), v) for (u, v) in prof], wts))
    mb.loft(rings, mats)


def placed_on_head(r, u, v, depth=0.0):
    """Point on the head ellipsoid surface. u = side angle (rad, 0 = front), v = elevation (rad)."""
    cx, cy, cz = r["head_c"]
    rx, ry, rz = r["head_r"]
    x = rx * math.sin(u) * math.cos(v)
    y = -ry * math.cos(u) * math.cos(v)
    z = rz * math.sin(v)
    n = np.array([x / rx ** 2, y / ry ** 2, z / rz ** 2])
    n /= np.linalg.norm(n)
    return np.array([cx + x, cy + y, cz + z]) - n * depth, n


# ---------------------------------------------------------------------------------------------
# Palettes and per-race body parts

PALETTES = {
    "aydor": dict(skin="#e8b893", tunic="#2f5d8a", trim="#d9b35a", shirt="#e9dfc6", trousers="#3b3f4a",
                  boots="#6b4428", belt="#4a2f1c", buckle="#d4a64a", lips="#b9776a", eye_white="#f4efe6"),
    "kargan": dict(skin="#d39a72", vest="#4a2f1d", fur="#cfc3ad", fur_dark="#8f8170", kilt="#3f5a3a",
                   trousers="#4a3a2c", boots="#5a4632", belt="#3a2616", buckle="#a9a9a9", bracer="#5b3b22",
                   lips="#9a5a4c", eye_white="#efe9dc", tattoo="#2f5f86"),
}


def body_aydor(r, P, mats, M):
    mb = vc.MeshBuilder()

    def torso_mat(c):
        z = c[2]
        if z > r["torso"][-4][0] + 0.03:
            return M["skin"]
        return M["tunic"]

    torso(mb, r, torso_mat)
    # Tunic skirt with a gold hem band.
    skirt(mb, r, [(1.04, 0.158, 0.103, 0.004), (0.95, 0.175, 0.118, 0.0), (0.84, 0.192, 0.132, 0.0),
                  (0.785, 0.198, 0.137, 0.0)], M["tunic"])
    skirt(mb, r, [(0.79, 0.200, 0.139, 0.0), (0.745, 0.205, 0.143, 0.0)], M["trim"], thickness=0.014)
    # Belt.
    belt_rings = []
    for z in (1.055, 0.995):
        pts = [(0.162 * math.cos(2 * math.pi * k / 16), 0.107 * math.sin(2 * math.pi * k / 16), z) for k in range(16)]
        belt_rings.append(([np.array(p) for p in reversed(pts)], {"pelvis": 1.0}))
    mb.loft(belt_rings, M["belt"])
    mb.box((0.0, bf(0.108), 1.025), (0.05, 0.012, 0.045), M["buckle"], {"pelvis": 1.0})
    # Collar trim.
    collar = []
    for z, rx, ry in ((1.468, 0.10, 0.082), (1.445, 0.15, 0.096)):
        pts = [(rx * math.cos(2 * math.pi * k / 14), ry * math.sin(2 * math.pi * k / 14), z) for k in range(14)]
        collar.append(([np.array(p) for p in reversed(pts)], {"chest": 0.7, "neck": 0.3}))
    mb.loft(collar, M["trim"], cap_start=False, cap_end=False)
    for side in ("l", "r"):
        arm_j = [P[f"upperarm_{side}"][0], P[f"forearm_{side}"][0], P[f"hand_{side}"][0]]
        arm_mats = [M["tunic"], M["tunic"], M["shirt"], M["shirt"], M["shirt"], M["shirt"], M["skin"], M["skin"]]
        limb(mb, arm_j, [f"upperarm_{side}", f"forearm_{side}"], r["arm"], arm_mats, 10, pre=f"clavicle_{side}")
        # Bracer.
        limb(mb, arm_j, [f"upperarm_{side}", f"forearm_{side}"], [(1.62, 0.044, 0.045), (1.86, 0.039, 0.040)],
             M["boots"], 10)
        hand(mb, r, P, side, M["skin"])
        leg_j = [P[f"thigh_{side}"][0], P[f"shin_{side}"][0], P[f"shin_{side}"][1]]
        leg_mats = [M["trousers"]] * 5 + [M["boots"]] * 5
        limb(mb, leg_j, [f"thigh_{side}", f"shin_{side}"], r["leg"], leg_mats, 10, pre="pelvis")
        # Boot cuff.
        limb(mb, leg_j, [f"thigh_{side}", f"shin_{side}"], [(1.18, 0.066, 0.072), (1.30, 0.066, 0.072)],
             M["boots"], 10)
        foot(mb, r, side, M["boots"])
    head_and_ears(mb, r, M["skin"], ear_scale=1.0)
    return mb


def body_kargan(r, P, mats, M):
    mb = vc.MeshBuilder()

    def torso_mat(c):
        z = c[2]
        if z > r["torso"][-4][0] + 0.03:
            return M["skin"]
        if z < 1.07:
            return M["trousers"]
        # Open leather vest: bare chest stripe in the front.
        if c[1] < -0.06 and abs(c[0]) < 0.07 + 0.25 * max(0.0, z - 1.15) and z > 1.12:
            return M["skin"]
        return M["vest"]

    torso(mb, r, torso_mat, sides=16)
    # Kilt.
    skirt(mb, r, [(1.07, 0.212, 0.152, 0.012), (0.96, 0.232, 0.168, 0.006), (0.80, 0.252, 0.182, 0.0),
                  (0.70, 0.258, 0.186, 0.0)], M["kilt"], sides=18, thickness=0.016, side_bias=0.55)
    # Heavy belt + buckle.
    belt_rings = []
    for z in (1.10, 1.03):
        pts = [(0.222 * math.cos(2 * math.pi * k / 18), 0.012 * 0 + 0.160 * math.sin(2 * math.pi * k / 18) - 0.012, z)
               for k in range(18)]
        belt_rings.append(([np.array(p) for p in reversed(pts)], {"pelvis": 1.0}))
    mb.loft(belt_rings, M["belt"])
    mb.box((0.0, bf(0.172), 1.065), (0.085, 0.016, 0.07), M["buckle"], {"pelvis": 1.0})
    # Fur mantle: a lumpy torus around the shoulders.
    rng = vc.Rng(r["seed"] + 5)
    seg, tube = 20, 7
    ring_ids = []
    for i in range(seg):
        a = 2 * math.pi * i / seg
        cx, cy = 0.215 * math.cos(a), 0.125 * math.sin(a) + 0.012
        cz = 1.60 - 0.05 * max(0.0, math.sin(a))  # dips lower on the back
        center = np.array([cx, cy, cz])
        radial = np.array([math.cos(a), math.sin(a), 0.0])
        lump = 1.0 + 0.18 * math.sin(3 * a + 0.7) + rng.uniform(-0.08, 0.08)
        row = []
        for j in range(tube):
            b = 2 * math.pi * j / tube
            off = radial * math.cos(b) * 0.075 * lump + np.array([0, 0, 1.0]) * math.sin(b) * 0.06 * lump
            p = center + off
            side = "clavicle_l" if p[0] > 0.12 else ("clavicle_r" if p[0] < -0.12 else None)
            w = {"chest": 1.0} if side is None else {"chest": 0.6, side: 0.4}
            row.append(mb.add_vertex(p, w))
        ring_ids.append(row)
    for i in range(seg):
        i2 = (i + 1) % seg
        for j in range(tube):
            j2 = (j + 1) % tube
            mb.add_face((ring_ids[i][j], ring_ids[i2][j], ring_ids[i2][j2], ring_ids[i][j2]),
                        M["fur"] if (i + j) % 5 else M["fur_dark"])
    for side in ("l", "r"):
        sgn = 1 if side == "l" else -1
        arm_j = [P[f"upperarm_{side}"][0], P[f"forearm_{side}"][0], P[f"hand_{side}"][0]]
        arm_mats = [M["skin"]] * 4 + [M["bracer"]] * 4
        limb(mb, arm_j, [f"upperarm_{side}", f"forearm_{side}"], r["arm"], arm_mats, 10, pre=f"clavicle_{side}")
        # Fur shoulder pad.
        sp = np.asarray(P[f"upperarm_{side}"][0]) + np.array([sgn * 0.02, 0.0, 0.035])
        mb.ellipsoid(sp, (0.105, 0.11, 0.075), 10, 5, M["fur"], {f"upperarm_{side}": 0.55, f"clavicle_{side}": 0.45})
        hand(mb, r, P, side, M["skin"])
        leg_j = [P[f"thigh_{side}"][0], P[f"shin_{side}"][0], P[f"shin_{side}"][1]]
        leg_mats = [M["trousers"]] * 7 + [M["fur"]] * 3
        limb(mb, leg_j, [f"thigh_{side}", f"shin_{side}"], r["leg"], leg_mats, 10, pre="pelvis")
        # Fur boot wrap.
        limb(mb, leg_j, [f"thigh_{side}", f"shin_{side}"], [(1.52, 0.086, 0.092), (1.66, 0.084, 0.090)],
             M["fur_dark"], 10)
        foot(mb, r, side, M["boots"])
    head_and_ears(mb, r, M["skin"], ear_scale=0.85, jaw=1.12)
    return mb


def hand(mb, r, P, side, mat):
    sgn = 1 if side == "l" else -1
    w0, w1 = np.asarray(P[f"hand_{side}"][0]), np.asarray(P[f"hand_{side}"][1])
    d = (w1 - w0) / np.linalg.norm(w1 - w0)
    k = 1.0 if r["height"] < 1.8 else 1.22
    prof = [(-0.05, 0.026, 0.032), (0.12, 0.030, 0.046), (0.55, 0.028, 0.050), (0.85, 0.022, 0.042), (1.0, 0.012, 0.024)]
    rings = []
    for (s, rs, rf) in prof:
        c = w0 + (w1 - w0) * s
        wts = {f"hand_{side}": 1.0} if s > 0.05 else {f"hand_{side}": 0.6, f"forearm_{side}": 0.4}
        rings.append((ring_points(c, d, rs * k, rf * k, 8), wts))
    mb.loft(rings, mat)
    # Thumb on the front-inner side.
    tc = w0 + (w1 - w0) * 0.35 + np.array([-sgn * 0.012, -0.040, 0.0]) * k
    mb.ellipsoid(tc, (0.016 * k, 0.03 * k, 0.018 * k), 6, 4, mat, {f"hand_{side}": 1.0})


def head_and_ears(mb, r, mat, ear_scale=1.0, jaw=1.0):
    cx, cy, cz = r["head_c"]

    def shape(u):
        sx = 1.0
        sy = 1.0
        sz = 1.0
        if u[2] < -0.15:  # lower head: narrower jaw, chin forward
            t = min(1.0, (-u[2] - 0.15) / 0.85)
            sx *= 1.0 - (0.28 / jaw) * t
            if u[1] < 0:
                sy *= 1.0 + 0.10 * t * jaw
        if u[1] > 0.3 and u[2] > -0.2:  # back of the skull
            sy *= 1.05
        return (sx, sy, sz)

    mb.ellipsoid(r["head_c"], r["head_r"], 16, 10, mat, {"head": 1.0}, shape=shape)
    for sgn in (1, -1):
        p, n = placed_on_head(r, sgn * math.radians(88), math.radians(-2), 0.012)
        mb.ellipsoid(p + np.array([sgn * 0.008, 0.012, 0.0]), (0.014 * ear_scale, 0.024 * ear_scale, 0.034 * ear_scale),
                     6, 4, mat, {"head": 1.0})


# ---------------------------------------------------------------------------------------------
# Hair and face variants (each its own object: hair_<i>, face_<i>)

def hair_cap(mb, r, mat, scale, keep, bump=0.0, seed=0, segments=16, rings=10, extra_shape=None):
    rng = vc.Rng(seed)
    noise = [rng.uniform(-1, 1) for _ in range(64)]
    cx, cy, cz = r["head_c"]
    rx, ry, rz = r["head_r"]
    radii = (rx * scale[0], ry * scale[1], rz * scale[2])
    keep_faces = []

    def shape(u):
        s = 1.0
        if bump:
            k = int((math.atan2(u[1], u[0]) + math.pi) / (2 * math.pi) * 16) % 16
            l = int((u[2] + 1) * 2.5) % 4
            s += bump * noise[(k * 4 + l) % 64]
        base = (s, s, s)
        if extra_shape:
            e = extra_shape(u)
            base = (base[0] * e[0], base[1] * e[1], base[2] * e[2])
        return base

    start_face = len(mb.faces)
    mb.ellipsoid((cx, cy + 0.004, cz + 0.006), radii, segments, rings, mat, {"head": 1.0}, shape=shape)
    # Drop faces that `keep` rejects (by unit-sphere direction of the face centre).
    kept_f, kept_m = mb.faces[:start_face], mb.mats[:start_face]
    for f, m in zip(mb.faces[start_face:], mb.mats[start_face:]):
        cen = np.mean([mb.verts[i] for i in f], axis=0) - np.array([cx, cy, cz])
        u = cen / np.array(radii)
        u /= np.linalg.norm(u)
        if keep(u):
            kept_f.append(f)
            kept_m.append(m)
    mb.faces, mb.mats = kept_f, kept_m


def tail_loft(mb, points, radii, mat, weights=None, sides=8):
    rings = []
    for i, (p, rr) in enumerate(zip(points, radii)):
        a = np.asarray(points[max(0, i - 1)], float)
        b = np.asarray(points[min(len(points) - 1, i + 1)], float)
        d = b - a
        rings.append((ring_points(p, d, rr, rr, sides, forward=(0, 0, 1)), weights or {"head": 1.0}))
    mb.loft(rings, mat)


def build_hair(race, r, i, arm_obj):
    mb = vc.MeshBuilder()
    cx, cy, cz = r["head_c"]
    rx, ry, rz = r["head_r"]
    if race == "aydor":
        colours = ["#5a3a22", "#d8b46a", "#1f1a17", "#8c3f1f"]
        mat = vc.material(f"hair_aydor_{i}", colours[i])
        hairline = lambda u: (u[2] > -0.12 and u[1] > -0.25) or u[2] > 0.42  # noqa: E731
        if i == 0:  # short crop with a small fringe
            hair_cap(mb, r, 0, (1.07, 1.07, 1.06), hairline, bump=0.03, seed=11)
        elif i == 1:  # tied ponytail
            hair_cap(mb, r, 0, (1.06, 1.06, 1.05), hairline, bump=0.02, seed=12)
            base = np.array([cx, cy + ry * 1.0, cz - 0.01])
            pts = [base, base + [0, 0.035, -0.05], base + [0, 0.045, -0.13], base + [0, 0.035, -0.22],
                   base + [0, 0.025, -0.27]]
            tail_loft(mb, pts, [0.03, 0.034, 0.03, 0.022, 0.008], 0)
            mb.ellipsoid(base + [0, 0.01, 0.0], (0.028, 0.02, 0.028), 6, 4, 0, {"head": 1.0})
        elif i == 2:  # long, swept to the side, down to the jaw at the back
            longkeep = lambda u: (u[2] > -0.62 and u[1] > 0.05) or (u[2] > -0.15 and u[1] > -0.25) or u[2] > 0.40  # noqa
            hair_cap(mb, r, 0, (1.10, 1.10, 1.08), longkeep, bump=0.04, seed=13,
                     extra_shape=lambda u: (1.0, 1.0, 1.0 + (0.06 if u[0] > 0.3 and u[2] > 0.2 else 0.0)))
        else:  # voluminous wavy
            wavy = lambda u: (u[2] > -0.45 and u[1] > 0.0) or (u[2] > -0.12 and u[1] > -0.3) or u[2] > 0.36  # noqa
            hair_cap(mb, r, 0, (1.16, 1.15, 1.12), wavy, bump=0.08, seed=14)
    else:
        colours = ["#7a2e17", "#1d1a18", "#b8b2a6", "#4b2f1d"]
        mat = vc.material(f"hair_kargan_{i}", colours[i])
        hairline = lambda u: (u[2] > -0.1 and u[1] > -0.2) or u[2] > 0.45  # noqa: E731
        if i == 0:  # shaved sides, top knot
            hair_cap(mb, r, 0, (1.03, 1.05, 1.04), lambda u: u[2] > 0.35 and abs(u[0]) < 0.6, bump=0.02, seed=21)
            top = np.array([cx, cy + 0.03, cz + rz * 1.02])
            mb.ellipsoid(top + [0, 0.0, 0.035], (0.04, 0.04, 0.045), 8, 5, 0, {"head": 1.0})
            tail_loft(mb, [top + [0, 0.02, 0.06], top + [0, 0.07, 0.03], top + [0, 0.09, -0.05]],
                      [0.02, 0.022, 0.006], 0)
        elif i == 1:  # long mane down to the shoulders
            mane = lambda u: (u[1] > 0.0 and u[2] > -0.95) or (u[2] > -0.1 and u[1] > -0.3) or u[2] > 0.4  # noqa
            hair_cap(mb, r, 0, (1.14, 1.16, 1.10), mane, bump=0.06, seed=22,
                     extra_shape=lambda u: (1.0 + 0.25 * max(0.0, -u[2]), 1.0 + 0.3 * max(0.0, -u[2]), 1.0))
        elif i == 2:  # cap + two braids in front of the shoulders
            hair_cap(mb, r, 0, (1.07, 1.08, 1.06), hairline, bump=0.03, seed=23)
            for sgn in (1, -1):
                base = np.array([cx + sgn * rx * 0.95, cy + 0.02, cz - 0.03])
                pts = [base, base + [sgn * 0.01, -0.005, -0.08], base + [sgn * 0.015, -0.03, -0.17],
                       base + [sgn * 0.02, -0.05, -0.26], base + [sgn * 0.02, -0.055, -0.31]]
                tail_loft(mb, pts, [0.03, 0.028, 0.025, 0.02, 0.012], 0, sides=6)
        else:  # wild bushy
            bush = lambda u: (u[2] > -0.4 and u[1] > -0.1) or (u[2] > -0.05 and u[1] > -0.35) or u[2] > 0.3  # noqa
            hair_cap(mb, r, 0, (1.22, 1.22, 1.18), bush, bump=0.12, seed=24)
    return mb.build(f"hair_{i}", [mat], smooth=False, armature=arm_obj)


def build_face(race, r, i, arm_obj):
    pal = PALETTES[race]
    mb = vc.MeshBuilder()
    eye_colours = ["#3b2a1e", "#2f5f86", "#3f6b3a", "#55575c"]
    brow_colours = (["#4a3020", "#a8844a", "#1f1a17", "#6a3218"] if race == "aydor"
                    else ["#5e2412", "#1d1a18", "#8f8a80", "#3b2516"])
    mats = [vc.material(f"eye_{race}_{i}", eye_colours[i]), vc.material(f"brow_{race}_{i}", brow_colours[i]),
            vc.material(f"lips_{race}", pal["lips"]), vc.material(f"skin_{race}", pal["skin"]),
            vc.material(f"white_{race}", pal["eye_white"])]
    EYE, BROW, LIPS, SKIN, WHITE = range(5)
    W = {"head": 1.0}
    big = r["height"] > 1.8
    # Eyes: white + iris.
    for sgn in (1, -1):
        p, n = placed_on_head(r, sgn * math.radians(24), math.radians(4), 0.006)
        mb.ellipsoid(p, (0.016, 0.008, 0.012), 8, 4, WHITE, W)
        mb.ellipsoid(p + n * 0.006, (0.009, 0.004, 0.010), 6, 4, EYE, W)
        # Brow.
        bp, bn = placed_on_head(r, sgn * math.radians(25), math.radians(17), -0.004)
        thick = [0.010, 0.008, 0.014, 0.011][i] * (1.3 if big else 1.0)
        mb.box(tuple(bp), (0.036, 0.012, thick), BROW, W, rot_z=sgn * math.radians(8 if i != 2 else -6))
    # Nose.
    np_, nn = placed_on_head(r, 0.0, math.radians(-8), 0.0)
    nose_len = [0.026, 0.030, 0.022, 0.028][i] * (1.25 if big else 1.0)
    mb.ellipsoid(np_ + nn * 0.010, (0.013 * (1.3 if big else 1), nose_len * 0.6, 0.024), 6, 4, SKIN, W)
    # Mouth.
    mp, mn = placed_on_head(r, 0.0, math.radians(-34), 0.002)
    mb.box(tuple(mp + mn * 0.004), (0.036, 0.008, 0.008), LIPS, W)
    # Facial hair / marks per variant.
    if race == "aydor":
        if i == 1:  # goatee
            gp, gn = placed_on_head(r, 0.0, math.radians(-52), 0.0)
            mb.ellipsoid(gp + gn * 0.01, (0.022, 0.016, 0.026), 6, 4, BROW, W)
        elif i == 2:  # moustache
            for sgn in (1, -1):
                mp2, _ = placed_on_head(r, sgn * math.radians(9), math.radians(-25), -0.006)
                mb.box(tuple(mp2), (0.028, 0.012, 0.009), BROW, W, rot_z=sgn * math.radians(-12))
        elif i == 3:  # short trimmed beard along the jaw
            for k in range(-4, 5):
                bp, bn = placed_on_head(r, math.radians(k * 16), math.radians(-48 + abs(k) * 3), -0.002)
                mb.ellipsoid(bp, (0.022, 0.016, 0.024), 5, 3, BROW, W)
    else:
        if i == 0:  # short full beard
            for k in range(-4, 5):
                bp, bn = placed_on_head(r, math.radians(k * 17), math.radians(-46 + abs(k) * 4), -0.004)
                mb.ellipsoid(bp + bn * 0.006, (0.03, 0.022, 0.034), 6, 3, BROW, W)
        elif i == 1:  # long beard
            bp, bn = placed_on_head(r, 0.0, math.radians(-50), 0.0)
            tail_loft(mb, [bp + [0, 0.005, 0.04], bp + [0, -0.02, -0.02], bp + [0, -0.03, -0.11],
                           bp + [0, -0.02, -0.19]], [0.055, 0.06, 0.045, 0.012], BROW, sides=8)
            for sgn in (1, -1):
                mp2, _ = placed_on_head(r, sgn * math.radians(10), math.radians(-24), -0.006)
                mb.box(tuple(mp2), (0.032, 0.014, 0.012), BROW, W, rot_z=sgn * math.radians(-15))
        elif i == 2:  # big moustache + chin tuft
            for sgn in (1, -1):
                mp2, _ = placed_on_head(r, sgn * math.radians(12), math.radians(-26), -0.008)
                tail_loft(mb, [mp2 - [sgn * 0.02, 0, 0], mp2, mp2 + [sgn * 0.03, 0.0, -0.03],
                               mp2 + [sgn * 0.035, 0.0, -0.06]], [0.008, 0.014, 0.012, 0.004], BROW, sides=6)
            gp, gn = placed_on_head(r, 0.0, math.radians(-55), 0.0)
            mb.ellipsoid(gp + gn * 0.01 + [0, 0, -0.02], (0.02, 0.016, 0.035), 6, 4, BROW, W)
        else:  # clean shaven with blue clan tattoo stripes on the cheeks
            mats[BROW] = vc.material(f"tattoo_{race}", pal["tattoo"])
            for sgn in (1, -1):
                for k in range(2):
                    tp, tn = placed_on_head(r, sgn * math.radians(40 + k * 9), math.radians(-12 - k * 3), -0.002)
                    mb.box(tuple(tp), (0.010, 0.010, 0.045), BROW, W, rot_z=sgn * math.radians(50))
    return mb.build(f"face_{i}", mats, smooth=False, armature=arm_obj)


# ---------------------------------------------------------------------------------------------
# Animation: procedural keyframes with analytic two-bone leg IK (no foot sliding).

def q_from(flex=0.0, twist=0.0, side=0.0):
    """Local bone rotation: flexion about X (forward swing), twist about Y (bone axis), side about
    Z. Applied as side(Z) . twist(Y) . flex(X)."""
    qx = Quaternion((1, 0, 0), flex)
    qy = Quaternion((0, 1, 0), twist)
    qz = Quaternion((0, 0, 1), side)
    return qz @ qy @ qx


class LegSolver:
    def __init__(self, P, r, side):
        hip = np.asarray(P[f"thigh_{side}"][0])
        knee = np.asarray(P[f"shin_{side}"][0])
        ankle = np.asarray(P[f"shin_{side}"][1])
        self.hip_f, self.hip_z = -hip[1], hip[2]
        self.L1 = np.linalg.norm(knee - hip)
        self.L2 = np.linalg.norm(ankle - knee)
        self.phi1r = math.atan2(-knee[1] + hip[1], -(knee[2] - hip[2]))
        self.phi2r = math.atan2(-ankle[1] + knee[1], -(ankle[2] - knee[2]))
        self.ankle_f, self.ankle_z = -ankle[1], ankle[2]
        # Pivots relative to the ankle's f: the heel touches the ground; at toe-off the foot rolls
        # about the ball joint itself (the toe bone head), so the planted toes do not creep.
        self.heel = (r["heel_f"], 0.0)
        self.ball = (r["ball_f"], float(P[f"toe_{side}"][0][2]))

    def solve(self, hip_dz, ankle_f, ankle_z):
        hf, hz = self.hip_f, self.hip_z + hip_dz
        df, dz = ankle_f - hf, ankle_z - hz
        d = math.hypot(df, dz)
        d = min(d, (self.L1 + self.L2) * 0.9995)
        a = math.atan2(df, -dz)
        cb = (self.L1 ** 2 + d ** 2 - self.L2 ** 2) / (2 * self.L1 * d)
        b = math.acos(max(-1.0, min(1.0, cb)))
        phi1 = a + b
        kf, kz = hf + self.L1 * math.sin(phi1), hz - self.L1 * math.cos(phi1)
        phi2 = math.atan2(ankle_f - kf, -(ankle_z - kz))
        d1 = phi1 - self.phi1r
        d2 = (phi2 - phi1) - (self.phi2r - self.phi1r)
        return d1, d2

    def ankle_from_pivot(self, pivot_f, pivot, pitch):
        """Ankle (f, z) when the foot pivots on `pivot` ('heel'|'ball') at ground position pivot_f,
        pitched by `pitch` (toes up positive)."""
        pf, pz = self.heel if pivot == "heel" else self.ball
        vf, vz = -pf, self.ankle_z - pz
        c, s = math.cos(pitch), math.sin(pitch)
        return pivot_f + vf * c - vz * s, pz + vf * s + vz * c


def smoothstep(x):
    x = min(1.0, max(0.0, x))
    return x * x * (3 - 2 * x)


def foot_track(solver, phase, stride, stance, heel_pitch, toe_pitch, lift, heel_frac, toe_frac, lift_skew=1.0,
               fwd_offset=0.0):
    """Body-frame ankle position and foot pitch for one foot at cycle phase in [0,1).
    Stance: [0, stance); the planted foot travels backwards by stance*stride at constant speed."""
    travel = stance * stride
    f_start = travel * 0.5 + fwd_offset  # ankle (flat-foot reference) f at heel strike
    phase %= 1.0

    def stance_pose(p):
        ref = f_start - travel * (p / stance)  # flat-foot ankle f
        if p < heel_frac:
            k = 1.0 - smoothstep(p / heel_frac)
            pitch = heel_pitch * k
            af, az = solver.ankle_from_pivot(ref + solver.heel[0], "heel", pitch)
        elif p > stance - toe_frac:
            k = smoothstep((p - (stance - toe_frac)) / toe_frac)
            pitch = -toe_pitch * k
            af, az = solver.ankle_from_pivot(ref + solver.ball[0], "ball", pitch)
        else:
            pitch = 0.0
            af, az = ref, solver.ankle_z
        return af, az, pitch

    if phase < stance:
        return stance_pose(phase)
    # Swing: from the toe-off pose to the heel-strike pose.
    u = (phase - stance) / (1.0 - stance)
    a0 = stance_pose(stance - 1e-6)
    a1 = stance_pose(0.0)
    e = 0.5 - 0.5 * math.cos(math.pi * u)
    af = a0[0] + (a1[0] - a0[0]) * e
    az = a0[1] + (a1[1] - a0[1]) * e + lift * math.sin(math.pi * (u ** lift_skew))
    pitch = a0[2] + (a1[2] - a0[2]) * smoothstep(u)
    return af, az, pitch


def pose_frame(arm_obj, solvers, r, clip, t):
    """Sets the pose for clip at normalised time t in [0,1)."""
    pb = arm_obj.pose.bones
    tau = 2 * math.pi
    rot = {name: Quaternion() for name in BONES}
    pelvis_loc = Vector((0, 0, 0))
    big = r["height"] > 1.8

    if clip == "idle":
        breath = math.sin(tau * t)
        sway = math.sin(tau * t + 0.6)
        bob = -0.012 - 0.004 * (0.5 + 0.5 * breath)
        pelvis_loc = Vector((0.006 * sway, bob, 0.0))  # pelvis local: x lateral, y up, z forward
        for side, sgn in (("l", 1), ("r", -1)):
            s = solvers[side]
            d1, d2 = s.solve(bob, s.ankle_f, s.ankle_z)
            rot[f"thigh_{side}"] = q_from(d1, 0, sgn * -0.006 * sway)
            rot[f"shin_{side}"] = q_from(d2)
            rot[f"foot_{side}"] = q_from(-(d1 + d2), 0, sgn * 0.006 * sway)
            rot[f"upperarm_{side}"] = q_from(0.06 + 0.02 * math.sin(tau * t + (0.3 if side == "l" else 1.2)), 0,
                                             -sgn * math.radians(r["arm_angle"] - 12.0))
            rot[f"forearm_{side}"] = q_from(math.radians(14) + 0.02 * breath)
            rot[f"hand_{side}"] = q_from(0.05)
            rot[f"clavicle_{side}"] = q_from(0, 0, sgn * 0.012 * breath)
        rot["spine"] = q_from(0.015 + 0.01 * breath, 0, 0.01 * sway)
        rot["chest"] = q_from(-0.012 * breath, 0.0, -0.008 * sway)
        rot["neck"] = q_from(0.01 * breath)
        rot["head"] = q_from(0.02 * math.sin(tau * t + 1.1), 0.03 * math.sin(tau * t * 1.0 + 2.0))
    else:
        run = clip == "run"
        stride = (RUN_SPEED if run else WALK_SPEED) * r[f"{clip}_frames"] / FPS
        g = GAITS[clip]
        # Pelvis bob: two cycles per stride, lowest at `low` (walk: just after heel strike,
        # run: mid-stance).
        bob = g["drop"] - g["bob"] * math.cos(2 * tau * (t - g["low"]))
        tracks = {}
        for side, ph in (("l", 0.0), ("r", 0.5)):
            tracks[side] = foot_track(solvers[side], t + ph, stride, g["stance"], g["heel_pitch"], g["toe_pitch"],
                                      g["lift"] * (1.06 if big else 1.0), g["heel_frac"], g["toe_frac"], g["skew"],
                                      fwd_offset=g["offset"])
        # Never let a leg over-extend (that would make the planted foot slide): lower the pelvis
        # just enough. min() of periodic functions keeps the loop seamless.
        for side in ("l", "r"):
            s = solvers[side]
            af, az, _ = tracks[side]
            reach = 0.985 * (s.L1 + s.L2)
            df = af - s.hip_f
            bob = min(bob, az + math.sqrt(max(0.0, reach * reach - df * df)) - s.hip_z)
        pelvis_loc = Vector((0.0, bob, 0.0))
        for side, sgn, ph in (("l", 1, 0.0), ("r", -1, 0.5)):
            s = solvers[side]
            af, az, pitch = tracks[side]
            d1, d2 = s.solve(bob, af, az)
            rot[f"thigh_{side}"] = q_from(d1)
            rot[f"shin_{side}"] = q_from(d2)
            rot[f"foot_{side}"] = q_from(pitch - d1 - d2)
            rot[f"toe_{side}"] = q_from(max(0.0, -pitch) * 0.9)
            # Arms swing opposite to the legs: left arm is back when the left foot strikes (t=0).
            swing = -math.cos(tau * (t + ph))
            if run:
                rot[f"upperarm_{side}"] = q_from(0.08 + 0.55 * swing, 0, -sgn * math.radians(r["arm_angle"] - 18))
                rot[f"forearm_{side}"] = q_from(math.radians(85) + 0.15 * swing)
                rot[f"hand_{side}"] = q_from(0.15)
            else:
                rot[f"upperarm_{side}"] = q_from(0.05 + 0.30 * swing, 0, -sgn * math.radians(r["arm_angle"] - 13))
                rot[f"forearm_{side}"] = q_from(math.radians(18) + 0.18 * (0.5 + 0.5 * swing))
                rot[f"hand_{side}"] = q_from(0.06)
            rot[f"clavicle_{side}"] = q_from(0.04 * swing, 0, 0)
        twist = math.cos(tau * t)
        if run:
            rot["spine"] = q_from(math.radians(9), 0.06 * twist)
            rot["chest"] = q_from(math.radians(4), 0.10 * twist)
            rot["neck"] = q_from(math.radians(-6), -0.07 * twist)
            rot["head"] = q_from(math.radians(-5), -0.05 * twist)
        else:
            rot["spine"] = q_from(math.radians(2.5), 0.03 * twist)
            rot["chest"] = q_from(math.radians(1.0), 0.06 * twist)
            rot["neck"] = q_from(math.radians(-1.5), -0.04 * twist)
            rot["head"] = q_from(0.0, -0.03 * twist)
        # Slight pelvis roll: the swing-side hip drops a little.
        rot["pelvis"] = q_from(0.0, 0.0, 0.0)

    for name in BONES:
        pb[name].rotation_quaternion = rot[name]
        pb[name].location = Vector((0, 0, 0))
    pb["pelvis"].location = pelvis_loc


def make_actions(arm_obj, P, r):
    solvers = {s: LegSolver(P, r, s) for s in ("l", "r")}
    arm_obj.animation_data_create()
    actions = []
    for clip in ("idle", "walk", "run"):
        frames = r[f"{clip}_frames"]
        act = bpy.data.actions.new(clip)
        act.use_fake_user = True
        arm_obj.animation_data.action = act
        track = {"l": [], "r": []}
        for f in range(frames + 1):
            t = (f % frames) / frames
            pose_frame(arm_obj, solvers, r, clip, t)
            bpy.context.view_layer.update()
            for side in ("l", "r"):
                track[side].append(np.array(arm_obj.pose.bones[f"toe_{side}"].head))
            for name in BONES:
                pbn = arm_obj.pose.bones[name]
                pbn.keyframe_insert("rotation_quaternion", frame=f)
                pbn.keyframe_insert("location", frame=f)
        # Foot-slide report: during flat stance the ball of the foot must move backwards (+Y) at
        # exactly the clip speed and stay at its rest height.
        if clip != "idle":
            g = GAITS[clip]
            v = (WALK_SPEED if clip == "walk" else RUN_SPEED) / FPS
            worst, worst_z = 0.0, 0.0
            for side, ph in (("l", 0.0), ("r", 0.5)):
                for f in range(frames):
                    p0 = ((f / frames) + ph) % 1.0
                    p1 = p0 + 1.0 / frames
                    if p0 >= g["heel_frac"] and p1 <= g["stance"] - g["toe_frac"]:
                        dy = track[side][f + 1][1] - track[side][f][1]
                        worst = max(worst, abs(dy - v) + abs(track[side][f + 1][0] - track[side][f][0]))
                        worst_z = max(worst_z, abs(track[side][f][2] - track[side][0][2] * 0 - 0.028))
            print(f"SLIDE {clip}: max per-frame slide {worst * 1000:.2f} mm, max ball height error "
                  f"{worst_z * 1000:.2f} mm")
        actions.append(act)
    arm_obj.animation_data.action = None
    for name in BONES:
        arm_obj.pose.bones[name].rotation_quaternion = Quaternion()
        arm_obj.pose.bones[name].location = Vector()
    return actions, solvers


# ---------------------------------------------------------------------------------------------
# Previews

def set_variant_visibility(objs, hair, face):
    for o in objs:
        if o.name.startswith("hair_"):
            o.hide_render = int(o.name[5:]) != hair
        elif o.name.startswith("face_"):
            o.hide_render = int(o.name[5:]) != face


def render_previews(race, r, arm_obj, mesh_objs, out_dir):
    sc = bpy.context.scene
    cam = vc.setup_preview((384, 512), sun_dir=(-0.5, -1.0, 1.2))
    ground_mat = vc.material("preview_ground", "#9a9384")
    bpy.ops.mesh.primitive_plane_add(size=12, location=(0, 0, 0))
    ground = bpy.context.active_object
    ground.data.materials.append(ground_mat)
    # Grid lines every 0.5 m along Y so foot sliding is visible in the contact sheets.
    line_mat = vc.material("preview_line", "#5d574c")
    for k in range(-12, 13):
        bpy.ops.mesh.primitive_plane_add(size=1, location=(0, k * 0.5, 0.001))
        ln = bpy.context.active_object
        ln.scale = (6, 0.008, 1)
        ln.data.materials.append(line_mat)
    H = r["height"]
    tgt = (0, 0, H * 0.52)
    dist = 4.6
    out = []
    set_variant_visibility(mesh_objs, 0, 0)
    arm_obj.animation_data.action = None
    views = {"front": (0, -dist, H * 0.55), "side": (dist, 0, H * 0.55), "threequarter": (dist * 0.7, -dist * 0.7, H * 0.7)}
    for vname, loc in views.items():
        vc.aim_camera(cam, loc, tgt, lens=58)
        out.append(vc.render_to(os.path.join(out_dir, f"{race}_bind_{vname}.png")))
    # Variant sheet: hair i + face i, three-quarter head close-up.
    tiles = []
    for v in range(4):
        set_variant_visibility(mesh_objs, v, v)
        hc = r["head_c"]
        vc.aim_camera(cam, (0.75, -1.05, hc[2] + 0.12), (0, 0, hc[2] - 0.03), lens=70)
        tiles.append(vc.render_to(os.path.join(out_dir, f"_tmp_{race}_var{v}.png")))
    vc.compose_sheet(tiles, os.path.join(out_dir, f"{race}_variants.png"), 4)
    vc.remove_files(tiles)
    set_variant_visibility(mesh_objs, 0, 0)
    # Contact sheets: 6 frames over one cycle, side view (left side), feet visible on the grid.
    for act in bpy.data.actions:
        if act.name not in ("walk", "run", "idle"):
            continue
        arm_obj.animation_data.action = act
        frames = r[f"{act.name}_frames"]
        tiles = []
        for k in range(6):
            f = round(k * frames / 6)
            sc.frame_set(f)
            vc.aim_camera(cam, (dist, -0.0, H * 0.5), (0, 0, H * 0.48), lens=58)
            tiles.append(vc.render_to(os.path.join(out_dir, f"_tmp_{race}_{act.name}{k}.png")))
        vc.compose_sheet(tiles, os.path.join(out_dir, f"{race}_{act.name}_sheet.png"), 6)
        vc.remove_files(tiles)
        if act.name != "idle":
            tiles = []
            for k in range(6):
                f = round(k * frames / 6)
                sc.frame_set(f)
                vc.aim_camera(cam, (dist * 0.75, -dist * 0.65, H * 0.62), (0, 0, H * 0.45), lens=58)
                tiles.append(vc.render_to(os.path.join(out_dir, f"_tmp_{race}_{act.name}q{k}.png")))
            vc.compose_sheet(tiles, os.path.join(out_dir, f"{race}_{act.name}_sheet_3q.png"), 6)
            vc.remove_files(tiles)
    arm_obj.animation_data.action = None
    sc.frame_set(0)
    return out


# ---------------------------------------------------------------------------------------------

def main():
    args = vc.script_args()
    race = args[args.index("--race") + 1] if "--race" in args else "aydor"
    preview = "--no-preview" not in args
    r = RACES[race]
    vc.reset_scene()
    arm_obj, P = build_armature(r)
    pal = PALETTES[race]
    names = list(pal.keys())
    mats = [vc.material(f"{k}_{race}", v) for k, v in pal.items()]
    M = {k: i for i, k in enumerate(names)}
    mb = body_aydor(r, P, mats, M) if race == "aydor" else body_kargan(r, P, mats, M)
    body = mb.build("body", mats, smooth=True, sharp_angle=50, armature=arm_obj)
    objs = [body]
    for i in range(4):
        objs.append(build_hair(race, r, i, arm_obj))
    for i in range(4):
        objs.append(build_face(race, r, i, arm_obj))
    actions, _ = make_actions(arm_obj, P, r)

    out_glb = os.path.join(vc.SRC, "characters", f"{race}.glb")
    vc.export_glb(out_glb, [arm_obj] + objs)
    with open(os.path.join(vc.SRC, "characters", f"{race}.anims.toml"), "w", newline="\n") as f:
        f.write(f"# Generated by tools/blender/gen_character.py --race {race}. Clip settings for assetc.\n")
        f.write(f"# walk: {r['walk_frames']} frames @ {FPS} fps, stride {WALK_SPEED * r['walk_frames'] / FPS:.3f} m"
                f" ({WALK_SPEED} m/s); run: {r['run_frames']} frames, stride {RUN_SPEED * r['run_frames'] / FPS:.3f} m"
                f" ({RUN_SPEED} m/s)\n")
        f.write('[[clip]]\nname = "idle"\nloop = true\nmove_speed = 0.0\n\n')
        f.write(f'[[clip]]\nname = "walk"\nloop = true\nmove_speed = {WALK_SPEED}\n\n')
        f.write(f'[[clip]]\nname = "run"\nloop = true\nmove_speed = {RUN_SPEED}\n')
    tris = {o.name: vc.triangle_count(o) for o in objs}
    print("STATS", race, "tris", tris, "visible(h0,f0)", tris["body"] + tris["hair_0"] + tris["face_0"])
    if preview:
        render_previews(race, r, arm_obj, objs, vc.ensure_dir(os.path.join(vc.PREVIEWS, "characters")))


main()
