# VedatOnline — Protocol v9: levels 1–20, hunting slots, armour and bosses (M15 "Avlaklar")

Protocol version **9** (`shared/net/Opcodes.h` `kProtocolVersion`). A v8 client is refused at HELLO as
before (`HelloResult::VersionMismatch`). M15 adds **no new opcode and no new field**; v9 widens two value
ranges and adds one result code. Everything else in this document is game data shared by client and
server (`gamedata/*.toml`, `shared/game/`), listed here because both sides must agree on it.
Development notes, numbers and measurements: `docs/DEVELOPMENT_PLAN.md` §35.

## 1. Level cap 20

| | Before (v8) | v9 |
|---|---|---|
| `game::kLevelCap` | 10 | **20** |
| Experience to the next level | `80 · L^1.6` (rounded to 10) | same for L = 1..9; from L = 10: `80 · L^1.6 · (1 + 0.6 · (L − 9))` |
| Stat points | 3 per level above 1 | unchanged (level 20: 57) |
| Skill points | 1 per level above 1 | unchanged (level 20: 19) |
| hp / mp / attack / defense per level | class tables (`Combat.cpp`) | unchanged, linear up to 20 |

Characters stored before M15 keep their level and xp (the curve below level 10 is unchanged; a level-10
character had xp 0 and now gains experience again). `CHAR_STATS.level`, `SELF_STATS.level` and the
character list already carried a u16 level. `vo_admin set-level` clamps to 1..20.

| L | xp to L+1 | L | xp to L+1 |
|---|---|---|---|
| 1 | 80 | 10 | 5 100 |

| 2 | 240 | 11 | 8 160 |
| 3 | 460 | 12 | 11 940 |
| 4 | 740 | 13 | 16 480 |
| 5 | 1 050 | 14 | 21 830 |
| 6 | 1 410 | 15 | 28 030 |
| 7 | 1 800 | 16 | 35 130 |
| 8 | 2 230 | 17 | 43 180 |
| 9 | 2 690 | 18 | 52 200 |
| | | 19 | 62 260 |

## 2. Skill ranks 4 and 5

Every skill has five ranks (`game::kMaxSkillRank` = 5). Ranks 1–3 are open at any level; **rank 4 needs
character level 11, rank 5 level 16** (`gamedata/skills.toml` `[rules] rank_levels = [1, 1, 1, 11, 16]`).

Wire changes:

* `SKILL_STATE` / `SKILL_LEARN_RES` / `SKILL_STATE` entries: `rank` is now **0..5** (`kMaxSkillRankWire`;
  0..3 before). A reader refuses a larger value as before.
* `SKILL_LEARN_RES.result` gains **`LevelTooLow = 6`**: the next rank needs a higher character level
  (nothing changed). The checks run in the order UnknownSkill, MaxRank (rank 5), NotEnoughPoints,
  TierLocked, LevelTooLow (`game::checkSkillLearn`). Client text: "Kademe {n} için {L}. seviyeye
  ulaşmalısın."; the tooltip names the level of a gated next rank ("Sonraki kademe (11. seviye): …").
* Database: migration `0008_skill_ranks_5.sql` widens `character_skills.rank` to 1..5.
* `vo_admin set-level` resets the ranks if they no longer fit the lower level (points **or** rank levels,
  `game::skillRanksFitLevel`).

Rank values: `skills.toml` writes five values per ranked number; a three-value array (the M12 form, used by
test data) is still read, ranks 4 and 5 then continue the rank 2 → 3 step (integers rounded, clamped).

## 3. Hunting slots (`[[slot]]`, data only)

A slot is a named, compact pack of one kind of monster at fixed home points (`gamedata/monsters.toml`,
`game::MonsterSlot`). The server expands every slot into one spawn per member (`MonsterSpawn::slot` = slot
index + 1); `ENTITY_SPAWN` is unchanged (a member is a monster like any other).

### 3.1 Fields

`key`, `name`, `zone`, `x`, `z`, `monsters` (template ids of one kind — same name and model; members
take them in turn), `count` (1..12; an elite stands alone), `ring` (radius of the home ring), `centre`
(one member at the centre; default for 7+ members), `start_angle` (degrees, 0 = +z), `wander` (default
1 m), `respawn_ms`, `leash` (overrides of the templates'), or `homes = [[dx, dz], …]` instead of the ring.
Area radius = farthest home + wander, at most 9 m.

### 3.2 Rules

| Where | Rule |
|---|---|
| load (`parseMonsterData`) | one kind per slot; homes ≥ 2.5 m apart; area ≤ 9 m; two slots of a zone at least `areaA + areaB + max(aggroA, aggroB) + 4 m` apart; area + aggro clear of every safe zone and loose spawn; respawn ≥ 3000 ms; leash > aggro |
| server start (`Zone::installMonsters`, `game::checkSlotTerrain`) | centre, every home and 8 points of its wander circle walkable; a straight walkable line from every home to the centre; every home reachable on foot from the zone's first `spawn_` / `arrive_` marker (4-neighbour flood fill of the walk mask); area + aggro ≥ gate radius + 12 m from every gate marker and ≥ 8 m from every `spawn_` / `arrive_` marker (a boss: its leash + 10 m) |

`vo_server` refuses to start (exit 6) if any rule fails, naming the slot.

### 3.3 Behaviour

* A member wanders within `wander` of its home, returns there at full hp when its target leaves the
  leash (from the home) or it gives up, and **respawns at its home** `respawn_ms` after its death.
* No monster aggroes on behalf of another (as since M9): pulling one member brings only those whose own
  aggro radius covers the player; the gap rule keeps the next slot out of that reach.
* The client shows the slot's name under the zone name (top right, the place of "● Güvenli Bölge") while
  the player stands within area + 6 m (`game::kSlotShowMargin`): "◆ Kül Çukuru 2 · Sv. 1-2" (amber).
  Automation state words: `slot`, `slot_<key>`.

## 4. Items: armour, the "unique" rarity, tints (data only)

* **Armour** (`kind = "armor"`, slots `head`, `chest`, `legs`, `hands`, `feet`): equipment slots 2–6 of
  the Equipment container, which existed on the wire and in the database since v2 (no migration). An
  armour piece goes only into its own slot (`ItemMoveResult::WrongSlot` otherwise); class, level and
  attribute rules as for weapons; `defense` and the attribute bonuses count like a weapon's. Armour has no
  `model` / `grip` (not drawn on the character yet) and is not part of `ENTITY_EQUIP` (visible equipment
  stays the two hands). Double-click / right-click equips it into its slot.
* **Rarity `unique`** ("Eşsiz", name colour `#F0A23C`): the boss drops. Shipped uniques have `value = 0`:
  they cannot be sold (`SHOP_SELL_RES` `CannotSell`) or bought; the tooltip says "Satılamaz". They follow
  the normal loot ownership, inventory and stack rules.
* **`tint = [r, g, b]`** (optional, any kind): multiplies the item's icon everywhere it is drawn and the
  weapon mesh in the hand (sRGB → linear), so items can share art. Presentation only.

## 5. Bosses (data and testing hooks)

Five monsters with `elite = true` are the bosses: Kor Bekçisi (6, Ortaocak), Anız Ağası (10, Tanyeri),
Körük Hanı (10, Demirağız), Cüruf Anası (15, Ortaocak), Ocak Efendisi (20, Ortaocak). Each stands alone in
its own named slot, respawns after 10 / 20 / 20 / 30 / 45 minutes and drops its own unique items. The
client shows "Boss" (gold chip) on the target frame and "· Boss" on the name label (it said "Elit").

Testing hooks (server config, default off; warned in the log):

| Key | Effect |
|---|---|
| `world.testing_boss_respawn_ms` (0 or 3000..86400000) | every elite respawns after this instead of its data's time |
| `world.testing_force_unique_drop` (0 / 1) | every boss loot roll also holds one of its table's unique items (`game::forceUniqueDrop`) |

## 6. Loot tables

A table may hold up to **24** drop lines (8 before); a roll keeps the first 8 items that drop, in line
order (a container still holds at most 8 entries, `LOOT_OPEN_RES` unchanged).
