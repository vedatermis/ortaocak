# VedatOnline Network Protocol — Item upgrade extension (v10)

Status: **contract for milestone M16 "Tav"** (upgrading weapons and armour at the smiths). Extends
docs/PROTOCOL.md and every extension up to PROTOCOL_LEVELS.md (v9); everything there stays valid unless
this document says otherwise. `PROTOCOL_VERSION` becomes **10** (client and server are released together;
an older client is refused with `VersionMismatch`).

Goal: the first real Akçe sink. A player brings a weapon, shield or armour piece to one of the three
smiths, pays an upgrade stone and an Akçe fee, and with a chance the item gains a level (+1 … +10) that
raises its attack / defence. From +4 an attempt can fail; from +6 a failure destroys the item. All names,
texts, icons and numbers are our own (docs/LORE.md); the mechanics follow the classic MMORPG pattern.

The rules of §2 are shared code (`shared/game/Upgrade.h`): the client shows what the server decides with
the same tables; only the roll is the server's. Tables below are the shipped values and must be kept in
step with the code and the data files.

## 1. Smiths

`gamedata/npcs.toml` gains an optional key `upgrades = true|false` (default false) on a merchant: such an
NPC is a **smith** and upgrades items. The three shipped smiths (Demirci Ilgaz, Usta Berkay, Örsbaşı
Gürşad) have it and also sell the three stones. A merchant's stock may now hold **32** entries (24 before
v10; `SHOP_OPEN_RES` count 0..32, the loader's limit likewise).

## 2. Rules

### 2.1 What can be upgraded

- Every **weapon, shield and armour piece** (`kind` weapon / shield / armor), uniques included. Never
  stackables, materials or potions (`NotUpgradable`).
- The level belongs to the **item instance** (database row), 0..10, shown as "Ad (+N)". It survives
  moves, equipping, relogs, zone changes. A new item (loot, purchase, starter kit, `give-item`) is +0.
- **Only bag items** are upgraded: the request names a bag slot (an equipped item must be taken off first;
  the client says "Önce çıkarmalısın: yalnız çantadaki eşya yükseltilir."). Reason: the visible
  equipment and the stats never change in the middle of a fight because of an attempt, and a destroyed
  item never leaves an equipment slot (no `ENTITY_EQUIP` from an upgrade).

### 2.2 One attempt

`UPGRADE_REQ(seq, npc, bag_slot, item_id, from_level, stone_slot)`. Checks, first failure wins:

| # | Result | Condition | Where |
|---|---|---|---|
| 1 | `NotOpen` | the shop of `npc` is not open for this player (also: another shop is open) | world |
| 2 | `NotSmith` | the open shop's NPC does not upgrade (`upgrades` not set) | world |
| 3 | `YouAreDead` | | world |
| 4 | `OutOfRange` | farther than the talk range 4 m + 0.5 m (the shop itself stays open up to 8 m) | world |
| 5 | `NoItem` | `bag_slot` ≥ 28, empty, a hidden item, or a different `item_id` | session |
| 6 | `Changed` | the item is not at `from_level` (a stale or repeated click: nothing is attempted) | session |
| 7 | `NotUpgradable` | not a weapon / shield / armour piece | session |
| 8 | `MaxLevel` | already +10 | session |
| 9 | `NoStone` | `stone_slot` = 0xFF and no fitting stone in the bag; or `stone_slot` out of range / empty | session |
| 10 | `WrongStone` | `stone_slot` holds something other than the fitting stone (or the item itself) | session |
| 11 | `NotEnoughCoins` | the fee is above the committed balance | session |
| 12 | `ServerError` | 16 item operations already waiting, or the write failed; nothing changed | session |
| — | `Success` / `Failed` / `Destroyed` | the attempt: stone and fee spent, the item changed (or not), in one transaction | |

`stone_slot` 0xFF = the lowest bag slot holding a stone of the needed grade (what the client sends).
The session decides 5–11 in its item queue, in order with moves, loot takes, purchases, sales and potion
uses, against the committed inventory and balance, rolls, writes, and answers after the commit (§4.3).

### 2.3 Stone, chance and failure

| Item | Stone (`upgrade_stone`) |
|---|---|
| tier 1–2 (required level 1–8) | **Ham Tav Taşı** (166, grade 1) |
| tier 3–4 (level 9–16) | **Saf Tav Taşı** (167, grade 2) |
| tier 5 (level 17–20) and **every unique** | **Usta Tav Taşı** (168, grade 3) |

Tier = `min(5, (required level − 1) / 4 + 1)`: the M15 weapon lines (levels 5 / 8 / 11 / 14 / 17) and
armour lines (1 / 5 / 9 / 13 / 17) fall into tiers 2 / 2 / 3 / 4 / 5 and 1 / 2 / 3 / 4 / 5; the old items
of levels 1–4 are tier 1.

| Target | +1 | +2 | +3 | +4 | +5 | +6 | +7 | +8 | +9 | +10 |
|---|---|---|---|---|---|---|---|---|---|---|
| Success chance | 100 % | 100 % | 100 % | 80 % | 60 % | 45 % | 30 % | 15 % | 8 % | 3 % |
| On failure | — | — | — | item stays | item stays | **destroyed** | destroyed | destroyed | destroyed | destroyed |

The stone and the fee are spent on every attempt, success or not. The server rolls a uniform number
0..999 per attempt (its own generator, seeded per session from the system's random device): success if
the roll is below the chance in permille. The client never rolls.

**Testing hook** `world.testing_upgrade_outcome` (server.toml; off by default, a warning in the log when
set): `"success"` — every attempt succeeds; `"fail"` — every attempt that can fail fails (targets from +4);
`"fail_from_<N>"` (N = 4..10) — targets below N succeed, from N on they fail. Used by the botclient
`upgrade` scenario, the UI flows and the tests. A second hook, `world.testing_enter_coins = <n>`, tops up
the balance of every character entering the world to at least n Akçe (stored like a gain), so flows and
the scenario need no database.

### 2.4 Fee

`fee = tier base × target multiplier` (Akçe, rounded down):

| Tier base | tier 1: 6 | tier 2: 20 | tier 3: 55 | tier 4: 110 | tier 5: 190 |
|---|---|---|---|---|---|

| Target | +1 | +2 | +3 | +4 | +5 | +6 | +7 | +8 | +9 | +10 |
|---|---|---|---|---|---|---|---|---|---|---|
| Multiplier | ×1 | ×1.5 | ×2 | ×3 | ×4 | ×6 | ×8 | ×11 | ×15 | ×20 |
| Tier 1 | 6 | 9 | 12 | 18 | 24 | 36 | 48 | 66 | 90 | 120 |
| Tier 3 | 55 | 82 | 110 | 165 | 220 | 330 | 440 | 605 | 825 | 1 100 |
| Tier 5 | 190 | 285 | 380 | 570 | 760 | 1 140 | 1 520 | 2 090 | 2 850 | 3 800 |

Uniques use the tier of their level for the fee (and always the Usta stone).

### 2.5 Stat effect

An upgrade raises the item's **attack and defence** (whichever it has) and nothing else (attributes,
requirements, value stay). Bonus at level L = `round(base × p(L) / 100)`, halves up:

| Level | +1 | +2 | +3 | +4 | +5 | +6 | +7 | +8 | +9 | +10 |
|---|---|---|---|---|---|---|---|---|---|---|
| p(L), % of the base | 5 | 10 | 15 | 20 | 25 | 31 | 38 | 46 | 55 | 65 |

Small steps (5 %) up to +5, larger ones (6–10 %) from +6. For a base of **8 or more** (weapons and
shields) every level from +6 on adds at least 1 more than the level before, so a risky attempt never leaves
the number unchanged; armour pieces (1–7) use the plain percentage (some low levels add nothing to a
piece of 1–3). Tuned so that **+7 on a tier-N item is about a tier-N+1 item at +0** (tiers 2–5:
+7 / next = 1.00–1.25; tier 1, the starter-kit level: 0.71–1.00 — DEVELOPMENT_PLAN.md §36 has the
table). The world uses the upgraded values wherever equipment counts (`game::equipmentBonus`: attack,
defence, `CHAR_STATS` / `SELF_STATS`, damage).

### 2.6 Selling, logging

- A merchant pays **the plain sale price** (`value / 2`) for an upgraded item: upgrading never creates
  Akçe. Uniques stay unsellable. The client asks before selling an upgraded item.
- The server logs **one line per attempt** (INFO): character, item id and name, `+from -> +target`, the
  outcome, the chance, the roll, the fee, the stone and the balance after it.

## 3. Wire

### 3.1 New opcodes

| Opcode | Name | Dir |
|---|---|---|
| 0x0A00 | UPGRADE_REQ | C→S |
| 0x0A01 | UPGRADE_RES | S→C |

`UPGRADE_REQ` is valid only in `InWorld` (otherwise `ERROR(BadState)`); one that arrives after a leave
started or while changing zone is dropped silently (still counted against the flood cap).

### 3.2 Result codes

`UpgradeResult`: 0 Success · 1 Failed · 2 Destroyed · 3 NotOpen · 4 NotSmith · 5 YouAreDead · 6 OutOfRange
· 7 NoItem · 8 Changed · 9 NotUpgradable · 10 MaxLevel · 11 NoStone · 12 WrongStone · 13 NotEnoughCoins ·
14 ServerError. 0–2 are attempts (stone and fee spent), 3–14 refusals (nothing changed).

## 4. Layouts (little-endian)

### 4.1 Requests and answers

```
UPGRADE_REQ        u32 seq                           // client-chosen, echoed
                   u32 npc_id                        // the smith (entity id) whose shop is open
                   u8  bag_slot                      // 0..27: the item
                   u64 item_id                       // the item meant (must be in that slot)
                   u8  from_level                    // its level as the client knows it
                   u8  stone_slot                    // 0..27, or 0xFF = the lowest fitting stone
UPGRADE_RES        u32 seq
                   u8  result                        // §3.2
                   u8  bag_slot                      // echoed
                   u16 template_id                   // the item's template (0 if the slot held nothing known)
                   u8  level                         // Success: the new level; Failed / refusals: the current
                                                     // one; Destroyed: the level it had
                   u16 stone_template_id             // the stone spent (0 unless an attempt)
                   u32 fee                           // Akçe paid (0 unless an attempt)
```

Sizes: UPGRADE_REQ 19, UPGRADE_RES 15. Readers reject an unknown result and a level above 10.

### 4.2 Changed packet: ItemEntry

`ItemEntry` (INVENTORY, ITEM_UPDATE) gains **`u8 upgrade`** (0..10) after `quantity` — 15 bytes per entry
(14 before). An empty entry has 0. Readers reject a level above 10. `SHOP_OPEN_RES` may carry 32 entries.
Nothing else changes (`ENTITY_SPAWN` / `ENTITY_EQUIP` still carry template ids only: other players do not see
upgrade levels).

### 4.3 Sequences

- **Attempt** (Success / Failed / Destroyed): `ITEM_UPDATE` (the item's slot — new level, or emptied — unless
  Failed, and the stone's slot), `COINS(balance, gained 0)`, `UPGRADE_RES` — after the transaction committed.
- **Refusal**: only `UPGRADE_RES(result)` (world refusals in the world's next batch).

## 5. Limits

| Limit | Value |
|---|---|
| UPGRADE_REQ per second per client | 5 (more → `ERROR(Flood)` and close); the client sends at most 4 |
| Item operations waiting in the session | 16 (as v8); beyond → `ServerError` |
| Upgrade level | 0..10 |
| Stock entries per merchant | 32 (24 before v10) |

## 6. Persistence

- Migration **`db/migrations/0009_item_upgrades.sql`**: `character_items.upgrade smallint NOT NULL DEFAULT 0`
  with `CHECK (upgrade BETWEEN 0 AND 10)` and `CHECK (upgrade = 0 OR quantity = 1)`. Existing rows get 0;
  nothing else is rewritten.
- An attempt writes through `IAccountService::applyItemWrite` in **one transaction**: the stone stack
  (`quantity` change, 0 deletes the row), the fee (`coins_removed`, refused below 0 under the row lock) and
  `ItemWrite::upgrades` — the item's new level (`UPDATE ... WHERE upgrade = before AND quantity = 1`) or its
  deletion (`DELETE ... WHERE upgrade = before`). If any part does not hold (e.g. `vo_admin set-coins` or
  `set-upgrade` meanwhile) nothing changes and the answer is `ServerError`.
- Loading items reads the level (`upgrade` column); a row with a level outside 0..10 or an upgraded stack
  makes the character's items inconsistent (refused entry, logged).

### 6.1 vo_admin

- `list-items <character>`: an `upg` column ("+7" / "-"), names as the game shows them ("Ad (+7)").
- `show-character <character>`: a `gear` section — every weapon / shield / armour piece with its place,
  "Ad (+N)", item id, `+N/10` and the upgraded attack / defence.
- `set-upgrade <character> <item> <level>` (**testing**): sets one item's level 0..10; `<item>` is an
  item id or a place as list-items prints it (`bag/3`, `equip/main_hand`). Refused for non-upgradable
  templates, stacks, unknown items. A character in the world sees it on its next entry (until then its own
  attempts on that item fail with `ServerError`: the stored level differs).

## 7. Client spec (presentation contract, not wire format)

All texts are Turkish and listed here.

### 7.1 Names everywhere

- An upgraded item is named "**Ad (+N)**" in its tooltip, the Yükselt section, the sale question and the
  sale / result lines; bag and equipment icons carry a "+N" badge in the top-left corner (gold; from +7 lilac).
- From **+7** the name is drawn in **lilac `#C98BFF`** (apart from uncommon green, rare blue and the uniques'
  amber `#F0A23C`, which a unique keeps at any level).
- Tooltip: "Saldırı +20 (+4 tav)" / "Savunma +9 (+2 tav)" (the upgraded value, the upgrade's share) and
  "Yükseltme: +N / +10"; for a bag item while the Yükselt tab is open "Sağ tık: Yükselt bölümüne koy"
  (or "Yükseltilemez").

### 7.2 The Yükselt section

- A smith's shop window has two tabs under the greeting: **"Satın al"** (the stock and the sell zone, as v8)
  and **"Yükselt"**. Other merchants have no tabs. The window opens on "Satın al".
- In "Yükselt" the stock and the sell zone give way to the section: an item well (a right click on a bag
  item or a drop on the window puts it in; a right click on the well empties it; an equipped item:
  "Önce çıkarmalısın: yalnız çantadaki eşya yükseltilir."); the hint while empty: "Yükseltmek için çantadan
  bir silah ya da zırh buraya sürükle ya da sağ tıkla."; with an item: its name (colour), "+4 → +5",
  "Saldırı 18 → 19"; "Taş:" with the stone's icon and name and "Elinde: n" (red at 0); "Ücret:" with the
  Akçe coin (red when the balance is short); "Başarı şansı: %45" (green ≥ 60 %, gold ≥ 30 %, red below);
  what a failure does — "Başarısız olamaz." / "Tutmazsa eşya olduğu gibi kalır." / **"Tutmazsa eşya
  parçalanıp YOK OLUR!"** (red, bold, in a red frame); the **"Yükselt"** button (disabled while a local check
  fails, with the reason in red: "Çantanda Saf Tav Taşı yok.", "Yeterli Akçen yok.", "… en yüksek seviyede
  (+10).", "… yükseltilemez: yalnız silah, kalkan ve zırh."); the result line of the last answer.
- **Confirmation** before every attempt that can destroy the item (+6 and above): a dialog titled
  "Dikkat: <Ad (+N)>", "<Ad (+N)> → +M: başarı şansı %X. Tutmazsa eşya YOK OLUR; <taş> ve <n> Akçe de gider.
  Yükseltilsin mi?", buttons "Yükselt" / "Vazgeç" (Esc = Vazgeç). Nothing is sent before "Yükselt".
- Effect over the well: success — a gold ring growing out of the well, rays, the well lit; failure — the
  well shakes, greyed; destroyed — a red flash, the item's icon in two halves drifting apart, cracks. About
  1.4 s; the result line stays 8 s.

### 7.3 Result texts

| Result | Text |
|---|---|
| Success | "Tav tuttu! <Ad (+N)> oldu." (gold) |
| Failed | "Tav tutmadı: <Ad (+N)> olduğu gibi kaldı." (orange) |
| Destroyed | "Tav tutmadı: <Ad (+N)> parçalandı!" (red) |
| NotOpen | "Önce demirciyle konuş." |
| NotSmith | "Bu satıcı eşya yükseltmez." |
| YouAreDead | "Ölüyken yükseltme yapamazsın." |
| OutOfRange | "Demirciye daha yakın olmalısın." |
| NoItem | "Yükseltilecek eşya çantada yok." |
| Changed | "Eşya değişti; yeniden seç." |
| NotUpgradable | "<Ad> yükseltilemez: yalnız silah, kalkan ve zırh." |
| MaxLevel | "<Ad> en yüksek seviyede (+10)." |
| NoStone | "Çantanda <taş> yok." |
| WrongStone | "Bu eşyaya <taş> gerekir." |
| NotEnoughCoins | "Yeterli Akçen yok." |
| ServerError | "Yükseltme kaydedilemedi, tekrar dene." |

The result goes to the section and to the line above the skill bar (not the window's footer); a refusal
while the Yükselt tab is not open (e.g. at a potion seller) to the footer as for shop results.

### 7.4 Automation (`scene` commands, states)

Commands: `shoptab buy|upgrade`, `upgradepick <bagSlot>`, `upgradeclear`, `upgradestart`, `upgradeforce
<bagSlot> [stoneSlot|any] [fromLevel]` (a raw request), `expectupgrade <bagSlot> <level>`,
`expectupgradepanel <text>`, `upgradelog`. States: `shoptabs`, `upgradetab`, `upgradeitem`,
`upgradebusy` / `upgradeidle`, `upgradeconfirm`, `upgradeready`, counters `upgradesentN`, `upgradeokN`,
`upgradefailedN`, `upgradedestroyedN`, `upgraderefusedN`, `upgradecancelledN`. Widgets: `shop.tab.buy`,
`shop.tab.upgrade`, `upgrade.panel`, `upgrade.slot`, `upgrade.name`, `upgrade.button`, `upgrade.result`,
`upgrade.fee`, `upgrade.chance`, `upgrade.owned`, `upgrade.fail`, `upgrade.hint`, `upgrade.contents`.

## 8. Shipped stones (items.toml)

| Id | Name | Grade | Icon | Price | Sells for | Max stack | Rarity |
|---|---|---|---|---|---|---|---|
| 166 | Ham Tav Taşı | 1 | `temper_stone_raw` | 25 | 12 | 50 | common |
| 167 | Saf Tav Taşı | 2 | `temper_stone_pure` | 110 | 55 | 50 | uncommon |
| 168 | Usta Tav Taşı | 3 | `temper_stone_master` | 320 | 160 | 50 | rare |

Icons: `tools/blender/gen_upgrade_icons.py` (headless Blender, procedural). Drops (loot.toml, the last lines of
each table): Ham 1–3 % from levels 1–10 (Kor Bekçisi 30 % ×1–2, Anız Ağası / Körük Hanı 40 % ×1–3), Saf
0.8–2.5 % from levels 8–19 (the level-10 bosses 15 %, Cüruf Anası 40 % ×1–2), Usta 0.3 % (Cüruf Zırhlısı)
and 1 % (Kor Büyücüsü), Cüruf Anası 10 %, Ocak Efendisi 50 % ×1–2. A loot table may hold 32 lines (24 before).
