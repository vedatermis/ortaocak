# VedatOnline Network Protocol — Merchants and consumables extension (v8)

Status: **contract for milestone M14 "Pazar"** (town merchants, buying and selling, potions on the
skill bar). Extends docs/PROTOCOL.md and every extension up to PROTOCOL_ZONES.md (v7); everything
there stays valid unless this document says otherwise. `PROTOCOL_VERSION` becomes **8** (client and
server are released together; an older client is refused with `VersionMismatch`).

Goal: the Akçe players collect gets a use. Every town (Ortaocak, Tanyeri, Demirağız) has two
merchants standing near its race spawn: one sells potions (şerbetçi / şifacı), one sells gear
(demirci). A player walks up, opens the shop, buys (Akçe out, items in) and sells (items out, Akçe
in). Potions restore hp or mp; they can be used from the bag or put on the skill bar next to skills.
Names, texts and numbers are our own (docs/LORE.md).

Out of scope: quests, repair / durability, storage, player trade, item upgrades, a level cap raise,
buying back what was sold, limited stock.

The data lives in `gamedata/npcs.toml` (merchants, loaded by `shared/game/Npcs.h`) and
`gamedata/items.toml` (prices = `value`, potions = `kind = "consumable"`); the rules of §2 are shared
code (`shared/game/Shop.h`), so the client predicts with the server's code. Tables below are the
shipped values and must be kept in step with the files.

## 1. NPCs

### 1.1 Data (`gamedata/npcs.toml`)

| Key | Meaning |
|---|---|
| `id` | 1..999, stable forever (it is the `template_id` of the NPC's `ENTITY_SPAWN`) |
| `name`, `title` | Turkish, ≤ 48 bytes each ("Sürmeli Bacı", "Şerbetçi") |
| `greeting` | Turkish, ≤ 200 bytes; the shop window's first line |
| `model` | model key `[a-z0-9_]` (art: `npcs/<model>.vmesh`), unique |
| `zone`, `x`, `z`, `yaw` | where it stands (y from the terrain), radians, forward = (sin yaw, cos yaw) |
| `kind` | `"merchant"` (the only kind in v8) |
| `stock` | merchant: 1..24 item ids of items.toml in display order, each with `value` ≥ 1, none twice |

Loader rules (strict, "file:line: problem" like zones.toml): unknown keys, a missing key, ids
outside 1..999 or used twice, a model key used twice or not `[a-z0-9_]`, empty / overlong texts,
coordinates outside 0..4096, yaw outside ±2π, an unknown kind, an empty / overlong stock, an item
twice, an unknown item or one without a value. At most 64 NPCs.

### 1.2 Startup checks (vo_server)

For every NPC of a loaded zone the server checks the spot against that zone's terrain and refuses to
start (exit code 6, a message naming the NPC, the file and the rule) unless:

- it is inside the zone and walkable, and so are 8 points 1 m round it;
- the road layer (splat channel G, as a share of the four weights) is at most 50 % there — merchants
  stand beside the road, never on its centre line;
- it lies more than gate radius + 2 m from every gate marker of its zone and more than 4 m from every
  race spawn (`spawn_aydor` / `spawn_kargan`) and arrival marker (`arrive_*`) of its zone.

NPCs of a zone the server does not load (not in zones.toml) are ignored with a warning, like monster
spawns.

### 1.3 In the world

- An NPC is an entity of **entity type 4** (`kEntityTypeNpc`) seen through the normal area of
  interest: `ENTITY_SPAWN` when it comes into view (and on world entry / zone arrival with the rest),
  `ENTITY_DESPAWN` when it leaves the view. Its entity id is allocated at startup and never changes
  while the server runs.
- `ENTITY_SPAWN` fields: `entity_type` 4, `name` = the NPC's name, `template_id` = the NPC id,
  position and yaw from the data, `level` 0, `race` / `class` / `face` / `hair` 0, `flags` 0, both hand
  templates 0, `hp` / `max_hp` 0, `state` 0. Title, model and greeting come from npcs.toml by the id.
- NPCs never move (no `ENTITY_MOVE`), never fight, cannot be targeted for attacks or skills
  (`ATTACK_REQ` and enemy `SKILL_REQ` naming one: `ACTION_FAIL(NotAttackable)`; a friendly skill:
  `WrongTargetType`), are never part of an area skill and do not block movement. `TARGET_REQ` with
  an NPC id is accepted (informational selection, as for players).

## 2. Rules

### 2.1 Prices

| Price | Formula |
|---|---|
| merchant sells (player buys) | `value` of the item (items.toml) per unit |
| merchant buys (player sells) | `floor(value / 2)` per unit |

Every shipped item has a value ≥ 1; an item worth 0 or 1 sells for 0 and **cannot be sold**
(`CannotSell`). Nothing is bought back above what it cost (no arbitrage). Any merchant buys any item
(not only its own stock).

### 2.2 Shop requests

- **Open**: `SHOP_OPEN_REQ(npc)`. Checks, first failure wins: `NotFound` (not a merchant spawned to
  the requester), `YouAreDead`, `OutOfRange` (horizontal distance > talk range **4 m** + 0.5 m), else
  `Ok` with the stock and prices. The shop is now **open** for that player: one at a time, opening
  another closes the first silently; `SHOP_CLOSE_REQ(npc)` closes it (no answer).
- The world closes an open shop with `SHOP_CLOSED(npc, reason)` when the player is farther than
  **8 m** from the merchant (`OutOfRange`; also when the merchant leaves the player's view, e.g. after a
  leap) or dies (`YouDied`); silently on a zone change or on leaving the world.
- **Buy**: `SHOP_BUY_REQ(seq, npc, template, quantity)`. Checks, first failure wins:

  | # | Result | Condition | Where |
  |---|---|---|---|
  | 1 | `NotOpen` | the shop of `npc` is not open for this player | world |
  | 2 | `YouAreDead` | | world |
  | 3 | `OutOfRange` | farther than 4 m + 0.5 m | world |
  | 4 | `NotInStock` | `template` is not in this merchant's stock | world |
  | 5 | `BadQuantity` | 0, or more than the item's `max_stack` (one stack per purchase; gear: 1) | session |
  | 6 | `NotEnoughCoins` | quantity × price above the committed balance | session |
  | 7 | `BagFull` | not every unit fits (stacks of the same template first, then free bag slots); nothing is bought | session |
  | 8 | `ServerError` | 16 item operations already waiting, or the write failed; nothing changed | session |
  | — | `Ok` | the units are in the bag and the price is paid, in one transaction | |

- **Sell**: `SHOP_SELL_REQ(seq, npc, bag_slot, quantity)`. Checks, first failure wins:

  | # | Result | Condition | Where |
  |---|---|---|---|
  | 1–3 | `NotOpen`, `YouAreDead`, `OutOfRange` | as for buying | world |
  | 4 | `BadQuantity` | quantity 0 | session |
  | 5 | `NoItem` | `bag_slot` ≥ 28, empty, or holding a hidden item (unknown template) | session |
  | 6 | `BadQuantity` | more than the stack holds | session |
  | 7 | `CannotSell` | the item's sell price is 0 | session |
  | 8 | `PurseFull` | the balance would pass 1 000 000 000 (`kMaxCoins`); nothing is sold | session |
  | 9 | `ServerError` | as for buying | session |
  | — | `Ok` | the units are gone (the row deleted when all of the stack went) and the Akçe added, in one transaction | |

  **What cannot be sold**: equipped items (the request names a bag slot only: unequip first),
  hidden items, items with a sell price of 0 (none ship), and nothing outside the bag. Starter-kit
  items can be sold like any other item (they are sold by the smiths too).
- Buying / selling while in combat is allowed (the merchant is in a safe town anyway). Requests a
  player sends while changing zone are dropped silently (PROTOCOL_ZONES.md §3.4, like loot requests);
  the client forgets what it had in flight on `ZONE_CHANGE_BEGIN`.
- Order: the session decodes and flood-checks, the world checks 1–4 against the live position and
  hands an accepted request back in the player's batch, the session decides the rest **in order with
  item moves and loot takes against the committed inventory and balance** (one queue), writes, and
  answers after the commit (§3.4).

### 2.3 Consumables (potions)

- Items of `kind = "consumable"` restore `restore_hp` and / or `restore_mp` **at once** when used
  (clamped to the maximum; no heal over time). They stack (`max_stack` 20), have no equipment slot
  and no level requirement, and their template id is ≤ 32767 (it must fit a bar slot, §4.1).
- **One shared cooldown** for every consumable: **10 s** (`kConsumableCooldownMs`), started when the
  world accepts a use. It is remembered across a relog and a zone change like skill cooldowns
  (PROTOCOL_SKILLS.md §1.7: 60 s of relog memory, lost on a server restart) and reported with
  `ITEM_COOLDOWN`.
- **Use**: `ITEM_USE_REQ(seq, template, bag_slot)` — `bag_slot` 0..27 names the stack, or `0xFF` =
  the lowest bag slot holding the template (what the skill bar sends). Checks, first failure wins:

  | # | Result | Condition | Where |
  |---|---|---|---|
  | 1 | `NotUsable` | the template is unknown or not a consumable | session |
  | 2 | `NoItem` | no such stack in the committed bag | session |
  | 3 | `YouAreDead` | | world |
  | 4 | `Stunned` | the player has a stun effect (no shipped source stuns players in v8) | world |
  | 5 | `OnCooldown` | the shared cooldown is running | world |
  | 6 | `AlreadyFull` | everything it restores is at the maximum already (an hp potion at full hp) | world |
  | 7 | `NoItem` | the stack was gone by the time its turn came in the item queue | session |
  | 8 | `ServerError` | 16 item operations waiting, or the write failed; nothing consumed | session |
  | — | `Ok` | one unit consumed (stored), then restored | |

- **Atomic**: the world accepts (checks 3–6) and starts the cooldown, the session consumes one unit
  in one transaction, and only after the commit the world restores. If the write fails nothing is
  consumed and nothing restored (the cooldown stays: it is the price of the failed attempt). If the
  player died between the acceptance and the commit, the unit is consumed and nothing is restored.
  Using a potion does not interrupt a cast and is allowed in combat, sitting (the player stays
  seated) and while casting.
- After the restore the player gets `SELF_STATS` (and observers `ENTITY_STATS` for the new hp) as for
  any hp / mp change, and the player plus everyone who has it in view get `ITEM_USED`.

### 2.4 Shipped consumables (items.toml)

| Id | Name | Icon key | Restores | Price | Sells for |
|---|---|---|---|---|---|
| 16 | Kızılca Şerbet | `potion_hp_small` | 70 hp | 8 | 4 |
| 17 | Közlü Şerbet | `potion_hp_medium` | 180 hp | 25 | 12 |
| 18 | Şafak Çiyi | `potion_mp_small` | 40 mp | 8 | 4 |
| 19 | Ayaz Özü | `potion_mp_medium` | 100 mp | 25 | 12 |

For scale: max hp is 90–98 at level 1 and 270–300 at level 10 without spent points; max mp 34
(warrior) to 70 (mage) at level 1. A small şerbet is most of a level-1 bar, a Közlü Şerbet most of a
level-6 one.

### 2.5 Merchants (npcs.toml)

| Id | Name | Title | Model key | Zone | x, z | Sells |
|---|---|---|---|---|---|---|
| 1 | Sürmeli Bacı | Şerbetçi | `npc_potion_esik` | 1 Ortaocak | 244.0, 276.0 | 16, 17, 18, 19 |
| 2 | Demirci Ilgaz | Pazar Demircisi | `npc_smith_esik` | 1 Ortaocak | 283.0, 252.0 | 1, 2, 3, 4, 5, 6, 12, 14 |
| 3 | Rahibe Aksun | Soluk Güneş Şifacısı | `npc_potion_aydor` | 2 Tanyeri | 240.0, 279.5 | 16, 17, 18, 19 |
| 4 | Usta Berkay | Burç Silahçısı | `npc_smith_aydor` | 2 Tanyeri | 251.5, 277.5 | 1, 2, 3, 4, 5, 7, 12 |
| 5 | Kül Anası Tamar | Ocak Şifacısı | `npc_potion_kargan` | 3 Demirağız | 268.0, 331.0 | 16, 17, 18, 19 |
| 6 | Örsbaşı Gürşad | Ocak Demircisi | `npc_smith_kargan` | 3 Demirağız | 261.0, 327.0 | 1, 2, 3, 4, 5, 6, 8, 12 |

Each merchant faces its town's race spawn (Ortaocak: the potion seller faces `spawn_aydor`, the
smith `spawn_kargan`), 6–12 m from it, with a straight walkable line from the spawn marker.

Gear prices (value): Er Kılıcı 12, Meşe Kalkan 10, Avcı Hançeri 10, Köz Asası 12, Soluk Güneş Topuzu
12, Ocak Baltası 20, Burç Kılıcı 60, Kırağı Asası 60, Çömez Bıçağı 15, Kömürlü Kılıç 18, Cüruf Topuzu
45, Bekçi Kalkanı 70 (the last two existed; Kömürlü Kılıç and Bekçi Kalkanı are not sold). What
each class can buy that helps:

| Class | Upgrade on sale | Where |
|---|---|---|
| Savaşçı | Burç Kılıcı (sv. 3, saldırı 9) · Ocak Baltası (iki el, 10) | Tanyeri · Ortaocak, Demirağız |
| Haydut | Çömez Bıçağı (sv. 2, saldırı 5, either hand) | every smith |
| Büyücü | Kırağı Asası (sv. 3, saldırı 10) — no better staff exists yet; mana potions are the mage's real buy | Demirağız |
| Şifacı | Cüruf Topuzu (sv. 4, saldırı 7, savunma 1, İrade +2) | Ortaocak |

Every smith also sells every class's starter weapon (and the warrior's shield) as a replacement.

## 3. Wire

### 3.1 New opcodes

| Opcode | Name | Dir |
|---|---|---|
| 0x0900 | SHOP_OPEN_REQ | C→S |
| 0x0901 | SHOP_OPEN_RES | S→C |
| 0x0902 | SHOP_CLOSE_REQ | C→S |
| 0x0903 | SHOP_CLOSED | S→C |
| 0x0904 | SHOP_BUY_REQ | C→S |
| 0x0905 | SHOP_BUY_RES | S→C |
| 0x0906 | SHOP_SELL_REQ | C→S |
| 0x0907 | SHOP_SELL_RES | S→C |
| 0x0908 | ITEM_USE_REQ | C→S |
| 0x0909 | ITEM_USE_RES | S→C |
| 0x090A | ITEM_COOLDOWN | S→C |
| 0x090B | ITEM_USED | S→C |

The C→S requests are valid only in `InWorld` (otherwise `ERROR(BadState)`); ones that arrive after a
leave started are dropped silently (like `MOVE_REQ`), ones sent while changing zone too (§2.2).

### 3.2 Layouts (little-endian)

```
SHOP_OPEN_REQ      u32 npc_id                        // entity id
SHOP_OPEN_RES      u32 npc_id
                   u8  result                        // §3.3
                   u8  count                         // 0..24; 0 unless result = Ok
                   ShopEntry[count]                  // the stock in the data file's order
ShopEntry:         u16 template_id                   // != 0
                   u32 price                         // Akçe per unit, >= 1
SHOP_CLOSE_REQ     u32 npc_id
SHOP_CLOSED        u32 npc_id
                   u8  reason                        // 1 OutOfRange, 2 YouDied

SHOP_BUY_REQ       u32 seq                           // client-chosen, echoed
                   u32 npc_id
                   u16 template_id
                   u16 quantity
SHOP_BUY_RES       u32 seq
                   u8  result
                   u16 template_id                   // echoed
                   u16 quantity                      // echoed
                   u32 cost                          // Akçe paid; 0 unless Ok

SHOP_SELL_REQ      u32 seq
                   u32 npc_id
                   u8  bag_slot                      // 0..27
                   u16 quantity
SHOP_SELL_RES      u32 seq
                   u8  result
                   u16 template_id                   // what the slot held (0 if nothing known)
                   u16 quantity                      // echoed
                   u32 gained                        // Akçe received; 0 unless Ok

ITEM_USE_REQ       u32 seq
                   u16 template_id
                   u8  bag_slot                      // 0..27, or 0xFF = the lowest slot holding it
ITEM_USE_RES       u32 seq
                   u8  result                        // §3.3
                   u16 template_id                   // echoed
ITEM_COOLDOWN      u32 remaining_ms                  // 1..total_ms
                   u32 total_ms                      // 10000
ITEM_USED          u32 entity_id                     // who drank it
                   u16 template_id
                   u32 hp_restored                   // actually gained (0 when already full)
                   u32 mp_restored
```

Sizes: SHOP_OPEN_REQ 4, SHOP_OPEN_RES 6 + 6n, SHOP_CLOSE_REQ 4, SHOP_CLOSED 5, SHOP_BUY_REQ 12,
SHOP_BUY_RES 13, SHOP_SELL_REQ 11, SHOP_SELL_RES 13, ITEM_USE_REQ 7, ITEM_USE_RES 7, ITEM_COOLDOWN 8,
ITEM_USED 14.

Readers reject (protocol error): an unknown result / reason, a SHOP_OPEN_RES count above 24 or
entries on a failure, a ShopEntry with template 0 or price 0, an ITEM_COOLDOWN with remaining 0 or
above the total. Ids, templates, slots and quantities in requests are not checked by the reader (the
server answers with a result).

### 3.3 Result codes

`ShopResult` (SHOP_OPEN_RES, SHOP_BUY_RES, SHOP_SELL_RES): 0 Ok · 1 NotFound · 2 YouAreDead ·
3 OutOfRange · 4 NotOpen · 5 NotInStock · 6 BadQuantity · 7 NotEnoughCoins · 8 BagFull · 9 NoItem ·
10 CannotSell · 11 PurseFull · 12 ServerError. (Open uses 0–3; buy 0, 2–8, 12; sell 0, 2–4, 6, 9–12.)

`ItemUseResult` (ITEM_USE_RES): 0 Ok · 1 NoItem · 2 NotUsable · 3 YouAreDead · 4 OnCooldown ·
5 AlreadyFull · 6 Stunned · 7 ServerError.

### 3.4 Sequences

- **Open**: `SHOP_OPEN_RES` in the world's next batch.
- **Buy Ok**: `ITEM_UPDATE` (every changed bag slot), `COINS(balance, gained = 0)`, `SHOP_BUY_RES(Ok)`
  — after the transaction committed. Failures: only `SHOP_BUY_RES(result)`.
- **Sell Ok**: `ITEM_UPDATE` (the slot, emptied or smaller), `COINS(balance, gained)`,
  `SHOP_SELL_RES(Ok)` — after the commit. Failures: only `SHOP_SELL_RES(result)`.
- **Use Ok**: `ITEM_COOLDOWN` (the world's batch that accepted it), then after the commit
  `ITEM_UPDATE` (the stack) and `ITEM_USE_RES(Ok)`, then in the world's next tick `ITEM_USED` (to the
  player and its observers) and `SELF_STATS`. Failures: only `ITEM_USE_RES(result)` (after the
  cooldown started, check 7 / 8 still come as `ITEM_USE_RES`).
- **World entry / zone arrival**: if the consumable cooldown is still running, `ITEM_COOLDOWN` right
  after `CHAR_STATS` (with `SKILL_COOLDOWNS`); NPCs in view arrive as `ENTITY_SPAWN`s like everyone
  else.

### 3.5 Changes to existing packets

- `ENTITY_SPAWN`: new `entity_type` **4** (NPC), fields as §1.3.
- `SKILL_BAR`, `SKILL_BAR_SET_REQ`, `SKILL_BAR_SET_RES`: unchanged layouts; a slot value may now be
  an item slot (§4.1).
- `COINS.gained` is 0 for a purchase (the balance went down) and the sale's Akçe for a sale.
- Everything else: unchanged.

## 4. The skill bar with items

### 4.1 Encoding

A bar slot (u16) is `0` (empty), `1..63` a skill (as v6), or **`0x8000 | template_id`** (template
1..32767) a consumable. The same consumable may sit on several slots, next to skills.

- `SKILL_BAR_SET_REQ` checks every slot: skill slots as v6; an item slot must name a consumable of
  items.toml (owning one is **not** required: a slot keeps its place when the last potion is used, and
  shows count 0) — else the whole request is `NotLearned` (nothing changed).
- On load, item slots naming an unknown or non-consumable template are sent as 0 (like unlearned
  skills).
- `vo_admin reset-skills` resets the whole bar (items too) to the default.

### 4.2 Using an item slot

The client sends `ITEM_USE_REQ(seq, template, 0xFF)` (not `SKILL_REQ`). A `SKILL_REQ` with an item
slot's value is answered `UnknownSkill` as any unknown id.

## 5. Economy

Akçe income comes from monster drops (loot.toml) and selling materials. Per kill: Kül Çömezi /
Anız Yanığı / Kırağı Kayası (levels 1–2) 3–9 Akçe (6 on average) plus ≈0.7 from selling the material
(Kül Tozu 1 Akçe each, Cüruf Parçası 2); Cüruf Yürüyen / Yanık Nöbetçi / Donuk Ocakçı (3–4)
12–25 (18.5) plus ≈1.5; the elite Kor Bekçisi (6) 120–180 (and half the time 1–2 Közlü Şerbet).

| Level | XP to next | Hunting | XP per kill | Kills | Akçe in the level | Main purchase | Share of the level's income |
|---|---|---|---|---|---|---|---|
| 1 | 80 | level-1 kinds | 25 | 4 | ≈ 27 | 3 Kızılca Şerbet (24) | ≈ 90 % (the first level is short) |
| 2 | 240 | level-2 kinds | 32 | 8 | ≈ 54 | Çömez Bıçağı (15) or 3 şerbet | 28–45 % |
| 3 | 460 | level-3 kinds | 60 | 8 | ≈ 160 | Burç Kılıcı / Kırağı Asası (60) | 38 % |
| 4 | 740 | level-4 kinds | 75 | 10 | ≈ 200 | Cüruf Topuzu (45) + 3 Közlü Şerbet (75) | 60 % |
| 5 | 1050 | level-4 kinds | 75 | 14 | ≈ 280 | 4 Közlü Şerbet + 4 Ayaz Özü (200) | 70 % |
| 6 | 1410 | level 4 / Kor Bekçisi | 75 | 19 | ≈ 380 | potions | ≈ 50 % |
| 7–9 | 1800–2690 | level 4 (xp 80–40 %) | 60–30 | 30–90 | 600–1800 | potions | < 30 % |

(XP to next = 80 × level^1.6, rounded to 10; experience falls off 20 % per level above monster + 2,
PROTOCOL_COMBAT.md. Akçe = kills × the average per kill.) Reading: a character that drinks one
şerbet every 3 kills spends ≈ 40 % of its income on potions at levels 1–2 (small ones) and ≈ 13 %
at 3–4 (small ones) or ≈ 40 % (Közlü Şerbet); a level-3 weapon costs about a third of level 3's
income, so gear is an occasional purchase and potions a regular one. Levels 7–9 earn more than they
can spend: an Akçe sink for later milestones (repair, upgrades) is not part of v8.

Starter-kit items are sold by the smiths for 10–20 Akçe and bought back for 5–10: a character
cannot farm Akçe by selling its kit (it is granted once).

## 6. Limits

| Limit | Value |
|---|---|
| SHOP_OPEN / CLOSE / BUY / SELL_REQ per second per client | 10 combined (more → `ERROR(Flood)` and close) |
| ITEM_USE_REQ per second per client | 10 (more → `ERROR(Flood)` and close) |
| Item operations waiting in the session (moves, loot takes, buys, sells, uses) | 16 moves; buys / sells / uses beyond 16 waiting → `ServerError` |
| Talk range / shop view distance | 4 m (+ 0.5 m) / 8 m |
| Stock entries per merchant | 24 |
| NPCs | 64 (ids 1..999) |
| Purchase | one stack (≤ max_stack) per request |
| Consumable cooldown | 10 s, shared |
| Akçe balance | 0..1 000 000 000 (unchanged) |

## 7. Persistence

- Buying, selling and using write through `IAccountService::applyItemWrite` (PROTOCOL_LOOT.md §6) in
  **one transaction** each: quantity changes (`WHERE quantity = before`; 0 deletes the row), new stacks
  in free bag slots, and the balance. `ItemWrite` gains `coins_removed`: the balance is lowered under
  the character row lock and the write fails (nothing changed, `ServerError`) if it would go below 0
  (e.g. `vo_admin set-coins` lowered it meanwhile). The answer is sent only after the commit.
- No new table. Migration **`db/migrations/0007_bar_items.sql`** widens the `characters_skill_bar`
  CHECK: a stored bar element is a skill id 0..63 or **−template_id** (−32767..−1) for an item slot
  (the wire's `0x8000 | id`). Existing rows (all 0..63) are unchanged and stay valid; nothing is
  rewritten.
- The consumable cooldown is not stored (relog memory only, §2.3).

### 7.1 vo_admin

- `vo_admin show-character <name>`: the bag lists consumables with what they restore, and the bar
  shows item slots as "Kızılca Şerbet (eşya 16)".
- `vo_admin give-item <name> <template> [count]` (exists) gives potions too.

## 8. Client spec (presentation contract, not wire format)

All texts are Turkish and listed here. The NPC's name, title and greeting come from npcs.toml (by
the spawn's template id), item names / descriptions from items.toml.

### 8.1 NPCs in the world

- Model `npcs/<model>.vmesh` standing at the spawn's position and yaw, idle animation; until the art
  exists any placeholder (or nothing) is fine. A client that does not know entity type 4 must ignore
  it (v7 clients did; the v8 minimal client does).
- Name plate over the head in a merchant colour (not the monster / player colours): the name, below it
  the title in a smaller line: "Sürmeli Bacı" / "‹Şerbetçi›".
- Hover cursor: a coin-bag cursor or highlight; tooltip "<Ad> — <Unvan>" and "Konuşmak için tıkla".
- Clicking an NPC (or pressing E next to one when no loot bag is reachable) walks the player into
  talk range (4 m) and sends `SHOP_OPEN_REQ`. Attack keys / skills on an NPC do nothing (no request).
- NPCs are not part of Tab targeting.

### 8.2 Shop window

- Title: the NPC's name, subtitle its title; first line the greeting (italic).
- Left: the stock as a list or grid: icon, name (rarity colour), price "<n> Akçe"; entries the player
  cannot afford are drawn with the price in red; gear the class cannot use or whose level is too high
  shows the requirement in red (it can still be bought).
- Bottom: "Akçe: <balance>" (live, from `COINS`).
- Buying: click (or right-click) an entry → for a stackable item a quantity box ("Kaç tane?", 1..max
  stack, default 1, up/down arrows, Enter = buy) showing "Toplam: <n> Akçe"; for gear a confirmation
  only above 50 Akçe: "<Ad> için <n> Akçe ödensin mi?" (Evet / Hayır). Sends `SHOP_BUY_REQ`; buttons wait
  while one is in flight.
- Selling: while the shop is open the inventory window opens next to it; right-click a bag item (or
  drag it onto the shop window) → for a stack of more than 1 the quantity box ("Kaç tane satılsın?",
  default the whole stack) with "Alınacak: <n> Akçe"; items with a rarity above common or worth ≥ 40
  Akçe ask "<Ad> <n> Akçe'ye satılsın mı?". Dragging an equipped item onto the shop: "Önce çıkarmalısın."
  (no request). Tooltip of a bag item while the shop is open adds "Satış: <n> Akçe" (or "Satılamaz").
- Esc / the X / walking away closes it (`SHOP_CLOSE_REQ` when the player closed it). Esc order: loot
  window → shop window → Tuşlar → Yetenekler → Künye → envanter → hedef → menü.
- `SHOP_CLOSED`: the window closes; OutOfRange → "<Ad> çok uzakta kaldı."; YouDied → no extra text.
- On `ZONE_CHANGE_BEGIN` the window closes silently and requests in flight are forgotten.

### 8.3 Result texts (combat message line / window footer)

| Event | Text |
|---|---|
| SHOP_BUY_RES Ok | "<Ad> alındı (<n> Akçe)." — for a stack "<n> × <Ad> alındı (<toplam> Akçe)." |
| SHOP_SELL_RES Ok | "<Ad> satıldı (+<n> Akçe)." — for several "<n> × <Ad> satıldı (+<toplam> Akçe)." |
| NotFound | "Burada satıcı yok." |
| YouAreDead | "Ölüyken alışveriş yapamazsın." |
| OutOfRange | "Satıcıya daha yakın olmalısın." |
| NotOpen | "Önce satıcıyla konuş." |
| NotInStock | "Bu satıcı bunu satmıyor." |
| BadQuantity | "Bu miktar olmaz." |
| NotEnoughCoins | "Yeterli Akçen yok." |
| BagFull | "Çantanda yer yok." |
| NoItem | "Satılacak bir şey yok." |
| CannotSell | "Bu satılamaz." |
| PurseFull | "Kesen dolu; daha fazla Akçe taşıyamazsın." |
| ServerError (shop) | "Alışveriş kaydedilemedi, tekrar dene." |
| SHOP_CLOSED OutOfRange | "<Ad> çok uzakta kaldı." |
| ITEM_USE_RES NoItem | "Elinde <Ad> kalmadı." |
| NotUsable | "Bu kullanılamaz." |
| YouAreDead (use) | "Ölüyken kullanamazsın." |
| OnCooldown | "Henüz yeniden içemezsin." |
| AlreadyFull | "Canın zaten dolu." (hp potion) / "Manan zaten dolu." (mp potion) |
| Stunned | "Sersemken kullanamazsın." |
| ServerError (use) | "Kullanılamadı, tekrar dene." |

### 8.4 Potions in the bag and on the bar

- Bag: double-click / right-click a consumable uses it (`ITEM_USE_REQ(seq, template, slot)`) instead
  of equipping. Tooltip: name, "Tüketilebilir", "Can: +<n>" / "Mana: +<n>", "Bekleme: 10 sn (bütün
  iksirler)", the description, the stack ("<n> / 20"); while a shop is open "Satış: <n> Akçe".
- Skill bar: drag a consumable from the bag onto a slot (`SKILL_BAR_SET_REQ` with `0x8000 | id`);
  drag it off / right-click clears it as for skills. The slot shows the item's icon
  (`ui/items/<icon>.png`) and the **count** of that template in the whole bag in the lower right
  corner (white; red "0" and the icon greyed when none is left; the slot stays). The key / click sends
  `ITEM_USE_REQ(seq, template, 0xFF)`; with 0 left: "Elinde <Ad> kalmadı." and no request.
- Cooldown: `ITEM_COOLDOWN` sweeps every consumable slot (bar and bag icons) for `remaining_ms`; the
  500 ms global skill cooldown does not apply to potions.
- Tooltip on a bar item slot: as in the bag, plus "Çubuktan kullan: <tuş>".
- `ITEM_USED`: a green floating number "+<hp>" over the drinker (blue "+<mp>" for mana), a short
  drinking gesture if the art has one; for the local player also the HUD bars move (from
  `SELF_STATS`).

### 8.5 Loot

Potions appear in loot windows like any item; the M10 pickup line applies ("+2 Kızılca Şerbet").
