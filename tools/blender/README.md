# tools/blender

Headless Blender (5.3, bundled Python + numpy) generators for every source asset in `assets_src`.
Run them all with `powershell -File tools/blender/build_all.ps1`. Details, conventions and how to
add content: [docs/ASSET_PIPELINE.md](../../docs/ASSET_PIPELINE.md).

| Script / folder | Purpose |
|---|---|
| `vo_common.py` | shared helpers: deterministic PNG writer (optional extra lossless optimisation), materials, mesh builder, glTF export, preview rendering |
| `vo_ual.py` | UAL skeleton + clips: leaf bones dropped, A-pose rest, look scale, joint fit, leg proportions fitted to the mesh with every clip re-targeted by leg IK, walk re-timed for 1.4 m/s, foot pinning, procedural ground-sit clips, six helper bones with baked secondary motion |
| `gen_textures.py` | `assets_src/textures/terrain/*.png` (tileable terrain layers) |
| `gen_props.py` | `assets_src/props/*.glb` |
| `gen_weapons.py` (`--prepare <raw dir>` once per new raw source) | `source_meshes/weapons/<name>.glb` → `assets_src/weapons/<name>.glb` + maps (grip frame), `assets_src/ui/items/<name>.png` (128² icons), `previews/weapons/<name>.png` |
| `gen_attachments.py` | `gamedata/attachments.toml` (weapon in hand per grip / look / hand) and `previews/weapons/hold_<look>[_hands].png` |
| `gen_character.py --look <race>_<n>` (or `--race <race>`; `--debug <dir>` adds weight renders) | import-rig-export of `source_meshes/<race>_<n>_tripo.glb` → `assets_src/characters/<race>_<n>.glb` + `.png` / `_n.png` / `_orm.png` + `.anims.toml` (three looks per race; look 0 also as the legacy `<race>.glb` / `.anims.toml`) |
| `gen_skill_art.py` (`--only icons\|status\|ui`, `--sheets <dir>`) | M12: 24 branch skill icons (`assets_src/ui/skills/`), buff / debuff icons (`ui/status/`), skill bar / tree pieces (`ui/skillbar/`); contact sheets to `build/art_sheets/m12` (see `docs/SKILL_ART.md`) |
| `gen_skill_effects.py` (`--sheets <dir>`) | M12: skill effect meshes in `assets_src/effects/` (manifest: `docs/SKILL_ART.md` §3) |
| `gen_hud_art.py` (`--sheets <dir>`) | HUD: the ten button glyphs of the HUD bar (`assets_src/ui/hud/`, 64²); contact sheet to `build/art_sheets/hud` |
| `gen_terrain.py` | `assets_src/terrain/zone1/*` (heightmap, splat, walk mask, description; M13 gates and markers) |
| `gen_zone_props.py` | M13: gates (three looks), Aydor / Kargan settlement props, gate veil effects (procedural, Cycles-baked; `docs/WORLD_ZONES_ART.md`) |
| `gen_zones.py` | M13: `assets_src/terrain/zone2/*`, `zone3/*` (the two homelands) |
| `gen_zone_shots.py` | M13 review renders into `docs/screenshots/m13/` (not assets) |
| `measure_ual_speeds.py` | prints the root-motion ground speed of the UAL loops (needs the uncommitted `_RM` file) |
| `source_meshes/weapons/` | the eight Tripo H3.1 weapons, reduced to 40k triangles with their embedded images unchanged (raw files outside the repo), `<name>_ref.png` concept images |
| `source_meshes/` | character meshes generated with Tripo H3.1 from the project's own A-pose concept images (`<race>_<n>_apose.png`), project-owned |
| `third_party/quaternius_ual/` | Quaternius Universal Animation Library (CC0 1.0), `License.txt`, `README.txt` |

Always run with `--background --factory-startup`; never attach to an interactive Blender session.

## Characters (semi-realistic PBR looks)

| Look | Source | Height |
|---|---|---|
| `aydor_0` | knight: plate pauldrons, vambraces, greaves over mail, blue-grey surcoat with the sun emblem | 1.82 m |
| `aydor_1` | woman knight, same kit, braid down the back | 1.72 m |
| `aydor_2` | grey-bearded veteran, dark red surcoat | 1.80 m |
| `kargan_0` | orc-kin warrior: scale armour, iron pauldrons, fur mantle, chest braids, loincloth | 2.00 m |
| `kargan_1` | orc-kin woman warrior: fur collar, loincloth, braid down the back | 1.90 m |
| `kargan_2` | elder smith: braided beard, leather apron, fur mantle, one carved pauldron | 1.98 m |

* **Sources**: one fused mesh of ≈38k triangles per look, one material with 4096² Color, NormalGL and
  ORM images, A-pose, facing +X (`rot_z = -90`). The UV-seam splits are merged, the mesh is decimated
  to `max_tris` = 30000 by edge collapse with the head (face, tusks) and hands protected, and the
  source's own vertex normals (the ones the normal map was baked against) are kept: they ride
  through the bind deformation and are exported as the glTF normals. No TANGENT is exported (assetc
  generates MikkTSpace tangents on the final mesh).
* **Textures**: each 4096² map is downscaled 2× by area averaging (base colour in linear light,
  normals averaged as vectors and renormalised, ORM plain) to 2048² and written as `<look>.png`,
  `<look>_n.png` (tangent space, OpenGL +Y, unchanged convention), `<look>_orm.png` (R occlusion,
  G roughness, B metallic). With `EXPORT_PBR_MAPS = True` (the committed state) the glb's single
  material references all three: `baseColorTexture` → `<look>.png`, `normalTexture` (scale 1) →
  `<look>_n.png`, `metallicRoughnessTexture` and `occlusionTexture` → `<look>_orm.png`, factors 1.
  The assetc on main reads only `baseColorTexture` and ignores the rest (same `.vmesh`); every PNG is
  copied to `assets/characters/` by `assetc build`.
* **Skeleton**: 53 UAL bones + 6 helper bones (`cloth_front`, `cloth_back` on the pelvis, `mantle`
  on spine_03, `braid_l`, `braid_r`, `beard` on the head) = 59, same names and hierarchy for every
  look (an unused helper carries no weights). The scale is hip height / 0.932; arms, hip width and
  thigh / calf lengths are fitted to the mesh (`fit` in `gen_character.LOOKS`), and every clip is
  re-targeted with a leg IK so each ankle keeps its library path (feet still plant; walk / run are
  pinned afterwards). Helper motion (`vo_ual.HELPER_MOTION`): gravity-biased target relative to the
  parent, a damped spring for lag, then hard limits (max deviation, never into the body, cloth panels
  kept in front of / behind the knees that are behind them and above the ground).
* **Weights** (`clean_weights`): bone heat in the mesh's own pose, then fingers into rigid hands,
  no arm weights in the torso column, shoulder cap, no thigh weights above the hips (belts, pouches),
  rigid forearm / shin armour, cloth panels / mantle / braids / beard from per-look regions on the
  helper bones, rigid pieces (metal islands from the ORM metallic mask, eroded so mail and thin trims
  drop out; the look's `rigid` boxes for non-metal plates and pouches) with one weight set per piece
  blended over two vertex rings at the seam.
* **Clips** (29): the 26 library clips plus `sit_ground_enter` (45 frames), `sit_ground_idle` (loop,
  120 frames, one breath) and `sit_ground_exit` (45 frames), made per look (`vo_ual.make_sit_ground`):
  through a squat onto the ground, cross-legged, wrists on the knees. The chair clips stay.
