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
| [stb](https://github.com/nothings/stb) (stb_truetype, stb_image, stb_rect_pack, stb_image_write, stb_dxt) | 2026-08-02 | MIT / Public Domain (dual) | client, assetc | Font atlas baking, PNG/JPG loading; assetc: PNG writing, BC1/BC3/BC4/BC5 block compression of `.vtex` textures (stb_dxt) |
| [cgltf](https://github.com/jkuhlmann/cgltf) | 1.15 | MIT | assetc (tools only) | glTF 2.0 / GLB parsing for the asset compiler |
| [MikkTSpace](https://github.com/mmikk/MikkTSpace) (vcpkg port `mikktspace`) | 2020-10-06 | zlib | assetc (tools only) | Reference tangent-space generator for normal-mapped meshes without glTF tangents |

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

## Content (art and animation)

| Asset | Source | License / terms | Where | Notes |
|---|---|---|---|---|
| Universal Animation Library (Standard) | [Quaternius](https://quaternius.com/packs/universalanimationlibrary.html) | CC0 1.0 (License.txt shipped next to the files) | `tools/blender/third_party/quaternius_ual/` | Skeleton and all character animation clips (idle, walk, run, combat, emotes) |
| Character meshes and textures (Aydor, Kargan) | Generated for this project with Tripo P2 via Higgsfield from our own concept images | Subject to the Higgsfield / Tripo terms of the generating account — **review before any commercial release** | `tools/blender/source_meshes/`, `assets_src/characters/` | Rigged to the UAL skeleton by `tools/blender/gen_character.py` |
| Environment props (houses, cottage, well, market stall, Ant Taşı, watchtower, fence, lamp post, goods, signpost, boulder, oak, pine) | Generated for this project with Tripo H3.1 from our own concept images (`<name>_ref.png` next to each source) | Subject to the Tripo terms of the generating account — **review before any commercial release** | `tools/blender/source_meshes/props/` (raw output), `assets_src/props/` (processed by `tools/blender/gen_props.py`) | Scaled, oriented, cleaned, decimated and re-textured offline; emissive masks derived from the base colour |
| Terrain materials: `forrest_ground_01`, `rocky_trail`, `rock_face_03`, `grass_ground` (diffuse, OpenGL normal, ARM; 2K) | [Poly Haven](https://polyhaven.com/textures) | CC0 1.0 | `tools/blender/source_textures/polyhaven/` (as downloaded), `assets_src/textures/terrain/` (graded, 1024², by `tools/blender/gen_textures.py`) | Zone 1 layers grass, dirt road, rock, dry grass. `brown_mud_leaves_01` was evaluated and not used |
| UI art (login background, race portraits) | Generated for this project with Higgsfield (GPT Image) | Subject to the Higgsfield terms of the generating account — **review before any commercial release** | `assets_src/ui/` | |
