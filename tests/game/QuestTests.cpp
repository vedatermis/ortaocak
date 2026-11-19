// Quests, shared code (M17, docs/PROTOCOL_QUESTS.md, v11): the strict loader of quests.toml (every
// refusal of the format), the shipped quest line (NPCs, monsters, items, chains, rewards that fit the
// classes, the XP / Akçe share of levels 1-10), the state machine (accept checks and their order, kill
// counting, readiness, collect counting, cooldowns), the hand-in plan (removal, rewards, BagFull,
// PurseFull, BadChoice), the markers and the v11 messages. VO_ECONOMY_TABLE=1 prints the totals.

#include "shared/game/Character.h"
#include "shared/game/Combat.h"
#include "shared/game/Items.h"
#include "shared/game/Monsters.h"
#include "shared/game/Npcs.h"
#include "shared/game/Quests.h"
#include "shared/net/Messages.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <format>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace vg = vo::game;
namespace vn = vo::net;
using Catch::Matchers::ContainsSubstring;
using QR = vn::QuestResult;

namespace {

std::string readFile(const std::filesystem::path& p)
{
    std::ifstream in(p, std::ios::binary);
    std::stringstream s;
    s << in.rdbuf();
    return s.str();
}

const vg::ItemCatalog& items()
{
    static const vg::ItemCatalog c = [] {
        vg::ItemCatalog out;
        std::string error;
        const auto f = vg::locateItemsFile();
        REQUIRE(f);
        REQUIRE(vg::loadItemCatalog(*f, out, error));
        return out;
    }();
    return c;
}

const vg::MonsterData& monsters()
{
    static const vg::MonsterData d = [] {
        vg::MonsterData out;
        std::string error;
        const auto f = vg::locateMonstersFile();
        REQUIRE(f);
        REQUIRE(vg::loadMonsterData(*f, out, error));
        return out;
    }();
    return d;
}

const vg::NpcCatalog& npcs()
{
    static const vg::NpcCatalog c = [] {
        vg::NpcCatalog out;
        std::string error;
        const auto f = vg::locateNpcsFile();
        REQUIRE(f);
        const bool ok = vg::loadNpcCatalog(*f, &items(), out, error);
        INFO(error);
        REQUIRE(ok);
        return out;
    }();
    return c;
}

const vg::QuestCatalog& shipped()
{
    static const vg::QuestCatalog c = [] {
        vg::QuestCatalog out;
        std::string error;
        const auto f = vg::locateQuestsFile();
        REQUIRE(f);
        const bool ok = vg::loadQuestCatalog(*f, &items(), &npcs(), &monsters(), out, error);
        INFO(error);
        REQUIRE(ok);
        return out;
    }();
    return c;
}

bool printTables()
{
    char* env = nullptr;
    std::size_t len = 0;
    const bool print = _dupenv_s(&env, &len, "VO_ECONOMY_TABLE") == 0 && env != nullptr;
    std::free(env);
    return print;
}

// A small catalog without the other data files (ids are not checked against them).
const std::string kSample = R"(
[rules]
max_active = 2

[[quest]]
id = 1
name = "Bir"
giver = 10
turn_in = 11
story = "s"
done_text = "d"
[[quest.objective]]
type = "talk"
npc = 11
[quest.reward]
xp = 10

[[quest]]
id = 2
name = "Iki"
giver = 11
level = 2
after = 1
story = "s"
done_text = "d"
[[quest.objective]]
type = "kill"
monsters = [1, 2]
count = 3
[[quest.objective]]
type = "collect"
item = 9
count = 2
[quest.reward]
xp = 50
coins = 20
items = [[16, 2]]
class_items = { warrior = 72, rogue = 77, mage = 82, priest = 72 }

[[quest]]
id = 3
name = "Uc"
giver = 11
race = "kargan"
repeatable = true
cooldown_s = 600
story = "s"
done_text = "d"
[[quest.objective]]
type = "boss"
monsters = [5]
count = 1
[quest.reward]
coins = 5
choice = [71, 76]
)";

std::string sampleError(const std::string& text, bool withCatalogs = false)
{
    vg::QuestCatalog c;
    std::string error;
    if (vg::parseQuestCatalog(text, "quests.toml", withCatalogs ? &items() : nullptr, withCatalogs ? &npcs() : nullptr,
                              withCatalogs ? &monsters() : nullptr, c, error))
        return {};
    return error;
}

std::string replaced(std::string text, const std::string& from, const std::string& to)
{
    const auto at = text.find(from);
    REQUIRE(at != std::string::npos);
    text.replace(at, from.size(), to);
    return text;
}

vg::QuestCatalog sample()
{
    vg::QuestCatalog c;
    std::string error;
    const bool ok = vg::parseQuestCatalog(kSample, "quests.toml", &items(), nullptr, nullptr, c, error);
    INFO(error);
    REQUIRE(ok);
    return c;
}

vg::Inventory inventoryWith(std::vector<vg::StoredItem> rows)
{
    vg::Inventory inv;
    std::string error;
    REQUIRE(vg::Inventory::fromItems(rows, inv, error));
    return inv;
}

vg::QuestRecord active(vg::QuestId id, std::array<std::uint16_t, 3> progress = {})
{
    vg::QuestRecord r;
    r.quest = id;
    r.state = vn::QuestState::Active;
    r.progress = progress;
    return r;
}

} // namespace

TEST_CASE("Quests: the sample parses with its fields", "[game][quests][data]")
{
    const vg::QuestCatalog c = sample();
    REQUIRE(c.size() == 3);
    CHECK(c.maxActive() == 2);
    const vg::QuestDef* q1 = c.find(1);
    REQUIRE(q1);
    CHECK(q1->giver == 10);
    CHECK(q1->turnIn == 11);
    CHECK(q1->objectives.size() == 1);
    CHECK(q1->objectives[0].kind == vg::ObjectiveKind::Talk);
    const vg::QuestDef* q2 = c.find(2);
    REQUIRE(q2);
    CHECK(q2->turnIn == 11); // default: the giver
    CHECK(q2->after == 1);
    CHECK(q2->level == 2);
    CHECK(q2->objectives[0].monsters == std::vector<std::uint16_t>{1, 2});
    CHECK(q2->objectives[1].item == 9);
    CHECK(q2->objectives[1].label == "Kül Tozu"); // the item's name (items given)
    CHECK(q2->reward.items == std::vector<vg::QuestRewardItem>{{16, 2}});
    CHECK(q2->reward.classItems[static_cast<int>(vg::CharClass::Rogue)] == 77);
    const vg::QuestDef* q3 = c.find(3);
    REQUIRE(q3);
    CHECK(q3->repeatable);
    CHECK(q3->cooldownS == 600);
    CHECK(q3->race == static_cast<std::uint8_t>(vg::Race::Kargan));
    CHECK(q3->reward.choice == std::vector<vg::ItemTemplateId>{71, 76});
    CHECK(c.givenBy(11).size() == 2);
    CHECK(c.handedInTo(11).size() == 3);
}

TEST_CASE("Quests: the loader refuses what the format forbids", "[game][quests][data]")
{
    struct Case
    {
        const char* name;
        std::string text;
        const char* message;
    };
    const std::vector<Case> cases = {
        {"unknown top key", kSample + "\n[other]\nx = 1\n", "unknown key 'other'"},
        {"unknown quest key", replaced(kSample, "name = \"Bir\"", "name = \"Bir\"\ncolour = 1"), "unknown key 'colour'"},
        {"id 0", replaced(kSample, "id = 1\n", "id = 0\n"), "out of range"},
        {"id twice", replaced(kSample, "id = 3\n", "id = 2\n"), "id 2 used twice"},
        {"empty name", replaced(kSample, "name = \"Bir\"", "name = \"\""), "must not be empty"},
        {"no story", replaced(kSample, "story = \"s\"\ndone_text = \"d\"\n[[quest.objective]]\ntype = \"talk\"", "done_text = \"d\"\n[[quest.objective]]\ntype = \"talk\""), "missing 'story'"},
        {"bad race", replaced(kSample, "race = \"kargan\"", "race = \"ork\""), "unknown race"},
        {"after later", replaced(kSample, "after = 1", "after = 3"), "earlier in the file"},
        {"after itself", replaced(kSample, "after = 1", "after = 2"), "earlier in the file"},
        {"repeatable without cooldown", replaced(kSample, "cooldown_s = 600\n", ""), "missing 'cooldown_s'"},
        {"short cooldown", replaced(kSample, "cooldown_s = 600", "cooldown_s = 5"), "out of range"},
        {"cooldown one-time", replaced(kSample, "after = 1\n", "after = 1\ncooldown_s = 600\n"), "only for a repeatable"},
        {"no objective", replaced(kSample, "[[quest.objective]]\ntype = \"talk\"\nnpc = 11\n", ""), "no [[quest.objective]]"},
        {"four objectives",
         replaced(kSample, "[[quest.objective]]\ntype = \"boss\"",
                  "[[quest.objective]]\ntype = \"boss\"\nmonsters = [5]\ncount = 1\n[[quest.objective]]\ntype = \"boss\"\nmonsters = [5]\ncount = 1\n[[quest.objective]]\ntype = \"boss\"\nmonsters = [5]\ncount = 1\n[[quest.objective]]\ntype = \"boss\""),
         "1..3"},
        {"unknown type", replaced(kSample, "type = \"talk\"", "type = \"escort\""), "unknown type 'escort'"},
        {"count 0", replaced(kSample, "count = 3", "count = 0"), "out of range"},
        {"count 51", replaced(kSample, "count = 3", "count = 51"), "out of range"},
        {"boss count 6", replaced(kSample, "monsters = [5]\ncount = 1", "monsters = [5]\ncount = 6"), "out of range"},
        {"talk to another", replaced(kSample, "npc = 11", "npc = 12"), "turn_in NPC"},
        {"handed in elsewhere without talk", replaced(kSample, "type = \"talk\"\nnpc = 11", "type = \"kill\"\nmonsters = [1]\ncount = 1"), "needs a talk objective"},
        {"item on a kill", replaced(kSample, "monsters = [1, 2]\ncount = 3", "monsters = [1, 2]\ncount = 3\nitem = 9"), "does not belong"},
        {"monster twice", replaced(kSample, "monsters = [1, 2]", "monsters = [1, 1]"), "twice"},
        {"no reward", replaced(kSample, "[quest.reward]\ncoins = 5\nchoice = [71, 76]\n", ""), "needs a [quest.reward]"},
        {"empty reward", replaced(kSample, "coins = 5\nchoice = [71, 76]\n", ""), "must reward something"},
        {"xp cap", replaced(kSample, "xp = 50", "xp = 100001"), "out of range"},
        {"choice of one", replaced(kSample, "choice = [71, 76]", "choice = [71]"), "2..4"},
        {"half class items", replaced(kSample, "class_items = { warrior = 72, rogue = 77, mage = 82, priest = 72 }", "class_items = { warrior = 72 }"), "missing 'rogue'"},
        {"class item of another class", replaced(kSample, "rogue = 77", "rogue = 72"), "is not equipment a rogue may wear"},
        {"collect of a weapon", replaced(kSample, "item = 9", "item = 1"), "must stack"},
        {"reward item twice", replaced(kSample, "items = [[16, 2]]", "items = [[16, 2], [16, 1]]"), "twice"},
        {"unknown item", replaced(kSample, "items = [[16, 2]]", "items = [[60000, 1]]"), "not in items.toml"},
    };
    for (const Case& c : cases)
    {
        INFO(c.name);
        vg::QuestCatalog out;
        std::string error;
        CHECK_FALSE(vg::parseQuestCatalog(c.text, "quests.toml", &items(), nullptr, nullptr, out, error));
        CHECK_THAT(error, ContainsSubstring(c.message));
        CHECK_THAT(error, ContainsSubstring("quests.toml"));
    }
    // With the other catalogs: unknown NPCs and monsters, a boss objective of a plain monster.
    CHECK_THAT(sampleError(replaced(kSample, "giver = 10", "giver = 900"), true), ContainsSubstring("NPC 900 is not in npcs.toml"));
    CHECK_THAT(sampleError(replaced(kSample, "monsters = [1, 2]", "monsters = [1, 999]"), true),
               ContainsSubstring("monster 999 is not in monsters.toml"));
    CHECK_THAT(sampleError(replaced(kSample, "monsters = [5]", "monsters = [1]"), true), ContainsSubstring("is not a boss"));
}

TEST_CASE("Quests: the shipped quest line", "[game][quests][content]")
{
    const vg::QuestCatalog& c = shipped();
    CHECK(c.maxActive() == 10);
    std::set<std::uint16_t> givers;
    int repeatables = 0;
    for (const vg::QuestDef& q : c.quests())
    {
        INFO(std::format("quest {} {}", q.id, q.name));
        const vg::NpcDef* giver = npcs().find(q.giver);
        const vg::NpcDef* turnIn = npcs().find(q.turnIn);
        REQUIRE(giver);
        REQUIRE(turnIn);
        givers.insert(q.giver);
        repeatables += q.repeatable ? 1 : 0;
        // A homeland quest is given in its race's home zone (2 Tanyeri for Aydor, 3 Demirağız for Kargan).
        if (giver->zone == 2)
            CHECK(q.race == static_cast<std::uint8_t>(vg::Race::Aydor));
        if (giver->zone == 3)
            CHECK(q.race == static_cast<std::uint8_t>(vg::Race::Kargan));
        // Monsters of kill objectives are not above the quest's level + 2 (bosses: + 1).
        for (const vg::QuestObjective& o : q.objectives)
            for (const std::uint16_t m : o.monsters)
            {
                const vg::MonsterTemplate* t = monsters().find(m);
                REQUIRE(t);
                CHECK(t->level <= q.level + 2);
            }
        // Reward items exist and the class items fit their class; collect items drop somewhere (stackable).
        for (std::uint8_t cls = 1; cls <= 4; ++cls)
            if (const vg::ItemTemplateId id = q.reward.classItems[cls]; id != 0)
                CHECK(items().find(id)->allowsClass(cls));
    }
    // 2-3 new quest givers in Eşik, one in each homeland, plus the two merchants that take part.
    int esikGivers = 0;
    for (const std::uint16_t id : givers)
        esikGivers += npcs().find(id)->zone == 1 ? 1 : 0;
    CHECK(esikGivers >= 3);
    CHECK(givers.count(13) == 1);
    CHECK(givers.count(14) == 1);
    CHECK(givers.count(2) == 1); // Demirci Ilgaz: a merchant that also gives a quest
    CHECK(repeatables >= 2);
    // The first quest of Eşik is open to every new character; a Ham Tav Taşı quest ends at the smith.
    const vg::QuestDef* first = c.find(1);
    REQUIRE(first);
    CHECK(first->level == 1);
    CHECK(first->after == 0);
    CHECK(first->race == 0);
    bool teachesUpgrade = false;
    for (const vg::QuestDef& q : c.quests())
        for (const vg::QuestRewardItem& r : q.reward.items)
            teachesUpgrade = teachesUpgrade || (r.templateId == 166 && q.turnIn == 2);
    CHECK(teachesUpgrade);
}

TEST_CASE("Quests: the line takes each race from Esik to its homeland; 25-35 % of the experience of levels 1-10", "[game][quests][economy]")
{
    const vg::QuestCatalog& c = shipped();
    std::uint32_t levelXp = 0;
    for (std::uint16_t l = 1; l < 10; ++l)
        levelXp += vg::xpToNext(l);
    CHECK(levelXp == 10'700);
    for (const vg::Race race : {vg::Race::Aydor, vg::Race::Kargan})
    {
        // Walk the chain as a player would: everything one-time of this race in file order.
        vg::QuestLog log;
        std::uint32_t xp = 0, coins = 0, esik = 0, home = 0, count = 0;
        for (const vg::QuestDef& q : c.quests())
        {
            if (q.repeatable || (q.race != 0 && q.race != static_cast<std::uint8_t>(race)))
                continue;
            INFO(std::format("quest {} {}", q.id, q.name));
            CHECK((q.after == 0 || log.isDone(q.after)));
            vg::QuestRecord done;
            done.quest = q.id;
            done.state = vn::QuestState::Done;
            done.completions = 1;
            log.put(done);
            xp += q.reward.xp;
            coins += q.reward.coins;
            ++count;
            (npcs().find(q.giver)->zone == 1 ? esik : home) += 1;
        }
        const double share = static_cast<double>(xp) / levelXp;
        if (printTables())
            std::printf("quests %s: %u quests (%u Eşik, %u homeland), %u xp (%.1f %% of levels 1-10), %u Akçe\n",
                        race == vg::Race::Aydor ? "Aydor" : "Kargan", count, esik, home, xp, share * 100.0, coins);
        CHECK(count >= 14);
        CHECK(esik >= 10);
        CHECK(home >= 4);
        CHECK(share >= 0.25);
        CHECK(share <= 0.35);
        CHECK(coins < 1500); // a supplement to the hunt's Akçe (~6-8 thousand by level 10), not a replacement
    }
}

TEST_CASE("Quests: accept checks in order", "[game][quests][rules]")
{
    const vg::QuestCatalog c = sample();
    const vg::QuestDef& q1 = *c.find(1);
    const vg::QuestDef& q2 = *c.find(2);
    const vg::QuestDef& q3 = *c.find(3);
    vg::QuestLog log;
    const vg::QuestUser aydor{1, 1, 1};
    const vg::QuestUser kargan{2, 1, 1};
    CHECK(vg::checkQuestAccept(c, q1, log, aydor, 0, 2) == QR::Ok);
    CHECK(vg::checkQuestAccept(c, q2, log, {1, 1, 1}, 0, 2) == QR::LevelTooLow);
    CHECK(vg::checkQuestAccept(c, q2, log, {1, 1, 2}, 0, 2) == QR::NeedsQuest);
    CHECK(vg::checkQuestAccept(c, q3, log, aydor, 0, 2) == QR::WrongRace);
    CHECK(vg::checkQuestAccept(c, q3, log, kargan, 0, 2) == QR::Ok);
    log.put(active(1));
    CHECK(vg::checkQuestAccept(c, q1, log, aydor, 0, 2) == QR::AlreadyActive);
    CHECK(vg::checkQuestAccept(c, q2, log, {1, 1, 2}, 0, 2) == QR::NeedsQuest); // active is not done
    log.put({1, vn::QuestState::Done, {}, 1, 100});
    CHECK(vg::checkQuestAccept(c, q1, log, aydor, 200, 2) == QR::AlreadyDone);
    CHECK(vg::checkQuestAccept(c, q2, log, {1, 1, 2}, 0, 2) == QR::Ok);
    // The limit (sample: 2): a third quest is refused.
    log.put(active(2));
    log.put(active(3));
    CHECK(log.activeCount() == 2);
    vg::QuestLog full = log;
    full.erase(3);
    full.put(active(50));
    CHECK(vg::checkQuestAccept(c, q3, full, kargan, 0, 2) == QR::TooManyActive);
    // Race and level come before the limit; the limit is the caller's (the server may lower it for tests).
    CHECK(vg::checkQuestAccept(c, q3, full, aydor, 0, 2) == QR::WrongRace);
    CHECK(vg::checkQuestAccept(c, q3, full, kargan, 0, 5) == QR::Ok);
}

TEST_CASE("Quests: repeatable cooldown", "[game][quests][rules]")
{
    const vg::QuestCatalog c = sample();
    const vg::QuestDef& q3 = *c.find(3);
    vg::QuestLog log;
    log.put({3, vn::QuestState::Done, {}, 1, 1000});
    const vg::QuestUser kargan{2, 1, 1};
    CHECK(vg::questCooldownLeft(q3, log.find(3), 1000) == 600);
    CHECK(vg::questCooldownLeft(q3, log.find(3), 1599) == 1);
    CHECK(vg::questCooldownLeft(q3, log.find(3), 1600) == 0);
    CHECK(vg::checkQuestAccept(c, q3, log, kargan, 1300, 2) == QR::OnCooldown);
    CHECK(vg::checkQuestAccept(c, q3, log, kargan, 1600, 2) == QR::Ok);
    // A clock that went back never makes the wait longer than the cooldown.
    CHECK(vg::questCooldownLeft(q3, log.find(3), 0) == 600);
    // Not on cooldown: one-time quests, active ones, no record.
    CHECK(vg::questCooldownLeft(*c.find(1), log.find(3), 1000) == 0);
    CHECK(vg::questCooldownLeft(q3, nullptr, 1000) == 0);
}

TEST_CASE("Quests: kills count for active kill / boss objectives only", "[game][quests][rules]")
{
    const vg::QuestCatalog c = sample();
    vg::QuestLog log;
    log.put(active(2));
    CHECK(vg::noteQuestKill(c, log, 2) == std::vector<vg::QuestId>{2});
    CHECK(vg::noteQuestKill(c, log, 1) == std::vector<vg::QuestId>{2});
    CHECK(vg::noteQuestKill(c, log, 7).empty()); // another monster
    CHECK(log.find(2)->progress[0] == 2);
    CHECK(vg::noteQuestKill(c, log, 1) == std::vector<vg::QuestId>{2});
    CHECK(vg::noteQuestKill(c, log, 1).empty()); // the objective is complete: no more counting
    CHECK(log.find(2)->progress[0] == 3);
    CHECK(log.find(2)->progress[1] == 0); // the collect objective counts the bag, not kills
    // A done quest does not count.
    log.put({3, vn::QuestState::Done, {}, 1, 0});
    CHECK(vg::noteQuestKill(c, log, 5).empty());
    log.put(active(3));
    CHECK(vg::noteQuestKill(c, log, 5) == std::vector<vg::QuestId>{3});
}

TEST_CASE("Quests: readiness, collect counting in the bag", "[game][quests][rules]")
{
    const vg::QuestCatalog c = sample();
    const vg::QuestDef& q2 = *c.find(2);
    const vg::Inventory empty;
    CHECK_FALSE(vg::questReady(q2, active(2, {3, 0, 0}), empty, &items()));
    const vg::Inventory one = inventoryWith({{5, vg::ItemPlace::bag(4), 9, 1}});
    CHECK(vg::objectiveProgress(q2, active(2), one, &items(), 1) == 1);
    CHECK_FALSE(vg::questReady(q2, active(2, {3, 0, 0}), one, &items()));
    const vg::Inventory two = inventoryWith({{5, vg::ItemPlace::bag(4), 9, 1}, {6, vg::ItemPlace::bag(9), 9, 4}});
    CHECK(vg::bagUnits(two, 9, &items()) == 5);
    CHECK(vg::objectiveProgress(q2, active(2), two, &items(), 1) == 2); // capped at the count
    CHECK(vg::questReady(q2, active(2, {3, 0, 0}), two, &items()));
    CHECK_FALSE(vg::questReady(q2, active(2, {2, 0, 0}), two, &items()));
    // The talk objective is met while the quest is active (it is handed in to that NPC).
    CHECK(vg::questReady(*c.find(1), active(1), empty, &items()));
    vg::QuestRecord done{1, vn::QuestState::Done, {}, 1, 0};
    CHECK_FALSE(vg::questReady(*c.find(1), done, empty, &items()));
}

TEST_CASE("Quests: hand-in plan", "[game][quests][rules]")
{
    const vg::QuestCatalog c = sample();
    const vg::QuestDef& q2 = *c.find(2);
    const vg::QuestUser rogue{1, static_cast<std::uint8_t>(vg::CharClass::Rogue), 2};
    vg::QuestLog log;
    const vg::Inventory inv = inventoryWith({{5, vg::ItemPlace::bag(0), 9, 1}, {6, vg::ItemPlace::bag(1), 9, 4}});
    CHECK(vg::planQuestHandIn(items(), inv, 0, q2, log, rogue, vn::kQuestNoChoice).result == QR::NotActive);
    log.put(active(2, {2, 0, 0}));
    CHECK(vg::planQuestHandIn(items(), inv, 0, q2, log, rogue, vn::kQuestNoChoice).result == QR::NotReady);
    log.put(active(2, {3, 0, 0}));
    CHECK(vg::planQuestHandIn(items(), inv, 0, q2, log, rogue, 0).result == QR::BadChoice); // no choice offered
    const vg::QuestHandInPlan plan = vg::planQuestHandIn(items(), inv, 0, q2, log, rogue, vn::kQuestNoChoice);
    REQUIRE(plan.result == QR::Ok);
    // Two units taken: the whole first stack, one of the second.
    REQUIRE(plan.removed.size() == 2);
    CHECK(plan.removed[0] == vg::ItemQuantityChange{5, vg::ItemPlace::bag(0), 1, 0});
    CHECK(plan.removed[1] == vg::ItemQuantityChange{6, vg::ItemPlace::bag(1), 4, 3});
    // Rewards: 2 Kızılca Şerbet, then the rogue's Eski Tozluk; bag slot 0 freed by the removal is used.
    CHECK(plan.rewards == std::vector<vg::QuestRewardItem>{{16, 2}, {77, 1}});
    REQUIRE(plan.add.created.size() == 2);
    CHECK(plan.add.created[0].place == vg::ItemPlace::bag(0));
    CHECK(plan.xp == 50);
    CHECK(plan.coins == 20);
    vg::Inventory after = inv;
    plan.apply(after, {100, 101});
    CHECK(vg::bagUnits(after, 9, &items()) == 3);
    CHECK(after.at(vg::ItemPlace::bag(0)).templateId == 16);
    CHECK(after.at(vg::ItemPlace::bag(2)).templateId == 77);
    const auto changes = plan.changes(inv, {100, 101});
    CHECK(changes.size() == 3); // bag 0 (removed, then the potions), bag 1, bag 2
    // PurseFull: the reward would pass the cap.
    CHECK(vg::planQuestHandIn(items(), inv, vn::kMaxCoins - 10, q2, log, rogue, vn::kQuestNoChoice).result == QR::PurseFull);
    // BagFull: 28 single items and nothing the removal frees (the collected units sit in a stack that remains).
    std::vector<vg::StoredItem> full;
    for (std::uint8_t s = 0; s < 27; ++s)
        full.push_back({static_cast<vg::ItemInstanceId>(10 + s), vg::ItemPlace::bag(s), 1, 1});
    full.push_back({99, vg::ItemPlace::bag(27), 9, 5});
    CHECK(vg::planQuestHandIn(items(), inventoryWith(full), 0, q2, log, rogue, vn::kQuestNoChoice).result == QR::BagFull);
    // A choice: an index into the list, nothing else.
    const vg::QuestDef& q3 = *c.find(3);
    log.put(active(3, {1, 0, 0}));
    CHECK(vg::planQuestHandIn(items(), inv, 0, q3, log, rogue, vn::kQuestNoChoice).result == QR::BadChoice);
    CHECK(vg::planQuestHandIn(items(), inv, 0, q3, log, rogue, 2).result == QR::BadChoice);
    const vg::QuestHandInPlan chosen = vg::planQuestHandIn(items(), inv, 0, q3, log, rogue, 1);
    REQUIRE(chosen.result == QR::Ok);
    CHECK(chosen.rewards == std::vector<vg::QuestRewardItem>{{76, 1}});
    CHECK(chosen.removed.empty());
}

TEST_CASE("Quests: status and NPC markers", "[game][quests][rules]")
{
    const vg::QuestCatalog c = sample();
    const vg::Inventory inv;
    vg::QuestLog log;
    const vg::QuestUser me{1, 1, 1};
    using S = vg::QuestStatus;
    using M = vg::QuestMarker;
    CHECK(vg::questStatus(c, *c.find(1), log, me, inv, &items(), 0, 2) == S::Available);
    CHECK(vg::questStatus(c, *c.find(2), log, me, inv, &items(), 0, 2) == S::Later);
    CHECK(vg::questStatus(c, *c.find(3), log, me, inv, &items(), 0, 2) == S::Hidden); // another race
    CHECK(vg::questMarkerFor(c, 10, log, me, inv, &items(), 0, 2) == M::Available);
    CHECK(vg::questMarkerFor(c, 11, log, me, inv, &items(), 0, 2) == M::None);
    log.put(active(1));
    // Quest 1 (talk to 11) is ready at once at its turn-in NPC; the giver shows it in progress.
    CHECK(vg::questMarkerFor(c, 11, log, me, inv, &items(), 0, 2) == M::Ready);
    CHECK(vg::questMarkerFor(c, 10, log, me, inv, &items(), 0, 2) == M::InProgress);
    log.put({1, vn::QuestState::Done, {}, 1, 0});
    CHECK(vg::questStatus(c, *c.find(1), log, me, inv, &items(), 0, 2) == S::Done);
    CHECK(vg::questMarkerFor(c, 10, log, me, inv, &items(), 0, 2) == M::None);
    const vg::QuestUser level2{1, 1, 2};
    CHECK(vg::questMarkerFor(c, 11, log, level2, inv, &items(), 0, 2) == M::Available);
    log.put(active(2, {3, 0, 0}));
    CHECK(vg::questMarkerFor(c, 11, log, level2, inv, &items(), 0, 2) == M::InProgress); // the collect is not met
    const vg::QuestUser kargan{2, 1, 1};
    log.put({3, vn::QuestState::Done, {}, 1, 100});
    CHECK(vg::questStatus(c, *c.find(3), log, kargan, inv, &items(), 300, 2) == S::Cooldown);
    CHECK(vg::questStatus(c, *c.find(3), log, kargan, inv, &items(), 700, 2) == S::Available);
    CHECK(vg::questMarkerName(M::Ready) == "ready");
}

TEST_CASE("Quests: v11 messages round trip and refuse what the format forbids", "[game][quests][net]")
{
    CHECK(vn::kProtocolVersion == 11);
    vn::QuestList list;
    list.quests = {{1, vn::QuestState::Done, {0, 0, 0}, 0}, {2, vn::QuestState::Active, {3, 1, 0}, 0},
                   {41, vn::QuestState::Done, {}, 1795}};
    const auto bytes = vn::encodeMessage(list);
    REQUIRE(bytes);
    CHECK(bytes->size() == 1 + 3 * 13);
    vn::QuestList back;
    REQUIRE(vn::decodeMessage(*bytes, back));
    CHECK(back == list);
    // State None is only for QUEST_UPDATE.
    vn::QuestList bad = list;
    bad.quests[0].state = vn::QuestState::None;
    CHECK_FALSE(vn::encodeMessage(bad));
    std::vector<std::uint8_t> raw = *bytes;
    raw[3] = 0; // first entry's state
    CHECK_FALSE(vn::decodeMessage(raw, back));
    raw[3] = 3;
    CHECK_FALSE(vn::decodeMessage(raw, back));
    const vn::QuestUpdate update{{7, vn::QuestState::None, {}, 0}};
    const auto ub = vn::encodeMessage(update);
    REQUIRE(ub);
    CHECK(ub->size() == 13);
    vn::QuestUpdate ubBack;
    REQUIRE(vn::decodeMessage(*ub, ubBack));
    CHECK(ubBack == update);
    const vn::QuestAcceptReq accept{9, 0x7E000001u, 4};
    vn::QuestAcceptReq aBack;
    REQUIRE(vn::decodeMessage(*vn::encodeMessage(accept), aBack));
    CHECK(aBack == accept);
    CHECK(vn::encodeMessage(accept)->size() == 10);
    const vn::QuestAbandonReq abandon{3, 2};
    vn::QuestAbandonReq abBack;
    REQUIRE(vn::decodeMessage(*vn::encodeMessage(abandon), abBack));
    CHECK(abBack == abandon);
    const vn::QuestHandInReq handIn{11, 5, 8, 2};
    vn::QuestHandInReq hBack;
    REQUIRE(vn::decodeMessage(*vn::encodeMessage(handIn), hBack));
    CHECK(hBack == handIn);
    CHECK(vn::encodeMessage(handIn)->size() == 11);
    const vn::QuestRes res{11, vn::QuestAction::HandIn, 8, QR::Ok, 220, 45};
    const auto rb = vn::encodeMessage(res);
    REQUIRE(rb);
    CHECK(rb->size() == 16);
    vn::QuestRes rBack;
    REQUIRE(vn::decodeMessage(*rb, rBack));
    CHECK(rBack == res);
    std::vector<std::uint8_t> badRes = *rb;
    badRes[4] = 0; // action
    CHECK_FALSE(vn::decodeMessage(badRes, rBack));
    badRes = *rb;
    badRes[7] = vn::kQuestResultMax + 1;
    CHECK_FALSE(vn::decodeMessage(badRes, rBack));
    CHECK(vn::questResultName(QR::TooManyActive) == "TooManyActive");
    CHECK(vn::opcodeName(vn::Opcode::QuestHandInReq) == "QUEST_HANDIN_REQ");
    CHECK(vn::isKnownOpcode(0x0B05));
    CHECK_FALSE(vn::isKnownOpcode(0x0B06));
}

TEST_CASE("Quests: NPC kinds, tints and shared models", "[game][quests][data]")
{
    const std::string base = "[[npc]]\nid = 1\nname = \"A\"\ntitle = \"T\"\ngreeting = \"G\"\nmodel = \"npc_test\"\nzone = 1\n"
                             "x = 10.0\nz = 10.0\nyaw = 0.0\nkind = \"quest\"\n";
    vg::NpcCatalog c;
    std::string error;
    REQUIRE(vg::parseNpcCatalog(base, "npcs.toml", &items(), c, error));
    CHECK(c.npcs().front().kind == vg::NpcKind::Quest);
    CHECK_FALSE(c.npcs().front().isMerchant());
    CHECK_FALSE(vg::parseNpcCatalog(base + "stock = [16]\n", "npcs.toml", &items(), c, error));
    CHECK_THAT(error, ContainsSubstring("a quest NPC has no 'stock'"));
    const std::string second = replaced(base, "id = 1", "id = 2");
    CHECK_FALSE(vg::parseNpcCatalog(base + second, "npcs.toml", &items(), c, error));
    CHECK_THAT(error, ContainsSubstring("used twice"));
    REQUIRE(vg::parseNpcCatalog(base + second + "tint = [200, 180, 160]\n", "npcs.toml", &items(), c, error));
    CHECK(c.find(2)->tint == std::array<std::uint8_t, 3>{200, 180, 160});
    REQUIRE(vg::parseNpcCatalog(base + second + "scale = 1.1\n", "npcs.toml", &items(), c, error));
    CHECK(c.find(2)->scale == 1.1f);
    CHECK_FALSE(vg::parseNpcCatalog(base + "scale = 2.0\n", "npcs.toml", &items(), c, error));
    CHECK_FALSE(vg::parseNpcCatalog(base + "tint = [300, 0, 0]\n", "npcs.toml", &items(), c, error));
    CHECK_FALSE(vg::parseNpcCatalog(base + "tint = [1, 2]\n", "npcs.toml", &items(), c, error));
    // The shipped quest givers.
    for (const std::uint16_t id : std::initializer_list<std::uint16_t>{10, 11, 12, 13, 14})
    {
        const vg::NpcDef* n = npcs().find(id);
        REQUIRE(n);
        CHECK(n->kind == vg::NpcKind::Quest);
        CHECK(n->stock.empty());
    }
}
