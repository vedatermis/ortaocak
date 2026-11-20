"""Tiling detail textures of the worn armour lines (M17 second pass), procedural and original.

Used by gen_armor.py (no Blender needed: numpy only). Every map is periodic (FFT noise, wrapped
differences), so it tiles seamlessly; the armour meshes repeat it with a wrap sampler
(docs/CLIENT_RENDERING.md §18). Per material stem, three PNGs in assets_src/armor/textures/:
  <stem>_c.png    base colour detail (sRGB; multiplied by the piece's vertex colour and the item tint, so
                  it stays light and nearly grey except the embroidery band, which carries its colours)
  <stem>_n.png    tangent-space normal map (OpenGL convention, +Y = image up)
  <stem>_orm.png  R occlusion, G roughness, B metallic

Surfaces (tile in u and v, u around a limb, v along it):
  steel    brushed streaks along u, faint hammer dents; roughness 0.38-0.55, metal
  leather  pebbled grain, mottling, a stitched seam every tile (vertical panels), matt
  cloth    plain weave, 32 threads per tile, slubs, very matt
Strips (u along the strip, v = 0..1 across it):
  trim     a rolled steel edge with one domed rivet per tile
  strap    a darker strap with bevelled edges and a stitch row along each edge
  band     an embroidered hem band: stepped lozenges and hooks in gold thread on a wine-red ground
"""

import os

import numpy as np

STEMS = ("steel", "trim", "leather", "strap", "cloth", "band")

# Metres per texture repeat along u (surfaces: along v too); strips span v = 0..1 across their width.
TILE = {"steel": 0.30, "trim": 0.05, "leather": 0.21, "strap": 0.07, "cloth": 0.085, "band": 0.11}


def pnoise(h, w, fx, fy, seed, power=2.0):
    """Periodic gaussian noise (zero mean, unit std): spectrum falling off beyond fx / fy cycles per image."""
    rng = np.random.default_rng(seed)
    ky = np.fft.fftfreq(h)[:, None] * h
    kx = np.fft.fftfreq(w)[None, :] * w
    k = np.sqrt((kx / fx) ** 2 + (ky / fy) ** 2)
    amp = 1.0 / (1.0 + k ** power)
    amp[0, 0] = 0.0
    spec = (rng.normal(size=(h, w)) + 1j * rng.normal(size=(h, w))) * amp
    n = np.real(np.fft.ifft2(spec))
    return (n - n.mean()) / (n.std() + 1e-12)


def normal_map(height, strength):
    """Height (texels) -> RGB8 normal map, OpenGL convention (+Y up = towards row 0); wraps."""
    gx = (np.roll(height, -1, 1) - np.roll(height, 1, 1)) * 0.5 * strength
    gr = (np.roll(height, -1, 0) - np.roll(height, 1, 0)) * 0.5 * strength
    n = np.stack([-gx, gr, np.ones_like(height)], -1)
    n /= np.linalg.norm(n, axis=-1, keepdims=True)
    return np.clip(np.round((n * 0.5 + 0.5) * 255.0), 0, 255).astype(np.uint8)


def normal_map_clamped(height, strength):
    """As normal_map, but rows (v, across a strip) do not wrap."""
    gx = (np.roll(height, -1, 1) - np.roll(height, 1, 1)) * 0.5 * strength
    hp = np.pad(height, ((1, 1), (0, 0)), mode="edge")
    gr = (hp[2:] - hp[:-2]) * 0.5 * strength
    n = np.stack([-gx, gr, np.ones_like(height)], -1)
    n /= np.linalg.norm(n, axis=-1, keepdims=True)
    return np.clip(np.round((n * 0.5 + 0.5) * 255.0), 0, 255).astype(np.uint8)


def u8(a):
    return np.clip(np.round(np.asarray(a) * 255.0), 0, 255).astype(np.uint8)


def grey(v, tint=(1.0, 1.0, 1.0)):
    return u8(np.stack([v * tint[0], v * tint[1], v * tint[2]], -1))


def orm(occ, rough, metal):
    return u8(np.stack([occ, rough, np.broadcast_to(metal, occ.shape)], -1))


def wrapdist(a, period):
    """Distance to the nearest multiple of `period` (periodic)."""
    m = np.mod(a, period)
    return np.minimum(m, period - m)


def steel():
    n = 256
    streak = pnoise(n, n, 1.2, 70.0, 11)
    fine = pnoise(n, n, 50.0, 50.0, 12)
    dents = pnoise(n, n, 2.5, 2.5, 13, power=3.0)
    height = 0.35 * streak + 0.15 * fine + 14.0 * dents
    albedo = np.clip(0.75 + 0.045 * streak + 0.035 * dents + 0.01 * fine, 0.0, 1.0)
    rough = np.clip(0.5 + 0.06 * streak + 0.03 * fine + 0.05 * dents, 0.3, 0.65)
    occ = np.clip(1.0 + 0.04 * dents, 0.85, 1.0)
    return grey(albedo, (0.98, 0.99, 1.0)), normal_map(height, 0.08), orm(occ, rough, 1.0)


def trim():
    w, h = 128, 64
    y, x = np.mgrid[0:h, 0:w].astype(float)
    v = (y + 0.5) / h
    bead = np.sin(np.pi * np.clip((v - 0.06) / 0.88, 0.0, 1.0)) ** 0.7  # the rolled edge, across
    groove = np.exp(-((v - 0.5) / 0.05) ** 2)                            # a pressed line along it
    streak = pnoise(h, w, 1.0, 30.0, 21)
    dx = wrapdist(x + 0.5 - w / 2, w)
    d = np.sqrt(dx ** 2 + (y + 0.5 - h / 2) ** 2) / (h * 0.22)
    rivet = np.sqrt(np.clip(1.0 - d ** 2, 0.0, 1.0))
    ring = np.exp(-((d - 1.08) / 0.12) ** 2)
    height = 9.0 * bead - 1.6 * groove * (rivet < 0.01) + 7.0 * rivet + 0.3 * streak
    albedo = np.clip(0.84 + 0.03 * streak - 0.10 * groove * (rivet < 0.01) - 0.18 * ring + 0.08 * rivet, 0.0, 1.0)
    rough = np.clip(0.40 + 0.04 * streak - 0.10 * rivet + 0.06 * ring, 0.25, 0.7)
    occ = np.clip(1.0 - 0.35 * ring - 0.2 * groove * (rivet < 0.01) - 0.25 * (1.0 - bead) ** 3, 0.4, 1.0)
    return grey(albedo, (0.98, 0.99, 1.0)), normal_map_clamped(height, 0.16), orm(occ, rough, 1.0)


def leather():
    n = 256
    grain = pnoise(n, n, 45.0, 45.0, 31, power=2.5)
    pebble = -np.abs(grain)
    mottle = pnoise(n, n, 3.0, 3.0, 32)
    crease = pnoise(n, n, 10.0, 3.5, 33)
    y, x = np.mgrid[0:n, 0:n].astype(float)
    sd = wrapdist(x + 0.5, n)                       # vertical seam at u = 0 (each tile = one panel)
    seam = np.exp(-(sd / 2.2) ** 2)
    stitch_row = np.exp(-((sd - 7.5) / 1.4) ** 2)
    dash = (np.mod(y, 9.0) < 5.5).astype(float)
    stitch = stitch_row * dash
    height = 1.2 * pebble + 0.6 * np.abs(crease) * -1.0 - 6.0 * seam + 2.2 * stitch
    albedo = np.clip(0.84 + 0.05 * mottle + 0.025 * pebble - 0.30 * seam + 0.10 * stitch, 0.0, 1.0)
    rough = np.clip(0.74 + 0.04 * pebble + 0.03 * mottle + 0.05 * seam - 0.08 * stitch, 0.55, 0.92)
    occ = np.clip(1.0 - 0.45 * seam + 0.04 * pebble, 0.4, 1.0)
    return grey(albedo, (1.0, 0.98, 0.95)), normal_map(height, 0.22), orm(occ, rough, 0.0)


def strap():
    w, h = 128, 64
    y, x = np.mgrid[0:h, 0:w].astype(float)
    v = (y + 0.5) / h
    edge = np.clip(np.minimum(v, 1.0 - v) / 0.12, 0.0, 1.0)
    grain = pnoise(h, w, 40.0, 20.0, 41)
    dash = (np.mod(x, 8.0) < 5.0).astype(float)
    stitch = (np.exp(-((v - 0.2) / 0.035) ** 2) + np.exp(-((v - 0.8) / 0.035) ** 2)) * dash
    height = 6.0 * np.sqrt(edge) + 0.5 * -np.abs(grain) + 1.8 * stitch
    albedo = np.clip(0.66 + 0.03 * grain - 0.12 * (1.0 - edge) + 0.22 * stitch, 0.0, 1.0)
    rough = np.clip(0.62 + 0.04 * grain + 0.1 * stitch, 0.45, 0.9)
    occ = np.clip(1.0 - 0.3 * (1.0 - edge), 0.5, 1.0)
    return grey(albedo, (1.0, 0.97, 0.93)), normal_map_clamped(height, 0.2), orm(occ, rough, 0.0)


def cloth():
    n = 256
    threads = 32
    p = n / threads
    y, x = np.mgrid[0:n, 0:n].astype(float)
    ix, iy = np.floor(x / p), np.floor(y / p)
    fu, fv = (x / p) % 1.0, (y / p) % 1.0
    over = (np.mod(ix + iy, 2.0) == 0)
    warp = np.sin(np.pi * fu) ** 0.6 * (0.55 + 0.45 * np.sin(np.pi * fv))   # thread along v, on top
    weft = np.sin(np.pi * fv) ** 0.6 * (0.55 + 0.45 * np.sin(np.pi * fu))
    weave = np.where(over, warp, weft)
    slub = pnoise(n, n, 4.0, 40.0, 51) * 0.5 + pnoise(n, n, 40.0, 4.0, 52) * 0.5
    fuzz = pnoise(n, n, 60.0, 60.0, 53)
    height = 2.4 * weave + 0.4 * slub
    albedo = np.clip(0.78 + 0.14 * weave + 0.04 * slub + 0.015 * fuzz, 0.0, 1.0)
    rough = np.clip(0.9 + 0.03 * fuzz, 0.8, 1.0)
    occ = np.clip(0.78 + 0.22 * weave, 0.0, 1.0)
    return grey(albedo, (1.0, 0.99, 0.97)), normal_map(height, 0.35), orm(occ, rough, 0.0)


def band():
    """Embroidered band: u along (one motif per tile), v across. Original pattern: a stepped lozenge with
    hooked arms, small crosses between, gold border lines; satin-stitch streaks follow the motif."""
    w, h = 256, 128
    y, x = np.mgrid[0:h, 0:w].astype(float)
    u = (x + 0.5) / w
    v = (y + 0.5) / h
    ground = np.array([0.36, 0.09, 0.10])
    gold = np.array([0.93, 0.74, 0.36])
    ivory = np.array([0.92, 0.88, 0.78])
    # Lozenge centred in the tile, stepped (quantised L1 distance), with a hollow core.
    du, dv = np.abs(u - 0.5) * 2.0, np.abs(v - 0.5) * 2.0
    l1 = du / 0.62 + dv / 0.62
    step = np.floor(l1 * 6.0) / 6.0
    loz = (step < 1.0) & (step >= 0.5)
    core = step < 0.17
    # Hooks: short arms leaving the lozenge's left and right tips and turning up / down.
    hook = ((np.abs(v - 0.5) < 0.06) & (du > 0.6) & (du < 0.82)) | \
           ((np.abs(du - 0.8) < 0.05) & (np.abs(v - 0.5) < 0.2) & (np.sign(v - 0.5) == np.sign(u - 0.5)))
    # Small crosses at the tile seams (u = 0).
    su = wrapdist(u, 1.0)
    cross = ((su < 0.035) & (np.abs(v - 0.5) < 0.14)) | ((np.abs(v - 0.5) < 0.05) & (su < 0.07))
    border = (np.abs(v - 0.1) < 0.035) | (np.abs(v - 0.9) < 0.035)
    edge = (v < 0.03) | (v > 0.97)
    motif_gold = loz | hook | border
    motif_ivory = core | cross
    col = np.ones((h, w, 3)) * ground
    col[motif_gold] = gold
    col[motif_ivory] = ivory
    col[edge] = ground * 0.6
    stitch = pnoise(h, w, 50.0, 3.0, 61)  # satin stitches across the band
    raised = (motif_gold | motif_ivory).astype(float)
    shade = 1.0 + 0.07 * stitch * raised + 0.03 * pnoise(h, w, 30.0, 30.0, 62) * (1.0 - raised)
    col = np.clip(col * shade[..., None], 0.0, 1.0)
    height = 3.0 * raised + 0.5 * stitch * raised
    rough = np.where(raised > 0, 0.62, 0.9)
    occ = np.clip(0.85 + 0.15 * raised, 0.0, 1.0)
    return u8(col), normal_map_clamped(height, 0.25), orm(occ, rough, 0.0)


MAKERS = {"steel": steel, "trim": trim, "leather": leather, "strap": strap, "cloth": cloth, "band": band}


def write_all(out_dir, write_png):
    """Writes <stem>_c / _n / _orm.png for every stem (unchanged files are not rewritten)."""
    os.makedirs(out_dir, exist_ok=True)
    paths = []
    for stem in STEMS:
        c, nrm, o = MAKERS[stem]()
        for suffix, img in (("_c", c), ("_n", nrm), ("_orm", o)):
            p = os.path.join(out_dir, f"{stem}{suffix}.png")
            tmp = p + ".tmp"
            write_png(tmp, img)
            with open(tmp, "rb") as f:
                new = f.read()
            old = None
            if os.path.exists(p):
                with open(p, "rb") as f:
                    old = f.read()
            if old == new:
                os.remove(tmp)
            else:
                os.replace(tmp, p)
            paths.append(p)
    return paths
