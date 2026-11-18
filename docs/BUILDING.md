# Building VedatOnline

## Requirements

- Windows 10/11 x64
- Visual Studio 2026 (or 2022) with the **Desktop development with C++** workload
  (MSVC, Windows SDK 10.0.26100, the bundled CMake, Ninja and vcpkg)
- Git, internet access on the first build (vcpkg downloads and builds the dependencies)

Nothing has to be on `PATH`: `build.ps1` loads the VS x64 developer environment itself.

## Quick start (fresh clone)

From a plain PowerShell (Windows PowerShell 5.1 is fine) in the repository root:

```powershell
powershell -ExecutionPolicy Bypass -File build.ps1                              # 1. Debug build (first run: vcpkg, 10-30 min)
powershell -ExecutionPolicy Bypass -File tools\dev\setup-dev.ps1                # 2. server key pair + local configs (once)
powershell -ExecutionPolicy Bypass -File tools\build-assets.ps1 -Config Debug   # 3. runtime assets (assets_src -> assets)
powershell -ExecutionPolicy Bypass -File tools\dev\run-dev.ps1                  # 4. server (own console) + client
```

Step 2 writes `config/keys/` (the server's Ed25519 key pair), `config/server.local.toml`
(127.0.0.1, `dev_auto_register = true`, no database: accounts live in memory) and
`config/client.local.toml` (the matching `server_public_key`); all three are git-ignored. In the
client, log in with any new user name (4-20 letters/digits/_) and a password of at least 6
characters: the dev server creates the account. For a persistent database set
`db.connection_string` in `config/server.local.toml` (see [DATABASE.md](DATABASE.md)).

Check the build with `powershell -File build.ps1 -Test` (ctest; the PostgreSQL tests are skipped
without `db.test_connection_string`).

## Build

From a plain PowerShell in the repository root:

```powershell
powershell -File build.ps1                    # Debug build (preset x64-debug)
powershell -File build.ps1 -Config Release    # Release build (preset x64-release)
powershell -File build.ps1 -Test              # build + run ctest
powershell -File build.ps1 -Target vo_client  # build a single target
powershell -File build.ps1 -Clean             # wipe build/<preset>, then configure + build
```

The script exits with a non-zero code on any failure.

The first configure installs the vcpkg packages (`vcpkg.json`, triplet `x64-windows-static-md`:
static libraries, dynamic CRT, so the executables need no DLLs next to them). Building OpenSSL and
libpq takes 10–30 minutes once; afterwards vcpkg restores them from its binary cache
(`%LOCALAPPDATA%\vcpkg\archives`). Installed packages live in `build/vcpkg_installed`, shared by
both presets and kept by `-Clean`.

### vcpkg selection

`build.ps1` picks vcpkg in this order: `VO_VCPKG_ROOT` env var, `external/vcpkg` (a git clone of
microsoft/vcpkg, git-ignored), then the vcpkg bundled with Visual Studio (`<VS>\VC\vcpkg`).
Package versions are pinned by `builtin-baseline` in `vcpkg.json`.

### Without the script

From a "x64 Native Tools" prompt (or after running `vcvars64.bat`), with `VCPKG_ROOT` set:

```bat
cmake --preset x64-debug
cmake --build --preset x64-debug
ctest --preset x64-debug
```

Visual Studio and VS Code (CMake Tools) can open the folder directly and use the presets.

## Outputs

All executables go to `build/<preset>/bin/`:

| Executable | Description |
|---|---|
| `vo_server.exe` | Game server |
| `vo_client.exe` | Game client (Win32 + D3D11) |
| `vo_tests.exe` | Unit tests (Catch2) |
| `assetc.exe` | Asset compiler |
| `botclient.exe` | Headless test client: end-to-end scenarios (`--scenario ping|login|chars|world|...`, see `--help`) |
| `vo_admin.exe` | Database administration (migrate, status, accounts, characters; docs/DATABASE.md) |
| `vo_fuzz_tests.exe` | Protocol fuzzing of an in-process server (M7, see "Tests") |
| `vo_asset_content_tests.exe` | Content checks on the compiled assets |

## Running

Both executables find `config/` by searching upwards from the working directory and then from the
executable directory, so they work from the repo root or from `build/<preset>/bin`.

### Server

```powershell
build\x64-debug\bin\vo_server.exe                     # runs until Ctrl+C
build\x64-debug\bin\vo_server.exe --run-seconds 5     # stops by itself (automation)
build\x64-debug\bin\vo_server.exe --config path\to\server.toml
```

Configuration: `config/server.toml` (committed defaults), deep-merged with
`config/server.local.toml` if present (git-ignored; start from `config/server.local.toml.example`).
The DB connection string goes only in `server.local.toml` or the `VEDAT_DB_URL` environment
variable (the env var wins). Without one, accounts and characters live in memory and are lost when
the server stops; with one, the PostgreSQL account service is used (docs/DATABASE.md). With
`auth.dev_auto_register = true` (local config only) an unknown username with a valid password
creates the account on first login.

#### Server keys (`--gen-keys`)

The handshake is signed with the server's long-term Ed25519 key, and the client pins the public key.

```powershell
build\x64-debug\bin\vo_server.exe --gen-keys           # writes config/keys/server_ed25519.{key,pub}, prints the public key hex
build\x64-debug\bin\vo_server.exe --gen-keys --force   # replaces an existing key pair
```

The keys go to `<config dir>/keys/` (`config/keys/` is git-ignored); without them the server exits
with code 2. The printed hex belongs in the client's `server_public_key` (see below).

Exit codes: 0 ok, 1 bad arguments, 2 config error, 3 library init error, 4 runtime error.

### Local development setup (once)

```powershell
powershell -File build.ps1
powershell -ExecutionPolicy Bypass -File tools\dev\setup-dev.ps1     # keys + local config files
powershell -ExecutionPolicy Bypass -File tools\dev\run-dev.ps1       # server (new console) + client
```

`setup-dev.ps1` (Windows PowerShell 5.1 compatible) generates `config/keys/` with `--gen-keys` if it
is missing, writes `config/server.local.toml` (127.0.0.1 only, `dev_auto_register = true`) and
`config/client.local.toml` (`server_public_key` of that key pair) if they are missing. Existing files
are never overwritten unless `-Force` is given (which also regenerates the keys). All three are
git-ignored. `run-dev.ps1 [-Config Release] [-KeepServer] [-ClientArgs ...]` runs the setup first
if needed, starts `vo_server` in its own console, waits for port 15001 and starts the client; the
server is stopped when the client exits unless `-KeepServer`.

### Client

```powershell
build\x64-debug\bin\vo_client.exe
```

Config: `config/client.toml` deep-merged with `config/client.local.toml`: `server_public_key`,
`[server] host`, `port`, `connect_timeout_ms` (resolve + connect + handshake, default 10000),
`request_timeout_ms` (default 15000; a request without an answer drops the connection). The log is
written to `vo_client.log` next to the executable (override with `--log <file>`); `--console`
also mirrors it to the calling console. "Beni hatırla" stores only the username in
`%LOCALAPPDATA%\VedatOnline\client_settings.toml` (override with `--settings <file>`).

Screens (M4), all text Turkish:

- **Login**: username/password, "Beni hatırla", Giriş/Çıkış, Enter submits. Input is checked with the
  shared name rules before connecting; while connecting the inputs are disabled, a spinner shows
  "Bağlanıyor…", and Esc cancels. Every failure (server unreachable, timeout, version mismatch,
  server full, maintenance, server not verified, wrong credentials, banned, too many attempts, server
  error, dropped connection) shows a message box and returns to a usable screen. Esc while idle
  asks before quitting.
- **Character select**: 3 slots (card or "Boş Slot — Karakter Yarat"), preview on the left,
  ↑/↓ + Enter, Oyuna Gir / Karakter Yarat / Sil (type the name to confirm) / Çıkış Yap. An account
  without characters goes straight to creation.
- **Character create**: race cards (Aydor, Kargan; texts from [LORE.md](LORE.md)), the Közaran oath
  line for the first character, then class (one line in the chosen race's words, e.g. Aydor mage =
  Közbilimci, Kargan mage = Körükçü), the "Görünüm n/N" look selector (◀ ▶, complete character
  looks; hidden while a race has only one), name with live rule hints, Yarat / Geri. Every
  CHAR_CREATE result has its own message.
- **World** (M6, details in [CLIENT_RENDERING.md](CLIENT_RENDERING.md)): a loading screen
  ("Ortaocak yükleniyor…", one short lore line, progress bar) while the zone, props and characters
  load, then the 3D world: terrain, ~600 props, sky, sun shadows, the own character and other
  players with name labels; a zone banner ("Ortaocak — Eşik'in tarafsız köyü") fades in and out.
  HUD (after M13, [CLIENT_RENDERING.md](CLIENT_RENDERING.md) §15): the player frame top left (level
  medallion, name, race · class, hp / mp), the zone name top right, a framed bar at the bottom
  centre (action buttons, the skill slots, the experience bar, menu buttons) and an info panel
  (position, zone, RTT, players, FPS, draw calls; off by default, F3 toggles it). Every control is
  listed in the "Tuşlar" window (H or its bar button); there are no help lines. The camera stays out of houses and other large
  props; when it is pushed right up to the character, the character dithers out.
  The select/create screens show the real 3D model (race and look; idle animation; drag to turn).
- A dropped connection in any screen after login returns to the login screen with the reason
  (including a kick by a duplicate login).

**The world needs the runtime assets** (`assets/`, git-ignored). Build them once (and after
changing `assets_src/`):

```powershell
build\x64-debug\bin\assetc.exe build assets_src assets      # or: powershell -File tools\build-assets.ps1
```

Without `assets/` the select/create screens fall back to the 2D portraits and entering the world
shows "Dünya dosyaları yüklenemedi" and returns to character select.

World controls:

| Input | Action |
|---|---|
| W / S (or ↑ / ↓) | forward / back, relative to the camera |
| A / D (or ← / →) | sideways (strafe) relative to the camera; the character turns to face where it goes (there are no strafe clips) |
| Left click on the ground | walk/run to that point (destination ring); any WASD key cancels it |
| Left click on a monster / player | select it (target frame at the top, ring under it, TARGET_REQ) |
| Double click on a monster, right click (press + release without dragging) on a monster, F, or the bar button "Saldır" | auto-attack the target (ATTACK_REQ); out of range the character runs into range first |
| Tab | next nearest living monster within 40 m |
| 1 … 8 (or a click on a slot of the skill bar) | M12: the skill in that slot of the bar (bottom centre; slot 1 holds the class's free skill at first). Enemy skills on the selected monster ("Önce bir hedef seç." without one), friendly skills on a selected player, else on yourself, self skills without a target; walks into range first if needed. A slot shows the cooldown sweep and seconds, red without mana, darker while an enemy skill has no monster selected or it is out of range; hovering shows the skill's tooltip |
| Drag on the skill bar | M12: drag a slot onto another (swap) or off the bar (clear); right click clears a slot; each change is one SKILL_BAR_SET_REQ, stored on the server |
| K (or the bar button "Yetenekler") | M12: skill window (left side; a gold marker with the number on the button while skill points are unspent): the class's two branches, "+" raises a skill one rank (SKILL_LEARN_REQ; asked once per session: spent points cannot be taken back), locked tiers show "Bu yolda N puan gerekli", drag a learned skill onto a slot (of the HUD bar or the bar row inside the window) |
| Right mouse drag | orbit the camera (cursor hidden and confined while dragging); a right click that does not move stays an attack |
| Mouse wheel | zoom (2–28 m) |
| Shift (held) | walk (1.4 m/s) instead of run (5.5 m/s) |
| R (or the bar button "Yürü / Koş") | toggle the default between run and walk (the button shows a boot while running, footprints while walking) |
| Space | jump (cosmetic: a short arc, no gameplay effect, the position stays on the ground) |
| X (or the bar button "Otur / Kalk") | sit down on the ground / stand up (M9: STANCE_REQ, the server seats the character; doubled regeneration; refused in combat); the button is lit while seated |
| F3 | toggle the info panel (off by default) |
| I (or the bar button "Envanter") | inventory window (right side; the button is lit while it is open) |
| E (or the bar button "Ganimet") | loot: take everything from the open loot window, else open the nearest bag (the button pulses while a bag you may take from is within reach); M14: with no bag in reach, E talks to a merchant within 6 m |
| Left / right click on a merchant | M14: walk into talk range and open its shop (the shop window on the left, the inventory on the right); hovering shows "<Ad> — <Unvan>" / "Konuşmak için tıkla" |
| Shop window | M14: click a row to buy (stacks: the quantity box "Kaç tane?", Enter buys; gear above 50 Akçe asks first); right click a bag item or drag it onto the shop to sell (stacks: "Kaç tane satılsın?"; rare / dear items ask first) |
| Double click / right click on a potion in the bag | M14: drink it (with a shop open a right click sells instead); drag a potion onto a bar slot to drink it with 1-8 (the slot shows how many are left and the shared 10 s cooldown) |
| C (or the bar button "Künye") | character sheet (M11, left side; a glowing number on the button while stat points are unspent). In it: "+" one pending point, Shift+"+" five (or the rest), right click on "+" or a row / "−" one back, "Onayla" stores them (STAT_ALLOCATE_REQ; asked once per session: spent points cannot be taken back), "Vazgeç" drops them; hovering an attribute explains it for the class |
| Esc (or the bar button "Menü" for the menu) | closes the loot window, then the shop window (M14), then "Tuşlar", then the Yetenekler window (M12), then the Künye, then the inventory, then drops the target (ATTACK_STOP_REQ if attacking), then opens the menu: "Devam Et", "Karakter Seçimine Dön" (LEAVE_WORLD_REQ), "Oyundan Çık" |
| F9 | (only with `--debug-ghosts`) inject a POSITION_CORRECT 3 m behind the player |

Jump is local only (nothing is sent, other players do not see it). Moving by hand (WASD or a click
on the ground) stops the walk into range but not the auto-attack: the server keeps it armed and
swings again once the target is in range (docs/PROTOCOL_COMBAT.md §3.1). While dead the world is
locked (death screen; "Yeniden Doğ" after 5 s). Every HUD button has a tooltip (name, key, one line,
its state); the experience bar's tooltip has the exact numbers.

World flags:

| Flag | Meaning |
|---|---|
| `--debug-ghosts <n>` | Debug: `n` (≤ 64) synthetic remote players wander near the character. Their ENTITY_SPAWN / ENTITY_MOVE / ENTITY_DESPAWN packets are injected into GameClient's event queue and take the exact path of network messages (exercises interpolation, labels, both races, enter/leave). Off by default. |
| `--fixed-dt <s>` | Automation: fixed simulation step per frame (e.g. `0.0333`) so scripted movement is deterministic. |
| `--vsync on\|off` | Overrides `window.vsync` (measure performance with `off`). |
| `--no-shadows` | Disables the sun shadow map (also `[graphics] shadows = false` in client.toml); blob shadows stay. |
| `--msaa 1\|2\|4\|8` | Overrides `[graphics] msaa` (multisampling of the 3D world pass, default 4; 1 = off). |

Optional art: `assets/ui/login_bg.png` (full-screen background, cover-fit with a dark vignette) and
`assets/ui/race_aydor.png` / `race_kargan.png` (race portraits); `assets_src/ui/` is searched too.
Missing files fall back to the procedural backdrop and silhouettes; `--no-art` forces the fallbacks.
The layout is designed for a logical canvas of at least 960x600: if window size / UI scale would be
smaller (e.g. 1024x640 at 150%) the UI scale is reduced to fit.

Flags: `--warp` (force the WARP software rasterizer), `--no-debug-layer` (Debug builds enable the
D3D11 debug layer when the Windows "Graphics Tools" optional feature is installed).

If hardware device creation fails (e.g. some remote sessions) the client falls back to WARP; the
adapter in use is logged.

#### Automated screenshot

`vo_client` is a GUI-subsystem program, so PowerShell does not wait for it by default. Use
`Start-Process -Wait`:

```powershell
$p = Start-Process build\x64-debug\bin\vo_client.exe -PassThru -Wait `
       -ArgumentList '--screenshot', "$env:TEMP\vo_shot.png", '--exit-after-frames', '5'
$p.ExitCode   # 0 = PNG written; 5 = screenshot failed; 3 = init failed
```

The back buffer of frame *n* is captured (before Present) and written as a 32-bit PNG via WIC.

## Tests

`powershell -File build.ps1 -Test` runs everything registered with ctest: the Catch2 unit tests
(one ctest entry per test case) plus smoke tests (`vo_server --run-seconds 1`, `assetc`,
`botclient`). Run the unit tests directly with `build\x64-debug\bin\vo_tests.exe`.

M7 robustness tests (all in ctest):

- `tests/net/ConnectionTests.cpp` (`[connection]`): coalesced writes, frame order / AEAD counters
  under concurrent traffic, the 256 KB cap, abort / peer reset / graceful close with a write in
  flight. Shake out races with repeated Release runs, e.g.
  `build\x64-release\bin\vo_tests.exe "[server],[connection]"` in a loop.
- `tests/server/ServerRobustnessTests.cpp` (`[robust]`): slowloris, FIN before the handshake, dead
  peers, forced stop; `tests/server/world/WorldTests.cpp` (`[overload]`): slow ticks / tick skipping;
  `tests/db/WorldDbOutageTests.cpp` (`[outage]`, needs the test database): DB connections killed
  while a player is in the world.
- `vo_fuzz_tests` (ctest `fuzz.*`): an in-process server driven with garbage before the handshake
  and seeded scripts of mutated / truncated / oversized / out-of-state frames after it; every
  connection's outcome is checked against PROTOCOL.md. ctest runs a short slice (~25k frames). A
  long run (Release, a few minutes):

  ```powershell
  $env:VO_FUZZ_SCALE = 15; $env:VO_FUZZ_SEED = 7; build\x64-release\bin\vo_fuzz_tests.exe
  ```

  It prints connections, frames, the outcomes and the process memory after each half.
- botclient scenarios `leaverace` (MOVE_REQ right behind LEAVE_WORLD_REQ / LOGOUT) and `dupworld`
  (duplicate login while in the world, watched by a third player) as `integration.leaverace` /
  `integration.dupworld`.

M9 combat tests (docs/PROTOCOL_COMBAT.md, docs/SERVER_WORLD.md §8b): `[combat]` / `[monsters]`
unit tests (`tests/game/CombatTests.cpp`, `tests/server/world/CombatZoneTests.cpp` with a fake clock,
`tests/server/CombatServerTests.cpp` on an in-process server, `tests/db/PostgresCombatTests.cpp`),
`content.Monster spawns:*` (gamedata/monsters.toml against the compiled zone 1), and
`integration.combat` / `integration.db.combat` (botclient `combat`: kill a Kül Çömezi, die to the
Kor Bekçisi, respawn; with the database also `vo_admin set-level`). Run one by hand:

```powershell
$env:VO_TEST_ASSET_DIR = "$PWD\assets"
powershell -NoProfile -File tests\net\integration\RunScenario.ps1 -ServerExe build\x64-debug\bin\vo_server.exe `
    -BotExe build\x64-debug\bin\botclient.exe -Scenario combat -WorkDir $env:TEMP\vo_it_combat
```

M10 loot tests (docs/PROTOCOL_LOOT.md, docs/SERVER_WORLD.md §8c): `[loot]` unit tests
(`tests/game/LootTests.cpp` for the loader, rolls and stack rules, `tests/server/world/LootZoneTests.cpp`
with a fake clock, `tests/server/LootServerTests.cpp` on an in-process server,
`tests/db/PostgresLootTests.cpp`), the loot codecs in `[net][messages][loot]`, and
`integration.loot` / `integration.db.loot` (botclient `loot`: kill Kül Çömezis, open and empty the
container, check ITEM_UPDATE / COINS / LOOT_TAKE_RES against LOOT_CONTENTS and the despawn, enter
again with the same balance and stacks; with the database on a new server process, plus vo_admin
`list-items`, `set-coins` and `give-item <character> 9 25`). `vo_server` also needs
`gamedata/loot.toml` now (exit 6 without it; `world.loot_file` overrides the search). The
`-Database` scenarios read the test connection string from `<RepoRoot>\config\server.local.toml`;
in a git worktree without its own copy pass the main checkout as `-RepoRoot` when running one by
hand (ctest passes the worktree, where they are skipped with exit 77).

```powershell
$env:VO_TEST_ASSET_DIR = "$PWD\assets"
powershell -NoProfile -File tests\net\integration\RunScenario.ps1 -ServerExe build\x64-debug\bin\vo_server.exe `
    -BotExe build\x64-debug\bin\botclient.exe -Scenario loot -WorkDir $env:TEMP\vo_it_loot
```

M11 stats tests (docs/PROTOCOL_STATS.md, docs/SERVER_WORLD.md §8d): `[stats]` unit tests (the shared
formulas, allocation checks and the tuning table in `tests/game/CombatTests.cpp` —
`$env:VO_TUNING_TABLE = 1` prints the level-10 builds against the Kor Bekçisi —, item bonuses and
requirements in `ItemsTests.cpp`, the zone with a fake clock in `CombatZoneTests.cpp`,
`tests/server/StatsServerTests.cpp` on an in-process server, the account-service suite on both
backends, `tests/db/PostgresStatsTests.cpp` incl. migration 0005 over an M10 database), the codecs
in `[net][messages][stats]`, and `integration.stats` / `integration.db.stats` (botclient `stats`, a
fixed `world.seed`: hunt to level 2 respawning after deaths, spend 3 points, CHAR_STATS / SELF_STATS
/ damage follow, re-enter, the flood cap; with the database also vo_admin `show-character`,
`set-level 10` and spending the 24 new points, `reset-stats`). The stats scenario takes 1–3 minutes
(ctest timeouts 300 s / 400 s).

```powershell
$env:VO_TEST_ASSET_DIR = "$PWD\assets"
powershell -NoProfile -File tests\net\integration\RunScenario.ps1 -ServerExe build\x64-debug\bin\vo_server.exe `
    -BotExe build\x64-debug\bin\botclient.exe -Scenario stats -WorkDir $env:TEMP\vo_it_stats
```
M12 skills tests (docs/PROTOCOL_SKILLS.md, docs/SERVER_WORLD.md §8e): `[skills]` unit tests — the
data file and its strict loader, the rules and the v6 codecs in `tests/game/SkillsTests.cpp`, the
balance simulation in `tests/game/SkillBalanceTests.cpp` (`$env:VO_TUNING_TABLE = 1` prints the
level-10 branch builds against the Kor Bekçisi), every mechanic in the zone with a fake clock in
`tests/server/world/SkillZoneTests.cpp`, `tests/server/SkillServerTests.cpp` on an in-process server,
`tests/db/PostgresSkillsTests.cpp` incl. migration 0006 over an M11 database —, the fuzzer, and
`integration.skills` / `integration.db.skills` (botclient `skills`, `world.seed` 1201: hunt to level
2, learn Kalkan Duvarı, its buff, expiry and remembered cooldown, the flood cap; with the database
also vo_admin `show-character`, `set-level 10`, spending 8 points through both branches,
`reset-skills`). The skills scenario takes 1–3 minutes (ctest timeouts 420 s / 520 s). `vo_server`
needs `gamedata/skills.toml` (exit 6 without it; `world.skills_file` overrides the search); the
client and the tools load the same file.

```powershell
powershell -NoProfile -File tests\net\integration\RunScenario.ps1 -ServerExe build\x64-debug\bin\vo_server.exe `
    -BotExe build\x64-debug\bin\botclient.exe -Scenario skills -WorkDir $env:TEMP\vo_it_skills
```

M13 zones tests (docs/PROTOCOL_ZONES.md, docs/SERVER_WORLD.md §8f): `[zones]` unit tests — the data
file, its strict loader, the gate rule and the v7 codecs in `tests/game/ZonesTests.cpp`, three
synthetic zones with a fake clock in `tests/server/world/ZoneChangeTests.cpp`,
`tests/server/ZoneServerTests.cpp` on an in-process server —, the fuzzer, and `integration.zones` /
`integration.db.zones` (botclient `zones`, both races and `--phase pair`; with the database also
vo_admin `show-character` and `move-character`). `vo_server` loads every zone of
`gamedata/zones.toml` (exit 6 if a terrain or a marker is missing; `world.zones_file` overrides the
search), so the compiled assets need `terrain/zone1..3`. The end-to-end scenarios written for
Ortaocak, the load test and the UI flows set `world.start_zone_override = 1` in their generated
config (new characters start in zone 1 as before M13); only `zones` runs with the homelands. The
`load` scenario adds `--zones` (commuters and homeland residents, 40 s window) when the terrains of
zones 2 and 3 are present. The UI flow `zone_gate` (`StartZone = 0` in `run-ui-flows.ps1`) walks an
Aydor from Tanyeri through `gate_esik` to Eşik and back through `gate_aydor`. The M13 client flows
(docs/DEVELOPMENT_PLAN.md §27, docs/CLIENT_RENDERING.md §14): `zones` (homelands on; run 1 an Aydor
round trip Tanyeri → Ortaocak → Tanyeri with banner, gate labels / hints / veils, the held gate and
loading screens, the arrival line, a simulated failed change and the zone on the character card; run 2
a Kargan on the same account, both homelands on the character list, Demirağız → Ortaocak →
Demirağız), `zones_refused` (an Aydor in Ortaocak refused at the Kargan gate, twice; 1280x720 and
1024x640 @150 %) and `zones_hunt` (a fight with an Anız Yanığı in Tanyeri and a Kırağı Kayası in
Demirağız, with loot; real time).

```powershell
powershell -NoProfile -File tests\net\integration\RunScenario.ps1 -ServerExe build\x64-debug\bin\vo_server.exe `
    -BotExe build\x64-debug\bin\botclient.exe -Scenario zones -WorkDir $env:TEMP\vo_it_zones
```

M14 merchants and consumables tests (docs/PROTOCOL_SHOP.md, docs/SERVER_WORLD.md §8g): `[shop]` unit
tests — npcs.toml and its strict loader, the NPC spot check, consumables, prices, buy / sell / use
rules, bar item slots and the v8 codecs in `tests/game/ShopTests.cpp`, NPCs / shops / consumables
with a fake clock in `tests/server/world/ShopZoneTests.cpp`, `tests/server/ShopServerTests.cpp` on an
in-process server, `tests/db/PostgresShopTests.cpp` incl. migration 0007 over an M13 database —, the
fuzzer, and `integration.shop` / `integration.db.shop` (botclient `shop`, about 25 s, deterministic:
no monsters; a new Aydor mage in Tanyeri, money from selling its spare staff, the missing hp / mp from
the testing hook `world.testing_enter_vitals_percent = 10` that RunScenario writes for this scenario
only; with the database also vo_admin `show-character` and a check on a new server process).
`vo_server` needs `gamedata/npcs.toml` (exit 6 without it or with an unusable merchant spot;
`world.npcs_file` overrides the search); the client and the tools load the same file.

Cleanup round fix1 (DEVELOPMENT_PLAN.md §33): `integration.shoperrors` / `integration.db.shoperrors`
(botclient `shoperrors`, ~60 s / ~80 s: a mage in Ortaocak gets ServerError for its first buy, sale and
use of a Kızılca Şerbet through the testing hook `world.testing_fail_first_item_write = 16`, AlreadyFull,
OutOfRange inside the open shop, YouAreDead after dying to the Kor Bekçisi; with the database also
PurseFull and BagFull after `vo_admin set-coins <name> 999999990`), `integration.zonesrace` (botclient
`zonesrace`, ~6-8 min: `--phase same` two bots into one gate in the same tick and back, `cross` both
directions of a gate pair at once with an observer in each zone, then on a server with
`world.testing_zone_arrival_delay_ms = 1500` `logout` (LOGOUT / LEAVE_WORLD_REQ right
after ZONE_CHANGE_BEGIN) and `drop` (a dropped connection, new logins during and after the change)).
The hunting scenarios (combat, loot, stats, skills, shoperrors) keep out of every gate trigger + 3 m
and fail with "the bot changed zone unexpectedly" on any ZONE_CHANGE_BEGIN. `vo_tests "[economy]"`
with `VO_ECONOMY_TABLE=1` prints the income / price tables of §33.

M14 client (docs/PROTOCOL_SHOP.md §8, docs/CLIENT_RENDERING.md §16): `[client][shop]` unit tests in
`tests/client/ShopViewTests.cpp` (merchants in view, the shop session, the potion cooldown, the quantity
box, stock entries, the buy / sell decisions, every text of §8.3, the potion tooltip, the Esc order, the
`scene` commands, the merchant clips). UI flows (`run-ui-flows.ps1`; `ServerOpts.EnterVitals` writes
`world.testing_enter_vitals_percent`, so a character enters with 10 % hp / mp and a potion has something
to restore; real time, the potion cooldown is a server timer): `shop` (Ortaocak; a new warrior sells its
kit, buys potions, drinks them from the bar and the bag until none is left, the refusals, Esc order,
walking away; 1280x720 and 1024x640 @150 %), `shop_homes` (PostgreSQL test database, skipped without one;
homelands on: run 1 creates an Aydor warrior and a Kargan mage, vo_admin seeds them; run 2 Tanyeri -
gear that asks first, equipping, selling with quantity and confirmation, typed quantities, "Canın zaten
dolu."; run 3 Demirağız at 1024x640 @150 % - a full bag, selling a stack, a mana potion on the bar; run 4
hunts until a potion drops), `two_clients_shop` (side B sees the "+hp" over side A). The homelands' stalls
are placed by `tools/blender/gen_zones.py` (`SHOP_STALLS`); after changing them run it (headless) and
`tools\build-assets.ps1`, then move the merchants in `gamedata/npcs.toml` beside them (the server checks
every spot at startup).

Cleanup round fix1 (client UI, DEVELOPMENT_PLAN §32) adds two flows: `fix1_windows` (one run from 2560x1440
down to 1920x1080, 1280x720 and 1024x640 @150 %: the HUD scale, the window placement manager, a title-bar
drag, the seated name label, the character list without a lingering loading line) and `fix1_shop` (1280x720,
1920x1080, 1024x640 @150 %: the merchant framed beside the shop pair, the italic greeting, the pickup line
format, a scrolling stock through the client-only `scene shopfakestock` hook). Screenshots:
`docs/screenshots/fix1/ui_*.png`.

```powershell
powershell -NoProfile -File tests\net\integration\RunScenario.ps1 -ServerExe build\x64-debug\bin\vo_server.exe `
    -BotExe build\x64-debug\bin\botclient.exe -Scenario shop -WorkDir $env:TEMP\vo_it_shop
```

Load test with three zones (Release; a server config without `start_zone_override` starts the bots
in their homelands, with `= 1` in Ortaocak; `--zones` makes every fifth bot commute between Eşik and
its homeland and another fifth go home and stay):

```powershell
build\x64-release\bin\botclient.exe --host 127.0.0.1 --port 15001 --server-key <hex> `
    --terrain assets\terrain\zone1\zone1.vterrain --bots 200 --duration 60 --zones
```

## Packaging

```powershell
powershell -ExecutionPolicy Bypass -File tools\package\package.ps1 -ServerPublicKey <hex> [-ServerHost <host>] [-Zip]
powershell -ExecutionPolicy Bypass -File tools\package\verify-package.ps1 -Package dist\VedatOnline-<version>
```

`package.ps1` builds Release and the assets and writes `dist/VedatOnline-<version>/` with `client/`
(vo_client.exe, assets, config/client.toml with the given host and server public key,
README-TR.txt) and `server/` (vo_server.exe, vo_admin.exe, config/server.toml,
config/server.local.toml.example, db/migrations, the zone 1 terrain, README-TR.txt); both get
`gamedata/items.toml` and `gamedata/monsters.toml`. It refuses to
produce a package containing keys, `*.local.toml` or a database password. `verify-package.ps1`
copies the package outside the repository and runs the packaged server and client against each
other (temporary local config and key pair, in-memory accounts, the happy-path UI flow).
`tools\build-assets.ps1` builds assetc first and fails loudly if assetc is stale, reports a
failure, or the required outputs are missing.

## Client scenes, UI demo and input scripts

`vo_client` flags added in M3:

| Flag | Meaning |
|---|---|
| `--scene login\|uidemo` | Start scene. `login` (default) is the login screen; `uidemo` shows every UI widget. |
| `--size WxH` | Client-area size in physical pixels (overrides `window.width/height`). |
| `--ui-scale <f>` | Multiplies the monitor DPI scale (the process is per-monitor-DPI-aware v2). `1.5` on a 96-DPI monitor renders the UI at 150%. |
| `--script <file.voscript>` | Plays timed synthetic input through the same event queue as real input, then exits. While a script runs, real mouse/keyboard input is ignored and the clipboard is in-memory. |
| `--screenshot-dir <dir>` | Base directory for relative `screenshot` paths in a script. |
| `--settings <file>` | User settings file ("Beni hatırla"), default `%LOCALAPPDATA%\VedatOnline\client_settings.toml`. |
| `--no-art` | Ignore the optional UI images and draw the procedural fallbacks. |
| `--protocol-version <n>` | Testing: announce another protocol version in HELLO (the server answers "version mismatch"). |

Script commands (one per line, `#` comments; see `client/automation/Script.h`): `wait <frames>`,
`move x y`, `click x y [left|right|middle]`, `dblclick x y`, `mousedown x y`, `mouseup x y`,
`wheel <notches>`, `key <chord>` (e.g. `key tab`, `key shift+tab`, `key ctrl+a`, `key VK_RETURN`),
`keydown <chord>`, `keyup <chord>`, `text <utf8>` (rest of line, optional quotes),
`screenshot <file.png>`, `resize w h`, `uiscale <f>`, `log <msg>`, `quit`. The end of the script
also quits. Script errors exit with code 1. M6 adds `hold <chord> <frames>` (key held for n frames),
`drag x0 y0 x1 y1 <frames> [button]` (button held while the mouse moves over n frames) and
`scene <command>` (handed to the active scene; the world scene understands `camera <yawDeg|behind>
<pitchDeg> <distance>`, `walkto <x> <z>` (same path as a click), `correct [metres]` (inject a
POSITION_CORRECT), `expectnear|expectfar <x> <z> <r>`, `expectwalkable`, `expectgrounded` (feet on
the terrain, camera above it), `scan <x> <z> <r>` (terrain survey) and `log` (player, camera,
remotes, `world: perf` FPS/draw calls/triangles)). A rejected scene command fails with code 6.
M7 adds world commands for multi-client flows - `expectremote <name> <race>`,
`expectnoremote <name>`, `markremote <name>` (then states `remotemoved` and `remotenear4|8|16`),
`walktoremote <name> <stopShort>`, `orbitremote <name> <radius> <angleDeg>`,
`expectnearremote <name> <r>`, `walkrel <dx> <dz>` (the walks follow a path found on the walk mask;
`walktoremote` / `orbitremote` take the nearest clear walkable spot, and `orbitremote` other angles,
when the point is blocked, e.g. by a house next to the other player)
- and application commands valid in any scene: `scene devicelost` (simulated device loss at the
next Present) and `scene window minimize|restore|deactivate|activate` (real window messages).
Polish round 1 adds: create screen `setlook <n>` (picks look n if the race has it, else logs and
keeps the current one) and `expectpreviewmesh` (also on the select screen: the 3D preview shows the
race + look's mesh); world `walkpath <x> <z>`, `expectlocalmesh`, `expectremotemesh <name>`,
`jump`, `sit`, `expectanimrate <min> <max>`, `footprobe start` / `footprobe check <ratio>` (planted
feet slide at most `ratio` x the body speed), `cameracollision on|off`, `expectcameraoutside`,
`expectcamerapulled`, `expectlocalfaded` / `expectlocalvisible`; states `looks<N>` / `look<n>`
(create), `banner`, `jumping`, `sitting` / `seated` (world).

State-aware commands keep network-dependent scripts independent of timing:

| Command | Meaning |
|---|---|
| `waitfor <state> <frames>` | Wait until the state holds; fail after the timeout. `<state>` is one token or several joined by `+` (all must hold). Tokens: the scene name (`login`, `charselect`, `charcreate`, `world`, `uidemo`), `idle` (no request in flight, no modal), `busy`, `modal`, plus per scene: `connecting`/`loggingin` (login), `chars<N>`, `loaded`, `selected<N>` (select), `race`/`details`, `race.aydor`/`race.kargan` (create), `loading`/`ready`/`failed`, `menu`, `moving`/`standing`, `running`/`walking`, `dest`/`nodest`, `remotes<N>` (world). |
| `expect <state>` | Fail unless the state holds now. |
| `clickon <widget> [frames]` | Click the centre of a named widget (topmost modal first); waits up to 120 frames for it to be visible and enabled. Names: `login.username`, `login.submit`, `select.slot0`, `select.enter`, `create.race.kargan`, `create.class.3`, `create.name`, `create.submit`, `world.menu.resume`, `world.menu.charselect`, `world.menu.quit`, `messagebox.button0`, `confirm.input`, `confirm.ok`, ... (see the scenes). |
| `expecttext <widget> <text>` | Fail unless the widget's text contains `<text>` (labels, buttons, text boxes, panel titles; e.g. `messagebox.text`). |

A failed state command exits with code 6. A failed waitfor reports the state tokens it saw.

The UI walkthrough used for M3 verification:

```powershell
$p = Start-Process build\x64-debug\bin\vo_client.exe -PassThru -Wait -ArgumentList `
       '--scene', 'uidemo', '--size', '1280x720', '--script', 'client/automation/uidemo.voscript', `
       '--screenshot-dir', "$env:TEMP\vo_uidemo"
$p.ExitCode   # 0 = all screenshots written
```

#### UI flows against real servers

```powershell
powershell -ExecutionPolicy Bypass -File tools\dev\run-ui-flows.ps1 [-Config Release] [-Only happy,delete] [-OutDir <dir>]
```

Runs `client/automation/flows/*.voscript`, each against its own `vo_server` on a free 127.0.0.1
port with a temporary config and key pair (never `config/` or port 15001): the happy path (fresh
user → creation → Kargan → create → select → world → Esc menu → back → log out, then a second run checking
"Beni hatırla"), the same without art, wrong password, input validation, name taken, delete,
server down, handshake timeout + Esc cancel (a listener that never answers), wrong pinned key,
maintenance, server full (a bot holds the only slot), protocol version mismatch, too many
attempts, duplicate-login kick (botclient logs in, `log HOOK kick`), server killed while in the
world (`log HOOK kill`), the layout script at 1024x640, 1920x1080, 1920x1080@150%, 1024x640@150%
(with and without art), the uidemo walkthrough, and the M6 world flows: `world_tour` (camera
default/rotated/zoomed, resize, walk, run, strafe, click-to-move, running into a house wall,
down and up a slope, Esc menu, back to select), `world_ghosts` (`--debug-ghosts 8`: remote players
of both races walking, running, stopping, leaving and coming back, then a POSITION_CORRECT),
`world_preview` (the 3D preview in create and select for both races and their looks, turned by
dragging) and `world_perf` (1920x1080, vsync off, 8 ghosts; logs `world: perf` lines with
FPS, draw calls and triangles). M7 flows: `resilience` (resize while the world loads,
minimise/restore and Alt+Tab while walking, a simulated device loss and logging in again), `msaa`
(the same view with `--msaa 1` and `--msaa 4`, a fresh server per run) and `two_clients` (one
server, two vo_client windows with their own settings and scripts `two_aydor` / `two_kargan`: an
Aydor walks to and around a Kargan, each side checks the other's name, race and movement, then
the Aydor leaves and the Kargan sees it despawn). Polish round 1 flows: `looks` (an Aydor with
look 0 and a Kargan with look 1 — look 0 while the assets have only one look — checked in the
select preview and in the world) and `polish` at 1280x720, 1024x640 and 1024x640@150% (story texts,
loading screen, zone banner, HUD zone name, walk/run foot-slide probes, the camera against a house
wall without and with collision, the character dithering out, jump and sit). M8 flows: `items`
at 1280x720, 1600x900 and 1600x900@125% (a warrior enters with sword + shield, opens the inventory,
reads a tooltip, drags the two-handed axe into the hand, double / right clicks the sword and shield
back, an illegal move refused locally and by the server, then leaves and re-enters: same
equipment), `classes` (the creation figure holding each class's kit, both races, 8 shots) and
`two_clients_items` (`two_items_a` / `two_items_b`: the other client sees the weapons on spawn and
the switch to the axe). M9 "İlk Kan" flows (real time, no `--fixed-dt`): `combat` at 1280x720 and
1600x900@125% (a warrior leaves the village, targets a Kül Çömezi — Esc / Tab checked —, F and
Ağır Darbe, damage numbers, the corpse, the xp bar), `combat_mage` (a Kargan mage: Köz Oku cast
bar and bolt, then the staff's ranged auto-attack), `death` at 1280x720 and 1024x640@150% (a priest
walks to the Kor Bekçisi and dies; locked movement and inventory, an early respawn refused with
TooEarly, "Yeniden Doğ", back in the village with half its hp), `levelup` (1920x1080, `scene hunt`
until level 2, resting on the ground between fights; per-run `TimeoutSec`) and `two_clients_combat`
(`two_combat_a` / `two_combat_b`: B watches A kill a thrall — hp bar, swings, death — and heals A
with Soluk Güneş). M10 "Ganimet" flows: `loot` at 1280x720 and 1024x640@150% (a warrior kills a
Kül Çömezi; its bag — gold label, ours — on the ground, E opens the loot window, Esc closes it, E
again with the inventory open takes everything: pickup lines, the Akçe balance, the bag
disappears; then `scene lootfarm` until two Kül Tozu pickups merged into one stack: slot count and
tooltip), `two_clients_loot` (`two_loot_a` / `two_loot_b`: B sees A's bag locked, its open is
refused with "Bu ganimet henüz başkasının", B sits down as a signal, A takes everything, the bag
disappears for both) and `loot_rich` (needs the PostgreSQL test database, else reported as
skipped: the server runs on its own `vo_it_*` schema of `vedatonline_test` — URL from
`VEDAT_TEST_DB_URL` or `db.test_connection_string` of the first `config/server.local.toml` found
upwards, passed only as `VEDAT_DB_URL` to the child processes, never printed —; run 1 creates a
warrior, vo_admin (run key `Admin`) seeds it with level 10, 1 234 567 Akçe, three rare Kor Çekirdeği
and an uncommon Bekçi Kalkanı; run 2 shows the grouped balance and the rarity tooltips, then
`scene lootfarm Kül Çömezi rows 2` stops with a multi-row bag open; the schema is dropped with
`vo_admin testdb drop-schema` afterwards). The script builds `assets/` with assetc first if it is missing.
Since M11 every flow's server runs with a fixed `world.seed` (4711; run/flow option `Seed` in
`ServerOpts`), so monster placement, wandering and the combat rolls repeat from run to run; `scene
hunt` (the `levelup` flow) prefers a Kül Çömezi without a neighbour, rests below 75 % hp and
respawns after an unlucky death instead of ending the hunt (the flow checks the level-up, not the
fight); the flow's refill check is the automation state `levelupfull` (a full hp bar seen within 2 s
of LEVEL_UP), so a hit by another monster right after the level-up no longer fails it. M11 flows:
`stats`, `stats_rich`, `dodge` (docs/DEVELOPMENT_PLAN.md §21). M12 "Yetenek Yolları" flows
(docs/DEVELOPMENT_PLAN.md §24): `skills_bar` at 1280x720 and 1024x640@150% (no database: the default
bar, tooltip, the window at level 1, drag / swap / clear, a use with the cooldown sweep, a hunt to
level 2 with the fourth toast line, the first "+" with its confirmation, relog) and, with the test
database, `skills_warrior` (3 runs; the last at 1024x640@150%), `skills_rogue`, `skills_mage`,
`skills_priest` (each: create, `vo_admin set-level 10`, spend the 9 points, use the branch's skills).
Skill automation: `skills open|close`, `skilllearn`, `barset`, `useslot`, `pullpack <monster> <n>`,
`expectrank`, `expectskillpoints`, `expectslot`, `expectslotlook`, `expectcooldown`, `expecteffect` /
`expectnoeffect` (model and HUD icon), `expecteffecttip`, `expectart`, `expectskillmessage`,
`skilllog` (client/world/SkillsView.h `parseSkillsCommand`); states such as `skillpoints<N>`,
`skillmarker`, `canraise` / `noraise`, `locked<branch>_<tier>`, `ready_<key>` / `cd_<key>`,
`selfeffect_<kind>`, `targeteffect_<kind>`, `art_<mesh>`, `toastabovelabel`, counters `learned<N>`,
`barsets<N>`, `skilluses<N>`, `usehits<N>`, `effectticks<N>`, `absorbs<N>`, `leaps<N>`
(WorldScene::skillsStates).

Shop automation (M14): `talk <npc|nearest>`, `walktonpc <npc> [m]`, `shopopenforce <npc>`, `shopbuy
<template> [n]`, `shopbuyforce <template> <n>`, `shopsell <bagSlot> [n]`, `shopclose`, `usepotion
<template> [bagSlot]`, `baritem <slot> <template|none>`, `hovernpc <npc|none>`, `expectshop <npc|none>`,
`expectshopmessage`, `expectbarcount <slot> <n>`, `expectpotioncd <min> <max>`, `shoplog`
(client/world/ShopView.h `parseShopCommand`; names with spaces as written); `camera at:<name>[@deg]`
aims at a merchant too (underscores for spaces). States `npcs<N>`, `shopopen` / `shopclosed`,
`shopbusy` / `shopidle`, `npcwalking`, `npchover`, `npctalking`, `npcinteract`, `quantitybox`,
`potioncd` / `potionready`, counters `bought<N>`, `sold<N>`, `buyrefused<N>`, `potionuses<N>`,
`potionrefused<N>`, `otherpotions<N>`, ... (WorldScene::shopStates).

Unattended runs never block on a crash dialog: the test executables (`tests/NoCrashDialogs.cpp`)
and `vo_client --script` turn off the CRT abort / assert message boxes and Windows Error Reporting
dialogs (`_set_abort_behavior`, `_CrtSetReportMode` to stderr, `SetErrorMode`), so a crash fails
the run with a message instead of waiting for a click.

Repeat a flow to check it is stable, e.g. `two_clients` 15 times:

```powershell
1..15 | % { powershell -ExecutionPolicy Bypass -File tools\dev\run-ui-flows.ps1 -Only two_clients -OutDir "build\ui-flows\two_$_" }
```
`-Only` takes names separated by commas. Screenshots, client and server logs go to
`build/ui-flows/<config>-<timestamp>/<flow>/` (default) with a `summary.txt`. The script fails if a
client exits non-zero, a run times out, a client log shows D3D debug-layer messages, live objects
or errors, or a server it started is still running at the end.

Debug builds log a CRT heap-allocation summary for the last 120 frames at shutdown (`heap: ...`) and
the D3D11 live-object report (`no live objects reported`). Shaders (`client/shaders/*.hlsl`) are
compiled by `fxc` from the Windows SDK at build time with warnings as errors.