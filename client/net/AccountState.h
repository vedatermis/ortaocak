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
    // M11: the player confirmed once on this connection that spent stat points cannot be taken back
    // (the Künye asks before the first "Onayla" only).
    bool statSpendConfirmed = false;
    // M12: the same for skill points (the Yetenekler window asks before the first "+" only).
    bool skillSpendConfirmed = false;

    void reset()
    {
        username.clear();
        accountId = 0;
        characters.clear();
        charactersLoaded = false;
        world.reset();
        statSpendConfirmed = false;
        skillSpendConfirmed = false;
    }
};

} // namespace vo::client::net
