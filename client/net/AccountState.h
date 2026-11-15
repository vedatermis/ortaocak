// What the client knows about the logged-in account while it moves between the login, character
// and world scenes. Owned by GameClient and reset on every new connection.
#pragma once

#include "shared/net/Messages.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace vo::client::net {

struct WorldEntry
{
    vo::net::CharSummary character;
    vo::net::EnterWorldRes enter;
};

struct AccountState
{
    std::string username;
    std::uint32_t accountId = 0;
    std::vector<vo::net::CharSummary> characters; // last CHAR_LIST_RES (kept up to date locally)
    bool charactersLoaded = false;
    std::optional<WorldEntry> world; // set while in the world

    void reset()
    {
        username.clear();
        accountId = 0;
        characters.clear();
        charactersLoaded = false;
        world.reset();
    }
};

} // namespace vo::client::net
