// Shared declarations of the 3D shaders (docs/CLIENT_RENDERING.md).
//
// Matrices arrive in the glTF / AssetMath layout (column-major storage, column vectors) and HLSL's
// default column_major packing, so transforms are `mul(M, float4(p, 1))`.
// Depth is reverse-Z (near = 1, far = 0). All lighting is in linear space; the render target is an
// sRGB view, so the hardware encodes on write. Vertex colours are sRGB-encoded and decoded here.

cbuffer FrameConstants : register(b0)
{
    float4x4 g_viewProj;
    float4x4 g_invViewProj;
    float4x4 g_shadowViewProj; // world -> shadow clip (standard depth, 0..1)
    float4 g_cameraPos;        // xyz eye, w time (s)
    float4 g_sunDir;           // xyz unit vector towards the sun, w unused
    float4 g_sunColor;         // rgb linear radiance
    float4 g_skyAmbient;       // rgb (normals facing up)
    float4 g_groundAmbient;    // rgb (normals facing down)
    float4 g_fogColor;         // rgb = horizon colour, w = density (1/m)
    float4 g_fogParams;        // x start distance (m), y shadow texel size, z shadows on (0/1), w shadow fade distance
    float4 g_zenithColor;      // rgb
    float4 g_terrainParams;    // x world size, y,z,w unused
    float4 g_layerTiling;      // 1 / tile size of the 4 terrain layers
};

Texture2D<float> g_shadowMap : register(t8);
SamplerComparisonState g_shadowSampler : register(s2);

float3 srgbToLinear(float3 c)
{
    float3 lo = c / 12.92;
    float3 hi = pow(abs((c + 0.055) / 1.055), 2.4);
    return lerp(hi, lo, step(c, 0.04045));
}

// 0 = fully shadowed, 1 = lit. 3x3 PCF on a comparison sampler.
float sunShadow(float3 worldPos, float3 normal)
{
    if (g_fogParams.z < 0.5)
        return 1.0;
    // Normal offset against acne on slopes.
    float3 p = worldPos + normal * 0.06;
    float4 c = mul(g_shadowViewProj, float4(p, 1.0));
    float3 uvz = c.xyz / c.w;
    float2 uv = float2(uvz.x * 0.5 + 0.5, 0.5 - uvz.y * 0.5);
    if (any(uv < 0.0) || any(uv > 1.0) || uvz.z > 1.0)
        return 1.0;
    float texel = g_fogParams.y;
    float sum = 0.0;
    [unroll] for (int y = -1; y <= 1; ++y)
    {
        [unroll] for (int x = -1; x <= 1; ++x)
        {
            sum += g_shadowMap.SampleCmpLevelZero(g_shadowSampler, uv + float2(x, y) * texel, uvz.z - 0.0015);
        }
    }
    float lit = sum / 9.0;
    // Fade out towards the edge of the shadowed area around the camera.
    float d = length(worldPos.xz - g_cameraPos.xz);
    float fade = saturate((d - g_fogParams.w * 0.8) / (g_fogParams.w * 0.2));
    return lerp(lit, 1.0, fade);
}

float3 applyFog(float3 color, float3 worldPos)
{
    float d = max(length(worldPos - g_cameraPos.xyz) - g_fogParams.x, 0.0);
    float f = 1.0 - exp(-(d * g_fogColor.w) * (d * g_fogColor.w));
    return lerp(color, g_fogColor.rgb, saturate(f));
}

// Lambert sun + hemispheric ambient, then distance fog. albedo and result are linear.
float3 shade(float3 albedo, float3 n, float3 worldPos, float shadow)
{
    float ndl = saturate(dot(n, g_sunDir.xyz));
    float3 ambient = lerp(g_groundAmbient.rgb, g_skyAmbient.rgb, n.y * 0.5 + 0.5);
    float3 c = albedo * (ambient + g_sunColor.rgb * (ndl * shadow));
    return applyFog(c, worldPos);
}
