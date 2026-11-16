// Loot containers in the zone (server/world/ZoneLoot.cpp, docs/PROTOCOL_LOOT.md) on synthetic
// terrain with a fake clock and fake links: drops on a kill (entity type 3, owner = top damage),
// AOI spawn / despawn with per-recipient lock bits, the 30 s owner window, range / dead / unknown
// checks, open / contents / viewers / auto-close, take one / coins / everything as reservations
// handed to the session, settles with leftovers, two looters racing for one entry, cancellation on
// leave (and keeping writes in flight), the in-flight cap, expiry after 90 s and despawn when empty.

#include "server/world/Zone.h"
#include "shared/game/Combat.h"
#include "shared/game/Items.h"
#include "shared/game/Loot.h"
#include "shared/game/Monsters.h"
#include "tests/server/world/FakeLink.h"
#include "tests/server/world/TestTerrain.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <format>
#include <map>
#include <memory>
#include <set>
#include <string>

namespace vw = vo::server::world;
namespace va = vo::server::auth;
namespace vn = vo::net;
namespace vg = vo::game;
using namespace std::chrono_literals;
using vo::tests::FakeLink;
using K = vw::CombatCommand::Kind;
using LK = vw::LootCommand::Kind;
using LR = vn::LootResult;

namespace {

std::shared_ptr<const vg::ItemCatalog> catalog()
{
    static const std::shared_ptr<const vg::ItemCatalog> c = [] {
        auto cat = std::make_shared<vg::ItemCatalog>();
        const auto path = vg::locateItemsFile();
        std::string error;
        if (!path || !vg::loadItemCatalog(*path, *cat, error))
            cat.reset();
        return std::shared_ptr<const vg::ItemCatalog>(cat);
    }();
    REQUIRE(c);
    return c;
}

// Monster 1: a weak thrall that never hits back (attack 0); monster 2: a dummy without a loot table;
// monster 5: an elite that kills a 1-hp player at once.
std::shared_ptr<const vg::MonsterData> monsters(const std::string& spawns)
{
    const std::string toml = std::format(R"(
[[monster]]
id = 1
name = "Kül Çömezi"
model = "ash_thrall"
level = 1
hp = 40
attack = 0
defense = 0
attack_range = 2.0
attack_interval_ms = 1500
move_speed = 4.0
wander_speed = 1.3
aggro_radius = 0.0
leash_radius = 30.0
xp = 25
respawn_ms = 60000

[[monster]]
id = 2
name = "Talim Kuklası"
model = "ash_thrall"
level = 1
hp = 20
attack = 0
defense = 0
attack_range = 2.0
attack_interval_ms = 1500
move_speed = 4.0
wander_speed = 1.0
aggro_radius = 0.0
leash_radius = 30.0
xp = 1
respawn_ms = 60000

[[monster]]
id = 5
name = "Kor Bekçisi"
model = "ember_warden"
level = 6
hp = 900
attack = 60
defense = 14
attack_range = 2.6
attack_interval_ms = 1000
move_speed = 4.0
wander_speed = 1.0
aggro_radius = 0.0
leash_radius = 30.0
xp = 300
respawn_ms = 90000
{}
)",
                                         spawns);
    auto data = std::make_shared<vg::MonsterData>();
    std::string error;
    INFO(error);
    REQUIRE(vg::parseMonsterData(toml, "test.toml", *data, error));
    return data;
}

std::string spawnAt(int monster, float x, float z)
{
    return std::format("\n[[spawn]]\nzone = 1\nmonster = {}\nx = {}\nz = {}\nradius = 0.0\n", monster, x, z);
}

// Thralls always drop 7 coins, 2 Kül Tozu and a Çömez Bıçağı.
std::shared_ptr<const vg::LootTables> lootTables(const vg::MonsterData& m)
{
    auto t = std::make_shared<vg::LootTables>();
    std::string error;
    INFO(error);
    REQUIRE(vg::parseLootTables(R"(
[[table]]
monsters = [1]
coins = [7, 7]
[[table.drop]]
item = 9
chance = 1.0
count = [2, 2]
[[table.drop]]
item = 12
chance = 1.0
)",
                                "loot.toml", *catalog(), &m, *t, error));
    return t;
}

vg::LootRoll roll(std::uint32_t coins, std::vector<vg::LootItem> items)
{
    vg::LootRoll r;
    r.coins = coins;
    r.items = std::move(items);
    return r;
}

struct Harness
{
    std::unique_ptr<vw::Zone> zone;
    std::map<vw::SessionKey, std::shared_ptr<FakeLink>> links;
    vo::core::MonoTime t0 = vo::core::monoNow();
    std::uint64_t tick = 0;
    vw::EntityId nextId = 1000;
    std::uint32_t seq = 0;

    explicit Harness(const std::string& spawns = "")
    {
        vw::ZoneSettings s;
        s.savePeriodTicks = 0;
        s.seed = 99;
        s.spawnSpread = 0.0f;
        std::string error;
        zone = vw::Zone::create(1, vo::tests::makeTestTerrain(), s, error);
        REQUIRE(zone);
        zone->beginTick(now(), ms());
        auto data = monsters(spawns);
        auto tables = lootTables(*data);
        REQUIRE(zone->installMonsters(data, [this] { return nextId++; }, now(), error));
        zone->installLoot(tables, [this] { return nextId++; });
    }

    [[nodiscard]] vo::core::MonoTime now() const { return t0 + tick * 50ms; }
    [[nodiscard]] std::uint32_t ms() const { return static_cast<std::uint32_t>(tick * 50); }

    std::shared_ptr<FakeLink> enter(vw::SessionKey s, float x, float z, vg::EquipBonus equip = {6, 4})
    {
        auto link = std::make_shared<FakeLink>();
        links[s] = link;
        vw::Zone::EnterParams p;
        p.entityId = static_cast<vw::EntityId>(s);
        p.session = s;
        p.generation = 1;
        p.link = link;
        p.account = static_cast<va::AccountId>(s);
        p.character.id = static_cast<va::CharacterId>(s * 10); // character ids differ from entity ids
        p.character.name = "Player" + std::to_string(s);
        p.character.race = 1;
        p.character.charClass = 1;
        p.character.level = 1;
        p.character.position = {1, x, 0.0f, z, 0.0f};
        p.equip = equip;
        zone->beginTick(now(), ms());
        zone->enter(std::move(p), now(), ms());
        return link;
    }

    void combat(vw::SessionKey s, K kind, std::uint32_t target = 0)
    {
        zone->beginTick(now(), ms());
        zone->combat(vw::CombatCommand{s, kind, target, 0, 0});
    }
    void open(vw::SessionKey s, vw::EntityId loot)
    {
        zone->beginTick(now(), ms());
        zone->loot(vw::LootCommand{s, LK::Open, loot, 0, 0});
    }
    void close(vw::SessionKey s, vw::EntityId loot)
    {
        zone->beginTick(now(), ms());
        zone->loot(vw::LootCommand{s, LK::Close, loot, 0, 0});
    }
    std::uint32_t take(vw::SessionKey s, vw::EntityId loot, std::uint8_t what)
    {
        zone->beginTick(now(), ms());
        zone->loot(vw::LootCommand{s, LK::Take, loot, ++seq, what});
        return seq;
    }
    void settle(const vw::LootGrant& g, std::uint32_t coinsBack, std::vector<vw::LootGrant::Item> itemsBack)
    {
        zone->beginTick(now(), ms());
        CHECK(zone->settleLoot(vw::LootSettleCommand{g.reservation, coinsBack, std::move(itemsBack)}));
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

    [[nodiscard]] std::vector<vw::EntityId> lootIds() const
    {
        std::vector<vw::EntityId> ids;
        zone->forEachLoot([&](const vw::Zone::Loot& l) { ids.push_back(l.id); });
        std::sort(ids.begin(), ids.end());
        return ids;
    }
    [[nodiscard]] std::vector<vw::EntityId> monsterIds() const
    {
        std::vector<vw::EntityId> ids;
        zone->forEachMonster([&](const vw::Zone::Monster& m) { ids.push_back(m.id); });
        std::sort(ids.begin(), ids.end());
        return ids;
    }

    // L in known(P) <=> P in observers(L) <=> in view; viewers are observers that point at it.
    void checkInvariants() const
    {
        zone->forEachLoot([&](const vw::Zone::Loot& l) {
            zone->forEachPlayer([&](const vw::Zone::Player& p) {
                const bool known = p.known.contains(l.id);
                INFO("loot " << l.id << " player " << p.id);
                CHECK(known == l.observers.contains(p.id));
                CHECK(known == zone->grid().inView(p.cell, l.cell));
                CHECK(links.at(p.session)->visible().contains(l.id) == known);
                CHECK(l.viewers.contains(p.id) == (p.viewingLoot == l.id));
                if (l.viewers.contains(p.id))
                    CHECK(known);
            });
        });
        for (const auto& [s, link] : links)
            if (zone->findPlayer(s))
                CHECK(link->errors().empty());
    }
};

template <class T, class P>
std::vector<T> filter(const FakeLink& link, P&& pred)
{
    std::vector<T> out;
    for (const auto& m : link.all<T>())
        if (pred(m))
            out.push_back(m);
    return out;
}

std::vector<vn::LootTakeRes> takeResults(const FakeLink& link)
{
    return link.all<vn::LootTakeRes>();
}

std::optional<vn::LootContents> lastContents(const FakeLink& link)
{
    const auto all = link.all<vn::LootContents>();
    if (all.empty())
        return std::nullopt;
    return all.back();
}

} // namespace

TEST_CASE("Loot zone: a kill drops a type 3 container at the corpse, owned by the top damage dealer",
          "[server][world][loot]")
{
    Harness h(spawnAt(1, 140.0f, 100.0f) + spawnAt(2, 160.0f, 100.0f));
    const auto ids = h.monsterIds();
    const vw::EntityId thrall = ids[0], dummy = ids[1];
    // B starts first but hits weakly; A deals most of the damage.
    auto a = h.enter(1, 138.0f, 100.0f, {40, 4});
    auto b = h.enter(2, 141.5f, 101.5f, {0, 4});
    auto far = h.enter(3, 300.0f, 300.0f);
    h.step();
    h.combat(2, K::Attack, thrall);
    h.step(2);
    h.combat(1, K::Attack, thrall);
    for (int i = 0; i < 400 && h.zone->findMonster(thrall)->state != vw::Zone::Monster::State::Dead; ++i)
        h.step();
    REQUIRE(h.zone->findMonster(thrall)->state == vw::Zone::Monster::State::Dead);
    h.step();
    const auto loots = h.lootIds();
    REQUIRE(loots.size() == 1);
    const vw::Zone::Loot& l = *h.zone->findLoot(loots[0]);
    const vw::Zone::Monster& m = *h.zone->findMonster(thrall);
    CHECK(l.owner == 10); // A's character
    CHECK(l.x == m.x);
    CHECK(l.z == m.z);
    CHECK(l.coins == 7);
    REQUIRE(l.entryCount == 2);
    CHECK(l.entries[0].templateId == 9);
    CHECK(l.entries[0].quantity == 2);
    CHECK(l.entries[1].templateId == 12);
    CHECK(l.entries[1].quantity == 1);
    CHECK(h.zone->counters().lootDrops == 1);

    const auto spawnOf = [&](const FakeLink& link) {
        const auto s = filter<vn::EntitySpawn>(link, [&](const vn::EntitySpawn& e) { return e.entityId == l.id; });
        REQUIRE(s.size() == 1);
        return s[0];
    };
    const vn::EntitySpawn sa = spawnOf(*a);
    CHECK(sa.entityType == vn::kEntityTypeLoot);
    CHECK(sa.name == "Kül Çömezi");
    CHECK(sa.templateId == 1);
    CHECK(sa.level == 1);
    CHECK(sa.x == l.x);
    CHECK(sa.hp == 0);
    CHECK(sa.maxHp == 0);
    CHECK(sa.race == 0);
    CHECK(sa.state == vn::kEntityStateLootMine);
    CHECK(spawnOf(*b).state == vn::kEntityStateLootLocked);
    CHECK_FALSE(far->visible().contains(l.id)); // out of view
    h.checkInvariants();

    // A monster without a loot table drops nothing.
    h.zone->placeEntity(1, 158.0f, 100.0f);
    h.combat(1, K::Attack, dummy);
    for (int i = 0; i < 400 && h.zone->findMonster(dummy)->state != vw::Zone::Monster::State::Dead; ++i)
        h.step();
    REQUIRE(h.zone->findMonster(dummy)->state == vw::Zone::Monster::State::Dead);
    h.step();
    CHECK(h.lootIds().size() == 1);
}

TEST_CASE("Loot zone: the owner window - only the owner for 30 s, then everybody; lock bits cleared",
          "[server][world][loot]")
{
    Harness h;
    auto a = h.enter(1, 200.0f, 200.0f);
    auto b = h.enter(2, 201.0f, 200.0f);
    h.step();
    const vw::EntityId id = h.zone->dropLootForTesting(200.0f, 201.0f, 10, roll(5, {{9, 3}}));
    h.step();
    h.open(2, id);
    h.take(2, id, vn::kLootTakeAll);
    h.open(1, id);
    h.step();
    CHECK(b->all<vn::LootOpenRes>().back() == vn::LootOpenRes{id, LR::NotYours});
    CHECK(takeResults(*b).back().result == LR::NotYours);
    CHECK(b->grants().empty());
    CHECK(a->all<vn::LootOpenRes>().back() == vn::LootOpenRes{id, LR::Ok});
    const auto c = lastContents(*a);
    REQUIRE(c);
    CHECK(c->lootId == id);
    CHECK(c->coins == 5);
    REQUIRE(c->entries.size() == 1);
    CHECK(c->entries[0] == vn::LootEntry{0, 9, 3});
    CHECK(b->count<vn::LootContents>() == 0);

    // At 30 s both observers get ENTITY_STATE(0); B may open and take now.
    a->clear();
    b->clear();
    h.stepFor(29'000ms);
    CHECK(filter<vn::EntityState>(*b, [&](const vn::EntityState& s) { return s.entityId == id; }).empty());
    h.stepFor(1'100ms);
    CHECK(filter<vn::EntityState>(*a, [&](const vn::EntityState& s) { return s.entityId == id; }) ==
          std::vector<vn::EntityState>{{id, 0}});
    CHECK(filter<vn::EntityState>(*b, [&](const vn::EntityState& s) { return s.entityId == id; }) ==
          std::vector<vn::EntityState>{{id, 0}});
    h.open(2, id);
    h.step();
    CHECK(b->all<vn::LootOpenRes>().back() == vn::LootOpenRes{id, LR::Ok});
    // A newcomer after the window sees no lock bits.
    auto c3 = h.enter(3, 205.0f, 205.0f);
    h.step();
    const auto s3 = filter<vn::EntitySpawn>(*c3, [&](const vn::EntitySpawn& e) { return e.entityId == id; });
    REQUIRE(s3.size() == 1);
    CHECK(s3[0].state == 0);
    h.checkInvariants();
}

TEST_CASE("Loot zone: NotFound, OutOfRange, YouAreDead; nothing is reserved on a failure", "[server][world][loot]")
{
    Harness h(spawnAt(5, 160.0f, 100.0f));
    auto a = h.enter(1, 158.0f, 100.0f);
    h.step();
    const vw::EntityId near = h.zone->dropLootForTesting(155.0f, 100.0f, 10, roll(5, {{9, 1}}));
    const vw::EntityId away = h.zone->dropLootForTesting(420.0f, 420.0f, 10, roll(5, {{9, 1}}));
    h.step();
    h.take(1, 999999, vn::kLootTakeAll); // no such entity
    h.take(1, away, vn::kLootTakeAll);    // exists but not spawned to the player
    h.take(1, 1, vn::kLootTakeAll);       // a player, not a container
    h.take(1, near, 0);                   // 3 m away + 0.5 m tolerance: 3.0 m is fine...
    h.step();
    auto r = takeResults(*a);
    REQUIRE(r.size() == 3);
    CHECK(r[0].result == LR::NotFound);
    CHECK(r[1].result == LR::NotFound);
    CHECK(r[2].result == LR::NotFound);
    REQUIRE(a->grants().size() == 1); // ... the in-range take was granted
    h.settle(a->takeGrants()[0], 0, {{0, 9, 1}}); // the session could not place it: back in
    h.zone->placeEntity(1, 151.4f, 100.0f);         // 3.6 m: out of range
    h.take(1, near, 0);
    h.open(1, near);
    h.step();
    CHECK(takeResults(*a).back().result == LR::OutOfRange);
    CHECK(a->all<vn::LootOpenRes>().back().result == LR::OutOfRange);

    // Dead: the elite kills a 1-hp player.
    h.zone->placeEntity(1, 158.0f, 100.0f);
    h.zone->setVitalsForTesting(1, 1, 0);
    h.combat(1, K::Attack, h.monsterIds()[0]);
    for (int i = 0; i < 100 && !h.zone->findPlayer(1)->combat.dead; ++i)
        h.step();
    REQUIRE(h.zone->findPlayer(1)->combat.dead);
    h.zone->placeEntity(1, 155.5f, 100.0f);
    h.take(1, near, 0);
    h.open(1, near);
    h.step();
    CHECK(takeResults(*a).back().result == LR::YouAreDead);
    CHECK(a->all<vn::LootOpenRes>().back().result == LR::YouAreDead);
    CHECK(a->grants().empty());
    const vw::Zone::Loot& l = *h.zone->findLoot(near);
    CHECK(l.coins == 5);
    CHECK(l.entries[0].quantity == 1);
    CHECK(l.reservations == 0);
}

TEST_CASE("Loot zone: take one, coins, everything - reservations, contents to viewers, settles, despawn when empty",
          "[server][world][loot]")
{
    Harness h;
    auto a = h.enter(1, 200.0f, 200.0f);
    auto b = h.enter(2, 201.0f, 199.0f);
    h.step();
    const vw::EntityId id = h.zone->dropLootForTesting(200.0f, 201.0f, 0, roll(12, {{9, 4}, {12, 1}, {10, 2}}));
    h.step();
    h.open(1, id);
    h.open(2, id); // no owner: free for all at once
    h.step();
    REQUIRE(lastContents(*b));
    CHECK(lastContents(*b)->entries.size() == 3);

    // Take entry 1: reserved (gone from everybody's view at once), handed to A's session.
    a->clear();
    b->clear();
    const auto s1 = h.take(1, id, 1);
    h.step();
    auto grants = a->takeGrants();
    REQUIRE(grants.size() == 1);
    CHECK(grants[0].loot == id);
    CHECK(grants[0].seq == s1);
    CHECK(grants[0].coins == 0);
    CHECK(grants[0].items == std::vector<vw::LootGrant::Item>{{1, 12, 1}});
    CHECK(takeResults(*a).empty()); // the session answers after its write
    for (const auto& link : {a, b})
    {
        const auto c = lastContents(*link);
        REQUIRE(c);
        CHECK(c->coins == 12);
        CHECK(c->entries == std::vector<vn::LootEntry>{{0, 9, 4}, {2, 10, 2}});
    }
    CHECK(h.zone->findLoot(id)->reservations == 1);
    h.settle(grants[0], 0, {}); // committed
    h.step();
    CHECK(h.zone->findLoot(id)->reservations == 0);

    // The same entry again: AlreadyTaken; a bad index too; coins.
    a->clear();
    h.take(1, id, 1);
    h.take(1, id, 7);
    h.take(1, id, 8);
    h.take(1, id, 0xFD);
    h.take(1, id, vn::kLootTakeCoins);
    h.step();
    auto r = takeResults(*a);
    REQUIRE(r.size() == 4);
    for (const auto& x : r)
        CHECK(x.result == LR::AlreadyTaken);
    grants = a->takeGrants();
    REQUIRE(grants.size() == 1);
    CHECK(grants[0].coins == 12);
    CHECK(grants[0].items.empty());
    // B takes the coins too, while A's take is in flight: nothing left.
    h.take(2, id, vn::kLootTakeCoins);
    h.step();
    CHECK(takeResults(*b).back().result == LR::AlreadyTaken);
    // A's session could only fit 2 of 12 coins (purse almost full): 10 go back.
    h.settle(grants[0], 10, {});
    h.step();
    CHECK(lastContents(*b)->coins == 10);
    CHECK(h.zone->counters().lootReturns == 1);

    // Everything: coins and both entries; the session fits 3 of the 4 Kül Tozu.
    a->clear();
    b->clear();
    h.take(1, id, vn::kLootTakeAll);
    h.step();
    grants = a->takeGrants();
    REQUIRE(grants.size() == 1);
    CHECK(grants[0].coins == 10);
    CHECK(grants[0].items == std::vector<vw::LootGrant::Item>{{0, 9, 4}, {2, 10, 2}});
    CHECK(lastContents(*b)->coins == 0);
    CHECK(lastContents(*b)->entries.empty());
    h.step(5);
    CHECK(h.zone->findLoot(id) != nullptr); // empty but reserved: stays
    h.settle(grants[0], 0, {{0, 9, 1}});
    h.step();
    REQUIRE(h.zone->findLoot(id) != nullptr);
    CHECK(lastContents(*b)->entries == std::vector<vn::LootEntry>{{0, 9, 1}});

    // The last unit: emptied -> LOOT_CLOSED(Gone) to both viewers, then ENTITY_DESPAWN.
    a->clear();
    b->clear();
    h.take(2, id, 0);
    h.step();
    grants = b->takeGrants();
    REQUIRE(grants.size() == 1);
    h.settle(grants[0], 0, {});
    h.step();
    CHECK(h.zone->findLoot(id) == nullptr);
    for (const auto& link : {a, b})
    {
        const auto packets = link->packets();
        const auto closed = std::find_if(packets.begin(), packets.end(), [](const FakeLink::Packet& p) {
            return p.opcode == static_cast<std::uint16_t>(vn::Opcode::LootClosed);
        });
        const auto despawn = std::find_if(packets.begin(), packets.end(), [](const FakeLink::Packet& p) {
            return p.opcode == static_cast<std::uint16_t>(vn::Opcode::EntityDespawn);
        });
        REQUIRE(closed != packets.end());
        REQUIRE(despawn != packets.end());
        CHECK(closed < despawn);
        CHECK(link->all<vn::LootClosed>() == std::vector<vn::LootClosed>{{id, vn::LootClosedReason::Gone}});
        CHECK_FALSE(link->visible().contains(id));
    }
    CHECK(h.zone->counters().lootDespawns == 1);
    CHECK(h.zone->lootReservationCount() == 0);
    h.checkInvariants();
}

TEST_CASE("Loot zone: two looters racing for one entry - the first request wins", "[server][world][loot]")
{
    Harness h;
    auto a = h.enter(1, 200.0f, 200.0f);
    auto b = h.enter(2, 201.0f, 200.0f);
    h.step();
    const vw::EntityId id = h.zone->dropLootForTesting(200.5f, 201.0f, 0, roll(0, {{12, 1}, {9, 5}}));
    h.step();
    // Same tick, B first.
    h.take(2, id, 0);
    h.take(1, id, 0);
    h.take(1, id, vn::kLootTakeAll); // whatever is left
    h.step();
    const auto gb = b->takeGrants();
    const auto ga = a->takeGrants();
    REQUIRE(gb.size() == 1);
    CHECK(gb[0].items == std::vector<vw::LootGrant::Item>{{0, 12, 1}});
    REQUIRE(ga.size() == 1);
    CHECK(ga[0].items == std::vector<vw::LootGrant::Item>{{1, 9, 5}});
    REQUIRE(takeResults(*a).size() == 1);
    CHECK(takeResults(*a)[0].result == LR::AlreadyTaken);
    // Every unit is in exactly one place: B's and A's reservations, nothing left.
    CHECK(h.zone->findLoot(id)->empty());
    CHECK(h.zone->lootReservationCount() == 2);
    h.settle(gb[0], 0, {});
    h.settle(ga[0], 0, {});
    h.step();
    CHECK(h.zone->findLoot(id) == nullptr);
}

TEST_CASE("Loot zone: leaving returns reservations not being written; one in flight waits for its settle",
          "[server][world][loot]")
{
    Harness h;
    auto a = h.enter(1, 200.0f, 200.0f);
    auto b = h.enter(2, 201.0f, 200.0f);
    h.step();
    const vw::EntityId id = h.zone->dropLootForTesting(200.0f, 201.0f, 0, roll(9, {{9, 2}, {12, 1}}));
    h.step();
    h.open(2, id);
    h.take(1, id, 0);
    h.take(1, id, 1);
    h.step();
    auto grants = a->takeGrants();
    REQUIRE(grants.size() == 2);
    // A disconnects: its session had started writing the second take only.
    REQUIRE(h.zone->leave(1));
    h.zone->cancelLootTakes(1, {grants[1].reservation});
    h.step();
    const vw::Zone::Loot* l = h.zone->findLoot(id);
    REQUIRE(l);
    CHECK(l->entries[0].quantity == 2); // the first take came back
    CHECK(l->entries[1].quantity == 0); // the second is still reserved
    CHECK(l->reservations == 1);
    CHECK(lastContents(*b)->entries == std::vector<vn::LootEntry>{{0, 9, 2}});
    // Expiry passes, but the container waits for the write; then it goes.
    h.stepFor(91'000ms);
    REQUIRE(h.zone->findLoot(id));
    b->clear();
    h.settle(grants[1], 0, {}); // committed
    h.step();
    CHECK(h.zone->findLoot(id) == nullptr);
    CHECK(b->all<vn::LootClosed>() == std::vector<vn::LootClosed>{{id, vn::LootClosedReason::Gone}});
    CHECK(h.zone->lootReservationCount() == 0);
    // Cancelling a session with nothing reserved, or settling the same reservation twice, is harmless.
    h.zone->cancelLootTakes(1, {});
    h.zone->cancelLootTakes(7, {});
}

TEST_CASE("Loot zone: viewers are closed at 6 m and on death; LOOT_CLOSE_REQ and a second open replace silently",
          "[server][world][loot]")
{
    Harness h(spawnAt(5, 215.0f, 200.0f));
    auto a = h.enter(1, 200.0f, 200.0f);
    h.step();
    const vw::EntityId one = h.zone->dropLootForTesting(200.0f, 201.0f, 0, roll(3, {}));
    const vw::EntityId two = h.zone->dropLootForTesting(201.0f, 200.0f, 0, roll(4, {}));
    h.step();
    h.open(1, one);
    h.step();
    CHECK(h.zone->findLoot(one)->viewers.contains(1));
    h.open(1, two); // replaces the first view, no LOOT_CLOSED
    h.step();
    CHECK_FALSE(h.zone->findLoot(one)->viewers.contains(1));
    CHECK(h.zone->findLoot(two)->viewers.contains(1));
    CHECK(a->count<vn::LootClosed>() == 0);
    h.close(1, one); // not the open one: ignored
    CHECK(h.zone->findPlayer(1)->viewingLoot == two);
    h.close(1, two);
    CHECK(h.zone->findPlayer(1)->viewingLoot == 0);
    CHECK(a->count<vn::LootClosed>() == 0);

    // 5.9 m is still open, 6.1 m closes with OutOfRange.
    h.open(1, two);
    h.zone->placeEntity(1, 201.0f, 205.9f);
    h.step();
    CHECK(h.zone->findPlayer(1)->viewingLoot == two);
    h.zone->placeEntity(1, 201.0f, 206.1f);
    h.step();
    CHECK(h.zone->findPlayer(1)->viewingLoot == 0);
    CHECK(a->all<vn::LootClosed>() == std::vector<vn::LootClosed>{{two, vn::LootClosedReason::OutOfRange}});

    // Death closes with YouDied.
    a->clear();
    h.zone->placeEntity(1, 201.0f, 201.0f);
    h.open(1, two);
    h.step();
    REQUIRE(h.zone->findPlayer(1)->viewingLoot == two);
    h.zone->placeEntity(1, 202.0f, 201.0f); // still within 6 m of the container
    h.zone->setVitalsForTesting(1, 1, 0);
    h.zone->placeMonsterForTesting(h.monsterIds()[0], 204.0f, 201.0f);
    h.combat(1, K::Attack, h.monsterIds()[0]);
    for (int i = 0; i < 100 && !h.zone->findPlayer(1)->combat.dead; ++i)
        h.step();
    REQUIRE(h.zone->findPlayer(1)->combat.dead);
    h.step();
    CHECK(h.zone->findPlayer(1)->viewingLoot == 0);
    CHECK(a->all<vn::LootClosed>() == std::vector<vn::LootClosed>{{two, vn::LootClosedReason::YouDied}});
    h.checkInvariants();
}

TEST_CASE("Loot zone: containers expire after 90 s; AOI spawn and despawn like any entity", "[server][world][loot]")
{
    Harness h;
    auto a = h.enter(1, 200.0f, 200.0f);
    h.step();
    const vw::EntityId id = h.zone->dropLootForTesting(200.0f, 201.0f, 10, roll(3, {{9, 1}}));
    h.step();
    CHECK(a->visible().contains(id));
    // Walk out of view and back: despawn, spawn (the lock bit still says "mine").
    h.zone->placeEntity(1, 450.0f, 450.0f);
    h.step();
    CHECK_FALSE(a->visible().contains(id));
    h.checkInvariants();
    a->clear();
    h.zone->placeEntity(1, 200.0f, 200.0f);
    h.step();
    const auto s = filter<vn::EntitySpawn>(*a, [&](const vn::EntitySpawn& e) { return e.entityId == id; });
    REQUIRE(s.size() == 1);
    CHECK(s[0].state == vn::kEntityStateLootMine);
    h.open(1, id);
    h.stepFor(89'000ms);
    REQUIRE(h.zone->findLoot(id));
    a->clear();
    h.stepFor(1'100ms);
    CHECK(h.zone->findLoot(id) == nullptr);
    CHECK(a->all<vn::LootClosed>() == std::vector<vn::LootClosed>{{id, vn::LootClosedReason::Gone}});
    CHECK(a->all<vn::EntityDespawn>() == std::vector<vn::EntityDespawn>{{id}});
    h.checkInvariants();
}

TEST_CASE("Loot zone: at most 8 takes in flight per player", "[server][world][loot]")
{
    Harness h;
    auto a = h.enter(1, 200.0f, 200.0f);
    h.step();
    const vw::EntityId one = h.zone->dropLootForTesting(200.0f, 201.0f, 0,
                                                        roll(1, {{9, 1}, {9, 1}, {9, 1}, {9, 1}, {9, 1}, {9, 1},
                                                                 {9, 1}, {9, 1}}));
    h.step();
    for (std::uint8_t i = 0; i < 8; ++i)
        h.take(1, one, i);
    h.take(1, one, vn::kLootTakeCoins);
    h.step();
    CHECK(a->grants().size() == 8);
    REQUIRE(takeResults(*a).size() == 1);
    CHECK(takeResults(*a)[0].result == LR::ServerError);
    CHECK(h.zone->findLoot(one)->coins == 1); // the refused take reserved nothing
}
