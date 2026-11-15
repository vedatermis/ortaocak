// Heightmap terrain: chunked grid with per-vertex normals from the heightmap, 4-layer splat
// blending of tiling layer textures (each sampled at two scales to hide the repeat), shaded with the
// same lighting as meshes (World.hlsli shadeSurface: rough dielectric, no normal or ORM map), sun
// shadow and fog.
#include "World.hlsli"

Texture2D g_splat : register(t0);  // RGBA weights, linear
Texture2D g_layer0 : register(t1); // sRGB layer textures
Texture2D g_layer1 : register(t2);
Texture2D g_layer2 : register(t3);
Texture2D g_layer3 : register(t4);
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

float3 layer(Texture2D t, float2 xz, float tiling)
{
    float3 a = t.Sample(g_layerSampler, xz * tiling).rgb;
    float3 b = t.Sample(g_layerSampler, xz * (tiling * 0.237) + 0.31).rgb;
    return lerp(a, b, 0.35);
}

float4 PSMain(PSInput input) : SV_Target
{
    float2 xz = input.worldPos.xz;
    float4 w = g_splat.Sample(g_splatSampler, xz / g_terrainParams.x);
    w = max(w, 0.0);
    w /= max(dot(w, 1.0), 1e-4);
    float3 albedo = layer(g_layer0, xz, g_layerTiling.x) * w.x + layer(g_layer1, xz, g_layerTiling.y) * w.y +
                    layer(g_layer2, xz, g_layerTiling.z) * w.z + layer(g_layer3, xz, g_layerTiling.w) * w.w;
    float3 n = normalize(input.normal);
    Surface s;
    s.albedo = albedo;
    s.n = n;
    s.ng = n;
    s.rough = lerp(0.92, 0.75, w.z); // grass/dirt very rough, rock a little smoother
    s.metallic = 0.0;
    s.ao = 1.0;
    s.character = false;
    float shadow = sunShadow(input.worldPos, n);
    return float4(shadeSurface(s, input.worldPos, shadow) * exposure(), 1.0);
}

float4 VSShadow(VSInput input) : SV_Position
{
    return mul(g_shadowViewProj[shadowCascade()], float4(input.position, 1.0));
}
