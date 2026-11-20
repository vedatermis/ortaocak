// Loot tables shared by client and server (docs/PROTOCOL_LOOT.md §2): what a monster drops when it
// dies, loaded from gamedata/loot.toml with the same strict validation as items.toml /
// monsters.toml, and the roll itself (pure; the caller injects the random generator).
//
//   LootTables tables;
//   if (!loadLootTables(*locateLootFile(), catalog, &monsters, tables, error)) -> refuse to start
//   if (const LootTable* t = tables.forMonster(monster.id)) { LootRoll r = rollLoot(*t, rng); ... }
#pragma once

#include "shared/game/Items.h"
#include "shared/game/Monsters.h"
#include "shared/net/Opcodes.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <vector>

namespace vo::game {

inline constexpr std::string_view kLootFile = "gamedata/loot.toml";
inline constexpr std::size_t kMaxLootEntries = net::kMaxLootEntries; // entries per container (and per roll)
// M15: drop lines per table (a roll keeps the first kMaxLootEntries items that drop, in line order).
inline constexpr std::size_t kMaxLootLines = 80; // M18 (the new gear); 32 in M16 (stone lines), 24 in M15, 8 before
inline constexpr std::uint32_t kMaxLootCoins = 100'000;             // per roll
inline constexpr std::uint32_t kMaxCoins = net::kMaxCoins;           // a character's balance cap

// Containers in the world (PROTOCOL_LOOT.md §1.3, §3, §5).
inline constexpr float kLootRange = 3.0f;        // + kRangeTolerance (shared/game/Combat.h)
inline constexpr float kLootViewDistance = 6.0f; // a viewer farther than this is closed
inline constexpr std::chrono::milliseconds kLootOwnerWindow{30'000};
inline constexpr std::chrono::milliseconds kLootLifetime{90'000};

struct LootDrop
{
    ItemTemplateId item = 0;
    double chance = 1.0; // 0 < chance <= 1
    std::uint16_t minCount = 1;
    std::uint16_t maxCount = 1; // <= the template's max_stack
    bool unique = false;        // M15: the item's rarity is "unique" (boss drops)
};

struct LootTable
{
    std::vector<MonsterTemplateId> monsters; // the monsters using this table
    std::uint32_t coinsMin = 0;
    std::uint32_t coinsMax = 0;
    std::vector<LootDrop> drops; // file order, at most kMaxLootLines, no item twice
};

struct LootItem
{
    ItemTemplateId templateId = 0;
    std::uint16_t quantity = 0;
    bool operator==(const LootItem&) const = default;
};

struct LootRoll
{
    std::uint32_t coins = 0;
    std::vector<LootItem> items; // in drop-line order
    [[nodiscard]] bool empty() const noexcept { return coins == 0 && items.empty(); }
};

class LootTables
{
public:
    // nullptr if the monster has no table (it drops nothing).
    [[nodiscard]] const LootTable* forMonster(MonsterTemplateId monster) const noexcept;
    [[nodiscard]] const std::vector<LootTable>& tables() const noexcept { return m_tables; }
    [[nodiscard]] bool empty() const noexcept { return m_tables.empty(); }

private:
    friend bool parseLootTables(std::string_view, std::string_view, const ItemCatalog&, const MonsterData*,
                                LootTables&, std::string&);
    std::vector<LootTable> m_tables;
    std::vector<std::int32_t> m_byMonster; // monster id -> index into m_tables, -1 = none
};

// Parses and validates the TOML text of gamedata/loot.toml. Strict: unknown keys, a table without
// monsters, a monster unknown to `monsters` (skipped if null) or in two tables, coin ranges outside
// 0..kMaxLootCoins or min > max, more than kMaxLootEntries drop lines, an item unknown to `items` or
// twice in one table, a chance outside (0, 1], counts outside 1..max_stack or min > max -> false with
// a message naming the file and line. On failure `out` is left unchanged. An empty file is valid
// (nothing drops).
[[nodiscard]] bool parseLootTables(std::string_view tomlText, std::string_view sourceName, const ItemCatalog& items,
                                   const MonsterData* monsters, LootTables& out, std::string& error);
[[nodiscard]] bool loadLootTables(const std::filesystem::path& file, const ItemCatalog& items,
                                  const MonsterData* monsters, LootTables& out, std::string& error);
// gamedata/loot.toml searched upwards from the working directory, then from the executable
// directory (core::locateDataFile).
[[nodiscard]] std::optional<std::filesystem::path> locateLootFile();

// One roll (§2): coins uniform in [coinsMin, coinsMax], then every drop line in order succeeds with
// its chance and yields uniform [minCount, maxCount] units.
[[nodiscard]] LootRoll rollLoot(const LootTable& table, std::mt19937& rng);
// Testing (world.testing_force_unique_drop, M15): makes sure oll holds one of the table's unique
// items (a random unique line, minCount units; it replaces the last non-unique item if the roll is full).
// No-op for a table without unique lines or a roll that already holds one.
void forceUniqueDrop(const LootTable& table, LootRoll& roll, std::mt19937& rng);

} // namespace vo::game
