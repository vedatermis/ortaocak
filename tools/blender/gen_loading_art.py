"""Loading-screen backgrounds, one per zone: an establishing shot of the zone rendered in Cycles from
its own committed terrain and props (gen_zone_shots.load_zone / build_scene / place_veils), with a
per-zone sky, sun and point lights, a distant procedural mountain ring beyond the 512 m map, and a
numpy post step (depth haze, glow, colour grade, vignette). Nothing downloaded, nothing external.

  zone1.png  Ortaocak   afternoon on the plaza beside the cracked Ant Taşı, the village and ridges behind
  zone2.png  Tanyeri    dawn: pale gold light low from the east on the white keep tower, chalk escarpments
  zone3.png  Demirağız  blue hour high in the snowy pass, the town under the cliff, doors and braziers glowing

Usage (headless; reads only assets_src/, writes assets_src/ui/loading/zone<N>.png, 1920x1080 RGB):
  blender --background --factory-startup --python tools/blender/gen_loading_art.py -- [--only 1,3]
          [--samples 96] [--scale 50] [--out <dir>]
--scale renders at that percentage (framing tests); --out writes elsewhere (default assets_src/ui/loading).
The client shows the image cover-fit with a dark text box at ~58 % height, centred: the compositions
keep that band calm and the landmarks in the upper part / on the thirds.
"""

import math
import os
import sys
import time

import bpy
import numpy as np
from mathutils import Vector

sys.dont_write_bytecode = True
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import vo_common as vc  # noqa: E402
import gen_zone_shots as zs  # noqa: E402  (its main() only runs as a script)

W, H = 1920, 1080


def hexf(h):
    return np.array(vc.hex_linear(h))


ZONES = {
    1: dict(
        name="Ortaocak",
        # engine (x, metres above ground, z) -> target (x, metres above ground, z)
        eye=(240.5, 2.2, 250.0), target=(311.2, 7.4, 320.7), lens=35.0,
        sun_az=238.0, sun_el=26.0, sun_col="#ffcf8a", sun_strength=7.0, sun_angle=2.0,
        sky=("#e6d6b4", "#8fbde0", "#3f78b8"), sky_glow="#ffd49a", sky_strength=2.4, ground="#55634a",
        ring=dict(r0=520.0, height=210.0, low="#5b6d74", high="#9aa7b4", snow=None, seed=11),
        haze=("#a8c2de", "#cfd6d0"), haze_dist=220.0, haze_max=0.7, haze_h=(20.0, 70.0),
        emission=2.0, lights=[("lamp_post", 3.2, 0.0, "#ffb060", 25.0)],
        grade=dict(lift=(0.02, 0.025, 0.035), gain=(1.03, 1.0, 0.95), gamma=1.0, sat=1.2,
                   shadow_tint=(-0.01, 0.0, 0.02), high_tint=(0.02, 0.01, -0.02)),
        bloom=(0.85, 0.25), vignette=0.32, sun_flare=0.0,
        clouds=dict(col="#fff3e2", cover=0.5, scale=2.2, amount=0.85, seed=1.0),
    ),
    2: dict(
        name="Tanyeri",
        eye=(212.5, 9.0, 297.5), target=(299.1, 14.0, 347.5), lens=30.0,
        sun_az=100.0, sun_el=6.0, sun_col="#ffc98a", sun_strength=4.8, sun_angle=1.5,
        sky=("#ffd9a0", "#dcdde2", "#7fa3d4"), sky_glow="#fff0c8", sky_strength=2.6, ground="#a08b62",
        ring=dict(r0=520.0, height=120.0, low="#b8a68a", high="#d9cbb3", snow=None, seed=23),
        haze=("#f1d9b0", "#ffe7bf"), haze_dist=330.0, haze_max=0.62, haze_h=(14.0, 60.0),
        emission=3.0, lights=[("lamp_post", 3.2, 0.0, "#ffb868", 12.0)],
        grade=dict(lift=(0.03, 0.02, 0.03), gain=(1.05, 1.0, 0.9), gamma=1.0, sat=1.05,
                   shadow_tint=(0.0, -0.005, 0.02), high_tint=(0.03, 0.015, -0.03)),
        bloom=(0.8, 0.35), vignette=0.28, sun_flare=0.55, exposure=0.5,
        clouds=dict(col="#fff1d6", cover=0.4, scale=1.6, amount=1.0, seed=2.0),
    ),
    3: dict(
        name="Demirağız",
        eye=(256.0, 38.0, 170.0), target=(256.0, 8.4, 270.0), lens=15.0, fill=(165.0, 28.0, "#8aa4e0", 3.5),
        sun_az=245.0, sun_el=3.0, sun_col="#ff9e7a", sun_strength=3.0, sun_angle=1.0,
        sky=("#c98f86", "#41557e", "#141c33"), sky_glow="#ff9f7c", sky_strength=6.0, ground="#20252f",
        ring=dict(r0=520.0, height=330.0, low="#2b313d", high="#59627a", snow="#c9d4e6", seed=37),
        haze=("#36456a", "#59607e"), haze_dist=380.0, haze_max=0.62, haze_h=(40.0, 120.0),
        emission=14.0, lights=[("kargan_brazier", 1.6, 0.0, "#ff7a2a", 450.0),
                               ("kargan_door", 3.4, 2.2, "#ff7a2a", 260.0),
                               ("kargan_forge", 1.8, 1.5, "#ff6a20", 220.0),
                               ("kargan_hut", 1.4, 3.8, "#ffa050", 40.0)],
        grade=dict(lift=(0.01, 0.015, 0.04), gain=(1.02, 0.98, 1.0), gamma=1.0, sat=1.1,
                   shadow_tint=(-0.01, 0.0, 0.03), high_tint=(0.04, 0.0, -0.03)),
        bloom=(0.45, 0.7), vignette=0.38, sun_flare=0.0, exposure=1.6,
        clouds=dict(col="#6f5f86", cover=0.55, scale=1.8, amount=0.8, seed=3.0),
    ),
}


# ---------------------------------------------------------------------------------------------
# Scene pieces

def eng_dir(az_deg, el_deg):
    """Engine compass direction (azimuth from north towards east, elevation) -> Blender vector."""
    az, el = math.radians(az_deg), math.radians(el_deg)
    ex, ey, ez = math.sin(az) * math.cos(el), math.sin(el), math.cos(az) * math.cos(el)
    return Vector((-ex, -ez, ey))


def splat_terrain_material(desc, zone):
    """Replaces build_scene's baked 4 px/m albedo with the layers tiled at their real size, blended
    by the splat (the foreground needs the detail)."""
    terrain = bpy.data.objects["terrain"]
    mat = bpy.data.materials.new("terrain_splat")
    mat.use_nodes = True
    nt = mat.node_tree
    bs = nt.nodes["Principled BSDF"]
    bs.inputs["Roughness"].default_value = 0.95
    uv = nt.nodes.new("ShaderNodeTexCoord")
    d = os.path.join(vc.SRC, "terrain", f"zone{zone}")
    sp = nt.nodes.new("ShaderNodeTexImage")
    sp.image = bpy.data.images.load(os.path.join(d, desc["terrain"]["splat"]), check_existing=False)
    sp.image.colorspace_settings.name = 'Non-Color'
    sp.image.alpha_mode = 'CHANNEL_PACKED'
    sp.interpolation = 'Linear'
    nt.links.new(uv.outputs["UV"], sp.inputs["Vector"])
    sep = nt.nodes.new("ShaderNodeSeparateColor")
    nt.links.new(sp.outputs["Color"], sep.inputs["Color"])
    weights = [sep.outputs[0], sep.outputs[1], sep.outputs[2], sp.outputs["Alpha"]]
    acc = None
    for k, L in enumerate(desc["layer"]):
        mp = nt.nodes.new("ShaderNodeMapping")
        # Box (triplanar) projection in metres: the cliffs and escarpments get real rock instead of the
        # top-down UV smeared down their faces.
        s = 1.0 / (L["tile_size"] * (2.5 if k == 2 else 1.0))  # rock (B) larger: it is seen from afar
        mp.inputs["Scale"].default_value = (s, s, s)
        nt.links.new(uv.outputs["Object"], mp.inputs["Vector"])
        tx = nt.nodes.new("ShaderNodeTexImage")
        tx.projection = 'BOX'
        tx.projection_blend = 0.35
        tx.image = bpy.data.images.load(os.path.join(vc.SRC, *L["texture"].split("/")), check_existing=True)
        nt.links.new(mp.outputs["Vector"], tx.inputs["Vector"])
        mul = nt.nodes.new("ShaderNodeMix")
        mul.data_type = 'RGBA'
        mul.blend_type = 'MULTIPLY'
        mul.inputs["Factor"].default_value = 1.0
        nt.links.new(tx.outputs["Color"], mul.inputs["A"])
        nt.links.new(weights[k], mul.inputs["B"])
        if acc is None:
            acc = mul.outputs["Result"]
        else:
            add = nt.nodes.new("ShaderNodeMix")
            add.data_type = 'RGBA'
            add.blend_type = 'ADD'
            add.inputs["Factor"].default_value = 1.0
            nt.links.new(acc, add.inputs["A"])
            nt.links.new(mul.outputs["Result"], add.inputs["B"])
            acc = add.outputs["Result"]
    # Large-scale light / dark variation so the tiling does not show at distance.
    nz = nt.nodes.new("ShaderNodeTexNoise")
    nz.inputs["Scale"].default_value = 1.0 / 45.0
    nz.inputs["Detail"].default_value = 4.0
    nt.links.new(uv.outputs["Object"], nz.inputs["Vector"])
    mr = nt.nodes.new("ShaderNodeMapRange")
    mr.inputs["From Min"].default_value = 0.3
    mr.inputs["From Max"].default_value = 0.7
    mr.inputs["To Min"].default_value = 0.66
    mr.inputs["To Max"].default_value = 1.2
    nt.links.new(nz.outputs["Fac"], mr.inputs["Value"])
    var = nt.nodes.new("ShaderNodeMix")
    var.data_type = 'RGBA'
    var.blend_type = 'MULTIPLY'
    var.inputs["Factor"].default_value = 1.0
    nt.links.new(acc, var.inputs["A"])
    nt.links.new(mr.outputs["Result"], var.inputs["B"])
    nt.links.new(var.outputs["Result"], bs.inputs["Base Color"])
    terrain.data.materials[0] = mat


def value_noise(rng, x, y, cell):
    """Smooth value noise sampled at world points (x, y) with lattice spacing `cell`."""
    n = 64
    g = rng.random((n, n))
    fx, fy = x / cell, y / cell
    i, j = np.floor(fx).astype(int), np.floor(fy).astype(int)
    tx, ty = fx - i, fy - j
    tx, ty = tx * tx * (3 - 2 * tx), ty * ty * (3 - 2 * ty)
    i0, j0, i1, j1 = i % n, j % n, (i + 1) % n, (j + 1) % n
    a = g[j0, i0] * (1 - tx) + g[j0, i1] * tx
    b = g[j1, i0] * (1 - tx) + g[j1, i1] * tx
    return a * (1 - ty) + b * ty


def mountain_ring(cfg, ground_z):
    """A ring of hazy procedural ranges 0.5 - 3 km out so no shot ends at the map edge."""
    rng = np.random.default_rng(cfg["seed"])
    na, nr = 720, 40
    th = np.linspace(0.0, 2 * math.pi, na, endpoint=False)
    rr = cfg["r0"] + (np.linspace(0.0, 1.0, nr) ** 1.6) * 2600.0
    T, R = np.meshgrid(th, rr)
    X, Y = np.cos(T) * R, np.sin(T) * R
    h = np.zeros_like(X)
    amp, cell = 1.0, 700.0
    for _ in range(5):
        n = value_noise(rng, X + 5000, Y + 5000, cell)
        h += amp * (1.0 - np.abs(2.0 * n - 1.0)) ** 2  # ridged
        amp *= 0.5
        cell *= 0.45
    h = h / 1.4
    rise = np.clip((R - cfg["r0"]) / 450.0, 0, 1)
    rise = rise * rise * (3 - 2 * rise)
    Z = ground_z - 30.0 + cfg["height"] * h * rise * (0.55 + 0.45 * np.clip(R / 1800.0, 0, 1))
    cx, cy = -256.0, -256.0
    verts = [(cx + X[r, a], cy + Y[r, a], Z[r, a]) for r in range(nr) for a in range(na)]
    faces = [(r * na + a, r * na + (a + 1) % na, (r + 1) * na + (a + 1) % na, (r + 1) * na + a)
             for r in range(nr - 1) for a in range(na)]
    me = bpy.data.meshes.new("ring")
    me.from_pydata(verts, [], faces)
    me.shade_smooth()
    mat = bpy.data.materials.new("ring")
    mat.use_nodes = True
    nt = mat.node_tree
    bs = nt.nodes["Principled BSDF"]
    bs.inputs["Roughness"].default_value = 0.9
    geo = nt.nodes.new("ShaderNodeNewGeometry")
    sxyz = nt.nodes.new("ShaderNodeSeparateXYZ")
    nt.links.new(geo.outputs["Position"], sxyz.inputs["Vector"])
    mr = nt.nodes.new("ShaderNodeMapRange")
    mr.inputs["From Min"].default_value = ground_z
    mr.inputs["From Max"].default_value = ground_z + cfg["height"] * 0.9
    nt.links.new(sxyz.outputs["Z"], mr.inputs["Value"])
    ramp = nt.nodes.new("ShaderNodeValToRGB")
    el = ramp.color_ramp.elements
    el[0].color = tuple(hexf(cfg["low"])) + (1.0,)
    el[1].position = 0.6
    el[1].color = tuple(hexf(cfg["high"])) + (1.0,)
    if cfg["snow"]:
        e = el.new(0.68)
        e.color = tuple(hexf(cfg["snow"])) + (1.0,)
        el[1].position = 0.55
    nt.links.new(mr.outputs["Result"], ramp.inputs["Fac"])
    nt.links.new(ramp.outputs["Color"], bs.inputs["Base Color"])
    me.materials.append(mat)
    vc.link(bpy.data.objects.new("ring", me))


def make_world(cfg, sun_vec):
    world = bpy.data.worlds.new("LoadingSky")
    world.use_nodes = True
    nt = world.node_tree
    bg = nt.nodes["Background"]
    tc = nt.nodes.new("ShaderNodeTexCoord")
    nrm = nt.nodes.new("ShaderNodeVectorMath")
    nrm.operation = 'NORMALIZE'
    nt.links.new(tc.outputs["Generated"], nrm.inputs[0])
    sxyz = nt.nodes.new("ShaderNodeSeparateXYZ")
    nt.links.new(nrm.outputs["Vector"], sxyz.inputs["Vector"])
    mr = nt.nodes.new("ShaderNodeMapRange")
    mr.inputs["From Min"].default_value = -0.03
    mr.inputs["From Max"].default_value = 0.75
    nt.links.new(sxyz.outputs["Z"], mr.inputs["Value"])
    ramp = nt.nodes.new("ShaderNodeValToRGB")
    el = ramp.color_ramp.elements
    horizon, mid, zenith = cfg["sky"]
    el[0].color = tuple(hexf(cfg["ground"])) + (1.0,)
    el[0].position = 0.0
    el[1].color = tuple(hexf(zenith)) + (1.0,)
    e = el.new(0.045)
    e.color = tuple(hexf(horizon)) + (1.0,)
    e = el.new(0.3)
    e.color = tuple(hexf(mid)) + (1.0,)
    e = el.new(0.032)
    e.color = tuple(hexf(horizon) * 0.9) + (1.0,)
    nt.links.new(mr.outputs["Result"], ramp.inputs["Fac"])
    dot = nt.nodes.new("ShaderNodeVectorMath")
    dot.operation = 'DOT_PRODUCT'
    dot.inputs[1].default_value = tuple(sun_vec)
    nt.links.new(nrm.outputs["Vector"], dot.inputs[0])
    mx = nt.nodes.new("ShaderNodeMath")
    mx.operation = 'MAXIMUM'
    mx.inputs[1].default_value = 0.0
    nt.links.new(dot.outputs["Value"], mx.inputs[0])
    glows = []
    for p, k in ((6.0, 0.55), (60.0, 1.2), (900.0, 6.0)):
        pw = nt.nodes.new("ShaderNodeMath")
        pw.operation = 'POWER'
        pw.inputs[1].default_value = p
        nt.links.new(mx.outputs["Value"], pw.inputs[0])
        ml = nt.nodes.new("ShaderNodeMath")
        ml.operation = 'MULTIPLY'
        ml.inputs[1].default_value = k
        nt.links.new(pw.outputs["Value"], ml.inputs[0])
        glows.append(ml.outputs["Value"])
    s1 = nt.nodes.new("ShaderNodeMath")
    nt.links.new(glows[0], s1.inputs[0])
    nt.links.new(glows[1], s1.inputs[1])
    s2 = nt.nodes.new("ShaderNodeMath")
    nt.links.new(s1.outputs["Value"], s2.inputs[0])
    nt.links.new(glows[2], s2.inputs[1])
    # The glow only above the horizon.
    above = nt.nodes.new("ShaderNodeMath")
    above.operation = 'GREATER_THAN'
    above.inputs[1].default_value = -0.01
    nt.links.new(sxyz.outputs["Z"], above.inputs[0])
    s3 = nt.nodes.new("ShaderNodeMath")
    s3.operation = 'MULTIPLY'
    nt.links.new(s2.outputs["Value"], s3.inputs[0])
    nt.links.new(above.outputs["Value"], s3.inputs[1])
    mix = nt.nodes.new("ShaderNodeMix")
    mix.data_type = 'RGBA'
    mix.blend_type = 'ADD'
    nt.links.new(s3.outputs["Value"], mix.inputs["Factor"])
    nt.links.new(ramp.outputs["Color"], mix.inputs["A"])
    mix.inputs["B"].default_value = tuple(hexf(cfg["sky_glow"])) + (1.0,)
    sky_out = mix.outputs["Result"]
    cl = cfg.get("clouds")
    if cl:
        # Soft cloud banks: noise on a plane above the camera (dir.xy / dir.z), fading at the horizon.
        dz = nt.nodes.new("ShaderNodeMath")
        dz.inputs[1].default_value = 0.12
        nt.links.new(sxyz.outputs["Z"], dz.inputs[0])
        px = nt.nodes.new("ShaderNodeMath")
        px.operation = 'DIVIDE'
        nt.links.new(sxyz.outputs["X"], px.inputs[0])
        nt.links.new(dz.outputs["Value"], px.inputs[1])
        py = nt.nodes.new("ShaderNodeMath")
        py.operation = 'DIVIDE'
        nt.links.new(sxyz.outputs["Y"], py.inputs[0])
        nt.links.new(dz.outputs["Value"], py.inputs[1])
        cmb = nt.nodes.new("ShaderNodeCombineXYZ")
        nt.links.new(px.outputs["Value"], cmb.inputs["X"])
        nt.links.new(py.outputs["Value"], cmb.inputs["Y"])
        cmb.inputs["Z"].default_value = cl["seed"]
        nz = nt.nodes.new("ShaderNodeTexNoise")
        nz.noise_dimensions = '3D'
        nz.inputs["Scale"].default_value = cl["scale"]
        nz.inputs["Detail"].default_value = 8.0
        nz.inputs["Roughness"].default_value = 0.58
        nt.links.new(cmb.outputs["Vector"], nz.inputs["Vector"])
        cov = nt.nodes.new("ShaderNodeMapRange")
        cov.inputs["From Min"].default_value = cl["cover"]
        cov.inputs["From Max"].default_value = cl["cover"] + 0.22
        nt.links.new(nz.outputs["Fac"], cov.inputs["Value"])
        fade = nt.nodes.new("ShaderNodeMapRange")
        fade.inputs["From Min"].default_value = 0.0
        fade.inputs["From Max"].default_value = 0.22
        nt.links.new(sxyz.outputs["Z"], fade.inputs["Value"])
        f1 = nt.nodes.new("ShaderNodeMath")
        f1.operation = 'MULTIPLY'
        nt.links.new(cov.outputs["Result"], f1.inputs[0])
        nt.links.new(fade.outputs["Result"], f1.inputs[1])
        f2 = nt.nodes.new("ShaderNodeMath")
        f2.operation = 'MULTIPLY'
        f2.inputs[1].default_value = cl["amount"]
        nt.links.new(f1.outputs["Value"], f2.inputs[0])
        ccol = nt.nodes.new("ShaderNodeMix")  # clouds catch the glow near the sun
        ccol.data_type = 'RGBA'
        ccol.blend_type = 'ADD'
        nt.links.new(s3.outputs["Value"], ccol.inputs["Factor"])
        ccol.inputs["A"].default_value = tuple(hexf(cl["col"])) + (1.0,)
        ccol.inputs["B"].default_value = tuple(hexf(cfg["sky_glow"])) + (1.0,)
        cm = nt.nodes.new("ShaderNodeMix")
        cm.data_type = 'RGBA'
        nt.links.new(f2.outputs["Value"], cm.inputs["Factor"])
        nt.links.new(sky_out, cm.inputs["A"])
        nt.links.new(ccol.outputs["Result"], cm.inputs["B"])
        sky_out = cm.outputs["Result"]
    nt.links.new(sky_out, bg.inputs["Color"])
    bg.inputs["Strength"].default_value = cfg["sky_strength"]
    bpy.context.scene.world = world
    return world


def boost_emission(k):
    for m in bpy.data.materials:
        if not m.use_nodes:
            continue
        for nd in m.node_tree.nodes:
            if nd.type == 'BSDF_PRINCIPLED':
                ec = nd.inputs["Emission Color"]
                lit = ec.is_linked or max(ec.default_value[:3]) > 0.0
                if lit:
                    nd.inputs["Emission Strength"].default_value *= k


def add_lights(desc, heights, lights):
    for i, (prop, up, fwd, col, watts) in enumerate(lights):
        for o in desc["object"]:
            if not o["mesh"].endswith(f"/{prop}.vmesh"):
                continue
            yaw = o.get("yaw", 0.0)
            x = o["x"] + math.sin(yaw) * fwd
            z = o["z"] + math.cos(yaw) * fwd
            y = zs.bilinear(heights, x, z) + up
            ld = bpy.data.lights.new(f"pt_{prop}", 'POINT')
            ld.energy = watts
            ld.color = tuple(hexf(col) / max(hexf(col)))
            ld.shadow_soft_size = 0.35
            ob = vc.link(bpy.data.objects.new(ld.name, ld))
            ob.location = (-x, -z, y)


def setup_render(samples, scale, exposure=0.0):
    sc = bpy.context.scene
    sc.render.engine = 'CYCLES'
    sc.cycles.device = 'CPU'
    sc.cycles.samples = samples
    sc.cycles.use_adaptive_sampling = True
    sc.cycles.adaptive_threshold = 0.02
    sc.cycles.seed = 7
    sc.cycles.use_denoising = True
    sc.cycles.max_bounces = 4
    sc.cycles.diffuse_bounces = 2
    sc.cycles.glossy_bounces = 1
    sc.cycles.transparent_max_bounces = 8
    sc.cycles.use_auto_tile = True
    sc.render.threads_mode = 'AUTO'
    sc.render.resolution_x, sc.render.resolution_y = W, H
    sc.render.resolution_percentage = scale
    sc.render.film_transparent = False
    sc.render.dither_intensity = 0.0
    sc.view_settings.view_transform = 'AgX'
    sc.view_settings.look = 'None'
    sc.view_settings.exposure = exposure


def render_png16(path):
    sc = bpy.context.scene
    st = sc.render.image_settings
    st.file_format = 'PNG'
    st.color_mode = 'RGB'
    st.color_depth = '16'
    sc.render.filepath = path
    bpy.ops.render.render(write_still=True)
    img = vc.read_image(path)[..., :3]
    os.remove(path)
    return img


def render_haze(path, cfg, eye_bl):
    """A second, cheap render: per pixel the haze amount (R: distance, G: height) and coverage (A)."""
    sc = bpy.context.scene
    vl = sc.view_layers[0]
    mat = bpy.data.materials.new("haze_probe")
    mat.use_nodes = True
    nt = mat.node_tree
    for nd in list(nt.nodes):
        nt.nodes.remove(nd)
    out = nt.nodes.new("ShaderNodeOutputMaterial")
    em = nt.nodes.new("ShaderNodeEmission")
    cam = nt.nodes.new("ShaderNodeCameraData")
    geo = nt.nodes.new("ShaderNodeNewGeometry")
    sxyz = nt.nodes.new("ShaderNodeSeparateXYZ")
    nt.links.new(geo.outputs["Position"], sxyz.inputs["Vector"])
    # R = 1 - exp(-d / dist)
    m1 = nt.nodes.new("ShaderNodeMath")
    m1.operation = 'MULTIPLY'
    m1.inputs[1].default_value = -1.0 / cfg["haze_dist"]
    nt.links.new(cam.outputs["View Distance"], m1.inputs[0])
    ex = nt.nodes.new("ShaderNodeMath")
    ex.operation = 'EXPONENT'
    nt.links.new(m1.outputs["Value"], ex.inputs[0])
    r = nt.nodes.new("ShaderNodeMath")
    r.operation = 'SUBTRACT'
    r.inputs[0].default_value = 1.0
    nt.links.new(ex.outputs["Value"], r.inputs[1])
    # G = height above the camera's ground, normalised to the fade band
    h0, h1 = cfg["haze_h"]
    mr = nt.nodes.new("ShaderNodeMapRange")
    mr.inputs["From Min"].default_value = eye_bl[2] - 10.0 + h0
    mr.inputs["From Max"].default_value = eye_bl[2] - 10.0 + h1 * 3.0
    nt.links.new(sxyz.outputs["Z"], mr.inputs["Value"])
    comb = nt.nodes.new("ShaderNodeCombineColor")
    nt.links.new(r.outputs["Value"], comb.inputs[0])
    nt.links.new(mr.outputs["Result"], comb.inputs[1])
    nt.links.new(comb.outputs["Color"], em.inputs["Color"])
    nt.links.new(em.outputs["Emission"], out.inputs["Surface"])
    old_world = sc.world
    w = bpy.data.worlds.new("black")
    w.use_nodes = True
    w.node_tree.nodes["Background"].inputs["Strength"].default_value = 0.0
    sc.world = w
    vl.material_override = mat
    lights = [o for o in sc.objects if o.type == 'LIGHT']
    for o in lights:
        o.hide_render = True
    samples, den, vt = sc.cycles.samples, sc.cycles.use_denoising, sc.view_settings.view_transform
    sc.cycles.samples, sc.cycles.use_denoising = 8, False
    sc.cycles.use_adaptive_sampling = False
    sc.render.film_transparent = True
    sc.view_settings.view_transform = 'Standard'
    st = sc.render.image_settings
    st.file_format = 'OPEN_EXR'
    st.color_mode = 'RGBA'
    st.color_depth = '32'
    sc.render.filepath = path
    bpy.ops.render.render(write_still=True)
    probe = vc.read_image(path)
    os.remove(path)
    vl.material_override = None
    sc.world = old_world
    for o in lights:
        o.hide_render = False
    sc.cycles.samples, sc.cycles.use_denoising, sc.view_settings.view_transform = samples, den, vt
    sc.cycles.use_adaptive_sampling = True
    sc.render.film_transparent = False
    return probe


# ---------------------------------------------------------------------------------------------
# Post (display-space floats, top row first)

def box_blur(a, r):
    if r < 1:
        return a
    out = a
    for axis in (0, 1):
        c = np.cumsum(np.pad(out, [(r + 1, r) if ax == axis else (0, 0) for ax in range(out.ndim)], mode='edge'),
                      axis=axis, dtype=np.float64)
        n = out.shape[axis]
        hi = np.take(c, np.arange(2 * r + 1, 2 * r + 1 + n), axis=axis)
        lo = np.take(c, np.arange(0, n), axis=axis)
        out = ((hi - lo) / (2 * r + 1)).astype(np.float32)
    return out


def gauss(a, r):
    for _ in range(3):
        a = box_blur(a, max(1, int(r / 1.7)))
    return a


def post(img, probe, cfg, sun_px):
    h, w = img.shape[:2]
    s = w / 1920.0
    yy, xx = np.mgrid[0:h, 0:w].astype(np.float32)
    v, u = yy / (h - 1), xx / (w - 1)
    # Depth haze (towards a top-to-bottom gradient of the zone's haze colour), less with height.
    hz_top = np.array([int(cfg["haze"][0][i:i + 2], 16) for i in (1, 3, 5)], np.float32) / 255.0
    hz_bot = np.array([int(cfg["haze"][1][i:i + 2], 16) for i in (1, 3, 5)], np.float32) / 255.0
    hcol = hz_top[None, None, :] * (1 - v[..., None]) + hz_bot[None, None, :] * v[..., None]
    dist = probe[..., 0]
    height = np.clip(probe[..., 1], 0, 1)
    cover = np.clip(probe[..., 3], 0, 1)
    a = cfg["haze_max"] * dist * (1.0 - 0.7 * height) * cover
    img = img * (1 - a[..., None]) + hcol * a[..., None]
    # Sun bloom (screen-space, only when the sun is near the frame).
    if cfg["sun_flare"] > 0 and sun_px is not None:
        sx, sy = sun_px
        d = np.hypot(xx - sx, yy - sy) / (w * 0.5)
        sc = np.array([int(cfg["sky_glow"][i:i + 2], 16) for i in (1, 3, 5)], np.float32) / 255.0
        g = cfg["sun_flare"] * (np.exp(-d * 3.5) * 0.6 + np.exp(-d * 12.0) * 0.6)
        img = 1 - (1 - img) * (1 - (g[..., None] * sc[None, None, :]))
    # Glow round the bright bits (embers, windows, the sky at the sun).
    thr, k = cfg["bloom"]
    br = np.clip(img - thr, 0, None) / (1 - thr)
    bl = gauss(br, 18 * s) * 0.6 + gauss(br, 60 * s) * 0.4
    img = img + k * bl
    # Grade: lift / gain / gamma, saturation, split toning.
    g = cfg["grade"]
    img = np.array(g["lift"])[None, None, :] + img * (np.array(g["gain"]) - np.array(g["lift"]))[None, None, :]
    img = np.clip(img, 0, None) ** (1.0 / g["gamma"])
    lum = (img * np.array([0.2126, 0.7152, 0.0722], np.float32)).sum(-1, keepdims=True)
    img = lum + (img - lum) * g["sat"]
    lum = np.clip(lum, 0, 1)
    img = img + np.array(g["shadow_tint"])[None, None, :] * (1 - lum) ** 2 + \
        np.array(g["high_tint"])[None, None, :] * lum ** 2
    # Calm the band the loading text sits on: soften a little (no hard box, the client draws one).
    band = np.exp(-(((u - 0.5) / 0.26) ** 4) - (((v - 0.6) / 0.12) ** 4))[..., None]
    soft = gauss(img, 6 * s)
    img = img * (1 - 0.55 * band) + soft * (0.55 * band)
    img = img * (1 - 0.1 * band)
    # Vignette.
    r2 = ((u - 0.5) / 0.75) ** 2 + ((v - 0.47) / 0.62) ** 2
    vig = 1.0 - cfg["vignette"] * np.clip(r2, 0, 1.6) ** 1.4
    img = img * vig[..., None]
    return np.clip(img, 0, 1)


# ---------------------------------------------------------------------------------------------

def render_zone(zone, out_dir, samples, scale):
    cfg = ZONES[zone]
    t0 = time.time()
    desc, heights, splat = zs.load_zone(zone)
    zs.build_scene(zone, desc, heights, splat)
    zs.place_veils(desc, heights)
    splat_terrain_material(desc, zone)
    sun_vec = eng_dir(cfg["sun_az"], cfg["sun_el"])
    mountain_ring(cfg["ring"], float(np.median(heights[:8, :])))
    make_world(cfg, sun_vec)
    sun = bpy.data.lights.new("Sun", 'SUN')
    sun.energy = cfg["sun_strength"]
    sun.angle = math.radians(cfg["sun_angle"])
    sun.color = tuple(hexf(cfg["sun_col"]) / max(hexf(cfg["sun_col"])))
    so = vc.link(bpy.data.objects.new("Sun", sun))
    so.rotation_euler = sun_vec.to_track_quat('Z', 'Y').to_euler()
    if "fill" in cfg:  # a second, cool sky light (blue hour: the open sky over the pass)
        az, el, col, strength = cfg["fill"]
        fl = bpy.data.lights.new("Fill", 'SUN')
        fl.energy = strength
        fl.angle = math.radians(20.0)
        fl.color = tuple(hexf(col) / max(hexf(col)))
        fo = vc.link(bpy.data.objects.new("Fill", fl))
        fo.rotation_euler = eng_dir(az, el).to_track_quat('Z', 'Y').to_euler()
    boost_emission(cfg["emission"])
    add_lights(desc, heights, cfg["lights"])
    camd = bpy.data.cameras.new("Cam")
    cam = vc.link(bpy.data.objects.new("Cam", camd))
    bpy.context.scene.camera = cam
    ex, ey, ez = cfg["eye"]
    tx, ty, tz = cfg["target"]
    eye = (-ex, -ez, zs.bilinear(heights, ex, ez) + ey)
    tgt = (-tx, -tz, zs.bilinear(heights, tx, tz) + ty)
    vc.aim_camera(cam, eye, tgt, lens=cfg["lens"])
    camd.clip_end = 8000.0
    camd.sensor_width = 36.0
    setup_render(samples, scale, cfg.get("exposure", 0.0))
    bpy.context.view_layer.update()
    # Sun position on screen (for the flare), if in front of the camera.
    from bpy_extras.object_utils import world_to_camera_view
    p = world_to_camera_view(bpy.context.scene, cam, Vector(eye) + sun_vec * 5000.0)
    rw, rh = W * scale // 100, H * scale // 100
    sun_px = (p.x * rw, (1 - p.y) * rh) if p.z > 0 else None
    t1 = time.time()
    tmp = os.path.join(out_dir, f"_zone{zone}")
    img = render_png16(tmp + "_beauty.png")
    t2 = time.time()
    probe = render_haze(tmp + "_haze.exr", cfg, Vector(eye))
    t3 = time.time()
    out = post(img, probe, cfg, sun_px)
    path = os.path.join(out_dir, f"zone{zone}.png")
    vc.write_png(path, vc.to_u8(out), optimize=False)
    print(f"[loading] zone{zone} {cfg['name']}: build {t1 - t0:.0f}s beauty {t2 - t1:.0f}s haze {t3 - t2:.0f}s "
          f"post {time.time() - t3:.0f}s -> {path} ({os.path.getsize(path) / 1e6:.2f} MB)")


def main():
    args = vc.script_args()
    out = args[args.index("--out") + 1] if "--out" in args else "assets_src/ui/loading"
    out_dir = vc.ensure_dir(out if os.path.isabs(out) else os.path.join(vc.REPO, *out.split("/")))
    zones = [int(a) for a in args[args.index("--only") + 1].split(",")] if "--only" in args else [1, 2, 3]
    samples = int(args[args.index("--samples") + 1]) if "--samples" in args else 96
    scale = int(args[args.index("--scale") + 1]) if "--scale" in args else 100
    for zone in zones:
        render_zone(zone, out_dir, samples, scale)


if __name__ == "__main__":
    main()
