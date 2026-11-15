"""Tileable terrain layer textures (512x512 RGB PNG), generated with periodic noise in numpy.

Usage (headless):
  blender --background --factory-startup --python tools/blender/gen_textures.py

Outputs assets_src/textures/terrain/{grass,dirt,rock,drygrass}.png and
assets_src/previews/textures/terrain_layers.png (each texture tiled 2x2 to show the seams).
"""

import os
import sys

import numpy as np

sys.dont_write_bytecode = True  # keep tools/blender free of __pycache__
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import vo_common as vc  # noqa: E402

N = 512


def periodic_noise(rng, cells, size=N):
    """Smooth value noise that wraps around (tileable): `cells` lattice cells per side."""
    lat = rng.random((cells, cells))
    x = np.arange(size) * cells / size
    i0 = np.floor(x).astype(int)
    t = x - i0
    t = t * t * (3 - 2 * t)
    i1 = (i0 + 1) % cells
    a = lat[np.ix_(i0, i0)]
    b = lat[np.ix_(i0, i1)]
    c = lat[np.ix_(i1, i0)]
    d = lat[np.ix_(i1, i1)]
    ty, tx = t[:, None], t[None, :]
    return (a * (1 - tx) + b * tx) * (1 - ty) + (c * (1 - tx) + d * tx) * ty


def fbm(rng, base_cells, octaves, gain=0.5):
    total, amp, norm = np.zeros((N, N)), 1.0, 0.0
    cells = base_cells
    for _ in range(octaves):
        total += amp * periodic_noise(rng, cells)
        norm += amp
        amp *= gain
        cells *= 2
    return total / norm


def palette(t, stops):
    """Maps t in [0,1] through colour stops [(pos, '#hex'), ...] (sRGB)."""
    pos = np.array([s[0] for s in stops])
    cols = np.array([[int(s[1][i:i + 2], 16) for i in (1, 3, 5)] for s in stops], float)
    out = np.zeros(t.shape + (3,))
    for ch in range(3):
        out[..., ch] = np.interp(t, pos, cols[:, ch])
    return out


def speckles(rng, count, radius, shape=(N, N)):
    """Tileable soft dots (wraps around the edges)."""
    img = np.zeros(shape)
    yy, xx = np.mgrid[-radius:radius + 1, -radius:radius + 1]
    disc = np.clip(1.0 - np.sqrt(xx ** 2 + yy ** 2) / (radius + 0.5), 0, 1)
    for _ in range(count):
        y, x = rng.integers(0, shape[0]), rng.integers(0, shape[1])
        ys = (np.arange(y - radius, y + radius + 1)) % shape[0]
        xs = (np.arange(x - radius, x + radius + 1)) % shape[1]
        img[np.ix_(ys, xs)] = np.maximum(img[np.ix_(ys, xs)], disc * rng.uniform(0.5, 1.0))
    return img


def blades(rng, count, length, shape=(N, N)):
    """Short tileable strokes (grass blades) leaning in random directions; returns intensity."""
    img = np.zeros(shape)
    for _ in range(count):
        y, x = rng.integers(0, shape[0]), rng.integers(0, shape[1])
        ang = rng.uniform(-0.6, 0.6) - np.pi / 2
        val = rng.uniform(0.4, 1.0)
        L = int(rng.integers(length // 2, length + 1))
        for k in range(L):
            py = int(round(y + np.sin(ang) * k)) % shape[0]
            px = int(round(x + np.cos(ang) * k)) % shape[1]
            img[py, px] = max(img[py, px], val * (1 - k / (L + 1) * 0.5))
    return img


def grass(rng):
    n = fbm(rng, 4, 5)
    b = blades(rng, 9000, 9)
    base = palette(n, [(0.0, "#3f6a2c"), (0.5, "#557f36"), (1.0, "#6f9442")])
    light = palette(n, [(0.0, "#6d9a45"), (1.0, "#93b85a")])
    t = b[..., None] * 0.65
    return base * (1 - t) + light * t


def drygrass(rng):
    n = fbm(rng, 4, 5)
    b = blades(rng, 8000, 8)
    base = palette(n, [(0.0, "#7c7a3c"), (0.5, "#988f4a"), (1.0, "#ad9f58")])
    light = palette(n, [(0.0, "#bfae68"), (1.0, "#d4c27c")])
    t = b[..., None] * 0.6
    patches = fbm(rng, 8, 3)
    green = palette(patches, [(0.0, "#6d7a38"), (1.0, "#7f8a40")])
    m = np.clip((patches - 0.55) * 4, 0, 1)[..., None] * 0.5
    col = base * (1 - m) + green * m
    return col * (1 - t) + light * t


def dirt(rng):
    n = fbm(rng, 4, 6)
    col = palette(n, [(0.0, "#6e5238"), (0.5, "#8a6a49"), (1.0, "#a3825b")])
    ruts = fbm(rng, 2, 3)
    col *= (0.9 + 0.1 * ruts)[..., None]
    peb = speckles(rng, 900, 3)
    pebc = palette(fbm(rng, 16, 2), [(0.0, "#8f8a80"), (1.0, "#b7ae9c")])
    t = (peb > 0.25)[..., None] * 0.85
    shade = speckles(rng, 900, 2) * 0.12
    return (col * (1 - shade[..., None])) * (1 - t) + pebc * t


def rock(rng):
    n = fbm(rng, 3, 6)
    ridged = 1.0 - np.abs(fbm(rng, 8, 4) * 2 - 1)
    col = palette(n, [(0.0, "#6a665f"), (0.5, "#83807a"), (1.0, "#a19d95")])
    facets = fbm(rng, 12, 2)
    col *= (0.9 + 0.2 * facets)[..., None]
    cracks = np.clip((ridged - 0.93) * 10, 0, 1)
    col *= (1.0 - 0.3 * cracks)[..., None]
    lichen = np.clip((fbm(rng, 6, 3) - 0.62) * 5, 0, 1)[..., None] * 0.35
    return col * (1 - lichen) + np.array([120, 138, 84]) * lichen


LAYERS = [("grass", grass, 101), ("dirt", dirt, 102), ("rock", rock, 103), ("drygrass", drygrass, 104)]


def main():
    out_dir = vc.ensure_dir(os.path.join(vc.SRC, "textures", "terrain"))
    tiles = []
    for name, fn, seed in LAYERS:
        rng = np.random.default_rng(seed)
        img = np.clip(fn(rng), 0, 255).astype(np.uint8)
        vc.write_png(os.path.join(out_dir, f"{name}.png"), img)
        tiles.append(np.tile(img, (2, 2, 1))[::2, ::2])  # 2x2 tiling shown at 512 px
        print(f"TEXTURE {name}: mean colour {img.reshape(-1, 3).mean(axis=0).round(1)}")
    pad = 6
    sheet = np.full((N + 2 * pad, 4 * N + 5 * pad, 3), 40, np.uint8)
    for i, t in enumerate(tiles):
        sheet[pad:pad + N, pad + i * (N + pad):pad + i * (N + pad) + N] = t
    vc.write_png(os.path.join(vc.PREVIEWS, "textures", "terrain_layers.png"), sheet)


main()
