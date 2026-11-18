# World zones — art and data (M13 "Üç Yurt")

Three zones, 512 × 512 m each, all generated headless in Blender from committed scripts; no
downloads, no third-party assets beyond the scans and meshes already in the repo. Names follow
[LORE.md](LORE.md). Coordinates are engine space (x right / east, z forward / north, metres; y comes
from the heightmap). Review renders: `docs/screenshots/m13/` (made by `tools/blender/gen_zone_shots.py`).

| Zone id | Name (UI) | Place in the lore | Who starts here | Asset |
|---|---|---|---|---|
| 1 | Ortaocak (Eşik) | the neutral market village round the cracked Ant Taşı | nobody new (both races until M13) | `terrain/zone1/zone1.vterrain` |
| 2 | Tanyeri Burcu (Aydor sınır boyu) | the Aydor border march: golden plain inside chalk escarpments, white-stone keep town, irrigated fields, scorched fields beyond | new Aydor characters | `terrain/zone2/zone2.vterrain` |
| 3 | Demirağız geçidi (Kargan) | the pass below the Kargan gate-fort: a narrow valley between dark snow-capped mountains, a hearth-town under a cliff with doors cut into the rock | new Kargan characters | `terrain/zone3/zone3.vterrain` |

Geography: Eşik lies between the southern plains and the northern mountains, so zone 1's **Aydor
gate is on the south side** of the village and its **Kargan gate on the north side**; zone 2's gate to
Eşik is at the **north** end of the march, zone 3's at the **south** end of the pass.

## 1. Markers (the server contract)

`[[spawn]]` entries of each terrain description (`assets_src/terrain/<zone>/<zone>.terrain.toml`,
compiled into the `.vterrain` spawn table). All stand on walkable ground; race spawns and arrival
points have the 3 m round them walkable; every gate opening is walkable 0.5 m either side of its
centre line for 2 m through, and a straight walkable line runs from the gate through its arrival
point to the road. **Gate yaw** = the way the gate's front (its approach side) faces = the direction
from the gate to its arrival point. **Arrival yaw** = the same direction (a player arriving faces away
from the gate). Engine yaw convention: forward = (sin yaw, cos yaw).

| Zone | Marker | x | z | yaw (rad) | Notes |
|---|---|---|---|---|---|
| 1 | `spawn_aydor` | 238.00 | 271.00 | 2.3562 | unchanged (M5) |
| 1 | `spawn_kargan` | 277.00 | 247.00 | −1.0383 | unchanged (M5) |
| 1 | `gate_aydor` | 287.00 | 213.00 | −0.6251 | 53 m SSE of the plaza (~10 s at run speed), just outside the house ring; faces the plaza (NW): the straight walk in passes between two houses onto the plaza dirt (36 m). The south-east Kül Çömezi pack is 4.8 m beyond its aggro reach (the south road runs through the south pack, so the gate does not face that road) |
| 1 | `arrive_aydor` | 282.32 | 219.49 | −0.6251 | 8 m in front of the gate, 45 m from the plaza; own safe zone r 8 (outside the village's r 40) |
| 1 | `gate_kargan` | 274.00 | 326.00 | −1.5778 | 72 m NNE of the plaza, east of the north road, 22 m west of the line `world_tour` walks; faces west to the north road (12 m) |
| 1 | `arrive_kargan` | 266.00 | 325.94 | −1.5778 | 8 m in front of the gate; own safe zone r 8 |
| 2 | `spawn_aydor` | 244.00 | 286.50 | 0.3491 | town plaza (250, 296), 11 m from its centre; nearest wall 19 m |
| 2 | `gate_esik` | 256.00 | 440.00 | 3.0315 | end of the north road, in the cut through the escarpment (closed behind the gate) |
| 2 | `arrive_esik` | 256.85 | 432.31 | 3.0315 | 8 m down the road from the gate, facing the town; safe zone r 10 |
| 3 | `spawn_kargan` | 261.00 | 335.00 | 3.4907 | hearth-town plaza (256, 344), 10 m from its centre; nearest wall 15 m |
| 3 | `gate_esik` | 256.00 | 100.00 | −0.1306 | end of the valley road at the foot of the southern gorge (closed behind the gate) |
| 3 | `arrive_esik` | 254.94 | 108.07 | −0.1306 | 8 m up the road from the gate, facing the valley; safe zone r 10 |

Wiring the gates is the server's job: `gate_aydor` (zone 1) ↔ zone 2 `arrive_esik`, zone 2
`gate_esik` ↔ zone 1 `arrive_aydor`; `gate_kargan` (zone 1) ↔ zone 3 `arrive_esik`, zone 3
`gate_esik` ↔ zone 1 `arrive_kargan`. A trigger radius of ~1.5 m round the gate marker sits inside
the opening (3.4 m wide, the pillars block the walk mask). Note: the server's current
`Zone::create` requires both `spawn_aydor` and `spawn_kargan` in every terrain; zones 2 and 3 carry
only their own race's spawn, as briefed.

## 2. Terrains

| | Zone 1 | Zone 2 | Zone 3 |
|---|---|---|---|
| Generator | `gen_terrain.py` | `gen_zones.py` | `gen_zones.py` |
| Shape | plateau village at 20 m, rolling hills, grey ridges with three road passes | golden plain ~12 m, town terrace 14 m, two-step chalk escarpments all round, cut for the north road | valley floor 30 → 41 m rising north, side basins W / E / SW, mountains to ~180 m with crags and gullies, cliff face north of the town |
| Stored heights | 4–92 m | 8–62 m | 29–181 m |
| Layers (splat R G B A) | grass, dirt, rock, drygrass | `steppe` (golden), `chalk_road`, `chalk_rock`, `scorched` | `ash_tundra`, `gravel`, `basalt`, `snow` |
| Walkable | 61.6 % | 75.8 % | 33.5 % |
| Placements | 602 | 534 | 327 |

The eight new layers (`gen_textures.py` `LAYERS_M13`) are the same four Poly Haven scans graded
further (saturation, brightness, colour pull; `snow` lifted towards white with flattened normals);
1024² base colour + normal + ORM, compiled like zone 1's. No ground layer is shared between the zones
(content test). The splat's G channel is the road in every zone. Zone 2's fenced fields carry furrow
stripes of bare earth (road layer at 45 %), the scorched hunting grounds the `scorched` layer; zone
3's snow lies above ~60 m over the valley floor (not on steep faces) and in drifts, scree (rock) along
the foot of the slopes.

Walk mask as zone 1: blocked where the slope exceeds 36°, in the 6 m margin, and under every
blocking footprint grown by the character radius. Zones 2 and 3 keep blocking props out of the monster
spawn circles of `gamedata/monsters.toml` (+2.5 m), so moving a spawn means re-running
`gen_zones.py`.

## 3. Props (`tools/blender/gen_zone_props.py`)

Procedural geometry with procedural Cycles materials (3D, in the prop's metres) baked into one PBR
set per prop: base colour, normal, ORM (AO baked over a ground plane), emissive where something glows.
Same format and conventions as zone 1's props (one material, origin on the ground below the centre,
base sunk). Meta (bounds, triangles, footprint) in `assets_src/props/zone_props_meta.json`.

| Prop | Size w × d × h (m) | Tris | Maps | Emissive | Footprint | Used in |
|---|---|---|---|---|---|---|
| `gate_aydor` | 5.9 × 1.8 × 7.9 | 904 | 1024² | gold sun emblem centre | 2 pillar hulls | zone 1 (`gate_aydor`) |
| `gate_kargan` | 5.9 × 1.8 × 7.8 | 898 | 1024² | ember runes, two braziers on top | 2 pillar hulls | zone 1 (`gate_kargan`) |
| `gate_esik` | 7.5 × 2.5 × 7.1 (with fallen blocks) | 636 | 1024² | pale rune inlays | 2 pillar hulls | zones 2 and 3 (`gate_esik`) |
| `aydor_wall` | 6.0 × 2.0 × 5.2 (6 m section) | 120 | 1024² | — | hull | zone 2 curtain wall (and grey ruins on the scorched fields) |
| `aydor_tower` | 6.9 × 7.6 × 16.4 | 676 | 1024² | lit upper windows | hull | zone 2 (Tanyeri Burcu's keep) |
| `aydor_house` | 7.1 × 6.0 × 5.7 | 220 | 1024² | windows | hull | zone 2 |
| `aydor_banner` | 1.3 × 0.6 × 4.9 | 316 | 512² | — | post disc | zone 2 |
| `kargan_door` | 7.2 × 4.3 × 6.1 | 912 | 1024² | ember lintel rune, two wall braziers | hull | zone 3 (in the cliff) |
| `kargan_forge` | 4.5 × 1.8 × 4.6 | 256 | 1024² | coal bed | hull | zone 3 |
| `kargan_brazier` | 1.0 × 1.0 × 1.4 | 260 | 512² | coals | base disc | zone 3 (plaza, road into town) |
| `kargan_palisade` | 4.0 × 0.4 × 3.4 (4 m section) | 360 | 512² | — | hull | zone 3 |
| `kargan_hut` | 6.9 × 6.9 × 5.4 | 438 | 1024² | windows | hull | zone 3 |

A gate is placed with `blocks = false` (its origin is the opening) and its two pillar hulls block the
walk mask (`multi` footprint). The "a gate wears the look of where it leads" rule picks the look:
zone 1's gates are Aydor and Kargan, the homelands' gates (to Eşik) are the neutral Eşik look.
Zone 2 and 3 also reuse zone 1 props (well, market stall, goods, fences, lamp posts, signposts, trees,
boulders; tinted where it helps: chalk-pale boulders, burnt trees, frosted pines).

**Gate veils** (effect meshes, `assets_src/effects/gate_veil_{aydor,kargan,esik}.glb` →
`effects/gate_veil_<look>.vmesh`): a double-sided arch-shaped sheet (768 triangles) exactly filling the
opening, in the gate's own frame (origin on the ground at the opening's centre, the sheet in the
plane z = 0), base + emissive gradient maps (`.png`, `_e.png`, 64 × 128; brighter at the jambs, fading
upwards, faint ripples), emissive strength 2. Draw it with the M12 effect path (`addEffectMesh`,
opacity ≈ 0.5, premultiplied) at the gate placement's transform (same position, `y_offset` and yaw);
animate by scrolling / pulsing the opacity or a slight scale on Y. Look per gate: zone 1 `gate_aydor`
→ `gate_veil_aydor`, `gate_kargan` → `gate_veil_kargan`; zones 2 and 3 `gate_esik` → `gate_veil_esik`.
The veil is separate so it can blend: the prop pipeline is opaque.

## 4. Monsters (`gamedata/monsters.toml` ids 6–13, `tools/blender/gen_monsters.py`)

No new generated meshes: each kind is an existing rigged source given a new skin (`recolor` in
`gen_monsters.MONSTERS`: palette remaps, procedural crack networks, frost / straw patterns painted in
UV space, ORM adjusted) and then built exactly like the M9 monsters (players' 59-bone skeleton, the 11
combat clips, 30 000 triangles, 1024² × 4 maps + emissive). LORE: the Külden Doğanlar rise from
"animals, the dead, sometimes the stone itself"; two of the four are fallen soldiers of the peoples.

fix1 — own silhouettes (`tools/blender/monster_shapes.py`, `shapes` / `reshape` in `gen_monsters.MONSTERS`):
the body is decimated to 27k triangles, optional rest-pose bone changes are baked into skeleton and
mesh (the clips are bone-local and play on the new proportions), and 0.6–1.8k triangles of added
pieces are skinned to the torso / shoulder / arm bones. The body's maps are squeezed into atlas columns
0–895; the pieces' swatches (straw, twine, charred wood, burnt iron, burnt banner cloth, ice, rimed
rock, frozen cloak) are painted in the strip at 904–1019. Same skeleton, clips, height, one material.

| Id | Name | Model | Source | Zone | Lvl | Scale | HP | Atk | Def | Range | Interval ms | Move / wander | Aggro | Leash | XP | Respawn s |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 6 | Anız Yanığı | `stubble_thrall` | ash_thrall (charred straw, gold embers); hunched, longer arms, a fan of burnt straw out of the back, straw at shoulders / elbows / wrists, straw skirt | 2 | 1 | 0.95 | 76 | 12 | 3 | 2.0 | 1400 | 4.4 / 1.4 | 6 | 25 | 25 | 15 |
| 7 | Anız Yanığı | `stubble_thrall` | | 2 | 2 | 1.00 | 100 | 15 | 4 | 2.0 | 1400 | 4.4 / 1.4 | 6 | 25 | 32 | 15 |
| 8 | Yanık Nöbetçi | `burnt_sentry` | aydor_2 (a border soldier burnt to ash, ember cracks); head lolled to one side, a broken charred spear with a torn burnt banner on the back, split plates on shoulders and forearms, broken arrows | 2 | 3 | 1.00 | 230 | 17 | 11 | 2.2 | 2000 | 3.0 / 1.0 | 7 | 25 | 60 | 30 |
| 9 | Yanık Nöbetçi | `burnt_sentry` | | 2 | 4 | 1.03 | 275 | 20 | 12 | 2.2 | 2000 | 3.0 / 1.0 | 7 | 25 | 75 | 30 |
| 10 | Kırağı Kayası | `rime_rock` | slag_walker (frost-bound basalt, cold blue glow); a crusted rime-rock hump on the back, ice-crystal clusters on shoulders, back and forearms | 3 | 1 | 0.78 | 90 | 10 | 5 | 2.0 | 1700 | 3.6 / 1.0 | 6 | 25 | 25 | 15 |
| 11 | Kırağı Kayası | `rime_rock` | | 3 | 2 | 0.82 | 118 | 13 | 6 | 2.0 | 1700 | 3.6 / 1.0 | 6 | 25 | 32 | 15 |
| 12 | Donuk Ocakçı | `frozen_hearthguard` | kargan_0 (a frozen hearth-guard, rime, cold cracks); frost crown and two ice horns, ice shards and icicles on the pauldrons, a stiff frozen cape with an icicle hem | 3 | 3 | 1.00 | 240 | 19 | 9 | 2.3 | 2300 | 2.8 / 0.9 | 7 | 25 | 60 | 30 |
| 13 | Donuk Ocakçı | `frozen_hearthguard` | | 3 | 4 | 1.03 | 285 | 22 | 10 | 2.3 | 2300 | 2.8 / 0.9 | 7 | 25 | 75 | 30 |

Compare Kül Çömezi 80 / 11 / 4 (lvl 1) … Cüruf Yürüyen 260 / 21 / 10 (lvl 4): the Aydor kinds hit a
little faster and break a little sooner, the Kargan kinds are slower and tougher. Loot
(`gamedata/loot.toml`, existing items only): Anız Yanığı as Kül Çömezi (Kül Tozu, Çömez Bıçağı,
Kömürlü Kılıç); Yanık Nöbetçi Kül Tozu, Kömürlü Kılıç, Cüruf Topuzu; Kırağı Kayası Cüruf Parçası,
Çömez Bıçağı; Donuk Ocakçı Cüruf Parçası, Kül Tozu, Cüruf Topuzu, rarely Kırağı Asası.

Spawns: zone 2 — four loose packs of three Anız Yanığı 75–90 m out just beyond the walls (members
12–16 m apart, pullable one at a time), Yanık Nöbetçi in pairs / singles on the scorched fields
120–175 m out; zone 3 — three packs of Kırağı Kayası down the valley 65–145 m south of the town,
Donuk Ocakçı in the side basins and on the valley floor 110–200 m out. Safe zones: zone 2 town r 46
and `arrive_esik` r 10, zone 3 town r 42 and `arrive_esik` r 10, zone 1 `arrive_aydor` / `arrive_kargan`
r 8 each (the village's r 40 is unchanged). Every wander circle + aggro radius stays > 3 m clear of
each race spawn and arrival point (content test; in practice ≥ 6 m in zone 1), and ≥ 12 m in the homelands.

## 5. What the client loads per zone

| Zone | Terrain | Props (from the terrain's placements) | Monsters (`monsters/<model>.vmesh` + clips) | Effects |
|---|---|---|---|---|
| 1 | `terrain/zone1/zone1.vterrain` | zone 1 set + `gate_aydor`, `gate_kargan` | `ash_thrall`, `slag_walker`, `ember_warden` | `gate_veil_aydor`, `gate_veil_kargan` |
| 2 | `terrain/zone2/zone2.vterrain` | `aydor_*`, `gate_esik`, well, market_stall, goods, fence, lamp_post, signpost, trees, boulder | `stubble_thrall`, `burnt_sentry` | `gate_veil_esik` |
| 3 | `terrain/zone3/zone3.vterrain` | `kargan_*`, `gate_esik`, goods, signpost, tree_pine, boulder | `rime_rock`, `frozen_hearthguard` | `gate_veil_esik` |

Placements may carry `tint` and `stretch` (`.vterrain` v2) — zones 2 and 3 use tints a lot (burnt
trees, chalk boulders, frosted pines). Sky / fog colours are not part of the terrain; suggested:
zone 2 warm pale (#b9c6cf sky, the renders' value), zone 3 cold grey (#9aa6b2).

## 6. Regenerating

```powershell
powershell -File tools/blender/build_all.ps1          # everything (gen_zone_props after gen_props, gen_zones last)
blender --background --factory-startup --python tools/blender/gen_zone_props.py -- [--only gate_aydor,...] [--no-preview]
blender --background --factory-startup --python tools/blender/gen_monsters.py -- --only stubble_thrall,burnt_sentry,rime_rock,frozen_hearthguard
blender --background --factory-startup --python tools/blender/gen_zones.py -- [--only zone2] [--no-preview]
blender --background --factory-startup --python tools/blender/gen_zone_shots.py -- [--only 2] [--out docs/screenshots/m13]
```

Order matters: props before terrains (footprints), `monsters.toml` spawns before `gen_zones.py`.
Zone 1's heightmap and splat are byte-identical to M12; its description gained the four markers and
the two gates, its walk mask the gates' pillars; two fence sections of a house yard gave way to the
Aydor gate's path, nothing else moved.
