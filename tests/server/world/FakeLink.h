// A fake IPlayerLink for the world tests: decodes and records every packet it is handed, so tests can
// look at exactly what a client would receive, without sockets.
#pragma once

#include "server/world/WorldTypes.h"
#include "shared/net/Messages.h"

#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <utility>
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
            // A zone change's arrival is ZONE_CHANGE on the wire (PROTOCOL_ZONES.md §3.3).
            m_packets.push_back({static_cast<std::uint16_t>(batch.entered->zoneChange ? net::Opcode::ZoneChange
                                                                                      : net::Opcode::EnterWorldRes),
                                 {},
                                 m_batches});
            if (batch.entered->zoneChange)
                ++m_arrivals;
        }
        for (const auto& item : batch.items)
        {
            const auto p = batch.payload(item);
            m_packets.push_back({item.opcode, std::vector<std::uint8_t>(p.begin(), p.end()), m_batches});
            applyToView(item.opcode, p);
        }
        for (auto& g : batch.lootGrants)
            m_grants.push_back(std::move(g));
        for (const auto& g : batch.shopGrants)
            m_shopGrants.push_back(g);
        for (const auto& g : batch.useGrants)
            m_useGrants.push_back(g);
        if (batch.zoneChangeBegin)
        {
            // Like a client: everything of the old zone is dropped (no ENTITY_DESPAWNs come).
            const auto bytes = net::encodeMessage(*batch.zoneChangeBegin);
            m_packets.push_back({static_cast<std::uint16_t>(net::Opcode::ZoneChangeBegin),
                                 bytes ? *bytes : std::vector<std::uint8_t>{}, m_batches});
            m_visible.clear();
            m_effects.clear();
            m_begins.push_back(*batch.zoneChangeBegin);
        }
    }

    // ZONE_CHANGE_BEGINs received and arrivals (zone-change entries) so far.
    [[nodiscard]] std::vector<net::ZoneChangeBegin> begins() const
    {
        const std::scoped_lock lock(m_mutex);
        return m_begins;
    }
    [[nodiscard]] std::size_t arrivals() const
    {
        const std::scoped_lock lock(m_mutex);
        return m_arrivals;
    }

    // Loot grants handed to the session (PROTOCOL_LOOT.md §3.4), oldest first; takeGrants() empties.
    [[nodiscard]] std::vector<server::world::LootGrant> grants() const
    {
        const std::scoped_lock lock(m_mutex);
        return m_grants;
    }
    std::vector<server::world::LootGrant> takeGrants()
    {
        const std::scoped_lock lock(m_mutex);
        return std::exchange(m_grants, {});
    }

    // Shop and consumable grants handed to the session (PROTOCOL_SHOP.md §2.2, §2.3), oldest first.
    [[nodiscard]] std::vector<server::world::ShopGrant> shopGrants() const
    {
        const std::scoped_lock lock(m_mutex);
        return m_shopGrants;
    }
    [[nodiscard]] std::vector<server::world::ItemUseGrant> useGrants() const
    {
        const std::scoped_lock lock(m_mutex);
        return m_useGrants;
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
    // The effect list of an entity as a client keeps it (PROTOCOL_SKILLS.md §3.7): EFFECT_LIST
    // replaces, EFFECT_APPLY inserts / refreshes, EFFECT_REMOVE removes; ENTITY_DEATH and despawn clear.
    [[nodiscard]] std::vector<net::EffectEntry> effects(std::uint32_t entity) const
    {
        const std::scoped_lock lock(m_mutex);
        const auto it = m_effects.find(entity);
        return it == m_effects.end() ? std::vector<net::EffectEntry>{} : it->second;
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
            m_effects.erase(d.entityId);
            break;
        }
        case net::Opcode::EntityDeath:
        {
            net::EntityDeath d;
            if (net::decodeMessage(p, d))
                m_effects.erase(d.entityId);
            break;
        }
        case net::Opcode::EffectList:
        {
            net::EffectList l;
            if (!net::decodeMessage(p, l) || !knownForEffects(l.entityId))
                m_errors.push_back("EFFECT_LIST of unknown entity " + std::to_string(l.entityId));
            else
                m_effects[l.entityId] = l.effects;
            break;
        }
        case net::Opcode::EffectApply:
        {
            net::EffectApply a;
            if (!net::decodeMessage(p, a) || !knownForEffects(a.entityId))
            {
                m_errors.push_back("EFFECT_APPLY of unknown entity " + std::to_string(a.entityId));
                break;
            }
            auto& list = m_effects[a.entityId];
            std::erase_if(list, [&](const net::EffectEntry& e) { return e.skillId == a.effect.skillId && e.kind == a.effect.kind; });
            list.push_back(a.effect);
            break;
        }
        case net::Opcode::EffectRemove:
        {
            net::EffectRemove r;
            if (!net::decodeMessage(p, r) || !knownForEffects(r.entityId))
            {
                m_errors.push_back("EFFECT_REMOVE of unknown entity " + std::to_string(r.entityId));
                break;
            }
            auto& list = m_effects[r.entityId];
            const auto before = list.size();
            std::erase_if(list, [&](const net::EffectEntry& e) { return e.skillId == r.skillId && e.kind == r.kind; });
            if (list.size() == before)
                m_errors.push_back("EFFECT_REMOVE of an effect the client does not have on " + std::to_string(r.entityId));
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

    [[nodiscard]] bool knownForEffects(std::uint32_t entity) const
    {
        return m_visible.contains(entity) || (m_entered && m_entered->entityId == entity);
    }

    mutable std::mutex m_mutex;
    std::map<std::uint32_t, std::vector<net::EffectEntry>> m_effects;
    std::size_t m_batches = 0;
    std::optional<server::world::EnteredInfo> m_entered;
    std::size_t m_enteredBatch = 0;
    std::vector<Packet> m_packets;
    std::vector<server::world::LootGrant> m_grants;
    std::vector<server::world::ShopGrant> m_shopGrants;
    std::vector<server::world::ItemUseGrant> m_useGrants;
    std::vector<net::ZoneChangeBegin> m_begins;
    std::size_t m_arrivals = 0;
    std::set<std::uint32_t> m_visible;
    std::vector<std::string> m_errors;
};

} // namespace vo::tests
