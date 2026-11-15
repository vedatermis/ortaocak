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


def sample_noise(lat, X, Y):
    """Periodic value noise of lattice `lat` (cells x cells) at float coordinates X, Y in tile units
    (wraps, so warped lookups stay tileable)."""
    cells = lat.shape[0]
    x, y = (X % 1.0) * cells, (Y % 1.0) * cells
    x0, y0 = np.floor(x).astype(int), np.floor(y).astype(int)
    tx, ty = x - x0, y - y0
    tx, ty = tx * tx * (3 - 2 * tx), ty * ty * (3 - 2 * ty)
    x0 %= cells
    y0 %= cells
    x1, y1 = (x0 + 1) % cells, (y0 + 1) % cells
    a, b, c, d = lat[y0, x0], lat[y0, x1], lat[y1, x0], lat[y1, x1]
    return (a * (1 - tx) + b * tx) * (1 - ty) + (c * (1 - tx) + d * tx) * ty


def warped_fbm(rng, base_cells, octaves, warp=0.08, gain=0.5):
    """fbm looked up through a low-frequency periodic domain warp: organic, non-grid blotches."""
    Y, X = np.mgrid[0:N, 0:N] / N
    wl = [rng.random((3, 3)), rng.random((3, 3)), rng.random((5, 5)), rng.random((5, 5))]
    wx = (sample_noise(wl[0], X, Y) - 0.5) * 2 * warp + (sample_noise(wl[2], X, Y) - 0.5) * warp
    wy = (sample_noise(wl[1], X, Y) - 0.5) * 2 * warp + (sample_noise(wl[3], X, Y) - 0.5) * warp
    total, amp, norm, cells = np.zeros((N, N)), 1.0, 0.0, base_cells
    for _ in range(octaves):
        total += amp * sample_noise(rng.random((cells, cells)), X + wx, Y + wy)
        norm += amp
        amp *= gain
        cells *= 2
    return total / norm


def stretch(n, k=1.0):
    """Normalises a noise field to [0, 1] by its own percentiles (contrast independent of seed),
    then keeps k of that contrast around the middle."""
    lo, hi = np.percentile(n, 2), np.percentile(n, 98)
    return 0.5 + (np.clip((n - lo) / (hi - lo), 0, 1) - 0.5) * k


def grass(rng):
    n = stretch(warped_fbm(rng, 6, 5, 0.10, gain=0.6), 0.6)
    b = blades(rng, 9000, 9)
    base = palette(n, [(0.0, "#42692e"), (0.5, "#557c37"), (1.0, "#688c40")])
    light = palette(n, [(0.0, "#678f43"), (1.0, "#86a955")])
    t = b[..., None] * 0.50
    return base * (1 - t) + light * t


def drygrass(rng):
    n = stretch(warped_fbm(rng, 6, 5, 0.10, gain=0.6), 0.6)
    b = blades(rng, 8000, 8)
    base = palette(n, [(0.0, "#7f7a40"), (0.5, "#938a4c"), (1.0, "#a59957")])
    light = palette(n, [(0.0, "#b3a466"), (1.0, "#c4b474")])
    t = b[..., None] * 0.45
    patches = stretch(warped_fbm(rng, 6, 3, 0.12), 0.8)
    green = palette(patches, [(0.0, "#717a3c"), (1.0, "#7f8842")])
    m = np.clip((patches - 0.6) * 3, 0, 1)[..., None] * 0.45
    col = base * (1 - m) + green * m
    return col * (1 - t) + light * t


def dirt(rng):
    """Packed earth: warped low-contrast blotches, faint darker damp patches and wheel-worn streaks,
    tiny low-contrast grit (no bright pebbles: they read as a repeating dot pattern at 4 m tiles)."""
    n = stretch(warped_fbm(rng, 5, 6, 0.12, gain=0.6), 0.65)
    col = palette(n, [(0.0, "#735740"), (0.5, "#82654a"), (1.0, "#917353")])
    damp = stretch(warped_fbm(rng, 4, 4, 0.15))
    col *= (1.0 - 0.05 * np.clip((damp - 0.55) * 2.5, 0, 1))[..., None]
    fine = stretch(fbm(rng, 64, 2))
    col *= (0.95 + 0.10 * fine)[..., None]
    grit_light = speckles(rng, 1400, 1) * 0.07
    grit_dark = speckles(rng, 1400, 1) * 0.08
    stones = speckles(rng, 160, 2) * 0.06  # a few small stones, barely lighter
    col *= (1.0 + grit_light + stones - grit_dark)[..., None]
    return col


def rock(rng):
    """Weathered stone: warped mottling, soft strata, gentle dark seams (no contour lines), lichen
    as faint olive patches."""
    n = stretch(warped_fbm(rng, 5, 6, 0.10, gain=0.6), 0.7)
    col = palette(n, [(0.0, "#6f6b64"), (0.5, "#807c75"), (1.0, "#928e86")])
    Y, X = np.mgrid[0:N, 0:N] / N
    strata = 0.5 + 0.5 * np.sin(TAU_ * (3 * Y + 1 * X) + 2.5 * warped_fbm(rng, 3, 3, 0.15))
    col *= (0.95 + 0.07 * strata)[..., None]
    seams = 1.0 - np.abs(warped_fbm(rng, 6, 4, 0.08) * 2 - 1)
    col *= (1.0 - 0.08 * np.clip((seams - 0.70) * 2.5, 0, 1))[..., None]
    fine = stretch(fbm(rng, 48, 2))
    col *= (0.95 + 0.10 * fine)[..., None]
    lichen = np.clip((stretch(warped_fbm(rng, 4, 3, 0.12)) - 0.68) * 3.5, 0, 1)[..., None] * 0.22
    return col * (1 - lichen) + np.array([112, 124, 82]) * lichen


TAU_ = 2 * np.pi


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
