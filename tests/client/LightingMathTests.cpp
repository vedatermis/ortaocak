// CPU twins of the shader lighting (client/render/LightingMath.h): BRDF terms, the split-sum fit,
// the tone map, and the SH irradiance of the procedural sky that feeds the ambient.

#include "client/render/LightingMath.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>

namespace lm = vo::client::render::lighting;
using Catch::Approx;

TEST_CASE("GGX distribution is normalised and the BRDF terms have the right limits", "[client][lighting]")
{
    // Integral of D(h) * cos(theta_h) over the hemisphere is 1 for any roughness.
    for (const float alpha : {0.1f, 0.3f, 0.6f, 1.0f})
    {
        double sum = 0.0;
        const int steps = 20000;
        for (int i = 0; i < steps; ++i)
        {
            const double theta = (i + 0.5) * (lm::kPi / 2.0) / steps;
            const double c = std::cos(theta);
            sum += lm::ggxD(static_cast<float>(c), alpha) * c * std::sin(theta) * (lm::kPi / 2.0 / steps) * 2.0 * lm::kPi;
        }
        INFO("alpha " << alpha);
        CHECK(sum == Approx(1.0).epsilon(0.01));
    }
    // Fresnel: F0 at normal incidence, 1 at grazing.
    CHECK(lm::fresnelSchlick(0.04f, 1.0f) == Approx(0.04f));
    CHECK(lm::fresnelSchlick(0.04f, 0.0f) == Approx(1.0f));
    // Smith visibility: V = 1 / (4 NdotL NdotV) for a perfectly smooth surface at normal incidence ~ 0.25.
    CHECK(lm::smithVisibility(1.0f, 1.0f, 0.001f) == Approx(0.25f).epsilon(0.01));
    CHECK(lm::smithVisibility(0.5f, 0.5f, 0.8f) < lm::smithVisibility(0.5f, 0.5f, 0.2f));
    // Burley diffuse is 1 at normal incidence when fd90 = 1 (roughness 0.25, LdotH = 1).
    CHECK(lm::burleyDiffuse(1.0f, 1.0f, 1.0f, 0.25f) == Approx(1.0f));
    // Split-sum fit stays in [0, 1] and smooth surfaces viewed head-on reflect about F0.
    for (float r = 0.0f; r <= 1.0f; r += 0.1f)
        for (float nv = 0.05f; nv <= 1.0f; nv += 0.1f)
        {
            const auto ab = lm::envBrdfApprox(r, nv);
            CHECK(ab[0] >= -0.01f);
            CHECK(ab[0] + ab[1] <= 1.05f);
        }
    const auto smooth = lm::envBrdfApprox(0.0f, 1.0f);
    CHECK(0.04f * smooth[0] + smooth[1] == Approx(0.04f).margin(0.02));
}

TEST_CASE("ACES fitted tone map is monotonic, maps black to black and saturates", "[client][lighting]")
{
    CHECK(lm::acesFitted({0, 0, 0}).x == Approx(0.0f).margin(1e-3));
    float last = -1.0f;
    for (float x = 0.0f; x < 20.0f; x += 0.05f)
    {
        const float y = lm::acesFitted({x, x, x}).y;
        CHECK(y >= last - 1e-6f);
        last = y;
    }
    CHECK(lm::acesFitted({16, 16, 16}).y > 0.97f);
    // Mid grey sits in the lower middle of the display range.
    const float grey = lm::linearToSrgb(lm::acesFitted({0.18f, 0.18f, 0.18f}).y);
    CHECK(grey > 0.30f);
    CHECK(grey < 0.45f);
}

TEST_CASE("SH irradiance of the procedural sky", "[client][lighting]")
{
    // A uniform environment of radiance S gives a white surface the brightness S in every direction.
    const lm::Float3 s{0.3f, 0.5f, 0.7f};
    const auto uniform = lm::irradianceSh(lm::projectSh([&](lm::Float3) { return s; }));
    for (const lm::Float3 n : {lm::Float3{0, 1, 0}, lm::Float3{1, 0, 0}, lm::Float3{0, -1, 0}, lm::Float3{0.6f, 0.0f, 0.8f}})
    {
        const lm::Float3 e = lm::evalSh(uniform, n);
        CHECK(e.x == Approx(s.x).epsilon(0.01));
        CHECK(e.z == Approx(s.z).epsilon(0.01));
    }
    // Sky above, ground below: up-facing sees mostly sky, down-facing mostly ground, sideways the
    // average (exact for a two-colour sphere: lerp(ground, sky, 0.5 + 0.5 n.y), up to L2 ringing).
    lm::SkyColors sky{{1.0f, 1.0f, 1.0f}, {1.0f, 1.0f, 1.0f}, {0.2f, 0.2f, 0.2f}};
    const auto split = lm::irradianceSh(lm::projectSh([&](lm::Float3 d) { return lm::skyRadiance(sky, d); }));
    CHECK(lm::evalSh(split, {0, 1, 0}).x == Approx(1.0f).margin(0.05));
    CHECK(lm::evalSh(split, {0, -1, 0}).x == Approx(0.2f).margin(0.05));
    CHECK(lm::evalSh(split, {1, 0, 0}).x == Approx(0.6f).margin(0.03));
    // The sky radiance function matches Sky.hlsl: horizon at y = 0, zenith straight up.
    lm::SkyColors grad{{0.5f, 0.6f, 0.8f}, {0.1f, 0.3f, 0.6f}, {0, 0, 0}};
    CHECK(lm::skyRadiance(grad, {1, 0, 0}).x == Approx(0.5f));
    CHECK(lm::skyRadiance(grad, {0, 1, 0}).x == Approx(0.1f));
    CHECK(lm::skyRadiance(grad, {0, -0.5f, 0.866f}).x == Approx(0.0f));
}
