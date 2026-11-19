# VedatOnline Server World Simulation (M6)

Status: M6 server half, plus the M8 item server half (§8a), the M9 "İlk Kan" combat server half
(§8b) and the M10 "Ganimet" loot server half (§8c), M11 stats (§8d), M12 skills (§8e) and the M13
"Üç Yurt" zones and gates (§8f, docs/PROTOCOL_ZONES.md, v7). Implements docs/PROTOCOL.md §4 and §5.4:
authoritative movement, area of interest, entity replication and position persistence;
docs/PROTOCOL_ITEMS.md; docs/PROTOCOL_COMBAT.md; and docs/PROTOCOL_LOOT.md (protocol v4). Code:
`server/world/` (`ZoneCombat.cpp` for monsters and combat, `ZoneLoot.cpp` for loot containers),
`shared/game/Movement.h`, `shared/game/TerrainWalk.h`, `shared/game/Items.h`,
`shared/game/Combat.h`, `shared/game/Monsters.h`, `shared/game/Loot.h`; network glue in
`server/net/Session.cpp`.

## 1. Architecture

```
 network threads (net.io_threads)      world thread (World)                  DB workers
 ─────────────────────                 ────────────────────                  ──────────
 Session ── ENTER/MOVE/LEAVE ──post──▶ command queue (mutex, swapped/tick)
                                       tick: commands → Zone(s) → flush
 Session ◀──post── SessionLink ◀────── WorldBatch per player per tick
                                       position saves ──────────────────────▶ IAccountService
 Session ◀──post──────────────────────────────────────────── save done (LEAVE_WORLD_RES / logout)
```

| Piece | File | Job |
|---|---|---|
| `World` | `server/world/World.h` | owns zones, the command queue, the 20 Hz loop on its own thread, session→zone and character→session maps, the "recently left" cache, tick statistics |
| `Connection` | `shared/net/Connection.h` | framed/encrypted TCP stream with coalesced writes, shared by server and client (§9) |
| `Zone` | `server/world/Zone.h` | terrain (`.vterrain`), players, AOI grid, placement, validation, spawn/despawn pairs, relay, corrections, periodic saves |
| `AoiGrid` | `server/world/AoiGrid.h` | 32 m cells, 5×5 view block, view-change enumeration |
| `MoveValidator` | `server/world/MoveValidator.h` | §5.4 rules + the distance budget (§4) |
| `MoveRateLimiter` | `server/world/MoveRateLimiter.h` | 30 MOVE_REQ/s hard cap (sliding 1 s window) |
| `TickStats` | `server/world/TickStats.h` | tick duration histogram, avg/p50/p99/max, slow ticks |
| `WorldTypes` | `server/world/WorldTypes.h` | `EnterCommand`/`MoveCommand`/`LeaveCommand`, `WorldBatch`, `IPlayerLink` |
| shared rules | `shared/game/Movement.h`, `TerrainWalk.h` | speeds, tolerances, rates, AOI constants, yaw normalisation, segment walkability (used by server and bots; usable by the client) |

## 2. Threading contract

- **One dedicated simulation thread** (`World::start()`), not an asio strand: all world state (zones,
  entities, AOI, known sets, maps) is owned by that thread and never locked. Before `start()` and
  after `stop()` the owner may call `World::tick()` itself (unit tests do; `stop()` does for the
  final drain).
- **Inbound**: the network thread never touches world state. `Session` posts commands with
  `World::post()` (mutex-protected vector, swapped out once per tick, executed in posting order).
  `MOVE_REQ` carries its receive timestamp from the network thread.
- **Outbound**: at the end of each tick every player with something to receive gets exactly one
  `IPlayerLink::deliver(WorldBatch)`. `SessionLink` posts the batch onto the session's executor
  (one `asio::post` per player per tick); the session sends the packets there. Batches carry the
  session's world generation; stale ones (after a leave) are dropped.
- **DB**: the world submits `saveCharacterPosition` (thread-safe service) from its thread. Completions
  run on DB worker threads and only log, or call the `LeaveCommand::done` callback, which the session
  wraps to post back onto its own executor. The one DB completion that reaches the world is the end
  of a loot take (§8c): it is *posted* as a `LootSettleCommand` (`World::post` is thread-safe), never
  run on the world's state directly; the account service is stopped before the world is destroyed,
  so the post is always safe (after `World::stop()` it is simply refused).
- Character loads for ENTER_WORLD happen in the session (DB worker → session executor) *before* the
  world sees the character.

## 3. Tick pipeline

Fixed timestep `1 / world.tick_rate` (20 Hz → 50 ms), absolute scheduling (`next += interval`), so
there is no drift. On Windows the thread sleeps on a high-resolution waitable timer (≈0.5 ms jitter
without changing the process timer resolution).

0. Every zone gets the tick's scheduled time (`Zone::beginTick`): the one clock of combat.
1. **Commands** in posting order: enter (placement, ENTER info, spawn pairs), move (validation,
   AOI cell change), equip, combat requests (§8b), loot requests and settles (§8c), leave (despawn,
   save, cancel the session's loot takes that were never written).
2. **Per zone** `update()`: one `POSITION_CORRECT` per player that had a rejected move this tick;
   combat (casts, swings, regeneration, monster AI and timers, then the tick's ENTITY_STATE /
   SELF_STATS / ENTITY_STATS); loot (owner windows ending, viewers out of range or dead,
   LOOT_CONTENTS of changed containers, despawns of emptied / expired ones); `ENTITY_MOVE` relay of
   players and monsters; saves due in this tick (periodic, and level-ups at once).
3. **Flush**: hand every non-empty batch to its link.

Overload handling: a late tick runs immediately (catch-up). If the loop is more than
`world.max_catch_up_ticks` (5) intervals behind, it skips ahead and logs a warning with the number
of skipped ticks. Every tick duration goes into `TickStats`; ticks longer than the budget are counted
and logged (rate-limited to one warning per 5 s with the count). Every `world.stats_interval_s` the
world logs players, tick avg/p99/max, slow/skipped ticks, moves accepted/rejected per second, relays
per second, spawns/despawns and saves; at shutdown it logs totals since start
(`world: totals: ...`).

Relay rate limiting uses the *scheduled* tick time (exactly 50 ms apart), so "every second tick" is
exact and timer jitter cannot drop a relay to 6.7 Hz.

## 4. Movement validation policy

Order of checks for each `MOVE_REQ` (network thread first, then world thread):

| Check | Where | Failure |
|---|---|---|
| more than 30 MOVE_REQ in any 1 s window (receive time) | session | `ERROR(Flood)` + close |
| decodes; finite x/y/z/yaw; \|x\|,\|y\|,\|z\| ≤ 100 km; \|yaw\| ≤ 10000; no unknown flag bits | session | `ERROR(ProtocolViolation)` + close |
| `seq` strictly greater than the previous MOVE_REQ of this world session | session | `ERROR(ProtocolViolation)` + close |
| inside the zone `[0, 512)` | world | correction |
| distance budget (below) | world | correction |
| target walk-mask cell walkable (slopes, ridges, margin, object footprints are baked into the mask) | world | correction |
| every mask cell on the straight segment old → new walkable (exact grid traversal; passing exactly through a corner needs both side cells open) | world | correction |
| \|y − terrain(x, z)\| ≤ 1 m | world | correction |

Accepted: position = (x, terrain(x, z), z) — y is snapped —, yaw normalised to (−π, π], flags as
sent. Rejected: the position stays, the moving bit is cleared (the player stands at the corrected
position; observers are told so), and one `POSITION_CORRECT` per tick goes to the client.

**Distance budget.** The spec bound is `speed × elapsed × 1.25 + 0.5 m` with *server-side* elapsed
time since the last accepted move. Applied literally per packet it has two holes: (1) elapsed time
can be banked (stand still 60 s, jump 400 m), and (2) the 0.5 m is granted per packet, so 30
packets/s of 0.6 m each are 4× run speed. Applied literally it also has a false positive: a client
whose packets were delayed (TCP stall) delivers them in a burst with ~0 ms between them and every
packet after the first is rejected.

The validator therefore keeps a per-player *credit* in metres:

```
on every processed MOVE_REQ at receive time t:
    speed  = running bit ? run_speed : walk_speed
    credit = min(credit + speed × (t − t_prev) × 1.25,  speed × world.move_bank_seconds × 1.25)
    t_prev = t
    accept iff horizontal_distance ≤ credit + 0.5
    on accept: credit = max(credit − distance, −0.5)
```

- For one move from rest this *is* the §5.4 formula (as long as elapsed ≤ the bank, 1 s by default).
- Unused allowance carries over, so a burst of delayed packets is accepted exactly like the same
  packets arriving on time — up to `move_bank_seconds` of backlog (1 s → 6.9 m running).
- It is capped, so idling cannot be turned into a teleport (max instant jump ≈ 7.4 m running,
  1.75 + 0.5 m walking at 1.4 m/s), and the 0.5 m slack is a one-off tolerance: the long-run speed can never
  exceed 1.25 × speed, however many packets are sent.
- Rejections do not consume credit; time keeps accruing.

The 30/s cap uses receive times too: a client stalled for more than 3 s at 10 Hz that then delivers
more than 30 queued packets at once is flood-kicked. That is inherent to a hard per-second cap.

## 5. Area of interest

- 32 m cells (16 × 16 for zone 1); A sees B iff their cells are within Chebyshev distance 2 (5×5
  block, ≥ 64 m view radius). Visibility depends only on the two cells, so it is symmetric.
- Every player has a `known` set = entities its client has spawned. Invariant (property-tested
  against a brute-force oracle under random walks, teleports, enters and leaves):
  `B ∈ known(A) ⇔ A ∈ known(B) ⇔ cells within 2`. Spawns are only sent for unknown entities and
  despawns only for known ones, so pairs are never duplicated.
- Enter: the newcomer is placed, then for each entity in its 5×5 block a spawn *pair* is created
  (newcomer gets `ENTITY_SPAWN(other)`, the other gets `ENTITY_SPAWN(newcomer)`).
- Cell change on an accepted move: entities in cells that left the block get despawn pairs, in cells
  that entered it spawn pairs.
- Leave/disconnect/kick: every observer gets `ENTITY_DESPAWN`; the leaver gets nothing (its client
  drops everything on LEAVE_WORLD_RES / disconnect).

## 6. Replication (what the client sees)

- **ENTER_WORLD_RES first.** The world's first batch for a session completes ENTER_WORLD_REQ: the
  session sends `ENTER_WORLD_RES` (real position, y on the terrain, walk 1.4 / run 5.5 m/s,
  server_time_ms), then, from the same batch, `ENTITY_SPAWN` for everyone already in range. Others
  get the newcomer's `ENTITY_SPAWN` in the same tick.
- Own entity: never spawned to itself, own moves are never echoed.
- **ENTITY_MOVE**: per entity at most once per 100 ms (every second tick), only to players that have
  the entity spawned, carrying the *latest* accepted state (intermediate states are dropped).
  `server_time_ms` is the time the server *received* that MOVE_REQ (ms since server start, the same
  clock as PONG and ENTER_WORLD_RES), so remote clients interpolate with correct spacing. Unchanged
  states are not resent: a standing player produces no traffic.
- **Stop**: the state of a stop packet (moving = 0, final position) is always relayed, at the next
  allowed tick, i.e. at most 100 ms later; a start → move → stop within one interval still ends with
  the stop state. After a rejected move observers get a stop at the corrected position, so remote
  avatars neither drift nor freeze mid-stride.
- **POSITION_CORRECT**: at most one per tick, when at least one MOVE_REQ of the tick was rejected;
  `seq` = the last MOVE_REQ seq processed in that tick (may be a later accepted one), position and
  yaw = authoritative. Moves the client sent before it received the correction will usually be
  rejected as well and produce further corrections with higher seq; applying each is idempotent.
  Accepted moves are not acknowledged.

## 7. Placement and spawns

- Saved position used if its zone is loaded, it is finite, inside the zone and walkable; y is
  snapped. Otherwise: the race spawn of the start zone (`spawn_aydor` race 1, `spawn_kargan` race 2).
  Since M13 (§8f): the zone must also allow the character's race, and the fallback is the race
  spawn of the race's **home zone**; new characters start there too.
- Players placed at a spawn point — fallback, or a saved position exactly equal to it (never moved
  since creation) — are spread to a random walkable point 0.5–`world.spawn_spread_m` (2 m) away
  with a walkable straight path, so simultaneous spawners do not stack.
- New characters are created at their race spawn: the account service receives
  `World::newCharacterSpawns()` (from the zone file) at startup; `auth::kNewCharacterSpawn` is only
  the fallback for services constructed without zone data (unit tests).
- Startup fails with exit code 6 and a clear message if the terrain is missing or corrupt, or lacks a
  walkable `spawn_aydor` / `spawn_kargan`.

## 8. Persistence cadence

| When | What |
|---|---|
| LEAVE_WORLD_REQ | despawn, save; `LEAVE_WORLD_RES` after the save completed → CharSelect |
| LOGOUT in world | despawn, save; close after the save completed |
| disconnect / kick (incl. duplicate login) | despawn, save (fire and forget) |
| every `world.save_interval_s` (60 s) in world | save if the position changed since the last save; each player has its own slot in the period (hash of the character id), so saves are spread over all ticks |
| shutdown | `Server::shutdown()` kicks everyone (each kick saves); `World::stop()` then runs what is still queued and saves whoever is left; `IAccountService::stop()` drains the DB queue. Bounded: sessions get 5 s, the world one tick, the DB its statement timeout |

Leaving (PROTOCOL §5.4): MOVE_REQ that arrive while a LEAVE_WORLD_REQ / LOGOUT is being processed,
and in CharSelect within `kLeaveGrace` (2 s) after LEAVE_WORLD_RES, were sent before the client
knew it had left: the session drops them silently (they still count against the 30/s cap) instead
of queueing them behind the leave and answering ERROR(BadState) afterwards (M7 fix; before, a
client that was walking when it pressed "Karakter Seçimine Dön" was kicked). Later MOVE_REQ in
CharSelect are BadState as before.

Ordering and freshness:
- The world is the only writer of positions. Requests of one account run in order in the account
  service (see docs/DATABASE.md), and a save is always *submitted* before the session can send the
  next ENTER_WORLD_REQ.
- Duplicate login: the new session may load the character before the kicked session's leave has
  been processed. If the character is still in the world, the new entity takes over the old one's
  live position (and the old entity is removed without a save). If it left within the last 60 s,
  its last in-world position is used even if the asynchronous save has not landed yet.

## 8a. Items (M8 "Kuşan", docs/PROTOCOL_ITEMS.md)

Rules and data are shared with the client: `shared/game/Items.h` loads `gamedata/items.toml`
(located like `config/` and `assets/`: upwards from the working directory, then the executable
directory; `world.items_file` overrides) with strict validation, and holds the inventory model and
the pure placement rules (`planItemMove` / `applyItemMove`). vo_server refuses to start (exit 6) if
the file is missing or invalid. The catalog is immutable and shared by all network threads
(`ServerContext::items`).

**Who owns what.** The *session* owns its character's inventory (network thread); the *world* only
knows the two visible template ids per player (`CharacterInfo::mainHand/offHandTemplate`), which it
puts into `ENTITY_SPAWN`. The database is reached only through the account service's workers, never
from the world thread.

**Entering.** `ENTER_WORLD_REQ` → `loadCharacterForWorld` → `loadCharacterItems(kit of the class)`
(grants the kit in the same transaction as the flag if not granted yet) → the session builds the
inventory (unknown templates are kept but hidden and logged once per item) and posts the
`EnterCommand` with the visible equipment. The world's first batch completes the request: the
session sends `ENTER_WORLD_RES`, then `INVENTORY` (every visible occupied slot, equipment first),
then the batch (`ENTITY_SPAWN`s carry everyone's visible equipment).

**Moves.** `ITEM_MOVE_REQ` (InWorld only; more than 10 per second → `ERROR(Flood)`; malformed →
ProtocolViolation) goes into a per-session queue (≤ 16 waiting, more are answered ServerError).
The session decides **one move at a time** against its *committed* inventory:
1. `planItemMove` → a failure is answered with `ITEM_MOVE_RES(result)` at once, nothing else;
2. an Ok plan's relocations (item id, expected old place, new place) go to
   `IAccountService::applyItemMove` (one transaction, deferred place uniqueness); the next request
   waits;
3. on commit: apply the plan to the inventory, send `ITEM_UPDATE` (every changed slot) then
   `ITEM_MOVE_RES(Ok)`; if MainHand/OffHand changed, post `EquipCommand` → the world sends
   `ENTITY_EQUIP` to the mover and to every player that has it in view (its known set), next tick;
4. on a failed write: `ITEM_MOVE_RES(ServerError)`, the inventory is unchanged.
So two rapid moves on the same slots are applied in order, the second one is decided on the state
after the first commit, and nothing is acknowledged before it is stored. Requests of one account
run in order in the account service, so a move still in flight when the player leaves is committed
before the next entry loads the items (its late completion is ignored by the session). Leaving
drops requests not decided yet.

## 8b. Combat (M9 "İlk Kan", docs/PROTOCOL_COMBAT.md)

**Data and rules.** `shared/game/Combat.h` holds the class tables, stat derivation (level +
Σ attack/defense of everything in the Equipment container), the damage / critical / experience
formulas (pure, the caller injects the rolls), the four skills and the level curve; the client
uses the same header for its numbers. `shared/game/Monsters.h` loads `gamedata/monsters.toml`
(located like items.toml; `world.monsters_file` overrides) with the strictness of the item loader:
templates (name, model, scale, level, hp, attack, defense, attack range/interval, chase and wander
speed, aggro and leash radius, xp, respawn), `[[safe_zone]]` circles and `[[spawn]]` points (zone,
monster, position, wander radius, count). vo_server refuses to start (exit 6) if the file is
missing or invalid, or a spawn centre is not walkable on its zone. Numbers (level 1 → 10):

| Class | hp | mp | attack (+kit) | defense (+kit) | range | interval | crit | skill |
|---|---|---|---|---|---|---|---|---|
| Savaşçı | 90 → 270 | 34 → 70 | 12+6 → 30+6 | 3+4 → 12+4 | 2.5 m | 1600 ms | 5 % | Ağır Darbe |
| Haydut | 98 → 260 | 35 → 80 | 10+8 → 28+8 | 2 → 11 | 2.5 m | 1200 ms | 15 % | Çifte Kesik |
| Büyücü | 90 → 216 | 70 → 160 | 11+7 → 29+7 | 2 → 11 | 14 m | 1800 ms | 5 % | Köz Oku |
| Şifacı | 95 → 248 | 64 → 145 | 11+5 → 29+5 | 3 → 12 | 2.5 m | 1700 ms | 5 % | Soluk Güneş |

Zone 1 (`gamedata/monsters.toml`): safe zone 40 m around the plaza (256/256); four loose packs of
Kül Çömezi (lvl 1: 80 hp, attack 11, defense 4, xp 25; lvl 2: 105/14/5, xp 32; aggro 6 m) 53–67 m
out in the NW, W, S and SE, members 12–14 m apart; Cüruf Yürüyen pairs (lvl 3/4: 220/260 hp,
18/21 attack, slow) 110–120 m out W, SW, SE; one Kor Bekçisi (lvl 6 elite: 900 hp, attack 38,
defense 14, xp 300, respawn 90 s) at 224/325 on the plateau's north-west edge. The north-east road
(the `world_tour` flow down to 334/338) is kept free; no idle monster notices anyone closer than
44 m to the plaza. A level-1 starter-kit character kills a level-1 thrall in 5 average hits (≈7 s for
a warrior) and survives 9–12 of its hits; levels 1→3 need 320 xp (≈11 thralls, a few minutes).

**Monsters in the world.** Monsters are entities of the zone (`Zone::Monster`, ids from the
world's allocator) in the same AOI grid as players. Each monster keeps the set of players that have
it spawned (`observers`); the invariant M ∈ known(P) ⇔ P ∈ observers(M) ⇔ in view is tested with
the players' one. They move with `ENTITY_MOVE` at ≤ 10 Hz like players (moving bit while walking,
running bit while chasing / returning), only along walkable segments: a straight line when
`isSegmentWalkable`, otherwise a bounded grid search (`findWalkPath`, ≤ 6000 cells, at most one per
second per monster); a step that would enter a safe zone is not taken.

AI state machine (per tick, all on `m_now`):

| State | Does | Leaves to |
|---|---|---|
| Idle | wanders to random points of its circle (walkable straight segment, outside safe zones) every 4–9 s; scans its observers every 200 ms; does nothing while nobody sees it | Chase: an alive player outside the safe zones within aggro radius and the leash, or any damage |
| Chase | target = highest threat (damage dealt; healing a player it fights counts ×0.5 for the healer); invalid threats (left, dead, in a safe zone, out of view, farther than the leash from the spawn) are dropped; walks into range, faces the target, attacks every interval (first attack 400 ms after the pull) | Return: no valid threat, itself beyond the leash, or no progress for 6 s |
| Return | heals to full at once (ENTITY_STATS), walks home, ignores damage (swings do nothing, skills `NotAttackable`); placed home if it cannot walk there for 10 s | Idle at its spawn point |
| Dead | corpse, state bit dead | Gone after 3 s (ENTITY_DESPAWN) |
| Gone | not in the grid | Idle at a random point of its circle `respawn_ms` after the death (ENTITY_SPAWN) |

**Players.** `Zone::PlayerCombat`: stats, hp/mp/xp, dead + time of death, sitting, selected
target, auto-attack target with the one-time OutOfRange / InSafeZone notices, next swing, global
cooldown (500 ms, shared by swings and skills), per-skill ready times, the cast in progress, the
time of the last damage dealt or taken (in combat for 6 s), regeneration accumulators, the last
state bits told and the rate limits of SELF_STATS (≤ 4 Hz) and ENTITY_STATS (≤ 2 Hz). Rules as in
the contract; the server's choices where it leaves room:

- ATTACK_REQ is accepted for any spawned, living monster (players: `NotAttackable`, unknown id or
  not spawned to the requester: `NoTarget`); range and the safe zone are checked at every swing
  (attacker or target inside a safe zone: one `InSafeZone` notice, no swing). A new ATTACK_REQ
  re-arms the notices but never shortens the swing interval. An auto-attack ends at once when its
  target dies (or despawns from view).
- Skills are refused in this order: `UnknownSkill` (not the class's), `YouAreDead`, `Busy` (casting),
  `OnCooldown` (global or own), `NotEnoughMp`, then the target (`NoTarget`, `NotAttackable`,
  `WrongTargetType`, `TargetDead`), `OutOfRange` (range + 0.5 m), `InSafeZone` (damage skills only;
  healing is allowed everywhere). Instant skills cost and start their cooldown at once; casts at
  completion, where the target is checked again (failure: CAST_END interrupted + ACTION_FAIL). An
  accepted MOVE_REQ that moves (moving bit, or more than 5 cm) interrupts a cast. Köz Oku's damage
  is applied at cast end (the projectile is presentation).
- Damage is reported as applied (never more than the remaining hp), so the amounts of all hits on a
  monster add up to its hp. A kill: ENTITY_DEATH (killer = the killing blow's player), XP to the
  player with the most damage (`xpForKill`), LEVEL_UP to it and its observers on a level-up, which
  also refills hp/mp and is saved in the same tick.
- Death: auto-attack and cast end, the player stops (observers see the stop); MOVE_REQ is answered
  with POSITION_CORRECT (`MoveVerdict::Dead`), attacks and skills with `YouAreDead`, sitting with
  `Busy`. RESPAWN_REQ: `NotDead` / `TooEarly` (< 5 s) / Ok: ENTITY_STATE (dead cleared) to the
  observers, the player moves to its race spawn (spread like an entry, AOI pairs as for any move),
  then RESPAWN_RES and an ENTITY_MOVE to whoever still sees it. No experience loss.
- Stance: sitting is refused (`Busy`) while dead, moving (current moving bit), casting or in combat;
  a MOVE_REQ with the moving bit, any ATTACK_REQ / SKILL_REQ / swing and taking damage stand up.
  Regeneration (out of combat, every second) is doubled while sitting.
- Routing (§3.3): every combat packet is encoded once and appended to the source and target players
  and every player that has one of them spawned, each recipient once.

**Session side.** ATTACK_REQ / SKILL_REQ / TARGET_REQ / STANCE_REQ share one 10/s sliding window
(more → ERROR(Flood)); ATTACK_STOP_REQ and RESPAWN_REQ are not counted. Malformed payloads (e.g.
stance 2) are protocol violations; combat requests outside InWorld are BadState, except those in
flight while leaving the world, which are dropped like MOVE_REQ (§8). The session computes the
equipment bonus with the shared rules and posts it with the entry and with every committed move
that changes the visible equipment or a bonus (`EquipCommand`); the world recomputes the stats and
sends SELF_STATS. The world tells the session in its batches when the character dies / respawns
(item moves are refused with `SlotBlocked` while dead) and its level (item level requirements).

**Persistence (§3.5).** Saves carry the position and the progress (level, xp, hp, mp; hp 0 = dead):
`IAccountService::saveCharacterState`, at the same cadence as positions (periodic only if something
changed, leave, logout, disconnect, shutdown) and at once on a level-up. Entry: stored hp/mp NULL
(new or pre-M9 characters) = full; hp 0 = left dead → race spawn with 50 % hp/mp; values are clamped
to the current maxima. The recently-left cache (§8) also holds the progress; a stored level above
the cached one wins (vo_admin `set-level` right after a logout).

**Cost.** Idle monsters nobody sees do nothing; aggro scans run every 200 ms over the observers only;
grid searches are bounded and rate-limited (`world: totals:` reports combat events, kills, deaths
and path searches). Measured with 200 bots (40 of them fighting): see §11.

## 8c. Loot (M10 "Ganimet", docs/PROTOCOL_LOOT.md)

**Data and rules.** `shared/game/Items.h` gained materials (`kind = "material"`, no equipment
slot), `max_stack`, `value` and `rarity`, the merge rule of a bag move onto a non-full stack of the
same template (`ItemMovePlan::merge`), and `planItemAdd` (units go into existing stacks first,
lowest slot first, then into the lowest free bag slots; used by loot pickups and `vo_admin
give-item`). `shared/game/Loot.h` loads `gamedata/loot.toml` (located like monsters.toml;
`world.loot_file` overrides; vo_server refuses to start, exit 6, if it is missing or invalid; the
monster ids are checked against monsters.toml and the items against items.toml) and rolls a table
with the zone's generator (`rollLoot`, pure, the RNG is injected).

**Containers in the world.** `Zone::Loot` (`ZoneLoot.cpp`): an entity in the same AOI grid as
players and monsters (ids from the world's allocator), with `observers` (players that have it
spawned; the same invariant as for monsters) and `viewers` (players that have it open; each player
views at most one). `killMonster` determines the owner from the threat list (the experience winner,
else the killing blow's player) and rolls the monster's table; a non-empty roll creates the
container at the corpse with coins and up to 8 entries. Per tick (`updateLoot`): at 30 s the owner
window ends (ENTITY_STATE 0 to the observers); viewers that are dead (YouDied) or farther than 6 m
(OutOfRange) are closed; changed contents go to the viewers as LOOT_CONTENTS; a container without
open reservations is removed when it is empty or 90 s old (LOOT_CLOSED(Gone) to its viewers, then
ENTITY_DESPAWN to its observers).

**Takes.** `LOOT_TAKE_REQ` is checked by the world (§3.1 of the contract: NotFound, YouAreDead,
OutOfRange 3 m + 0.5 m, NotYours, the 8-in-flight cap, AlreadyTaken); the requested units are taken
out of the visible contents into a `LootReservation` (id = zone id << 48 | counter) and handed to
the player's session in its batch (`WorldBatch::lootGrants`). The session queues the grant behind
its item moves (both change the inventory and are decided one at a time against the committed
state), plans it (`planItemAdd`, coins up to the cap) and writes it with
`IAccountService::applyItemWrite` (one transaction). Its completion callback (DB thread) posts a
`LootSettleCommand` with what was *not* taken (on failure: everything) straight to the world, then
answers the client on the session's executor (ITEM_UPDATEs, COINS, LOOT_TAKE_RES). The world puts
the returned units back under their indices and closes the reservation.

**No duplication, no loss.** A unit is always in exactly one of: the container, a reservation, the
character's rows. Reservations end exactly once:
- the session started the write → the settle comes from the DB completion, even if the session is
  gone by then;
- the session never started it (the grant was queued behind a move, or still in a batch on its way
  when the player left) → the session's `LeaveCommand` lists only the reservation being written
  (`lootInFlight`); `World::onLeave` returns every other reservation of that session to its
  container (`Zone::cancelLootTakes`), in every zone, also for a session already replaced by a
  duplicate login; a grant that arrives later is dropped with the stale world generation;
- a container is never removed while it has an open reservation (a slow write keeps it past 90 s).
Two players taking the same entry in one tick: the command order decides, the second gets
AlreadyTaken. Merge moves use the same `applyItemWrite` (two quantity changes, `WHERE quantity =
expected`), so a stale plan fails as a whole.

**Session side.** LOOT_OPEN_REQ / LOOT_TAKE_REQ / LOOT_CLOSE_REQ share one 10/s window (more →
ERROR(Flood)); malformed payloads are protocol violations; outside InWorld BadState, except requests
in flight while leaving (dropped like MOVE_REQ). COINS (gained 0) follows INVENTORY on entry.

**Cost.** Containers do nothing per tick unless they are viewed, change or expire; a take is one
grant in the batch and one transaction. Measured with 200 bots (40 fighting and looting their kills):
see §11.

## 8d. Stats (M11 "Künye", docs/PROTOCOL_STATS.md)

**Shared rules.** `shared/game/Combat.h`: five attributes (Güç, Çeviklik, Dayanıklılık, Zekâ,
İrade), the class starting spreads (sum 35), `pointsForLevel` (3 per level above 1),
`unspentPoints`, `checkAllocation` (InvalidAmount → NotEnoughPoints → AttributeCap) and
`deriveStats(class, level, allocated, equipment)` — the one function the world, the session, the
bots, vo_admin and the client's previews use. Item templates carry optional attribute bonuses and
requirements (`EquipBonus::attributes`, `ItemUser::attributes`, `AttributeTooLow`).

**Who owns what.**
- The **session** owns the committed allocation (`Session::m_statPoints`, loaded with the
  character) and decides `STAT_ALLOCATE_REQ`s one at a time (queue of 4 behind the one in flight,
  5/s flood cap) with `checkAllocation` against the **live level** it learns from the world's
  batches (`WorldBatch::level`; a level-up can only make it stricter for a tick). An accepted request
  is written with `IAccountService::allocateStats` (budget = points of that level, class start for
  the cap) and answered after the commit with the stored values; then the session posts a
  `StatsCommand{session, allocated}` to the world. The world never writes the allocation.
- The **world** (`Zone::PlayerCombat::allocated`) derives the stats from it on entry (from
  `CharacterInfo::statPoints`), on `StatsCommand` (`Zone::setAllocation`: recompute, hp/mp kept and
  clamped, CHAR_STATS, SELF_STATS and observers' ENTITY_STATS if a maximum changed), on a level-up
  (+3 unspent) and on an equipment change (item bonuses). `CHAR_STATS` is appended to the entering
  player's first batch right after the session's `ENTER_WORLD_RES`/`INVENTORY`/`COINS` and before
  any `ENTITY_SPAWN`; later ones go out at the end of the tick (`PlayerCombat::charStatsDirty`, at
  most one per tick, owner only).
- **Dodge**: `Zone::monsterAttack` rolls the player's dodge chance first (one extra RNG draw when
  the chance is above 0); a dodge is `Zone::missPlayer`: COMBAT_EVENT kind Miss (amount 0, hp
  unchanged) to the same audience as a hit, in combat, standing up; `ZoneCounters::dodges`.
- **Heals** use the caster's `Stats::healMultiplier` (Zekâ + İrade), **regeneration** the
  `Stats::hpRegen` / `mpRegen` per second (Dayanıklılık / İrade).
- `world.seed` (testing; `ZoneSettings::seed`): a fixed RNG seed for spawns, wandering and every
  combat roll. The UI flows and `integration.stats` use one.

**Cost.** Nothing per tick: stats are recomputed only on the events above; CHAR_STATS is 48 bytes.

## 8e. Skills and effects (M12 "Yetenek Yolları", docs/PROTOCOL_SKILLS.md)

**Data and shared rules.** `gamedata/skills.toml` is loaded and validated by `shared/game/Skills.h`
(`SkillCatalog`; `vo_server` refuses to start without a valid file, `world.skills_file` overrides
the search, and installs it as the process-wide `game::skills()` before any thread runs; tools and
the client load the same file on first use). The rules are pure shared functions:
`skillPointsForLevel` (1 per level above 1), `skillPointsSpent` / `branchPointsSpent`,
`checkSkillLearn` (UnknownSkill → MaxRank → NotEnoughPoints → TierLocked), `validSkillBar` /
`sanitizeSkillBar`, `describeSkill`, `applyEffectModifiers` and the periodic / shield amounts in
`Combat.h`.

**Who owns what.**
- The **session** owns the committed ranks and bar (`Session::m_skillRanks`, `m_skillBar`, loaded
  with the character). `SKILL_LEARN_REQ` and `SKILL_BAR_SET_REQ` share one queue (8 behind the one in
  flight, 10/s flood cap together); each is checked against the committed state and the live level,
  written with `IAccountService::learnSkill` / `setSkillBar` (the job repeats the check on the stored
  ranks under a row lock) and answered after the commit; a learn then sends `SKILL_STATE` and posts
  `SkillsCommand{session, ranks}`. The session sends `SKILL_STATE` + `SKILL_BAR` right after `COINS`
  on entry and a new `SKILL_STATE` after a batch that raised the level.
- The **world** keeps the normalised ranks (`PlayerCombat::skillRanks`) for `SKILL_REQ`
  (`NotLearned` at rank 0; range, mp, cast and cooldown of the current rank), the per-skill
  cooldowns (`skillReadyAt`, `skillCooldownMs`), and the effects of every player and monster
  (`Zone::ActiveEffect`: skill, kind, source, value, amount, duration, end, next tick).

**Execution** (`server/world/ZoneSkills.cpp`, `Zone::executeSkill`): pay mp, start the cooldown
(`SKILL_COOLDOWNS` to the caster), the leap if any (`findLeapPoint`: 1.5 m before the target on the
line, else ±30°/60°/90° around it, each walkable, outside safe zones and reachable by a walkable
straight segment; the caster is moved with AOI bookkeeping, its move credit dropped and a
`POSITION_CORRECT` queued; `SKILL_LEAP` to the caster and observers), `SKILL_USED`, then the targets:
the primary, or an area (`collectEnemies`: monsters the caster has spawned, alive, not returning,
outside safe zones, within radius + 0.5 m, primary first then nearest; `collectAllies`: the caster
and living players it has spawned), damage hits with `crit + crit_bonus` (≤ 85 %), heals, and the
effects (`on = target` on every hit entity, `on = self` on the caster).

**Effects.** Upsert by (skill, kind) (a refresh replaces), at most 16 per entity; every change goes
out as `EFFECT_APPLY` / `EFFECT_REMOVE` with `sendAround` (the entity and everyone who has it
spawned), and `sendSpawn` / `sendMonsterSpawn` append an `EFFECT_LIST` after the `ENTITY_SPAWN` of an
entity that has effects. Ticks run at the start of the entity's combat update
(`updatePlayerEffects`: heal-over-time, expiry; `updateMonsterEffects`: damage-over-time — which may
kill — and expiry). Stat-changing effects on players recompute `PlayerCombat::stats` =
`applyEffectModifiers(baseStats, modifiers)`; every combat formula, SELF_STATS, CHAR_STATS and
ENTITY_STATS use those effective values; max hp up raises hp by the gain. Shields absorb in
`damagePlayer` before hp (`absorbDamage`; `EFFECT_APPLY` with the absorb left, `Depleted` at 0).
Monsters honour them in the AI: stun (stand still, no attacks, leash timer paused), root (no
movement), slow (move speed × (1 − v), attack interval × (1 + v / 200)), weaken (attack × (1 − Σ),
≤ 75 %), taunt (the latest taunter is the target while valid; applying puts it at top threat + 10 %).
A harmful effect counts 1 threat and pulls an idle monster. Death clears effects without packets
(clients clear on `ENTITY_DEATH`), a monster that starts returning home clears them with `Cleared`,
a despawned monster loses them. A damage-over-time tick whose caster left counts for nobody.

**Cooldowns across a relog.** `Zone::leave` returns the running cooldowns (`LeftPlayer::cooldowns`);
`World` keeps them with the recently-left state (`World::Recent`, 60 s) and hands them to
`Zone::enter` (`EnterParams::cooldowns`), which restores them and sends `SKILL_COOLDOWNS` right after
`CHAR_STATS`. Not stored in the database (a restart makes everything ready; no shipped cooldown is
longer than the 60 s memory).

**Cost.** Per tick only for entities that have effects (a vector scan); skills cost nothing while
nobody uses them. `EFFECT_*` packets go only to the entity's audience.

## 8f. Zones and gates (M13 "Üç Yurt", docs/PROTOCOL_ZONES.md)

**Data.** `gamedata/zones.toml` (shared strict loader `shared/game/ZoneData.h`, `game::zones()`;
`world.zones_file` overrides the search; exit 6 if missing or invalid) lists the zones (id, key,
Turkish name and tagline, terrain path relative to the assets directory, allowed races), the home
zone of each race and the gates (marker in this zone, trigger radius, target zone, arrival marker
there). vo_server loads **every** listed zone (`World::loadZone(id, file, error, ZoneLayout)`), then
`World::finalizeZones(homeAydor, homeKargan)` checks every gate against its target (the target is
loaded, the arrival marker exists, is walkable and lies more than radius + 1 m from every gate of the
target zone) and fixes the home zones (`world.start_zone_override` replaces them for every race the
override zone allows: the UI flows and the older integration scenarios run in Ortaocak with it).
`Zone::create` checks the zone's own markers: a walkable `spawn_<race>` for every race it allows and
every gate marker. Any failure is exit 6 with the zone, the file and the marker. Monster spawns of
zones that are not loaded are ignored with a warning; each zone installs its own monsters, safe
zones and loot (unchanged since M9: they were keyed by zone already).

**Isolation.** Every zone has its own players, monsters, loot containers, effects, AOI grid and RNG
(`world.seed + id − 1`, so zone 1 keeps the M11/M12 rolls). Packets are only ever built from one
zone's entity sets, so an entity of one zone is never replicated to a player of another; entity ids
come from one allocator for the whole world (unique across zones). `World` maps session → zone.

**Gates** (`Zone::updateGates`, once per tick after the moves, before the saves; free for zones
without gates). A player is inside a gate when its distance to the marker is ≤ radius. The decision
is the shared `game::checkGate` (Loading → Dead → Disarmed → RaceForbidden → Pass): disarmed for
2 s after the player appeared in the zone (entry, arrival, ZONE_LOADED_REQ) and until it has been
outside every gate since then (`Player::beenOutsideGates`; a character logging in inside a gate must
step out first). `Pass` queues a `GateTrigger`; `RaceForbidden` sends one `ZONE_GATE_REFUSED` per
stay inside, at least 3 s apart. Level-triggered: standing inside fires as soon as the conditions
hold (no key press, no further step). Arrivals and spawn spreads never land inside a trigger
(`spreadAround` skips such points), so walking out of a gate never bounces back.

**The change** (`World::runGateTriggers`, after every zone's update, before the flush):
1. `Zone::depart` = `leave` (observers get `ENTITY_DESPAWN`, the loot window closes silently, the
   running cooldowns are collected) plus everything the player carries: the live `CharacterInfo`
   (level, xp, hp, mp, stat points, ranks, visible equipment), the equipment bonus, the effects, and
   the batch the old zone produced for it in this tick. Monsters drop it like any player that left;
   a cast in progress ends without cost; DoTs it put on old-zone monsters count for nobody.
2. That batch is delivered with `WorldBatch::zoneChangeBegin` set (the session sends the old zone's
   packets, then `ZONE_CHANGE_BEGIN`; loot grants in it are handled as usual).
3. The arrival position is computed in the target zone (`Zone::arrivalPosition`: the marker, spread
   like a spawn point) and stored with the normal state save; the player is in `World::m_transit`
   (in no zone). The save's completion — any thread — posts `ZoneArrivedCommand{session, token, ok}`.
4. `onZoneArrived`: stored → `Zone::enter` in the target zone with a new entity id and
   `EnterParams::zoneChange` (the batch's `EnteredInfo` says zone change, result, from zone; the
   effects are restored with their end times, the player's own re-sourced to the new id, others' with
   source 0; the cooldowns as after a relog; `EFFECT_LIST` about itself after `CHAR_STATS` /
   `SKILL_COOLDOWNS`). Not stored → the same into the **old** zone where it stood (inside the gate,
   disarmed), result Failed.
5. The arrived player is **loading** until `ZONE_LOADED_REQ` (`ZoneReadyCommand` → `Zone::setReady`)
   or 20 s: its moves are ignored (`MoveVerdict::Ignored`, no correction), combat commands are
   dropped, monsters neither pull nor attack it (`validMonsterTarget`), gates do not fire.

Between 3 and 4 the session's commands find no zone: moves, combat and loot are dropped;
`EquipCommand` / `StatsCommand` / `SkillsCommand` update the transit's `EnterParams`, so the new zone
starts with the committed values. A `LeaveCommand` during transit waits for the arrival's write
(`Transit::leaveDone`; stored → done, recently-left cache at the arrival; failed → one more save where
it stood). A duplicate login during transit takes the arrival over (the old change's completion is
ignored by its token). `World::stop()` saves every transit's arrival again (one account's writes run
in order, so this cannot be overtaken) and completes the leaves. `World::playerCount()` counts the
players in transit.

**Session side.** `onWorldBatch`: `zoneChangeBegin` → `ZONE_CHANGE_BEGIN` after the batch's packets,
then movement / combat / loot requests are dropped silently (still counted by their flood windows);
an `EnteredInfo` with `zoneChange` → `ZONE_CHANGE` + `INVENTORY` + `COINS` + `SKILL_STATE` +
`SKILL_BAR`, then the batch (as a fresh entry). `ZONE_LOADED_REQ` is valid once per change, after
`ZONE_CHANGE` (else `ERROR(BadState)`; another zone id is a protocol violation); it restarts the
MOVE_REQ seq check and posts `ZoneReadyCommand`. Item moves, stat allocations and skill requests run
on throughout (they belong to the session).

**Placement** (`World::onEnter`): the saved zone must be loaded and allow the race and the saved
position must be placeable, else the race spawn of the home zone. Logged out dead: the race spawn of
the zone it is in. Respawn: the race spawn of the zone it died in (a homeland's own spawn; Ortaocak's
race spawns in Eşik). `World::newCharacterSpawns()` gives the account service the home zones' spawns.

**Persistence and crash-safety.** One row per character (`characters.zone_id` + position): the arrival
is written before the player appears there, so a crash leaves it either at its last saved place in
the old zone or at the arrival — never in both, never lost. The DB outage path (§8, the outage tests)
applies unchanged: a failed arrival write puts the player back where it stood.

**Cost.** Per tick one distance test per player and gate of the zones that have gates; a change is
one save, one `depart` (like a leave) and one `enter`. Measured with three zones: see §11.

## 8g. Merchants and consumables (M14 "Pazar", docs/PROTOCOL_SHOP.md)

**Data.** `gamedata/npcs.toml` (shared strict loader `shared/game/Npcs.h`, `game::npcs()`;
`world.npcs_file` overrides the search; exit 6 if missing or invalid, with the stock checked against
items.toml: every entry must exist and have a value) lists the town NPCs. After the loot tables
`World::installNpcs(catalog, items)` gives every zone its NPCs (`Zone::installNpcs`): each spot must
pass `game::checkNpcSpot` on the zone's terrain — inside, room for a customer (a point 2.5 m out walkable with 0.5 m round it; the spot itself may be behind a counter, fix1), the
road layer (splat G / sum of the four weights) at most 50 %, more than gate radius + 2 m from every
gate marker and more than 4 m from every `spawn_*` / `arrive_*` marker — else exit 6 naming the NPC,
the zone and the rule. NPCs of zones not loaded are ignored with a warning. Prices come from
items.toml (`game::buyPrice` = value) and are fixed for the server's life.

**Entities.** `Zone::Npc` (entity id from the world allocator, `NpcDef*`, position, yaw, AOI cell,
the stock with prices, observers) — handled like loot containers in the AOI: `npcSeen` / `npcUnseen`
on entry, cell changes and leaves (`hasEntity` includes them), `ENTITY_SPAWN` type 4 with the NPC id
as `template_id` and the name, nothing else ever (no moves, stats, states). Combat: `ATTACK_REQ` and
enemy skills naming one answer `NotAttackable`, friendly skills `WrongTargetType`; area skills only
collect monsters / players, so NPCs are never hit.

**Shops** (`ZoneShop.cpp`, `Zone::shop`, on `ShopCommand`): `Player::shopNpc` is the open shop (one).
Open checks NotFound (not a merchant the player has spawned) → YouAreDead → OutOfRange (talk range 4 m
+ 0.5 m) and answers `SHOP_OPEN_RES` with the stock; Close clears it silently; Buy / Sell check NotOpen
→ YouAreDead → OutOfRange (→ NotInStock for a buy) and either answer the failure themselves or append
a `ShopGrant` (kind, seq, npc, template / bag slot, quantity, unit price) to the player's batch.
`updateShops` (per tick, after loot): a dead player's shop closes with `SHOP_CLOSED(YouDied)`, one
farther than 8 m with `OutOfRange`; a merchant leaving the view closes it (`OutOfRange`), a leave or a
zone change silently. Requests of a loading player (zone change) are dropped.

**Session side of a shop grant** (`Session::startShopGrant`, in the item queue with moves and loot
grants, against the committed inventory and balance): `game::planShopBuy` (BadQuantity →
NotEnoughCoins → BagFull, stacks first then free bag slots) / `game::planShopSell` (BadQuantity →
NoItem → BadQuantity → CannotSell → PurseFull), then **one** `applyItemWrite` (grown stacks + new stacks
+ `coinsRemoved`, or the shrunk / deleted stack + `coinsAdded`); after the commit `ITEM_UPDATE`,
`COINS`, `SHOP_*_RES(Ok)`; a failed write answers ServerError and changes nothing. Nothing is
reserved in the world, so a grant lost on the way (the player left) needs no settling; more than 16
operations waiting answer ServerError at once.

**Consumables.** The session checks NotUsable / NoItem against the committed bag
(`game::checkItemUseStack`) and posts `ItemUseCommand(Use)` with the template's restore amounts. The
world (`Zone::useItem`) checks YouAreDead → Stunned → OnCooldown → AlreadyFull (`game::checkItemUse`),
starts the shared 10 s cooldown (`PlayerCombat::potionReadyAt`), sends `ITEM_COOLDOWN` and appends an
`ItemUseGrant`. The session finds the stack again (it may be gone: NoItem), writes `quantity − 1`
(`applyItemWrite`), answers `ITEM_UPDATE` + `ITEM_USE_RES(Ok)` after the commit and posts
`ItemUseCommand(Apply)`; the world then restores (clamped), marks `SELF_STATS` / `ENTITY_STATS` and the
save dirty and sends `ITEM_USED` to the player and its observers (`sendAround`). A dead player gets
nothing back; a player between two zones gets the restore added to the transit's hp / mp (no
`ITEM_USED`). The cooldown travels with the relog memory and zone changes as `RememberedCooldown`
skill 0 and is reported with `ITEM_COOLDOWN` after `CHAR_STATS` on entry.

**Testing hook** `world.testing_enter_vitals_percent` (0 = off): a world entry (not a zone change) of a
living character sets hp / mp to that percentage — the shop integration scenario drinks potions
without a fight.

**Testing hook** `world.testing_fail_first_item_write` (0 = off): in every session the first buy, the first
sale and the first use of that item template complete as if their write had failed (ServerError, nothing
changed; a failed use keeps its cooldown) — the `shoperrors` integration scenario uses 16 to reach the
ServerError answers through the protocol (`Session::applyShopWrite`).

**Cost.** Per tick one distance test per player with an open shop; NPCs add AOI entries (six in the
shipped data) and nothing per tick.

## 9. Network threads and outbound efficiency

- Sessions are spread round-robin over `net.io_threads` network threads, each running its own
  single-threaded io_context (the connection classes require that). The listener, signals and
  shutdown run on the main thread. The shared registry (`ServerContext::sessions/online/counters`)
  is mutex-guarded; cross-session actions (duplicate-login kick, shutdown kick) are posted onto the
  target session's thread (`Session::kickAsync`).
- One `WorldBatch` per player per tick: payloads are encoded once per tick (an `ENTITY_MOVE` is
  encoded once and copied to every observer), stored back to back in one buffer, and crossed to the
  network thread in one `asio::post`.
- Outbound frames are coalesced by `shared/net/Connection` (used by server, client and bots since
  M7; the M6 server-only copy `ServerConnection` is gone): a whole tick batch
  (`beginBatch()`/`endBatch()` around `onWorldBatch`) and all replies produced while handling one
  read are encrypted frame by frame, in send order, into one buffer and written with a single
  `async_write`; frames sent while a write is in flight join the next one. The buffer of the write
  in flight is never touched until its completion handler ran (also on abort / peer reset), and the
  AEAD nonce counter advances only for frames that were actually queued. Writing one frame per
  `async_write` and waiting for each completion (the M1–M5 behaviour) capped a connection at
  roughly 1000 frames/s on loopback: with 200 bots in one area (≈1300 `ENTITY_MOVE`/s per client)
  every client overflowed the 256 KB cap within seconds. Frames are sealed in place (no temporary
  plaintext buffer per frame). Tests: `tests/net/ConnectionTests.cpp`.
- The 256 KB outbound cap (in flight + pending) closes the connection (`OutboundOverflow`); the
  session logs a rate-limited warning when more than 128 KB are queued for a slow reader.

## 10. Configuration

`[world]` in `config/server.toml`:

| Key | Default | Meaning |
|---|---|---|
| `tick_rate` | 20 | ticks per second (1..1000; < 10 warns: the relay needs 10 Hz) |
| `assets_dir` | `""` | compiled assets; zone 1 is `<assets_dir>/terrain/zone1/zone1.vterrain`. Relative = to the repository root. Empty = search `assets/terrain/zone1/zone1.vterrain` upwards from the working directory, then the executable directory |
| `save_interval_s` | 60 | periodic position save (0 = only on leave/logout/disconnect/shutdown) |
| `move_bank_seconds` | 1.0 | cap of the carried-over movement allowance (0.2..10) |
| `spawn_spread_m` | 2.0 | spread radius at spawn points (0..20) |
| `stats_interval_s` | 60 | periodic world statistics log (0 = off) |
| `max_catch_up_ticks` | 5 | lag (in ticks) after which the loop skips ahead instead of catching up |
| `items_file` | `""` | item templates; relative = to the repository root. Empty = search `gamedata/items.toml` upwards from the working directory, then the executable directory |
| `monsters_file` | `""` | monsters, spawn points, safe zones; like `items_file` (`gamedata/monsters.toml`) |
| `skills_file` | `""` | skills, branches and point rules; like `items_file` (`gamedata/skills.toml`; exit 6 if missing or invalid) |
| `seed` | 0 | testing: fixed RNG seed of the zones (spawn spread, monster placement and wandering, damage / critical / dodge rolls); 0 = random. Logged at startup when set. Zone N uses `seed + N − 1` |
| `zones_file` | `""` | zones and gates (§8f); like `items_file` (`gamedata/zones.toml`; exit 6 if missing or invalid). Every zone's terrain is `<assets_dir>/<terrain>` |
| `start_zone_override` | 0 | testing: new characters of every race the zone allows start in this zone instead of their homeland (UI flows and the older integration scenarios use 1); 0 = off; a zone not in zones.toml is a config error (exit 2) |
| `npcs_file` | `""` | town NPCs / merchants (§8g); like `items_file` (`gamedata/npcs.toml`; exit 6 if missing or invalid, or an NPC spot is unusable) |
| `testing_enter_vitals_percent` | 0 | testing: every world entry (not a zone change) of a living character starts at this % of hp / mp (0..100; the shop integration scenario uses 10); 0 = off; logged as a warning when set |
| `testing_fail_first_item_write` | 0 | testing: the first buy, sale and use of this item template in every session fail like a failed write (ServerError; the shoperrors scenario uses 16); 0 = off; logged as a warning when set |
| `testing_zone_arrival_delay_ms` | 0 | testing: a zone change completes at the earliest this long after ZONE_CHANGE_BEGIN (0..10000; the zonesrace scenario uses 1500 so a logout or a dropped connection lands in the middle); 0 = off; logged as a warning when set |
| `testing_boss_respawn_ms` | 0 | testing (M15): every elite (boss) respawns this long after its death instead of its tens of minutes (0 or 3000..86400000); 0 = off; warned |
| `testing_force_unique_drop` | 0 | testing (M15): 1 = every boss loot roll also holds one of its table's unique items; warned |
| `testing_xp_percent` | 0 | testing (M15): experience gains scaled by this percentage (0..1000000; the hunt scenario uses 100000); 0 = off; warned |
| `testing_enter_level` | 0 | testing (M15): a world entry (not a zone change) below this level is raised to it (xp 0, full hp / mp; stored with the next progress save — until then the account service still knows the old level, so stat / skill spending is refused); 0 = off; warned |
| `testing_monster_attack_percent` | 0 | testing (M15): monsters hit with this percentage of their attack (0..1000; the m15 UI flows use 25 / 40); 0 = off; warned |
| `testing_upgrade_outcome` | "" | testing (M16, docs/PROTOCOL_UPGRADE.md §2.3): "success" (every upgrade attempt succeeds), "fail" (every attempt that can fail fails: targets +4 and up), "fail_from_<4..10>" (targets below N succeed, from N on fail) instead of the server's roll; "" = the real chance; logged as a warning when set |
| `testing_enter_coins` | 0 | testing (M16): a character entering the world with fewer Akçe is topped up to this balance (0..1000000000, stored like a gain before the entry; the upgrade scenario and flows use 5000); logged as a warning when set |

M15 hunting slots (`gamedata/monsters.toml` `[[slot]]`, docs/PROTOCOL_LEVELS.md §3): `Zone::installMonsters` checks every
slot of the zone with `game::checkSlotTerrain` (walkable homes and wander circles, a straight line to the centre, reachable
from the first `spawn_` / `arrive_` marker, area + aggro clear of the gates + 12 m and the markers + 8 m, bosses their leash +
10 m) and refuses to start (exit 6) naming the slot. Each member keeps its slot's leash and respawn time
(`Monster::leash`, `respawnMs`, `slot`) and respawns at its own home.

`[net] io_threads` (default 0 = auto: logical cores / 4, clamped to 1..4): network threads.

vo_server exit code 6: world data (a zone terrain or one of its markers, `gamedata/zones.toml`,
`items.toml`, `skills.toml`, `monsters.toml`, `loot.toml` or `npcs.toml`) missing or invalid, a monster
spawn point not walkable, or an NPC spot unusable (§8g).

Idle timeout: time a session spends waiting for the server (queued login hashing, a world or DB
request) no longer counts as idle; at 1000 simultaneous logins the in-memory service needs ~45 s
and sessions were being dropped right after their LOGIN_RES.

## 11. Tests and the load test

- Unit (no sockets): `tests/server/world/MoveValidatorTests.cpp` (formula boundaries, walk/run,
  bounds, mask, tunnelling, height, bank cap, lag burst, packet spam, NaN/absurd/flags),
  `ZoneTests.cpp` (placement, ENTER/SPAWN order, AOI property test vs. brute-force oracle, relay
  rate + final stop, corrections, leave, staggered saves), `WorldTests.cpp` (lifecycle with fake
  links, take-over, recently-left cache, stop() saves, threaded loop, flood limiter, TickStats),
  `tests/game/MovementTests.cpp` (shared rules, segment walkability vs. dense sampling).
- In-process with sockets: `tests/server/ServerTests.cpp` (two players, implausible MOVE_REQ, flood,
  duplicate login in world).
- End to end (`ctest -R integration`): `world`, `speedhack`, `wallhack`, `floodmove`, `leaverace`,
  `dupworld`, `aoi`, `persist`, `load` (24 bots, 8 s), `noterrain`; with the test database also
  `integration.db.persist` (stay in world, server stops, new server, same position) and
  `integration.db.aoi`.
- M7 robustness: `tests/server/ServerRobustnessTests.cpp` (slowloris, half-open, dead peer, forced
  stop), the `[overload]` world test (a 170 ms tick is caught up, a 600 ms stall skipped, 20 Hz
  afterwards), `tests/db/WorldDbOutageTests.cpp` (pool connections killed while a player is in the
  world: no correction, saves retried, nothing blocks the world thread) and `vo_fuzz_tests`
  (docs/BUILDING.md "Tests").
- M8 items: `tests/game/ItemsTests.cpp` (loader validation, every rule of PROTOCOL_ITEMS.md §1.2),
  `tests/net/MessagesTests.cpp` (codecs, limits, decoder fuzzing), `tests/server/ItemServerTests.cpp`
  (kit once, INVENTORY order, CharSummary kit preview, ENTITY_EQUIP in view / not out of view,
  pipelined moves, flood, storage failure, hidden items), the item suite of
  `AccountServiceTests.cpp` (both backends), `tests/db/PostgresItemTests.cpp` (concurrent kit grant,
  constraints, cascade, admin helpers, Server + PostgreSQL), `integration.items` /
  `integration.db.items` (botclient `items` scenario, vo_admin `list-items` / `give-item`), and the
  load test (every third bot equips/unequips with the shared rules and must get Ok).
- M9 combat: `tests/game/CombatTests.cpp` (formulas with injected rolls, level curve, experience
  reduction, stats with equipment, skills, the tuning target against the shipped gamedata, the
  strict monster loader), `tests/assets/content/MonsterSpawnTests.cpp` (every spawn circle walkable
  on the compiled zone 1, reachable from the plaza, out of reach of the flows' paths),
  `tests/net/MessagesTests.cpp` (combat codecs, layout, rejects, decoder fuzzing; combat requests in
  the server fuzz), `tests/server/world/CombatZoneTests.cpp` (the world's rules and the AI on
  synthetic terrain with a fake clock: AOI of monsters, auto-attack, skills, casts and interrupts,
  heal and its threat, safe zone, AI chase around a wall / attack interval / leash and return heal /
  wander radius, death and respawn timing, xp reduction, level-up broadcast and save, routing to
  third observers, stance, regeneration and rate limits, equipment, progress),
  `tests/server/CombatServerTests.cpp` (in-process server: v3 ENTER_WORLD_RES, monster spawns,
  SELF_STATS after equip, flood, malformed / out-of-state requests, routing to a third client, item
  moves refused while dead, RESPAWN timing, progress stored, set-level), `tests/db/PostgresCombatTests.cpp`
  (0003 columns, state saves, constraints, set-level, logging out dead → 50 % on a new server),
  `integration.combat` / `integration.db.combat` (botclient `combat`), and the load test (every
  fifth bot fights and respawns; the others stay inside the safe zone).
- M10 loot: `tests/game/LootTests.cpp` (the new templates, material validation, merge moves, adding
  units, the strict loot loader, the shipped tables, roll distributions with a seeded generator),
  `tests/net/MessagesTests.cpp` (loot codecs, layout, rejects, decoder fuzzing; loot requests in the
  server fuzz), `tests/server/world/LootZoneTests.cpp` (fake clock: drop on a kill with the top
  damage dealer as owner, lock bits, the 30 s window, NotFound / OutOfRange / YouAreDead, take one /
  coins / everything with settles and leftovers, two looters racing, leaving with and without a
  write in flight, viewers closed at 6 m / on death, expiry at 90 s, the in-flight cap),
  `tests/server/LootServerTests.cpp` (in-process server: kill → open → take everything, stored rows
  and balance, stacking, BagFull with the rest staying, merge moves, a disconnect during and before
  the write, flood / malformed / out-of-state), `tests/db/PostgresLootTests.cpp` (0004 column and
  CHECK, applyItemWrite all or nothing, set-coins, give-items, Server + PostgreSQL across a restart),
  `integration.loot` / `integration.db.loot` (botclient `loot`, vo_admin `list-items` /
  `set-coins` / `give-item` with a count), and the load test (fighters loot their kills).
- M11 stats: `tests/game/CombatTests.cpp` (starting spreads, points, `checkAllocation`, every
  attribute's effect, caps, item bonuses, and the tuning table: v4 numbers at level 1 and unspent
  level 10, sensible level-10 builds against the Kor Bekçisi in a simulated straight fight; set
  `VO_TUNING_TABLE=1` to print it), `tests/game/ItemsTests.cpp` (attribute bonuses / requirements,
  AttributeTooLow), `tests/net/MessagesTests.cpp` (CHAR_STATS / STAT_ALLOCATE_* codecs, layout,
  rejects, fuzzing), `tests/server/world/CombatZoneTests.cpp` (CHAR_STATS on entry before any spawn,
  `setAllocation`, a level-up's 3 points, the dodge rate and Miss events, heals with İrade, item
  bonuses in CHAR_STATS), `tests/server/StatsServerTests.cpp` (in-process server: refusals, stored
  before the answer, CHAR_STATS after it, kept across re-entering, a failed write, reset-stats,
  set-level resets, pipelined requests, flood / malformed / out-of-state), the stats suite of
  `AccountServiceTests.cpp` (both backends), `tests/db/PostgresStatsTests.cpp` (0005 on a database
  from M10: every point unspent; budget, cap, ownership, the stored-level CHECK, constraints, admin
  helpers, a restart), `integration.stats` / `integration.db.stats` (botclient `stats`: hunt to
  level 2 with a fixed `world.seed`, spend, damage follows, re-enter, flood; with the database also
  vo_admin `show-character` / `set-level 10` / `reset-stats`), and the load test (fighters spend
  their points after every level-up; a refused allocation fails the run).
- M12 skills: `tests/game/SkillsTests.cpp` (the shipped file: the fixed 28 ids / names / icon keys /
  branches, rank 1 of the M9 skills exactly v5; ~40 broken files the loader must refuse with a clear
  message; points, spending, tier gates, every class able to spend 9 points in either branch; the
  bar; descriptions per rank; effect modifiers; the v6 codecs, sizes and rejects),
  `tests/game/SkillBalanceTests.cpp` (simulated level-10 branch builds against the Kor Bekçisi, see
  DEVELOPMENT_PLAN.md §22), `tests/server/world/SkillZoneTests.cpp` (NotLearned, rank scaling, areas
  around self / the target with max targets and nearest first, NoTarget, safe zone, slow and its
  expiry to observers, damage over time and a DoT kill with experience, buffs on the stats and their
  expiry, max hp up, heal over time, the ring heal, the absorb shield, stun, weaken against the
  formula, taunt, the leap and NoPath / OutOfRange, EFFECT_LIST after ENTITY_SPAWN, the leash return
  and death clearing effects, cooldown reports and their memory across a leave, new ranks from the
  session), `tests/server/SkillServerTests.cpp` (in-process server: entry order, stored before the
  answer, SKILL_STATE after it, the world using the new rank, the bar, a failed write, reset-skills,
  set-level, an admin reset while online, pipelined requests, flood / malformed / out-of-state),
  `tests/db/PostgresSkillsTests.cpp` (0006 on an M11 database, learn / bar rules on the stored ranks,
  ownership, the constraints, admin helpers, a restart), the fuzzer (learn / bar / skill requests
  for random ids and targets), `integration.skills` / `integration.db.skills` (botclient `skills`:
  hunt to level 2 with Ağır Darbe, learn Kalkan Duvarı, its buff and expiry, kept with the
  cooldown after re-entering, flood; with the database also vo_admin `show-character` / `set-level
  10` / spending 8 points through both branches / `reset-skills`), and the load test (fighters store
  their bar, learn on every level-up and use every learned skill; a bad answer fails the run).
- M13 zones: `tests/game/ZonesTests.cpp` (the shipped zones.toml and its fixed markers / gates, the
  names equal to `shared/game/Zones.h`, ~40 broken files refused with file:line, the gate decision,
  the v7 codecs, sizes, rejects and a decoder fuzz), `tests/server/world/ZoneChangeTests.cpp` (three
  synthetic zones, fake clock: isolation, the full change sequence with the write held back, moves
  ignored while loading, re-arm after an arrival and after logging in inside a gate, refusals once per
  stay / 3 s apart, dead players, a failed write, leave / duplicate login / stop() during a change,
  effects and cooldowns carried, placement by race and home zone, respawn spawns, the startup checks,
  12 commuters and the AOI per zone), `tests/server/ZoneServerTests.cpp` (in-process server: the wire
  sequence, moves and an attack dropped while loading, the seq restart, out-of-state and wrong-zone
  ZONE_LOADED_REQ, the stored arrival in the character list and on re-entry, the Kargan road), the
  fuzzer (ZONE_LOADED_REQ for random zones, a server packet), `integration.zones` /
  `integration.db.zones` (botclient `zones`: both races through homeland → Eşik → refusal → home with
  relogs in every zone, `--phase pair`; with the database vo_admin `show-character` /
  `move-character`), and the load test with `--zones` (commuters and homeland residents).
- M14 merchants and consumables: `tests/game/ShopTests.cpp` (the shipped npcs.toml, ~25 broken files
  refused with file:line, the spot check on synthetic terrain, consumables in items.toml and their
  strict keys, prices, buy / sell planning, the consumable checks, bar item slots, the potion loot
  lines, the v8 codecs / sizes / rejects, the minimal client's refusal lines),
  `tests/server/world/ShopZoneTests.cpp` (fake clock: NPC entities through the AOI and never
  attackable, the startup spot checks, open / buy / sell checks and grants, closing at 8 m / on death /
  by another merchant / on leave, the consumable checks, the shared cooldown and its relog memory,
  Apply clamped and broadcast, the vitals hook), `tests/server/ShopServerTests.cpp` (in-process
  server: sell → buy → drink with the wire order and stored balance, a failed write, bar item slots
  kept across a re-entry with the running cooldown, flood caps, out-of-state / malformed requests),
  the purchase / sale part of the account service suite (both backends),
  `tests/db/PostgresShopTests.cpp` (0007 on an M13 database, bar item slots stored as −template, a
  purchase after `set-coins` lowered the balance, Server + PostgreSQL storing each step before its
  answer), the fuzzer (shop / use requests, item bar slots, a server packet, with a merchant near the
  spawn), and `integration.shop` / `integration.db.shop` (botclient `shop`, deterministic: a mage sells
  its spare staff, buys potions, puts them on the bar, drinks both at the testing vitals, relogs twice,
  floods; with the database vo_admin `show-character` and a new server process).

Measured (M10, Release, one machine, 200 bots for 60 s, 40 of them fighting and looting): all 200 in
the world after 8.7 s, 0 corrections, 0 disconnects; world tick avg 1.6–1.7 ms, p99 ≤ 2.75 ms, max
2.8 ms after the ramp (11.6 ms during it), 0 slow, 0 skipped; 74 KB/s and 1640 packets/s per client
(1620 ENTITY_MOVE/s: everyone in one village); PING RTT p50 0.27 ms, p99 6.5 ms; 593 item moves Ok;
26 kills, 26 loot takes (174 Akçe, 15 bag slot updates), 0 bad loot answers.

Measured (M11, Release, the same machine and run shape; the fighters spend their stat points after a
level-up): all 200 in the world after 8.7 s, 0 corrections, 0 disconnects; world tick avg
1.63–1.68 ms, p99 ≤ 2.55 ms, max 2.82 ms after the ramp (12.0 ms during it), 0 slow, 0 skipped;
74 KB/s and 1653 packets/s per client; PING RTT p50 0.26 ms, p99 5.9 ms; 592 item moves Ok; 24
kills, 24 loot takes (151 Akçe), 2 level-ups and 2 stat allocations stored, 0 bad answers.

Measured (M12, Release, the same machine and run shape; the 40 fighters store their skill bar,
learn a skill on every level-up and use every learned skill): all 200 in the world after 8.6 s, 0
corrections, 0 disconnects; world tick avg 1.52–1.60 ms, p99 ≤ 2.15 ms, max 2.40 ms after the ramp
(12.7 ms during it), 0 slow, 0 skipped; 74 KB/s and 1655 packets/s per client; PING RTT p50 0.24 ms,
p99 4.5 ms; 586 item moves Ok; 28 kills, 27 loot takes (182 Akçe), 3 level-ups, 3 stat allocations,
40 skill bars and 3 skill learns stored, 11 skill uses, 0 bad answers.

Load test against a running server (Release build, auto-registration on, e.g. a local config with
`auth.dev_auto_register = true`, `net.max_connections` above the bot count):

```
build\x64-release\bin\vo_server.exe --config <cfg>\server.toml --port 15001
build\x64-release\bin\botclient.exe --host 127.0.0.1 --port 15001 --server-key <hex> ^
    --terrain assets\terrain\zone1\zone1.vterrain --bots 200 --duration 60
```

Options: `--threads N` (client threads, default half the cores, max 8), `--ramp-ms N` (delay
between bot starts, default 5), `--max-corrections K` (PASS threshold, default 0). Since M9 every
fifth bot is a fighter (nearest monster of its level, auto-attack, class skill, respawn 5.5 s after
death; corrections of moves that were in flight when it died are reported separately and allowed;
since M10 it also walks to every container of its own kills and takes everything, and a
`ServerError` or a missing LOOT_TAKE_RES fails the run), and the others pick their walks inside the
village safe zone. The tool prints
the ramp time, received bytes/packets per client per second (min/median/avg/max), ENTITY_MOVE rate,
MOVE_REQ rate, spawns/despawns, corrections, disconnects and PING RTT p50/p90/p99/max, then logs
every bot out. Server-side numbers (tick avg/p99/max, slow/skipped ticks) are in the server log
(`world: ... s:` lines and `world: totals:` at shutdown). With the in-memory account service logins
are hashed on one worker thread (Argon2id, ~50 ms each), so 1000 bots need about a minute to get in.
