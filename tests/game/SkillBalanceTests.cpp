// Skill balance (docs/PROTOCOL_SKILLS.md §2, DEVELOPMENT_PLAN.md §22): simulated straight fights of
// level-10 builds that spent all 9 skill points in one branch, against the Kor Bekçisi (the elite),
// with the shipped gamedata (skills.toml, items.toml, monsters.toml) and the server's formulas
// (shared code: deriveStats, applyEffectModifiers, computeDamage, periodic / heal / shield amounts).
//
// The simulation uses expected values (average variance, critical hits and dodges spread over the
// hits), 50 ms steps, the global cooldown, casts blocking swings, cooldowns, mana without
// regeneration in combat, and every effect kind that matters in a one-on-one fight (attack /
// defense / haste / dodge / max hp buffs, shields, heal and damage over time, stun, slow, weaken).
// A simple player AI: heal below 60 % hp, keep buffs and debuffs up, otherwise the ready skill with
// the most expected damage per cast, basic attacks in between. Taunts do nothing alone (skipped).
//
// VO_TUNING_TABLE=1 prints the table (copied into DEVELOPMENT_PLAN.md §22 and the final report).

#include "shared/game/Combat.h"
#include "shared/game/Items.h"
#include "shared/game/Monsters.h"
#include "shared/game/Skills.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>
#include <vector>

namespace vg = vo::game;

namespace {

vg::EquipBonus bonusOf(const vg::ItemCatalog& items, const std::vector<std::uint16_t>& equipped)
{
    vg::EquipBonus b;
    for (const auto id : equipped)
        if (const vg::ItemTemplate* t = items.find(id))
        {
            b.attack += t->attack;
            b.defense += t->defense;
            for (std::size_t i = 0; i < vg::kAttributeCount; ++i)
                b.attributes[i] = static_cast<std::uint16_t>(b.attributes[i] + t->attributes[i]);
        }
    return b;
}

vg::Attributes pts(std::uint16_t str, std::uint16_t dex, std::uint16_t vit, std::uint16_t intl, std::uint16_t spi)
{
    return vg::Attributes{str, dex, vit, intl, spi};
}

double expectedHit(double attack, float multiplier, std::uint32_t defense, float critChance)
{
    const auto a = static_cast<std::uint32_t>(std::lround(std::max(attack, 0.0)));
    const double normal = vg::computeDamage(a, multiplier, defense, 0.0f, 0.5f, 1.0f).amount;
    const double crit = vg::computeDamage(a, multiplier, defense, 1.0f, 0.5f, 0.0f).amount;
    return normal * (1.0 - critChance) + crit * critChance;
}

struct Effect
{
    std::uint16_t skill = 0;
    vg::EffectKind kind{};
    float value = 0.0f;
    double amount = 0.0; // dot / hot per tick, shield left
    double ends = 0.0;
    double nextTick = 0.0;
    double tick = 1.0;
};

struct Result
{
    bool won = false;
    double killSeconds = 0.0;
    double dieSeconds = 0.0; // the player's death, or (when it won) how long it had left at the same rate
    double damagePerSecond = 0.0;
    double hpLeft = 0.0; // fraction at the kill
    double mpUsed = 0.0;
    int skillUses = 0;
};

struct Build
{
    const char* name;
    std::uint8_t charClass;
    vg::Attributes allocated;
    std::vector<std::uint16_t> items;
    std::vector<std::pair<std::uint16_t, std::uint8_t>> ranks;
    bool support = false; // a defensive / healing branch: judged by survival, not damage
};

class Fight
{
public:
    Fight(const vg::SkillCatalog& catalog, const Build& b, const vg::ItemCatalog& items, const vg::MonsterTemplate& m)
        : m_catalog(catalog), m_class(b.charClass), m_monster(m)
    {
        m_base = vg::deriveStats(b.charClass, 10, b.allocated, bonusOf(items, b.items));
        m_ranks = vg::defaultSkillRanks(catalog, b.charClass);
        for (const auto& [id, rank] : b.ranks)
            m_ranks[id] = rank;
        for (const auto& s : catalog.skills())
            if (s.charClass == b.charClass && m_ranks[s.id] > 0)
                m_skills.push_back(&s);
    }

    [[nodiscard]] const vg::Stats& base() const noexcept { return m_base; }
    [[nodiscard]] std::uint32_t pointsSpent() const { return vg::skillPointsSpent(m_catalog, m_class, m_ranks); }

    Result run()
    {
        m_hp = m_base.maxHp;
        m_mp = m_base.maxMp;
        double monsterHp = m_monster.hp;
        double nextSwing = 0.0, gcd = 0.0, castEnds = -1.0, nextMonster = 0.4;
        const vg::SkillDef* casting = nullptr;
        std::map<std::uint16_t, double> ready;
        double dealt = 0.0, lastHpAt = 0.0;
        Result r;
        constexpr double kStep = 0.05, kLimit = 240.0;
        for (double t = 0.0; t < kLimit; t += kStep)
        {
            m_now = t;
            expire(m_self);
            expire(m_target);
            const vg::Stats s = stats();
            // Ticks.
            for (auto& e : m_target)
                while (e.kind == vg::EffectKind::Dot && e.nextTick <= t + 1e-9 && e.nextTick <= e.ends + 1e-9)
                {
                    e.nextTick += e.tick;
                    monsterHp -= e.amount;
                    dealt += e.amount;
                }
            for (auto& e : m_self)
                while (e.kind == vg::EffectKind::Hot && e.nextTick <= t + 1e-9 && e.nextTick <= e.ends + 1e-9)
                {
                    e.nextTick += e.tick;
                    m_hp = std::min<double>(s.maxHp, m_hp + e.amount);
                }
            // A cast finishing.
            if (casting && t >= castEnds - 1e-9)
            {
                dealt += execute(*casting, monsterHp);
                casting = nullptr;
            }
            // Choose a skill.
            if (!casting && t >= gcd - 1e-9)
            {
                if (const vg::SkillDef* pick = choose(ready, s))
                {
                    const vg::SkillRankData& rd = pick->at(m_ranks[pick->id]);
                    m_mp -= rd.mpCost;
                    r.mpUsed += rd.mpCost;
                    ++r.skillUses;
                    gcd = t + 0.5;
                    if (rd.castMs > 0)
                    {
                        casting = pick;
                        castEnds = t + rd.castMs / 1000.0;
                        ready[pick->id] = castEnds + rd.cooldownMs / 1000.0;
                    }
                    else
                    {
                        ready[pick->id] = t + rd.cooldownMs / 1000.0;
                        dealt += execute(*pick, monsterHp);
                    }
                }
            }
            const vg::Stats s2 = stats();
            if (!casting && t >= nextSwing - 1e-9 && t >= gcd - 1e-9)
            {
                nextSwing = t + s2.attackIntervalMs / 1000.0;
                gcd = t + 0.5;
                const double h = expectedHit(s2.attack, 1.0f, m_monster.defense, s2.critChance);
                monsterHp -= h;
                dealt += h;
            }
            if (monsterHp <= 0.0)
            {
                r.won = true;
                r.killSeconds = t;
                break;
            }
            // The monster.
            if (t >= nextMonster - 1e-9 && !has(m_target, vg::EffectKind::Stun))
            {
                const float slow = strongest(m_target, vg::EffectKind::Slow);
                nextMonster = t + m_monster.attackIntervalMs / 1000.0 * (1.0 + slow / 200.0);
                const float weaken = std::min(summed(m_target, vg::EffectKind::Weaken), 75.0f);
                double taken = expectedHit(m_monster.attack, 1.0f - weaken / 100.0f, s2.defense, 0.05f) *
                               (1.0 - s2.dodgeChance);
                for (auto& e : m_self)
                    if (e.kind == vg::EffectKind::Shield && taken > 0.0)
                    {
                        const double take = std::min(taken, e.amount);
                        e.amount -= take;
                        taken -= take;
                    }
                std::erase_if(m_self, [](const Effect& e) { return e.kind == vg::EffectKind::Shield && e.amount <= 1e-9; });
                m_hp -= taken;
                m_taken += taken;
                lastHpAt = t;
                if (m_hp <= 0.0)
                {
                    r.dieSeconds = t;
                    break;
                }
            }
        }
        const double elapsed = r.won ? r.killSeconds : r.dieSeconds;
        r.damagePerSecond = elapsed > 0.0 ? dealt / elapsed : 0.0;
        r.hpLeft = std::max(0.0, m_hp) / m_base.maxHp;
        if (r.won)
        {
            // At the same net rate of hp loss, how long it would have lasted.
            const double lossPerSecond = (m_base.maxHp - m_hp) / std::max(r.killSeconds, 1e-9);
            r.dieSeconds = lossPerSecond > 1e-9 ? m_base.maxHp / lossPerSecond : 999.0;
        }
        (void)lastHpAt;
        return r;
    }

private:
    [[nodiscard]] vg::Stats stats() const
    {
        vg::EffectModifiers m;
        for (const auto& e : m_self)
            switch (e.kind)
            {
            case vg::EffectKind::AttackUp: m.attackPct += e.value; break;
            case vg::EffectKind::DefenseUp: m.defensePct += e.value; break;
            case vg::EffectKind::Haste: m.hastePct += e.value; break;
            case vg::EffectKind::DodgeUp: m.dodgePct += e.value; break;
            case vg::EffectKind::MaxHpUp: m.maxHpPct += e.value; break;
            default: break;
            }
        return vg::applyEffectModifiers(m_base, m);
    }

    void expire(std::vector<Effect>& list)
    {
        std::erase_if(list, [&](const Effect& e) { return m_now >= e.ends - 1e-9; });
    }

    static bool has(const std::vector<Effect>& list, vg::EffectKind k)
    {
        return std::any_of(list.begin(), list.end(), [&](const Effect& e) { return e.kind == k; });
    }
    bool active(const std::vector<Effect>& list, std::uint16_t skill, vg::EffectKind k) const
    {
        return std::any_of(list.begin(), list.end(), [&](const Effect& e) {
            return e.skill == skill && e.kind == k && e.ends - m_now > 1.0;
        });
    }
    static float strongest(const std::vector<Effect>& list, vg::EffectKind k)
    {
        float v = 0.0f;
        for (const auto& e : list)
            if (e.kind == k)
                v = std::max(v, e.value);
        return v;
    }
    static float summed(const std::vector<Effect>& list, vg::EffectKind k)
    {
        float v = 0.0f;
        for (const auto& e : list)
            if (e.kind == k)
                v += e.value;
        return v;
    }

    // Expected direct damage of one use against the elite (no effects).
    [[nodiscard]] double directDamage(const vg::SkillDef& s, const vg::Stats& st) const
    {
        const vg::SkillRankData& r = s.at(m_ranks[s.id]);
        if (r.multiplier <= 0.0f)
            return 0.0;
        const float crit = std::min(vg::kMaxSkillCritChance, st.critChance + r.critBonus);
        return expectedHit(st.attack, r.multiplier, m_monster.defense, crit) * s.hits;
    }

    // Whether using `s` now does something useful (the AI).
    [[nodiscard]] bool useful(const vg::SkillDef& s, const vg::Stats& st) const
    {
        const vg::SkillRankData& r = s.at(m_ranks[s.id]);
        const double hpFraction = m_hp / st.maxHp;
        if (r.healFraction > 0.0f)
            return hpFraction < 0.6;
        bool anyEffect = false, needed = false;
        for (const auto& e : s.effects)
        {
            if (e.kind == vg::EffectKind::Taunt)
                continue; // nothing to win alone
            anyEffect = true;
            const bool helpful = vg::isHelpfulEffect(e.kind);
            const auto& list = helpful ? m_self : m_target;
            if (active(list, s.id, e.kind))
                continue;
            if (e.kind == vg::EffectKind::Hot && hpFraction >= 0.75)
                continue;
            needed = true;
        }
        if (r.multiplier > 0.0f)
            return true;
        return anyEffect && needed;
    }

    const vg::SkillDef* choose(const std::map<std::uint16_t, double>& ready, const vg::Stats& st) const
    {
        const vg::SkillDef* best = nullptr;
        double bestScore = -1.0;
        for (const vg::SkillDef* s : m_skills)
        {
            const vg::SkillRankData& r = s->at(m_ranks[s->id]);
            const auto it = ready.find(s->id);
            if ((it != ready.end() && m_now < it->second - 1e-9) || m_mp < r.mpCost || !useful(*s, st))
                continue;
            // Heals first, then buffs / debuffs, then the most damage per use.
            double score = directDamage(*s, st);
            if (r.healFraction > 0.0f)
                score = 1e6;
            else if (!s->effects.empty())
                score += 1e4;
            if (score > bestScore)
            {
                bestScore = score;
                best = s;
            }
        }
        return best;
    }

    // Applies one use; returns the direct damage dealt.
    double execute(const vg::SkillDef& s, double& monsterHp)
    {
        const std::uint8_t rank = m_ranks[s.id];
        const std::size_t ri = rank - 1u;
        const vg::SkillRankData& r = s.at(rank);
        const vg::Stats st = stats();
        const double damage = directDamage(s, st);
        monsterHp -= damage;
        if (r.healFraction > 0.0f)
            m_hp = std::min<double>(st.maxHp, m_hp + vg::healAmount(s, st.maxHp, st.healMultiplier, rank));
        for (const auto& e : s.effects)
        {
            if (e.kind == vg::EffectKind::Taunt)
                continue;
            Effect x;
            x.skill = s.id;
            x.kind = e.kind;
            x.value = e.value[ri];
            x.ends = m_now + e.durationMs[ri] / 1000.0;
            x.tick = e.tickMs / 1000.0;
            x.nextTick = m_now + x.tick;
            if (e.kind == vg::EffectKind::Dot)
                x.amount = vg::periodicDamage(st.attack, x.value);
            else if (e.kind == vg::EffectKind::Hot)
                x.amount = vg::periodicHeal(st.maxHp, x.value, st.healMultiplier);
            else if (e.kind == vg::EffectKind::Shield)
                x.amount = vg::shieldAmount(st.attack, x.value);
            auto& list = vg::isHelpfulEffect(e.kind) ? m_self : m_target;
            std::erase_if(list, [&](const Effect& o) { return o.skill == x.skill && o.kind == x.kind; });
            const std::uint32_t maxBefore = stats().maxHp;
            list.push_back(x);
            if (e.kind == vg::EffectKind::MaxHpUp)
                m_hp += std::max<double>(0.0, static_cast<double>(stats().maxHp) - maxBefore);
        }
        return damage;
    }

    const vg::SkillCatalog& m_catalog;
    std::uint8_t m_class;
    const vg::MonsterTemplate& m_monster;
    vg::Stats m_base;
    vg::SkillRanks m_ranks{};
    std::vector<const vg::SkillDef*> m_skills;
    std::vector<Effect> m_self, m_target;
    double m_hp = 0.0, m_mp = 0.0, m_now = 0.0, m_taken = 0.0;
};

} // namespace

TEST_CASE("Skills balance: every branch beats the Kor Bekçisi, none is useless or far ahead", "[game][skills][balance]")
{
    vg::ItemCatalog items;
    vg::MonsterData monsters;
    std::string error;
    REQUIRE(vg::loadItemCatalog(*vg::locateItemsFile(), items, error));
    REQUIRE(vg::loadMonsterData(*vg::locateMonstersFile(), monsters, error));
    const vg::SkillCatalog& catalog = vg::skills();
    REQUIRE(catalog.size() == 28);
    const vg::MonsterTemplate* warden = nullptr;
    for (const auto& m : monsters.templates())
        if (m.model == "ember_warden")
            warden = &m;
    REQUIRE(warden);

    // The M11 "sensible" level-10 stat builds and gear (CombatTests.cpp), plus all 9 skill points.
    const auto warrior = pts(18, 0, 9, 0, 0);
    const auto rogue = pts(0, 18, 9, 0, 0);
    const auto mage = pts(0, 0, 9, 18, 0);
    const auto priest = pts(9, 0, 9, 0, 9);
    const std::vector<std::uint16_t> wg{13, 2}, rg{12, 3}, mg{8}, pg{14};
    const std::vector<Build> baselines = {
        {"Savaşçı, yalnız Ağır Darbe 1", 1, warrior, wg, {}},
        {"Haydut, yalnız Çifte Kesik 1", 2, rogue, rg, {}},
        {"Büyücü, yalnız Köz Oku 1", 3, mage, mg, {}},
        {"Şifacı, yalnız Soluk Güneş 1", 4, priest, pg, {}},
    };
    const std::vector<Build> builds = {
        {"Savaşçı Hücum", 1, warrior, wg, {{1, 3}, {5, 3}, {6, 3}, {7, 1}}},
        {"Savaşçı Siper", 1, warrior, wg, {{8, 3}, {9, 3}, {10, 3}}, true},
        {"Haydut Pusu", 2, rogue, rg, {{2, 3}, {11, 3}, {13, 3}, {12, 1}}},
        {"Haydut Zehir", 2, rogue, rg, {{14, 3}, {15, 3}, {16, 3}}},
        {"Büyücü Köz", 3, mage, mg, {{3, 3}, {17, 3}, {19, 3}, {18, 1}}},
        {"Büyücü Kırağı", 3, mage, mg, {{20, 3}, {21, 3}, {22, 3}}},
        {"Şifacı Şifa", 4, priest, pg, {{4, 3}, {23, 3}, {24, 3}, {25, 1}}, true},
        {"Şifacı Yargı", 4, priest, pg, {{26, 3}, {27, 3}, {28, 3}}},
    };
    char* env = nullptr;
    std::size_t envLength = 0;
    const bool print = _dupenv_s(&env, &envLength, "VO_TUNING_TABLE") == 0 && env != nullptr;
    std::free(env);
    if (print)
        std::printf("%-30s %4s %4s %4s %4s %7s %7s %6s %6s %5s %4s %s\n", "build", "hp", "mp", "atk", "def", "kill_s",
                    "die_s", "dps", "hp%", "mp", "uses", "vs Kor Bekçisi");
    const auto row = [&](const Build& b, const Result& r, const Fight& f) {
        if (print)
            std::printf("%-30s %4u %4u %4u %4u %7.1f %7.1f %6.1f %5.0f%% %5.0f %4d %s\n", b.name, f.base().maxHp,
                        f.base().maxMp, f.base().attack, f.base().defense, r.won ? r.killSeconds : 0.0, r.dieSeconds,
                        r.damagePerSecond, r.hpLeft * 100.0, r.mpUsed, r.skillUses, r.won ? "wins" : "dies");
    };
    std::array<Result, 4> base{};
    for (std::size_t i = 0; i < baselines.size(); ++i)
    {
        Fight f(catalog, baselines[i], items, *warden);
        base[i] = f.run();
        row(baselines[i], base[i], f);
    }
    std::vector<Result> results;
    for (const Build& b : builds)
    {
        Fight f(catalog, b, items, *warden);
        REQUIRE(f.pointsSpent() == 9); // every build spends exactly the level-10 points
        results.push_back(f.run());
        row(b, results.back(), f);
    }
    std::vector<double> offensive;
    for (std::size_t i = 0; i < builds.size(); ++i)
    {
        INFO(builds[i].name);
        const Result& r = results[i];
        const Result& b = base[builds[i].charClass - 1u];
        // 1. Every branch wins the straight fight against the elite.
        CHECK(r.won);
        // 2. Not useless: an offensive branch kills clearly faster than the class with only its free
        //    skill (+15 % damage per second), a support branch lives clearly longer (+30 %).
        if (builds[i].support)
            CHECK(r.dieSeconds >= 1.3 * b.dieSeconds);
        else
        {
            CHECK(r.damagePerSecond >= 1.15 * b.damagePerSecond);
            offensive.push_back(r.damagePerSecond);
        }
    }
    // 3. Not far ahead: every offensive branch's damage per second within 35 % of their median.
    std::vector<double> sorted = offensive;
    std::sort(sorted.begin(), sorted.end());
    const std::size_t n = sorted.size();
    const double median = n % 2 == 1 ? sorted[n / 2] : (sorted[n / 2 - 1] + sorted[n / 2]) / 2.0;
    if (print)
        std::printf("offensive branches: median dps %.1f, max %.1f (%.0f %%), min %.1f (%.0f %%)\n", median,
                    sorted.back(), 100.0 * sorted.back() / median, sorted.front(), 100.0 * sorted.front() / median);
    for (const double d : offensive)
    {
        INFO("dps " << d << " median " << median);
        CHECK(d <= 1.35 * median);
        CHECK(d >= median / 1.35);
    }
    // 4. The two offensive branches of a class kill within 35 % of each other's time.
    for (std::size_t i = 0; i < builds.size(); i += 2)
    {
        if (builds[i].support || builds[i + 1].support)
            continue;
        INFO(builds[i].name << " vs " << builds[i + 1].name);
        const double a = results[i].killSeconds, b = results[i + 1].killSeconds;
        CHECK(std::max(a, b) <= 1.35 * std::min(a, b));
    }
}