// Terrain description (<name>.terrain.toml + heightmap/splat/mask PNGs) -> TerrainData.
// Schema: docs/ASSET_PIPELINE.md "Terrain description".
#pragma once

#include "shared/assets/Terrain.h"

#include <filesystem>
#include <string>
#include <vector>

namespace vo::tools::assetc {

// Lists the files the description depends on (the toml itself and its three images).
[[nodiscard]] bool terrainDependencies(const std::filesystem::path& desc, std::vector<std::filesystem::path>& deps,
                                       std::string& error);

[[nodiscard]] bool compileTerrain(const std::filesystem::path& desc, assets::TerrainData& out, std::string& error);

} // namespace vo::tools::assetc
