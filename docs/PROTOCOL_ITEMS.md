# VedatOnline Network Protocol — Items extension (v2)

Status: **contract for milestone M8 "Kuşan"** (items, inventory, equipment). Extends docs/PROTOCOL.md;
everything there stays valid. `PROTOCOL_VERSION` becomes **2** when this ships (client and server are
released together; a v1 client is refused with VersionMismatch). The combat extension
(docs/PROTOCOL_COMBAT.md) comes after this one and will be version 3; where it says "No items" and
"version 2", this document wins.

M10 (docs/PROTOCOL_LOOT.md, protocol v4) adds stackable materials (`quantity` > 1, merging bag
moves, §1.2 there), the `material` kind and the `max_stack` / `value` / `rarity` keys (§7 there).
M14 (docs/PROTOCOL_SHOP.md, protocol v8) adds the `consumable` kind (potions, `restore_hp` /
`restore_mp`), makes `value` the merchants' price and lets consumables sit on the skill bar.

Goal: every character owns items, wears a weapon that everybody can see, can open a bag, and can
move, equip and unequip items. Each class starts with its own weapon, so classes look different.
No loot, no trade, no shops, no dropping items on the ground, no consumables yet.

## 1. Model

- **Item template**: static data in `gamedata/items.toml` (id `u16` ≥ 1, name, kind, slot rule, allowed
  classes, level requirement, attack/defense bonus, model, icon, grip). Loaded by server and client
  from the same file through one shared loader (`shared/game/Items.h`). Template id 0 means "nothing".
  The server refuses to start if the file is missing or invalid; the client shows an unknown
  template as "?" and draws no model for it.
- **Item instance**: one row in the database, `item_id` (`u64`, unique forever), a template id, a
  quantity (always 1 in M8; the field exists for later stackables), and a place.
- **Place** = `container` + `slot`:

| container | value | slots |
|---|---|---|
| Equipment | 0 | 0 MainHand · 1 OffHand · 2 Head · 3 Chest · 4 Legs · 5 Hands · 6 Feet · 7 reserved (8 slots) |
| Bag | 1 | 0..27 (28 slots, shown 7 × 4) |

  M8 has templates only for MainHand and OffHand; the other equipment slots exist on the wire and in
  the UI (empty) and accept nothing.
- Everything is **server-authoritative**. The client asks to move an item; the server validates and
  answers. The client may show the move optimistically but must roll back on a failed result.
- Bonuses (`attack`, `defense`) are shown in tooltips now and feed the combat formulas later; M8 has
  no derived stats on the wire.

### 1.1 Slot rules

| template `slot` | may be placed in |
|---|---|
| `main_hand` | Equipment/MainHand |
| `off_hand` | Equipment/OffHand |
| `either_hand` | Equipment/MainHand or Equipment/OffHand |
| `two_hand` | Equipment/MainHand, and only while OffHand is empty (see below) |

Equipping also requires: the character's class is in `classes`, and `level ≥ template.level`.
Nothing may be placed in OffHand while MainHand holds a `two_hand` item.

### 1.2 Moves

One request covers everything: `ITEM_MOVE_REQ(from, to)`.

- Bag → empty Bag slot: move. Bag ↔ occupied Bag slot: swap.
- Bag → Equipment: equip. If the target slot is occupied the two items swap, provided the displaced
  item fits where the moved one came from (a bag slot always fits).
- Equipping a `two_hand` item while OffHand is occupied: the off-hand item is moved to the lowest
  free bag slot (the slot the two-hander left counts as free once it has moved); if there is none the
  whole request fails with `BagFull` and nothing changes.
- Equipping into OffHand while MainHand holds a `two_hand` item fails with `SlotBlocked`.
- Equipment → Bag: unequip (to an empty slot, or swap if the bag item may legally take the
  equipment slot; otherwise `WrongSlot`).
- Equipment → Equipment (MainHand ↔ OffHand): allowed when both results are legal (two daggers swap).
- `from == to`, an empty `from`, or an out-of-range slot fails; nothing changes.

A move is atomic: either every affected slot changes (in memory and in one database transaction) or
none does.

### 1.3 Starter kit

`gamedata/items.toml` has one `[[starter_kit]]` per class. It is granted **once** per character, the
first time the character enters the world with `characters.starter_kit_granted = false` (this covers
characters created before this milestone). Granting and setting the flag happen in one transaction.

### 1.4 Visible equipment

Other players see only MainHand and OffHand template ids. They are carried in `CharSummary` (so the
character list shows the weapon), in `ENTITY_SPAWN`, and in `ENTITY_EQUIP` when they change.

A character whose kit has not been granted yet (never entered the world since this milestone) is
listed with the kit's template ids in `CharSummary`, so the list never shows an unarmed character
that will be armed a moment later.

## 2. Changes to existing packets

```
CharSummary        (append after zone_id)
                   u16 main_hand_template, u16 off_hand_template

ENTITY_SPAWN       (append after flags)
                   u16 main_hand_template, u16 off_hand_template
```

`ENTER_WORLD_RES` is unchanged; on success the server sends `INVENTORY` right after it, before the
first `ENTITY_SPAWN`.

### 2.1 Worn armour (M17)

Armour is visible too (docs/CLIENT_RENDERING.md §18). Three packets end with the template ids of
Equipment/Head, Chest, Legs, Hands, Feet (slots 2..6; 0 = empty or hidden item), 10 bytes:

```
CharSummary        (append after off_hand_template)       u16 armor[5]
ENTITY_SPAWN       (append after state, the v3 tail)       u16 armor[5]
ENTITY_EQUIP       (append after off_hand_template)       u16 armor[5]
```

`ENTITY_EQUIP` is now sent when the weapons **or** the armour change. A character without a granted kit
lists the kit's armour (none today). Implemented without a protocol version bump on purpose (the quest
milestone bumps to v11 in parallel; the two changes are merged into that version).

## 3. New opcodes

| Opcode | Name | Dir |
|---|---|---|
| 0x0400 | INVENTORY | S→C |
| 0x0401 | ITEM_MOVE_REQ | C→S |
| 0x0402 | ITEM_MOVE_RES | S→C |
| 0x0403 | ITEM_UPDATE | S→C |
| 0x0404 | ENTITY_EQUIP | S→C |

All are valid only in the `InWorld` state (C→S in any other state: `ERROR(BadState)`).

```
ItemEntry:
    u8  container
    u8  slot
    u64 item_id          // 0 = the slot is now empty (then template_id and quantity are 0)
    u16 template_id
    u16 quantity

INVENTORY         u8 bag_size                     // 28
                  u8 count, ItemEntry[count]      // every occupied slot, equipment first, by slot

ITEM_MOVE_REQ     u32 seq                         // client-chosen, echoed back
                  u8 from_container, u8 from_slot
                  u8 to_container,   u8 to_slot

ITEM_MOVE_RES     u32 seq
                  u8  result

ITEM_UPDATE       u8 count (1..8), ItemEntry[count]   // new content of each changed slot

ENTITY_EQUIP      u32 entity_id
                  u16 main_hand_template, u16 off_hand_template
```

Move `result`: 0 Ok · 1 NoItem (from is empty) · 2 BadSlot (out of range, or from == to) ·
3 WrongSlot (template not allowed there) · 4 WrongClass · 5 LevelTooLow · 6 SlotBlocked ·
7 BagFull · 8 ServerError.

Order on success: `ITEM_UPDATE` (all changed slots) first, then `ITEM_MOVE_RES(Ok)`; then, if the
visible equipment changed, `ENTITY_EQUIP` to the mover **and** to every player who has the mover in
view. On failure only `ITEM_MOVE_RES(result)` is sent.

## 4. Limits

| Limit | Value |
|---|---|
| Bag slots | 28 |
| Equipment slots | 8 |
| `ITEM_MOVE_REQ` per second per client | 10 (more → `ERROR(Flood)` and close) |
| Template ids | 1..65535 |

## 5. Persistence

Migration `db/migrations/0002_items.sql`:

- `characters.starter_kit_granted boolean not null default false`
- `character_items(item_id bigserial primary key, char_id → characters, container smallint,
  slot smallint, template_id integer, quantity integer, created_at)`, with
  `unique (char_id, container, slot)` (deferrable, so a swap fits in one transaction) and checks on
  the container/slot ranges and `quantity ≥ 1`.

Items are loaded with the character on `ENTER_WORLD_REQ` and written through on every successful
move (the move is not acknowledged before the transaction commits). A row whose template id is
unknown to the server is kept in the database, logged once, and not sent to the client.

## 6. Client presentation contract (not wire format, but agreed behaviour)

- **I** (and a HUD button) opens and closes the inventory window: equipment slots on the left around a
  small character figure or silhouette, the 7 × 4 bag on the right. Esc closes it before opening the
  menu.
- Items are moved by drag and drop; a double click (or right click) on a bag item equips it into its
  natural slot, on an equipped item it unequips to the first free bag slot.
- Hovering an item shows a tooltip: name, kind, slot, attack/defense, allowed classes, level
  requirement; requirements the character does not meet are red. A failed move shows a short Turkish
  message.
- MainHand is drawn attached to the right hand bone and OffHand to the left hand bone (shields on the
  left forearm), for the local player, remote players, the character list and the creation screen.
  On the creation screen the figure holds the starter kit of the class being chosen, so picking a
  class visibly changes the character.
- Attachment transforms per grip and per character look come from the asset pipeline
  (docs/ASSET_PIPELINE.md, "Weapons").
