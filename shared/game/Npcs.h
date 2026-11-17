// NPCs shared by client, server and tools (docs/PROTOCOL_SHOP.md §1, v8): the town merchants of
// gamedata/npcs.toml (id, Turkish name, title and greeting, model key, zone, position, facing, kind,
// the stock of a merchant), loaded and strictly validated like zones.toml / skills.toml.
//
//   game::NpcCatalog catalog;
//   if (!game::loadNpcCatalog(path, &items, catalog, error)) -> refuse to start
//   for (const game::NpcDef& n : catalog.npcs()) ...
//
// Positions are checked against the zone's terrain by the server at startup (checkNpcSpot): the
// file cannot know the walk mask.
//
// The process-wide catalog (game::npcs()) is the one installed with installNpcCatalog() (the server
// installs the file it validated), or else gamedata/npcs.toml found like the other data files on
// first use (its stock checked against gamedata/items.toml when that is found too).
#pragma once

#include "shared/game/Items.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace vo::assets {
struct TerrainData;
}

namespace vo::game {

inline constexpr std::string_view kNpcsFile = "gamedata/npcs.toml";
inline constexpr std::size_t kMaxNpcs = 64;
inline constexpr std::uint16_t kMaxNpcId = 999;
inline constexpr std::size_t kMaxStockEntries = net::kMaxShopEntries; // 24

// Rules of PROTOCOL_SHOP.md §1.3 / §2.
inline constexpr float kNpcTalkRange = 4.0f;    // open / buy / sell (+ kRangeTolerance, Combat.h)
inline constexpr float kShopViewDistance = 8.0f; // an open shop closes (OutOfRange) beyond this
// Startup checks of a merchant's spot (§1.2).
inline constexpr float kNpcClearance = 1.0f;     // the ground this far round the spot is walkable
inline constexpr float kNpcGateMargin = 2.0f;    // > gate radius + this from every gate
inline constexpr float kNpcMarkerMargin = 4.0f;  // > this from every race spawn / arrival marker
inline constexpr float kNpcMaxRoadWeight = 0.5f; // the road layer (splat G) weighs at most this at the spot

enum class NpcKind : std::uint8_t
{
    Merchant, // "merchant": has a shop
};

struct NpcDef
{
    std::uint16_t id = 0;       // 1..999, stable (the wire's template_id of entity type 4)
    std::string name;           // "Sürmeli Ana" (UTF-8 Turkish)
    std::string title;          // "İksirci"
    std::string greeting;       // shown when the shop opens
    std::string model;          // model key, npcs/<model>.vmesh ("npc_potion_esik")
    std::uint16_t zone = 0;
    float x = 0.0f, z = 0.0f;   // metres, y from the terrain
    float yaw = 0.0f;           // radians, forward = (sin yaw, cos yaw)
    NpcKind kind = NpcKind::Merchant;
    std::vector<ItemTemplateId> stock; // merchants: 1..24 templates, in display order
};

class NpcCatalog
{
public:
    [[nodiscard]] const NpcDef* find(std::uint16_t id) const noexcept;
    [[nodiscard]] const std::vector<NpcDef>& npcs() const noexcept { return m_npcs; } // ascending id
    [[nodiscard]] bool empty() const noexcept { return m_npcs.empty(); }
    [[nodiscard]] std::size_t size() const noexcept { return m_npcs.size(); }

private:
    friend bool parseNpcCatalog(std::string_view, std::string_view, const ItemCatalog*, NpcCatalog&, std::string&);
    std::vector<NpcDef> m_npcs;
};

// Parses and validates npcs.toml text. Strict: unknown keys, duplicate ids / models, ids outside
// 1..999, empty or overlong texts, a model key that is not [a-z0-9_], zone 0, positions outside
// 0..4096 m, an unknown kind, a merchant without stock / with more than 24 entries / an item twice.
// With `items` every stock entry must be a known template with a value >= 1 (it has a price).
// False + "<source>:<line>: <problem>"; `out` is unchanged then.
[[nodiscard]] bool parseNpcCatalog(std::string_view tomlText, std::string_view sourceName, const ItemCatalog* items,
                                   NpcCatalog& out, std::string& error);
[[nodiscard]] bool loadNpcCatalog(const std::filesystem::path& file, const ItemCatalog* items, NpcCatalog& out,
                                  std::string& error);
[[nodiscard]] std::optional<std::filesystem::path> locateNpcsFile();

// The installed catalog, else gamedata/npcs.toml loaded on first use (an empty catalog if that
// fails). Thread-safe to read; install before other threads use it.
[[nodiscard]] const NpcCatalog& npcs() noexcept;
void installNpcCatalog(std::shared_ptr<const NpcCatalog> catalog);

// "merchant".
[[nodiscard]] std::string_view npcKindName(NpcKind k) noexcept;

// ---- Startup check of a spot (PROTOCOL_SHOP.md §1.2) ----

// A circle the spot must stay out of (gates: radius + kNpcGateMargin; markers: kNpcMarkerMargin).
struct KeepOut
{
    std::string name; // for the message ("gate 'gate_esik'")
    float x = 0.0f, z = 0.0f, radius = 0.0f;
};

// Whether an NPC may stand at (x, z) of `terrain`: inside the zone, walkable on the spot and on 8
// points kNpcClearance round it, the road layer (splat G) at most kNpcMaxRoadWeight there, outside
// every `keepOut` circle. False + a message saying which rule failed.
[[nodiscard]] bool checkNpcSpot(const assets::TerrainData& terrain, float x, float z, std::span<const KeepOut> keepOut,
                                std::string& why);
// The road layer's share 0..1 at (x, z) (splat channel G over the sum of the four, nearest texel);
// 0 without a splat map.
[[nodiscard]] float roadWeightAt(const assets::TerrainData& terrain, float x, float z) noexcept;

} // namespace vo::game
