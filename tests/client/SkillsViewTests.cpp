// M12 "Yetenek Yolları" client logic (client/world/SkillsView.h, the multi-skill SkillTimers and the
// periodic / absorbed numbers of CombatView.h): the skill book (SKILL_STATE, learn gating against the
// shared rules, the one learn request in flight, the bar and its queued wish), bar edits, the effect
// book (list / apply / refresh / remove / clear / expiry), the cooldown countdown, skill use and slot
// tints, the Turkish texts and number format of PROTOCOL_SKILLS.md §7, the effect art plan, the Esc
// order and the automation parser.
#include "client/world/CombatView.h"
#include "client/world/SkillsView.h"
#include "shared/game/Combat.h"
#include "shared/game/Skills.h"
#include "shared/net/Messages.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <random>
#include <set>
#include <string>

namespace world = vo::client::world;
namespace vg = vo::game;
namespace vn = vo::net;
using Catch::Approx;

namespace {

constexpr std::uint8_t kWarrior = 1;
constexpr std::uint8_t kRogue = 2;
constexpr std::uint8_t kMage = 3;
constexpr std::uint8_t kPriest = 4;

const vg::SkillDef& skill(std::string_view key)
{
    const vg::SkillDef* s = world::parseSkillName(key);
    REQUIRE(s != nullptr);
    return *s;
}

// SKILL_STATE as the server builds it (ranks >= 1, ascending), passed through the wire format.
vn::SkillState stateOf(std::uint8_t charClass, std::uint16_t level, const vg::SkillRanks& ranks)
{
    const vg::SkillCatalog& c = vg::skills();
    vn::SkillState m;
    m.points = static_cast<std::uint16_t>(vg::skillPointsForLevel(c, level));
    m.unspent = vg::unspentSkillPoints(c, charClass, level, ranks);
    for (std::uint16_t id = 1; id <= vg::kMaxSkillId; ++id)
        if (ranks[id] > 0)
            m.skills.push_back({id, ranks[id]});
    const auto bytes = vn::encodeMessage(m);
    REQUIRE(bytes);
    vn::SkillState back;
    REQUIRE(vn::decodeMessage(*bytes, back));
    return back;
}

world::SkillBook bookAt(std::uint8_t charClass, std::uint16_t level, const vg::SkillRanks& ranks)
{
    world::SkillBook b;
    b.reset(charClass, level);
    b.applyState(stateOf(charClass, level, ranks));
    return b;
}

vn::EffectEntry effect(std::uint16_t skillId, vg::EffectKind kind, std::uint32_t durationMs, std::uint32_t remainingMs,
                       std::uint32_t amount, std::uint32_t source = 7)
{
    vn::EffectEntry e;
    e.skillId = skillId;
    e.kind = static_cast<std::uint8_t>(kind);
    e.sourceId = source;
    e.durationMs = durationMs;
    e.remainingMs = remainingMs;
    e.amount = amount;
    return e;
}

std::string joined(const std::vector<world::TipLine>& lines)
{
    std::string s;
    for (const world::TipLine& l : lines)
        s += l.text + "\n";
    return s;
}

} // namespace

TEST_CASE("Skills view: the skill book follows SKILL_STATE and the default bar", "[client][skills]")
{
    world::SkillBook b;
    b.reset(kWarrior, 10);
    CHECK_FALSE(b.known());
    CHECK(b.rank(vg::kSkillHeavyBlow) == 1); // the free skill before SKILL_STATE
    CHECK(b.slot(0) == vg::kSkillHeavyBlow);
    for (std::size_t i = 1; i < 8; ++i)
        CHECK(b.slot(i) == 0);
    CHECK(b.unspent() == 0);
    CHECK_FALSE(b.canRaise(vg::kSkillHeavyBlow)); // nothing known yet

    b.applyState(stateOf(kWarrior, 10, vg::defaultSkillRanks(vg::skills(), kWarrior)));
    CHECK(b.known());
    CHECK(b.points() == 9);
    CHECK(b.unspent() == 9);
    CHECK(b.rank(vg::kSkillHeavyBlow) == 1);
    CHECK(b.rank(skill("yarma").id) == 0);
    CHECK(b.branchSpent(0) == 0);
    CHECK(b.rank(200) == 0);
    // Branch skills by tier: Hücum = Ağır Darbe, Yarma, Kan Öfkesi + Sarsıntı.
    const auto hucum = b.branchSkills(0);
    REQUIRE(hucum.size() == 4);
    CHECK(hucum[0]->key == "agir_darbe");
    CHECK(hucum[1]->key == "yarma");
    CHECK(hucum[2]->tier == 3);
    CHECK(hucum[3]->tier == 3);
    CHECK(b.branchSkills(1).size() == 3);
    // Another class's skill in SKILL_STATE is dropped (normalized).
    vn::SkillState foreign = stateOf(kWarrior, 10, vg::defaultSkillRanks(vg::skills(), kWarrior));
    foreign.skills.push_back({skill("kiragi_oku").id, 2});
    b.applyState(foreign);
    CHECK(b.rank(skill("kiragi_oku").id) == 0);
}

TEST_CASE("Skills view: learn gating mirrors the shared rules for every class", "[client][skills]")
{
    const vg::SkillCatalog& c = vg::skills();
    std::mt19937 rng(12345);
    for (std::uint8_t cls = kWarrior; cls <= kPriest; ++cls)
    {
        for (int round = 0; round < 60; ++round)
        {
            const std::uint16_t level = static_cast<std::uint16_t>(1 + rng() % 10);
            // Random ranks, then trimmed until they fit the level (as the server keeps them).
            vg::SkillRanks ranks = vg::defaultSkillRanks(c, cls);
            for (const vg::SkillDef& s : c.skills())
                if (s.charClass == cls)
                    ranks[s.id] = static_cast<std::uint8_t>(std::max<int>(s.free ? 1 : 0, static_cast<int>(rng() % 4)));
            while (vg::skillPointsSpent(c, cls, ranks) > vg::skillPointsForLevel(c, level))
                for (const vg::SkillDef& s : c.skills())
                    if (s.charClass == cls && ranks[s.id] > (s.free ? 1 : 0) &&
                        vg::skillPointsSpent(c, cls, ranks) > vg::skillPointsForLevel(c, level))
                        --ranks[s.id];
            const world::SkillBook b = bookAt(cls, level, ranks);
            for (const vg::SkillDef& s : c.skills())
            {
                const vn::SkillLearnResult shared = vg::checkSkillLearn(c, cls, level, ranks, s.id);
                CAPTURE(cls, level, s.id);
                CHECK(b.check(s.id) == shared);
                CHECK(b.canRaise(s.id) == (shared == vn::SkillLearnResult::Ok));
                if (s.charClass == cls)
                    CHECK(b.gateMet(s) == (vg::branchPointsSpent(c, cls, s.branch, ranks) >= vg::tierGatePoints(c, s)));
            }
        }
    }
}

TEST_CASE("Skills view: one learn request in flight, the answer is the truth", "[client][skills]")
{
    const vg::SkillCatalog& c = vg::skills();
    world::SkillBook b = bookAt(kWarrior, 10, vg::defaultSkillRanks(c, kWarrior));
    const vg::SkillDef& yarma = skill("yarma");
    // Tier 2 needs 2 points in Hücum.
    CHECK(b.check(yarma.id) == vn::SkillLearnResult::TierLocked);
    CHECK_FALSE(b.gateMet(yarma));
    CHECK_FALSE(b.beginLearn(yarma.id).has_value());

    const auto req = b.beginLearn(vg::kSkillHeavyBlow);
    REQUIRE(req);
    CHECK(req->skillId == vg::kSkillHeavyBlow);
    CHECK(req->seq != 0);
    CHECK(b.learnBusy());
    CHECK_FALSE(b.canRaise(vg::kSkillHeavyBlow)); // buttons disabled while in flight
    CHECK_FALSE(b.beginLearn(vg::kSkillHeavyBlow).has_value());
    // An answer to another seq is applied but does not end the wait.
    const auto stray = b.finishLearn({req->seq + 99, vg::kSkillHeavyBlow, vn::SkillLearnResult::Ok, 2, 8});
    CHECK_FALSE(stray.matched);
    CHECK(b.learnBusy());
    const auto a = b.finishLearn({req->seq, vg::kSkillHeavyBlow, vn::SkillLearnResult::Ok, 2, 8});
    CHECK(a.matched);
    CHECK_FALSE(b.learnBusy());
    CHECK(b.rank(vg::kSkillHeavyBlow) == 2);
    CHECK(b.unspent() == 8);
    CHECK(b.branchSpent(0) == 1);
    CHECK(b.check(yarma.id) == vn::SkillLearnResult::TierLocked);
    // Rank 3: the gate opens.
    const auto r2 = b.beginLearn(vg::kSkillHeavyBlow);
    REQUIRE(r2);
    (void)b.finishLearn({r2->seq, vg::kSkillHeavyBlow, vn::SkillLearnResult::Ok, 3, 7});
    CHECK(b.gateMet(yarma));
    CHECK(b.canRaise(yarma.id));
    CHECK(b.check(vg::kSkillHeavyBlow) == vn::SkillLearnResult::MaxRank);
    // A refusal keeps the rank; its unspent is taken over.
    const auto r3 = b.beginLearn(yarma.id);
    REQUIRE(r3);
    const auto refused = b.finishLearn({r3->seq, yarma.id, vn::SkillLearnResult::ServerError, 0, 7});
    CHECK(refused.matched);
    CHECK(refused.result == vn::SkillLearnResult::ServerError);
    CHECK(b.rank(yarma.id) == 0);
    CHECK(b.canRaise(yarma.id));
    // The server's unspent decides even when the local level allows more (a level-up's SKILL_STATE
    // has not come yet).
    vn::SkillState stale = stateOf(kWarrior, 10, b.ranks());
    stale.unspent = 0;
    b.applyState(stale);
    CHECK(b.check(yarma.id) == vn::SkillLearnResult::NotEnoughPoints);
    // Not sent: nothing in flight.
    world::SkillBook d = bookAt(kWarrior, 2, vg::defaultSkillRanks(c, kWarrior));
    REQUIRE(d.beginLearn(vg::kSkillHeavyBlow));
    d.abandonLearn();
    CHECK_FALSE(d.learnBusy());
    // Unknown and other classes' skills.
    CHECK(d.check(99) == vn::SkillLearnResult::UnknownSkill);
    CHECK(d.check(skill("koz_oku").id) == vn::SkillLearnResult::UnknownSkill);
}

TEST_CASE("Skills view: bar edits send one request each and queue the last wish", "[client][skills]")
{
    const vg::SkillCatalog& c = vg::skills();
    world::SkillBook b = bookAt(kWarrior, 10, vg::defaultSkillRanks(c, kWarrior));
    const vg::SkillBar start = b.bar();
    // Pure edits.
    const vg::SkillBar placed = world::barWithPlaced(start, 3, 5);
    CHECK(placed[3] == 5);
    CHECK(placed[0] == vg::kSkillHeavyBlow);
    const vg::SkillBar swapped = world::barWithSwapped(placed, 0, 3);
    CHECK(swapped[0] == 5);
    CHECK(swapped[3] == vg::kSkillHeavyBlow);
    CHECK(world::barWithCleared(swapped, 3)[3] == 0);
    CHECK(world::barWithPlaced(start, 8, 5) == start); // out of range: unchanged
    CHECK(world::barWithSwapped(start, 0, 9) == start);

    // No change: nothing to send.
    CHECK_FALSE(b.requestBar(start).has_value());
    const auto req = b.requestBar(world::barWithSwapped(start, 0, 1));
    REQUIRE(req);
    CHECK(req->slots[1] == vg::kSkillHeavyBlow);
    CHECK(req->slots[0] == 0);
    CHECK(b.barBusy());
    CHECK(b.bar() == start); // the bar shows what the server stored, not the wish
    // While in flight: the wishes wait, only the last one is sent.
    CHECK_FALSE(b.requestBar(world::barWithPlaced(start, 2, vg::kSkillHeavyBlow)).has_value());
    CHECK_FALSE(b.requestBar(world::barWithPlaced(start, 4, vg::kSkillHeavyBlow)).has_value());
    CHECK(b.barQueued());
    vn::SkillBarSetRes res;
    res.seq = req->seq;
    res.result = vn::SkillBarResult::Ok;
    res.slots = req->slots;
    const auto a = b.finishBar(res);
    CHECK(a.matched);
    CHECK(b.bar()[1] == vg::kSkillHeavyBlow);
    REQUIRE(a.next);
    CHECK(a.next->slots[4] == vg::kSkillHeavyBlow);
    CHECK(a.next->slots[0] == vg::kSkillHeavyBlow);
    CHECK(b.barBusy());
    CHECK_FALSE(b.barQueued());
    // A refusal: the stored bar of the answer is shown.
    vn::SkillBarSetRes no;
    no.seq = a.next->seq;
    no.result = vn::SkillBarResult::NotLearned;
    no.slots = res.slots;
    const auto r = b.finishBar(no);
    CHECK(r.matched);
    CHECK(r.result == vn::SkillBarResult::NotLearned);
    CHECK_FALSE(r.next);
    CHECK_FALSE(b.barBusy());
    CHECK(b.bar()[4] == 0);
    CHECK(b.bar()[1] == vg::kSkillHeavyBlow);
    // SKILL_BAR (world entry) replaces the bar.
    vn::SkillBar sb;
    sb.slots = {0, 0, 0, 0, 0, 0, 0, vg::kSkillHeavyBlow};
    b.applyBar(sb);
    CHECK(b.slot(7) == vg::kSkillHeavyBlow);
    CHECK(b.slot(0) == 0);
    CHECK(b.slot(8) == 0);
}

TEST_CASE("Skills view: cooldowns count down per skill, the global one sweeps every slot", "[client][skills]")
{
    world::SkillTimers t;
    const vg::SkillDef& yarma = skill("yarma");       // 8 s
    const vg::SkillDef& kan = skill("kan_ofkesi");    // 30 s
    // SKILL_COOLDOWNS on world entry: 12 of 30 s left (relog memory).
    t.setCooldown(kan.id, 12000, 30000, 100.0);
    CHECK(t.remaining(kan.id, 100.0) == Approx(12.0).margin(1e-6));
    CHECK(t.sweep(kan.id, 100.0) == Approx(0.4f).margin(1e-4f));
    CHECK(t.ownRemaining(kan.id, 106.0) == Approx(6.0).margin(1e-6));
    CHECK(t.ready(kan.id, 112.0));
    CHECK(t.ready(yarma.id, 100.0));
    // Using Yarma: its own 8 s and the global 0.5 s on everything else.
    t.onSkillUsed(yarma.id, 200.0);
    CHECK(t.remaining(yarma.id, 200.0) == Approx(8.0).margin(1e-6));
    CHECK(t.globalActive(200.2));
    CHECK_FALSE(t.ready(vg::kSkillHeavyBlow, 200.2)); // the global cooldown
    CHECK(t.sweep(vg::kSkillHeavyBlow, 200.25) == Approx(0.5f).margin(1e-3f));
    CHECK(t.ready(vg::kSkillHeavyBlow, 200.5));
    // Further COMBAT_EVENTs of the same use (an area hits five) do not restart it; the exact
    // SKILL_COOLDOWNS does replace it.
    t.onSkillUsed(yarma.id, 200.1);
    CHECK(t.cooldownEnd(yarma.id) == Approx(208.0).margin(1e-6));
    t.setCooldown(yarma.id, 8000, 8000, 200.05);
    CHECK(t.cooldownEnd(yarma.id) == Approx(208.05).margin(1e-6));
    t.onSkillUsed(yarma.id, 200.2); // the COMBAT_EVENT after it: unchanged
    CHECK(t.cooldownEnd(yarma.id) == Approx(208.05).margin(1e-6));
    // Two skills at once.
    t.onSkillUsed(kan.id, 201.0, 30000);
    CHECK(t.remaining(kan.id, 201.0) == Approx(30.0).margin(1e-6));
    CHECK(t.remaining(yarma.id, 201.0) == Approx(7.05).margin(1e-6));
    // Countdown text of the slot: whole seconds up.
    CHECK(world::effectSecondsText(7.05) == "8");
    CHECK(world::effectSecondsText(0.2) == "1");
    CHECK(world::effectSecondsText(0.0).empty());
    // Ids beyond the table change nothing.
    t.setCooldown(70, 1000, 1000, 1.0);
    CHECK(t.ownRemaining(70, 1.0) == 0.0);
}

TEST_CASE("Skills view: effect book bookkeeping", "[client][skills]")
{
    world::EffectBook e;
    const std::uint32_t me = 5;
    const std::uint32_t monster = 77;
    // EFFECT_LIST after a spawn.
    const vn::EffectEntry list[] = {effect(17, vg::EffectKind::Dot, 8000, 5000, 9),
                                    effect(20, vg::EffectKind::Slow, 5000, 1000, 30)};
    e.list(monster, list, 10.0);
    REQUIRE(e.of(monster).size() == 2);
    CHECK(e.has(monster, 6));
    CHECK(e.has(monster, 8, 20));
    CHECK_FALSE(e.has(monster, 8, 17));
    CHECK(world::effectRemaining(e.of(monster)[0], 12.0) == Approx(3.0));
    // APPLY: new, then a refresh of the same (skill, kind) replaces it.
    e.apply(me, effect(6, vg::EffectKind::AttackUp, 12000, 12000, 15), 20.0);
    e.apply(me, effect(8, vg::EffectKind::DefenseUp, 12000, 12000, 40), 20.0);
    e.apply(me, effect(6, vg::EffectKind::AttackUp, 12000, 12000, 20), 25.0);
    REQUIRE(e.of(me).size() == 2);
    CHECK(e.find(me, 1)->amount == 20);
    CHECK(e.find(me, 1)->end == Approx(37.0));
    // Same kind from another skill: both stay.
    e.apply(me, effect(28, vg::EffectKind::AttackUp, 20000, 20000, 15), 25.0);
    CHECK(e.of(me).size() == 3);
    CHECK(e.find(me, 1, 28)->amount == 15);
    // REMOVE.
    CHECK(e.remove(me, 8, 2));
    CHECK_FALSE(e.remove(me, 8, 2));
    CHECK(e.of(me).size() == 2);
    CHECK_FALSE(e.remove(999, 1, 1));
    // Death / despawn: the whole list goes.
    e.clear(monster);
    CHECK(e.of(monster).empty());
    CHECK(e.entityCount() == 1);
    // An empty EFFECT_LIST clears too.
    e.list(me, {}, 30.0);
    CHECK(e.entityCount() == 0);
    // At most 16.
    for (std::uint16_t i = 1; i <= 20; ++i)
        e.apply(monster, effect(i, vg::EffectKind::Dot, 8000, 8000, 1), 40.0);
    CHECK(e.of(monster).size() == vg::kMaxEffectsPerEntity);
    // The safety net drops what is long past its end.
    e.apply(me, effect(13, vg::EffectKind::Haste, 10000, 1000, 15), 50.0);
    e.expire(51.5);
    CHECK(e.has(me, 3));
    e.expire(53.5);
    CHECK_FALSE(e.has(me, 3));
    CHECK(e.of(monster).empty());
}

TEST_CASE("Skills view: effect icons and their tooltips", "[client][skills]")
{
    CHECK(world::effectHelpful(1));
    CHECK(world::effectHelpful(7));
    CHECK(world::effectHelpful(11));
    CHECK_FALSE(world::effectHelpful(6));
    CHECK_FALSE(world::effectHelpful(13));
    CHECK_FALSE(world::effectHelpful(0));
    CHECK(world::effectStatusIcon(3, 13) == "ui/status/attack_speed_up.png");
    CHECK(world::effectStatusIcon(6, skill("yakici_iz").id) == "ui/status/burn.png");
    CHECK(world::effectStatusIcon(6, skill("zehirli_kesik").id) == "ui/status/poison.png");
    CHECK(world::effectStatusIcon(12, 27) == "ui/status/weakened.png");
    CHECK(world::effectStatusIcon(13, 9) == "ui/status/taunted.png");
    CHECK(world::effectStatusIcon(99, 9).empty());
    CHECK(world::effectKindLine(1, 15, 6) == "Saldırı +%15");
    CHECK(world::effectKindLine(2, 40, 8) == "Savunma +%40");
    CHECK(world::effectKindLine(3, 20, 13) == "Saldırı hızı +%20");
    CHECK(world::effectKindLine(4, 10, 16) == "Kaçınma +%10");
    CHECK(world::effectKindLine(5, 10, 24) == "En yüksek can +%10");
    CHECK(world::effectKindLine(6, 9, skill("zehirli_kesik").id) == "Zehir: saniyede 9");
    CHECK(world::effectKindLine(6, 12, skill("yakici_iz").id) == "Yanma: saniyede 12");
    CHECK(world::effectKindLine(7, 5, 23) == "Yenilenme: saniyede 5");
    CHECK(world::effectKindLine(8, 30, 20) == "Yavaşlatıldı %30");
    CHECK(world::effectKindLine(9, 0, 7) == "Sersemledi");
    CHECK(world::effectKindLine(10, 0, 7) == "Kök saldı");
    CHECK(world::effectKindLine(11, 140, 21) == "Kalkan: 140");
    CHECK(world::effectKindLine(12, 20, 27) == "Zayıfladı %20");
    CHECK(world::effectKindLine(13, 0, 9) == "Kışkırtıldı");
    world::ShownEffect shield;
    shield.skillId = skill("buz_kabugu").id;
    shield.kind = 11;
    shield.amount = 96;
    CHECK(world::effectTipText(shield) == "Buz Kabuğu\nKalkan: 96");
}

TEST_CASE("Skills view: skill use targets and slot tints follow §7.1", "[client][skills]")
{
    const vg::SkillDef& heavy = *vg::findSkill(vg::kSkillHeavyBlow); // enemy, 2.5 m
    const vg::SkillDef& sun = *vg::findSkill(vg::kSkillPaleSun);     // friendly, 14 m
    const vg::SkillDef& kan = skill("kan_ofkesi");                     // self
    const vg::SkillDef& leap = skill("golge_adimi");                   // enemy, 10 / 12 / 14 m
    world::SkillSelection none;
    world::SkillSelection monster{77, false, true, false, false, 2.0f};
    world::SkillSelection farMonster{77, false, true, false, false, 9.0f};
    world::SkillSelection deadMonster{77, false, true, false, true, 2.0f};
    world::SkillSelection friendPlayer{40, false, false, true, false, 10.0f};
    world::SkillSelection farPlayer{40, false, false, true, false, 30.0f};
    world::SkillSelection self{5, true, false, false, false, 0.0f};

    CHECK(world::decideSkillUse(heavy, 1, none).verdict == world::SkillUseVerdict::NoTarget);
    CHECK(world::decideSkillUse(heavy, 1, self).verdict == world::SkillUseVerdict::NoTarget);
    CHECK(world::decideSkillUse(heavy, 1, friendPlayer).verdict == world::SkillUseVerdict::WrongTarget);
    CHECK(world::decideSkillUse(heavy, 1, deadMonster).verdict == world::SkillUseVerdict::TargetDead);
    const auto hit = world::decideSkillUse(heavy, 2, monster);
    CHECK(hit.verdict == world::SkillUseVerdict::Send);
    CHECK(hit.targetId == 77);
    const auto walk = world::decideSkillUse(heavy, 1, farMonster);
    CHECK(walk.verdict == world::SkillUseVerdict::WalkFirst);
    CHECK(walk.range == Approx(2.5f));
    CHECK(world::decideSkillUse(heavy, 0, monster).verdict == world::SkillUseVerdict::NotLearned);
    // The leap reaches 10 m at rank 1: a monster 9 m away is in range (no walking first).
    CHECK(world::decideSkillUse(leap, 1, farMonster).verdict == world::SkillUseVerdict::Send);
    // Friendly: the selected living player, else ourselves (0).
    const auto heal = world::decideSkillUse(sun, 1, friendPlayer);
    CHECK(heal.verdict == world::SkillUseVerdict::Send);
    CHECK(heal.targetId == 40);
    CHECK(world::decideSkillUse(sun, 1, farPlayer).verdict == world::SkillUseVerdict::WalkFirst);
    CHECK(world::decideSkillUse(sun, 1, monster).targetId == 0);
    CHECK(world::decideSkillUse(sun, 1, self).targetId == 0);
    CHECK(world::decideSkillUse(sun, 1, none).targetId == 0);
    // Self: always 0.
    CHECK(world::decideSkillUse(kan, 1, monster).targetId == 0);
    CHECK(world::decideSkillUse(kan, 1, friendPlayer).verdict == world::SkillUseVerdict::Send);

    // Tints.
    CHECK(world::slotLook(nullptr, 0, 100, false, monster) == world::SlotLook::Empty);
    CHECK(world::slotLook(&heavy, 1, 100, true, monster) == world::SlotLook::Dead);
    CHECK(world::slotLook(&heavy, 1, 11, false, monster) == world::SlotLook::NoMana); // 12 mp
    CHECK(world::slotLook(&heavy, 1, 12, false, monster) == world::SlotLook::Ready);
    CHECK(world::slotLook(&heavy, 1, 100, false, none) == world::SlotLook::NoTarget);
    CHECK(world::slotLook(&heavy, 1, 100, false, farMonster) == world::SlotLook::NoTarget);
    CHECK(world::slotLook(&heavy, 1, 100, false, friendPlayer) == world::SlotLook::NoTarget);
    CHECK(world::slotLook(&kan, 1, 100, false, none) == world::SlotLook::Ready);
    CHECK(world::slotLook(&kan, 1, 5, false, none) == world::SlotLook::NoMana);
    CHECK(world::slotLook(&sun, 1, 100, false, none) == world::SlotLook::Ready);
    CHECK(world::slotLookName(world::SlotLook::NoMana) == "nomana");
}

TEST_CASE("Skills view: tooltip lines and the Turkish number format of §7.3", "[client][skills]")
{
    const vg::SkillDef& heavy = *vg::findSkill(vg::kSkillHeavyBlow);
    const auto t1 = world::skillTipLines(heavy, 1);
    REQUIRE(t1.size() == 7);
    CHECK(t1[0].text == "Ağır Darbe");
    CHECK(t1[0].tone == world::TipTone::Title);
    CHECK(t1[1].text == "Hücum · Kademe 1/3");
    CHECK(t1[2].text == "Mana: 12   Bekleme: 6 sn");
    CHECK(t1[3].text == "Menzil: 2,5 m");
    CHECK(t1[4].text == "Anında");
    CHECK(t1[5].text == "Silahını tüm gücüyle indirir: saldırı gücünün %220 oranında hasar.");
    CHECK(t1[6].text == "Sonraki kademe: Silahını tüm gücüyle indirir: saldırı gücünün %250 oranında hasar.");
    CHECK(t1[6].tone == world::TipTone::Dim);
    // Rank 3: no next rank line.
    CHECK(world::skillTipLines(heavy, 3).size() == 6);
    // Not learned: rank 1's description, "Öğrenilmedi", no next rank.
    const vg::SkillDef& yarma = skill("yarma");
    const auto t0 = world::skillTipLines(yarma, 0);
    REQUIRE(t0.size() == 6);
    CHECK(t0[1].text == "Hücum · Öğrenilmedi");
    CHECK(t0[3].text == "Çevrene");
    CHECK(t0[5].text.find("3 m içindeki en çok 5 düşmana") != std::string::npos);
    CHECK(t0[5].text.find("%140") != std::string::npos);
    // Self, a cast with decimals, a decimal cooldown.
    CHECK(joined(world::skillTipLines(skill("kan_ofkesi"), 1)).find("\nKendine\n") != std::string::npos);
    const std::string bolt = joined(world::skillTipLines(*vg::findSkill(vg::kSkillEmberBolt), 2));
    CHECK(bolt.find("Hazırlık: 0,6 sn") != std::string::npos);
    CHECK(bolt.find("Menzil: 16 m") != std::string::npos);
    CHECK(bolt.find("Bekleme: 3 sn") != std::string::npos);
    const std::string strike = joined(world::skillTipLines(skill("isik_carpmasi"), 1));
    CHECK(strike.find("Bekleme: 3,5 sn") != std::string::npos);
    CHECK(strike.find("Hazırlık: 0,7 sn") != std::string::npos);
    const std::string stun = joined(world::skillTipLines(skill("sarsinti"), 1));
    CHECK(stun.find("1,5 sn sersemletir") != std::string::npos);
    CHECK(stun.find("Sonraki kademe:") != std::string::npos);
    CHECK(stun.find("2 sn sersemletir") != std::string::npos);
    // Every skill has a complete tooltip at every rank (no unfilled placeholder).
    for (const vg::SkillDef& s : vg::skills().skills())
        for (std::uint8_t r = 0; r <= 3; ++r)
            CHECK(joined(world::skillTipLines(s, r)).find('{') == std::string::npos);
}

TEST_CASE("Skills view: result texts of §7.4", "[client][skills]")
{
    using R = vn::SkillLearnResult;
    const vg::SkillDef& yarma = skill("yarma");
    const vg::SkillDef& kan = skill("kan_ofkesi");
    CHECK(world::learnResultText(R::Ok, &yarma, 2) == "Yarma kademe 2 oldu.");
    CHECK(world::learnResultText(R::UnknownSkill, &yarma, 0) == "Bu yeteneği öğrenemezsin.");
    CHECK(world::learnResultText(R::MaxRank, &yarma, 3) == "Bu yetenek en yüksek kademede.");
    CHECK(world::learnResultText(R::NotEnoughPoints, &yarma, 0) == "Yeterli yetenek puanın yok.");
    CHECK(world::learnResultText(R::TierLocked, &yarma, 0) == "Bu yetenek için bu yolda önce 2 puan harcamalısın.");
    CHECK(world::learnResultText(R::TierLocked, &kan, 0) == "Bu yetenek için bu yolda önce 4 puan harcamalısın.");
    CHECK(world::learnResultText(R::ServerError, &yarma, 0) == "Yetenek kaydedilemedi, tekrar dene.");
    CHECK(world::barResultText(vn::SkillBarResult::Ok).empty());
    CHECK(world::barResultText(vn::SkillBarResult::NotLearned) == "Bu yeteneği henüz öğrenmedin.");
    CHECK(world::barResultText(vn::SkillBarResult::ServerError) == "Yetenek çubuğu kaydedilemedi, tekrar dene.");
    CHECK(world::skillPointsText(3) == "Yetenek puanı: 3");
    CHECK(world::branchSpentText(4) == "Bu yolda harcanan: 4");
    CHECK(world::tierGateText(2) == "Bu yolda 2 puan gerekli");
    CHECK(world::rankText(0) == "Öğrenilmedi");
    CHECK(world::rankText(2) == "2/3");
    CHECK(world::levelUpSkillPointsText(1) == "+1 yetenek puanı  ·  Yetenekler: K");
    CHECK(world::kNoTargetText == "Önce bir hedef seç.");
    CHECK(world::actionFailText(vn::CombatAction::Skill, vn::ActionFailReason::NotLearned) == "Bu yeteneği henüz öğrenmedin.");
    CHECK(world::actionFailText(vn::CombatAction::Skill, vn::ActionFailReason::NoPath) == "Hedefe atlanacak yol yok.");
    // Branch plaques and icons exist for every class.
    for (std::uint8_t cls = kWarrior; cls <= kPriest; ++cls)
        for (std::uint8_t br = 0; br < 2; ++br)
            CHECK_FALSE(world::branchArtKey(cls, br).empty());
    CHECK(world::branchArtKey(kMage, 1) == "kiragi");
    CHECK(world::branchArtKey(5, 0).empty());
    CHECK(world::skillIconPath(yarma) == "ui/skills/yarma.png");
}

TEST_CASE("Skills view: periodic and absorbed numbers", "[client][skills]")
{
    using world::FloatKind;
    CHECK(world::classifyCombatNumber(vn::kCombatKindDamage, vn::kCombatFlagPeriodic, false, 9) == FloatKind::PeriodicDamage);
    CHECK(world::classifyCombatNumber(vn::kCombatKindHeal, vn::kCombatFlagPeriodic, true, 9) == FloatKind::PeriodicHeal);
    CHECK(world::classifyCombatNumber(vn::kCombatKindDamage, vn::kCombatFlagAbsorbed, true, 0) == FloatKind::Absorbed);
    CHECK(world::classifyCombatNumber(vn::kCombatKindDamage, vn::kCombatFlagAbsorbed, true, 4) == FloatKind::TakenDamage);
    CHECK(world::floatText(FloatKind::Absorbed, 0) == "Emildi");
    CHECK(world::floatText(FloatKind::PeriodicHeal, 7) == "+7");
    CHECK(world::floatText(FloatKind::PeriodicDamage, 7) == "7");
    CHECK(world::floatStyle(FloatKind::PeriodicDamage).fontScale < world::floatStyle(FloatKind::Damage).fontScale);
    CHECK(world::floatStyle(FloatKind::PeriodicHeal).fontScale < world::floatStyle(FloatKind::Heal).fontScale);
}

TEST_CASE("Skills view: effect art plan uses the manifest's meshes", "[client][skills]")
{
    const std::set<std::string_view> meshes = {
        "ember_bolt",         "frost_bolt",          "ember_mote",          "light_strike",       "ground_ring_flame",
        "ground_ring_frost",  "ground_ring_heal",    "ground_ring_fog",     "ground_ring_neutral", "shield_bubble_gold",
        "shield_bubble_frost", "shield_bubble_neutral", "buff_aura_gold",   "buff_aura_red",      "buff_aura_steel",
        "buff_aura_green",    "buff_aura_neutral",   "tick_wisp_burn",      "tick_wisp_poison",   "stun_marker",
        "root_spikes",        "leap_trail",          "fog_cloud"};
    std::size_t withArt = 0;
    for (const vg::SkillDef& s : vg::skills().skills())
    {
        for (std::uint8_t r = 1; r <= 3; ++r)
            for (const world::ArtSpawn& a : world::skillUseArt(s, r))
            {
                CAPTURE(s.key, a.mesh);
                CHECK(meshes.count(a.mesh) == 1);
                CHECK(a.duration > 0.0f);
                CHECK(a.scale > 0.0f);
            }
        withArt += world::skillUseArt(s, 1).empty() ? 0 : 1;
        for (const vg::EffectDef& e : s.effects)
            if (const auto aura = world::effectAura(s.id, static_cast<std::uint8_t>(e.kind)))
                CHECK(meshes.count(aura->mesh) == 1);
    }
    CHECK(withArt >= 20);
    // Area skills scale their ring to the radius; the fog lasts as long as the slow of the rank.
    const auto ring = world::skillUseArt(skill("alev_halkasi"), 1);
    REQUIRE(ring.size() == 1);
    CHECK(ring[0].mesh == "ground_ring_flame");
    CHECK(ring[0].scale == Approx(4.0f));
    CHECK(ring[0].at == world::ArtAt::Caster);
    const auto fog = world::skillUseArt(skill("sis_bombasi"), 3);
    REQUIRE(fog.size() == 2);
    CHECK(fog[0].mesh == "fog_cloud");
    CHECK(fog[0].duration == Approx(6.0f));
    CHECK(fog[0].opacity < 1.0f); // translucent
    const auto rain = world::skillUseArt(skill("kor_yagmuru"), 1);
    REQUIRE_FALSE(rain.empty());
    CHECK(rain[0].motion == world::ArtMotion::Fall);
    CHECK(rain[0].count >= 8);
    CHECK(world::skillUseArt(*vg::findSkill(vg::kSkillHeavyBlow), 1).empty()); // melee: the swing only
    CHECK(world::skillUseArt(*vg::findSkill(vg::kSkillEmberBolt), 1)[0].motion == world::ArtMotion::Projectile);
    // Auras while an effect lasts.
    CHECK(world::effectAura(skill("sarsinti").id, 9)->mesh == "stun_marker");
    CHECK(world::effectAura(skill("sarsinti").id, 9)->at == world::AuraAt::Head);
    CHECK(world::effectAura(skill("buz_kabugu").id, 11)->mesh == "shield_bubble_frost");
    CHECK(world::effectAura(skill("kutlu_siper").id, 2)->mesh == "shield_bubble_gold");
    CHECK_FALSE(world::effectAura(skill("kutlu_siper").id, 5).has_value()); // one bubble, not two
    CHECK(world::effectAura(skill("kan_ofkesi").id, 1)->mesh == "buff_aura_red");
    CHECK_FALSE(world::effectAura(skill("sis_bombasi").id, 8).has_value());
    CHECK(world::tickArt(skill("yakici_iz").id) == "tick_wisp_burn");
    CHECK(world::tickArt(skill("zehirli_kesik").id) == "tick_wisp_poison");
    CHECK(world::tickArt(vg::kSkillHeavyBlow).empty());
}

TEST_CASE("Skills view: Esc order and name parsing", "[client][skills]")
{
    using A = world::EscapeAction;
    CHECK(world::escapeActionWithSkills(true, true, true, true, true, false) == A::CloseLoot);
    CHECK(world::escapeActionWithSkills(false, true, true, true, true, false) == A::CloseSkills);
    CHECK(world::escapeActionWithSkills(false, false, true, true, true, false) == A::CloseKunye);
    CHECK(world::escapeActionWithSkills(false, false, false, true, true, false) == A::CloseInventory);
    CHECK(world::escapeActionWithSkills(false, false, false, false, true, false) == A::ClearTarget);
    CHECK(world::escapeActionWithSkills(false, false, false, false, false, false) == A::OpenMenu);
    CHECK(world::escapeActionWithSkills(true, true, true, true, true, true) == A::None);
    CHECK(world::parseSkillName("5")->key == "yarma");
    CHECK(world::parseSkillName("donduran_nefes")->id == 22);
    CHECK(world::parseSkillName("99") == nullptr);
    CHECK(world::parseSkillName("bilinmez") == nullptr);
    CHECK(*world::parseEffectKind("attack_up") == 1);
    CHECK(*world::parseEffectKind("dot") == 6);
    CHECK(*world::parseEffectKind("13") == 13);
    CHECK_FALSE(world::parseEffectKind("14"));
    CHECK_FALSE(world::parseEffectKind("hiz"));
}

TEST_CASE("Skills view: automation parser", "[client][skills]")
{
    std::string err;
    auto c = world::parseSkillsCommand("skills open", err);
    REQUIRE(c);
    CHECK(c->verb == world::SkillsVerb::Window);
    CHECK(c->mode == "open");
    c = world::parseSkillsCommand("skilllearn yarma", err);
    REQUIRE(c);
    CHECK(c->skill->id == 5);
    c = world::parseSkillsCommand("barset 2 kan_ofkesi", err);
    REQUIRE(c);
    CHECK(c->verb == world::SkillsVerb::BarSet);
    CHECK(c->a == 2);
    CHECK(c->skill->key == "kan_ofkesi");
    c = world::parseSkillsCommand("barset 8 none", err);
    REQUIRE(c);
    CHECK(c->skill == nullptr);
    c = world::parseSkillsCommand("expectslotlook 1 nomana", err);
    REQUIRE(c);
    CHECK(c->mode == "nomana");
    c = world::parseSkillsCommand("expectcooldown agir_darbe 4.5 6", err);
    REQUIRE(c);
    CHECK(c->lo == Approx(4.5));
    CHECK(c->hi == Approx(6.0));
    c = world::parseSkillsCommand("expecteffect target dot zehirli_kesik", err);
    REQUIRE(c);
    CHECK(c->kind == 6);
    CHECK(c->mode == "target");
    CHECK(c->skill->key == "zehirli_kesik");
    c = world::parseSkillsCommand("expecteffecttip self attack_up Saldırı +%15", err);
    REQUIRE(c);
    CHECK(c->text == "Saldırı +%15");
    c = world::parseSkillsCommand("expectart fog_cloud 1", err);
    REQUIRE(c);
    CHECK(c->text == "fog_cloud");
    CHECK(c->a == 1);
    c = world::parseSkillsCommand("pullpack Kül Çömezi 3", err);
    REQUIRE(c);
    CHECK(c->text == "Kül Çömezi");
    CHECK(c->a == 3);
    c = world::parseSkillsCommand("expectskillmessage Yarma kademe 1 oldu.", err);
    REQUIRE(c);
    CHECK(c->text == "Yarma kademe 1 oldu.");
    // Not a skill verb.
    CHECK_FALSE(world::parseSkillsCommand("target nearest", err));
    CHECK(err.empty());
    // Malformed ones name their usage.
    for (const char* bad : {"skills", "skilllearn nothing", "barset 9 yarma", "barset 1", "useslot 0", "expectrank yarma 4",
                            "expectslot 2 nope", "expectslotlook 1 pink", "expecteffect me dot", "expecteffect self hiz",
                            "expectcooldown yarma x 1", "expectart fog", "pullpack Kül 0", "skilllog now"})
    {
        CAPTURE(bad);
        CHECK_FALSE(world::parseSkillsCommand(bad, err));
        CHECK(err.starts_with("usage: "));
    }
}
