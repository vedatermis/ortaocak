#include "client/world/ShopView.h"

#include "client/world/HudView.h"
#include "shared/assets/Terrain.h"
#include "shared/game/Character.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <format>

namespace vo::client::world {

namespace vn = vo::net;

// ---- NPCs ----

bool NpcEntity::sellsPotions() const noexcept
{
    // The art keys say it (npc_potion_* / npc_smith_*, docs/SHOP_ART.md §1).
    return def != nullptr && def->model.find("potion") != std::string::npos;
}

bool NpcBook::spawn(const vn::EntitySpawn& m, const game::NpcCatalog& catalog, const assets::TerrainData* terrain)
{
    if (m.entityType != vn::kEntityTypeNpc)
        return false;
    NpcEntity* e = find(m.entityId);
    if (e == nullptr)
        e = &m_npcs.emplace_back();
    ++m_spawns;
    e->id = m.entityId;
    e->npcId = m.templateId;
    e->def = catalog.find(m.templateId);
    e->name = m.name;
    Vec3 p{m.x, m.y, m.z};
    if (terrain != nullptr && terrain->resolution >= 2)
        p.y = terrain->heightAt(p.x, p.z);
    e->position = p;
    e->yaw = wrapAngle(m.yaw);
    e->age = 0.0f;
    e->interact = -1.0f;
    e->animatorMesh = nullptr; // rebound by the scene
    e->animatorClips = nullptr;
    return true;
}

bool NpcBook::despawn(std::uint32_t entityId)
{
    const auto it = std::find_if(m_npcs.begin(), m_npcs.end(), [&](const NpcEntity& n) { return n.id == entityId; });
    if (it == m_npcs.end())
        return false;
    m_npcs.erase(it);
    return true;
}

NpcEntity* NpcBook::find(std::uint32_t entityId) noexcept
{
    for (NpcEntity& n : m_npcs)
        if (n.id == entityId)
            return &n;
    return nullptr;
}

const NpcEntity* NpcBook::find(std::uint32_t entityId) const noexcept
{
    return const_cast<NpcBook*>(this)->find(entityId);
}

std::uint32_t NpcBook::nearest(Vec3 from, float maxDistance) const noexcept
{
    std::uint32_t best = 0;
    float bestD = 0.0f;
    for (const NpcEntity& n : m_npcs)
    {
        const float d = horizontalDistance(n.position, from);
        if (d > maxDistance)
            continue;
        if (best == 0 || d < bestD || (d == bestD && n.id < best))
        {
            best = n.id;
            bestD = d;
        }
    }
    return best;
}

std::uint32_t NpcBook::byName(std::string_view name) const noexcept
{
    for (const NpcEntity& n : m_npcs)
        if (n.name == name)
            return n.id;
    return 0;
}

// ---- Shop session ----

bool ShopSession::finishOpen(const vn::ShopOpenRes& m)
{
    if (m.npcId == 0 || m.npcId != m_opening)
        return false;
    m_opening = 0;
    if (m.result != vn::ShopResult::Ok)
        return true;
    m_npc = m.npcId;
    m_offers.clear();
    for (const vn::ShopEntry& e : m.entries)
        m_offers.push_back({e.templateId, e.price});
    return true;
}

void ShopSession::close(bool forgetRequests) noexcept
{
    m_npc = 0;
    m_opening = 0;
    m_offers.clear();
    if (forgetRequests)
        m_request.reset();
}

const ShopOffer* ShopSession::offer(std::uint16_t templateId) const noexcept
{
    for (const ShopOffer& o : m_offers)
        if (o.templateId == templateId)
            return &o;
    return nullptr;
}

std::optional<vn::ShopBuyReq> ShopSession::beginBuy(std::uint16_t templateId, std::uint16_t quantity)
{
    if (!isOpen() || busy())
        return std::nullopt;
    const std::uint32_t seq = m_nextSeq++;
    if (m_nextSeq == 0)
        m_nextSeq = 1;
    m_request = Request{Op::Buy, seq, m_npc, templateId, quantity, 0};
    return vn::ShopBuyReq{seq, m_npc, templateId, quantity};
}

std::optional<vn::ShopSellReq> ShopSession::beginSell(std::uint8_t bagSlot, std::uint16_t templateId, std::uint16_t quantity,
                                                     std::uint8_t upgrade)
{
    if (!isOpen() || busy())
        return std::nullopt;
    const std::uint32_t seq = m_nextSeq++;
    if (m_nextSeq == 0)
        m_nextSeq = 1;
    m_request = Request{Op::Sell, seq, m_npc, templateId, quantity, bagSlot, upgrade};
    return vn::ShopSellReq{seq, m_npc, bagSlot, quantity};
}

std::optional<ShopSession::Request> ShopSession::finishBuy(const vn::ShopBuyRes& m)
{
    if (!m_request || m_request->op != Op::Buy || m_request->seq != m.seq)
        return std::nullopt;
    const Request r = *m_request;
    m_request.reset();
    return r;
}

std::optional<ShopSession::Request> ShopSession::finishSell(const vn::ShopSellRes& m)
{
    if (!m_request || m_request->op != Op::Sell || m_request->seq != m.seq)
        return std::nullopt;
    const Request r = *m_request;
    m_request.reset();
    return r;
}

// ---- Cooldown ----

void ConsumableCooldown::set(std::uint32_t remainingMs, std::uint32_t totalMs, double now) noexcept
{
    m_total = std::max(0.001, static_cast<double>(std::max(totalMs, remainingMs)) / 1000.0);
    m_end = now + static_cast<double>(remainingMs) / 1000.0;
}

float ConsumableCooldown::sweep(double now) const noexcept
{
    if (!active(now))
        return 0.0f;
    return static_cast<float>(std::clamp(remaining(now) / m_total, 0.0, 1.0));
}

// ---- Quantity ----

QuantityModel::QuantityModel(std::uint16_t maxValue, std::uint16_t initial) noexcept
    : max(std::max<std::uint16_t>(maxValue, 1))
{
    set(initial);
}

void QuantityModel::set(int v) noexcept
{
    value = static_cast<std::uint16_t>(std::clamp(v, 1, static_cast<int>(max)));
}

// ---- Entries ----

namespace {

std::string classNames(const game::ItemTemplate& t)
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
    return out;
}

} // namespace

void ShopSession::appendOffersForTesting(std::span<const ShopOffer> offers)
{
    if (m_npc == 0)
        return;
    for (const ShopOffer& o : offers)
        if (offer(o.templateId) == nullptr)
            m_offers.push_back(o);
}

ShopListLayout shopListLayout(std::size_t rows, float maxHeight, float rowH, float rowGap, std::size_t first) noexcept
{
    ShopListLayout l;
    const std::size_t shown = std::max<std::size_t>(rows, 1); // an empty stock keeps one row's room
    std::size_t fit = shown;
    if (maxHeight > 0.0f && rowH > 0.0f)
    {
        const auto n = static_cast<std::size_t>(std::max(0.0f, std::floor((maxHeight + rowGap) / (rowH + rowGap))));
        fit = std::clamp<std::size_t>(n, std::min(kShopMinVisibleRows, shown), shown);
    }
    l.visible = std::min(fit, shown);
    l.scrolls = rows > l.visible;
    l.maxFirst = l.scrolls ? rows - l.visible : 0;
    l.first = std::min(first, l.maxFirst);
    const auto n = static_cast<float>(l.visible);
    l.height = n * rowH + (n - 1.0f) * rowGap;
    if (l.scrolls)
    {
        l.thumbHeight = std::max(kShopMinThumb, std::round(l.height * n / static_cast<float>(rows)));
        l.thumbHeight = std::min(l.thumbHeight, l.height);
        l.thumbTop = std::round((l.height - l.thumbHeight) * static_cast<float>(l.first) / static_cast<float>(l.maxFirst));
    }
    return l;
}

std::size_t shopScrollForThumb(const ShopListLayout& l, float thumbTop) noexcept
{
    if (!l.scrolls || l.maxFirst == 0)
        return 0;
    const float travel = l.height - l.thumbHeight;
    if (!(travel > 0.0f))
        return 0;
    const float u = std::clamp(thumbTop / travel, 0.0f, 1.0f);
    return static_cast<std::size_t>(std::lround(u * static_cast<float>(l.maxFirst)));
}

std::string priceText(std::uint64_t akce)
{
    return std::format("{} Akçe", formatThousands(akce));
}

std::string balanceText(std::uint32_t coins)
{
    return std::format("Akçe: {}", formatThousands(coins));
}

ShopEntryView shopEntryView(const game::ItemTemplate& item, std::uint32_t price, std::uint32_t coins,
                            const game::ItemUser& user)
{
    ShopEntryView v;
    v.affordable = price <= coins;
    if (item.isConsumable())
    {
        if (item.restoreHp > 0 && item.restoreMp > 0)
            v.detail = std::format("Can +{} · Mana +{}", item.restoreHp, item.restoreMp);
        else if (item.restoreHp > 0)
            v.detail = std::format("Can +{}", item.restoreHp);
        else
            v.detail = std::format("Mana +{}", item.restoreMp);
        return v;
    }
    if (item.kind == game::ItemKind::Material)
    {
        v.detail = "Malzeme";
        return v;
    }
    // Gear: a class it is not for, else a level too high, in red; else what it gives and the level.
    if (!item.allowsClass(user.charClass))
    {
        v.detail = "Yalnız " + classNames(item);
        v.detailUnmet = true;
        return v;
    }
    if (item.level > user.level)
    {
        v.detail = std::format("Gereken seviye: {}", item.level);
        v.detailUnmet = true;
        return v;
    }
    std::string stat;
    if (item.attack != 0)
        stat = std::format("Saldırı +{}", item.attack);
    if (item.defense != 0)
        stat += std::format("{}Savunma +{}", stat.empty() ? "" : " · ", item.defense);
    v.detail = stat.empty() ? std::format("Sv. {}", item.level) : std::format("{} · Sv. {}", stat, item.level);
    return v;
}

// ---- Decisions ----

bool buyNeedsQuantity(const game::ItemTemplate& item) noexcept
{
    return item.isStackable();
}

bool buyNeedsConfirm(const game::ItemTemplate& item, std::uint64_t total) noexcept
{
    return !item.isStackable() && total > kBuyConfirmAbove;
}

bool sellNeedsQuantity(std::uint16_t stackQuantity) noexcept
{
    return stackQuantity > 1;
}

bool sellNeedsConfirm(const game::ItemTemplate& item) noexcept
{
    return item.rarity != game::Rarity::Common || item.value >= kSellConfirmValue;
}

std::string buyConfirmText(std::string_view name, std::uint64_t total)
{
    return std::format("{} için {} Akçe ödensin mi?", name, formatThousands(total));
}

std::string sellConfirmText(std::string_view name, std::uint16_t quantity, std::uint64_t gained)
{
    if (quantity > 1)
        return std::format("{} × {} {} Akçe'ye satılsın mı?", quantity, name, formatThousands(gained));
    return std::format("{} {} Akçe'ye satılsın mı?", name, formatThousands(gained));
}

std::string buyTotalText(std::uint64_t total)
{
    return std::format("Toplam: {} Akçe", formatThousands(total));
}

std::string sellTotalText(std::uint64_t gained)
{
    return std::format("Alınacak: {} Akçe", formatThousands(gained));
}

std::string sellTipText(const game::ItemTemplate& item, std::uint16_t quantity)
{
    const std::uint32_t unit = item.value / 2;
    if (unit == 0)
        return "Satılamaz";
    if (quantity > 1)
        return std::format("Satış: {} Akçe (yığın {} Akçe)", formatThousands(unit),
                           formatThousands(static_cast<std::uint64_t>(unit) * quantity));
    return std::format("Satış: {} Akçe", formatThousands(unit));
}

// ---- Texts ----

std::string_view shopResultText(vn::ShopResult r) noexcept
{
    using R = vn::ShopResult;
    switch (r)
    {
    case R::Ok: return "";
    case R::NotFound: return "Burada satıcı yok.";
    case R::YouAreDead: return "Ölüyken alışveriş yapamazsın.";
    case R::OutOfRange: return "Satıcıya daha yakın olmalısın.";
    case R::NotOpen: return "Önce satıcıyla konuş.";
    case R::NotInStock: return "Bu satıcı bunu satmıyor.";
    case R::BadQuantity: return "Bu miktar olmaz.";
    case R::NotEnoughCoins: return "Yeterli Akçen yok.";
    case R::BagFull: return "Çantanda yer yok.";
    case R::NoItem: return "Satılacak bir şey yok.";
    case R::CannotSell: return "Bu satılamaz.";
    case R::PurseFull: return "Kesen dolu; daha fazla Akçe taşıyamazsın.";
    case R::ServerError: return "Alışveriş kaydedilemedi, tekrar dene.";
    }
    return "Alışveriş kaydedilemedi, tekrar dene.";
}

std::string buyOkText(std::string_view name, std::uint16_t quantity, std::uint32_t cost)
{
    if (quantity > 1)
        return std::format("{} × {} alındı ({} Akçe).", quantity, name, formatThousands(cost));
    return std::format("{} alındı ({} Akçe).", name, formatThousands(cost));
}

std::string sellOkText(std::string_view name, std::uint16_t quantity, std::uint32_t gained)
{
    if (quantity > 1)
        return std::format("{} × {} satıldı (+{} Akçe).", quantity, name, formatThousands(gained));
    return std::format("{} satıldı (+{} Akçe).", name, formatThousands(gained));
}

std::string shopClosedText(vn::ShopClosedReason r, std::string_view npcName)
{
    if (r == vn::ShopClosedReason::OutOfRange)
        return std::format("{} çok uzakta kaldı.", npcName);
    return {};
}

std::vector<std::string> npcTipLines(std::string_view name, std::string_view title)
{
    std::vector<std::string> out;
    out.push_back(title.empty() ? std::string(name) : std::format("{} — {}", name, title));
    out.emplace_back("Konuşmak için tıkla");
    return out;
}

std::string npcTitlePlate(std::string_view title)
{
    return title.empty() ? std::string() : std::format("‹{}›", title);
}

// ---- Potions ----

std::uint32_t bagCount(const game::Inventory& inv, game::ItemTemplateId templateId) noexcept
{
    std::uint32_t n = 0;
    if (templateId == 0)
        return 0;
    for (std::uint8_t i = 0; i < game::kBagSlots; ++i)
    {
        const game::ItemStack& s = inv.at(game::ItemPlace::bag(i));
        if (!s.empty() && s.templateId == templateId)
            n += s.quantity;
    }
    return n;
}

std::vector<TipLine> consumableTipLines(const game::ItemTemplate& item, std::uint32_t quantity, bool showZero)
{
    std::vector<TipLine> out;
    out.push_back({item.name, TipTone::Title, rarityRgb(item.rarity), false});
    out.push_back({"Tüketilebilir", TipTone::Dim, 0, false});
    if (item.restoreHp > 0)
        out.push_back({std::format("Can: +{}", item.restoreHp), TipTone::Good, 0, false});
    if (item.restoreMp > 0)
        out.push_back({std::format("Mana: +{}", item.restoreMp), TipTone::Good, 0, false});
    out.push_back({std::format("Bekleme: {} sn (bütün iksirler)", game::kConsumableCooldownMs / 1000), TipTone::Normal, 0,
                   false});
    if (!item.description.empty())
        out.push_back({item.description, TipTone::Dim, 0, true});
    if (quantity > 0 || showZero)
        out.push_back({std::format("{} / {}", quantity, item.maxStack), quantity > 0 ? TipTone::Normal : TipTone::Bad, 0,
                       false});
    return out;
}

std::string barUseText(std::string_view key)
{
    return std::format("Çubuktan kullan: {}", key);
}

std::string restoreText(std::uint32_t amount)
{
    return std::format("+{}", amount);
}

// ---- Input ----

EscapeAction escapeActionShop(bool lootOpen, bool shopOpen, bool controlsOpen, bool skillsOpen, bool kunyeOpen,
                              bool inventoryOpen, bool hasTarget, bool menuOpen) noexcept
{
    if (menuOpen)
        return EscapeAction::None;
    if (lootOpen)
        return EscapeAction::CloseLoot;
    if (shopOpen)
        return EscapeAction::CloseShop;
    return escapeActionHud(false, controlsOpen, skillsOpen, kunyeOpen, inventoryOpen, hasTarget, false);
}

// ---- Automation ----

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

bool number(std::string_view s, std::uint32_t& out)
{
    const auto r = std::from_chars(s.data(), s.data() + s.size(), out);
    return r.ec == std::errc{} && r.ptr == s.data() + s.size();
}

bool real(std::string_view s, double& out)
{
    const auto r = std::from_chars(s.data(), s.data() + s.size(), out);
    return r.ec == std::errc{} && r.ptr == s.data() + s.size();
}

// The words from `first` on, joined by single spaces (NPC names have spaces).
std::string rest(const std::vector<std::string_view>& t, std::size_t first, std::size_t end)
{
    std::string out;
    for (std::size_t i = first; i < end; ++i)
    {
        if (!out.empty())
            out += ' ';
        out += t[i];
    }
    return out;
}

} // namespace

std::optional<ShopCommand> parseShopCommand(std::string_view line, std::string& error)
{
    error.clear();
    const std::vector<std::string_view> t = tokens(line);
    if (t.empty())
        return std::nullopt;
    const std::string_view v = t[0];
    ShopCommand c;
    const auto bad = [&](std::string_view usage) -> std::optional<ShopCommand> {
        error = std::format("usage: {}", usage);
        return std::nullopt;
    };
    if (v == "talk" || v == "shopopenforce" || v == "hovernpc" || v == "expectshop")
    {
        if (t.size() < 2)
            return bad(std::format("{} <npc name>", v));
        c.verb = v == "talk" ? ShopVerb::Talk : v == "shopopenforce" ? ShopVerb::OpenForce
               : v == "hovernpc"                                    ? ShopVerb::Hover
                                                                    : ShopVerb::ExpectOpen;
        c.text = rest(t, 1, t.size());
        return c;
    }
    if (v == "walktonpc")
    {
        if (t.size() < 2)
            return bad("walktonpc <npc name> [metres]");
        c.verb = ShopVerb::WalkTo;
        double m = 2.5;
        std::size_t end = t.size();
        if (t.size() >= 3 && real(t.back(), m))
            --end;
        else
            m = 2.5;
        c.lo = m;
        c.text = rest(t, 1, end);
        return c;
    }
    if (v == "shopbuy" || v == "shopbuyforce")
    {
        c.verb = v == "shopbuy" ? ShopVerb::Buy : ShopVerb::BuyForce;
        if (t.size() < 2 || !number(t[1], c.a) || (t.size() >= 3 && !number(t[2], c.b)) || t.size() > 3 ||
            (c.verb == ShopVerb::BuyForce && t.size() != 3))
            return bad(c.verb == ShopVerb::Buy ? "shopbuy <template> [quantity]" : "shopbuyforce <template> <quantity>");
        c.hasB = t.size() == 3;
        return c;
    }
    if (v == "shopsell")
    {
        c.verb = ShopVerb::Sell;
        if (t.size() < 2 || t.size() > 3 || !number(t[1], c.a) || c.a >= game::kBagSlots ||
            (t.size() == 3 && !number(t[2], c.b)))
            return bad("shopsell <bagSlot 0-27> [quantity]");
        c.hasB = t.size() == 3;
        return c;
    }
    if (v == "shopclose" || v == "shoplog")
    {
        if (t.size() != 1)
            return bad(v);
        c.verb = v == "shopclose" ? ShopVerb::Close : ShopVerb::Log;
        return c;
    }
    if (v == "expectshopmessage")
    {
        if (t.size() < 2)
            return bad("expectshopmessage <text>");
        c.verb = ShopVerb::ExpectMessage;
        c.text = std::string(line.substr(static_cast<std::size_t>(t[1].data() - line.data())));
        return c;
    }
    if (v == "expectbarcount")
    {
        c.verb = ShopVerb::ExpectCount;
        if (t.size() != 3 || !number(t[1], c.a) || c.a < 1 || c.a > 8 || !number(t[2], c.b))
            return bad("expectbarcount <slot 1-8> <n>");
        return c;
    }
    if (v == "expectpotioncd")
    {
        c.verb = ShopVerb::ExpectCooldown;
        if (t.size() != 3 || !real(t[1], c.lo) || !real(t[2], c.hi) || c.lo > c.hi)
            return bad("expectpotioncd <minSec> <maxSec>");
        return c;
    }
    if (v == "usepotion")
    {
        c.verb = ShopVerb::Use;
        if (t.size() < 2 || t.size() > 3 || !number(t[1], c.a) || (t.size() == 3 && !number(t[2], c.b)))
            return bad("usepotion <template> [bagSlot]");
        c.hasB = t.size() == 3;
        return c;
    }
    if (v == "baritem")
    {
        c.verb = ShopVerb::BarItem;
        if (t.size() != 3 || !number(t[1], c.a) || c.a < 1 || c.a > 8 || (t[2] != "none" && !number(t[2], c.b)))
            return bad("baritem <slot 1-8> <template>|none");
        return c;
    }
    return std::nullopt;
}

} // namespace vo::client::world
