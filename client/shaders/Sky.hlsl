// Procedural sky: a full-screen triangle at the far plane (reverse-Z depth 0, drawn after the opaque
// geometry with a GREATER_EQUAL read-only depth test so it only fills empty pixels). Gradient from
// the horizon (= fog colour, so distant terrain melts into it) to the zenith, plus a sun disk/glow.
// The same gradient (without the disk) is the environment the meshes reflect and the source of the
// SH ambient (World.hlsli envRadiance / evalSh, client/render/LightingMath.h), so they always match.
#include "World.hlsli"

struct PSInput
{
    float4 position : SV_Position;
    float2 ndc : TEXCOORD0;
};

PSInput VSMain(uint id : SV_VertexID)
{
    PSInput o;
    float2 uv = float2((id << 1) & 2, id & 2);
    o.ndc = uv * float2(2.0, -2.0) + float2(-1.0, 1.0);
    o.position = float4(o.ndc, 0.0, 1.0);
    return o;
}

float4 PSMain(PSInput input) : SV_Target
{
    float4 a = mul(g_invViewProj, float4(input.ndc, 1.0, 1.0));
    float4 b = mul(g_invViewProj, float4(input.ndc, 0.25, 1.0));
    float3 dir = normalize(b.xyz / b.w - a.xyz / a.w);
    float up = dir.y;
    float t = saturate(pow(saturate(up), 0.55));
    float3 sky = lerp(g_fogColor.rgb, g_zenithColor.rgb, t);
    // Slightly darker haze below the horizon line.
    sky = up < 0.0 ? lerp(g_fogColor.rgb, g_fogColor.rgb * 0.85, saturate(-up * 4.0)) : sky;
    float s = saturate(dot(dir, g_sunDir.xyz));
    float3 sunTint = g_sunColor.rgb / max(max(g_sunColor.r, g_sunColor.g), max(g_sunColor.b, 1e-3));
    // HDR: the disk is properly bright (the tone map rolls it off), the glow stays subtle.
    sky += sunTint * (pow(s, 600.0) * 12.0 + pow(s, 24.0) * 0.18);
    return float4(sky * exposure(), 1.0);
}
