// Heightmap terrain (docs/CLIENT_RENDERING.md §4 "Terrain"): chunked grid with per-vertex normals
// from the heightmap; four PBR layers (base colour + tangent-space normal + ORM each), blended by the
// splat weights, shaded with the same lighting as meshes (World.hlsli shadeSurface), sun shadow, fog.
//
// Per layer:
//   * planar xz projection at the layer's tile size, plus a second sample at ~3.7x the tile size,
//     rotated and offset, cross-faded by a world-space value noise so neither repeat reads as a grid;
//     beyond the detail-fade distance only the large sample remains (distant slopes do not tile);
//   * layers flagged triplanar (rock) are projected along x, y and z and blended by the normal, so
//     cliffs do not stretch;
//   * a low-frequency macro variation (brightness / warmth over 30-90 m) breaks up large areas.
// Layers are blended by splat weight and height (the ORM occlusion is the height proxy: pebbles and
// rock bumps stand up, cracks sink) over a narrow transition depth, so road edges and rock/grass
// borders follow the shapes in the textures instead of the 1 m splat cells.
#include "World.hlsli"

Texture2D g_splat : register(t0);     // RGBA weights, linear
Texture2D g_albedo[4] : register(t1); // sRGB base colour (t1..t4)
Texture2D g_normal[4] : register(t9); // BC5 normal maps, OpenGL +Y = image up (t9..t12; t8 is the shadow map)
Texture2D g_orm[4] : register(t13);   // R occlusion, G roughness, B metallic (t13..t16)
SamplerState g_splatSampler : register(s0); // linear clamp
SamplerState g_layerSampler : register(s1); // anisotropic wrap

struct VSInput
{
    float3 position : POSITION;
    float3 normal : NORMAL;
};

struct PSInput
{
    float4 position : SV_Position;
    float3 worldPos : TEXCOORD0;
    float3 normal : TEXCOORD1;
};

PSInput VSMain(VSInput input)
{
    PSInput o;
    o.position = mul(g_viewProj, float4(input.position, 1.0));
    o.worldPos = input.position;
    o.normal = input.normal;
    return o;
}

// ---- Noise (world space, no texture) ----

float hash12(float2 p)
{
    float3 p3 = frac(float3(p.xyx) * 0.1031);
    p3 += dot(p3, p3.yzx + 33.33);
    return frac((p3.x + p3.y) * p3.z);
}

float valueNoise(float2 p)
{
    float2 i = floor(p);
    float2 f = frac(p);
    float2 u = f * f * (3.0 - 2.0 * f);
    float a = hash12(i), b = hash12(i + float2(1, 0)), c = hash12(i + float2(0, 1)), d = hash12(i + float2(1, 1));
    return lerp(lerp(a, b, u.x), lerp(c, d, u.x), u.y);
}

// ---- One layer sample: albedo (linear), tangent-space normal, ORM ----

struct LayerSample
{
    float3 albedo;
    float3 n; // world-space normal
    float3 orm;
};

float3 unpackNormal(float2 rg, float strength)
{
    float2 xy = (rg * 2.0 - 1.0) * strength;
    return float3(xy, sqrt(saturate(1.0 - dot(xy, xy))));
}

// Planar sample in the xz plane with a tangent frame (T = +x = +u, image up = -z = -v).
LayerSample samplePlanar(int i, float2 uv, float3 ng, float strength, float2 uDir)
{
    LayerSample s;
    s.albedo = g_albedo[i].Sample(g_layerSampler, uv).rgb;
    s.orm = g_orm[i].Sample(g_layerSampler, uv).rgb;
    float3 ts = unpackNormal(g_normal[i].Sample(g_layerSampler, uv).rg, strength);
    // uDir = direction of increasing u in xz; v grows along (-uDir.y, uDir.x), image up is against it.
    float3 tu = float3(uDir.x, 0, uDir.y);
    float3 up = float3(uDir.y, 0, -uDir.x);
    float3 t = normalize(tu - ng * dot(ng, tu));
    float3 b = normalize(up - ng * dot(ng, up));
    s.n = normalize(t * ts.x + b * ts.y + ng * ts.z);
    return s;
}

LayerSample blendSample(LayerSample a, LayerSample b, float t)
{
    LayerSample s;
    s.albedo = lerp(a.albedo, b.albedo, t);
    s.orm = lerp(a.orm, b.orm, t);
    s.n = normalize(lerp(a.n, b.n, t));
    return s;
}

static const float2x2 kRot = float2x2(0.7986, -0.6018, 0.6018, 0.7986); // 37 degrees

// Two scales cross-faded by noise; the fine one fades out with distance.
LayerSample sampleLayerPlanar(int i, float2 xz, float tiling, float3 ng, float strength, float mixNoise, float detail)
{
    LayerSample fine = samplePlanar(i, xz * tiling, ng, strength, float2(1, 0));
    LayerSample coarse = samplePlanar(i, mul(kRot, xz) * (tiling * 0.27) + float2(0.37, 0.71), ng, strength,
                                      float2(0.7986, -0.6018));
    // Near: mostly fine with patches of the coarse one; far: coarse only.
    float t = lerp(1.0, saturate(mixNoise * 1.6 - 0.35), detail);
    return blendSample(fine, coarse, t);
}

// Triplanar sample (cliffs): three planar projections blended by the geometric normal.
LayerSample sampleLayerTriplanar(int i, float3 p, float tiling, float3 ng, float strength, float mixNoise,
                                 float detail)
{
    float3 w = pow(abs(ng), 4.0);
    w /= dot(w, 1.0);
    LayerSample s;
    s.albedo = 0.0;
    s.orm = 0.0;
    float3 n = 0.0;
    // A fixed scale: a scale that varied with distance would make the coordinates a function of the
    // distance (smeared bands along iso-distance lines). The coarse repeat is mixed in afterwards.
    float scale = tiling;
    // X projection: u = z, image up = +y.
    {
        float2 uv = float2(p.z * sign(ng.x), -p.y) * scale + 0.13;
        float3 ts = unpackNormal(g_normal[i].Sample(g_layerSampler, uv).rg, strength);
        s.albedo += g_albedo[i].Sample(g_layerSampler, uv).rgb * w.x;
        s.orm += g_orm[i].Sample(g_layerSampler, uv).rgb * w.x;
        n += (float3(0, ts.y, ts.x * sign(ng.x)) + float3(ng.x, 0, 0) * ts.z) * w.x;
    }
    // Y projection (flat ground): u = x, image up = -z.
    {
        float2 uv = p.xz * scale;
        float3 ts = unpackNormal(g_normal[i].Sample(g_layerSampler, uv).rg, strength);
        s.albedo += g_albedo[i].Sample(g_layerSampler, uv).rgb * w.y;
        s.orm += g_orm[i].Sample(g_layerSampler, uv).rgb * w.y;
        n += (float3(ts.x, 0, -ts.y) + float3(0, ng.y, 0) * ts.z) * w.y;
    }
    // Z projection: u = -x for +z faces (mirror kept consistent with the normal), image up = +y.
    {
        float2 uv = float2(-p.x * sign(ng.z), -p.y) * scale + 0.57;
        float3 ts = unpackNormal(g_normal[i].Sample(g_layerSampler, uv).rg, strength);
        s.albedo += g_albedo[i].Sample(g_layerSampler, uv).rgb * w.z;
        s.orm += g_orm[i].Sample(g_layerSampler, uv).rgb * w.z;
        n += (float3(-ts.x * sign(ng.z), ts.y, 0) + float3(0, 0, ng.z) * ts.z) * w.z;
    }
    s.n = normalize(n);
    // Coarse base colour (~2.7x the tile) over patches near by and everywhere far away.
    float c = tiling * 0.37;
    float3 coarse = g_albedo[i].Sample(g_layerSampler, float2(p.z * sign(ng.x), -p.y) * c + 0.71).rgb * w.x +
                    g_albedo[i].Sample(g_layerSampler, p.xz * c + 0.29).rgb * w.y +
                    g_albedo[i].Sample(g_layerSampler, float2(-p.x * sign(ng.z), -p.y) * c + 0.43).rgb * w.z;
    s.albedo = lerp(s.albedo, coarse, lerp(0.75, saturate(mixNoise * 1.6 - 0.35), detail));
    return s;
}

float4 PSMain(PSInput input) : SV_Target
{
    float2 xz = input.worldPos.xz;
    float3 ng = normalize(input.normal);
    float4 w = g_splat.Sample(g_splatSampler, xz / g_terrainParams.x);
    w = max(w, 0.0);
    w /= max(dot(w, 1.0), 1e-4);

    float dist = length(g_cameraPos.xyz - input.worldPos);
    float detail = 1.0 - saturate((dist - g_terrainShading.y) / g_terrainShading.z);
    float mixNoise = valueNoise(xz * 0.11) * 0.65 + valueNoise(xz * 0.37 + 17.0) * 0.35;
    float strength = g_features.x > 0.5 ? 1.0 : 0.0;
    float layerTiling[4] = {g_layerTiling.x, g_layerTiling.y, g_layerTiling.z, g_layerTiling.w};

    // Sample the layers that contribute (weights are smooth; a small threshold skips most work).
    LayerSample L[4];
    float h[4];
    [unroll] for (int i = 0; i < 4; ++i)
    {
        L[i].albedo = 0.0;
        L[i].orm = float3(1.0, 1.0, 0.0);
        L[i].n = ng;
        h[i] = 0.0;
        float wi = i == 0 ? w.x : (i == 1 ? w.y : (i == 2 ? w.z : w.w));
        [branch] if (wi > 0.002)
        {
            float4 lp = g_layerParams[i];
            if (lp.w > 0.5)
                L[i] = sampleLayerTriplanar(i, input.worldPos, layerTiling[i], ng, strength * lp.x, mixNoise, detail);
            else
                L[i] = sampleLayerPlanar(i, xz, layerTiling[i], ng, strength * lp.x, mixNoise, detail);
            h[i] = L[i].orm.r * lp.z;
        }
    }
    // Height blend: weight x height, keep only what is within `depth` of the highest.
    float wv[4] = {w.x, w.y, w.z, w.w};
    float m = -1e9;
    [unroll] for (int j = 0; j < 4; ++j)
        m = max(m, wv[j] > 0.002 ? wv[j] + h[j] : -1e9);
    float depth = g_terrainShading.x;
    float total = 0.0;
    float bw[4];
    [unroll] for (int k = 0; k < 4; ++k)
    {
        bw[k] = wv[k] > 0.002 ? max(wv[k] + h[k] - (m - depth), 0.0) : 0.0;
        total += bw[k];
    }
    float3 albedo = 0.0, n = 0.0, orm = 0.0;
    float ao = 0.0;
    [unroll] for (int q = 0; q < 4; ++q)
    {
        float f = bw[q] / max(total, 1e-5);
        albedo += L[q].albedo * f;
        n += L[q].n * f;
        orm += L[q].orm * f;
        ao += lerp(1.0, L[q].orm.r, g_layerParams[q].y) * f;
    }
    n = normalize(n);
    // Macro variation: slow brightness and warmth changes (30-90 m) so meadows are not one flat tone.
    float macro = valueNoise(xz * 0.013) * 0.6 + valueNoise(xz * 0.031 + 5.0) * 0.4;
    float mv = g_terrainShading.w;
    albedo *= lerp(1.0 - mv, 1.0 + mv, macro) * lerp(float3(1.02, 1.0, 0.95), float3(0.97, 1.0, 1.04), macro);

    Surface s;
    s.albedo = albedo;
    s.n = g_features.x > 0.5 ? n : ng;
    s.ng = ng;
    s.rough = clamp(orm.g, 0.35, 1.0);
    s.metallic = 0.0;
    s.ao = ao;
    s.character = false;
    s.emissive = 0.0;
    if (g_fogParams.y > 1.5)
        return float4(s.n * 0.5 + 0.5, 1.0);
    if (g_fogParams.y > 0.5)
    {
        s.albedo = 0.5;
        s.ao = 1.0;
        s.rough = 0.6;
    }
    float shadow = sunShadow(input.worldPos, ng);
    return float4(shadeSurface(s, input.worldPos, shadow) * exposure(), 1.0);
}

float4 VSShadow(VSInput input) : SV_Position
{
    return mul(g_shadowViewProj[shadowCascade()], float4(input.position, 1.0));
}
