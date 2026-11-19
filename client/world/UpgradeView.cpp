#include "client/world/UpgradeView.h"

#include <charconv>
#include <format>
#include <vector>

namespace vo::client::world {

namespace vn = vo::net;
using R = vn::UpgradeResult;

// ---- UpgradeSession ----

bool UpgradeSession::pick(const game::Inventory& inv, std::uint8_t bagSlot)
{
    if (bagSlot >= game::kBagSlots)
        return false;
    const game::ItemStack& s = inv.at(game::ItemPlace::bag(bagSlot));
    if (s.empty())
        return false;
    m_slot = bagSlot;
    m_itemId = s.id;
    return true;
}

void UpgradeSession::sync(const game::Inventory& inv) noexcept
{
    if (!m_slot)
        return;
    const game::ItemStack& s = inv.at(game::ItemPlace::bag(*m_slot));
    if (s.empty() || s.id != m_itemId)
        m_slot.reset();
}

std::optional<vn::UpgradeReq> UpgradeSession::begin(const game::Inventory& inv, std::uint32_t npc, std::uint8_t stoneSlot)
{
    if (busy() || !m_slot || npc == 0)
        return std::nullopt;
    const game::ItemStack& s = inv.at(game::ItemPlace::bag(*m_slot));
    if (s.empty() || s.id != m_itemId)
        return std::nullopt;
    return beginRaw(npc, *m_slot, s.id, s.upgrade, stoneSlot, s.templateId);
}

std::optional<vn::UpgradeReq> UpgradeSession::beginRaw(std::uint32_t npc, std::uint8_t bagSlot, std::uint64_t itemId,
                                                       std::uint8_t fromLevel, std::uint8_t stoneSlot,
                                                       std::uint16_t templateId)
{
    if (busy())
        return std::nullopt;
    Request r;
    r.seq = m_nextSeq++;
    if (m_nextSeq == 0)
        m_nextSeq = 1;
    r.npc = npc;
    r.bagSlot = bagSlot;
    r.itemId = itemId;
    r.fromLevel = fromLevel;
    r.templateId = templateId;
    m_request = r;
    return vn::UpgradeReq{r.seq, npc, bagSlot, itemId, fromLevel, stoneSlot};
}

std::optional<UpgradeSession::Request> UpgradeSession::finish(const vn::UpgradeRes& m, double now)
{
    if (!m_request || m_request->seq != m.seq)
        return std::nullopt;
    const Request r = *m_request;
    m_request.reset();
    m_outcome = Outcome{m.result, m.templateId != 0 ? m.templateId : r.templateId, m.level, now};
    if (m.result == R::Destroyed && m_slot && *m_slot == r.bagSlot)
        m_slot.reset(); // the item is gone
    return r;
}

void UpgradeSession::close(bool forgetRequest) noexcept
{
    m_slot.reset();
    m_outcome.reset();
    if (forgetRequest)
        m_request.reset();
}

// ---- The section ----

std::string chancePercentText(std::uint16_t permille)
{
    if (permille % 10 == 0)
        return std::format("%{}", permille / 10);
    return std::format("%{},{}", permille / 10, permille % 10);
}

UpgradePanelView upgradePanelView(const game::ItemCatalog& catalog, const game::Inventory& inv, std::uint32_t coins,
                                  std::optional<std::uint8_t> bagSlot)
{
    UpgradePanelView v;
    if (!bagSlot || *bagSlot >= game::kBagSlots)
    {
        v.blockText = std::string(kUpgradeHintText);
        return v;
    }
    const game::ItemStack& s = inv.at(game::ItemPlace::bag(*bagSlot));
    const game::ItemTemplate* t = s.empty() ? nullptr : catalog.find(s.templateId);
    if (t == nullptr)
    {
        v.blockText = std::string(kUpgradeHintText);
        return v;
    }
    v.hasItem = true;
    v.templateId = s.templateId;
    v.level = s.upgrade;
    v.name = itemDisplayName(*t, s.upgrade);
    v.nameRgb = itemNameRgb(*t, s.upgrade);
    if (!game::isUpgradable(*t))
    {
        v.blockText = std::string(upgradeResultText(R::NotUpgradable, t, s.upgrade));
        return v;
    }
    v.stone = game::requiredStone(*t);
    v.stoneName = std::string(game::stoneGradeName(v.stone));
    if (const game::ItemTemplate* st = game::stoneTemplate(catalog, v.stone))
    {
        v.stoneTemplate = st->id;
        v.stoneName = st->name;
    }
    v.stonesOwned = game::countStones(catalog, inv, v.stone);
    if (s.upgrade >= game::kMaxUpgradeLevel)
    {
        v.levelLine = std::format("+{} (en yüksek)", s.upgrade);
        v.blockText = upgradeResultText(R::MaxLevel, t, s.upgrade);
        return v;
    }
    v.target = static_cast<std::uint8_t>(s.upgrade + 1);
    v.levelLine = std::format("+{} → +{}", s.upgrade, v.target);
    std::string stats;
    const auto stat = [&](std::string_view what, std::int32_t now, std::int32_t next) {
        if (now == 0 && next == 0)
            return;
        if (!stats.empty())
            stats += " · ";
        stats += std::format("{} {} → {}", what, now, next);
    };
    stat("Saldırı", game::upgradedAttack(*t, s.upgrade), game::upgradedAttack(*t, v.target));
    stat("Savunma", game::upgradedDefense(*t, s.upgrade), game::upgradedDefense(*t, v.target));
    v.statLine = stats;
    v.fee = game::upgradeFee(*t, v.target);
    v.affordable = v.fee <= coins;
    v.chancePermille = game::upgradeChancePermille(v.target);
    v.chanceText = chancePercentText(v.chancePermille);
    v.failDestroys = game::failureDestroys(v.target);
    v.failText = v.chancePermille >= 1000 ? "Başarısız olamaz."
                 : v.failDestroys         ? "Tutmazsa eşya parçalanıp YOK OLUR!"
                                          : "Tutmazsa eşya olduğu gibi kalır.";
    if (v.stonesOwned == 0)
        v.blockText = std::format("Çantanda {} yok.", v.stoneName);
    else if (!v.affordable)
        v.blockText = "Yeterli Akçen yok.";
    v.canAttempt = v.blockText.empty();
    v.needsConfirm = v.failDestroys;
    return v;
}

std::string upgradeConfirmText(const UpgradePanelView& v)
{
    return std::format("{} → +{}: başarı şansı {}. Tutmazsa eşya YOK OLUR; {} ve {} Akçe de gider. Yükseltilsin mi?",
                       v.name, v.target, v.chanceText, v.stoneName, formatThousands(v.fee));
}

std::string upgradeResultText(R r, const game::ItemTemplate* item, std::uint8_t level)
{
    const std::string name = item != nullptr ? item->name : std::string("Eşya");
    const std::string named = item != nullptr ? itemDisplayName(*item, level) : name;
    const std::string stone =
        item != nullptr ? std::string(game::stoneGradeName(game::requiredStone(*item))) : std::string("tav taşı");
    switch (r)
    {
    case R::Success: return std::format("Tav tuttu! {} oldu.", named);
    case R::Failed: return std::format("Tav tutmadı: {} olduğu gibi kaldı.", named);
    case R::Destroyed: return std::format("Tav tutmadı: {} parçalandı!", named);
    case R::NotOpen: return "Önce demirciyle konuş.";
    case R::NotSmith: return "Bu satıcı eşya yükseltmez.";
    case R::YouAreDead: return "Ölüyken yükseltme yapamazsın.";
    case R::OutOfRange: return "Demirciye daha yakın olmalısın.";
    case R::NoItem: return "Yükseltilecek eşya çantada yok.";
    case R::Changed: return "Eşya değişti; yeniden seç.";
    case R::NotUpgradable: return std::format("{} yükseltilemez: yalnız silah, kalkan ve zırh.", name);
    case R::MaxLevel: return std::format("{} en yüksek seviyede (+{}).", name, game::kMaxUpgradeLevel);
    case R::NoStone: return std::format("Çantanda {} yok.", stone);
    case R::WrongStone: return std::format("Bu eşyaya {} gerekir.", stone);
    case R::NotEnoughCoins: return "Yeterli Akçen yok.";
    case R::ServerError: return "Yükseltme kaydedilemedi, tekrar dene.";
    }
    return "";
}

bool upgradeResultIsBad(R r) noexcept
{
    return r != R::Success;
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

} // namespace

std::optional<UpgradeCommand> parseUpgradeCommand(std::string_view line, std::string& error)
{
    error.clear();
    const std::vector<std::string_view> t = tokens(line);
    if (t.empty())
        return std::nullopt;
    const std::string_view v = t[0];
    UpgradeCommand c;
    const auto bad = [&](std::string_view usage) -> std::optional<UpgradeCommand> {
        error = std::format("usage: {}", usage);
        return std::nullopt;
    };
    if (v == "shoptab")
    {
        if (t.size() != 2 || (t[1] != "buy" && t[1] != "upgrade"))
            return bad("shoptab buy|upgrade");
        c.verb = UpgradeVerb::Tab;
        c.text = std::string(t[1]);
        return c;
    }
    if (v == "upgradepick")
    {
        c.verb = UpgradeVerb::Pick;
        if (t.size() != 2 || !number(t[1], c.a) || c.a >= game::kBagSlots)
            return bad("upgradepick <bagSlot 0-27>");
        return c;
    }
    if (v == "upgradeclear" || v == "upgradestart" || v == "upgradelog")
    {
        if (t.size() != 1)
            return bad(v);
        c.verb = v == "upgradeclear" ? UpgradeVerb::Clear : v == "upgradestart" ? UpgradeVerb::Start : UpgradeVerb::Log;
        return c;
    }
    if (v == "upgradeforce")
    {
        c.verb = UpgradeVerb::Force;
        if (t.size() < 2 || t.size() > 4 || !number(t[1], c.a))
            return bad("upgradeforce <bagSlot> [stoneSlot|any] [fromLevel]");
        c.b = vn::kUpgradeAnyStone;
        if (t.size() >= 3 && t[2] != "any" && !number(t[2], c.b))
            return bad("upgradeforce <bagSlot> [stoneSlot|any] [fromLevel]");
        c.hasB = t.size() >= 3;
        if (t.size() == 4 && !number(t[3], c.c))
            return bad("upgradeforce <bagSlot> [stoneSlot|any] [fromLevel]");
        c.hasC = t.size() == 4;
        return c;
    }
    if (v == "expectupgrade")
    {
        c.verb = UpgradeVerb::ExpectLevel;
        if (t.size() != 3 || !number(t[1], c.a) || c.a >= game::kBagSlots || !number(t[2], c.b) ||
            c.b > game::kMaxUpgradeLevel)
            return bad("expectupgrade <bagSlot 0-27> <level 0-10>");
        return c;
    }
    if (v == "expectupgradepanel")
    {
        if (t.size() < 2)
            return bad("expectupgradepanel <text>");
        c.verb = UpgradeVerb::ExpectPanel;
        c.text = std::string(line.substr(static_cast<std::size_t>(t[1].data() - line.data())));
        return c;
    }
    return std::nullopt;
}

} // namespace vo::client::world
