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
above the two help lines; the skill button (`world.skill`, clickable) with its icon
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
on Esc. **Esc order** (`escapeAction`): loot window, inventory, target, menu.

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
`gained > 0` adds a line ("Kül Tozu x2 alındı" in the item's rarity colour, "+7 Akçe" in gold) to a
short log at the bottom left above the experience bar, drawn above the windows, at most 5 lines,
3.6 s each.

**Automation.** Scene commands (`client/world/LootView.h` `parseLootCommand`): `lootnearest [any]`
(as E; `any` also opens locked bags, to test the refusal), `walktoloot [mine|locked|free|any]`,
`takeall`, `take <index>|coins`, `lootclose`, `expectcoins [op] <n>`, `expectitem <template> [op]
<units>`, `expectstack <slot> <template> <quantity>`, `expectlootrows <n>`, `expectlootmessage
<text>`, `lootfarm <monster> <template> <pickups>` / `lootfarm stop` (the hunt loop that also loots
every own / free bag until that many pickups of the template), `movestack <template> <slot>`,
`lootlog` (op: `=`, `>=`, `<=`, `>`, `<`). States: `lootvisible` / `noloot`, `lootmine`,
`lootlocked`, `lootfree`, `lootinrange`, `lootwin` / `lootwinclosed`, `lootcontents` / `lootempty`,
`loottaking` / `lootidle`, `lootwalking`, `lootopening`, `coinsknown`, `pickupshown`,
`lootfarming`, `farmdone`, `remotesitting` (the marked remote sits: a signal between two scripted
clients) and counters (1..12) `lootbags`, `lootopened`, `loottakes`, `lootrefused`, `pickups`,
`coingains`, `lootclosed`, `lootspawns`, `lootdespawns`. Widgets: `loot.window`, `loot.coins`,
`loot.entry.<index>`, `loot.takeall`, `loot.close`, `loot.message`, `loot.source`,
`loot.contents` (invisible: one row per line, for `expecttext`), `inv.coins`.
