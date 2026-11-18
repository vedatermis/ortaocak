// M15 hunting slots in the zone (server/world/ZoneCombat.cpp, docs/PROTOCOL_LEVELS.md §3): members at their
// homes, the slot's leash and respawn (at the home), no neighbour pulled with a slot, the start-up rules
// (walkable / reachable homes, gate and marker keep-outs), the boss testing hooks (respawn, forced unique).
// Synthetic terrain, fake clock and links, fixed seed.

#include "server/world/Zone.h"
#include "shared/game/Combat.h"
#include "shared/game/Items.h"
#include "shared/game/Loot.h"
#include "shared/game/Monsters.h"
#include "tests/server/world/FakeLink.h"
#include "tests/server/world/TestTerrain.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <chrono>
#include <cmath>
#include <format>
#include <map>
#include <memory>
#include <string>

namespace vw = vo::server::world;
namespace va = vo::server::auth;
namespace vn = vo::net;
namespace vg = vo::game;
using namespace std::chrono_literals;
using vo::tests::FakeLink;
using K = vw::CombatCommand::Kind;
using Catch::Matchers::ContainsSubstring;

namespace {

// One aggressive kind (ids 1-2, levels 1-2, aggro 6, leash 25), an elite (5) and the safe zone round the
// Aydor spawn (100, 100) of the test terrain.
const std::string kMonsters = R"(
[[monster]]
id = 1
name = "Kül Çömezi"
model = "ash_thrall"
level = 1
hp = 80
attack = 1
defense = 4
attack_range = 2.0
attack_interval_ms = 1500
move_speed = 4.0
wander_speed = 1.3
aggro_radius = 6.0
leash_radius = 25.0
xp = 25
respawn_ms = 30000

[[monster]]
id = 2
name = "Kül Çömezi"
model = "ash_thrall"
level = 2
hp = 105
attack = 1
defense = 5
attack_range = 2.0
attack_interval_ms = 1500
move_speed = 4.0
wander_speed = 1.3
aggro_radius = 6.0
leash_radius = 25.0
xp = 32
respawn_ms = 30000

[[monster]]
id = 5
name = "Kor Bekçisi"
model = "ember_warden"
level = 6
hp = 900
attack = 1
defense = 14
attack_range = 2.6
attack_interval_ms = 2000
move_speed = 4.0
wander_speed = 1.0
aggro_radius = 0.0
leash_radius = 22.0
xp = 300
respawn_ms = 600000
elite = true

[[safe_zone]]
zone = 1
x = 100.0
z = 100.0
radius = 20.0
)";

std::string slot(const std::string& key, float x, float z, const std::string& extra = "")
{
    return std::format("\n[[slot]]\nkey = \"{}\"\nname = \"Yuva\"\nzone = 1\nx = {}\nz = {}\nmonsters = [1, 2]\ncount = 6\n"
                       "ring = 6.5\nwander = 0.0\nrespawn_ms = 12000\nleash = 14.0\n{}",
                       key, x, z, extra);
}

const std::string kBoss = "\n[[slot]]\nkey = \"boss\"\nname = \"Taht\"\nzone = 1\nx = 300\nz = 300\nmonsters = [5]\n"
                          "count = 1\nwander = 0.0\n";

std::shared_ptr<const vg::MonsterData> data(const std::string& slots)
{
    auto d = std::make_shared<vg::MonsterData>();
    std::string error;
    const bool ok = vg::parseMonsterData(kMonsters + slots, "test.toml", *d, error);
    INFO(error);
    REQUIRE(ok);
    return d;
}

struct Harness
{
    std::unique_ptr<vw::Zone> zone;
    std::map<vw::SessionKey, std::shared_ptr<FakeLink>> links;
    vo::core::MonoTime t0 = vo::core::monoNow();
    std::uint64_t tick = 0;
    vw::EntityId nextId = 1000;

    explicit Harness(std::shared_ptr<const vg::MonsterData> d, vw::ZoneSettings s = {},
                     vo::assets::TerrainData terrain = vo::tests::makeTestTerrain())
    {
        s.savePeriodTicks = 0;
        s.seed = 1515;
        std::string error;
        zone = vw::Zone::create(1, std::move(terrain), s, error);
        REQUIRE(zone);
        zone->beginTick(now(), ms());
        const bool ok = zone->installMonsters(std::move(d), [this] { return nextId++; }, now(), error);
        INFO(error);
        REQUIRE(ok);
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
        p.character.id = static_cast<va::CharacterId>(s);
        p.character.name = "Avci" + std::to_string(s);
        p.character.race = 1;
        p.character.charClass = 1;
        p.character.level = 10;
        p.character.position = {1, x, 0.0f, z, 0.0f};
        p.equip = equip;
        zone->beginTick(now(), ms());
        zone->enter(std::move(p), now(), ms());
        return link;
    }
    void cmd(vw::SessionKey s, K kind, std::uint32_t target = 0)
    {
        zone->beginTick(now(), ms());
        zone->combat(vw::CombatCommand{s, kind, target, 0, 0});
    }
    void step(int n = 1)
    {
        for (int i = 0; i < n; ++i)
        {
            zone->beginTick(now(), ms());
            zone->update(now(), tick, [](va::AccountId, va::CharacterId, va::CharacterPosition, va::CharacterProgress,
                                         std::function<void(bool)> done) {
                if (done)
                    done(true);
            });
            zone->flush();
            ++tick;
        }
    }
    void stepFor(std::chrono::milliseconds d) { step(static_cast<int>(d / 50ms)); }

    // Monsters of slot index + 1.
    [[nodiscard]] std::vector<const vw::Zone::Monster*> slotMembers(std::uint16_t slot) const
    {
        std::vector<const vw::Zone::Monster*> out;
        zone->forEachMonster([&](const vw::Zone::Monster& m) {
            if (m.slot == slot)
                out.push_back(&m);
        });
        return out;
    }
};

} // namespace

TEST_CASE("Slot zone: members stand at their homes with the slot's leash and respawn", "[server][world][slots]")
{
    Harness h(data(slot("a", 200.0f, 200.0f) + kBoss));
    const auto members = h.slotMembers(1);
    REQUIRE(members.size() == 6);
    for (const auto* m : members)
    {
        CHECK(m->leash == 14.0f);
        CHECK(m->respawnMs == 12000);
        CHECK(m->x == m->homeX); // wander 0: exactly at home
        CHECK(m->z == m->homeZ);
        CHECK(std::abs(std::hypot(m->homeX - 200.0f, m->homeZ - 200.0f) - 6.5f) < 1e-3f);
    }
    const auto boss = h.slotMembers(2);
    REQUIRE(boss.size() == 1);
    CHECK(boss[0]->respawnMs == 600000); // the template's: tens of minutes
    CHECK(boss[0]->leash == 22.0f);
}

TEST_CASE("Slot zone: a killed member respawns at its own home after the slot's respawn time", "[server][world][slots]")
{
    Harness h(data(slot("a", 200.0f, 200.0f)));
    const vw::Zone::Monster* m = h.slotMembers(1).front();
    const float hx = m->homeX, hz = m->homeZ;
    const auto id = m->id;
    // Stand beside it (outside the others' 6 m: the ring's outside), hit it hard.
    const float dx = (hx - 200.0f) / 6.5f, dz = (hz - 200.0f) / 6.5f;
    auto a = h.enter(1, hx + 2.0f * dx, hz + 2.0f * dz, {400, 4});
    h.step();
    h.cmd(1, K::Attack, id);
    for (int i = 0; i < 100 && a->count<vn::EntityDeath>() == 0; ++i)
        h.step();
    REQUIRE(a->count<vn::EntityDeath>() == 1);
    // Corpse 3 s, gone, back after 12 s from its death - at its home.
    h.stepFor(11800ms);
    CHECK(h.zone->findMonster(id)->state == vw::Zone::Monster::State::Gone);
    h.stepFor(400ms);
    const vw::Zone::Monster& back = *h.zone->findMonster(id);
    CHECK(back.state != vw::Zone::Monster::State::Gone);
    CHECK(back.x == hx);
    CHECK(back.z == hz);
    CHECK(back.hp == back.tmpl->hp);
}

TEST_CASE("Slot zone: pulling one slot does not pull its neighbour; the slot's leash brings them home",
          "[server][world][slots]")
{
    // Two slots 25 m apart (7.5 + 7.5 + 6 + 4 m: the closest the loader allows).
    Harness h(data(slot("a", 200.0f, 200.0f) + slot("b", 225.0f, 200.0f)));
    // A player walks up to the east member of slot a (home 206.5 / 200: angle 90) and stands 2 m beyond
    // it, between the two slots; then retreats west, dragging the chasers away.
    auto a = h.enter(1, 209.0f, 200.0f);
    for (int i = 0; i < 20; ++i)
        h.step();
    int chasingA = 0;
    for (const auto* m : h.slotMembers(1))
        chasingA += m->state == vw::Zone::Monster::State::Chase ? 1 : 0;
    CHECK(chasingA >= 1);
    for (const auto* m : h.slotMembers(2))
        CHECK(m->state != vw::Zone::Monster::State::Chase); // b's west member is 9.5 m away: beyond its 6 m
    // Run west 20 m beyond slot a's leash (14 m from every home): they give up and walk home.
    h.zone->placeEntity(1, 170.0f, 200.0f);
    h.stepFor(3000ms);
    h.zone->placeEntity(1, 150.0f, 200.0f);
    h.stepFor(15000ms);
    for (const auto* m : h.slotMembers(1))
    {
        CHECK(m->state == vw::Zone::Monster::State::Idle);
        CHECK(std::hypot(m->x - m->homeX, m->z - m->homeZ) < 0.6f);
    }
    for (const auto* m : h.slotMembers(2))
        CHECK(m->state == vw::Zone::Monster::State::Idle);
    CHECK(h.zone->counters().monsterKills == 0);
}

TEST_CASE("Slot zone: start-up rules - blocked, unreachable, near a gate or a marker", "[server][world][slots]")
{
    std::string error;
    const auto install = [&](vo::assets::TerrainData t, const std::string& slots, vw::ZoneLayout layout = {}) {
        auto zone = vw::Zone::create(1, std::move(t), vw::ZoneSettings{}, error, std::move(layout));
        REQUIRE(zone);
        vw::EntityId next = 1;
        return zone->installMonsters(data(slots), [&] { return next++; }, vo::core::monoNow(), error);
    };
    CHECK(install(vo::tests::makeTestTerrain(), slot("a", 200.0f, 200.0f)));
    {
        auto t = vo::tests::makeTestTerrain();
        vo::tests::blockCell(t, 200.0f, 206.5f); // the first home
        CHECK_FALSE(install(t, slot("a", 200.0f, 200.0f)));
        CHECK_THAT(error, ContainsSubstring("zone 1: slot 'a'"));
        CHECK_THAT(error, ContainsSubstring("is not walkable"));
    }
    {
        // Walled in: walkable, but not reachable from spawn_aydor.
        auto t = vo::tests::makeTestTerrain();
        for (int k = 0; k < 720; ++k)
        {
            const double a = k * 3.14159265358979 / 360.0;
            vo::tests::blockCell(t, 200.0f + 12.0f * static_cast<float>(std::sin(a)), 200.0f + 12.0f * static_cast<float>(std::cos(a)));
        }
        CHECK_FALSE(install(t, slot("a", 200.0f, 200.0f)));
        CHECK_THAT(error, ContainsSubstring("cannot be reached on foot"));
    }
    {
        // A gate 25 m away: needs 2.5 + 12 + 7.5 + 6 = 28 m.
        auto t = vo::tests::makeTestTerrain();
        t.spawns.push_back({"gate_x", {225.0f, 0.0f, 200.0f}, 0.0f});
        vw::ZoneLayout layout;
        layout.gates.push_back({vg::GateDef{"gate_x", 2.5f, 2, "arrive_x"}, vg::kAllRaces});
        CHECK_FALSE(install(t, slot("a", 200.0f, 200.0f), layout));
        CHECK_THAT(error, ContainsSubstring("gate 'gate_x'"));
        auto far = vo::tests::makeTestTerrain();
        far.spawns.push_back({"gate_x", {229.0f, 0.0f, 200.0f}, 0.0f});
        CHECK(install(far, slot("a", 200.0f, 200.0f), layout));
    }
    {
        // An arrival marker: 8 m; a boss its leash (22) + 10 m.
        auto t = vo::tests::makeTestTerrain();
        t.spawns.push_back({"arrive_x", {220.0f, 0.0f, 200.0f}, 0.0f});
        CHECK_FALSE(install(t, slot("a", 200.0f, 200.0f)));
        CHECK_THAT(error, ContainsSubstring("marker 'arrive_x'"));
        auto b = vo::tests::makeTestTerrain();
        b.spawns.push_back({"arrive_x", {330.0f, 0.0f, 300.0f}, 0.0f});
        CHECK_FALSE(install(b, kBoss));
        auto b2 = vo::tests::makeTestTerrain();
        b2.spawns.push_back({"arrive_x", {333.0f, 0.0f, 300.0f}, 0.0f});
        CHECK(install(b2, kBoss));
    }
}

TEST_CASE("Slot zone: the boss hooks - a short respawn and a forced unique drop", "[server][world][slots][bosses]")
{
    vg::ItemCatalog items;
    std::string error;
    REQUIRE(vg::loadItemCatalog(*vg::locateItemsFile(), items, error));
    // A loot table for the boss with one ordinary and two unique lines (145, 146 are boss uniques).
    auto tables = std::make_shared<vg::LootTables>();
    REQUIRE(vg::parseLootTables("[[table]]\nmonsters = [5]\ncoins = [1, 1]\n[[table.drop]]\nitem = 9\nchance = 1\n"
                                "[[table.drop]]\nitem = 145\nchance = 0.001\n[[table.drop]]\nitem = 146\nchance = 0.001\n",
                                "loot.toml", items, nullptr, *tables, error));
    for (const bool hooks : {false, true})
    {
        INFO("hooks " << hooks);
        vw::ZoneSettings s;
        if (hooks)
        {
            s.testingBossRespawnMs = 5000;
            s.testingForceUniqueDrop = true;
        }
        Harness h(data(kBoss), s);
        h.zone->installLoot(tables, nullptr);
        const vw::Zone::Monster* boss = h.slotMembers(1).front();
        CHECK(boss->respawnMs == (hooks ? 5000u : 600000u));
        const auto id = boss->id;
        auto a = h.enter(1, 302.0f, 300.0f, {2000, 4});
        h.step();
        h.cmd(1, K::Attack, id);
        for (int i = 0; i < 200 && a->count<vn::EntityDeath>() == 0; ++i)
            h.step();
        REQUIRE(a->count<vn::EntityDeath>() == 1);
        h.step();
        int uniques = 0, loots = 0;
        h.zone->forEachLoot([&](const vw::Zone::Loot& l) {
            ++loots;
            for (std::size_t k = 0; k < l.entryCount; ++k)
                if (items.find(l.entries[k].templateId)->rarity == vg::Rarity::Unique)
                    ++uniques;
        });
        CHECK(loots == 1);
        CHECK(uniques == (hooks ? 1 : 0));
        h.stepFor(8500ms);
        CHECK((h.zone->findMonster(id)->state == vw::Zone::Monster::State::Gone) == !hooks);
    }
}
