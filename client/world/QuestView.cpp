#include "client/world/QuestView.h"

#include "shared/game/Npcs.h"

#include <algorithm>
#include <charconv>
#include <format>
#include <sstream>

namespace vo::client::world {

namespace vn = vo::net;
using game::ObjectiveKind;
using game::QuestStatus;

namespace {

game::QuestRecord fromEntry(const vn::QuestEntry& e, double now, const game::QuestRecord* old)
{
    game::QuestRecord r = old ? *old : game::QuestRecord{};
    r.quest = e.questId;
    r.state = e.state;
    r.progress = e.state == vn::QuestState::Active ? e.progress : std::array<std::uint16_t, 3>{};
    if (e.state == vn::QuestState::Done)
    {
        r.completions = std::max<std::uint32_t>(r.completions, 1);
        // The cooldown on the client clock: as if handed in (cooldown - left) seconds ago.
        if (const game::QuestDef* def = game::quests().find(e.questId); def != nullptr && def->repeatable)
            r.completedAt = QuestBook::clock(now) + static_cast<std::int64_t>(e.cooldownS) - def->cooldownS;
        else
            r.completedAt = QuestBook::clock(now);
    }
    return r;
}

bool objectiveDone(const game::QuestDef& def, const game::QuestRecord& rec, const game::Inventory& inv,
                   const game::ItemCatalog* items, std::size_t i)
{
    return game::objectiveProgress(def, rec, inv, items, i) >= def.objectives[i].count;
}

std::string itemName(const game::ItemCatalog* items, game::ItemTemplateId id)
{
    const game::ItemTemplate* t = items != nullptr ? items->find(id) : nullptr;
    return t != nullptr ? t->name : std::format("Eşya #{}", id);
}

} // namespace

// ---- QuestBook ----

void QuestBook::apply(const vn::QuestList& m, double now)
{
    m_log.clear();
    m_order.clear();
    m_ready.clear();
    m_doneObjectives.clear();
    for (const vn::QuestEntry& e : m.quests)
    {
        m_log.put(fromEntry(e, now, nullptr));
        if (e.state == vn::QuestState::Active)
            m_order.push_back(e.questId);
    }
    m_known = true;
    ++m_updates;
}

void QuestBook::apply(const vn::QuestUpdate& m, double now, const game::Inventory& inv, const game::ItemCatalog* items)
{
    const vn::QuestEntry& e = m.quest;
    const game::QuestRecord* old = m_log.find(e.questId);
    const bool wasActive = old != nullptr && old->state == vn::QuestState::Active;
    const game::QuestDef* def = game::quests().find(e.questId);
    ++m_updates;
    if (e.state == vn::QuestState::None)
    {
        m_log.erase(e.questId);
        std::erase(m_order, e.questId);
        std::erase(m_ready, e.questId);
        std::erase_if(m_doneObjectives, [&](const auto& p) { return p.first == e.questId; });
        if (wasActive && def != nullptr)
            m_notices.push_back({QuestNotice::Kind::Abandoned, e.questId, std::format("Görev bırakıldı: {}", def->name)});
        return;
    }
    m_log.put(fromEntry(e, now, old));
    if (e.state == vn::QuestState::Done)
    {
        std::erase(m_order, e.questId);
        std::erase(m_ready, e.questId);
        std::erase_if(m_doneObjectives, [&](const auto& p) { return p.first == e.questId; });
        return; // (the hand-in line comes with QUEST_RES)
    }
    if (!wasActive)
    {
        if (std::find(m_order.begin(), m_order.end(), e.questId) == m_order.end())
            m_order.push_back(e.questId);
        std::erase(m_ready, e.questId);
        std::erase_if(m_doneObjectives, [&](const auto& p) { return p.first == e.questId; });
        if (def != nullptr)
            m_notices.push_back({QuestNotice::Kind::Accepted, e.questId, std::format("Yeni görev: {}", def->name)});
        // What is met at once (a talk quest, a collect whose units are in the bag) is noted silently: the
        // tracker and the marker show it; a notice comes only when something completes later.
        noteReadiness(e.questId, inv, items, false);
        return;
    }
    noteReadiness(e.questId, inv, items, true);
}

void QuestBook::noteInventory(const game::Inventory& inv, const game::ItemCatalog* items)
{
    for (const game::QuestId id : std::vector<game::QuestId>(m_order))
        noteReadiness(id, inv, items, true);
}

void QuestBook::noteReadiness(game::QuestId id, const game::Inventory& inv, const game::ItemCatalog* items, bool announce)
{
    const game::QuestRecord* rec = m_log.find(id);
    const game::QuestDef* def = game::quests().find(id);
    if (rec == nullptr || def == nullptr || rec->state != vn::QuestState::Active)
        return;
    const bool ready = game::questReady(*def, *rec, inv, items);
    const bool wasReady = std::find(m_ready.begin(), m_ready.end(), id) != m_ready.end();
    for (std::size_t i = 0; i < def->objectives.size(); ++i)
    {
        const bool done = objectiveDone(*def, *rec, inv, items, i);
        const auto key = std::pair<game::QuestId, std::uint8_t>{id, static_cast<std::uint8_t>(i)};
        const auto it = std::find(m_doneObjectives.begin(), m_doneObjectives.end(), key);
        if (done && it == m_doneObjectives.end())
        {
            m_doneObjectives.push_back(key);
            // A talk objective is met from the start; the quest's own notice says it.
            if (announce && !ready && def->objectives[i].kind != ObjectiveKind::Talk)
                m_notices.push_back({QuestNotice::Kind::ObjectiveDone, id,
                                     std::format("Tamam: {}", objectiveText(*def, i, def->objectives[i].count))});
        }
        else if (!done && it != m_doneObjectives.end())
            m_doneObjectives.erase(it); // (a collect unit sold / dropped again)
    }
    if (ready && !wasReady)
    {
        m_ready.push_back(id);
        if (announce)
            m_notices.push_back({QuestNotice::Kind::Ready, id,
                                 std::format("Görev tamamlandı: {} — {} ile konuş", def->name,
                                             npcName(&game::npcs(), def->turnIn))});
    }
    else if (!ready && wasReady)
        std::erase(m_ready, id);
}

void QuestBook::clear() noexcept
{
    m_log.clear();
    m_order.clear();
    m_ready.clear();
    m_doneObjectives.clear();
    m_notices.clear();
    m_known = false;
}

std::vector<game::QuestId> QuestBook::activeInOrder() const
{
    std::vector<game::QuestId> out;
    for (const game::QuestId id : m_order)
        if (const game::QuestRecord* r = m_log.find(id); r != nullptr && r->state == vn::QuestState::Active)
            out.push_back(id);
    return out;
}

std::vector<QuestNotice> QuestBook::takeNotices()
{
    std::vector<QuestNotice> out;
    out.swap(m_notices);
    return out;
}

// ---- QuestRequests ----

bool QuestRequests::canSend(double now) const noexcept
{
    if (busy())
        return false;
    std::size_t recent = 0;
    for (const double t : m_sends)
        recent += now - t < 1.0 ? 1 : 0;
    return recent < m_sends.size();
}

std::uint32_t QuestRequests::begin(vn::QuestAction action, game::QuestId quest, std::uint32_t npc, double now)
{
    const std::uint32_t seq = m_nextSeq++;
    m_pending = Pending{seq, action, quest, npc};
    m_sends[m_sendNext] = now;
    m_sendNext = (m_sendNext + 1) % m_sends.size();
    return seq;
}

std::optional<QuestRequests::Pending> QuestRequests::finish(const vn::QuestRes& m)
{
    if (!m_pending || m_pending->seq != m.seq)
        return std::nullopt;
    const Pending p = *m_pending;
    m_pending.reset();
    return p;
}

// ---- NPC entries ----

std::vector<NpcQuestEntry> npcQuestEntries(const game::QuestCatalog& catalog, std::uint16_t npcId, const game::QuestLog& log,
                                           const game::QuestUser& user, const game::Inventory& inv,
                                           const game::ItemCatalog* items, std::int64_t now, std::size_t maxActive)
{
    std::vector<NpcQuestEntry> ready, offered, progress, later;
    for (const game::QuestDef& q : catalog.quests())
    {
        const bool giver = q.giver == npcId;
        const bool turnIn = q.turnIn == npcId;
        if (!giver && !turnIn)
            continue;
        const QuestStatus s = game::questStatus(catalog, q, log, user, inv, items, now, maxActive);
        const NpcQuestEntry e{&q, s, giver, turnIn};
        if (s == QuestStatus::Ready && turnIn)
            ready.push_back(e);
        else if (s == QuestStatus::Available && giver)
            offered.push_back(e);
        else if (s == QuestStatus::Active || s == QuestStatus::Ready)
            progress.push_back(e);
        else if (s == QuestStatus::Later && giver && q.level <= user.level + 2)
            later.push_back(e); // only what is near (two levels)
    }
    std::vector<NpcQuestEntry> out = std::move(ready);
    out.insert(out.end(), offered.begin(), offered.end());
    out.insert(out.end(), progress.begin(), progress.end());
    out.insert(out.end(), later.begin(), later.end());
    return out;
}

bool npcHasQuestBusiness(const game::QuestCatalog& catalog, std::uint16_t npcId, const game::QuestLog& log,
                         const game::QuestUser& user, const game::Inventory& inv, const game::ItemCatalog* items,
                         std::int64_t now, std::size_t maxActive)
{
    for (const NpcQuestEntry& e : npcQuestEntries(catalog, npcId, log, user, inv, items, now, maxActive))
        if (e.status != QuestStatus::Later)
            return true;
    return false;
}

// ---- Texts ----

std::string objectiveText(const game::QuestDef& def, std::size_t index, std::uint16_t progress)
{
    if (index >= def.objectives.size())
        return {};
    const game::QuestObjective& o = def.objectives[index];
    switch (o.kind)
    {
    case ObjectiveKind::Kill:
    case ObjectiveKind::Collect: return std::format("{} {}/{}", o.label, std::min(progress, o.count), o.count);
    case ObjectiveKind::Boss: return std::format("Yen: {} {}/{}", o.label, std::min(progress, o.count), o.count);
    case ObjectiveKind::Talk: return std::format("{} ile konuş", o.label);
    }
    return {};
}

std::vector<ObjectiveLine> objectiveLines(const game::QuestDef& def, const game::QuestRecord& rec, const game::Inventory& inv,
                                          const game::ItemCatalog* items)
{
    std::vector<ObjectiveLine> out;
    for (std::size_t i = 0; i < def.objectives.size(); ++i)
    {
        const std::uint16_t p = game::objectiveProgress(def, rec, inv, items, i);
        out.push_back({objectiveText(def, i, p), p >= def.objectives[i].count});
    }
    return out;
}

std::vector<RewardLine> rewardLines(const game::QuestDef& def, std::uint8_t charClass, const game::ItemCatalog* items)
{
    std::vector<RewardLine> out;
    if (def.reward.xp > 0)
        out.push_back({std::format("{} deneyim", def.reward.xp), 0, 0});
    if (def.reward.coins > 0)
        out.push_back({std::format("{} Akçe", def.reward.coins), 0, 0});
    for (const game::QuestRewardItem& r : def.reward.items)
        out.push_back({r.count > 1 ? std::format("{} × {}", r.count, itemName(items, r.templateId)) : itemName(items, r.templateId),
                       r.templateId, r.count});
    if (charClass < def.reward.classItems.size() && def.reward.classItems[charClass] != 0)
    {
        const game::ItemTemplateId id = def.reward.classItems[charClass];
        out.push_back({std::format("{} (sınıfına göre)", itemName(items, id)), id, 1});
    }
    return out;
}

std::vector<RewardLine> rewardChoices(const game::QuestDef& def, const game::ItemCatalog* items)
{
    std::vector<RewardLine> out;
    for (const game::ItemTemplateId id : def.reward.choice)
        out.push_back({itemName(items, id), id, 1});
    return out;
}

std::string questStatusText(const game::QuestDef& def, QuestStatus status)
{
    switch (status)
    {
    case QuestStatus::Available: return "Yeni görev";
    case QuestStatus::Active: return "Devam ediyor";
    case QuestStatus::Ready: return "Teslim edilebilir";
    case QuestStatus::Later: return std::format("{}. seviyede", def.level);
    case QuestStatus::Cooldown: return "Daha sonra yeniden";
    case QuestStatus::Done: return "Tamamlandı";
    case QuestStatus::Hidden: break;
    }
    return {};
}

std::string npcName(const game::NpcCatalog* npcs, std::uint16_t npcId)
{
    const game::NpcDef* n = npcs != nullptr ? npcs->find(npcId) : nullptr;
    return n != nullptr ? n->name : std::string("?");
}

std::string questResultText(vn::QuestResult r, const game::QuestDef* def, const game::NpcCatalog* npcs)
{
    using R = vn::QuestResult;
    switch (r)
    {
    case R::Ok: return {};
    case R::UnknownQuest: return "Böyle bir görev yok.";
    case R::WrongNpc:
        if (def != nullptr)
            return std::format("Bu görev {} ile konuşularak verilir ve teslim edilir.", npcName(npcs, def->turnIn));
        return "Bu görev onunla değil.";
    case R::OutOfRange: return "Çok uzaktasın: biraz yaklaş.";
    case R::YouAreDead: return "Ölüyken görev alınmaz, teslim edilmez.";
    case R::LevelTooLow:
        return def != nullptr ? std::format("Bu görev için {}. seviyeye ulaşmalısın.", def->level)
                              : std::string("Seviyen bu görev için yetmiyor.");
    case R::WrongRace: return "Bu görev senin halkına verilmez.";
    case R::NeedsQuest:
        if (def != nullptr)
            if (const game::QuestDef* after = game::quests().find(def->after))
                return std::format("Önce \"{}\" görevini bitirmelisin.", after->name);
        return "Önce önceki görevi bitirmelisin.";
    case R::AlreadyActive: return "Bu görev zaten sende.";
    case R::AlreadyDone: return "Bu görevi zaten tamamladın.";
    case R::OnCooldown: return "Bu görev henüz yeniden alınamaz: biraz sonra gel.";
    case R::TooManyActive:
        return std::format("En çok {} görev taşıyabilirsin: önce birini bitir ya da bırak.", game::quests().maxActive());
    case R::NotActive: return "Bu görev sende değil.";
    case R::NotReady: return "Görev henüz tamamlanmadı.";
    case R::BagFull: return "Çantan dolu: ödül sığmıyor. Yer açıp yeniden dene.";
    case R::BadChoice: return "Önce ödüllerden birini seç.";
    case R::PurseFull: return "Kesen dolu: Akçe ödülü sığmıyor.";
    case R::ServerError: return "Sunucu hatası: görev değişmedi, yeniden dene.";
    }
    return {};
}

std::string abandonQuestion(const game::QuestDef& def)
{
    return std::format("\"{}\" bırakılsın mı? İlerlemen silinir; görevi yeniden alabilirsin.", def.name);
}

std::string handInText(const game::QuestDef& def, std::uint32_t xp, std::uint32_t coins)
{
    std::string reward;
    if (xp > 0)
        reward += std::format("+{} deneyim", xp);
    if (coins > 0)
        reward += std::format("{}+{} Akçe", reward.empty() ? "" : ", ", coins);
    return reward.empty() ? std::format("Görev teslim edildi: {}", def.name)
                          : std::format("Görev teslim edildi: {} ({})", def.name, reward);
}

// ---- Automation ----

std::optional<QuestCommand> parseQuestCommand(std::string_view line, std::string& error)
{
    error.clear();
    std::istringstream in{std::string(line)};
    std::string verb;
    in >> verb;
    QuestCommand c;
    const auto readId = [&](std::uint32_t& out, const char* usage) {
        if (!(in >> out))
        {
            error = std::string("usage: ") + usage;
            return false;
        }
        return true;
    };
    const auto rest = [&]() {
        std::string s;
        std::getline(in >> std::ws, s);
        return s;
    };
    // Names with spaces: underscores in the script ("Avcı_Selvi").
    const auto npcWord = [&](std::string& out, const char* usage) {
        if (!(in >> out))
        {
            error = std::string("usage: ") + usage;
            return false;
        }
        std::replace(out.begin(), out.end(), '_', ' ');
        return true;
    };
    if (verb == "questselect")
    {
        c.verb = QuestVerb::Select;
        if (!readId(c.a, "questselect <quest id>"))
            return std::nullopt;
    }
    else if (verb == "questaccept")
        c.verb = QuestVerb::Accept;
    else if (verb == "questhandin")
    {
        c.verb = QuestVerb::HandIn;
        if (in >> c.b)
            c.hasB = true;
    }
    else if (verb == "questdecline")
        c.verb = QuestVerb::Decline;
    else if (verb == "questacceptforce" || verb == "questhandinforce")
    {
        c.verb = verb == "questacceptforce" ? QuestVerb::AcceptForce : QuestVerb::HandInForce;
        if (!npcWord(c.text, "questacceptforce|questhandinforce <npc name> <quest id> [choice]") ||
            !readId(c.a, "questacceptforce|questhandinforce <npc name> <quest id> [choice]"))
            return std::nullopt;
        if (in >> c.b)
            c.hasB = true;
    }
    else if (verb == "questabandon" || verb == "questabandonforce")
    {
        c.verb = verb == "questabandon" ? QuestVerb::Abandon : QuestVerb::AbandonForce;
        if (!readId(c.a, "questabandon[force] <quest id>"))
            return std::nullopt;
    }
    else if (verb == "questlog")
    {
        c.verb = QuestVerb::Log;
        if (!(in >> c.text) || (c.text != "open" && c.text != "close"))
        {
            error = "usage: questlog open|close";
            return std::nullopt;
        }
    }
    else if (verb == "questlogselect")
    {
        c.verb = QuestVerb::LogSelect;
        if (!readId(c.a, "questlogselect <quest id>"))
            return std::nullopt;
    }
    else if (verb == "expectquest")
    {
        c.verb = QuestVerb::ExpectState;
        if (!readId(c.a, "expectquest <id> none|active|ready|done|cooldown [p1 p2 p3]") || !(in >> c.text))
        {
            error = "usage: expectquest <id> none|active|ready|done|cooldown [p1 p2 p3]";
            return std::nullopt;
        }
        std::uint32_t p = 0;
        while (in >> p)
            c.progress.push_back(static_cast<std::uint16_t>(p));
    }
    else if (verb == "expectmarker")
    {
        c.verb = QuestVerb::ExpectMarker;
        std::string state;
        if (!npcWord(c.text, "expectmarker <npc name> none|available|progress|ready") || !(in >> state))
        {
            error = "usage: expectmarker <npc name> none|available|progress|ready";
            return std::nullopt;
        }
        c.text += '\n' + state;
    }
    else if (verb == "expecttracker")
    {
        c.verb = QuestVerb::ExpectTracker;
        c.text = rest();
    }
    else if (verb == "expectquestmessage")
    {
        c.verb = QuestVerb::ExpectMessage;
        c.text = rest();
    }
    else if (verb == "expectquestnotice")
    {
        c.verb = QuestVerb::ExpectNotice;
        c.text = rest();
    }
    else if (verb == "expectquestdialog")
    {
        c.verb = QuestVerb::ExpectDialog;
        if (!npcWord(c.text, "expectquestdialog <npc name>|none"))
            return std::nullopt;
    }
    else if (verb == "questdump")
        c.verb = QuestVerb::Dump;
    else
        return std::nullopt;
    return c;
}

} // namespace vo::client::world
