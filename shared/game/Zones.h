// Zone display names (docs/LORE.md "Yerler"): the HUD, the loading screen, the zone banner and the
// character cards show these instead of the numeric zone id. Header-only, UTF-8 Turkish.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace vo::game {

struct ZoneInfo
{
    std::uint16_t id = 0;
    std::string_view name;    // "Ortaocak"
    std::string_view tagline; // one short line under the name on the zone banner
};

inline constexpr ZoneInfo kZones[] = {
    {1, "Ortaocak", "Eşik'in tarafsız köyü"},
};

// nullptr for an unknown zone id.
[[nodiscard]] constexpr const ZoneInfo* findZone(std::uint16_t id) noexcept
{
    for (const ZoneInfo& z : kZones)
    {
        if (z.id == id)
            return &z;
    }
    return nullptr;
}

// The zone's name, or "Bölge <id>" for an id the table does not know (a newer server).
[[nodiscard]] inline std::string zoneDisplayName(std::uint16_t id)
{
    if (const ZoneInfo* z = findZone(id))
        return std::string(z->name);
    return "Bölge " + std::to_string(id);
}

} // namespace vo::game
