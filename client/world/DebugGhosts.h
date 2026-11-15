// Debug facility (--debug-ghosts N, off by default): synthetic remote players that wander near the
// local player, so the remote-entity and correction paths can be exercised without a server that
// relays movement. Ghosts produce ENTITY_SPAWN / ENTITY_MOVE (10 Hz while moving, immediately on
// start / stop / run toggle) / ENTITY_DESPAWN payloads, encoded with the shared message writers and
// handed to a sink; the scene injects them into GameClient's event queue, so they travel the exact
// handler path of real network messages. Deterministic for a given seed and frame times.
//
// Behaviours by index: 0 runner, 1 walker, 2 walk-then-stop, 3 mixed run/walk/stand; the last ghost
// (with N >= 4) leaves (despawn) and comes back (spawn) every few seconds. Races alternate; the look
// index (wire `face`) varies over 0-3, hair is 0.
#pragma once

#include "client/world/LocalMovement.h"
#include "client/world/RemoteMotion.h"
#include "client/world/WorldMath.h"
#include "shared/assets/Terrain.h"

#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <vector>

namespace vo::client::world {

class DebugGhosts
{
public:
    using Sink = std::function<void(std::uint16_t opcode, std::vector<std::uint8_t> payload)>;
    static constexpr std::uint32_t kFirstId = 0x7F000001u;
    static constexpr int kMaxGhosts = 64;

    // Spawns `count` ghosts around `anchor` (on walkable ground).
    void start(int count, Vec3 anchor, const assets::TerrainData* terrain, const ServerClock& clock,
               double localNow, const Sink& sink, std::uint32_t seed = 0x5EED1234u);
    void stop(const Sink& sink); // despawns every present ghost
    void update(double localNow, float dt, const ServerClock& clock, Vec3 playerPos, const MovementParams& speeds,
                const Sink& sink);
    // A POSITION_CORRECT for the local player (as a validating server would send).
    static void correction(std::uint32_t seq, Vec3 position, float yaw, const Sink& sink);

    [[nodiscard]] int count() const noexcept { return static_cast<int>(m_ghosts.size()); }
    [[nodiscard]] int presentCount() const noexcept;
    [[nodiscard]] std::uint64_t packetsSent() const noexcept { return m_packets; }

private:
    enum class Mode : std::uint8_t
    {
        Stand,
        Walk,
        Run,
    };
    struct Ghost
    {
        std::uint32_t id = 0;
        std::string name;
        std::uint8_t race = 1;
        std::uint8_t charClass = 1;
        std::uint8_t face = 0;
        std::uint8_t hair = 0;
        std::uint16_t level = 1;
        int behaviour = 0;
        bool leaver = false;
        bool present = true;
        float presenceTimer = 0.0f;
        Vec3 position{};
        float yaw = 0.0f;
        Mode mode = Mode::Stand;
        Vec2 target{};
        float timer = 0.0f;   // time left in the current mode
        int step = 0;         // behaviour sequence position
        float sendTimer = 0.0f;
        float stuckTime = 0.0f;
        bool sentMoving = false;
        bool sentRunning = false;
    };

    [[nodiscard]] float random01() noexcept;
    [[nodiscard]] bool pickTarget(Vec3 around, float minR, float maxR, Vec2& out) noexcept;
    void nextMode(Ghost& g, Vec3 playerPos) noexcept;
    void sendSpawn(const Ghost& g, const Sink& sink);
    void sendMove(Ghost& g, std::uint32_t serverMs, const Sink& sink);
    void sendDespawn(const Ghost& g, const Sink& sink);

    const assets::TerrainData* m_terrain = nullptr;
    std::vector<Ghost> m_ghosts;
    std::uint32_t m_rng = 1;
    std::uint64_t m_packets = 0;
};

} // namespace vo::client::world
