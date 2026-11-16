#include "client/world/LootView.h"

#include "shared/game/Character.h"

#include <algorithm>
#include <charconv>
#include <format>
#include <map>

namespace vo::client::world {

namespace vn = vo::net;

LootLabel lootLabel(std::uint8_t entityState) noexcept
{
    if ((entityState & vn::kEntityStateLootMine) != 0)
        return LootLabel::Mine;
    if ((entityState & vn::kEntityStateLootLocked) != 0)
        return LootLabel::Locked;
    return LootLabel::Free;
}

const char* lootLabelName(LootLabel l) noexcept
{
    switch (l)
    {
    case LootLabel::Mine: return "mine";
    case LootLabel::Locked: return "locked";
    case LootLabel::Free: return "free";
    }
    return "?";
}

LootLabelStyle lootLabelStyle(LootLabel l) noexcept
{
    LootLabelStyle s;
    switch (l)
    {
    case LootLabel::Mine: // warm gold, a breathing gold ring
        s.labelRgb = 0xF2C14E;
        s.labelAlpha = 1.0f;
        s.ringRgb = 0xF5C04A;
        s.ringAlpha = 0.9f;
        s.ringPulse = true;
        s.meshTint = 1.0f;
        break;
    case LootLabel::Locked: // dim grey, a faint grey ring, the bag itself darkened
        s.labelRgb = 0x8F8A82;
        s.labelAlpha = 0.8f;
        s.ringRgb = 0x7C7A76;
        s.ringAlpha = 0.35f;
        s.ringPulse = false;
        s.meshTint = 0.5f;
        break;
    case LootLabel::Free: // pale parchment, a soft warm ring
        s.labelRgb = 0xF1E4C2;
        s.labelAlpha = 1.0f;
        s.ringRgb = 0xE8C27A;
        s.ringAlpha = 0.55f;
        s.ringPulse = false;
        s.meshTint = 1.0f;
        break;
    }
    return s;
}

std::string formatThousands(std::uint64_t value)
{
    const std::string digits = std::to_string(value);
    std::string out;
    out.reserve(digits.size() + digits.size() / 3);
    const std::size_t lead = digits.size() % 3;
    for (std::size_t i = 0; i < digits.size(); ++i)
    {
        if (i != 0 && (i + 3 - lead) % 3 == 0)
            out += '.';
        out += digits[i];
    }
    return out;
}

std::string_view lootResultText(vn::LootResult r) noexcept
{
    // docs/PROTOCOL_LOOT.md §9.
    switch (r)
    {
    case vn::LootResult::Ok: return "";
    case vn::LootResult::NotFound: return "Ganimet bulunamadı";
    case vn::LootResult::YouAreDead: return "Ölüyken ganimet alınmaz";
    case vn::LootResult::OutOfRange: return "Çok uzaktasın";
    case vn::LootResult::NotYours: return "Bu ganimet henüz başkasının";
    case vn::LootResult::AlreadyTaken: return "Bu çoktan alındı";
    case vn::LootResult::BagFull: return "Çantan dolu";
    case vn::LootResult::PurseFull: return "Kesen dolu";
    case vn::LootResult::ServerError: return "Ganimet alınamadı, tekrar dene";
    }
    return "Ganimet alınamadı";
}

std::string_view lootClosedText(vn::LootClosedReason r) noexcept
{
    switch (r)
    {
    case vn::LootClosedReason::Gone: return "";
    case vn::LootClosedReason::OutOfRange: return "Ganimetten uzaklaştın";
    case vn::LootClosedReason::YouDied: return "";
    }
    return "";
}

std::uint32_t rarityRgb(game::Rarity r) noexcept
{
    switch (r)
    {
    case game::Rarity::Common: return 0xEDE6D3;
    case game::Rarity::Uncommon: return 0x8FD06A;
    case game::Rarity::Rare: return 0x6FAEF5;
    }
    return 0xEDE6D3;
}

// ---- LootWindowModel ----

void LootWindowModel::open(std::uint32_t lootId)
{
    if (lootId == m_lootId)
        return;
    m_lootId = lootId;
    m_hasContents = false;
    m_coins = 0;
    m_entries.clear();
}

void LootWindowModel::close() noexcept
{
    m_lootId = 0;
    m_hasContents = false;
    m_coins = 0;
    m_entries.clear();
}

bool LootWindowModel::applyContents(const vn::LootContents& m)
{
    if (m.lootId == 0 || m.lootId != m_lootId)
        return false;
    m_hasContents = true;
    m_coins = m.coins;
    m_entries = m.entries;
    std::sort(m_entries.begin(), m_entries.end(),
              [](const vn::LootEntry& a, const vn::LootEntry& b) { return a.index < b.index; });
    return true;
}

std::vector<LootRow> LootWindowModel::rows() const
{
    std::vector<LootRow> out;
    out.reserve(m_entries.size() + 1);
    if (m_coins > 0)
        out.push_back({true, 0, 0, m_coins});
    for (const vn::LootEntry& e : m_entries)
        out.push_back({false, e.index, e.templateId, e.quantity});
    return out;
}

std::optional<LootRow> LootWindowModel::row(std::uint8_t what) const
{
    if (what == vn::kLootTakeCoins)
    {
        if (m_coins == 0)
            return std::nullopt;
        return LootRow{true, 0, 0, m_coins};
    }
    for (const vn::LootEntry& e : m_entries)
        if (e.index == what)
            return LootRow{false, e.index, e.templateId, e.quantity};
    return std::nullopt;
}

std::uint32_t LootWindowModel::beginTake(std::uint8_t what)
{
    const std::uint32_t seq = m_nextSeq++;
    if (m_nextSeq == 0)
        m_nextSeq = 1;
    m_takes.push_back({seq, m_lootId, what});
    return seq;
}

std::optional<LootWindowModel::Take> LootWindowModel::finishTake(std::uint32_t seq)
{
    for (auto it = m_takes.begin(); it != m_takes.end(); ++it)
    {
        if (it->seq != seq)
            continue;
        const Take t = *it;
        m_takes.erase(it);
        return t;
    }
    return std::nullopt;
}

// ---- Pickup feedback ----

std::vector<ItemGain> inventoryGains(const game::Inventory& before, const game::Inventory& after)
{
    std::map<std::uint16_t, std::int64_t> delta;
    for (const game::StoredItem& s : before.items())
        delta[s.templateId] -= s.quantity;
    for (const game::StoredItem& s : after.items())
        delta[s.templateId] += s.quantity;
    std::vector<ItemGain> out;
    for (const auto& [id, d] : delta)
        if (d > 0)
            out.push_back({id, static_cast<std::uint32_t>(d)});
    return out;
}

std::string pickupText(const game::ItemTemplate* item, std::uint16_t templateId, std::uint32_t units)
{
    const std::string name = item != nullptr ? item->name : std::format("Eşya #{}", templateId);
    if (units > 1)
        return std::format("{} x{} alındı", name, units);
    return std::format("{} alındı", name);
}

std::string coinPickupText(std::uint32_t gained)
{
    return std::format("+{} Akçe", formatThousands(gained));
}

// ---- Tooltip ----

namespace {

std::string classList(const game::ItemTemplate& t)
{
    std::string out;
    for (std::uint8_t c = 1; c <= 4; ++c)
    {
        if (!t.allowsClass(c))
            continue;
        if (!out.empty())
            out += ", ";
        out += game::className(static_cast<game::CharClass>(c));
    }
    return out.empty() ? std::string("—") : out;
}

} // namespace

std::vector<TipLine> itemTipLines(const game::ItemTemplate& item, const game::ItemUser& user, std::uint32_t quantity)
{
    std::vector<TipLine> out;
    out.push_back({item.name, TipTone::Title, rarityRgb(item.rarity), false});
    const std::string_view slot = game::slotRuleDisplayName(item.slot);
    if (slot.empty())
        out.push_back({std::format("{} · {}", game::itemKindDisplayName(item.kind), game::rarityDisplayName(item.rarity)),
                       TipTone::Dim, 0, false});
    else
        out.push_back({std::format("{} · {} · {}", game::itemKindDisplayName(item.kind), slot,
                                   game::rarityDisplayName(item.rarity)),
                       TipTone::Dim, 0, false});
    if (item.isStackable() && quantity > 0)
        out.push_back({std::format("Adet: {} / {}", quantity, item.maxStack), TipTone::Normal, 0, false});
    if (item.attack != 0)
        out.push_back({std::format("Saldırı +{}", item.attack), TipTone::Good, 0, false});
    if (item.defense != 0)
        out.push_back({std::format("Savunma +{}", item.defense), TipTone::Good, 0, false});
    // M11: attribute bonuses while equipped ("Dayanıklılık +3").
    for (std::size_t i = 0; i < game::kAttributeCount; ++i)
        if (item.attributes[i] != 0)
            out.push_back({std::format("{} +{}", game::attributeDisplayName(static_cast<game::Attribute>(i)),
                                       item.attributes[i]),
                           TipTone::Good, 0, false});
    if (item.kind != game::ItemKind::Material)
    {
        out.push_back({"Sınıf: " + classList(item), item.allowsClass(user.charClass) ? TipTone::Normal : TipTone::Bad,
                       0, true});
        out.push_back({std::format("Gereken seviye: {}", item.level),
                       user.level >= item.level ? TipTone::Normal : TipTone::Bad, 0, false});
        // M11: base attribute requirements ("Gerekli Güç: 20", red when the base value is lower).
        for (std::size_t i = 0; i < game::kAttributeCount; ++i)
            if (item.requirements[i] != 0)
                out.push_back({std::format("Gerekli {}: {}", game::attributeDisplayName(static_cast<game::Attribute>(i)),
                                           item.requirements[i]),
                               user.attributes[i] >= item.requirements[i] ? TipTone::Normal : TipTone::Bad, 0, false});
    }
    if (item.value > 0)
    {
        if (item.isStackable() && quantity > 1)
            out.push_back({std::format("Değer: {} Akçe (yığın {} Akçe)", formatThousands(item.value),
                                       formatThousands(static_cast<std::uint64_t>(item.value) * quantity)),
                           TipTone::Value, 0, false});
        else
            out.push_back({std::format("Değer: {} Akçe", formatThousands(item.value)), TipTone::Value, 0, false});
    }
    if (!item.description.empty())
        out.push_back({item.description, TipTone::Dim, 0, true});
    return out;
}

// ---- Input ----

EscapeAction escapeAction(bool lootOpen, bool inventoryOpen, bool hasTarget, bool menuOpen) noexcept
{
    if (menuOpen)
        return EscapeAction::None;
    if (lootOpen)
        return EscapeAction::CloseLoot;
    if (inventoryOpen)
        return EscapeAction::CloseInventory;
    if (hasTarget)
        return EscapeAction::ClearTarget;
    return EscapeAction::OpenMenu;
}

std::uint32_t nearestLoot(std::span<const LootCandidate> candidates, Vec3 from, float maxDistance,
                          bool includeLocked) noexcept
{
    std::uint32_t best = 0;
    float bestD = 0.0f;
    for (const LootCandidate& c : candidates)
    {
        if (c.id == 0 || (!includeLocked && lootLabel(c.state) == LootLabel::Locked))
            continue;
        const float d = horizontalDistance(c.position, from);
        if (d > maxDistance)
            continue;
        if (best == 0 || d < bestD || (d == bestD && c.id < best))
        {
            best = c.id;
            bestD = d;
        }
    }
    return best;
}

// ---- Automation ----

bool compare(CompareOp op, std::uint64_t value, std::uint64_t n) noexcept
{
    switch (op)
    {
    case CompareOp::Eq: return value == n;
    case CompareOp::Ge: return value >= n;
    case CompareOp::Le: return value <= n;
    case CompareOp::Gt: return value > n;
    case CompareOp::Lt: return value < n;
    }
    return false;
}

const char* compareOpText(CompareOp op) noexcept
{
    switch (op)
    {
    case CompareOp::Eq: return "=";
    case CompareOp::Ge: return ">=";
    case CompareOp::Le: return "<=";
    case CompareOp::Gt: return ">";
    case CompareOp::Lt: return "<";
    }
    return "?";
}

namespace {

std::vector<std::string_view> tokens(std::string_view s)
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

bool unsignedNumber(std::string_view s, std::uint32_t& out)
{
    const auto [p, ec] = std::from_chars(s.data(), s.data() + s.size(), out);
    return ec == std::errc{} && p == s.data() + s.size();
}

std::optional<CompareOp> parseOp(std::string_view s)
{
    if (s == "=" || s == "==")
        return CompareOp::Eq;
    if (s == ">=")
        return CompareOp::Ge;
    if (s == "<=")
        return CompareOp::Le;
    if (s == ">")
        return CompareOp::Gt;
    if (s == "<")
        return CompareOp::Lt;
    return std::nullopt;
}

} // namespace

std::optional<LootCommand> parseLootCommand(std::string_view line, std::string& error)
{
    error.clear();
    const std::vector<std::string_view> t = tokens(line);
    if (t.empty())
        return std::nullopt;
    const std::string_view verb = t[0];
    const std::span<const std::string_view> args(t.data() + 1, t.size() - 1);
    LootCommand c;
    const auto bad = [&](std::string_view usage) -> std::optional<LootCommand> {
        error = std::format("usage: {}", usage);
        return std::nullopt;
    };
    if (verb == "lootnearest")
    {
        if (args.size() > 1 || (args.size() == 1 && args[0] != "any"))
            return bad("lootnearest [any]");
        c.verb = LootVerb::Nearest;
        c.mode = args.empty() ? std::string() : std::string(args[0]);
        return c;
    }
    if (verb == "walktoloot")
    {
        c.mode = args.empty() ? std::string("any") : std::string(args[0]);
        if (args.size() > 1 || (c.mode != "any" && c.mode != "mine" && c.mode != "locked" && c.mode != "free"))
            return bad("walktoloot [mine|locked|free|any]");
        c.verb = LootVerb::WalkTo;
        return c;
    }
    if (verb == "takeall" || verb == "lootclose" || verb == "lootlog")
    {
        if (!args.empty())
            return bad(verb);
        c.verb = verb == "takeall" ? LootVerb::TakeAll : verb == "lootclose" ? LootVerb::Close : LootVerb::Log;
        return c;
    }
    if (verb == "take")
    {
        if (args.size() != 1)
            return bad("take <entry index 0..7>|coins");
        c.verb = LootVerb::Take;
        if (args[0] == "coins")
        {
            c.a = vn::kLootTakeCoins;
            return c;
        }
        if (!unsignedNumber(args[0], c.a) || c.a >= vn::kMaxLootEntries)
            return bad("take <entry index 0..7>|coins");
        return c;
    }
    if (verb == "expectcoins")
    {
        if (args.size() == 1 && unsignedNumber(args[0], c.a))
        {
            c.verb = LootVerb::ExpectCoins;
            return c;
        }
        const auto op = args.size() == 2 ? parseOp(args[0]) : std::nullopt;
        if (!op || !unsignedNumber(args[1], c.a))
            return bad("expectcoins [= | >= | <= | > | <] <n>");
        c.verb = LootVerb::ExpectCoins;
        c.op = *op;
        return c;
    }
    if (verb == "expectitem")
    {
        if (args.size() == 2 && unsignedNumber(args[0], c.a) && unsignedNumber(args[1], c.b) && c.a > 0)
        {
            c.verb = LootVerb::ExpectItem;
            return c;
        }
        const auto op = args.size() == 3 ? parseOp(args[1]) : std::nullopt;
        if (!op || !unsignedNumber(args[0], c.a) || c.a == 0 || !unsignedNumber(args[2], c.b))
            return bad("expectitem <templateId> [= | >= | <= | > | <] <units>");
        c.verb = LootVerb::ExpectItem;
        c.op = *op;
        return c;
    }
    if (verb == "expectstack")
    {
        if (args.size() != 3 || !unsignedNumber(args[0], c.a) || c.a >= game::kBagSlots || !unsignedNumber(args[1], c.b) ||
            c.b == 0 || !unsignedNumber(args[2], c.c) || c.c == 0)
            return bad("expectstack <bag slot 0..27> <templateId> <quantity>");
        c.verb = LootVerb::ExpectStack;
        return c;
    }
    if (verb == "expectlootrows")
    {
        if (args.size() != 1 || !unsignedNumber(args[0], c.a))
            return bad("expectlootrows <n>");
        c.verb = LootVerb::ExpectRows;
        return c;
    }
    if (verb == "expectlootmessage")
    {
        if (args.empty())
            return bad("expectlootmessage <text>");
        c.verb = LootVerb::ExpectMessage;
        for (const std::string_view a : args)
        {
            if (!c.text.empty())
                c.text += ' ';
            c.text += a;
        }
        return c;
    }
    if (verb == "lootfarm")
    {
        c.verb = LootVerb::Farm;
        if (args.size() == 1 && args[0] == "stop")
        {
            c.mode = "stop";
            return c;
        }
        if (args.size() >= 3 && args[args.size() - 2] == "rows")
        {
            // lootfarm <monster name> rows <n>: stop with the window open on a bag of >= n rows.
            if (!unsignedNumber(args.back(), c.b) || c.b == 0 || c.b > vn::kMaxLootEntries + 1)
                return bad("lootfarm <monster name> rows <1..9>");
            c.mode = "rows";
        }
        else if (args.size() < 3 || !unsignedNumber(args[args.size() - 2], c.a) || c.a == 0 ||
                 !unsignedNumber(args.back(), c.b) || c.b == 0)
            return bad("lootfarm <monster name> <templateId> <pickups> | lootfarm <monster name> rows <n> | lootfarm stop");
        for (std::size_t i = 0; i + 2 < args.size(); ++i)
        {
            if (!c.text.empty())
                c.text += ' ';
            c.text += args[i];
        }
        return c;
    }
    if (verb == "movestack")
    {
        if (args.size() != 2 || !unsignedNumber(args[0], c.a) || c.a == 0 || !unsignedNumber(args[1], c.b) ||
            c.b >= game::kBagSlots)
            return bad("movestack <templateId> <bag slot 0..27>");
        c.verb = LootVerb::MoveStack;
        return c;
    }
    return std::nullopt; // not a loot verb
}

} // namespace vo::client::world
