// M11 "Künye" client logic (client/world/StatsView.h, the Miss number of CombatView.h, the attribute
// lines of LootView.h's item tooltips): the pending-allocation model (add / remove / caps / confirm /
// cancel / answers), CHAR_STATS and STAT_ALLOCATE_RES handling, the preview against the shared
// game::deriveStats, the Turkish texts, the Esc order with the Künye and the automation parser.
#include "client/world/CombatView.h"
#include "client/world/LootView.h"
#include "client/world/StatsView.h"
#include "shared/game/Character.h"
#include "shared/game/Combat.h"
#include "shared/game/Items.h"

#include <catch2/catch_test_macros.hpp>

#include <set>
#include <string>

namespace world = vo::client::world;
namespace vg = vo::game;
namespace vn = vo::net;
using vg::Attribute;

namespace {

constexpr std::uint8_t kWarrior = 1;
constexpr std::uint8_t kRogue = 2;
constexpr std::uint8_t kMage = 3;
constexpr std::uint8_t kPriest = 4;

const vg::ItemCatalog& catalog()
{
    static const vg::ItemCatalog c = [] {
        vg::ItemCatalog out;
        const auto path = vg::locateItemsFile();
        REQUIRE(path.has_value());
        std::string error;
        REQUIRE(vg::loadItemCatalog(*path, out, error));
        return out;
    }();
    return c;
}

// CHAR_STATS as the server builds it (ZoneCombat sendCharStats) for a character.
vn::CharStats serverStats(std::uint8_t cls, std::uint16_t level, const vg::Attributes& allocated, vg::EquipBonus equip = {})
{
    return world::toCharStats(vg::deriveStats(cls, level, allocated, equip), level);
}

// Through the wire format, as the client receives it.
vn::CharStats roundTrip(const vn::CharStats& m)
{
    const auto bytes = vn::encodeMessage(m);
    REQUIRE(bytes.has_value());
    CHECK(bytes->size() == 64); // PROTOCOL_STATS.md §3.1 (48), PROTOCOL_EQUIPMENT.md §4.1: v12 appends 16 bytes
    vn::CharStats out;
    REQUIRE(vn::decodeMessage(*bytes, out));
    return out;
}

constexpr std::size_t idx(Attribute a)
{
    return vg::attributeIndex(a);
}

} // namespace

TEST_CASE("Stats view: a fresh level-1 sheet has nothing to spend", "[client][stats]")
{
    world::StatSheet s;
    CHECK_FALSE(s.known());
    CHECK_FALSE(s.canAdd(0)); // nothing known yet
    s.applyCharStats(roundTrip(serverStats(kWarrior, 1, {})));
    REQUIRE(s.known());
    CHECK(s.unspent() == 0);
    CHECK(s.base(idx(Attribute::Strength)) == 12);
    CHECK(s.base(idx(Attribute::Vitality)) == 11);
    for (std::size_t i = 0; i < vg::kAttributeCount; ++i)
    {
        CHECK_FALSE(s.canAdd(i));
        CHECK_FALSE(s.canRemove(i));
        CHECK(s.add(i) == 0);
    }
    CHECK(s.allocated(kWarrior) == vg::Attributes{});
    CHECK_FALSE(s.beginConfirm(0.0).has_value());
}

TEST_CASE("Stats view: pending points - add, remove, Shift step, totals and cancel", "[client][stats]")
{
    world::StatSheet s;
    s.applyCharStats(serverStats(kWarrior, 2, {})); // 3 points
    REQUIRE(s.unspent() == 3);
    CHECK(s.add(idx(Attribute::Strength)) == 1);
    CHECK(s.add(idx(Attribute::Strength)) == 1);
    CHECK(s.add(idx(Attribute::Vitality)) == 1);
    CHECK(s.pending(idx(Attribute::Strength)) == 2);
    CHECK(s.pending(idx(Attribute::Vitality)) == 1);
    CHECK(s.pendingTotal() == 3);
    CHECK(s.remaining() == 0);
    CHECK_FALSE(s.canAdd(idx(Attribute::Dexterity))); // no points left
    CHECK(s.add(idx(Attribute::Dexterity)) == 0);
    CHECK(s.canRemove(idx(Attribute::Strength)));
    CHECK_FALSE(s.canRemove(idx(Attribute::Dexterity)));
    CHECK(s.remove(idx(Attribute::Strength)) == 1);
    CHECK(s.pending(idx(Attribute::Strength)) == 1);
    CHECK(s.remaining() == 1);
    CHECK(s.remove(idx(Attribute::Dexterity)) == 0);
    // "Vazgeç" drops everything; the server's values are untouched.
    CHECK(s.clearPending());
    CHECK_FALSE(s.hasPending());
    CHECK(s.unspent() == 3);
    CHECK_FALSE(s.clearPending()); // nothing to drop

    // Shift+click: 5 at once, or the rest.
    world::StatSheet ten;
    ten.applyCharStats(serverStats(kWarrior, 10, {})); // 27 points
    REQUIRE(ten.unspent() == 27);
    CHECK(ten.add(idx(Attribute::Strength), 5) == 5);
    CHECK(ten.add(idx(Attribute::Strength), 5) == 5);
    CHECK(ten.add(idx(Attribute::Strength), 5) == 5);
    CHECK(ten.add(idx(Attribute::Strength), 5) == 5);
    CHECK(ten.add(idx(Attribute::Strength), 5) == 5);
    CHECK(ten.add(idx(Attribute::Strength), 5) == 2); // the rest
    CHECK(ten.pending(idx(Attribute::Strength)) == 27);
    CHECK(ten.remaining() == 0);
}

TEST_CASE("Stats view: the 255 cap per attribute", "[client][stats]")
{
    vn::CharStats m = serverStats(kWarrior, 10, {});
    m.unspent = 27;
    m.attributes[idx(Attribute::Strength)].base = 253;
    m.attributes[idx(Attribute::Dexterity)].base = 255;
    world::StatSheet s;
    s.applyCharStats(m);
    CHECK_FALSE(s.canAdd(idx(Attribute::Dexterity)));
    CHECK(s.add(idx(Attribute::Strength), 5) == 2); // 253 + 2 = 255
    CHECK_FALSE(s.canAdd(idx(Attribute::Strength)));
    CHECK(s.add(idx(Attribute::Strength)) == 0);
    CHECK(s.canAdd(idx(Attribute::Vitality)));
    // A CHAR_STATS that raises the base trims pending points past the cap.
    m.attributes[idx(Attribute::Strength)].base = 254;
    s.applyCharStats(m);
    CHECK(s.pending(idx(Attribute::Strength)) == 1);
}

TEST_CASE("Stats view: confirm sends all five amounts once, CHAR_STATS ends the wait", "[client][stats]")
{
    world::StatSheet s;
    s.applyCharStats(serverStats(kWarrior, 2, {}));
    s.add(idx(Attribute::Strength), 2);
    s.add(idx(Attribute::Vitality), 1);
    const auto req = s.beginConfirm(10.0);
    REQUIRE(req.has_value());
    CHECK(req->seq != 0);
    CHECK(req->points == std::array<std::uint16_t, 5>{2, 0, 1, 0, 0});
    CHECK(vn::encodeMessage(*req)->size() == 14);
    // In flight: everything is disabled, no second request.
    CHECK(s.busy());
    CHECK(s.inFlight());
    CHECK_FALSE(s.canAdd(idx(Attribute::Dexterity)));
    CHECK_FALSE(s.canRemove(idx(Attribute::Strength)));
    CHECK_FALSE(s.clearPending());
    CHECK_FALSE(s.beginConfirm(10.1).has_value());
    // An answer for another seq is ignored.
    auto other = s.finish({req->seq + 7, vn::StatAllocateResult::Ok, 0}, 10.2);
    CHECK_FALSE(other.matched);
    CHECK(s.inFlight());
    // Ok: the points stay shown (as the preview) until CHAR_STATS carries them.
    auto a = s.finish({req->seq, vn::StatAllocateResult::Ok, 0}, 10.2);
    CHECK(a.matched);
    CHECK(a.result == vn::StatAllocateResult::Ok);
    CHECK_FALSE(s.inFlight());
    CHECK(s.awaitingStats());
    CHECK(s.busy());
    CHECK(s.pendingTotal() == 3);
    s.applyCharStats(roundTrip(serverStats(kWarrior, 2, {2, 0, 1, 0, 0})));
    CHECK_FALSE(s.busy());
    CHECK_FALSE(s.hasPending());
    CHECK(s.base(idx(Attribute::Strength)) == 14);
    CHECK(s.base(idx(Attribute::Vitality)) == 12);
    CHECK(s.unspent() == 0);
    CHECK(s.allocated(kWarrior) == vg::Attributes{2, 0, 1, 0, 0});
    // The next request gets a new seq.
    world::StatSheet t;
    t.applyCharStats(serverStats(kWarrior, 3, {}));
    t.add(0);
    const auto r1 = t.beginConfirm(0.0);
    REQUIRE(r1);
    (void)t.finish({r1->seq, vn::StatAllocateResult::Ok, 5}, 0.0);
    t.applyCharStats(serverStats(kWarrior, 3, {1, 0, 0, 0, 0}));
    t.add(0);
    const auto r2 = t.beginConfirm(1.0);
    REQUIRE(r2);
    CHECK(r2->seq != r1->seq);
}

TEST_CASE("Stats view: refusals keep the pending points, trimmed to the answer", "[client][stats]")
{
    world::StatSheet s;
    s.applyCharStats(serverStats(kWarrior, 3, {})); // 6 points
    s.add(idx(Attribute::Strength), 4);
    s.add(idx(Attribute::Spirit), 2);
    auto req = s.beginConfirm(0.0);
    REQUIRE(req);
    // ServerError: nothing changed, everything stays pending (try again).
    auto a = s.finish({req->seq, vn::StatAllocateResult::ServerError, 6}, 0.1);
    CHECK(a.matched);
    CHECK_FALSE(s.busy());
    CHECK(s.pendingTotal() == 6);
    // NotEnoughPoints (the server knows fewer points, e.g. after reset-stats raced): trimmed.
    req = s.beginConfirm(0.2);
    REQUIRE(req);
    a = s.finish({req->seq, vn::StatAllocateResult::NotEnoughPoints, 3}, 0.3);
    CHECK(a.result == vn::StatAllocateResult::NotEnoughPoints);
    CHECK(s.unspent() == 3);
    CHECK(s.pendingTotal() == 3);
    CHECK(s.pending(idx(Attribute::Strength)) == 3);
    CHECK(s.pending(idx(Attribute::Spirit)) == 0);
}

TEST_CASE("Stats view: an accepted request without CHAR_STATS expires", "[client][stats]")
{
    world::StatSheet s;
    s.applyCharStats(serverStats(kRogue, 2, {}));
    s.add(idx(Attribute::Dexterity), 3);
    const auto req = s.beginConfirm(1.0);
    REQUIRE(req);
    (void)s.finish({req->seq, vn::StatAllocateResult::Ok, 0}, 1.0);
    s.expire(2.0);
    CHECK(s.busy());
    s.expire(4.5);
    CHECK_FALSE(s.busy());
    CHECK_FALSE(s.hasPending());
}

TEST_CASE("Stats view: a level-up CHAR_STATS while points are pending keeps them", "[client][stats]")
{
    world::StatSheet s;
    s.applyCharStats(serverStats(kMage, 2, {}));
    s.add(idx(Attribute::Intelligence), 2);
    s.applyCharStats(serverStats(kMage, 3, {}));
    CHECK(s.unspent() == 6);
    CHECK(s.pending(idx(Attribute::Intelligence)) == 2);
    CHECK(s.remaining() == 4);
}

TEST_CASE("Stats view: the preview equals the shared deriveStats", "[client][stats]")
{
    const vg::Inventory kit = [] {
        vg::Inventory inv;
        // Warrior kit (sword + oak shield) plus the warden's shield instead of the oak one.
        inv.at(vg::ItemPlace::equipment(vg::EquipSlot::MainHand)) = {1, 1, 1};
        inv.at(vg::ItemPlace::equipment(vg::EquipSlot::OffHand)) = {2, 15, 1};
        return inv;
    }();
    const vg::EquipBonus equip = vg::equipmentBonus(catalog(), kit);
    REQUIRE(equip.attributes[idx(Attribute::Vitality)] == 3); // Bekçi Kalkanı

    for (const std::uint8_t cls : {kWarrior, kRogue, kMage, kPriest})
    {
        world::StatSheet s;
        const vg::Attributes spent{4, 1, 3, 0, 1};
        s.applyCharStats(roundTrip(serverStats(cls, 10, spent, equip)));
        CHECK(s.allocated(cls) == spent);
        CHECK(s.bonus(idx(Attribute::Vitality)) == 3);
        // Without pending points the client's derivation is the server's CHAR_STATS.
        CHECK(world::toCharStats(s.current(cls, equip), 10) == s.stats());
        // With pending points: exactly deriveStats of the sum.
        s.add(idx(Attribute::Strength), 5);
        s.add(idx(Attribute::Vitality), 4);
        s.add(idx(Attribute::Intelligence), 2);
        const vg::Attributes total{9, 1, 7, 2, 1};
        CHECK(s.preview(cls, equip) == vg::deriveStats(cls, 10, total, equip));
        // The preview ignores whatever attribute bonus `equip` carries: CHAR_STATS' bonus counts.
        vg::EquipBonus noAttr = equip;
        noAttr.attributes = {};
        CHECK(s.preview(cls, noAttr) == vg::deriveStats(cls, 10, total, equip));
    }
}

TEST_CASE("Stats view: derived rows, units and the preview column", "[client][stats]")
{
    const vn::CharStats now = serverStats(kWarrior, 1, {});
    auto rows = world::derivedRows(kWarrior, now, nullptr);
    REQUIRE(rows.size() == 11); // no heal row; M18: Temel Vuruş and Erim
    CHECK(rows[0].label == "Can");
    CHECK(rows[0].value == "90");
    CHECK(rows[2].label == "Saldırı Gücü");
    CHECK(rows[3].label == "Temel Vuruş");
    CHECK(rows[5].label == "Kritik Vuruş");
    CHECK(rows[5].value == "%5,0");
    CHECK(rows[6].label == "Kaçınma");
    CHECK(rows[10].label == "Erim");
    CHECK(rows[10].value == "2,5 m");
    CHECK(rows[6].value == "%2,4");
    CHECK(rows[7].label == "Can Yenilenmesi");
    CHECK(rows[7].value.ends_with("/sn"));
    CHECK(rows[9].label == "Saldırı Hızı");
    CHECK(rows[9].value == "1,6 sn");
    for (const auto& r : rows)
        CHECK(r.preview.empty());

    const vn::CharStats more = serverStats(kWarrior, 2, {2, 0, 1, 0, 0});
    rows = world::derivedRows(kWarrior, serverStats(kWarrior, 2, {}), &more);
    CHECK_FALSE(rows[0].preview.empty()); // Can grows with Dayanıklılık
    CHECK(rows[1].preview.empty());       // Mana does not
    CHECK_FALSE(rows[2].preview.empty()); // Saldırı Gücü grows with Güç
    const std::string text = world::derivedRowsText(rows);
    CHECK(text.find("Can ") != std::string::npos);
    CHECK(text.find(" → ") != std::string::npos);

    CHECK(world::derivedRows(kMage, now, nullptr)[2].label == "Büyü Gücü");
    const auto priest = world::derivedRows(kPriest, serverStats(kPriest, 1, {}), nullptr);
    REQUIRE(priest.size() == 12);
    CHECK(priest.back().label == "İyileştirme");
    CHECK(priest.back().value.starts_with("%"));

    CHECK(world::formatDecimal(12.5) == "12,5");
    CHECK(world::formatDecimal(0.0) == "0,0");
    CHECK(world::derivedValue(now, world::DerivedField::Crit) == 5.0);
    CHECK(world::derivedValue(now, world::DerivedField::AttackSpeed) == 1.6);
}

TEST_CASE("Stats view: Turkish texts", "[client][stats]")
{
    CHECK(world::statResultText(vn::StatAllocateResult::Ok).empty());
    CHECK(world::statResultText(vn::StatAllocateResult::InvalidAmount) == "Dağıtılacak puan seçilmedi");
    CHECK(world::statResultText(vn::StatAllocateResult::NotEnoughPoints) == "Yeterli puanın yok");
    CHECK(world::statResultText(vn::StatAllocateResult::AttributeCap) == "Bu nitelik en yüksek değerde (255)");
    CHECK(world::statResultText(vn::StatAllocateResult::ServerError) == "Puanlar kaydedilemedi, tekrar dene");
    CHECK(world::unspentText(3) == "Dağıtılmamış puan: 3");
    CHECK(world::levelUpPointsText(3).starts_with("+3 nitelik puanı"));
    CHECK(world::attributeRowText(Attribute::Vitality, 14, 3, 2) == "Dayanıklılık 14 (+3) +2");
    CHECK(world::attributeRowText(Attribute::Strength, 12, 0, 0) == "Güç 12");

    // Tooltips: the contract's sentence, then what it means for the class.
    const std::string str = world::attributeTooltip(Attribute::Strength, kWarrior);
    CHECK(str.starts_with("Savaşçı ve Şifacı saldırı gücünü artırır."));
    CHECK(str.find("Savaşçı olarak saldırı gücün bu niteliğe bağlı") != std::string::npos);
    CHECK(world::attributeTooltip(Attribute::Strength, kMage).find("saldırı gücünü artırmaz") != std::string::npos);
    CHECK(world::attributeTooltip(Attribute::Intelligence, kMage).find("büyü gücün bu niteliğe bağlı") != std::string::npos);
    CHECK(world::attributeTooltip(Attribute::Dexterity, kRogue).find("Haydut olarak saldırı gücün") != std::string::npos);
    CHECK(world::attributeTooltip(Attribute::Spirit, kPriest).find("Soluk Güneş") != std::string::npos);
    std::set<std::string> distinct;
    for (std::size_t i = 0; i < vg::kAttributeCount; ++i)
        distinct.insert(world::attributeTooltip(static_cast<Attribute>(i), kWarrior));
    CHECK(distinct.size() == vg::kAttributeCount);
}

TEST_CASE("Stats view: dodges show a grey Iska", "[client][stats][combat]")
{
    CHECK(world::classifyCombatNumber(vn::kCombatKindMiss, 0, true) == world::FloatKind::Miss);
    CHECK(world::classifyCombatNumber(vn::kCombatKindMiss, 0, false) == world::FloatKind::Miss);
    CHECK(world::floatText(world::FloatKind::Miss, 0) == "Iska");
    CHECK(world::floatText(world::FloatKind::Heal, 12) == "+12");
    CHECK(world::floatText(world::FloatKind::Critical, 34) == "34!");
    CHECK(world::floatText(world::FloatKind::TakenDamage, 7) == "7");
    // Muted: distinct from every damage colour, and not brighter than white damage numbers.
    const auto miss = world::floatStyle(world::FloatKind::Miss);
    for (const auto k : {world::FloatKind::Damage, world::FloatKind::Critical, world::FloatKind::Heal,
                         world::FloatKind::TakenDamage})
        CHECK(world::floatStyle(k).rgb != miss.rgb);
    CHECK(miss.fontScale <= 1.0f);
}

TEST_CASE("Stats view: item tooltips list attribute bonuses and requirements", "[client][stats][items]")
{
    const vg::ItemTemplate* shield = catalog().find(15); // Bekçi Kalkanı, Dayanıklılık +3
    REQUIRE(shield != nullptr);
    const vg::ItemUser warrior{kWarrior, 10, vg::baseAttributes(kWarrior, {})};
    auto lines = world::itemTipLines(*shield, warrior, 1);
    bool bonus = false;
    for (const auto& l : lines)
        if (l.text == "Dayanıklılık +3")
        {
            bonus = true;
            CHECK(l.tone == world::TipTone::Bonus); // M18: bonus lines have their own colour
        }
    CHECK(bonus);

    // A made-up item with requirements: met ones normal, unmet ones Bad.
    vg::ItemTemplate t = *shield;
    t.requirements = {20, 0, 11, 0, 0};
    lines = world::itemTipLines(t, warrior, 1); // warrior base: Güç 12, Dayanıklılık 11
    const world::TipLine* str = nullptr;
    const world::TipLine* vit = nullptr;
    for (const auto& l : lines)
    {
        if (l.text == "Gerekli Güç: 20")
            str = &l;
        if (l.text == "Gerekli Dayanıklılık: 11")
            vit = &l;
    }
    REQUIRE(str != nullptr);
    REQUIRE(vit != nullptr);
    CHECK(str->tone == world::TipTone::Bad);
    CHECK(vit->tone == world::TipTone::Normal);
    // Spending Güç makes it met.
    lines = world::itemTipLines(t, {kWarrior, 10, vg::baseAttributes(kWarrior, {8, 0, 0, 0, 0})}, 1);
    for (const auto& l : lines)
        if (l.text == "Gerekli Güç: 20")
            CHECK(l.tone == world::TipTone::Normal);
}

TEST_CASE("Stats view: Esc closes the loot window, then the Kunye, then the inventory", "[client][stats]")
{
    using A = world::EscapeAction;
    CHECK(world::escapeActionWithKunye(true, true, true, true, false) == A::CloseLoot);
    CHECK(world::escapeActionWithKunye(false, true, true, true, false) == A::CloseKunye);
    CHECK(world::escapeActionWithKunye(false, false, true, true, false) == A::CloseInventory);
    CHECK(world::escapeActionWithKunye(false, false, false, true, false) == A::ClearTarget);
    CHECK(world::escapeActionWithKunye(false, false, false, false, false) == A::OpenMenu);
    CHECK(world::escapeActionWithKunye(true, true, true, true, true) == A::None);
}

TEST_CASE("Stats view: the automation parser", "[client][stats]")
{
    std::string error;
    auto c = world::parseStatsCommand("kunye open", error);
    REQUIRE(c);
    CHECK(c->verb == world::StatsVerb::Kunye);
    CHECK(c->mode == "open");
    c = world::parseStatsCommand("statadd guc 3", error);
    REQUIRE(c);
    CHECK(c->verb == world::StatsVerb::Add);
    CHECK(c->attribute == Attribute::Strength);
    CHECK(c->a == 3);
    c = world::parseStatsCommand("statremove vitality", error);
    REQUIRE(c);
    CHECK(c->verb == world::StatsVerb::Remove);
    CHECK(c->attribute == Attribute::Vitality);
    CHECK(c->a == 1);
    c = world::parseStatsCommand("expectattr dayaniklilik 14 3", error);
    REQUIRE(c);
    CHECK(c->verb == world::StatsVerb::ExpectAttr);
    CHECK(c->a == 14);
    CHECK(c->b == 3);
    c = world::parseStatsCommand("expectattr zeka 3", error);
    REQUIRE(c);
    CHECK(c->attribute == Attribute::Intelligence);
    CHECK(c->b == -1);
    c = world::parseStatsCommand("expectunspent 27", error);
    REQUIRE(c);
    CHECK(c->a == 27);
    c = world::parseStatsCommand("expectpending irade 2", error);
    REQUIRE(c);
    CHECK(c->attribute == Attribute::Spirit);
    c = world::parseStatsCommand("expectderived crit 5.5", error);
    REQUIRE(c);
    CHECK(c->verb == world::StatsVerb::ExpectDerived);
    CHECK(c->field == world::DerivedField::Crit);
    CHECK(c->value == 5.5);
    c = world::parseStatsCommand("expectpreview maxhp 120", error);
    REQUIRE(c);
    CHECK(c->verb == world::StatsVerb::ExpectPreview);
    c = world::parseStatsCommand("expectstatmessage Yeterli puanın yok", error);
    REQUIRE(c);
    CHECK(c->text == "Yeterli puanın yok");
    c = world::parseStatsCommand("statlog", error);
    REQUIRE(c);
    CHECK(c->verb == world::StatsVerb::Log);

    // Not a stats verb: nullopt, no error (the scene tries its other commands).
    CHECK_FALSE(world::parseStatsCommand("takeall", error));
    CHECK(error.empty());
    // Malformed: an error naming the usage.
    const char* bad[] = {"kunye", "kunye toggle", "statadd", "statadd luck", "statadd guc 0", "expectattr guc",
                         "expectunspent x", "expectpending guc", "expectderived luck 1", "expectpreview maxhp",
                         "expectstatmessage", "statlog now"};
    for (const char* line : bad)
    {
        CHECK_FALSE(world::parseStatsCommand(line, error));
        CHECK(error.starts_with("usage: "));
    }
}
