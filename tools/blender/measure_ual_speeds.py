"""Measures the natural ground speed of the UAL locomotion clips from the root-motion pack.

Usage (headless; the RM file is not committed, pass its path):
  blender --background --factory-startup --python tools/blender/measure_ual_speeds.py -- <UAL1_Standard_RM.glb>

Prints `SPEED <action> <metres> <frames> <m/s>` (root bone displacement over one cycle at 30 fps).
The numbers are copied into vo_ual.NATURAL_SPEED.
"""

import sys

import bpy

path = sys.argv[sys.argv.index("--") + 1]
bpy.ops.wm.read_factory_settings(use_empty=True)
bpy.context.scene.render.fps = 30
bpy.ops.import_scene.gltf(filepath=path)
arm = next(o for o in bpy.data.objects if o.type == 'ARMATURE')
arm.animation_data_create()
for act in bpy.data.actions:
    if not act.name.endswith("_Loop"):
        continue
    arm.animation_data.action = act
    f0, f1 = int(act.frame_range[0]), int(round(act.frame_range[1]))
    bpy.context.scene.frame_set(f0)
    p0 = (arm.matrix_world @ arm.pose.bones["root"].matrix).translation.copy()
    bpy.context.scene.frame_set(f1)
    p1 = (arm.matrix_world @ arm.pose.bones["root"].matrix).translation.copy()
    d = (p1 - p0).length
    if d > 1e-3:
        print(f"SPEED {act.name} {d:.4f} {f1 - f0} {d * 30 / (f1 - f0):.4f}")
