# VedatOnline client rendering (M6)

How the client draws the world: conventions, the frame, how assets get from `assets/` to the GPU,
and what was measured. Formats are in [ASSET_FORMATS.md](ASSET_FORMATS.md), the pipeline that
produces them in [ASSET_PIPELINE.md](ASSET_PIPELINE.md), movement/networking rules in
[PROTOCOL.md](PROTOCOL.md) §5.4.

## 1. Conventions

| Topic | Choice |
|---|---|
| Axes | left-handed, +X right, +Y up, +Z forward, metres; yaw 0 = +Z, clockwise from above: `forward(yaw) = (sin, 0, cos)` |
| Matrices | `vo::assets::Mat4` (glTF layout: column-major storage, column vectors, translation in m[12..14]). Uploaded with `memcpy`; HLSL keeps the default `column_major` packing and multiplies `mul(M, v)`. `clip = proj * view * world * p`. World matrix of a placement/character: `T(position) * RotY(yaw) * S(scale)` (`world::makeWorld`). |
| Depth | **reverse-Z**, `D32_FLOAT`: near plane -> 1, far plane -> 0, cleared to 0, test `GREATER_EQUAL` (`DepthMode::ReverseReadWrite/ReadOnly`). Near 0.1 m, far 1500 m. The shadow map uses standard depth (`LESS_EQUAL`, cleared to 1). |
| Culling | front faces are **clockwise** (D3D11 default, `FrontCounterClockwise = FALSE`), `CULL_BACK`; submeshes flagged double-sided use `CULL_NONE` and flip the normal in the pixel shader. Terrain triangles are emitted clockwise seen from above. |
| Colour | All shading in linear space. The back buffer (`B8G8R8A8_UNORM`) is rendered through a `_SRGB` view, so the hardware encodes on write. Vertex colours are sRGB-encoded u8 and decoded in the vertex shader (`srgbToLinear`); colour textures are `R8G8B8A8_UNORM_SRGB` (decoded by the sampler); material base colours are linear; the terrain splat map is linear data. No tone mapping: light intensities are chosen so a fully lit albedo stays below 1. |
| Lighting | one directional sun (Lambert) + hemispheric ambient (sky colour for normals pointing up, ground colour for normals pointing down) + exponential-squared distance fog whose colour is the sky's horizon colour. |

## 2. Code map

| Where | What |
|---|---|
| `client/world/` (in `vo_client_core`, no D3D, unit-tested) | `WorldMath` (angles, look-at, reverse-Z projection, frustum, screen rays), `TerrainQuery` (ray vs heightmap picking, label occlusion), `LocalMovement` (prediction, slide-along-blocked, MOVE_REQ scheduling, POSITION_CORRECT), `RemoteMotion` (server clock, snapshot interpolation / extrapolation cap), `Animator` (idle/walk/run selection, cross-fades, speed-scaled playback, skinning matrices via shared `Pose.h`), `OrbitCamera` (orbit, clamps, terrain collision), `WorldModel` (local player + remote entities fed by the world messages), `DebugGhosts` (`--debug-ghosts`). |
| `client/render/` | `RenderDevice` (swap chain + depth buffer), `RenderStates` (blend/sampler/raster/depth state cache), `GpuMesh` (canonical vertex layouts), `AssetCache` (async loading), `WorldRenderer` (all 3D passes), `RenderTarget` (offscreen colour+depth for the character preview). |
| `client/shaders/` | `World.hlsli` (frame constants, sRGB decode, shadow PCF, fog, `shade()`), `Mesh.hlsl` (static instanced + skinned VS, shared PS, shadow VS), `Terrain.hlsl`, `Sky.hlsl`, `Decal.hlsl`. Compiled by `fxc` at build time (`/WX`, one header per entry point, `client/CMakeLists.txt`). |
| `client/scenes/WorldScene` | loading screen, input, HUD, labels, Esc menu, automation commands. |
| `client/scenes/CharacterPreview` | 3D model on the select/create screens (render-to-texture). |

## 3. Assets: from disk to the GPU

1. `WorldScene` requests the zone terrain (`terrain/zone<id>/zone<id>.vterrain`) and both races'
   character meshes and clips (`characters/<race>.vmesh`, `characters/<race>/{idle,walk,run}.vanim`).
2. `AssetCache::request()` queues a job for its worker thread: read the file, check header + CRC,
   parse (`vo::assets::loadMesh/loadAnimation/loadTerrain`), convert mesh vertices to the canonical
   layout, decode PNGs (stb_image). Nothing D3D happens on the worker.
3. Every loading frame the scene calls `AssetCache::pump(8 ms)`, which creates the GPU resources of
   finished jobs (immutable vertex/index buffers, textures + `GenerateMips`). The loading screen keeps
   rendering its progress bar meanwhile.
4. When the terrain is in, the scene requests its 4 layer textures and every distinct placement
   mesh (13 for zone 1). When the cache is idle, `WorldRenderer::setTerrain()` builds the terrain
   chunks, the splat texture and the prop instance batches, and the world becomes interactive.
   Zone 1 loads in ~0.1-0.3 s on the development machine (26 files).
5. Entries are keyed by asset path (relative to the `assets/` directory, forward slashes) and stay
   loaded until shutdown; the character preview loads the same entries synchronously
   (`loadCharacterNow`) so both screens share one copy. Character looks: the cache discovers at
   startup which look files exist (`client/world/CharacterAssets.h`: look n of a race is
   `characters/<race>_<n>.vmesh` with its own clips, look 0 falls back to the legacy
   `characters/<race>.vmesh`, a missing look to look 0) and binds clips per race + look (bone
   lengths differ per mesh). The world loads every available look of both races. A mesh whose
   texture another mesh already loaded gets it hooked up at once.

Canonical vertex layouts (`GpuMesh.h`): static 36 B (position, normal, uv0, colour u8x4 sRGB; 255s
if the file has none), skinned 44 B (+ joints u8x4, weights unorm8x4). Tangents are dropped (no
normal maps yet). The assets root is found by searching upwards from the working directory and the
executable for `assets/terrain/zone1/zone1.vterrain`; build it with
`build\x64-debug\bin\assetc.exe build assets_src assets` (`run-ui-flows.ps1` does it if missing).

## 4. The frame

```
WorldRenderer::beginFrame(view)      matrices, frustum, sun shadow box (texel-snapped)
  addCharacter() x N                 skin palette span (Animator), world matrix, tint, opacity
  addDecal()                         blob shadows, destination ring
WorldRenderer::render(target)
  constants                          one dynamic per-frame cbuffer (b0)
  cull props                         per batch: distance + frustum (main) and shadow box (shadow);
                                     visible instance rows -> one dynamic instance buffer (1 map)
  shadow pass  (2048^2, D32)         terrain chunks + prop instances + skinned characters, depth only,
                                     slope-scaled bias, 96 m box around the player
  main pass (4x MSAA target,         terrain chunks -> props (DrawIndexedInstanced per mesh/submesh)
   graphics.msaa)
                                     -> characters (bone palette cbuffer b2 per character)
                                     -> sky (full-screen triangle at depth 0, GREATER_EQUAL: only
                                        empty pixels) -> decals (premultiplied, depth read-only)
resolve                              MSAA sRGB target -> single-sample sRGB texture (linear average)
                                     -> CopyResource into the back buffer (RenderDevice::resolveScene)
2D: SpriteBatch                      name labels (outlined text), HUD, menu - single-sampled, after the resolve
```

* **Terrain**: the 513² heightmap is cut into 16x16 chunks of 32x32 quads (33² vertices each, one
  shared 16-bit index pattern, `DrawIndexed` with a base vertex per chunk); per-vertex normals by
  central differences of the heights; chunk AABBs for frustum culling. No LOD: the whole zone is
  0.52 M triangles and a typical view draws 60-90 chunks (see §6). The pixel shader normalises the
  RGBA splat weights and blends 4 layer textures (8x anisotropic, wrap, GPU mips), each sampled at
  two scales (tile size and ~4.2x larger) to hide the repeat.
* **Props**: one batch per mesh with every placement's world matrix (3 rows) and bounding sphere.
  Distance cull at `140 m + 60 x radius` (clamped to 600 m), then frustum. ~600 objects cost
  13-30 draw calls.
* **Characters**: GPU skinning with up to 4 influences from a 64-entry `float4x4` palette;
  `skin[b] = model[b] * inverseBind[b]` straight from `vo::assets::computeSkinningMatrices`. Looks are
  whole meshes (no face/hair variant submeshes). `CharacterDraw::opacity` < 1 dithers the character
  out with an ordered 4x4 pattern in the pixel shader (the camera pushed into the local character).
* **Shadows**: a single 2048² shadow map fitted to a 96 m box around the local player (texel-snapped
  against shimmering), 3x3 PCF with a comparison sampler, normal offset + slope-scaled bias, faded
  out towards the box edge. Plus soft blob decals under every character (contact shadow, also when
  `--no-shadows` / `graphics.shadows = false`).
* **Sky / fog**: gradient from the horizon colour (also the fog colour, so far terrain melts into
  the sky) to the zenith, a sun disk and glow; fog starts at 25 m, density 1/650 m (exp²).
* **Labels**: projected head position (+16 cm above the bind-pose height), outlined bold text,
  full opacity up to 28 m, fading out at 70 m; remote labels are hidden when a hill is between the
  camera and the head (CPU segment test against the heightmap) and when a prop's or another
  character's bounding box is in the way (`world::LabelOccluder`: box placed with its world matrix,
  sphere reject + slab test; conservative, no depth readback). The own label is hidden only while the
  character is dithered out.
* **Zone banner**: on entering, the zone's name and tagline (`shared/game/Zones.h`) fade in over a
  soft dark band in the lower third, stay ~3 s and fade out. The loading screen names the zone and
  shows one short lore line.
* **Character preview**: the same renderer draws one character into a `RenderTarget` sized to the
  preview frame (no terrain, no shadows, race-coloured sky as a backdrop, a warm light pool and a
  blob decal on the "floor"), then SpriteBatch draws it as an image. Rendered during the UI draw;
  afterwards the back buffer is re-bound.

## 5. Movement, camera and remote players (summary)

See the headers in `client/world/` for the details; the scene only wires input and messages:

* WASD/arrows move relative to the camera yaw (A/D sideways); the character turns to face the
  direction it actually travels (12 rad/s). Left click raycasts the heightmap and walks/runs there;
  WASD cancels it; it gives up after 0.5 s without progress. Shift held = walk, R toggles the
  default (run).
* Collision: every blocked walk-mask cell (and the zone border) is grown by a 0.3 m body radius and
  the position is pushed out across the nearest face after each 0.25 m sub-step (the 1 m mask cells
  under-cover wall footprints by up to ~0.5 m). That keeps the tangential part of the move: the
  character slides along walls, rounds corners hit diagonally and stops when running straight into
  a wall; y = `heightAt(x, z)`. Stricter than the server's point test, so predicted
  positions are always acceptable to it.
* MOVE_REQ: immediately on start, stop (moving = 0) and run/walk toggle (also while standing), at
  10 Hz while moving (a cadence that does not drift with the frame rate), never closer than 40 ms.
  POSITION_CORRECT: snap to the server position (y from the terrain), drop the click target, blend
  the visible offset away over ~0.2 s; a correction whose seq was sent before the previous snap is
  stale and ignored.
* Remote entities: snapshots stamped with ENTITY_MOVE.server_time_ms, rendered 100 ms behind the
  estimated server clock (offset = least-delayed packet, slow decay); interpolation between the
  bracketing snapshots; past the newest one, extrapolation along the last velocity for at most
  250 ms if it says "moving", then hold. A jump > 12 m snaps. Feet follow the terrain.
* Animation: idle / walk / run chosen from the (smoothed) ground speed with hysteresis (start above
  0.35 m/s, stop below 0.15 m/s) and the running flag; cross-fade 0.25 s; walk <-> run keep the
  normalised stride phase; playback rate = speed / clip.moveSpeed, clamped 0.6-1.6x (walk 1.4 m/s
  plays the ~1 m/s library walk at 1.36-1.48x, run 5.5 m/s the jog at 0.88-0.96x). Local-only
  cosmetic actions replace the locomotion pose: Space = jump_start -> jump_loop -> jump_land with a
  0.42 m arc (drawn offset only), X = sit_enter -> sit_idle -> sit_exit (movement input stands up
  first; the character does not move until it stands).
* Camera: orbit around a point 1.55 m above the feet, smoothed follow (14/s), distance 2-28 m (wheel,
  factor 1.15 per notch), pitch -20°..80°; the boom is shortened at the first terrain hit and the eye
  kept 0.45 m above the ground. Static obstacles (`world::CameraObstacles`): every prop placement
  except trees and thin/small props (narrower than 0.8 m or lower than 1.2 m: lamp posts, fences,
  signposts, crates, barrels, small rocks) is an oriented box (mesh bounds x placement, grown by
  0.25 m) in a 16 m x/z grid; the boom is pulled in to just outside the first box it enters
  (immediately; easing back out is smooth). If the focus is already inside a box (under a roof's
  eaves), the walk mask finds the wall instead. Below ~1 m from the character's body axis the local
  character dithers out (gone below 0.45 m).

## 6. Performance notes

Release, vsync off, 1920x1080, RTX 3080 Ti, standing in the village with 8 debug ghosts
(`run-ui-flows.ps1 -Config Release -Only world_perf`):

| View | FPS | 3D draw calls (main + shadow) | Triangles (main / shadow) |
|---|---|---|---|
| default, behind the character | ~2700 | 130 + 61 | 229 k / 92 k |
| wide over the village (28 m boom) | ~2070 | 115 + 61 | 231 k / 92 k |

Per-frame CPU work is culling ~600 spheres twice, 256 chunk boxes twice and ~10 skeletons; GPU
work is dominated by terrain triangles (no LOD needed at this budget). If zones grow: terrain LOD
with skirts, merged static batches per region, BC-compressed textures (`.vtex`).

Steady-state allocations (Debug CRT allocation hook, `heap:` line at shutdown): standing still in
the world, 0 per frame except the HUD's 4 Hz text refresh (~4 small allocations from the UI text
re-layout); while moving, the MOVE_REQ send path allocates per packet (10/s: the packet writer
and the event payload; frames are sealed in place into the connection's reused buffer since M7); with `--debug-ghosts`, every synthetic packet allocates its
payload (as every received network message does in `GameClient`).

## 7. Robustness (M7)

* **MSAA**: `[graphics] msaa` in client.toml (1, 2, 4, 8; default 4; `--msaa` overrides). The world
  pass renders into a `B8G8R8A8_UNORM_SRGB` + `D32_FLOAT` multisampled target; `resolveScene()`
  resolves it with the sRGB format (the hardware averages in linear space) into a single-sample
  sRGB texture and copies that into the back buffer (same B8G8R8A8 type group), so labels and UI
  are drawn single-sampled on top. Unsupported sample counts fall back to the next lower one. Flow
  `msaa` renders the same view with `--msaa 1` and `--msaa 4`.
* **Device loss**: `Present` returning `DXGI_ERROR_DEVICE_REMOVED/RESET` (or `scene devicelost` in a
  script) closes the scene, sends LOGOUT, destroys the device and every GPU object (render states,
  sprite batch, fonts, asset cache, world renderer, UI art; the Debug build reports live objects of
  the old device: none), creates them again on a new device and opens the login screen with
  "Ekran aygıtı sıfırlandı. Oturumunuz kapatıldı; lütfen tekrar giriş yapın." The session is not
  kept: scenes and the world model reference GPU-side data, and a fresh login gets a complete
  world state from the server. Tested with the simulated loss (flow `resilience`); a real TDR was
  not forced (it would reset the GPU for the whole desktop).
* **Minimise / Alt+Tab / resize**: while minimised nothing is rendered but the network is polled
  (pings keep the session); the frame delta is clamped to 0.25 s, so the first frame after a restore
  cannot produce an implausible move. Flow `resilience` sends the real window messages
  (`WM_SYSCOMMAND SC_MINIMIZE/SC_RESTORE`, `WM_ACTIVATEAPP`/`WM_ACTIVATE`/`WM_KILLFOCUS`) while
  walking, and resizes the window while the world is loading.
* **Leaving the world**: from LEAVE_WORLD_REQ / LOGOUT on, `WorldModel::beginLeaving()` stops the
  local character and `GameClient` refuses MOVE_REQ until the next ENTER_WORLD_RES; world input is
  ignored until the answer; remote entities are dropped on LEAVE_WORLD_RES, on a disconnect and on
  every ENTER_WORLD_RES (PROTOCOL §5.4).