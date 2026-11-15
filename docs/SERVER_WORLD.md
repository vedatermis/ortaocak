# VedatOnline Server World Simulation (M6)

Status: M6 server half. Implements docs/PROTOCOL.md §4 and §5.4: authoritative movement, area of
interest, entity replication and position persistence. Code: `server/world/`, `shared/game/Movement.h`,
`shared/game/TerrainWalk.h`; network glue in `server/net/Session.cpp`.

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
| `ServerConnection` | `server/net/ServerConnection.h` | server-side framed/encrypted TCP stream with coalesced writes (§9) |
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
  wraps to post back onto its own executor. No DB completion ever enters the world.
- Character loads for ENTER_WORLD happen in the session (DB worker → session executor) *before* the
  world sees the character.

## 3. Tick pipeline

Fixed timestep `1 / world.tick_rate` (20 Hz → 50 ms), absolute scheduling (`next += interval`), so
there is no drift. On Windows the thread sleeps on a high-resolution waitable timer (≈0.5 ms jitter
without changing the process timer resolution).

1. **Commands** in posting order: enter (placement, ENTER info, spawn pairs), move (validation,
   AOI cell change), leave (despawn, save).
2. **Per zone** `update()`: one `POSITION_CORRECT` per player that had a rejected move this tick;
   `ENTITY_MOVE` relay; periodic saves due in this tick.
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
  2.5 + 0.5 m walking), and the 0.5 m slack is a one-off tolerance: the long-run speed can never
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
  session sends `ENTER_WORLD_RES` (real position, y on the terrain, walk 2.0 / run 5.5 m/s,
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

Ordering and freshness:
- The world is the only writer of positions. Requests of one account run in order in the account
  service (see docs/DATABASE.md), and a save is always *submitted* before the session can send the
  next ENTER_WORLD_REQ.
- Duplicate login: the new session may load the character before the kicked session's leave has
  been processed. If the character is still in the world, the new entity takes over the old one's
  live position (and the old entity is removed without a save). If it left within the last 60 s,
  its last in-world position is used even if the asynchronous save has not landed yet.

## 9. Network threads and outbound efficiency

- Sessions are spread round-robin over `net.io_threads` network threads, each running its own
  single-threaded io_context (the connection classes require that). The listener, signals and
  shutdown run on the main thread. The shared registry (`ServerContext::sessions/online/counters`)
  is mutex-guarded; cross-session actions (duplicate-login kick, shutdown kick) are posted onto the
  target session's thread (`Session::kickAsync`).
- One `WorldBatch` per player per tick: payloads are encoded once per tick (an `ENTITY_MOVE` is
  encoded once and copied to every observer), stored back to back in one buffer, and crossed to the
  network thread in one `asio::post`.
- The server uses `server/net/ServerConnection` instead of `shared/net/Connection`: identical wire
  format and close semantics, but outbound frames are coalesced — a whole tick batch (and all
  replies produced while handling one read) is encrypted frame by frame into one buffer and written
  with a single `async_write`; frames sent while a write is in flight join the next one.
  `vo::net::Connection` writes one frame per `async_write` and waits for each completion, which on
  loopback capped a connection at roughly 1000 frames/s with both processes mostly idle: with 200
  bots in one area (≈1300 `ENTITY_MOVE`/s per client) every client overflowed the 256 KB cap within
  seconds. With coalescing the same run passes (see the report). The coalescing should move into
  `shared/net/Connection` (it would help the client too); `ServerConnection` can then go.
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

`[net] io_threads` (default 0 = auto: logical cores / 4, clamped to 1..4): network threads.

vo_server exit code 6: world data (zone terrain) missing or corrupt.

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
- End to end (`ctest -R integration`): `world`, `speedhack`, `wallhack`, `floodmove`, `aoi`,
  `persist`, `load` (24 bots, 8 s), `noterrain`; with the test database also `integration.db.persist`
  (stay in world, server stops, new server, same position) and `integration.db.aoi`.

Load test against a running server (Release build, auto-registration on, e.g. a local config with
`auth.dev_auto_register = true`, `net.max_connections` above the bot count):

```
build\x64-release\bin\vo_server.exe --config <cfg>\server.toml --port 15001
build\x64-release\bin\botclient.exe --host 127.0.0.1 --port 15001 --server-key <hex> ^
    --terrain assets\terrain\zone1\zone1.vterrain --bots 200 --duration 60
```

Options: `--threads N` (client threads, default half the cores, max 8), `--ramp-ms N` (delay
between bot starts, default 5), `--max-corrections K` (PASS threshold, default 0). The tool prints
the ramp time, received bytes/packets per client per second (min/median/avg/max), ENTITY_MOVE rate,
MOVE_REQ rate, spawns/despawns, corrections, disconnects and PING RTT p50/p90/p99/max, then logs
every bot out. Server-side numbers (tick avg/p99/max, slow/skipped ticks) are in the server log
(`world: ... s:` lines and `world: totals:` at shutdown). With the in-memory account service logins
are hashed on one worker thread (Argon2id, ~50 ms each), so 1000 bots need about a minute to get in.
