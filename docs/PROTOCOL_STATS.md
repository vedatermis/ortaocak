# VedatOnline Network Protocol — Stats extension (v5)

Status: **contract for milestone M11 "Künye"** (primary attributes, stat points, derived combat
values). Extends docs/PROTOCOL.md, docs/PROTOCOL_ITEMS.md (v2), docs/PROTOCOL_COMBAT.md (v3) and
docs/PROTOCOL_LOOT.md (v4); everything there stays valid unless this document says otherwise.
`PROTOCOL_VERSION` becomes **5** (client and server are released together; an older client is
refused with `VersionMismatch`).

Goal: every character has five primary attributes. Its class gives it a fixed starting spread;
every level-up grants free points that the player spends on the attributes, permanently. The
attributes, the level and the equipment together give every combat value (hit points, mana,
attack, defense, critical and dodge chance, regeneration, heal strength). One pure function in
shared code (`game::deriveStats`, `shared/game/Combat.h`) computes them; the server uses it for the
rules and the client for its character window and previews. Names, numbers and texts are our own.

## 1. Attributes

| Index | Key | Display name | Drives |
|---|---|---|---|
| 0 | `strength` | **Güç** | attack of Warriors and Priests |
| 1 | `dexterity` | **Çeviklik** | attack of Rogues; critical chance; dodge chance |
| 2 | `vitality` | **Dayanıklılık** | max hp; defense; hp regeneration |
| 3 | `intelligence` | **Zekâ** | attack (magic power) of Mages; max mp; heal strength |
| 4 | `spirit` | **İrade** | max mp; mp regeneration; heal strength |

The index is the wire value and the array order everywhere (CHAR_STATS, STAT_ALLOCATE_REQ, the
database columns, `game::Attributes`).

Three values per attribute:

- **base** = class starting value (§1.1) + **allocated** points (§1.2). Capped at **255**
  (`kAttributeCap`).
- **bonus** = sum of that attribute over the equipped items (§5). Not capped.
- **effective** = base + bonus. Every formula of §2 uses the effective value.

### 1.1 Class starting spread (sum 35 for every class)

| Class | Güç | Çeviklik | Dayanıklılık | Zekâ | İrade | Attack attribute |
|---|---|---|---|---|---|---|
| 1 Warrior (Savaşçı) | 12 | 6 | 11 | 3 | 3 | Güç |
| 2 Rogue (Haydut) | 6 | 13 | 8 | 4 | 4 | Çeviklik |
| 3 Mage (Büyücü) | 3 | 5 | 6 | 13 | 8 | Zekâ |
| 4 Priest (Rahip) | 5 | 3 | 9 | 8 | 10 | Güç |

The *attack attribute* scales the class's attack (§2): Warriors swing swords and axes with Güç,
Rogues their daggers with Çeviklik, Mages cast through the staff with Zekâ (their basic attack and
Köz Oku are magic), Priests swing the mace with Güç (their heal scales with Zekâ and İrade).

### 1.2 Points

- `points(level) = 3 × (level − 1)` (`kPointsPerLevel` = 3): level 1 has 0, level 10 has 27.
- `allocated[i]` is what the player spent on attribute `i` (stored per character, §6).
- `unspent = max(0, points(level) − Σ allocated)`.
- Spending is permanent. The only way back is `vo_admin reset-stats <character>` (§6.2), which sets
  every `allocated[i]` to 0 (all points become unspent again).
- Characters from before M11 have `allocated = 0`, so they get all points of their level at once
  (a level-7 character logs in with 18 unspent points).
- Robustness: if a stored allocation is larger than `points(level)` (the level was lowered outside
  the server, or a level-up save was lost), `unspent` is 0 and the allocation is kept as it is.

## 2. Derived values (`game::deriveStats`)

Inputs: class, level `L` (clamped to 1..10), the effective attributes `STR DEX VIT INT SPI`, and
the equipment sums `W` (Σ `attack` of the equipped items) and `E` (Σ `defense`). `P` is the
effective value of the class's attack attribute. All arithmetic is in double precision;
`round` = round half away from zero (`std::lround`).

| Value | Formula | Range |
|---|---|---|
| max_hp | `base_hp + hp_per_level × L + 5 × VIT` | ≥ 1 |
| max_mp | `base_mp + mp_per_level × L + 3 × SPI + INT` | ≥ 0 |
| attack | `round(base_attack + 2 × L + W × (1 + P / 20) + P / 3)` | 0 .. 65535 |
| defense | `round(base_defense + L + E + VIT / 5)` | 0 .. 65535 |
| critical chance | `base_crit + 0.5 % × DEX` | ≤ 50 % |
| dodge chance | `0.4 % × DEX` | ≤ 25 % |
| hp regeneration | `max_hp × 2 % × (1 + VIT / 100)` per second | out of combat |
| mp regeneration | `max_mp × 3 % × (1 + SPI / 100)` per second | out of combat |
| heal strength | `19 % × (1 + (INT + SPI) / 60)` of the **target's** max_hp | Soluk Güneş |

Regeneration still only runs out of combat (6 s without damage dealt or taken) and doubles while
sitting (PROTOCOL_COMBAT.md §1.1); fractions carry over between seconds as before. The heal amount
of Soluk Güneş is `max(1, round(target_max_hp × heal_strength))`, with the **caster's** INT and SPI.

Class constants (`shared/game/Combat.cpp`, `ClassTable`):

| Class | base_hp | hp_per_level | base_mp | mp_per_level | base_attack | base_defense | base_crit | attack interval | range |
|---|---|---|---|---|---|---|---|---|---|
| Warrior | 15 | 20 | 18 | 4 | 2 | 0 | 2.0 % | 1600 ms | 2.5 m |
| Rogue | 40 | 18 | 14 | 5 | −2 | −1 | 8.5 % | 1200 ms | 2.5 m |
| Mage | 46 | 14 | 23 | 10 | 0 | 0 | 2.5 % | 1800 ms | 14 m |
| Priest | 33 | 17 | 17 | 9 | 6 | 0 | 3.5 % | 1700 ms | 2.5 m |

Unknown classes use the Warrior row and spread.

Damage, critical hits, skills and monsters are unchanged (PROTOCOL_COMBAT.md §1.1–§1.3):
`damage = max(1, round(attack × multiplier × uniform(0.9, 1.1) − defense × 0.5))`, ×1.5 on a
critical hit; `attack` of a player is the value above (a Mage's "attack" is its magic power).

### 2.1 Dodge

A monster's basic attack on a player first rolls the player's dodge chance. A dodged attack deals
**0**: the player's hp is unchanged and the attack is reported as `COMBAT_EVENT` with
`kind = 3` (**Miss**, defined since v3 and unused until now), `flags = 0`, `amount = 0`,
`target_hp` = the unchanged hp. Everything else about the attack is as if it hit: the monster's
attack interval restarts, the player counts as in combat, a sitting player stands up. Monsters
cannot dodge; players' attacks always roll damage. (Kind 3 is only ever produced by a dodge in v5.)

### 2.2 Tuning (checked by tests/game/CombatTests.cpp against the shipped gamedata)

At level 1 with the starting spread every class has exactly its v4 numbers:

| Class | max_hp | max_mp | attack (kit) | defense (kit) | crit | dodge (new) |
|---|---|---|---|---|---|---|
| Warrior | 90 | 34 | 18 | 7 | 5 % | 2.4 % |
| Rogue | 98 | 35 | 18 | 2 | 15 % | 5.2 % |
| Mage | 90 | 70 | 18 | 2 | 5 % | 2.0 % |
| Priest | 95 | 64 | 16 | 3 | 5 % | 1.2 % |

A level-10 character with nothing spent also matches its v4 numbers (±1). Spending the 27 points
sensibly (e.g. two thirds on the attack attribute, the rest on Dayanıklılık) gives roughly +30–50 %
damage or survivability; such a level-10 Warrior, Rogue or Mage beats the Kor Bekçisi (level-6
elite, 900 hp, attack 38) in a straight fight with average rolls, while level-1..5 characters lose.

## 3. Wire

### 3.1 New opcodes

| Opcode | Name | Dir |
|---|---|---|
| 0x0600 | CHAR_STATS | S→C |
| 0x0601 | STAT_ALLOCATE_REQ | C→S |
| 0x0602 | STAT_ALLOCATE_RES | S→C |

`STAT_ALLOCATE_REQ` is valid only in `InWorld` (any other state: `ERROR(BadState)`; a request
still in flight while leaving the world is dropped silently, like `MOVE_REQ`).

```
CHAR_STATS        u16 level
                  StatAttr[5]           // index order of §1
                  u16 unspent           // §1.2
                  u32 max_hp
                  u32 max_mp
                  u16 attack            // §2 (a Mage's magic power)
                  u16 defense
                  u16 crit_permille     // critical chance in 1/1000 (150 = 15.0 %), 0..1000
                  u16 dodge_permille    // dodge chance in 1/1000, 0..1000
                  u16 hp_regen_tenths   // out-of-combat hp regeneration per second (standing) × 10, rounded
                  u16 mp_regen_tenths   // the same for mp
                  u16 attack_interval_ms
                  u16 heal_permille     // heal strength (§2) in 1/1000 of the target's max_hp
StatAttr:
    u16 base                           // class start + allocated, 0..255
    u16 bonus                          // from equipped items

STAT_ALLOCATE_REQ u32 seq               // client-chosen, echoed back
                  u16 points[5]         // points to add per attribute (index order of §1)
STAT_ALLOCATE_RES u32 seq
                  u8  result            // §3.3
                  u16 unspent           // after the request (unchanged on failure)
```

Sizes: CHAR_STATS 48 bytes, STAT_ALLOCATE_REQ 14, STAT_ALLOCATE_RES 7.
Readers reject (protocol error): a `base` above 255, a permille above 1000, an unknown result.
`points` values are not checked by the reader (the server answers with a result).

### 3.2 When CHAR_STATS is sent (routing: the owner only)

- On world entry, as the first world packet: `ENTER_WORLD_RES`, `INVENTORY`, `COINS`,
  **`CHAR_STATS`**, then the `ENTITY_SPAWN`s.
- After every change of any of its values, at the end of the world tick in which it happened (at
  most one per tick): a level-up (after `LEVEL_UP` / `XP_GAIN`), an equipment change that changes
  item bonuses (after the session's `ITEM_UPDATE` / `ITEM_MOVE_RES`, in the next world tick), an
  accepted `STAT_ALLOCATE_REQ` (after its `STAT_ALLOCATE_RES`, in the next world tick, i.e. within
  about 50 ms).

`SELF_STATS` (PROTOCOL_COMBAT.md §3.3) keeps its v3 layout and meaning: the fast hp/mp channel (≤ 4
Hz) whose `max_hp`, `max_mp`, `attack`, `defense` are always the same derived values as in the
latest `CHAR_STATS`. It is still sent after equipment changes and level-ups, and now also after an
allocation that changes max_hp/max_mp/attack/defense. Raising max_hp or max_mp does not fill hp or
mp (only a level-up refills both, as before).

### 3.3 Allocation (`STAT_ALLOCATE_REQ` → `STAT_ALLOCATE_RES`)

Requests of one player are decided one at a time, in order, against the committed allocation
(the next waits until the previous one's database write finished). Checks, first failure wins:

| # | Result | Condition |
|---|---|---|
| 1 | `InvalidAmount` (1) | all five `points` are 0 |
| 2 | `NotEnoughPoints` (2) | Σ points > unspent (with the server's current level) |
| 3 | `AttributeCap` (3) | for some attribute base + points > 255 |
| 4 | `ServerError` (4) | 4 requests are already waiting behind the one in flight, or the database write failed (nothing changed) |
| — | `Ok` (0) | stored: the allocation grew by `points` |

- Being dead, in combat, casting or sitting does not matter.
- The new allocation is **written to the database before** `STAT_ALLOCATE_RES(Ok)` is sent
  (`characters.stat_*`, §6). The answer's `unspent` and the following `CHAR_STATS` are computed
  from the stored values.
- `STAT_ALLOCATE_RES` always comes from the session in request order; the `CHAR_STATS` with the new
  values follows from the world (next tick). A client must not assume CHAR_STATS arrives before the
  RES.
- Hit points and mana stay as they are (clamped if a maximum shrank; allocation never lowers one).

### 3.4 Result values (`STAT_ALLOCATE_RES.result`)

| Value | Name |
|---|---|
| 0 | Ok |
| 1 | InvalidAmount |
| 2 | NotEnoughPoints |
| 3 | AttributeCap |
| 4 | ServerError |

### 3.5 Other packets

- `COMBAT_EVENT.kind = 3` (Miss) now occurs: a dodged monster attack (§2.1). No new flag bits.
- `ITEM_MOVE_RES.result` gains **9 `AttributeTooLow`** (§5): the moved item's attribute
  requirement is not met at the target equipment slot.
- `ENTER_WORLD_RES`, `ENTITY_SPAWN`, `SELF_STATS`, `LEVEL_UP`, `XP_GAIN` are unchanged on the wire.

## 4. Limits

| Limit | Value |
|---|---|
| `STAT_ALLOCATE_REQ` per second per client | 5 → `ERROR(Flood)` and close |
| Requests waiting behind the one in flight | 4 (more → `STAT_ALLOCATE_RES(ServerError)`) |
| Attribute base (class start + allocated) | 0 .. 255 |
| Points per level | 3 (level 10: 27) |
| Critical chance / dodge chance caps | 50 % / 25 % |

## 5. Item data (`gamedata/items.toml`, additions)

New optional integer keys of a weapon or shield `[[item]]` (materials may not have them), default 0:

| Key | Meaning | Range |
|---|---|---|
| `strength`, `dexterity`, `vitality`, `intelligence`, `spirit` | bonus to that attribute while equipped | 0 .. 50 |
| `req_strength`, `req_dexterity`, `req_vitality`, `req_intelligence`, `req_spirit` | needed **base** value (class start + allocated, without item bonuses) to equip it | 0 .. 255 |

- A requirement is checked when the item is put into an equipment slot (`ITEM_MOVE_REQ`, after the
  class and level checks: WrongClass, LevelTooLow, then **AttributeTooLow**), never afterwards: an
  equipped item stays equipped after `reset-stats`. Starter kit items may not have requirements.
- Item bonuses count in `CHAR_STATS.bonus` and in every formula of §2 (effective values).
- M11 data: Cüruf Topuzu (14) gets İrade +2; Bekçi Kalkanı (15) gets Dayanıklılık +3. No shipped
  item has a requirement yet.

Client prediction (`game::planItemMove`) gets the base attributes in `game::ItemUser::attributes`.

## 6. Persistence

Migration `db/migrations/0005_stats.sql`: five columns on `characters`, the **allocated** points
(not the totals; the class start comes from code):

```sql
stat_strength, stat_dexterity, stat_vitality, stat_intelligence, stat_spirit
    smallint NOT NULL DEFAULT 0, CHECK (each BETWEEN 0 AND 255)
CHECK (stat_strength + stat_dexterity + stat_vitality + stat_intelligence + stat_spirit
       <= 3 * (level - 1))
```

Existing rows get 0 everywhere, i.e. all points of their level unspent (§1.2). An allocation is
one statement: `UPDATE characters SET stat_x = stat_x + n, ... WHERE id = .. AND account_id = ..
AND deleted_at IS NULL AND <sum after> <= <points of the server's level> AND <each base after> <=
255 RETURNING stat_*` — the returned values are what the session and the world adopt (so a
`reset-stats` done meanwhile is picked up by the next allocation). A refused or failed statement
changes nothing (`ServerError`).

### 6.1 Account service

`IAccountService::allocateStats(account, character, points[5], budget, classStart[5], done)`
(both backends): adds `points` if the sum after ≤ `budget` and every class start + allocated ≤ 255;
`done({ok, allocated[5]})` after commit. `CharacterInfo` carries the stored allocation
(`statPoints`).

### 6.2 vo_admin

- `vo_admin reset-stats <character>`: all five allocations to 0 (every point unspent again). A
  character in the world keeps its live allocation until its next world entry or its next
  allocation (which reads the stored values back).
- `vo_admin show-character <character>`: id, account, class, race, level, xp, attributes (start +
  allocated = base), unspent points, coins, hp/mp.
- `vo_admin set-level` (M9) to a lower level resets the allocation to 0 if it no longer fits the
  new level (the CHECK above), and says so.

## 7. Client presentation contract (not wire format, but agreed behaviour)

- **Künye window** (character sheet), toggled with **C** (and a HUD button); Esc closes it like
  the inventory (loot window → Künye → inventory → target → menu). Title "Künye"; header line with
  the character's name, class and "Seviye N".
- One row per attribute in index order: display name, `base` and, if `bonus > 0`, "(+bonus)" in
  the bonus colour; a "+" button per row while `unspent > 0`. "Dağıtılmamış puan: N" above or
  below the rows (highlighted while N > 0).
- Spending: "+" adds a *pending* point to that row (shown as "+k" next to the value; right click or
  a "−" button removes one), never more than `unspent` in total and never past 255; while points
  are pending, every derived row shows its preview computed with `game::deriveStats` (shared code)
  with an arrow, e.g. "Saldırı 36 → 41". "Onayla" sends one `STAT_ALLOCATE_REQ` with all pending
  points; "Vazgeç" drops them. Buttons are disabled while a request is in flight.
- Spending is permanent: the first "Onayla" of a session asks once "Dağıtılan puanlar geri
  alınamaz. Onaylıyor musun?" (Evet / Hayır).
- Derived rows (Turkish labels): "Can" (max_hp), "Mana" (max_mp), "Saldırı Gücü" (attack; for a
  Mage "Büyü Gücü"), "Savunma", "Kritik Vuruş %x,y", "Kaçınma %x,y", "Can Yenilenmesi x,y/sn",
  "Mana Yenilenmesi x,y/sn", "Saldırı Hızı x,y sn" (attack interval), for a Priest additionally
  "İyileştirme %x,y" (heal_permille). Percentages with one decimal and a decimal comma.
- Tooltip per attribute: Güç "Savaşçı ve Rahip saldırı gücünü artırır.", Çeviklik "Haydut
  saldırı gücünü, kritik vuruş ve kaçınma şansını artırır.", Dayanıklılık "Canı, savunmayı ve can
  yenilenmesini artırır.", Zekâ "Büyücü büyü gücünü, manayı ve iyileştirmeyi artırır.", İrade
  "Manayı, mana yenilenmesini ve iyileştirmeyi artırır."
- Results: Ok → pending cleared, values from the next `CHAR_STATS`; InvalidAmount "Dağıtılacak
  puan seçilmedi"; NotEnoughPoints "Yeterli puanın yok"; AttributeCap "Bu nitelik en yüksek
  değerde (255)"; ServerError "Puanlar kaydedilemedi, tekrar dene" (pending points stay).
- Level-up: besides the v3 effects, the message "+3 nitelik puanı" and a small marker on the Künye
  HUD button while `unspent > 0`.
- Dodge: a `COMBAT_EVENT` with kind Miss shows a floating "Iska" (grey) over the target instead of
  a number; on the local player "Kaçındın!" briefly. No hit reaction.
- Item tooltips list attribute bonuses ("Güç +2", in the bonus colour) and requirements
  ("Gerekli Güç: 20", red if the base value is lower); `ITEM_MOVE_RES(AttributeTooLow)`: "Bu eşya
  için niteliklerin yetersiz."
