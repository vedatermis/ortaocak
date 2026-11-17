"""HUD art (after M13): the glyphs of the HUD bar's buttons. Same painter as the M9 / M12 icons
(`gen_effects.py`, `gen_skill_art.py`: signed distance fields, 4x supersampled, box-filtered on
premultiplied linear colour), same ink outline and gold / ivory / steel palette, so the glyphs sit
in the M12 slot art (slot_back + glyph + slot_frame_*) next to the skill icons as one set.

A glyph is symbol-only on a transparent background: the client draws it into the slot's icon window
(52/64 of the button). Drawn at 32-48 px (40 / 34 logical buttons at 100-150 % UI scale); shapes are
kept bold with few details so they read at 26 px.

Usage (headless):
  blender --background --factory-startup --python tools/blender/gen_hud_art.py -- [--sheets <dir>] [--no-preview]

Outputs (straight sRGB alpha PNGs; assetc writes the premultiplied .vtex next to each copy):
  assets_src/ui/hud/<name>.png   64x64: attack, sit, loot, run, walk, inventory, kunye, skills,
                                 controls, menu
Contact sheet (not an asset; default <repo>/build/art_sheets/hud, git-ignored):
  hud_glyphs.png   every glyph in the slot art at 48, 40, 34 and 28 px on the HUD colour, raw at 64
Deterministic: no time, no randomness.
"""

import math
import os
import sys

import numpy as np

sys.dont_write_bytecode = True
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import vo_common as vc  # noqa: E402
import gen_skill_art as sa  # noqa: E402
from gen_effects import (sd_round_rect, sd_circle, sd_stroke, sd_polygon, arc_points,  # noqa: E402
                         hexc, lerp_field, _lin, _srgb)
from gen_skill_art import Canvas, symbol, union, sub, sd_ellipse, rot, draw_sword, INK  # noqa: E402

HUD_DIR = os.path.join(vc.SRC, "ui", "hud")
DEFAULT_SHEETS = os.path.join(vc.REPO, "build", "art_sheets", "hud")

OUT = 0.045  # ink outline (a little bolder than the skill icons: the glyphs are drawn smaller)
GOLD = ("#fff0b8", "#c98a2a")
IVORY = ("#fbf3dc", "#c9b88e")
LEATHER = ("#b8834a", "#5a3416")
STEEL = ("#f4f6fa", "#8d97a6")


def shade_y(cv, y0, y1, c):
    return lerp_field((cv.y - y0) / max(y1 - y0, 1e-6), *c)


def ink(cv, d, w=OUT):
    cv.over(cv.cover(d - w), hexc(INK))


def paint(cv, d, shade):
    cv.over(cv.cover(d), shade)


# ---------------------------------------------------------------------------------------------
# Glyphs (icon units 0..1, y down)

def glyph_attack(cv):
    """Two swords crossed point-up (Saldır)."""
    draw_sword(cv, 0.30, 0.74, 0.80, 0.18, w=0.058, guard=0.13, grip=0.13, outline=OUT)
    draw_sword(cv, 0.70, 0.74, 0.20, 0.18, w=0.058, guard=0.13, grip=0.13, outline=OUT)
    # The first blade again on top of the second's hilt: a woven cross.
    x, y = cv.x, cv.y
    blade = sa.sd_blade(x, y, 0.30, 0.74, 0.80, 0.18, 0.058)
    upper = np.maximum(blade, -(0.47 - y))  # only the part above the crossing
    ink(cv, upper)
    paint(cv, upper, sa.blade_shade(x, y, 0.30, 0.74, 0.80, 0.18, *STEEL))


def glyph_sit(cv):
    """A figure sitting cross-legged on the ground (Otur / Kalk)."""
    x, y = cv.x, cv.y
    head = sd_circle(x, y, 0.5, 0.2, 0.095)
    torso = sd_stroke(x, y, [(0.5, 0.33), (0.5, 0.56)], [0.085, 0.1])
    legs = union(sd_stroke(x, y, [(0.5, 0.66), (0.28, 0.62), (0.18, 0.72)], [0.075, 0.07, 0.06]),
                 sd_stroke(x, y, [(0.5, 0.66), (0.72, 0.62), (0.82, 0.72)], [0.075, 0.07, 0.06]),
                 sd_ellipse(x, y, 0.5, 0.68, 0.24, 0.085))
    arms = union(sd_stroke(x, y, [(0.43, 0.38), (0.32, 0.5), (0.3, 0.6)], [0.045, 0.042, 0.04]),
                 sd_stroke(x, y, [(0.57, 0.38), (0.68, 0.5), (0.7, 0.6)], [0.045, 0.042, 0.04]))
    body = union(head, torso, legs, arms)
    ground = sd_round_rect(x, y, 0.5, 0.84, 0.36, 0.025, 0.02)
    ink(cv, union(body, ground))
    paint(cv, ground, shade_y(cv, 0.81, 0.87, ("#c9a052", "#6a4c1c")))
    paint(cv, union(legs, torso, arms), shade_y(cv, 0.3, 0.8, IVORY))
    paint(cv, head, shade_y(cv, 0.1, 0.3, IVORY))
    # A small rest mark: a crescent above the right shoulder.
    moon = sub(sd_circle(x, y, 0.78, 0.22, 0.085), sd_circle(x, y, 0.815, 0.19, 0.075))
    ink(cv, moon, 0.035)
    paint(cv, moon, shade_y(cv, 0.14, 0.3, GOLD))


def glyph_loot(cv):
    """A tied coin sack with two coins in front (Ganimet)."""
    x, y = cv.x, cv.y
    sack = union(sd_ellipse(x, y, 0.46, 0.6, 0.27, 0.25),
                 sd_polygon(x, y, [(0.36, 0.22), (0.56, 0.22), (0.52, 0.4), (0.4, 0.4)]))
    ears = union(sd_polygon(x, y, [(0.36, 0.22), (0.27, 0.13), (0.4, 0.17)]),
                 sd_polygon(x, y, [(0.56, 0.22), (0.65, 0.13), (0.52, 0.17)]))
    ink(cv, union(sack, ears))
    paint(cv, union(sack, ears), lerp_field(np.hypot(cv.x - 0.38, cv.y - 0.5) / 0.4, *LEATHER))
    # The tie: a gold cord.
    tie = sd_round_rect(x, y, 0.46, 0.33, 0.12, 0.03, 0.02)
    ink(cv, tie, 0.03)
    paint(cv, tie, shade_y(cv, 0.3, 0.36, GOLD))
    # Two coins, front right.
    for (cx, cy, r) in ((0.66, 0.74, 0.13), (0.78, 0.6, 0.11)):
        c = sd_circle(x, y, cx, cy, r)
        ink(cv, c)
        paint(cv, c, lerp_field(np.hypot(cv.x - cx + 0.04, cv.y - cy + 0.04) / r, *GOLD))
        rim = np.abs(sd_circle(x, y, cx, cy, r * 0.68)) - 0.012
        cv.over(cv.cover(rim) * 0.7, hexc("#8a5a14"))


def _boot(cv, ox, oy, s, lift=0.0):
    x, y = cv.x, cv.y
    rx, ry = rot(x, y, ox, oy, lift)
    shaft = sd_round_rect(rx, ry, ox, oy - 0.12 * s, 0.11 * s, 0.17 * s, 0.03 * s)
    foot = sd_round_rect(rx, ry, ox + 0.08 * s, oy + 0.08 * s, 0.19 * s, 0.07 * s, 0.06 * s)
    toe = sd_circle(rx, ry, ox + 0.24 * s, oy + 0.07 * s, 0.08 * s)
    boot = union(shaft, foot, toe)
    sole = np.maximum(boot, -(ry - (oy + 0.11 * s)))
    cuff = np.maximum(boot, (ry - (oy - 0.25 * s)))
    return boot, sole, cuff


def glyph_run(cv):
    """A striding boot with three speed streaks behind it (Koş: the current gait is running)."""
    x, y = cv.x, cv.y
    boot, sole, cuff = _boot(cv, 0.52, 0.56, 1.25, lift=-12)
    for (yy, x0, w) in ((0.36, 0.08, 0.045), (0.52, 0.04, 0.05), (0.68, 0.1, 0.04)):
        st = sd_stroke(x, y, [(x0, yy), (x0 + 0.24, yy)], [0.004, w])
        ink(cv, st, 0.03)
        paint(cv, st, lerp_field((cv.x - x0) / 0.24, "#7a5a24", "#ffe08a"))
    ink(cv, boot)
    paint(cv, boot, lerp_field(np.hypot(cv.x - 0.45, cv.y - 0.4) / 0.5, *LEATHER))
    paint(cv, sole, hexc("#2a1a0c"))
    paint(cv, cuff, shade_y(cv, 0.2, 0.3, GOLD))


def glyph_walk(cv):
    """Two footprints, one ahead of the other (Yürü: the current gait is walking)."""
    x, y = cv.x, cv.y
    for (cx, cy, ang) in ((0.36, 0.64, -8), (0.64, 0.34, 8)):
        rx, ry = rot(x, y, cx, cy, ang)
        sole = sd_ellipse(rx, ry, cx, cy, 0.1, 0.17)
        heel = sd_ellipse(rx, ry, cx, cy + 0.23, 0.075, 0.07)
        toes = union(*[sd_circle(rx, ry, cx - 0.07 + 0.035 * k, cy - 0.2 - 0.012 * (2 - abs(k - 2)), 0.032)
                       for k in range(5)])
        foot = union(sole, heel, toes)
        ink(cv, foot)
        paint(cv, foot, shade_y(cv, cy - 0.25, cy + 0.3, IVORY))


def glyph_inventory(cv):
    """A leather satchel with a flap and a gold buckle (Envanter)."""
    x, y = cv.x, cv.y
    strap = np.abs(sd_round_rect(x, y, 0.5, 0.3, 0.2, 0.17, 0.16)) - 0.035
    strap = np.maximum(strap, y - 0.34)
    body = sd_round_rect(x, y, 0.5, 0.6, 0.33, 0.25, 0.07)
    flap = sd_polygon(x, y, [(0.17, 0.4), (0.83, 0.4), (0.8, 0.6), (0.5, 0.68), (0.2, 0.6)])
    ink(cv, union(strap, body, flap))
    paint(cv, strap, shade_y(cv, 0.12, 0.34, LEATHER))
    paint(cv, body, shade_y(cv, 0.36, 0.86, ("#9a6a38", "#4a2a10")))
    paint(cv, flap, shade_y(cv, 0.38, 0.68, ("#c8935a", "#6a4220")))
    seam = np.abs(sd_polygon(x, y, [(0.21, 0.43), (0.79, 0.43), (0.76, 0.58), (0.5, 0.645), (0.24, 0.58)])) - 0.006
    cv.over(cv.cover(np.maximum(seam, flap)) * 0.55, hexc("#f0d090"))
    buckle = sd_round_rect(x, y, 0.5, 0.66, 0.075, 0.065, 0.02)
    ink(cv, buckle, 0.03)
    paint(cv, buckle, shade_y(cv, 0.6, 0.73, GOLD))
    hole = sd_round_rect(x, y, 0.5, 0.66, 0.03, 0.025, 0.01)
    paint(cv, hole, hexc("#3a2408"))


def glyph_kunye(cv):
    """A parchment scroll with text lines and a red wax seal (Künye: the character's record)."""
    x, y = cv.x, cv.y
    sheet = sd_round_rect(x, y, 0.5, 0.5, 0.27, 0.3, 0.02)
    top_roll = sd_round_rect(x, y, 0.5, 0.2, 0.33, 0.065, 0.06)
    bot_roll = sd_round_rect(x, y, 0.5, 0.8, 0.33, 0.065, 0.06)
    ink(cv, union(sheet, top_roll, bot_roll))
    paint(cv, sheet, shade_y(cv, 0.2, 0.8, ("#fbf0d0", "#d8c08a")))
    for roll, y0 in ((top_roll, 0.14), (bot_roll, 0.74)):
        paint(cv, roll, shade_y(cv, y0, y0 + 0.13, ("#f0dca8", "#a8884a")))
        end = np.maximum(roll, -(np.abs(x - 0.5) - 0.28))
        paint(cv, end, shade_y(cv, y0, y0 + 0.13, ("#d8bc80", "#7a5a24")))
    for k, (x0, x1) in enumerate(((0.32, 0.68), (0.32, 0.62), (0.32, 0.66), (0.32, 0.5))):
        yy = 0.34 + 0.085 * k
        ln = sd_stroke(x, y, [(x0, yy), (x1, yy)], [0.016, 0.016])
        cv.over(cv.cover(ln) * 0.85, hexc("#6a5430"))
    seal = sd_circle(x, y, 0.64, 0.64, 0.105)
    ink(cv, seal, 0.03)
    paint(cv, seal, lerp_field(np.hypot(cv.x - 0.61, cv.y - 0.61) / 0.12, "#f05a48", "#7a0c0c"))
    star = sa.sd_star(x, y, 0.64, 0.64, 0.055, 0.024, 6, -90)
    cv.over(cv.cover(star) * 0.8, hexc("#ffc0a0"))


def glyph_skills(cv):
    """A small skill tree: a root gem branching up into two, and one above (Yetenekler)."""
    x, y = cv.x, cv.y
    nodes = {"root": (0.5, 0.8), "left": (0.26, 0.48), "right": (0.74, 0.48), "top": (0.5, 0.18)}
    links = union(sd_stroke(x, y, [nodes["root"], (0.5, 0.64), nodes["left"]], [0.03, 0.03, 0.03]),
                  sd_stroke(x, y, [(0.5, 0.64), nodes["right"]], [0.03, 0.03]),
                  sd_stroke(x, y, [nodes["left"], (0.5, 0.33), nodes["top"]], [0.03, 0.03, 0.03]))
    ink(cv, links, 0.03)
    paint(cv, links, shade_y(cv, 0.15, 0.85, GOLD))
    gems = {"root": ("#7ad8ff", "#1a4a8a"), "left": ("#ff8a70", "#8a1414"), "right": ("#9af07a", "#1e6a14"),
            "top": ("#fff4b0", "#c98a1a")}
    for name, (cx, cy) in nodes.items():
        s = 0.13 if name != "top" else 0.12
        d = sd_polygon(x, y, [(cx, cy - s), (cx + s, cy), (cx, cy + s), (cx - s, cy)])
        ink(cv, d)
        paint(cv, d, shade_y(cv, cy - s, cy + s, GOLD))
        inner = sd_polygon(x, y, [(cx, cy - s * 0.6), (cx + s * 0.6, cy), (cx, cy + s * 0.6), (cx - s * 0.6, cy)])
        paint(cv, inner, lerp_field(np.hypot(cv.x - cx + 0.02, cv.y - cy + 0.03) / (s * 0.7), *gems[name]))
        hl = sd_circle(x, y, cx - s * 0.18, cy - s * 0.25, s * 0.13)
        cv.over(cv.cover(hl) * 0.8, hexc("#ffffff"))


def _keycap(cv, cx, cy, s, arrow):
    """A key cap seen slightly from above: dark base edge, ivory top, an engraved arrow."""
    x, y = cv.x, cv.y
    base = sd_round_rect(x, y, cx, cy + 0.03 * s, 0.16 * s, 0.15 * s, 0.05 * s)
    top = sd_round_rect(x, y, cx, cy - 0.01 * s, 0.13 * s, 0.11 * s, 0.04 * s)
    ink(cv, base, 0.035)
    paint(cv, base, shade_y(cv, cy - 0.12 * s, cy + 0.2 * s, ("#a08a60", "#3a2c14")))
    paint(cv, top, shade_y(cv, cy - 0.12 * s, cy + 0.1 * s, IVORY))
    a = math.radians({"up": -90, "left": 180, "right": 0, "down": 90}[arrow])
    ux, uy = math.cos(a), math.sin(a)
    nx, ny = -uy, ux
    tip = (cx + ux * 0.07 * s, cy - 0.01 * s + uy * 0.07 * s)
    back = (cx - ux * 0.05 * s, cy - 0.01 * s - uy * 0.05 * s)
    tri = sd_polygon(x, y, [tip, (back[0] + nx * 0.06 * s, back[1] + ny * 0.06 * s),
                            (back[0] - nx * 0.06 * s, back[1] - ny * 0.06 * s)])
    paint(cv, tri, hexc("#4a3818"))


def glyph_controls(cv):
    """Four key caps in the movement cluster, arrows engraved (Tuşlar)."""
    _keycap(cv, 0.5, 0.3, 1.45, "up")
    _keycap(cv, 0.24, 0.68, 1.45, "left")
    _keycap(cv, 0.5, 0.68, 1.45, "down")
    _keycap(cv, 0.76, 0.68, 1.45, "right")


def glyph_menu(cv):
    """Three gold bars with diamond ends (Menü)."""
    x, y = cv.x, cv.y
    parts = []
    for yy in (0.27, 0.5, 0.73):
        bar = sd_round_rect(x, y, 0.5, yy, 0.27, 0.05, 0.03)
        d0 = sd_polygon(x, y, [(0.15, yy - 0.09), (0.24, yy), (0.15, yy + 0.09), (0.06, yy)])
        d1 = sd_polygon(x, y, [(0.85, yy - 0.09), (0.94, yy), (0.85, yy + 0.09), (0.76, yy)])
        parts.append((bar, d0, d1, yy))
    ink(cv, union(*[union(b, d0, d1) for b, d0, d1, _ in parts]))
    for b, d0, d1, yy in parts:
        paint(cv, b, shade_y(cv, yy - 0.05, yy + 0.05, GOLD))
        paint(cv, union(d0, d1), shade_y(cv, yy - 0.09, yy + 0.09, ("#fff4c8", "#a87a2a")))


GLYPHS = {
    "attack": glyph_attack, "sit": glyph_sit, "loot": glyph_loot, "run": glyph_run, "walk": glyph_walk,
    "inventory": glyph_inventory, "kunye": glyph_kunye, "skills": glyph_skills, "controls": glyph_controls,
    "menu": glyph_menu,
}


def make_glyphs():
    vc.ensure_dir(HUD_DIR)
    out = {}
    for name, fn in GLYPHS.items():
        cv = Canvas(64)
        fn(cv)
        rgba = cv.result()
        vc.write_png(os.path.join(HUD_DIR, f"{name}.png"), vc.to_u8(rgba))
        out[name] = rgba
        print(f"ICON hud {name}: 64x64")
    return out


def sheet(glyphs, path):
    """Every glyph in the slot art at the HUD sizes (button px -> window = px * 52 / 64)."""
    bar_dir = os.path.join(vc.SRC, "ui", "skillbar")
    back = sa.load_png(os.path.join(bar_dir, "slot_back.png"))
    frame = sa.load_png(os.path.join(bar_dir, "slot_frame_normal.png"))
    hover = sa.load_png(os.path.join(bar_dir, "slot_frame_hover.png"))
    sizes = (60, 48, 40, 34, 28)
    pad = 12
    names = list(glyphs)
    W = pad + len(names) * (64 + pad)
    H = pad + sum(s + pad for s in sizes) + 64 + pad
    sh = sa.Sheet(W, H, bg=(26, 31, 38))
    y = pad
    for s in sizes:
        win = int(round(s * 52 / 64))
        off = (s - win) // 2
        for k, n in enumerate(names):
            x0 = pad + k * (64 + pad) + (64 - s) // 2
            sh.place(back, x0, y, s)
            sh.place(glyphs[n], x0 + off, y + off, win)
            sh.place(hover if s == 60 and k % 3 == 0 else frame, x0, y, s)
        y += s + pad
    for k, n in enumerate(names):
        sh.place(glyphs[n], pad + k * (64 + pad), y)
    sh.save(path)


def main():
    args = vc.script_args()
    sheets = DEFAULT_SHEETS
    if "--sheets" in args:
        sheets = args[args.index("--sheets") + 1]
    glyphs = make_glyphs()
    if "--no-preview" in args:
        return
    vc.ensure_dir(sheets)
    sheet(glyphs, os.path.join(sheets, "hud_glyphs.png"))


if __name__ == "__main__":
    main()
