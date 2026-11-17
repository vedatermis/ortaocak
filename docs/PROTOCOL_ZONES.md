# VedatOnline Network Protocol — Zones extension (v7)

Status: **contract for milestone M13 "Üç Yurt"** (three zones, gates between them, race homelands).
Extends docs/PROTOCOL.md, PROTOCOL_ITEMS.md (v2), PROTOCOL_COMBAT.md (v3), PROTOCOL_LOOT.md (v4),
PROTOCOL_STATS.md (v5) and PROTOCOL_SKILLS.md (v6); everything there stays valid unless this
document says otherwise. `PROTOCOL_VERSION` becomes **7** (client and server are released
together; an older client is refused with `VersionMismatch`).

Goal: the world has three zones. Each race has a homeland where its new characters start; the
village of Eşik is neutral common ground for both. A player who walks into a **gate** is moved to
the gate's arrival point in the target zone. A race's homeland is closed to the other race. The
mechanics follow the classic two-nation model; every name, text and number is our own (docs/LORE.md).

## 1. Data model

### 1.1 Zones (`gamedata/zones.toml`, shared loader `shared/game/ZoneData.h`)

| id | key | Name (banner) | Tagline | Terrain | Races | Home of |
|---|---|---|---|---|---|---|
| 1 | `esik` | **Ortaocak** | Eşik'in tarafsız köyü | `terrain/zone1/zone1.vterrain` | Aydor, Kargan | — |
| 2 | `tanyeri` | **Tanyeri** | Aydor'un sınır yurdu | `terrain/zone2/zone2.vterrain` | Aydor | Aydor |
| 3 | `demiragiz` | **Demirağız** | Kargan'ın geçit yurdu | `terrain/zone3/zone3.vterrain` | Kargan | Kargan |

```toml
[start]                 # home zone of each race: new characters are created there
aydor = 2
kargan = 3

[[zone]]
id = 1                  # 1..65535, unique (ENTER_WORLD_RES.zone_id, characters.zone_id)
key = "esik"            # [a-z0-9_]{1,32}, unique (vo_admin, logs)
name = "Ortaocak"       # UTF-8 Turkish, the banner / HUD / character card name
tagline = "Eşik'in tarafsız köyü"
terrain = "terrain/zone1/zone1.vterrain"  # relative to the compiled assets directory
races = ["aydor", "kargan"]               # who may be in this zone (non-empty)

[[zone.gate]]
marker = "gate_aydor"   # named marker in this zone's terrain: the trigger centre
radius = 2.5            # trigger radius in metres, 1..10
to_zone = 2             # target zone (must be listed, not this zone)
arrive = "arrive_esik"  # named marker in the target zone's terrain
```

The loader is strict like `skills.toml`: unknown keys, missing keys, wrong types, duplicate ids /
keys / gate markers, out-of-range numbers, unknown races, a gate to an unknown zone or to its own
zone, a gate nobody allowed in this zone may pass (no common race), a start zone that is not
listed or does not allow its race → refused with `<file>:<line>: <problem>`. At most 16 zones and
8 gates per zone. The process-wide catalog is `game::zones()` (installed by the server at startup,
loaded on first use by tools and the client), like `game::skills()`.

### 1.2 Markers (named spawn points in the `.vterrain` file)

| Zone | Markers |
|---|---|
| 1 | `spawn_aydor`, `spawn_kargan` (the Ortaocak race spawns), `gate_aydor`, `arrive_aydor`, `gate_kargan`, `arrive_kargan` |
| 2 | `spawn_aydor`, `gate_esik`, `arrive_esik` |
| 3 | `spawn_kargan`, `gate_esik`, `arrive_esik` |

Gates (shipped `zones.toml`): zone 1 `gate_aydor` → zone 2 `arrive_esik`; zone 2 `gate_esik` →
zone 1 `arrive_aydor`; zone 1 `gate_kargan` → zone 3 `arrive_esik`; zone 3 `gate_esik` → zone 1
`arrive_kargan`. So the Aydor road out of Ortaocak leads to Tanyeri and back to the same spot, and
the same for the Kargan road and Demirağız.

**Startup checks** (vo_server exit code 6 with a message naming the zone, file and marker): every
listed zone's terrain loads; it has a walkable `spawn_<race>` for every race it allows; every gate
marker exists and is walkable; every arrival marker exists in the target zone, is walkable and
lies **outside every gate trigger of that zone by at least 1 m** (`distance > radius + 1`) — so an
arrival can never fire a gate. Placement around an arrival point (§3.3) also stays outside the
triggers. Monster spawns of `monsters.toml` for zones that are not loaded are ignored with a warning.

### 1.3 Who may be where

- A zone's `races` list is enforced by the server only: a character is never placed in, nor moved
  into, a zone its race may not be in.
- **Home zone** of a race: `[start]` (Aydor → 2 Tanyeri, Kargan → 3 Demirağız). Testing hook:
  the server config key `world.start_zone_override = <id>` replaces the home zone of every race
  that may enter that zone (the UI flows and the older integration scenarios use 1, so their new
  characters start in Ortaocak as before M13). 0 = off (default).

## 2. Gates

### 2.1 Trigger

Checked by the world once per tick for every player after the tick's moves:

- The player is **inside** a gate when its horizontal distance to the gate marker is ≤ `radius`.
- A gate **fires** when the player is inside, alive, not loading (§3.4), armed (below), and its
  race may enter the target zone. No key press: walking in is enough.
- **Armed**: (a) at least **2 s** since the player appeared in this zone (world entry, zone change
  or ZONE_LOADED_REQ, whichever is latest), and (b) the player has been outside every gate radius
  since it appeared in this zone (a character that logs in standing inside a gate must step out
  and back in). Arrival points are outside the triggers (§1.2), so walking out of a gate never
  bounces the player straight back; together (a) and (b) rule out ping-pong.
- Dead players never fire (they cannot move; the gate is checked again after the respawn, which
  places them at a spawn point outside the triggers).

### 2.2 Refusal (the other race's homeland)

- A player inside a gate whose target zone does not allow its race gets `ZONE_GATE_REFUSED(to_zone,
  RaceForbidden)` — **once per stay inside the radius** and never more often than once per **3 s**
  (if the 3 s are not over when it walks in, the packet follows when they are, if it is still
  inside). Standing in the gate therefore produces exactly one packet; stepping out and in again
  produces another at most every 3 s. Nothing else happens; the player stays where it is.

### 2.3 What happens to the player's state

| State at the moment the gate fires | Rule |
|---|---|
| In combat (fighting monsters) | Allowed. The monsters of the old zone lose the player (its threat entries are dropped, they return home as when a player leaves). Damage-over-time effects the player put on old-zone monsters keep ticking and count for nobody |
| Casting | The cast ends without effect: no mana, no cooldown (casts pay at completion), no `CAST_END` to anyone (the observers get `ENTITY_DESPAWN`, the player `ZONE_CHANGE_BEGIN`) |
| Auto-attack, selected target | Cleared |
| Sitting | Arrives standing |
| Loot window open | Closed without a packet (the client closes it on `ZONE_CHANGE_BEGIN`) |
| Loot take being written | Completes normally: the units come out of the old zone's container, `ITEM_UPDATE` / `COINS` / `LOOT_TAKE_RES` may arrive during or after the change (the client applies item and coin changes and ignores the old loot id) |
| Item move, stat allocation, skill learn / bar request in flight | Unaffected (the session owns them); the result is used in the new zone |
| Effects (buffs, heal over time, shields) | Carried with their remaining time. In `EFFECT_LIST` the source of an effect the player cast on itself is its new entity id; effects cast by other players have `source_id` 0 (a heal over time from another player keeps healing and counts for nobody) |
| Skill cooldowns, global cooldown | Carried (as across a relog, PROTOCOL_SKILLS.md §1.7) |
| hp, mp, xp, level, stat points, ranks, bar, inventory, coins | Unchanged |

## 3. The zone change on the wire

### 3.1 New opcodes (block 0x0800)

| Opcode | Name | Dir |
|---|---|---|
| 0x0800 | ZONE_CHANGE_BEGIN | S→C |
| 0x0801 | ZONE_CHANGE | S→C |
| 0x0802 | ZONE_LOADED_REQ | C→S |
| 0x0803 | ZONE_GATE_REFUSED | S→C |

### 3.2 Layouts (little-endian, as everywhere)

```
ZONE_CHANGE_BEGIN  u16 from_zone
                   u16 to_zone

ZONE_CHANGE        u8  result            // 0 Ok: in to_zone · 1 Failed: back in from_zone (§3.5)
                   u16 from_zone
                   // then exactly the body of ENTER_WORLD_RES after its result byte
                   // (PROTOCOL.md §5.3 + PROTOCOL_COMBAT.md §2), always present:
                   u32 entity_id         // a NEW entity id
                   u16 zone_id           // the zone the player is in now
                   f32 x, f32 y, f32 z, f32 yaw
                   f32 walk_speed, f32 run_speed
                   u32 server_time_ms
                   u16 level
                   u32 xp, u32 xp_to_next
                   u32 hp, u32 max_hp, u32 mp, u32 max_mp
                   u16 attack, u16 defense
                   u8  is_dead           // always 0

ZONE_LOADED_REQ    u16 zone_id           // the zone_id of the last ZONE_CHANGE

ZONE_GATE_REFUSED  u16 to_zone           // the zone behind the gate
                   u8  reason            // 1 RaceForbidden
```

Sizes: ZONE_CHANGE_BEGIN 4 bytes, ZONE_CHANGE 68, ZONE_LOADED_REQ 2, ZONE_GATE_REFUSED 3. Decoders
refuse unknown `result` / `reason` values, non-finite floats, `is_dead` other than 0/1 and trailing
bytes (protocol violation for ZONE_LOADED_REQ).

### 3.3 Sequence

To the player who changes zone:

1. Its last batch of the old zone (whatever the old zone produced for it in that tick).
2. **`ZONE_CHANGE_BEGIN(from, to)`** — in the tick the gate fired. From here on nothing of the old
   zone reaches the client (no `ENTITY_DESPAWN`s: the client drops every entity itself, as on
   LEAVE_WORLD_RES), and nothing of the new one yet.
3. The server stores the new zone and the arrival position (§3.5). Typically 1–3 ticks.
4. **`ZONE_CHANGE(Ok, …)`**, then exactly what a fresh world entry gets after `ENTER_WORLD_RES`:
   `INVENTORY`, `COINS`, `SKILL_STATE`, `SKILL_BAR` (from the session), then the new zone's first
   batch: `CHAR_STATS`, `SKILL_COOLDOWNS` (if any are running), **`EFFECT_LIST` about the player
   itself** (if it has effects; new in v7, also sent on a world entry that carries effects — today
   only zone changes do), the `ENTITY_SPAWN`s of everything in range (each followed by its
   `EFFECT_LIST` if it has effects), then the normal stream.
5. The client loads the zone and sends **`ZONE_LOADED_REQ(zone_id)`**. From then on it may move.

To the others: the players of the old zone that saw it get `ENTITY_DESPAWN` in step 2's tick;
players of the new zone in range get its `ENTITY_SPAWN` in step 4's tick (with its `EFFECT_LIST`).
An entity of one zone is never sent to a player of another zone.

Placement at the arrival point: like a spawn point — spread 0.5–`world.spawn_spread_m` (2 m) away
along a walkable straight path, never inside a gate trigger; yaw = the marker's yaw.

### 3.4 Loading: what the client may send

- After `ZONE_CHANGE_BEGIN` and until it sent `ZONE_LOADED_REQ`, the client sends **no**
  `MOVE_REQ`, combat (`TARGET_REQ`, `ATTACK_REQ`, `ATTACK_STOP_REQ`, `SKILL_REQ`, `RESPAWN_REQ`,
  `STANCE_REQ`) or loot requests. Ones that were already in flight are dropped by the server
  silently (they still count against their flood caps); requests about old-zone entities may go
  unanswered, so the client forgets pending loot / target state on `ZONE_CHANGE_BEGIN`.
- `ITEM_MOVE_REQ`, `STAT_ALLOCATE_REQ`, `SKILL_LEARN_REQ`, `SKILL_BAR_SET_REQ`, `PING`,
  `LEAVE_WORLD_REQ` and `LOGOUT` stay valid all the time.
- `ZONE_LOADED_REQ` is valid only once per `ZONE_CHANGE` and only after it (otherwise
  `ERROR(BadState)`); a `zone_id` other than the last `ZONE_CHANGE.zone_id` is a protocol
  violation. One still in flight while leaving the world is dropped silently.
- **`MOVE_REQ.seq` restarts** after `ZONE_LOADED_REQ`: the first MOVE_REQ of the new zone may carry
  any seq; later ones must increase as usual.
- While loading, the player cannot be attacked or pulled by monsters, does not fire gates and its
  moves are ignored. This protection ends at `ZONE_LOADED_REQ`, or after **20 s** without it (the
  client then still cannot move until it sends `ZONE_LOADED_REQ`).

### 3.5 Persistence and failure

- The arrival (zone id, position, and the progress: level, xp, hp, mp) is written with the normal
  state save **before** the player appears in the new zone: `ZONE_CHANGE(Ok)` is sent only after the
  write committed. A character is one row (`characters.zone_id` + position), so it is always in
  exactly one zone: a crash before the commit leaves it at its last saved place in the old zone, a
  crash after it at the arrival point. Nothing is lost or duplicated.
- If the write fails, the player is put back into the old zone where it stood (inside the gate,
  which is disarmed until it steps out): `ZONE_CHANGE(Failed, …)` with `zone_id = from_zone`, then
  the entry packets as for Ok. The client reloads (or keeps) that zone and sends `ZONE_LOADED_REQ`.
- `LEAVE_WORLD_REQ`, `LOGOUT` or a disconnect while the write is in flight: the leave waits for it;
  the character is stored at the arrival point (or, if the write failed, where it stood).
  `LEAVE_WORLD_RES` follows as usual.
- Periodic saves, leaves and logouts store the zone the player is in (as before).

## 4. Placement, respawn

- **World entry**: the saved position is used if its zone is loaded **and allows the character's
  race**, and the position is finite, inside the zone and walkable (y snapped). Otherwise the
  character goes to the race spawn of its **home zone**. Existing characters (all in zone 1) keep
  their saved zone and position.
- **New characters** are created at the race spawn of their home zone (`spawn_aydor` of zone 2,
  `spawn_kargan` of zone 3), level 1; CHAR_CREATE_RES's `CharSummary.zone_id` says which.
- **Logged out dead**: the race spawn of the zone it was saved in (50 % hp/mp, as before).
- **Respawn** (RESPAWN_REQ): the race spawn of the zone the player died in. In a homeland that is
  the home spawn (Tanyeri's `spawn_aydor`, Demirağız's `spawn_kargan`); in Eşik the Ortaocak race
  spawn (`spawn_aydor` / `spawn_kargan`), as before M13.

## 5. Limits

| Limit | Value |
|---|---|
| Zones | ≤ 16 (data), ids 1..65535 |
| Gates per zone | ≤ 8 |
| Gate radius | 1..10 m (shipped 2.5 m) |
| Re-arm after appearing in a zone | 2 s, plus "has been outside every gate" |
| ZONE_GATE_REFUSED | once per stay inside, ≥ 3 s apart |
| Loading protection | until ZONE_LOADED_REQ, at most 20 s |
| ZONE_LOADED_REQ | once per ZONE_CHANGE (not part of any flood cap; a second one is BadState) |

## 6. Persistence (database)

No migration: `characters.zone_id` exists since 0001 (`CHECK (zone_id BETWEEN 0 AND 65535)`), and
the state save writes it already. New characters get the zone id of their home zone.

### 6.1 vo_admin

- `vo_admin show-character <name>` prints the zone as `zone 2 (Tanyeri)` (unknown ids: `zone 7
  (unknown)`).
- `vo_admin move-character <name> <zone> [marker]`: moves an **offline** character to a marker of a
  zone (`<zone>` = id or key; default marker `spawn_<race>`), y on the terrain, after checking that
  the zone allows the character's race and the marker is walkable. Refused while the character's
  account is online (it would be overwritten by the next save).

## 7. Client spec (presentation contract, not wire format)

The zone names and taglines come from `zones.toml` (`game::zones()`); the texts below are Turkish
and fixed. `<Bölge>` is the zone's `name`.

### 7.1 Loading

- On `ZONE_CHANGE_BEGIN`: stop moving at once (no more MOVE_REQ), close the loot window, cancel
  auto-attack / target display, show the loading screen with the target zone's name as title and
  the line **"Geçitten geçiliyor…"**.
- On `ZONE_CHANGE`: drop every entity and effect, unload the old terrain, load the new one
  (`terrain` of the zone in `zones.toml`; the same path pattern `terrain/zone<id>/zone<id>.vterrain`
  as world entry), line **"<Bölge> yükleniyor…"** (the same as at world entry). Packets that arrive
  while loading are applied after it (as at world entry). Then send `ZONE_LOADED_REQ` and show the
  world.
- The simplest correct client treats `ZONE_CHANGE` as a fresh world entry (rebuild the world scene
  with the new ENTER_WORLD_RES body).

### 7.2 Zone banner

Shown for 4 s, fading, at the top centre whenever the player appears in a zone (world entry and
every zone change): the name large, the tagline below it.

| Zone | Banner |
|---|---|
| 1 | **Ortaocak** — Eşik'in tarafsız köyü |
| 2 | **Tanyeri** — Aydor'un sınır yurdu |
| 3 | **Demirağız** — Kargan'ın geçit yurdu |

The HUD's top-right zone label shows the name (as today for Ortaocak).

### 7.3 Gates

- A label floats above every gate marker within 40 m: **"<target Bölge> Geçidi"** (e.g.
  "Tanyeri Geçidi").
- Within 8 m of a gate the player may pass, a hint near the bottom: **"Geçide yürü: <target Bölge>"**.
- Within 8 m of a gate closed to its race: **"<target Bölge> yalnızca <ırk> açık."** (below).

### 7.4 Refusal and failure texts

| Event | Text |
|---|---|
| ZONE_GATE_REFUSED RaceForbidden, target allows only Aydor | "Tanyeri yalnızca Aydorlulara açık." (with the target's name) |
| ZONE_GATE_REFUSED RaceForbidden, target allows only Kargan | "Demirağız yalnızca Karganlara açık." (with the target's name) |
| ZONE_CHANGE Failed | "Geçit şu an kullanılamıyor, tekrar dene." |
| arrival (system line in the combat log) | "<Bölge> bölgesine geçtin." |

`<ırk>` for a zone open to one race: Aydor → "Aydorlulara", Kargan → "Karganlara".

### 7.5 Character select

The character card's zone line uses the zone name (`zones.toml`), e.g. "Savaşçı · Tanyeri".
