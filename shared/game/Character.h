// Character definitions shared by client and server (docs/PROTOCOL.md §5.3): races, classes, which
// combinations are allowed, appearance limits and starting values.
#pragma once

#include <cstdint>
#include <string_view>

namespace vo::game {

enum class Race : std::uint8_t
{
    Aydor = 1,
    Kargan = 2,
};

enum class CharClass : std::uint8_t
{
    Warrior = 1, // Savaşçı
    Rogue = 2,   // Haydut
    Mage = 3,    // Büyücü
    Priest = 4,  // Şifacı
};

inline constexpr std::uint8_t kMaxFace = 3; // face 0..3
inline constexpr std::uint8_t kMaxHair = 3; // hair 0..3
inline constexpr std::uint16_t kStartLevel = 1;
inline constexpr std::uint16_t kStartZoneId = 1;

[[nodiscard]] constexpr bool isValidRace(std::uint8_t race) noexcept
{
    return race == static_cast<std::uint8_t>(Race::Aydor) || race == static_cast<std::uint8_t>(Race::Kargan);
}

[[nodiscard]] constexpr bool isValidClass(std::uint8_t cls) noexcept
{
    return cls >= static_cast<std::uint8_t>(CharClass::Warrior) && cls <= static_cast<std::uint8_t>(CharClass::Priest);
}

// Both races may currently play every class; keep this the single place that decides.
[[nodiscard]] constexpr bool isAllowedRaceClass(std::uint8_t race, std::uint8_t cls) noexcept
{
    return isValidRace(race) && isValidClass(cls);
}

[[nodiscard]] constexpr bool isValidAppearance(std::uint8_t face, std::uint8_t hair) noexcept
{
    return face <= kMaxFace && hair <= kMaxHair;
}

// Display names (UTF-8, Turkish).
[[nodiscard]] constexpr std::string_view raceName(Race race) noexcept
{
    switch (race)
    {
    case Race::Aydor: return "Aydor";
    case Race::Kargan: return "Kargan";
    }
    return "?";
}

[[nodiscard]] constexpr std::string_view className(CharClass cls) noexcept
{
    switch (cls)
    {
    case CharClass::Warrior: return "Savaşçı";
    case CharClass::Rogue: return "Haydut";
    case CharClass::Mage: return "Büyücü";
    case CharClass::Priest: return "Şifacı";
    }
    return "?";
}

} // namespace vo::game
