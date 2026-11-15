// glTF 2.0 (.glb/.gltf, via cgltf) -> .vmesh / .vanim conversion, including the right-handed glTF
// to left-handed engine conversion (docs/ASSET_PIPELINE.md "Coordinate conversion").
#pragma once

#include "shared/assets/Animation.h"
#include "shared/assets/AssetMath.h"
#include "shared/assets/Mesh.h"

#include <filesystem>
#include <string>
#include <vector>

namespace vo::tools::assetc {

// glTF (right-handed, +Y up, +Z forward, +X = the asset's left) -> engine (left-handed, +X right):
// mirror X. Positions/normals/tangent xyz: (x, y, z) -> (-x, y, z); tangent w -> -w;
// quaternions (x, y, z, w) -> (x, -y, -z, w); matrices M -> S·M·S with S = diag(-1, 1, 1, 1);
// triangles (i0, i1, i2) -> (i0, i2, i1).
[[nodiscard]] constexpr assets::Vec3 gltfToEngine(assets::Vec3 v) noexcept { return {-v.x, v.y, v.z}; }
[[nodiscard]] constexpr assets::Quat gltfToEngine(assets::Quat q) noexcept { return {q.x, -q.y, -q.z, q.w}; }
[[nodiscard]] assets::Mat4 gltfToEngine(const assets::Mat4& m) noexcept;

struct MeshImportOptions
{
    // Directory that asset paths of referenced textures are made relative to (the source assets root;
    // `assetc build` passes <assets_src>). Empty = the glTF file's directory.
    std::filesystem::path assetRoot;
};

// Converts every mesh node of the default scene into one MeshData. `textureFiles` (optional)
// receives the absolute source paths of referenced texture images.
[[nodiscard]] bool importMesh(const std::filesystem::path& gltfPath, const MeshImportOptions& options,
                              assets::MeshData& out, std::string& error,
                              std::vector<std::filesystem::path>* textureFiles = nullptr);

// Per-clip settings from the optional sidecar `<stem>.anims.toml`:
//   [[clip]]
//   name = "walk"
//   loop = true          # optional, default true
//   move_speed = 2.0     # optional, default 0 (m/s)
struct ClipSettings
{
    std::string name;
    bool loop = true;
    float moveSpeed = 0.0f;
};

[[nodiscard]] std::filesystem::path animSidecarPath(const std::filesystem::path& gltfPath);
// Missing file = no settings (true). Unknown keys, bad types or duplicates are errors.
[[nodiscard]] bool readAnimSidecar(const std::filesystem::path& sidecar, std::vector<ClipSettings>& out,
                                   std::string& error);

struct AnimImportOptions
{
    float sampleRate = 30.0f;
    bool quantizeRotations = false;
    std::vector<ClipSettings> clips; // usually from readAnimSidecar
};

// One clip per glTF animation (sampled at sampleRate, one track per skin joint). A file without
// animations yields an empty vector and success.
[[nodiscard]] bool importAnimations(const std::filesystem::path& gltfPath, const AnimImportOptions& options,
                                    std::vector<assets::AnimationClip>& out, std::string& error);

} // namespace vo::tools::assetc
