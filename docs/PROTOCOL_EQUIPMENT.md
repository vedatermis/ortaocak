# VedatOnline Network Protocol — Equipment extension (v12)

Status: **contract for milestone M18 "Kuşanma"** (weapon properties, bonus lines, accessories). Extends
docs/PROTOCOL_ITEMS.md (v2), docs/PROTOCOL_STATS.md (v5) and docs/PROTOCOL_UPGRADE.md (v10); everything there
stays valid unless this document says otherwise. `PROTOCOL_VERSION` becomes **12** (a v11 client is refused
with VersionMismatch; client and server are released together). Design, numbers and balance:
docs/DEVELOPMENT_PLAN.md §39 (Turkish). The mechanics follow classic MMORPG equipment (weapon delay, reach,
damage range, stat lines, rings / earrings / necklace / belt); every name, text, icon and number is our own.

## 1. Model

- **Weapons differ in more than one number.** A weapon template may carry its own basic attack interval
  (`speed_ms`), melee reach (`reach`) and a damage range around its `attack` (`damage = [low, high]`). The
  **main-hand** weapon decides the character's basic attack interval and reach; every equipped weapon adds its
  attack and its damage range (a rogue's two daggers both count, as before).
- **Bonus lines.** Every equippable item (weapon, shield, armour, accessory) may add maximum hit points, maximum
  mana, critical chance and dodge chance, next to the attribute bonuses of M11.
- **Requirements** are the M11 attribute requirements (`req_<attribute>`, base values: class start + spent
  points), next to the level requirement; since M18 the shipped data uses them on the top tiers.
- **Accessories** (`kind = "accessory"`): rings, earrings, a necklace and a belt. They have bonus lines and a
  small defence, never an attack, are never drawn on the character and are not upgradable (§5).
- Everything stays **server-authoritative**: the server derives the totals (`game::deriveStats`, shared code)
  and validates interval, reach and requirements; the client shows the server's CHAR_STATS and predicts moves
  with the same shared rules.

## 2. Item data (`gamedata/items.toml`, additions)

| Key | Kinds | Meaning | Range |
|---|---|---|---|
| `damage = [low, high]` | weapon | the damage range; `low + high` must equal `2 x attack` (the mean stays `attack`) | 0 ≤ low ≤ high |
| `speed_ms` | weapon | basic attack interval while in the main hand; absent = the class's interval | 800 .. 3000 |
| `reach` | weapon | melee reach in metres while in the main hand; absent = the class's range | 1.5 .. 4.0 |
| `max_hp`, `max_mp` | weapon, shield, armour, accessory | added to the maximum | 0 .. 1000 |
| `crit`, `dodge` | weapon, shield, armour, accessory | added chance in permille (10 = 1 %) | 0 .. 100 |
| `kind = "accessory"` | — | a ring, earring, necklace or belt | — |
| `slot = "ring" \| "earring" \| "necklace" \| "belt"` | accessory | which accessory slots it takes | — |

Validation (strict, the file is refused with file and line otherwise): `damage`, `speed_ms` and `reach` only
on weapons; the bonus lines never on materials or consumables; an accessory has an accessory slot (and an
accessory slot only an accessory), `attack = 0`, an `icon`, and no `model` or `grip`; armour and weapons keep
their M15 / M17 rules. A starter kit may name the new equipment slots (`necklace`, `belt`, `ring1`, `ring2`,
`earring1`, `earring2`); the old name `reserved` is gone.

Shipped data (§39 of the plan): every weapon has `speed_ms`, every melee-capable weapon `reach` (a Mage's staff
casts its basic attack at 14 m, so Mage-only staves have none) and a damage range; 303 templates in total.

## 3. Rules

### 3.1 Derived values (additions to PROTOCOL_STATS.md §2)

With `I` = the main-hand weapon's `speed_ms` (absent: the class's interval `Ic`) and `S` = the sum of the
equipped weapons' half-ranges (`attack - low`):

| Value | Formula |
|---|---|
| attack interval | `I` (haste effects shorten it as before) |
| swing factor | `I / Ic` (1 without a weapon speed) — the multiplier of a **basic** attack |
| reach | the main-hand weapon's `reach` for the melee classes, else the class range (a Mage keeps 14 m) |
| attack_min / attack_max | `round(attack_raw ∓ S × (1 + P / 20))` (the weapon part scales as in §2 of PROTOCOL_STATS.md) |
| max_hp / max_mp | `+ Σ max_hp` / `+ Σ max_mp` of the equipped items (before regeneration is derived) |
| critical / dodge | `+ Σ crit` / `+ Σ dodge` permille (caps unchanged: 50 % / 25 %) |

A basic attack hits for `attack × swing factor`: a slow weapon hits harder, a fast one softer, so the damage
per second before defence is the same for every weapon speed (`attack / Ic`); a slow weapon loses less of it to
the target's defence (subtracted per hit), a fast one benefits more from per-hit effects. Skills keep their
own multipliers and cooldowns and do not use the swing factor.

### 3.2 Damage with a range (addition to PROTOCOL_COMBAT.md §1.1)

`damage = max(1, round((rolled × multiplier − defense × 0.5) × (critical ? 1.5 : 1)))` with `rolled` uniform in
`[attack_min − 0.1 × mid, attack_max + 0.1 × mid]`, `mid = (attack_min + attack_max) / 2`. With
`attack_min == attack_max` this is exactly the old `attack × uniform(0.9, 1.1)`. Basic attacks and damaging
skills use it; periodic damage and absorb shields keep the mean `attack`.

### 3.3 What the server validates

- A basic swing waits the character's interval (with haste) after the previous one and the 500 ms global
  cooldown (unchanged mechanism, new interval); the target must be within the character's reach + 0.5 m.
- Equipping: the slot rule (a ring into Ring1 or Ring2, an earring into Earring1 or Earring2, a necklace /
  belt into its own slot), the class, the level, the attribute requirements (`AttributeTooLow`, as M11),
  two-handers keep the off hand empty (M8).
- Every change of the equipment's totals sends CHAR_STATS (and SELF_STATS) as in PROTOCOL_STATS.md §3.2; a
  changed maximum hp also refreshes the observers' bars (ENTITY_STATS).

## 4. Wire

### 4.1 CHAR_STATS (appended after `heal_permille`, 16 bytes; 64 in total)

```
u16 attack_min            // attack power with the weapons' lowest damage (<= attack)
u16 attack_max            // ... highest damage (>= attack)
u16 reach_cm              // basic attack reach in centimetres
u16 swing_permille        // basic hit factor x 1000 (weapon interval / class interval)
u16 bonus_hp              // the equipment's max_hp lines (already in max_hp)
u16 bonus_mp              // the equipment's max_mp lines (already in max_mp)
u16 bonus_crit_permille   // the equipment's crit lines (already in crit_permille), 0..1000
u16 bonus_dodge_permille  // the equipment's dodge lines (already in dodge_permille), 0..1000
```

Readers reject a bonus permille above 1000 (and, as before, a short packet).

### 4.2 The Equipment container has 13 slots

| slot | name | takes |
|---|---|---|
| 0 | MainHand | main_hand, either_hand, two_hand |
| 1 | OffHand | off_hand, either_hand (not next to a two-hander) |
| 2..6 | Head, Chest, Legs, Hands, Feet | armour (M15) |
| **7** | **Necklace** (reserved before) | necklace |
| **8** | **Belt** | belt |
| **9, 10** | **Ring1, Ring2** | ring |
| **11, 12** | **Earring1, Earring2** | earring |

`INVENTORY` therefore lists at most 13 + 28 = **41** entries; `ITEM_MOVE_REQ` places with container 0 are valid
for slots 0..12 (13+ → `BadSlot`). `ITEM_MOVE_RES` results are unchanged (`WrongSlot` for a ring in an earring
slot, `AttributeTooLow` for an unmet requirement). Accessories are never visible to others: `CharSummary`,
`ENTITY_SPAWN` and `ENTITY_EQUIP` still carry the hands and the five armour slots only.

### 4.3 Unchanged

Opcodes, `ENTER_WORLD_RES`, `SELF_STATS`, `ITEM_UPDATE`, `ItemEntry` (15 bytes), the upgrade, loot, shop and
quest packets.

## 5. Persistence and upgrades

- Migration **`db/migrations/0011_accessory_slots.sql`**: the CHECK `character_items_slot` allows Equipment
  slots 0..12 (docs/DATABASE.md §3.10). Nothing else changes; accessories are ordinary item rows.
- **Upgrades** (PROTOCOL_UPGRADE.md) keep scaling attack and defence only: the damage range moves with the
  upgraded attack (its width stays), **bonus lines and attribute bonuses never scale**. Accessories are **not
  upgradable** (`NotUpgradable`): their only scalable number would be a defence of 0..5, so a +7 accessory would
  change almost nothing while costing the most expensive stones; their value is in the lines.

## 6. Client presentation contract (not wire format, but agreed behaviour)

- **Inventory window**: the left column Head / Chest / Legs, the right column Hands / Feet / **Belt** (where the
  dimmed reserved slot was), the two hands under the figure and a row of five smaller slots under them:
  **Necklace, Ring, Ring, Earring, Earring** (empty slots show "Kolye", "Yüzük", "Küpe", "Kemer"). A double /
  right click puts a ring or an earring into the first free of its two slots (else the first). Under the bag
  a gold totals line from CHAR_STATS: "Saldırı 75 (70–81)  ·  Savunma 28  ·  Hız 2,3 sn" (a Mage: "Büyü").
- **Tooltips** (weapons): "Saldırı 13–19" (upgraded: "Saldırı 15–21 (+2 tav)"), "Hız: 2,3 sn (yavaş)" (below
  1,3 s "hızlı", from 2,0 s "yavaş", else "orta"), "Erim: 3,3 m". Bonus lines in their own colour (light blue
  `#8CC4F2`): "Güç +5", "Can +75", "Mana +30", "Kritik Vuruş +%1,5", "Kaçınma +%0,9". Requirements as M11
  ("Gerekli Güç: 25", red while unmet); class and level lines red when unmet. Accessories: "Takı · Yüzük ·
  Nadir" and "Takılar yükseltilemez.".
- **Künye** (character sheet): the derived rows are Can, Mana, Saldırı Gücü (the mean), **Temel Vuruş**
  ("187–249": a basic hit before defence, the attack range × the swing factor; a Mage "Temel Büyü"), Savunma,
  Kritik Vuruş, Kaçınma, Can / Mana Yenilenmesi, Saldırı Hızı, **Erim** ("3,3 m"), a Priest's İyileştirme.
  The equipment's share is shown left of the value in the bonus colour: "Can (+145) 665", "Kritik Vuruş
  (+%1,5) %8,0".
- **Swing tempo**: a player's basic swing animation lasts 42 % of its attack interval (0,45..1,05 s): a dagger
  (1,2 s) flicks in ~0,5 s, a sword (1,6 s) ~0,67 s, an axe (2,2 s) ~0,92 s, a halberd (2,3 s) ~0,97 s; the
  local player uses the server's interval (CHAR_STATS, haste included), other players their main-hand
  weapon's. Monsters are unchanged. The client's auto-attack chase uses the server's reach.
- **Automation**: places `necklace`, `belt`, `ring1`, `ring2`, `earring1`, `earring2` (`scene itemmove`,
  `scene expectequipslot <place> <id|empty>`), widgets `inv.equip.<place>` and `inv.summary`, derived fields
  `hit` and `reach` (`scene expectderived reach 3.3`), states `swingsN`, `swingfast` / `swingmedium` /
  `swingslow` (the last own swing), `scene combatlog` prints the swing tempo and the server's gap.

## 7. vo_admin

`vo_admin show-character` prints every equipment slot (`equip ring1 Korlu Kaya Yüzüğü`, `equip earring2 -`) and a
`totals:` line derived with the shared code (hp and mp with the items' share, attack with its range, defence,
interval, swing factor, reach, critical / dodge chance with the items' share).

## 8. Testing hook (server configuration, off by default, a warning in the log when on)

`world.testing_enter_items = "171, 205, 279"`: a character entering the world gets one unit of each listed
template it holds nowhere yet, in its lowest free bag slots, stored before the entry (unknown ids refuse the
start). Used by the botclient `equipment` scenario and the `m18_gear` UI flow.

## 9. Limits

| Limit | Value |
|---|---|
| Equipment slots | 13 (was 8) |
| INVENTORY entries | 41 (was 36) |
| CHAR_STATS size | 64 bytes (was 48) |
| `speed_ms` | 800..3000 ms (shipped 1200..2400) |
| `reach` | 1.5..4.0 m (shipped 2.2..3.3) |
| Bonus lines per item | max_hp / max_mp ≤ 1000, crit / dodge ≤ 100 permille |
| Loot table lines (`gamedata/loot.toml`) | 80 (was 32; data only, a roll still keeps 8 items) |
