// Ground decals: soft blob shadows under characters and the click-to-move destination ring. A quad
// lying on the terrain plane at the decal centre (lifted a few cm), premultiplied-alpha output,
// depth-tested read-only so props in front still hide it.
#include "World.hlsli"

cbuffer DecalConstants : register(b1)
{
    float4 g_center; // xyz, w = radius (m)
    float4 g_axisU;  // xyz unit tangent on the ground plane
    float4 g_axisV;  // xyz unit bitangent
    float4 g_color;  // linear rgb, a = opacity
    float4 g_mode;   // x: 0 = blob, 1 = ring; y = ring phase (animation)
};

struct PSInput
{
    float4 position : SV_Position;
    float2 local : TEXCOORD0; // -1..1
    float3 worldPos : TEXCOORD1;
};

PSInput VSMain(uint id : SV_VertexID)
{
    PSInput o;
    float2 c = float2((id & 1) ? 1.0 : -1.0, (id & 2) ? 1.0 : -1.0);
    float3 wp = g_center.xyz + (g_axisU.xyz * c.x + g_axisV.xyz * c.y) * g_center.w;
    o.position = mul(g_viewProj, float4(wp, 1.0));
    o.local = c;
    o.worldPos = wp;
    return o;
}

float4 PSMain(PSInput input) : SV_Target
{
    float r = length(input.local);
    float a;
    if (g_mode.x < 0.5)
    {
        a = saturate(1.0 - r);
        a = a * a * (3.0 - 2.0 * a); // soft falloff
    }
    else
    {
        float ring = 1.0 - saturate(abs(r - 0.78) / 0.12);
        float inner = 1.0 - saturate(abs(r - (0.25 + 0.5 * g_mode.y)) / 0.06);
        a = saturate(ring + inner * (1.0 - g_mode.y) * 0.8);
    }
    // Fog fades decals out with the ground they lie on.
    float d = max(length(input.worldPos - g_cameraPos.xyz) - g_fogParams.x, 0.0);
    float fog = saturate(1.0 - exp(-(d * g_fogColor.w) * (d * g_fogColor.w)));
    a *= g_color.a * (1.0 - fog);
    return float4(g_color.rgb * a, a);
}
