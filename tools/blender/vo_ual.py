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
  5. scales uniformly (bones and location keys) and fits the joints to the look's mesh with pure
     translations of bone subtrees (shoulders, arm lengths, hip width), so the shared clips apply;
  6. re-times the walk for the game's 1.4 m/s (WALK_FRAMES, WALK_STRIDE);
  7. fits thigh / calf lengths to the mesh's knee (fit_legs) and re-targets every clip with a leg
     IK so each ankle follows its library path (retarget_legs), then pins the feet of the
     locomotion loops (plant_feet);
  8. makes the ground-sit clips (make_sit_ground), for player looks the drink clip (make_drink), and
     adds the helper bones (cloth panels, mantle, braids, beard) with procedural secondary motion
     baked into every clip (bake_helpers).

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

# (game clip name, UAL action or None for a clip made procedurally per look, loop). The first three
# are the ones the client plays today.
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
    # Sitting on the ground (the library's Sitting_* clips sit on a chair): made per look by
    # make_sit_ground from the fitted skeleton.
    ("sit_ground_enter", None, False),
    ("sit_ground_idle", None, True),
    ("sit_ground_exit", None, False),
    # Drinking a small flask (right hand to the mouth, head tipped back, hand down again): made per
    # look by make_drink. Player looks only (load_skeleton(drink=...)); NPCs and monsters do not get it.
    ("drink", None, False),
]
LIBRARY_CLIPS = [c for c in CLIPS if c[1] is not None]

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


def use_action(arm, action):
    """Assigns `action` for evaluation after resetting every pose bone to rest, so channels the
    action does not animate never keep values from the previous clip or an IK pass."""
    arm.animation_data.action = None
    for pb in arm.pose.bones:
        pb.rotation_mode = 'QUATERNION'
        pb.location = (0.0, 0.0, 0.0)
        pb.rotation_quaternion = (1.0, 0.0, 0.0, 0.0)
        pb.scale = (1.0, 1.0, 1.0)
    arm.animation_data.action = action
    bpy.context.view_layer.update()


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
        # Hip width here; thigh/calf lengths in fit_legs (after the clips' ankle paths are sampled).
        th = eb[f"thigh_{side}"]
        shift(f"thigh_{side}", Vector((hp.x - th.head.x, 0.0, 0.0)))
        del kn, an
    bpy.ops.object.mode_set(mode='OBJECT')


def fit_legs(arm, fit):
    """Thigh / calf lengths from the mesh: the thigh takes the hip-to-knee distance of the fit targets
    and the calf the rest of the library leg length (hip-to-ankle stays the library's, scaled, so the
    clips' foot contacts stay reachable); rest directions unchanged. The clips are then re-targeted
    with retarget_legs. Returns {side: (old thigh, new thigh, old calf, new calf)}."""
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

    out = {}
    for side, sgn in (("l", 1.0), ("r", -1.0)):
        kn = Vector((sgn * fit["knee"][0], fit["knee"][1], fit["knee"][2]))
        th, ca = eb[f"thigh_{side}"], eb[f"calf_{side}"]
        t0, c0 = (th.tail - th.head).length, (ca.tail - ca.head).length
        t1 = (kn - th.head).length
        c1 = t0 + c0 - t1
        set_len(f"thigh_{side}", f"calf_{side}", t1)
        set_len(f"calf_{side}", f"foot_{side}", c1)
        out[side] = (t0, t1, c0, c1)
    bpy.ops.object.mode_set(mode='OBJECT')
    return out


LEG_BONES = tuple(f"{b}_{s}" for s in ("l", "r") for b in ("thigh", "calf", "foot"))


def _densify(action, bones):
    """Rotation channels of `bones` get one LINEAR key per integer frame (sampled from the curves),
    so the IK passes can rewrite any frame."""
    end = int(round(action.frame_range[1]))
    for _, fc in fcurves(action):
        if bone_of(fc) not in bones or not fc.data_path.endswith("rotation_quaternion"):
            continue
        vals = [fc.evaluate(k) for k in range(end + 1)]
        fc.keyframe_points.clear()
        fc.keyframe_points.add(end + 1)
        for k, v in enumerate(vals):
            kp = fc.keyframe_points[k]
            kp.co = (float(k), v)
            kp.interpolation = 'LINEAR'
            kp.handle_left_type = kp.handle_right_type = 'VECTOR'
            kp.handle_left = (k - 0.33, v)
            kp.handle_right = (k + 0.33, v)
        fc.update()


def _sample_ankles(arm, actions):
    """Ankle (foot head) positions per clip, frame and side, armature space."""
    sc = bpy.context.scene
    out = {}
    for name in sorted(actions):
        act = actions[name]
        use_action(arm, act)
        n = int(round(act.frame_range[1]))
        rows = []
        for f in range(n + 1):
            sc.frame_set(f)
            rows.append({s: arm.pose.bones[f"foot_{s}"].head.copy() for s in ("l", "r")})
        out[name] = rows
    arm.animation_data.action = None
    return out


def retarget_legs(arm, actions, ankles):
    """After fit_legs: every frame's ankle is moved back to where it was with the library leg
    proportions (two-bone IK in the leg's own plane, foot keeps its world orientation), so planted
    feet stay planted. Returns the largest ankle correction (m)."""
    sc = bpy.context.scene
    worst = 0.0
    loops = {name: loop for name, src, loop in CLIPS}
    for name in sorted(actions):
        act = actions[name]
        use_action(arm, act)
        n = int(round(act.frame_range[1]))
        cw = 0.0
        for f in range(n + 1):
            sc.frame_set(f)
            for s in ("l", "r"):
                d = ankles[name][f][s] - arm.pose.bones[f"foot_{s}"].head
                worst = max(worst, d.length)
                cw = max(cw, d.length)
                if d.length > 1e-6:
                    _leg_ik(arm, act, s, f, n if loops[name] else None, d)
                    sc.frame_set(f)
        for _, fc in fcurves(act):
            fc.update()
        res = 0.0
        for f in range(n + 1):
            sc.frame_set(f)
            for s in ("l", "r"):
                res = max(res, (ankles[name][f][s] - arm.pose.bones[f"foot_{s}"].head).length)
        print(f"RETARGET {name}: {n + 1} frames, ankle correction up to {cw * 1000:.1f} mm, residual {res * 1000:.2f} mm")
    arm.animation_data.action = None
    return worst


def load_skeleton(scale=1.0, lean=(), fit=None, helpers=None, sit=None, drink=None):
    """Imports the UAL armature into the current (empty) scene and prepares it as described in the
    module docstring. `drink` (a make_drink cfg dict, None = no drink clip) is passed by the player
    looks only. Returns (armature object, {game clip name: action}, {game clip name: (loop,
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
    wanted = {src: (name, loop) for name, src, loop in LIBRARY_CLIPS}
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
    for name, src, loop in LIBRARY_CLIPS:
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
    for bone, deg in lean:
        _lean(arm, actions, bone, deg)
    for pb in arm.pose.bones:
        pb.rotation_mode = 'QUATERNION'
    settings = {}
    for name, src, loop in CLIPS:
        settings[name] = (loop, round(NATURAL_SPEED.get(src, 0.0) * scale, 4) if src else 0.0)
    _retime(actions["walk"], WALK_FRAMES)
    reach = stretch_stride(arm, actions["walk"], WALK_STRIDE, WALK_DROP * scale)
    print(f"WALK re-timed to {WALK_FRAMES} frames, stride x{WALK_STRIDE}, max leg reach {reach:.3f}")
    if fit and "knee" in fit:
        # Leg proportions from the mesh (knee height), then every clip re-targeted so the ankles
        # follow their library paths exactly (feet still plant; walk/run are pinned below).
        for act in actions.values():
            _densify(act, LEG_BONES)
        ankles = _sample_ankles(arm, actions)
        lengths = fit_legs(arm, fit)
        moved = retarget_legs(arm, actions, ankles)
        t0, t1, c0, c1 = lengths["l"]
        print(f"LEGS thigh {t0:.3f} -> {t1:.3f} m, calf {c0:.3f} -> {c1:.3f} m; clips re-targeted "
              f"(largest ankle correction {moved * 1000:.1f} mm)")
    for name in PLANTED:
        v = plant_feet(arm, actions[name])
        settings[name] = (True, round(v, 4))
    make_sit_ground(arm, actions, scale, sit or {})
    if drink is not None:
        make_drink(arm, actions, scale, drink)
    if helpers:
        add_helpers(arm, helpers)
        bake_helpers(arm, actions, helpers, scale)
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
    use_action(arm, action)
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
            _leg_ik(arm, action, s, f, n, d)
    for _, fc in fcurves(action):
        fc.update()
    return v


def _leg_ik(arm, action, s, f, n, d):
    """Moves the ankle of leg `s` by d at frame f (pose already evaluated there) with a two-bone IK in
    the leg's own plane (the foot keeps its world orientation) and writes the thigh/calf/foot rotation
    keys (frame 0 also into the closing key n). Returns the needed reach / leg length."""
    pbs = [arm.pose.bones[f"{b}_{s}"] for b in ("thigh", "calf", "foot")]
    Mt, Mc, Mf = (pb.matrix.copy() for pb in pbs)
    H, K, A = Mt.translation.copy(), Mc.translation.copy(), Mf.translation.copy()
    A2 = A + d
    L1, L2 = (K - H).length, (A - K).length
    reach = (A2 - H).length / (L1 + L2)
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
        if f == 0 and n is not None:  # loops: the closing key equals the first
            _write_rot(action, pb.name, n, pb.rotation_quaternion.copy())
    return reach


# The game walks at 1.4 m/s. The library Walk_Loop is a 0.98 m/s stroll (40 frames, 1.31 m stride,
# 90 steps/min) and Walk_Formal_Loop has exactly the same legs (only the arms differ), so neither plays
# naturally at 1.4 m/s (rate 1.43-1.5). The walk is instead re-timed to WALK_FRAMES frames per cycle
# and its stride lengthened by WALK_STRIDE (feet moved along the walking axis about their mean by a
# leg IK, heights unchanged): 1.44 m stride at 120 steps/min, i.e. rate ~1.0 at 1.4 m/s.
WALK_FRAMES = 30
WALK_STRIDE = 1.10
WALK_DROP = 0.02  # m (at scale 1): pelvis lowered so the longer stride stays within leg reach


def _retime(action, frames):
    """Resamples every channel of a loop clip onto `frames` + 1 integer keys (linear)."""
    src_end = action.frame_range[1]
    for _, fc in fcurves(action):
        vals = [fc.evaluate(src_end * k / frames) for k in range(frames + 1)]
        fc.keyframe_points.clear()
        fc.keyframe_points.add(frames + 1)
        for k, v in enumerate(vals):
            kp = fc.keyframe_points[k]
            kp.co = (float(k), v)
            kp.interpolation = 'LINEAR'
            kp.handle_left_type = kp.handle_right_type = 'VECTOR'
            kp.handle_left = (k - 0.33, v)
            kp.handle_right = (k + 0.33, v)
        fc.update()
    action.frame_start = 0
    action.frame_end = frames


def stretch_stride(arm, action, k, drop):
    """Scales each foot's position along the walking axis (Y) about its cycle mean by k (constant X
    and Z), via the leg IK, with the pelvis lowered by `drop` (the longer stride needs the reach).
    Returns the largest needed reach / leg length (>= 1 would be clamped)."""
    sc = bpy.context.scene
    use_action(arm, action)
    n = int(round(action.frame_range[1]))
    track = {s: [] for s in ("l", "r")}
    for f in range(n + 1):
        sc.frame_set(f)
        for s in ("l", "r"):
            track[s].append(_ball(arm, s))
    loc = {fc.array_index: fc for _, fc in fcurves(action)
           if bone_of(fc) == "pelvis" and fc.data_path.endswith("location")}
    for f in range(n + 1):
        sc.frame_set(f)
        pb = arm.pose.bones["pelvis"]
        M = pb.matrix.copy()
        M.translation.z -= drop
        pb.matrix = M
        bpy.context.view_layer.update()
        for i in range(3):
            _set_key(next(kp for kp in loc[i].keyframe_points if abs(kp.co[0] - f) < 1e-3), pb.location[i])
    for fc in loc.values():
        fc.update()
    worst = 0.0
    for s in ("l", "r"):
        mean_y = sum(p.y for p in track[s][:n]) / n
        for f in range(n):
            d = Vector((0.0, (k - 1.0) * (track[s][f].y - mean_y), drop))
            sc.frame_set(f)
            worst = max(worst, _leg_ik(arm, action, s, f, n, d))
    for _, fc in fcurves(action):
        fc.update()
    return worst


def joints(arm):
    """Rest head/tail positions (armature = world space) per bone name."""
    return {b.name: (tuple(b.head_local), tuple(b.tail_local)) for b in arm.data.bones}


# ---------------------------------------------------------------------------------------------
# Pose helpers (armature object at the origin: pose matrices are world matrices)

def _upd():
    bpy.context.view_layer.update()


def capture_pose(arm):
    return {pb.name: (pb.location.copy(), pb.rotation_quaternion.copy()) for pb in arm.pose.bones}


def apply_pose(arm, pose):
    for name, (loc, q) in pose.items():
        pb = arm.pose.bones[name]
        pb.location = loc
        pb.rotation_quaternion = q
        pb.scale = (1.0, 1.0, 1.0)
    _upd()


def _rot_world(arm, bone, axis, deg):
    """Rotates a pose bone about its head by deg around a world axis."""
    pb = arm.pose.bones[bone]
    M = pb.matrix.copy()
    h = M.translation.copy()
    pb.matrix = Matrix.Translation(h) @ Matrix.Rotation(math.radians(deg), 4, Vector(axis)) @ Matrix.Translation(-h) @ M
    _upd()


def _frame(d, h):
    """Rotation matrix with columns (x = h made orthogonal to d, y = d, z = x cross y)."""
    d = d.normalized()
    x = (h - d * h.dot(d)).normalized()
    return Matrix((x, d, x.cross(d))).transposed()


def _rest_rot(arm, bone):
    return arm.data.bones[bone].matrix_local.to_3x3()


def _ik_points(P, E, L1, L2, pole):
    """Two-bone chain from P towards E with segment lengths L1, L2, bending towards `pole`
    (a direction). Returns (middle joint, end point)."""
    d = E - P
    dist = min(max(d.length, abs(L1 - L2) + 1e-4), (L1 + L2) * 0.9999)
    x = d.normalized()
    p = pole - x * pole.dot(x)
    p.normalize()
    a = (L1 * L1 + dist * dist - L2 * L2) / (2 * dist)
    h = math.sqrt(max(0.0, L1 * L1 - a * a))
    return P + x * a + p * h, P + x * dist


def _pose_limb(arm, upper, lower, mid, end, bend):
    """Poses upper/lower so they point at mid / end, bending about the limb's own hinge: `bend` is
    the rest-pose direction the lower bone swings to when the joint flexes (knee: back, +Y; elbow:
    front, -Y). The hinge is carried over from the rest pose, so the joint never twists."""
    u0 = _rest_rot(arm, upper).col[1]
    l0 = _rest_rot(arm, lower).col[1]
    h0 = u0.cross(Vector(bend)).normalized()
    pu = arm.pose.bones[upper]
    P = pu.head.copy()
    L1 = (arm.pose.bones[lower].head - P).length
    u1 = (mid - P).normalized()
    M = P + u1 * L1
    l1 = (end - M).normalized()
    a1 = l1 - u1 * l1.dot(u1)
    h1 = u1.cross(a1).normalized() if a1.length > 1e-4 else h0
    pu.matrix = Matrix.Translation(P) @ (_frame(u1, h1) @ _frame(u0, h0).transposed() @ _rest_rot(arm, upper)).to_4x4()
    _upd()
    pl = arm.pose.bones[lower]
    M = pl.head.copy()
    pl.matrix = Matrix.Translation(M) @ (_frame(l1, h1) @ _frame(l0, h0).transposed() @ _rest_rot(arm, lower)).to_4x4()
    _upd()
    return h0, h1


def _pose_end(arm, bone, direction, h0, h1):
    """Gives an end bone (foot, hand) the world direction `direction`, sharing its limb's hinge."""
    e0 = _rest_rot(arm, bone).col[1]
    pb = arm.pose.bones[bone]
    H = pb.head.copy()
    pb.matrix = Matrix.Translation(H) @ (_frame(Vector(direction), h1) @ _frame(e0, h0).transposed() @ _rest_rot(arm, bone)).to_4x4()
    _upd()


def _channelbag(action):
    for layer in action.layers:
        for strip in layer.strips:
            for cb in strip.channelbags:
                return cb
    raise RuntimeError(f"{action.name}: no channel bag")


def _write_clip(arm, action, poses):
    """Replaces every location / rotation key of `action` by the per-frame poses (list of
    capture_pose dicts), one LINEAR key per frame; missing channels are created."""
    cb = _channelbag(action)
    have = {(bone_of(fc), fc.data_path.rsplit(".", 1)[-1], fc.array_index): fc for _, fc in fcurves(action)}
    n = len(poses)
    for name in sorted(poses[0]):
        prev = None
        quats = []
        for p in poses:
            q = p[name][1].copy()
            if prev is not None and prev.dot(q) < 0.0:
                q = -q
            quats.append(q)
            prev = q
        for prop, count in (("location", 3), ("rotation_quaternion", 4), ("scale", 3)):
            for i in range(count):
                fc = have.get((name, prop, i))
                if fc is None:
                    if prop == "scale":
                        continue
                    fc = cb.fcurves.new(f'pose.bones["{name}"].{prop}', index=i)
                if prop == "location":
                    vals = [p[name][0][i] for p in poses]
                elif prop == "rotation_quaternion":
                    vals = [q[i] for q in quats]
                else:
                    vals = [1.0] * n
                fc.keyframe_points.clear()
                fc.keyframe_points.add(n)
                for k, v in enumerate(vals):
                    kp = fc.keyframe_points[k]
                    kp.co = (float(k), v)
                    kp.interpolation = 'LINEAR'
                    kp.handle_left_type = kp.handle_right_type = 'VECTOR'
                    kp.handle_left = (k - 0.33, v)
                    kp.handle_right = (k + 0.33, v)
                fc.update()
    action.use_frame_range = True
    action.frame_start = 0
    action.frame_end = n - 1


# ---------------------------------------------------------------------------------------------
# Sitting on the ground (procedural, per look)

SIT_ENTER = 45   # frames: stand -> squat (18) -> cross-legged on the ground
SIT_IDLE = 120   # one 4 s breath
SIT_EXIT = 45    # ground -> squat (27) -> stand
SIT_SQUAT = 18   # frame of the squat key in the enter clip (exit: SIT_EXIT - SIT_SQUAT + ... see code)


def _smooth(t):
    t = min(1.0, max(0.0, t))
    return t * t * (3 - 2 * t)


def _blend(a, b, t):
    out = {}
    for name in a:
        la, qa = a[name]
        lb, qb = b[name]
        out[name] = (la.lerp(lb, t), qa.slerp(qb if qa.dot(qb) >= 0 else -qb, t))
    return out


def _sit_pose(arm, base, scale, cfg, squat):
    """The cross-legged ground pose (squat=False) or the deep squat on the way down (squat=True),
    built on top of `base` (the idle's first frame). Armature space: the character faces -Y, left
    is +X. Returns a capture_pose dict."""
    s = scale
    apply_pose(arm, base)
    pel = arm.pose.bones["pelvis"]
    M = pel.matrix.copy()
    feet = {side: arm.pose.bones[f"foot_{side}"].head.copy() for side in ("l", "r")}
    foot_rot = {side: arm.pose.bones[f"foot_{side}"].matrix.to_3x3() for side in ("l", "r")}
    if squat:
        head = Vector((0.0, 0.10 * s, cfg.get("squat_z", 0.50) * s))
        tilt, spine = 12.0, (10.0, 8.0, 6.0)
    else:
        head = Vector((0.0, 0.03 * s, cfg.get("sit_z", 0.115) * s))
        tilt, spine = -10.0, (9.0, 8.0, 6.0)
    pel.matrix = Matrix.Translation(head) @ Matrix.Rotation(math.radians(tilt), 4, 'X') @ M.to_3x3().to_4x4()
    _upd()
    for bone, deg in zip(("spine_01", "spine_02", "spine_03"), spine):
        _rot_world(arm, bone, (1, 0, 0), deg)
    _rot_world(arm, "neck_01", (1, 0, 0), -8.0 if squat else -6.0)
    _rot_world(arm, "Head", (1, 0, 0), -6.0 if squat else -2.0)
    knees = {}
    for side, sgn in (("l", 1.0), ("r", -1.0)):
        th, ca, ft = (arm.pose.bones[f"{b}_{side}"] for b in ("thigh", "calf", "foot"))
        L1 = (ca.head - th.head).length
        L2 = (ft.head - ca.head).length
        H = th.head.copy()
        if squat:
            mid, end = _ik_points(H, feet[side], L1, L2, Vector((0.35 * sgn, -1.0, 0.0)))
            h0, h1 = _pose_limb(arm, f"thigh_{side}", f"calf_{side}", mid, end, (0, 1, 0))
            ft.matrix = Matrix.Translation(ft.head) @ foot_rot[side].to_4x4()
            _upd()
        else:
            kdir = Vector((0.55 * sgn, -0.82, 0.10)).normalized()
            mid = H + kdir * L1
            # Shins crossed in front: the left one on top, a little further forward.
            if side == "l":
                end = Vector((-0.07 * s, -0.50 * s, 0.10 * s))
            else:
                end = Vector((0.07 * s, -0.34 * s, 0.075 * s))
            h0, h1 = _pose_limb(arm, f"thigh_{side}", f"calf_{side}", mid, end, (0, 1, 0))
            shin = (arm.pose.bones[f"foot_{side}"].head - arm.pose.bones[f"calf_{side}"].head).normalized()
            _pose_end(arm, f"foot_{side}", (shin + Vector((0, -0.25, 0.05))).normalized(), h0, h1)
        knees[side] = arm.pose.bones[f"calf_{side}"].head.copy()
    for side, sgn in (("l", 1.0), ("r", -1.0)):
        ua, la, hd = (arm.pose.bones[f"{b}_{side}"] for b in ("upperarm", "lowerarm", "hand"))
        L1 = (la.head - ua.head).length
        L2 = (hd.head - la.head).length
        if squat:
            wrist = Vector((0.20 * sgn * s, -0.36 * s, 0.62 * s))
            pole = Vector((0.6 * sgn, 0.5, -0.4))
        else:
            k = knees[side]
            wrist = k + Vector((-0.06 * sgn * s, 0.05 * s, 0.10 * s))
            pole = Vector((0.8 * sgn, 0.6, -0.2))
        mid, end = _ik_points(ua.head.copy(), wrist, L1, L2, pole)
        h0, h1 = _pose_limb(arm, f"upperarm_{side}", f"lowerarm_{side}", mid, end, (0, -1, 0))
        fa = (end - mid).normalized()
        _pose_end(arm, f"hand_{side}", (fa + Vector((0, 0, -0.5))).normalized(), h0, h1)
    return capture_pose(arm)


def make_sit_ground(arm, actions, scale, cfg):
    """sit_ground_enter / sit_ground_idle / sit_ground_exit: from the idle's first frame down
    through a squat onto the ground, cross-legged, hands resting on the knees, a slow breath; and
    back up. Seated: pelvis lowered to the ground (cfg sit_z: hip height above the ground at scale
    1, default 0.115 m), slight posterior pelvic tilt and forward spine, shins crossed (left on top),
    wrists on the knees. Every frame is a full pose (LINEAR keys), so the idle loops exactly and the
    enter/exit clips end/start on the idle's first frame."""
    sc = bpy.context.scene
    use_action(arm, actions["idle"])
    sc.frame_set(0)
    base = capture_pose(arm)
    arm.animation_data.action = None
    squat = _sit_pose(arm, base, scale, cfg, True)
    sit = _sit_pose(arm, base, scale, cfg, False)
    # Breathing: spine_02 / spine_03 / neck pitch offsets (local X), one cycle per clip.
    breath = []
    for f in range(SIT_IDLE + 1):
        w = math.sin(2 * math.pi * f / SIT_IDLE)
        p = {k: (l.copy(), q.copy()) for k, (l, q) in sit.items()}
        for bone, deg in (("spine_02", 1.2), ("spine_03", 1.0), ("neck_01", -0.8)):
            l, q = p[bone]
            p[bone] = (l, q @ Quaternion(Vector((1, 0, 0)), math.radians(deg * w)))
        breath.append(p)
    breath[-1] = breath[0]
    enter = []
    for f in range(SIT_ENTER + 1):
        if f <= SIT_SQUAT:
            enter.append(_blend(base, squat, _smooth(f / SIT_SQUAT)))
        else:
            enter.append(_blend(squat, sit, _smooth((f - SIT_SQUAT) / (SIT_ENTER - SIT_SQUAT))))
    up = SIT_EXIT - SIT_SQUAT
    exit_ = []
    for f in range(SIT_EXIT + 1):
        if f <= up:
            exit_.append(_blend(sit, squat, _smooth(f / up)))
        else:
            exit_.append(_blend(squat, base, _smooth((f - up) / SIT_SQUAT)))
    for name, poses in (("sit_ground_enter", enter), ("sit_ground_idle", breath), ("sit_ground_exit", exit_)):
        act = actions["idle"].copy()
        act.name = name
        act.use_fake_user = True
        _write_clip(arm, act, poses)
        actions[name] = act
    apply_pose(arm, base)
    for pb in arm.pose.bones:
        pb.location = (0, 0, 0)
        pb.rotation_quaternion = (1, 0, 0, 0)
    _upd()
    print(f"SIT ground: hip height {sit['pelvis'][0].z:.3f} (pelvis loc z), enter {SIT_ENTER}, idle {SIT_IDLE}, "
          f"exit {SIT_EXIT} frames")


# ---------------------------------------------------------------------------------------------
# Drinking a small flask (procedural, per look)

DRINK_FRAMES = 30                 # 1 s
DRINK_LIFT, DRINK_TIP, DRINK_HOLD = 9, 14, 20   # hand at the mouth / head back / start lowering


def _drink_pose(arm, base, scale, mouth, tilt, hand_dir, knuckles, pole, palm):
    """The right hand at the mouth on top of `base` (the idle's first frame), armature space (faces
    -Y, left +X): spine_03 / neck_01 / Head pitched back by `tilt` (deg about world X, negative =
    back), the hand (0.8 of the way from the wrist to the middle knuckle) at the lips + `palm`
    (scale-1 offset), the right arm reaching it
    by two-bone IK with the elbow bending towards `pole`, the hand pointing along `hand_dir`
    (wrist -> fingers) with its index-side knuckles turned towards `knuckles`. The finger bones keep
    the idle's pose (the meshes' fingers are skinned to the hand). `mouth`: (y, z) of the lips at
    scale 1 relative to the Head bone's rest head. Returns (capture_pose dict, lips, wrist)."""
    s = scale
    apply_pose(arm, base)
    for bone, deg in zip(("spine_03", "neck_01", "Head"), tilt):
        _rot_world(arm, bone, (1, 0, 0), deg)
    hb = arm.data.bones["Head"]
    lips = hb.head_local + Vector((0.0, mouth[0] * s, mouth[1] * s))
    M = arm.pose.bones["Head"].matrix @ (hb.matrix_local.inverted() @ lips)
    d = Vector(hand_dir).normalized()
    reach = (arm.data.bones["middle_01_r"].head_local - arm.data.bones["hand_r"].head_local).length * 0.8
    wrist = M + Vector(palm) * s - d * reach
    ua = arm.pose.bones["upperarm_r"]
    L1 = (arm.pose.bones["lowerarm_r"].head - ua.head).length
    L2 = (arm.pose.bones["hand_r"].head - arm.pose.bones["lowerarm_r"].head).length
    mid, end = _ik_points(ua.head.copy(), wrist, L1, L2, Vector(pole))
    _pose_limb(arm, "upperarm_r", "lowerarm_r", mid, end, (0, -1, 0))
    hand = arm.pose.bones["hand_r"]
    R0 = _rest_rot(arm, "hand_r")
    kn = arm.data.bones["index_01_r"].head_local - arm.data.bones["pinky_01_r"].head_local
    H = hand.head.copy()
    hand.matrix = Matrix.Translation(H) @ (_frame(d, Vector(knuckles)) @ _frame(R0.col[1], kn).transposed() @ R0).to_4x4()
    _upd()
    return capture_pose(arm), M, hand.head.copy()


def make_drink(arm, actions, scale, cfg):
    """drink: from the idle's first frame the right hand comes up to the mouth (palm turned to the
    face, fingers up, as if holding a small flask at the lips; the client may attach the flask),
    the head and upper back tip back while drinking, and the hand goes down again; the clip starts
    and ends exactly on the idle's first frame (not a loop). Keys (full poses, smoothstep between):
    0 idle, DRINK_LIFT hand at the lips, DRINK_TIP head back and elbow raised (flask tipped up),
    DRINK_HOLD a little further back, DRINK_FRAMES idle. cfg mouth: (y, z) of the lips at scale 1
    relative to the Head bone's rest head (default (-0.10, 0.04), measured on the mesh)."""
    sc = bpy.context.scene
    use_action(arm, actions["idle"])
    sc.frame_set(0)
    base = capture_pose(arm)
    arm.animation_data.action = None
    mouth = cfg.get("mouth", (-0.10, 0.04))
    lift, m1, w1 = _drink_pose(arm, base, scale, mouth, (-2.0, -2.0, -4.0),
                               (0.45, -0.50, 0.74), (0.2, 0.9, 0.2), (-0.75, -0.15, -0.65), (0.0, -0.10, -0.05))
    tip, m2, w2 = _drink_pose(arm, base, scale, mouth, (-4.0, -7.0, -12.0),
                              (0.50, -0.40, 0.77), (0.1, 0.95, 0.3), (-0.85, -0.10, -0.40), (0.0, -0.095, -0.04))
    hold, m3, w3 = _drink_pose(arm, base, scale, mouth, (-5.0, -8.0, -14.0),
                               (0.50, -0.38, 0.78), (0.1, 0.95, 0.3), (-0.85, -0.10, -0.35), (0.0, -0.095, -0.035))
    keys = ((0, base), (DRINK_LIFT, lift), (DRINK_TIP, tip), (DRINK_HOLD, hold), (DRINK_FRAMES, base))
    poses = []
    for f in range(DRINK_FRAMES + 1):
        for (f0, a), (f1, b) in zip(keys, keys[1:]):
            if f0 <= f <= f1:
                poses.append(_blend(a, b, _smooth((f - f0) / (f1 - f0))))
                break
    poses[-1] = base
    act = actions["idle"].copy()
    act.name = "drink"
    act.use_fake_user = True
    _write_clip(arm, act, poses)
    actions["drink"] = act
    apply_pose(arm, base)
    for pb in arm.pose.bones:
        pb.location = (0, 0, 0)
        pb.rotation_quaternion = (1, 0, 0, 0)
    _upd()
    print(f"DRINK {DRINK_FRAMES} frames: wrist to lips {(w1 - m1).length:.3f} / {(w2 - m2).length:.3f} / "
          f"{(w3 - m3).length:.3f} m (lift / tip / hold)")


# ---------------------------------------------------------------------------------------------
# Helper bones: hanging cloth, braids, beard, mantle. Shared by every look (a look without such a
# part simply has no weights on the bone); procedural secondary motion baked into every clip.

# name: (parent, reference bone for "away from the body", gravity share, max deviation from the
#        parent-rigid direction (deg), spring angular frequency (rad/s), damping ratio,
#        knee push (cloth panels), length at scale 1 (m, for the ground clamp; 0 = none))
HELPER_MOTION = {
    "cloth_front": ("pelvis", "pelvis", 0.85, 60.0, 14.0, 0.6, True, 0.45),
    "cloth_back": ("pelvis", "pelvis", 0.85, 60.0, 14.0, 0.6, True, 0.45),
    "mantle": ("spine_03", "spine_03", 0.25, 12.0, 10.0, 0.7, False, 0.0),
    "braid_l": ("Head", "spine_03", 0.4, 20.0, 9.0, 0.6, False, 0.0),
    "braid_r": ("Head", "spine_03", 0.4, 20.0, 9.0, 0.6, False, 0.0),
    "beard": ("Head", "spine_03", 0.35, 20.0, 10.0, 0.6, False, 0.0),
}
HELPERS = tuple(HELPER_MOTION)


def add_helpers(arm, spec):
    """spec: {name: (head, tail)} armature-space rest positions for every name in HELPERS (bones
    hang from head to tail; parents from HELPER_MOTION)."""
    bpy.context.view_layer.objects.active = arm
    bpy.ops.object.mode_set(mode='EDIT')
    eb = arm.data.edit_bones
    for name in HELPERS:
        head, tail = spec[name]
        b = eb.new(name)
        b.head = Vector(head)
        b.tail = Vector(tail)
        b.roll = 0.0
        b.parent = eb[HELPER_MOTION[name][0]]
        b.use_connect = False
    bpy.ops.object.mode_set(mode='OBJECT')
    for name in HELPERS:
        arm.pose.bones[name].rotation_mode = 'QUATERNION'


def _helper_dirs(arm, action, name, spec, scale, loop):
    """Per-frame world direction of a helper bone: gravity-biased target relative to the parent,
    a damped spring for lag (loops: three cycles, last one used), then hard constraints: within
    max deviation of the parent-rigid direction, never swinging into the body (towards the
    reference bone's inward side), cloth panels kept in front of / behind the knees and above the
    ground. Returns (list of rigid quaternions, list of rigid dirs, list of final dirs)."""
    parent, ref, grav, max_dev, omega, zeta, knee_push, length = HELPER_MOTION[name]
    sc = bpy.context.scene
    n = int(round(action.frame_range[1]))
    bone = arm.data.bones[name]
    P0 = arm.data.bones[parent].matrix_local
    R0 = arm.data.bones[ref].matrix_local
    head_rest = bone.head_local.copy()
    d_rest = (bone.tail_local - bone.head_local).normalized()
    side = head_rest.y - arm.data.bones[ref].head_local.y
    o_rest = Vector((0.0, 1.0 if side > 0 else -1.0, 0.0))
    rows = []
    for f in range(n + 1):
        sc.frame_set(f)
        Pm = arm.pose.bones[parent].matrix @ P0.inverted()
        Rm = arm.pose.bones[ref].matrix @ R0.inverted()
        Mr = Pm @ bone.matrix_local
        rows.append(dict(q=Mr.to_quaternion(), d=(Pm.to_3x3() @ d_rest).normalized(), h=Pm @ head_rest,
                         dref=(Rm.to_3x3() @ d_rest).normalized(), o=(Rm.to_3x3() @ o_rest).normalized(),
                         lat=(Rm.to_3x3() @ Vector((1.0, 0.0, 0.0))).normalized(),
                         up=(Pm.to_3x3() @ Vector((0.0, 0.0, 1.0))).normalized().z,
                         knees=[arm.pose.bones[f"calf_{s}"].head.copy() for s in ("l", "r")]))
    g = Vector((0.0, 0.0, -1.0))
    targets = []
    for r in rows:
        # Gravity only while the parent is upright: rolling, lying or upside down the part stays
        # with the body instead of flopping through it.
        k = grav * max(0.0, r["up"])
        t = r["d"] * (1.0 - k) + g * k
        t = r["d"] if t.length < 1e-3 else t.normalized()
        targets.append(t)
    # Damped spring on the unit sphere (world space), 4 substeps per frame; loops run three cycles
    # (the clip's last frame equals its first) and keep the last one.
    steps = 4
    dt = 1.0 / (FPS * steps)
    d = targets[0].copy()
    v = Vector()
    out = [None] * (n + 1)
    out[0] = d.copy()
    for c in range(3 if loop else 1):
        for f in range(n):
            for k in range(steps):
                t1 = targets[f].lerp(targets[f + 1], (k + 1) / steps).normalized()
                a = (t1 - d) * (omega * omega) - v * (2.0 * zeta * omega)
                v += a * dt
                d += v * dt
                d.normalize()
                v -= d * v.dot(d)
            out[f + 1] = d.copy()
        if loop:
            out[0] = out[n].copy()
    final = []
    for f, r in enumerate(rows):
        x = out[f].copy()
        # max deviation from the parent-rigid direction
        lim = math.radians(15.0 + (max_dev - 15.0) * max(0.0, r["up"]))  # tighter when not upright
        ang = r["d"].angle(x, 0.0)
        if ang > lim:
            x = r["d"].slerp(x, lim / ang).normalized()
        e1 = r["dref"]
        e2 = r["o"] - e1 * r["o"].dot(e1)
        if e2.length > 1e-4:
            e2.normalize()
            e3 = e1.cross(e2)

            def phi(vec):
                return math.atan2(vec.dot(e2), vec.dot(e1))

            need = 0.0  # never swing into the body
            if knee_push:
                for kp in r["knees"]:
                    w = kp + e2 * (0.08 * scale) - r["h"]
                    # only knees behind / in front of the panel itself (sitting cross-legged the
                    # knees are wide apart and the panel hangs between them)
                    if w.dot(e1) > 0.05 * scale and abs((kp - r["h"]).dot(r["lat"])) < 0.20 * scale:
                        need = max(need, phi(w))
            cur = phi(x)
            if cur < need:
                x = (Matrix.Rotation(need - cur, 3, e3) @ x).normalized()
            if length > 0.0:
                L = length * scale
                floor = 0.03
                if (r["h"] + x * L).z < floor:
                    lo, hi = 0.0, math.pi / 2
                    for _ in range(24):
                        mid = (lo + hi) / 2
                        if (r["h"] + (Matrix.Rotation(mid, 3, e3) @ x) * L).z < floor:
                            lo = mid
                        else:
                            hi = mid
                    x = (Matrix.Rotation(hi, 3, e3) @ x).normalized()
        if not knee_push:
            # Hair and fur: the deviation limit is final (rolls and falls turn the head far against
            # the spine; a braid swinging further than this stretches its root).
            ang = r["d"].angle(x, 0.0)
            if ang > lim:
                x = r["d"].slerp(x, lim / ang).normalized()
        final.append(x)
    if loop:
        final[n] = final[0].copy()
    return rows, final


def bake_helpers(arm, actions, spec, scale):
    """Writes rotation keys (one per frame) for every helper bone into every clip."""
    loops = {name: loop for name, src, loop in CLIPS}
    for name in sorted(actions):
        act = actions[name]
        use_action(arm, act)
        n = int(round(act.frame_range[1]))
        cb = _channelbag(act)
        for hn in HELPERS:
            rows, dirs = _helper_dirs(arm, act, hn, spec, scale, loops.get(name, False))
            quats = []
            prev = None
            for r, x in zip(rows, dirs):
                qw = r["d"].rotation_difference(x)
                q = r["q"].inverted() @ qw @ r["q"]
                q.normalize()
                if prev is not None and prev.dot(q) < 0.0:
                    q = -q
                quats.append(q)
                prev = q
            for i in range(4):
                fc = cb.fcurves.new(f'pose.bones["{hn}"].rotation_quaternion', index=i)
                fc.keyframe_points.add(n + 1)
                for k, q in enumerate(quats):
                    kp = fc.keyframe_points[k]
                    kp.co = (float(k), q[i])
                    kp.interpolation = 'LINEAR'
                fc.update()
    arm.animation_data.action = None
