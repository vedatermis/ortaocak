#include "client/render/AssetCache.h"

#include "shared/assets/AssetFile.h"
#include "shared/core/Clock.h"
#include "shared/core/Log.h"
#include "shared/core/Paths.h"
#include "shared/game/Character.h"

#include <algorithm>

namespace vo::client::render {

namespace {

const char* kindName(AssetCache::Kind k)
{
    switch (k)
    {
    case AssetCache::Kind::Mesh:
        return "mesh";
    case AssetCache::Kind::Texture:
        return "texture";
    case AssetCache::Kind::Animation:
        return "animation";
    case AssetCache::Kind::Terrain:
        return "terrain";
    }
    return "?";
}

std::size_t raceSlot(std::uint8_t race)
{
    return game::isValidRace(race) ? race : 0;
}

} // namespace

AssetCache::AssetCache() = default;

AssetCache::~AssetCache()
{
    shutdown();
}

bool AssetCache::init(ID3D11Device* device, ID3D11DeviceContext* context, std::string& error)
{
    m_device = device;
    m_context = context;
    if (auto marker = vo::core::locateDataFile("assets/terrain/zone1/zone1.vterrain"))
    {
        m_root = marker->parent_path().parent_path().parent_path();
        VO_LOG_INFO("assets: root {}", vo::core::pathToUtf8(m_root));
    }
    else
    {
        VO_LOG_WARN("assets: no assets/ directory found (build it with `assetc build assets_src assets`); "
                    "the world and the 3D preview are unavailable");
    }
    try
    {
        m_stop = false;
        m_worker = std::thread([this]() { workerMain(); });
    }
    catch (const std::exception& e)
    {
        error = std::string("asset worker thread: ") + e.what();
        return false;
    }
    return true;
}

void AssetCache::shutdown() noexcept
{
    {
        std::lock_guard lock(m_mutex);
        m_stop = true;
        m_jobs.clear();
    }
    m_wake.notify_all();
    if (m_worker.joinable())
        m_worker.join();
    m_results.clear();
    m_raceClips = {};
    m_entries.clear();
    m_progress = {};
    m_device = nullptr;
    m_context = nullptr;
}

void AssetCache::workerMain()
{
    for (;;)
    {
        Job job;
        {
            std::unique_lock lock(m_mutex);
            m_wake.wait(lock, [this]() { return m_stop || !m_jobs.empty(); });
            if (m_stop)
                return;
            job = std::move(m_jobs.front());
            m_jobs.pop_front();
        }
        auto result = std::make_unique<Result>();
        load(job, *result);
        std::lock_guard lock(m_mutex);
        if (m_stop)
            return;
        m_results.push_back(std::move(result));
    }
}

void AssetCache::load(const Job& job, Result& out) const
{
    out.kind = job.kind;
    out.path = job.path;
    if (m_root.empty())
    {
        out.error = "no assets directory";
        return;
    }
    if (!assets::isValidAssetPath(job.path))
    {
        out.error = "invalid asset path";
        return;
    }
    const std::filesystem::path file = m_root / vo::core::pathFromUtf8(job.path);
    switch (job.kind)
    {
    case Kind::Mesh:
    {
        auto mesh = std::make_shared<assets::MeshData>();
        if (!assets::loadMesh(file, *mesh, out.error))
            return;
        convertVertices(*mesh, out.vertices);
        out.mesh = std::move(mesh);
        out.ok = true;
        return;
    }
    case Kind::Texture:
        out.ok = Texture2D::decodeFile(file, out.image, out.error);
        return;
    case Kind::Animation:
        out.animation = std::make_unique<assets::AnimationFile>();
        out.ok = assets::loadAnimation(file, *out.animation, out.error);
        return;
    case Kind::Terrain:
        out.terrain = std::make_unique<assets::TerrainData>();
        out.ok = assets::loadTerrain(file, *out.terrain, out.error);
        return;
    }
}

void AssetCache::request(Kind kind, std::string_view path)
{
    if (m_entries.find(path) != m_entries.end())
        return;
    Entry e;
    e.kind = kind;
    m_entries.emplace(std::string(path), std::move(e));
    ++m_progress.requested;
    {
        std::lock_guard lock(m_mutex);
        m_jobs.push_back(Job{kind, std::string(path)});
    }
    m_wake.notify_one();
}

void AssetCache::finish(Result& r)
{
    auto it = m_entries.find(std::string_view(r.path));
    if (it == m_entries.end())
        return;
    Entry& e = it->second;
    if (e.state != State::Queued)
        return; // already loaded synchronously
    std::string error = r.error;
    bool ok = r.ok;
    if (ok)
    {
        switch (r.kind)
        {
        case Kind::Mesh:
        {
            auto gpu = std::make_unique<GpuMesh>();
            ok = uploadMesh(m_device, r.mesh, r.vertices, r.path.c_str(), *gpu, error);
            if (ok)
                e.mesh = std::move(gpu);
            break;
        }
        case Kind::Texture:
        {
            auto tex = std::make_unique<Texture2D>();
            Texture2D::Options opts;
            opts.srgb = true;
            opts.premultiply = false; // opaque colour textures
            opts.generateMips = true;
            opts.debugName = r.path.c_str();
            ok = tex->createFromRgba(m_device, m_context, r.image.width, r.image.height, r.image.rgba.data(), opts,
                                     error);
            if (ok)
                e.texture = std::move(tex);
            break;
        }
        case Kind::Animation:
            e.animation = std::move(r.animation);
            break;
        case Kind::Terrain:
            e.terrain = std::move(r.terrain);
            break;
        }
    }
    e.state = ok ? State::Ready : State::Failed;
    ++m_progress.done;
    if (!ok)
    {
        ++m_progress.failed;
        VO_LOG_ERROR("assets: {} {}: {}", kindName(r.kind), r.path, error);
    }
    else
    {
        VO_LOG_DEBUG("assets: loaded {} {}", kindName(r.kind), r.path);
    }
    if (ok && r.kind == Kind::Mesh)
    {
        for (const GpuSubmesh& s : e.mesh->submeshes)
        {
            if (!s.texturePath.empty())
                request(Kind::Texture, s.texturePath);
        }
    }
    if (ok && r.kind == Kind::Texture)
        resolveTextures();
}

void AssetCache::pump(double budgetSeconds)
{
    const vo::core::Stopwatch watch;
    for (;;)
    {
        std::unique_ptr<Result> r;
        {
            std::lock_guard lock(m_mutex);
            if (m_results.empty())
                return;
            r = std::move(m_results.front());
            m_results.pop_front();
        }
        finish(*r);
        if (watch.elapsedSeconds() >= budgetSeconds)
            return;
    }
}

void AssetCache::resolveTextures()
{
    for (auto& [path, e] : m_entries)
    {
        if (!e.mesh)
            continue;
        for (GpuSubmesh& s : e.mesh->submeshes)
        {
            if (!s.texturePath.empty() && s.texture == nullptr)
                s.texture = texture(s.texturePath);
        }
    }
}

const AssetCache::Entry* AssetCache::find(std::string_view path) const
{
    const auto it = m_entries.find(path);
    if (it == m_entries.end() || it->second.state != State::Ready)
        return nullptr;
    return &it->second;
}

const GpuMesh* AssetCache::mesh(std::string_view path) const
{
    const Entry* e = find(path);
    return e != nullptr ? e->mesh.get() : nullptr;
}

const Texture2D* AssetCache::texture(std::string_view path) const
{
    const Entry* e = find(path);
    return e != nullptr ? e->texture.get() : nullptr;
}

const assets::AnimationFile* AssetCache::animation(std::string_view path) const
{
    const Entry* e = find(path);
    return e != nullptr ? e->animation.get() : nullptr;
}

const assets::TerrainData* AssetCache::terrain(std::string_view path) const
{
    const Entry* e = find(path);
    return e != nullptr ? e->terrain.get() : nullptr;
}

bool AssetCache::failed(std::string_view path) const
{
    const auto it = m_entries.find(path);
    return it != m_entries.end() && it->second.state == State::Failed;
}

const GpuMesh* AssetCache::loadMeshNow(std::string_view path)
{
    auto it = m_entries.find(path);
    if (it != m_entries.end() && it->second.state != State::Queued)
        return it->second.mesh.get();
    if (it == m_entries.end())
    {
        Entry e;
        e.kind = Kind::Mesh;
        it = m_entries.emplace(std::string(path), std::move(e)).first;
        ++m_progress.requested;
    }
    Result r;
    load(Job{Kind::Mesh, std::string(path)}, r);
    finish(r); // a queued worker result for the same path is ignored later (state no longer Queued)
    // finish() only queued the submesh textures; screens using this path (character preview) do not
    // pump worker results, so load them synchronously too.
    if (const GpuMesh* m = mesh(path))
        for (const GpuSubmesh& s : m->submeshes)
            if (!s.texturePath.empty() && s.texture == nullptr)
            {
                Result tr;
                load(Job{Kind::Texture, s.texturePath}, tr);
                finish(tr);
            }
    return mesh(path);
}

const assets::AnimationFile* AssetCache::loadAnimationNow(std::string_view path)
{
    auto it = m_entries.find(path);
    if (it != m_entries.end() && it->second.state != State::Queued)
        return it->second.animation.get();
    if (it == m_entries.end())
    {
        Entry e;
        e.kind = Kind::Animation;
        it = m_entries.emplace(std::string(path), std::move(e)).first;
        ++m_progress.requested;
    }
    Result r;
    load(Job{Kind::Animation, std::string(path)}, r);
    finish(r);
    return animation(path);
}

const char* AssetCache::characterMeshPath(std::uint8_t race) noexcept
{
    return race == static_cast<std::uint8_t>(game::Race::Kargan) ? "characters/kargan.vmesh"
                                                                 : "characters/aydor.vmesh";
}

std::array<std::string, 3> AssetCache::characterClipPaths(std::uint8_t race)
{
    const std::string base = race == static_cast<std::uint8_t>(game::Race::Kargan) ? "characters/kargan/"
                                                                                    : "characters/aydor/";
    return {base + "idle.vanim", base + "walk.vanim", base + "run.vanim"};
}

void AssetCache::requestCharacter(std::uint8_t race)
{
    request(Kind::Mesh, characterMeshPath(race));
    for (const std::string& p : characterClipPaths(race))
        request(Kind::Animation, p);
}

void AssetCache::loadCharacterNow(std::uint8_t race)
{
    (void)loadMeshNow(characterMeshPath(race));
    for (const std::string& p : characterClipPaths(race))
        (void)loadAnimationNow(p);
}

const world::CharacterClips* AssetCache::characterClips(std::uint8_t race)
{
    RaceClips& rc = m_raceClips[raceSlot(race)];
    if (rc.bound)
        return &rc.clips;
    const GpuMesh* m = mesh(characterMeshPath(race));
    if (m == nullptr || !m->cpu)
        return nullptr;
    const auto paths = characterClipPaths(race);
    const assets::AnimationFile* idle = animation(paths[0]);
    const assets::AnimationFile* walk = animation(paths[1]);
    const assets::AnimationFile* run = animation(paths[2]);
    // Wait until every clip is ready or has failed (a missing clip falls back to another one).
    for (const std::string& p : paths)
    {
        if (animation(p) == nullptr && !failed(p))
            return nullptr;
    }
    auto first = [](const assets::AnimationFile* f) -> const assets::AnimationClip*
    { return f != nullptr && !f->clips.empty() ? &f->clips.front() : nullptr; };
    rc.clips.idle = first(idle);
    rc.clips.walk = first(walk);
    rc.clips.run = first(run);
    if (rc.clips.idle == nullptr)
        rc.clips.idle = rc.clips.walk != nullptr ? rc.clips.walk : rc.clips.run;
    rc.clips.bind(m->cpu->skeleton);
    rc.bound = true;
    return &rc.clips;
}

} // namespace vo::client::render
