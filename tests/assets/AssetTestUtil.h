// Helpers for the asset tests: temp directories, small sample assets, byte patching.
#pragma once

#include "shared/assets/Assets.h"

#include <atomic>
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

namespace vo::tests::assets {

// A fresh, empty directory under the system temp dir (removed on destruction).
class TempDir
{
public:
    explicit TempDir(std::string_view tag)
    {
        static std::atomic<int> counter{0};
        m_path = std::filesystem::temp_directory_path() / "vo_asset_tests" /
                 (std::string(tag) + "_" + std::to_string(counter++));
        std::error_code ec;
        std::filesystem::remove_all(m_path, ec);
        std::filesystem::create_directories(m_path);
    }
    ~TempDir()
    {
        std::error_code ec;
        std::filesystem::remove_all(m_path, ec);
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;

    [[nodiscard]] const std::filesystem::path& path() const noexcept { return m_path; }

private:
    std::filesystem::path m_path;
};

inline void writeText(const std::filesystem::path& p, std::string_view text)
{
    std::filesystem::create_directories(p.parent_path());
    std::ofstream f(p, std::ios::binary);
    f.write(text.data(), static_cast<std::streamsize>(text.size()));
}

inline std::vector<std::byte> readBytes(const std::filesystem::path& p)
{
    std::vector<std::byte> out;
    std::string err;
    if (!vo::assets::readFileBytes(p, out, err))
        out.clear();
    return out;
}

template <class T>
void patch(std::vector<std::byte>& file, std::size_t offset, T value)
{
    std::memcpy(file.data() + offset, &value, sizeof(T));
}

template <class T>
T peek(const std::vector<std::byte>& file, std::size_t offset)
{
    T v{};
    std::memcpy(&v, file.data() + offset, sizeof(T));
    return v;
}

// Recomputes the header CRC so that structural validation (not the CRC) is what a test exercises.
inline void fixCrc(std::vector<std::byte>& file)
{
    const std::uint32_t crc = vo::assets::crc32(std::span<const std::byte>(file).subspan(16));
    patch(file, 12, crc);
}

inline vo::assets::Mat4 translationMatrixForTest(vo::assets::Vec3 t)
{
    vo::assets::Mat4 m;
    m.m[12] = t.x;
    m.m[13] = t.y;
    m.m[14] = t.z;
    return m;
}

// A small skinned mesh: 2 bones, 4 vertices, 2 triangles in two submeshes (body + hair_1).
inline vo::assets::MeshData makeSampleMesh()
{
    using namespace vo::assets;
    MeshData m;
    m.vertexFormat = kVertexNormal | kVertexColor | kVertexSkin | kVertexTangent;
    m.vertexCount = 4;
    const VertexLayout l = m.layout();
    m.vertices.assign(static_cast<std::size_t>(l.stride) * 4, 0);
    const Vec3 pos[4] = {{0, 0, 0}, {1, 0, 0}, {0, 1, 0}, {1, 1, 0}};
    for (std::uint32_t v = 0; v < 4; ++v)
    {
        std::uint8_t* d = m.vertices.data() + static_cast<std::size_t>(v) * l.stride;
        std::memcpy(d + l.position, &pos[v], 12);
        const Vec3 n{0, 0, -1};
        std::memcpy(d + l.normal, &n, 12);
        const float t[4] = {1, 0, 0, 1};
        std::memcpy(d + l.tangent, t, 16);
        const float uv[2] = {pos[v].x, pos[v].y};
        std::memcpy(d + l.uv0, uv, 8);
        const std::uint8_t c[4] = {200, 100, 50, 255};
        std::memcpy(d + l.color, c, 4);
        const std::uint8_t j[4] = {static_cast<std::uint8_t>(v < 2 ? 0 : 1), 0, 0, 0};
        std::memcpy(d + l.joints, j, 4);
        const std::uint8_t w[4] = {255, 0, 0, 0};
        std::memcpy(d + l.weights, w, 4);
    }
    const std::uint16_t idx[6] = {0, 2, 1, 1, 2, 3};
    m.indexSize = 2;
    m.indices.resize(12);
    std::memcpy(m.indices.data(), idx, 12);
    Submesh body;
    body.indexStart = 0;
    body.indexCount = 3;
    body.name = "body";
    Submesh hair;
    hair.indexStart = 3;
    hair.indexCount = 3;
    hair.name = "hair_1";
    hair.texture = "textures/hair.png";
    hair.baseColor = {0.5f, 0.25f, 0.125f, 1.0f};
    hair.variantGroup = VariantGroup::Hair;
    hair.variantIndex = 1;
    m.submeshes = {body, hair};
    Bone a;
    a.name = "a";
    a.parent = -1;
    Bone b;
    b.name = "b";
    b.parent = 0;
    b.bindLocal.translation = {0, 1, 0};
    b.inverseBind = translationMatrixForTest({0, -1, 0});
    m.skeleton.bones = {a, b};
    m.computeBounds();
    return m;
}

} // namespace vo::tests::assets
