#include "tools/assetc/Tangents.h"

#include <mikktspace.h>

#include <cmath>

namespace vo::tools::assetc {

using assets::Vec3;

namespace {

struct MikkUser
{
    std::span<const Vec3> positions;
    std::span<const Vec3> normals;
    std::span<const std::array<float, 2>> uvs;
    std::span<const std::uint32_t> tris;
    std::vector<std::array<float, 4>>* out = nullptr;
};

MikkUser& user(const SMikkTSpaceContext* ctx)
{
    return *static_cast<MikkUser*>(ctx->m_pUserData);
}

std::uint32_t vertexOf(const SMikkTSpaceContext* ctx, int face, int vert)
{
    return user(ctx).tris[static_cast<std::size_t>(face) * 3 + static_cast<std::size_t>(vert)];
}

int getNumFaces(const SMikkTSpaceContext* ctx)
{
    return static_cast<int>(user(ctx).tris.size() / 3);
}

int getNumVerticesOfFace(const SMikkTSpaceContext*, const int)
{
    return 3;
}

void getPosition(const SMikkTSpaceContext* ctx, float out[], const int face, const int vert)
{
    const Vec3 p = user(ctx).positions[vertexOf(ctx, face, vert)];
    out[0] = p.x;
    out[1] = p.y;
    out[2] = p.z;
}

void getNormal(const SMikkTSpaceContext* ctx, float out[], const int face, const int vert)
{
    const Vec3 n = user(ctx).normals[vertexOf(ctx, face, vert)];
    out[0] = n.x;
    out[1] = n.y;
    out[2] = n.z;
}

void getTexCoord(const SMikkTSpaceContext* ctx, float out[], const int face, const int vert)
{
    const auto& uv = user(ctx).uvs[vertexOf(ctx, face, vert)];
    out[0] = uv[0];
    out[1] = 1.0f - uv[1]; // V up, as the baker (Blender) sees it
}

void setTSpaceBasic(const SMikkTSpaceContext* ctx, const float tangent[], const float sign, const int face,
                    const int vert)
{
    (*user(ctx).out)[static_cast<std::size_t>(face) * 3 + static_cast<std::size_t>(vert)] = {tangent[0], tangent[1],
                                                                                             tangent[2], sign};
}

} // namespace

Vec3 anyPerpendicular(Vec3 n) noexcept
{
    n = assets::normalize(n);
    const Vec3 ref = std::fabs(n.y) < 0.9f ? Vec3{0.0f, 1.0f, 0.0f} : Vec3{1.0f, 0.0f, 0.0f};
    const Vec3 t = assets::normalize(assets::cross(ref, n));
    return assets::length(t) > 0.5f ? t : Vec3{1.0f, 0.0f, 0.0f};
}

bool generateTangents(std::span<const Vec3> positions, std::span<const Vec3> normals,
                      std::span<const std::array<float, 2>> uvs, std::span<const std::uint32_t> tris,
                      std::vector<std::array<float, 4>>& cornerTangents)
{
    if (positions.size() != normals.size() || positions.size() != uvs.size() || tris.size() % 3 != 0)
        return false;
    for (std::uint32_t i : tris)
        if (i >= positions.size())
            return false;
    cornerTangents.assign(tris.size(), {0.0f, 0.0f, 0.0f, 0.0f});
    MikkUser u{positions, normals, uvs, tris, &cornerTangents};
    SMikkTSpaceInterface iface{};
    iface.m_getNumFaces = &getNumFaces;
    iface.m_getNumVerticesOfFace = &getNumVerticesOfFace;
    iface.m_getPosition = &getPosition;
    iface.m_getNormal = &getNormal;
    iface.m_getTexCoord = &getTexCoord;
    iface.m_setTSpaceBasic = &setTSpaceBasic;
    SMikkTSpaceContext ctx{};
    ctx.m_pInterface = &iface;
    ctx.m_pUserData = &u;
    if (!tris.empty() && genTangSpaceDefault(&ctx) == 0)
        return false;
    // Orthonormalise against the vertex normal and repair degenerate results.
    for (std::size_t c = 0; c < cornerTangents.size(); ++c)
    {
        auto& t = cornerTangents[c];
        const Vec3 n = assets::normalize(normals[tris[c]]);
        Vec3 v{t[0], t[1], t[2]};
        if (!(std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z)))
            v = {};
        v = v - n * assets::dot(n, v);
        v = assets::length(v) > 1e-6f ? assets::normalize(v) : anyPerpendicular(n);
        t = {v.x, v.y, v.z, t[3] < 0.0f ? -1.0f : 1.0f};
    }
    return true;
}

} // namespace vo::tools::assetc
