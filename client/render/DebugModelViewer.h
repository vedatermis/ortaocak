// `vo_client --debug-model <asset path>`: a server-less viewer for tuning materials and lighting.
// Shows one or more .vmesh files (e.g. raw generated characters converted with `assetc mesh`) as
// characters through the real character path (GPU skinning with an identity palette, rim light,
// shadows) either in the village (world light rig, zone 1 terrain and props) or with the character
// preview's studio rig. Logs `viewer: perf` lines (FPS, draw calls, triangles, GPU memory) every
// second; combine with --screenshot / --exit-after-frames for automated shots.
#pragma once

#include "client/render/GpuMesh.h"
#include "client/world/WorldMath.h"

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace vo::client::render {

class AssetCache;
class RenderDevice;
class WorldRenderer;

struct DebugViewerOptions
{
    std::vector<std::string> models; // asset paths of .vmesh files (relative to assets/)
    std::uint32_t count = 0;         // instances in a grid (0 = one per model)
    // front | threeq | side | back | face | armor | wide | "yawDeg,pitchDeg,distance,heightFraction"
    // (yaw relative to the model's facing, 0 = in front of it).
    std::string camera = "threeq";
    std::string light = "world"; // world | preview-aydor | preview-kargan
    float modelYawDeg = 0.0f;    // facing of the models (world yaw)
    float facingDeg = 0.0f;      // the mesh's own front direction as a yaw in model space (raw Tripo: -90)
    float orbitDegPerSecond = 0.0f;
    bool shadows = true; // --no-shadows / graphics.shadows
    std::optional<std::pair<float, float>> at; // world x, z (default: spawn_aydor)
};

class DebugModelViewer
{
public:
    DebugModelViewer();
    ~DebugModelViewer();
    DebugModelViewer(const DebugModelViewer&) = delete;
    DebugModelViewer& operator=(const DebugModelViewer&) = delete;

    // Loads the terrain (world light) and the models synchronously.
    bool init(RenderDevice& device, AssetCache& cache, WorldRenderer& renderer, const DebugViewerOptions& options,
              std::string& error);
    void shutdown() noexcept;
    void update(double dt, double realDt);
    void render();

private:
    struct Instance
    {
        const GpuMesh* mesh = nullptr;
        world::Mat4 world;
    };

    [[nodiscard]] bool buildSkinnedCopy(const GpuMesh& src, GpuMesh& out, std::string& error);

    RenderDevice* m_device = nullptr;
    AssetCache* m_cache = nullptr;
    WorldRenderer* m_renderer = nullptr;
    DebugViewerOptions m_options;
    std::vector<std::unique_ptr<GpuMesh>> m_meshes;
    std::vector<Instance> m_instances;
    std::array<world::Mat4, 4> m_palette{};
    bool m_world = true;
    bool m_kargan = false;
    float m_modelHeight = 2.0f;
    world::Vec3 m_base{};
    double m_time = 0.0;
    double m_perfTime = 0.0;
    std::uint32_t m_perfFrames = 0;
};

} // namespace vo::client::render
