// Town NPCs, shops and consumables in the zone (server/world/ZoneShop.cpp, docs/PROTOCOL_SHOP.md) on
// synthetic terrain with a fake clock and fake links: NPC entities through the AOI (type 4, never
// attackable), the startup spot checks, opening / closing shops (range, death, distance, another
// merchant), buy / sell checks handed to the session as grants, consumable checks, the shared
// cooldown (also across a leave), the restore after the commit and the vitals testing hook.

#include "server/world/Zone.h"
#include "shared/game/Items.h"
#include "shared/game/Npcs.h"
#include "tests/server/world/FakeLink.h"
#include "tests/server/world/TestTerrain.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <chrono>
#include <map>
#include <memory>
#include <string>

namespace vw = vo::server::world;
namespace va = vo::server::auth;
namespace vn = vo::net;
namespace vg = vo::game;
using namespace std::chrono_literals;
using Catch::Matchers::ContainsSubstring;
using vo::tests::FakeLink;
using SK = vw::ShopCommand::Kind;
using UK = vw::ItemUseCommand::Kind;
using SR = vn::ShopResult;
using UR = vn::ItemUseResult;

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

// A potion seller 20 m east of spawn_aydor (100, 100), a smith far away at (300, 300), and an NPC of
// zone 2 (ignored by zone 1).
std::shared_ptr<const vg::NpcCatalog> npcs(const std::string& extra = "")
{
    const std::string toml = R"([[npc]]
id = 1
name = "Sürmeli Bacı"
title = "Şerbetçi"
greeting = "Hoş geldin"
model = "npc_potion_test"
zone = 1
x = 120.0
z = 100.0
yaw = -1.5
kind = "merchant"
stock = [16, 17, 18, 19]

[[npc]]
id = 2
name = "Demirci Ilgaz"
title = "Demirci"
greeting = "Buyur"
model = "npc_smith_test"
zone = 1
x = 300.0
z = 300.0
yaw = 0.0
kind = "merchant"
stock = [1, 4]
upgrades = true

[[npc]]
id = 3
name = "Öteki"
title = "Başka bölge"
greeting = "-"
model = "npc_other_zone"
zone = 2
x = 50.0
z = 50.0
yaw = 0.0
kind = "merchant"
stock = [16]
)" + extra;
    auto c = std::make_shared<vg::NpcCatalog>();
    std::string error;
    INFO(error);
    REQUIRE(vg::parseNpcCatalog(toml, "npcs.toml", &items(), *c, error));
    return c;
}

struct Harness
{
    std::unique_ptr<vw::Zone> zone;
    std::map<vw::SessionKey, std::shared_ptr<FakeLink>> links;
    vo::core::MonoTime t0 = vo::core::monoNow();
    std::uint64_t tick = 0;
    vw::EntityId nextId = 1000;
    std::uint32_t seq = 0;

    explicit Harness(std::uint32_t vitalsPercent = 0)
    {
        vw::ZoneSettings s;
        s.savePeriodTicks = 0;
        s.seed = 7;
        s.spawnSpread = 0.0f;
        s.testingEnterVitalsPercent = vitalsPercent;
        std::string error;
        zone = vw::Zone::create(1, vo::tests::makeTestTerrain(), s, error);
        REQUIRE(zone);
        zone->beginTick(now(), ms());
        INFO(error);
        REQUIRE(zone->installNpcs(npcs(), items(), [this] { return nextId++; }, error));
    }

    [[nodiscard]] vo::core::MonoTime now() const { return t0 + tick * 50ms; }
    [[nodiscard]] std::uint32_t ms() const { return static_cast<std::uint32_t>(tick * 50); }

    vw::Zone::EnterParams params(vw::SessionKey s, float x, float z)
    {
        auto link = std::make_shared<FakeLink>();
        links[s] = link;
        vw::Zone::EnterParams p;
        p.entityId = static_cast<vw::EntityId>(s);
        p.session = s;
        p.generation = 1;
        p.link = link;
        p.account = static_cast<va::AccountId>(s);
        p.character.id = static_cast<va::CharacterId>(s * 10);
        p.character.name = "Player" + std::to_string(s);
        p.character.race = 1;
        p.character.charClass = 3;
        p.character.level = 5; // a mage with more than 100 hp
        p.character.position = {1, x, 0.0f, z, 0.0f};
        return p;
    }
    std::shared_ptr<FakeLink> enter(vw::SessionKey s, float x, float z)
    {
        auto p = params(s, x, z);
        auto link = links[s];
        zone->beginTick(now(), ms());
        zone->enter(std::move(p), now(), ms());
        return link;
    }
    void shop(vw::SessionKey s, SK kind, vw::EntityId npc, std::uint16_t tmpl = 0, std::uint16_t qty = 1,
              std::uint8_t slot = 0)
    {
        zone->beginTick(now(), ms());
        vw::ShopCommand c;
        c.session = s;
        c.kind = kind;
        c.npc = npc;
        c.seq = ++seq;
        c.templateId = tmpl;
        c.quantity = qty;
        c.bagSlot = slot;
        zone->shop(c);
    }
    void use(vw::SessionKey s, std::uint16_t tmpl, UK kind = UK::Use)
    {
        zone->beginTick(now(), ms());
        const vg::ItemTemplate* t = items().find(tmpl);
        REQUIRE(t);
        zone->useItem(vw::ItemUseCommand{s, kind, ++seq, tmpl, vn::kItemUseAnyStack, t->restoreHp, t->restoreMp});
    }
    void step(int n = 1)
    {
        for (int i = 0; i < n; ++i)
        {
            zone->beginTick(now(), ms());
            zone->update(now(), tick, nullptr);
            zone->flush();
            ++tick;
        }
    }
    void stepFor(std::chrono::milliseconds d) { step(static_cast<int>(d / 50ms)); }

    [[nodiscard]] vw::EntityId npcId(std::uint16_t def) const
    {
        vw::EntityId id = 0;
        zone->forEachNpc([&](const vw::Zone::Npc& n) {
            if (n.def->id == def)
                id = n.id;
        });
        return id;
    }
    [[nodiscard]] const vw::Zone::PlayerCombat& combat(vw::SessionKey s) const { return zone->findPlayer(s)->combat; }

    // N in known(P) <=> P in observers(N) <=> in view <=> the client has it spawned.
    void checkInvariants() const
    {
        zone->forEachNpc([&](const vw::Zone::Npc& n) {
            zone->forEachPlayer([&](const vw::Zone::Player& p) {
                const bool known = p.known.contains(n.id);
                INFO("npc " << n.id << " player " << p.id);
                CHECK(known == n.observers.contains(p.id));
                CHECK(known == zone->grid().inView(p.cell, n.cell));
                CHECK(links.at(p.session)->visible().contains(n.id) == known);
                if (p.shopNpc == n.id)
                    CHECK(known);
            });
        });
        for (const auto& [s, link] : links)
            if (zone->findPlayer(s))
                CHECK(link->errors().empty());
    }
};

template <class T>
std::vector<T> since(const FakeLink& link, std::size_t& seen)
{
    const auto all = link.all<T>();
    std::vector<T> out(all.begin() + static_cast<std::ptrdiff_t>(std::min(seen, all.size())), all.end());
    seen = all.size();
    return out;
}

} // namespace

TEST_CASE("Shop zone: NPCs are entity type 4 in the area of interest and never attackable", "[server][world][shop]")
{
    Harness h;
    CHECK(h.zone->npcCount() == 2); // the zone 2 NPC is not here
    const vw::EntityId potion = h.npcId(1), smith = h.npcId(2);
    REQUIRE(potion != 0);
    REQUIRE(smith != 0);
    CHECK(h.zone->hasEntity(potion));
    auto a = h.enter(1, 110.0f, 100.0f);
    h.step();
    std::vector<vn::EntitySpawn> npcSpawns;
    for (const auto& s : a->all<vn::EntitySpawn>())
        if (s.entityType == vn::kEntityTypeNpc)
            npcSpawns.push_back(s);
    REQUIRE(npcSpawns.size() == 1); // the smith is out of view
    const vn::EntitySpawn& s = npcSpawns.front();
    CHECK(s.entityId == potion);
    CHECK(s.templateId == 1);
    CHECK(s.name == "Sürmeli Bacı");
    CHECK(s.x == 120.0f);
    CHECK(s.z == 100.0f);
    CHECK(std::abs(s.yaw + 1.5f) < 1e-5f);
    CHECK(s.level == 0);
    CHECK(s.hp == 0);
    CHECK(s.maxHp == 0);
    CHECK(s.state == 0);
    h.checkInvariants();

    // Out of view and back: despawn, spawn again (the same entity).
    h.zone->beginTick(h.now(), h.ms());
    h.zone->placeEntity(1, 300.0f, 295.0f);
    h.step();
    CHECK_FALSE(a->visible().contains(potion));
    CHECK(a->visible().contains(smith));
    h.checkInvariants();
    h.zone->placeEntity(1, 110.0f, 100.0f);
    h.step();
    CHECK(a->visible().contains(potion));
    CHECK_FALSE(a->visible().contains(smith));
    h.checkInvariants();
    // A second player sees it on entry; leaving removes the observer.
    auto b = h.enter(2, 112.0f, 100.0f);
    h.step();
    CHECK(b->visible().contains(potion));
    CHECK(h.zone->findNpc(potion)->observers.size() == 2);
    REQUIRE(h.zone->leave(2));
    CHECK(h.zone->findNpc(potion)->observers.size() == 1);

    // Never moves: no ENTITY_MOVE about it however long the zone runs.
    h.stepFor(2s);
    for (const auto& m : a->all<vn::EntityMove>())
        CHECK(m.entityId != potion);
    // Attacks / enemy skills on it are refused; it is no friendly target either.
    h.zone->beginTick(h.now(), h.ms());
    h.zone->combat(vw::CombatCommand{1, vw::CombatCommand::Kind::Attack, potion, 0, 0});
    h.zone->combat(vw::CombatCommand{1, vw::CombatCommand::Kind::Skill, potion, vg::kSkillEmberBolt, 0});
    h.step();
    const auto fails = a->all<vn::ActionFail>();
    REQUIRE(fails.size() == 2);
    CHECK(fails[0].reason == vn::ActionFailReason::NotAttackable);
    CHECK(fails[1].reason == vn::ActionFailReason::NotAttackable);
    CHECK(h.zone->findNpc(potion)->observers.size() == 1);
}

TEST_CASE("Shop zone: installNpcs refuses unusable spots", "[server][world][shop]")
{
    const auto install = [](const std::string& extra, vo::assets::TerrainData t, std::string& error) {
        vw::ZoneSettings s;
        vw::ZoneLayout layout;
        t.spawns.push_back({"gate_test", {200.0f, 0.0f, 200.0f}, 0.0f});
        t.spawns.push_back({"arrive_test", {150.0f, 0.0f, 150.0f}, 0.0f});
        layout.gates.push_back({{"gate_test", 2.5f, 2, "arrive_x"}, vg::kAllRaces});
        auto zone = vw::Zone::create(1, std::move(t), s, error, layout);
        REQUIRE(zone);
        vw::EntityId next = 1;
        return zone->installNpcs(npcs(extra), items(), [&] { return next++; }, error);
    };
    const auto npcAt = [](float x, float z) {
        return std::format("\n[[npc]]\nid = 9\nname = \"N\"\ntitle = \"T\"\ngreeting = \"G\"\nmodel = \"npc_9\"\nzone = 1\n"
                           "x = {}\nz = {}\nyaw = 0.0\nkind = \"merchant\"\nstock = [16]\n",
                           x, z);
    };
    std::string error;
    CHECK(install("", vo::tests::makeTestTerrain(), error));
    CHECK_FALSE(install(npcAt(102.0f, 101.0f), vo::tests::makeTestTerrain(), error));
    CHECK_THAT(error, ContainsSubstring("NPC 9 (N) in zone 1"));
    CHECK_THAT(error, ContainsSubstring("marker 'spawn_aydor'"));
    CHECK_FALSE(install(npcAt(151.0f, 152.0f), vo::tests::makeTestTerrain(), error));
    CHECK_THAT(error, ContainsSubstring("marker 'arrive_test'"));
    CHECK_FALSE(install(npcAt(203.0f, 202.0f), vo::tests::makeTestTerrain(), error));
    CHECK_THAT(error, ContainsSubstring("gate 'gate_test'"));
    CHECK(install(npcAt(205.0f, 200.0f), vo::tests::makeTestTerrain(), error)); // radius 2.5 + 2 m = 4.5 m
    // fix1: a merchant may stand on a blocked cell (behind its counter), but not where no customer fits
    // within talk range.
    auto blocked = vo::tests::makeTestTerrain();
    vo::tests::blockCell(blocked, 250.5f, 250.5f);
    CHECK(install(npcAt(250.5f, 250.5f), blocked, error));
    for (int dz = -4; dz <= 4; ++dz)
        for (int dx = -4; dx <= 4; ++dx)
            vo::tests::blockCell(blocked, 250.5f + static_cast<float>(dx), 250.5f + static_cast<float>(dz));
    CHECK_FALSE(install(npcAt(250.5f, 250.5f), blocked, error));
    CHECK_THAT(error, ContainsSubstring("no room for a customer"));
}

TEST_CASE("Shop zone: open, buy and sell checks hand grants to the session", "[server][world][shop]")
{
    Harness h;
    const vw::EntityId potion = h.npcId(1), smith = h.npcId(2);
    auto a = h.enter(1, 110.0f, 100.0f); // 10 m from the potion seller
    h.step();
    std::size_t opens = 0, buys = 0, sells = 0, closes = 0;
    h.shop(1, SK::Open, potion);
    h.shop(1, SK::Open, smith); // not spawned to the player
    h.shop(1, SK::Open, 1);     // the player itself
    h.shop(1, SK::Buy, potion, 16, 1);
    h.shop(1, SK::Sell, potion, 0, 1, 0);
    h.step();
    auto o = since<vn::ShopOpenRes>(*a, opens);
    REQUIRE(o.size() == 3);
    CHECK(o[0].result == SR::OutOfRange);
    CHECK(o[0].entries.empty());
    CHECK(o[1].result == SR::NotFound);
    CHECK(o[2].result == SR::NotFound);
    CHECK(since<vn::ShopBuyRes>(*a, buys).at(0).result == SR::NotOpen);
    CHECK(since<vn::ShopSellRes>(*a, sells).at(0).result == SR::NotOpen);
    CHECK(a->shopGrants().empty());

    // Within 4 m (+0.5): open, the stock with the items' values.
    h.zone->beginTick(h.now(), h.ms());
    h.zone->placeEntity(1, 115.6f, 100.0f); // 4.4 m
    h.shop(1, SK::Open, potion);
    h.step();
    o = since<vn::ShopOpenRes>(*a, opens);
    REQUIRE(o.size() == 1);
    REQUIRE(o[0].result == SR::Ok);
    CHECK(o[0].entries == std::vector<vn::ShopEntry>{{16, 8}, {17, 25}, {18, 8}, {19, 25}});
    CHECK(h.zone->findPlayer(1)->shopNpc == potion);
    h.checkInvariants();

    // Buy: in stock -> a grant with the price; not in stock -> refused here. Sell -> a grant.
    h.shop(1, SK::Buy, potion, 17, 3);
    h.shop(1, SK::Buy, potion, 4, 1);
    h.shop(1, SK::Sell, potion, 0, 2, 5);
    h.step();
    const auto g = a->shopGrants();
    REQUIRE(g.size() == 2);
    CHECK(g[0].kind == vw::ShopGrant::Kind::Buy);
    CHECK(g[0].templateId == 17);
    CHECK(g[0].quantity == 3);
    CHECK(g[0].unitPrice == 25);
    CHECK(g[0].npc == potion);
    CHECK(g[1].kind == vw::ShopGrant::Kind::Sell);
    CHECK(g[1].bagSlot == 5);
    CHECK(g[1].quantity == 2);
    auto b = since<vn::ShopBuyRes>(*a, buys);
    REQUIRE(b.size() == 1);
    CHECK(b[0].result == SR::NotInStock);
    CHECK(b[0].templateId == 4);
    CHECK(h.zone->counters().shopOpens == 1);
    CHECK(h.zone->counters().shopGrants == 2);

    // Walking out of 4.5 m: buying is OutOfRange, the shop stays open up to 8 m.
    h.zone->beginTick(h.now(), h.ms());
    h.zone->placeEntity(1, 113.0f, 100.0f); // 7 m
    h.shop(1, SK::Buy, potion, 16, 1);
    h.step();
    CHECK(since<vn::ShopBuyRes>(*a, buys).at(0).result == SR::OutOfRange);
    CHECK(a->count<vn::ShopClosed>() == 0);
    h.zone->beginTick(h.now(), h.ms());
    h.zone->placeEntity(1, 111.5f, 100.0f); // 8.5 m: closed
    h.step();
    auto c = since<vn::ShopClosed>(*a, closes);
    REQUIRE(c.size() == 1);
    CHECK(c[0].npcId == potion);
    CHECK(c[0].reason == vn::ShopClosedReason::OutOfRange);
    h.shop(1, SK::Buy, potion, 16, 1);
    h.step();
    CHECK(since<vn::ShopBuyRes>(*a, buys).at(0).result == SR::NotOpen);

    // SHOP_CLOSE_REQ closes without an answer; a death closes with YouDied.
    h.zone->beginTick(h.now(), h.ms());
    h.zone->placeEntity(1, 117.0f, 100.0f);
    h.shop(1, SK::Open, potion);
    h.shop(1, SK::Close, potion);
    h.shop(1, SK::Sell, potion, 0, 1, 0);
    h.step();
    CHECK(since<vn::ShopOpenRes>(*a, opens).at(0).result == SR::Ok);
    CHECK(since<vn::ShopSellRes>(*a, sells).at(0).result == SR::NotOpen);
    CHECK(since<vn::ShopClosed>(*a, closes).empty());
    h.shop(1, SK::Open, potion);
    h.step();
    h.zone->beginTick(h.now(), h.ms());
    h.zone->killPlayerForTesting(1);
    h.step();
    c = since<vn::ShopClosed>(*a, closes);
    REQUIRE(c.size() == 1);
    CHECK(c[0].reason == vn::ShopClosedReason::YouDied);
    h.shop(1, SK::Open, potion);
    h.step();
    CHECK(since<vn::ShopOpenRes>(*a, opens).back().result == SR::YouAreDead);
    CHECK(h.zone->findPlayer(1)->shopNpc == 0);
    h.checkInvariants();
}

TEST_CASE("Shop zone: opening another merchant closes the first silently; leaving ends a shop", "[server][world][shop]")
{
    Harness h;
    const vw::EntityId potion = h.npcId(1), smith = h.npcId(2);
    auto a = h.enter(1, 118.0f, 100.0f);
    h.step();
    h.shop(1, SK::Open, potion);
    h.step();
    h.zone->beginTick(h.now(), h.ms());
    h.zone->placeEntity(1, 298.0f, 300.0f);
    h.step(); // the potion seller is out of view now: closed (OutOfRange) and despawned
    CHECK(a->count<vn::ShopClosed>() == 1);
    h.shop(1, SK::Open, smith);
    h.step();
    CHECK(h.zone->findPlayer(1)->shopNpc == smith);
    const auto opened = a->all<vn::ShopOpenRes>();
    CHECK(opened.back().entries == std::vector<vn::ShopEntry>{{1, 12}, {4, 12}});
    REQUIRE(h.zone->leave(1));
    CHECK(h.zone->findNpc(smith)->observers.empty());
}

TEST_CASE("Shop zone: consumables check, start the shared cooldown and restore after the commit",
          "[server][world][shop]")
{
    Harness h;
    auto a = h.enter(1, 110.0f, 100.0f);
    auto b = h.enter(2, 112.0f, 100.0f); // watches
    h.step();
    const std::uint32_t maxHp = h.combat(1).stats.maxHp;
    const std::uint32_t maxMp = h.combat(1).stats.maxMp;
    REQUIRE(maxHp > 100);

    // Full hp: AlreadyFull, no cooldown.
    h.use(1, 16);
    h.step();
    auto res = a->all<vn::ItemUseRes>();
    REQUIRE(res.size() == 1);
    CHECK(res[0].result == UR::AlreadyFull);
    CHECK(a->count<vn::ItemCooldown>() == 0);
    CHECK(a->useGrants().empty());

    // Hurt: accepted -> ITEM_COOLDOWN(10 s) and a grant; nothing restored yet.
    h.zone->beginTick(h.now(), h.ms());
    h.zone->setVitalsForTesting(1, 20, 5);
    h.use(1, 16);
    h.use(1, 18); // the cooldown is shared
    h.step();
    const auto cd = a->all<vn::ItemCooldown>();
    REQUIRE(cd.size() == 1);
    CHECK(cd[0].remainingMs == vg::kConsumableCooldownMs);
    CHECK(cd[0].totalMs == vg::kConsumableCooldownMs);
    REQUIRE(a->useGrants().size() == 1);
    CHECK(a->useGrants()[0].templateId == 16);
    CHECK(a->useGrants()[0].restoreHp == 70);
    res = a->all<vn::ItemUseRes>();
    REQUIRE(res.size() == 2);
    CHECK(res[1].result == UR::OnCooldown);
    CHECK(res[1].templateId == 18);
    CHECK(h.combat(1).hp <= 21); // regeneration at most
    CHECK(a->count<vn::ItemUsed>() == 0);
    CHECK(h.zone->counters().itemUses == 1);

    // The session committed: Apply restores, ITEM_USED to the drinker and its observers, SELF_STATS.
    const std::uint32_t hpBefore = h.combat(1).hp;
    h.use(1, 16, UK::Apply);
    h.step();
    CHECK(h.combat(1).hp == hpBefore + 70);
    auto used = a->all<vn::ItemUsed>();
    REQUIRE(used.size() == 1);
    CHECK(used[0].entityId == 1);
    CHECK(used[0].templateId == 16);
    CHECK(used[0].hpRestored == 70);
    CHECK(used[0].mpRestored == 0);
    CHECK(b->count<vn::ItemUsed>() == 1);
    h.step(6); // SELF_STATS at most every 250 ms
    CHECK(a->all<vn::SelfStats>().back().hp >= hpBefore + 70);
    CHECK(h.zone->counters().itemRestores == 1);

    // Clamped to the maximum.
    h.zone->beginTick(h.now(), h.ms());
    h.zone->setVitalsForTesting(1, maxHp - 10, maxMp);
    h.use(1, 16, UK::Apply);
    h.step();
    CHECK(h.combat(1).hp == maxHp);
    CHECK(a->all<vn::ItemUsed>().back().hpRestored == 10);

    // After 10 s the cooldown is over: the mana potion works.
    h.stepFor(10s);
    h.zone->beginTick(h.now(), h.ms());
    h.zone->setVitalsForTesting(1, maxHp, 1);
    h.use(1, 18);
    h.step();
    CHECK(a->useGrants().size() == 2);
    CHECK(a->count<vn::ItemCooldown>() == 2);

    // Stunned (no shipped source stuns players, the check exists) and dead.
    h.stepFor(10s);
    h.zone->beginTick(h.now(), h.ms());
    h.zone->setVitalsForTesting(1, 10, 1);
    h.zone->addPlayerEffectForTesting(1, vg::EffectKind::Stun, 5000);
    h.use(1, 16);
    h.step();
    CHECK(a->all<vn::ItemUseRes>().back().result == UR::Stunned);
    h.zone->beginTick(h.now(), h.ms());
    h.zone->killPlayerForTesting(1);
    h.use(1, 16);
    h.step();
    CHECK(a->all<vn::ItemUseRes>().back().result == UR::YouAreDead);
    // An Apply for a dead player restores nothing (the unit was consumed: PROTOCOL_SHOP.md §2.3).
    const std::size_t usedNow = a->count<vn::ItemUsed>();
    h.use(1, 16, UK::Apply);
    h.step();
    CHECK(h.combat(1).hp == 0);
    CHECK(a->count<vn::ItemUsed>() == usedNow);
}

TEST_CASE("Shop zone: the consumable cooldown is remembered across a leave and reported on entry",
          "[server][world][shop]")
{
    Harness h;
    auto a = h.enter(1, 110.0f, 100.0f);
    h.step();
    h.zone->beginTick(h.now(), h.ms());
    h.zone->setVitalsForTesting(1, 10, 10);
    h.use(1, 16);
    h.step();
    REQUIRE(a->useGrants().size() == 1);
    h.stepFor(3s);
    auto left = h.zone->leave(1);
    REQUIRE(left);
    bool remembered = false;
    for (const auto& rc : left->cooldowns)
        if (rc.skill == 0)
        {
            remembered = true;
            CHECK(rc.totalMs == vg::kConsumableCooldownMs);
        }
    REQUIRE(remembered);
    auto p = h.params(1, 110.0f, 100.0f);
    p.cooldowns = left->cooldowns;
    auto again = h.links[1];
    h.zone->beginTick(h.now(), h.ms());
    h.zone->enter(std::move(p), h.now(), h.ms());
    h.step();
    const auto cd = again->all<vn::ItemCooldown>();
    REQUIRE(cd.size() == 1);
    CHECK(cd[0].remainingMs <= vg::kConsumableCooldownMs - 2900);
    CHECK(cd[0].remainingMs >= vg::kConsumableCooldownMs - 3100);
    // ITEM_COOLDOWN follows CHAR_STATS (PROTOCOL_SHOP.md §3.4).
    const auto packets = again->packets();
    std::size_t stats = 0, cool = 0;
    for (std::size_t i = 0; i < packets.size(); ++i)
    {
        if (packets[i].opcode == static_cast<std::uint16_t>(vn::Opcode::CharStats) && stats == 0)
            stats = i + 1;
        if (packets[i].opcode == static_cast<std::uint16_t>(vn::Opcode::ItemCooldown))
            cool = i + 1;
    }
    CHECK(stats != 0);
    CHECK(cool > stats);
    h.zone->beginTick(h.now(), h.ms());
    h.zone->setVitalsForTesting(1, 10, 10);
    h.use(1, 16);
    h.step();
    CHECK(again->all<vn::ItemUseRes>().back().result == UR::OnCooldown);
}

TEST_CASE("Shop zone: the vitals testing hook sets hp and mp on a world entry", "[server][world][shop]")
{
    Harness h(10);
    auto a = h.enter(1, 110.0f, 100.0f);
    h.step();
    const auto e = a->entered();
    REQUIRE(e);
    CHECK(e->hp == std::max<std::uint32_t>(1, e->maxHp / 10));
    CHECK(e->mp == e->maxMp / 10);
    Harness off;
    auto b = off.enter(1, 110.0f, 100.0f);
    off.step();
    CHECK(b->entered()->hp == b->entered()->maxHp);
}

TEST_CASE("Shop zone: upgrade requests need the open shop of a smith, life and range", "[server][world][shop][upgrade]")
{
    // M16 (docs/PROTOCOL_UPGRADE.md §2.2 checks 1-4): the world's part; an accepted attempt is a grant.
    Harness h;
    const vw::EntityId potion = h.npcId(1), smith = h.npcId(2);
    auto a = h.enter(1, 110.0f, 100.0f);
    h.step();
    std::size_t answers = 0;
    const auto upgrade = [&](vw::EntityId npc) {
        h.zone->beginTick(h.now(), h.ms());
        vw::ShopCommand c;
        c.session = 1;
        c.kind = SK::Upgrade;
        c.npc = npc;
        c.seq = ++h.seq;
        c.bagSlot = 4;
        c.itemId = 77;
        c.fromLevel = 3;
        c.stoneSlot = 9;
        h.zone->shop(c);
        h.step();
    };
    using UR2 = vn::UpgradeResult;
    upgrade(smith); // nothing open
    CHECK(since<vn::UpgradeRes>(*a, answers).at(0).result == UR2::NotOpen);
    h.zone->beginTick(h.now(), h.ms());
    h.zone->placeEntity(1, 117.0f, 100.0f);
    h.shop(1, SK::Open, potion);
    h.step();
    upgrade(potion); // a merchant that does not upgrade
    auto r = since<vn::UpgradeRes>(*a, answers);
    REQUIRE(r.size() == 1);
    CHECK(r[0].result == UR2::NotSmith);
    CHECK(r[0].bagSlot == 4);
    upgrade(smith); // the open shop is another one
    CHECK(since<vn::UpgradeRes>(*a, answers).at(0).result == UR2::NotOpen);
    CHECK(a->shopGrants().empty());

    // At the smith: a grant carrying the item, its level and the stone slot.
    h.zone->beginTick(h.now(), h.ms());
    h.zone->placeEntity(1, 297.0f, 300.0f);
    h.step();
    h.shop(1, SK::Open, smith);
    h.step();
    REQUIRE(h.zone->findPlayer(1)->shopNpc == smith);
    upgrade(smith);
    CHECK(since<vn::UpgradeRes>(*a, answers).empty());
    auto g = a->shopGrants();
    REQUIRE(g.size() == 1);
    CHECK(g[0].kind == vw::ShopGrant::Kind::Upgrade);
    CHECK(g[0].npc == smith);
    CHECK(g[0].bagSlot == 4);
    CHECK(g[0].itemId == 77);
    CHECK(g[0].fromLevel == 3);
    CHECK(g[0].stoneSlot == 9);
    CHECK(h.zone->counters().upgradeGrants == 1);
    // Out of talk range (the shop stays open up to 8 m): OutOfRange.
    h.zone->beginTick(h.now(), h.ms());
    h.zone->placeEntity(1, 293.0f, 300.0f); // 7 m
    h.step();
    upgrade(smith);
    CHECK(since<vn::UpgradeRes>(*a, answers).at(0).result == UR2::OutOfRange);
    // Dead (before the world closes the shop): YouAreDead.
    h.zone->beginTick(h.now(), h.ms());
    h.zone->placeEntity(1, 297.0f, 300.0f);
    h.zone->killPlayerForTesting(1);
    vw::ShopCommand c;
    c.session = 1;
    c.kind = SK::Upgrade;
    c.npc = smith;
    c.seq = ++h.seq;
    h.zone->shop(c);
    h.step();
    CHECK(since<vn::UpgradeRes>(*a, answers).at(0).result == UR2::YouAreDead);
    CHECK(a->shopGrants().size() == 1);
}