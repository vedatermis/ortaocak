# assets_src

Source assets, compiled by `tools/assetc` into the runtime formats under `assets/` (generated,
git-ignored): `powershell -File tools/build-assets.ps1`. See docs/ASSET_PIPELINE.md and
docs/ASSET_FORMATS.md.

| Directory | Content | Origin |
|---|---|---|
| `characters/` | `aydor.glb`, `kargan.glb` (skinned, 22 bones, hair/face variants, idle/walk/run) + `.anims.toml` | `tools/blender/gen_character.py` |
| `props/` | 13 static props (`.glb`) | `tools/blender/gen_props.py` |
| `textures/terrain/` | 4 tileable 512² terrain layers | `tools/blender/gen_textures.py` (numpy noise) |
| `terrain/zone1/` | heightmap, splat, walk mask, `zone1.terrain.toml` | `tools/blender/gen_terrain.py` |
| `previews/` | preview renders (not copied to `assets/`) | the generators |
| `ui/`, `fonts/` | UI art (owned by the client work package) | see files there |

Everything here is generated procedurally from the scripts in this repository with fixed seeds —
no downloaded, scanned or AI-generated content. All content must stay original (see
docs/DEVELOPMENT_PLAN.md, section 6). Regenerate with `powershell -File tools/blender/build_all.ps1`.
