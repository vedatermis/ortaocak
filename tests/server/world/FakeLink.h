// A fake IPlayerLink for the world tests: decodes and records every packet it is handed, so tests can
// look at exactly what a client would receive, without sockets.
#pragma once

#include "server/world/WorldTypes.h"
#include "shared/net/Messages.h"

#include <cstdint>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace vo::tests {

class FakeLink final : public server::world::IPlayerLink
{
public:
    struct Packet
    {
        std::uint16_t opcode = 0;
        std::vector<std::uint8_t> payload;
        std::size_t batch = 0; // which delivery it came in
    };

    void deliver(server::world::WorldBatch batch) override
    {
        const std::scoped_lock lock(m_mutex);
        ++m_batches;
        if (batch.entered)
        {
            m_entered = *batch.entered;
            m_enteredBatch = m_batches;
            m_packets.push_back({static_cast<std::uint16_t>(net::Opcode::EnterWorldRes), {}, m_batches});
        }
        for (const auto& item : batch.items)
        {
            const auto p = batch.payload(item);
            m_packets.push_back({item.opcode, std::vector<std::uint8_t>(p.begin(), p.end()), m_batches});
            applyToView(item.opcode, p);
        }
    }

    // ---- Inspection (any thread) ----

    [[nodiscard]] std::optional<server::world::EnteredInfo> entered() const
    {
        const std::scoped_lock lock(m_mutex);
        return m_entered;
    }
    [[nodiscard]] std::size_t batches() const
    {
        const std::scoped_lock lock(m_mutex);
        return m_batches;
    }
    [[nodiscard]] std::vector<Packet> packets() const
    {
        const std::scoped_lock lock(m_mutex);
        return m_packets;
    }
    template <class T>
    [[nodiscard]] std::vector<T> all() const
    {
        const std::scoped_lock lock(m_mutex);
        std::vector<T> out;
        for (const auto& p : m_packets)
        {
            T msg;
            if (p.opcode == static_cast<std::uint16_t>(T::kOpcode) && net::decodeMessage(p.payload, msg))
                out.push_back(msg);
        }
        return out;
    }
    template <class T>
    [[nodiscard]] std::size_t count() const
    {
        return all<T>().size();
    }
    // Entities this client currently has spawned (from the packet stream).
    [[nodiscard]] std::set<std::uint32_t> visible() const
    {
        const std::scoped_lock lock(m_mutex);
        return m_visible;
    }
    // Protocol inconsistencies seen in the stream (duplicate spawn, despawn/move of an unknown entity).
    [[nodiscard]] std::vector<std::string> errors() const
    {
        const std::scoped_lock lock(m_mutex);
        return m_errors;
    }
    void clear()
    {
        const std::scoped_lock lock(m_mutex);
        m_packets.clear();
    }

private:
    void applyToView(std::uint16_t opcode, std::span<const std::uint8_t> p)
    {
        switch (static_cast<net::Opcode>(opcode))
        {
        case net::Opcode::EntitySpawn:
        {
            net::EntitySpawn s;
            if (!net::decodeMessage(p, s) || !m_visible.insert(s.entityId).second)
                m_errors.push_back("duplicate or malformed ENTITY_SPAWN " + std::to_string(s.entityId));
            break;
        }
        case net::Opcode::EntityDespawn:
        {
            net::EntityDespawn d;
            if (!net::decodeMessage(p, d) || m_visible.erase(d.entityId) == 0)
                m_errors.push_back("ENTITY_DESPAWN of unknown entity " + std::to_string(d.entityId));
            break;
        }
        case net::Opcode::EntityMove:
        {
            net::EntityMove m;
            if (!net::decodeMessage(p, m) || !m_visible.contains(m.entityId))
                m_errors.push_back("ENTITY_MOVE of unknown entity " + std::to_string(m.entityId));
            break;
        }
        default: break;
        }
    }

    mutable std::mutex m_mutex;
    std::size_t m_batches = 0;
    std::optional<server::world::EnteredInfo> m_entered;
    std::size_t m_enteredBatch = 0;
    std::vector<Packet> m_packets;
    std::set<std::uint32_t> m_visible;
    std::vector<std::string> m_errors;
};

} // namespace vo::tests
