// Meshes: static props (instanced, world matrix per instance) and skinned characters (bone palette
// in a constant buffer, up to 4 influences; positions, normals and tangents are skinned). Shared PBR
// pixel shader (World.hlsli shadeSurface): base colour = vertex colour (sRGB decoded) x base colour x
// optional texture; tangent-space normal map (BC5 RG, Z reconstructed, OpenGL +Y up); ORM map
// (R occlusion, G roughness, B metallic) x factors.
#include "World.hlsli"

cbuffer DrawConstants : register(b1)
{
    float4x4 g_world;    // skinned: model -> world
    float4 g_baseColor;  // linear RGBA (submesh factor x per-instance tint)
    float4 g_drawParams; // x textured (0/1), y opacity (< 1: screen-door dither), z character (0/1), w ORM map (0/1)
    float4 g_material;   // x normal scale (0 = no normal map), y roughness factor, z metallic factor, w occlusion strength
    float4 g_normalFlip; // x = +1, or -1 to flip the normal map's green (graphics normal_flip_y, debugging)
};

cbuffer BoneConstants : register(b2)
{
    float4x4 g_bones[64]; // skin[b] = model[b] * inverseBind[b]
};

Texture2D g_albedo : register(t0);
Texture2D g_normalMap : register(t1);
Texture2D g_ormMap : register(t2);
SamplerState g_materialSampler : register(s3); // anisotropic, clamp

struct StaticVSInput
{
    float3 position : POSITION;
    float3 normal : NORMAL;
    float2 uv : TEXCOORD0;
    float4 color : COLOR0;
    float4 tangent : TANGENT; // snorm16: xyz, w = handedness; (0,0,0,0) when the mesh has none
    float4 row0 : INSTANCE0;  // world matrix rows (affine)
    float4 row1 : INSTANCE1;
    float4 row2 : INSTANCE2;
};

struct SkinnedVSInput
{
    float3 position : POSITION;
    float3 normal : NORMAL;
    float2 uv : TEXCOORD0;
    float4 color : COLOR0;
    float4 tangent : TANGENT;
    uint4 joints : BLENDINDICES;
    float4 weights : BLENDWEIGHT;
};

struct PSInput
{
    float4 position : SV_Position;
    float3 worldPos : TEXCOORD0;
    float3 normal : TEXCOORD1;
    float2 uv : TEXCOORD2;
    float4 tangent : TEXCOORD3; // world xyz, w = handedness
    float4 color : COLOR0;      // linear
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
    float3 wt = instanceTransform(input.row0, input.row1, input.row2, float4(input.tangent.xyz, 0.0));
    o.position = mul(g_viewProj, float4(wp, 1.0));
    o.worldPos = wp;
    o.normal = normalize(wn);
    o.tangent = float4(wt, input.tangent.w);
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
    float3 mt = mul((float3x3)skin, input.tangent.xyz);
    float4 wp = mul(g_world, mp);
    float3 wn = mul((float3x3)g_world, mn);
    float3 wt = mul((float3x3)g_world, mt);
    o.position = mul(g_viewProj, wp);
    o.worldPos = wp.xyz;
    o.normal = normalize(wn);
    o.tangent = float4(wt, input.tangent.w);
    o.uv = input.uv;
    o.color = float4(srgbToLinear(input.color.rgb), input.color.a);
    return o;
}

// Ordered 4x4 Bayer thresholds in (0, 1): a pixel survives while opacity is above its threshold.
static const float kBayer4[16] = {0.5, 8.5, 2.5, 10.5, 12.5, 4.5, 14.5, 6.5, 3.5, 11.5, 1.5, 9.5, 15.5, 7.5, 13.5, 5.5};

// Tangent-space normal from a two-channel texel (shared/assets/NormalMapping.h decodeNormalTexel).
float3 decodeNormalMap(float2 rg, float scale)
{
    float2 xy = rg * 2.0 - 1.0;
    float z = sqrt(saturate(1.0 - dot(xy, xy)));
    return normalize(float3(xy * scale, z));
}

float4 PSMain(PSInput input, bool frontFace : SV_IsFrontFace) : SV_Target
{
    if (g_drawParams.y < 1.0)
    {
        uint2 p = uint2(input.position.xy) & 3u;
        if (g_drawParams.y * 16.0 < kBayer4[p.y * 4u + p.x])
            discard;
    }
    float3 albedo = input.color.rgb * g_baseColor.rgb;
    if (g_drawParams.x > 0.5)
        albedo *= g_albedo.Sample(g_materialSampler, input.uv).rgb;

    float3 ng = normalize(input.normal);
    float3 t = input.tangent.xyz;
    float handed = input.tangent.w < 0.0 ? -1.0 : 1.0;
    if (!frontFace)
    {
        // Double-sided back face: the whole tangent frame is mirrored (glTF rule).
        ng = -ng;
        t = -t;
        handed = -handed;
    }
    float3 n = ng;
    if (g_material.x > 0.0 && g_features.x > 0.5 && dot(t, t) > 1e-6)
    {
        // Gram-Schmidt (shared/assets/NormalMapping.h perturbNormal): bitangent points image-up.
        t = normalize(t - ng * dot(ng, t));
        float3 b = cross(ng, t) * handed;
        float3 ts = decodeNormalMap(g_normalMap.Sample(g_materialSampler, input.uv).rg, g_material.x);
        ts.y *= g_normalFlip.x;
        n = normalize(t * ts.x + b * ts.y + ng * ts.z);
    }
    float3 orm = g_drawParams.w > 0.5 ? g_ormMap.Sample(g_materialSampler, input.uv).rgb : float3(1.0, 1.0, 1.0);

    Surface s;
    s.albedo = albedo;
    s.n = n;
    s.ng = ng;
    s.rough = saturate(orm.g * g_material.y);
    s.metallic = saturate(orm.b * g_material.z);
    s.ao = lerp(1.0, orm.r, g_material.w);
    s.character = g_drawParams.z > 0.5;
    // Debug views (graphics debug_view): 1 = lighting only (grey dielectric, normal map kept),
    // 2 = shading normal as colour.
    if (g_fogParams.y > 1.5)
        return float4(n * 0.5 + 0.5, 1.0);
    if (g_fogParams.y > 0.5)
    {
        s.albedo = 0.5;
        s.metallic = 0.0;
        s.ao = 1.0;
        s.rough = 0.6;
    }
    float shadow = sunShadow(input.worldPos, ng);
    return float4(shadeSurface(s, input.worldPos, shadow) * exposure(), 1.0);
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
    o.position = mul(g_shadowViewProj[shadowCascade()], float4(wp, 1.0));
    return o;
}

ShadowOutput VSSkinnedShadow(SkinnedVSInput input)
{
    ShadowOutput o;
    float4x4 skin = skinMatrix(input.joints, input.weights);
    float4 wp = mul(g_world, mul(skin, float4(input.position, 1.0)));
    o.position = mul(g_shadowViewProj[shadowCascade()], wp);
    return o;
}
