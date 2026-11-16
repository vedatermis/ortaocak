# VedatOnline Network Protocol — Skills extension (v6)

Status: **contract for milestone M12 "Yetenek Yolları"** (skill points, two skill trees per class,
an 8-slot skill bar, timed effects). Extends docs/PROTOCOL.md, PROTOCOL_ITEMS.md (v2),
PROTOCOL_COMBAT.md (v3), PROTOCOL_LOOT.md (v4) and PROTOCOL_STATS.md (v5); everything there stays
valid unless this document says otherwise. `PROTOCOL_VERSION` becomes **6** (client and server are
released together; an older client is refused with `VersionMismatch`).

Goal: every class has two **branches** (skill trees) of skills. Each level above 1 grants one
**skill point**; points raise a skill's **rank** (0 = not learned, 1..3). Higher tiers of a branch
open once enough points are spent in it. Learned skills go onto an **8-slot skill bar** (keys 1–8).
Skills can put **timed effects** on entities (buffs on players; damage over time, slows, stuns,
taunts and the like on monsters) that every observer sees. All names, texts and numbers are our
own.

The numbers of every skill live in `gamedata/skills.toml` (loaded and validated by shared code,
`shared/game/Skills.h`); the tables in this document are the shipped values (§2) and must be kept
in step with the file. Client and server use the same file.

## 1. Rules

### 1.1 Points

- `points(level) = points_per_level × (level − 1)` with `points_per_level = 1` (skills.toml
  `[rules]`), level clamped to 1..level cap (10): level 1 has 0, level 10 has 9. A later cap raise
  only changes data.
- `spent = Σ over the class's skills (rank − base)`, where `base` is 1 for the class's **free
  skill** and 0 otherwise; `unspent = max(0, points(level) − spent)`.
- Characters from before M12 have no learned skills: they get all points of their level, unspent.
- If a stored spending exceeds `points(level)` (level lowered outside the server) `unspent` is 0 and
  the ranks stay as they are; `vo_admin set-level` resets them (§6.2).
- Spending is permanent: no refund in game. `vo_admin reset-skills <character>` sets every rank back
  (free skill to 1, everything else to 0) and clears the bar to its default.

### 1.2 Ranks, branches, tiers

- Each skill belongs to one class, one branch (0 or 1 of that class) and one tier (1..3).
- Ranks 0..3 (`kMaxSkillRank`). **Free skill**: the tier-1 skill of branch 0 of each class (ids 1,
  2, 3, 4 — the M9 class skills) is rank 1 from the start and can never go below 1.
- **Tier gate**: learning (any rank of) a tier-2 skill needs at least **2** points spent in that
  branch, tier 3 at least **4** (`tier_points = [0, 2, 4]` in `[rules]`). Points spent on the skill
  being raised count too (raising a tier-2 skill from 1 to 2 needs the gate only once, at rank 1).
  "Points spent in a branch" = Σ (rank − base) over that branch's skills.
- One rank per request (`SKILL_LEARN_REQ`, §3.3).

### 1.3 Skill scaling (formulas)

Every skill scales with the M11 attributes **only through the derived stats** of
PROTOCOL_STATS.md §2 (no hidden formulas): `A` = the caster's attack (a Mage's magic power) with
active effects (§1.5), `H` = the caster's heal multiplier `1 + (INT + SPI) / 60`, `crit` = the
caster's critical chance. Values marked "per rank" are three numbers (rank 1, 2, 3).

| Part | Formula |
|---|---|
| direct damage (each hit) | `max(1, round((A × m × uniform(0.9, 1.1) − D × 0.5) × (crit ? 1.5 : 1)))`, `m` = the rank's `damage`, `D` = target defense; critical chance = `crit + crit_bonus`, at most 85 % |
| heal | `max(1, round(target_max_hp × heal% × H))` |
| damage over time (DoT), per tick | `max(1, round(A × value%))`, snapshot when applied; ignores defense, never critical |
| heal over time (HoT), per tick | `max(1, round(target_max_hp × value% × H))`, snapshot when applied |
| absorb shield | `max(1, round(A × value%))` points of damage, snapshot when applied |
| ticks | every `tick_ms` (1000 ms), the first one `tick_ms` after the effect started; `duration / tick` ticks |

Monster damage against a player: `max(1, round(monster_attack × (1 − weaken%) × uniform(0.9, 1.1) −
player_defense × 0.5))` (×1.5 on a 5 % monster critical), then an absorb shield takes what it can.

### 1.4 Targets and areas

`target` of a skill (skills.toml):

| target | SKILL_REQ.target_id | Meaning |
|---|---|---|
| `enemy` | a monster spawned to the caster | as v3 Enemy skills (range, safe zone, returning monsters refused) |
| `friendly` | 0 = self, or a friendly player spawned to the caster | as v3 Soluk Güneş; no party system yet: **any** living player in range is friendly |
| `self` | ignored (send 0) | the caster |

`area` (optional): `around_self` or `around_target` with `radius` (m) and `max_targets` (1..16):
every valid entity of the skill's `affects` kind (`enemies` = monsters: alive, not returning home,
spawned to the caster, outside safe zones; `allies` = living players spawned to the caster, the
caster included) whose horizontal distance to the centre is ≤ radius (+0.5 m tolerance), nearest
first, at most `max_targets`. For `around_target` the primary target is always one of them. An
`around_self` enemy skill that finds nobody is refused with `ACTION_FAIL(NoTarget)` and costs
nothing; inside a safe zone `ACTION_FAIL(InSafeZone)`.

**Leap** (`leap = true`, Gölge Adımı): an `enemy` skill that first moves the caster next to the
target. Landing point: on the line from the caster to the target, `leap_stop` (1.5 m) before the
target. It must be walkable, outside safe zones, and the straight segment from the caster to it
walkable (the shared `isSegmentWalkable`); otherwise the server tries the points 1.5 m from the
target at ±30°, ±60°, ±90° from that line (each again with the segment check); if none works the
skill is refused with `ACTION_FAIL(NoPath)` and costs nothing. Closer than `leap_stop` + 0.5 m: no
movement. The server moves the caster (AOI bookkeeping, observers get `ENTITY_MOVE`), cancels its
`MOVE_REQ` credit, sends `SKILL_LEAP` (§3.7) to the caster and its observers and a
`POSITION_CORRECT` to the caster (moves the client sent before it learned of the leap are corrected
anyway), then resolves the hit.

### 1.5 Effects

A skill has 0..2 effects (`[[skill.effect]]`), each with a `kind`, `on` (`target` = every entity
the skill hits, or `self`), a per-rank `duration_ms` and a per-rank `value`. Kinds (the wire value
is the number):

| # | kind | lands on | value means | effect |
|---|---|---|---|---|
| 1 | `attack_up` | players | % | attack × (1 + Σ value / 100) |
| 2 | `defense_up` | players | % | defense × (1 + Σ value / 100) |
| 3 | `haste` | players | % | basic attack interval ÷ (1 + Σ value / 100) |
| 4 | `dodge_up` | players | % points | dodge chance + Σ value / 100, at most 50 % in total |
| 5 | `max_hp_up` | players | % | max hp × (1 + Σ value / 100); applying raises hp by the gain, expiry clamps hp |
| 6 | `dot` | monsters | % of A per tick | damage over time (§1.3) |
| 7 | `hot` | players | % of max hp per tick | heal over time (§1.3) |
| 8 | `slow` | monsters | % | move speed × (1 − value / 100); attack interval × (1 + value / 200) |
| 9 | `stun` | monsters | — | no movement, no attacks |
| 10 | `root` | monsters | — | no movement (attacks whoever is in reach) |
| 11 | `shield` | players | % of A | absorbs damage (§1.3) until used up or expired |
| 12 | `weaken` | monsters | % | the monster's attack × (1 − value / 100) |
| 13 | `taunt` | monsters | — | the monster must attack the effect's source (§1.6) |

- Helpful kinds (1–5, 7, 11) only land on players, harmful kinds (6, 8–10, 12, 13) only on
  monsters (there is no PvP and monsters have no skills in v6). A stat-changing effect on a player
  recomputes its stats at once (CHAR_STATS / SELF_STATS / ENTITY_STATS as in v5: their values are
  always the *effective* ones, with active effects).
- **Identity** on one entity = (skill id, kind). Applying the same pair again **refreshes** it
  (new duration, value, amount and source; `EFFECT_APPLY` again); different skills with the same
  kind add up (percentages are summed; several DoTs tick independently). `stacks` is always 1 in
  v6 (reserved). At most 16 effects per entity (a 17th is dropped silently; unreachable with the
  shipped data).
- Several slows: the strongest counts (not summed). Several weakens: summed, at most 75 %.
- **Expiry**: at `start + duration` → `EFFECT_REMOVE(Expired)`. A shield used up →
  `EFFECT_REMOVE(Depleted)`. A monster that starts returning home loses all its effects
  (`EFFECT_REMOVE(Cleared)` each). **Death** clears every effect of the dead entity **without**
  `EFFECT_REMOVE`: a client clears an entity's list on its `ENTITY_DEATH`. Despawn: the client
  forgets the entity and its list. Effects do not persist across a relog (buffs are lost).
- A DoT whose caster left the world keeps ticking; its damage counts for no player (threat and
  experience as for any damage, credited to the caster while it is in the zone).
- Stunned / rooted monsters stand still (a stunned one does not attack); a stun interrupts nothing
  else. A monster stunned while chasing keeps its leash timer paused.

### 1.6 Taunt

`taunt` on a monster: for its duration the monster's target is the effect's source if that player
is a valid target (alive, in view, outside safe zones, inside the leash) — even if others have more
threat; when applied, the source's threat on that monster is raised to the highest threat on it +
10 % (so it stays the target afterwards unless others out-damage it), and an idle monster starts
chasing. Several taunts on one monster: the latest wins.

### 1.7 Cooldowns and the global cooldown

- Every skill has its own cooldown (per rank in data, the same for all ranks in the shipped file);
  basic attacks and skills share the 500 ms global cooldown (unchanged).
- When a skill executes the caster gets `SKILL_COOLDOWNS` with that skill's entry (§3.6). Cast
  skills start their cooldown at cast end; an interrupted cast starts none (unchanged).
- **Relog**: the world remembers a character's running cooldowns for 60 s after it left (the same
  memory that keeps its last position, SERVER_WORLD.md §6); on the next entry they continue and
  the client gets them in `SKILL_COOLDOWNS` (§4). After 60 s, or after a server restart, everything
  is ready (no shipped cooldown is longer than 60 s, enforced by the loader). Not persisted in the
  database.

### 1.8 SKILL_REQ in v6

`SKILL_REQ(skill_id, target_id)` is unchanged on the wire and still works by skill id (the bar is
presentation only; any learned skill can be used without being on the bar). Checks, first failure
wins: unknown id or another class's skill → `UnknownSkill`; rank 0 → **`NotLearned` (13)**; dead →
`YouAreDead`; casting → `Busy`; global or own cooldown → `OnCooldown`; mana below the rank's cost →
`NotEnoughMp`; target checks (§1.4, v3 reasons; leap without a landing point → **`NoPath`
(14)**). The rank used is the caster's current rank at execution (cast end for cast skills).

## 2. Shipped skills (gamedata/skills.toml)

Columns: id, branch / tier, target (area radius m / max targets), range m, mp, cooldown s, cast s,
per-rank numbers `r1 / r2 / r3`. "×" = damage multiplier of A; durations in seconds.

### Warrior (Savaşçı) — A = attack (Güç)

| id | name | branch / tier | target | range | mp | cd | cast | per rank |
|---|---|---|---|---|---|---|---|---|
| 1 | Ağır Darbe | Hücum / 1 (free) | enemy | 2.5 | 12 | 6 | — | ×2.2 / 2.5 / 2.8 |
| 5 | Yarma | Hücum / 2 | self, enemies around self (3 / 5) | — | 14 | 8 | — | ×1.4 / 1.6 / 1.8 |
| 6 | Kan Öfkesi | Hücum / 3 | self | — | 10 | 30 | — | attack_up 15 / 20 / 25 % for 12 s |
| 7 | Sarsıntı | Hücum / 3 | enemy | 2.5 | 14 | 15 | — | ×1.2 / 1.4 / 1.6, stun 1.5 / 2 / 2.5 s |
| 8 | Kalkan Duvarı | Siper / 1 | self | — | 10 | 25 | — | defense_up 40 / 60 / 80 % for 12 s |
| 9 | Meydan Okuma | Siper / 2 | self, enemies around self (6 / 6) | — | 8 | 15 | — | taunt 4 / 5 / 6 s |
| 10 | Dirençli Soluk | Siper / 3 | self | — | 12 | 30 | — | hot 2.5 / 3.5 / 4.5 % per s for 10 s |

### Rogue (Haydut) — A = attack (Çeviklik)

| id | name | branch / tier | target | range | mp | cd | cast | per rank |
|---|---|---|---|---|---|---|---|---|
| 2 | Çifte Kesik | Pusu / 1 (free) | enemy | 2.5 | 10 | 5 | — | 2 hits ×1.2 / 1.35 / 1.5 |
| 11 | Sırt Hançeri | Pusu / 2 | enemy | 2.5 | 14 | 9 | — | ×2.0 / 2.3 / 2.6, crit +25 / 30 / 35 % |
| 12 | Gölge Adımı | Pusu / 3 | enemy, leap | 10 / 12 / 14 | 12 | 14 | — | ×1.3 / 1.5 / 1.7 |
| 13 | Seri Eller | Pusu / 3 | self | — | 10 | 30 | — | haste 20 / 27 / 35 % for 10 s |
| 14 | Zehirli Kesik | Zehir / 1 | enemy | 2.5 | 10 | 8 | — | ×0.8 / 0.9 / 1.0, dot 25 / 32 / 40 % per s for 8 s |
| 15 | Sis Bombası | Zehir / 2 | enemy, around target (4 / 5) | 10 | 12 | 12 | — | ×0.6 / 0.7 / 0.8, slow 30 / 40 / 50 % for 6 s |
| 16 | Kaçamak | Zehir / 3 | self | — | 8 | 30 | — | dodge_up 10 / 15 / 20 % for 10 s |

### Mage (Büyücü) — A = magic power (Zekâ)

| id | name | branch / tier | target | range | mp | cd | cast | per rank |
|---|---|---|---|---|---|---|---|---|
| 3 | Köz Oku | Köz / 1 (free) | enemy | 16 | 14 | 3 | 0.6 | ×2.0 / 2.2 / 2.4 |
| 17 | Yakıcı İz | Köz / 2 | enemy | 16 | 12 | 10 | — | dot 35 / 45 / 55 % per s for 8 s |
| 18 | Alev Halkası | Köz / 3 | self, enemies around self (4 / 6) | — | 18 | 10 | — | ×1.4 / 1.6 / 1.8 |
| 19 | Kor Yağmuru | Köz / 3 | enemy, around target (4 / 6) | 16 | 24 | 14 | 1.2 | ×2.0 / 2.3 / 2.6 |
| 20 | Kırağı Oku | Kırağı / 1 | enemy | 16 | 12 | 5 | 0.6 | ×1.5 / 1.7 / 1.9, slow 30 / 40 / 50 % for 5 s |
| 21 | Buz Kabuğu | Kırağı / 2 | self | — | 20 | 25 | — | shield 150 / 200 / 250 % of A for 15 s |
| 22 | Donduran Nefes | Kırağı / 3 | enemy, around target (3 / 4) | 12 | 18 | 15 | — | ×0.8 / 0.9 / 1.0, stun 1 / 1.5 / 2 s |

### Priest (Şifacı) — A = attack (Güç), H = heal multiplier

| id | name | branch / tier | target | range | mp | cd | cast | per rank |
|---|---|---|---|---|---|---|---|---|
| 4 | Soluk Güneş | Şifa / 1 (free) | friendly | 14 | 16 | 4 | 0.8 | heal 19 / 22 / 25 % |
| 23 | Yenileyen Işık | Şifa / 2 | friendly | 14 | 14 | 12 | — | hot 4 / 5 / 6 % per s for 10 s |
| 24 | Kutlu Siper | Şifa / 3 | friendly | 14 | 18 | 30 | — | defense_up 20 / 30 / 40 % and max_hp_up 10 / 15 / 20 % for 20 s |
| 25 | Halka Şifası | Şifa / 3 | self, allies around self (10 / 6) | — | 22 | 12 | — | heal 15 / 18 / 21 % |
| 26 | Işık Çarpması | Yargı / 1 | enemy | 14 | 12 | 4 | 0.7 | ×1.8 / 2.1 / 2.4 |
| 27 | Bağlayan Söz | Yargı / 2 | enemy | 14 | 12 | 15 | — | weaken 20 / 25 / 30 % and slow 20 / 25 / 30 % for 8 s |
| 28 | Öfkeli Dua | Yargı / 3 | friendly | 14 | 16 | 30 | — | attack_up 10 / 15 / 20 % for 20 s |

(Rank-1 values of ids 1–4 are exactly the v5 skills: existing behaviour is unchanged at rank 1.)

## 3. Wire

### 3.1 New opcodes

| Opcode | Name | Dir |
|---|---|---|
| 0x0700 | SKILL_STATE | S→C |
| 0x0701 | SKILL_LEARN_REQ | C→S |
| 0x0702 | SKILL_LEARN_RES | S→C |
| 0x0703 | SKILL_BAR | S→C |
| 0x0704 | SKILL_BAR_SET_REQ | C→S |
| 0x0705 | SKILL_BAR_SET_RES | S→C |
| 0x0706 | SKILL_COOLDOWNS | S→C |
| 0x0707 | EFFECT_LIST | S→C |
| 0x0708 | EFFECT_APPLY | S→C |
| 0x0709 | EFFECT_REMOVE | S→C |
| 0x070A | SKILL_LEAP | S→C |
| 0x070B | SKILL_USED | S→C |

`SKILL_LEARN_REQ` and `SKILL_BAR_SET_REQ` are valid only in `InWorld` (otherwise
`ERROR(BadState)`; one still in flight while leaving the world is dropped silently, like
`MOVE_REQ`).

### 3.2 Layouts (little-endian, as everywhere)

```
SKILL_STATE        u16 points            // points(level), §1.1
                   u16 unspent
                   u8  count             // 0..32
                   SkillRank[count]      // strictly ascending skill_id; only rank >= 1 (the free skill always)
SkillRank:         u16 skill_id
                   u8  rank              // 1..3

SKILL_LEARN_REQ    u32 seq               // client-chosen, echoed
                   u16 skill_id          // raise this skill by one rank
SKILL_LEARN_RES    u32 seq
                   u16 skill_id
                   u8  result            // §3.4
                   u8  rank              // the skill's rank after the request (unchanged on failure), 0..3
                   u16 unspent           // after the request

SKILL_BAR          u16 slots[8]          // slot 0 = key 1 ... slot 7 = key 8; 0 = empty

SKILL_BAR_SET_REQ  u32 seq
                   u16 slots[8]          // the whole bar as it should be
SKILL_BAR_SET_RES  u32 seq
                   u8  result            // §3.5
                   u16 slots[8]          // the stored bar after the request

SKILL_COOLDOWNS    u8  count             // 1..32
                   Cooldown[count]
Cooldown:          u16 skill_id
                   u32 remaining_ms      // 1..total_ms
                   u32 total_ms          // the skill's cooldown at the rank used

EFFECT_LIST        u32 entity_id
                   u8  count             // 0..16
                   Effect[count]
EFFECT_APPLY       u32 entity_id
                   Effect                // new, or a refresh of the same (skill_id, kind)
EFFECT_REMOVE      u32 entity_id
                   u16 skill_id
                   u8  kind
                   u8  reason            // 1 Expired, 2 Depleted, 3 Cleared
Effect:            u16 skill_id
                   u8  kind              // 1..13, §1.5
                   u8  stacks            // 1 in v6
                   u32 source_id         // the caster's entity id (may no longer be spawned)
                   u32 duration_ms       // full duration
                   u32 remaining_ms      // 0..duration_ms, at the time of sending
                   u32 amount            // kinds 1-5, 8, 12: the value in %; 6/7: per tick; 11: absorb left; 9, 10, 13: 0

SKILL_LEAP         u32 entity_id         // the leaping player
                   f32 x, f32 y, f32 z   // where it landed (y on the terrain)
                   f32 yaw               // facing the target
                   u32 target_id

SKILL_USED         u32 caster_id
                   u16 skill_id
                   u32 target_id         // the primary target; the caster for self and around-self skills
                   u8  rank              // 1..3
```

Sizes: SKILL_STATE 5 + 3n, SKILL_LEARN_REQ 6, SKILL_LEARN_RES 10, SKILL_BAR 16,
SKILL_BAR_SET_REQ 20, SKILL_BAR_SET_RES 21, SKILL_COOLDOWNS 1 + 10n, EFFECT_LIST 5 + 20n,
EFFECT_APPLY 24, EFFECT_REMOVE 8, SKILL_LEAP 24, SKILL_USED 11.

Readers reject (protocol error): counts above the limits, SKILL_STATE ids not strictly ascending
or a rank outside 1..3, an unknown result / kind / reason, a learn rank above 3, stacks 0,
`remaining_ms > duration_ms`, a cooldown with `remaining_ms` 0 or above `total_ms`, non-finite
floats, SKILL_USED rank outside 1..3. `skill_id` values in requests and bar slots are not checked by
the reader (the server answers with a result).

### 3.3 Learning (`SKILL_LEARN_REQ` → `SKILL_LEARN_RES`)

Requests are decided one at a time, in order, against the **committed** ranks and the server's
live level (the next waits until the previous one's database write finished; learn and bar requests
share this queue). Checks, first failure wins:

| # | Result | Condition |
|---|---|---|
| 1 | `UnknownSkill` (1) | no such skill, or another class's |
| 2 | `MaxRank` (2) | already rank 3 |
| 3 | `NotEnoughPoints` (3) | unspent = 0 |
| 4 | `TierLocked` (4) | the tier gate of §1.2 is not met |
| 5 | `ServerError` (5) | 8 requests already waiting behind the one in flight, or the write failed (nothing changed) |
| — | `Ok` (0) | stored: the skill is one rank higher |

- The new rank is **written to the database before** `SKILL_LEARN_RES(Ok)`; the database job
  repeats the checks against the stored ranks under a row lock, so a `vo_admin reset-skills` done
  meanwhile is picked up (the answer then carries the stored truth, and `SKILL_STATE` follows).
- After every `Ok`: `SKILL_LEARN_RES`, then `SKILL_STATE` (the whole state). The world learns the
  new ranks in its next tick (a `SKILL_REQ` sent right after the RES may still use the old rank for
  up to one tick). Learning while dead, in combat or casting is allowed.
- `CHAR_STATS.heal_permille` follows the rank of Soluk Güneş (sent again when it changes).

### 3.4 `SKILL_LEARN_RES.result`

| Value | Name |
|---|---|
| 0 | Ok |
| 1 | UnknownSkill |
| 2 | MaxRank |
| 3 | NotEnoughPoints |
| 4 | TierLocked |
| 5 | ServerError |

### 3.5 Skill bar (`SKILL_BAR_SET_REQ` → `SKILL_BAR_SET_RES`)

- The request carries the whole bar (a drag that swaps two slots is one request). Every non-zero
  slot must be a skill of the character's class with rank ≥ 1; the same skill may sit on several
  slots. Result `NotLearned` (1) if any slot fails (nothing changed), `ServerError` (2) if the queue
  is full or the write failed, else `Ok` (0) after the bar was **stored**.
- The answer always carries the stored bar; the client shows that.
- Default bar (new characters, characters from before M12, after `reset-skills`): the free skill in
  slot 0 (key 1), everything else empty. On load, slots holding a skill that is unknown or not
  learned (data changed, ranks reset) are sent as 0.

### 3.6 Cooldowns (`SKILL_COOLDOWNS`, routing: the caster only)

- On world entry (after `CHAR_STATS`) if any skill is still cooling down (§1.7), one message with
  all of them.
- When a skill executes (instant: at once; cast: at cast end), one entry for that skill with
  `remaining_ms = total_ms`. The global cooldown is not reported (the client knows it, 500 ms).

### 3.7 Effects, leap, use (routing: the entity's observers and the entity itself)

- `EFFECT_APPLY` / `EFFECT_REMOVE` go to every player that has the entity spawned plus the entity
  itself if it is a player, in the tick they happen.
- **Entering the area of interest**: right after the `ENTITY_SPAWN` of an entity that has effects,
  the newcomer gets its `EFFECT_LIST`. (No `EFFECT_LIST` for entities without effects; nothing for
  the player itself on world entry, as effects do not survive a relog.)
- `SKILL_USED` is sent when a skill **executes** (after `CAST_END(completed)` for cast skills),
  before the `COMBAT_EVENT`s / `EFFECT_APPLY`s it causes, to the observers of the caster and of the
  primary target and to both if they are players. Clients use it for animations and effects
  (`ui/skills/<icon>.png`, effect art keyed by the skill's icon key).
- `SKILL_LEAP` is sent before the leap's `SKILL_USED`; the caster snaps to the position (the
  `POSITION_CORRECT` that follows confirms it).

### 3.8 Changes to existing packets

- `COMBAT_EVENT.flags` gains **bit2 `periodic`** (a DoT / HoT tick; `skill_id` = the effect's
  skill) and **bit3 `absorbed`** (a shield took part or all of the damage; `amount` = hp actually
  lost, may be 0). Kinds are unchanged.
- `ACTION_FAIL.reason` gains **13 `NotLearned`** and **14 `NoPath`**.
- `CHAR_STATS`, `SELF_STATS`, `ENTITY_STATS`: unchanged layouts; values are the effective ones
  (with active effects: attack, defense, max hp, attack interval, dodge).
- `ENTER_WORLD_RES`, `ENTITY_SPAWN`, `SKILL_REQ`, `CAST_*`: unchanged.

## 4. Sequencing on world entry

`ENTER_WORLD_RES`, `INVENTORY`, `COINS`, **`SKILL_STATE`**, **`SKILL_BAR`** (from the session),
then the world's first batch: `CHAR_STATS`, **`SKILL_COOLDOWNS`** (only if any), the
`ENTITY_SPAWN`s (each followed by its `EFFECT_LIST` if it has effects), then the stream.

After a level-up the session sends a new `SKILL_STATE` (more unspent points) after the batch that
carried `LEVEL_UP`.

## 5. Limits

| Limit | Value |
|---|---|
| `SKILL_LEARN_REQ` + `SKILL_BAR_SET_REQ` per second per client | 10 combined → `ERROR(Flood)` and close |
| Requests waiting behind the one in flight | 8 (more → `ServerError` answer) |
| Skill ids | 1..63 (`kMaxSkillId`); shipped 1..28 |
| Ranks | 0..3 |
| Bar slots | 8 |
| Effects per entity | 16 |
| Area targets | 1..16 per skill (data) |
| Cooldown | ≤ 60 s (data) |
| `SKILL_REQ` | unchanged: part of the 10/s combat cap |

## 6. Persistence

Migration `db/migrations/0006_skills.sql`:

```sql
CREATE TABLE character_skills (
    char_id  bigint   NOT NULL REFERENCES characters (id) ON DELETE CASCADE,
    skill_id smallint NOT NULL CHECK (skill_id BETWEEN 1 AND 63),
    rank     smallint NOT NULL CHECK (rank BETWEEN 1 AND 3),
    PRIMARY KEY (char_id, skill_id));
ALTER TABLE characters ADD COLUMN skill_bar smallint[];  -- NULL = default bar (§3.5)
    CHECK (skill_bar IS NULL OR (cardinality(skill_bar) = 8 AND 0 <= ALL (skill_bar) AND 63 >= ALL (skill_bar)))
```

- A row exists only for a rank the player paid for (a free skill at rank 1 has no row; at rank 2 its
  row says 2). Class membership, tiers and the point budget are checked by the server's code (the
  database cannot know the data file); the learn job locks the character row, reads the rows,
  checks with the shared rules and upserts one row in one transaction.
- The bar is one `UPDATE characters SET skill_bar = $slots` after checking the slots against the
  stored ranks in the same transaction.
- Existing characters: no rows, bar NULL → every point unspent, default bar.

### 6.1 Account service

`IAccountService::learnSkill(account, character, SkillLearn{skill, level}, done)` and
`setSkillBar(account, character, bar, done)` (both backends; the in-memory one runs the same shared
check). `CharacterInfo` carries the stored ranks (`skillRanks`) and bar (`skillBar`, nullopt =
default). Requests of one account run in submission order, so a level-up save of the world commits
before a learn the session sends after learning that level.

### 6.2 vo_admin

- `vo_admin reset-skills <character>`: deletes the character's skill rows and sets the bar to
  NULL. A character in the world keeps its live ranks until its next entry or its next learn (which
  reads the stored ranks back).
- `vo_admin show-character <character>`: also lists the skill points (total / unspent), every rank
  ≥ 1 (name, branch, rank) and the bar.
- `vo_admin set-level <character> <level>` to a lower level resets the skills (as `reset-skills`)
  if the spent points no longer fit the new level, and says so.

## 7. Client spec (presentation contract, not wire format)

All player-facing texts are Turkish and listed here; the skill descriptions come from the
`description` of each skill in `gamedata/skills.toml` (the same text as §7.6), rendered by the
shared `game::describeSkill(skill, rank)`.

### 7.1 Skill bar

- 8 slots in one row at the bottom centre of the HUD, slot labels "1" … "8" (keys 1–8 use the slot
  of that number; the v5 "skill on key 1" becomes slot 1).
- A slot shows the skill's icon `ui/skills/<icon>.png`, a cooldown sweep (from `SKILL_COOLDOWNS`,
  counting down locally; the 500 ms global cooldown sweeps every filled slot), a red tint while the
  mana is below the cost, a darker tint while an `enemy` skill's selected target is out of range
  or none is selected.
- Using a slot: `enemy` skills → the selected monster (walk into range as in v5; "Önce bir hedef
  seç" if none); `friendly` skills → the selected player if one is selected, else the player
  itself; `self` skills → no target (target_id 0).
- Rearranging: drag a skill from the skill window onto a slot, drag slots onto each other (swap),
  drag a slot off the bar or right-click it to clear it. Each change sends one `SKILL_BAR_SET_REQ`
  with the whole bar; while one is in flight further drags wait; the bar always shows the last
  `SKILL_BAR` / `SKILL_BAR_SET_RES` slots.
- Tooltip on hover: as in §7.3.

### 7.2 Skill window ("Yetenekler")

- Toggled with **K** (and a HUD button with the tooltip "Yetenekler (K)"); Esc closes it like the
  other windows (loot window → Yetenekler → Künye → inventory → target → menu).
- Title "Yetenekler"; header "Yetenek puanı: N" (highlighted while N > 0).
- Two columns, one per branch of the character's class, headed by the branch name (§7.5); three
  rows (tier 1 at the top). Each skill cell: icon, name, "k/3", a "+" button while the skill can be
  raised (enough points, rank < 3, tier gate met). A skill whose tier gate is not met is drawn
  greyed with the note "Bu yolda N puan gerekli" (N = 2 or 4); rank 0 skills are drawn dimmed
  ("Öğrenilmedi").
- Under each column: "Bu yolda harcanan: N".
- "+" sends one `SKILL_LEARN_REQ`; buttons are disabled while one is in flight. The first "+" of a
  session asks once: "Harcanan yetenek puanları geri alınamaz. Onaylıyor musun?" (Evet / Hayır).
- Dragging a learned skill's icon onto the bar places it there (§7.1).

### 7.3 Tooltip

```
<name>                              (bold)
<branch name> · Kademe k/3          ("Öğrenilmedi" for rank 0)
Mana: <cost>   Bekleme: <cd> sn     ("Bekleme yok" for 0)
Menzil: <range> m | "Kendine" (self) | "Çevrene" (around self)
Hazırlık: <cast> sn | "Anında"
<description at the current rank, or rank 1 if not learned>
Sonraki kademe: <description at rank k+1>    (if k is 1 or 2; in a lighter colour)
```

Numbers use a decimal comma and drop ",0" (2,5 sn; 12 sn).

### 7.4 Result and failure texts

| Event | Text |
|---|---|
| SKILL_LEARN_RES Ok | "<name> kademe <k> oldu." |
| UnknownSkill | "Bu yeteneği öğrenemezsin." |
| MaxRank | "Bu yetenek en yüksek kademede." |
| NotEnoughPoints | "Yeterli yetenek puanın yok." |
| TierLocked | "Bu yetenek için bu yolda önce N puan harcamalısın." (N from the tier) |
| ServerError (learn) | "Yetenek kaydedilemedi, tekrar dene." |
| SKILL_BAR_SET_RES NotLearned | "Bu yeteneği henüz öğrenmedin." |
| SKILL_BAR_SET_RES ServerError | "Yetenek çubuğu kaydedilemedi, tekrar dene." |
| ACTION_FAIL NotLearned | "Bu yeteneği henüz öğrenmedin." |
| ACTION_FAIL NoPath | "Hedefe atlanacak yol yok." |
| level-up (besides v5) | "+1 yetenek puanı" and a marker on the Yetenekler HUD button while unspent > 0 |
| enemy slot, no target | "Önce bir hedef seç." |

### 7.5 Branch names

| Class | Branch 0 | Branch 1 |
|---|---|---|
| Savaşçı | Hücum | Siper |
| Haydut | Pusu | Zehir |
| Büyücü | Köz | Kırağı |
| Şifacı | Şifa | Yargı |

### 7.6 Skill descriptions (skills.toml `description`; placeholders filled per rank)

Placeholders: `{damage}` damage multiplier in % (220), `{hits}`, `{crit}` critical bonus in %,
`{heal}` heal in %, `{radius}` m, `{targets}`, `{range}` m, `{value}` / `{duration}` (s) of the
first effect, `{value2}` / `{duration2}` of the second. Values are before the attribute multiplier
(the formulas of §1.3 apply on top).

| id | Description |
|---|---|
| 1 | Silahını tüm gücüyle indirir: saldırı gücünün %{damage} oranında hasar. |
| 5 | Geniş bir savuruş: {radius} m içindeki en çok {targets} düşmana saldırı gücünün %{damage} oranında hasar. |
| 6 | Öfkesini bileyler: {duration} sn boyunca saldırı gücü %{value} artar. |
| 7 | Hedefi sarsan bir vuruş: saldırı gücünün %{damage} oranında hasar verir ve hedefi {duration} sn sersemletir. |
| 8 | Kalkanının ardına siper alır: {duration} sn boyunca savunması %{value} artar. |
| 9 | Narasıyla {radius} m içindeki en çok {targets} canavarın öfkesini üstüne çeker; {duration} sn boyunca yalnız ona saldırırlar. |
| 10 | Derin bir soluk alır: {duration} sn boyunca her saniye en yüksek canının %{value} kadarını yeniler. |
| 2 | Peş peşe iki kesik: her biri saldırı gücünün %{damage} oranında hasar verir. |
| 11 | Hançeri zayıf noktaya saplar: saldırı gücünün %{damage} oranında hasar; kritik vuruş şansı %{crit} daha yüksek. |
| 12 | {range} m içindeki hedefin yanına gölge gibi süzülür ve saldırı gücünün %{damage} oranında hasar verir. |
| 13 | Elleri hızlanır: {duration} sn boyunca saldırı hızı %{value} artar. |
| 14 | Zehirli bir kesik: saldırı gücünün %{damage} oranında hasar, ardından {duration} sn boyunca her saniye %{value} oranında zehir hasarı. |
| 15 | Hedefin çevresine boğucu bir sis atar: {radius} m içindeki en çok {targets} düşmana %{damage} oranında hasar verir ve onları {duration} sn boyunca %{value} yavaşlatır. |
| 16 | Gölgelere karışır: {duration} sn boyunca kaçınma şansı %{value} artar. |
| 3 | Bir köz oku fırlatır: büyü gücünün %{damage} oranında hasar. |
| 17 | Hedefi için için yanan bir izle damgalar: {duration} sn boyunca her saniye büyü gücünün %{value} oranında yanma hasarı. |
| 18 | Çevresinde bir alev halkası patlatır: {radius} m içindeki en çok {targets} düşmana büyü gücünün %{damage} oranında hasar. |
| 19 | Hedefin üstüne kor yağdırır: {radius} m içindeki en çok {targets} düşmana büyü gücünün %{damage} oranında hasar. |
| 20 | Dondurucu bir ok: büyü gücünün %{damage} oranında hasar verir ve hedefi {duration} sn boyunca %{value} yavaşlatır. |
| 21 | Bedenini buz kabuğuyla sarar: {duration} sn boyunca büyü gücünün %{value} kadar hasarı emer. |
| 22 | Hedefin çevresine dondurucu bir nefes üfler: {radius} m içindeki en çok {targets} düşmana %{damage} oranında hasar verir ve onları {duration} sn dondurur. |
| 4 | Solgun güneşin ışığıyla bir dostu ya da kendini iyileştirir: en yüksek canın %{heal} kadarı. |
| 23 | Bir dostu ya da kendini yenileyen bir ışıkla sarar: {duration} sn boyunca her saniye en yüksek canın %{value} kadarı. |
| 24 | Bir dostu ya da kendini kutsar: {duration} sn boyunca savunma %{value}, en yüksek can %{value2} artar. |
| 25 | Bir ışık halkası yayar: {radius} m içindeki en çok {targets} dostu en yüksek canlarının %{heal} kadarı iyileştirir. |
| 26 | Hedefe bir ışık hüzmesi çarptırır: saldırı gücünün %{damage} oranında hasar. |
| 27 | Bağlayıcı bir söz söyler: hedef {duration} sn boyunca %{value} daha az hasar verir ve %{value2} daha yavaş hareket eder. |
| 28 | Öfkeli bir duayla bir dostu ya da kendini güçlendirir: {duration} sn boyunca saldırı gücü %{value} artar. |

### 7.7 Effects on screen

- The target frame and the player's own HUD show the entity's effects as small icons (the skill's
  icon) with the remaining seconds; helpful effects with a green frame, harmful ones with a red
  frame; a shield also shows its remaining absorb. Tooltip: skill name and, by kind: "Saldırı +%v",
  "Savunma +%v", "Saldırı hızı +%v", "Kaçınma +%v", "En yüksek can +%v", "Zehir/Yanma: saniyede v",
  "Yenilenme: saniyede v", "Yavaşlatıldı %v", "Sersemledi", "Kök saldı", "Kalkan: v", "Zayıfladı
  %v", "Kışkırtıldı".
- `COMBAT_EVENT` with the periodic flag shows a smaller floating number; with the absorbed flag a
  grey "Emildi" when the amount is 0.
- `SKILL_USED` starts the cast-release / swing animation and the skill's effect art; `SKILL_LEAP`
  moves the leaping entity quickly (not a teleport snap) to the landing point.
