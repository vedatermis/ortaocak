"""Terrain layer materials (base colour + normal + ORM) from scanned Poly Haven textures (CC0).

Usage (headless):
  blender --background --factory-startup --python tools/blender/gen_textures.py [-- --no-preview]

Sources: tools/blender/source_textures/polyhaven/<asset>_{Diffuse,nor_gl,arm}_2k.jpg (Poly Haven, CC0;
THIRD_PARTY.md). `arm` is R ambient occlusion, G roughness, B metallic: the engine's ORM layout.
`nor_gl` is an OpenGL-convention (+Y up) tangent-space normal map: the engine's convention.

Outputs assets_src/textures/terrain/<layer>.png (sRGB), <layer>_n.png, <layer>_orm.png, LAYER_SIZE^2,
and assets_src/previews/textures/terrain_layers.png (each layer tiled 2x2, colour / normal / ORM).
Per layer the base colour is graded (saturation, brightness, a slight hue pull) so the four sit
together and next to the PBR characters: muted highland grass, grey-brown rock. Downsampling is a box
filter (colour in linear light, normals as renormalised vectors, ORM linear). The scans are tileable;
the grading is per texel, so they stay tileable. Deterministic.

1024^2 per map: with the 3-6 m tiles the shader uses, 1024 texels per tile are ~170-340 texels/m,
more than the ~150-200 px/m the default camera puts on the nearest ground at 1080p (2048 was
compared in game: no visible difference, 4x the memory and repository size).
"""

import os
import sys

import numpy as np

sys.dont_write_bytecode = True  # keep tools/blender free of __pycache__
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import vo_common as vc  # noqa: E402

SOURCE_DIR = os.path.join(vc.HERE, "source_textures", "polyhaven")
LAYER_SIZE = 1024

# layer name -> (Poly Haven asset, saturation, brightness, (r, g, b) linear multiplier). Splat order
# (gen_terrain.py LAYERS): grass, dirt road, rock, dry grass.
LAYERS = {
    "grass": ("forrest_ground_01", 1.05, 0.80, (0.88, 1.0, 0.84)),
    "dirt": ("rocky_trail", 0.85, 0.92, (1.0, 0.98, 0.95)),
    "rock": ("rock_face_03", 0.42, 1.05, (0.96, 0.97, 1.0)),
    "drygrass": ("grass_ground", 0.85, 1.08, (1.06, 1.0, 0.84)),
}
# M13 homeland layers (gen_zones.py), the same four scans graded further. Extra per layer: `lift`
# (colour pulled towards white: lin = lift + (1 - lift) * lin, for snow) and `flat` (normal map xy
# scaled down: smoother ground). Zone 2 (Aydor march): golden steppe, pale chalk road, chalk rock,
# scorched earth. Zone 3 (Kargan pass): ash-grey tundra, dark gravel, basalt, snow.
LAYERS_M13 = {
    "steppe": ("grass_ground", 1.0, 1.32, (1.16, 1.0, 0.56), 0.0, 1.0),
    "chalk_road": ("rocky_trail", 0.45, 1.30, (1.06, 1.02, 0.94), 0.0, 1.0),
    "chalk_rock": ("rock_face_03", 0.22, 1.85, (1.05, 1.03, 0.96), 0.0, 1.0),
    "scorched": ("forrest_ground_01", 0.28, 0.42, (1.08, 0.94, 0.86), 0.0, 1.0),
    "ash_tundra": ("forrest_ground_01", 0.38, 0.66, (0.93, 0.98, 1.02), 0.0, 1.0),
    "gravel": ("rocky_trail", 0.30, 0.58, (0.95, 0.97, 1.02), 0.0, 1.0),
    "basalt": ("rock_face_03", 0.18, 0.50, (0.95, 0.97, 1.03), 0.0, 1.0),
    "snow": ("rock_face_03", 0.08, 0.40, (0.94, 0.97, 1.04), 0.66, 0.45),
}


def srgb_to_lin(c):
    return np.where(c <= 0.04045, c / 12.92, ((c + 0.055) / 1.055) ** 2.4)


def lin_to_srgb(c):
    c = np.clip(c, 0.0, 1.0)
    return np.where(c <= 0.0031308, c * 12.92, 1.055 * np.power(c, 1.0 / 2.4) - 0.055)


def box_down(a, size):
    f = a.shape[0] // size
    if f <= 1:
        return a
    h, w, c = a.shape
    return a.reshape(h // f, f, w // f, f, c).mean(axis=(1, 3), dtype=np.float64)


def load(asset, kind):
    return vc.read_image(os.path.join(SOURCE_DIR, f"{asset}_{kind}_2k.jpg"))[..., :3].astype(np.float64)


def grade(srgb, sat, bright, mult):
    lin = srgb_to_lin(srgb)
    lum = (lin * np.array([0.2126, 0.7152, 0.0722])).sum(-1, keepdims=True)
    lin = lum + (lin - lum) * sat
    lin = lin * bright * np.array(mult)
    return lin


def build_layer(name, asset, sat, bright, mult, lift=0.0, flat=1.0):
    out = os.path.join(vc.SRC, "textures", "terrain")
    col = box_down(grade(load(asset, "Diffuse"), sat, bright, mult), LAYER_SIZE)
    if lift:
        col = lift * np.array(mult) + (1.0 - lift) * col
    col_srgb = lin_to_srgb(col)
    vc.write_png(os.path.join(out, f"{name}.png"), vc.to_u8(col_srgb))
    nrm = load(asset, "nor_gl")
    xy = (nrm[..., :2] * 2.0 - 1.0) * flat
    z = np.sqrt(np.clip(1.0 - (xy ** 2).sum(-1, keepdims=True), 0.0, 1.0))
    v = box_down(np.concatenate([xy, z], -1), LAYER_SIZE)
    v /= np.maximum(np.linalg.norm(v, axis=-1, keepdims=True), 1e-6)
    nrm_out = v * 0.5 + 0.5
    vc.write_png(os.path.join(out, f"{name}_n.png"), vc.to_u8(nrm_out))
    orm = box_down(load(asset, "arm"), LAYER_SIZE)
    vc.write_png(os.path.join(out, f"{name}_orm.png"), vc.to_u8(orm))
    mean = col_srgb.reshape(-1, 3).mean(0)
    print(f"TEXTURE {name} <- {asset}: {LAYER_SIZE}^2, mean sRGB {np.round(mean * 255).astype(int)}, roughness "
          f"{orm[..., 1].mean():.2f}, ao {orm[..., 0].mean():.2f}")
    return col_srgb, nrm_out, orm


def main():
    args = vc.script_args()
    tiles = []
    for name, (asset, sat, bright, mult) in LAYERS.items():
        tiles.append(build_layer(name, asset, sat, bright, mult))
    tiles_m13 = [build_layer(name, *spec) for name, spec in LAYERS_M13.items()]
    if "--no-preview" in args:
        return
    rows = [np.concatenate([np.tile(box_down(m, 256), (2, 2, 1)) for m in maps], axis=1) for maps in tiles_m13]
    vc.write_png(os.path.join(vc.PREVIEWS, "textures", "terrain_layers_m13.png"),
                 vc.to_u8(np.concatenate(rows, axis=0)))
    # Preview: per layer a row of colour / normal / ORM, each 2x2-tiled at 256 px per tile.
    rows = []
    for maps in tiles:
        row = []
        for m in maps:
            small = box_down(m, 256)
            row.append(np.tile(small, (2, 2, 1)))
        rows.append(np.concatenate(row, axis=1))
    vc.write_png(os.path.join(vc.PREVIEWS, "textures", "terrain_layers.png"), vc.to_u8(np.concatenate(rows, axis=0)))


main()
