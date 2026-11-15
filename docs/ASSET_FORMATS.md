# VedatOnline runtime asset formats (v1)

Binary formats written by `tools/assetc` and read by `shared/assets` (client and server). This
document is the contract: byte layouts are exact. How the sources are produced is in
[ASSET_PIPELINE.md](ASSET_PIPELINE.md).

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
  PNG textures are sRGB (create the SRV as `*_UNORM_SRGB`), except data maps embedded in
  `.vterrain` which are linear.
- **Triangle winding:** front faces are **clockwise** when seen from the side the face normal
  points to, in engine space (D3D11 default: `FrontCounterClockwise = FALSE`, `CullMode = BACK`).
  Numerically, for a front face `(p0, p1, p2)` the vector `cross(p1 - p0, p2 - p0)` (the usual
  component formula) points along the outward face normal.

### 1.1 Common header (16 bytes, offset 0)

| Offset | Type | Field | Notes |
|---|---|---|---|
| 0 | `char[4]` | magic | `VMSH`, `VANM` or `VTER` (bytes in file order) |
| 4 | u16 | version | `1` for everything in this document |
| 6 | u16 | flags | reserved, must be 0 |
| 8 | u32 | fileSize | total file size in bytes; must equal the actual size |
| 12 | u32 | crc32 | CRC-32 (IEEE 802.3, reflected, poly 0xEDB88320, init/xorout 0xFFFFFFFF — the zlib `crc32`) of bytes `[16, fileSize)` |

Readers reject a file if the magic or version differs, `fileSize` does not match, the CRC does not
match, or any count/offset below points outside the file. Every count has a hard upper bound
(listed per format) that is checked **before** any allocation, and every `offset + count × size`
is computed in 64 bits and checked against `fileSize`.

## 2. `.vmesh` — static or skinned mesh

```
Header        16 bytes (magic "VMSH")
MeshInfo      at offset 16, 104 bytes (ends at 120)
vertex data   vertexCount × vertexStride bytes (interleaved)
index data    indexCount × indexSize bytes
Submesh[]     submeshCount × 44 bytes
Bone[]        boneCount × 120 bytes
string table  stringSize bytes
```

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

`vertexFormat` bits: `0x01` NORMAL (must be set), `0x02` TANGENT, `0x04` COLOR, `0x08` SKIN. Bit
`0x08` is set if and only if `boneCount > 0`. Other bits must be 0.

### Vertex layout (interleaved, in this order; absent streams take no space)

| Stream | Type | Bytes | Present |
|---|---|---|---|
| position | f32 × 3 | 12 | always |
| normal | f32 × 3 | 12 | always (unit length) |
| tangent | f32 × 4 | 16 | TANGENT: xyz unit, w = ±1; bitangent = `cross(normal, tangent.xyz) * w` |
| uv0 | f32 × 2 | 8 | always (0,0 if the source has none); glTF convention, (0,0) = top-left of the image |
| color | u8 × 4 | 4 | COLOR: sRGB-encoded RGBA (`R8G8B8A8_UNORM`) |
| joints | u8 × 4 | 4 | SKIN: bone indices (`R8G8B8A8_UINT`), each `< boneCount` |
| weights | u8 × 4 | 4 | SKIN: `R8G8B8A8_UNORM`, the four bytes **sum to exactly 255**; unused slots have weight 0 and joint 0 |

Stride examples: static + colour = 36, skinned + colour = 44, skinned + tangent + colour = 60.

### Index data

`indexSize` 2 when `vertexCount <= 65535`, else 4. Indices are absolute into the vertex buffer
(no base vertex) and each `< vertexCount`. Triangle list, winding as in §1.

### Submesh (44 bytes)

| Offset | Type | Field | Notes |
|---|---|---|---|
| 0 | u32 | indexStart | first index; multiple of 3 |
| 4 | u32 | indexCount | multiple of 3, ≥ 3; `indexStart + indexCount <= indexCount(mesh)` |
| 8 | u32, u32 | name | string ref (e.g. `body`, `hair_2`); may be empty |
| 16 | u32, u32 | texture | string ref, asset path of the base-colour PNG; length 0 = untextured |
| 24 | f32 × 4 | baseColor | linear RGBA factor |
| 40 | u8 | variantGroup | 0 = always drawn, 1 = hair, 2 = face (others reserved, rejected) |
| 41 | u8 | variantIndex | 0..15 within the group (0 when group = 0) |
| 42 | u16 | flags | bit 0 double-sided, bit 1 alpha-blend; others 0 |

Final albedo = `srgbToLinear(vertexColor) × baseColor × texture` (missing terms are 1). assetc bakes
untextured glTF material colours into the vertex colours and sets `baseColor = (1,1,1,1)` for such
submeshes, so most meshes are a handful of submeshes.

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
(`assets/characters/aydor/walk.vanim`), but readers accept up to 256.

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
Header        16 bytes (magic "VTER")
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

### Layer (12 bytes)

| Offset | Type | Field |
|---|---|---|
| 0 | u32, u32 | texture (asset path of a tileable PNG) |
| 8 | f32 | tileSize — metres covered by one repeat of the texture, `> 0` |

Zone 1 layers: 0 grass, 1 dirt road, 2 rock, 3 dry grass.

### Spawn (24 bytes)

| Offset | Type | Field |
|---|---|---|
| 0 | u32, u32 | name (unique; zone 1 has `spawn_aydor` and `spawn_kargan`) |
| 8 | f32 × 3 | position x, y, z (`y` = `heightAt(x, z)` at build time) |
| 20 | f32 | yaw |

### Object (32 bytes)

| Offset | Type | Field | Notes |
|---|---|---|---|
| 0 | u32, u32 | mesh | asset path of a `.vmesh` |
| 8 | f32 × 3 | position | x, y, z of the mesh origin (y from the heightmap plus the source `y_offset`) |
| 20 | f32 | yaw | |
| 24 | f32 | scale | uniform, `> 0` |
| 28 | u32 | flags | bit 0 BLOCKS (footprint is blocked in the walk mask); others 0 |

World transform of a placed mesh (column vectors): `T(position) · RotY(yaw) · S(scale)`, where
`RotY(yaw)` maps `+Z` to `forward(yaw)`.

## 5. Textures: PNG as-is (no `.vtex` this milestone)

Textures ship as the source PNG files (8-bit RGBA or RGB, sRGB), copied unchanged by
`assetc build`. Reasons: the client already decodes PNG (stb_image) and generates mips on the GPU
(`GenerateMips`) in a few milliseconds per texture; there are only a handful of ≤ 1024² textures
this milestone; and a custom RGBA8+mips container would make files ~4× larger than PNG with no
quality gain. A `.vtex` container (with BC-compressed mips) is planned once texture counts or load
times justify it; its magic `VTEX` is reserved.
