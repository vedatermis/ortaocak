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
2. `AssetCache::request()` queues a job for its worker threads (half the hardware threads, 1-4): read the file, check header + CRC,
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
   mesh (13 for zone 1); the cache itself queues the layers' normal and ORM maps (.vterrain v2) when
   the terrain finishes, and every submesh's base colour / normal / ORM / emissive map when a mesh
   finishes. When the cache is idle, `WorldRenderer::setTerrain()` builds the terrain chunks, the
   splat texture and the prop instance batches, and the world becomes interactive. Zone 1 (12
   terrain maps, 13 props with 44 maps, all `.vtex`) is ready ~0.13 s after entering (Release, §6).
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
  0.52 M triangles and a typical view draws 60-90 chunks (see §6). `Terrain.hlsl` shades four PBR
  layers (Poly Haven scans, `.vtex` BC1 base colour / BC5 normal / BC1 ORM, 1024², 8x anisotropic
  wrap) with the meshes' `shadeSurface()`, so shadows, ambient, fog and tone map match:
  * **tiling**: each planar layer is sampled at its tile size (2.6-3.6 m) and again rotated by 37°,
    offset and at ~3.7x the tile, cross-faded by a world-space value noise; the fine sample fades out
    between 30 and 100 m (`kTerrainShading`), so neither repeat forms a grid near by or on distant
    slopes. The tangent frame follows each projection (T = direction of increasing u, B = image up).
  * **cliffs**: layers flagged triplanar (rock, `kLayerParams` w) project along x, y and z, blended
    by |n|⁴, with a fixed scale plus a coarse base-colour triplanar for the large repeat (a scale
    that varies with distance turns the coordinates into a function of distance: smeared bands).
  * **blending**: splat weight + height (the layer's ORM occlusion × a per-layer weight), keeping
    what is within 0.16 of the highest, so road edges and rock/grass borders follow pebbles and
    cracks instead of the 1 m splat cells; normals, ORM and albedo blend with the same weights.
  * **macro variation**: ±13 % brightness and a slight warm/cool shift over 30-90 m noise.
  * ORM occlusion × a per-layer strength feeds the ambient; roughness from ORM (≥ 0.35), metallic 0.
* **Props**: one batch per mesh with every placement's world matrix (3 rows), tint (row 4: the
  placement's sRGB tint, linear) and bounding sphere. Distance cull at `140 m + 60 x radius`
  (clamped to 600 m), then frustum. Meshes with submeshes named `lod0` / `lod1` (gen_props.py: the
  houses, the watchtower, trees) draw `lod1` beyond 75 m + the batch's largest radius: per batch a
  near and a far instance range; the 64 m shadow cascade casts with `lod1`, the 14 m one with
  `lod0`. Non-uniform placement stretch: the vertex shader transforms normals by the cofactor
  (inverse-transpose) of the instance matrix. ~600 objects cost 20-40 draw calls.
* **Emission**: `.vmesh` v3 submeshes add `emissiveFactor × emissiveMap` radiance after lighting
  (before fog): lanterns, window glass, the Ant Taşı's ember crack (strength 1.4-4; a sunlit
  surface is ~1-1.5 in the same units, so it reads as a glow at dusk exposure and stays subtle by
  day). Alpha-tested submeshes (`alphaCutoff`) `clip` in the main pass (the shader can also output
  an alpha-to-coverage alpha; the blend state and a clipping shadow pass are not wired, no asset uses
  it yet).
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
  the sky) to the zenith, a sun disk and glow, and a warm forward-scattering haze towards the sun
  that is strongest at the horizon; `applyFog` adds the same haze term, so fogged terrain matches the
  sky in every direction. Fog starts at 30 m, density 1/380 m (exp²): the plateau stays clear, the
  border mountains 200-300 m away fade by a third to a half.
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
* **Rig** (`Lighting::world()`, late-afternoon highland): warm sun (2.75, 2.30, 1.72) 27° above the
  south-west horizon (long, soft-edged shadows), pale slightly desaturated sky — horizon / fog
  (0.62, 0.64, 0.66), zenith (0.20, 0.30, 0.50) — ground albedo (0.17, 0.16, 0.12), ambient × 1.3
  (shadowed sides keep their colour under the low sun), exposure 0.80; tuned in game against the
  PBR characters, the scanned terrain and the generated props.
* **Emission** (`Surface::emissive`): added after the lighting, before fog; zero for terrain and
  characters, the lighting debug view and the shadow pass.
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
  signposts, small boulders) is an oriented box (mesh bounds x placement incl. per-axis stretch, grown by
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

**Environment pass** (generated PBR props, Poly Haven terrain layers, new light rig; Release, vsync
off, 1920x1080, 4x MSAA, RTX 3080 Ti). PBR budget scene as above (30 raw characters in the plaza,
`--debug-at 250,268`) and two more camera positions:

| View | FPS before → after | Draw calls main + shadow | Triangles main / shadow | GPU textures |
|---|---|---|---|---|
| plaza, 30 characters (`15,14,14,0.4`) | ~700 → ~520 | 149 + 116 → 150 + 119 | 1.36 M / 2.38 M → 1.62 M / 2.68 M | 26.7 → 103.5 MiB |
| higher over the plaza (`15,30,60,0.4`) | ~580 → ~540 | 149 + 116 → 151 + 119 | 1.37 M / 2.38 M → 1.69 M / 2.68 M | |
| forest edge (`--debug-at 330,150`) | ~630 → ~540 | 163 + 107 → 165 + 109 | 1.38 M / 2.40 M → 1.90 M / 2.55 M | |
| in game, `world_perf` default (8 ghosts) | ~1030 → ~800 | 111 + 74 → 111 + 78 | 306 k / 318 k → 664 k / 651 k | |
| in game, `world_perf` wide (28 m boom) | ~930 → ~740 | 97 + 74 → 99 + 78 | 316 k / 318 k → 636 k / 651 k | |

Meshes 5.5 → 10.3 MiB; renderer and scene targets unchanged (39.8 / 94.9 MiB). The props cost
~0.3-0.5 M triangles in view thanks to the far LODs (a house is 14 k triangles near, 2.6 k beyond
~80 m; trees 10-12 k / 1.2-1.6 k), still one instanced draw per mesh, submesh and LOD level. Zone 1
loads in 0.13 s (`world: ready after`; 0.09 s before) for 136 assets / 62 `.vtex` maps (86 MiB);
the cache's single worker became 4 (373 ms of read + CRC + parse summed over the workers).

Per-frame CPU work is culling ~600 spheres twice (plus the LOD split), 256 chunk boxes twice and
~10 skeletons; GPU work is dominated by the characters and terrain. If zones grow: terrain LOD with
skirts, merged static batches per region, an impostor / billboard level for distant trees.

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

## 9. Weapons and the inventory (M8 "Kuşan")

Contract: [PROTOCOL_ITEMS.md](PROTOCOL_ITEMS.md) §6; attachment data and maths:
[ASSET_PIPELINE.md](ASSET_PIPELINE.md) §4b.

**Data.** At startup `world::ItemData` loads `gamedata/items.toml` (the server's loader,
`shared/game/Items.h`) and `gamedata/attachments.toml` (`shared/game/Attachments.h`: strict, every
(grip, hand) needs its `look = "*"` row; lookup = exact look, else `*`). Missing or invalid files
are logged as errors; the client still runs, shows items as "?" and draws no weapons.

**Attachment.** `world::placeWeapons` (no D3D, unit-tested) turns a character's MainHand / OffHand
template ids into up to two world matrices:

```
grip  = items.toml template.grip          look key = "<race>_<effective look>"  ("aydor_0", "kargan_2")
A     = attachments.find(grip, look, hand).matrix()        T(translation) * R(rotation) * S(scale)
world = characterWorld * boneModel[bone] * A               boneModel = Animator::modelMatrices() of this frame
```

`characterWorld` is the same matrix the skinned body is drawn with (jump lift included), so the
weapon follows the animated hand exactly. The test `attachments: worked example` checks the
committed file against the numbers of ASSET_PIPELINE.md §4b (grip at (0.542, 0.941, -0.061), tip
at (0.916, 0.495, 0.464)) and that `A * boneModel` would be wrong. Two daggers = the same template
(and mesh) in both hands; a two-handed weapon leaves the off hand empty; an unknown template, a
missing row or a bone the skeleton lacks draws nothing. Weapon meshes (`weapons/<model>.vmesh`)
and icons are requested with the zone, so whatever a remote holds is ready when it spawns.

**Drawing.** `render::CharacterDraw::attached` carries up to two static meshes. `WorldRenderer`
queues them while it draws characters and draws the queued ones right after, with the static
(instanced) pipeline and one instance each from a small dynamic buffer (`kMaxCharacters * 2`
instances), in every pass: a weapon is culled with its owner (main pass and each shadow cascade),
casts and receives shadows, takes the owner's tint (spawn fade-in) and screen-door opacity (the
local player fading when the camera is close), gets the characters' rim light (`character = 1`)
and its emissive (staff crystals). Cost: one draw call per weapon and pass.

**Preview.** `CharacterPreview` (select / create) attaches the same way, loading the weapon mesh
synchronously. Create screen: the selected class's starter kit (`ItemCatalog::kitFor`), so the
figure changes with the class; select screen: the `CharSummary` weapons (also shown as icons on the
character cards).

**Icons.** `ui/items/<icon>.png` is requested as `AssetCache::Kind::UiImage`: the compiled
`.vtex` (premultiplied RGBA8, ASSET_FORMATS.md §5.1) is uploaded as is; the PNG fallback is
premultiplied on load. The sprite batch blends `ONE, INV_SRC_ALPHA`, so neither path gets dark halos.

**Inventory window** (`scenes/InventoryWindow`): not modal; I or the HUD button "Envanter (I)"
toggles it, Esc closes it before opening the menu, the movement keys keep working (no widget in it
takes keyboard focus). Left: Head, Chest, Legs | Hands, Feet, reserved (dimmed, M8 has no armour)
around a silhouette, MainHand + OffHand below it; right: the 7 x 4 bag. Drag & drop (legal targets
lit, the others darkened, decided by `planItemMove` on the shown inventory), double / right click =
quick move (`quickMoveTarget`), tooltip (name, kind · slot, attack / defense, classes, required
level; unmet requirements red; description) kept inside the screen. It shows
`world::ClientInventory::view()`: the confirmed state plus the unanswered moves replayed on top,
so a move appears at once; ITEM_UPDATE settles the oldest pending move, a refused ITEM_MOVE_RES
removes it (rollback) and puts its Turkish message (`net::itemMoveResultMessage`) in the window's
message line. Moves the shared rules refuse are not sent; at most 8 requests per second; none after
LEAVE_WORLD_REQ (`GameClient` refuses ITEM_MOVE_REQ like MOVE_REQ).

**Automation.** Widget names `inv.window`, `inv.equip.<main|off|head|...>`, `inv.bag.<n>`,
`inv.close`, `inv.message`, `inv.tooltip` (text of the shown tooltip); script verbs `dragon`,
`hoveron`, `dblclickon`, `rclickon` address widgets instead of pixels; world `scene` verbs in
`WorldScene.h`. Flows: `items` (1280x720, 1600x900, 1600x900 @125%), `classes` (8 creation-screen
shots), `two_clients_items` (B sees A's weapons on spawn and the switch to the axe).

**Known limits** (art, not engine): open hands, so handles lie against the palm; staffs and the axe
are held upright near the shoulder; on kargan_2 the mace partly sinks into the apron. The engine
screenshots match the Blender proofs `assets_src/previews/weapons/hold_<look>.png`. The inventory
window is anchored right and covers part of the character at 1280 px width.

## 10. Combat (M9 "İlk Kan")

Contract: [PROTOCOL_COMBAT.md](PROTOCOL_COMBAT.md) §5. Code: `scenes/WorldCombat.cpp` (the
combat half of `WorldScene`), pure logic in `world/CombatView.h` (unit-tested in
`tests/client/CombatViewTests.cpp`), the combat actions of `world/Animator.h`.

**Monsters.** `ENTITY_SPAWN` with `entity_type` 2 becomes a `RemoteEntity` like a player (same
interpolation, `ENTITY_MOVE`, despawn) plus `hp`, `max_hp`, `template_id` and the state bits. The
template (`gamedata/monsters.toml`, loaded by `world::ItemData` with the server's loader) gives the
model and the drawn `scale`. `AssetCache::requestMonster(model)` queues `monsters/<model>.vmesh`
and whichever character clips exist as `monsters/<model>/<clip>.vanim` (the combat set: idle, walk,
run, punch_jab, punch_cross, spell_*, hit_chest, death), bound like a character look. Monsters are
drawn on the skinned path (emissive maps included) with `makeWorld(pos, yaw, scale)`; walk / run are
chosen from the interpolated speed and the running bit and played at `speed / (clip.moveSpeed ×
scale)` (`Animator::setMoveScale`: the 1.25× Kor Bekçisi strides 1.25× as far). The name label reads
"<name> · Sv. N" (" · Elit" for elites) coloured by `levelTint(player, monster)`: grey 3+ levels
below, white up to equal, yellow 1–2 above, orange 3–4, red 5+. A corpse keeps its death pose
(held last frame) until `ENTITY_DESPAWN` and is labelled only while targeted. The HUD player count
and the automation state `remotes<N>` count players only.

**Character clips.** The look table now also resolves `sword_attack`, `punch_jab`,
`punch_cross`, `spell_enter/idle/shoot`, `hit_chest`, `death` and `sit_ground_enter/idle/exit`;
a combat clip whose file is missing is not requested (old asset trees still load). Animator actions:
`playAttack` (one clip, squeezed to ≤ 0.9 s, back to locomotion), `playHit` (only when idle /
moving: never interrupts a swing or a cast), `beginCast` / `endCast(shoot)` (spell_enter →
spell_idle loop → spell_shoot), `die` (held on the last frame; `die(true)` for a spawn that is
already dead), `revive`, and `setSeated` for the server-driven ground sit (`sit_ground_*`; the
chair clips only if the ground ones are missing).

**Which clip.** Own and remote basic attacks and the instant melee skills use `attackClipFor`:
`sword_attack` for a sword / dagger / mace / two-handed axe in the main hand, `spell_shoot` for a
staff, `punch_jab` / `punch_cross` alternating bare-handed; monsters always alternate the punches.
Cast skills: `CAST_START` → cast loop, `CAST_END` completed → `spell_shoot`. The victim of a damage
event plays `hit_chest` if nothing else runs. A remote player that swings or casts is drawn turned
towards its target for a moment (monsters face their target on the server).

**Targeting.** Left click: `pickAt` casts the mouse ray against a vertical capsule per entity
(radius 0.42 m × scale, height = mesh bounds × scale; a dead player is a low, wide capsule; living
monsters and players only) and drops the hit if the terrain is in front of it; a hit selects (and a
double click on a monster attacks), a miss walks there as before. Right click = press and release
within 6 px and 0.45 s (the camera drag counts the pixels it turned): attack what is under the
cursor; a real right drag still turns the camera. Tab (taken before the UI, which would move the
focus): `nextTabTarget` over living monsters within 40 m, nearest first, wrapping. Esc: inventory,
then target, then menu (M10: the loot window before all of them, §11). A new target sends `TARGET_REQ` (and `ATTACK_STOP_REQ` if the old one was
being attacked); a target that despawns is forgotten. Requests that count against the server's
10/s combat window (attack, skill, target, stance) are kept to 8 per second.

**Attacking.** F / right click / double click send `ATTACK_REQ` once; if the target is out of range
the character runs at it (`m_chasing`: a destination on the target every 0.25 s, a grid path when
the straight line is blocked) and stops a little inside the range (melee 1.9 m, the mage's staff
12.5 m). The server's one-time `OutOfRange` notice is not shown while chasing. WASD or a ground
click cancels the chase (not the server's auto-attack). Standing in range the character turns to
the target. Key 1 checks mana, cooldown, cast in progress and target type locally (Turkish message
instead of a request), walks into the skill's range if needed and then sends `SKILL_REQ`.

**HUD** (drawn by a full-screen, mouse-transparent `CombatHudWidget` above the panels and below the
inventory window; all sizes × UI scale): hp (red) and mp (blue) bars with numbers in the top-left
panel; the target frame at the top centre, kept between the left panel and the zone name /
inventory button (narrower down to 150 px when space is short) with the level-tinted name,
"Sv. N" ("Elit · Sv. N"), hp bar or "Ölü"; the experience bar ("Seviye N · xp / next deneyim")
above the two help lines (after M13 all of this is the HUD of §15); the skill button (`world.skill`, clickable) with its icon
`ui/skills/<icon>.vtex`, key hint "1", a clockwise cooldown sweep with the seconds left (global
500 ms cooldown included), greyed / blue-tinted without mana or while dead; the cast bar above it;
short Turkish combat messages above that (ACTION_FAIL texts from `actionFailText`, "+N deneyim");
a red edge flash when the local player is hit; "Güvenli Bölge" under the inventory button inside a
safe zone (`MonsterData::inSafeZone`).

**Floating numbers** (`FloatingNumbers`, over the target's head, rising and fading): white normal
damage, larger yellow "N!" critical hits, green "+N" heals, red damage the local player takes;
simultaneous numbers fan out sideways; at most 48. Hp bars over monsters that are damaged or
targeted and over players that took damage in the last 8 s or are targeted.

**Köz Oku.** On its `COMBAT_EVENT` an `effects/ember_bolt.vmesh` flies from the caster's hand
height to the target in 0.25 s on a slight arc (presentation only; the server applied the damage at
cast end), drawn with `WorldRenderer::addEffectMesh` (static pipeline, main pass, no shadow, 1.6×
size) plus a screen-space ember glow.

**Death and respawn.** `ENTITY_DEATH` / `ENTITY_STATE` dead for the local player: death clip,
movement and item moves locked (`InventoryWindow::setLocked`), auto-attack / chase / cast dropped,
and the death screen (`world.death`: dims and consumes clicks; "Öldün", a countdown, the
`world.death.respawn` button enabled 5 s after the death). `RESPAWN_RES` TooEarly re-disables it
for a second with "Henüz yeniden doğamazsın"; Ok teleports the prediction (`Prediction::teleport`,
no blend, older corrections ignored), resets the camera and sets the new hp / mp. Remote players
die and revive by `ENTITY_DEATH` / `ENTITY_STATE`; their jump to the spawn snaps (teleport
distance of the interpolation).

**Level-up.** `LEVEL_UP` for us: "Seviye N!" in large golden text over a soft band with "Can ve
mana yenilendi", a golden ring widening around the character and a short warm glow on its body
(tint), the class line in the HUD refreshed, the inventory's level rule updated. `XP_GAIN` drives
the xp bar.

**Stance.** X sends `STANCE_REQ`; the pose follows the server's `ENTITY_STATE` bit 2 (edge
triggered: a local stand-up started by a movement key is not undone by a late "sitting"). Remote
players sit and stand the same way; one that spawns sitting starts in the seated loop.

**Automation.** Scene commands (`client/world/CombatView.h` `parseCombatCommand`): `target nearest
[<name>] | none | self | player <name>`, `tab`, `attack`, `attackstop`, `cast`, `respawn`, `stance
sit|stand`, `walktomonster <name> [stopShort]` (a free spot on a circle around it, never in a safe
zone), `hunt <name> <level>` / `hunt stop` (target, attack, Ağır Darbe when ready, rest sitting
below 60 % hp, until the level), `expecthp`, `expecttargethp`, `expectlevel`, `expectcombatmessage`,
`combatlog`; `camera side`. States: `target`/`notarget`, `targetmonster`, `targetdead`,
`targetinrange`, `attacking`, `chasing`, `hunting`, `casting`, `dead`/`alive`, `canrespawn`,
`level<N>`, `safezone`/`outsidesafe`, `incombat`, `serversitting`, `fullhp`, `hpabove25/50/75/90`,
`skillready`, `levelupfx`, `bolt`, `numbers`, `monsters`, and counters (states for 1..12):
`kills`, `ownhits`, `skillhits`, `hitstaken`, `heals`, `healed`, `seenhits` (between two other
entities), `seendeaths`, `seenattacks` (remote attack / cast clips played), `seenheals`,
`seenplayerdamage`, `levelups`, `respawns`, `bolts`.

## 11. Loot (M10 "Ganimet")

Contract: [PROTOCOL_LOOT.md](PROTOCOL_LOOT.md) §9. Code: `scenes/WorldLoot.cpp` (the loot half of
`WorldScene`), `scenes/LootWindow.{h,cpp}`, `scenes/ItemTooltip.{h,cpp}` (tooltips shared with the
inventory), pure logic in `world/LootView.h` (unit-tested in `tests/client/LootViewTests.cpp`).

**Bags in the world.** `ENTITY_SPAWN` with `entity_type` 3 becomes a `RemoteEntity` with
`isLoot()` (name / level / template of the monster it came from, the LootMine / LootLocked state
bits; it never moves). It is drawn as `props/loot_bag.vmesh` with `WorldRenderer::addEffectMesh`
(static pipeline, emissive ember bed, 1.3× size, turned by its id, settling onto the ground over
0.35 s) plus a blob shadow and a ground ring; `addEffectMesh` got a base-colour tint and takes up to
48 meshes a frame. `lootLabel(state)` picks the look (`lootLabelStyle`): **ours** (LootMine) gold
label and a breathing gold ring; **locked** (LootLocked, someone else's for now) grey label at 80 %,
a faint grey ring and the bag dimmed to half; **free** a parchment label and a soft warm ring. The
label "Ganimet" floats just above the bag (occluded by hills and props like the name labels); while
the bag is hovered or open it reads "Ganimet · <monster>" and, if it is not free, a second line
"Senin ganimetin" / "Başkasının ganimeti". `ENTITY_STATE` updates the bits (the owner window
ending turns every bag free); `ENTITY_DESPAWN` removes it. Bags never count as players or monsters
(`WorldModel::lootCount()`), are never targeted and hide no labels.

**Opening.** `pickAt` also has a small capsule per bag (radius 0.5 m, 0.75 m high); a left click
(single), a right click without drag or **E** (`lootInteract`: the nearest bag within 12 m that is
not locked) opens it: within 2.5 m `LOOT_OPEN_REQ` at once, else the character walks there first
like an attack walks into range (a destination on the bag, a grid path if the straight line is
blocked; WASD or a ground click cancels) and sends it at 2.2 m. `LOOT_OPEN_RES` Ok shows the window
(a stale answer for another bag is ignored); a refusal is a short Turkish message
(`lootResultText`, PROTOCOL_LOOT §9: "Bu ganimet henüz başkasının", "Çok uzaktasın", …) on the
bottom message line. E with no bag near: "Yakında ganimet yok" (or the NotYours text if only a
locked one is near). Loot requests are kept to 8 per second (server: 10/s).

**Loot window** (`loot.window`, not modal, Window style, 264 logical px wide): on the left below
the HUD panel (below the F3 info panel when it is shown; moved up when the screen is short) so the
character in the middle and the inventory window on the right stay visible, at every size from
1024x640 @150 % up. Subtitle "<monster> · Sv. N" with "Senin" / "Başkasının" / "Herkesin" on the
right; rows (`world::LootWindowModel::rows()`): the Akçe row first (coin icon `ui/items/coin`,
"N Akçe" in gold, only if coins > 0), then each entry by index with its icon (a neutral "?" well if
the icon is missing), the name in its rarity colour (`rarityRgb`: common warm white, uncommon
green, rare blue) and "×N"; hover shows the item tooltip. A click on a row sends `LOOT_TAKE_REQ`
with its index (0xFE for Akçe), "Hepsini Al · E" (or E) sends 0xFF. `LOOT_CONTENTS` replaces the
rows (taken units disappear at once on the server's side, returned ones come back). Results other
than Ok show on the window's message line and the bottom message line. The window closes on
`LOOT_CLOSED` (OutOfRange: "Ganimetten uzaklaştın"), when the bag despawns, when the character is
more than 4.5 m away (sends `LOOT_CLOSE_REQ`), on death and when leaving the world (no packet), and
on Esc. **Esc order** (`escapeAction`): loot window, inventory, target, menu (M11: the Künye between the loot window and the inventory, §12).

**Takes and the inventory.** Each take gets a client seq (`LootWindowModel::beginTake`) and is
registered with `ClientInventory::beginTake`: the server answers moves and takes in request order,
and a take's `ITEM_UPDATE`s (0..n packets before its `LOOT_TAKE_RES`) must not settle an item move
sent after it; `ClientInventory` therefore settles the oldest pending move only if no older take is
still unanswered (unit test with a merge in flight). Nothing is predicted for a take: the bag view
follows `LOOT_CONTENTS`, the inventory follows `ITEM_UPDATE`.

**Akçe, stacks, pickups.** `COINS` sets the balance shown in the inventory footer (`inv.coins`:
"Akçe", the coin icon, the grouped number `formatThousands`: 1.234.567). Bag slots show the stack
size bottom-right when > 1. Dragging a stack onto a non-full stack of the same material is a normal
move (the drop target lights up: the shared `planItemMove` merges, so the view predicts it; the
server decides). Tooltips (`itemTipLines`, inventory and loot window): name in its rarity colour;
"Malzeme · Sıradan" / "Silah · Sağ el · Seyrek"; "Adet: N / max" for stackable items; attack /
defense; class and level only for weapons and shields (unmet in red); "Değer: N Akçe (yığın M
Akçe)"; the description. Pickup feedback: every `ITEM_UPDATE` compares the confirmed inventory
before and after (`inventoryGains`: moves and merges gain nothing) and every `COINS` with
`gained > 0` adds a line ("+2 Kül Tozu" in the item's rarity colour, "+7 Akçe" in gold; fix1: the format of
PROTOCOL_SHOP.md §8.5, the same for loot, purchases and sales; before it was "Kül Tozu x2 alındı") to a
short log at the bottom left above the experience bar, drawn above the windows, at most 5 lines,
3.6 s each.

**Automation.** Scene commands (`client/world/LootView.h` `parseLootCommand`): `lootnearest [any]`
(as E; `any` also opens locked bags, to test the refusal), `walktoloot [mine|locked|free|any]`,
`takeall`, `take <index>|coins`, `lootclose`, `expectcoins [op] <n>`, `expectitem <template> [op]
<units>`, `expectstack <slot> <template> <quantity>`, `expectlootrows <n>`, `expectlootmessage
<text>`, `lootfarm <monster> <template> <pickups>` / `lootfarm <monster> rows <n>` / `lootfarm stop` (the hunt loop that also loots
every own / free bag until that many pickups of the template, or until a bag with at least n rows is open, which then stays open), `movestack <template> <slot>`,
`lootlog` (op: `=`, `>=`, `<=`, `>`, `<`). States: `lootvisible` / `noloot`, `lootmine`,
`lootlocked`, `lootfree`, `lootinrange`, `lootwin` / `lootwinclosed`, `lootcontents` / `lootempty`,
`loottaking` / `lootidle`, `lootwalking`, `lootopening`, `coinsknown`, `pickupshown`,
`lootfarming`, `farmdone`, `remotesitting` (the marked remote sits: a signal between two scripted
clients) and counters (1..12) `lootbags`, `lootopened`, `loottakes`, `lootrefused`, `pickups`,
`coingains`, `lootclosed`, `lootspawns`, `lootdespawns`. Widgets: `loot.window`, `loot.coins`,
`loot.item.<k>` (k-th item row), `loot.takeall`, `loot.close`, `loot.message`, `loot.source`,
`loot.contents` (invisible: one row per line, for `expecttext`), `inv.coins`.

## 12. Character sheet (M11 "Künye")

Contract: [PROTOCOL_STATS.md](PROTOCOL_STATS.md) §7. Code: `scenes/WorldStats.cpp` (the stats half of
`WorldScene`), `scenes/KunyeWindow.{h,cpp}`, pure logic in `world/StatsView.h` (unit-tested in
`tests/client/StatsViewTests.cpp`).

**State.** `CHAR_STATS` (on entry, after level-ups, equipment bonus changes and accepted
allocations) feeds `world::StatSheet`: the authoritative base / bonus / unspent / derived values,
plus the player's *pending* points and the one request in flight. `SELF_STATS` stays the fast hp /
mp channel of the bars. The spent points are `base − classStartAttributes(class)`; the preview is
`game::deriveStats(class, level, spent + pending, equip)` with `equip` = `equipmentBonus` of the
confirmed inventory (attack / defense sums) and the attribute bonuses of CHAR_STATS, converted to
CHAR_STATS units by `toCharStats` (the server's tenths / permille rounding). Without pending points
that conversion equals the server's CHAR_STATS (unit test for all four classes with the warden's
shield). The inventory's rules (`ItemUser::attributes`) are the CHAR_STATS base values, so
requirements are predicted (`AttributeTooLow`) and shown red in tooltips.

**Window** (`kunye.window`, not modal, Window style, 580 × 388 logical px): left side, vertically
centred, so at 1280 logical pixels it sits next to the inventory (right side) without overlapping;
at 1024x640 @150 % (683 × 427 logical) it still fits but covers the HUD panel and the bottom help
lines while open (§15: with the fitted scale that screen is 960 × 600 logical and the Künye fits between the player frame and the bar). The loot window opens over it. Header: name, "Aydor · Savaşçı · Seviye N",
"Deneyim: x / y". Left column "Nitelikler": one row per attribute (a gold diamond marks the class's
attack attribute) with the base value (gold), the item bonus " (+3)" in green and pending points
"  +2" in amber on an amber-tinted row; "−" (only while that row has pending points) and "+"
(drawn glyphs, not font characters); "Dağıtılmamış puan: N" (amber while N > 0, "· seçilen: k"
while points are pending); "Onayla" / "Vazgeç". Right column "Değerler": Can, Mana, Saldırı Gücü
(Mage: Büyü Gücü), Savunma, Kritik Vuruş %x,y, Kaçınma %x,y, Can / Mana Yenilenmesi x,y/sn,
Saldırı Hızı x,y sn, Priest: İyileştirme %x,y; with pending points every changed row shows
"36  →  41" (the preview in amber). Hover on an attribute row: tooltip with the contract's sentence
and what it means for the class ("Savaşçı olarak saldırı gücün bu niteliğe bağlı.", "... saldırı
gücünü artırmaz."), plus "Temel N · eşyalardan +k".

**Spending.** "+" adds one pending point (Shift: 5 or the rest), never past `unspent` in total or
255 per attribute; right click on "+" or a row, or "−", takes one back; "Vazgeç" drops all.
"Onayla" sends one `STAT_ALLOCATE_REQ` with all five amounts; the first time on a connection a
message box asks "Dağıtılan puanlar geri alınamaz. Onaylıyor musun?" (Evet / Hayır;
`AccountState::statSpendConfirmed`). While the request is in flight, and after its Ok until the next
CHAR_STATS (at most 3 s), every button is disabled and the pending points stay shown as the
preview; CHAR_STATS then replaces them ("Puanlar dağıtıldı."). Refusals keep the pending points
(trimmed to the answer's `unspent`) and show the contract's Turkish text in red on the window's
message line. **Esc order** (`escapeActionWithKunye`): loot window, Künye, inventory, target, menu.

**HUD.** "Künye (C)" (`world.kunye`) under "Envanter (I)"; the safe-zone indicator moved below it. (After M13: a menu button of the HUD bar, §15.)
While `unspent > 0` a gold disc with the number and a pulsing glow sits on the button's left edge.
The level-up toast gets a third line "+3 nitelik puanı  ·  Künye: C" (3 per level gained).

**Dodge.** `COMBAT_EVENT` kind 3 (Miss): `FloatKind::Miss`, a grey (0xB9BEC6), slightly smaller
"Iska" over the dodger instead of a number, no hit reaction (the attacker still swings); on the
local player also the message "Kaçındın!" and a log line.

**Item tooltips.** `itemTipLines` adds the attribute bonuses ("Dayanıklılık +3", green) after attack
/ defense and the requirements ("Gerekli Güç: 20", red when the base value is lower) after the level
line.

**Automation.** Scene commands (`world/StatsView.h` `parseStatsCommand`; attributes by key
`strength` … or ASCII Turkish `guc ceviklik dayaniklilik zeka irade`): `kunye open|close`,
`statadd <attr> [n]`, `statremove <attr> [n]`, `expectattr <attr> <base> [bonus]`, `expectunspent
<n>`, `expectpending <attr> <n>`, `expectderived <field> <value>` / `expectpreview <field> <value>`
(fields `maxhp maxmp attack defense crit dodge hpregen mpregen speed heal`, display units: percent,
per second, seconds), `expectstatmessage <text>`, `statlog` (also warns if the client's derivation
differs from CHAR_STATS). States: `kunye` / `kunyeclosed`, `statsknown`, `unspent<N>` (exact),
`points` / `nopoints`, `unspentmarker`, `pending` / `nopending`, `statbusy` / `statidle`, `canadd` /
`noadd`, `levelpoints` (the toast's points line is shown) and counters (1..12) `charstats`,
`allocated`, `allocrefused`, `dodged`, `seenmiss`. Widgets: `kunye.window`, `kunye.header`,
`kunye.class`, `kunye.xp`, `kunye.attr.<key>` (text "Güç 14 (+2) +3"), `kunye.plus.<key>`,
`kunye.minus.<key>`, `kunye.unspent`, `kunye.derived` (invisible: one row per line, "Can 110 → 115"),
`kunye.confirm`, `kunye.cancel`, `kunye.close`, `kunye.message`, `kunye.tooltip`, `world.kunye`.
Shift+click in a script: `keydown shift`, `clickon kunye.plus.<key>`, `keyup shift`.

## 13. Skills (M12 "Yetenek Yolları")

Contract: [PROTOCOL_SKILLS.md](PROTOCOL_SKILLS.md) §7; art: [SKILL_ART.md](SKILL_ART.md). Code:
`scenes/WorldSkills.cpp` (the skills half of `WorldScene`), `scenes/SkillBar.{h,cpp}` (bar slots, the
HUD button, effect icon rows), `scenes/SkillWindow.{h,cpp}` ("Yetenekler"), pure logic in
`world/SkillsView.h` (unit-tested in `tests/client/SkillsViewTests.cpp`) and the per-skill
`world::SkillTimers` of `world/CombatView.h`. The widgets draw through the `SkillUiHost` interface
(implemented privately by `WorldScene`); they own nothing.

**State.** `SKILL_STATE` / `SKILL_LEARN_RES` / `SKILL_BAR` / `SKILL_BAR_SET_RES` feed
`world::SkillBook` (ranks normalised with the shared rules, points, unspent, the bar, one learn and one
bar request in flight, the last bar wish queued while one is in flight). "+" is offered when
`game::checkSkillLearn` (local level) is Ok *and* the server's unspent > 0. `SKILL_COOLDOWNS` sets
exact per-skill cooldowns; `SKILL_USED` / instant `COMBAT_EVENT`s / completed casts predict them in
between (a running cooldown is never restarted by a further hit of the same use). The global 500 ms
cooldown sweeps every filled slot. `EFFECT_LIST` / `APPLY` / `REMOVE` feed `world::EffectBook` per
entity (refresh by (skill, kind), at most 16); an entity's list is dropped on its `ENTITY_SPAWN`,
`ENTITY_DESPAWN` and `ENTITY_DEATH` (peeked before the world model handles them), and two seconds
past an effect's end as a safety net. Learn + bar requests share the server's 10/s limit: the client
sends at most 8 a second and lets a faster click wait (sent from `updateSkills`).

**Bar** (`skillbar`, slots `skillbar.slot.1`…`8`, 48 logical px, 6 apart, bottom centre 74 px up):
per slot `slot_back` → icon (×52/64) → red veil + reddened icon below the rank's mana cost /
darker veil while an enemy skill has no living monster selected or it is out of range / grey while
dead → clockwise sweep of the binding cooldown (own or global) with the seconds → `slot_ready_flash`
for 0.25 s when an own cooldown ends → `slot_frame_normal|hover|pressed` → `key_badge` + digit;
`slot_empty` for empty slots. Keys 1–8 and a click use the slot (`decideSkillUse`: enemy → the
selected monster, walking into the rank's range first; friendly → a selected living player, else 0 =
self; self → 0). Drag a slot onto another (swap) or off the bars (clear), right click clears; the
window's cells and its own bar row (`skills.bar.<n>`, 34 px) take part in the same drag & drop. The
tooltip (§7.3, `world::skillTipLines`) is drawn centred above the hovered slot after the UI pass
(`drawSkillOverlay`), with the dragged icon. The M9 single skill button is gone; the cast bar sits
above the bar.

**Window** (`skills.window`, 656 × 424 logical, left side, vertically centred, above the Künye; fits
683 × 427 logical = 1024x640 @150 %): "Yetenek puanı: N" (gold while N > 0) and the class line;
per branch a `branch_<key>` plaque with the name, three tier rows (tier 3 of branch 0 holds two
skills side by side), `tree_v` / `tree_h` connectors lit once the lower tier's gate is met, a pill
(padlock icon) "Bu yolda N puan gerekli" in the gap above each locked row (the cells under it greyed), "Bu yolda
harcanan: N" under each column, the bar row, a message line. A cell: icon (dimmed at rank 0, grey
while locked), `rank_pips_<k>` under it, the name (two lines at most), "k/3" or "Öğrenilmedi", "+"
(`StatStepButton`) while the skill can be raised, disabled while a request is in flight. The first
"+" of a connection asks "Harcanan yetenek puanları geri alınamaz. Onaylıyor musun?"
(`AccountState::skillSpendConfirmed`). Results go to the message line (and the combat message while
the window is closed). **Esc order** (`escapeActionWithSkills`): loot window, Yetenekler, Künye,
inventory, target, menu.

**HUD.** (After M13 a menu button of the HUD bar, §15.) "Yetenekler (K)" (`world.skills`) under "Künye (C)" with a `skill_point_marker_blank` disc
carrying the unspent points (pulsing) and a tooltip; the safe-zone indicator moved one row down. The
player's effect icons (`effects.self.<i>`, 26 px) sit under the HUD panel (the F3 info panel moved
down), the target's (`effects.target.<i>`) under the target frame: the skill's icon with the effect's
status icon (`ui/status/`) in the corner, a green (helpful) / red (harmful) frame that blinks in the
last two seconds, the seconds left under it and, for a shield, the absorb left on it; the tooltip is
the skill's name, the kind line of §7.7 ("Saldırı +%15", "Zehir: saniyede 9", "Kalkan: 54",
"Sersemledi", …) and the seconds left. Periodic `COMBAT_EVENT`s show smaller numbers
(`FloatKind::PeriodicDamage|PeriodicHeal`), a fully absorbed hit a grey "Emildi"; a heal of 0 (full
bar) shows nothing. The level-up toast has up to four lines ("+1 yetenek puanı  ·  Yetenekler: K" as
the fourth); the block is placed so it ends 8 px above the own name label (below the target frame),
shrinking down to 60 % when that space is short — the M11 third line no longer covers the label.

**Effect art** (`world::skillUseArt` / `effectAura` / `tickArt`, drawn in `addSkillDraws`):
`SKILL_USED` starts the caster's swing (enemy skills that are not projectiles or target areas) or
`spell_shoot` (instant spells; cast skills already released at `CAST_END`) and the skill's art:
Köz Oku's ember bolt and Kırağı Oku's spinning frost bolt fly hand → chest (`m_bolts`, with a glow);
Işık Çarpması's `light_strike` at the target's feet; Kor Yağmuru's ten `ember_mote`s fall from 7 m
into the area over 0.6 s over a `ground_ring_flame`; Alev Halkası / Donduran Nefes / Halka Şifası /
Yenileyen Işık / Soluk Güneş / Buz Kabuğu / Bağlayan Söz / Yarma / Meydan Okuma: ground rings at the
caster or target (scaled to the radius, tilted to the terrain normal, slow spin, fade); Sis Bombası:
`fog_cloud` + `ground_ring_fog` for the slow's duration; buffs: a `buff_aura_*` / bubble burst. While
an effect lasts: `stun_marker` over the head, `root_spikes`, `shield_bubble_frost` (Buz Kabuğu) /
`_gold` (Kutlu Siper), a faint `buff_aura_*` for attack / defense / haste / dodge / heal-over-time
buffs. DoT ticks raise a `tick_wisp_burn` / `_poison` from the chest. `SKILL_LEAP`: the leaper glides
0.25 s from its take-off point to the landing point with a 0.35 m hop (the local mover is
teleported at once, the draw position and the camera follow the glide; the `POSITION_CORRECT` after
it is stale and ignored), and a `leap_trail` ribbon fades over the path.

**Translucent effects (renderer).** `WorldRenderer::addEffectMesh(mesh, world, tint, opacity,
tintEmissive)`: opacity < 1 meshes are drawn after the decals, far to near, with premultiplied
blending, depth tested, not written (the static mesh shader outputs `rgb × a, a` when the draw's
`alphaTest.z` > 0); `tintEmissive` multiplies the submesh's emissive factor by the tint (the greyscale
`_neutral` rings take any colour). Fog, bubbles, rings and auras are 0.35–0.9 opaque; bolts, motes,
stun stars and spikes stay opaque.

**Automation.** Scene commands (`world/SkillsView.h` `parseSkillsCommand`; skills by id or key):
`skills open|close`, `skilllearn <skill>` (as "+"), `barset <slot> <skill|none>`, `useslot <slot>`,
`expectrank <skill> <k>`, `expectskillpoints <n>`, `expectslot <slot> <skill|empty>`,
`expectslotlook <slot> ready|nomana|notarget|dead|empty`, `expectcooldown <skill> <min> <max>`,
`expecteffect` / `expectnoeffect self|target <kind> [skill]` (the effect book and its HUD icon),
`expecteffecttip self|target <kind> <text>`, `expectart <mesh> <min>` (instances submitted to the
renderer last frame), `expectskillmessage <text>`, `pullpack <monster> <n>` (walk near idle monsters
until n chase us), `skilllog`. States: `skillsopen` / `skillsclosed`, `skillsknown`,
`skillpoints<N>`, `skillmarker`, `canraise` / `noraise`, `learnbusy` / `learnidle`, `barbusy` /
`baridle`, `locked<b>_<t>` / `unlocked<b>_<t>`, `ready_<key>` / `cd_<key>` (bar skills; ready = own and
global cooldown over), `gcd`, `selfeffect_<kind>`, `targeteffect_<kind>`, `art_<mesh>`, `leaping`,
`skilldrag`, `skilllevelup`, `toastabovelabel`, `pulling` / `pulled` and counters `skillstates`,
`learned`, `learnrefused`, `barsets`, `barrefused`, `skilluses`, `otheruses`, `selfeffects`,
`effectsapplied`, `effectticks`, `absorbs`, `leaps`, `cooldownlists`, `usehits` (targets hit by the
last own use), `closemonsters` (living monsters within 3 m). Widgets: see `SkillBar.h` /
`SkillWindow.h`; probes `skills.tooltip` (any skill tooltip, also the HUD button's) and
`effects.tooltip`.

## 14. Zones (M13 "Üç Yurt")

Contract: [PROTOCOL_ZONES.md](PROTOCOL_ZONES.md) §7; art: [WORLD_ZONES_ART.md](WORLD_ZONES_ART.md). Code:
`scenes/WorldZones.cpp` (the zones half of `WorldScene`), pure logic in `world/ZoneView.h` (names and
taglines from `game::zones()` with the `shared/game/Zones.h` table as fallback, the fixed Turkish texts,
gate views, veil mesh per gate prop, label fade, lore lines per zone; unit-tested in
`tests/client/ZoneViewTests.cpp`).

**Zone change.** `ZONE_CHANGE_BEGIN` (`beginZoneChange`): the destination, route, pending click, held
move, camera drag, skill drag, target, auto-attack, chase, queued skill and cast are dropped, the loot
window closes without a packet, every remote entity, effect, bolt, art instance and number is dropped
(no ENTITY_DESPAWNs follow), the Esc menu closes, the UI root is hidden and `render()` draws the
loading screen instead of the world: the target zone's name and tagline, "Geçitten geçiliyor…", a
sweeping bar and one of the target zone's lore lines. `onFrame` does not simulate the world meanwhile;
it only pumps the asset cache, which has been asked for the target terrain (so the next scene usually
finds it cached). Input is ignored (`m_zoneChanging`), message boxes still work. `ZONE_CHANGE(Ok)`:
`account.world` takes the new ENTER_WORLD_RES body and the scene is replaced (`UiScene::changeScene`
stops draining events, so everything after ZONE_CHANGE goes to the new scene); the new scene's loading
screen says "<Bölge> yükleniyor…" with the file progress, `becomeReady` sends `ZONE_LOADED_REQ`, starts
the banner and shows the arrival line "<Bölge> bölgesine geçtin." in the combat message area (the
client has no scrolling combat log; this line is its system line). `ZONE_CHANGE(Failed)` in a ready
scene of the same zone (`resumeAfterFailedChange`): `WorldModel::enter` with the new entity id and
place (terrain kept), combat state and effects reset, `ZONE_LOADED_REQ`, the HUD back, "Geçit şu an
kullanılamıyor, tekrar dene." and no banner; a failure while the scene was still loading rebuilds it the
same way (the line instead of the arrival). `ZONE_GATE_REFUSED`: "<Bölge> yalnızca Aydorlulara /
Karganlara açık." in the combat message area.

**Banner.** Top centre (where the target frame goes; nobody has a target on entering, and selecting
one fades the banner out), 4 s in all (0.6 s in, 2.6 s held, 0.8 s out; before M13 5.2 s): the name (40 px, 30 px if the gap between
the player frame and the zone name (§15) is too narrow, else the banner moves below the panels), a gold rule, the
tagline. Before M13 it sat in the lower third, where the M12 skill bar covered its tagline.

**Gates.** `buildGateViews` (on ready): for every `[[zone.gate]]` of this zone the marker's position and
yaw, whether our race may enter the target, the label "<Bölge> Geçidi", the hint ("Geçide yürü:
<Bölge>" or the closed text), the gate prop (the nearest `props/gate_*` placement within 4 m of the
marker) and its veil `effects/gate_veil_<look>.vmesh`. Per frame: the veil is submitted with
`addEffectMesh` at the prop placement's own matrix, opacity 0.5 ± 0.08 breathing (the three veil
meshes are requested while loading every zone); the label is drawn 0.7 m above the prop's top within
40 m of the player (fading from 32 m; hidden behind terrain, not behind houses: it is a sign seen over
the roofs), gold for an open gate, rust for a closed one; within 8 m the hint is a pill near the bottom
(above the combat message; not while the combat message says the same words).

**Automation.** `scene` commands: `gates` (log), `expectgates <n>`, `walktogate <toZone|marker>
[metres in front]` (path-found; 0 walks into the trigger), `facegate <toZone|marker> <pitch> <dist>`
(camera behind the player towards the gate), `expectgatelabel` / `expectnogatelabel <text>` (labels
drawn last frame; the failure names each gate's distance and why its label was not drawn),
`expectgatehint <text>` / `expectnogatehint`, `expectzonemessage <text>` (the last arrival / refusal /
failure line), `expectveils <n>`, `expectzoneseen <begins> <failed> <refusals>` (counts of this scene),
`zonehold <gateSeconds> <loadSeconds>` (process-wide: the next zone changes keep the gate screen /
loading screen up at least that long, for screenshots; `UiScene::holdNetEvents` leaves the queued
packets where they are meanwhile), `zonefail <toZone>` (a local simulation of ZONE_CHANGE(Failed):
BEGIN and a Failed ZONE_CHANGE with the current entity and place are injected into the client's own
event queue; no ZONE_LOADED_REQ is sent afterwards). States: `zone<N>`, `zonechanging`, `gatescreen`,
`gatehint` / `nogatehint`, `gatelabel`, `nobanner` (and `banner`).

## 15. HUD (after M13)

Code: `scenes/HudBar.{h,cpp}` (widgets), the HUD half of `WorldScene` (`buildHud`, `buildHudBar`,
`layoutHud`, `drawPlayerFrame` in `WorldCombat.cpp`), pure logic in `world/HudView.h` (bar layout,
button names / keys / tooltip lines, the controls reference, window placement, experience texts, the
Esc order; unit-tested in `tests/client/HudViewTests.cpp`). Art: `ui/hud/*.png` button glyphs
(`tools/blender/gen_hud_art.py`, same painter as the M12 icons) drawn in the M12 slot pieces
(`ui/skillbar/slot_back`, `slot_frame_*`, `slot_ready_flash`, `key_badge`).

**Gone:** the two grey help lines along the bottom (`world.help`, `world.help.combat`) and the three
text buttons at the top right (`Envanter (I)`, `Künye (C)`, `Yetenekler (K)`). Every control is in the
"Tuşlar" window now.

**Player frame** (`world.playerframe`, top left, 300x92 logical): a round level medallion (gold rim,
red-gold and a soft red pulse while in combat, "SV" over the number), the name (`world.name`), "race ·
class" (`world.level`; the level is in the medallion), the hp (16 px) and mp (12 px) bars with numbers.
The player's effect icons sit under it (`kHudSelfEffectsTop`). **Target frame** (top centre between the
player frame and the zone name, up to 300x66): name in the level tint, a "Sv. N" chip, a gold "Elit"
chip and frame for elites, a second line ("Canavar", "Elit canavar", or the race · class of a player;
"Ölü" when dead), the hp bar with numbers; its effect icons under it. **Zone name** top right on a soft
dark plate that fades in from the left, "● Güvenli Bölge" right under it.

**Bar** (`hud.bar`, bottom centre): one framed piece (the windows' palette: dark gradient, bronze border,
inner bevel, a gold rule on the top edge with diamonds, corner brackets). Left to right: four action
buttons (40 logical), a separator, the 8 skill slots (`skillbar`, 48 logical, behaviour unchanged), a
separator, five menu buttons (34 logical); under them the experience bar (violet, tenth ticks,
"Seviye N · xp / next · %p", a shorter "Sv. N · %p" if it does not fit; hovering it shows "Deneyim
(Seviye N)", the exact numbers and what is left). `computeHudBar` lays it out for the logical screen:
843x86 logical at full size, shrunk uniformly (to 0.6 at most) only when the screen is narrower than
859 logical pixels (the fitted UI scale never makes it that narrow: 1024x640 @150 % is 960x600 logical,
`UiScale.h`). Clicks on the frame do not reach the world.

| Button | Widget | Key | Does | State shown |
|---|---|---|---|---|
| Saldır | `hud.attack` | F | `startAttack()` | lit while auto-attacking; darker without a living monster selected |
| Otur / Kalk | `hud.sit` | X | `toggleSit()` | lit while seated |
| Ganimet | `hud.loot` | E | `lootInteract(false)` | pulses while a bag we may take from is within reach, lit while the loot window is open, darker otherwise |
| Yürü / Koş | `hud.run` | R | toggles the default gait | the glyph: a boot while running, footprints while walking |
| Envanter | `world.inventory` | I | the inventory window | lit while open |
| Künye | `world.kunye` | C | the Künye | lit while open; a pulsing gold disc with the unspent stat points |
| Yetenekler | `world.skills` | K | the Yetenekler window | lit while open; a pulsing gold disc with the unspent skill points |
| Tuşlar | `hud.controls` | H | the controls reference | lit while open |
| Menü | `hud.menu` | Esc | the Esc menu | lit while open |

Every button carries its key in a corner badge (the M12 key plate, nine-sliced for "Esc") and has a
tooltip over the bar: "<name> (<key>)", one line, and its state where it has one ("Şu an: oturuyorsun",
"Şu an: yürüyüş (Shift basılıyken koş)", "Önce bir canavar seç…", unspent points, "Açık: …"). Probe
`hud.tooltip` (the Yetenekler button's text also goes to `skills.tooltip`, as in M12).

**Bottom lines** (`hudBottomLines`): the cast bar, the combat / zone message line, the near-gate hint
pill and the pickup log's bottom are placed from the bar's top, so none of them can touch the bar.

**Windows next to the HUD** (`placeWindow`, per frame in `layoutHud`): the Künye and the Yetenekler
window on the left, the inventory on the right, "Tuşlar" centred. A window is centred in the band
between the HUD above it (the player frame and its effect row on the left; the zone name on the
right) and the bar; if it is taller than that band, it goes from the top of the screen down to the
bar; only if it is taller still is it centred on the whole screen. Whatever an open window still
covers (the bar, the player frame with its effect row, the target frame, the zone name with its plate,
the F3 panel) is hidden until it closes, so nothing is drawn under or over a window. At 1280x720 every
window fits in the free band; at 1024x640 @150 % (960x600 logical) the Künye fits, the Yetenekler window
(424 logical tall) covers the player frame (it hides) and never the bar.

**"Tuşlar"** (`controls.window`, H, the bar button, closed by Esc / H / its "X" `controls.close`): two
columns of groups (Hareket, Kamera, Pazar (M14), Diğer | Dövüş, Pencereler), each row the keys as key caps and one
line; "Kapatmak için Esc ya da H" under them. The window measures its text and shrinks its content
(not the title bar) to fit the screen. Opaque backing (it opens over other windows). Probe
`controls.text`: every row as "Grup: tuş / tuş — açıklama".

**Esc order** (`escapeActionHud`): loot window, Tuşlar, Yetenekler, Künye, inventory, target, menu (M14
puts the shop window after the loot window: `escapeActionShop`, §16).

**F3** info panel: off by default (`m_showInfo = false`); flows that need its labels read them hidden
(`expecttext` does not need a visible widget).

**Automation states:** `controlsopen` / `controlsclosed`, `rundefault` / `walkdefault`, `infopanel` /
`noinfopanel`, `nomenu`, `upright` (not seated), `hudbar` / `hudbarhidden`, `playerframehidden`. Flow:
`hud` (one run: 1920x1080, then resized to 1280x720 and 1024x640 @150 %; screenshots
`docs/screenshots/hud/01…25`).
## 16. Merchants, the shop and potions (M14 "Pazar")

Contract: `docs/PROTOCOL_SHOP.md` §8. Code: `scenes/WorldShop.cpp` (the shop half of `WorldScene`),
`scenes/ShopWindow.{h,cpp}` (the shop window and the quantity box), pure logic in `world/ShopView.h`
(`NpcBook`, `ShopSession`, `ConsumableCooldown`, `QuantityModel`, stock entries, decisions, every text of
§8.3, the potion tooltip, `escapeActionShop`, the `scene` commands; unit-tested in
`tests/client/ShopViewTests.cpp`). Art: `npcs/<key>.vmesh` + clips, `ui/shop/*`, `ui/items/potion_*`
(`docs/SHOP_ART.md`).

**Merchants** (entity type 4) never enter the `WorldModel`: `handleShopMessage` takes their
`ENTITY_SPAWN` / `ENTITY_DESPAWN` into a `world::NpcBook` (id, npcs.toml entry by the spawn's
template id, position, yaw). So they are no remote, no target, no Tab candidate, never picked for an attack
or a skill, and the `remotes<N>` count stays the players'. Assets: `AssetCache::requestNpc` (the monster
layout under `npcs/`: mesh + whichever clips exist) for the merchants of the scene's zone. Drawn like a
monster (skinned, blob shadow, fade-in), at scale 1 (1.75-2.04 m, the players' size). Clips: the new
`CharacterClip` / `AnimState` entries `IdleTalking` and `Interact` (players have no such files: never
requested); `Animator::setTalking` swaps the standing idle for `idle_talking` while their shop is open with
us (the merchant also turns towards us at 3 rad/s and back to its own yaw after), `playInteract` plays
`interact` once on every purchase / sale. Side by side merchants start their idle at different phases.

**Over the head** (`drawNpcOverlay`): the title "‹Şerbetçi›" (12.5 px, pale teal) right over the head and
the name above it (15 px bold, teal `0x86E3D3`: neither the players' off-white nor a monster's level
tint), fading 28 → 70 m; hidden by hills and by props (but not by the merchant's own stall, whose
awning box reaches over its head). The marker (`ui/shop/marker_potion` for `npc_potion_*`,
`marker_smith` for smiths) is a screen-space billboard: the projected size of 0.55 m at head + 0.70 m,
clamped to 30-52 logical px, its bottom always 3 px above the name plate (so the plate never runs into it
at any distance), bobbing ±2 px; fades 40 → 60 m. **Hover**: the merchant is drawn brighter, a teal ring
breathes at its feet and a tooltip by the cursor says "<Ad> — <Unvan>" / "Konuşmak için tıkla"
(state `npchover`). While its shop is open the ring stays.

**Talking**: a left or right click on a merchant (when it is nearer than any other entity under the
cursor) or E (no loot bag in reach: E keeps its loot meaning first) walks into talk range (stops 3.2 m
from it, aiming at the point 1.6 m in front of it; path-found when the straight line is blocked) and
sends `SHOP_OPEN_REQ`. Moving by hand or a click on the ground cancels the walk. Shop requests (open,
close, buy, sell) stay at 8 a second (the server allows 10), `ITEM_USE_REQ` at 8 a second.

**Shop window** (`shop.window`, left, 300 logical wide, placed by `layoutHud` like the Künye; the
inventory opens on the right next to it - both 8 logical px from the edges while the shop is open, so
they fit side by side on 960 logical px - and closes with it if the shop opened it): title bar the
merchant's name, "‹Unvan›", the greeting quoted, italic (fix1: Segoe UI Italic, else the regular face slanted) in a warm dim colour,
a gold rule, the stock rows (icon, name in its rarity colour, a second line: "Can +70" / "Mana +40" for
potions, "Saldırı +9 · Sv. 3" for gear, "Gereken seviye: 3" or "Yalnız Büyücü" in red when unmet; the
price right-aligned with the Akçe coin, red when the balance is below it), the sell zone (`tab_sat` glyph
and "Satmak için çantada sağ tıkla ya da eşyayı buraya sürükle."; lit while a bag item is dragged), a
message line and "Akçe: <balance>" (live from `COINS`). 8 rows: 476 logical tall (Free at 1280x720;
OverTop at 960x600: the player frame yields while it is open). Row tooltip: the item's tooltip with
"Fiyat: n Akçe" instead of the value. While a buy / sell is in flight the rows wait ("Bekleniyor…").

**Buying**: a click / right click on a row. Stacks open the quantity box (`QuantityDialog`, modal:
"Kaç tane?", − / value / + with arrows, PgUp / PgDn, Home / End, the wheel and typed digits, "Toplam:
<n> Akçe", Al / Vazgeç, Enter / Esc); gear above 50 Akçe asks "<Ad> için <n> Akçe ödensin mi?"
(Evet / Hayır). The client does not refuse what it cannot pay: the server answers and the line says why.
**Selling** (only while a shop is open): a right click on a bag item or a drag onto the shop window
(anywhere on it). A stack of more than one opens the box ("Kaç tane satılsın?", default the whole stack,
"Alınacak: <n> Akçe"); a rarer item or one worth 40 Akçe or more asks "<Ad> <n> Akçe'ye satılsın mı?"
(after the box for a stack). An equipped item dropped on the shop: "Önce çıkarmalısın." (no request).
Items with a sale price of 0 say "Bu satılamaz." without a request. While a shop is open every bag
tooltip adds "Satış: <n> Akçe" (per unit, and the stack) or "Satılamaz"; equipped items "Önce
çıkarmalısın.". The `ITEM_UPDATE` / `COINS` of a purchase or a sale add the pickup line of loot ("+3 Kızılca Şerbet",
"+10 Akçe"; fix1) beside the result line; their `ITEM_UPDATE`s settle no item move (`ClientInventory::beginTake` with their own seq
ranges).

**Closing**: Esc (order: loot window, **shop**, Tuşlar, Yetenekler, Künye, inventory, target, menu), the X,
walking away (the server's `SHOP_CLOSED(OutOfRange)` at 8 m: "<Ad> çok uzakta kaldı."; the client closes
it itself beyond 9.5 m if no such packet came, e.g. after a leap), death (`YouDied`, no line), the merchant
leaving the view, a zone change (silently; requests in flight forgotten), leaving the world (silently).
Every result of §8.3 goes to the window's message line and to the message line above the bar.

**Potions**: in the bag a double click or a right click (no shop open) drinks it (`ITEM_USE_REQ` with
the slot). A drag from the bag onto a bar slot puts it there (`SKILL_BAR_SET_REQ` with `0x8000 | id`;
anything else: "Çubuğa yalnız iksir konabilir."); bar slots drag / swap / clear as before. A potion slot
shows the icon, the count of the template in the whole bag in the lower right corner (white; a red 0 and
the icon greyed when none is left: the slot stays) and the shared cooldown (`ITEM_COOLDOWN` → sweep and
seconds on every potion slot of the bar and on every potion in the bag; the 500 ms skill cooldown does
not apply). Key / click: `ITEM_USE_REQ(template, 0xFF)`; with none left "Elinde <Ad> kalmadı." and no
request. Tooltip (bag and bar): name, "Tüketilebilir", "Can: +n" / "Mana: +n", "Bekleme: 10 sn (bütün
iksirler)", the description, "<n> / 20"; on the bar also "Çubuktan kullan: <tuş>"; while a shop is open
"Satış: n Akçe". `ITEM_USED`: a green "+hp" and / or a blue "+mp" (`FloatKind::Mana`) over the drinker -
us or another player (state `otherpotions<N>`); our bars follow `SELF_STATS`. fix1: the drinker plays its
`drink` clip (`CharacterClip::Drink`, `characters/<race>_<n>/drink.vanim`) once when its look has one and it
stands still and free (not seated, attacking, casting, dead); moving blends back to the walk at once; no
clip: no gesture (states `drinks<N>` / `drinknogesture<N>`).

**Automation** (`world::parseShopCommand`): `talk <npc|nearest>`, `shopopenforce <npc>`, `walktonpc <npc>
[m]`, `shopbuy <template> [n]`, `shopbuyforce <template> <n>`, `shopsell <bagSlot> [n]`, `shopclose`,
`expectshop <npc|none>`, `expectshopmessage <text>`, `expectbarcount <slot> <n>`, `expectpotioncd <min>
<max>`, `usepotion <template> [bagSlot]`, `baritem <slot> <template|none>`, `hovernpc <npc|none>`,
`shoplog`; `camera at:<name>[@deg]` also aims at merchants (underscores for spaces, `@deg` turns further).
States: `npcs<N>`, `shopopen` / `shopclosed`, `shopopening`, `shopbusy` / `shopidle`, `npcwalking`,
`npchover`, `npctalking`, `npcinteract`, `npcanimated`, `quantitybox`, `potioncd` / `potionready` and the
counters `shopopened`, `shoprefused`, `bought`, `buyrefused`, `sold`, `sellrefused`, `shopclosedby`,
`potionuses`, `potionrefused`, `otherpotions`, `potioncds`, `npcinteracts`. Widgets: `shop.*` (ShopWindow.h),
`quantity.*`. Flows: `shop`, `shop_homes`, `two_clients_shop` (docs/BUILDING.md).
## 17. Cleanup round fix1 (client UI)

Code: `world/HudView.{h,cpp}` (`WindowArranger`, `placeWindowAt`, `hudResolutionScale`), `scenes/WorldWindows.cpp`
(the arrangement in `WorldScene`, title-bar drags, the merchant framing, automation), `scenes/ShopWindow.*`
(scrolling stock), `render/Font.*`, `render/TextRenderer.*`, `render/SpriteBatch.*`, `ui/Label.*` (italic).

* **HUD scale**: the world scene's UI scale is `max(player scale, nearest quarter step of height / 720)`
  (`hudResolutionScale`; 1080 -> 1.5, 1440 -> 2, a windowed 1061 -> 1.5, 1421 -> 2), then `fitUiScale`. The
  whole world UI (bar, frames, windows, labels) is laid out on ~1280x720 logical pixels on 720p, 1080p and
  1440p alike; fonts are baked at the scaled size (crisp). Other scenes keep the player's scale.
* **Window placement manager** (`WindowArranger`): shop, inventory, Künye, Yetenekler, Tuşlar. Preferred
  sides: shop / Künye / Yetenekler left, inventory right, Tuşlar centred. A window that is open keeps its
  place (edge anchors follow resizes); a newly opened one takes its preferred place, else the first free
  one (beside an open window, the centre, the other side); windows sit in the band above the bar (over the
  top HUD when taller, which then yields) and never on the bar. No free place: the least recently used
  window in the way closes (opening, clicking in or dragging a window makes it the most recently used);
  that can be the window itself after a resize. Tuşlar shrinks to 85 % at most to fit beside a left window.
  A title bar (34 logical px, not the X) drags a window; it stays there until it is in the way of another
  window (released over one, or a new one needs its room): then it is placed like any other again.
  Automation: `expectnooverlap`, `expectwindow <shop|inv|kunye|skills|controls> open|closed|left|right|middle|dragged|notdragged`,
  `dragwindow <name> <dx> <dy>`, `closewindows`, `expectuiscale <min> <max>`; state `winclosed<N>`.
* **Shop pair and the merchant**: shop left, inventory right (margins 8); when the screen is too narrow for
  a 160 logical px gap between them (960x600) both sit just above the bar, touching. A lens shift of the
  projection (`WorldScene::cameraProjection`, used for drawing, labels and picking alike) moves the
  merchant's chest into the gap, or (narrow) its feet just above the windows, smoothly (10/s); it returns
  to 0 when the shop closes. State `merchantvisible` (chest and head on screen, outside every window).
* **Shop stock scrolling**: the window's maximum height is the band under the player frame; a longer stock
  shows whole rows (`shopListLayout`, at least 3) with a scrollbar (drag the thumb, click the track to page)
  and the wheel (a row per notch). Test hook: `shopfakestock <n>` pads the open shop's list on the client
  (buying one is refused by the server); `shopscroll <first>`; states `shopscrolls` / `shopnoscroll`,
  `shopfirst<N>`, `greetingitalic`.
* **Italic**: `FontWeight::Italic` (segoeuii.ttf, ariali.ttf); without one `TextStyle::slant` shears the
  glyph quads (0.2). `Label::setItalic`.
* **Name labels**: seated characters (own and others) drop the label by how far the head bone is below its
  rest height (`seatedHeadDrop`). Automation `expectlabellift <min> <max>`, `expectremotelabellift <name> <min> <max>`.
* **Pickup lines**: "+<n> <Ad>" for items (loot and purchases), "+<n> Akçe" for coins (loot and sales);
  `expectpickup <text>`. The log moves right of a side window that reaches down to it.
* **Loading screen**: `ui/loading/zone<N>` (.png / .jpg under assets/ui or assets_src/ui, else the asset
  cache's compiled .vtex) of the zone being entered (or behind the gate), else the common background;
  state `loadingart<N>` / `loadingartdefault`.
* **Character select**: "Karakterler yükleniyor…" goes when the list arrives (states `listloading` /
  `nolistloading`).
* **Follow-up**: Yetenekler cell names are one line, shrunk to fit (`SkillCellWidget::skillNameFontSize`,
  `expectskillnamesfit`); the loot window is `HudWindow::Loot`; tooltip lines that do not wrap widen the box
  (`tipoverflows<N>`); the Tuşlar window measures its columns with the fonts it draws at when shrunk
  (`expectcontrolsfit`); on a narrow screen the shop is no taller than the inventory and the framing puts the
  merchant's marker and head first; the seated name label follows the head bone in 3D.
