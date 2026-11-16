// Skill data and rules (shared/game/Skills.h, docs/PROTOCOL_SKILLS.md): the shipped gamedata/skills.toml
// (the fixed list of ids, names, icon keys, branches; rank-1 values of the M9 skills), the strict
// loader, points / ranks / tier gates, the skill bar, the description text, the effect modifiers and
// the v6 messages.

#include "shared/game/Combat.h"
#include "shared/game/Skills.h"
#include "shared/net/Messages.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <cmath>
#include <set>
#include <string>
#include <vector>

namespace vg = vo::game;
namespace vn = vo::net;
using Catch::Matchers::ContainsSubstring;
using R = vn::SkillLearnResult;

namespace {

const vg::SkillCatalog& shipped()
{
    static const vg::SkillCatalog c = [] {
        vg::SkillCatalog out;
        std::string error;
        const auto file = vg::locateSkillsFile();
        REQUIRE(file);
        INFO(error);
        REQUIRE(vg::loadSkillCatalog(*file, out, error));
        return out;
    }();
    return c;
}

// A minimal valid file: per class two branches, the free skill (= class id) and one tier-1 skill
// of branch 1 (ids 41..44); `extra` is appended (more [[skill]] tables).
std::string minimalToml(const std::string& extra = {}, const std::string& rules = "points_per_level = 1\ntier_points = [0, 2, 4]\n")
{
    std::string s = "[rules]\n" + rules;
    for (int c = 1; c <= 4; ++c)
        for (int b = 0; b < 2; ++b)
            s += "[[branch]]\nclass = " + std::to_string(c) + "\nindex = " + std::to_string(b) + "\nname = \"B" +
                 std::to_string(c) + std::to_string(b) + "\"\n";
    for (int c = 1; c <= 4; ++c)
    {
        s += "[[skill]]\nid = " + std::to_string(c) + "\nkey = \"free" + std::to_string(c) + "\"\nname = \"Free " +
             std::to_string(c) + "\"\ndescription = \"%{damage}\"\nclass = " + std::to_string(c) +
             "\nbranch = 0\ntier = 1\nfree = true\ntarget = \"enemy\"\nrange = 2.5\nmp = 5\ncooldown_ms = 1000\n"
             "damage = 1.5\n";
        s += "[[skill]]\nid = " + std::to_string(40 + c) + "\nkey = \"other" + std::to_string(c) +
             "\"\nname = \"Other " + std::to_string(c) + "\"\ndescription = \"x\"\nclass = " + std::to_string(c) +
             "\nbranch = 1\ntier = 1\ntarget = \"self\"\nmp = 5\ncooldown_ms = 1000\n"
             "[[skill.effect]]\nkind = \"attack_up\"\non = \"self\"\nduration_ms = 5000\nvalue = 10\n";
    }
    return s + extra;
}

std::string parseError(const std::string& text)
{
    vg::SkillCatalog c;
    std::string error;
    if (vg::parseSkillCatalog(text, "test.toml", c, error))
        return "(parsed)";
    return error;
}

// A warrior skill table appended to minimalToml.
std::string warriorSkill(const std::string& body)
{
    return "[[skill]]\nid = 30\nkey = \"t30\"\nname = \"T30\"\ndescription = \"x\"\nclass = 1\nbranch = 0\n" + body;
}

vg::SkillRanks ranksOf(std::initializer_list<std::pair<std::uint16_t, std::uint8_t>> list)
{
    vg::SkillRanks r{};
    for (const auto& [id, rank] : list)
        r[id] = rank;
    return r;
}

} // namespace

TEST_CASE("Skills: the shipped file has the fixed 28 skills, names, icon keys and branches", "[game][skills]")
{
    const auto& c = shipped();
    REQUIRE(c.size() == 28);
    struct Row
    {
        std::uint16_t id;
        std::uint8_t cls, branch, tier;
        const char* name;
        const char* key;
    };
    const Row rows[] = {
        {1, 1, 0, 1, "Ağır Darbe", "agir_darbe"},         {5, 1, 0, 2, "Yarma", "yarma"},
        {6, 1, 0, 3, "Kan Öfkesi", "kan_ofkesi"},         {7, 1, 0, 3, "Sarsıntı", "sarsinti"},
        {8, 1, 1, 1, "Kalkan Duvarı", "kalkan_duvari"},   {9, 1, 1, 2, "Meydan Okuma", "meydan_okuma"},
        {10, 1, 1, 3, "Dirençli Soluk", "direncli_soluk"}, {2, 2, 0, 1, "Çifte Kesik", "cifte_kesik"},
        {11, 2, 0, 2, "Sırt Hançeri", "sirt_hanceri"},    {12, 2, 0, 3, "Gölge Adımı", "golge_adimi"},
        {13, 2, 0, 3, "Seri Eller", "seri_eller"},        {14, 2, 1, 1, "Zehirli Kesik", "zehirli_kesik"},
        {15, 2, 1, 2, "Sis Bombası", "sis_bombasi"},      {16, 2, 1, 3, "Kaçamak", "kacamak"},
        {3, 3, 0, 1, "Köz Oku", "koz_oku"},               {17, 3, 0, 2, "Yakıcı İz", "yakici_iz"},
        {18, 3, 0, 3, "Alev Halkası", "alev_halkasi"},    {19, 3, 0, 3, "Kor Yağmuru", "kor_yagmuru"},
        {20, 3, 1, 1, "Kırağı Oku", "kiragi_oku"},        {21, 3, 1, 2, "Buz Kabuğu", "buz_kabugu"},
        {22, 3, 1, 3, "Donduran Nefes", "donduran_nefes"}, {4, 4, 0, 1, "Soluk Güneş", "soluk_gunes"},
        {23, 4, 0, 2, "Yenileyen Işık", "yenileyen_isik"}, {24, 4, 0, 3, "Kutlu Siper", "kutlu_siper"},
        {25, 4, 0, 3, "Halka Şifası", "halka_sifasi"},    {26, 4, 1, 1, "Işık Çarpması", "isik_carpmasi"},
        {27, 4, 1, 2, "Bağlayan Söz", "baglayan_soz"},    {28, 4, 1, 3, "Öfkeli Dua", "ofkeli_dua"},
    };
    for (const Row& r : rows)
    {
        INFO("skill " << r.id);
        const vg::SkillDef* s = c.find(r.id);
        REQUIRE(s);
        CHECK(s->name == r.name);
        CHECK(s->key == r.key);
        CHECK(s->charClass == r.cls);
        CHECK(s->branch == r.branch);
        CHECK(s->tier == r.tier);
        CHECK(s->free == (r.id <= 4));
        CHECK_FALSE(s->description.empty());
        for (std::uint8_t rank = 1; rank <= 3; ++rank)
            CHECK(vg::describeSkill(*s, rank).find('{') == std::string::npos);
    }
    CHECK(c.branchName(1, 0) == "Hücum");
    CHECK(c.branchName(1, 1) == "Siper");
    CHECK(c.branchName(2, 0) == "Pusu");
    CHECK(c.branchName(2, 1) == "Zehir");
    CHECK(c.branchName(3, 0) == "Köz");
    CHECK(c.branchName(3, 1) == "Kırağı");
    CHECK(c.branchName(4, 0) == "Şifa");
    CHECK(c.branchName(4, 1) == "Yargı");
    CHECK(c.rules().pointsPerLevel == 1);
    CHECK(c.rules().tierPoints == std::array<std::uint16_t, 3>{0, 2, 4});
    for (std::uint8_t cls = 1; cls <= 4; ++cls)
    {
        REQUIRE(c.freeSkill(cls));
        CHECK(c.freeSkill(cls)->id == cls);
    }
    // The process-wide catalog is the shipped file.
    CHECK(vg::skills().size() == 28);
    CHECK(vg::findSkill(19)->name == "Kor Yağmuru");
}

TEST_CASE("Skills: rank 1 of the M9 skills is exactly v5", "[game][skills]")
{
    const auto& c = shipped();
    const vg::SkillDef& heavy = *c.find(1);
    CHECK(heavy.at(1).multiplier == 2.2f);
    CHECK(heavy.at(1).mpCost == 12);
    CHECK(heavy.at(1).cooldownMs == 6000);
    CHECK(heavy.at(1).range == 2.5f);
    CHECK(heavy.at(3).multiplier == 2.8f);
    const vg::SkillDef& twin = *c.find(2);
    CHECK(twin.hits == 2);
    CHECK(twin.at(1).multiplier == 1.2f);
    CHECK(twin.at(1).cooldownMs == 5000);
    const vg::SkillDef& bolt = *c.find(3);
    CHECK(bolt.at(1).castMs == 600);
    CHECK(bolt.at(1).range == 16.0f);
    CHECK(bolt.projectile);
    const vg::SkillDef& sun = *c.find(4);
    CHECK(sun.target == vg::SkillTarget::SelfOrFriendly);
    CHECK(sun.at(1).healFraction == 0.19f);
    CHECK(sun.at(1).castMs == 800);
    CHECK(vg::healAmount(sun, 95, 1.3f, 1) == 23);
    CHECK(vg::healAmount(sun, 95, 1.3f, 3) == 31); // 25 %
    CHECK(vg::healPermille(sun, 1.3f, 2) == 286);
}

TEST_CASE("Skills: every shipped skill is sane", "[game][skills]")
{
    for (const vg::SkillDef& s : shipped().skills())
    {
        INFO("skill " << s.id << " " << s.name);
        for (std::uint8_t r = 1; r <= 3; ++r)
        {
            CHECK(s.at(r).cooldownMs <= vg::kMaxSkillCooldownMs);
            if (r > 1) // ranks never get worse
            {
                CHECK(s.at(r).multiplier >= s.at(r - 1).multiplier);
                CHECK(s.at(r).healFraction >= s.at(r - 1).healFraction);
            }
        }
        for (const auto& e : s.effects)
        {
            CHECK(e.durationMs[0] <= e.durationMs[2]);
            CHECK(e.value[0] <= e.value[2]);
            CHECK((vg::isHelpfulEffect(e.kind) == !s.hitsEnemies() || e.onSelf));
        }
    }
}

TEST_CASE("Skills: the loader accepts the minimal file and rejects every broken one", "[game][skills][loader]")
{
    REQUIRE(parseError(minimalToml()) == "(parsed)");
    struct Case
    {
        std::string text;
        const char* expect;
    };
    const Case cases[] = {
        {"[[skill", "test.toml:1"},
        {minimalToml("", "points_per_level = 0\ntier_points = [0, 2, 4]\n"), "points_per_level"},
        {minimalToml("", "points_per_level = 1\ntier_points = [1, 2, 4]\n"), "tier_points"},
        {minimalToml("", "points_per_level = 1\ntier_points = [0, 4, 2]\n"), "tier_points"},
        {minimalToml("", "points_per_level = 1\n"), "missing 'tier_points'"},
        {minimalToml("[[skill]]\nid = 1\nkey = \"dup\"\nname = \"Dup\"\ndescription = \"x\"\nclass = 1\nbranch = 0\n"
                     "tier = 2\ntarget = \"enemy\"\nrange = 2\nmp = 1\ncooldown_ms = 1\ndamage = 1\n"),
         "duplicate skill id 1"},
        {minimalToml(warriorSkill("tier = 2\ntarget = \"enemy\"\nrange = 2\nmp = 1\ncooldown_ms = 1\ndamage = 1\nbogus = 1\n")),
         "unknown key 'bogus'"},
        {minimalToml(warriorSkill("tier = 4\ntarget = \"enemy\"\nrange = 2\nmp = 1\ncooldown_ms = 1\ndamage = 1\n")),
         "'tier' = 4 is out of range"},
        {minimalToml(warriorSkill("tier = 2\ntarget = \"foe\"\nrange = 2\nmp = 1\ncooldown_ms = 1\ndamage = 1\n")),
         "target must be"},
        {minimalToml(warriorSkill("tier = 2\ntarget = \"enemy\"\nmp = 1\ncooldown_ms = 1\ndamage = 1\n")),
         "missing 'range'"},
        {minimalToml(warriorSkill("tier = 2\ntarget = \"self\"\nrange = 2\nmp = 1\ncooldown_ms = 1\ndamage = 1\n")),
         "a self skill has no range"},
        {minimalToml(warriorSkill("tier = 2\ntarget = \"enemy\"\nrange = 2\nmp = 1\ncooldown_ms = 61000\ndamage = 1\n")),
         "'cooldown_ms' = 61000 is out of range"},
        {minimalToml(warriorSkill("tier = 2\ntarget = \"enemy\"\nrange = 2\nmp = 1\ncooldown_ms = 1\ndamage = [1, 2]\n")),
         "exactly 3 values"},
        {minimalToml(warriorSkill("tier = 2\ntarget = \"enemy\"\nrange = 2\nmp = 1.5\ncooldown_ms = 1\ndamage = 1\n")),
         "'mp' must be an integer"},
        {minimalToml(warriorSkill("tier = 2\ntarget = \"enemy\"\nrange = 2\nmp = 1\ncooldown_ms = 1\n")),
         "does nothing"},
        {minimalToml(warriorSkill("tier = 2\ntarget = \"friendly\"\nrange = 2\nmp = 1\ncooldown_ms = 1\ndamage = 1\n")),
         "damage needs an enemy target"},
        {minimalToml(warriorSkill("tier = 2\ntarget = \"enemy\"\nrange = 2\nmp = 1\ncooldown_ms = 1\nheal = 10\n")),
         "heal needs"},
        {minimalToml(warriorSkill("tier = 2\ntarget = \"enemy\"\narea = \"around_self\"\nradius = 3\nmax_targets = 2\nrange = 2\n"
                                  "mp = 1\ncooldown_ms = 1\ndamage = 1\n")),
         "around_self needs target"},
        {minimalToml(warriorSkill("tier = 2\ntarget = \"self\"\narea = \"around_self\"\nradius = 3\nmax_targets = 17\nmp = 1\n"
                                  "cooldown_ms = 1\ndamage = 1\n")),
         "'max_targets' = 17"},
        {minimalToml(warriorSkill("tier = 2\ntarget = \"self\"\narea = \"around_self\"\nradius = 31\nmax_targets = 2\nmp = 1\n"
                                  "cooldown_ms = 1\ndamage = 1\n")),
         "radius must be"},
        {minimalToml(warriorSkill("tier = 2\ntarget = \"enemy\"\nradius = 3\nrange = 2\nmp = 1\ncooldown_ms = 1\ndamage = 1\n")),
         "'radius' needs an area"},
        {minimalToml(warriorSkill("tier = 2\ntarget = \"self\"\nleap = true\nmp = 1\ncooldown_ms = 1\n"
                                  "[[skill.effect]]\nkind = \"haste\"\non = \"self\"\nduration_ms = 1000\nvalue = 5\n")),
         "a leap needs"},
        {minimalToml(warriorSkill("tier = 2\ntarget = \"enemy\"\nrange = 2\nmp = 1\ncooldown_ms = 1\n"
                                  "[[skill.effect]]\nkind = \"frenzy\"\nduration_ms = 1000\n")),
         "unknown kind 'frenzy'"},
        {minimalToml(warriorSkill("tier = 2\ntarget = \"enemy\"\nrange = 2\nmp = 1\ncooldown_ms = 1\n"
                                  "[[skill.effect]]\nkind = \"attack_up\"\nduration_ms = 1000\nvalue = 5\n")),
         "cannot land on monsters"},
        {minimalToml(warriorSkill("tier = 2\ntarget = \"self\"\nmp = 1\ncooldown_ms = 1\n"
                                  "[[skill.effect]]\nkind = \"stun\"\non = \"self\"\nduration_ms = 1000\n")),
         "is harmful"},
        {minimalToml(warriorSkill("tier = 2\ntarget = \"enemy\"\nrange = 2\nmp = 1\ncooldown_ms = 1\n"
                                  "[[skill.effect]]\nkind = \"stun\"\nduration_ms = 1000\nvalue = 3\n")),
         "takes no value"},
        {minimalToml(warriorSkill("tier = 2\ntarget = \"enemy\"\nrange = 2\nmp = 1\ncooldown_ms = 1\n"
                                  "[[skill.effect]]\nkind = \"dot\"\nduration_ms = 500\nvalue = 3\n")),
         "duration_ms must be at least tick_ms"},
        {minimalToml(warriorSkill("tier = 2\ntarget = \"enemy\"\nrange = 2\nmp = 1\ncooldown_ms = 1\n"
                                  "[[skill.effect]]\nkind = \"slow\"\nduration_ms = 1000\nvalue = 95\n")),
         "'value' = 95 is out of range"},
        {minimalToml(warriorSkill("tier = 2\ntarget = \"enemy\"\nrange = 2\nmp = 1\ncooldown_ms = 1\n"
                                  "[[skill.effect]]\nkind = \"slow\"\nduration_ms = 1000\nvalue = 9\n"
                                  "[[skill.effect]]\nkind = \"slow\"\nduration_ms = 1000\nvalue = 9\n")),
         "two effects of the same kind"},
        {minimalToml("[[skill]]\nid = 30\nkey = \"t30\"\nname = \"T30\"\ndescription = \"{nope}\"\nclass = 1\nbranch = 0\n"
                     "tier = 2\ntarget = \"enemy\"\nrange = 2\nmp = 1\ncooldown_ms = 1\ndamage = 1\n"),
         "unknown placeholder '{nope}'"},
        {minimalToml("[[skill]]\nid = 30\nkey = \"t30\"\nname = \"T30\"\ndescription = \"{value}\"\nclass = 1\nbranch = 0\n"
                     "tier = 2\ntarget = \"enemy\"\nrange = 2\nmp = 1\ncooldown_ms = 1\ndamage = 1\n"),
         "has no value for this skill"},
        {minimalToml("[[skill]]\nid = 30\nkey = \"t30\"\nname = \"T30\"\ndescription = \"oops {damage\"\nclass = 1\nbranch = 0\n"
                     "tier = 2\ntarget = \"enemy\"\nrange = 2\nmp = 1\ncooldown_ms = 1\ndamage = 1\n"),
         "unbalanced"},
        {minimalToml("[[skill]]\nid = 30\nkey = \"Bad Key\"\nname = \"T30\"\ndescription = \"x\"\nclass = 1\nbranch = 0\n"
                     "tier = 2\ntarget = \"enemy\"\nrange = 2\nmp = 1\ncooldown_ms = 1\ndamage = 1\n"),
         "key 'Bad Key'"},
        {minimalToml("[[skill]]\nid = 30\nkey = \"free1\"\nname = \"T30\"\ndescription = \"x\"\nclass = 1\nbranch = 0\n"
                     "tier = 2\ntarget = \"enemy\"\nrange = 2\nmp = 1\ncooldown_ms = 1\ndamage = 1\n"),
         "key 'free1' already used"},
        {minimalToml("[[skill]]\nid = 30\nkey = \"t30\"\nname = \"T30\"\ndescription = \"x\"\nclass = 9\nbranch = 0\n"
                     "tier = 2\ntarget = \"enemy\"\nrange = 2\nmp = 1\ncooldown_ms = 1\ndamage = 1\n"),
         "unknown class 9"},
        {minimalToml("[[skill]]\nid = 64\nkey = \"t64\"\nname = \"T64\"\ndescription = \"x\"\nclass = 1\nbranch = 0\n"
                     "tier = 2\ntarget = \"enemy\"\nrange = 2\nmp = 1\ncooldown_ms = 1\ndamage = 1\n"),
         "'id' = 64 is out of range"},
        {minimalToml("[[skill]]\nid = 30\nkey = \"t30\"\nname = \"T30\"\ndescription = \"x\"\nclass = 1\nbranch = 0\n"
                     "tier = 1\nfree = true\ntarget = \"enemy\"\nrange = 2\nmp = 1\ncooldown_ms = 1\ndamage = 1\n"),
         "exactly one free skill"},
    };
    for (const Case& c : cases)
    {
        INFO(c.text.substr(c.text.size() > 300 ? c.text.size() - 300 : 0));
        CHECK_THAT(parseError(c.text), ContainsSubstring(c.expect));
    }
    // A class whose free skill is not its M9 skill, or without a tier-1 skill in a branch.
    std::string noBranchSkill = minimalToml();

    const std::size_t at = noBranchSkill.find("branch = 1\ntier = 1\ntarget = \"self\"");
    noBranchSkill.replace(at, std::string("branch = 1\ntier = 1").size(), "branch = 1\ntier = 2");
    CHECK_THAT(parseError(noBranchSkill), ContainsSubstring("has no tier-1 skill"));
}

TEST_CASE("Skills: points per level, spending, unspent", "[game][skills][rules]")
{
    const auto& c = shipped();
    CHECK(vg::skillPointsForLevel(c, 1) == 0);
    CHECK(vg::skillPointsForLevel(c, 2) == 1);
    CHECK(vg::skillPointsForLevel(c, 10) == 9);
    CHECK(vg::skillPointsForLevel(c, 0) == 0);
    CHECK(vg::skillPointsForLevel(c, 500) == 9); // clamped to the cap
    const vg::SkillRanks base = vg::defaultSkillRanks(c, 1);
    CHECK(base[1] == 1);
    CHECK(vg::skillPointsSpent(c, 1, base) == 0);
    CHECK(vg::unspentSkillPoints(c, 1, 7, base) == 6); // a level-7 character from before M12
    const vg::SkillRanks some = ranksOf({{1, 3}, {5, 1}, {8, 2}});
    CHECK(vg::skillPointsSpent(c, 1, some) == 5);
    CHECK(vg::branchPointsSpent(c, 1, 0, some) == 3);
    CHECK(vg::branchPointsSpent(c, 1, 1, some) == 2);
    CHECK(vg::unspentSkillPoints(c, 1, 10, some) == 4);
    CHECK(vg::unspentSkillPoints(c, 1, 3, some) == 0); // more spent than the level grants: 0, kept
    // normalise: other classes' / unknown ids dropped, ranks clamped, free >= 1.
    vg::SkillRanks dirty{};
    dirty[2] = 3;  // a rogue skill on a warrior
    dirty[5] = 7;  // clamped
    dirty[63] = 1; // unknown
    const vg::SkillRanks n = vg::normalizeSkillRanks(c, 1, dirty);
    CHECK(n[1] == 1);
    CHECK(n[2] == 0);
    CHECK(n[5] == 3);
    CHECK(n[63] == 0);
}

TEST_CASE("Skills: learning - unknown, max rank, points, tier gates", "[game][skills][rules]")
{
    const auto& c = shipped();
    const vg::SkillRanks base = vg::defaultSkillRanks(c, 1);
    CHECK(vg::checkSkillLearn(c, 1, 10, base, 0) == R::UnknownSkill);
    CHECK(vg::checkSkillLearn(c, 1, 10, base, 2) == R::UnknownSkill);  // a rogue skill
    CHECK(vg::checkSkillLearn(c, 1, 10, base, 63) == R::UnknownSkill); // unknown
    CHECK(vg::checkSkillLearn(c, 1, 1, base, 1) == R::NotEnoughPoints); // level 1: 0 points
    CHECK(vg::checkSkillLearn(c, 1, 2, base, 1) == R::Ok);
    CHECK(vg::checkSkillLearn(c, 1, 2, base, 8) == R::Ok);              // tier 1 of the other branch
    CHECK(vg::checkSkillLearn(c, 1, 10, base, 5) == R::TierLocked);     // tier 2 needs 2 in Hücum
    CHECK(vg::checkSkillLearn(c, 1, 10, ranksOf({{1, 2}}), 5) == R::TierLocked); // 1 point
    CHECK(vg::checkSkillLearn(c, 1, 10, ranksOf({{1, 3}}), 5) == R::Ok);         // 2 points
    CHECK(vg::checkSkillLearn(c, 1, 10, ranksOf({{1, 3}, {8, 3}}), 6) == R::TierLocked); // Siper points do not count
    CHECK(vg::checkSkillLearn(c, 1, 10, ranksOf({{1, 3}, {5, 1}}), 6) == R::TierLocked); // 3 points
    CHECK(vg::checkSkillLearn(c, 1, 10, ranksOf({{1, 3}, {5, 2}}), 6) == R::Ok);         // 4 points
    CHECK(vg::checkSkillLearn(c, 1, 10, ranksOf({{1, 3}, {5, 2}}), 7) == R::Ok);
    CHECK(vg::checkSkillLearn(c, 1, 10, ranksOf({{1, 3}}), 1) == R::MaxRank);
    // Siper: 3 + 3 + 3 = all 9 points.
    CHECK(vg::checkSkillLearn(c, 1, 10, ranksOf({{8, 2}}), 9) == R::Ok);
    CHECK(vg::checkSkillLearn(c, 1, 10, ranksOf({{8, 3}, {9, 1}}), 10) == R::Ok);
    CHECK(vg::checkSkillLearn(c, 1, 10, ranksOf({{8, 3}, {9, 3}, {10, 3}}), 1) == R::NotEnoughPoints);
    // Max rank wins over points.
    CHECK(vg::checkSkillLearn(c, 1, 1, ranksOf({{8, 3}}), 8) == R::MaxRank);
    // Every class can spend all 9 points in either branch.
    for (std::uint8_t cls = 1; cls <= 4; ++cls)
        for (std::uint8_t b = 0; b < 2; ++b)
        {
            vg::SkillRanks r = vg::defaultSkillRanks(c, cls);
            int learned = 0;
            for (std::uint8_t tier = 1; tier <= 3; ++tier)
                for (const auto& s : c.skills())
                    if (s.charClass == cls && s.branch == b && s.tier == tier)
                        while (vg::checkSkillLearn(c, cls, 10, r, s.id) == R::Ok)
                        {
                            ++r[s.id];
                            ++learned;
                        }
            INFO("class " << int(cls) << " branch " << int(b));
            CHECK(learned == 9);
            CHECK(vg::unspentSkillPoints(c, cls, 10, r) == 0);
        }
}

TEST_CASE("Skills: the skill bar - default, validation, sanitising", "[game][skills][bar]")
{
    const auto& c = shipped();
    const vg::SkillBar def = vg::defaultSkillBar(c, 3);
    CHECK(def == vg::SkillBar{3, 0, 0, 0, 0, 0, 0, 0});
    const vg::SkillRanks r = ranksOf({{3, 1}, {17, 2}});
    CHECK(vg::validSkillBar(c, 3, r, {3, 17, 0, 0, 0, 0, 0, 17}));    // duplicates allowed
    CHECK(vg::validSkillBar(c, 3, r, {}));                            // empty bar
    CHECK_FALSE(vg::validSkillBar(c, 3, r, {3, 18, 0, 0, 0, 0, 0, 0})); // not learned
    CHECK_FALSE(vg::validSkillBar(c, 3, r, {1, 0, 0, 0, 0, 0, 0, 0}));  // a warrior skill
    CHECK_FALSE(vg::validSkillBar(c, 3, r, {999, 0, 0, 0, 0, 0, 0, 0})); // unknown
    CHECK(vg::sanitizeSkillBar(c, 3, r, {18, 17, 1, 3, 999, 0, 0, 0}) == vg::SkillBar{0, 17, 0, 3, 0, 0, 0, 0});
    // The free skill is always learned, even with no stored row.
    CHECK(vg::validSkillBar(c, 3, vg::SkillRanks{}, {3, 0, 0, 0, 0, 0, 0, 0}));
}

TEST_CASE("Skills: descriptions are filled per rank in Turkish number format", "[game][skills][text]")
{
    const auto& c = shipped();
    CHECK(vg::formatSkillNumber(2.5) == "2,5");
    CHECK(vg::formatSkillNumber(12.0) == "12");
    CHECK(vg::formatSkillNumber(220.00001) == "220");
    CHECK(vg::describeSkill(*c.find(1), 1) == "Silahını tüm gücüyle indirir: saldırı gücünün %220 oranında hasar.");
    CHECK(vg::describeSkill(*c.find(1), 3) == "Silahını tüm gücüyle indirir: saldırı gücünün %280 oranında hasar.");
    CHECK(vg::describeSkill(*c.find(7), 1) ==
          "Hedefi sarsan bir vuruş: saldırı gücünün %120 oranında hasar verir ve hedefi 1,5 sn sersemletir.");
    CHECK(vg::describeSkill(*c.find(24), 2) ==
          "Bir dostu ya da kendini kutsar: 20 sn boyunca savunma %30, en yüksek can %15 artar.");
    CHECK_THAT(vg::describeSkill(*c.find(12), 3), ContainsSubstring("14 m içindeki"));
    CHECK_THAT(vg::describeSkill(*c.find(5), 1), ContainsSubstring("3 m içindeki en çok 5 düşmana"));
    CHECK_THAT(vg::describeSkill(*c.find(10), 2), ContainsSubstring("%3,5"));
}

TEST_CASE("Skills: effect modifiers on derived stats", "[game][skills][effects]")
{
    const vg::Stats s = vg::deriveStats(1, 10);
    CHECK(vg::applyEffectModifiers(s, {}) == s);
    vg::EffectModifiers m;
    m.attackPct = 20.0f;
    m.defensePct = 50.0f;
    m.hastePct = 25.0f;
    m.dodgePct = 10.0f;
    m.maxHpPct = 10.0f;
    const vg::Stats e = vg::applyEffectModifiers(s, m);
    CHECK(e.attack == static_cast<std::uint16_t>(std::lround(s.attack * 1.2)));
    CHECK(e.defense == static_cast<std::uint16_t>(std::lround(s.defense * 1.5)));
    CHECK(e.attackIntervalMs == 1280); // 1600 / 1.25
    CHECK(e.dodgePermille == s.dodgePermille + 100);
    CHECK(e.dodgeChance == static_cast<float>(e.dodgePermille) / 1000.0f);
    CHECK(e.maxHp == static_cast<std::uint32_t>(std::lround(s.maxHp * 1.1)));
    m = {};
    m.dodgePct = 49.0f;
    CHECK(vg::applyEffectModifiers(s, m).dodgePermille == vg::kMaxDodgeWithEffectsPermille);
    m = {};
    m.hastePct = 1000.0f;
    CHECK(vg::applyEffectModifiers(s, m).attackIntervalMs == vg::kMinAttackIntervalMs);
    CHECK(vg::periodicDamage(50, 25.0f) == 13); // 12.5 -> 13
    CHECK(vg::periodicDamage(1, 1.0f) == 1);
    CHECK(vg::periodicHeal(300, 4.0f, 1.5f) == 18);
    CHECK(vg::shieldAmount(56, 150.0f) == 84);
}

TEST_CASE("Skills: v6 messages round-trip and reject malformed input", "[net][skills]")
{
    const auto roundTrip = [](const auto& msg) {
        using T = std::decay_t<decltype(msg)>;
        const auto bytes = vn::encodeMessage(msg);
        REQUIRE(bytes);
        T back{};
        REQUIRE(vn::decodeMessage(*bytes, back));
        CHECK(back == msg);
        return bytes->size();
    };
    CHECK(roundTrip(vn::SkillState{9, 4, {{1, 3}, {5, 1}}}) == 11);
    CHECK(roundTrip(vn::SkillLearnReq{7, 5}) == 6);
    CHECK(roundTrip(vn::SkillLearnRes{7, 5, R::TierLocked, 0, 3}) == 10);
    CHECK(roundTrip(vn::SkillBar{{1, 5, 0, 0, 0, 0, 0, 6}}) == 16);
    CHECK(roundTrip(vn::SkillBarSetReq{3, {1, 0, 0, 0, 0, 0, 0, 0}}) == 20);
    CHECK(roundTrip(vn::SkillBarSetRes{3, vn::SkillBarResult::NotLearned, {1, 0, 0, 0, 0, 0, 0, 0}}) == 21);
    CHECK(roundTrip(vn::SkillCooldowns{{{1, 6000, 6000}, {5, 10, 8000}}}) == 21);
    const vn::EffectEntry e{17, 6, 1, 42, 8000, 7950, 25};
    CHECK(roundTrip(vn::EffectList{9, {e, e}}) == 45);
    CHECK(roundTrip(vn::EffectList{9, {}}) == 5);
    CHECK(roundTrip(vn::EffectApply{9, e}) == 24);
    CHECK(roundTrip(vn::EffectRemove{9, 17, 6, vn::EffectRemoveReason::Depleted}) == 8);
    CHECK(roundTrip(vn::SkillLeap{3, 1.0f, 2.0f, 3.0f, 0.5f, 9}) == 24);
    CHECK(roundTrip(vn::SkillUsed{3, 12, 9, 2}) == 11);
    CHECK(vn::kProtocolVersion == 6);
    CHECK(vn::opcodeName(vn::Opcode::SkillUsed) == "SKILL_USED");
    CHECK(static_cast<std::uint16_t>(vn::Opcode::SkillState) == 0x0700);
    CHECK(static_cast<std::uint16_t>(vn::Opcode::SkillUsed) == 0x070B);

    // Writers refuse what readers would reject.
    CHECK_FALSE(vn::encodeMessage(vn::SkillState{0, 0, {{5, 1}, {1, 1}}})); // not ascending
    CHECK_FALSE(vn::encodeMessage(vn::SkillState{0, 0, {{5, 4}}}));         // rank 4
    CHECK_FALSE(vn::encodeMessage(vn::SkillCooldowns{}));                   // empty
    CHECK_FALSE(vn::encodeMessage(vn::SkillCooldowns{{{1, 0, 10}}}));       // remaining 0
    CHECK_FALSE(vn::encodeMessage(vn::EffectApply{1, {1, 14, 1, 0, 10, 5, 0}})); // kind 14
    CHECK_FALSE(vn::encodeMessage(vn::EffectApply{1, {1, 1, 0, 0, 10, 5, 0}}));  // stacks 0
    CHECK_FALSE(vn::encodeMessage(vn::EffectApply{1, {1, 1, 1, 0, 10, 50, 0}})); // remaining > duration
    CHECK_FALSE(vn::encodeMessage(vn::SkillLeap{1, NAN, 0, 0, 0, 0}));

    // Readers: unknown results / reasons, bad counts, trailing bytes.
    const auto bad = [](std::vector<std::uint8_t> bytes, auto tag) {
        decltype(tag) out{};
        return !vn::decodeMessage(bytes, out);
    };
    CHECK(bad({7, 0, 0, 0, 5, 0, 6, 0, 3, 0}, vn::SkillLearnRes{}));    // result 6
    CHECK(bad({7, 0, 0, 0, 5, 0, 0, 4, 3, 0}, vn::SkillLearnRes{}));    // rank 4
    CHECK(bad({1, 0, 0, 0, 3, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}, vn::SkillBarSetRes{})); // result 3
    CHECK(bad({9, 0, 0, 0, 17, 0, 6, 4}, vn::EffectRemove{}));          // reason 4
    CHECK(bad({9, 0, 0, 0, 17, 0, 0, 1}, vn::EffectRemove{}));          // kind 0
    CHECK(bad({9, 0, 0, 0, 17}, vn::EffectList{}));                     // count 17
    CHECK(bad({0, 0, 0, 0, 33}, vn::SkillState{}));                     // count 33
    CHECK(bad({0}, vn::SkillCooldowns{}));                              // count 0
    CHECK(bad({7, 0, 0, 0, 5, 0, 1}, vn::SkillLearnReq{}));             // trailing byte
    CHECK(bad({3, 0, 0, 0, 12, 0, 9, 0, 0, 0, 0}, vn::SkillUsed{}));     // rank 0
    // ACTION_FAIL accepts the two new reasons, COMBAT_EVENT the two new flags.
    CHECK(roundTrip(vn::ActionFail{vn::CombatAction::Skill, 12, vn::ActionFailReason::NoPath}) == 4);
    CHECK(roundTrip(vn::ActionFail{vn::CombatAction::Skill, 12, vn::ActionFailReason::NotLearned}) == 4);
    vn::CombatEvent ce;
    ce.flags = vn::kCombatFlagPeriodic | vn::kCombatFlagAbsorbed;
    CHECK(roundTrip(ce) == 24);
}
