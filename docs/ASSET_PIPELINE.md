# VedatOnline asset pipeline

```
tools/blender/*.py  --(Blender 5.3, headless)-->  assets_src/  (.glb, .png, .toml; committed)
                                                       |
                         tools/assetc (C++, cgltf)     v
                                                    assets/  (.vmesh, .vanim, .vterrain, .png; git-ignored)
```

Runtime formats are specified byte-exactly in [ASSET_FORMATS.md](ASSET_FORMATS.md). Every source
asset is produced deterministically (fixed seeds, no time, own PNG writer) from committed inputs:
procedural geometry, generated meshes (characters, environment props: `tools/blender/source_meshes/`)
and scanned CC0 textures (`tools/blender/source_textures/`); THIRD_PARTY.md lists their origin. The
committed `assets_src` lets a machine without Blender build the game assets with `assetc` alone.

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
blender --background --factory-startup --python tools/blender/gen_props.py -- --only well,house_a
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
| Colours | characters and props: textured PBR materials (base colour + normal + ORM, props optionally emissive); terrain: four PBR layers; flat Principled colours are still baked into sRGB vertex colours by assetc |
| Textures | PNG (base colour / emissive sRGB, normal / ORM linear), power-of-two, tileable for terrain; referenced by asset path; compiled to `.vtex` |
| Names | `snake_case`; asset paths relative to `assets/`, forward slashes |
| Triangle budgets | character ≤ 13k (generated meshes ≈ 12k); props per `gen_props.PROPS` (house 14k + far LOD 2.6k, tree 10-12k + 1.2-1.6k, small props 2.4-5k) |

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
* **Semi-realistic generation (PBR) — implemented** (formats in ASSET_FORMATS.md §2 and §5, shading
  in CLIENT_RENDERING.md §4.1). The Blender side decimates sources to the look's `max_tris`
  (default 13000; raise it together with the content test when the budget changes); assetc itself
  takes any size (32-bit indices above 65535 vertices, tested end to end). The **material contract**
  with the art pipeline, as implemented:
  * Files, for look n of a race, all next to the glb in `assets_src/characters/`, power of two
    (2048² expected), PNG: `<race>_<n>.png` base colour (sRGB, RGB or RGBA), `<race>_<n>_n.png`
    tangent-space normal map (**OpenGL convention, +Y = image up**, as Tripo's "NormalGL" and
    Blender bake; linear; RGB, Z is ignored and reconstructed), `<race>_<n>_orm.png` (linear;
    **R occlusion, G roughness, B metallic**). Props may do the same with `props/<name>.png`,
    `props/<name>_n.png`, `props/<name>_orm.png`.
  * The glb has **one material** whose `pbrMetallicRoughness.baseColorTexture` → `<race>_<n>.png`,
    `normalTexture` → `<race>_<n>_n.png` (`scale` → `normalScale`, default 1),
    `metallicRoughnessTexture` **and** `occlusionTexture` → the same `<race>_<n>_orm.png`
    (`strength` → `occlusionStrength`), `metallicFactor` / `roughnessFactor` multiply B / G
    (write them explicitly: glTF defaults both to 1 = full metal), all `texCoord` 0, external URIs
    (relative, under assets_src), no `KHR_texture_transform`.
  * Tangents: glTF `TANGENT` is used when every primitive has it (export with
    `export_tangents=True`; xyz mirrored and w negated by the X mirror). Primitives with a normal map
    and no `TANGENT` get **MikkTSpace** tangents in assetc (the reference implementation, vcpkg port
    `mikktspace`, run on UV `(u, 1 − v)` = Blender's bake space), vertices split where corners'
    tangents differ and re-welded deterministically. Either way the bitangent `cross(N, T)·w` points
    image-up, which is what the OpenGL-convention maps need (proven in
    `tests/assets/GltfPbrTests.cpp`); the `.vmesh` stores snorm16 tangents.
  * Output: the `.vmesh` submesh references the three PNG asset paths; `assetc build` copies the PNGs
    (as for every PNG) and the glb item writes `<race>_<n>.vtex`, `<race>_<n>_n.vtex`,
    `<race>_<n>_orm.vtex` next to them (≤ `--max-texture`, default 2048; BC1 / BC5 / BC1 with full
    mips; `--no-compress` stores RGBA8), which the client loads. The glb's build stamp lists the PNGs
    as `input:` lines, so editing a map rebuilds its `.vtex`. `assetc mesh <in> <out.vmesh>
    --out-root <assets>` does the same for one file.
  * Other cases assetc handles: separate occlusion and metallic-roughness images are packed into a
    generated `<stem>_orm.png` next to the `.vmesh` (R from the occlusion image); metallic-roughness
    without occlusion → R = 1 (or, with `--assume-orm`, the image's R is taken as occlusion — raw
    Tripo output wires its "ORM" image only to metallicRoughness); images embedded in the glb (data
    URIs or buffer views, PNG or JPEG) are written as `<stem>.png`, `<stem>_n.png`, `<stem>_orm.png`
    next to the `.vmesh` (a generated name that collides with a source PNG is an error).
  * Also handled since the environment pass (`.vmesh` v3, ASSET_FORMATS.md §2): `alphaMode MASK` + `alphaCutoff`
    (alpha-tested cards) and `emissiveTexture` / `emissiveFactor` / `KHR_materials_emissive_strength`.
    Not supported (rejected): `texCoord` 1, texture transforms.
  * Deviations from the proposal: the snorm16 tangent stream is a new vertex bit (`0x20`) next to
    the old f32 one rather than a re-definition of bit `0x02` (v1 layouts stay valid as they are);
    the material adds `occlusionStrength` (glTF `occlusionTexture.strength`); untextured,
    vertex-coloured primitives keep their roughness/metallic per submesh (merged only when equal);
    textures ship as `.vtex` next to the PNG instead of being bound as PNGs (load time and GPU
    memory, CLIENT_RENDERING.md §6).
* **Another clip**: add `(game name, UAL action, loop)` to `vo_ual.CLIPS` (and to `PLANTED` if it is
  locomotion with flat-foot contacts), extend the clip list in `tests/assets/content/ContentTests.cpp`,
  run `build_all.ps1`. Another library pack must use the same bone names.
## 4. Props (`tools/blender/gen_props.py`)

Thirteen environment props, generated with Tripo H3.1 from the project's own concept images and
imported by a data-driven pipeline. Sources (raw output, never edited) and concept images live in
`tools/blender/source_meshes/props/<src>.glb` / `<src>_ref.png`: one mesh, one material, embedded
4096² Color / NormalGL / ORM images (R occlusion, G roughness, B metallic — the engine's layout),
normalised to ≈1 unit around the bounds centre, arbitrary facing.

| Prop | Source | Size (w × d × h, m) | Facing fix | Sink | lod0 / lod1 tris | Maps | Emissive | Footprint |
|---|---|---|---|---|---|---|---|---|
| `house_a` | house_a | 5.95 × 7.81 × 8.0 | −90° (door on the gable) | 0.18 | 14 000 / 2 600 | 2048² | wall lantern, window glass | hull < 2.2 m |
| `cottage` | cottage | 6.9 × 6.0 × 5.8 | 180° | 0.18 | 14 000 / 2 600 | 2048² | — | hull < 2.2 m |
| `well` | well | 2.07 × 2.56 × 2.9 | 0 | 0.08 | 7 000 | 1024² | — | hull < 1 m |
| `market_stall` | market_stall | 3.41 × 2.42 × 3.21 | −90° | 0.05 | 9 000 | 1024² | two lanterns | hull < 1 m |
| `ant_tasi` | ant_tasi | 2.69 × 1.96 × 4.9 (with plinth) | −90° (written face front) | 0.10 | 11 564 (kept) | 2048² | ember crack | hull < 1 m |
| `watchtower` | watchtower | 4.24 × 5.04 × 8.8 | 0 | 0.15 | 9 000 / 2 000 | 1024² | brazier | hull < 1.6 m |
| `fence` | fence (one 4 m section) | 4.0 × 0.3 × 1.25 (per-axis scale) | 90° (runs along local x) | 0.06 | 2 400 | 512² | — | hull |
| `lamp_post` | lamp_post | 0.61 × 1.66 × 3.2 | 180° (lantern in front) | 0.06 | 3 000 | 512² | lantern | post disc |
| `goods` | goods (barrels, crates, sacks) | 1.8 × 1.92 × 1.35 | 0 | 0.04 | 5 000 | 1024² | — | hull |
| `signpost` | signpost | 0.91 × 1.62 × 2.6 | 0 | 0.08 | 3 000 | 512² | — | post disc |
| `boulder` | boulder | 2.1 × 2.07 × 1.57 | 0 | 0.25 | 3 000 | 1024² | — | hull |
| `tree_oak` | oak | 6.95 × 7.02 × 9.0 | 0 | 0.20 | 12 000 / 1 600 | 2048² | — | trunk disc |
| `tree_pine` | pine | 6.21 × 6.61 × 12.0 | 0 | 0.20 | 10 000 / 1 200 | 2048² | — | trunk disc |

Per prop (`build()`): import and join; rotate so the front faces −Y; scale uniformly from one axis
(or per axis: the fence section); **clean** — weld coincident vertices (0.1 mm), make the winding
consistent on every closed shell (`recalc_face_normals`, flips counted), delete shells under 0.5 %
of the faces whose box touches no other shell (floating bits), drop loose vertices, report open and
non-manifold edges; **ground** — origin below the centre of the base (the lowest 2 % / 5 cm), base
on Z = 0, then sunk by `sink` so an uneven base never shows a gap; **textures** — box-filter the
4096² maps to `tex` (colour in linear light, normals as renormalised vectors, ORM linear; the UV
layouts are kept) and write `<name>.png`, `<name>_n.png`, `<name>_orm.png`; **decimate** (Blender
collapse, UVs interpolated) to the budget; a far LOD is a second, coarser copy named `lod1` (the
full one `lod0`; same material, so assetc keeps the node names as submesh names and the client picks
by distance, CLIENT_RENDERING.md §4); **export** `<name>.glb` with one material: base colour, normal
map, ORM wired to metallic-roughness **and** occlusion (`glTF Material Output` group; factors 1),
optional emissive. Every run prints `PROP` lines (triangles, size, shells, welds, flips, floating
shells, open edges, base spread) and writes `assets_src/props/props_meta.json` (bounds in engine
axes, triangle counts, clean-up stats, footprint) for `gen_terrain.py`. Findings on the current
sources: no flipped or floating parts; a few open edges (cottage 27, market stall 91: cloth and
open-backed details, not holes in solid walls); every base needs the sink (house plinth spread 0.2 m).

**Emissive masks** (`EMISSIVE`): a texel glows when it belongs to a face inside one of the prop's
region boxes (final metres, read off `PROP` runs and preview renders) **and** its colour / roughness
passes the part's rule — `warm` (hue 15-62°, saturation ≥ 0.3, value ≥ 0.5: lantern glass, candles),
`glass` (ORM roughness < 0.45 and mid-bright: window panes), `ember` (orange-red, saturated: the Ant
Taşı crack, the brazier). The mask (softened by one texel) times the base colour, pulled towards a
warm candle colour by the part's `warmth` (grey panes glow as lit windows), is written as
`<name>_e.png` (512-1024²) and wired as `emissiveTexture` with `KHR_materials_emissive_strength`
(1.4-4). The cottage, well, goods, signpost, fence, boulder and trees have none (nothing reliably
identifiable). Previews: `previews/props/<name>.png` (the prop and its far LOD next to a 1.8 m
marker), `props_sheet.png`.

**Variation** is per placement (.vterrain v2): `stretch` (per axis, houses within ±8 %) and `tint`
(sRGB multiplier) — two variants per house kind in `gen_terrain.HOUSE_VARIANTS`; random yaw and
three sizes (0.78 / 1.0 / 1.18 ± 6 %) for trees, random yaw and 0.55-1.5 scale for boulders.

**To add a prop**: drop the source in `source_meshes/props/`, add a `PROPS` row (look at the
`PROP` line and the preview: facing, size, sink), optionally `EMISSIVE` parts, place it in
`gen_terrain.py` and add it to `kProps` in `tests/assets/content/EnvironmentTests.cpp`.

**Trees.** The generated oak and pine were evaluated in game at the default camera, at the forest
edge and in wide shots: closed meshes with intact silhouettes (no broken or missing foliage); the
pine reads well, the oak's canopy is a stack of clumpy leaf plates — stylised rather than
semi-realistic up close, convincing at the default camera distance and beyond. They are kept;
the format and import side of alpha-tested cards is in place (`.vmesh` v3 alpha test, assetc `alphaMode MASK`, the
main-pass `clip`); the shadow-pass clip and the alpha-to-coverage blend state are not wired yet (no asset uses alpha test).

## 5. Terrain (`tools/blender/gen_terrain.py`, zone 1)

* 513² heightmap over 512 m (1 m spacing), 16-bit PNG; stored height range [4, 92] m.
* Village plateau (≈20 m) around (256, 256) with a dirt plaza; rolling fbm hills; a north–south road
  and an east branch (Catmull-Rom, 6.4 m wide, graded) running through passes in steep, impassable
  ridges along the borders; 6 m outer margin blocked.
* Walk mask (512², 1 m cells): blocked where slope > 36°, in the margin, and in every cell whose
  centre lies within 0.35 m (character radius) of a blocking placement's **real footprint**: the
  prop's convex hull below head height from `props_meta.json` (houses, cottage, well, stall, stone,
  tower, fences, goods, boulders), or a disc around the origin for posts (lamp posts, signposts:
  ~0.1-0.2 m) and tree trunks (canopies do not block), with yaw, scale and stretch applied.
* Splat (512² RGBA): R grass, G dirt road / plaza (edges wander by 1-2 m: worn verges), B rock
  (steep or high), A dry grass (noise patches and higher ground). Layers (`gen_textures.py`): Poly
  Haven scans graded per layer (saturation, brightness, a slight colour pull) at 1024² — grass
  `forrest_ground_01` (2.6 m tile), dirt road `rocky_trail` (3.0 m), rock `rock_face_03` (7 m,
  triplanar in the shader), dry grass `grass_ground` (3.6 m); base colour + normal + ORM each.
  1024² was chosen over 2048²: at these tiles it is 170-340 texels/m, more than the nearest ground
  gets on screen at the default camera.
* Ant Taşı at (249.0, 263.5) in the north-west quarter of the plaza (off both roads, 7 m from the
  N–S road's centre line), its written (Aydor) face turned to the Aydor spawn; placed first so nothing
  else comes within its clearance; footprint blocked, ground around it walkable.
* Spawns: `spawn_aydor` (238, 271), `spawn_kargan` (277, 247) facing the stone, 45.8 m apart on the
  plateau (y ≈ 20.0 m), 13.3 / 32.5 m from the stone, nearest wall (house, cottage or watchtower
  footprint) 15.1 / 14.9 m (`check_layout` requires ≥ 8 m, 40–60 m apart, ≤ 40 m to the stone).
* 602 placements: the Ant Taşı; 13 houses (7 `house_a`, 6 `cottage`, two variants each) ringed
  around the plaza with their doors to it — the west house of the UI flows at (216.4, 258.1) facing
  +X exactly; 3 market stalls with goods behind them, 10 goods clusters (beside house doors and
  stalls), the well, 15 lamp posts along the roads (lanterns over the road), 4 signposts (the
  crossroads, the passes), 3 watchtowers at the passes, 28 fence sections (house yards with a gate
  gap, road edges 70-105 m out; sections whose ends differ by > 0.45 m in height are skipped),
  429 trees (249 oaks, 180 pines; 30 groves plus singles, none within 60 m of the plaza), 95
  boulders mostly on slopes. Each placement's `y_offset` puts the lowest ground under its footprint
  at the base, so nothing floats on a slope (the uphill side sinks; props are sunk anyway).
  Nothing within road width + clearance, nothing within 4 m of a spawn; the straight line the
  world_tour flow walks from the west house to the north-east hill (226, 255.5) → (334, 338) and
  26 m around its end stay free of blocking props (`CLEAR_PATHS`, `CLEAR_AREAS`).

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
texture = "textures/terrain/grass.png"     # asset path (base colour, sRGB)
normal = "textures/terrain/grass_n.png"    # optional: tangent-space normal map (OpenGL +Y up)
orm = "textures/terrain/grass_orm.png"     # optional: R occlusion, G roughness, B metallic
tile_size = 2.6                  # metres per repeat

[[spawn]]                        # y = heightAt(x, z); must be inside and walkable
name = "spawn_aydor"
x = 238.0
z = 271.0
yaw = 2.356

[[object]]
mesh = "props/house_a.vmesh"     # asset path of a .vmesh (must exist after the mesh pass)
x = 1.0
z = 2.0
yaw = 0.0                        # optional, default 0
scale = 1.0                      # optional, default 1
y_offset = 0.0                   # optional, added to heightAt(x, z)
stretch = [1.06, 0.95, 1.04]     # optional: per-axis factor on top of scale (0.25 .. 4); .vterrain v2
tint = [238, 228, 212]           # optional: sRGB base-colour multiplier (integers 0..255); .vterrain v2
blocks = true                    # optional, default true
```

Unknown keys are errors. Layer maps are compiled to `.vtex` by `assetc build` (the terrain item
lists them as inputs).

## 6. Previews

Every generator renders previews (EEVEE, Standard view transform, no dither, back-face culling on so
flipped faces show as holes) into `assets_src/previews/` (never copied to `assets/`). All PNGs
(textures, maps, previews) go through `vo_common.write_png`: no metadata, zlib level 9, per-row PNG
filter chosen by the libpng heuristic (lossless; ~30 % smaller than unfiltered rows).

* `characters/<look>_bind_{front,side,back,threequarter}.png` (rest pose), `<look>_head.png` (idle head:
  front, three-quarter, side, back), `<look>_<clip>_sheet[_3q|_front|_back].png` for idle, walk, run,
  sword_attack, sit_idle (6 frames over the clip, 0.5 m grid lines; side / three-quarter / front / back as useful per clip), `<look>_clips_sheet.png` (every
  clip at its middle frame)
* `props/<name>.png` (prop and far LOD next to a 1.8 m marker), `props/props_sheet.png`
* `textures/terrain_layers.png` (per layer: base colour, normal, ORM, each tiled 2×2 to show the seams)
* `terrain/zone1_topdown.png` (orthographic, +Z up, +X right), `zone1_oblique.png`,
  `zone1_village.png`, `zone1_walkmask.png` (red = blocked, blue/yellow = Aydor/Kargan spawns,
  dots = objects; image row = z, so +Z points down in this one)

Generators also print checks: triangle counts, per-frame foot slide during flat stance (must be
0.00 mm), terrain walkability and spawn heights.
