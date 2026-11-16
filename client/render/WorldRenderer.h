// 3D renderer of the world (docs/CLIENT_RENDERING.md): terrain chunks, instanced static props,
// GPU-skinned characters, ground decals (blob shadows, destination marker), a procedural sky with
// matching distance fog, physically based lighting (GGX + Burley, metalness, normal + ORM maps) from
// one directional sun with a 2-cascade shadow map, up to three extra lights (rim / fill) and an
// image-free ambient derived from the sky colours (SH irradiance + analytic prefiltered environment).
//
// Per frame:   beginFrame(view)  ->  addCharacter() / addDecal() ...  ->  render(target)
// render() draws the shadow cascades, then the main pass into the given colour + depth target (the
// HDR scene target or the character preview's offscreen target; RenderDevice / RenderTarget tone-map
// it afterwards). Conventions: reverse-Z depth (clear 0, GREATER_EQUAL), clockwise front faces with
// back-face culling, matrices in the glTF column-major layout used as mul(M, v) in HLSL, linear HDR
// radiance x exposure.
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
#include <string_view>
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
    float shadowScale = 1.0f; // multiplies the cascade radii (the preview frames ~2 m)
};

// A static mesh carried by a character (M8: the weapon in each hand). Drawn with the static mesh
// pipeline right after its owner, with the owner's tint and opacity, in the shadow passes too, and
// culled together with it (an off-screen character's sword is not drawn either).
struct AttachedMesh
{
    const GpuMesh* mesh = nullptr; // null = nothing
    world::Mat4 world;             // mesh model -> world
};

struct CharacterDraw
{
    const GpuMesh* mesh = nullptr;
    std::array<AttachedMesh, 2> attached{}; // MainHand, OffHand weapons
    std::span<const world::Mat4> skin; // bone palette (Animator::skinMatrices); must stay valid until render()
    world::Mat4 world;                 // model -> world
    std::array<float, 4> tint{1.0f, 1.0f, 1.0f, 1.0f};
    // < 1: screen-door dither (ordered 4x4 pattern) so the character fades out when the camera is
    // pushed right up to it; <= 0 skips it in the main pass (it still casts its shadow).
    float opacity = 1.0f;
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
    std::uint32_t attachments = 0; // weapons drawn in the main pass
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

// [graphics] switches (config/client.toml, `--graphics key=value`, `scene graphics ...`).
struct GraphicsSettings
{
    std::uint32_t shadowCascades = 2; // 1 (one 48 m cascade, the old behaviour) or 2 (14 m + 64 m)
    bool shadowPcf = true;            // 4x4-tap bilinear PCF; false: one bilinear compare tap
    bool normalMaps = true;
    bool specular = true;    // GGX direct + split-sum ambient specular; false = diffuse only
    bool extraLights = true; // rim light on characters (world) / fill + rim (preview)
    float exposure = 1.0f;   // multiplies the light rig's exposure
    std::uint32_t debugView = 0; // meshes: 0 off, 1 lighting only (grey material), 2 shading normals
    bool normalFlipY = false;    // debugging: flip the normal maps' green (DirectX-convention maps)
};

// Applies one [graphics] key (config/client.toml spelling) to the settings; `toneMapClamp` receives
// the tone map choice. Keys: shadow_cascades (1|2), shadow_filter (pcf|hard), normal_maps, specular,
// rim_light (bools: true|false|on|off|1|0), exposure (0.05..16), tonemap (aces|clamp). Unknown keys
// and bad values are errors.
[[nodiscard]] bool applyGraphicsSetting(std::string_view key, std::string_view value, GraphicsSettings& settings,
                                        bool& toneMapClamp, std::string& error);

// A directional light without shadows (rim / fill).
struct ExtraLight
{
    world::Vec3 direction{0.0f, 1.0f, 0.0f}; // towards the light; camera space (x right, y up, z forward) if cameraRelative
    std::array<float, 3> color{0.0f, 0.0f, 0.0f}; // 0 = off
    bool charactersOnly = true;
    bool cameraRelative = true;
};

// The light rig: sun (key, shadowed), sky colours (backdrop, fog, environment and ambient are all
// derived from them), extra lights, exposure.
struct Lighting
{
    world::Vec3 sunDirection{0.52f, 0.70f, 0.49f}; // towards the sun (normalised at use)
    std::array<float, 3> sunColor{2.6f, 2.4f, 2.1f};
    std::array<float, 3> horizonColor{0.52f, 0.64f, 0.78f}; // = fog colour
    std::array<float, 3> zenithColor{0.15f, 0.30f, 0.62f};
    // Diffuse albedo of the ground; the lower hemisphere of the environment is this ground lit by
    // the sun and the sky.
    std::array<float, 3> groundAlbedo{0.20f, 0.19f, 0.14f};
    std::array<ExtraLight, 3> extra{};
    float ambientScale = 1.0f; // multiplies the SH ambient and the environment (studio rigs)
    float exposure = 1.0f;
    float fogDensity = 1.0f / 650.0f;
    float fogStart = 25.0f;

    // Clear afternoon in the village (sun 44 deg up) with a cool rim light on characters.
    [[nodiscard]] static Lighting world();
    // Character select / create: key, fill and rim on a dark race-coloured backdrop.
    [[nodiscard]] static Lighting characterPreview(bool kargan);
};

class WorldRenderer
{
public:
    static constexpr std::size_t kMaxCharacters = 256;
    static constexpr std::size_t kMaxDecals = 512;
    static constexpr std::uint32_t kShadowMapSize = 2048;
    static constexpr std::uint32_t kShadowCascades = 2;

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
    // A free static mesh for this frame (M9: the Köz Oku bolt), drawn with the attachment path in
    // the main pass only (no shadow), culled by its bounding sphere.
    void addEffectMesh(const GpuMesh* mesh, const world::Mat4& world);
    void render(const RenderTargetViews& target);

    [[nodiscard]] const WorldRenderStats& stats() const noexcept { return m_stats; }
    [[nodiscard]] const world::Mat4& viewProjection() const noexcept { return m_viewProj; }
    // GPU memory owned by the renderer itself (shadow maps, terrain buffers, splat; bytes).
    [[nodiscard]] std::uint64_t gpuBytes() const noexcept;
    // One line describing the active settings (logged at init and by `scene graphics log`).
    [[nodiscard]] std::string describeSettings() const;

    GraphicsSettings settings;
    Lighting lighting = Lighting::world();

private:
    struct TerrainChunk
    {
        world::Vec3 min{};
        world::Vec3 max{};
        std::uint32_t baseVertex = 0;
    };
    struct PropInstance
    {
        std::array<float, 16> rows{}; // world matrix rows 0..2, then the tint (linear rgb, 1)
        world::Vec3 center{};         // bounding sphere (world)
        float radius = 0.0f;
        float maxDistance = 0.0f;
    };
    // Submeshes named "lod0" / "lod1" (tools/blender/gen_props.py) form a two-level LOD: lod0 is
    // drawn up to the LOD distance, lod1 beyond it; other submeshes are always drawn.
    enum class Lod : std::uint8_t
    {
        Always,
        Near,
        Far,
    };
    struct PropBatch
    {
        const GpuMesh* mesh = nullptr;
        std::vector<PropInstance> instances;
        std::vector<Lod> submeshLod; // per submesh
        bool hasLod = false;
        float lodDistance = 0.0f;
        std::uint32_t mainFirst = 0; // near (or all) instances
        std::uint32_t mainCount = 0;
        std::uint32_t farFirst = 0;  // instances beyond the LOD distance (hasLod only)
        std::uint32_t farCount = 0;
        std::array<std::uint32_t, kShadowCascades> shadowFirst{};
        std::array<std::uint32_t, kShadowCascades> shadowCount{};
    };

    bool createBuffer(UINT size, UINT bind, D3D11_USAGE usage, const void* init, const char* name,
                      Microsoft::WRL::ComPtr<ID3D11Buffer>& out, std::string& error);
    void updateEnvironment();
    void updateFrameConstants();
    void cullAndUploadProps();
    void renderShadowMap();
    void drawTerrain(int shadowCascade);
    void drawProps(int shadowCascade);
    void drawCharacters(int shadowCascade);
    // Draws the attached meshes queued by drawCharacters (static pipeline, one instance each).
    void drawAttachments(bool shadowPass);
    void drawDecals();
    void drawSky();
    // Writes the draw constants of a submesh (material + world/opacity already in `dc`) and binds
    // its three material textures.
    void bindSubmesh(const GpuSubmesh& s, const std::array<float, 4>& color, float character, float opacity,
                     const world::Mat4& worldMatrix);
    void drawPropRange(const PropBatch& b, std::uint32_t first, std::uint32_t count, Lod skip, bool shadowPass);
    template <class T>
    void writeConstants(ID3D11Buffer* buffer, const T& data);
    void bindCommon();
    [[nodiscard]] float cascadeRadius(std::uint32_t cascade) const noexcept;

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
    Microsoft::WRL::ComPtr<ID3D11Buffer> m_passCb;
    Texture2D m_white;

    // Shadow cascades (R32_TYPELESS array: DSV D32_FLOAT per slice, SRV R32_FLOAT array).
    Microsoft::WRL::ComPtr<ID3D11Texture2D> m_shadowTex;
    std::array<Microsoft::WRL::ComPtr<ID3D11DepthStencilView>, kShadowCascades> m_shadowDsv;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_shadowSrv;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> m_dummyShadowTex; // bound while the shadow map is written
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_dummyShadowSrv;

    // Terrain.
    Microsoft::WRL::ComPtr<ID3D11Buffer> m_terrainVb;
    Microsoft::WRL::ComPtr<ID3D11Buffer> m_terrainIb;
    std::uint32_t m_chunkIndexCount = 0;
    std::vector<TerrainChunk> m_chunks;
    Texture2D m_splat;
    Texture2D m_flatNormal; // 1x1 (0.5, 0.5, 1): layers without a normal map
    std::array<const Texture2D*, 4> m_layers{};
    std::array<const Texture2D*, 4> m_layerNormals{};
    std::array<const Texture2D*, 4> m_layerOrm{};
    std::array<float, 4> m_layerTiling{};
    float m_worldSize = 0.0f;
    bool m_hasTerrain = false;
    std::uint64_t m_terrainBytes = 0;

    // Props.
    std::vector<PropBatch> m_batches;
    Microsoft::WRL::ComPtr<ID3D11Buffer> m_instanceVb;
    std::size_t m_instanceCapacity = 0; // instances (main + shadow regions)
    std::vector<float> m_instanceScratch;
    std::array<ID3D11RasterizerState*, 2> m_propRaster{}; // [0] culled (or shadow caster), [1] double-sided

    // Environment (SH irradiance of the sky, ground radiance), recomputed when the rig changes.
    Lighting m_envFor{};
    bool m_envValid = false;
    std::array<std::array<float, 4>, 9> m_sh{};
    std::array<float, 3> m_groundRadiance{};

    // Per frame.
    ViewSetup m_view;
    world::Mat4 m_viewProj;
    world::Mat4 m_invViewProj;
    std::array<world::Mat4, kShadowCascades> m_shadowViewProj{};
    world::Frustum m_frustum;
    std::array<world::Frustum, kShadowCascades> m_shadowFrustum{};
    std::uint32_t m_cascades = 0; // active this frame (0 = no shadows)
    bool m_shadowsThisFrame = false;
    std::vector<CharacterDraw> m_characters;
    std::vector<DecalDraw> m_decals;
    // Attached meshes of the characters that passed culling in the current pass.
    struct QueuedAttachment
    {
        const GpuMesh* mesh = nullptr;
        std::array<float, 16> rows{}; // instance rows + tint
        float opacity = 1.0f;
    };
    std::vector<QueuedAttachment> m_attachQueue;
    std::vector<AttachedMesh> m_effects; // addEffectMesh, this frame
    Microsoft::WRL::ComPtr<ID3D11Buffer> m_attachVb; // kMaxCharacters * 2 instances
    WorldRenderStats m_stats;
};

} // namespace vo::client::render
