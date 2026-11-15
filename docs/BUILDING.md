# Building VedatOnline

## Requirements

- Windows 10/11 x64
- Visual Studio 2026 (or 2022) with the **Desktop development with C++** workload
  (MSVC, Windows SDK 10.0.26100, the bundled CMake, Ninja and vcpkg)
- Git, internet access on the first build (vcpkg downloads and builds the dependencies)

Nothing has to be on `PATH`: `build.ps1` loads the VS x64 developer environment itself.

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
variable (the env var wins). The server does not use the database yet: accounts and characters live
in memory and are lost when it stops. With `auth.dev_auto_register = true` (local config only) an
unknown username with a valid password creates the account on first login.

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
- **Character create**: race cards (Aydor, Kargan), then class, face/hair (◀ ▶), name with live
  rule hints, Yarat / Geri. Every CHAR_CREATE result has its own message.
- **World** (M6, details in [CLIENT_RENDERING.md](CLIENT_RENDERING.md)): a loading screen with a
  progress bar while the zone, props and characters load, then the 3D world: terrain, ~600 props,
  sky, sun shadows, the own character and other players with name labels. HUD: name/level top
  left, an info panel (position, zone, RTT, players, FPS, draw calls; F3 toggles it) and a help line.
  The select/create screens show the real 3D model (race, face, hair; idle animation; drag to turn).
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
| Right mouse drag | orbit the camera (cursor hidden and confined while dragging) |
| Mouse wheel | zoom (2–28 m) |
| Shift (held) | walk instead of run |
| R | toggle the default between run and walk |
| F3 | toggle the info panel |
| Esc | menu: "Devam Et", "Karakter Seçimine Dön" (LEAVE_WORLD_REQ), "Oyundan Çık" |
| F9 | (only with `--debug-ghosts`) inject a POSITION_CORRECT 3 m behind the player |

World flags:

| Flag | Meaning |
|---|---|
| `--debug-ghosts <n>` | Debug: `n` (≤ 64) synthetic remote players wander near the character. Their ENTITY_SPAWN / ENTITY_MOVE / ENTITY_DESPAWN packets are injected into GameClient's event queue and take the exact path of network messages (exercises interpolation, labels, both races, enter/leave). Off by default. |
| `--fixed-dt <s>` | Automation: fixed simulation step per frame (e.g. `0.0333`) so scripted movement is deterministic. |
| `--vsync on\|off` | Overrides `window.vsync` (measure performance with `off`). |
| `--no-shadows` | Disables the sun shadow map (also `[graphics] shadows = false` in client.toml); blob shadows stay. |

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
`world_preview` (the 3D preview in create and select for both races with different faces/hair,
turned by dragging) and `world_perf` (1920x1080, vsync off, 8 ghosts; logs `world: perf` lines with
FPS, draw calls and triangles). The script builds `assets/` with assetc first if it is missing.
`-Only` takes names separated by commas. Screenshots, client and server logs go to
`build/ui-flows/<config>-<timestamp>/<flow>/` (default) with a `summary.txt`. The script fails if a
client exits non-zero, a run times out, a client log shows D3D debug-layer messages, live objects
or errors, or a server it started is still running at the end.

Debug builds log a CRT heap-allocation summary for the last 120 frames at shutdown (`heap: ...`) and
the D3D11 live-object report (`no live objects reported`). Shaders (`client/shaders/*.hlsl`) are
compiled by `fxc` from the Windows SDK at build time with warnings as errors.