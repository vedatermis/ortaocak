#include "client/world/SkillsView.h"

#include "shared/game/Combat.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <format>

namespace vo::client::world {

namespace vn = vo::net;

// ---------------------------------------------------------------------------------------------
// SkillBook
// ---------------------------------------------------------------------------------------------

void SkillBook::reset(std::uint8_t charClass, std::uint16_t level)
{
    const game::SkillCatalog& c = game::skills();
    m_class = charClass;
    m_level = std::max<std::uint16_t>(level, 1);
    m_known = false;
    m_ranks = game::defaultSkillRanks(c, charClass);
    m_bar = game::defaultSkillBar(c, charClass);
    m_points = static_cast<std::uint16_t>(game::skillPointsForLevel(c, m_level));
    m_unspent = 0; // until SKILL_STATE says otherwise
    m_learnSeq = 0;
    m_learnSkill = 0;
    m_barSeq = 0;
    m_barQueued.reset();
}

void SkillBook::applyState(const vn::SkillState& m)
{
    const game::SkillCatalog& c = game::skills();
    game::SkillRanks ranks{};
    for (const vn::SkillRankEntry& e : m.skills)
        if (e.skillId <= game::kMaxSkillId)
            ranks[e.skillId] = e.rank;
    m_ranks = game::normalizeSkillRanks(c, m_class, ranks);
    m_points = m.points;
    m_unspent = m.unspent;
    m_known = true;
}

std::uint8_t SkillBook::rank(std::uint16_t skillId) const noexcept
{
    return skillId <= game::kMaxSkillId ? m_ranks[skillId] : 0;
}

std::uint32_t SkillBook::branchSpent(std::uint8_t branch) const noexcept
{
    return game::branchPointsSpent(game::skills(), m_class, branch, m_ranks);
}

std::vector<const game::SkillDef*> SkillBook::branchSkills(std::uint8_t branch) const
{
    std::vector<const game::SkillDef*> out;
    for (const game::SkillDef& s : game::skills().skills())
        if (s.charClass == m_class && s.branch == branch)
            out.push_back(&s);
    std::stable_sort(out.begin(), out.end(), [](const game::SkillDef* a, const game::SkillDef* b) {
        return a->tier != b->tier ? a->tier < b->tier : a->id < b->id;
    });
    return out;
}

vn::SkillLearnResult SkillBook::check(std::uint16_t skillId) const noexcept
{
    const vn::SkillLearnResult r = game::checkSkillLearn(game::skills(), m_class, m_level, m_ranks, skillId);
    // The server decides with its unspent (a level-up's SKILL_STATE may still be on its way).
    if (r == vn::SkillLearnResult::Ok && m_unspent == 0)
        return vn::SkillLearnResult::NotEnoughPoints;
    return r;
}

bool SkillBook::gateMet(const game::SkillDef& s) const noexcept
{
    return branchSpent(s.branch) >= game::tierGatePoints(game::skills(), s);
}

bool SkillBook::canRaise(std::uint16_t skillId) const noexcept
{
    return m_known && !learnBusy() && check(skillId) == vn::SkillLearnResult::Ok;
}

std::optional<vn::SkillLearnReq> SkillBook::beginLearn(std::uint16_t skillId)
{
    if (!canRaise(skillId))
        return std::nullopt;
    m_learnSeq = m_nextSeq++;
    m_learnSkill = skillId;
    return vn::SkillLearnReq{m_learnSeq, skillId};
}

SkillBook::LearnAnswer SkillBook::finishLearn(const vn::SkillLearnRes& m)
{
    LearnAnswer a;
    a.result = m.result;
    a.skillId = m.skillId;
    a.rank = m.rank;
    a.matched = m_learnSeq != 0 && m.seq == m_learnSeq;
    if (a.matched)
    {
        m_learnSeq = 0;
        m_learnSkill = 0;
    }
    const game::SkillDef* s = game::skills().find(m.skillId);
    if (s != nullptr && s->charClass == m_class)
    {
        m_ranks[m.skillId] = m.rank;
        m_ranks = game::normalizeSkillRanks(game::skills(), m_class, m_ranks);
    }
    m_unspent = m.unspent;
    return a;
}

void SkillBook::abandonLearn() noexcept
{
    m_learnSeq = 0;
    m_learnSkill = 0;
}

void SkillBook::applyBar(const vn::SkillBar& m)
{
    for (std::size_t i = 0; i < m_bar.size(); ++i)
        m_bar[i] = m.slots[i];
}

std::optional<vn::SkillBarSetReq> SkillBook::requestBar(const game::SkillBar& wanted)
{
    if (barBusy())
    {
        if (wanted != m_bar || m_barQueued)
            m_barQueued = wanted; // sent once the one in flight is answered (only the last wish)
        return std::nullopt;
    }
    if (wanted == m_bar)
        return std::nullopt;
    m_barSeq = m_nextSeq++;
    vn::SkillBarSetReq req;
    req.seq = m_barSeq;
    for (std::size_t i = 0; i < wanted.size(); ++i)
        req.slots[i] = wanted[i];
    return req;
}

SkillBook::BarAnswer SkillBook::finishBar(const vn::SkillBarSetRes& m)
{
    BarAnswer a;
    a.result = m.result;
    a.matched = m_barSeq != 0 && m.seq == m_barSeq;
    for (std::size_t i = 0; i < m_bar.size(); ++i)
        m_bar[i] = m.slots[i]; // the answer always carries the stored bar
    if (a.matched)
    {
        m_barSeq = 0;
        if (m_barQueued)
        {
            const game::SkillBar wish = *m_barQueued;
            m_barQueued.reset();
            a.next = requestBar(wish);
        }
    }
    return a;
}

void SkillBook::abandonBar() noexcept
{
    m_barSeq = 0;
    m_barQueued.reset();
}

game::SkillBar barWithPlaced(const game::SkillBar& bar, std::size_t slot, std::uint16_t skillId) noexcept
{
    game::SkillBar b = bar;
    if (slot < b.size())
        b[slot] = skillId;
    return b;
}

game::SkillBar barWithSwapped(const game::SkillBar& bar, std::size_t a, std::size_t b) noexcept
{
    game::SkillBar out = bar;
    if (a < out.size() && b < out.size())
        std::swap(out[a], out[b]);
    return out;
}

game::SkillBar barWithCleared(const game::SkillBar& bar, std::size_t slot) noexcept
{
    return barWithPlaced(bar, slot, 0);
}

// ---------------------------------------------------------------------------------------------
// EffectBook
// ---------------------------------------------------------------------------------------------

namespace {

ShownEffect shown(const vn::EffectEntry& e, double now)
{
    ShownEffect s;
    s.skillId = e.skillId;
    s.kind = e.kind;
    s.sourceId = e.sourceId;
    s.durationMs = e.durationMs;
    s.amount = e.amount;
    s.end = now + static_cast<double>(e.remainingMs) / 1000.0;
    return s;
}

} // namespace

EffectBook::Entry* EffectBook::entry(std::uint32_t entity, bool create)
{
    for (Entry& e : m_entries)
        if (e.entity == entity)
            return &e;
    if (!create)
        return nullptr;
    m_entries.push_back({entity, {}});
    return &m_entries.back();
}

void EffectBook::list(std::uint32_t entity, std::span<const vn::EffectEntry> effects, double now)
{
    if (effects.empty())
    {
        clear(entity);
        return;
    }
    Entry* e = entry(entity, true);
    e->effects.clear();
    for (const vn::EffectEntry& x : effects)
        if (e->effects.size() < game::kMaxEffectsPerEntity)
            e->effects.push_back(shown(x, now));
}

void EffectBook::apply(std::uint32_t entity, const vn::EffectEntry& x, double now)
{
    Entry* e = entry(entity, true);
    for (ShownEffect& s : e->effects)
        if (s.skillId == x.skillId && s.kind == x.kind)
        {
            s = shown(x, now); // refresh
            return;
        }
    if (e->effects.size() < game::kMaxEffectsPerEntity)
        e->effects.push_back(shown(x, now));
}

bool EffectBook::remove(std::uint32_t entity, std::uint16_t skillId, std::uint8_t kind)
{
    Entry* e = entry(entity, false);
    if (e == nullptr)
        return false;
    const std::size_t before = e->effects.size();
    std::erase_if(e->effects, [&](const ShownEffect& s) { return s.skillId == skillId && s.kind == kind; });
    const bool removed = e->effects.size() != before;
    if (e->effects.empty())
        clear(entity);
    return removed;
}

void EffectBook::clear(std::uint32_t entity)
{
    std::erase_if(m_entries, [entity](const Entry& e) { return e.entity == entity; });
}

void EffectBook::expire(double now, double grace)
{
    for (Entry& e : m_entries)
        std::erase_if(e.effects, [&](const ShownEffect& s) { return s.end + grace < now; });
    std::erase_if(m_entries, [](const Entry& e) { return e.effects.empty(); });
}

std::span<const ShownEffect> EffectBook::of(std::uint32_t entity) const noexcept
{
    for (const Entry& e : m_entries)
        if (e.entity == entity)
            return e.effects;
    return {};
}

const ShownEffect* EffectBook::find(std::uint32_t entity, std::uint8_t kind, std::uint16_t skillId) const noexcept
{
    for (const ShownEffect& s : of(entity))
        if (s.kind == kind && (skillId == 0 || s.skillId == skillId))
            return &s;
    return nullptr;
}

bool EffectBook::has(std::uint32_t entity, std::uint8_t kind, std::uint16_t skillId) const noexcept
{
    return find(entity, kind, skillId) != nullptr;
}

std::vector<std::uint32_t> EffectBook::entities() const
{
    std::vector<std::uint32_t> out;
    out.reserve(m_entries.size());
    for (const Entry& e : m_entries)
        out.push_back(e.entity);
    return out;
}

double effectRemaining(const ShownEffect& e, double now) noexcept
{
    return std::max(0.0, e.end - now);
}

std::string effectSecondsText(double remaining)
{
    if (remaining <= 0.0)
        return {};
    return std::to_string(static_cast<int>(std::ceil(remaining - 1e-6)));
}

bool effectHelpful(std::uint8_t kind) noexcept
{
    if (kind < 1 || kind > game::kEffectKindCount)
        return false;
    return game::isHelpfulEffect(static_cast<game::EffectKind>(kind));
}

namespace {

bool mageSkill(std::uint16_t skillId)
{
    const game::SkillDef* s = game::skills().find(skillId);
    return s != nullptr && s->charClass == 3;
}

} // namespace

std::string effectStatusIcon(std::uint8_t kind, std::uint16_t skillId)
{
    std::string_view name;
    switch (static_cast<game::EffectKind>(kind))
    {
    case game::EffectKind::AttackUp: name = "attack_up"; break;
    case game::EffectKind::DefenseUp: name = "defense_up"; break;
    case game::EffectKind::Haste: name = "attack_speed_up"; break;
    case game::EffectKind::DodgeUp: name = "dodge_up"; break;
    case game::EffectKind::MaxHpUp: name = "max_hp_up"; break;
    case game::EffectKind::Dot: name = mageSkill(skillId) ? "burn" : "poison"; break;
    case game::EffectKind::Hot: name = "heal_over_time"; break;
    case game::EffectKind::Slow: name = "slow"; break;
    case game::EffectKind::Stun: name = "stun"; break;
    case game::EffectKind::Root: name = "root"; break;
    case game::EffectKind::Shield: name = "shield"; break;
    case game::EffectKind::Weaken: name = "weakened"; break;
    case game::EffectKind::Taunt: name = "taunted"; break;
    default: return {};
    }
    return std::format("ui/status/{}.png", name);
}

std::string effectKindLine(std::uint8_t kind, std::uint32_t amount, std::uint16_t skillId)
{
    switch (static_cast<game::EffectKind>(kind))
    {
    case game::EffectKind::AttackUp: return std::format("Saldırı +%{}", amount);
    case game::EffectKind::DefenseUp: return std::format("Savunma +%{}", amount);
    case game::EffectKind::Haste: return std::format("Saldırı hızı +%{}", amount);
    case game::EffectKind::DodgeUp: return std::format("Kaçınma +%{}", amount);
    case game::EffectKind::MaxHpUp: return std::format("En yüksek can +%{}", amount);
    case game::EffectKind::Dot: return std::format("{}: saniyede {}", mageSkill(skillId) ? "Yanma" : "Zehir", amount);
    case game::EffectKind::Hot: return std::format("Yenilenme: saniyede {}", amount);
    case game::EffectKind::Slow: return std::format("Yavaşlatıldı %{}", amount);
    case game::EffectKind::Stun: return "Sersemledi";
    case game::EffectKind::Root: return "Kök saldı";
    case game::EffectKind::Shield: return std::format("Kalkan: {}", amount);
    case game::EffectKind::Weaken: return std::format("Zayıfladı %{}", amount);
    case game::EffectKind::Taunt: return "Kışkırtıldı";
    }
    return {};
}

std::string effectTipText(const ShownEffect& e)
{
    const game::SkillDef* s = game::skills().find(e.skillId);
    const std::string name = s != nullptr ? s->name : std::format("Yetenek #{}", e.skillId);
    return name + "\n" + effectKindLine(e.kind, e.amount, e.skillId);
}

// ---------------------------------------------------------------------------------------------
// Using a skill
// ---------------------------------------------------------------------------------------------

SkillUse decideSkillUse(const game::SkillDef& skill, std::uint8_t rank, const SkillSelection& sel) noexcept
{
    SkillUse u;
    u.range = skill.at(rank).range;
    if (rank == 0)
    {
        u.verdict = SkillUseVerdict::NotLearned;
        return u;
    }
    switch (skill.target)
    {
    case game::SkillTarget::Enemy:
        if (sel.id == 0 || sel.self || (!sel.monster && !sel.player))
        {
            u.verdict = SkillUseVerdict::NoTarget;
            return u;
        }
        if (!sel.monster)
        {
            u.verdict = SkillUseVerdict::WrongTarget;
            return u;
        }
        if (sel.dead)
        {
            u.verdict = SkillUseVerdict::TargetDead;
            return u;
        }
        u.targetId = sel.id;
        u.verdict = game::inRange(sel.distance, u.range) ? SkillUseVerdict::Send : SkillUseVerdict::WalkFirst;
        return u;
    case game::SkillTarget::SelfOrFriendly:
        if (sel.player && !sel.dead && !sel.self && sel.id != 0)
        {
            u.targetId = sel.id;
            u.verdict = game::inRange(sel.distance, u.range) ? SkillUseVerdict::Send : SkillUseVerdict::WalkFirst;
            return u;
        }
        u.verdict = SkillUseVerdict::Send; // on ourselves
        return u;
    case game::SkillTarget::Self:
        u.verdict = SkillUseVerdict::Send;
        return u;
    }
    return u;
}

SlotLook slotLook(const game::SkillDef* skill, std::uint8_t rank, std::uint32_t mp, bool dead,
                  const SkillSelection& sel) noexcept
{
    if (skill == nullptr)
        return SlotLook::Empty;
    if (dead)
        return SlotLook::Dead;
    if (mp < skill->at(std::max<std::uint8_t>(rank, 1)).mpCost)
        return SlotLook::NoMana;
    if (skill->target == game::SkillTarget::Enemy)
    {
        if (sel.id == 0 || !sel.monster || sel.dead)
            return SlotLook::NoTarget;
        if (!game::inRange(sel.distance, skill->at(std::max<std::uint8_t>(rank, 1)).range))
            return SlotLook::NoTarget;
    }
    return SlotLook::Ready;
}

std::string_view slotLookName(SlotLook l) noexcept
{
    switch (l)
    {
    case SlotLook::Empty: return "empty";
    case SlotLook::Ready: return "ready";
    case SlotLook::NoMana: return "nomana";
    case SlotLook::NoTarget: return "notarget";
    case SlotLook::Dead: return "dead";
    }
    return "?";
}

// ---------------------------------------------------------------------------------------------
// Texts
// ---------------------------------------------------------------------------------------------

namespace {

std::string seconds(std::uint32_t ms)
{
    return game::formatSkillNumber(static_cast<double>(ms) / 1000.0);
}

} // namespace

std::vector<TipLine> skillTipLines(const game::SkillDef& skill, std::uint8_t rank)
{
    std::vector<TipLine> out;
    const game::SkillCatalog& c = game::skills();
    const std::uint8_t shownRank = std::clamp<std::uint8_t>(rank, 1, game::kMaxSkillRank);
    const game::SkillRankData& r = skill.at(shownRank);
    out.push_back({skill.name, TipTone::Title, 0xF2CA64, false});
    const std::string branch(c.branchName(skill.charClass, skill.branch));
    out.push_back({rank == 0 ? std::format("{} · Öğrenilmedi", branch)
                             : std::format("{} · Kademe {}/{}", branch, rank, game::kMaxSkillRank),
                   rank == 0 ? TipTone::Dim : TipTone::Normal, 0, false});
    out.push_back({r.cooldownMs > 0 ? std::format("Mana: {}   Bekleme: {} sn", r.mpCost, seconds(r.cooldownMs))
                                    : std::format("Mana: {}   Bekleme yok", r.mpCost),
                   TipTone::Normal, 0, false});
    std::string range;
    if (skill.target == game::SkillTarget::Self)
        range = skill.area == game::SkillArea::AroundSelf ? "Çevrene" : "Kendine";
    else
        range = std::format("Menzil: {} m", game::formatSkillNumber(r.range));
    out.push_back({range, TipTone::Normal, 0, false});
    out.push_back({r.castMs > 0 ? std::format("Hazırlık: {} sn", seconds(r.castMs)) : std::string("Anında"),
                   TipTone::Normal, 0, false});
    out.push_back({game::describeSkill(skill, shownRank), TipTone::Good, 0, true});
    if (rank >= 1 && rank < game::kMaxSkillRank)
    {
        // M15: ranks 4 and 5 need a character level (docs/PROTOCOL_LEVELS.md §2).
        const auto next = static_cast<std::uint8_t>(rank + 1);
        const std::uint16_t needs = game::rankLevel(c, next);
        out.push_back({(needs > 1 ? std::format("Sonraki kademe ({}. seviye): ", needs) : std::string("Sonraki kademe: ")) +
                           game::describeSkill(skill, next),
                       TipTone::Dim, 0, true});
    }
    return out;
}

std::string learnResultText(vn::SkillLearnResult r, const game::SkillDef* skill, std::uint8_t rank)
{
    using R = vn::SkillLearnResult;
    switch (r)
    {
    case R::Ok: return std::format("{} kademe {} oldu.", skill != nullptr ? skill->name : std::string("Yetenek"), rank);
    case R::UnknownSkill: return "Bu yeteneği öğrenemezsin.";
    case R::MaxRank: return "Bu yetenek en yüksek kademede.";
    case R::NotEnoughPoints: return "Yeterli yetenek puanın yok.";
    case R::TierLocked:
        return std::format("Bu yetenek için bu yolda önce {} puan harcamalısın.",
                           skill != nullptr ? game::tierGatePoints(game::skills(), *skill) : 0);
    case R::ServerError: return "Yetenek kaydedilemedi, tekrar dene.";
    case R::LevelTooLow:
        return std::format("Kademe {} için {}. seviyeye ulaşmalısın.", rank + 1,
                           game::rankLevel(game::skills(), static_cast<std::uint8_t>(rank + 1)));
    }
    return "Yetenek kaydedilemedi, tekrar dene.";
}

std::string_view barResultText(vn::SkillBarResult r) noexcept
{
    switch (r)
    {
    case vn::SkillBarResult::Ok: return {};
    case vn::SkillBarResult::NotLearned: return "Bu yeteneği henüz öğrenmedin.";
    case vn::SkillBarResult::ServerError: return "Yetenek çubuğu kaydedilemedi, tekrar dene.";
    }
    return "Yetenek çubuğu kaydedilemedi, tekrar dene.";
}

std::string itemUseResultText(vn::ItemUseResult r, const game::ItemTemplate* item)
{
    using R = vn::ItemUseResult;
    const std::string name = item != nullptr ? item->name : std::string("bu eşya");
    switch (r)
    {
    case R::Ok: return {};
    case R::NoItem: return std::format("Elinde {} kalmadı.", name);
    case R::NotUsable: return "Bu kullanılamaz.";
    case R::YouAreDead: return "Ölüyken kullanamazsın.";
    case R::OnCooldown: return "Henüz yeniden içemezsin.";
    case R::AlreadyFull:
        return item != nullptr && item->restoreHp == 0 && item->restoreMp > 0 ? "Manan zaten dolu." : "Canın zaten dolu.";
    case R::Stunned: return "Sersemken kullanamazsın.";
    case R::ServerError: return "Kullanılamadı, tekrar dene.";
    }
    return "Kullanılamadı, tekrar dene.";
}

std::string skillPointsText(std::uint16_t unspent)
{
    return std::format("Yetenek puanı: {}", unspent);
}

std::string branchSpentText(std::uint32_t spent)
{
    return std::format("Bu yolda harcanan: {}", spent);
}

std::string tierGateText(std::uint16_t points)
{
    return std::format("Bu yolda {} puan gerekli", points);
}

std::string rankText(std::uint8_t rank)
{
    return rank == 0 ? std::string("Öğrenilmedi") : std::format("{}/{}", rank, game::kMaxSkillRank);
}

std::string levelUpSkillPointsText(std::uint32_t points)
{
    return std::format("+{} yetenek puanı  ·  Yetenekler: K", points);
}

std::string_view branchArtKey(std::uint8_t charClass, std::uint8_t branch) noexcept
{
    static constexpr std::array<std::array<std::string_view, 2>, 4> kKeys = {{
        {"hucum", "siper"},
        {"pusu", "zehir"},
        {"koz", "kiragi"},
        {"sifa", "yargi"},
    }};
    if (charClass < 1 || charClass > 4 || branch > 1)
        return {};
    return kKeys[charClass - 1u][branch];
}

std::string skillIconPath(const game::SkillDef& skill)
{
    return std::format("ui/skills/{}.png", skill.key);
}

// ---------------------------------------------------------------------------------------------
// Effect art
// ---------------------------------------------------------------------------------------------

namespace {

float effectSeconds(const game::SkillDef& s, game::EffectKind kind, std::uint8_t rank, float fallback)
{
    for (const game::EffectDef& e : s.effects)
        if (e.kind == kind)
            return static_cast<float>(e.durationMs[std::clamp<std::size_t>(rank, 1, 3) - 1]) / 1000.0f;
    return fallback;
}

constexpr std::array<float, 3> kWarm{1.0f, 0.42f, 0.18f};
constexpr std::array<float, 3> kSteel{0.7f, 0.8f, 0.95f};
constexpr std::array<float, 3> kAmber{1.0f, 0.78f, 0.35f};

} // namespace

std::vector<ArtSpawn> skillUseArt(const game::SkillDef& skill, std::uint8_t rank)
{
    std::vector<ArtSpawn> out;
    const std::string_view k = skill.key;
    const float radius = skill.radius > 0.0f ? skill.radius : 1.0f;
    const auto add = [&](std::string_view mesh, ArtMotion m, ArtAt at, float scale, float duration, float opacity = 1.0f,
                         std::array<float, 3> tint = {1.0f, 1.0f, 1.0f}, int count = 1) {
        out.push_back({mesh, m, at, scale, duration, opacity, count, tint});
    };
    if (k == "koz_oku")
        add("ember_bolt", ArtMotion::Projectile, ArtAt::Target, 1.6f, 0.25f);
    else if (k == "kiragi_oku")
        add("frost_bolt", ArtMotion::Projectile, ArtAt::Target, 1.3f, 0.3f);
    else if (k == "isik_carpmasi")
        add("light_strike", ArtMotion::Strike, ArtAt::Target, 1.0f, 0.45f, 0.9f);
    else if (k == "kor_yagmuru")
    {
        add("ember_mote", ArtMotion::Fall, ArtAt::Target, radius, 1.0f, 1.0f, {1.0f, 1.0f, 1.0f}, 10);
        add("ground_ring_flame", ArtMotion::Ring, ArtAt::Target, radius, 1.0f, 0.8f);
    }
    else if (k == "alev_halkasi")
        add("ground_ring_flame", ArtMotion::Ring, ArtAt::Caster, radius, 0.9f, 0.9f);
    else if (k == "donduran_nefes")
        add("ground_ring_frost", ArtMotion::Ring, ArtAt::Target, radius, 0.6f, 0.9f);
    else if (k == "halka_sifasi")
        add("ground_ring_heal", ArtMotion::Ring, ArtAt::Caster, radius, 0.8f, 0.85f);
    else if (k == "yenileyen_isik")
        add("ground_ring_heal", ArtMotion::Ring, ArtAt::Target, 1.2f, 0.8f, 0.85f);
    else if (k == "soluk_gunes")
        add("ground_ring_heal", ArtMotion::Ring, ArtAt::Target, 1.0f, 0.7f, 0.85f);
    else if (k == "sis_bombasi")
    {
        const float fog = effectSeconds(skill, game::EffectKind::Slow, rank, 6.0f);
        add("fog_cloud", ArtMotion::Cloud, ArtAt::Target, radius, fog, 0.42f, {0.85f, 1.0f, 0.8f});
        add("ground_ring_fog", ArtMotion::Ring, ArtAt::Target, radius, fog, 0.75f);
    }
    else if (k == "yarma")
        add("ground_ring_neutral", ArtMotion::Ring, ArtAt::Caster, radius, 0.5f, 0.7f, kWarm);
    else if (k == "kan_ofkesi")
        add("buff_aura_red", ArtMotion::Burst, ArtAt::Caster, 1.0f, 1.2f);
    else if (k == "ofkeli_dua")
        add("buff_aura_gold", ArtMotion::Burst, ArtAt::Target, 1.0f, 1.2f);
    else if (k == "kalkan_duvari")
    {
        add("shield_bubble_gold", ArtMotion::Burst, ArtAt::Caster, 1.0f, 0.6f, 0.5f);
        add("buff_aura_steel", ArtMotion::Burst, ArtAt::Caster, 1.0f, 1.2f);
    }
    else if (k == "meydan_okuma")
    {
        add("buff_aura_steel", ArtMotion::Burst, ArtAt::Caster, 1.0f, 1.2f);
        add("ground_ring_neutral", ArtMotion::Ring, ArtAt::Caster, radius, 0.6f, 0.8f, kSteel);
    }
    else if (k == "direncli_soluk")
        add("buff_aura_steel", ArtMotion::Burst, ArtAt::Caster, 1.0f, 1.2f);
    else if (k == "seri_eller" || k == "kacamak")
        add("buff_aura_green", ArtMotion::Burst, ArtAt::Caster, 1.0f, 1.2f);
    else if (k == "kutlu_siper")
        add("buff_aura_gold", ArtMotion::Burst, ArtAt::Target, 1.0f, 1.2f);
    else if (k == "buz_kabugu")
        add("ground_ring_frost", ArtMotion::Ring, ArtAt::Caster, 1.2f, 0.6f, 0.9f);
    else if (k == "baglayan_soz")
        add("ground_ring_neutral", ArtMotion::Ring, ArtAt::Target, 1.0f, 0.8f, 0.85f, kAmber);
    else if (k == "yakici_iz")
        add("tick_wisp_burn", ArtMotion::Wisp, ArtAt::Target, 1.0f, 0.6f);
    else if (k == "zehirli_kesik")
        add("tick_wisp_poison", ArtMotion::Wisp, ArtAt::Target, 1.0f, 0.6f);
    return out;
}

std::string_view tickArt(std::uint16_t skillId)
{
    const game::SkillDef* s = game::skills().find(skillId);
    if (s == nullptr)
        return {};
    for (const game::EffectDef& e : s->effects)
        if (e.kind == game::EffectKind::Dot)
            return s->charClass == 3 ? "tick_wisp_burn" : "tick_wisp_poison";
    return {};
}

std::optional<AuraArt> effectAura(std::uint16_t skillId, std::uint8_t kind)
{
    const game::SkillDef* s = game::skills().find(skillId);
    const std::string_view k = s != nullptr ? std::string_view(s->key) : std::string_view();
    switch (static_cast<game::EffectKind>(kind))
    {
    case game::EffectKind::Stun: return AuraArt{"stun_marker", AuraAt::Head, 1.7f, 3.1416f, 1.0f}; // larger than the manifest's 1.0: reads at range
    case game::EffectKind::Root: return AuraArt{"root_spikes", AuraAt::Feet, 1.0f, 0.0f, 1.0f};
    case game::EffectKind::Shield:
        // fix1: the ice shell reads solid-but-translucent: higher opacity (its texture alpha thins it) and a rim.
        if (k == "buz_kabugu")
            return AuraArt{"shield_bubble_frost", AuraAt::Feet, 1.0f, 0.12f, 0.8f, 1.0f};
        return AuraArt{"shield_bubble_neutral", AuraAt::Feet, 1.0f, 0.3f, 0.4f};
    case game::EffectKind::DefenseUp:
        if (k == "kutlu_siper")
            return AuraArt{"shield_bubble_gold", AuraAt::Feet, 1.0f, 0.3f, 0.35f};
        if (k == "kalkan_duvari")
            return AuraArt{"buff_aura_steel", AuraAt::Feet, 1.0f, 0.8f, 0.45f};
        return std::nullopt;
    case game::EffectKind::AttackUp:
        if (k == "kan_ofkesi")
            return AuraArt{"buff_aura_red", AuraAt::Feet, 1.0f, 0.8f, 0.45f};
        return AuraArt{"buff_aura_gold", AuraAt::Feet, 1.0f, 0.8f, 0.45f};
    case game::EffectKind::Haste:
    case game::EffectKind::DodgeUp: return AuraArt{"buff_aura_green", AuraAt::Feet, 1.0f, 0.8f, 0.45f};
    case game::EffectKind::Hot:
        return AuraArt{k == "direncli_soluk" ? "buff_aura_steel" : "buff_aura_gold", AuraAt::Feet, 1.0f, 0.8f, 0.4f};
    default: return std::nullopt;
    }
}

// ---------------------------------------------------------------------------------------------
// Input
// ---------------------------------------------------------------------------------------------

EscapeAction escapeActionWithSkills(bool lootOpen, bool skillsOpen, bool kunyeOpen, bool inventoryOpen, bool hasTarget,
                                    bool menuOpen) noexcept
{
    if (menuOpen)
        return EscapeAction::None;
    if (lootOpen)
        return EscapeAction::CloseLoot;
    if (skillsOpen)
        return EscapeAction::CloseSkills;
    if (kunyeOpen)
        return EscapeAction::CloseKunye;
    return escapeAction(false, inventoryOpen, hasTarget, false);
}

const game::SkillDef* parseSkillName(std::string_view s) noexcept
{
    std::uint32_t id = 0;
    const auto [p, ec] = std::from_chars(s.data(), s.data() + s.size(), id);
    if (ec == std::errc{} && p == s.data() + s.size())
        return id <= game::kMaxSkillId ? game::skills().find(static_cast<std::uint16_t>(id)) : nullptr;
    for (const game::SkillDef& d : game::skills().skills())
        if (d.key == s)
            return &d;
    return nullptr;
}

std::optional<std::uint8_t> parseEffectKind(std::string_view s) noexcept
{
    std::uint32_t n = 0;
    const auto [p, ec] = std::from_chars(s.data(), s.data() + s.size(), n);
    if (ec == std::errc{} && p == s.data() + s.size())
    {
        if (n >= 1 && n <= game::kEffectKindCount)
            return static_cast<std::uint8_t>(n);
        return std::nullopt;
    }
    if (const auto k = game::effectKindFromKey(s))
        return static_cast<std::uint8_t>(*k);
    return std::nullopt;
}

// ---------------------------------------------------------------------------------------------
// Automation
// ---------------------------------------------------------------------------------------------

namespace {

std::vector<std::string_view> splitTokens(std::string_view s)
{
    std::vector<std::string_view> out;
    std::size_t i = 0;
    while (i < s.size())
    {
        while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\r'))
            ++i;
        const std::size_t start = i;
        while (i < s.size() && s[i] != ' ' && s[i] != '\t' && s[i] != '\r')
            ++i;
        if (i > start)
            out.push_back(s.substr(start, i - start));
    }
    return out;
}

bool parseUnsigned(std::string_view s, std::uint32_t& out)
{
    const auto [p, ec] = std::from_chars(s.data(), s.data() + s.size(), out);
    return ec == std::errc{} && p == s.data() + s.size();
}

bool parseDouble(std::string_view s, double& out)
{
    const auto [p, ec] = std::from_chars(s.data(), s.data() + s.size(), out);
    return ec == std::errc{} && p == s.data() + s.size();
}

// The rest of the line from token i on (trailing blanks dropped).
std::string restOf(std::string_view line, std::string_view token)
{
    std::string s(line.substr(static_cast<std::size_t>(token.data() - line.data())));
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r'))
        s.pop_back();
    return s;
}

} // namespace

std::optional<SkillsCommand> parseSkillsCommand(std::string_view line, std::string& error)
{
    error.clear();
    const std::vector<std::string_view> t = splitTokens(line);
    if (t.empty())
        return std::nullopt;
    const std::string_view verb = t[0];
    const std::size_t n = t.size() - 1;
    SkillsCommand c;
    const auto bad = [&](std::string_view usage) -> std::optional<SkillsCommand> {
        error = std::format("usage: {}", usage);
        return std::nullopt;
    };
    const auto skill = [&](std::string_view s) {
        c.skill = parseSkillName(s);
        return c.skill != nullptr;
    };
    const auto slotNumber = [&](std::string_view s) { return parseUnsigned(s, c.a) && c.a >= 1 && c.a <= 8; };
    if (verb == "skills")
    {
        if (n != 1 || (t[1] != "open" && t[1] != "close"))
            return bad("skills open|close");
        c.verb = SkillsVerb::Window;
        c.mode = std::string(t[1]);
        return c;
    }
    if (verb == "skilllearn")
    {
        if (n != 1 || !skill(t[1]))
            return bad("skilllearn <skill id|key>");
        c.verb = SkillsVerb::Learn;
        return c;
    }
    if (verb == "barset")
    {
        if (n != 2 || !slotNumber(t[1]) || (t[2] != "none" && !skill(t[2])))
            return bad("barset <slot 1-8> <skill id|key|none>");
        c.verb = SkillsVerb::BarSet;
        return c;
    }
    if (verb == "useslot")
    {
        if (n != 1 || !slotNumber(t[1]))
            return bad("useslot <slot 1-8>");
        c.verb = SkillsVerb::UseSlot;
        return c;
    }
    if (verb == "expectrank")
    {
        if (n != 2 || !skill(t[1]) || !parseUnsigned(t[2], c.a) || c.a > 3)
            return bad("expectrank <skill> <0-3>");
        c.verb = SkillsVerb::ExpectRank;
        return c;
    }
    if (verb == "expectskillpoints")
    {
        if (n != 1 || !parseUnsigned(t[1], c.a))
            return bad("expectskillpoints <unspent>");
        c.verb = SkillsVerb::ExpectPoints;
        return c;
    }
    if (verb == "expectslot")
    {
        if (n != 2 || !slotNumber(t[1]) || (t[2] != "empty" && !skill(t[2])))
            return bad("expectslot <slot 1-8> <skill|empty>");
        c.verb = SkillsVerb::ExpectSlot;
        return c;
    }
    if (verb == "expectslotlook")
    {
        if (n != 2 || !slotNumber(t[1]) ||
            (t[2] != "ready" && t[2] != "nomana" && t[2] != "notarget" && t[2] != "dead" && t[2] != "empty"))
            return bad("expectslotlook <slot 1-8> ready|nomana|notarget|dead|empty");
        c.verb = SkillsVerb::ExpectSlotLook;
        c.mode = std::string(t[2]);
        return c;
    }
    if (verb == "expectcooldown")
    {
        if (n != 3 || !skill(t[1]) || !parseDouble(t[2], c.lo) || !parseDouble(t[3], c.hi))
            return bad("expectcooldown <skill> <minSeconds> <maxSeconds>");
        c.verb = SkillsVerb::ExpectCooldown;
        return c;
    }
    if (verb == "expecteffect" || verb == "expectnoeffect")
    {
        const std::string_view usage = verb == "expecteffect" ? "expecteffect self|target <kind> [skill]"
                                                              : "expectnoeffect self|target <kind> [skill]";
        if (n < 2 || n > 3 || (t[1] != "self" && t[1] != "target"))
            return bad(usage);
        const auto k = parseEffectKind(t[2]);
        if (!k || (n == 3 && !skill(t[3])))
            return bad(usage);
        c.kind = *k;
        c.mode = std::string(t[1]);
        c.verb = verb == "expecteffect" ? SkillsVerb::ExpectEffect : SkillsVerb::ExpectNoEffect;
        return c;
    }
    if (verb == "expecteffecttip")
    {
        constexpr std::string_view usage = "expecteffecttip self|target <kind> <text>";
        if (n < 3 || (t[1] != "self" && t[1] != "target"))
            return bad(usage);
        const auto k = parseEffectKind(t[2]);
        if (!k)
            return bad(usage);
        c.kind = *k;
        c.mode = std::string(t[1]);
        c.text = restOf(line, t[3]);
        c.verb = SkillsVerb::ExpectEffectTip;
        return c;
    }
    if (verb == "expectart")
    {
        if (n != 2 || !parseUnsigned(t[2], c.a))
            return bad("expectart <mesh> <min count>");
        c.text = std::string(t[1]);
        c.verb = SkillsVerb::ExpectArt;
        return c;
    }
    if (verb == "expectskillmessage")
    {
        if (n == 0)
            return bad("expectskillmessage <text>");
        c.text = restOf(line, t[1]);
        c.verb = SkillsVerb::ExpectMessage;
        return c;
    }
    if (verb == "pullpack")
    {
        constexpr std::string_view usage = "pullpack <monster name> <count>";
        if (n < 2 || !parseUnsigned(t[n], c.a) || c.a == 0 || c.a > 8)
            return bad(usage);
        const std::string_view first = t[1];
        const std::string_view last = t[n - 1];
        c.text = std::string(line.substr(static_cast<std::size_t>(first.data() - line.data()),
                                         static_cast<std::size_t>(last.data() + last.size() - first.data())));
        c.verb = SkillsVerb::PullPack;
        return c;
    }
    if (verb == "skilllog")
    {
        if (n != 0)
            return bad("skilllog");
        c.verb = SkillsVerb::Log;
        return c;
    }
    return std::nullopt;
}

} // namespace vo::client::world
