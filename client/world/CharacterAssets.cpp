#include "client/world/CharacterAssets.h"

#include <format>

namespace vo::client::world {

namespace {

constexpr std::array<std::string_view, kCharacterClipCount> kClipNames = {
    "idle",        "walk",       "run",         "jump_start",   "jump_loop", "jump_land",
    "sit_enter",   "sit_idle",   "sit_exit",    "sword_attack", "punch_jab", "punch_cross",
    "spell_enter", "spell_idle", "spell_shoot", "hit_chest",    "death",     "sit_ground_enter",
    "sit_ground_idle", "sit_ground_exit", "idle_talking", "interact",
};

// First existing candidate, else the first one (`found` tells which).
std::string pick(const AssetExists& exists, std::string a, std::string b, bool& found)
{
    found = true;
    if (exists && exists(a))
        return a;
    if (exists && exists(b))
        return b;
    found = false;
    return a;
}

} // namespace

std::string_view characterClipName(CharacterClip clip) noexcept
{
    const auto i = static_cast<std::size_t>(clip);
    return i < kClipNames.size() ? kClipNames[i] : std::string_view("?");
}

std::string_view raceAssetName(std::uint8_t race) noexcept
{
    return race == static_cast<std::uint8_t>(vo::game::Race::Kargan) ? "kargan" : "aydor";
}

LookFiles resolveLookFiles(std::uint8_t race, int look, const AssetExists& exists)
{
    LookFiles f;
    const std::string_view r = raceAssetName(race);
    const std::string suffixed = std::format("characters/{}_{}", r, look);
    const std::string legacy = std::format("characters/{}", r);
    if (exists && exists(suffixed + ".vmesh"))
    {
        f.available = true;
        f.mesh = suffixed + ".vmesh";
        for (std::size_t c = 0; c < kCharacterClipCount; ++c)
            f.clips[c] = pick(exists, std::format("{}.{}.vanim", suffixed, kClipNames[c]),
                              std::format("{}/{}.vanim", suffixed, kClipNames[c]), f.clipFound[c]);
        return f;
    }
    // Not found (or look 0 in its legacy form): the legacy names, available only for look 0.
    f.legacy = true;
    f.available = look == 0 && exists && exists(legacy + ".vmesh");
    f.mesh = legacy + ".vmesh";
    for (std::size_t c = 0; c < kCharacterClipCount; ++c)
        f.clips[c] = pick(exists, std::format("{}/{}.vanim", legacy, kClipNames[c]),
                          std::format("{}.{}.vanim", legacy, kClipNames[c]), f.clipFound[c]);
    return f;
}

CharacterLookTable::CharacterLookTable()
{
    discover({});
}

std::size_t CharacterLookTable::raceSlot(std::uint8_t race) noexcept
{
    return race == static_cast<std::uint8_t>(vo::game::Race::Kargan) ? 1 : 0;
}

void CharacterLookTable::discover(const AssetExists& exists)
{
    for (std::uint8_t race : {static_cast<std::uint8_t>(vo::game::Race::Aydor),
                              static_cast<std::uint8_t>(vo::game::Race::Kargan)})
    {
        for (int look = 0; look < kMaxLooks; ++look)
            m_files[raceSlot(race)][static_cast<std::size_t>(look)] = resolveLookFiles(race, look, exists);
    }
}

int CharacterLookTable::lookCount(std::uint8_t race) const noexcept
{
    int n = 0;
    for (const LookFiles& f : m_files[raceSlot(race)])
        n += f.available ? 1 : 0;
    return n;
}

bool CharacterLookTable::available(std::uint8_t race, int look) const noexcept
{
    return look >= 0 && look < kMaxLooks && m_files[raceSlot(race)][static_cast<std::size_t>(look)].available;
}

int CharacterLookTable::effectiveLook(std::uint8_t race, int look) const noexcept
{
    if (available(race, look))
        return look;
    if (available(race, 0))
        return 0;
    for (int i = 1; i < kMaxLooks; ++i)
    {
        if (available(race, i))
            return i;
    }
    return 0;
}

const LookFiles& CharacterLookTable::files(std::uint8_t race, int look) const noexcept
{
    return m_files[raceSlot(race)][static_cast<std::size_t>(effectiveLook(race, look))];
}

int CharacterLookTable::nthAvailable(std::uint8_t race, int index) const noexcept
{
    const int count = lookCount(race);
    if (count == 0)
        return 0;
    int want = ((index % count) + count) % count;
    for (int look = 0; look < kMaxLooks; ++look)
    {
        if (!available(race, look))
            continue;
        if (want-- == 0)
            return look;
    }
    return 0;
}

int CharacterLookTable::indexOfLook(std::uint8_t race, int look) const noexcept
{
    int index = 0;
    for (int i = 0; i < kMaxLooks; ++i)
    {
        if (!available(race, i))
            continue;
        if (i == look)
            return index;
        ++index;
    }
    return 0;
}

} // namespace vo::client::world
