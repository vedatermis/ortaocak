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
| Colours | flat materials (Principled base colour), baked by assetc into sRGB vertex colours |
| Textures | PNG, sRGB, power-of-two, tileable for terrain; referenced by asset path |
| Names | `snake_case`; asset paths relative to `assets/`, forward slashes |
| Triangle budgets | character (one appearance) 1.5–4k; small prop < 300; building/tree < 1k |

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

## 3. Characters (`tools/blender/gen_character.py`)

* One script, two races (`RACES` table): **Aydor** 1.75 m, lean, tunic + belt, trousers, knee
  boots; **Kargan** 1.95 m, very broad shoulders, fur mantle and shoulder pads, open leather vest,
  heavy bracered forearms, stocky legs, kilt, fur-wrapped boots.
* Body = lofted rings (torso, limbs, hands, boots) + ellipsoid head, with explicit smooth weights
  that blend across each joint (shoulder → clavicle, hip → pelvis, elbows, knees, wrists, ankles).
  The tunic/kilt is a double-walled skirt weighted to pelvis + the thigh on its side.
* Skeleton (22 bones, same names and hierarchy for both races):
  `root, pelvis, spine, chest, neck, head, clavicle_l/r, upperarm_l/r, forearm_l/r, hand_l/r,
  thigh_l/r, shin_l/r, foot_l/r, toe_l/r`. A-pose bind (arms 40–42° from vertical).
* **Appearance variants**: separate mesh objects `hair_0..3` and `face_0..3` (eyes, brows, nose,
  mouth, facial hair / tattoos) skinned to `head`. assetc turns the node name into
  `variantGroup` (1 hair, 2 face) + `variantIndex`; the runtime draws group 0 plus the submesh of
  each group whose index equals the character's `hair` / `face` (`MeshData::isSubmeshVisible`).
* Clips (30 fps, in place, every frame keyed, loops closed: last key = first key):

| Race | Clip | Frames | Cycle | Speed | Stride (2 steps) | Step |
|---|---|---|---|---|---|---|
| Aydor | idle | 60 | 2.00 s | 0 | – | – |
| Aydor | walk | 24 | 0.800 s | 2.0 m/s | 1.600 m | 0.80 m |
| Aydor | run | 20 | 0.667 s | 5.5 m/s | 3.667 m | 1.83 m |
| Kargan | idle | 60 | 2.00 s | 0 | – | – |
| Kargan | walk | 26 | 0.867 s | 2.0 m/s | 1.733 m | 0.87 m |
| Kargan | run | 21 | 0.700 s | 5.5 m/s | 3.850 m | 1.93 m |

  Legs use analytic two-bone IK on procedural foot tracks: during stance the foot moves backwards
  at exactly the clip speed (heel rocker → flat → roll about the ball joint), so planted feet do
  not slide; if a pose would over-extend a leg the pelvis drops instead. Arms swing opposite to the
  legs; the run has a flight phase, forward lean and ~85° elbows. `move_speed` is stored per clip
  (`<race>.anims.toml` sidecar) so the client can scale playback: rate = speed / moveSpeed.
* Bone axes in Blender: limb/spine bones have local Z forward, feet/toes local Z up, so a positive
  rotation about local X is always flexion/forward swing (useful for new clips).

### Adding a clip

1. Add a branch in `pose_frame()` (or a new gait in `GAITS`) and its frame count in `RACES`.
2. Add it to the loop in `make_actions()` and to the sidecar writer in `main()` (`loop`,
   `move_speed`).
3. Run `build_all.ps1`, look at the contact sheets, run `build-assets.ps1` and ctest (the content
   tests check seams, continuity and foot planting for walk/run).

### Adding a character race

Add a `RACES` entry (joint heights, ring profiles, frame counts), a palette in `PALETTES`, a
`body_<race>()` builder and hair/face variants, then a step in `build_all.ps1`.

## 4. Props (`tools/blender/gen_props.py`)

Thirteen static props built from lofts, boxes and lumpy ellipsoids, flat shaded:
`tree_oak, tree_pine, rock_a, rock_b, fence (2 m segment along X), well, market_stall, lamp_post,
crate, barrel, signpost, house, watchtower`. To add one: write `def my_prop(p)` using the `Prop`
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
  patches and higher ground). Layer textures (`gen_textures.py`): 512² tileable periodic-noise PNGs,
  tile sizes 6 / 4 / 8 / 6 m.
* Spawns: `spawn_aydor` (238, 271), `spawn_kargan` (281, 236), 55 m apart on the plateau (y ≈ 20.0 m).
* 597 placements: 10 houses ringed around the plaza facing it, 3 market stalls with crates/barrels,
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

Every generator renders previews (EEVEE, Standard view transform, back-face culling on so flipped
faces show as holes) into `assets_src/previews/` (never copied to `assets/`):

* `characters/<race>_bind_{front,side,threequarter}.png`, `<race>_variants.png` (hair i + face i),
  `<race>_{idle,walk,run}_sheet.png` (side view, 6 frames over a cycle, 0.5 m grid lines) and
  `<race>_{walk,run}_sheet_3q.png`
* `props/<name>.png`, `props/props_sheet.png`
* `textures/terrain_layers.png` (each layer tiled 2×2 to show the seams)
* `terrain/zone1_topdown.png` (orthographic, +Z up, +X right), `zone1_oblique.png`,
  `zone1_village.png`, `zone1_walkmask.png` (red = blocked, blue/yellow = Aydor/Kargan spawns,
  dots = objects; image row = z, so +Z points down in this one)

Generators also print checks: triangle counts, per-frame foot slide during flat stance (must be
0.00 mm), terrain walkability and spawn heights.
