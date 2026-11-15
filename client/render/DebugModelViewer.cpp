#include "client/render/DebugModelViewer.h"

#include "client/render/AssetCache.h"
#include "client/render/RenderDevice.h"
#include "client/render/WorldRenderer.h"
#include "shared/core/Log.h"

#include <Windows.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <format>

namespace vo::client::render {

using world::Mat4;
using world::Vec3;

namespace {

constexpr const char* kTerrainPath = "terrain/zone1/zone1.vterrain";

bool waitIdle(AssetCache& cache, double timeoutSeconds)
{
    for (double t = 0.0; t < timeoutSeconds; t += 0.002)
    {
        cache.pump(0.05);
        if (cache.idle())
            return true;
        ::Sleep(2);
    }
    return cache.idle();
}

double mib(std::uint64_t bytes)
{
    return static_cast<double>(bytes) / (1024.0 * 1024.0);
}

} // namespace

DebugModelViewer::DebugModelViewer() = default;

DebugModelViewer::~DebugModelViewer()
{
    shutdown();
}

bool DebugModelViewer::buildSkinnedCopy(const GpuMesh& src, GpuMesh& out, std::string& error)
{
    // The raw meshes have no skeleton: give them a 4-bone identity skin (4 influences per vertex) so
    // they take the character path at the cost of real skinning.
    const assets::MeshData& m = *src.cpu;
    auto skinned = std::make_shared<assets::MeshData>();
    skinned->vertexFormat = m.vertexFormat | assets::kVertexSkin;
    skinned->vertexCount = m.vertexCount;
    const assets::VertexLayout l = skinned->layout();
    skinned->vertices.assign(static_cast<std::size_t>(m.vertexCount) * l.stride, 0);
    for (std::uint32_t v = 0; v < m.vertexCount; ++v)
    {
        std::uint8_t* d = skinned->vertices.data() + static_cast<std::size_t>(v) * l.stride;
        const Vec3 p = m.position(v), n = m.normal(v);
        const auto uv = m.uv0(v);
        std::memcpy(d + l.position, &p, 12);
        std::memcpy(d + l.normal, &n, 12);
        std::memcpy(d + l.uv0, uv.data(), 8);
        if (l.color != assets::kAbsent)
        {
            const auto c = m.color(v);
            std::memcpy(d + l.color, c.data(), 4);
        }
        const std::uint8_t joints[4] = {0, 1, 2, 3};
        const std::uint8_t weights[4] = {64, 64, 64, 63};
        std::memcpy(d + l.joints, joints, 4);
        std::memcpy(d + l.weights, weights, 4);
        if (m.hasTangents())
            skinned->setTangent(v, m.tangent(v));
    }
    skinned->indexSize = m.indexSize;
    skinned->indices = m.indices;
    skinned->submeshes = m.submeshes;
    skinned->boundsMin = m.boundsMin;
    skinned->boundsMax = m.boundsMax;
    skinned->sphereCenter = m.sphereCenter;
    skinned->sphereRadius = m.sphereRadius;
    for (int b = 0; b < 4; ++b)
    {
        assets::Bone bone;
        bone.name = std::format("b{}", b);
        skinned->skeleton.bones.push_back(bone);
    }
    std::vector<std::uint8_t> vertices;
    convertVertices(*skinned, vertices);
    if (!uploadMesh(m_device->device(), skinned, vertices, "debug.model", out, error))
        return false;
    for (std::size_t i = 0; i < out.submeshes.size() && i < src.submeshes.size(); ++i)
    {
        out.submeshes[i].texture = src.submeshes[i].texture;
        out.submeshes[i].normalTexture = src.submeshes[i].normalTexture;
        out.submeshes[i].ormTexture = src.submeshes[i].ormTexture;
    }
    return true;
}

bool DebugModelViewer::init(RenderDevice& device, AssetCache& cache, WorldRenderer& renderer,
                            const DebugViewerOptions& options, std::string& error)
{
    m_device = &device;
    m_cache = &cache;
    m_renderer = &renderer;
    m_options = options;
    m_world = options.light == "world";
    m_kargan = options.light == "preview-kargan";
    if (!m_world && options.light != "preview-aydor" && !m_kargan)
        return error = "--debug-light must be world, preview-aydor or preview-kargan", false;
    if (options.models.empty())
        return error = "no --debug-model given", false;
    for (int i = 0; i < 4; ++i)
        m_palette[static_cast<std::size_t>(i)] = Mat4::identity();

    // Terrain + props (world light only).
    m_base = {238.0f, 0.0f, 271.0f}; // spawn_aydor
    if (m_world)
    {
        cache.request(AssetCache::Kind::Terrain, kTerrainPath);
        if (!waitIdle(cache, 60.0) || cache.terrain(kTerrainPath) == nullptr)
            return error = "zone 1 terrain did not load", false;
        const assets::TerrainData& t = *cache.terrain(kTerrainPath);
        for (const assets::TerrainLayer& layer : t.layers)
            cache.request(AssetCache::Kind::Texture, layer.texture);
        for (const assets::ObjectPlacement& o : t.objects)
            cache.request(AssetCache::Kind::Mesh, o.mesh);
        if (!waitIdle(cache, 120.0))
            return error = "zone 1 props did not load", false;
        if (!renderer.setTerrain(t, cache, error))
            return false;
        if (options.at)
            m_base = {options.at->first, 0.0f, options.at->second};
        else if (const assets::SpawnPoint* s = t.findSpawn("spawn_aydor"))
            m_base = s->position;
        m_base.y = t.heightAt(m_base.x, m_base.z);
    }
    else
    {
        m_base = {0.0f, 0.0f, 0.0f};
    }

    // Models.
    std::vector<const GpuMesh*> kinds;
    for (const std::string& path : options.models)
    {
        const GpuMesh* src = cache.loadMeshNow(path);
        if (src == nullptr || !src->cpu)
            return error = std::format("debug model {} did not load", path), false;
        auto copy = std::make_unique<GpuMesh>();
        if (!buildSkinnedCopy(*src, *copy, error))
            return false;
        kinds.push_back(copy.get());
        m_meshes.push_back(std::move(copy));
    }
    waitIdle(cache, 60.0); // textures requested by the meshes
    for (std::size_t i = 0; i < m_meshes.size(); ++i)
    {
        const GpuMesh* src = cache.mesh(options.models[i]);
        for (std::size_t s = 0; src != nullptr && s < m_meshes[i]->submeshes.size(); ++s)
        {
            m_meshes[i]->submeshes[s].texture = src->submeshes[s].texture;
            m_meshes[i]->submeshes[s].normalTexture = src->submeshes[s].normalTexture;
            m_meshes[i]->submeshes[s].ormTexture = src->submeshes[s].ormTexture;
        }
    }
    const std::uint32_t count = options.count > 0 ? options.count : static_cast<std::uint32_t>(kinds.size());
    const auto columns = static_cast<std::uint32_t>(std::ceil(std::sqrt(static_cast<double>(count))));
    const float yaw = options.modelYawDeg * world::kPi / 180.0f;
    const Vec3 right{std::cos(yaw), 0.0f, -std::sin(yaw)}; // the model's right-hand side... in world x/z
    const Vec3 back{-std::sin(yaw), 0.0f, -std::cos(yaw)};
    m_modelHeight = 0.0f;
    for (std::uint32_t i = 0; i < count; ++i)
    {
        const GpuMesh* mesh = kinds[i % kinds.size()];
        const assets::MeshData& m = *mesh->cpu;
        m_modelHeight = std::max(m_modelHeight, m.boundsMax.y - m.boundsMin.y);
        // First row side by side facing the camera side, further rows behind.
        const float col = static_cast<float>(i % columns) - static_cast<float>(std::min(count, columns) - 1) * 0.5f;
        const float row = static_cast<float>(i / columns);
        Vec3 p = m_base + right * (col * 1.6f) + back * (row * 1.8f);
        if (m_world && m_renderer != nullptr)
        {
            const assets::TerrainData* t = cache.terrain(kTerrainPath);
            p.y = t != nullptr ? t->heightAt(p.x, p.z) : m_base.y;
        }
        p.y -= m.boundsMin.y; // feet on the ground (raw meshes are centred on their bounds)
        m_instances.push_back({mesh, world::makeWorld(p, yaw - options.facingDeg * world::kPi / 180.0f, 1.0f)});
    }
    VO_LOG_INFO("viewer: {} model(s), {} instance(s), {} light at ({:.1f}, {:.1f}, {:.1f}), model height {:.2f} m",
                kinds.size(), m_instances.size(), options.light, m_base.x, m_base.y, m_base.z, m_modelHeight);
    return true;
}

void DebugModelViewer::shutdown() noexcept
{
    m_instances.clear();
    m_meshes.clear();
    m_device = nullptr;
    m_cache = nullptr;
    m_renderer = nullptr;
}

void DebugModelViewer::update(double dt, double realDt)
{
    m_time += dt;
    m_perfTime += realDt;
    ++m_perfFrames;
}

void DebugModelViewer::render()
{
    if (m_renderer == nullptr || m_instances.empty())
        return;
    WorldRenderer& r = *m_renderer;
    RenderDevice& device = *m_device;
    r.lighting = m_world ? Lighting::world() : Lighting::characterPreview(m_kargan);

    // Camera relative to the first instance.
    struct Cam
    {
        float yaw, pitch, dist, height;
    };
    Cam cam{35.0f, 8.0f, 3.4f, 0.52f};
    const std::string& c = m_options.camera;
    if (c == "front")
        cam = {0.0f, 6.0f, 3.3f, 0.52f};
    else if (c == "threeq")
        cam = {40.0f, 8.0f, 3.3f, 0.52f};
    else if (c == "side")
        cam = {90.0f, 6.0f, 3.3f, 0.52f};
    else if (c == "back")
        cam = {180.0f, 8.0f, 3.3f, 0.52f};
    else if (c == "face")
        cam = {25.0f, 4.0f, 0.75f, 0.90f};
    else if (c == "armor")
        cam = {35.0f, 10.0f, 1.25f, 0.70f};
    else if (c == "wide")
        cam = {30.0f, 14.0f, 9.0f, 0.45f};
    else
    {
        float v[4] = {cam.yaw, cam.pitch, cam.dist, cam.height};
        if (sscanf_s(c.c_str(), "%f,%f,%f,%f", &v[0], &v[1], &v[2], &v[3]) == 4)
            cam = {v[0], v[1], v[2], v[3]};
    }
    cam.yaw += static_cast<float>(m_time) * m_options.orbitDegPerSecond;
    const Mat4& w0 = m_instances.front().world;
    const Vec3 origin{w0.at(0, 3), w0.at(1, 3), w0.at(2, 3)};
    const float feet = origin.y + m_instances.front().mesh->cpu->boundsMin.y;
    const Vec3 target{origin.x, feet + m_modelHeight * cam.height, origin.z};
    const float modelYaw = m_options.modelYawDeg * world::kPi / 180.0f;
    const float a = modelYaw + cam.yaw * world::kPi / 180.0f;
    const float p = cam.pitch * world::kPi / 180.0f;
    const Vec3 dir{std::sin(a) * std::cos(p), std::sin(p), std::cos(a) * std::cos(p)};
    const Vec3 eye = target + dir * cam.dist;

    const float aspect = static_cast<float>(device.width()) / static_cast<float>(std::max(device.height(), 1u));
    ViewSetup view;
    view.view = world::lookAtLH(eye, target, {0.0f, 1.0f, 0.0f});
    view.projection = world::perspectiveReverseZ(50.0f * world::kPi / 180.0f, aspect, 0.05f, 1500.0f);
    if (!m_world)
        view.projection = world::perspectiveReverseZ(30.0f * world::kPi / 180.0f, aspect, 0.05f, 100.0f);
    view.eye = eye;
    view.time = static_cast<float>(m_time);
    view.drawTerrain = m_world;
    view.shadows = m_options.shadows;
    view.shadowFocus = m_world ? origin : Vec3{origin.x, feet + m_modelHeight * 0.5f, origin.z};
    view.shadowScale = m_world ? 1.0f : 0.09f;
    r.beginFrame(view);
    for (const Instance& inst : m_instances)
    {
        CharacterDraw d;
        d.mesh = inst.mesh;
        d.skin = m_palette;
        d.world = inst.world;
        r.addCharacter(d);
        DecalDraw blob;
        blob.center = {inst.world.at(0, 3), feet, inst.world.at(2, 3)};
        blob.radius = 0.62f;
        blob.color = {0.0f, 0.0f, 0.0f, m_world ? 0.42f : 0.6f};
        r.addDecal(blob);
    }
    RenderTargetViews views;
    views.color = device.sceneColorView();
    views.depth = device.sceneDepthView();
    views.width = device.width();
    views.height = device.height();
    constexpr float kClear[4] = {0.03f, 0.04f, 0.05f, 1.0f};
    device.beginScene(kClear);
    r.render(views);
    device.resolveScene();

    if (m_perfTime >= 1.0)
    {
        const WorldRenderStats& s = r.stats();
        const AssetCache::Stats& cs = m_cache->stats();
        std::uint64_t viewerMeshes = 0;
        for (const auto& m : m_meshes)
            viewerMeshes += m->gpuBytes;
        VO_LOG_INFO("viewer: perf {:.1f} fps ({:.2f} ms), draw calls {} + {} shadow, triangles {}k + {}k shadow, "
                    "{} characters; GPU textures {:.1f} MiB, meshes {:.1f} MiB, renderer {:.1f} MiB, scene targets "
                    "{:.1f} MiB",
                    m_perfFrames / m_perfTime, 1000.0 * m_perfTime / m_perfFrames, s.drawCalls, s.shadowDrawCalls,
                    s.triangles / 1000, s.shadowTriangles / 1000, s.characters, mib(cs.textureBytes),
                    mib(cs.meshBytes + viewerMeshes), mib(r.gpuBytes()), mib(device.sceneTargetBytes()));
        m_perfTime = 0.0;
        m_perfFrames = 0;
    }
}

} // namespace vo::client::render
