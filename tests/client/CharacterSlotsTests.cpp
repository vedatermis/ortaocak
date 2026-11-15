#include "client/scenes/CharacterSlots.h"

#include <catch2/catch_test_macros.hpp>

#include <vector>

using namespace vo::client::scenes;
using vo::net::CharSummary;

namespace {
CharSummary make(std::uint32_t id, const char* name)
{
    CharSummary c;
    c.charId = id;
    c.name = name;
    c.race = 1;
    c.charClass = 1;
    c.level = 1;
    c.zoneId = 1;
    return c;
}
} // namespace

TEST_CASE("Empty account: three empty slots, first selected", "[client][slots]")
{
    CharacterSlots s;
    s.setCharacters({});
    REQUIRE(s.count() == 0);
    REQUIRE(s.hasFreeSlot());
    REQUIRE(s.selectedIndex() == 0);
    REQUIRE(s.selected() == nullptr);
    REQUIRE_FALSE(s.selectedId().has_value());
    for (int i = 0; i < CharacterSlots::kSlotCount; ++i)
        REQUIRE(s.slot(i) == nullptr);
    REQUIRE(s.slot(-1) == nullptr);
    REQUIRE(s.slot(3) == nullptr);
}

TEST_CASE("Selection keeps the preferred or previous character", "[client][slots]")
{
    CharacterSlots s;
    s.setCharacters({make(10, "Ada"), make(20, "Bora"), make(30, "Cem")});
    REQUIRE(s.count() == 3);
    REQUIRE_FALSE(s.hasFreeSlot());
    REQUIRE(s.selectedId() == 10u); // first character by default

    s.setCharacters(s.characters(), 30u);
    REQUIRE(s.selectedIndex() == 2);
    REQUIRE(s.selected()->name == "Cem");

    // A refreshed list in another order keeps the same character selected.
    s.setCharacters({make(30, "Cem"), make(10, "Ada")});
    REQUIRE(s.selectedIndex() == 0);
    REQUIRE(s.selectedId() == 30u);

    // Unknown preferred id falls back to the previous selection.
    s.setCharacters({make(10, "Ada"), make(30, "Cem")}, 99u);
    REQUIRE(s.selectedId() == 30u);

    // More than three entries are cut.
    s.setCharacters({make(1, "A1a"), make(2, "B2b"), make(3, "C3c"), make(4, "D4d")});
    REQUIRE(s.count() == 3);
    REQUIRE(s.indexOf(4) == -1);
}

TEST_CASE("Keyboard navigation is clamped and may land on empty slots", "[client][slots]")
{
    CharacterSlots s;
    s.setCharacters({make(1, "Ada")});
    REQUIRE(s.selectedIndex() == 0);
    REQUIRE_FALSE(s.moveSelection(-1)); // already at the top
    REQUIRE(s.moveSelection(1));
    REQUIRE(s.selectedIndex() == 1);
    REQUIRE(s.selected() == nullptr); // empty slot
    REQUIRE(s.moveSelection(5));
    REQUIRE(s.selectedIndex() == 2);
    REQUIRE_FALSE(s.moveSelection(1));
    REQUIRE(s.select(0));
    REQUIRE_FALSE(s.select(0));
    REQUIRE(s.select(2));
    REQUIRE(s.select(-4)); // clamped to the first slot
    REQUIRE(s.selectedIndex() == 0);
}

TEST_CASE("Adding and removing characters moves the selection sensibly", "[client][slots]")
{
    CharacterSlots s;
    s.setCharacters({make(1, "Ada"), make(2, "Bora")});
    REQUIRE(s.add(make(3, "Cem")));
    REQUIRE(s.selectedId() == 3u);
    REQUIRE_FALSE(s.add(make(4, "Dila"))); // full
    REQUIRE_FALSE(s.hasFreeSlot());

    REQUIRE(s.select(1));
    REQUIRE(s.remove(2)); // middle: the next character moves up into the selected slot
    REQUIRE(s.selectedIndex() == 1);
    REQUIRE(s.selectedId() == 3u);
    REQUIRE(s.remove(3)); // last one: selection moves to the previous character
    REQUIRE(s.selectedIndex() == 0);
    REQUIRE(s.selectedId() == 1u);
    REQUIRE_FALSE(s.remove(3));
    REQUIRE(s.remove(1));
    REQUIRE(s.count() == 0);
    REQUIRE(s.selectedIndex() == 0);
    REQUIRE(s.selected() == nullptr);
    REQUIRE(s.add(make(5, "Ece")));
    REQUIRE(s.selectedIndex() == 0);
    REQUIRE_FALSE(s.add(make(5, "Ece"))); // same id twice
}
