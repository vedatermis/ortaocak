# Shop art — merchants, stalls, potion icons, shop UI (M14 "Pazar")

Everything here is generated headless in Blender 5.3 from committed scripts and committed sources
(the player looks' meshes, the Quaternius CC0 animation library already in the repo, procedural
geometry and textures). No downloads, no third-party assets, nothing taken from another game. Names
and palettes follow [LORE.md](LORE.md) and the M13 homeland art ([WORLD_ZONES_ART.md](WORLD_ZONES_ART.md)).
Review renders: `docs/screenshots/m14/art_*.png` (`tools/blender/gen_shop_shots.py`, §6).

**Nothing below has been seen in the game client yet** (the client engineer wires it up later); the
renders are Blender (Cycles) renders of the source files, and the content tests check the compiled
files (`tests/assets/content/ShopTests.cpp`).

## 1. Merchant NPCs

### Model keys → files (the contract with the server)

The server's NPC `model` is the key. Runtime files follow the **monster layout exactly**
(`AssetCache::monsterMeshPath` → `monsters/<model>.vmesh`, clips `monsters/<model>/<clip>.vanim`), in
their own folder:

```
assets/npcs/<key>.vmesh                 skinned mesh, the players' 59-bone skeleton, one submesh
assets/npcs/<key>/<clip>.vanim          clip = idle (loop), idle_talking (loop), interact (once)
assets/npcs/<key>.png / _n / _orm (.png + .vtex)   1024² PBR maps; _e (emissive) where something glows
```

Source: `assets_src/npcs/<key>.glb` + maps + `<key>.anims.toml` (assetc's generic `**/*.glb` rule; no
new rule was needed). `assets_src/npcs/npcs_meta.json` records base look, height, triangles.

| Key | Town | Role | Built on | Height | Triangles (body + outfit) | Glow (`_e`) |
|---|---|---|---|---|---|---|
| `npc_potion_esik` | Ortaocak (Eşik) | potion seller ("herb-wife") | `aydor_1` | 1.75 m | 29 836 (24 992 + 4 844) | flask liquids |
| `npc_smith_esik` | Ortaocak (Eşik) | smith | `kargan_0` | 2.04 m | 29 555 (24 999 + 4 556) | — |
| `npc_potion_aydor` | Tanyeri Burcu | apothecary | `aydor_2` | 1.83 m | 29 844 (25 000 + 4 844) | flask liquids |
| `npc_smith_aydor` | Tanyeri Burcu | smith | `aydor_0` | 1.86 m | 29 556 (25 000 + 4 556) | — |
| `npc_potion_kargan` | Demirağız | ember-brewer | `kargan_1` | 1.98 m (hood peak) | 29 844 (25 000 + 4 844) | flask liquids |
| `npc_smith_kargan` | Demirağız | master smith | `kargan_2` | 2.00 m | 29 760 (25 000 + 4 760) | hot ingot in the tongs |

(Exact numbers: `npcs_meta.json` and the `STATS` lines of a `gen_npcs.py` run.)

### Look

Each merchant is a player look's own rigged mesh (so it animates with the same skeleton and clips)
with **real outfit geometry** modelled around its rest-pose body and the **town palette** painted
over its clothes and armour:

* **Potion sellers**: a hood open round the face (with a lining and a trim-coloured rim) and a
  cowl over the shoulders that hides the pauldrons; a long cloth apron to mid-shin with a coloured
  hem band and stitching; a belt with three potion flasks (red, blue, red — the liquids glow
  faintly); a satchel on the left hip with two more flask necks; a strap across the chest.
* **Smiths**: a heavy leather apron to the knee (bib to the upper chest, neck straps over the
  shoulders down to the back of the belt, rivets, a pocket); leather gauntlets over the forearms
  with riveted cuffs; a soft cap with a rolled band (Kargan: a tight iron-studded skullcap); a belt
  with a hammer on the right hip and tongs on the left (Kargan: a glowing ingot in the tongs).
* **Palettes** (body recolour + outfit swatches): Ortaocak neutral and earthy (moss green, undyed
  linen, brown leather; ochre and felt for the smith); Tanyeri / Aydor white, blue and gold (white
  hood and apron with a blue hem and gold trim; blue clothes, white linen cap, tan leather with
  brass); Demirağız / Kargan dark, iron and ember (charcoal hood with an ember rim, ember hem; black
  leather, iron studs, the hot ingot).

Potion seller vs smith reads from the silhouette (hood + cowl + long light apron vs cap + dark
square apron + bulky gauntlets + hammer) and from the colour (light cloth vs dark leather); both
read as townsfolk rather than fighters (no weapon in hand, armour hidden or recoloured as cloth and
leather), and differ from the players' looks in silhouette and palette.

### How it is built (`tools/blender/gen_npcs.py`)

1. The base look goes through the character pipeline unchanged (`gen_character.load_source`,
   `rig`: fit, bone heat weights, weight clean-up, deformation into the shared A-pose rest), decimated
   to 25 000 triangles instead of 30 000 so body + outfit stay inside the players' 30k budget.
2. **Recolour** (`recolor`) on the 2048² source maps in UV space: the head (above the neck), the
   hands and braids / beards are kept (rasterised from their faces' UVs); Kargan skin is kept by hue;
   everything else is luminance-mapped through the town gradients by class (saturated cloth →
   primary, plain cloth / leather → secondary, metal → the metal ramp). Metal turned into cloth or
   leather loses its metalness unless the town keeps iron (Kargan).
3. **Outfit** (`outfit_for`): closed shells sampled against the rest-pose body with rays
   (`ray_extent`: the farthest body point near each ray + an offset), so nothing pokes through in the
   rest pose: apron (front sheet that follows the chest and hangs straight below the belt, with
   thickness), belt (ring round the waist over the apron), straps (ribbons laid on the surface),
   hood (spherical grid round the head with its pole forward — the face opening is the cap left out;
   closed in round the neck below the chin), cowl (rings round the torso easing from the collar to
   the shoulders), cap, gauntlets (tubes round the forearm bones), flasks / hammer / tongs / satchel
   (lathes and boxes on the belt).
4. **Weights**: the apron's skirt hangs on `pelvis` → `cloth_front` (the helper bone with baked
   secondary motion, like the looks' own cloth panels); belt items rigid on `pelvis`; hood and cap on
   `Head` (the hood's neck ring blends into `neck_01`); everything else takes the inverse-distance
   blend of the six nearest body vertices' weights (top 4, normalised).
5. **One material** (one submesh): the 1024² atlas holds the recoloured body squeezed into the left
   three quarters (columns 0–767; body UVs u × 0.75) and the outfit swatches in the right strip
   (columns 776–1019: apron, hood, lining, trim, strap, bag, flap, gauntlet, cuff, wood, iron,
   fitting, potion red / blue, cork, ember), each painted procedurally (cloth weave, leather grain and
   stitching, brushed metal, wood grain, liquid with a meniscus) with its own normal, roughness /
   metal and emissive. The body keeps the source's vertex normals; the outfit its own split normals.
6. **Clips**: `idle`, `idle_talking` (the library's talking idle: hand gestures), `interact` (a
   reach forward and back), fitted and given helper-bone motion exactly like the looks'.

### Suggested use in the client

* Load like a monster: `npcs/<key>.vmesh` + whichever of the clips exist. The client's clip list
  (`world::characterClipName`) knows `idle` only; `idle_talking` and `interact` are new names to add.
* Play `idle` normally, `idle_talking` while that merchant's shop window is open (or when a player is
  within ~4 m), `interact` once when a purchase / sale goes through.
* Facing: the meshes face engine +Z like the players; turn the merchant to the stall's front.
* **Overhead marker**: billboard `ui/shop/marker_potion` (potion sellers) or `ui/shop/marker_smith`
  (smiths), about **0.55 m** wide in world units (or 40–56 px on screen), its pin tip **0.45 m above
  the head top** (the mesh's top: 1.75–2.03 m, see the table; centre = top + 0.45 + 0.25 m), so it
  floats above the name plate. The plaque has a dark centre and a gold rim, readable on sky and on
  dark walls (`art_markers.png`, `art_icons_real.png`).

## 2. Stall props (`tools/blender/gen_shop_props.py`)

Ortaocak already has its market stall (`props/market_stall`, three in zone 1). The homelands get a
potion stall and a smithy corner each, built and baked exactly like the M13 homeland props
(procedural geometry, Cycles-baked base colour / normal / ORM / emissive, one material, the zone
props' format). Meta: `assets_src/props/shop_props_meta.json` (bounds, footprint hull, `stand`).

| Prop | Look | Size w × d × h (m) | Triangles | Maps | Emissive | Merchant `stand` (engine x, z) |
|---|---|---|---|---|---|---|
| `aydor_potion_stall` | white-stone counter, oak top, shelves of red / blue potions, blue-and-cream striped awning on gold-capped posts, pale-sun disc | 3.55 × 2.02 × 2.77 | 6 548 | 1024² | potion glass (2.0) | (0.0, −0.15) |
| `aydor_smithy` | anvil on a white-stone block, stone quench trough, oak rack with hammers and swords under a small blue lean-to, bar stock, coal bucket | 3.49 × 1.83 × 2.57 | 966 | 1024² | coals (3.0) | (0.60, −0.35) |
| `kargan_potion_stall` | dark-stone counter, potions in iron cages, iron-banded shelves, hanging roots and herbs, hide awning with an ember-red edge, lantern, brazier | 3.69 × 2.04 × 2.80 | 5 872 | 1024² | potion glass, lantern, coals (2.5) | (0.0, −0.15) |
| `kargan_smithy` | great anvil on a rock stump, iron quench trough, iron rack with spears, axes and a round shield, grindstone, coal bucket | 3.77 × 1.83 × 2.30 | 878 | 1024² | coals (3.0) | (0.55, −0.30) |

Front = engine +Z (the customer's side). `stand` is where the merchant stands, in the prop's own
frame (rotate / translate with the placement): behind the counter for the stalls, behind the anvil
for the smithies, facing +Z like the prop.

**Placement (M14 client, done)**: `tools/blender/gen_zones.py` `SHOP_STALLS` places the potion stall and the
smithy of each homeland last (nothing else moves), facing the race spawn, on the walkable ring between the
plaza and the houses: Tanyeri `aydor_potion_stall` (234, 281) and `aydor_smithy` (258, 273), Demirağız
`kargan_potion_stall` (240.5, 336) and `kargan_smithy` (276.5, 346); the walk masks of zones 2 / 3 were
regenerated with them. The merchants do **not** stand at `stand`: the server's spot rule (PROTOCOL_SHOP.md
§1.2: walkable ground 1 m round the merchant) cannot hold behind a counter whose footprint blocks the walk
mask, so each merchant stands beside its stall (about 4 m from its centre, 0.6 m forward, the stall's yaw;
`gamedata/npcs.toml`). Ortaocak's merchants stand beside two of the three `market_stall`s (the west one for
Sürmeli Bacı, the east one by the Kargan spawn for Demirci Ilgaz). The suggestion it replaced:
* Tanyeri Burcu (zone 2): both on the town plaza (≈ (250, 296)), the potion stall facing the
  spawn, the smithy against a house wall a few metres away; both are 3.5 m wide, keep 1.5 m clear in
  front for the walk mask (the footprint hull blocks, as for every prop).
* Demirağız (zone 3): the potion stall on the hearth-town plaza (≈ (256, 344)); the smithy next to
  the existing `kargan_forge` (the forge on its left, as in `art_stalls.png`), under the cliff.
* Ortaocak (zone 1): the Eşik potion seller behind one of the three `market_stall`s; the Eşik smith
  beside it or at the goods piles. There is no Eşik smithy prop; one of the two homeland smithies
  would look out of place there (white stone / dark iron), so a neutral smithy is left for later.

## 3. Potion item icons (`tools/blender/gen_shop_icons.py --only items`)

`assets_src/ui/items/<icon>.png` (128², straight alpha) → `ui/items/<icon>.vtex` (the existing
item-icon rule), rendered exactly like the weapon and loot icons (Cycles CPU, 512² box-filtered 4×,
soft dark halo, stable writer).

| Icon key | Shape | Colour |
|---|---|---|
| `potion_hp_small` | round-bellied flask, short neck, cork, a cord round the neck | red liquid |
| `potion_hp_medium` | tall shouldered bottle, long neck, dark wax-sealed stopper, cord and a paper tag | red liquid |
| `potion_mp_small` | as hp_small | blue liquid |
| `potion_mp_medium` | as hp_medium | blue liquid |

All four use **the same frame** (the medium bottle's bounds), so the small flask is drawn smaller in
the slot (≈ 70 % of the medium's height) as well as rounder: size and silhouette both differ. The
liquid fills the lower part, the glass above it is pale; the liquids glow a little so they stay
saturated on dark slots. Checked at 72 / 64 / 48 / 40 px in the slot art (`art_icons_real.png`,
`art_icons_enlarged.png`; 72 px = a 48 px slot at 150 % UI scale).

## 4. Shop UI art (`tools/blender/gen_shop_icons.py --only ui`)

`assets_src/ui/shop/<name>.png` → `ui/shop/<name>.vtex`: assetc's UI-icon rule now covers `ui/shop/`
(one line in `tools/assetc/Build.cpp`, `isUiIcon`). Painted with the HUD glyphs' SDF painter, ink
outline and gold / ivory palette (`gen_hud_art.py`), symbol only on a transparent background.

| Name | Size | What | Use |
|---|---|---|---|
| `akce` | 64² | one Akçe: gold disc, raised rim, a stamped square hole in a diamond | price column, purse line (reads at 16 px); the existing `ui/items/coin` (a stack) stays the inventory / loot icon |
| `tab_al` | 64² | open palm, a red flask dropping into it, a green arrow down | the "Al" (buy) tab |
| `tab_sat` | 64² | open palm holding out a coin, an amber arrow up | the "Sat" (sell) tab |
| `marker_potion` | 128² | gold-rimmed round plaque with a pin, a red potion flask | overhead marker, potion sellers |
| `marker_smith` | 128² | the same plaque, hammer over anvil with sparks | overhead marker, smiths |

The tab glyphs are a pair (same palm, opposite arrows, goods vs coin) and also sit in the M12 slot
art (`slot_back` + glyph + `slot_frame_*`) like the HUD glyphs.

## 5. Regenerating

```powershell
$b = "C:\Program Files\Blender Foundation\Blender 5.3\blender.exe"
& $b --background --factory-startup --python tools/blender/gen_npcs.py           # ~90 s per merchant; --only <key>, --no-preview
& $b --background --factory-startup --python tools/blender/gen_shop_props.py     # ~20 s per prop;  --only <name>, --no-preview
& $b --background --factory-startup --python tools/blender/gen_shop_icons.py     # ~25 s;          --only items|ui, --sheets <dir>
& $b --background --factory-startup --python tools/blender/gen_shop_shots.py     # review renders into docs/screenshots/m14 (not assets)
powershell -File tools/build-assets.ps1 -Config Debug                            # compile into assets/
```

`tools/blender/build_all.ps1` runs the three generators (`gen_shop_props.py` after
`gen_zone_props.py`, `gen_npcs.py` after `gen_monsters.py`, `gen_shop_icons.py` after `gen_loot.py`).
All three are deterministic (fixed seeds, the stable writers for path-traced output).
Contact sheet of the icons: `build/art_sheets/m14/shop_icons.png` (git-ignored).

Changing a merchant: its row in `gen_npcs.NPCS` (base look, palette, colours); the outfit pieces are
in `outfit_for`. Adding one: a row there and the key in `kNpcs` of `ShopTests.cpp`.

## 6. Review renders (`docs/screenshots/m14/`)

| File | Shows |
|---|---|
| `art_npcs_lineup.png` | the six merchants (idle pose) between two player looks for scale |
| `art_npcs_esik.png`, `art_npcs_aydor.png`, `art_npcs_kargan.png` | per town: a player of that race, the potion seller, the smith; front and three-quarter |
| `art_npcs_closeup.png` | the six from the chest up, three-quarter |
| `art_npcs_talk.png` | `idle_talking` and `interact` frames (Aydor apothecary, Kargan smith) |
| `art_stalls.png` | the four stalls with their merchants at `stand` (Kargan smithy beside `kargan_forge`), Ortaocak's market stall with the Eşik merchants |
| `art_markers.png` | two merchants with their overhead markers at the suggested height, a player in front |
| `art_icons_real.png` | potion icons and shop glyphs at real pixel sizes in the slot art |
| `art_icons_enlarged.png` | the slot rows of the same sheet, 3× nearest |

## 7. Known weak spots (from reviewing the renders)

* Each merchant shares its face, body and boots with a player look (`aydor_0..2`, `kargan_0..2`);
  the outfit and palette change the silhouette, not the face. `npc_smith_kargan` is built on
  `kargan_2`, which is itself an elder smith; the dark palette, cap, gauntlets and tools set him
  apart, but he stays the closest to a player look.
* The Kargan ember-brewer's cowl is still boxy: `kargan_1`'s fur collar is wide at the top, so the
  cowl starts wide; it reads as a dark square mantle from the front.
* The soft caps are simple domes with a rolled band: the Eşik smith's olive felt cap can read as a
  bowl / helmet at a distance; the Aydor smith's white cap as a baker's cap.
* Aprons are stiff slabs (no cloth simulation): they follow `cloth_front` as a whole and the legs can
  touch the inside in the idle sway; the potion flasks on the belt are rigid on `pelvis` and the apron
  skirt can move over them by a centimetre or two.
* The outfit swatches are flat procedural textures (weave, grain, stitches) next to the scanned-
  looking Tripo body textures; up close the outfit looks cleaner and simpler than the body.
* Only `idle`, `idle_talking` and `interact` exist; a merchant cannot walk.
* The new stall props are low-poly and clean-stylised like the M13 homeland props (900–6 500
  triangles), simpler than Ortaocak's generated market stall.
* The potion icons' glass reads pale grey-white above the liquid; at 40 px the cord and the tag on the
  medium bottle disappear (size, shape and colour still separate the four).
