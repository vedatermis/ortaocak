// Client upgrade logic (M16 "Tav", docs/PROTOCOL_UPGRADE.md §7; client/world/UpgradeView.h): the item names
// with "(+N)" and their colours, the tooltip lines of an upgraded item, the Yükselt section's view (stone,
// owned, fee, chance, failure, why not), the confirmation and result texts, the session (choice, request,
// answer, destruction) and the automation parser.

#include "client/world/LootView.h"
#include "client/world/UpgradeView.h"
#include "shared/game/Items.h"
#include "shared/game/Upgrade.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <format>
#include <string>
#include <vector>

namespace vg = vo::game;
namespace vn = vo::net;
namespace cw = vo::client::world;
using Catch::Matchers::ContainsSubstring;
using UR = vn::UpgradeResult;

namespace {

const vg::ItemCatalog& items()
{
    static const vg::ItemCatalog c = [] {
        vg::ItemCatalog out;
        std::string error;
        const auto file = vg::locateItemsFile();
        REQUIRE(file);
        REQUIRE(vg::loadItemCatalog(*file, out, error));
        return out;
    }();
    return c;
}

vg::Inventory bag(const std::vector<vg::StoredItem>& rows)
{
    vg::Inventory inv;
    std::string error;
    INFO(error);
    REQUIRE(vg::Inventory::fromItems(rows, inv, error));
    return inv;
}

bool hasLine(const std::vector<cw::TipLine>& lines, const std::string& text)
{
    for (const auto& l : lines)
        if (l.text == text)
            return true;
    return false;
}

const vg::ItemTemplate* firstUnique()
{
    for (const vg::ItemTemplate& t : items().templates())
        if (t.rarity == vg::Rarity::Unique)
            return &t;
    return nullptr;
}

} // namespace

TEST_CASE("Upgrade view: names with (+N), the +7 colour and the tooltip", "[client][upgrade]")
{
    const vg::ItemTemplate& sword = *items().find(7); // Burç Kılıcı, attack 9
    CHECK(cw::itemDisplayName(sword, 0) == "Burç Kılıcı");
    CHECK(cw::itemDisplayName(sword, 7) == "Burç Kılıcı (+7)");
    CHECK(cw::itemNameRgb(sword, 6) == cw::rarityRgb(sword.rarity));
    CHECK(cw::itemNameRgb(sword, 7) == cw::kUpgradeHighRgb);
    CHECK(cw::kUpgradeHighRgb != cw::rarityRgb(vg::Rarity::Unique)); // not the uniques' amber
    CHECK(cw::kUpgradeHighRgb != cw::rarityRgb(vg::Rarity::Rare));
    CHECK(cw::kUpgradeHighRgb != cw::rarityRgb(vg::Rarity::Uncommon));
    const vg::ItemTemplate* unique = firstUnique();
    REQUIRE(unique);
    CHECK(cw::itemNameRgb(*unique, 9) == cw::rarityRgb(vg::Rarity::Unique)); // a unique keeps its amber

    const vg::ItemUser user{1, 5, {}};
    const auto plain = cw::itemTipLines(sword, user, 1);
    CHECK(plain.front().text == "Burç Kılıcı");
    CHECK(hasLine(plain, "Saldırı 8–10")); // M18: the damage range around 9
    const auto up = cw::itemTipLines(sword, user, 1, 7);
    CHECK(up.front().text == "Burç Kılıcı (+7)");
    CHECK(up.front().rgb == cw::kUpgradeHighRgb);
    CHECK(hasLine(up, std::format("Saldırı {}–{} (+{} tav)", 8 + vg::upgradeBonus(9, 7), 10 + vg::upgradeBonus(9, 7),
                                  vg::upgradeBonus(9, 7))));
    CHECK(hasLine(up, "Yükseltme: +7 / +10"));
    // +1 on a 9: the bonus rounds to 0 - the plain line, the level still shown.
    const auto one = cw::itemTipLines(sword, user, 1, 1);
    CHECK(hasLine(one, "Saldırı 8–10"));
    CHECK(hasLine(one, "Yükseltme: +1 / +10"));
}

TEST_CASE("Upgrade view: the section for the chosen item", "[client][upgrade]")
{
    const auto& c = items();
    // Bag: 0 Ocak Baltası +0 (tier 1), 1 two Ham stones, 2 Kül Tozu, 3 an axe +5, 4 an axe +10.
    const auto inv = bag({{1, vg::ItemPlace::bag(0), 6, 1, 0},
                          {2, vg::ItemPlace::bag(1), 166, 2, 0},
                          {3, vg::ItemPlace::bag(2), 9, 4, 0},
                          {4, vg::ItemPlace::bag(3), 6, 1, 5},
                          {5, vg::ItemPlace::bag(4), 6, 1, 10}});
    // Nothing chosen: the hint.
    auto v = cw::upgradePanelView(c, inv, 100, std::nullopt);
    CHECK_FALSE(v.hasItem);
    CHECK(v.blockText == cw::kUpgradeHintText);
    CHECK_FALSE(v.canAttempt);

    v = cw::upgradePanelView(c, inv, 100, 0);
    REQUIRE(v.hasItem);
    CHECK(v.name == "Ocak Baltası");
    CHECK(v.levelLine == "+0 → +1");
    CHECK(v.statLine == std::format("Saldırı 10 → {}", 10 + vg::upgradeBonus(10, 1)));
    CHECK(v.stone == vg::StoneGrade::Ham);
    CHECK(v.stoneName == "Ham Tav Taşı");
    CHECK(v.stoneTemplate == 166);
    CHECK(v.stonesOwned == 2);
    CHECK(v.fee == 6);
    CHECK(v.affordable);
    CHECK(v.chanceText == "%100");
    CHECK(v.failText == "Başarısız olamaz.");
    CHECK_FALSE(v.needsConfirm);
    CHECK(v.canAttempt);
    CHECK(v.blockText.empty());
    // Too poor: blocked with the reason.
    v = cw::upgradePanelView(c, inv, 5, 0);
    CHECK_FALSE(v.canAttempt);
    CHECK_FALSE(v.affordable);
    CHECK(v.blockText == "Yeterli Akçen yok.");
    // +5 -> +6: 45 %, the item would be destroyed: ask first.
    v = cw::upgradePanelView(c, inv, 1000, 3);
    CHECK(v.name == "Ocak Baltası (+5)");
    CHECK(v.target == 6);
    CHECK(v.chanceText == "%45");
    CHECK(v.failDestroys);
    CHECK(v.needsConfirm);
    CHECK_THAT(v.failText, ContainsSubstring("YOK OLUR"));
    const std::string q = cw::upgradeConfirmText(v);
    CHECK_THAT(q, ContainsSubstring("Ocak Baltası (+5) → +6"));
    CHECK_THAT(q, ContainsSubstring("%45"));
    CHECK_THAT(q, ContainsSubstring("YOK OLUR"));
    CHECK_THAT(q, ContainsSubstring("Ham Tav Taşı"));
    CHECK_THAT(q, ContainsSubstring(std::format("{} Akçe", vg::upgradeFeeForTier(1, 6))));
    // +10: the end; a material: not upgradable.
    v = cw::upgradePanelView(c, inv, 1000, 4);
    CHECK_FALSE(v.canAttempt);
    CHECK_THAT(v.blockText, ContainsSubstring("en yüksek seviyede (+10)"));
    v = cw::upgradePanelView(c, inv, 1000, 2);
    CHECK(v.hasItem);
    CHECK_FALSE(v.canAttempt);
    CHECK_THAT(v.blockText, ContainsSubstring("yükseltilemez"));
    // No stone of the grade: blocked, the stone named.
    const auto none = bag({{1, vg::ItemPlace::bag(0), 6, 1, 0}});
    v = cw::upgradePanelView(c, none, 1000, 0);
    CHECK(v.stonesOwned == 0);
    CHECK_FALSE(v.canAttempt);
    CHECK(v.blockText == "Çantanda Ham Tav Taşı yok.");
    // +3 -> +4: 80 %, a failure keeps it.
    const auto at3 = bag({{1, vg::ItemPlace::bag(0), 6, 1, 3}, {2, vg::ItemPlace::bag(1), 166, 1, 0}});
    v = cw::upgradePanelView(c, at3, 1000, 0);
    CHECK(v.chanceText == "%80");
    CHECK_FALSE(v.failDestroys);
    CHECK(v.failText == "Tutmazsa eşya olduğu gibi kalır.");
    CHECK(cw::chancePercentText(30) == "%3");
    CHECK(cw::chancePercentText(85) == "%8,5");
}

TEST_CASE("Upgrade view: result texts", "[client][upgrade]")
{
    const vg::ItemTemplate* axe = items().find(6);
    REQUIRE(axe);
    CHECK(cw::upgradeResultText(UR::Success, axe, 5) == "Tav tuttu! Ocak Baltası (+5) oldu.");
    CHECK(cw::upgradeResultText(UR::Failed, axe, 3) == "Tav tutmadı: Ocak Baltası (+3) olduğu gibi kaldı.");
    CHECK(cw::upgradeResultText(UR::Destroyed, axe, 6) == "Tav tutmadı: Ocak Baltası (+6) parçalandı!");
    CHECK(cw::upgradeResultText(UR::NotSmith, axe, 0) == "Bu satıcı eşya yükseltmez.");
    CHECK(cw::upgradeResultText(UR::NoStone, axe, 0) == "Çantanda Ham Tav Taşı yok.");
    CHECK(cw::upgradeResultText(UR::WrongStone, axe, 0) == "Bu eşyaya Ham Tav Taşı gerekir.");
    CHECK(cw::upgradeResultText(UR::NotEnoughCoins, axe, 0) == "Yeterli Akçen yok.");
    for (std::uint8_t r = 0; r <= vn::kUpgradeResultMax; ++r)
    {
        CHECK_FALSE(cw::upgradeResultText(static_cast<UR>(r), axe, 1).empty());
        CHECK_FALSE(cw::upgradeResultText(static_cast<UR>(r), nullptr, 0).empty());
    }
    CHECK_FALSE(cw::upgradeResultIsBad(UR::Success));
    CHECK(cw::upgradeResultIsBad(UR::Destroyed));
}

TEST_CASE("Upgrade view: session - choice, one request, answers, destruction", "[client][upgrade]")
{
    auto inv = bag({{1, vg::ItemPlace::bag(0), 6, 1, 4}, {2, vg::ItemPlace::bag(1), 166, 2, 0}});
    cw::UpgradeSession s;
    CHECK_FALSE(s.pick(inv, 5)); // empty
    CHECK_FALSE(s.begin(inv, 9));
    REQUIRE(s.pick(inv, 0));
    CHECK(s.slot() == 0);
    CHECK(s.itemId() == 1);
    CHECK_FALSE(s.begin(inv, 0)); // no npc
    const auto req = s.begin(inv, 9);
    REQUIRE(req);
    CHECK(req->npcId == 9);
    CHECK(req->bagSlot == 0);
    CHECK(req->itemId == 1);
    CHECK(req->fromLevel == 4);
    CHECK(req->stoneSlot == vn::kUpgradeAnyStone);
    CHECK(s.busy());
    CHECK_FALSE(s.begin(inv, 9)); // one at a time
    CHECK_FALSE(s.finish(vn::UpgradeRes{req->seq + 1, UR::Success, 0, 6, 5, 166, 30}, 1.0)); // not ours
    const auto done = s.finish(vn::UpgradeRes{req->seq, UR::Destroyed, 0, 6, 4, 166, 30}, 2.0);
    REQUIRE(done);
    CHECK_FALSE(s.busy());
    REQUIRE(s.outcome());
    CHECK(s.outcome()->result == UR::Destroyed);
    CHECK(s.outcome()->at == 2.0);
    CHECK_FALSE(s.slot()); // the item is gone
    // The choice follows the bag: an item that left its slot is dropped.
    REQUIRE(s.pick(inv, 1));
    inv = bag({{2, vg::ItemPlace::bag(3), 166, 2, 0}});
    s.sync(inv);
    CHECK_FALSE(s.slot());
    // A forced request (automation) and a forgotten one.
    REQUIRE(s.beginRaw(9, 27, 0, 0, 3, 0));
    s.close(true);
    CHECK_FALSE(s.busy());
    CHECK_FALSE(s.outcome());
}

TEST_CASE("Upgrade view: automation parser", "[client][upgrade]")
{
    std::string error;
    auto c = cw::parseUpgradeCommand("shoptab upgrade", error);
    REQUIRE(c);
    CHECK(c->verb == cw::UpgradeVerb::Tab);
    CHECK(c->text == "upgrade");
    CHECK_FALSE(cw::parseUpgradeCommand("shoptab sell", error));
    CHECK_THAT(error, ContainsSubstring("usage"));
    c = cw::parseUpgradeCommand("upgradepick 4", error);
    REQUIRE(c);
    CHECK(c->a == 4);
    CHECK_FALSE(cw::parseUpgradeCommand("upgradepick 28", error));
    c = cw::parseUpgradeCommand("upgradeforce 3 any 5", error);
    REQUIRE(c);
    CHECK(c->verb == cw::UpgradeVerb::Force);
    CHECK(c->b == vn::kUpgradeAnyStone);
    CHECK(c->hasC);
    CHECK(c->c == 5);
    c = cw::parseUpgradeCommand("upgradeforce 3 7", error);
    REQUIRE(c);
    CHECK(c->b == 7);
    CHECK_FALSE(c->hasC);
    c = cw::parseUpgradeCommand("expectupgrade 2 7", error);
    REQUIRE(c);
    CHECK(c->verb == cw::UpgradeVerb::ExpectLevel);
    CHECK_FALSE(cw::parseUpgradeCommand("expectupgrade 2 11", error));
    c = cw::parseUpgradeCommand("expectupgradepanel Başarı şansı: %45", error);
    REQUIRE(c);
    CHECK(c->text == "Başarı şansı: %45");
    CHECK(cw::parseUpgradeCommand("upgradestart", error)->verb == cw::UpgradeVerb::Start);
    CHECK_FALSE(cw::parseUpgradeCommand("upgradestart now", error));
    CHECK_FALSE(cw::parseUpgradeCommand("talk Demirci", error)); // not ours
    CHECK(error.empty());
}
