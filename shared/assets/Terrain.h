// .vterrain: heightmap zone with splat weights, walk mask, spawn points and static object placements
// (docs/ASSET_FORMATS.md §4). Used by the client (rendering) and the server (heightAt, isWalkable,
// spawn points).
#pragma once

#include "shared/assets/AssetMath.h"

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace vo::assets {

struct TerrainLayer
{
    std::string texture; // asset path of a tileable PNG
    float tileSize = 8.0f; // metres per texture repeat
};

struct SpawnPoint
{
    std::string name;
    Vec3 position{};
    float yaw = 0.0f;
};

enum ObjectFlags : std::uint32_t
{
    kObjectBlocks = 0x1,
};

struct ObjectPlacement
{
    std::string mesh; // asset path of a .vmesh
    Vec3 position{};
    float yaw = 0.0f;
    float scale = 1.0f;
    std::uint32_t flags = kObjectBlocks;

    [[nodiscard]] bool blocks() const noexcept { return (flags & kObjectBlocks) != 0; }
    // T(position) * RotY(yaw) * S(scale), column vectors.
    [[nodiscard]] Mat4 worldMatrix() const noexcept;
};

struct TerrainData
{
    std::uint32_t resolution = 0; // N; heights are N x N, index j * N + i
    float worldSize = 0.0f;
    float minHeight = 0.0f;
    float maxHeight = 1.0f;
    std::vector<std::uint16_t> heights;
    std::uint32_t splatResolution = 0;
    std::vector<std::uint8_t> splat; // RGBA8, S x S
    std::uint32_t maskResolution = 0;
    std::vector<std::uint8_t> walkMask; // 1 walkable, 0 blocked, M x M
    std::vector<TerrainLayer> layers;   // exactly 4
    std::vector<SpawnPoint> spawns;
    std::vector<ObjectPlacement> objects;

    // Height in metres of sample (i, j); indices are clamped.
    [[nodiscard]] float sampleHeight(int i, int j) const noexcept;
    // Bilinear height at world (x, z); outside the zone the position is clamped to the edge.
    [[nodiscard]] float heightAt(float x, float z) const noexcept;
    // Unit surface normal from central differences of heightAt.
    [[nodiscard]] Vec3 normalAt(float x, float z) const noexcept;
    // False outside [0, worldSize) or on a blocked mask cell.
    [[nodiscard]] bool isWalkable(float x, float z) const noexcept;
    [[nodiscard]] bool contains(float x, float z) const noexcept;
    [[nodiscard]] const SpawnPoint* findSpawn(std::string_view name) const noexcept;
    // Height in metres -> stored u16 (rounded, clamped).
    [[nodiscard]] std::uint16_t quantizeHeight(float metres) const noexcept;
};

[[nodiscard]] bool parseTerrain(std::span<const std::byte> file, TerrainData& out, std::string& error);
[[nodiscard]] bool loadTerrain(const std::filesystem::path& path, TerrainData& out, std::string& error);
[[nodiscard]] bool serializeTerrain(const TerrainData& terrain, std::vector<std::byte>& out, std::string& error);

} // namespace vo::assets
