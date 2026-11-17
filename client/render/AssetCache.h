// Runtime asset cache: meshes (uploaded as GpuMesh), PNG textures, animation clips and terrains,
// each loaded once and looked up by asset path (relative to the assets root, forward slashes, as in
// docs/ASSET_FORMATS.md). Loading is asynchronous: request() queues the file for a worker thread
// that reads, validates and converts it (file IO, CRC, parsing, PNG decoding); pump() then creates
// the GPU resources on the render thread within a time budget, so loading screens keep animating.
// The assets root is the `assets/` directory found next to/above the working directory or the
// executable (it contains terrain/zone1/zone1.vterrain).
#pragma once

#include "client/render/GpuMesh.h"
#include "client/render/Texture2D.h"
#include "client/world/Animator.h"
#include "client/world/CharacterAssets.h"
#include "shared/assets/Animation.h"
#include "shared/assets/Terrain.h"
#include "shared/assets/Texture.h"

#include <d3d11.h>

#include <array>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

namespace vo::client::render {

class AssetCache
{
public:
    // Textures are requested by their PNG asset path. If the compiled sibling `<stem>.vtex` exists
    // (assetc build writes it for mesh material textures) it is loaded instead: pre-mipped,
    // block-compressed, uploaded as is. Otherwise the PNG is decoded and mipped on the GPU.
    enum class Kind : std::uint8_t
    {
        Mesh,
        Texture, // colour (sRGB)
        Animation,
        Terrain,
        NormalMap, // tangent-space normal map (linear)
        DataMap,   // linear data, e.g. ORM
        // UI image with alpha (M8 item icons, ASSET_FORMATS.md §5.1): sRGB colour; the compiled .vtex is
        // already premultiplied and uploaded as is, the PNG fallback is premultiplied on load.
        UiImage,
    };

    struct Progress
    {
        std::size_t requested = 0;
        std::size_t done = 0; // ready or failed
        std::size_t failed = 0;
    };

    // Load statistics since init (automation / docs/CLIENT_RENDERING.md performance notes).
    struct Stats
    {
        std::size_t textures = 0;
        std::size_t texturesFromVtex = 0;
        std::uint64_t textureBytes = 0; // GPU memory of all textures
        std::size_t meshes = 0;
        std::uint64_t meshBytes = 0;     // vertex + index buffers
        double textureLoadSeconds = 0.0; // worker: read + parse/decode (summed)
        double textureUploadSeconds = 0.0; // render thread: create (+ GenerateMips)
    };

    AssetCache();
    ~AssetCache();
    AssetCache(const AssetCache&) = delete;
    AssetCache& operator=(const AssetCache&) = delete;

    // Finds the assets root and starts the worker. Without an assets directory every request fails
    // (callers fall back); returns false only if the worker could not start.
    bool init(ID3D11Device* device, ID3D11DeviceContext* context, std::string& error);
    // Stops the worker and releases every GPU object (call before the device shuts down).
    void shutdown() noexcept;

    [[nodiscard]] bool hasRoot() const noexcept { return !m_root.empty(); }
    [[nodiscard]] const std::filesystem::path& root() const noexcept { return m_root; }

    void request(Kind kind, std::string_view path);
    // Render thread: finishes loads the worker completed, spending at most ~budgetSeconds.
    void pump(double budgetSeconds);
    [[nodiscard]] Progress progress() const noexcept { return m_progress; }
    [[nodiscard]] bool idle() const noexcept { return m_progress.done == m_progress.requested; }
    [[nodiscard]] const Stats& stats() const noexcept { return m_stats; }
    // graphics.compressed_textures: false ignores the .vtex files (always decode the PNG; for
    // comparisons). Set before the first request.
    void setPreferCompiledTextures(bool on) noexcept { m_preferCompiled = on; }
    // Logs the statistics (one INFO line: "assets: loaded ...").
    void logStats() const;

    // nullptr when not (yet) loaded or failed.
    [[nodiscard]] const GpuMesh* mesh(std::string_view path) const;
    [[nodiscard]] const Texture2D* texture(std::string_view path) const;
    [[nodiscard]] const assets::AnimationFile* animation(std::string_view path) const;
    [[nodiscard]] const assets::TerrainData* terrain(std::string_view path) const;
    [[nodiscard]] bool failed(std::string_view path) const;

    // Synchronous variants (render thread) for the character preview: load now if missing.
    const GpuMesh* loadMeshNow(std::string_view path);
    const assets::AnimationFile* loadAnimationNow(std::string_view path);
    // Texture of any texture kind, loaded now if not yet requested (or still queued).
    const Texture2D* loadTextureNow(Kind kind, std::string_view path);

    // Character looks (client/world/CharacterAssets.h): discovered in init() from the files under
    // the assets root. `look` is the wire `face` value; unavailable looks fall back to look 0.
    [[nodiscard]] const world::CharacterLookTable& looks() const noexcept { return m_looks; }
    [[nodiscard]] const std::string& characterMeshPath(std::uint8_t race, int look) const noexcept;
    // Queues the mesh and clips of every available look of the race.
    void requestCharacter(std::uint8_t race);
    // Loads one look's mesh + clips synchronously (preview).
    void loadCharacterNow(std::uint8_t race, int look);
    // Clips of the race's look bound to that look's skeleton; nullptr until mesh and clips are
    // loaded (or failed).
    [[nodiscard]] const world::CharacterClips* characterClips(std::uint8_t race, int look);

    // Monsters (M9, docs/PROTOCOL_COMBAT.md §1.3): monsters/<model>.vmesh and whichever of the
    // character clips exist as monsters/<model>/<clip>.vanim (the combat set).
    [[nodiscard]] static std::string monsterMeshPath(std::string_view model);
    void requestMonster(std::string_view model);
    // Bound clips once the mesh and its clips are loaded (or failed); nullptr before / unknown model.
    [[nodiscard]] const world::CharacterClips* monsterClips(std::string_view model);

    // Merchants (M14, docs/SHOP_ART.md §1): npcs/<model>.vmesh and whichever clips exist as
    // npcs/<model>/<clip>.vanim (idle, idle_talking, interact), exactly like the monsters.
    [[nodiscard]] static std::string npcMeshPath(std::string_view model);
    void requestNpc(std::string_view model);
    [[nodiscard]] const world::CharacterClips* npcClips(std::string_view model);

private:
    void requestCreature(std::string_view folder, std::string_view model);
    [[nodiscard]] const world::CharacterClips* creatureClips(std::string_view key);
    [[nodiscard]] static bool wantClip(const world::LookFiles& f, std::size_t clip) noexcept;
    struct StringHash
    {
        using is_transparent = void;
        std::size_t operator()(std::string_view s) const noexcept { return std::hash<std::string_view>{}(s); }
    };
    enum class State : std::uint8_t
    {
        Queued,
        Ready,
        Failed,
    };
    struct Entry
    {
        Kind kind = Kind::Mesh;
        State state = State::Queued;
        std::unique_ptr<GpuMesh> mesh;
        std::unique_ptr<Texture2D> texture;
        std::unique_ptr<assets::AnimationFile> animation;
        std::unique_ptr<assets::TerrainData> terrain;
    };
    struct Job
    {
        Kind kind = Kind::Mesh;
        std::string path;
    };
    struct Result
    {
        Kind kind = Kind::Mesh;
        std::string path;
        bool ok = false;
        std::string error;
        std::shared_ptr<assets::MeshData> mesh;
        std::vector<std::uint8_t> vertices;
        DecodedImage image;
        std::unique_ptr<assets::TextureData> compiled; // from the .vtex sibling
        double loadSeconds = 0.0;
        std::unique_ptr<assets::AnimationFile> animation;
        std::unique_ptr<assets::TerrainData> terrain;
    };
    struct RaceClips
    {
        bool bound = false;
        world::CharacterClips clips;
    };

    void workerMain();
    void load(const Job& job, Result& out) const; // any thread
    void finish(Result& result);                  // render thread
    void resolveTextures();
    [[nodiscard]] const Entry* find(std::string_view path) const;

    ID3D11Device* m_device = nullptr;
    ID3D11DeviceContext* m_context = nullptr;
    std::filesystem::path m_root;
    std::unordered_map<std::string, Entry, StringHash, std::equal_to<>> m_entries;
    Progress m_progress;
    Stats m_stats;
    bool m_loggedIdle = true;
    bool m_preferCompiled = true;
    world::CharacterLookTable m_looks;
    std::array<RaceClips, 2 * world::kMaxLooks> m_raceClips{}; // [race slot * kMaxLooks + look]
    struct MonsterSet
    {
        std::string mesh;
        std::array<std::string, world::kCharacterClipCount> clips; // empty = no such file
        bool bound = false;
        world::CharacterClips clips_;
    };
    std::unordered_map<std::string, MonsterSet, StringHash, std::equal_to<>> m_monsters;

    std::vector<std::thread> m_workers; // worker threads (see init())
    std::mutex m_mutex;
    std::condition_variable m_wake;
    std::deque<Job> m_jobs;
    std::deque<std::unique_ptr<Result>> m_results;
    bool m_stop = false;
};

} // namespace vo::client::render
