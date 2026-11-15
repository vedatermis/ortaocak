# VedatOnline client rendering (M6, PBR)

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
| Colour | All shading in linear HDR. The 3D pass writes radiance × exposure into an `R16G16B16A16_FLOAT` target (MSAA as configured); `PostProcess` tone-maps it (ACES fitted) into the back buffer (`B8G8R8A8_UNORM`, rendered through a `_SRGB` view, so the hardware encodes on write). Vertex colours are sRGB-encoded u8 and decoded in the vertex shader (`srgbToLinear`); base-colour textures are `*_UNORM_SRGB` (decoded by the sampler); normal and ORM maps, the terrain splat map and material factors are linear. |
| Lighting | physically based (§4.1): one shadowed directional sun, up to three unshadowed extra lights (the character rim light in the world; key/fill/rim in the preview), SH irradiance + an analytic prefiltered environment derived from the sky colours, then exponential-squared distance fog whose colour is the sky's horizon colour. |

## 2. Code map

| Where | What |
|---|---|
| `client/world/` (in `vo_client_core`, no D3D, unit-tested) | `WorldMath` (angles, look-at, reverse-Z projection, frustum, screen rays), `TerrainQuery` (ray vs heightmap picking, label occlusion), `LocalMovement` (prediction, slide-along-blocked, MOVE_REQ scheduling, POSITION_CORRECT), `RemoteMotion` (server clock, snapshot interpolation / extrapolation cap), `Animator` (idle/walk/run selection, cross-fades, speed-scaled playback, skinning matrices via shared `Pose.h`), `OrbitCamera` (orbit, clamps, terrain collision), `WorldModel` (local player + remote entities fed by the world messages), `DebugGhosts` (`--debug-ghosts`). |
| `client/render/` | `RenderDevice` (swap chain, depth buffer, HDR scene target), `PostProcess` (tone map + MSAA resolve), `RenderStates` (blend/sampler/raster/depth state cache), `GpuMesh` (canonical vertex layouts), `AssetCache` (async loading, `.vtex`), `WorldRenderer` (all 3D passes, light rigs, `[graphics]` settings), `RenderTarget` (offscreen HDR + LDR target for the character preview), `LightingMath.h` (CPU twins of the shader formulas, SH projection; unit-tested), `DebugModelViewer` (`--debug-model`). |
| `client/shaders/` | `World.hlsli` (frame constants, BRDF, `shadeSurface()`, cascaded shadow PCF, SH + environment, fog), `Mesh.hlsl` (static instanced + skinned VS, shared PBR PS, shadow VS), `Terrain.hlsl`, `Sky.hlsl`, `Decal.hlsl`, `PostProcess.hlsl`. Compiled by `fxc` at build time (`/WX`, one header per entry point, `client/CMakeLists.txt`). |
| `client/scenes/WorldScene` | loading screen, input, HUD, labels, Esc menu, automation commands. |
| `client/scenes/CharacterPreview` | 3D model on the select/create screens (render-to-texture). |

## 3. Assets: from disk to the GPU

1. `WorldScene` requests the zone terrain (`terrain/zone<id>/zone<id>.vterrain`) and both races'
   character meshes and clips (`characters/<race>.vmesh`, `characters/<race>/{idle,walk,run}.vanim`).
2. `AssetCache::request()` queues a job for its worker thread: read the file, check header + CRC,
   parse (`vo::assets::loadMesh/loadAnimation/loadTerrain/loadTexture`), convert mesh vertices to
   the canonical layout, load the compiled `.vtex` of a texture if it exists, else decode the PNG
   (stb_image). Nothing D3D happens on the worker. Textures are requested by kind: colour (sRGB),
   normal map or data map (linear); a `.vtex` whose usage does not match is an error.
3. Every loading frame the scene calls `AssetCache::pump(8 ms)`, which creates the GPU resources of
   finished jobs (immutable vertex/index buffers; `.vtex` textures as immutable textures with every
   mip, PNGs + `GenerateMips`). The loading screen keeps rendering its progress bar meanwhile. When
   the cache becomes idle it logs `assets: loaded N texture(s) (n from .vtex, m decoded from PNG),
   X MiB GPU; worker read+decode .. ms, upload .. ms; ...`.
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

Canonical vertex layouts (`GpuMesh.h`): static 44 B (position, normal, uv0, colour u8x4 sRGB; 255s
if the file has none, tangent snorm16x4; zeros if the file has none), skinned 52 B (+ joints u8x4,
weights unorm8x4). Submeshes carry base colour, normal map, ORM map and the factors of the `.vmesh`
v2 material (version 1 meshes: rough dielectric). The assets root is found by searching upwards from the working directory and the
executable for `assets/terrain/zone1/zone1.vterrain`; build it with
`build\x64-debug\bin\assetc.exe build assets_src assets` (`run-ui-flows.ps1` does it if missing).

## 4. The frame

```
WorldRenderer::beginFrame(view)      matrices, frustum, sun shadow cascades (texel-snapped)
  addCharacter() x N                 skin palette span (Animator), world matrix, tint, opacity
  addDecal()                         blob shadows, destination ring
WorldRenderer::render(target)
  constants                          per-frame cbuffer (b0): matrices, lights, SH ambient (recomputed
                                     on the CPU only when the light rig changes), features
  cull props                         per batch: distance + frustum (main) and per cascade box;
                                     visible instance rows -> one dynamic instance buffer (1 map)
  shadow passes (2 x 2048^2 array,   per cascade: terrain chunks + prop instances + skinned characters,
   D32)                              depth only, small slope-scaled bias; 14 m and 64 m boxes around
                                     the player (graphics.shadow_cascades = 1: one 48 m box)
  main pass (R16G16B16A16_FLOAT,     terrain chunks -> props (DrawIndexedInstanced per mesh/submesh)
   4x MSAA, graphics.msaa)           -> characters (bone palette cbuffer b2 per character)
                                     -> sky (full-screen triangle at depth 0, GREATER_EQUAL: only
                                        empty pixels) -> decals (premultiplied, depth read-only)
resolve (RenderDevice::resolveScene) PostProcess: per sample ACES-fitted tone map, average, write the
                                     sRGB back buffer (one full-screen triangle)
2D: SpriteBatch                      name labels (outlined text), HUD, menu - single-sampled, LDR, after the resolve
```

**Resolve after tone mapping.** The MSAA samples are tone-mapped one by one and the display values
averaged (`PostProcess.hlsl`), instead of resolving the HDR radiance first: averaging radiance lets a
single very bright sample (a sun glint on metal next to a dark background) dominate the pixel, so the
edge looks aliased after the curve. The cost is one full-screen pass reading 4 samples (~0.1 ms at
1080p); a plain `ResolveSubresource` is no longer used. Bloom (would need a resolved HDR copy) is not
implemented.

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
* **Shadows**: two 2048² cascades in one texture array, fitted to 28 m and 128 m boxes around the
  local player (texel-snapped against shimmering; 1.4 cm and 6.2 cm texels). The pixel uses cascade 0
  inside 80 % of its box, blends to cascade 1 over the next 15 %, and fades to lit at the far
  cascade's border. Filtering: 16 bilinear comparison taps on a 4x4 grid (a smooth ~5-texel tent;
  `graphics.shadow_filter = hard`: one tap). Bias: a normal offset of `(0.7 + 1.8 (1 − N·L))` texels
  along the geometric normal plus a quarter-texel depth bias and a small slope-scaled raster bias;
  the old 0.0015 depth bias (≈ 1 m in the 700 m light range: visible peter-panning) is gone. Plus
  soft blob decals under every character (contact shadow, also when `--no-shadows` /
  `graphics.shadows = false`).
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
* **Character preview**: the same renderer and shader draw one character into a 4x MSAA HDR
  `RenderTarget` sized to the preview frame, tone-mapped into an sRGB image that SpriteBatch draws.
  Studio rig (`Lighting::characterPreview`): warm key light (shadowed: the cascades are scaled to
  the figure, 1.3 m / 5.8 m), cool fill from the other side, two rim lights from behind in the
  race's colour, a dark nearly neutral backdrop (the sky gradient) whose colours also give the
  ambient and the reflections (lifted ×4 as a studio would), a warm light pool and a blob decal on
  the "floor". Rendered during the UI draw; afterwards the back buffer is re-bound.

### 4.1 Lighting model

`World.hlsli shadeSurface()` for meshes and terrain (CPU twins in `client/render/LightingMath.h`,
tested in `tests/client/LightingMathTests.cpp`):

* **Units**: light colours are irradiance at normal incidence with π folded in (diffuse BRDF =
  albedo, specular = π·D·V·F); sky colours are displayed radiance in the same units (a white surface
  facing up under a uniform sky S shows S). HDR values × exposure go to the float target.
* **Material**: base colour = vertex colour × base colour factor × texture; tangent-space normal map
  (two channels, Z reconstructed, OpenGL +Y up, `normalScale`; B = cross(N, T)·w; skinned tangents
  are transformed by the bone matrices like the normals; double-sided back faces mirror the whole
  frame); ORM: roughness = G × factor, metallic = B × factor, occlusion = lerp(1, R, strength).
  Meshes without maps are rough dielectrics with their factors (props: Blender roughness 0.85).
* **Direct**: GGX / Trowbridge-Reitz D, height-correlated Smith visibility, Schlick Fresnel with
  F0 = lerp(0.04, albedo, metallic); Burley diffuse × albedo × (1 − metallic). Perceptual roughness
  ≥ 0.045; specular anti-aliasing widens α by the screen-space variation of the geometric normal.
  Sun shadowed; extra lights unshadowed.
* **Ambient (image-free)**: diffuse = SH9 irradiance of the sky dome (`Sky.hlsl` gradient from the
  horizon to the zenith above, the ground below = ground albedo × (sun + sky) irradiance), projected
  on the CPU whenever the light rig changes, × ORM occlusion. Specular = split sum: a prefiltered
  radiance of the same environment, evaluated analytically per pixel (the sharp sky/ground gradient
  with the horizon softened by the GGX lobe width, blending into the SH irradiance as roughness
  grows) × Karis' analytic environment BRDF, × specular occlusion (Lagarde, from the ORM occlusion)
  × a horizon term for normal-mapped reflections pointing into the surface. Metal therefore always
  reflects a bright sky over a darker ground that matches the visible sky.
* **Rim**: in the world one extra light, camera-relative from behind the character and above
  (cool, 0.6), applied to characters only, separates them from dark backgrounds.
* **Rig** (`Lighting::world()`): sun (2.6, 2.4, 2.1) at 44° elevation, sky horizon (0.52, 0.64,
  0.78) / zenith (0.15, 0.30, 0.62), ground albedo (0.20, 0.19, 0.14), ambient × 1.2, exposure 0.82
  — calibrated so the village keeps the muted look of `assets_src/previews/terrain/zone1_village.png`.
* **Tone map**: Stephen Hill's ACES RRT + ODT fit (`graphics.tonemap = clamp` for comparison).

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

| View | FPS before PBR (M6 notes) | FPS with PBR / HDR / 2 cascades | 3D draw calls (main + shadow) | Triangles (main / shadow) |
|---|---|---|---|---|
| default, behind the character | ~2700 | ~1360 | 112 + 73 | 318 k / 315 k |
| wide over the village (28 m boom) | ~2070 | ~1210 | 98 + 73 | 320 k / 315 k |

**PBR budget scene** (`vo_client --debug-model dbg/kargan_a.vmesh --debug-model dbg/aydor_a.vmesh
--debug-facing -90 --debug-count 30 --debug-camera 15,14,14,0.4 --size 1920x1080 --vsync off`,
Release): 30 characters of ~38 k triangles / ~23 k vertices each (the raw generated meshes, skinned
with 4 influences through the real character path), 2048² BC base colour + normal + ORM maps, the
village around them, 4x MSAA HDR:

| Settings | FPS (ms) | Draw calls main + shadow | Triangles main / shadow |
|---|---|---|---|
| default (2 cascades, PCF16, specular, normal maps) | ~720 (1.38) | 150 + 117 | 1.36 M / 2.38 M |
| `shadow_cascades=1` | ~820 (1.21) | 150 + 63 | 1.36 M / 1.20 M |
| `shadow_filter=hard` | ~830 (1.20) | 150 + 117 | |
| `specular=false normal_maps=false` | ~760 (1.32) | 150 + 117 | |
| `--msaa 1` | ~850 (1.18) | 150 + 117 | |

GPU memory in that scene: textures 26.7 MiB (two looks × 3 maps 2048² as BC1/BC5/BC1 = 2 × 10.7 MiB,
plus the terrain layers as PNG + GPU mips), meshes 5.5 MiB, renderer 39.8 MiB (2 × 2048² D32 shadow
cascades 32 MiB, terrain buffers, splat), scene targets 94.9 MiB (1080p 4x R16G16B16A16 + D32).
Skinning 30 × 23 k vertices with 4 influences three times (main + 2 shadow cascades) is not a
bottleneck on this GPU (the whole frame is 1.4 ms); LODs are not needed for this budget. If crowds
grow (100+ characters) or for weaker GPUs: an assetc-generated LOD chain (edge-collapse to ~12 k / 4 k
triangles with the same skin, selected by projected size) and skinning the shadow casters with the
lowest LOD would cut the vertex work ~3-5x.

**Texture loading** (Release, the two looks' six 2048² maps, `graphics.compressed_textures`
true/false): PNG decode 660 ms + upload/`GenerateMips` 50 ms (~118 ms per map, 128 MiB on the GPU
as RGBA8 + mips) → `.vtex` read + CRC + parse 85 ms + upload 8.5 ms (~16 ms per map, 21.3 MiB).
Loading is 7.6× faster and uses 6× less GPU memory; six character looks are ~64 MiB instead of
~384 MiB.

Per-frame CPU work is culling ~600 spheres twice, 256 chunk boxes twice and ~10 skeletons; GPU
work is dominated by terrain triangles (no LOD needed at this budget). If zones grow: terrain LOD
with skirts, merged static batches per region, BC-compressed textures (`.vtex`).

Steady-state allocations (Debug CRT allocation hook, `heap:` line at shutdown): standing still in
the world, 0 per frame except the HUD's 4 Hz text refresh (~4 small allocations from the UI text
re-layout); while moving, the MOVE_REQ send path allocates per packet (10/s: the packet writer
and the event payload; frames are sealed in place into the connection's reused buffer since M7); with `--debug-ghosts`, every synthetic packet allocates its
payload (as every received network message does in `GameClient`).

## 7. Robustness (M7) and graphics settings

* **MSAA**: `[graphics] msaa` in client.toml (1, 2, 4, 8; default 4; `--msaa` overrides). The world
  pass renders into an `R16G16B16A16_FLOAT` + `D32_FLOAT` target with that many samples;
  `resolveScene()` tone-maps and resolves it into the sRGB back buffer (§4), so labels and UI are
  drawn single-sampled on top. Unsupported sample counts fall back to the next lower one. Flow
  `msaa` renders the same view with `--msaa 1` and `--msaa 4`.
* **Graphics settings** (`[graphics]` in config/client.toml; `--graphics key=value`, repeatable,
  overrides; a script can change them at run time with `scene graphics <key> <value>` and
  `scene graphics log` logs the state as `automation: graphics: lighting pbr ..., normal maps on,
  specular on, extra lights (rim/fill) on, shadows 2 cascade(s) 2048x2048 pcf16, exposure 1.00, ...,
  msaa 4x, tonemap aces`; the same line is logged at startup):

  | Key | Default | Meaning |
  |---|---|---|
  | `shadows` | true | sun shadow map (`--no-shadows`) |
  | `msaa` | 4 | samples of the HDR world pass (`--msaa`) |
  | `shadow_cascades` | 2 | 1 = one 48 m cascade (old behaviour), 2 = 14 m + 64 m |
  | `shadow_filter` | `pcf` | `pcf` (16 bilinear taps) or `hard` (one tap) |
  | `normal_maps` | true | tangent-space normal maps |
  | `specular` | true | GGX direct and ambient specular; false = diffuse only |
  | `rim_light` | true | the extra lights (world: character rim; preview: fill + rims) |
  | `exposure` | 1.0 | multiplies the light rig's exposure (0.05 .. 16) |
  | `tonemap` | `aces` | `aces` (fitted) or `clamp` |
  | `compressed_textures` | true | load `.vtex`; false decodes the PNGs (startup only) |
  | `debug_view` | `none` | `lighting` (grey material, normal maps kept) or `normals` (meshes only) |
  | `normal_flip_y` | false | flip the normal maps' green (to diagnose DirectX-convention maps) |

  SSAO and bloom are not implemented: on the target GPU the frame has headroom, but the generated
  albedo maps already carry baked occlusion and the ORM occlusion covers small-scale contact; a
  half-resolution AO would mostly darken the grass under the props. `--debug-model` (§8) is the
  tool for comparing settings.
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

## 8. Model viewer (`--debug-model`)

A server-less viewer for material and lighting work (`client/render/DebugModelViewer.h`): it loads
`.vmesh` files by asset path, gives them a 4-bone identity skin so they take the real character path,
and shows them in the village (world light rig, zone 1 terrain and props) or with the character
preview's studio rig. It logs `viewer: perf` (FPS, draw calls, triangles, GPU memory of textures,
meshes, renderer and scene targets) every second.

```powershell
# raw generator output -> assets/dbg (not committed; embedded 4096^2 images become 2048^2 PNG + .vtex)
build\x64-release\bin\assetc.exe mesh <tripo>\kargan_a.glb assets\dbg\kargan_a.vmesh --out-root assets --assume-orm
build\x64-release\bin\vo_client.exe --debug-model dbg/kargan_a.vmesh --debug-facing -90 --debug-camera threeq `
    --graphics debug_view=lighting --screenshot shot.png --exit-after-frames 8
```

| Flag | Meaning |
|---|---|
| `--debug-model <asset.vmesh>` | repeatable; models side by side |
| `--debug-count <n>` | instances in a grid (performance tests) |
| `--debug-camera <preset>` | `front`, `threeq`, `side`, `back`, `face`, `armor`, `wide` or `yawDeg,pitchDeg,distance,heightFraction` relative to the model's facing |
| `--debug-light <rig>` | `world` (default), `preview-aydor`, `preview-kargan` |
| `--debug-yaw <deg>` / `--debug-facing <deg>` | world yaw of the models / the mesh's own front as a yaw in model space (raw Tripo meshes face glTF +X: `-90`) |
| `--debug-orbit <deg/s>`, `--debug-at <x>,<z>` | camera orbit; position in the zone (default `spawn_aydor`) |
