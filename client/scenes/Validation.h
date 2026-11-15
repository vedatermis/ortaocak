// Client-side input validation with specific Turkish messages, built on the shared name rules
// (shared/game/Names.h) so the client never accepts what the server would reject or vice versa.
// Pure logic (vo_client_core), unit-tested.
#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace vo::client::scenes {

// nullopt = valid; otherwise the message to show.
[[nodiscard]] std::optional<std::string> validateUsername(std::string_view username);
[[nodiscard]] std::optional<std::string> validatePassword(std::string_view password);

// Live hints for the character name box: one flag per rule plus the first failing rule's message.
struct CharacterNameCheck
{
    bool empty = true;
    bool lengthOk = false;       // 3..16 characters
    bool charactersOk = false;   // only letters (ASCII + Turkish) and digits
    bool startsWithLetter = false;
    bool valid = false;          // == vo::game::isValidCharacterName
    std::size_t length = 0;      // code points (or bytes if the UTF-8 is broken)
    std::string message;         // empty if valid
};

[[nodiscard]] CharacterNameCheck checkCharacterName(std::string_view name);

} // namespace vo::client::scenes
