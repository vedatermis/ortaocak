# Third-party software

All dependencies are installed by vcpkg from `vcpkg.json` (versions pinned by `builtin-baseline`)
and linked statically (`x64-windows-static-md`). The full license text of each package is in
`build/vcpkg_installed/x64-windows-static-md/share/<package>/copyright` after a build; ship those
texts with any binary distribution.

## Direct dependencies

| Library | Version | License | Used by | Purpose |
|---|---|---|---|---|
| [Asio](https://think-async.com/Asio/) (standalone) | 1.38.2 | BSL-1.0 | server, client (later), botclient (later) | TCP networking (IOCP) |
| [libsodium](https://libsodium.org) | 1.0.22 | ISC | server, client (later) | X25519, XChaCha20-Poly1305, Argon2id |
| [libpqxx](https://github.com/jtv/libpqxx) | 8.0.2 | BSD-3-Clause | server | PostgreSQL C++ client |
| [toml++](https://marzer.github.io/tomlplusplus/) | 3.4.0 | MIT | shared (config) | TOML config parsing |
| [Catch2](https://github.com/catchorg/Catch2) | 3.16.1 | BSL-1.0 | tests only | Unit testing |
| [stb](https://github.com/nothings/stb) (stb_truetype, stb_image, stb_rect_pack) | 2026-08-02 | MIT / Public Domain (dual) | client | Font atlas baking, PNG/JPG loading |

## Transitive dependencies (via libpqxx -> libpq)

| Library | Version | License |
|---|---|---|
| libpq (PostgreSQL client library) | 18.4 | PostgreSQL License (BSD/MIT-like) |
| OpenSSL | 3.6.5 | Apache-2.0 |
| zlib | 1.4 | zlib |
| LZ4 | 1.11.1 | BSD-2-Clause (library) |

Note: OpenSSL 3.x is Apache-2.0 — permissive, but outside the MIT/BSL/ISC/zlib list in
`docs/DEVELOPMENT_PLAN.md` §6. It is only linked into `vo_server` (libpq TLS). Apache-2.0 requires
shipping its license/NOTICE with binaries.

## System components (not redistributed)

Windows SDK: Win32, Direct3D 11, DXGI, WIC, DirectXMath (MIT, header-only, part of the SDK).
