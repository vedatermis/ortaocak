#include "client/world/DebugGhosts.h"

#include "shared/net/Messages.h"
#include "shared/net/Opcodes.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <string_view>

namespace vo::client::world {

namespace vn = vo::net;

namespace {

constexpr std::array<std::string_view, 16> kNames{
    "Ayla", "Bora", "Ceren", "Doğan", "Ece", "Kaan", "Selin", "Tolga",
    "Yağmur", "Emre", "Şule", "Güneş", "İlkay", "Oğuz", "Çağla", "Ümit"};

template <class T>
void emit(const T& msg, const DebugGhosts::Sink& sink)
{
    if (auto payload = vn::encodeMessage(msg))
        sink(static_cast<std::uint16_t>(T::kOpcode), std::move(*payload));
}

} // namespace

float DebugGhosts::random01() noexcept
{
    // xorshift32: deterministic and good enough for wandering.
    m_rng ^= m_rng << 13;
    m_rng ^= m_rng >> 17;
    m_rng ^= m_rng << 5;
    return static_cast<float>(m_rng & 0xFFFFFFu) / static_cast<float>(0x1000000u);
}

bool DebugGhosts::pickTarget(Vec3 around, float minR, float maxR, Vec2& out) noexcept
{
    for (int attempt = 0; attempt < 16; ++attempt)
    {
        const float angle = random01() * kTwoPi;
        const float r = minR + (maxR - minR) * random01();
        const float x = around.x + std::sin(angle) * r;
        const float z = around.z + std::cos(angle) * r;
        if (m_terrain == nullptr || m_terrain->resolution < 2)
        {
            out = {x, z};
            return true;
        }
        if (!m_terrain->isWalkable(x, z))
            continue;
        out = {x, z};
        return true;
    }
    return false;
}

void DebugGhosts::start(int count, Vec3 anchor, const assets::TerrainData* terrain, const ServerClock& clock,
                        double localNow, const Sink& sink, std::uint32_t seed)
{
    m_terrain = terrain;
    m_rng = seed != 0 ? seed : 1u;
    m_ghosts.clear();
    count = std::clamp(count, 0, kMaxGhosts);
    m_ghosts.reserve(static_cast<std::size_t>(count));
    for (int i = 0; i < count; ++i)
    {
        Ghost g;
        g.id = kFirstId + static_cast<std::uint32_t>(i);
        g.name = std::string(kNames[static_cast<std::size_t>(i) % kNames.size()]);
        g.race = static_cast<std::uint8_t>(i % 2 == 0 ? 1 : 2);
        g.charClass = static_cast<std::uint8_t>(1 + i % 4);
        // Look index 0-3: indices the assets do not have exercise the look-0 fallback.
        g.face = static_cast<std::uint8_t>((i * 3 + 1) % 4);
        g.hair = 0;
        g.level = static_cast<std::uint16_t>(1 + (i * 7) % 20);
        g.behaviour = i % 4;
        g.leaver = count >= 4 && i == count - 1;
        g.presenceTimer = g.leaver ? 7.0f : 0.0f;
        Vec2 p{anchor.x + 3.0f + static_cast<float>(i), anchor.z + 3.0f};
        (void)pickTarget(anchor, 4.0f, 12.0f, p);
        g.position = {p.x, terrain != nullptr && terrain->resolution >= 2 ? terrain->heightAt(p.x, p.y) : anchor.y,
                      p.y};
        g.yaw = random01() * kTwoPi - kPi;
        g.mode = Mode::Stand;
        g.timer = 0.3f + 0.4f * static_cast<float>(i);
        m_ghosts.push_back(std::move(g));
    }
    (void)clock;
    (void)localNow;
    for (const Ghost& g : m_ghosts)
        sendSpawn(g, sink);
}

void DebugGhosts::stop(const Sink& sink)
{
    for (const Ghost& g : m_ghosts)
    {
        if (g.present)
            sendDespawn(g, sink);
    }
    m_ghosts.clear();
}

int DebugGhosts::presentCount() const noexcept
{
    return static_cast<int>(std::count_if(m_ghosts.begin(), m_ghosts.end(), [](const Ghost& g) { return g.present; }));
}

void DebugGhosts::nextMode(Ghost& g, Vec3 playerPos) noexcept
{
    // Behaviour programs: (mode, radius / duration) steps, looping.
    struct Step
    {
        Mode mode;
        float a; // min radius or stand time
        float b; // max radius
    };
    static constexpr Step kRunner[] = {{Mode::Run, 8.0f, 18.0f}, {Mode::Stand, 0.6f, 0.0f}};
    static constexpr Step kWalker[] = {{Mode::Walk, 4.0f, 11.0f}, {Mode::Stand, 1.5f, 0.0f}};
    static constexpr Step kStopper[] = {{Mode::Walk, 5.0f, 12.0f}, {Mode::Stand, 4.0f, 0.0f}};
    static constexpr Step kMixed[] = {{Mode::Run, 8.0f, 16.0f}, {Mode::Walk, 4.0f, 9.0f}, {Mode::Stand, 2.5f, 0.0f}};
    std::span<const Step> program;
    switch (g.behaviour)
    {
    case 0: program = kRunner; break;
    case 1: program = kWalker; break;
    case 2: program = kStopper; break;
    default: program = kMixed; break;
    }
    const Step& s = program[static_cast<std::size_t>(g.step) % program.size()];
    g.step = (g.step + 1) % static_cast<int>(program.size());
    g.stuckTime = 0.0f;
    if (s.mode == Mode::Stand)
    {
        g.mode = Mode::Stand;
        g.timer = s.a;
        return;
    }
    // Wander around the player (re-centred on them as they move).
    Vec2 t{};
    if (!pickTarget(playerPos, s.a, s.b, t))
    {
        g.mode = Mode::Stand;
        g.timer = 1.0f;
        return;
    }
    g.mode = s.mode;
    g.target = t;
    g.timer = 15.0f; // give up after this long
}

void DebugGhosts::update(double localNow, float dt, const ServerClock& clock, Vec3 playerPos,
                         const MovementParams& speeds, const Sink& sink)
{
    if (m_ghosts.empty() || !(dt > 0.0f))
        return;
    const std::uint32_t serverMs = clock.toServerMs(clock.now(localNow));
    for (Ghost& g : m_ghosts)
    {
        if (g.leaver)
        {
            g.presenceTimer -= dt;
            if (g.presenceTimer <= 0.0f)
            {
                g.present = !g.present;
                g.presenceTimer = g.present ? 7.0f : 3.0f;
                if (g.present)
                {
                    Vec2 p{};
                    if (pickTarget(playerPos, 6.0f, 14.0f, p))
                        g.position = {p.x, m_terrain != nullptr ? m_terrain->heightAt(p.x, p.y) : playerPos.y, p.y};
                    g.mode = Mode::Stand;
                    g.timer = 0.5f;
                    g.sentMoving = false;
                    g.sentRunning = false;
                    sendSpawn(g, sink);
                }
                else
                {
                    sendDespawn(g, sink);
                }
            }
            if (!g.present)
                continue;
        }

        g.timer -= dt;
        if (g.mode == Mode::Stand)
        {
            if (g.timer <= 0.0f)
                nextMode(g, playerPos);
        }
        else
        {
            const float dx = g.target.x - g.position.x;
            const float dz = g.target.y - g.position.z;
            const float dist = std::sqrt(dx * dx + dz * dz);
            const float speed = g.mode == Mode::Run ? speeds.runSpeed : speeds.walkSpeed;
            const float travel = std::min(speed * dt, dist);
            if (dist < 0.2f || g.timer <= 0.0f)
            {
                nextMode(g, playerPos);
            }
            else
            {
                const Vec3 before = g.position;
                if (m_terrain != nullptr && m_terrain->resolution >= 2)
                    g.position = LocalMover::slideMove(*m_terrain, g.position, dx / dist * travel, dz / dist * travel,
                                                       0.25f, 0.3f);
                else
                    g.position = {g.position.x + dx / dist * travel, g.position.y, g.position.z + dz / dist * travel};
                const float moved = horizontalDistance(before, g.position);
                if (moved > 1e-4f)
                    g.yaw = approachAngle(g.yaw, yawFromDirection(g.position.x - before.x, g.position.z - before.z),
                                          10.0f * dt);
                g.stuckTime = moved < 0.2f * travel ? g.stuckTime + dt : 0.0f;
                if (g.stuckTime > 0.5f)
                    nextMode(g, playerPos);
            }
        }

        const bool moving = g.mode != Mode::Stand;
        const bool running = g.mode == Mode::Run;
        g.sendTimer -= dt;
        if (moving != g.sentMoving || running != g.sentRunning || (moving && g.sendTimer <= 0.0f))
            sendMove(g, serverMs, sink);
    }
}

void DebugGhosts::correction(std::uint32_t seq, Vec3 position, float yaw, const Sink& sink)
{
    vn::PositionCorrect m;
    m.seq = seq;
    m.x = position.x;
    m.y = position.y;
    m.z = position.z;
    m.yaw = yaw;
    emit(m, sink);
}

void DebugGhosts::sendSpawn(const Ghost& g, const Sink& sink)
{
    vn::EntitySpawn m;
    m.entityId = g.id;
    m.entityType = vn::kEntityTypePlayer;
    m.name = g.name;
    m.race = g.race;
    m.charClass = g.charClass;
    m.face = g.face;
    m.hair = g.hair;
    m.level = g.level;
    m.x = g.position.x;
    m.y = g.position.y;
    m.z = g.position.z;
    m.yaw = g.yaw;
    m.flags = 0;
    emit(m, sink);
    ++m_packets;
}

void DebugGhosts::sendMove(Ghost& g, std::uint32_t serverMs, const Sink& sink)
{
    const bool moving = g.mode != Mode::Stand;
    const bool running = g.mode == Mode::Run;
    vn::EntityMove m;
    m.entityId = g.id;
    m.flags = static_cast<std::uint8_t>((moving ? vn::kMoveFlagMoving : 0) | (running ? vn::kMoveFlagRunning : 0));
    m.x = g.position.x;
    m.y = g.position.y;
    m.z = g.position.z;
    m.yaw = g.yaw;
    m.serverTimeMs = serverMs;
    emit(m, sink);
    ++m_packets;
    g.sentMoving = moving;
    g.sentRunning = running;
    g.sendTimer = 0.1f;
}

void DebugGhosts::sendDespawn(const Ghost& g, const Sink& sink)
{
    vn::EntityDespawn m;
    m.entityId = g.id;
    emit(m, sink);
    ++m_packets;
}

} // namespace vo::client::world
