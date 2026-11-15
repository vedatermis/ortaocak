# VedatOnline asset pipeline

```
tools/blender/*.py  --(Blender 5.3, headless)-->  assets_src/  (.glb, .png, .toml; committed)
                                                       |
                         tools/assetc (C++, cgltf)     v
                                                    assets/  (.vmesh, .vanim, .vterrain, .png; git-ignored)
```

Runtime formats are specified byte-exactly in [ASSET_FORMATS.md](ASSET_FORMATS.md). Everything is
generated procedurally and deterministically (fixed seeds, no time, own PNG writer); no downloaded
or AI-generated content. The committed `assets_src` lets a machine without Blender build the game
assets with `assetc` alone.

## 1. Commands

```powershell
# Regenerate every source asset (needs Blender 5.3; ~1 minute; never touches a running Blender).
powershell -File tools/blender/build_all.ps1              # + preview renders in assets_src/previews
powershell -File tools/blender/build_all.ps1 -NoPreview   # sources only
#   -Blender <path> or $env:VO_BLENDER overrides "C:\Program Files\Blender Foundation\Blender 5.3\blender.exe"

# Compile assets_src -> assets (builds assetc first if needed). Incremental; -Force rebuilds all.
powershell -File tools/build-assets.ps1                   # Release assetc
powershell -File tools/build-assets.ps1 -Config Debug -Force

# Single generators
blender --background --factory-startup --python tools/blender/gen_character.py -- --race kargan
blender --background --factory-startup --python tools/blender/gen_props.py -- --only well,house
```

`assetc` (build/x64-*/bin/assetc.exe):

| Command | Does |
|---|---|
| `assetc mesh <in.glb> <out.vmesh> [--root <src_root>]` | one mesh (all mesh nodes merged) |
| `assetc anim <in.glb> <outdir> [--quantize]` | one `<clip>.vanim` per glTF animation |
| `assetc terrain <desc.terrain.toml> <out.vterrain>` | terrain from a description + PNG maps |
| `assetc info <file>` | header, counts, bounds, submeshes, bones, clips, spawns, placements |
| `assetc build <assets_src> <assets> [--force] [--quantize]` | whole tree, incremental, exit 2 on any failure |
| `assetc` (no args) | version + usage, exit 0 |

`build` rules (sorted, deterministic): `**/*.glb|*.gltf` → `<rel>/<stem>.vmesh` plus
`<rel>/<stem>/<clip>.vanim` if it has animations; `**/<name>.terrain.toml` → `<rel>/<name>.vterrain`
(after all meshes; every placement mesh and layer texture must then exist in the output);
`**/*.png` copied verbatim, except under `previews/` and in directories holding a `*.terrain.toml`.
An item is skipped when its stamp (`assets/.assetc/<rel>.stamp`, listing the outputs) is newer than
all its inputs (source, `.anims.toml` sidecar, terrain maps) and than `assetc.exe`, and all outputs
exist. Same input → identical bytes.

ctest runs `assets.build` (converts the committed `assets_src` into `build/<preset>/tests/test_assets`)
as a fixture for the `content.*` tests.

## 2. Conventions

| Topic | Rule |
|---|---|
| Units | metres, seconds, radians |
| Engine axes | left-handed, +X right, +Y up, +Z forward (PROTOCOL.md §5.4) |
| Blender authoring | Z up, the asset **faces −Y** (Blender front view), its left side is **+X**, feet/base on Z = 0 |
| glTF | exporter `export_yup=True` (asset faces +Z, left = +X), assetc then mirrors X |
| Origin | ground point below the centre of the footprint (characters: between the feet) |
| Winding | Blender/glTF: CCW front faces; engine: **CW** front faces (assetc swaps indices) |
| Colours | props/terrain: flat materials (Principled base colour), baked by assetc into sRGB vertex colours; characters: one PBR material, base colour + tangent-space normal (OpenGL +Y) + ORM maps, 2048² each (`<look>.png`, `<look>_n.png`, `<look>_orm.png`; see `tools/blender/README.md`) |
| Textures | PNG, sRGB, power-of-two, tileable for terrain; referenced by asset path |
| Names | `snake_case`; asset paths relative to `assets/`, forward slashes |
| Triangle budgets | character ≤ 30k (≈38k-triangle sources decimated, ≈15k vertices, 16-bit indices); small prop < 300; building/tree < 1k |

### Coordinate conversion (glTF → engine)

glTF is right-handed with the asset facing +Z and its left at +X; the engine is left-handed with +X
to the right. Mirroring X (S = diag(−1, 1, 1)) maps one to the other without turning the asset:

| Quantity | Conversion |
|---|---|
| position, normal, tangent.xyz | (x, y, z) → (−x, y, z) |
| tangent.w | −w (bitangent = `cross(n, t.xyz) * w` stays the mirrored glTF bitangent) |
| quaternion | (x, y, z, w) → (x, −y, −z, w) |
| matrix (node, inverse bind) | M → S·M·S |
| triangle | (i0, i1, i2) → (i0, i2, i1) |
| animation keys | as translation / quaternion / scale above |

Static meshes have every node's world transform baked in. Skinned meshes ignore the mesh node
transform (glTF rule); a non-joint parent of the root joint (Blender's armature object) is folded
into the root bone. Proven in `tests/assets/GltfConversionTests.cpp` with hand-computed answers.

## 3. Characters (`tools/blender/gen_character.py`, `vo_ual.py`)

Characters are **textured source meshes rigged to a shared library skeleton** and animated with
library clips. **Three looks per race**, each a separate character file (no hair/face variant
submeshes: every submesh has `variantGroup` 0, so `isSubmeshVisible(i, face, hair)` is always true).

### Looks and output names

| Look | Source | Height | Skeleton scale | Notes |
|---|---|---|---|---|
| `aydor_0` | young man, brown hood | 1.77 m | 0.967 | heat weights at bind scale 10 |
| `aydor_1` | woman, auburn braid on the chest, green cloak | 1.70 m | 0.929 | heat weights at bind scale 1; braid region → Head/neck/spine_03 |
| `aydor_2` | grey-bearded veteran, red cloak | 1.77 m | 0.967 | heat weights at bind scale 1; **the source file's texture is untextured grey** (generator output, not a pipeline fault): replace `aydor_2_tripo.glb` and rerun |
| `kargan_0` | orc-kin man, fur mantle | 1.93 m | 1.055 | heat weights at bind scale 1 |
| `kargan_1` | orc-kin woman, back braid, pale fur mantle | 1.86 m | 1.017 | braid region → Head/neck/spine_03 |
| `kargan_2` | bald elder, braided grey beard, dark fur mantle | 1.93 m | 1.055 | beard region → Head/neck/spine_03 |

Per look `<look>` = `<race>_<n>` (n = 0, 1, 2), the build produces:

| Source (`assets_src/characters/`) | Runtime (`assets/characters/`) |
|---|---|
| `<race>_<n>.glb` | `<race>_<n>.vmesh` (submesh texture `characters/<race>_<n>.png`) and `<race>_<n>/<clip>.vanim` (26 clips) |
| `<race>_<n>.png` | `<race>_<n>.png` (1024², copied) |
| `<race>_<n>.anims.toml` | (clip loop flags and move speeds, read by assetc) |

**Legacy names (temporary):** `gen_character.py` also writes `<race>.glb` and `<race>.anims.toml` as
byte copies of look 0, so `characters/<race>.vmesh` and `characters/<race>/<clip>.vanim` still exist
for clients that only know the un-suffixed names (the copy references `<race>_0.png`, so the
converted mesh is byte-identical to `<race>_0.vmesh`; a content test checks this). Drop the copies
(the block marked LEGACY in `gen_character.build_look`, the legacy test, and the two names in
`tools/build-assets.ps1`'s required list) once every client loads looks by index.

### Sources

| What | Where | Origin / licence |
|---|---|---|
| Aydor / Kargan meshes | `tools/blender/source_meshes/<race>_<n>_tripo.glb` (+ the A-pose concept image `<race>_<n>_apose.png`) | generated with Tripo P2 via Higgsfield from the project's own A-pose concept images; owned by the project |
| Skeleton + clips | `tools/blender/third_party/quaternius_ual/UAL1_Standard.glb` (+ `License.txt`, `README.txt`) | Quaternius "Universal Animation Library" (Standard), CC0 1.0 |

Both live outside `assets_src` on purpose: `assetc build` converts every `.glb` under `assets_src`
(the raw Tripo file has an embedded texture, which assetc rejects, and the UAL file has 65 bones and
a mannequin). The root-motion twin `UAL1_Standard_RM.glb` is not committed; it was only used to
measure ground speeds (`tools/blender/measure_ual_speeds.py -- <path>`).

### Flow (`gen_character.py --look <race>_<n>`, or `--race <race>` for its three looks)

Every look is its own run with its own skeleton fit and its own baked clips (joint positions, leg
IK and foot pinning depend on the mesh); the exported skeleton (53 bone names, order, hierarchy) is
the same for all six. The per-look settings are data in `gen_character.LOOKS` (source, height,
scale, `fit`, `lean`, weight rules); nothing else is look-specific.

1. **Skeleton** (`vo_ual.load_skeleton`): import the UAL glb at 30 fps (its keys are 30 fps), drop
   the stock meshes, the unused actions and the 12 `*_leaf_*` bones (→ **53 bones**, the client's
   palette holds 64), close the two open loops (Idle_Loop, Jog_Fwd_Loop: last 8 frames blended into
   the first), re-bind in an A-pose (upper arms 50° down from the T-pose; their keys are rewritten
   B' = P⁻¹B so every clip keeps its absolute transforms), scale uniformly (bones + location keys)
   to the look (0.929-1.055 of the 1.83 m mannequin, chosen so the hip height matches the mesh),
   re-time the walk (see Clips), fit the joints to the mesh
   (`fit_joints`: shoulder positions and arm lengths, hip width; pure subtree translations so the
   clips still apply; leg lengths stay the library's because the clips' foot contacts depend on
   them), optional constant posture (`lean`: Kargan spine_02 +4°, neck −4°), then **pin the feet**
   (`plant_feet`): in walk/run/crouch_walk the ball of each foot is held still during ground
   contact by a two-bone leg IK written back into the thigh/calf/foot keys (the library jog's stance
   foot moved at 5.9 m/s against a 5.36 m/s body, and the walk's toe dragged forward at push-off).
2. **Mesh** (`load_source`): import, rotate (`rot_z`) so it faces −Y with its left at +X, scale to
   the race height, feet on Z = 0, centred; merge doubles, dissolve degenerate edges, drop slivers
   (< 4e-7 m²) and loose vertices, outward normals, smooth shading. UVs live on the loops and survive.
3. **Texture** (`bake_texture`): the source's 4096² base colour is downscaled to `texture` (1024²; 2048² gave no visible gain at game distances and doubled the size) and
   written by the deterministic PNG writer to `assets_src/characters/<race>.png`; the material is
   rebuilt as one base-colour texture (glTF `baseColorTexture`, URI `<race>.png`). assetc stores the
   asset path `characters/<race>.png` in the submesh and the build copies the PNG. No vertex
   colours (the client then uses white).
4. **Rig** (`rig`): a copy of the skeleton is posed like the mesh (arm and leg directions from the
   `fit` targets) and made its rest pose; the mesh is bound there with Blender's automatic (bone
   heat) weights (tried at bind scales 1, 10, 3, 30 until ≥ 85 % of the vertices get weights — the
   solver's tolerances are absolute; it gives up on the whole mesh when it meets polygons with
   collinear corners, which is why `load_source` re-triangulates after dissolving degenerate
   edges). If every scale still fails, a **proxy** is bound instead (fallback, unused today): a 12 mm voxel remesh of the mesh, largest shell only, heat-weighted,
   and its weights transferred to the real mesh from the nearest proxy surface point. Weights are
   then cleaned (`clean_weights`): rigid hair/beard `regions` (a box in the normalised source mesh
   whose vertices take weights interpolated over height from a chain such as Head → neck_01 →
   spine_03, so braids and beards ride on the head/chest and never swing through the body), finger weights merged
   into rigid hands, no arm weights inside the torso/skirt column (`torso_column`: (z_max, half
   width) bands) so tunic flaps next to the hanging hands do not follow them, upper-arm share of the
   shoulder cloth/fur capped (`shoulder_cap`), vertices the heat solver missed take the weights of
   the nearest weighted vertex, at most 4 influences, normalised. The copy is then posed back to the
   shared rest, the deformation is applied to the mesh, and the mesh is skinned to the clean
   skeleton.
5. **Export**: `<look>.glb` (armature + `body`, 26 clips; the exporter's animation-size
   optimisation drops keys equal to both neighbours, which halves the file — the converted clips
   differ from an unoptimised export by < 1e-4), `<look>.anims.toml` (loop flags, `move_speed`),
   the legacy copies for look 0, previews; prints `STATS` (bones, triangles, height, unweighted
   count, bind scale) and `SLIDE` (planted frame pairs and per-frame slide for walk/run, must be 0.00 mm).

### Clips

| Clip | UAL action | Loop | Frames (30 fps) | move_speed (Aydor / Kargan) |
|---|---|---|---|---|
| idle | Idle_Loop | yes | 76 | 0 |
| walk | Walk_Loop, re-timed 40 → 30 frames, stride ×1.10 | yes | 31 | 1.33-1.52 m/s (per look, see below) |
| run | Jog_Fwd_Loop | yes | 29 | 5.48-6.23 m/s (per look) |
| sword_idle, sword_attack | Sword_Idle, Sword_Attack | yes, no | 51, 47 | 0 |
| punch_jab, punch_cross | Punch_Jab, Punch_Cross | no | 27, 31 | 0 |
| spell_enter / idle / shoot / exit | Spell_Simple_* | no / yes / no / no | 17, 64, 16, 14 | 0 |
| hit_chest, hit_head, death | Hit_Chest, Hit_Head, Death01 | no | 11, 14, 73 | 0 |
| jump_start / loop / land | Jump_* | no / yes / no | 41, 76, 39 | 0 |
| roll | Roll | no | 45 | 0 |
| sit_enter / idle / exit | Sitting_Enter / Sitting_Idle_Loop / Sitting_Exit | no / yes / no | 40, 51, 32 | 0 |
| interact, pickup | Interact, PickUp_Table | no | 61, 26 | 0 |
| idle_talking | Idle_Talking_Loop | yes | 89 | 0 |
| crouch_idle, crouch_walk | Crouch_Idle_Loop, Crouch_Fwd_Loop | yes | 89, 61 | 0 / measured |

`move_speed` is the clip's own planted-foot speed (scaled with the look); the client plays the clip
at rate = ground speed / move_speed (clamped 0.3–3), so feet stay planted.

**Walk at 1.4 m/s.** The library Walk_Loop is a 0.98 m/s stroll (40 frames, 1.31 m stride, 90
steps/min at scale 1) and would need rate ≈ 1.43-1.5 at the game's 1.4 m/s walk (133 steps/min
with a short stride: a scurry). Walk_Formal_Loop was measured too: its legs are identical to
Walk_Loop (same 40 frames, same stance speed 0.979 m/s, same 0.68 m foot separation; only the
arms differ), so it needs the same rate. The walk is therefore **re-timed to 30 frames** (resampled
linearly from the library keys) and its **stride lengthened by 10 %** (`vo_ual.stretch_stride`: each
foot's position along the walking axis scaled about its cycle mean through the leg IK, heights kept,
pelvis lowered 2 cm × scale so the longer stride stays within reach — max reach 0.989 of the leg),
then the feet are pinned as before. Result: 120 steps/min, 1.33-1.52 m stride, playback rate at
1.4 m/s between 0.92 (tall Kargan: a slower cadence, as for a tall walker) and 1.05 (`aydor_1`).

| Look | walk move_speed | rate @ 1.4 m/s | run move_speed | rate @ 5.5 m/s |
|---|---|---|---|---|
| aydor_0 | 1.3865 | 1.01 | 5.705 | 0.96 |
| aydor_1 | 1.3322 | 1.05 | 5.4808 | 1.00 |
| aydor_2 | 1.3849 | 1.01 | 5.7056 | 0.96 |
| kargan_0 | 1.5094 | 0.93 | 6.2254 | 0.88 |
| kargan_1 | 1.4559 | 0.96 | 6.0009 | 0.92 |
| kargan_2 | 1.5094 | 0.93 | 6.2254 | 0.88 |

Natural root-motion speeds measured from the RM pack: Walk_Loop 0.975, Jog_Fwd_Loop 5.357,
Sprint_Loop 8.25, Crouch_Fwd_Loop 0.75 m/s.

### Replacing a mesh / adding a look or race

* **Replace a mesh / add a look**: drop the textured glb at
  `tools/blender/source_meshes/<race>_<n>_tripo.glb` (A-pose, one material with a base-colour
  texture). Render it front/side with a 10 cm grid (any orthographic view of the normalised mesh),
  read the left shoulder, elbow, wrist, hip, knee and ankle positions, add/adjust its entry in
  `gen_character.LOOKS` (`scale` = hip height / 0.931, `fit`, `rot_z` if it does not face −Y,
  `torso_column` / `shoulder_cap` / `back` if cloth follows the arms, `regions` for braids and
  beards), add a `--look` step to `build_all.ps1` and the look to `kLooks` in
  `tests/assets/content/ContentTests.cpp`, run `build_all.ps1` and look at the previews (bind views,
  head close-ups, idle/walk/run/sword_attack/sit contact sheets, every clip's middle frame).
* **Next mesh generation (semi-realistic, PBR)**: the importer already copes with ~30-40k triangle
  sources (decimated by edge collapse to the look's `max_tris`, default 13000, the current budget;
  raise it together with the content test when the budget changes — 16-bit indices allow ~65k
  vertices) and with PBR materials: only the image feeding the Principled Base Color is exported
  (downscaled to `texture`; a 2048² source is fine), normal / metallic-roughness / occlusion images
  are ignored and stay in the source file (tested with a synthetic 47k-triangle, 2048² base colour +
  normal + metallic-roughness glb). Shipping those maps needs (proposal, not implemented):
  * `.vmesh` v2: optional per-vertex tangent (snorm16 × 4, xyz + handedness w; vertex flag bit,
    stride +8 B), taken from glTF TANGENT (export with `export_tangents=True`) or generated with
    MikkTSpace in assetc; the X mirror negates tangent.w (already specified in §2).
  * Submesh material: next to `texture` (base colour, sRGB) two more asset paths, `normal_texture`
    (tangent-space, glTF/OpenGL convention +Y up, linear) and `orm_texture` (linear; R occlusion,
    G roughness, B metallic — glTF's metallicRoughness channels with occlusion packed into R), plus
    `normal_scale`, `roughness_factor`, `metallic_factor` (f32). Empty path = flat normal / factors
    only. v1 files read as before.
  * Files: `characters/<look>.png` (base colour), `characters/<look>_n.png`, `characters/<look>_orm.png`;
    props `props/<name>_n.png`, `props/<name>_orm.png`. assetc reads the glTF `normalTexture`,
    `metallicRoughnessTexture`, `occlusionTexture` URIs (relative, under assets_src, power of two,
    same rules as base colour) and copies them; gen_character writes them with the deterministic PNG
    writer. The client binds them as linear (non-sRGB) textures in two more slots, for skinned and
    static meshes alike.
* **Another clip**: add `(game name, UAL action, loop)` to `vo_ual.CLIPS` (and to `PLANTED` if it is
  locomotion with flat-foot contacts), extend the clip list in `tests/assets/content/ContentTests.cpp`,
  run `build_all.ps1`. Another library pack must use the same bone names.
## 4. Props (`tools/blender/gen_props.py`)

Fourteen static props built from lofts, boxes and lumpy ellipsoids, flat shaded:
`tree_oak, tree_pine, rock_a, rock_b, fence (2 m segment along X), well, market_stall, lamp_post,
crate, barrel, signpost, house, watchtower, ant_tasi`.

**Ant Taşı** (`ant_tasi`, docs/LORE.md): the cracked oath stone of Ortaocak, ~1.1k triangles. A
weathered grey slab 2.5 m wide, 0.9 m thick and ~4.5 m tall on a two-step 3.6 × 2.4 × 0.42 m
plinth (4.92 m overall), built as two lofted halves that meet below a third of the height and part
above it: a vertical crack from the top down to 1/3, its walls dark at the rim and ember-tinted in
the middle, with a thin ember sheet deep inside (`ember` / `ember_hot` palette entries: plain vertex
colours, no emissive). Front (−Y, engine +Z): neat ruled lines of dashes between two border rules
(Aydor's written oath); back: columns of deep dark notches — wide bars, paired notches, tall strokes
(Kargan's carved oath). Abstract marks only. Previews `props/ant_tasi_{front,back,top}.png` show it
next to a 1.8 m marker. To add one: write `def my_prop(p)` using the `Prop`
helpers (`p.m("colour")` picks a palette entry from `PAL`), register it in `PROPS`, add its footprint
to `FOOTPRINT` in `gen_terrain.py` if it will be placed, run the generators.

## 5. Terrain (`tools/blender/gen_terrain.py`, zone 1)

* 513² heightmap over 512 m (1 m spacing), 16-bit PNG; stored height range [4, 92] m.
* Village plateau (≈20 m) around (256, 256) with a dirt plaza; rolling fbm hills; a north–south road
  and an east branch (Catmull-Rom, 6.4 m wide, graded) running through passes in steep, impassable
  ridges along the borders; 6 m outer margin blocked.
* Walk mask (512², 1 m cells): blocked where slope > 36°, in the margin, and under blocking prop
  footprints grown by a 0.35 m character radius.
* Splat (512² RGBA): R grass, G dirt road / plaza, B rock (steep or high), A dry grass (noise
  patches and higher ground). Layer textures (`gen_textures.py`): 512² tileable PNGs from domain-warped
  periodic value noise, kept low-contrast so the 6 / 4 / 8 / 6 m tiles (sampled at two scales by the
  shader) do not read as a repeat; the dirt has fine low-contrast grit instead of bright pebbles.
* Ant Taşı at (249.0, 263.5) in the north-west quarter of the plaza (off both roads, 7 m from the
  N–S road's centre line), its Aydor face turned to the Aydor spawn; placed first so nothing else
  comes within its clearance; footprint blocked, ground around it walkable.
* Spawns: `spawn_aydor` (238, 271), `spawn_kargan` (277, 247) facing the stone, 45.8 m apart on the
  plateau (y ≈ 20.0 m), 13.3 / 32.5 m from the stone, nearest house wall 12.8 / 15.4 m
  (`check_layout` requires ≥ 8 m, 40–60 m apart, ≤ 40 m to the stone; the Kargan spawn used to be
  at (281, 236), 5.5 m from a house wall, so the follow camera started inside it).
* 598 placements: the Ant Taşı, 10 houses ringed around the plaza facing it, 3 market stalls with crates/barrels,
  the well, 18 lamp posts along the roads, 4 signposts, 3 watchtowers at the passes, a fenced
  paddock, ~390 trees in groves and singles, ~110 rocks mostly on slopes. Nothing within road width
  + clearance, nothing within 4 m of a spawn.

### Terrain description schema (`<name>.terrain.toml`)

```toml
[terrain]
heightmap = "zone1_height.png"   # relative to the toml; 16- or 8-bit gray, N x N, N = 2^k+1; pixel (col i,row j) = sample at x=i, z=j
world_size = 512.0
height_min = 4.0                 # metres for pixel value 0
height_max = 92.0                # metres for pixel value 65535
splat = "zone1_splat.png"        # RGBA8 S x S
walk_mask = "zone1_walk.png"     # gray M x M, >= 128 walkable

[[layer]]                        # exactly 4, in splat channel order
texture = "textures/terrain/grass.png"   # asset path
tile_size = 6.0                  # metres per repeat

[[spawn]]                        # y = heightAt(x, z); must be inside and walkable
name = "spawn_aydor"
x = 238.0
z = 271.0
yaw = 2.356

[[object]]
mesh = "props/house.vmesh"       # asset path of a .vmesh (must exist after the mesh pass)
x = 1.0
z = 2.0
yaw = 0.0                        # optional, default 0
scale = 1.0                      # optional, default 1
y_offset = 0.0                   # optional, added to heightAt(x, z)
blocks = true                    # optional, default true
```

Unknown keys are errors.

## 6. Previews

Every generator renders previews (EEVEE, Standard view transform, no dither, back-face culling on so
flipped faces show as holes) into `assets_src/previews/` (never copied to `assets/`). All PNGs
(textures, maps, previews) go through `vo_common.write_png`: no metadata, zlib level 9, per-row PNG
filter chosen by the libpng heuristic (lossless; ~30 % smaller than unfiltered rows).

* `characters/<look>_bind_{front,side,back,threequarter}.png` (rest pose, PBR material under a
  neutral studio light: grey world, key / fill / rim suns), `<look>_head.png` (idle head: front,
  three-quarter, side, back), `<look>_<clip>_sheet[_3q].png` for walk, run, sword_attack, death,
  roll, sit_ground_enter, sit_ground_idle (6 frames over the clip, 0.5 m grid lines; side and
  three-quarter), `<look>_clips_sheet.png` (every clip at its middle frame)
* `props/<name>.png`, `props/props_sheet.png`, `props/ant_tasi_{front,back,top}.png`
* `textures/terrain_layers.png` (each layer tiled 2×2 to show the seams)
* `terrain/zone1_topdown.png` (orthographic, +Z up, +X right), `zone1_oblique.png`,
  `zone1_village.png`, `zone1_walkmask.png` (red = blocked, blue/yellow = Aydor/Kargan spawns,
  dots = objects; image row = z, so +Z points down in this one)

Generators also print checks: triangle counts, per-frame foot slide during flat stance (must be
0.00 mm), terrain walkability and spawn heights.
