// Weapon attachments (M8 "Kuşan", docs/ASSET_PIPELINE.md §4b "Attachments"): where a weapon of a
// given grip sits in a character's hand, per character look, loaded from gamedata/attachments.toml
// (written by tools/blender/gen_attachments.py). Client only (the server has no use for it), but
// kept next to the item catalog because it is game data with the same loader style: strict, and
// the error names the file and line.
//
//   AttachmentTable table;
//   if (!loadAttachmentTable(*locateAttachmentsFile(), table, error)) -> no weapons drawn
//   const WeaponAttachment* a = table.find(Grip::Sword, "aydor_0", WeaponHand::Main); // or the "*" row
//   weaponWorld = characterWorld * boneModel(t) * a->matrix()
//
// Conventions (ASSET_FORMATS.md §1): left-handed, +X right, +Y up, +Z forward, metres, column
// vectors p' = M p, A = T(translation) * R(rotation) * S(scale) as vo::assets::composeTrs.
#pragma once

#include "shared/assets/AssetMath.h"
#include "shared/game/Items.h"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace vo::game {

inline constexpr std::string_view kAttachmentsFile = "gamedata/attachments.toml";
inline constexpr std::string_view kAnyLook = "*";

enum class WeaponHand : std::uint8_t
{
    Main, // MainHand slot, right hand
    Off,  // OffHand slot, left hand
};

struct WeaponAttachment
{
    Grip grip = Grip::Sword;
    std::string look; // "aydor_0", ... or "*"
    WeaponHand hand = WeaponHand::Main;
    std::string bone; // skeleton bone, e.g. "hand_r"
    assets::Vec3 translation{};
    assets::Quat rotation{}; // unit (normalised on load)
    float scale = 1.0f;

    // A = T * R * S: grip frame (the weapon mesh's model space) -> bone space.
    [[nodiscard]] assets::Mat4 matrix() const noexcept;
};

class AttachmentTable
{
public:
    // The row for exactly `look`, else the `look = "*"` row of the grip and hand; nullptr if neither.
    [[nodiscard]] const WeaponAttachment* find(Grip grip, std::string_view look, WeaponHand hand) const noexcept;
    [[nodiscard]] const std::vector<WeaponAttachment>& rows() const noexcept { return m_rows; }
    [[nodiscard]] std::size_t size() const noexcept { return m_rows.size(); }
    [[nodiscard]] bool empty() const noexcept { return m_rows.empty(); }

private:
    friend bool parseAttachmentTable(std::string_view, std::string_view, AttachmentTable&, std::string&);
    std::vector<WeaponAttachment> m_rows;
};

// Parses and validates the TOML text. Strict: `version = 1`, only [[attach]] tables with exactly the
// keys grip, look, hand, bone, translation (3 finite numbers, |t| < 10 m), rotation (4 numbers whose
// length is 1 within 1e-3; normalised), scale (0 < s <= 10); known grip and hand names; no duplicate
// (grip, look, hand); every (grip, hand) that has a row also has its "*" fallback row. On failure
// `out` is unchanged and `error` names the file and line.
[[nodiscard]] bool parseAttachmentTable(std::string_view tomlText, std::string_view sourceName, AttachmentTable& out,
                                        std::string& error);
[[nodiscard]] bool loadAttachmentTable(const std::filesystem::path& file, AttachmentTable& out, std::string& error);
// gamedata/attachments.toml searched like gamedata/items.toml (core::locateDataFile).
[[nodiscard]] std::optional<std::filesystem::path> locateAttachmentsFile();

// "main" / "off".
[[nodiscard]] std::string_view weaponHandName(WeaponHand h) noexcept;
// The equipment slot a hand draws (MainHand / OffHand).
[[nodiscard]] constexpr EquipSlot equipSlotOf(WeaponHand h) noexcept
{
    return h == WeaponHand::Main ? EquipSlot::MainHand : EquipSlot::OffHand;
}

// weaponWorld = characterWorld * boneModel * A (docs/ASSET_PIPELINE.md §4b "Maths").
[[nodiscard]] assets::Mat4 weaponWorldMatrix(const assets::Mat4& characterWorld, const assets::Mat4& boneModel,
                                             const WeaponAttachment& attachment) noexcept;

} // namespace vo::game
