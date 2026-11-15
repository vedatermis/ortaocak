// SpriteBatch shader: 2D quads in pixel coordinates (top-left origin).
//
// Vertex colors arrive as straight-alpha sRGB (RGBA8 UNORM) and are converted to linear,
// premultiplied alpha here; the render target is an sRGB view, so blending happens in linear space.
// Blend state: ONE / INV_SRC_ALPHA (premultiplied).
//
// params.z selects the pixel path:
//   0  textured: premultiplied texture * tint
//   1  glyph:    tint * coverage (R8 atlas), with a luminance-dependent coverage curve so light text
//                on dark backgrounds does not look bloated when blended in linear space
//   2  shape:    signed-distance rounded box (shape.xy = pixel offset from the box centre,
//                shape.zw = half size, params.x = corner radius, params.y = border width, 0 = filled)

cbuffer SpriteConstants : register(b0)
{
    float2 g_invViewport; // 1 / render target size in pixels
    float2 g_padding;
};

Texture2D g_texture : register(t0);
SamplerState g_sampler : register(s0);

struct VSInput
{
    float2 position : POSITION;
    float2 uv : TEXCOORD0;
    float4 color : COLOR0;
    float4 shape : TEXCOORD1;
    float4 params : TEXCOORD2;
};

struct PSInput
{
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
    float4 color : COLOR0;
    float4 shape : TEXCOORD1;
    float4 params : TEXCOORD2;
};

float3 srgbToLinear(float3 c)
{
    float3 lo = c / 12.92;
    float3 hi = pow(abs((c + 0.055) / 1.055), 2.4);
    return lerp(hi, lo, step(c, 0.04045));
}

PSInput VSMain(VSInput input)
{
    PSInput output;
    float2 ndc = input.position * g_invViewport * float2(2.0, -2.0) + float2(-1.0, 1.0);
    output.position = float4(ndc, 0.0, 1.0);
    output.uv = input.uv;
    output.color = float4(srgbToLinear(input.color.rgb) * input.color.a, input.color.a);
    output.shape = input.shape;
    output.params = input.params;
    return output;
}

float roundedBoxDistance(float2 p, float2 halfSize, float radius)
{
    float2 q = abs(p) - halfSize + radius;
    return length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - radius;
}

float4 PSMain(PSInput input) : SV_Target
{
    // Sample unconditionally: gradient operations must stay out of divergent flow control.
    float4 texel = g_texture.Sample(g_sampler, input.uv);
    float mode = input.params.z;

    float4 textured = texel * input.color;

    float coverage = texel.r;
    float alpha = max(input.color.a, 1e-4);
    float luminance = dot(input.color.rgb / alpha, float3(0.2126, 0.7152, 0.0722));
    // Light text: thin the coverage (gamma > 1); dark text: thicken it.
    float lightCurve = pow(abs(coverage), lerp(1.0, 1.45, saturate(luminance * 2.0 - 1.0)));
    float darkCurve = 1.0 - pow(abs(1.0 - coverage), lerp(1.0, 1.45, saturate(1.0 - luminance * 2.0)));
    float glyphCoverage = luminance >= 0.5 ? lightCurve : darkCurve;
    float4 glyph = input.color * glyphCoverage;

    float d = roundedBoxDistance(input.shape.xy, input.shape.zw, input.params.x);
    float shapeCoverage = saturate(0.5 - d);
    if (input.params.y > 0.0)
        shapeCoverage *= saturate(0.5 + d + input.params.y);
    float4 shape = input.color * shapeCoverage;

    return mode < 0.5 ? textured : (mode < 1.5 ? glyph : shape);
}
