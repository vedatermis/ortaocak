// Selection model of the character select screen: three slots (kMaxCharactersPerAccount), each
// holding a character from CHAR_LIST_RES (in server order) or empty, plus the selected slot. The
// selection may sit on an empty slot (activating it opens character creation). Pure logic
// (vo_client_core), unit-tested.
#pragma once

#include "shared/net/Messages.h"
#include "shared/net/Opcodes.h"

#include <cstdint>
#include <optional>
#include <vector>

namespace vo::client::scenes {

class CharacterSlots
{
public:
    static constexpr int kSlotCount = static_cast<int>(vo::net::kMaxCharactersPerAccount);

    // Replaces the characters (extra entries beyond kSlotCount are ignored). Selects the character
    // with `preferId` if present, else keeps the previously selected character if still present,
    // else the first character, else slot 0.
    void setCharacters(std::vector<vo::net::CharSummary> characters,
                       std::optional<std::uint32_t> preferId = std::nullopt);
    // Appends a character into the first empty slot and selects it. False if there is no free slot.
    bool add(const vo::net::CharSummary& character);
    // Removes a character; the selection moves to the character now in that slot, else the
    // previous one, else slot 0. False if the id was not present.
    bool remove(std::uint32_t charId);

    [[nodiscard]] const vo::net::CharSummary* slot(int index) const noexcept;
    [[nodiscard]] int count() const noexcept { return static_cast<int>(m_characters.size()); }
    [[nodiscard]] bool hasFreeSlot() const noexcept { return count() < kSlotCount; }
    [[nodiscard]] const std::vector<vo::net::CharSummary>& characters() const noexcept { return m_characters; }

    [[nodiscard]] int selectedIndex() const noexcept { return m_selected; }
    [[nodiscard]] const vo::net::CharSummary* selected() const noexcept { return slot(m_selected); }
    [[nodiscard]] std::optional<std::uint32_t> selectedId() const noexcept;
    [[nodiscard]] int indexOf(std::uint32_t charId) const noexcept;

    // Clamped to [0, kSlotCount). Returns true if the selection changed.
    bool select(int index) noexcept;
    // Moves by `delta` slots, clamped (no wrap-around).
    bool moveSelection(int delta) noexcept;

private:
    std::vector<vo::net::CharSummary> m_characters;
    int m_selected = 0;
};

} // namespace vo::client::scenes
