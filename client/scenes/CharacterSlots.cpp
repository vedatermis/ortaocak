#include "client/scenes/CharacterSlots.h"

#include <algorithm>

namespace vo::client::scenes {

void CharacterSlots::setCharacters(std::vector<vo::net::CharSummary> characters,
                                   std::optional<std::uint32_t> preferId)
{
    const std::optional<std::uint32_t> previous = selectedId();
    m_characters = std::move(characters);
    if (m_characters.size() > static_cast<std::size_t>(kSlotCount))
        m_characters.resize(static_cast<std::size_t>(kSlotCount));

    int index = -1;
    if (preferId)
        index = indexOf(*preferId);
    if (index < 0 && previous)
        index = indexOf(*previous);
    if (index < 0)
        index = 0; // first character, or the first (empty) slot
    m_selected = index;
}

bool CharacterSlots::add(const vo::net::CharSummary& character)
{
    if (!hasFreeSlot() || indexOf(character.charId) >= 0)
        return false;
    m_characters.push_back(character);
    m_selected = count() - 1;
    return true;
}

bool CharacterSlots::remove(std::uint32_t charId)
{
    const int index = indexOf(charId);
    if (index < 0)
        return false;
    m_characters.erase(m_characters.begin() + index);
    if (index < count())
        m_selected = index;
    else
        m_selected = std::max(0, count() - 1);
    return true;
}

const vo::net::CharSummary* CharacterSlots::slot(int index) const noexcept
{
    if (index < 0 || index >= count())
        return nullptr;
    return &m_characters[static_cast<std::size_t>(index)];
}

std::optional<std::uint32_t> CharacterSlots::selectedId() const noexcept
{
    if (const auto* c = selected())
        return c->charId;
    return std::nullopt;
}

int CharacterSlots::indexOf(std::uint32_t charId) const noexcept
{
    for (int i = 0; i < count(); ++i)
    {
        if (m_characters[static_cast<std::size_t>(i)].charId == charId)
            return i;
    }
    return -1;
}

bool CharacterSlots::select(int index) noexcept
{
    const int clamped = std::clamp(index, 0, kSlotCount - 1);
    if (clamped == m_selected)
        return false;
    m_selected = clamped;
    return true;
}

bool CharacterSlots::moveSelection(int delta) noexcept
{
    return select(m_selected + delta);
}

} // namespace vo::client::scenes
