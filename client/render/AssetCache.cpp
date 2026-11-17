#include "client/render/AssetCache.h"

#include "shared/assets/AssetFile.h"
#include "shared/core/Clock.h"
#include "shared/core/Log.h"
#include "shared/core/Paths.h"
#include "shared/game/Character.h"

#include <algorithm>
#include <format>
#include <utility>

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
    case AssetCache::Kind::NormalMap:
        return "normal map";
    case AssetCache::Kind::DataMap:
        return "data map";
    case AssetCache::Kind::UiImage:
        return "ui image";
    }
    return "?";
}

bool isTextureKind(AssetCache::Kind k)
{
    return k == AssetCache::Kind::Texture || k == AssetCache::Kind::NormalMap || k == AssetCache::Kind::DataMap ||
           k == AssetCache::Kind::UiImage;
}

assets::TextureUsage usageOf(AssetCache::Kind k)
{
    switch (k)
    {
    case AssetCache::Kind::NormalMap: return assets::TextureUsage::Normal;
    case AssetCache::Kind::DataMap: return assets::TextureUsage::Data;
    default: return assets::TextureUsage::Color;
    }
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
        const std::filesystem::path root = m_root;
        m_looks.discover(
            [&root](std::string_view path)
            {
                std::error_code ec;
                return assets::isValidAssetPath(path) &&
                       std::filesystem::is_regular_file(root / vo::core::pathFromUtf8(path), ec);
            });
        for (const std::uint8_t race : {static_cast<std::uint8_t>(game::Race::Aydor),
                                        static_cast<std::uint8_t>(game::Race::Kargan)})
        {
            std::string list;
            for (int look = 0; look < world::kMaxLooks; ++look)
            {
                if (m_looks.available(race, look))
                    list += (list.empty() ? "" : ", ") + m_looks.files(race, look).mesh;
            }
            VO_LOG_INFO("assets: {} look(s) for race {}: {}", m_looks.lookCount(race), race,
                        list.empty() ? "none" : list);
        }
    }
    else
    {
        VO_LOG_WARN("assets: no assets/ directory found (build it with `assetc build assets_src assets`); "
                    "the world and the 3D preview are unavailable");
    }
    try
    {
        m_stop = false;
        // Several workers: a zone needs ~60 block-compressed maps (read + CRC + parse each); one
        // thread made that the bulk of the zone load time.
        const unsigned workers = std::clamp(std::thread::hardware_concurrency() / 2u, 1u, 4u);
        for (unsigned w = 0; w < workers; ++w)
            m_workers.emplace_back([this]() { workerMain(); });
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
    for (std::thread& w : m_workers)
        if (w.joinable())
            w.join();
    m_workers.clear();
    m_results.clear();
    m_raceClips = {};
    m_monsters.clear();
    m_entries.clear();
    m_progress = {};
    m_stats = {};
    m_loggedIdle = true;
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
    case Kind::NormalMap:
    case Kind::DataMap:
    case Kind::UiImage:
    {
        const vo::core::Stopwatch watch;
        const std::string compiledPath = assets::compiledTexturePath(job.path);
        const std::filesystem::path compiled = m_root / vo::core::pathFromUtf8(compiledPath);
        std::error_code ec;
        if (m_preferCompiled && compiledPath != job.path && std::filesystem::is_regular_file(compiled, ec))
        {
            out.compiled = std::make_unique<assets::TextureData>();
            out.ok = assets::loadTexture(compiled, *out.compiled, out.error);
            if (out.ok && out.compiled->usage != usageOf(job.kind))
            {
                out.ok = false;
                out.error = std::format("{} is a {} texture, requested as {}", compiledPath,
                                        assets::textureUsageName(out.compiled->usage), kindName(job.kind));
            }
        }
        else
        {
            out.ok = Texture2D::decodeFile(file, out.image, out.error);
        }
        out.loadSeconds = watch.elapsedSeconds();
        return;
    }
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
            {
                ++m_stats.meshes;
                m_stats.meshBytes += gpu->gpuBytes;
                e.mesh = std::move(gpu);
            }
            break;
        }
        case Kind::Texture:
        case Kind::NormalMap:
        case Kind::DataMap:
        case Kind::UiImage:
        {
            const vo::core::Stopwatch watch;
            auto tex = std::make_unique<Texture2D>();
            if (r.compiled)
            {
                ok = tex->createFromTextureData(m_device, *r.compiled, r.path.c_str(), error);
            }
            else
            {
                Texture2D::Options opts;
                opts.srgb = r.kind == Kind::Texture || r.kind == Kind::UiImage;
                // Opaque colour textures / data as they are; UI images with alpha premultiplied (the
                // sprite batch blends ONE, INV_SRC_ALPHA; a compiled .vtex icon already is).
                opts.premultiply = r.kind == Kind::UiImage;
                opts.generateMips = true;
                opts.debugName = r.path.c_str();
                ok = tex->createFromRgba(m_device, m_context, r.image.width, r.image.height, r.image.rgba.data(), opts,
                                         error);
            }
            if (ok)
            {
                ++m_stats.textures;
                m_stats.texturesFromVtex += r.compiled ? 1 : 0;
                m_stats.textureBytes += tex->gpuBytes();
                m_stats.textureLoadSeconds += r.loadSeconds;
                m_stats.textureUploadSeconds += watch.elapsedSeconds();
                e.texture = std::move(tex);
            }
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
            if (!s.normalTexturePath.empty())
                request(Kind::NormalMap, s.normalTexturePath);
            if (!s.ormTexturePath.empty())
                request(Kind::DataMap, s.ormTexturePath);
            if (!s.emissiveTexturePath.empty())
                request(Kind::Texture, s.emissiveTexturePath);
        }
        // A texture another mesh already loaded (two looks sharing one) is not loaded again, so
        // its finish() never comes: hook it up now.
        resolveTextures();
    }
    // A terrain's layer normal / ORM maps (.vterrain v2) are queued here with their linear kinds, so
    // whoever waits for the terrain's assets (cache idle) also waits for them; the base colours are
    // requested by the caller (and here, harmlessly, as well).
    if (ok && r.kind == Kind::Terrain && e.terrain)
        for (const assets::TerrainLayer& l : e.terrain->layers)
        {
            request(Kind::Texture, l.texture);
            if (!l.normalTexture.empty())
                request(Kind::NormalMap, l.normalTexture);
            if (!l.ormTexture.empty())
                request(Kind::DataMap, l.ormTexture);
        }
    if (ok && isTextureKind(r.kind))
        resolveTextures();
}

void AssetCache::logStats() const
{
    const Stats& s = m_stats;
    VO_LOG_INFO("assets: loaded {} texture(s) ({} from .vtex, {} decoded from PNG), {:.1f} MiB GPU; worker "
                "read+decode {:.1f} ms, upload {:.1f} ms; {} mesh(es) {:.1f} MiB",
                s.textures, s.texturesFromVtex, s.textures - s.texturesFromVtex,
                static_cast<double>(s.textureBytes) / (1024.0 * 1024.0), s.textureLoadSeconds * 1000.0,
                s.textureUploadSeconds * 1000.0, s.meshes, static_cast<double>(s.meshBytes) / (1024.0 * 1024.0));
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
                break;
            r = std::move(m_results.front());
            m_results.pop_front();
        }
        m_loggedIdle = false;
        finish(*r);
        if (watch.elapsedSeconds() >= budgetSeconds)
            break;
    }
    if (!m_loggedIdle && idle())
    {
        m_loggedIdle = true;
        logStats();
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
            if (!s.normalTexturePath.empty() && s.normalTexture == nullptr)
                s.normalTexture = texture(s.normalTexturePath);
            if (!s.ormTexturePath.empty() && s.ormTexture == nullptr)
                s.ormTexture = texture(s.ormTexturePath);
            if (!s.emissiveTexturePath.empty() && s.emissiveTexture == nullptr)
                s.emissiveTexture = texture(s.emissiveTexturePath);
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
        {
            const std::pair<Kind, const std::string*> maps[4] = {{Kind::Texture, &s.texturePath},
                                                                 {Kind::NormalMap, &s.normalTexturePath},
                                                                 {Kind::DataMap, &s.ormTexturePath},
                                                                 {Kind::Texture, &s.emissiveTexturePath}};
            for (const auto& [kind, texPath] : maps)
            {
                if (texPath->empty() || texture(*texPath) != nullptr || failed(*texPath))
                    continue;
                const auto te = m_entries.find(std::string_view(*texPath));
                if (te != m_entries.end() && te->second.state != State::Queued)
                    continue;
                Result tr;
                load(Job{kind, *texPath}, tr);
                finish(tr);
            }
        }
    resolveTextures(); // textures that were already loaded (finish() skips those)
    return mesh(path);
}

const Texture2D* AssetCache::loadTextureNow(Kind kind, std::string_view path)
{
    if (!isTextureKind(kind))
        return nullptr;
    auto it = m_entries.find(path);
    if (it != m_entries.end() && it->second.state != State::Queued)
        return it->second.texture.get();
    if (it == m_entries.end())
    {
        Entry e;
        e.kind = kind;
        it = m_entries.emplace(std::string(path), std::move(e)).first;
        ++m_progress.requested;
    }
    Result r;
    load(Job{kind, std::string(path)}, r);
    finish(r);
    return texture(path);
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

const std::string& AssetCache::characterMeshPath(std::uint8_t race, int look) const noexcept
{
    return m_looks.files(race, look).mesh;
}

void AssetCache::requestCharacter(std::uint8_t race)
{
    for (int look = 0; look < world::kMaxLooks; ++look)
    {
        if (!m_looks.available(race, look) && !(look == 0 && m_looks.lookCount(race) == 0))
            continue; // (no look at all: still request the legacy names so the failure is logged)
        const world::LookFiles& f = m_looks.files(race, look);
        request(Kind::Mesh, f.mesh);
        for (std::size_t c = 0; c < world::kCharacterClipCount; ++c)
            if (wantClip(f, c))
                request(Kind::Animation, f.clips[c]);
    }
}

bool AssetCache::wantClip(const world::LookFiles& f, std::size_t clip) noexcept
{
    // The polish-round clips are always asked for (a missing one is logged); the M9 combat clips
    // only when the file exists.
    return clip <= static_cast<std::size_t>(world::CharacterClip::SitExit) || f.clipFound[clip];
}

void AssetCache::loadCharacterNow(std::uint8_t race, int look)
{
    const world::LookFiles& f = m_looks.files(race, look);
    (void)loadMeshNow(f.mesh);
    for (std::size_t c = 0; c < world::kCharacterClipCount; ++c)
        if (wantClip(f, c))
            (void)loadAnimationNow(f.clips[c]);
}

std::string AssetCache::monsterMeshPath(std::string_view model)
{
    return std::format("monsters/{}.vmesh", model);
}

void AssetCache::requestMonster(std::string_view model)
{
    requestCreature("monsters", model);
}

std::string AssetCache::npcMeshPath(std::string_view model)
{
    return std::format("npcs/{}.vmesh", model);
}

void AssetCache::requestNpc(std::string_view model)
{
    requestCreature("npcs", model);
}

const world::CharacterClips* AssetCache::npcClips(std::string_view model)
{
    return creatureClips(std::format("npcs/{}", model));
}

const world::CharacterClips* AssetCache::monsterClips(std::string_view model)
{
    return creatureClips(model);
}

void AssetCache::requestCreature(std::string_view folder, std::string_view model)
{
    // Monsters keep their bare model key (as before M14); merchants are keyed "npcs/<model>".
    const std::string key = folder == "monsters" ? std::string(model) : std::format("{}/{}", folder, model);
    if (m_monsters.find(key) != m_monsters.end())
        return;
    MonsterSet set;
    set.mesh = std::format("{}/{}.vmesh", folder, model);
    for (std::size_t c = 0; c < world::kCharacterClipCount; ++c)
    {
        const std::string path =
            std::format("{}/{}/{}.vanim", folder, model, world::characterClipName(static_cast<world::CharacterClip>(c)));
        std::error_code ec;
        if (!m_root.empty() && std::filesystem::is_regular_file(m_root / vo::core::pathFromUtf8(path), ec))
            set.clips[c] = path; // only the combat set exists (no jump / sit / sword clips)
    }
    request(Kind::Mesh, set.mesh);
    for (const std::string& p : set.clips)
        if (!p.empty())
            request(Kind::Animation, p);
    m_monsters.emplace(key, std::move(set));
}

const world::CharacterClips* AssetCache::creatureClips(std::string_view key)
{
    const auto it = m_monsters.find(key);
    if (it == m_monsters.end())
        return nullptr;
    MonsterSet& set = it->second;
    if (set.bound)
        return &set.clips_;
    const GpuMesh* m = mesh(set.mesh);
    if (m == nullptr || !m->cpu)
        return nullptr;
    for (const std::string& p : set.clips)
        if (!p.empty() && animation(p) == nullptr && !failed(p))
            return nullptr;
    for (std::size_t c = 0; c < world::kCharacterClipCount; ++c)
    {
        const assets::AnimationFile* file = set.clips[c].empty() ? nullptr : animation(set.clips[c]);
        set.clips_.clips[c] = file != nullptr && !file->clips.empty() ? &file->clips.front() : nullptr;
    }
    set.clips_.bind(m->cpu->skeleton);
    set.bound = true;
    return &set.clips_;
}

const world::CharacterClips* AssetCache::characterClips(std::uint8_t race, int look)
{
    const int effective = m_looks.effectiveLook(race, look);
    const std::size_t slot = (race == static_cast<std::uint8_t>(game::Race::Kargan) ? 1u : 0u) * world::kMaxLooks +
                             static_cast<std::size_t>(effective);
    RaceClips& rc = m_raceClips[slot];
    if (rc.bound)
        return &rc.clips;
    const world::LookFiles& f = m_looks.files(race, effective);
    const GpuMesh* m = mesh(f.mesh);
    if (m == nullptr || !m->cpu)
        return nullptr;
    // Wait until every clip is ready or has failed (a missing clip falls back to another one).
    for (std::size_t c = 0; c < world::kCharacterClipCount; ++c)
    {
        if (wantClip(f, c) && animation(f.clips[c]) == nullptr && !failed(f.clips[c]))
            return nullptr;
    }
    for (std::size_t c = 0; c < world::kCharacterClipCount; ++c)
    {
        const assets::AnimationFile* file = wantClip(f, c) ? animation(f.clips[c]) : nullptr;
        rc.clips.clips[c] = file != nullptr && !file->clips.empty() ? &file->clips.front() : nullptr;
    }
    if (!rc.clips.has(world::AnimState::Idle))
        rc.clips.set(world::AnimState::Idle, rc.clips.has(world::AnimState::Walk)
                                                 ? rc.clips.clips[static_cast<std::size_t>(world::AnimState::Walk)]
                                                 : rc.clips.clips[static_cast<std::size_t>(world::AnimState::Run)]);
    rc.clips.bind(m->cpu->skeleton);
    rc.bound = true;
    return &rc.clips;
}

} // namespace vo::client::render
