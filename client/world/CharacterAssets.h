// Character looks (no D3D, no file IO of its own): which mesh and clip files make up look n of a
// race, and how many looks each race has. The asset cache discovers the table once at startup by
// probing which files exist under assets/; everything that shows a character (preview on the
// select/create screens, the local player, remote players) asks it for the paths.
//
// Look n of race r (r = "aydor" | "kargan", n = 0..kMaxLooks-1) — lookup order, first hit wins:
//   mesh   1. characters/<r>_<n>.vmesh
//          2. n == 0 only: characters/<r>.vmesh                        (legacy single look)
//   clips  for the `_n` mesh: characters/<r>_<n>.<clip>.vanim, then characters/<r>_<n>/<clip>.vanim
//          for the legacy mesh: characters/<r>/<clip>.vanim, then characters/<r>.<clip>.vanim
//          (a clip found nowhere keeps its first candidate; loading it fails and the animator falls
//          back to another clip of the same look — never to another look's clip, the bone lengths
//          differ per mesh)
// A look whose mesh does not exist is not available; asking for it (e.g. a face index sent by the
// server that no asset carries) gives look 0, or the first available look if look 0 is missing.
//
// Wire mapping: CHAR_CREATE_REQ / CharSummary / ENTITY_SPAWN `face` (0-3) carries the look index,
// `hair` is always 0 (docs/PROTOCOL.md §5.3).
#pragma once

#include "shared/game/Character.h"

#include <array>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

namespace vo::client::world {

inline constexpr int kMaxLooks = vo::game::kMaxFace + 1; // face 0..3

// Clips every look tries to load, in CharacterClips order.
enum class CharacterClip : std::uint8_t
{
    Idle,
    Walk,
    Run,
    JumpStart,
    JumpLoop,
    JumpLand,
    SitEnter,
    SitIdle,
    SitExit,
    // M9 combat set (docs/PROTOCOL_COMBAT.md §5) and the ground sit of the server-driven stance.
    SwordAttack,
    PunchJab,
    PunchCross,
    SpellEnter,
    SpellIdle,
    SpellShoot,
    HitChest,
    Death,
    SitGroundEnter,
    SitGroundIdle,
    SitGroundExit,
    // M14 merchants (docs/SHOP_ART.md §1): talking idle while their shop is open, a reach on a sale.
    IdleTalking,
    Interact,
    // fix1: a potion drunk (ITEM_USED); optional, the art adds it per look.
    Drink,
    Count,
};
inline constexpr std::size_t kCharacterClipCount = static_cast<std::size_t>(CharacterClip::Count);
[[nodiscard]] std::string_view characterClipName(CharacterClip clip) noexcept;

struct LookFiles
{
    bool available = false; // the mesh exists
    bool legacy = false;    // the un-suffixed characters/<race>.vmesh
    std::string mesh;
    std::array<std::string, kCharacterClipCount> clips;
    // Whether the clip file was found. A missing one is not requested (an asset tree without the
    // M9 combat clips still loads; the animator then skips that action).
    std::array<bool, kCharacterClipCount> clipFound{};
};

// Asset path (relative to assets/, forward slashes) -> file exists.
using AssetExists = std::function<bool(std::string_view path)>;

// "aydor" / "kargan" (unknown races map to "aydor").
[[nodiscard]] std::string_view raceAssetName(std::uint8_t race) noexcept;

// Files of exactly look `look` (no fallback to another look).
[[nodiscard]] LookFiles resolveLookFiles(std::uint8_t race, int look, const AssetExists& exists);

class CharacterLookTable
{
public:
    CharacterLookTable(); // nothing discovered: no look available, legacy paths for look 0
    void discover(const AssetExists& exists);

    // Number of available looks of the race (0 if it has no mesh at all).
    [[nodiscard]] int lookCount(std::uint8_t race) const noexcept;
    [[nodiscard]] bool available(std::uint8_t race, int look) const noexcept;
    // The look actually shown for `look`: itself if available, else look 0, else the first available
    // one (0 when the race has none).
    [[nodiscard]] int effectiveLook(std::uint8_t race, int look) const noexcept;
    // Files of effectiveLook(race, look).
    [[nodiscard]] const LookFiles& files(std::uint8_t race, int look) const noexcept;
    // The `index`-th available look (0-based, wraps), for the create screen's selector.
    [[nodiscard]] int nthAvailable(std::uint8_t race, int index) const noexcept;
    // Position of `look` among the available looks (0 if not available).
    [[nodiscard]] int indexOfLook(std::uint8_t race, int look) const noexcept;

private:
    static std::size_t raceSlot(std::uint8_t race) noexcept;
    std::array<std::array<LookFiles, kMaxLooks>, 2> m_files{};
};

} // namespace vo::client::world
