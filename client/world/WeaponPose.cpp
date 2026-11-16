#include "client/world/WeaponPose.h"

#include "client/world/CharacterAssets.h"

#include <format>

namespace vo::client::world {

std::string attachmentLookKey(std::uint8_t race, int look)
{
    return std::format("{}_{}", raceAssetName(race), look);
}

std::string weaponMeshPath(const game::ItemTemplate& item)
{
    return std::format("weapons/{}.vmesh", item.model);
}

std::string itemIconPath(const game::ItemTemplate& item)
{
    return std::format("ui/items/{}.png", item.icon);
}

std::size_t placeWeapons(const game::ItemCatalog& catalog, const game::AttachmentTable& attachments,
                         std::string_view lookKey, const assets::Skeleton& skeleton,
                         std::span<const assets::Mat4> boneModel, const assets::Mat4& characterWorld,
                         game::VisibleEquipment eq, std::array<WeaponPlacement, 2>& out)
{
    std::size_t n = 0;
    const std::pair<game::WeaponHand, game::ItemTemplateId> hands[2] = {{game::WeaponHand::Main, eq.mainHand},
                                                                        {game::WeaponHand::Off, eq.offHand}};
    for (const auto& [hand, id] : hands)
    {
        const game::ItemTemplate* item = catalog.find(id);
        if (item == nullptr || item->model.empty())
            continue;
        const game::WeaponAttachment* a = attachments.find(item->grip, lookKey, hand);
        if (a == nullptr)
            continue;
        const int bone = skeleton.findBone(a->bone);
        if (bone < 0 || static_cast<std::size_t>(bone) >= boneModel.size())
            continue;
        WeaponPlacement& p = out[n++];
        p.item = item;
        p.hand = hand;
        p.world = game::weaponWorldMatrix(characterWorld, boneModel[static_cast<std::size_t>(bone)], *a);
    }
    return n;
}

} // namespace vo::client::world
