# VedatOnline Coding Standards

Short and binding. If something is not covered here, follow the surrounding code.

## Language and build

- C++20, MSVC (VS 2026). `/W4 /WX /permissive-`: **warnings are errors**. Don't silence a warning
  with a pragma unless the warning is a known false positive; comment why when you do.
- Third-party headers come in through CMake imported targets and are compiled as external
  (`/external:W0`); never copy third-party code into our directories.
- Every first-party target links `vo::build_options`.
- Format with `.clang-format` (4 spaces, 110 columns, Allman braces).
- Source is UTF-8 (`/utf-8`); all strings inside the program are UTF-8 `std::string`. Convert to
  UTF-16 only at the Win32 API boundary (`client/platform/Win32Util.h`), and use
  `vo::core::pathToUtf8` for paths in logs.

## Naming

| Thing | Style | Example |
|---|---|---|
| Types, enums, concepts | `PascalCase` | `RenderDevice`, `Level` |
| Functions, methods, variables | `camelCase` | `loadWithOverlay`, `frameCount` |
| Member variables | `m_` + camelCase | `m_swapChain` |
| Constants, `constexpr`, enumerators | `k` + PascalCase / PascalCase | `kMaxPacketSize`, `Level::Info` |
| Macros | `VO_UPPER_CASE` | `VO_LOG_INFO`, `VO_ASSERT` |
| Files | `PascalCase.h/.cpp`, one main class per file | `Window.h`, `Window.cpp` |
| Tests | `<Thing>Tests.cpp` | `ConfigTests.cpp` |

## Namespaces

- Everything lives in `vo::`. Shared code: `vo::core`, `vo::log`, `vo::net`, `vo::game`.
  Client: `vo::client::{platform,render,ui,scenes,net}`. Server: `vo::server::{net,auth,world,db}`.
  Tools: `vo::tools::<tool>`.
- No `using namespace` in headers. Namespace aliases are fine in `.cpp` files.
- File-local helpers go in an anonymous namespace.

## Headers

- `#pragma once`. Includes are rooted at the repository: `#include "shared/core/Log.h"`.
- Include order: own header, project headers, platform headers, third-party, standard library
  (blank line between groups; clang-format enforces it).
- Headers include what they use and nothing more; prefer forward declarations or pimpl to keep
  heavy headers (Windows.h, asio, toml++, pqxx) out of widely included headers. `shared/` headers
  must stay platform-neutral (Windows-only code behind `#if defined(_WIN32)` in `.cpp` files).
- A header starts with a short comment saying what the module is for.

## Error handling

- **Expected failures** (I/O, parsing, network, DB, user input) are reported by return value:
  `bool` + `std::string& error`, `std::optional<T>`, or a small result struct. Callers must check
  (`[[nodiscard]]` where it helps).
- **Exceptions** only cross library boundaries we don't own (toml++, pqxx, std); catch them at the
  nearest wrapper and convert. Every `main` has a top-level `catch (const std::exception&)` that
  logs and returns a non-zero exit code. Never throw across threads, callbacks or C APIs.
- **Programming errors** (broken invariants) use `VO_ASSERT` (Debug) or `VO_VERIFY` (always on).
  Anything parsed from the network is *input*, never an assertion: validate and reject.
- HRESULTs are always checked; log with `hresultToString`.
- Executables return documented exit codes (`0` = success).

## Ownership and resources

- **No raw `new`/`delete`.** Use values, `std::unique_ptr` (default), `std::shared_ptr` (only for
  genuinely shared lifetime), `std::vector`, `Microsoft::WRL::ComPtr` for COM.
- RAII for every OS handle/resource; wrap it in a small class if no wrapper exists.
- Raw pointers and references are non-owning and must not outlive the owner.
- Classes that own resources are non-copyable unless copying is meaningful; mark it explicitly
  (`= delete`).
- Release order matters for D3D/DXGI: everything is released before the device, and the swap chain
  before its window. The client's Debug build reports live D3D objects at shutdown — keep it clean.

## Threading

- The server world simulation is **single-threaded** (tick loop). Network (asio) threads and DB
  workers communicate with it only through locked queues/posted handlers; never touch world state
  from another thread.
- Shared mutable state needs a documented owner thread or a mutex. Prefer message passing.
- `std::mutex` + `std::lock_guard`/`std::scoped_lock`; no manual `lock()/unlock()`.
- Atomics only for simple flags/counters.
- The logger is thread-safe; `Config` is not (load at startup, then read-only).
- Client rendering and the Win32 message pump run on the main thread.
- Time: `vo::core` monotonic clock (`steady_clock`) for durations, timeouts and ticks. Wall clock
  only for display/log timestamps.

## Logging

- `VO_LOG_TRACE/DEBUG/INFO/WARN/ERROR/FATAL("fmt {}", args...)` (std::format syntax).
- Never log secrets: passwords, keys, session tokens, full DB connection strings.
- INFO is for events an operator cares about; per-packet/per-frame logging is TRACE.

## Tests

- Unit tests (Catch2) go in `tests/`, mirroring the source path (`tests/core/ConfigTests.cpp`).
- Tests must be deterministic: inject time/env/IO where needed (see `Config::setEnvLookup`).
- New shared code comes with tests. `build.ps1 -Test` must pass before a delivery is accepted.
