# VedatOnline Network Protocol — Loot extension (v4)

Status: **contract for milestone M10 "Ganimet"** (coins, stackable materials, loot drops).
Extends docs/PROTOCOL.md, docs/PROTOCOL_ITEMS.md (v2) and docs/PROTOCOL_COMBAT.md (v3); everything
there stays valid unless this document says otherwise. `PROTOCOL_VERSION` becomes **4** (client and
server are released together; an older client is refused with `VersionMismatch`).

Goal: killed monsters drop a loot container at the corpse with **Akçe** (coins) and items; the
killer has it to itself for 30 s, then anybody may take from it; materials stack in the bag.
No vendors, no trade, no dropping items on the ground, no splitting stacks in this milestone.

## 1. Model

### 1.1 Akçe (coins)

- Every character has one coin balance, a `u32` in `0 .. 1 000 000 000` (`kMaxCoins`), persisted
  in `characters.coins` (§6). New characters and every character from before M10 start with 0.
- The owner learns it from `COINS` (§4.4): once on world entry (right after `INVENTORY`) and after
  every change. Other players never see it.
- Coins only come from loot in M10 (and `vo_admin set-coins`, which a character in the world sees
  after its next entry; a loot gain in the meantime reports the stored balance, so the admin value
  is never lost).

### 1.2 Stackable items

- An item template has `max_stack` (default 1). Weapons and shields are 1; materials more (§7).
- `ItemEntry.quantity` (already on the wire since v2) carries the stack size, `1 .. max_stack`.
  A stored quantity above the template's `max_stack` is accepted as it is (never grows further).
- **Merging on move.** `ITEM_MOVE_REQ` from a bag slot to another bag slot holding the **same
  template** with `max_stack > 1` and a quantity below `max_stack` merges instead of swapping:
  `n = min(from.quantity, max_stack − to.quantity)` units move; the target keeps its `item_id` and
  grows by `n`; the source shrinks by `n` and disappears (row deleted) when it reaches 0. The result
  is `ITEM_UPDATE` with both slots (an emptied slot as `item_id` 0) and `ITEM_MOVE_RES(Ok)`. A full
  target stack (`quantity = max_stack`) swaps as before. Equipment ↔ bag moves never merge.
- **Merging on pickup** (loot, §3.4): units go first into existing bag stacks of the same template
  that are not full (lowest slot first), then into the lowest free bag slots (each new stack at most
  `max_stack`).
- Materials have no equipment slot: moving one into any equipment slot is `WrongSlot`.
- There is no split request in M10.

### 1.3 Loot containers

- A loot container is an **entity** of the zone: `entity_type` **3**. It is spawned and despawned
  through the normal area of interest (`ENTITY_SPAWN` / `ENTITY_DESPAWN`, PROTOCOL.md §5.4); it
  never moves (no `ENTITY_MOVE`).
- It appears when a monster dies and its roll (§2) is not empty, at the corpse position (the
  monster's position at death). The corpse itself still despawns 3 s after death as in v3; the
  container is a separate entity with its own id.
- **Owner.** The container belongs to the *owner character*: the player who dealt the most damage
  to the monster (the same player who receives the experience, PROTOCOL_COMBAT.md §1.1). Ties go to
  the player who engaged the monster first (pulled it or first damaged it: the earlier entry of its
  threat list). Players that dropped off the threat list before the death (died, left, ran beyond
  the leash or into a safe zone) do not count. If nobody on the list dealt damage or the winner is
  no longer in the zone, the player who dealt the killing blow is the owner. Ownership is tracked by
  **character**, so it survives a re-login.
- **Owner window.** For **30 s** after creation only the owner may open or take from it; afterwards
  anybody may.
- **Lifetime.** It despawns **90 s** after creation, or as soon as it is empty (no coins and no
  items) — in both cases only once no take from it is still being written (§3.4); such a take keeps
  it a little longer.
- **Contents.** A coin amount (`u32`) and up to **8** item entries. Each entry has a fixed
  `index` (0..7) for the container's whole life, a template id and a quantity (`1 .. max_stack`).
  Indices are assigned 0, 1, 2… in loot-table order when the container is created.

### 1.4 `ENTITY_SPAWN` of a loot container

The v3 layout is unchanged; for `entity_type` 3 the fields mean:

| Field | Value |
|---|---|
| `name` | display name of the monster it came from (e.g. "Kül Çömezi"); the client labels it |
| `race`, `class`, `face`, `hair` | 0 |
| `level` | level of the monster it came from |
| `x`, `y`, `z` | corpse position (`y` on the terrain) |
| `yaw`, `flags` | 0 |
| `main_hand_template`, `off_hand_template` | 0 |
| `hp`, `max_hp` | 0 |
| `template_id` | template id of the monster it came from (`gamedata/monsters.toml`) |
| `state` | loot bits below (the dead / in-combat / sitting bits are never set) |

Loot state bits (`ENTITY_SPAWN.state`, `ENTITY_STATE.state`; new in v4, so a reader accepts the
mask `0x1F`):

| Bit | Name | Meaning (per recipient) |
|---|---|---|
| 0x08 | LootLocked | the owner window is running and the recipient is **not** the owner |
| 0x10 | LootMine | the owner window is running and the recipient **is** the owner |

When the owner window ends, every observer that saw a non-zero value gets `ENTITY_STATE(id, 0)`.
Both bits are 0 on players and monsters.

## 2. Loot tables (`gamedata/loot.toml`)

Static data, loaded by the server with a strict loader (`shared/game/Loot.h`, the same strictness
as items.toml / monsters.toml; vo_server refuses to start, exit 6, if the file is missing or
invalid). Keyed by monster template id.

```toml
[[table]]
monsters = [1, 2]        # monster template ids (each id in at most one table; must exist)
coins = [3, 9]           # optional, default [0, 0]: uniform integer in [min, max], always dropped
[[table.drop]]
item = 9                 # item template id (must exist)
chance = 0.45            # 0 < chance <= 1, independent per drop line
count = [1, 2]           # optional, default [1, 1]: uniform in [min, max], 1 <= min <= max <= max_stack
```

Rules: at most 8 drop lines per table, no item twice in one table, coins `0 .. 100 000`. A monster
without a table drops nothing. The roll (server RNG): coins uniform in the range, then each drop
line in file order succeeds with its `chance` and yields one entry of `count` units. An empty roll
(0 coins, no entry) creates no container.

M10 tables: §8.

## 3. Rules

### 3.1 Checks

`LOOT_OPEN_REQ` and `LOOT_TAKE_REQ` are checked in this order (first failure wins):

| # | Result | Condition |
|---|---|---|
| 1 | `NotFound` | the id is not a loot container currently spawned to the requester |
| 2 | `YouAreDead` | the requester is dead |
| 3 | `OutOfRange` | horizontal distance between player and container > **3 m** (+ the 0.5 m tolerance of every range, so > 3.5 m) |
| 4 | `NotYours` | the owner window is running and the requester is not the owner |
| 5 | `ServerError` | take only: the requester already has 8 takes in flight (§5) |
| 6 | `AlreadyTaken` | take only: `what` names an index ≥ 8 (other than 0xFE / 0xFF), an empty entry, or an entry being taken by someone right now; 0xFE with 0 coins left; 0xFF with nothing left at all |

Looting is allowed in combat and inside safe zones.

### 3.2 Opening and viewers

- `LOOT_OPEN_REQ(id)` → `LOOT_OPEN_RES(id, result)`; on `Ok` it is followed by `LOOT_CONTENTS`
  and the requester becomes a **viewer** of the container. Opening an already open container
  answers `Ok` and resends the contents.
- A player views at most one container: opening another one ends the previous view silently (no
  `LOOT_CLOSED`; the client replaced it itself).
- Every viewer gets `LOOT_CONTENTS` whenever the visible contents change (a take starts, or units
  that did not fit come back).
- A view ends on `LOOT_CLOSE_REQ` (no answer; a `LOOT_CLOSE_REQ` for a container that is not the
  open one is ignored), or by the server with `LOOT_CLOSED(id, reason)`: `Gone` (the container is
  despawning: emptied or expired; `ENTITY_DESPAWN` follows in the same tick), `OutOfRange` (the
  viewer is farther than **6 m** from it, or it left the viewer's area of interest), `YouDied`.
  Leaving the world ends it without a packet.
- Taking does **not** require the container to be open.

### 3.3 Visible contents

`LOOT_CONTENTS` lists the coins and the entries that are available now. Units being written for a
take (§3.4) are not listed (for anybody); units that did not fit come back with their old index.
A container with everything in flight is shown empty (coins 0, no entries) and is not despawned
before the write finishes.

### 3.4 Taking

`LOOT_TAKE_REQ(seq, id, what)`: `what` = entry index 0..7, **0xFE** coins only, **0xFF**
everything (coins first, then the entries by index).

1. The world checks §3.1. A failure is answered at once with `LOOT_TAKE_RES(seq, id, result)`,
   nothing else.
2. Otherwise the requested units are **reserved**: removed from the visible contents (viewers get
   `LOOT_CONTENTS`) and handed to the player's session.
3. The session places them against its committed inventory and balance: coins go to the balance
   (at most up to `kMaxCoins`, the rest stays: `PurseFull`); items fill existing stacks, then free
   bag slots (§1.2); what does not fit stays (`BagFull`). The changes (stack quantities, new
   stacks, balance) are written in **one database transaction**; the next item move or take of that
   player waits for it.
4. After the commit the owner gets `ITEM_UPDATE`s with every changed bag slot (at most 8 entries
   per packet, as many packets as needed, none if only coins were taken), then `COINS` (if coins
   were taken), then `LOOT_TAKE_RES(seq, id, result)`. Units
   that were not taken (did not fit, or the write failed) go back into the container under their
   old index; viewers get `LOOT_CONTENTS` again in the next world tick. That `LOOT_CONTENTS` may
   reach the taker before or after its `LOOT_TAKE_RES` (the world and the session answer
   independently): a client must not assume an order between the two. If nothing fits at all
   (and no coins are taken) there is no write: `LOOT_TAKE_RES(BagFull / PurseFull)` comes at once.
5. If the container is now empty and nothing is in flight, viewers get `LOOT_CLOSED(Gone)` and
   observers `ENTITY_DESPAWN`.

Result of a take: `Ok` (everything requested was taken), `BagFull` (some or all item units did not
fit; what fit was taken), `PurseFull` (the balance would pass `kMaxCoins`; coins that fit were
taken), `ServerError` (nothing was taken: the write failed, or the player already has 8 takes in
flight). If several apply: `ServerError` > `BagFull` > `PurseFull`.

**No duplication, no loss.** Units are in exactly one place at any time: the container, a
reservation, or the character's inventory. A reservation ends exactly once: committed (gone from
the container for good), or returned (back in the container). Leaving the world or disconnecting
returns every reservation whose transaction has not started; one whose transaction has started is
committed or returned when it finishes (the next entry loads the committed items). Two players
taking the same entry: the first request the world processes wins, the second gets `AlreadyTaken`
(or, for 0xFF, whatever is left). A container is never despawned while a reservation is open.

### 3.5 Routing

| Packet | Goes to |
|---|---|
| `ENTITY_SPAWN` / `ENTITY_DESPAWN` (type 3) | players that have the container in view (AOI) |
| `ENTITY_STATE` (loot bits) | observers whose bits change |
| `LOOT_OPEN_RES`, `LOOT_TAKE_RES` | the requester |
| `LOOT_CONTENTS` | the requester on open, every viewer on change |
| `LOOT_CLOSED` | the viewer |
| `ITEM_UPDATE`, `COINS` | the owner of the inventory |

## 4. New opcodes

| Opcode | Name | Dir |
|---|---|---|
| 0x0500 | LOOT_OPEN_REQ | C→S |
| 0x0501 | LOOT_OPEN_RES | S→C |
| 0x0502 | LOOT_CONTENTS | S→C |
| 0x0503 | LOOT_TAKE_REQ | C→S |
| 0x0504 | LOOT_TAKE_RES | S→C |
| 0x0505 | LOOT_CLOSE_REQ | C→S |
| 0x0506 | LOOT_CLOSED | S→C |
| 0x0507 | COINS | S→C |

All are valid only in `InWorld` (C→S in any other state: `ERROR(BadState)`; requests still in
flight while leaving the world are dropped silently, like `MOVE_REQ`).

```
LOOT_OPEN_REQ     u32 loot_id
LOOT_OPEN_RES     u32 loot_id
                  u8  result                // §4.1
LootEntry:
    u8  index                               // 0..7
    u16 template_id                         // >= 1
    u16 quantity                            // >= 1
LOOT_CONTENTS     u32 loot_id
                  u32 coins
                  u8  count (0..8), LootEntry[count]   // strictly ascending index
LOOT_TAKE_REQ     u32 seq                   // client-chosen, echoed back
                  u32 loot_id
                  u8  what                  // 0..7 entry index, 0xFE coins, 0xFF everything
LOOT_TAKE_RES     u32 seq
                  u32 loot_id
                  u8  result                // §4.1
LOOT_CLOSE_REQ    u32 loot_id
LOOT_CLOSED       u32 loot_id
                  u8  reason                // §4.2
COINS             u32 coins                 // balance after the change
                  u32 gained                // added by this change (0 on world entry)
```

Readers reject (protocol error): an unknown result / reason value, `LOOT_CONTENTS` with more than
8 entries, an index ≥ 8, indices not strictly ascending, template id 0 or quantity 0. `what` values
8..0xFD are **not** a protocol error: the server answers `AlreadyTaken`.

### 4.1 Loot results (`LOOT_OPEN_RES.result`, `LOOT_TAKE_RES.result`)

| Value | Name | Open | Take |
|---|---|---|---|
| 0 | Ok | ✓ | ✓ |
| 1 | NotFound | ✓ | ✓ |
| 2 | YouAreDead | ✓ | ✓ |
| 3 | OutOfRange | ✓ | ✓ |
| 4 | NotYours | ✓ | ✓ |
| 5 | AlreadyTaken | | ✓ |
| 6 | BagFull | | ✓ |
| 7 | PurseFull | | ✓ |
| 8 | ServerError | | ✓ |

### 4.2 `LOOT_CLOSED.reason`

| Value | Name |
|---|---|
| 1 | Gone (emptied or expired; `ENTITY_DESPAWN` follows) |
| 2 | OutOfRange (farther than 6 m) |
| 3 | YouDied |

### 4.3 World entry

Order on a successful `ENTER_WORLD_REQ`: `ENTER_WORLD_RES`, `INVENTORY`, **`COINS`** (gained 0),
then the world packets (`ENTITY_SPAWN`s, including loot containers in view).

### 4.4 Item moves (changes to v2)

`ITEM_MOVE_REQ` / `ITEM_MOVE_RES` / `ITEM_UPDATE` are unchanged on the wire; a merge (§1.2) is an
`Ok` move whose `ITEM_UPDATE` lists the target slot (new quantity) and the source slot (smaller
quantity, or empty). A material into an equipment slot is `WrongSlot`.

## 5. Limits

| Limit | Value |
|---|---|
| `LOOT_OPEN_REQ` + `LOOT_TAKE_REQ` + `LOOT_CLOSE_REQ` per second per client | 10 combined → `ERROR(Flood)` and close |
| Takes in flight per player | 8 (more → `LOOT_TAKE_RES(ServerError)`) |
| Entries per container | 8 |
| Loot range | 3 m + 0.5 m tolerance (horizontal) |
| Viewer auto-close distance | 6 m |
| Owner window | 30 s from creation |
| Container lifetime | 90 s from creation (or until empty) |
| Coin balance | 0 .. 1 000 000 000 |
| Coins per table roll | 0 .. 100 000 |

## 6. Persistence

Migration `db/migrations/0004_loot.sql`:

- `characters.coins bigint NOT NULL DEFAULT 0`, `CHECK (coins BETWEEN 0 AND 1000000000)`.
- `character_items.quantity` already exists (0002, `1 .. 65535`); stacks use it.

A take is one transaction: quantity updates of existing stacks (`WHERE quantity = <expected>`),
inserts of new stacks (place uniqueness as for moves), the coin increment (`RETURNING coins`, which
is what `COINS` then reports). It fails as a whole (nothing changed, `ServerError`, the units return
to the container) if the character is not a live character of the account, a stack is not as
expected, or a place is taken. A merge move is one transaction as well (two quantity updates or an
update and a delete). Loot containers themselves are **not** persisted: a server restart loses
unlooted containers. Known limit: if the database connection drops exactly during `COMMIT` (an
"in doubt" commit, never retried) the take is reported `ServerError` and its units return to the
container although the commit may have happened; the character's next entry loads the truth.

## 7. Item data (`gamedata/items.toml`, additions)

New optional keys of `[[item]]`:

| Key | Meaning |
|---|---|
| `kind = "material"` | new kind: no equipment slot; `slot`, `classes`, `level`, `attack`, `defense`, `model`, `grip` must be absent |
| `max_stack` | 1..999; default 1; weapons and shields must be 1 |
| `value` | 0..1 000 000 Akçe, informational (no vendors yet); default 0 |
| `rarity` | `"common"` (default), `"uncommon"`, `"rare"`; presentation (name colour) |

Weapons and shields still require `slot`, `classes`, `level`, `attack`, `defense`, `model`, `grip`.
Display names: kind Material "Malzeme"; rarity "Sıradan" / "Seyrek" / "Nadir".

New templates (ids 1–8 unchanged; icons are `ui/items/<icon>.vtex`, weapon models reuse existing
meshes):

| Id | Name | Kind | Slot | Classes | Level | Atk | Def | Stack | Value | Rarity | Model | Icon |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 9 | Kül Tozu | material | — | — | — | — | — | 20 | 2 | common | — | `ash_dust` |
| 10 | Cüruf Parçası | material | — | — | — | — | — | 20 | 5 | common | — | `slag_shard` |
| 11 | Kor Çekirdeği | material | — | — | — | — | — | 5 | 40 | rare | — | `ember_core` |
| 12 | Çömez Bıçağı | weapon (dagger) | either_hand | rogue | 2 | 5 | 0 | 1 | 15 | common | `dagger_hunter` | `dagger_thrall` |
| 13 | Kömürlü Kılıç | weapon (sword) | main_hand | warrior | 2 | 7 | 0 | 1 | 18 | common | `sword_soldier` | `sword_charred` |
| 14 | Cüruf Topuzu | weapon (mace) | main_hand | priest | 4 | 7 | 1 | 1 | 45 | uncommon | `mace_sun` | `mace_slag` |
| 15 | Bekçi Kalkanı | shield | off_hand | warrior, priest | 5 | 0 | 7 | 1 | 70 | uncommon | `shield_oak` | `shield_warden` |

## 8. M10 loot tables

| Monsters | Coins | Drops |
|---|---|---|
| 1, 2 Kül Çömezi | 3–9 | Kül Tozu 45 % (1–2), Çömez Bıçağı 4 %, Kömürlü Kılıç 4 % |
| 3, 4 Cüruf Yürüyen | 12–25 | Cüruf Parçası 50 % (1–2), Cüruf Topuzu 5 % |
| 5 Kor Bekçisi | 120–180 | Kor Çekirdeği 100 % (1–2), Bekçi Kalkanı 35 %, Burç Kılıcı 15 %, Kırağı Asası 15 % |

## 9. Client presentation contract (not wire format, but agreed behaviour)

- A loot container is drawn on the ground at its position as a small pouch / heap with a soft
  glow; the label shows "Ganimet" (with the monster name on hover). LootMine: gold glow; LootLocked:
  dim, tooltip "Başkasının ganimeti"; free: normal glow.
- Right click (or double click, or the interact key) on a container: if farther than 3 m walk
  there, then `LOOT_OPEN_REQ`. The loot window lists the Akçe row (if > 0) and every entry (icon,
  name in its rarity colour, "×N" when N > 1, tooltip like the inventory's); clicking a row sends
  `LOOT_TAKE_REQ` with its index (0xFE for the Akçe row); a "Hepsini Al" button sends 0xFF. The
  window closes on `LOOT_CLOSED`, Esc, or when the container despawns.
- The coin balance is shown in the inventory window ("Akçe: N") and briefly as "+N Akçe" on a gain.
- Turkish messages: NotFound "Ganimet bulunamadı", YouAreDead "Ölüyken ganimet alınmaz",
  OutOfRange "Çok uzaktasın", NotYours "Bu ganimet henüz başkasının", AlreadyTaken "Bu çoktan
  alındı", BagFull "Çantan dolu", PurseFull "Kesen dolu", ServerError "Ganimet alınamadı, tekrar
  dene".
- A missing item icon (`ui/items/<icon>.vtex` not present) is drawn as a neutral placeholder, never
  an error.
- Stacks show their quantity in the bag slot corner when > 1; dropping a stack on a stack of the
  same material merges (the server decides; the client may predict with the shared rule).
