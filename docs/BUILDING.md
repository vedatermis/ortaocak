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
| `assetc.exe` | Asset compiler (skeleton) |
| `botclient.exe` | Headless test client (skeleton) |

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
variable (the env var wins). M0 does not connect to the database yet.

Exit codes: 0 ok, 1 bad arguments, 2 config error, 3 library init error, 4 runtime error.

### Client

```powershell
build\x64-debug\bin\vo_client.exe
```

Esc or closing the window quits. Config: `config/client.toml` (+ `client.local.toml`). The log is
written to `vo_client.log` next to the executable (override with `--log <file>`); `--console`
also mirrors it to the calling console.

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
| `--scene login\|uidemo` | Start scene. `login` (default) is the placeholder title screen; `uidemo` shows every UI widget. |
| `--size WxH` | Client-area size in physical pixels (overrides `window.width/height`). |
| `--ui-scale <f>` | Multiplies the monitor DPI scale (the process is per-monitor-DPI-aware v2). `1.5` on a 96-DPI monitor renders the UI at 150%. |
| `--script <file.voscript>` | Plays timed synthetic input through the same event queue as real input, then exits. While a script runs, real mouse/keyboard input is ignored and the clipboard is in-memory. |
| `--screenshot-dir <dir>` | Base directory for relative `screenshot` paths in a script. |

Script commands (one per line, `#` comments; see `client/automation/Script.h`): `wait <frames>`,
`move x y`, `click x y [left|right|middle]`, `dblclick x y`, `mousedown x y`, `mouseup x y`,
`wheel <notches>`, `key <chord>` (e.g. `key tab`, `key shift+tab`, `key ctrl+a`, `key VK_RETURN`),
`keydown <chord>`, `keyup <chord>`, `text <utf8>` (rest of line, optional quotes),
`screenshot <file.png>`, `resize w h`, `uiscale <f>`, `log <msg>`, `quit`. The end of the script
also quits. Script errors exit with code 1.

The UI walkthrough used for M3 verification:

```powershell
$p = Start-Process build\x64-debug\bin\vo_client.exe -PassThru -Wait -ArgumentList `
       '--scene', 'uidemo', '--size', '1280x720', '--script', 'client/automation/uidemo.voscript', `
       '--screenshot-dir', "$env:TEMP\vo_uidemo"
$p.ExitCode   # 0 = all screenshots written
```

Debug builds log a CRT heap-allocation summary for the last 120 frames at shutdown (`heap: ...`) and
the D3D11 live-object report (`no live objects reported`). Shaders (`client/shaders/*.hlsl`) are
compiled by `fxc` from the Windows SDK at build time with warnings as errors.