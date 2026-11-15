# tools/blender

Headless Blender (5.3, bundled Python + numpy) generators for every source asset in `assets_src`.
Run them all with `powershell -File tools/blender/build_all.ps1`. Details, conventions and how to
add content: [docs/ASSET_PIPELINE.md](../../docs/ASSET_PIPELINE.md).

| Script | Output |
|---|---|
| `vo_common.py` | shared helpers: deterministic PNG writer, materials, mesh builder, glTF export, preview rendering |
| `gen_textures.py` | `assets_src/textures/terrain/*.png` (tileable terrain layers) |
| `gen_props.py` | `assets_src/props/*.glb` |
| `gen_character.py --race aydor\|kargan` | `assets_src/characters/<race>.glb` + `.anims.toml` |
| `gen_terrain.py` | `assets_src/terrain/zone1/*` (heightmap, splat, walk mask, description) |

Always run with `--background --factory-startup`; never attach to an interactive Blender session.
