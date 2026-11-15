// Meshes: static props (instanced, world matrix per instance) and skinned characters (bone palette
// in a constant buffer, up to 4 influences). Shared pixel shader: albedo = vertex colour (sRGB
// decoded) x base colour x optional texture, Lambert sun + hemispheric ambient + shadow + fog.
#include "World.hlsli"

cbuffer DrawConstants : register(b1)
{
    float4x4 g_world;    // skinned: model -> world
    float4 g_baseColor;  // linear RGBA (submesh factor x per-instance tint)
    float4 g_drawParams; // x = textured (0/1), y = fade-in alpha (unused for opaque), zw unused
};

cbuffer BoneConstants : register(b2)
{
    float4x4 g_bones[64]; // skin[b] = model[b] * inverseBind[b]
};

Texture2D g_albedo : register(t0);
SamplerState g_albedoSampler : register(s0);

struct StaticVSInput
{
    float3 position : POSITION;
    float3 normal : NORMAL;
    float2 uv : TEXCOORD0;
    float4 color : COLOR0;
    float4 row0 : INSTANCE0; // world matrix rows (affine)
    float4 row1 : INSTANCE1;
    float4 row2 : INSTANCE2;
};

struct SkinnedVSInput
{
    float3 position : POSITION;
    float3 normal : NORMAL;
    float2 uv : TEXCOORD0;
    float4 color : COLOR0;
    uint4 joints : BLENDINDICES;
    float4 weights : BLENDWEIGHT;
};

struct PSInput
{
    float4 position : SV_Position;
    float3 worldPos : TEXCOORD0;
    float3 normal : TEXCOORD1;
    float2 uv : TEXCOORD2;
    float4 color : COLOR0; // linear
};

float3 instanceTransform(float4 r0, float4 r1, float4 r2, float4 p)
{
    return float3(dot(r0, p), dot(r1, p), dot(r2, p));
}

PSInput VSStatic(StaticVSInput input)
{
    PSInput o;
    float3 wp = instanceTransform(input.row0, input.row1, input.row2, float4(input.position, 1.0));
    float3 wn = instanceTransform(input.row0, input.row1, input.row2, float4(input.normal, 0.0));
    o.position = mul(g_viewProj, float4(wp, 1.0));
    o.worldPos = wp;
    o.normal = normalize(wn);
    o.uv = input.uv;
    o.color = float4(srgbToLinear(input.color.rgb), input.color.a);
    return o;
}

float4x4 skinMatrix(uint4 j, float4 w)
{
    return g_bones[j.x] * w.x + g_bones[j.y] * w.y + g_bones[j.z] * w.z + g_bones[j.w] * w.w;
}

PSInput VSSkinned(SkinnedVSInput input)
{
    PSInput o;
    float4x4 skin = skinMatrix(input.joints, input.weights);
    float4 mp = mul(skin, float4(input.position, 1.0));
    float3 mn = mul((float3x3)skin, input.normal);
    float4 wp = mul(g_world, mp);
    float3 wn = mul((float3x3)g_world, mn);
    o.position = mul(g_viewProj, wp);
    o.worldPos = wp.xyz;
    o.normal = normalize(wn);
    o.uv = input.uv;
    o.color = float4(srgbToLinear(input.color.rgb), input.color.a);
    return o;
}

float4 PSMain(PSInput input, bool frontFace : SV_IsFrontFace) : SV_Target
{
    float4 tex = g_albedo.Sample(g_albedoSampler, input.uv);
    float3 albedo = input.color.rgb * g_baseColor.rgb;
    if (g_drawParams.x > 0.5)
        albedo *= tex.rgb;
    float3 n = normalize(input.normal);
    if (!frontFace)
        n = -n; // double-sided submeshes
    float shadow = sunShadow(input.worldPos, n);
    return float4(shade(albedo, n, input.worldPos, shadow), 1.0);
}

// ---- Shadow map (depth only) ----

struct ShadowOutput
{
    float4 position : SV_Position;
};

ShadowOutput VSStaticShadow(StaticVSInput input)
{
    ShadowOutput o;
    float3 wp = instanceTransform(input.row0, input.row1, input.row2, float4(input.position, 1.0));
    o.position = mul(g_shadowViewProj, float4(wp, 1.0));
    return o;
}

ShadowOutput VSSkinnedShadow(SkinnedVSInput input)
{
    ShadowOutput o;
    float4x4 skin = skinMatrix(input.joints, input.weights);
    float4 wp = mul(g_world, mul(skin, float4(input.position, 1.0)));
    o.position = mul(g_shadowViewProj, wp);
    return o;
}
