"""Weapon attachment data and in-hand previews (M8 "Kusan").

Usage (headless, after gen_character.py and gen_weapons.py):
  blender --background --factory-startup --python tools/blender/gen_attachments.py -- [--no-preview] [--only aydor_0,...]

Writes gamedata/attachments.toml: per grip (sword, dagger, mace, staff, axe2h, shield), character look
(aydor_0..2, kargan_0..2) and hand (main = hand_r, off = hand_l) the bone and the bone-local transform
of the weapon's grip frame, in engine conventions (docs/ASSET_PIPELINE.md "Weapons" has the maths and
a worked example). Then renders every look holding the kits of KITS in the idle pose and in a run
pose, front and side, to assets_src/previews/weapons/hold_<look>.png. The previews place the weapons
from the attachments file it just wrote (read back with tomllib) and the bind matrices in the
character glb, i.e. with exactly the client's maths, so a wrong convention shows up there.

How a grip is placed (all per look, measured on the rest pose):
  hand frame   F = fingers (hand bone +Y), T = thumb (hand bone +Z, forward at rest), B = back of the
               hand (out of the hand, away from the palm: +X of hand_r, -X of hand_l);
  palm point   the hand's vertices (hand weight > 0.6) in that frame: the band 30-55 % of the way from
               the wrist to the finger tips; P = wrist + F * (42 % of the hand length) + B * (palm
               surface - `palm`) + T * (band centre);
  weapon       length L = cos(tilt) T + sin(tilt) F (tilt > 0 towards the finger tips: blade forward-
               down in idle; < 0 towards the wrist: staff / axe head up), leaned out by `lean` towards
               B; edge / face E perpendicular in the T-F plane, turned by `roll` about L; shields: face
               E = B, top L = -F turned by `tilt` about B; the grip origin goes to
               P + B * `out` + T * `fwd` + L * `slide`.
"""

import json
import math
import os
import struct
import sys
import tomllib

import bpy
import numpy as np
from mathutils import Matrix, Quaternion, Vector

sys.dont_write_bytecode = True
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import vo_common as vc  # noqa: E402
import vo_ual  # noqa: E402

CHAR_DIR = os.path.join(vc.SRC, "characters")
WEAPON_DIR = os.path.join(vc.SRC, "weapons")
OUT_PATH = os.path.join(vc.REPO, "gamedata", "attachments.toml")
PREVIEW_DIR = os.path.join(vc.PREVIEWS, "weapons")

LOOKS = ("aydor_0", "aydor_1", "aydor_2", "kargan_0", "kargan_1", "kargan_2")
HAND_BONE = {"main": "hand_r", "off": "hand_l"}

# Per grip: hands it may be held in and the placement parameters (degrees, metres).
#   tilt   degrees from the thumb direction T towards the finger tips F (negative: towards the wrist)
#   lean   degrees the length axis leans out towards B (away from the body)
#   roll   degrees about the length axis (edge / front)
#   palm   distance of the handle axis from the palm surface, towards the palm side (m; the handle's
#          radius lets it touch the palm, less sinks it into the open hand)
#   out / fwd / slide: extra metres along B / T / the weapon length
DEFAULT = dict(tilt=0.0, lean=0.0, roll=0.0, palm=0.014, out=0.0, fwd=0.0, slide=0.0)
GRIPS = {
    "sword": dict(DEFAULT, hands=("main", "off"), tilt=48.0),
    "dagger": dict(DEFAULT, hands=("main", "off"), tilt=40.0),
    "mace": dict(DEFAULT, hands=("main", "off"), tilt=40.0),
    "staff": dict(DEFAULT, hands=("main",), tilt=-55.0, lean=16.0, fwd=0.06),
    "axe2h": dict(DEFAULT, hands=("main",), tilt=-55.0, lean=16.0, fwd=0.06),
    "shield": dict(DEFAULT, hands=("off",), out=0.03),
}
# Per look overrides: {(grip, look): {param: value}}. Kargan looks: big hands resting against the
# scale skirt (one-handed grips sink into the hand instead of lying between hand and thigh), thick
# bracers and a wide fur mantle (shield further out, staff / axe lean further out of the shoulder).
OVERRIDES = {}
for _look in ("kargan_0", "kargan_1", "kargan_2"):
    for _grip in ("sword", "dagger", "mace"):
        # kargan_2's hand presses into the stiff leather apron: the handle goes through the middle
        # of the hand, otherwise it disappears in the apron.
        OVERRIDES[(_grip, _look)] = dict(palm=-0.025 if _look == "kargan_2" else 0.0)
    OVERRIDES[("shield", _look)] = dict(out=0.10)
    OVERRIDES[("staff", _look)] = dict(tilt=-50.0, lean=22.0)
    OVERRIDES[("axe2h", _look)] = dict(tilt=-50.0, lean=22.0)

# Preview kits: (label, main-hand weapon, off-hand weapon).
KITS = (("sword + shield", "sword_soldier", "shield_oak"), ("dual daggers", "dagger_hunter", "dagger_hunter"),
        ("staff", "staff_ember", None), ("mace", "mace_sun", None), ("two-handed axe", "axe_hearth", None))

# Blender (Z up, front -Y) -> engine (+Y up, +Z forward, +X right): x = -X, y = Z, z = -Y.
C3 = np.array([[-1.0, 0.0, 0.0], [0.0, 0.0, 1.0], [0.0, -1.0, 0.0]])
C4 = np.eye(4)
C4[:3, :3] = C3
S4 = np.diag([-1.0, 1.0, 1.0, 1.0])  # glTF -> engine (X mirror)


# ---------------------------------------------------------------------------------------------
# glTF bind matrices (engine space), exactly what assetc stores

def _quat_mat(q):
    x, y, z, w = q
    return np.array([[1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w)],
                     [2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w)],
                     [2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y)]])


def _node_local(n):
    if "matrix" in n:
        return np.array(n["matrix"], float).reshape(4, 4).T
    m = np.eye(4)
    m[:3, :3] = _quat_mat(n.get("rotation", (0, 0, 0, 1))) * np.array(n.get("scale", (1, 1, 1)))
    m[:3, 3] = n.get("translation", (0, 0, 0))
    return m


def bind_models(glb_path):
    """{joint name: 4x4 engine-space model matrix at bind} from the glb's node hierarchy (the
    armature node folded in, as assetc does), checked against the skin's inverse bind matrices."""
    with open(glb_path, "rb") as f:
        data = f.read()
    jlen = struct.unpack_from("<I", data, 12)[0]
    g = json.loads(data[20:20 + jlen])
    binary = data[20 + jlen + 8:]
    nodes = g["nodes"]
    parent = {}
    for i, n in enumerate(nodes):
        for c in n.get("children", ()):
            parent[c] = i
    world = {}

    def w(i):
        if i not in world:
            world[i] = (w(parent[i]) if i in parent else np.eye(4)) @ _node_local(nodes[i])
        return world[i]

    skin = g["skins"][0]
    acc = g["accessors"][skin["inverseBindMatrices"]]
    bv = g["bufferViews"][acc["bufferView"]]
    off = bv.get("byteOffset", 0) + acc.get("byteOffset", 0)
    ibm = np.frombuffer(binary, "<f4", count=16 * acc["count"], offset=off).reshape(-1, 4, 4).transpose(0, 2, 1)
    out = {}
    worst = 0.0
    for k, j in enumerate(skin["joints"]):
        m = w(j)
        worst = max(worst, float(np.abs(m @ ibm[k] - np.eye(4)).max()))
        out[nodes[j]["name"]] = S4 @ m @ S4
    if worst > 1e-3:
        raise SystemExit(f"{glb_path}: joint world x inverse bind differs from identity by {worst}")
    return out


# ---------------------------------------------------------------------------------------------
# Hand frames and placement (Blender space, rest pose)

def load_character(look):
    vc.reset_scene()
    bpy.ops.import_scene.gltf(filepath=os.path.join(CHAR_DIR, f"{look}.glb"))
    arm = next(o for o in bpy.data.objects if o.type == 'ARMATURE')
    body = next(o for o in bpy.data.objects if o.type == 'MESH')
    rest_pose(arm)
    return arm, body


def rest_pose(arm):
    if arm.animation_data:
        arm.animation_data.action = None
    for pb in arm.pose.bones:
        pb.rotation_mode = 'QUATERNION'
        pb.location = (0, 0, 0)
        pb.rotation_quaternion = (1, 0, 0, 0)
        pb.scale = (1, 1, 1)
    bpy.context.view_layer.update()


def hand_frame(arm, body, hand):
    """(origin, F, T, B, palm point params) of the hand in Blender world space at rest."""
    bone = HAND_BONE[hand]
    m = arm.matrix_world @ arm.data.bones[bone].matrix_local
    o = np.array(m.translation)
    F = np.array(m.col[1][:3])
    T = np.array(m.col[2][:3])
    X = np.array(m.col[0][:3])
    B = X if hand == "main" else -X
    F, T, B = (v / np.linalg.norm(v) for v in (F, T, B))
    me = body.data
    gi = body.vertex_groups[bone].index
    wts = np.zeros(len(me.vertices))
    for v in me.vertices:
        for e in v.groups:
            if e.group == gi:
                wts[v.index] = e.weight
    co = np.array([body.matrix_world @ v.co for v in me.vertices])[wts > 0.6] - o
    cf, ct, cb = co @ F, co @ T, co @ B
    y0, y1 = np.percentile(cf, 2), np.percentile(cf, 98)
    L = y1 - y0
    band = (cf > y0 + 0.30 * L) & (cf < y0 + 0.55 * L)
    return dict(o=o, F=F, T=T, B=B, along=y0 + 0.42 * L, palm=float(np.percentile(cb[band], 3)),
                thumb=float(0.5 * (np.percentile(ct[band], 5) + np.percentile(ct[band], 95))), length=L)


def placement(frame, p):
    """Blender world matrix (4x4 numpy) of the weapon grip frame for grip parameters p."""
    F, T, B = frame["F"], frame["T"], frame["B"]
    a = math.radians(p["tilt"])
    if p["shield"]:
        up = -F
        L = math.cos(a) * up + math.sin(a) * np.cross(B, up)
        E = B
    else:
        L = math.cos(a) * T + math.sin(a) * F
        E = -math.sin(a) * T + math.cos(a) * F
        L = L + math.tan(math.radians(p["lean"])) * B
        r = math.radians(p["roll"])
        E = math.cos(r) * E + math.sin(r) * np.cross(L, E)
    L /= np.linalg.norm(L)
    E -= L * np.dot(E, L)
    E /= np.linalg.norm(E)
    Yw = -E
    Xw = np.cross(Yw, L)
    pt = (frame["o"] + F * frame["along"] + B * (frame["palm"] - p["palm"]) + T * frame["thumb"]
          + B * p["out"] + T * p["fwd"] + L * p["slide"])
    m = np.eye(4)
    m[:3, 0], m[:3, 1], m[:3, 2], m[:3, 3] = Xw, Yw, L, pt
    return m


def params(grip, look):
    p = dict(GRIPS[grip])
    p.update(OVERRIDES.get((grip, look), {}))
    p["shield"] = grip == "shield"
    return p


def to_engine(m_bl):
    return C4 @ m_bl @ C4.T  # C is its own inverse transpose (orthogonal)


def decompose(m):
    s = float(np.cbrt(np.linalg.det(m[:3, :3])))
    r = m[:3, :3] / s
    q = Matrix(r.tolist()).to_quaternion()  # (w, x, y, z), numeric: the same formula as the engine's
    if q.w < 0:
        q.negate()
    return m[:3, 3].copy(), np.array([q.x, q.y, q.z, q.w]), s


def compose(t, q, s):
    m = np.eye(4)
    m[:3, :3] = _quat_mat(q) * s
    m[:3, 3] = t
    return m


# ---------------------------------------------------------------------------------------------

HEADER = """# Weapon attachments (M8 "Kusan"), generated by tools/blender/gen_attachments.py -- do not edit by
# hand: change GRIPS / OVERRIDES there and rerun (docs/ASSET_PIPELINE.md "Weapons").
#
# One [[attach]] per (grip, look, hand). Lookup: the exact look, else the row with look = "*" (a copy
# of aydor_0's values) for looks this file does not know.
#   grip         gamedata/items.toml grip: sword, dagger, mace, staff, axe2h, shield
#   look         character look (characters/<look>.vmesh), or "*"
#   hand         "main" (MainHand slot, right hand) or "off" (OffHand slot, left hand)
#   bone         skeleton bone the weapon follows
#   translation  metres, rotation quaternion [x, y, z, w], scale uniform: the transform A from the
#                weapon's grip frame (= the weapon .vmesh's model space) to the bone's space.
# Engine conventions (left-handed, +X right, +Y up, +Z forward, metres, column vectors p' = M p):
#   A = T(translation) * R(rotation) * S(scale)
#   weaponWorld = characterWorld * boneModel(t) * A
# where boneModel(t) is the bone's model-space matrix for the current animation pose, the same
# matrix the skinning uses before the inverse bind (model[b] = model[parent] * local[b]).
# Weapon grip frame: origin where the hand closes (shields: the handle behind the boss), +Y along
# the weapon towards the blade tip / head / crystal (shields: up), +Z the cutting edge / front
# (shields: outward face).
version = 1
"""


def write_file(rows):
    lines = [HEADER]
    for r in rows:
        t, q, s = r["t"], r["q"], r["s"]
        lines.append("\n[[attach]]\n")
        lines.append(f'grip = "{r["grip"]}"\nlook = "{r["look"]}"\nhand = "{r["hand"]}"\nbone = "{r["bone"]}"\n')
        lines.append("translation = [" + ", ".join(f"{v:.5f}" for v in t) + "]\n")
        lines.append("rotation = [" + ", ".join(f"{v:.6f}" for v in q) + "]\n")
        lines.append(f"scale = {s:.4f}\n")
    text = "".join(lines).replace("-0.00000,", "0.00000,").replace("-0.000000,", "0.000000,")
    vc.ensure_dir(os.path.dirname(OUT_PATH))
    with open(OUT_PATH, "w", encoding="utf-8", newline="\n") as f:
        f.write(text)


def compute(looks):
    rows = []
    checks = []
    for look in looks:
        models = bind_models(os.path.join(CHAR_DIR, f"{look}.glb"))
        arm, body = load_character(look)
        for hand in ("main", "off"):
            bone = HAND_BONE[hand]
            # Sanity: the bind matrix's translation is the Blender bone head through C.
            head_bl = np.array((arm.matrix_world @ arm.data.bones[bone].matrix_local).translation)
            checks.append(float(np.abs(C3 @ head_bl - models[bone][:3, 3]).max()))
            frame = hand_frame(arm, body, hand)
            for grip, g in GRIPS.items():
                if hand not in g["hands"]:
                    continue
                w_eng = to_engine(placement(frame, params(grip, look)))
                a = np.linalg.inv(models[bone]) @ w_eng
                t, q, s = decompose(a)
                rows.append(dict(grip=grip, look=look, hand=hand, bone=bone, t=t, q=q, s=s))
        print(f"ATTACH {look}: hand length r {hand_frame(arm, body, 'main')['length']:.3f} m, "
              f"bind/head mismatch {max(checks):.2e} m")
    if max(checks) > 1e-4:
        raise SystemExit("bind matrices do not match the Blender skeleton (convention error)")
    order = {k: i for i, k in enumerate(GRIPS)}
    rows.sort(key=lambda r: (order[r["grip"]], r["hand"] != "main", LOOKS.index(r["look"])))
    out = []
    for r in rows:
        if r["look"] == LOOKS[0]:
            out.append(dict(r, look="*"))
        out.append(r)
    return out


# ---------------------------------------------------------------------------------------------
# Previews

def read_attachments():
    with open(OUT_PATH, "rb") as f:
        data = tomllib.load(f)
    return {(a["grip"], a["look"], a["hand"]): a for a in data["attach"]}


def load_weapon(name, grips):
    before = set(bpy.data.objects)
    bpy.ops.import_scene.gltf(filepath=os.path.join(WEAPON_DIR, f"{name}.glb"))
    objs = [o for o in bpy.data.objects if o not in before]
    obj = next(o for o in objs if o.type == 'MESH')
    obj.data.transform(obj.matrix_world)
    obj.matrix_world = Matrix.Identity(4)
    for o in objs:
        if o is not obj:
            bpy.data.objects.remove(o)
    return obj


def place_weapons(arm, models, att, look, held):
    """held: [(obj, grip, hand)]. Uses the file's transform and the bind matrix exactly as the client:
    world(t) = C^-1 * engineModel(t) * A * C with engineModel(t) = C * (posed * rest^-1) * C^-1 * bind."""
    for obj, grip, hand in held:
        a = att.get((grip, look, hand)) or att[(grip, "*", hand)]
        A = compose(np.array(a["translation"]), np.array(a["rotation"]), a["scale"])
        bone = a["bone"]
        rest = np.array(arm.matrix_world @ arm.data.bones[bone].matrix_local)
        posed = np.array(arm.matrix_world @ arm.pose.bones[bone].matrix)
        eng = C4 @ (posed @ np.linalg.inv(rest)) @ C4.T @ models[bone]
        obj.matrix_world = Matrix((C4.T @ eng @ A @ C4).tolist())


def setup_studio(resolution):
    cam = vc.setup_preview(resolution, background="#7d7f82", sun_dir=(-0.6, -1.0, 1.1), strength=2.6)
    sc = bpy.context.scene
    sc.render.engine = 'CYCLES'
    sc.cycles.device = 'CPU'
    sc.cycles.samples = 24
    sc.cycles.use_adaptive_sampling = False
    sc.cycles.seed = 0
    sc.cycles.use_animated_seed = False
    sc.cycles.use_denoising = False
    sc.cycles.filter_width = 1.5
    sc.cycles.max_bounces = 4
    sc.render.threads_mode = 'FIXED'
    sc.render.threads = 8
    sc.world.node_tree.nodes["Background"].inputs["Strength"].default_value = 0.55
    for name, direction, energy, col in (("Fill", (1.0, -0.6, 0.4), 0.9, (0.85, 0.9, 1.0)),
                                         ("Rim", (0.2, 1.0, 0.8), 2.0, (1.0, 1.0, 1.0))):
        lt = bpy.data.lights.new(name, 'SUN')
        lt.energy = energy
        lt.color = col
        lt.angle = math.radians(10)
        lt.use_shadow = False
        lo = vc.link(bpy.data.objects.new(name, lt))
        lo.rotation_euler = Vector(direction).normalized().to_track_quat('Z', 'Y').to_euler()
    ground = vc.material("preview_ground", "#77746d")
    bpy.ops.mesh.primitive_plane_add(size=12, location=(0, 0, 0))
    bpy.context.active_object.data.materials.append(ground)
    line = vc.material("preview_line", "#55524b")
    for k in range(-12, 13):
        bpy.ops.mesh.primitive_plane_add(size=1, location=(0, k * 0.5, 0.001))
        ln = bpy.context.active_object
        ln.scale = (6, 0.008, 1)
        ln.data.materials.append(line)
    return cam


def render_look(look, att):
    models = bind_models(os.path.join(CHAR_DIR, f"{look}.glb"))
    arm, body = load_character(look)
    sc = bpy.context.scene
    cam = setup_studio((224, 320))
    H = max((body.matrix_world @ v.co).z for v in body.data.vertices)
    weapons = {}
    for _label, main, off in KITS:
        for n in (main, off):
            if n and n not in weapons:
                weapons[n] = load_weapon(n, None)
    # The dual-dagger kit needs a second copy.
    second = weapons["dagger_hunter"].copy()
    vc.link(second)
    grip_of = {"sword_soldier": "sword", "shield_oak": "shield", "dagger_hunter": "dagger",
               "staff_ember": "staff", "mace_sun": "mace", "axe_hearth": "axe2h"}
    idle, run = bpy.data.actions["idle"], bpy.data.actions["run"]
    # Idle, and the run at both arm-swing extremes (right arm forward / back).
    poses = (("idle", idle, 0), ("run", run, int(round(run.frame_range[1] * 0.25))),
             ("run", run, int(round(run.frame_range[1] * 0.75))))
    views = {"front": (0.0, -4.4, H * 0.55), "side": (-4.4, 0.0, H * 0.55)}
    tiles = []
    for _label, main, off in KITS:
        held = [(weapons[main], grip_of[main], "main")]
        if off:
            held.append((second if off == main else weapons[off], grip_of[off], "off"))
        shown = {id(o) for o, _, _ in held}
        for o in list(weapons.values()) + [second]:
            o.hide_render = id(o) not in shown
        for _pname, act, frame in poses:
            vo_ual.use_action(arm, act)
            sc.frame_set(frame)
            place_weapons(arm, models, att, look, held)
            for o, grip, hand in held:
                zmin = min((o.matrix_world @ v.co).z for v in o.data.vertices)
                print(f"GROUND {look} {_pname} {grip}/{hand}: lowest point {zmin:+.3f} m")
            for _vname, loc in views.items():
                vc.aim_camera(cam, loc, (0, 0, H * 0.48), lens=50)
                tiles.append(vc.render_to(os.path.join(PREVIEW_DIR, f"_tmp_{look}_{len(tiles)}.png")))
    out = vc.compose_sheet(tiles, os.path.join(PREVIEW_DIR, f"hold_{look}.png"), 6, stable=True)
    vc.remove_files(tiles)
    # Hand close-ups in idle: per kit the main hand from outside and from the front, and the off hand
    # (or the main hand from below-front when the kit has none).
    sc.render.resolution_x, sc.render.resolution_y = 224, 224
    tiles = []
    vo_ual.use_action(arm, idle)
    sc.frame_set(0)
    for _label, main, off in KITS:
        held = [(weapons[main], grip_of[main], "main")]
        if off:
            held.append((second if off == main else weapons[off], grip_of[off], "off"))
        shown = {id(o) for o, _, _ in held}
        for o in list(weapons.values()) + [second]:
            o.hide_render = id(o) not in shown
        place_weapons(arm, models, att, look, held)
        hr = (arm.matrix_world @ arm.pose.bones["hand_r"].matrix).translation
        hl = (arm.matrix_world @ arm.pose.bones["hand_l"].matrix).translation
        shots = [(hr + Vector((-0.75, -0.25, 0.1)), hr), (hr + Vector((0.0, -0.8, 0.05)), hr)]
        shots.append((hl + Vector((0.75, -0.25, 0.1)), hl) if off else (hr + Vector((-0.3, -0.6, -0.35)), hr))
        for loc, tgt in shots:
            vc.aim_camera(cam, loc, tgt + Vector((0, 0, -0.06)), lens=50)
            tiles.append(vc.render_to(os.path.join(PREVIEW_DIR, f"_tmp_{look}_c{len(tiles)}.png")))
    vc.compose_sheet(tiles, os.path.join(PREVIEW_DIR, f"hold_{look}_hands.png"), 6, stable=True)
    vc.remove_files(tiles)
    print(f"HOLD {look}: {out} and the hand close-ups")
    return out


def main():
    args = vc.script_args()
    looks = LOOKS
    if "--only" in args:
        looks = tuple(l for l in LOOKS if l in set(args[args.index("--only") + 1].split(",")))
    if "--previews-only" not in args:
        rows = compute(LOOKS)
        write_file(rows)
        print(f"ATTACH wrote {len(rows)} rows to {OUT_PATH}")
    if "--no-preview" in args:
        return
    att = read_attachments()
    for look in looks:
        render_look(look, att)


if __name__ == "__main__":
    main()
