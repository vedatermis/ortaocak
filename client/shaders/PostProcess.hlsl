// HDR scene -> sRGB back buffer: tone map (ACES fitted, Stephen Hill's RRT+ODT fit; or a plain
// clamp) and, for a multisampled scene, the MSAA resolve. The resolve happens after the tone map:
// each sample is tone-mapped and the display-referred values are averaged, so an edge between a
// very bright highlight and a dark background is antialiased like an LDR edge (averaging HDR first
// lets one 50x-bright sample dominate the pixel and the edge looks aliased). The render target view
// is sRGB: the hardware encodes on write. Exposure is already applied by the scene shaders.

cbuffer PostConstants : register(b0)
{
    float4 g_post; // x sample count, y tone map (0 ACES fitted, 1 clamp), zw unused
};

Texture2DMS<float4> g_sceneMs : register(t0);
Texture2D<float4> g_scene : register(t1);

struct PSInput
{
    float4 position : SV_Position;
};

PSInput VSMain(uint id : SV_VertexID)
{
    PSInput o;
    float2 uv = float2((id << 1) & 2, id & 2);
    o.position = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    return o;
}

static const float3x3 kAcesIn = {0.59719, 0.35458, 0.04823, 0.07600, 0.90834, 0.01566, 0.02840, 0.13383, 0.83777};
static const float3x3 kAcesOut = {1.60475, -0.53108, -0.07367, -0.10208, 1.10813, -0.00605, -0.00327, -0.07276, 1.07602};

float3 rrtOdtFit(float3 v)
{
    float3 a = v * (v + 0.0245786) - 0.000090537;
    float3 b = v * (0.983729 * v + 0.4329510) + 0.238081;
    return a / b;
}

float3 toneMap(float3 c)
{
    c = max(c, 0.0);
    if (g_post.y > 0.5)
        return saturate(c);
    return saturate(mul(kAcesOut, rrtOdtFit(mul(kAcesIn, c))));
}

float4 PSResolveMs(PSInput input) : SV_Target
{
    int2 p = int2(input.position.xy);
    int n = (int)g_post.x;
    float3 sum = 0.0;
    for (int i = 0; i < n; ++i)
        sum += toneMap(g_sceneMs.Load(p, i).rgb);
    return float4(sum / n, 1.0);
}

float4 PSResolve(PSInput input) : SV_Target
{
    return float4(toneMap(g_scene.Load(int3(int2(input.position.xy), 0)).rgb), 1.0);
}
