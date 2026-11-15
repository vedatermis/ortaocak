// Shared declarations and the lighting model of the 3D shaders (docs/CLIENT_RENDERING.md §4).
//
// Matrices arrive in the glTF / AssetMath layout (column-major storage, column vectors) and HLSL's
// default column_major packing, so transforms are `mul(M, float4(p, 1))`.
// Depth is reverse-Z (near = 1, far = 0). Lighting is linear HDR: shaders write radiance x exposure
// into a float target; PostProcess.hlsl tone-maps (ACES fitted) into the sRGB back buffer.
// Vertex colours are sRGB-encoded and decoded here.
//
// Units (client/render/LightingMath.h has the CPU twins): light colours are irradiance at normal
// incidence with pi folded in (diffuse BRDF = albedo, specular = pi * D * Vis * F); sky colours are
// displayed radiance in the same units.

cbuffer FrameConstants : register(b0)
{
    float4x4 g_viewProj;
    float4x4 g_invViewProj;
    float4x4 g_shadowViewProj[2]; // world -> shadow clip per cascade (standard depth, 0..1)
    float4 g_cameraPos;           // xyz eye, w time (s)
    float4 g_sunDir;              // xyz unit vector towards the sun (key light), w unused
    float4 g_sunColor;            // rgb
    float4 g_fogColor;            // rgb = horizon colour, w = fog density (1/m)
    float4 g_fogParams;           // x fog start (m), y debug view (0 off, 1 lighting, 2 normals), z shadows on (0/1), w cascade count
    float4 g_zenithColor;         // rgb, w = exposure
    float4 g_groundColor;         // rgb radiance of the ground (environment below the horizon), w ambient scale
    float4 g_terrainParams;       // x world size
    float4 g_layerTiling;         // 1 / tile size of the 4 terrain layers
    float4 g_shadowParams[2];     // per cascade: x texel (uv), y texel size (m), z depth bias, w unused
    float4 g_sh[9];               // L2 SH of the sky irradiance (dot(basis(n), sh) = displayed ambient)
    float4 g_lightDir[3];         // extra lights (fill/rim): xyz towards the light, w = 1: characters only
    float4 g_lightColor[3];       // rgb (0 = off)
    float4 g_features;            // x normal maps, y specular, z extra lights, w shadow filter (0 hard, 1 PCF)
    float4 g_layerParams[4];      // terrain layer: x normal strength, y AO strength, z height-blend weight, w triplanar (0/1)
    float4 g_terrainShading;      // x blend depth (m-ish), y detail fade start (m), z detail fade range (m), w macro variation
};

cbuffer PassConstants : register(b3)
{
    float4 g_pass; // x = shadow cascade being rendered (shadow pass)
};

int shadowCascade()
{
    return (int)g_pass.x;
}

Texture2DArray<float> g_shadowMap : register(t8);
SamplerComparisonState g_shadowSampler : register(s2);

static const float kPi = 3.14159265;

float3 srgbToLinear(float3 c)
{
    float3 lo = c / 12.92;
    float3 hi = pow(abs((c + 0.055) / 1.055), 2.4);
    return lerp(hi, lo, step(c, 0.04045));
}

float exposure()
{
    return g_zenithColor.w;
}

// ---- Sun shadow: 2 cascades in a texture array, 4x4-tap bilinear PCF, normal offset ----

float2 shadowUv(float3 p, int cascade, out float depth)
{
    float4 c = mul(g_shadowViewProj[cascade], float4(p, 1.0));
    depth = c.z / c.w;
    return float2(c.x * 0.5 + 0.5, 0.5 - c.y * 0.5);
}

float cascadeShadow(int cascade, float3 worldPos, float3 ng, float nDotL)
{
    // Normal offset grows with the texel size and the slope against the light (no constant bias,
    // which is what lets thin geometry peter-pan).
    float texelWorld = g_shadowParams[cascade].y;
    float3 p = worldPos + ng * texelWorld * (0.7 + 1.8 * saturate(1.0 - nDotL));
    float depth;
    float2 uv = shadowUv(p, cascade, depth);
    depth -= g_shadowParams[cascade].z;
    float texel = g_shadowParams[cascade].x;
    float lit = 0.0;
    [branch] if (g_features.w < 0.5)
    {
        lit = g_shadowMap.SampleCmpLevelZero(g_shadowSampler, float3(uv, cascade), depth);
    }
    else
    {
        [unroll] for (int y = 0; y < 4; ++y)
        {
            [unroll] for (int x = 0; x < 4; ++x)
            {
                float2 o = (float2(x, y) - 1.5) * texel;
                lit += g_shadowMap.SampleCmpLevelZero(g_shadowSampler, float3(uv + o, cascade), depth);
            }
        }
        lit /= 16.0;
    }
    return lit;
}

// 0 = fully shadowed, 1 = lit. `ng` is the geometric (vertex) normal.
float sunShadow(float3 worldPos, float3 ng)
{
    if (g_fogParams.z < 0.5)
        return 1.0;
    float nDotL = dot(ng, g_sunDir.xyz);
    int cascades = (int)g_fogParams.w;
    float d0;
    float2 uv0 = shadowUv(worldPos, 0, d0);
    float edge0 = max(abs(uv0.x - 0.5), abs(uv0.y - 0.5)) * 2.0; // 0 centre .. 1 border
    if (cascades < 2)
    {
        float lit = cascadeShadow(0, worldPos, ng, nDotL);
        return lerp(lit, 1.0, saturate((edge0 - 0.8) / 0.2));
    }
    if (edge0 < 0.8)
        return cascadeShadow(0, worldPos, ng, nDotL);
    float d1;
    float2 uv1 = shadowUv(worldPos, 1, d1);
    float edge1 = max(abs(uv1.x - 0.5), abs(uv1.y - 0.5)) * 2.0;
    float far = lerp(cascadeShadow(1, worldPos, ng, nDotL), 1.0, saturate((edge1 - 0.8) / 0.2));
    if (edge0 >= 0.95)
        return far;
    return lerp(cascadeShadow(0, worldPos, ng, nDotL), far, (edge0 - 0.8) / 0.15);
}

float3 applyFog(float3 color, float3 worldPos)
{
    float3 toPoint = worldPos - g_cameraPos.xyz;
    float dist = length(toPoint);
    float d = max(dist - g_fogParams.x, 0.0);
    float f = 1.0 - exp(-(d * g_fogColor.w) * (d * g_fogColor.w));
    // Haze brightens towards the sun like the sky near the horizon (Sky.hlsl), so far terrain melts
    // into the sky in every direction.
    float s = saturate(dot(toPoint / max(dist, 1e-3), g_sunDir.xyz));
    float3 sunTint = g_sunColor.rgb / max(max(g_sunColor.r, g_sunColor.g), max(g_sunColor.b, 1e-3));
    float3 haze = g_fogColor.rgb + sunTint * (pow(s, 6.0) * 0.16);
    return lerp(color, haze, saturate(f));
}

// ---- Image-free environment ----

float3 evalSh(float3 n)
{
    float3 r = g_sh[0].rgb * 0.282095;
    r += g_sh[1].rgb * (0.488603 * n.y) + g_sh[2].rgb * (0.488603 * n.z) + g_sh[3].rgb * (0.488603 * n.x);
    r += g_sh[4].rgb * (1.092548 * n.x * n.y) + g_sh[5].rgb * (1.092548 * n.y * n.z);
    r += g_sh[6].rgb * (0.315392 * (3.0 * n.z * n.z - 1.0)) + g_sh[7].rgb * (1.092548 * n.x * n.z);
    r += g_sh[8].rgb * (0.546274 * (n.x * n.x - n.y * n.y));
    return max(r, 0.0);
}

// Prefiltered radiance of the procedural sky/ground environment in direction r for a GGX lobe of
// perceptual roughness `rough` (split-sum, analytic): the sharp gradient with a horizon softened by
// the lobe width, fading into the cosine-blurred SH irradiance as the lobe widens.
float3 envRadiance(float3 r, float rough)
{
    float a = rough * rough;
    float w = 0.02 + a * 1.5;
    float t = saturate(pow(saturate(r.y), 0.55));
    float3 sky = lerp(g_fogColor.rgb, g_zenithColor.rgb, t) * g_groundColor.w; // w = ambient scale
    float3 sharp = lerp(g_groundColor.rgb, sky, smoothstep(-w, w, r.y));
    return lerp(sharp, evalSh(r), saturate(a * 1.6));
}

// Karis' analytic split-sum environment BRDF: env * (F0 * x + y).
float2 envBrdfApprox(float rough, float nDotV)
{
    const float4 c0 = float4(-1.0, -0.0275, -0.572, 0.022);
    const float4 c1 = float4(1.0, 0.0425, 1.04, -0.04);
    float4 r = rough * c0 + c1;
    float a004 = min(r.x * r.x, exp2(-9.28 * nDotV)) * r.x + r.y;
    return float2(-1.04, 1.04) * a004 + r.zw;
}

// ---- BRDF ----

float ggxD(float nDotH, float alpha)
{
    float a2 = alpha * alpha;
    float d = nDotH * nDotH * (a2 - 1.0) + 1.0;
    return a2 / (kPi * d * d);
}

float smithVisibility(float nDotV, float nDotL, float alpha)
{
    float a2 = alpha * alpha;
    float gv = nDotL * sqrt(nDotV * nDotV * (1.0 - a2) + a2);
    float gl = nDotV * sqrt(nDotL * nDotL * (1.0 - a2) + a2);
    return 0.5 / max(gv + gl, 1e-5);
}

float3 fresnelSchlick(float3 f0, float vDotH)
{
    return f0 + (1.0 - f0) * pow(1.0 - saturate(vDotH), 5.0);
}

float burleyDiffuse(float nDotV, float nDotL, float lDotH, float rough)
{
    float fd90 = 0.5 + 2.0 * rough * lDotH * lDotH;
    float ls = 1.0 + (fd90 - 1.0) * pow(1.0 - nDotL, 5.0);
    float vs = 1.0 + (fd90 - 1.0) * pow(1.0 - nDotV, 5.0);
    return ls * vs;
}

struct Surface
{
    float3 albedo;   // linear base colour
    float3 n;        // shading normal (normal-mapped)
    float3 ng;       // geometric (interpolated vertex) normal
    float rough;     // perceptual roughness
    float metallic;
    float ao;        // material occlusion (ORM.r), applies to the ambient terms
    bool character;  // receives the character-only extra lights (rim)
    float3 emissive; // emitted radiance, added after the lighting (fogged like the rest)
};

float3 directLight(Surface s, float3 v, float nDotV, float3 l, float3 radiance, float3 f0, float3 diffColor, float alpha)
{
    float nDotL = saturate(dot(s.n, l));
    if (nDotL <= 0.0)
        return 0.0;
    float3 h = normalize(l + v);
    float nDotH = saturate(dot(s.n, h));
    float lDotH = saturate(dot(l, h));
    float3 diffuse = diffColor * burleyDiffuse(nDotV, nDotL, lDotH, s.rough);
    float3 spec = (kPi * ggxD(nDotH, alpha) * smithVisibility(nDotV, nDotL, alpha)) * fresnelSchlick(f0, lDotH);
    return (diffuse + spec * g_features.y) * radiance * nDotL;
}

// Full lighting of an opaque surface: sun (shadowed) + extra lights + SH diffuse ambient + split-sum
// specular ambient, then fog. Result is not yet multiplied by the exposure.
float3 shadeSurface(Surface s, float3 worldPos, float shadow)
{
    float3 v = normalize(g_cameraPos.xyz - worldPos);
    // Back-facing shading normals (normal maps at silhouettes) are bent towards the viewer.
    float nDotVRaw = dot(s.n, v);
    if (nDotVRaw < 0.0)
        s.n = normalize(s.n - v * (nDotVRaw * 1.01));
    float nDotV = max(dot(s.n, v), 1e-4);
    float rough = clamp(s.rough, 0.045, 1.0);
    // Specular anti-aliasing: widen the lobe where the geometric normal varies within the pixel.
    float3 dn = abs(ddx(s.ng)) + abs(ddy(s.ng));
    float alpha = rough * rough;
    alpha = sqrt(saturate(alpha * alpha + min(0.5 * dot(dn, dn), 0.18)));
    float3 f0 = lerp(0.04, s.albedo, s.metallic);
    float3 diffColor = s.albedo * (1.0 - s.metallic);

    float3 color = directLight(s, v, nDotV, g_sunDir.xyz, g_sunColor.rgb * shadow, f0, diffColor, alpha);
    if (g_features.z > 0.5)
    {
        [unroll] for (int i = 0; i < 3; ++i)
        {
            if (g_lightDir[i].w > 0.5 && !s.character)
                continue;
            color += directLight(s, v, nDotV, g_lightDir[i].xyz, g_lightColor[i].rgb, f0, diffColor, alpha);
        }
    }

    // Ambient: SH irradiance for diffuse, analytic prefiltered environment for specular.
    color += diffColor * evalSh(s.n) * s.ao;
    if (g_features.y > 0.5)
    {
        float3 r = reflect(-v, s.n);
        float2 ab = envBrdfApprox(rough, nDotV);
        // Specular occlusion from the material AO (Lagarde) and a horizon term for normal-mapped
        // reflections that would point into the surface.
        float specAo = saturate(pow(abs(nDotV + s.ao), exp2(-16.0 * rough - 1.0)) - 1.0 + s.ao);
        float horizon = saturate(1.0 + 1.2 * dot(r, s.ng));
        color += envRadiance(r, rough) * (f0 * ab.x + ab.y) * specAo * horizon * horizon;
    }
    color += s.emissive;
    return applyFog(color, worldPos);
}
