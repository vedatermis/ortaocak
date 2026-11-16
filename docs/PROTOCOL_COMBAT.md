# VedatOnline Network Protocol — Combat extension (v3)

Status: **contract for milestone M9 "İlk Kan"**. Extends docs/PROTOCOL.md and docs/PROTOCOL_ITEMS.md (v2, shipped); everything there stays valid.
`PROTOCOL_VERSION` becomes **3** when this ships (client and server are released together; an older client is refused with VersionMismatch). Where this text says "M8" read "M9" (it was drafted before the items milestone took that number).

Design goals for M8: a player can target a monster, hit it with a basic attack and one class skill, take damage, die and respawn, gain experience and level up; other players see all of it. Equipped items count (§1.1). No loot, no PvP, no parties yet.

## 1. Model

- Everything that can fight is an **entity** with `hp`, `max_hp`, `level`. Players also have `mp`, `max_mp`, `xp`.
- `entity_type`: 1 = player (as before), **2 = monster**.
- All combat is **server-authoritative**. The client sends intents ("attack this target", "use skill N on this target"); the server validates range, cooldown, line of sight (M8: range only), state, and resolves damage on its tick. The client never reports damage.
- Time base: the server tick (20 Hz). Durations are in milliseconds, rounded up to ticks.
- Zone rule: **Ortaocak's oath ground** — inside the village safe radius (zone data: `safe_zones`, circle x,z,r) nobody can be attacked and monsters do not enter. M8 has no PvP at all; `ATTACK`/`SKILL` on a player is rejected with `NotAttackable`.

### 1.1 Stats (derived on the server, sent to the owner)

| Stat | Formula (M8) |
|---|---|
| max_hp | class_base_hp + level × class_hp_per_level |
| max_mp | class_base_mp + level × class_mp_per_level |
| attack | class_base_attack + level × class_attack_per_level + Σ `attack` of equipped items |
| defense | class_base_defense + level × class_defense_per_level + Σ `defense` of equipped items |
| attack_range | melee 2.5 m (Warrior, Rogue, Priest), 14 m (Mage) |
| attack_interval | Warrior 1600 ms, Rogue 1200 ms, Mage 1800 ms, Priest 1700 ms |

Class tables live in `shared/game/Combat.h` (single source for client display and server maths). Race gives no stat difference in M8.

Equipment (docs/PROTOCOL_ITEMS.md): the bonuses of every item in the Equipment container are added; a successful equip/unequip recomputes the stats at once and sends `SELF_STATS`. Fighting bare-handed is allowed (no bonus). Tune the class base tables so that a level-1 character with its starter kit kills a level-1 `Kül Çömezi` in roughly 4–6 basic attacks and survives about 10 of its hits. Moving items is refused with `ITEM_MOVE_RES(SlotBlocked)` while dead; it is allowed in combat.

Damage: `max(1, round(attack × skill_multiplier × uniform(0.9, 1.1) − defense × 0.5))`; critical hit chance 5 % (Rogue 15 %), ×1.5. Monsters use the same formula with their own attack/defense.

Regeneration: out of combat (no damage dealt or taken for 6 s) 2 % max_hp and 3 % max_mp per second; sitting (see §3.6) doubles it.

Experience: killing a monster grants `monster.xp` to the player who dealt the most damage (M8: no sharing), reduced by 20 % per level the player is above the monster beyond 2, zero at 7+. Level cap 10 in M8. `xp_to_next(level) = 80 × level^1.6` rounded to 10.

Death: at 0 hp a player is dead (cannot move, attack or be targeted); `RESPAWN_REQ` returns them to their race spawn in the zone with 50 % hp/mp after a 5 s minimum. No experience loss in M8. A dead monster despawns after 3 s and its spawn point respawns it after `respawn_ms`.

### 1.2 Skills (M8: one per class)

| id | Class | Name | Target | Range | Cost | Cooldown | Effect |
|---|---|---|---|---|---|---|---|
| 1 | Warrior | Ağır Darbe | enemy | 2.5 m | 12 mp | 6 s | ×2.2 damage |
| 2 | Rogue | Çifte Kesik | enemy | 2.5 m | 10 mp | 5 s | two hits ×1.2, each can crit |
| 3 | Mage | Köz Oku | enemy | 16 m | 14 mp | 3 s | ×2.0 damage, 600 ms cast, projectile |
| 4 | Priest | Soluk Güneş | self or friendly player | 14 m | 16 mp | 4 s | heal 25 % of target max_hp, 800 ms cast |

A cast is interrupted by moving; an interrupted cast costs nothing and starts no cooldown. Basic attacks and skills share a 500 ms global cooldown.

### 1.3 Monsters (M8)

Defined in data (`assets_src/gameplay/monsters.toml`, compiled into the zone or loaded by the server): id, name, model, level, hp, attack, defense, attack_range, attack_interval, move_speed, aggro_radius, leash_radius, xp, respawn_ms. Spawn points per zone: monster id, position, wander radius, count.

AI states: `Idle` (wander inside the spawn radius) → `Chase` (a player enters aggro_radius or damages it; target = highest threat, threat = damage dealt, healing counts ×0.5 toward the healer) → `Attack` (in range, on interval) → `Return` (target dead/gone/outside leash_radius from the spawn point: walk home, heal to full, ignore damage while returning). Monsters move on walkable cells only, with the same terrain data as players.

M9 roster (lore: Külden Doğanlar, docs/LORE.md). All three are humanoid so they use the character skeleton and its clips (`assets/monsters/<model>.vmesh`, clips `monsters/<model>/<clip>.vanim`): `Kül Çömezi` (ash thrall, model `ash_thrall`, lvl 1–2, small, quick, weak; several packs just outside the village), `Cüruf Yürüyen` (slag walker, model `slag_walker`, lvl 3–4, slow, tough; further out), `Kor Bekçisi` (ember warden, model `ember_warden`, lvl 6, one elite near a shard at the edge of the plateau, drawn 1.25× size). Monster definitions and spawn points live in `gamedata/monsters.toml` (shared loader like items; this replaces the `assets_src/gameplay/monsters.toml` path named above).

## 2. Changes to existing packets

```
ENTER_WORLD_RES   (append after server_time_ms)
                  u16 level
                  u32 xp, u32 xp_to_next
                  u32 hp, u32 max_hp
                  u32 mp, u32 max_mp
                  u16 attack, u16 defense
                  u8  is_dead

ENTITY_SPAWN      (append after off_hand_template)
                  u32 hp, u32 max_hp
                  u16 template_id        // monster id for entity_type 2, 0 for players
                  u8  state              // bit0 dead, bit1 in_combat, bit2 sitting
```

For monsters, `ENTITY_SPAWN.name` is the monster's display name, `race/class/face/hair` are 0, `level` is its level. Monster movement uses `ENTITY_MOVE` exactly like players (flags bit0 moving, bit1 running).

`MOVE_REQ` from a dead or casting-locked player: dead → `POSITION_CORRECT`; moving while casting is allowed and interrupts the cast (§1.2).

## 3. New opcodes

| Opcode | Name | Dir |
|---|---|---|
| 0x0300 | TARGET_REQ | C→S |
| 0x0301 | ATTACK_REQ | C→S |
| 0x0302 | ATTACK_STOP_REQ | C→S |
| 0x0303 | SKILL_REQ | C→S |
| 0x0304 | ACTION_FAIL | S→C |
| 0x0305 | COMBAT_EVENT | S→C |
| 0x0306 | CAST_START | S→C |
| 0x0307 | CAST_END | S→C |
| 0x0308 | ENTITY_STATS | S→C |
| 0x0309 | SELF_STATS | S→C |
| 0x030A | XP_GAIN | S→C |
| 0x030B | LEVEL_UP | S→C |
| 0x030C | ENTITY_DEATH | S→C |
| 0x030D | RESPAWN_REQ | C→S |
| 0x030E | RESPAWN_RES | S→C |
| 0x030F | ENTITY_STATE | S→C |
| 0x0310 | STANCE_REQ | C→S |

All are valid only in `InWorld`.

### 3.1 Targeting and basic attack

```
TARGET_REQ        u32 entity_id          // 0 clears; informational, lets the server show "X targets you" later
ATTACK_REQ        u32 entity_id          // start auto-attacking this entity
ATTACK_STOP_REQ   (empty)
ACTION_FAIL       u8 action              // 1 attack, 2 skill, 3 respawn, 4 stance
                  u16 skill_id           // 0 unless action == 2
                  u8 reason
```

`reason`: 1 NoTarget · 2 OutOfRange · 3 NotAttackable · 4 TargetDead · 5 YouAreDead · 6 OnCooldown · 7 NotEnoughMp · 8 WrongTargetType · 9 Interrupted · 10 InSafeZone · 11 UnknownSkill · 12 Busy.

Auto-attack: after `ATTACK_REQ` the server swings whenever the target is alive, in `attack_range` (+0.5 m tolerance, measured horizontally between positions) and the attack interval has elapsed. Out of range does **not** cancel the attack; it resumes when back in range (the client walks the player there). It ends on `ATTACK_STOP_REQ`, target death/despawn, own death, or a new `ATTACK_REQ`. The first out-of-range check after `ATTACK_REQ` sends `ACTION_FAIL(OutOfRange)` once so the client can show it.

### 3.2 Skills

```
SKILL_REQ         u16 skill_id
                  u32 target_id          // 0 = self where the skill allows it
CAST_START        u32 caster_id, u16 skill_id, u32 target_id, u16 cast_ms    // to caster and observers
CAST_END          u32 caster_id, u16 skill_id, u8 result                      // 0 completed, 1 interrupted
```

Instant skills send no CAST_START/CAST_END; their `COMBAT_EVENT` is the notification.

### 3.3 Results

```
COMBAT_EVENT      u32 source_id
                  u32 target_id
                  u16 skill_id           // 0 = basic attack
                  u8  kind               // 1 damage, 2 heal, 3 miss (unused in M8)
                  u8  flags              // bit0 critical, bit1 killing blow
                  u32 amount
                  u32 target_hp          // after applying
                  u32 server_time_ms

ENTITY_STATS      u32 entity_id, u32 hp, u32 max_hp, u16 level     // regen ticks, level changes; rate-limited to 2 Hz per entity
SELF_STATS        u32 hp, u32 max_hp, u32 mp, u32 max_mp, u16 attack, u16 defense   // to the owner on change, ≤ 4 Hz
ENTITY_STATE      u32 entity_id, u8 state                           // bit0 dead, bit1 in_combat, bit2 sitting
ENTITY_DEATH      u32 entity_id, u32 killer_id
XP_GAIN           u32 amount, u32 xp, u32 xp_to_next, u32 source_entity_id
LEVEL_UP          u32 entity_id, u16 level                          // to the player and observers; SELF_STATS follows for the owner
```

`COMBAT_EVENT`, `CAST_*`, `ENTITY_STATS`, `ENTITY_STATE`, `ENTITY_DEATH`, `LEVEL_UP` go to every player who has the **target or the source** spawned (AOI), plus the participants themselves. A recipient that does not know one of the two ids ignores that side (e.g. shows the damage number without a swing animation).

### 3.4 Death and respawn

```
RESPAWN_REQ       (empty)
RESPAWN_RES       u8 result              // 0 ok, 1 NotDead, 2 TooEarly
                  f32 x, f32 y, f32 z, f32 yaw
                  u32 hp, u32 mp
```

Observers get `ENTITY_STATE` (dead cleared) and an `ENTITY_MOVE` with the new position (the client snaps when the distance exceeds its teleport threshold).

### 3.5 Persistence

`characters` gains `xp`, `hp`, `mp` (migration `0003_combat.sql`; level already exists; existing characters start at full hp/mp, xp 0). Saved with the position (same cadence) and on level-up. A character that logs out dead logs in at its race spawn with 50 % hp/mp.

### 3.6 Stance (sitting and other visible poses)

```
STANCE_REQ        u8 stance              // 0 stand, 1 sit
```

Result is broadcast as `ENTITY_STATE` (bit2). Sitting is refused while moving, casting, dead or in combat (`ACTION_FAIL(action=4, Busy)`); any accepted `MOVE_REQ` with the moving bit, any attack or skill, or taking damage stands the player up. This replaces the client-local sit from the polish round; jumping stays cosmetic and local. The client plays the ground-sit clips (`sit_ground_enter/idle/exit`), not the chair ones.

## 4. Limits

| Limit | Value |
|---|---|
| ATTACK_REQ / SKILL_REQ / TARGET_REQ / STANCE_REQ | 10 per second combined → `ERROR(Flood)` |
| Skill ids | 1–4 in M8; unknown → `ACTION_FAIL(UnknownSkill)` |
| Target id | must be spawned to the requester, else `ACTION_FAIL(NoTarget)` |

## 5. Client presentation contract (not wire format, but agreed behaviour)

- Left click on an entity selects it (target frame with name, level, hp bar); double click or pressing the attack key starts auto-attack and walks into range; skill on key `1`. Tab cycles nearest monsters. Esc clears the target before opening the menu.
- Animations: basic attack → `sword_attack` when a weapon with grip sword/dagger/mace/axe2h is in the main hand, `spell_shoot` for a staff, `punch_jab`/`punch_cross` alternating when bare-handed; monsters use `punch_jab`/`punch_cross`; skills → Mage/Priest `spell_enter` → `spell_idle` (cast) → `spell_shoot`; hit reaction `hit_chest` on taking damage when not attacking; `death` on death, held on the last frame; `sit_*` for the sitting state.
- Floating damage numbers over the target (white normal, yellow critical, green heal, red for damage the local player takes), hp bars over monsters and over damaged players, own hp/mp/xp bars in the HUD, a skill button with cooldown sweep, a death overlay with a "Yeniden Doğ" button enabled after 5 s, a level-up flash and message.
- Monsters render with the same skinned-mesh path as players; their name label is coloured by level difference (grey / white / yellow / red).
