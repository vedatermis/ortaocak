#include "client/world/StatsView.h"

#include "shared/game/Character.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <format>

namespace vo::client::world {

namespace vn = vo::net;

// ---------------------------------------------------------------------------------------------
// StatSheet
// ---------------------------------------------------------------------------------------------

void StatSheet::reset() noexcept
{
    *this = StatSheet{};
}

void StatSheet::applyCharStats(const vn::CharStats& m)
{
    if (m_awaitingStats)
    {
        // The accepted points are part of `base` now.
        m_pending = {};
        m_awaitingStats = false;
    }
    m_stats = m;
    m_known = true;
    trimPending();
}

game::Attributes StatSheet::baseValues() const noexcept
{
    game::Attributes out{};
    for (std::size_t i = 0; i < game::kAttributeCount; ++i)
        out[i] = m_stats.attributes[i].base;
    return out;
}

game::Attributes StatSheet::bonusValues() const noexcept
{
    game::Attributes out{};
    for (std::size_t i = 0; i < game::kAttributeCount; ++i)
        out[i] = m_stats.attributes[i].bonus;
    return out;
}

game::Attributes StatSheet::allocated(std::uint8_t charClass) const noexcept
{
    const game::Attributes start = game::classStartAttributes(charClass);
    game::Attributes out{};
    for (std::size_t i = 0; i < game::kAttributeCount; ++i)
        out[i] = m_stats.attributes[i].base > start[i] ? static_cast<std::uint16_t>(m_stats.attributes[i].base - start[i])
                                                       : std::uint16_t{0};
    return out;
}

std::uint32_t StatSheet::pendingTotal() const noexcept
{
    std::uint32_t sum = 0;
    for (const std::uint16_t p : m_pending)
        sum += p;
    return sum;
}

std::uint16_t StatSheet::remaining() const noexcept
{
    const std::uint32_t total = pendingTotal();
    return total >= m_stats.unspent ? std::uint16_t{0} : static_cast<std::uint16_t>(m_stats.unspent - total);
}

bool StatSheet::canAdd(std::size_t i) const noexcept
{
    if (i >= game::kAttributeCount || !m_known || busy() || remaining() == 0)
        return false;
    return static_cast<std::uint32_t>(m_stats.attributes[i].base) + m_pending[i] < game::kAttributeCap;
}

bool StatSheet::canRemove(std::size_t i) const noexcept
{
    return i < game::kAttributeCount && !busy() && m_pending[i] > 0;
}

std::uint16_t StatSheet::add(std::size_t i, std::uint16_t n) noexcept
{
    if (!canAdd(i))
        return 0;
    const std::uint32_t room = game::kAttributeCap - (static_cast<std::uint32_t>(m_stats.attributes[i].base) + m_pending[i]);
    const auto added = static_cast<std::uint16_t>(std::min<std::uint32_t>({n, remaining(), room}));
    m_pending[i] = static_cast<std::uint16_t>(m_pending[i] + added);
    return added;
}

std::uint16_t StatSheet::remove(std::size_t i, std::uint16_t n) noexcept
{
    if (!canRemove(i))
        return 0;
    const std::uint16_t removed = std::min(n, m_pending[i]);
    m_pending[i] = static_cast<std::uint16_t>(m_pending[i] - removed);
    return removed;
}

bool StatSheet::clearPending() noexcept
{
    if (busy() || !hasPending())
        return false;
    m_pending = {};
    return true;
}

std::optional<vn::StatAllocateReq> StatSheet::beginConfirm(double now)
{
    (void)now;
    if (!m_known || busy() || !hasPending())
        return std::nullopt;
    vn::StatAllocateReq req;
    m_seq = m_nextSeq++;
    if (m_nextSeq == 0)
        m_nextSeq = 1;
    req.seq = m_seq;
    req.points = m_pending;
    m_inFlight = true;
    return req;
}

StatSheet::Answer StatSheet::finish(const vn::StatAllocateRes& m, double now)
{
    Answer a;
    a.result = m.result;
    if (!m_inFlight || m.seq != m_seq)
        return a;
    a.matched = true;
    m_inFlight = false;
    if (m.result == vn::StatAllocateResult::Ok)
    {
        // Shown as the preview until CHAR_STATS (next world tick) carries the new base values.
        m_awaitingStats = true;
        m_answeredAt = now;
        return a;
    }
    m_stats.unspent = m.unspent; // unchanged on failure: the server's view
    trimPending();
    return a;
}

void StatSheet::expire(double now, double timeout) noexcept
{
    if (m_awaitingStats && now - m_answeredAt > timeout)
    {
        m_awaitingStats = false;
        m_pending = {};
    }
}

void StatSheet::trimPending() noexcept
{
    std::uint32_t budget = m_stats.unspent;
    for (std::size_t i = 0; i < game::kAttributeCount; ++i)
    {
        const std::uint32_t base = m_stats.attributes[i].base;
        const std::uint32_t room = base >= game::kAttributeCap ? 0u : game::kAttributeCap - base;
        const auto keep = std::min<std::uint32_t>({m_pending[i], room, budget});
        m_pending[i] = static_cast<std::uint16_t>(keep);
        budget -= keep;
    }
}

game::Stats StatSheet::preview(std::uint8_t charClass, game::EquipBonus equip) const noexcept
{
    game::Attributes alloc = allocated(charClass);
    for (std::size_t i = 0; i < game::kAttributeCount; ++i)
        alloc[i] = static_cast<std::uint16_t>(alloc[i] + m_pending[i]);
    equip.attributes = bonusValues();
    return game::deriveStats(charClass, m_stats.level, alloc, equip);
}

game::Stats StatSheet::current(std::uint8_t charClass, game::EquipBonus equip) const noexcept
{
    equip.attributes = bonusValues();
    return game::deriveStats(charClass, m_stats.level, allocated(charClass), equip);
}

// ---------------------------------------------------------------------------------------------
// Text
// ---------------------------------------------------------------------------------------------

std::string_view statResultText(vn::StatAllocateResult r) noexcept
{
    switch (r)
    {
    case vn::StatAllocateResult::Ok: return {};
    case vn::StatAllocateResult::InvalidAmount: return "Dağıtılacak puan seçilmedi";
    case vn::StatAllocateResult::NotEnoughPoints: return "Yeterli puanın yok";
    case vn::StatAllocateResult::AttributeCap: return "Bu nitelik en yüksek değerde (255)";
    case vn::StatAllocateResult::ServerError: return "Puanlar kaydedilemedi, tekrar dene";
    }
    return "Puanlar kaydedilemedi, tekrar dene";
}

std::string attributeTooltip(game::Attribute a, std::uint8_t charClass)
{
    std::string out;
    switch (a)
    {
    case game::Attribute::Strength: out = "Savaşçı ve Şifacı saldırı gücünü artırır."; break;
    case game::Attribute::Dexterity: out = "Haydut saldırı gücünü, kritik vuruş ve kaçınma şansını artırır."; break;
    case game::Attribute::Vitality: out = "Canı, savunmayı ve can yenilenmesini artırır."; break;
    case game::Attribute::Intelligence: out = "Büyücü büyü gücünü, manayı ve iyileştirmeyi artırır."; break;
    case game::Attribute::Spirit: out = "Manayı, mana yenilenmesini ve iyileştirmeyi artırır."; break;
    }
    const game::ClassTable* table = game::classTable(charClass);
    const std::string_view cls = game::isValidClass(charClass) ? game::className(static_cast<game::CharClass>(charClass))
                                                               : std::string_view("Savaşçı");
    const game::Attribute attack = table != nullptr ? table->attackAttribute : game::Attribute::Strength;
    const bool mage = charClass == static_cast<std::uint8_t>(game::CharClass::Mage);
    const bool priest = charClass == static_cast<std::uint8_t>(game::CharClass::Priest);
    if (a == attack)
        out += std::format(" {} olarak {} gücün bu niteliğe bağlı.", cls, mage ? "büyü" : "saldırı");
    else if (a == game::Attribute::Dexterity)
        out += std::format(" {} olarak saldırı gücünü değil, kritik vuruş ve kaçınma şansını artırır.", cls);
    else if ((a == game::Attribute::Intelligence || a == game::Attribute::Spirit) && priest)
        out += " Şifacı olarak Soluk Güneş'in iyileştirmesini de artırır.";
    else if (a == game::Attribute::Strength || a == game::Attribute::Intelligence)
        out += std::format(" {} olarak saldırı gücünü artırmaz.", cls);
    return out;
}

std::string formatDecimal(double value)
{
    std::string s = std::format("{:.1f}", value);
    std::replace(s.begin(), s.end(), '.', ',');
    return s;
}

std::string unspentText(std::uint16_t n)
{
    return std::format("Dağıtılmamış puan: {}", n);
}

std::string levelUpPointsText(std::uint32_t points)
{
    return std::format("+{} nitelik puanı  ·  Künye: C", points);
}

std::string attributeRowText(game::Attribute a, std::uint16_t base, std::uint16_t bonus, std::uint16_t pending)
{
    std::string out = std::format("{} {}", game::attributeDisplayName(a), base);
    if (bonus > 0)
        out += std::format(" (+{})", bonus);
    if (pending > 0)
        out += std::format(" +{}", pending);
    return out;
}

vn::CharStats toCharStats(const game::Stats& s, std::uint16_t level)
{
    vn::CharStats m;
    m.level = level;
    for (std::size_t i = 0; i < game::kAttributeCount; ++i)
        m.attributes[i] = {std::min(s.base[i], game::kAttributeCap), s.bonus[i]};
    m.unspent = s.unspent;
    m.maxHp = s.maxHp;
    m.maxMp = s.maxMp;
    m.attack = s.attack;
    m.defense = s.defense;
    m.critPermille = s.critPermille;
    m.dodgePermille = s.dodgePermille;
    const auto tenths = [](float perSecond) {
        return static_cast<std::uint16_t>(std::clamp(std::lround(perSecond * 10.0f), 0L, 65535L));
    };
    m.hpRegenTenths = tenths(s.hpRegen);
    m.mpRegenTenths = tenths(s.mpRegen);
    m.attackIntervalMs = static_cast<std::uint16_t>(std::min<std::uint32_t>(s.attackIntervalMs, 65535));
    const game::SkillDef* heal = game::findSkill(game::kSkillPaleSun);
    m.healPermille = heal != nullptr ? game::healPermille(*heal, s.healMultiplier) : 0;
    return m;
}

std::string_view derivedFieldKey(DerivedField f) noexcept
{
    switch (f)
    {
    case DerivedField::MaxHp: return "maxhp";
    case DerivedField::MaxMp: return "maxmp";
    case DerivedField::Attack: return "attack";
    case DerivedField::Defense: return "defense";
    case DerivedField::Crit: return "crit";
    case DerivedField::Dodge: return "dodge";
    case DerivedField::HpRegen: return "hpregen";
    case DerivedField::MpRegen: return "mpregen";
    case DerivedField::AttackSpeed: return "speed";
    case DerivedField::Heal: return "heal";
    }
    return "?";
}

std::optional<DerivedField> parseDerivedField(std::string_view key) noexcept
{
    for (std::size_t i = 0; i < kDerivedFieldCount; ++i)
    {
        const auto f = static_cast<DerivedField>(i);
        if (derivedFieldKey(f) == key)
            return f;
    }
    return std::nullopt;
}

double derivedValue(const vn::CharStats& s, DerivedField f) noexcept
{
    switch (f)
    {
    case DerivedField::MaxHp: return s.maxHp;
    case DerivedField::MaxMp: return s.maxMp;
    case DerivedField::Attack: return s.attack;
    case DerivedField::Defense: return s.defense;
    case DerivedField::Crit: return s.critPermille / 10.0;
    case DerivedField::Dodge: return s.dodgePermille / 10.0;
    case DerivedField::HpRegen: return s.hpRegenTenths / 10.0;
    case DerivedField::MpRegen: return s.mpRegenTenths / 10.0;
    case DerivedField::AttackSpeed: return s.attackIntervalMs / 1000.0;
    case DerivedField::Heal: return s.healPermille / 10.0;
    }
    return 0.0;
}

namespace {

std::string fieldText(const vn::CharStats& s, DerivedField f)
{
    switch (f)
    {
    case DerivedField::MaxHp: return std::to_string(s.maxHp);
    case DerivedField::MaxMp: return std::to_string(s.maxMp);
    case DerivedField::Attack: return std::to_string(s.attack);
    case DerivedField::Defense: return std::to_string(s.defense);
    case DerivedField::Crit:
    case DerivedField::Dodge:
    case DerivedField::Heal: return "%" + formatDecimal(derivedValue(s, f));
    case DerivedField::HpRegen:
    case DerivedField::MpRegen: return formatDecimal(derivedValue(s, f)) + "/sn";
    case DerivedField::AttackSpeed: return formatDecimal(derivedValue(s, f)) + " sn";
    }
    return {};
}

std::string_view fieldLabel(DerivedField f, bool mage)
{
    switch (f)
    {
    case DerivedField::MaxHp: return "Can";
    case DerivedField::MaxMp: return "Mana";
    case DerivedField::Attack: return mage ? "Büyü Gücü" : "Saldırı Gücü";
    case DerivedField::Defense: return "Savunma";
    case DerivedField::Crit: return "Kritik Vuruş";
    case DerivedField::Dodge: return "Kaçınma";
    case DerivedField::HpRegen: return "Can Yenilenmesi";
    case DerivedField::MpRegen: return "Mana Yenilenmesi";
    case DerivedField::AttackSpeed: return "Saldırı Hızı";
    case DerivedField::Heal: return "İyileştirme";
    }
    return {};
}

} // namespace

std::vector<DerivedRow> derivedRows(std::uint8_t charClass, const vn::CharStats& now, const vn::CharStats* preview)
{
    const bool mage = charClass == static_cast<std::uint8_t>(game::CharClass::Mage);
    const bool priest = charClass == static_cast<std::uint8_t>(game::CharClass::Priest);
    std::vector<DerivedRow> out;
    for (std::size_t i = 0; i < kDerivedFieldCount; ++i)
    {
        const auto f = static_cast<DerivedField>(i);
        if (f == DerivedField::Heal && !priest)
            continue;
        DerivedRow r;
        r.field = f;
        r.label = std::string(fieldLabel(f, mage));
        r.value = fieldText(now, f);
        if (preview != nullptr)
        {
            std::string p = fieldText(*preview, f);
            if (p != r.value)
                r.preview = std::move(p);
        }
        out.push_back(std::move(r));
    }
    return out;
}

std::string derivedRowsText(const std::vector<DerivedRow>& rows)
{
    std::string out;
    for (const DerivedRow& r : rows)
    {
        if (!out.empty())
            out += '\n';
        out += r.label + " " + r.value;
        if (!r.preview.empty())
            out += " → " + r.preview;
    }
    return out;
}

std::optional<game::Attribute> parseAttributeName(std::string_view s) noexcept
{
    struct Name
    {
        std::string_view key;
        std::string_view ascii;
        game::Attribute a;
    };
    static constexpr Name kNames[] = {
        {"strength", "guc", game::Attribute::Strength},
        {"dexterity", "ceviklik", game::Attribute::Dexterity},
        {"vitality", "dayaniklilik", game::Attribute::Vitality},
        {"intelligence", "zeka", game::Attribute::Intelligence},
        {"spirit", "irade", game::Attribute::Spirit},
    };
    for (const Name& n : kNames)
        if (s == n.key || s == n.ascii || s == game::attributeDisplayName(n.a))
            return n.a;
    return std::nullopt;
}

// ---------------------------------------------------------------------------------------------
// Input
// ---------------------------------------------------------------------------------------------

EscapeAction escapeActionWithKunye(bool lootOpen, bool kunyeOpen, bool inventoryOpen, bool hasTarget,
                                   bool menuOpen) noexcept
{
    if (menuOpen)
        return EscapeAction::None;
    if (lootOpen)
        return EscapeAction::CloseLoot;
    if (kunyeOpen)
        return EscapeAction::CloseKunye;
    return escapeAction(false, inventoryOpen, hasTarget, false);
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
        while (i < s.size() && (s[i] == ' ' || s[i] == '\t'))
            ++i;
        const std::size_t start = i;
        while (i < s.size() && s[i] != ' ' && s[i] != '\t')
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

} // namespace

std::optional<StatsCommand> parseStatsCommand(std::string_view line, std::string& error)
{
    error.clear();
    const std::vector<std::string_view> t = splitTokens(line);
    if (t.empty())
        return std::nullopt;
    const std::string_view verb = t[0];
    const std::size_t n = t.size() - 1;
    StatsCommand c;
    const auto bad = [&](std::string_view usage) -> std::optional<StatsCommand> {
        error = std::format("usage: {}", usage);
        return std::nullopt;
    };
    const auto attribute = [&](std::string_view s) {
        const auto a = parseAttributeName(s);
        if (a)
            c.attribute = *a;
        return a.has_value();
    };
    if (verb == "kunye")
    {
        if (n != 1 || (t[1] != "open" && t[1] != "close"))
            return bad("kunye open|close");
        c.verb = StatsVerb::Kunye;
        c.mode = std::string(t[1]);
        return c;
    }
    if (verb == "statadd" || verb == "statremove")
    {
        const std::string_view usage = verb == "statadd" ? "statadd <attribute> [n]" : "statremove <attribute> [n]";
        if (n < 1 || n > 2 || !attribute(t[1]))
            return bad(usage);
        c.a = 1;
        if (n == 2 && (!parseUnsigned(t[2], c.a) || c.a == 0 || c.a > 255))
            return bad(usage);
        c.verb = verb == "statadd" ? StatsVerb::Add : StatsVerb::Remove;
        return c;
    }
    if (verb == "expectattr")
    {
        constexpr std::string_view usage = "expectattr <attribute> <base> [bonus]";
        if (n < 2 || n > 3 || !attribute(t[1]) || !parseUnsigned(t[2], c.a))
            return bad(usage);
        if (n == 3)
        {
            std::uint32_t bonus = 0;
            if (!parseUnsigned(t[3], bonus) || bonus > 65535)
                return bad(usage);
            c.b = static_cast<std::int32_t>(bonus);
        }
        c.verb = StatsVerb::ExpectAttr;
        return c;
    }
    if (verb == "expectunspent")
    {
        if (n != 1 || !parseUnsigned(t[1], c.a))
            return bad("expectunspent <n>");
        c.verb = StatsVerb::ExpectUnspent;
        return c;
    }
    if (verb == "expectpending")
    {
        if (n != 2 || !attribute(t[1]) || !parseUnsigned(t[2], c.a))
            return bad("expectpending <attribute> <n>");
        c.verb = StatsVerb::ExpectPending;
        return c;
    }
    if (verb == "expectderived" || verb == "expectpreview")
    {
        const std::string_view usage = verb == "expectderived" ? "expectderived <field> <value>" : "expectpreview <field> <value>";
        const auto f = n == 2 ? parseDerivedField(t[1]) : std::nullopt;
        if (!f || !parseDouble(t[2], c.value))
            return bad(std::string(usage) + " (fields: maxhp maxmp attack defense crit dodge hpregen mpregen speed heal)");
        c.field = *f;
        c.verb = verb == "expectderived" ? StatsVerb::ExpectDerived : StatsVerb::ExpectPreview;
        return c;
    }
    if (verb == "expectstatmessage")
    {
        if (n == 0)
            return bad("expectstatmessage <text>");
        c.text = std::string(line.substr(static_cast<std::size_t>(t[1].data() - line.data())));
        while (!c.text.empty() && (c.text.back() == ' ' || c.text.back() == '\t' || c.text.back() == '\r'))
            c.text.pop_back();
        c.verb = StatsVerb::ExpectMessage;
        return c;
    }
    if (verb == "statlog")
    {
        if (n != 0)
            return bad("statlog");
        c.verb = StatsVerb::Log;
        return c;
    }
    return std::nullopt;
}

} // namespace vo::client::world
