// Item game data the client reads at startup (M8): gamedata/items.toml (templates, starter kits; the
// same file and loader as the server) and gamedata/attachments.toml (weapon placement per look).
// Either may be missing or invalid: the client then shows unknown items as "?" and draws no weapons
// (an error is logged once), it never refuses to start.
#pragma once

#include "shared/game/Attachments.h"
#include "shared/game/Items.h"
#include "shared/game/Monsters.h"

#include <string>

namespace vo::client::world {

// (M9) Also gamedata/monsters.toml: monster models, scale, level, elite flag and the safe zones.
// Missing or invalid: monsters are drawn with their wire name and no model, no safe-zone indicator.
struct ItemData
{
    game::ItemCatalog catalog;
    game::AttachmentTable attachments;
    game::MonsterData monsters;
    bool catalogLoaded = false;
    bool attachmentsLoaded = false;
    bool monstersLoaded = false;

    // Locates and loads both files (logs what it found).
    void load();
    // Starter kit visible equipment of a class ({0, 0} without a kit or catalog).
    [[nodiscard]] game::VisibleEquipment kitOf(std::uint8_t charClass) const noexcept
    {
        return game::starterKitVisible(catalog, charClass);
    }
};

} // namespace vo::client::world
