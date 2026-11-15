# tools/blender

Headless Blender (5.3, bundled Python + numpy) generators for every source asset in `assets_src`.
Run them all with `powershell -File tools/blender/build_all.ps1`. Details, conventions and how to
add content: [docs/ASSET_PIPELINE.md](../../docs/ASSET_PIPELINE.md).

| Script / folder | Purpose |
|---|---|
| `vo_common.py` | shared helpers: deterministic PNG writer, materials, mesh builder, glTF export, preview rendering |
| `vo_ual.py` | UAL skeleton + clips: leaf bones dropped, A-pose rest, look scale, joint fit, walk re-timed for 1.4 m/s, foot pinning |
| `gen_textures.py` | `assets_src/textures/terrain/*.png` (tileable terrain layers) |
| `gen_props.py` | `assets_src/props/*.glb` |
| `gen_character.py --look <race>_<n>` (or `--race <race>`) | import-rig-export of `source_meshes/<race>_<n>_tripo.glb` → `assets_src/characters/<race>_<n>.glb` + `.png` + `.anims.toml` (three looks per race; look 0 also as the legacy `<race>.glb` / `.anims.toml`) |
| `gen_terrain.py` | `assets_src/terrain/zone1/*` (heightmap, splat, walk mask, description) |
| `measure_ual_speeds.py` | prints the root-motion ground speed of the UAL loops (needs the uncommitted `_RM` file) |
| `source_meshes/` | generated character meshes (Tripo P2 via Higgsfield, project-owned) and their concept images |
| `third_party/quaternius_ual/` | Quaternius Universal Animation Library (CC0 1.0), `License.txt`, `README.txt` |

Always run with `--background --factory-startup`; never attach to an interactive Blender session.
