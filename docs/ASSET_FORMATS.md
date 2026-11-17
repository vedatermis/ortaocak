# VedatOnline runtime asset formats (.vmesh v2/v3, .vterrain v1/v2, .vanim / .vtex v1)

Binary formats written by `tools/assetc` and read by `shared/assets` (client and server). This
document is the contract: byte layouts are exact. How the sources are produced is in
[ASSET_PIPELINE.md](ASSET_PIPELINE.md).

| Format | Version written | Versions read |
|---|---|---|
| `.vmesh` | 2 (PBR materials, snorm16 tangents); 3 only when a submesh is alpha-tested or emissive | 1, 2, 3 |
| `.vterrain` | 1; 2 only when layers carry normal/ORM maps or objects a stretch/tint | 1, 2 |
| `.vanim` | 1 | 1 |
| `.vtex` | 1 | 1 |

Writers pick the lowest version that can hold the data, so files that do not use a newer feature
stay byte-identical to what older writers produced.

## 1. Conventions shared by every format

- **Engine space:** left-handed, **+X right, +Y up, +Z forward**, metres (docs/PROTOCOL.md §5.4).
  Yaw is radians about +Y, 0 = facing +Z, increasing clockwise seen from above:
  `forward(yaw) = (sin yaw, 0, cos yaw)`.
- **Byte order:** little-endian. `f32` is IEEE-754 binary32. No padding inside records unless a
  `pad` field is listed. Records are tightly packed; readers copy fields with `memcpy`, so alignment
  of section offsets is not required (assetc still aligns every section to 4 bytes).
- **No pointers.** Sections are located by absolute byte offsets from the start of the file.
- **Strings** live in one string table per file. A string reference is `u32 offset, u32 length`
  relative to the start of the string table; the bytes are UTF-8 without terminator. Readers
  reject references with `offset + length > stringSize`. Asset paths are relative to the
  **assets root** (the `assets/` directory), use forward slashes, never start with `/`, and never
  contain `..` or `\` (readers reject them).
- **Matrices** are 16 × f32 in the glTF layout: column-major storage of a matrix used with column
  vectors (`p' = M · p`), translation in elements 12, 13, 14. The same 16 floats loaded with
  `XMLoadFloat4x4` give the DirectXMath (row-vector, `p' = p · M`) form of the same transform, and
  HLSL with the default `column_major` packing uses them as `mul(M, float4(p, 1))`.
- **Quaternions** are `x, y, z, w` (w last), unit length.
- **Colours:** material colour factors are **linear** RGBA f32. 8-bit vertex colours are
  **sRGB-encoded** (decode in the vertex shader: `linear = srgbToLinear(c.rgb)`, alpha linear).
  Colour PNG textures (base colour, terrain layers, UI) are sRGB (create the SRV as
  `*_UNORM_SRGB`); normal maps, ORM maps and the data maps embedded in `.vterrain` are linear.
- **Triangle winding:** front faces are **clockwise** when seen from the side the face normal
  points to, in engine space (D3D11 default: `FrontCounterClockwise = FALSE`, `CullMode = BACK`).
  Numerically, for a front face `(p0, p1, p2)` the vector `cross(p1 - p0, p2 - p0)` (the usual
  component formula) points along the outward face normal.

### 1.1 Common header (16 bytes, offset 0)

| Offset | Type | Field | Notes |
|---|---|---|---|
| 0 | `char[4]` | magic | `VMSH`, `VANM`, `VTER` or `VTEX` (bytes in file order) |
| 4 | u16 | version | `.vmesh`: 1 or 2; everything else: 1 |
| 6 | u16 | flags | reserved, must be 0 |
| 8 | u32 | fileSize | total file size in bytes; must equal the actual size |
| 12 | u32 | crc32 | CRC-32 (IEEE 802.3, reflected, poly 0xEDB88320, init/xorout 0xFFFFFFFF — the zlib `crc32`) of bytes `[16, fileSize)` |

Readers reject a file if the magic or version differs, `fileSize` does not match, the CRC does not
match, or any count/offset below points outside the file. Every count has a hard upper bound
(listed per format) that is checked **before** any allocation, and every `offset + count × size`
is computed in 64 bits and checked against `fileSize`.

## 2. `.vmesh` — static or skinned mesh

```
Header        16 bytes (magic "VMSH", version 2; version 1 still read)
MeshInfo      at offset 16, 104 bytes (ends at 120)
vertex data   vertexCount × vertexStride bytes (interleaved)
index data    indexCount × indexSize bytes
Submesh[]     submeshCount × 80 bytes (version 1: × 44 bytes, version 3: × 104 bytes)
Bone[]        boneCount × 120 bytes
string table  stringSize bytes
```

**Version 2** (written since the PBR milestone) differs from version 1 only in (a) the vertex
format bit `0x20` (snorm16 tangent stream) and (b) the 80-byte submesh record with the PBR material.
Everything else (MeshInfo, bones, strings, validation) is identical. A reader that gets a version 1
file reads 44-byte submeshes and gives every submesh the default material (no normal/ORM map,
`normalScale` 1, `roughnessFactor` 1, `metallicFactor` 0, `occlusionStrength` 1: a rough
dielectric); bit `0x20` is rejected in version 1 files.

### MeshInfo (104 bytes)

| Offset | Type | Field | Notes |
|---|---|---|---|
| 16 | u32 | vertexCount | 1 .. 16 777 216 |
| 20 | u32 | indexCount | multiple of 3, 3 .. 50 331 648 |
| 24 | u32 | submeshCount | 1 .. 1024 |
| 28 | u32 | boneCount | 0 (static) .. 255 |
| 32 | u32 | vertexFormat | bit flags, see below |
| 36 | u32 | vertexStride | must equal the stride derived from `vertexFormat` |
| 40 | u32 | indexSize | 2 (u16) or 4 (u32) |
| 44 | f32[3] | boundsMin | axis-aligned box of all positions (bind pose for skinned meshes) |
| 56 | f32[3] | boundsMax | |
| 68 | f32[3] | sphereCenter | bounding sphere containing every vertex |
| 80 | f32 | sphereRadius | |
| 84 | u32 | vertexOffset | |
| 88 | u32 | indexOffset | |
| 92 | u32 | submeshOffset | |
| 96 | u32 | boneOffset | 0 when `boneCount == 0` |
| 100 | u32 | stringOffset | |
| 104 | u32 | stringSize | 0 .. 16 MiB |
| 108 | u32[3] | reserved | 0 |

`vertexFormat` bits: `0x01` NORMAL (must be set), `0x02` TANGENT (f32 × 4), `0x04` COLOR, `0x08`
SKIN, `0x20` TANGENT16 (snorm16 × 4; version 2 only). Bit `0x08` is set if and only if
`boneCount > 0`. At most one of `0x02` / `0x20`. Other bits must be 0. assetc writes `0x20`; `0x02`
stays readable (version 1 layout).

### Vertex layout (interleaved, in this order; absent streams take no space)

| Stream | Type | Bytes | Present |
|---|---|---|---|
| position | f32 × 3 | 12 | always |
| normal | f32 × 3 | 12 | always (unit length) |
| tangent | f32 × 4 | 16 | TANGENT: xyz unit, w = ±1 |
| tangent | i16 × 4 | 8 | TANGENT16: snorm16 (`v / 32767`): xyz unit (`|len² − 1| <= 0.02` after decoding, no component −32768), w exactly +32767 or −32767 |
| uv0 | f32 × 2 | 8 | always (0,0 if the source has none); glTF convention, (0,0) = top-left of the image |
| color | u8 × 4 | 4 | COLOR: sRGB-encoded RGBA (`R8G8B8A8_UNORM`) |
| joints | u8 × 4 | 4 | SKIN: bone indices (`R8G8B8A8_UINT`), each `< boneCount` |
| weights | u8 × 4 | 4 | SKIN: `R8G8B8A8_UNORM`, the four bytes **sum to exactly 255**; unused slots have weight 0 and joint 0 |

Stride examples: static + colour = 36, skinned + colour = 44, skinned + TANGENT + colour = 60,
skinned + TANGENT16 + colour = 52, static + TANGENT16 (generated characters before rigging) = 40.

**Tangent frame and normal maps** (engine space, after the X mirror of §ASSET_PIPELINE "Coordinate
conversion"): bitangent `B = cross(N, T.xyz) * T.w` (component formula) points **image-up**, i.e.
towards decreasing glTF V; `T` points towards increasing U. A tangent-space normal-map texel
`(x, y, z)` (OpenGL convention, +Y = image up; stored as unorm `c * 0.5 + 0.5`) gives the shading
normal `normalize(T' * x + B * y + N * z)` with `T'` = `T.xyz` made perpendicular to `N`
(`shared/assets/NormalMapping.h`, identical in `client/shaders/Mesh.hlsl`). This is exactly the glTF
2.0 rule (`bitangent = cross(normal, tangent.xyz) * w`) carried through the mirror: glTF tangents
convert as xyz mirrored, w negated (§ASSET_PIPELINE). Tangents that assetc generates (MikkTSpace on
`(u, 1 − v)`, the space Blender bakes in) follow the same convention.
`tests/assets/GltfPbrTests.cpp` proves it with a quad and known texels for normal, U-mirrored and
V-flipped UVs (both `w` signs), with glTF `TANGENT` and with generated tangents.

### Index data

`indexSize` 2 when `vertexCount <= 65535`, else 4. Indices are absolute into the vertex buffer
(no base vertex) and each `< vertexCount`. Triangle list, winding as in §1.

### Submesh (80 bytes; version 1: the first 44 bytes; version 3: 104 bytes)

| Offset | Type | Field | Notes |
|---|---|---|---|
| 0 | u32 | indexStart | first index; multiple of 3 |
| 4 | u32 | indexCount | multiple of 3, ≥ 3; `indexStart + indexCount <= indexCount(mesh)` |
| 8 | u32, u32 | name | string ref (e.g. `body`, `hair_2`); may be empty |
| 16 | u32, u32 | texture | string ref, asset path of the base-colour PNG (sRGB); length 0 = untextured |
| 24 | f32 × 4 | baseColor | linear RGBA factor |
| 40 | u8 | variantGroup | 0 = always drawn, 1 = hair, 2 = face (others reserved, rejected) |
| 41 | u8 | variantIndex | 0..15 within the group (0 when group = 0) |
| 42 | u16 | flags | bit 0 double-sided, bit 1 alpha-blend, bit 2 alpha test (v3 only); others 0 |
| 44 | u32, u32 | normalTexture | v2: asset path of the tangent-space normal map PNG (linear, OpenGL +Y up); length 0 = none. Non-empty requires a tangent stream |
| 52 | u32, u32 | ormTexture | v2: asset path of the ORM PNG (linear; **R occlusion, G roughness, B metallic**); length 0 = factors only |
| 60 | f32 | normalScale | v2: XY of the decoded normal × this (glTF `normalTexture.scale`), 0 .. 16 |
| 64 | f32 | roughnessFactor | v2: perceptual roughness = `ORM.g × factor`, 0 .. 1 |
| 68 | f32 | metallicFactor | v2: metallic = `ORM.b × factor`, 0 .. 1 |
| 72 | f32 | occlusionStrength | v2: occlusion = `lerp(1, ORM.r, strength)` (glTF `occlusionTexture.strength`), 0 .. 1 |
| 76 | u32 / f32 | reserved / alphaCutoff | v2: 0. v3: f32 alpha cutoff, `(0, 1)` exactly when flag bit 2 (alpha test) is set, else 0 |
| 80 | u32, u32 | emissiveTexture | v3: asset path of the emitted-colour PNG (sRGB); length 0 = none (the factor alone emits) |
| 88 | f32 × 3 | emissiveFactor | v3: linear emitted radiance multiplier, each 0 .. 1000 (glTF `emissiveFactor` × `KHR_materials_emissive_strength`) |
| 100 | u32 | reserved | v3: 0 |

**Version 3** (104-byte submesh records) adds flag bit 2 **alpha test** (glTF `alphaMode MASK`:
a texel is kept where base-colour alpha ≥ `alphaCutoff`; not combined with bit 1 alpha-blend) and
emission: emitted radiance = `emissiveFactor × emissiveTexture` (texture term 1 if absent), added
after lighting in the same HDR units as the light colours (CLIENT_RENDERING.md §4.1), so a factor
of 3-5 glows at dusk exposure and stays subtle next to sunlit surfaces. A version 1 or 2 file reads
`alphaCutoff` 0 and no emission; flag bit 2 is rejected in them.

Final albedo = `srgbToLinear(vertexColor) × baseColor × texture` (missing terms are 1). assetc bakes
untextured glTF material colours into the vertex colours and sets `baseColor = (1,1,1,1)` for such
submeshes, so most meshes are a handful of submeshes (one per distinct flags / roughness / metallic
combination). Roughness and metallic without an ORM map are the factors alone (glTF defaults 1 / 1
when a material omits them; Blender's exports always write them).

Texture paths name the **PNG** asset. Next to each material PNG assetc writes the compiled
`<same path minus extension>.vtex` (§5); the client loads that and falls back to decoding the PNG.

**Appearance variants.** A character mesh contains every hair and face variant. To draw a
character with appearance `(face f, hair h)` (docs/PROTOCOL.md §5.3, both 0..3) draw every submesh
with `variantGroup == 0`, the submeshes with `variantGroup == 1 && variantIndex == h` and those
with `variantGroup == 2 && variantIndex == f`. `vo::assets::MeshData::isSubmeshVisible(i, face,
hair)` implements exactly that rule.

### Bone (120 bytes) — skinned meshes only

Bones are ordered so that `parent < index` (index 0 is a root). Names are unique and non-empty.

| Offset | Type | Field | Notes |
|---|---|---|---|
| 0 | u32, u32 | name | string ref |
| 8 | i32 | parent | −1 for a root, else `0 <= parent < index` |
| 12 | f32 × 16 | inverseBind | model space → bone space at bind pose (§1 matrix layout) |
| 76 | f32 × 3 | bindTranslation | bind-pose local transform relative to the parent bone (model space for roots) |
| 88 | f32 × 4 | bindRotation | quaternion x, y, z, w |
| 104 | f32 × 3 | bindScale | |
| 116 | u32 | reserved | 0 |

Skinning (column vectors): `model[b] = model[parent] · local[b]` (`model[root] = local[root]`),
`skin[b] = model[b] · inverseBind[b]`, `p' = Σ_k w_k · skin[j_k] · p`. With bind-pose locals
`skin[b]` is the identity (within float error).

## 3. `.vanim` — animation clips

One file holds one or more clips for one skeleton layout. Tracks are matched to skeleton bones
**by name** (a track whose bone is missing from the skeleton is ignored; a bone without a track
keeps its bind-pose local transform). assetc writes one clip per file
(`assets/characters/aydor_0/walk.vanim`; one directory per character look, see ASSET_PIPELINE.md
§3), but readers accept up to 256.

```
Header        16 bytes (magic "VANM")
AnimInfo      at offset 16, 16 bytes
Clip[]        clipCount × 36 bytes
Track[]       Σ trackCount × 12 bytes (each clip's tracks are contiguous at its trackOffset)
key data      per track: frameCount keys
string table
```

### AnimInfo (16 bytes)

| Offset | Type | Field | Notes |
|---|---|---|---|
| 16 | u32 | clipCount | 1 .. 256 |
| 20 | u32 | clipOffset | |
| 24 | u32 | stringOffset | |
| 28 | u32 | stringSize | 0 .. 1 MiB |

### Clip (36 bytes)

| Offset | Type | Field | Notes |
|---|---|---|---|
| 0 | u32, u32 | name | string ref, non-empty (`idle`, `walk`, `run`) |
| 8 | f32 | duration | seconds, `> 0` unless `frameCount == 1`; must equal `(frameCount − 1) / sampleRate` within 1e-4 |
| 12 | f32 | sampleRate | frames per second, 1 .. 240 (assetc: 30) |
| 16 | u32 | frameCount | 1 .. 65536; key `k` is at time `k / sampleRate` (both ends included) |
| 20 | u32 | flags | bit 0 LOOP, bit 1 QUANTIZED_ROTATION; others 0 |
| 24 | f32 | moveSpeed | m/s of ground travel the clip is authored for (0 for in-place clips such as idle) |
| 28 | u32 | trackCount | 0 .. 255 |
| 32 | u32 | trackOffset | |

For a LOOP clip the last key equals the first key (assetc samples the closed interval), so a looping
clip played with `t mod duration` is seamless. Playback rate for a character moving at speed `v` is
`v / moveSpeed` when `moveSpeed > 0`.

### Track (12 bytes)

| Offset | Type | Field | Notes |
|---|---|---|---|
| 0 | u32, u32 | bone | string ref, bone name, unique within the clip |
| 8 | u32 | keyOffset | absolute offset of `frameCount` keys |

### Key (40 bytes, or 32 with QUANTIZED_ROTATION)

| Field | Type | Notes |
|---|---|---|
| translation | f32 × 3 | local, relative to the parent bone, engine space |
| rotation | f32 × 4, or i16 × 4 | quaternion x, y, z, w; quantised form is snorm16 (`v / 32767`, renormalise after decoding) |
| scale | f32 × 3 | |

Sampling: between keys, translation and scale are linearly interpolated, rotation uses normalised
lerp along the shortest arc (`dot < 0` → negate one side). `vo::assets::samplePose` implements it.

## 4. `.vterrain` — terrain zone

```
Header        16 bytes (magic "VTER", version 1 or 2)
TerrainInfo   at offset 16, 72 bytes
heights       resolution² × u16
splat         splatResolution² × 4 bytes (RGBA8)
walk mask     maskResolution² × u8
Layer[]       layerCount × 12 bytes
Spawn[]       spawnCount × 24 bytes
Object[]      objectCount × 32 bytes
string table
```

### TerrainInfo (72 bytes)

| Offset | Type | Field | Notes |
|---|---|---|---|
| 16 | u32 | resolution | N, heights are N × N samples; `N = 2^k + 1`, 3 .. 4097 |
| 20 | f32 | worldSize | metres; samples span `[0, worldSize]` on X and Z (zone 1: 512) |
| 24 | f32 | minHeight | metres |
| 28 | f32 | maxHeight | `> minHeight` |
| 32 | u32 | splatResolution | S, 1 .. 4096 |
| 36 | u32 | maskResolution | M, 1 .. 4096 |
| 40 | u32 | layerCount | exactly 4 |
| 44 | u32 | spawnCount | 0 .. 1024 |
| 48 | u32 | objectCount | 0 .. 1 000 000 |
| 52 | u32 | heightOffset | |
| 56 | u32 | splatOffset | |
| 60 | u32 | maskOffset | |
| 64 | u32 | layerOffset | |
| 68 | u32 | spawnOffset | |
| 72 | u32 | objectOffset | |
| 76 | u32 | stringOffset | |
| 80 | u32 | stringSize | 0 .. 16 MiB |
| 84 | u32 | reserved | 0 |

**Heights.** Sample `(i, j)` (`i` along X, `j` along Z, both `0..N−1`) is at
`x = i · worldSize / (N − 1)`, `z = j · worldSize / (N − 1)` and stored at index `j · N + i`.
Height in metres = `minHeight + (h / 65535) · (maxHeight − minHeight)`. `heightAt(x, z)` is the
bilinear interpolation of the four surrounding samples; positions outside `[0, worldSize]` are
clamped to the edge.

**Splat** (`S × S`, row-major by Z like the heights, texel `(i, j)` covers
`[i·c, (i+1)·c) × [j·c, (j+1)·c)` with `c = worldSize / S`): byte 0..3 = weight of layer 0..3
(linear, the shader normalises the sum).

**Walk mask** (`M × M`, same cell layout with `c = worldSize / M`): 1 = walkable, 0 = blocked
(other values rejected). Blocked covers steep slopes, the outer margin, ridges, and the footprints of
blocking objects. `isWalkable(x, z)` is false outside `[0, worldSize)`.

### Layer (12 bytes; version 2: 28 bytes)

| Offset v1 | Offset v2 | Type | Field |
|---|---|---|---|
| 0 | 0 | u32, u32 | texture (asset path of a tileable base-colour PNG, sRGB) |
| — | 8 | u32, u32 | normalTexture (tangent-space normal map, OpenGL +Y up, linear; length 0 = none) |
| — | 16 | u32, u32 | ormTexture (R occlusion, G roughness, B metallic, linear; length 0 = none) |
| 8 | 24 | f32 | tileSize — metres covered by one repeat of the texture, `> 0` |

Zone 1 layers: 0 grass, 1 dirt road, 2 rock, 3 dry grass, each with all three maps (`assetc build`
compiles them to `.vtex`, §5). Version 1 files read no normal/ORM maps.

### Spawn (24 bytes)

| Offset | Type | Field |
|---|---|---|
| 0 | u32, u32 | name (unique; zone 1 has `spawn_aydor` and `spawn_kargan`) |
| 8 | f32 × 3 | position x, y, z (`y` = `heightAt(x, z)` at build time) |
| 20 | f32 | yaw |

### Object (32 bytes; version 2: 48 bytes)

| Offset v1 | Offset v2 | Type | Field | Notes |
|---|---|---|---|---|
| 0 | 0 | u32, u32 | mesh | asset path of a `.vmesh` |
| 8 | 8 | f32 × 3 | position | x, y, z of the mesh origin (y from the heightmap plus the source `y_offset`) |
| 20 | 20 | f32 | yaw | |
| 24 | 24 | f32 | scale | uniform, `> 0` |
| — | 28 | f32 × 3 | stretch | per-axis factor on top of `scale` (mesh local x, y, z), each in `(0, 4]`; v1: (1, 1, 1) |
| — | 40 | u8 × 4 | tint | sRGB multiplier of the base colour (rgb; a unused); v1: white |
| 28 | 44 | u32 | flags | bit 0 BLOCKS (footprint is blocked in the walk mask); others 0 |

World transform of a placed mesh (column vectors): `T(position) · RotY(yaw) · S(scale · stretch)`,
where `RotY(yaw)` maps `+Z` to `forward(yaw)` (`ObjectPlacement::worldMatrix`). The server only reads
heights, the walk mask and spawns; it loads v1 and v2 alike.

## 5. Textures: PNG sources + `.vtex` (pre-mipped, block-compressed)

Every PNG under `assets_src` is still copied as is (UI images are loaded as PNG).
For every texture a mesh **material** references (base colour, normal map, ORM map, emissive map) and every map of a terrain layer (base colour, normal, ORM) `assetc build`
also writes `<path minus extension>.vtex` next to the PNG: downscaled to at most `--max-texture`
(default 2048), a full mip chain filtered for its content, block-compressed (stb_dxt) into the
D3D11-native BC payload. The client loads the `.vtex` when it exists (one read, one immutable
texture with all mips, no decode, no `GenerateMips`) and otherwise decodes the PNG
(`graphics.compressed_textures = false` forces that path for comparisons).

| Material slot | `.vtex` usage | Format | Mip filter |
|---|---|---|---|
| base colour, emissive | Color (sRGB view) | BC1 (BC3 if any alpha < 255) | sRGB → linear, 2×2 box, → sRGB |
| normal map | Normal (linear) | BC5 (R = X, G = Y; Z = `sqrt(1 − x² − y²)` in the shader) | decode, average vectors, renormalise |
| ORM | Data (linear) | BC1 (5:6:5 keeps roughness, in G, at 6 bits) | linear 2×2 box |

Measured (Release client, 2048² maps; docs/CLIENT_RENDERING.md §6): GPU memory per character look
64 MiB as RGBA8 + mips → 10.7 MiB as BC1 + BC5 + BC1 (6×); load time per 2048² map ~118 ms (PNG
decode ~110 ms + upload and `GenerateMips` ~8 ms) → ~16 ms (`.vtex` read + CRC + parse ~14 ms +
upload ~1.4 ms). BC1 was chosen over BC7 for colour and ORM because the BC1/BC5 encoder (stb_dxt) is
already a dependency; the format field leaves room for BC7 later.

```
Header        16 bytes (magic "VTEX", version 1)
TexInfo       at offset 16, 32 bytes (ends at 48)
MipEntry[]    mipCount × 8 bytes at mipTableOffset
mip data      per mip, at its offset (assetc aligns each to 16 bytes; readers do not require it)
```

### TexInfo (32 bytes)

| Offset | Type | Field | Notes |
|---|---|---|---|
| 16 | u32 | width | 1 .. 16384; a multiple of 4 for block formats |
| 20 | u32 | height | 1 .. 16384; a multiple of 4 for block formats |
| 24 | u32 | mipCount | 1 .. 15 and ≤ `bit_width(max(width, height))` (the full chain ends at 1×1) |
| 28 | u32 | format | 1 RGBA8, 2 BC1, 3 BC3, 4 BC4, 5 BC5 (D3D11 `BC*_UNORM` layouts) |
| 32 | u32 | usage | 0 Color (sRGB view: RGBA8, BC1, BC3 only), 1 Normal (linear: RGBA8 or BC5), 2 Data (linear, any format) |
| 36 | u32 | mipTableOffset | ≥ 48 |
| 40 | u32 × 2 | reserved | 0 |

### MipEntry (8 bytes)

| Offset | Type | Field | Notes |
|---|---|---|---|
| 0 | u32 | offset | absolute; after the mip table and after the previous mip's data (no overlap) |
| 4 | u32 | size | exactly the size of level `i` (`max(1, width >> i)` × `max(1, height >> i)`): RGBA8 `w·h·4`; block formats `max(1, ⌈w/4⌉)·max(1, ⌈h/4⌉)·(8 for BC1/BC4, 16 for BC3/BC5)` |

Rows are top first; blocks row-major. Readers reject everything else (bad sizes, counts, overlap,
format not allowed for the usage, reserved fields, CRC), like the other formats
(`tests/assets/PbrFormatTests.cpp`).

### 5.1 UI images with alpha: inventory icons (`ui/items/*.vtex`)

PNGs directly in `assets_src/ui/items/` (the item icons, `gamedata/items.toml` `icon` →
`ui/items/<icon>.vtex`), in `assets_src/ui/skills/` (the 64² skill icons, `ui/skills/<id>.vtex`,
7 mips) and, since M12, in `assets_src/ui/status/` (64² buff / debuff icons) and
`assets_src/ui/skillbar/` (skill bar / tree pieces; every side a power of two), after M13 in `assets_src/ui/hud/` (64² HUD button glyphs) and in M14 in `assets_src/ui/shop/` (shop glyphs, Akçe coin, merchant markers) are copied like every PNG **and** compiled to `<icon>.vtex` next to the copy,
with a different recipe from the material textures (the file layout above is unchanged):

| Field | Value | Why |
|---|---|---|
| format | 1 RGBA8, never block-compressed | BC1 has no alpha; BC3 (interpolated 4×4 alpha, 5:6:5 colour) visibly smears a 128² icon's silhouette and gradients. 128² RGBA8 with mips is 87 KiB |
| usage | 0 Color (sRGB view) | |
| alpha | **premultiplied**: RGB = linearToSrgb(srgbToLinear(rgb) × a) before the mips are built; a = 0 texels are black | the sprite shader blends premultiplied (ONE, INV_SRC_ALPHA) and the PNG path premultiplies on load (`Texture2D::Options::premultiply`); a `.vtex` is uploaded as is, so it must already be premultiplied. Mips filtered on premultiplied colour have no dark / bright fringes |
| mips | full chain (128² → 8 levels), sRGB-correct 2×2 box | |

So a client that loads an icon from the `.vtex` must **not** premultiply it again (load it like a
material `.vtex`: create the SRV as `R8G8B8A8_UNORM_SRGB` and draw it with the sprite batch's
textured mode); a client that falls back to the PNG premultiplies on load as for every UI PNG. The
source PNG holds straight (un-premultiplied) sRGB alpha. Implementation: `ItemKind::UiIcon` in
`tools/assetc/Build.cpp` with `TextureRequest::premultiplyAlpha` / `uncompressed`;
`content.weapons` tests check format, size, mip count, transparent corners and premultiplication.
