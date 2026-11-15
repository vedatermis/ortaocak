// CPU side of the lighting model (docs/CLIENT_RENDERING.md §4): the same formulas as
// client/shaders/World.hlsli and PostProcess.hlsl, used to derive the image-free ambient from the sky
// colours (L2 spherical-harmonics irradiance) and by the unit tests (tests/client/LightingMathTests).
// Header-only and D3D-free.
//
// Units: light colours are "irradiance at normal incidence" with pi folded in, so a white Lambert
// surface facing a light of colour E shows E (diffuse BRDF = albedo, specular BRDF = pi * D * V * F).
// Sky colours are the displayed radiance in the same units: a white surface facing up under a uniform
// sky S shows S.
#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>

namespace vo::client::render::lighting {

inline constexpr float kPi = 3.14159265358979f;

struct Float3
{
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
};

[[nodiscard]] constexpr Float3 operator+(Float3 a, Float3 b) noexcept { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
[[nodiscard]] constexpr Float3 operator*(Float3 a, float s) noexcept { return {a.x * s, a.y * s, a.z * s}; }
[[nodiscard]] constexpr Float3 operator*(Float3 a, Float3 b) noexcept { return {a.x * b.x, a.y * b.y, a.z * b.z}; }
[[nodiscard]] constexpr float dot3(Float3 a, Float3 b) noexcept { return a.x * b.x + a.y * b.y + a.z * b.z; }
[[nodiscard]] inline Float3 normalize3(Float3 v) noexcept
{
    const float l = std::sqrt(dot3(v, v));
    return l > 0.0f ? v * (1.0f / l) : Float3{};
}
[[nodiscard]] constexpr Float3 lerp3(Float3 a, Float3 b, float t) noexcept { return a + (b + a * -1.0f) * t; }

// ---- BRDF (metalness workflow) ----

// GGX / Trowbridge-Reitz normal distribution; alpha = perceptual roughness^2.
[[nodiscard]] inline float ggxD(float nDotH, float alpha) noexcept
{
    const float a2 = alpha * alpha;
    const float d = nDotH * nDotH * (a2 - 1.0f) + 1.0f;
    return a2 / (kPi * d * d);
}

// Height-correlated Smith visibility V = G / (4 NdotL NdotV).
[[nodiscard]] inline float smithVisibility(float nDotV, float nDotL, float alpha) noexcept
{
    const float a2 = alpha * alpha;
    const float gv = nDotL * std::sqrt(nDotV * nDotV * (1.0f - a2) + a2);
    const float gl = nDotV * std::sqrt(nDotL * nDotL * (1.0f - a2) + a2);
    return 0.5f / std::max(gv + gl, 1e-5f);
}

[[nodiscard]] inline float fresnelSchlick(float f0, float vDotH) noexcept
{
    const float f = std::pow(1.0f - std::clamp(vDotH, 0.0f, 1.0f), 5.0f);
    return f0 + (1.0f - f0) * f;
}

// Burley diffuse (without 1/pi: see the units note above); 1 for a Lambert surface at roughness 0.25.
[[nodiscard]] inline float burleyDiffuse(float nDotV, float nDotL, float lDotH, float roughness) noexcept
{
    const float fd90 = 0.5f + 2.0f * roughness * lDotH * lDotH;
    const float lightScatter = 1.0f + (fd90 - 1.0f) * std::pow(1.0f - nDotL, 5.0f);
    const float viewScatter = 1.0f + (fd90 - 1.0f) * std::pow(1.0f - nDotV, 5.0f);
    return lightScatter * viewScatter;
}

// Split-sum environment BRDF, analytic fit (Karis, "Physically Based Shading on Mobile"):
// specular ambient = prefiltered env * (F0 * a + b).
[[nodiscard]] inline std::array<float, 2> envBrdfApprox(float roughness, float nDotV) noexcept
{
    const float c0[4] = {-1.0f, -0.0275f, -0.572f, 0.022f};
    const float c1[4] = {1.0f, 0.0425f, 1.04f, -0.04f};
    float r[4];
    for (int i = 0; i < 4; ++i)
        r[i] = roughness * c0[i] + c1[i];
    const float a004 = std::min(r[0] * r[0], std::exp2(-9.28f * nDotV)) * r[0] + r[1];
    return {a004 * -1.04f + r[2], a004 * 1.04f + r[3]};
}

// ---- Tone mapping ----

// ACES fitted (Stephen Hill's RRT + ODT fit in the ACES AP1-ish working space), linear in, linear out
// in [0, 1] (the sRGB view encodes afterwards).
[[nodiscard]] inline Float3 acesFitted(Float3 c) noexcept
{
    const Float3 in{0.59719f * c.x + 0.35458f * c.y + 0.04823f * c.z, 0.07600f * c.x + 0.90834f * c.y + 0.01566f * c.z,
                    0.02840f * c.x + 0.13383f * c.y + 0.83777f * c.z};
    const auto fit = [](float v) {
        const float a = v * (v + 0.0245786f) - 0.000090537f;
        const float b = v * (0.983729f * v + 0.4329510f) + 0.238081f;
        return a / b;
    };
    const Float3 f{fit(in.x), fit(in.y), fit(in.z)};
    const Float3 out{1.60475f * f.x - 0.53108f * f.y - 0.07367f * f.z, -0.10208f * f.x + 1.10813f * f.y - 0.00605f * f.z,
                     -0.00327f * f.x - 0.07276f * f.y + 1.07602f * f.z};
    return {std::clamp(out.x, 0.0f, 1.0f), std::clamp(out.y, 0.0f, 1.0f), std::clamp(out.z, 0.0f, 1.0f)};
}

[[nodiscard]] inline float linearToSrgb(float c) noexcept
{
    c = std::clamp(c, 0.0f, 1.0f);
    return c <= 0.0031308f ? c * 12.92f : 1.055f * std::pow(c, 1.0f / 2.4f) - 0.055f;
}

// ---- Procedural sky (Sky.hlsl) and its irradiance ----

struct SkyColors
{
    Float3 horizon;      // = fog colour
    Float3 zenith;
    Float3 groundRadiance; // what the lower hemisphere sends back (bounce off the ground)
};

// Displayed radiance of the sky dome in direction d (unit), without the sun disk (direct light covers
// the sun): horizon -> zenith gradient above, the ground below.
[[nodiscard]] inline Float3 skyRadiance(const SkyColors& s, Float3 d) noexcept
{
    if (d.y < 0.0f)
        return s.groundRadiance;
    const float t = std::clamp(std::pow(std::clamp(d.y, 0.0f, 1.0f), 0.55f), 0.0f, 1.0f);
    return lerp3(s.horizon, s.zenith, t);
}

// L2 spherical harmonics, 9 RGB coefficients, real basis in the usual (l, m) order.
using Sh9 = std::array<Float3, 9>;

[[nodiscard]] inline std::array<float, 9> shBasis(Float3 d) noexcept
{
    return {0.282095f,
            0.488603f * d.y,
            0.488603f * d.z,
            0.488603f * d.x,
            1.092548f * d.x * d.y,
            1.092548f * d.y * d.z,
            0.315392f * (3.0f * d.z * d.z - 1.0f),
            1.092548f * d.x * d.z,
            0.546274f * (d.x * d.x - d.y * d.y)};
}

// Projects radiance(d) onto SH by numerical integration (deterministic latitude-longitude grid).
template <class Radiance>
[[nodiscard]] Sh9 projectSh(Radiance radiance, int rings = 64, int segments = 128)
{
    Sh9 sh{};
    for (int i = 0; i < rings; ++i)
    {
        const float theta = (static_cast<float>(i) + 0.5f) * kPi / static_cast<float>(rings);
        const float dOmegaRing = std::sin(theta) * (kPi / static_cast<float>(rings)) * (2.0f * kPi / static_cast<float>(segments));
        for (int j = 0; j < segments; ++j)
        {
            const float phi = (static_cast<float>(j) + 0.5f) * 2.0f * kPi / static_cast<float>(segments);
            // Y up: theta from +Y.
            const Float3 d{std::sin(theta) * std::cos(phi), std::cos(theta), std::sin(theta) * std::sin(phi)};
            const Float3 l = radiance(d);
            const auto y = shBasis(d);
            for (std::size_t k = 0; k < 9; ++k)
                sh[k] = sh[k] + l * (y[k] * dOmegaRing);
        }
    }
    return sh;
}

// Radiance SH -> coefficients that give the displayed diffuse ambient of a white surface with normal
// n as dot(basis(n), coeffs): cosine convolution (Ramamoorthi & Hanrahan: pi, 2pi/3, pi/4) / pi.
[[nodiscard]] inline Sh9 irradianceSh(const Sh9& radiance) noexcept
{
    const float band[9] = {1.0f, 2.0f / 3.0f, 2.0f / 3.0f, 2.0f / 3.0f, 0.25f, 0.25f, 0.25f, 0.25f, 0.25f};
    Sh9 out{};
    for (std::size_t k = 0; k < 9; ++k)
        out[k] = radiance[k] * band[k];
    return out;
}

[[nodiscard]] inline Float3 evalSh(const Sh9& sh, Float3 n) noexcept
{
    const auto y = shBasis(n);
    Float3 r{};
    for (std::size_t k = 0; k < 9; ++k)
        r = r + sh[k] * y[k];
    return {std::max(r.x, 0.0f), std::max(r.y, 0.0f), std::max(r.z, 0.0f)};
}

} // namespace vo::client::render::lighting
