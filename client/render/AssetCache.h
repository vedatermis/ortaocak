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
#include "shared/assets/Animation.h"
#include "shared/assets/Terrain.h"

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
    enum class Kind : std::uint8_t
    {
        Mesh,
        Texture, // sRGB colour PNG, mips generated on the GPU
        Animation,
        Terrain,
    };

    struct Progress
    {
        std::size_t requested = 0;
        std::size_t done = 0; // ready or failed
        std::size_t failed = 0;
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

    // nullptr when not (yet) loaded or failed.
    [[nodiscard]] const GpuMesh* mesh(std::string_view path) const;
    [[nodiscard]] const Texture2D* texture(std::string_view path) const;
    [[nodiscard]] const assets::AnimationFile* animation(std::string_view path) const;
    [[nodiscard]] const assets::TerrainData* terrain(std::string_view path) const;
    [[nodiscard]] bool failed(std::string_view path) const;

    // Synchronous variants (render thread) for the character preview: load now if missing.
    const GpuMesh* loadMeshNow(std::string_view path);
    const assets::AnimationFile* loadAnimationNow(std::string_view path);

    // Character assets per race (vo::game::Race): mesh path and the idle/walk/run clip paths.
    [[nodiscard]] static const char* characterMeshPath(std::uint8_t race) noexcept;
    [[nodiscard]] static std::array<std::string, 3> characterClipPaths(std::uint8_t race);
    void requestCharacter(std::uint8_t race);
    // Loads a race's mesh + clips synchronously (preview).
    void loadCharacterNow(std::uint8_t race);
    // Clips of the race bound to its skeleton; nullptr until mesh and clips are loaded.
    [[nodiscard]] const world::CharacterClips* characterClips(std::uint8_t race);

private:
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
    std::array<RaceClips, 3> m_raceClips{};

    std::thread m_worker;
    std::mutex m_mutex;
    std::condition_variable m_wake;
    std::deque<Job> m_jobs;
    std::deque<std::unique_ptr<Result>> m_results;
    bool m_stop = false;
};

} // namespace vo::client::render
