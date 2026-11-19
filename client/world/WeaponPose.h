// Where a character's visible weapons are drawn this frame (M8, docs/CLIENT_RENDERING.md "Weapons"),
// no D3D: MainHand / OffHand template -> item grip -> attachments.toml row of the character's look
// and hand -> weaponWorld = characterWorld * boneModel(t) * A. Shared by the world (local and remote
// players) and the character preview (select / create screens).
#pragma once

#include "shared/assets/AssetMath.h"
#include "shared/assets/Mesh.h"
#include "shared/game/Attachments.h"
#include "shared/game/Items.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>

namespace vo::client::world {

struct WeaponPlacement
{
    const game::ItemTemplate* item = nullptr;
    game::WeaponHand hand = game::WeaponHand::Main;
    assets::Mat4 world; // weapon mesh model -> world
};

// The look name attachments.toml uses: "<race asset name>_<look>", e.g. "aydor_0", "kargan_2".
[[nodiscard]] std::string attachmentLookKey(std::uint8_t race, int look);
// "weapons/<model>.vmesh" (ASSET_PIPELINE.md §4b).
[[nodiscard]] std::string weaponMeshPath(const game::ItemTemplate& item);
// M17: "armor/<lookKey>/<model>.vmesh", the armour piece fitted to that look (ASSET_PIPELINE.md §4h);
// empty for items that are not armour or have no model (armour that is not drawn).
[[nodiscard]] std::string armorMeshPath(std::string_view lookKey, const game::ItemTemplate& item);
// "ui/items/<icon>.png" (the asset cache loads the compiled premultiplied .vtex next to it).
[[nodiscard]] std::string itemIconPath(const game::ItemTemplate& item);
// Whether the icon (its .png or compiled .vtex) exists under the asset root. Icons of new templates
// may arrive later than gamedata (M10); a missing one is drawn as an empty slot, never requested.
[[nodiscard]] bool itemIconAvailable(const std::filesystem::path& assetRoot, const game::ItemTemplate& item);

// Fills `out` with the weapons to draw for equipment `eq` (0 = empty hand) and returns how many.
// Skipped: template 0 or unknown to the catalog, no attachment row for (grip, look or "*", hand),
// bone not in the skeleton or outside `boneModel`. Two daggers: the same template in both hands.
std::size_t placeWeapons(const game::ItemCatalog& catalog, const game::AttachmentTable& attachments,
                         std::string_view lookKey, const assets::Skeleton& skeleton,
                         std::span<const assets::Mat4> boneModel, const assets::Mat4& characterWorld,
                         game::VisibleEquipment eq, std::array<WeaponPlacement, 2>& out);

} // namespace vo::client::world
