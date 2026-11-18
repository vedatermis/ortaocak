"""Humanoid monsters: Tripo H3.1 meshes rigged to the players' skeleton (59 bones: 53 UAL + 6
helpers) with the players' clips, through the character pipeline (gen_character.py: mesh clean-up,
fit, bone heat bind, weight clean-up, deformation into the shared A-pose rest).

Usage (headless):
  blender --background --factory-startup --python tools/blender/gen_monsters.py -- [--only ash_thrall] [--no-preview]
  blender ... gen_monsters.py -- --prepare <raw dir> [--only <names>]   (once per new raw source)
  blender ... gen_monsters.py -- --measure <dir> [--only <names>]        (scratch renders for the fit)
  blender ... gen_monsters.py -- --only slag_walker --debug <dir>        (+ weight renders in <dir>)

Sources: tools/blender/source_meshes/monsters/<name>.glb. The raw Tripo output (~1.45M triangles,
~44 MB each, external_sources/monsters_a/, not committed) is reduced once by `--prepare` exactly like
the weapons (gen_weapons.prepare: welded, collapse-decimated to PREP_TRIS, the raw file's three
embedded 2048^2 images copied byte for byte); the A-pose concept image goes next to it as
<name>_apose.png (half size).

Outputs per monster (model name = gamedata/monsters.toml `model`):
  assets_src/monsters/<name>.glb          skinned mesh "body", the players' 59-bone skeleton, MONSTER_CLIPS
  assets_src/monsters/<name>.png          base colour (sRGB)            } `texture`^2, from the 2048^2
  assets_src/monsters/<name>_n.png        normal map (OpenGL +Y)        } source maps
  assets_src/monsters/<name>_orm.png      R occlusion, G roughness, B metallic
  assets_src/monsters/<name>_e.png        emissive (glowing cracks / eyes / crystal), sRGB
  assets_src/monsters/<name>.anims.toml   clip loop flags and move speeds for assetc
  assets_src/monsters/monsters_meta.json  height, triangles, maps, walk / run stride speeds per monster
  assets_src/previews/monsters/<name>_*.png
Deterministic: same inputs -> same bytes.
"""

import json
import math
import os
import sys

import bmesh
import bpy
import numpy as np
from mathutils import Matrix, Vector

sys.dont_write_bytecode = True  # keep tools/blender free of __pycache__
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import vo_common as vc  # noqa: E402
import vo_ual  # noqa: E402
import gen_character as gc  # noqa: E402  (its main() does not run on import)
import gen_props as gp  # noqa: E402
import gen_weapons as gw  # noqa: E402
import monster_shapes as ms  # noqa: E402

SOURCE_DIR = os.path.join(vc.HERE, "source_meshes", "monsters")
OUT_DIR = os.path.join(vc.SRC, "monsters")
PREVIEW_DIR = os.path.join(vc.PREVIEWS, "monsters")
META_PATH = os.path.join(OUT_DIR, "monsters_meta.json")

PREP_TRIS = 50000  # triangles of the committed reduced sources
REF_FACTOR = 2     # concept images are stored at half size

# The clips a monster needs (docs/PROTOCOL_COMBAT.md §1.3): locomotion, two attacks, the hit
# reaction, death and the simple spell set. Order = vo_ual.CLIPS order.
MONSTER_CLIP_NAMES = ("idle", "walk", "run", "punch_jab", "punch_cross", "spell_enter", "spell_idle",
                      "spell_shoot", "spell_exit", "hit_chest", "death")
MONSTER_CLIPS = [c for c in vo_ual.CLIPS if c[0] in MONSTER_CLIP_NAMES]

COMMON = dict(gc.COMMON, texture=1024, max_tris=30000)

# Per monster: the gen_character.LOOKS fields (rot_z, height, fit, lean, torso_column, regions,
# rigid, helpers; see the comment there; `look` is the model name) plus
#   texture   size of the four maps (the sources are 2048^2; 1024^2 keeps the repo small, and a
#             monster is seen at combat distance)
#   emissive  glow map: rules ((hue0, hue1, sat_min, value_min), ...) on the source base colour (hue in
#             degrees, -15..60 = red..yellow, saturation and value 0..1); a texel glows with
#             ramp(sat - sat_min) * ramp(value - value_min) (each ramp 0 -> 1 over EM_RAMP), coloured
#             colour x (1 - tint_mix) + tint x tint_mix, times `strength` (KHR_materials_emissive_strength)
EM_RAMP = 0.12


def _mirror_box(box):
    (x0, x1), y, z = box
    return ((-x1, -x0), y, z)


def _side_pair(box, weights_l):
    """The same rigid piece on both sides (box given for the left, +X)."""
    weights_r = {k[:-2] + "_r" if k.endswith("_l") else k: v for k, v in weights_l.items()}
    return (dict(box=box, weights=weights_l), dict(box=_mirror_box(box), weights=weights_r))


MONSTERS = [
    # Kül Çömezi: small (1.50 m), gaunt, ash-grey cracked skin with glowing cracks on the chest and
    # forearms and ember eyes, a tattered loin wrap. Arms hang wide in the A-pose.
    dict(COMMON, look="ash_thrall", source="monsters/ash_thrall.glb", height=1.50,
         fit=dict(shoulder=(0.165, 0.03, 1.190), elbow=(0.265, 0.04, 0.990), wrist=(0.375, 0.02, 0.835),
                  hip=(0.085, 0.0, 0.770), knee=(0.125, 0.015, 0.440), ankle=(0.170, 0.045, 0.080)),
         lean=(("spine_02", 5.0), ("neck_01", 3.0)),
         torso_column=((0.88, 0.20), (1.08, 0.15)),
         emissive=dict(rules=((0.0, 35.0, 0.50, 0.42),), strength=3.0, tint=(1.0, 0.42, 0.10), tint_mix=0.3)),
    # Cüruf Yürüyen: 2.00 m, massive rock body with lava seams, boulder pauldrons with rusted pipes,
    # huge forearms and fists hanging almost vertically, head sunk between the shoulders.
    dict(COMMON, look="slag_walker", source="monsters/slag_walker.glb", height=2.00,
         fit=dict(shoulder=(0.440, 0.03, 1.520), elbow=(0.550, 0.0, 1.270), wrist=(0.620, -0.07, 1.030),
                  hip=(0.170, 0.0, 1.080), knee=(0.235, -0.02, 0.720), ankle=(0.290, 0.03, 0.160)),
         lean=(("spine_02", 6.0), ("neck_01", -4.0)),
         torso_column=((1.12, 0.38), (1.36, 0.33)),
         rigid=_side_pair(((0.26, 0.80), (-0.34, 0.34), (1.50, 2.00)), {"clavicle_l": 0.6, "upperarm_l": 0.4}),
         emissive=dict(rules=((5.0, 32.0, 0.55, 0.62),), strength=2.5, tint=(1.0, 0.40, 0.08), tint_mix=0.25)),
    # Kor Bekçisi: 2.05 m (the game draws it 1.25x), dark plate with gold trim, ember veins, a
    # glowing crystal on the chest and a glowing visor; layered pauldrons, a split cloth skirt with
    # side tassets.
    dict(COMMON, look="ember_warden", source="monsters/ember_warden.glb", height=2.05,
         fit=dict(shoulder=(0.270, 0.03, 1.620), elbow=(0.450, 0.05, 1.380), wrist=(0.600, -0.03, 1.130),
                  hip=(0.120, 0.0, 0.960), knee=(0.195, -0.02, 0.630), ankle=(0.270, 0.03, 0.120)),
         torso_column=((1.20, 0.33), (1.40, 0.28)),
         regions=(gc._panel("cloth_front", (-0.12, 0.12), (-0.40, -0.06), 1.04, 0.60, drop=0.10, feather=0.04),
                  gc._panel("cloth_back", (-0.12, 0.12), (0.06, 0.40), 1.04, 0.60, drop=0.10, feather=0.04)),
         rigid=_side_pair(((0.27, 0.56), (-0.26, 0.26), (1.44, 1.90)), {"clavicle_l": 0.5, "upperarm_l": 0.5}),
         helpers=dict(cloth_front=((0.0, -0.16, 1.04), (0.0, -0.18, 0.62)),
                      cloth_back=((0.0, 0.16, 1.04), (0.0, 0.18, 0.64))),
         emissive=dict(rules=((3.0, 31.0, 0.50, 0.65), (31.0, 55.0, 0.35, 0.88)), strength=3.0,
                       tint=(1.0, 0.45, 0.10), tint_mix=0.25)),
]
# M13 "Üç Yurt": the homelands' monsters. No new generated meshes: each is an existing rigged source
# (a Külden Doğan, or a fallen soldier of one of the two peoples - docs/LORE.md: "hayvanlar, ölüler,
# bazen taşın kendisi" rise with an ember inside) given a new skin by `recolor` (RECOLORS below:
# palette remaps, procedural crack networks and frost / straw patterns painted in UV space, ORM
# adjusted) before the usual bake. Fit and weight rules are the source's own (same mesh, same height).
#   Aydor march, scorched fields:  stubble_thrall  "Anız Yanığı"   (ash_thrall: charred straw, gold embers)
#                                  burnt_sentry    "Yanık Nöbetçi" (aydor_2: a border soldier burnt to ash)
#   Kargan pass, cold:             rime_rock       "Kırağı Kayası" (slag_walker: frost-bound basalt, cold glow)
#                                  frozen_hearthguard "Donuk Ocakçı" (kargan_0: a frozen hearth-guard)
# fix1: each also gets its own silhouette (monster_shapes.py): `reshape` = rest-pose bone changes
# baked into the skeleton and the body ({bone: (local euler degrees, uniform scale)}), `shapes` =
# added geometry on the bones (monster_shapes.SHAPES) textured from a swatch strip at the right of
# the atlas (the body's maps squeezed into u 0..0.875). The body is decimated to SHAPED_TRIS so body +
# pieces stay under the 30k budget.
COLD_GLOW = dict(rules=((-175.0, -150.0, 0.32, 0.55),), strength=2.5, tint=(0.45, 0.85, 1.0), tint_mix=0.3)
SHAPED_TRIS = 27000
_M9 = {M["look"]: M for M in MONSTERS}
MONSTERS += [
    # A hunched scarecrow of burnt stubble: long arms, a fan of straw out of the back, straw tufts at
    # the shoulders, elbows and wrists, a straw skirt.
    dict(_M9["ash_thrall"], look="stubble_thrall", recolor="stubble", shapes="stubble", max_tris=SHAPED_TRIS,
         reshape={"spine_02": ((10.0, 0.0, 0.0), 1.0), "spine_03": ((8.0, 0.0, 0.0), 1.0),
                  "neck_01": ((-12.0, 0.0, 0.0), 1.0), "Head": ((-4.0, 0.0, 0.0), 1.0),
                  "upperarm_l": ((0.0, 0.0, 0.0), 1.05), "upperarm_r": ((0.0, 0.0, 0.0), 1.05),
                  "lowerarm_l": ((0.0, 0.0, 0.0), 1.10), "lowerarm_r": ((0.0, 0.0, 0.0), 1.10),
                  "hand_l": ((0.0, 0.0, 0.0), 0.95), "hand_r": ((0.0, 0.0, 0.0), 0.95)},
         emissive=dict(rules=((15.0, 50.0, 0.62, 0.75),), strength=3.0, tint=(1.0, 0.62, 0.18), tint_mix=0.3)),
    # The burnt sentry: head lolled to one side, a broken spear with a torn burnt banner on the back,
    # split plates on the shoulders and forearms, broken arrows in the chest and back.
    dict(gc.LOOK["aydor_2"], look="burnt_sentry", texture=1024, recolor="burnt", shapes="burnt",
         max_tris=SHAPED_TRIS,
         reshape={"spine_03": ((8.0, 0.0, 0.0), 1.0), "neck_01": ((0.0, 0.0, 20.0), 1.0),
                  "Head": ((0.0, 0.0, 12.0), 1.0)},
         emissive=dict(rules=((0.0, 35.0, 0.55, 0.55),), strength=3.0, tint=(1.0, 0.42, 0.10), tint_mix=0.3)),
    # Rime rock: a crusted hump of rimed rock on the back, ice-crystal clusters on shoulders, back and
    # forearms (no bone changes: wider shoulders lift the boulder pauldrons over the authored height and
    # the source already reaches 1.63 of the 1.70 m limit lying dead).
    dict(_M9["slag_walker"], look="rime_rock", recolor="rime", emissive=COLD_GLOW, shapes="rime", max_tris=SHAPED_TRIS),
    # The frozen hearth-guard: frost crown and ice horns, icicles and shards on the pauldrons, a
    # frozen cloak with an icicle hem.
    dict(gc.LOOK["kargan_0"], look="frozen_hearthguard", texture=1024, recolor="frozen", emissive=COLD_GLOW,
         shapes="frozen", max_tris=SHAPED_TRIS, reshape={}),
]
MONSTER = {M["look"]: M for M in MONSTERS}


# ---------------------------------------------------------------------------------------------
# --prepare: raw Tripo glb -> reduced committed source (geometry decimated, images byte-identical)

def prepare(raw_dir, names):
    for name in names:
        raw_path = os.path.join(raw_dir, f"{name}_tripo.glb")
        vc.reset_scene()
        obj = gw.import_glb(raw_path)
        obj.data.calc_loop_triangles()
        n0 = len(obj.data.loop_triangles)
        bm = bmesh.new()
        bm.from_mesh(obj.data)
        bmesh.ops.remove_doubles(bm, verts=bm.verts, dist=gp.FLOAT_EPS * 0.01)  # raw units (1 unit tall)
        bm.to_mesh(obj.data)
        bm.free()
        n1 = gp.decimate(obj, PREP_TRIS)
        gp.triangulate(obj)
        raw_gltf, raw_bin = gw._read_glb(raw_path)
        verts, idx = gw._mesh_arrays(obj)
        out = os.path.join(SOURCE_DIR, f"{name}.glb")
        gw.write_glb(out, verts, idx, raw_gltf, raw_bin)
        ref = os.path.join(raw_dir, f"{name}_apose.png")
        if os.path.exists(ref):
            img = vc.read_image(ref)
            h, w = img.shape[:2]
            img = img[:h - h % REF_FACTOR, :w - w % REF_FACTOR, :3]
            small = gp.down_color(np.ascontiguousarray(img), img.shape[0] // REF_FACTOR) if img.shape[0] == img.shape[1] \
                else gp.lin_to_srgb(gp.srgb_to_lin(img).reshape(img.shape[0] // REF_FACTOR, REF_FACTOR,
                                                               img.shape[1] // REF_FACTOR, REF_FACTOR, 3).mean(axis=(1, 3)))
            vc.write_png(os.path.join(SOURCE_DIR, f"{name}_apose.png"), vc.to_u8(small))
        print(f"PREPARE {name}: {n0} -> {n1} tris, {len(verts)} verts, {os.path.getsize(out) / 1e6:.2f} MB")


# ---------------------------------------------------------------------------------------------
# --measure: the normalised mesh (no decimation) in orthographic views with a 10 cm grid

def measure(R, out_dir):
    vc.reset_scene()
    Rm = dict(R, max_tris=None)
    body, _ = gc.load_source(Rm)
    V = np.array([v.co for v in body.data.vertices])
    print(f"MEASURE {R['look']}: bounds {np.round(V.min(0), 3)} .. {np.round(V.max(0), 3)}")
    # Slices of the left half (x >= 0) every 2 cm: clusters along x (gap > 2.5 cm) with their x
    # range and y centre, to read the arm, torso and leg centre lines.
    L = V[V[:, 0] >= -0.01]
    with open(os.path.join(out_dir, f"{R['look']}_slices.txt"), "w") as f:
        for z in np.arange(0.0, R["height"], 0.02):
            s = L[(L[:, 2] >= z) & (L[:, 2] < z + 0.02)]
            if not len(s):
                continue
            order = np.argsort(s[:, 0])
            s = s[order]
            gaps = np.where(np.diff(s[:, 0]) > 0.025)[0]
            parts = np.split(s, gaps + 1)
            txt = "  ".join(f"[{p[:, 0].min():.3f}..{p[:, 0].max():.3f} y{p[:, 1].mean():+.3f} "
                            f"({p[:, 1].min():+.3f}..{p[:, 1].max():+.3f})]" for p in parts)
            f.write(f"z {z:.2f}: {txt}\n")
    cam = vc.setup_preview((900, 1200), background="#9a9a9a", sun_dir=(-0.5, -1.0, 0.8), strength=3.0)
    sc = bpy.context.scene
    sc.render.engine = 'BLENDER_WORKBENCH'
    sc.display.shading.light = 'STUDIO'
    sc.display.shading.color_type = 'TEXTURE'
    H = R["height"]
    grid_mat = vc.material("grid", "#ff2020")
    lines = []
    for k in range(0, int(H * 10) + 3):  # horizontal lines every 10 cm (front and side planes)
        for plane in ("front", "side"):
            bpy.ops.mesh.primitive_cube_add(size=1)
            ln = bpy.context.active_object
            if plane == "front":
                ln.location = (0, -1.0, k * 0.1)
                ln.scale = (2.4, 0.001, 0.0012 if k % 5 else 0.004)
            else:
                ln.location = (1.0, 0, k * 0.1)
                ln.scale = (0.001, 2.4, 0.0012 if k % 5 else 0.004)
            ln.data.materials.append(grid_mat)
            lines.append(ln)
    for k in range(-12, 13):  # vertical lines
        for plane in ("front", "side"):
            bpy.ops.mesh.primitive_cube_add(size=1)
            ln = bpy.context.active_object
            if plane == "front":
                ln.location = (k * 0.1, -1.0, H / 2 + 0.1)
                ln.scale = (0.0012 if k % 5 else 0.004, 0.001, H + 0.4)
            else:
                ln.location = (1.0, k * 0.1, H / 2 + 0.1)
                ln.scale = (0.001, 0.0012 if k % 5 else 0.004, H + 0.4)
            ln.data.materials.append(grid_mat)
    for vname, loc in (("front", (0, -6, H / 2)), ("side", (6, 0, H / 2))):
        vc.aim_camera(cam, loc, (0, 0, H / 2), ortho_scale=max(H, 1.6) + 0.3)
        sc.render.filepath = os.path.join(out_dir, f"{R['look']}_{vname}.png")
        bpy.ops.render.render(write_still=True)


# ---------------------------------------------------------------------------------------------
# Build

def emissive_weight(srgb, rules):
    """Per-texel glow weight 0..1 from the base colour (see MONSTERS `emissive`)."""
    r, g, b = srgb[..., 0], srgb[..., 1], srgb[..., 2]
    mx = np.maximum(np.maximum(r, g), b)
    mn = np.minimum(np.minimum(r, g), b)
    sat = (mx - mn) / np.maximum(mx, 1e-4)
    hue = np.degrees(np.arctan2(np.sqrt(3.0) * (g - b), 2 * r - g - b)) % 360.0
    hue = np.where(hue > 180.0, hue - 360.0, hue)
    w = np.zeros(mx.shape, np.float32)
    for h0, h1, smin, vmin in rules:
        inside = (hue >= h0) & (hue <= h1)
        ramp = np.clip((sat - smin) / EM_RAMP, 0, 1) * np.clip((mx - vmin) / EM_RAMP, 0, 1)
        w = np.maximum(w, np.where(inside, ramp, 0.0).astype(np.float32))
    return w


def bake_textures(obj, R):
    """The source's 2048^2 maps downscaled to `texture` (colour in linear light, normals as vectors,
    ORM plain) plus the emissive map; the material is rebuilt around them (glTF base colour,
    normal, ORM as metallic-roughness and occlusion, emissive + strength). Returns (paths, per-vertex
    metalness, emissive texel share)."""
    name, size = R["look"], R["texture"]
    imgs = gc.source_images(obj)
    metal = gc.vertex_metal(obj, imgs["orm"])
    paths = dict(color=os.path.join(OUT_DIR, f"{name}.png"), normal=os.path.join(OUT_DIR, f"{name}_n.png"),
                 orm=os.path.join(OUT_DIR, f"{name}_orm.png"), emissive=os.path.join(OUT_DIR, f"{name}_e.png"))
    col = gc.image_array(imgs["color"])[:, :, :3].astype(np.float64)
    lin = gc._srgb_to_linear(col)
    maps = {}
    maps["color"] = gc._linear_to_srgb(gc._box_down(lin, size))
    em = R["emissive"]
    w = emissive_weight(col, em["rules"])
    glow = lin * (1.0 - em["tint_mix"]) + np.array(em["tint"]) * em["tint_mix"]
    emis = gc._box_down(glow * w[..., None], size)
    maps["emissive"] = gc._linear_to_srgb(emis)
    share = float((w > 0.5).mean())
    del col, lin, glow, emis
    a = gc.image_array(imgs["normal"])[:, :, :3].astype(np.float64) * 2.0 - 1.0
    n = gc._box_down(a, size)
    n /= np.maximum(np.linalg.norm(n, axis=2, keepdims=True), 1e-8)
    maps["normal"] = n * 0.5 + 0.5
    a = gc.image_array(imgs["orm"])[:, :, :3].astype(np.float64)
    maps["orm"] = gc._box_down(a, size)
    del a, n
    if R.get("shapes"):
        # The body into columns 0..BODY_W-1, the pieces' swatches in the strip at the right.
        assert size == ms.TEX
        for k in maps:
            maps[k] = ms.squeeze(maps[k])
        rng = np.random.default_rng(7 + sum(ord(ch) for ch in R["look"]))
        ms.paint_strip(maps, ms.SHAPES[R["shapes"]]["swatches"], rng)
        share = float((vc.to_u8(np.clip(maps["emissive"], 0, 1))[..., 0] > 60).mean())
    for k in ("color", "emissive", "normal", "orm"):
        vc.write_png(paths[k], vc.to_u8(np.clip(maps[k], 0, 1)), optimize=True)
    del maps
    for img in list(bpy.data.images):
        bpy.data.images.remove(img)
    mat = gp.build_material(f"{name}_skin", paths, em["strength"])
    obj.data.materials.clear()
    obj.data.materials.append(mat)
    for p in obj.data.polygons:
        p.material_index = 0
    for m in list(bpy.data.materials):
        if m is not mat and m.users == 0:
            bpy.data.materials.remove(m)
    return paths, metal, share


# ---------------------------------------------------------------------------------------------
# M13 recolours (in UV space on the source maps, before bake_textures; sRGB values 0..1)

RECOLOR_SIZE = 2048  # player sources (4096^2) are box-filtered to this first


def _get(img):
    return np.ascontiguousarray(gc.image_array(img)[:, :, :3], dtype=np.float32)


def _set(img, arr):
    h, w = arr.shape[:2]
    if tuple(img.size) != (w, h):
        img.scale(w, h)
    rgba = np.concatenate([arr, np.ones((h, w, 1), np.float32)], -1)[::-1]
    img.pixels.foreach_set(np.ascontiguousarray(rgba, dtype=np.float32).ravel())


def _hsv(c):
    r, g, b = c[..., 0], c[..., 1], c[..., 2]
    mx = np.maximum(np.maximum(r, g), b)
    mn = np.minimum(np.minimum(r, g), b)
    sat = (mx - mn) / np.maximum(mx, 1e-4)
    hue = np.degrees(np.arctan2(np.sqrt(3.0) * (g - b), 2 * r - g - b)) % 360.0
    return np.where(hue > 180.0, hue - 360.0, hue), sat, mx


def _gmap(t, stops):
    """Gradient map: t (h, w) 0..1 -> colour through [(pos, '#hex'), ...] (sRGB)."""
    pos = [p for p, _ in stops]
    cols = np.array([[int(h.lstrip("#")[i:i + 2], 16) / 255.0 for i in (0, 2, 4)] for _, h in stops])
    return np.stack([np.interp(np.clip(t, 0, 1), pos, cols[:, k]) for k in range(3)], -1).astype(np.float32)


def _hex(h):
    return np.array([int(h.lstrip("#")[i:i + 2], 16) / 255.0 for i in (0, 2, 4)], np.float32)


def _noise(rng, cells, octaves, n):
    import gen_terrain as gt  # noise helpers
    return gt.fbm(rng, cells, octaves, n=n).astype(np.float32)


def _cracks(rng, n, cells=26, width=(0.955, 0.99), region=0.55):
    """A network of thin lines (ridged noise) in patches (low-frequency mask)."""
    r = 1.0 - np.abs(_noise(rng, cells, 3, n) * 2.0 - 1.0)
    line = np.clip((r - width[0]) / (width[1] - width[0]), 0, 1)
    mask = np.clip((_noise(rng, 5, 3, n) - region) / 0.08, 0, 1)
    return line * mask


def _down(a, n):
    f = a.shape[0] // n
    return a if f <= 1 else a.reshape(n, f, n, f, a.shape[2]).mean(axis=(1, 3)).astype(np.float32)


def recolor_stubble(c, orm, rng):
    """Anız Yanığı: the ash thrall as charred straw and stubble - near-black char, burnt umber, dry
    straw-gold on the highlights, straw fibre streaks; its cracks burn gold instead of red."""
    n = c.shape[0]
    hue, sat, mx = _hsv(c)
    lum = c @ np.array([0.2126, 0.7152, 0.0722], np.float32)
    glow = ((hue > -15) & (hue < 45) & (sat > 0.5) & (mx > 0.42)).astype(np.float32)
    fib = _noise(rng, 180, 2, n)
    streak = np.clip((np.sin(np.arange(n)[None, :] * 0.9 + _noise(rng, 12, 3, n) * 40.0) * 0.5 + 0.5), 0, 1)
    t = np.clip(lum * 1.45 + 0.18 * (streak - 0.5) + 0.12 * (fib - 0.5), 0, 1)
    out = _gmap(t, [(0.0, "#140f0b"), (0.25, "#3a2615"), (0.55, "#7d5a2e"), (0.85, "#c9a45c"), (1.0, "#e2c88a")])
    new = _cracks(rng, n, 30, region=0.6) * (1 - glow)
    ember = np.clip(glow + new, 0, 1)[..., None]
    out = out * (1 - ember) + _hex("#ffb23a") * ember
    orm[..., 1] = np.clip(orm[..., 1] * 0.3 + 0.68, 0, 1)
    return out


def recolor_burnt(c, orm, rng):
    """Yanık Nöbetçi: an Aydor border soldier burnt to ash - blackened, rust-flecked plate, charred
    cloth, ash-grey skin, ember cracks running through the cloth and skin."""
    n = c.shape[0]
    lum = c @ np.array([0.2126, 0.7152, 0.0722], np.float32)
    metal = np.clip((orm[..., 2] - 0.35) / 0.3, 0, 1)
    soot = _noise(rng, 40, 4, n)
    ash = _gmap(np.clip(lum * 1.25 + 0.2 * (soot - 0.5), 0, 1),
                [(0.0, "#100d0b"), (0.3, "#2e2723"), (0.6, "#5f5650"), (1.0, "#9b938a")])
    rust = np.clip((_noise(rng, 18, 4, n) - 0.58) / 0.1, 0, 1)[..., None]
    iron = (np.clip(lum * 0.45, 0, 1)[..., None] * np.array([0.95, 0.92, 0.9]) + 0.05) * (1 - 0.6 * rust) \
        + _hex("#4a2a18") * 0.6 * rust
    m = metal[..., None]
    out = ash * (1 - m) + iron.astype(np.float32) * m
    ember = (_cracks(rng, n, 24, region=0.42) * (1 - metal))[..., None]
    out = out * (1 - ember) + _hex("#ff6418") * ember
    orm[..., 1] = np.where(metal > 0.5, np.clip(orm[..., 1] + 0.2, 0, 1), 0.93)
    return out


def recolor_rime(c, orm, rng):
    """Kırağı Kayası: the slag walker as frost-bound basalt - blue-black rock, rime crystals in the
    hollows and on the brows, the lava seams turned into a pale cold glow."""
    n = c.shape[0]
    hue, sat, mx = _hsv(c)
    lum = c @ np.array([0.2126, 0.7152, 0.0722], np.float32)
    glow = ((hue > 0) & (hue < 36) & (sat > 0.5) & (mx > 0.55)).astype(np.float32)
    rock = _gmap(np.clip(lum * 1.9, 0, 1), [(0.0, "#0f1216"), (0.35, "#28303a"), (0.7, "#4f5b68"), (1.0, "#7d8a98")])
    frost = np.clip((_noise(rng, 34, 4, n) - 0.55) / 0.12, 0, 1) * np.clip((lum - 0.12) / 0.2, 0, 1)
    f = (frost * 0.75)[..., None]
    out = rock * (1 - f) + _hex("#cfe0ee") * f
    g = glow[..., None]
    out = out * (1 - g) + _hex("#78d8ff") * g
    orm[..., 1] = np.clip(orm[..., 1] * (1 - 0.45 * frost), 0, 1)
    return out


def recolor_frozen(c, orm, rng):
    """Donuk Ocakçı: a Kargan hearth-guard frozen where he stood - skin gone pale blue-grey, fur and
    leather rimed white-grey, the iron frosted, cold-glowing cracks in the skin."""
    n = c.shape[0]
    hue, sat, mx = _hsv(c)
    lum = c @ np.array([0.2126, 0.7152, 0.0722], np.float32)
    metal = np.clip((orm[..., 2] - 0.35) / 0.3, 0, 1)
    skin = ((hue > 50) & (hue < 160) & (sat > 0.12) & (metal < 0.5)).astype(np.float32)
    rime = np.clip((_noise(rng, 30, 4, n) - 0.5) / 0.15, 0, 1)
    flesh = _gmap(np.clip(lum * 1.6, 0, 1), [(0.0, "#1e252c"), (0.4, "#5d6c78"), (0.8, "#9fb0bd"), (1.0, "#c7d3dc")])
    grey = lum[..., None] * np.array([0.92, 0.98, 1.06], np.float32) * 1.05 + 0.04
    grey = grey * (1 - 0.55 * rime[..., None]) + _hex("#dde6ee") * 0.55 * rime[..., None]
    iron = lum[..., None] * np.array([0.85, 0.93, 1.05], np.float32) * 0.9 + 0.06
    s, m = skin[..., None], metal[..., None]
    out = flesh * s + grey * (1 - s)
    out = out * (1 - m) + iron * m
    cold = (_cracks(rng, n, 22, region=0.45) * skin)[..., None]
    out = out * (1 - cold) + _hex("#70d4ff") * cold
    orm[..., 1] = np.clip(orm[..., 1] * (1 - 0.35 * rime), 0, 1)
    return np.clip(out, 0, 1).astype(np.float32)


RECOLORS = {"stubble": recolor_stubble, "burnt": recolor_burnt, "rime": recolor_rime, "frozen": recolor_frozen}


def apply_recolor(body, R):
    imgs = gc.source_images(body)
    rng = np.random.default_rng(sum(ord(ch) for ch in R["look"]))
    c, orm, nrm = _get(imgs["color"]), _get(imgs["orm"]), _get(imgs["normal"])
    if c.shape[0] > RECOLOR_SIZE:
        c, orm, nrm = (_down(a, RECOLOR_SIZE) for a in (c, orm, nrm))
        _set(imgs["normal"], nrm)
    out = RECOLORS[R["recolor"]](c, orm, rng)
    _set(imgs["color"], np.clip(out, 0, 1).astype(np.float32))
    _set(imgs["orm"], np.clip(orm, 0, 1).astype(np.float32))
    print(f"STATS {R['look']}: recoloured ({R['recolor']}) at {c.shape[0]}^2")


SHEET_CLIPS = ("idle", "walk", "run", "punch_jab", "punch_cross", "hit_chest", "death", "spell_shoot")


def render_previews(R, arm, actions):
    """Bind pose front / side / back / three-quarter, and one contact sheet: a row of six frames per
    clip in SHEET_CLIPS (three-quarter view, the ground grid shows foot contact)."""
    name, H = R["look"], R["height"]
    out_dir = vc.ensure_dir(PREVIEW_DIR)
    sc = bpy.context.scene
    cam = gc.setup_studio((384, 512))
    ground_mat = vc.material("preview_ground", "#77746d")
    bpy.ops.mesh.primitive_plane_add(size=12, location=(0, 0, 0))
    bpy.context.active_object.data.materials.append(ground_mat)
    line_mat = vc.material("preview_line", "#55524b")
    for k in range(-12, 13):
        bpy.ops.mesh.primitive_plane_add(size=1, location=(0, k * 0.5, 0.001))
        ln = bpy.context.active_object
        ln.scale = (6, 0.008, 1)
        ln.data.materials.append(line_mat)
    dist = 4.8 * H / 1.85
    tgt = (0, 0, H * 0.52)
    gc.reset_pose(arm)
    views = {"front": (0, -dist, H * 0.55), "side": (dist, 0, H * 0.55), "back": (0, dist, H * 0.6),
             "threequarter": (dist * 0.7, -dist * 0.7, H * 0.7)}
    made = set()
    for vname, loc in views.items():
        vc.aim_camera(cam, loc, tgt, lens=58)
        vc.render_to(os.path.join(out_dir, f"{name}_bind_{vname}.png"), stable=True)
        made.add(f"{name}_bind_{vname}.png")
    sc.render.resolution_x, sc.render.resolution_y = 192, 256
    tiles = []
    for clip in SHEET_CLIPS:
        act = actions[clip]
        vo_ual.use_action(arm, act)
        frames = int(round(act.frame_range[1]))
        for k in range(6):
            # Six frames spread over the clip, the last one at its end (death: lying).
            sc.frame_set(round(k * frames / 5))
            vc.aim_camera(cam, (dist * 0.72, -dist * 0.68, H * 0.62), (0, 0, H * 0.42), lens=64)
            tiles.append(vc.render_to(os.path.join(out_dir, f"_tmp_{name}_{clip}{k}.png")))
    vc.compose_sheet(tiles, os.path.join(out_dir, f"{name}_clips_sheet.png"), 6, stable=True)
    made.add(f"{name}_clips_sheet.png")
    vc.remove_files(tiles)
    gc.reset_pose(arm)
    sc.frame_set(0)
    vc.remove_files(sorted(os.path.join(out_dir, f) for f in os.listdir(out_dir)
                           if f.startswith(f"{name}_") and f not in made))


def add_shapes(body, arm, R):
    """fix1 silhouettes: the rest-pose reshape, then the pieces (monster_shapes) joined into the body
    with their UVs in the swatch strip; the body's UVs squeezed into u 0..BODY_W / TEX."""
    name, H = R["look"], R["height"]
    me = body.data
    uv = np.empty(len(me.loops) * 2, np.float32)
    me.uv_layers.active.data.foreach_get("uv", uv)
    uv = uv.reshape(-1, 2)
    uv[:, 0] = np.clip(uv[:, 0], 0.0, 1.0) * (ms.BODY_W / ms.TEX)
    me.uv_layers.active.data.foreach_set("uv", uv.ravel())
    if R.get("reshape"):
        ms.reshape(arm, body, R["reshape"])
    body_tris = vc.triangle_count(body)
    B = ms.Body(body, arm)
    spec = ms.SHAPES[R["shapes"]]
    rng = np.random.default_rng(11 + sum(ord(ch) * (k + 1) for k, ch in enumerate(name)))
    parts = spec["parts"](B, H, rng)
    rects = ms.swatch_rects(spec["swatches"])
    extra = ms.build_object(parts, rects, B.names, me.uv_layers.active.name, max_z=H - 0.002)
    extra_tris = vc.triangle_count(extra)
    ms.join_into(body, extra)
    V = np.array([v.co for v in body.data.vertices])
    print(f"STATS {name} shapes: body {body_tris} + pieces {extra_tris} = {vc.triangle_count(body)} tris, "
          f"body top {B.top:.3f}, top {V[:, 2].max():.3f} (authored {H})")


def clip_reach(body, arm, actions):
    """Lowest skinned vertex and farthest vertex from the pelvis per clip (every 3rd frame), for the
    content test's ground and reach limits."""
    out = {}
    for clip in MONSTER_CLIP_NAMES:
        act = actions[clip]
        vo_ual.use_action(arm, act)
        lo, far = 1e9, 0.0
        for f in range(0, int(round(act.frame_range[1])) + 1, 3):
            bpy.context.scene.frame_set(f)
            dg = bpy.context.evaluated_depsgraph_get()
            ev = body.evaluated_get(dg)
            m = ev.to_mesh()
            co = np.empty(len(m.vertices) * 3, np.float32)
            m.vertices.foreach_get("co", co)
            ev.to_mesh_clear()
            co = co.reshape(-1, 3)
            hip = np.array(arm.pose.bones["pelvis"].head)
            if float(co[:, 2].min()) < lo:
                lo = float(co[:, 2].min())
                lowest_rest = tuple(round(c, 2) for c in body.data.vertices[int(co[:, 2].argmin())].co)
            far = max(far, float(np.linalg.norm(co - hip, axis=1).max()))
        out[clip] = (round(lo, 3), f"{far:.3f} (lowest point at rest {lowest_rest})")
    gc.reset_pose(arm)
    bpy.context.scene.frame_set(0)
    return out


def build_monster(R, preview, debug_dir=None):
    name = R["look"]
    vc.reset_scene()
    s = gc.look_scale(R)
    arm, actions, settings = vo_ual.load_skeleton(s, lean=R["lean"], fit=R["fit"], helpers=gc.default_helpers(R),
                                                  sit=R["sit"])
    gc.vo_ual_actions.clear()
    gc.vo_ual_actions.update(actions)
    gc.reset_pose(arm)
    body, (src_tris, src_verts) = gc.load_source(R)
    if R.get("shapes") and vc.triangle_count(body) > SHAPED_TRIS + 200:
        # The slag walker's protected head and shoulder rock (everything above the neck line) keep
        # load_source's decimation at ~30k: a second, unprotected pass down to the shaped budget.
        n0 = vc.triangle_count(body)
        dec = body.modifiers.new("Decimate2", 'DECIMATE')
        dec.decimate_type = 'COLLAPSE'
        dec.ratio = SHAPED_TRIS / n0
        dec.use_collapse_triangulate = True
        bpy.context.view_layer.objects.active = body
        bpy.ops.object.modifier_apply(modifier=dec.name)
        gc.remove_slivers(body.data)
        print(f"STATS {name}: second decimation {n0} -> {vc.triangle_count(body)} triangles")
    if R.get("recolor"):
        apply_recolor(body, R)
    paths, metal, em_share = bake_textures(body, R)
    missing, bind_scale, summary = gc.rig(body, arm, R, metal)
    for kind, n, ctr, w in summary:
        print(f"RIGID {name} {kind}: {n} vertices at {ctr} -> {w}")
    gc.reset_pose(arm)
    if R.get("shapes"):
        add_shapes(body, arm, R)
    gc.slide_report(name, arm, actions, settings)
    if debug_dir:
        weights = [{g.group: g.weight for g in v.groups} for v in body.data.vertices]
        names = {g.index: g.name for g in body.vertex_groups}
        weights = [{names[k]: x for k, x in w.items()} for w in weights]
        gc.debug_renders(name, R, arm, body, weights, vc.ensure_dir(debug_dir))
    # Only the monster clips go into the file.
    for clip, act in list(actions.items()):
        if clip not in MONSTER_CLIP_NAMES:
            bpy.data.actions.remove(act)
            del actions[clip]
    gc.reset_pose(arm)
    if R.get("shapes") and "--reach" in vc.script_args():
        for clip, (lo, far) in clip_reach(body, arm, actions).items():
            print(f"REACH {name} {clip}: lowest {lo} far {far} (limit {0.85 * R['height']:.3f})")
    out_glb = os.path.join(OUT_DIR, f"{name}.glb")
    vc.export_glb(out_glb, [arm, body], textures=True)
    with open(os.path.join(OUT_DIR, f"{name}.anims.toml"), "w", newline="\n") as f:
        f.write(f"# Generated by tools/blender/gen_monsters.py ({name}). Clip settings for assetc.\n")
        f.write("# Clips: Quaternius Universal Animation Library (CC0, see tools/blender/vo_ual.py), fitted to\n")
        f.write("# this monster. move_speed = the clip's own planted-foot speed (the client scales playback\n")
        f.write("# to the monster's real ground speed: rate = speed / move_speed).\n")
        for clip, src, loop in MONSTER_CLIPS:
            lp, sp = settings[clip]
            f.write(f'\n[[clip]]\nname = "{clip}"\nloop = {"true" if lp else "false"}\nmove_speed = {sp}\n')
    V = np.array([v.co for v in body.data.vertices])
    tris = vc.triangle_count(body)
    frames = {clip: int(round(actions[clip].frame_range[1])) for clip in ("walk", "run")}
    meta = dict(height=round(float(V[:, 2].max()), 3), bottom=round(float(V[:, 2].min()), 3), tris=tris,
                verts=len(V), source_tris=src_tris, skeleton_scale=round(s, 4), bones=len(arm.data.bones),
                texture=R["texture"], emissive_strength=R["emissive"]["strength"],
                emissive_share=round(em_share, 4),
                clips=[c[0] for c in MONSTER_CLIPS],
                # Natural stride speeds: the walk / run clips' planted-foot ground speed (= their
                # move_speed) and the distance one full cycle covers at that speed.
                walk_speed=settings["walk"][1], run_speed=settings["run"][1],
                walk_cycle_m=round(settings["walk"][1] * frames["walk"] / vo_ual.FPS, 3),
                run_cycle_m=round(settings["run"][1] * frames["run"] / vo_ual.FPS, 3),
                unweighted=missing, bind_scale=str(bind_scale))
    print("STATS", name, "bones", meta["bones"], "tris", tris, "verts", len(V), "source_tris", src_tris,
          "height", meta["height"], "bottom", meta["bottom"], "unweighted", missing, "bind_scale", bind_scale,
          "scale", round(s, 4), "walk", meta["walk_speed"], "run", meta["run_speed"], "emissive_share",
          meta["emissive_share"])
    if preview:
        render_previews(R, arm, actions)
    return meta


def main():
    args = vc.script_args()
    names = [M["look"] for M in MONSTERS] or ["ash_thrall", "slag_walker", "ember_warden"]
    if "--only" in args:
        names = args[args.index("--only") + 1].split(",")
    if "--prepare" in args:
        prepare(args[args.index("--prepare") + 1], names)
        return
    if "--measure" in args:
        out = vc.ensure_dir(args[args.index("--measure") + 1])
        for n in names:
            measure(MONSTER[n], out)
        return
    preview = "--no-preview" not in args
    debug_dir = args[args.index("--debug") + 1] if "--debug" in args else None
    meta_all = {}
    if os.path.exists(META_PATH):
        with open(META_PATH, "r", encoding="utf-8") as f:
            meta_all = json.load(f)
    for n in names:
        meta_all[n] = build_monster(MONSTER[n], preview, debug_dir)
    meta_all = {k: meta_all[k] for k in MONSTER if k in meta_all}
    with open(META_PATH, "w", encoding="utf-8", newline="\n") as f:
        json.dump(meta_all, f, indent=1, sort_keys=True)
        f.write("\n")


if __name__ == "__main__":
    main()
