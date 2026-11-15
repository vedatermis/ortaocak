// 3D renderer of the world (docs/CLIENT_RENDERING.md): terrain chunks, instanced static props,
// GPU-skinned characters, ground decals (blob shadows, destination marker), a procedural sky with
// matching distance fog, one directional sun with a single camera-centred shadow map.
//
// Per frame:   beginFrame(view)  ->  addCharacter() / addDecal() ...  ->  render(target)
// render() draws the shadow map, then the main pass into the given colour + depth target (the back
// buffer, or the character preview's offscreen target). Conventions: reverse-Z depth (clear 0,
// GREATER_EQUAL), clockwise front faces with back-face culling, matrices in the glTF column-major
// layout used as mul(M, v) in HLSL, linear lighting into an sRGB render target view.
// Steady-state frames do not allocate: per-frame arrays and GPU buffers are sized at load time.
#pragma once

#include "client/render/GpuMesh.h"
#include "client/render/Shader.h"
#include "client/render/Texture2D.h"
#include "client/world/WorldMath.h"
#include "shared/assets/Terrain.h"

#include <d3d11.h>
#include <wrl/client.h>

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace vo::client::render {

class AssetCache;
class RenderStates;

struct ViewSetup
{
    world::Mat4 view;
    world::Mat4 projection;
    world::Vec3 eye{};
    world::Vec3 shadowFocus{}; // centre of the shadow-mapped area (the local player)
    float time = 0.0f;
    bool drawTerrain = true; // false for the character preview (sky backdrop only)
    bool shadows = true;
};

struct CharacterDraw
{
    const GpuMesh* mesh = nullptr;
    std::span<const world::Mat4> skin; // bone palette (Animator::skinMatrices); must stay valid until render()
    world::Mat4 world;                 // model -> world
    std::array<float, 4> tint{1.0f, 1.0f, 1.0f, 1.0f};
    std::uint8_t face = 0;
    std::uint8_t hair = 0;
};

struct DecalDraw
{
    world::Vec3 center{};
    world::Vec3 normal{0.0f, 1.0f, 0.0f};
    float radius = 0.5f;
    std::array<float, 4> color{0.0f, 0.0f, 0.0f, 0.5f}; // linear rgb + opacity
    bool ring = false;
    float phase = 0.0f;
};

struct WorldRenderStats
{
    std::uint32_t drawCalls = 0;
    std::uint64_t triangles = 0;
    std::uint32_t terrainChunks = 0;
    std::uint32_t propInstances = 0;
    std::uint32_t characters = 0;
    std::uint32_t shadowDrawCalls = 0;
    std::uint64_t shadowTriangles = 0;
};

struct RenderTargetViews
{
    ID3D11RenderTargetView* color = nullptr;
    ID3D11DepthStencilView* depth = nullptr;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
};

class WorldRenderer
{
public:
    static constexpr std::size_t kMaxCharacters = 256;
    static constexpr std::size_t kMaxDecals = 512;
    static constexpr std::uint32_t kShadowMapSize = 2048;
    static constexpr float kShadowRadius = 48.0f; // metres around the focus

    WorldRenderer() = default;
    ~WorldRenderer();
    WorldRenderer(const WorldRenderer&) = delete;
    WorldRenderer& operator=(const WorldRenderer&) = delete;

    bool init(ID3D11Device* device, ID3D11DeviceContext* context, const RenderStates& states, std::string& error);
    void shutdown() noexcept;
    [[nodiscard]] bool initialized() const noexcept { return m_device != nullptr; }

    // Builds the terrain chunks, splat and layer textures (layer textures come from the cache and
    // must be loaded) and the prop batches from the terrain's placement list.
    bool setTerrain(const assets::TerrainData& terrain, const AssetCache& cache, std::string& error);
    void clearTerrain() noexcept;

    void beginFrame(const ViewSetup& view);
    void addCharacter(const CharacterDraw& draw);
    void addDecal(const DecalDraw& decal);
    void render(const RenderTargetViews& target);

    [[nodiscard]] const WorldRenderStats& stats() const noexcept { return m_stats; }
    [[nodiscard]] const world::Mat4& viewProjection() const noexcept { return m_viewProj; }

    // Light setup (linear colours); defaults are a clear afternoon.
    world::Vec3 sunDirection = world::Vec3{0.52f, 0.70f, 0.49f}; // towards the sun (normalised at use)
    std::array<float, 3> sunColor{1.10f, 1.00f, 0.86f};
    std::array<float, 3> skyAmbient{0.33f, 0.40f, 0.50f};
    std::array<float, 3> groundAmbient{0.17f, 0.15f, 0.12f};
    std::array<float, 3> horizonColor{0.52f, 0.64f, 0.78f}; // = fog colour
    std::array<float, 3> zenithColor{0.15f, 0.30f, 0.62f};
    float fogDensity = 1.0f / 650.0f;
    float fogStart = 25.0f;

private:
    struct TerrainChunk
    {
        world::Vec3 min{};
        world::Vec3 max{};
        std::uint32_t baseVertex = 0;
    };
    struct PropInstance
    {
        std::array<float, 12> rows{}; // world matrix rows 0..2
        world::Vec3 center{};         // bounding sphere (world)
        float radius = 0.0f;
        float maxDistance = 0.0f;
    };
    struct PropBatch
    {
        const GpuMesh* mesh = nullptr;
        std::vector<PropInstance> instances;
        std::uint32_t mainFirst = 0;
        std::uint32_t mainCount = 0;
        std::uint32_t shadowFirst = 0;
        std::uint32_t shadowCount = 0;
    };

    bool createBuffer(UINT size, UINT bind, D3D11_USAGE usage, const void* init, const char* name,
                      Microsoft::WRL::ComPtr<ID3D11Buffer>& out, std::string& error);
    void updateFrameConstants();
    void cullAndUploadProps();
    void renderShadowMap();
    void drawTerrain(bool shadowPass);
    void drawProps(bool shadowPass);
    void drawCharacters(bool shadowPass);
    void drawDecals();
    void drawSky();
    template <class T>
    void writeConstants(ID3D11Buffer* buffer, const T& data);
    void bindCommon();

    ID3D11Device* m_device = nullptr;
    ID3D11DeviceContext* m_context = nullptr;
    const RenderStates* m_states = nullptr;

    Pipeline m_staticPipeline;
    Pipeline m_skinnedPipeline;
    Pipeline m_staticShadowPipeline;
    Pipeline m_skinnedShadowPipeline;
    Pipeline m_terrainPipeline;
    Pipeline m_terrainShadowPipeline;
    Pipeline m_skyPipeline;
    Pipeline m_decalPipeline;
    Microsoft::WRL::ComPtr<ID3D11Buffer> m_frameCb;
    Microsoft::WRL::ComPtr<ID3D11Buffer> m_drawCb;
    Microsoft::WRL::ComPtr<ID3D11Buffer> m_boneCb;
    Microsoft::WRL::ComPtr<ID3D11Buffer> m_decalCb;
    Texture2D m_white;

    // Shadow map (R32_TYPELESS: DSV D32_FLOAT, SRV R32_FLOAT).
    Microsoft::WRL::ComPtr<ID3D11Texture2D> m_shadowTex;
    Microsoft::WRL::ComPtr<ID3D11DepthStencilView> m_shadowDsv;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_shadowSrv;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> m_dummyShadowTex; // bound while the shadow map is written
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_dummyShadowSrv;

    // Terrain.
    Microsoft::WRL::ComPtr<ID3D11Buffer> m_terrainVb;
    Microsoft::WRL::ComPtr<ID3D11Buffer> m_terrainIb;
    std::uint32_t m_chunkIndexCount = 0;
    std::vector<TerrainChunk> m_chunks;
    Texture2D m_splat;
    std::array<const Texture2D*, 4> m_layers{};
    std::array<float, 4> m_layerTiling{};
    float m_worldSize = 0.0f;
    bool m_hasTerrain = false;

    // Props.
    std::vector<PropBatch> m_batches;
    Microsoft::WRL::ComPtr<ID3D11Buffer> m_instanceVb;
    std::size_t m_instanceCapacity = 0; // instances (main + shadow regions)
    std::vector<float> m_instanceScratch;

    // Per frame.
    ViewSetup m_view;
    world::Mat4 m_viewProj;
    world::Mat4 m_invViewProj;
    world::Mat4 m_shadowViewProj;
    world::Frustum m_frustum;
    world::Frustum m_shadowFrustum;
    bool m_shadowsThisFrame = false;
    std::vector<CharacterDraw> m_characters;
    std::vector<DecalDraw> m_decals;
    WorldRenderStats m_stats;
};

} // namespace vo::client::render
