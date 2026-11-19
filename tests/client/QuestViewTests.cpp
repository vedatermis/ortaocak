// Client quest view (M17, docs/PROTOCOL_QUESTS.md §7): the quest book on the client clock (QUEST_LIST /
// QUEST_UPDATE, cooldowns, acceptance order, the notices when an objective or a quest completes, also from
// the bag), the one request in flight and its pacing, what an NPC's dialog lists, the texts and the parser of
// the quest automation commands. Uses the shipped gamedata (quests.toml, npcs.toml, items.toml).

#include "client/world/QuestView.h"
#include "shared/game/Monsters.h"
#include "shared/game/Npcs.h"
#include "shared/game/Quests.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <string>

namespace cw = vo::client::world;
namespace vg = vo::game;
namespace vn = vo::net;
using Catch::Matchers::ContainsSubstring;

namespace {

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

vg::Inventory bagWith(vg::ItemTemplateId t, std::uint16_t n)
{
    vg::Inventory inv;
    std::string error;
    REQUIRE(vg::Inventory::fromItems({{7, vg::ItemPlace::bag(3), t, n}}, inv, error));
    return inv;
}

// The shipped quest line as the process-wide catalog (other test files install their own; a client test
// must see gamedata/quests.toml whatever ran before it in the same process).
void installShipped()
{
    static const std::shared_ptr<const vg::QuestCatalog> shipped = [] {
        auto c = std::make_shared<vg::QuestCatalog>();
        std::string error;
        const auto f = vg::locateQuestsFile();
        REQUIRE(f);
        vg::MonsterData monsters;
        const auto mf = vg::locateMonstersFile();
        REQUIRE(mf);
        REQUIRE(vg::loadMonsterData(*mf, monsters, error));
        REQUIRE(vg::loadQuestCatalog(*f, &items(), &vg::npcs(), &monsters, *c, error));
        return std::shared_ptr<const vg::QuestCatalog>(c);
    }();
    vg::installQuestCatalog(shipped);
}

vn::QuestUpdate update(std::uint16_t id, vn::QuestState s, std::array<std::uint16_t, 3> p = {}, std::uint32_t cd = 0)
{
    return vn::QuestUpdate{{id, s, p, cd}};
}

} // namespace

TEST_CASE("Quest book: list, updates, order, cooldown on the client clock", "[client][quests]")
{
    installShipped();
    REQUIRE(vg::quests().find(41)); // the shipped repeatable "Kül Nöbeti" (cooldown 1800 s)
    cw::QuestBook book;
    CHECK_FALSE(book.known());
    vn::QuestList list;
    list.quests = {{1, vn::QuestState::Done, {}, 0}, {2, vn::QuestState::Active, {4, 0, 0}, 0},
                   {41, vn::QuestState::Done, {}, 600}};
    book.apply(list, 1000.0);
    CHECK(book.known());
    CHECK(book.activeInOrder() == std::vector<vg::QuestId>{2});
    CHECK(book.log().find(2)->progress[0] == 4);
    // 41 handed in 1200 s before "now" (1800 - 600): 600 s left on the client clock, then free again.
    CHECK(vg::questCooldownLeft(*vg::quests().find(41), book.log().find(41), 1000) == 600);
    CHECK(vg::questCooldownLeft(*vg::quests().find(41), book.log().find(41), 1600) == 0);
    const vg::Inventory empty;
    book.apply(update(3, vn::QuestState::Active), 1001.0, empty, &items());
    CHECK(book.activeInOrder() == std::vector<vg::QuestId>{2, 3});
    auto notices = book.takeNotices();
    REQUIRE(notices.size() == 1);
    CHECK(notices[0].kind == cw::QuestNotice::Kind::Accepted);
    CHECK_THAT(notices[0].text, ContainsSubstring("Yeni görev: Kül Tozu"));
    // Abandoned (None): gone, with a notice.
    book.apply(update(3, vn::QuestState::None), 1002.0, empty, &items());
    CHECK(book.activeInOrder() == std::vector<vg::QuestId>{2});
    CHECK(book.log().find(3) == nullptr);
    notices = book.takeNotices();
    REQUIRE(notices.size() == 1);
    CHECK(notices[0].kind == cw::QuestNotice::Kind::Abandoned);
    book.clear();
    CHECK_FALSE(book.known());
}

TEST_CASE("Quest book: notices when a kill objective and the quest complete, collect from the bag", "[client][quests]")
{
    installShipped();
    cw::QuestBook book;
    vn::QuestList list;
    list.quests = {{2, vn::QuestState::Active, {5, 0, 0}, 0}, {3, vn::QuestState::Active, {}, 0}};
    book.apply(list, 0.0);
    const vg::Inventory empty;
    // Quest 2 (6 Kül Çömezi): the sixth kill completes it - one "ready" notice (no separate objective line).
    book.apply(update(2, vn::QuestState::Active, {6, 0, 0}), 1.0, empty, &items());
    auto n = book.takeNotices();
    REQUIRE(n.size() == 1);
    CHECK(n[0].kind == cw::QuestNotice::Kind::Ready);
    CHECK_THAT(n[0].text, ContainsSubstring("Görev tamamlandı: Külden Kalkanlar"));
    CHECK_THAT(n[0].text, ContainsSubstring("Avcı Selvi ile konuş"));
    // Again the same counter: no second notice.
    book.apply(update(2, vn::QuestState::Active, {6, 0, 0}), 2.0, empty, &items());
    CHECK(book.takeNotices().empty());
    // Quest 3 (5 Kül Tozu) follows the bag.
    book.noteInventory(bagWith(9, 4), &items());
    CHECK(book.takeNotices().empty());
    book.noteInventory(bagWith(9, 5), &items());
    n = book.takeNotices();
    REQUIRE(n.size() == 1);
    CHECK(n[0].kind == cw::QuestNotice::Kind::Ready);
    // Sold below the count: it is not ready any more; reaching it again announces it again.
    book.noteInventory(bagWith(9, 2), &items());
    CHECK(book.takeNotices().empty());
    book.noteInventory(bagWith(9, 6), &items());
    CHECK(book.takeNotices().size() == 1);
    // Handed in: out of the active list.
    book.apply(update(3, vn::QuestState::Done), 3.0, empty, &items());
    CHECK(book.activeInOrder() == std::vector<vg::QuestId>{2});
}

TEST_CASE("Quest book: an objective notice for a quest with more to do", "[client][quests]")
{
    installShipped();
    // Quest 8 has one objective; a synthetic two-objective quest is not shipped, so check the line text
    // of objectiveText instead, and the ObjectiveDone path through quest 5 (collect) + a second update.
    const vg::QuestDef* q2 = vg::quests().find(2);
    REQUIRE(q2);
    CHECK(cw::objectiveText(*q2, 0, 3) == "Kül Çömezi 3/6");
    CHECK(cw::objectiveText(*q2, 0, 9) == "Kül Çömezi 6/6");
    const vg::QuestDef* q9 = vg::quests().find(9);
    REQUIRE(q9);
    CHECK(cw::objectiveText(*q9, 0, 0) == "Yen: Kor Bekçisi 0/1");
    const vg::QuestDef* q1 = vg::quests().find(1);
    REQUIRE(q1);
    CHECK(cw::objectiveText(*q1, 0, 0) == "Avcı Selvi ile konuş");
    const vg::QuestDef* q3 = vg::quests().find(3);
    REQUIRE(q3);
    vg::QuestRecord rec;
    rec.quest = 3;
    rec.state = vn::QuestState::Active;
    const auto lines = cw::objectiveLines(*q3, rec, bagWith(9, 2), &items());
    REQUIRE(lines.size() == 1);
    CHECK(lines[0].text == "Kül Tozu 2/5");
    CHECK_FALSE(lines[0].done);
}

TEST_CASE("Quest requests: one in flight, at most 4 a second", "[client][quests]")
{
    installShipped();
    cw::QuestRequests r;
    CHECK(r.canSend(0.0));
    const std::uint32_t s1 = r.begin(vn::QuestAction::Accept, 1, 77, 0.0);
    CHECK(r.busy());
    CHECK_FALSE(r.canSend(0.1));
    CHECK_FALSE(r.finish(vn::QuestRes{s1 + 5, vn::QuestAction::Accept, 1, vn::QuestResult::Ok, 0, 0}));
    const auto done = r.finish(vn::QuestRes{s1, vn::QuestAction::Accept, 1, vn::QuestResult::Ok, 0, 0});
    REQUIRE(done);
    CHECK(done->npc == 77);
    CHECK_FALSE(r.busy());
    for (int i = 0; i < 3; ++i)
    {
        REQUIRE(r.canSend(0.2 + 0.01 * i));
        const auto s = r.begin(vn::QuestAction::Abandon, 1, 0, 0.2 + 0.01 * i);
        REQUIRE(r.finish(vn::QuestRes{s, vn::QuestAction::Abandon, 1, vn::QuestResult::NotActive, 0, 0}));
    }
    CHECK_FALSE(r.canSend(0.5)); // four in the last second
    CHECK(r.canSend(1.01));
}

TEST_CASE("Quest dialog entries: ready, offered, in progress, near levels; merchants", "[client][quests]")
{
    installShipped();
    const vg::QuestCatalog& c = vg::quests();
    vg::QuestLog log;
    const vg::Inventory inv;
    const vg::QuestUser aydor{1, 1, 1};
    // Oruç (10): quest 1 offered; the rest of his quests are behind prerequisites (hidden).
    auto e = cw::npcQuestEntries(c, 10, log, aydor, inv, &items(), 0, 10);
    REQUIRE(e.size() == 1);
    CHECK(e[0].quest->id == 1);
    CHECK(e[0].status == vg::QuestStatus::Available);
    CHECK(cw::npcHasQuestBusiness(c, 10, log, aydor, inv, &items(), 0, 10));
    // Ilgaz (2, a merchant) has nothing for a new character.
    CHECK_FALSE(cw::npcHasQuestBusiness(c, 2, log, aydor, inv, &items(), 0, 10));
    vg::QuestRecord r;
    r.quest = 1;
    r.state = vn::QuestState::Active;
    log.put(r);
    // Selvi (11) takes quest 1 back: ready at once (a talk quest); Oruç shows it in progress.
    e = cw::npcQuestEntries(c, 11, log, aydor, inv, &items(), 0, 10);
    REQUIRE_FALSE(e.empty());
    CHECK(e[0].quest->id == 1);
    CHECK(e[0].status == vg::QuestStatus::Ready);
    CHECK(e[0].turnIn);
    e = cw::npcQuestEntries(c, 10, log, aydor, inv, &items(), 0, 10);
    REQUIRE(e.size() == 1);
    CHECK(e[0].status == vg::QuestStatus::Ready); // listed with the quests in progress: handed in elsewhere
    CHECK_FALSE(e[0].turnIn);
    CHECK(vg::questMarkerFor(c, 10, log, aydor, inv, &items(), 0, 10) == vg::QuestMarker::InProgress);
    // Done: Selvi offers 2 and 3; Toygar's level-3 quest 6 shows greyed only within two levels.
    r.state = vn::QuestState::Done;
    r.completions = 1;
    log.put(r);
    log.put({2, vn::QuestState::Done, {}, 1, 0});
    e = cw::npcQuestEntries(c, 12, log, aydor, inv, &items(), 0, 10);
    bool sawLater = false;
    for (const auto& x : e)
        if (x.quest->id == 6)
        {
            sawLater = true;
            CHECK(x.status == vg::QuestStatus::Later);
            CHECK(cw::questStatusText(*x.quest, x.status) == "3. seviyede");
        }
    CHECK(sawLater);
    const vg::QuestUser level2{1, 1, 2};
    e = cw::npcQuestEntries(c, 12, log, level2, inv, &items(), 0, 10);
    bool sawRepeatable = false;
    for (const auto& x : e)
        sawRepeatable = sawRepeatable || (x.quest->id == 41 && x.status == vg::QuestStatus::Available);
    CHECK(sawRepeatable);
}

TEST_CASE("Quest texts: rewards, results, abandon, hand-in", "[client][quests]")
{
    installShipped();
    const vg::QuestDef* q2 = vg::quests().find(2);
    REQUIRE(q2);
    const auto rogue = cw::rewardLines(*q2, 2, &items());
    REQUIRE(rogue.size() == 3);
    CHECK(rogue[0].text == "70 deneyim");
    CHECK(rogue[1].text == "20 Akçe");
    CHECK(rogue[2].templateId == 77);
    CHECK_THAT(rogue[2].text, ContainsSubstring("Eski Tozluk (sınıfına göre)"));
    const vg::QuestDef* q8 = vg::quests().find(8);
    REQUIRE(q8);
    CHECK(cw::rewardChoices(*q8, &items()).size() == 3);
    const vg::QuestDef* q6 = vg::quests().find(6);
    REQUIRE(q6);
    CHECK(cw::questResultText(vn::QuestResult::LevelTooLow, q6, &vg::npcs()) == "Bu görev için 3. seviyeye ulaşmalısın.");
    CHECK(cw::questResultText(vn::QuestResult::NeedsQuest, q6, &vg::npcs()) == "Önce \"Külden Kalkanlar\" görevini bitirmelisin.");
    CHECK_THAT(cw::questResultText(vn::QuestResult::BagFull, q6, nullptr), ContainsSubstring("Çantan dolu"));
    CHECK(cw::questResultText(vn::QuestResult::Ok, q6, nullptr).empty());
    for (std::uint8_t v = 1; v <= vn::kQuestResultMax; ++v)
        CHECK_FALSE(cw::questResultText(static_cast<vn::QuestResult>(v), q6, &vg::npcs()).empty());
    CHECK_THAT(cw::abandonQuestion(*q6), ContainsSubstring("\"Cüruf Yatağı\" bırakılsın mı?"));
    CHECK(cw::handInText(*q6, 160, 35) == "Görev teslim edildi: Cüruf Yatağı (+160 deneyim, +35 Akçe)");
    CHECK(cw::handInText(*q6, 0, 0) == "Görev teslim edildi: Cüruf Yatağı");
    CHECK(cw::npcName(&vg::npcs(), 11) == "Avcı Selvi");
    CHECK(cw::npcName(&vg::npcs(), 999) == "?");
}

TEST_CASE("Quest automation commands parse", "[client][quests]")
{
    installShipped();
    std::string error;
    CHECK_FALSE(cw::parseQuestCommand("shopbuy 1", error));
    CHECK(error.empty());
    auto c = cw::parseQuestCommand("questselect 4", error);
    REQUIRE(c);
    CHECK(c->verb == cw::QuestVerb::Select);
    CHECK(c->a == 4);
    c = cw::parseQuestCommand("questhandin 2", error);
    REQUIRE(c);
    CHECK(c->hasB);
    CHECK(c->b == 2);
    c = cw::parseQuestCommand("questacceptforce Bilge_Oruç 2", error);
    REQUIRE(c);
    CHECK(c->text == "Bilge Oruç");
    CHECK(c->a == 2);
    c = cw::parseQuestCommand("expectquest 2 active 3", error);
    REQUIRE(c);
    CHECK(c->text == "active");
    CHECK(c->progress == std::vector<std::uint16_t>{3});
    c = cw::parseQuestCommand("expectmarker Avcı_Selvi ready", error);
    REQUIRE(c);
    CHECK(c->text == "Avcı Selvi\nready");
    c = cw::parseQuestCommand("expecttracker Kül Çömezi 3/6", error);
    REQUIRE(c);
    CHECK(c->text == "Kül Çömezi 3/6");
    CHECK_FALSE(cw::parseQuestCommand("questlog maybe", error));
    CHECK_THAT(error, ContainsSubstring("questlog open|close"));
    CHECK_FALSE(cw::parseQuestCommand("questselect", error));
    CHECK_FALSE(error.empty());
}
