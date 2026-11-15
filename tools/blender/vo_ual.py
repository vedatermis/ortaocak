"""Skeleton and animation clips from the Quaternius "Universal Animation Library" (UAL, CC0 1.0).

Source: tools/blender/third_party/quaternius_ual/UAL1_Standard.glb (in-place clips, 30 fps keys,
T-pose rest, faces -Y with its left side at +X, 65 bones with UE-style names). The pack is data
only; nothing in it is executed.

load_skeleton(scale, ...) imports it into the (empty) scene and turns it into the game skeleton:
  1. drops the stock meshes and every action not in CLIPS, renames the kept actions to game names;
  2. deletes the 12 `*_leaf_*` bones (no weights, end markers) -> 53 bones, and their channels;
  3. closes loops whose last key does not equal the first (blend over the last few frames);
  4. re-binds in an A-pose (upper arms 50 deg down) and rewrites the upper-arm keys so every clip
     keeps exactly the same absolute bone transforms (B' = P^-1 B);
  5. scales uniformly (bones and location keys) and applies optional per-race offsets (shoulder /
     hip width) that are pure translations of bone subtrees, so the shared clips still apply.

Clip move speeds: natural ground speed measured from the root-motion twin UAL1_Standard_RM.glb
(tools/blender/measure_ual_speeds.py) times the race scale, so the client's playback-rate scaling
(rate = speed / moveSpeed) keeps planted feet still.
"""

import math
import os

import bpy
from mathutils import Matrix, Quaternion, Vector

HERE = os.path.dirname(os.path.abspath(__file__))
UAL_GLB = os.path.join(HERE, "third_party", "quaternius_ual", "UAL1_Standard.glb")
FPS = 30

# (game clip name, UAL action, loop). The first three are the ones the client plays today.
CLIPS = [
    ("idle", "Idle_Loop", True),
    ("walk", "Walk_Loop", True),
    ("run", "Jog_Fwd_Loop", True),
    ("sword_idle", "Sword_Idle", True),
    ("sword_attack", "Sword_Attack", False),
    ("punch_jab", "Punch_Jab", False),
    ("punch_cross", "Punch_Cross", False),
    ("spell_enter", "Spell_Simple_Enter", False),
    ("spell_idle", "Spell_Simple_Idle_Loop", True),
    ("spell_shoot", "Spell_Simple_Shoot", False),
    ("spell_exit", "Spell_Simple_Exit", False),
    ("hit_chest", "Hit_Chest", False),
    ("hit_head", "Hit_Head", False),
    ("death", "Death01", False),
    ("jump_start", "Jump_Start", False),
    ("jump_loop", "Jump_Loop", True),
    ("jump_land", "Jump_Land", False),
    ("roll", "Roll", False),
    ("sit_enter", "Sitting_Enter", False),
    ("sit_idle", "Sitting_Idle_Loop", True),
    ("sit_exit", "Sitting_Exit", False),
    ("interact", "Interact", False),
    ("pickup", "PickUp_Table", False),
    ("idle_talking", "Idle_Talking_Loop", True),
    ("crouch_idle", "Crouch_Idle_Loop", True),
    ("crouch_walk", "Crouch_Fwd_Loop", True),
]

# Natural ground speed (m/s at scale 1) of the locomotion clips, measured from UAL1_Standard_RM.glb
# (root displacement over one cycle / cycle time): Walk_Loop 1.300 m / 40 frames, Jog_Fwd_Loop
# 5.000 m / 28 frames, Crouch_Fwd_Loop 1.500 m / 60 frames. (Sprint_Loop: 5.5 m / 20 frames =
# 8.25 m/s, not used: the game's run is 5.5 m/s.)
NATURAL_SPEED = {"Walk_Loop": 1.300 * FPS / 40, "Jog_Fwd_Loop": 5.000 * FPS / 28, "Crouch_Fwd_Loop": 1.500 * FPS / 60}

APOSE_DEG = 50.0  # upper arms rotated this far down from the T-pose (40 deg from vertical)
LOOP_BLEND = 8    # frames over which an open loop is blended into its first frame


def fcurves(action):
    out = []
    for layer in action.layers:
        for strip in layer.strips:
            for cb in strip.channelbags:
                out.extend((cb, fc) for fc in cb.fcurves)
    return out


def bone_of(fc):
    p = fc.data_path
    return p.split('"')[1] if p.startswith("pose.bones[") else None


def _set_key(kp, value):
    d = value - kp.co[1]
    kp.co[1] = value
    kp.handle_left[1] += d
    kp.handle_right[1] += d


def _close_loop(action):
    """Blends the last LOOP_BLEND frames so the last key equals the first key exactly."""
    groups = {}
    for _, fc in fcurves(action):
        groups.setdefault((bone_of(fc), fc.data_path.rsplit(".", 1)[-1]), {})[fc.array_index] = fc
    for (bone, prop), chans in groups.items():
        idx = sorted(chans)
        fcs = [chans[i] for i in idx]
        n = len(fcs[0].keyframe_points)
        if n < 3 or any(len(f.keyframe_points) != n for f in fcs):
            continue
        first = [f.keyframe_points[0].co[1] for f in fcs]
        last = [f.keyframe_points[n - 1].co[1] for f in fcs]
        if prop == "rotation_quaternion" and sum(a * b for a, b in zip(first, last)) < 0:
            first = [-v for v in first]
        if max(abs(a - b) for a, b in zip(first, last)) < 1e-7:
            continue
        end = fcs[0].keyframe_points[n - 1].co[0]
        for k in range(n):
            f = fcs[0].keyframe_points[k].co[0]
            u = (f - (end - LOOP_BLEND)) / LOOP_BLEND
            if u <= 0.0:
                continue
            w = 1.0 if u >= 1.0 else u * u * (3 - 2 * u)
            for c, fc in enumerate(fcs):
                kp = fc.keyframe_points[k]
                _set_key(kp, first[c] if w >= 1.0 else kp.co[1] + (first[c] - last[c]) * w)
        for fc in fcs:
            fc.update()


def _rotate_bone_keys(action, bone, p_inv):
    """Basis B -> P^-1 B for one bone (rotation and location keys)."""
    rot = {}
    loc = {}
    for _, fc in fcurves(action):
        if bone_of(fc) != bone:
            continue
        if fc.data_path.endswith("rotation_quaternion"):
            rot[fc.array_index] = fc
        elif fc.data_path.endswith("location"):
            loc[fc.array_index] = fc
    if len(rot) == 4:
        n = len(rot[0].keyframe_points)
        for k in range(n):
            q = Quaternion([rot[i].keyframe_points[k].co[1] for i in range(4)])
            q2 = p_inv @ q
            for i in range(4):
                _set_key(rot[i].keyframe_points[k], q2[i])
        for fc in rot.values():
            fc.update()
    if len(loc) == 3:
        n = len(loc[0].keyframe_points)
        for k in range(n):
            v = Vector([loc[i].keyframe_points[k].co[1] for i in range(3)])
            v2 = p_inv @ v
            for i in range(3):
                _set_key(loc[i].keyframe_points[k], v2[i])
        for fc in loc.values():
            fc.update()


def _scale_location_keys(action, s):
    for _, fc in fcurves(action):
        if fc.data_path.endswith("location"):
            for kp in fc.keyframe_points:
                kp.co[1] *= s
                kp.handle_left[1] *= s
                kp.handle_right[1] *= s
            fc.update()


def _lean(arm, actions, bone, deg):
    """Adds a constant forward bend to one bone in every clip: B' = L B with L a rotation about the
    world lateral axis expressed in the bone's rest frame."""
    rest = arm.data.bones[bone].matrix_local.to_3x3()
    axis = rest.inverted() @ Vector((1.0, 0.0, 0.0))
    L = Quaternion(axis.normalized(), math.radians(deg))
    for act in actions.values():
        rot = {}
        for _, fc in fcurves(act):
            if bone_of(fc) == bone and fc.data_path.endswith("rotation_quaternion"):
                rot[fc.array_index] = fc
        for k in range(len(rot[0].keyframe_points)):
            q = Quaternion([rot[i].keyframe_points[k].co[1] for i in range(4)])
            q2 = L @ q
            for i in range(4):
                _set_key(rot[i].keyframe_points[k], q2[i])
        for fc in rot.values():
            fc.update()


def fit_joints(arm, fit):
    """Moves joints to a mesh's proportions with pure translations of bone subtrees (rest rotations
    unchanged, so every clip still applies). fit: left-side targets (x, y, z) for 'shoulder',
    'elbow', 'wrist', 'hip', 'knee', 'ankle' (the right side mirrors X). The shoulder heads move to
    their targets and the upper arm / forearm keep their rest directions but take the target lengths;
    the hips only move sideways (knee/ankle targets are used for posing, not for leg lengths)."""
    bpy.context.view_layer.objects.active = arm
    bpy.ops.object.mode_set(mode='EDIT')
    eb = arm.data.edit_bones

    def shift(root, d):
        todo = [eb[root]]
        while todo:
            b = todo.pop()
            b.head += d
            b.tail += d
            todo.extend(b.children)

    def set_len(bone, child, length):
        b = eb[bone]
        dirv = (b.tail - b.head).normalized()
        d = b.head + dirv * length - eb[child].head
        b.tail = b.head + dirv * length
        shift(child, d)

    T = {k: Vector(v) for k, v in fit.items()}
    for side, sgn in (("l", 1.0), ("r", -1.0)):
        m = lambda v: Vector((sgn * v.x, v.y, v.z))  # noqa: E731
        sh, el, wr = m(T["shoulder"]), m(T["elbow"]), m(T["wrist"])
        hp, kn, an = m(T["hip"]), m(T["knee"]), m(T["ankle"])
        ua = eb[f"upperarm_{side}"]
        shift(f"upperarm_{side}", Vector((sh.x, ua.head.y, sh.z)) - ua.head)
        set_len(f"upperarm_{side}", f"lowerarm_{side}", (el - sh).length)
        set_len(f"lowerarm_{side}", f"hand_{side}", (wr - el).length)
        # Legs keep the library's lengths and thigh:shin ratio (the clips' foot contacts depend on
        # them); only the hip width follows the mesh.
        th = eb[f"thigh_{side}"]
        shift(f"thigh_{side}", Vector((hp.x - th.head.x, 0.0, 0.0)))
        del kn, an
    bpy.ops.object.mode_set(mode='OBJECT')


def load_skeleton(scale=1.0, shoulder_dx=0.0, hip_dx=0.0, forearm_ext=0.0, lean=(), fit=None):
    """Imports the UAL armature into the current (empty) scene and prepares it as described in the
    module docstring. Returns (armature object, {game clip name: action}, {game clip name: (loop,
    move_speed)})."""
    sc = bpy.context.scene
    sc.render.fps = FPS
    sc.render.fps_base = 1.0
    bpy.ops.import_scene.gltf(filepath=UAL_GLB)
    arm = next(o for o in bpy.data.objects if o.type == 'ARMATURE')
    for o in list(bpy.data.objects):
        if o is not arm:
            bpy.data.objects.remove(o, do_unlink=True)
    for m in list(bpy.data.meshes):
        bpy.data.meshes.remove(m)
    arm.name = "Armature"
    arm.data.name = "Armature"
    wanted = {src: (name, loop) for name, src, loop in CLIPS}
    actions = {}
    for act in list(bpy.data.actions):
        if act.name not in wanted:
            bpy.data.actions.remove(act)
    for src, (name, loop) in wanted.items():
        act = bpy.data.actions[src]
        act.use_fake_user = True
        actions[name] = act
    for name, act in actions.items():
        act.name = name
        # Integer frame range (some imported ranges end at x.99999, which would drop the last key).
        act.use_frame_range = True
        act.frame_start = round(act.frame_range[0])
        act.frame_end = round(act.frame_range[1])

    # 2. Leaf bones out.
    arm.animation_data_create()
    arm.animation_data.action = None
    bpy.context.view_layer.objects.active = arm
    arm.select_set(True)
    bpy.ops.object.mode_set(mode='EDIT')
    leaves = [b.name for b in arm.data.edit_bones if "_leaf" in b.name]
    for n in leaves:
        arm.data.edit_bones.remove(arm.data.edit_bones[n])
    bpy.ops.object.mode_set(mode='OBJECT')
    for act in actions.values():
        for cb, fc in fcurves(act):
            if bone_of(fc) in leaves:
                cb.fcurves.remove(fc)

    # 3. Close open loops.
    for name, src, loop in CLIPS:
        if loop:
            _close_loop(actions[name])

    # 4. A-pose bind: rotate the upper arms down about the forward axis, apply as rest, rewrite keys.
    for pb in arm.pose.bones:
        pb.rotation_mode = 'QUATERNION'
        pb.rotation_quaternion = Quaternion()
        pb.location = Vector()
        pb.scale = Vector((1, 1, 1))
    bpy.context.view_layer.update()
    pinv = {}
    for side, sgn in (("l", 1.0), ("r", -1.0)):
        pb = arm.pose.bones[f"upperarm_{side}"]
        rest = pb.bone.matrix_local.copy()
        head = rest.translation.copy()
        rw = Matrix.Rotation(sgn * math.radians(APOSE_DEG), 4, 'Y')
        world = Matrix.Translation(head) @ rw @ Matrix.Translation(-head) @ rest
        pb.matrix = world
        bpy.context.view_layer.update()
        pinv[pb.name] = pb.rotation_quaternion.copy().inverted()
    bpy.ops.object.mode_set(mode='POSE')
    bpy.ops.pose.select_all(action='SELECT')
    bpy.ops.pose.armature_apply(selected=False)
    bpy.ops.object.mode_set(mode='OBJECT')
    for act in actions.values():
        for bone, pi in pinv.items():
            _rotate_bone_keys(act, bone, pi)

    # 5. Race scale and widths.
    arm.scale = (scale, scale, scale)
    bpy.ops.object.transform_apply(location=False, rotation=False, scale=True)
    for act in actions.values():
        _scale_location_keys(act, scale)
    if fit:
        fit_joints(arm, fit)
    if shoulder_dx or hip_dx or forearm_ext:
        bpy.ops.object.mode_set(mode='EDIT')
        eb = arm.data.edit_bones

        def shift(root, d):
            todo = [eb[root]]
            while todo:
                b = todo.pop()
                b.head += d
                b.tail += d
                todo.extend(b.children)

        for side, sgn in (("l", 1.0), ("r", -1.0)):
            if shoulder_dx:
                shift(f"upperarm_{side}", Vector((sgn * shoulder_dx, 0.0, 0.0)))
            if hip_dx:
                shift(f"thigh_{side}", Vector((sgn * hip_dx, 0.0, 0.0)))
            if forearm_ext:  # longer forearms: move the hand subtree along the forearm
                fa = eb[f"lowerarm_{side}"]
                d = (fa.tail - fa.head).normalized() * forearm_ext
                fa.tail += d
                shift(f"hand_{side}", d)
        bpy.ops.object.mode_set(mode='OBJECT')
    for bone, deg in lean:
        _lean(arm, actions, bone, deg)
    for pb in arm.pose.bones:
        pb.rotation_mode = 'QUATERNION'
    settings = {}
    for name, src, loop in CLIPS:
        settings[name] = (loop, round(NATURAL_SPEED.get(src, 0.0) * scale, 4))
    for name in PLANTED:
        v = plant_feet(arm, actions[name])
        settings[name] = (True, round(v, 4))
    arm.animation_data.action = None
    return arm, actions, settings


# Locomotion loops whose feet are pinned during ground contact (see plant_feet).
PLANTED = ("walk", "run")
CONTACT = 0.003  # ball of the foot within 3 mm of its rest height = on the ground


def _ball(arm, side):
    return arm.pose.bones[f"ball_{side}"].head.copy()


def _write_rot(action, bone, frame, q):
    keys = {}
    for _, fc in fcurves(action):
        if bone_of(fc) == bone and fc.data_path.endswith("rotation_quaternion"):
            for kp in fc.keyframe_points:
                if abs(kp.co[0] - frame) < 1e-3:
                    keys[fc.array_index] = kp
                    break
    old = Quaternion([keys[i].co[1] for i in range(4)])
    if old.dot(q) < 0.0:  # keep the sign continuous with the original key
        q = -q
    for i in range(4):
        _set_key(keys[i], q[i])


def plant_feet(arm, action):
    """Pins the ball of each foot while it is on the ground.

    The stance speed v (m/s, in-place: the planted ball moves backwards, +Y) is the median ball speed
    over the frames where it rests within 0.5 mm of its rest height. For every contact run (ball
    within CONTACT of the ground) the ball is made to move at exactly v along +Y with constant X,
    anchored at the run's middle frame; the correction fades out over the 3 frames around the run.
    Each corrected frame gets a two-bone IK solve (thigh/calf in the leg's own plane, foot keeps its
    world orientation), written back into the rotation keys. Returns v."""
    sc = bpy.context.scene
    arm.animation_data.action = action
    n = int(round(action.frame_range[1]))
    track = {s: [] for s in ("l", "r")}
    for f in range(n + 1):
        sc.frame_set(f)
        for s in ("l", "r"):
            track[s].append(_ball(arm, s))
    # Ground contact level: the lowest the ball gets in this clip (at most its rest height).
    rest = {s: min(arm.data.bones[f"ball_{s}"].head_local.z, min(p.z for p in track[s])) for s in ("l", "r")}
    speeds = []
    for tol in (0.0005, 0.0015, 0.003, 0.006):  # flat-foot frames; wider if the clip rolls the foot
        for s in ("l", "r"):
            for f in range(n):
                a, b = track[s][f], track[s][f + 1]
                if abs(a.z - rest[s]) < tol and abs(b.z - rest[s]) < tol:
                    speeds.append((b.y - a.y) * FPS)
        if len(speeds) >= 4:
            break
        speeds = []
    speeds.sort()
    v = speeds[len(speeds) // 2]
    deltas = {s: [Vector() for _ in range(n)] for s in ("l", "r")}
    for s in ("l", "r"):
        on = [abs(track[s][f].z - rest[s]) <= CONTACT for f in range(n)]
        if all(on) or not any(on):
            continue
        start = next(f for f in range(n) if on[f] and not on[(f - 1) % n])
        f = start
        runs = []
        while True:  # cyclic contact runs
            if on[f] and not on[(f - 1) % n]:
                run = []
                g = f
                while on[g % n] and len(run) < n:
                    run.append(g % n)
                    g += 1
                runs.append(run)
            f = (f + 1) % n
            if f == start:
                break
        for run in runs:
            mid = run[len(run) // 2]
            anchor = track[s][mid]
            for k, f in enumerate(run):
                df = k - run.index(mid)
                want = Vector((anchor.x, anchor.y + v * df / FPS, track[s][f].z))
                deltas[s][f] = want - track[s][f]
            for k in range(1, 4):  # fade out after / in before the run
                w = 1.0 - k / 4.0
                fa, fb = (run[-1] + k) % n, (run[0] - k) % n
                if not on[fa]:
                    deltas[s][fa] = deltas[s][run[-1]] * w
                if not on[fb]:
                    deltas[s][fb] = deltas[s][run[0]] * w
    for f in range(n):
        for s in ("l", "r"):
            d = deltas[s][f]
            d.z = 0.0
            if d.length < 1e-5:
                continue
            sc.frame_set(f)
            pbs = [arm.pose.bones[f"{b}_{s}"] for b in ("thigh", "calf", "foot")]
            Mt, Mc, Mf = (pb.matrix.copy() for pb in pbs)
            H, K, A = Mt.translation.copy(), Mc.translation.copy(), Mf.translation.copy()
            A2 = A + d
            L1, L2 = (K - H).length, (A - K).length
            dist = min((A2 - H).length, (L1 + L2) * 0.9999)
            x = (A2 - H).normalized()
            pole = (K - H) - x * (K - H).dot(x)
            pole.normalize()
            a = (L1 * L1 + dist * dist - L2 * L2) / (2 * dist)
            h = math.sqrt(max(0.0, L1 * L1 - a * a))
            K2 = H + x * a + pole * h
            A2 = H + x * dist
            R1 = (K - H).rotation_difference(K2 - H).to_matrix().to_4x4()
            Mt2 = Matrix.Translation(H) @ R1 @ Matrix.Translation(-H) @ Mt
            calf_dir = (R1.to_3x3() @ (A - K))
            R2 = calf_dir.rotation_difference(A2 - K2).to_matrix().to_4x4()
            Mc2 = Matrix.Translation(K2) @ R2 @ R1 @ Matrix.Translation(-K) @ Mc
            Mf2 = Matrix.Translation(A2 - A) @ Mf
            for pb, M in zip(pbs, (Mt2, Mc2, Mf2)):
                pb.matrix = M
                bpy.context.view_layer.update()
            for pb in pbs:
                _write_rot(action, pb.name, f, pb.rotation_quaternion.copy())
                if f == 0:
                    _write_rot(action, pb.name, n, pb.rotation_quaternion.copy())
    for _, fc in fcurves(action):
        fc.update()
    return v


def joints(arm):
    """Rest head/tail positions (armature = world space) per bone name."""
    return {b.name: (tuple(b.head_local), tuple(b.tail_local)) for b in arm.data.bones}
