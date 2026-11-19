#include "shared/game/Quests.h"

#include "shared/core/Log.h"
#include "shared/core/Paths.h"
#include "shared/game/Character.h"
#include "shared/game/Combat.h"
#include "shared/game/Monsters.h"
#include "shared/game/Npcs.h"

#include <toml++/toml.hpp>

#include <algorithm>
#include <atomic>
#include <format>
#include <fstream>
#include <mutex>
#include <sstream>

namespace vo::game {

namespace {

// Collects the first validation error with "<source>:<line>: " in front (as Npcs.cpp).
class Problems
{
public:
    explicit Problems(std::string_view source) : m_source(source) {}

    void add(const toml::node* at, const std::string& what)
    {
        if (!m_error.empty())
            return;
        if (at && at->source().begin.line > 0)
            m_error = std::format("{}:{}: {}", m_source, at->source().begin.line, what);
        else
            m_error = std::format("{}: {}", m_source, what);
    }
    [[nodiscard]] bool any() const noexcept { return !m_error.empty(); }
    [[nodiscard]] const std::string& error() const noexcept { return m_error; }

private:
    std::string m_source;
    std::string m_error;
};

bool onlyKeys(const toml::table& t, std::initializer_list<std::string_view> allowed, std::string_view what,
              Problems& p)
{
    for (auto&& [key, value] : t)
    {
        if (std::find(allowed.begin(), allowed.end(), key.str()) == allowed.end())
        {
            p.add(&value, std::format("{}: unknown key '{}'", what, key.str()));
            return false;
        }
    }
    return true;
}

std::optional<std::string> getString(const toml::table& t, std::string_view key, std::string_view what, Problems& p,
                                     std::size_t maxBytes, bool required = true)
{
    const toml::node* n = t.get(key);
    if (!n)
    {
        if (required)
            p.add(&t, std::format("{}: missing '{}'", what, key));
        return std::nullopt;
    }
    const auto* s = n->as_string();
    if (!s)
    {
        p.add(n, std::format("{}: '{}' must be a string", what, key));
        return std::nullopt;
    }
    if (s->get().empty())
    {
        p.add(n, std::format("{}: '{}' must not be empty", what, key));
        return std::nullopt;
    }
    if (s->get().size() > maxBytes)
    {
        p.add(n, std::format("{}: '{}' is longer than {} bytes", what, key, maxBytes));
        return std::nullopt;
    }
    return s->get();
}

// nullopt + a problem when present but invalid; `fallback` when absent (unless `required`).
std::optional<std::int64_t> getInt(const toml::table& t, std::string_view key, std::string_view what, Problems& p,
                                   std::int64_t min, std::int64_t max, std::optional<std::int64_t> fallback = std::nullopt)
{
    const toml::node* n = t.get(key);
    if (!n)
    {
        if (!fallback)
            p.add(&t, std::format("{}: missing '{}'", what, key));
        return fallback;
    }
    const auto* i = n->as_integer();
    if (!i)
    {
        p.add(n, std::format("{}: '{}' must be an integer", what, key));
        return std::nullopt;
    }
    const std::int64_t v = i->get();
    if (v < min || v > max)
    {
        p.add(n, std::format("{}: '{}' = {} is out of range ({}..{})", what, key, v, min, max));
        return std::nullopt;
    }
    return v;
}

// An array of integer ids 1..65535 (1..maxCount entries, no duplicates).
std::optional<std::vector<std::uint16_t>> getIds(const toml::table& t, std::string_view key, std::string_view what,
                                                 Problems& p, std::size_t minCount, std::size_t maxCount)
{
    const toml::node* n = t.get(key);
    const toml::array* a = n ? n->as_array() : nullptr;
    if (!n)
    {
        p.add(&t, std::format("{}: missing '{}'", what, key));
        return std::nullopt;
    }
    if (!a || a->size() < minCount || a->size() > maxCount)
    {
        p.add(n, std::format("{}: '{}' must be an array of {}..{} ids", what, key, minCount, maxCount));
        return std::nullopt;
    }
    std::vector<std::uint16_t> out;
    for (const toml::node& e : *a)
    {
        const auto* i = e.as_integer();
        if (!i || i->get() < 1 || i->get() > 65535)
        {
            p.add(&e, std::format("{}: '{}' entries must be ids 1..65535", what, key));
            return std::nullopt;
        }
        const auto v = static_cast<std::uint16_t>(i->get());
        if (std::find(out.begin(), out.end(), v) != out.end())
        {
            p.add(&e, std::format("{}: id {} twice in '{}'", what, v, key));
            return std::nullopt;
        }
        out.push_back(v);
    }
    return out;
}

constexpr std::array<std::string_view, 5> kClassKeys = {"", "warrior", "rogue", "mage", "priest"};

bool checkNpc(const NpcCatalog* npcs, std::uint16_t id, const toml::node* at, std::string_view what, Problems& p)
{
    if (npcs && !npcs->find(id))
    {
        p.add(at, std::format("{}: NPC {} is not in npcs.toml", what, id));
        return false;
    }
    return true;
}

bool checkItem(const ItemCatalog* items, ItemTemplateId id, const toml::node* at, std::string_view what, Problems& p,
               const ItemTemplate** out = nullptr)
{
    if (!items)
        return true;
    const ItemTemplate* t = items->find(id);
    if (!t)
    {
        p.add(at, std::format("{}: item {} is not in items.toml", what, id));
        return false;
    }
    if (out)
        *out = t;
    return true;
}

bool parseObjective(const toml::table& t, const QuestDef& q, const ItemCatalog* items, const NpcCatalog* npcs,
                    const MonsterData* monsters, QuestObjective& out, Problems& p)
{
    const std::string what = std::format("[[quest]] {} objective", q.id);
    if (!onlyKeys(t, {"type", "monsters", "item", "npc", "count", "label"}, what, p))
        return false;
    const auto type = getString(t, "type", what, p, 16);
    if (!type)
        return false;
    QuestObjective o;
    if (*type == "kill")
        o.kind = ObjectiveKind::Kill;
    else if (*type == "boss")
        o.kind = ObjectiveKind::Boss;
    else if (*type == "collect")
        o.kind = ObjectiveKind::Collect;
    else if (*type == "talk")
        o.kind = ObjectiveKind::Talk;
    else
    {
        p.add(t.get("type"), std::format("{}: unknown type '{}' (\"kill\", \"boss\", \"collect\", \"talk\")", what, *type));
        return false;
    }
    const auto forbid = [&](std::string_view key) {
        if (const toml::node* n = t.get(key))
        {
            p.add(n, std::format("{}: '{}' does not belong to a {} objective", what, key, *type));
            return false;
        }
        return true;
    };
    if (const auto label = getString(t, "label", what, p, 48, false))
        o.label = *label;
    if (p.any())
        return false;
    switch (o.kind)
    {
    case ObjectiveKind::Kill:
    case ObjectiveKind::Boss:
    {
        if (!forbid("item") || !forbid("npc"))
            return false;
        const auto ids = getIds(t, "monsters", what, p, 1, 8);
        const auto count = getInt(t, "count", what, p, 1, o.kind == ObjectiveKind::Boss ? 5 : kMaxObjectiveCount);
        if (!ids || !count)
            return false;
        o.monsters = *ids;
        o.count = static_cast<std::uint16_t>(*count);
        if (monsters)
        {
            for (const std::uint16_t m : o.monsters)
            {
                const MonsterTemplate* mt = monsters->find(m);
                if (!mt)
                {
                    p.add(t.get("monsters"), std::format("{}: monster {} is not in monsters.toml", what, m));
                    return false;
                }
                if (o.kind == ObjectiveKind::Boss && !mt->elite)
                {
                    p.add(t.get("monsters"), std::format("{}: monster {} ({}) is not a boss (elite)", what, m, mt->name));
                    return false;
                }
                if (o.label.empty())
                    o.label = mt->name;
            }
        }
        break;
    }
    case ObjectiveKind::Collect:
    {
        if (!forbid("monsters") || !forbid("npc"))
            return false;
        const auto item = getInt(t, "item", what, p, 1, 65535);
        const auto count = getInt(t, "count", what, p, 1, kMaxObjectiveCount);
        if (!item || !count)
            return false;
        o.item = static_cast<ItemTemplateId>(*item);
        o.count = static_cast<std::uint16_t>(*count);
        const ItemTemplate* it = nullptr;
        if (!checkItem(items, o.item, t.get("item"), what, p, &it))
            return false;
        if (it)
        {
            if (!it->isStackable() || it->maxStack < o.count)
            {
                p.add(t.get("item"), std::format("{}: item {} ({}) must stack at least {} high", what, o.item, it->name, o.count));
                return false;
            }
            if (o.label.empty())
                o.label = it->name;
        }
        break;
    }
    case ObjectiveKind::Talk:
    {
        if (!forbid("monsters") || !forbid("item") || !forbid("count"))
            return false;
        const auto npc = getInt(t, "npc", what, p, 1, kMaxNpcId);
        if (!npc)
            return false;
        o.npc = static_cast<std::uint16_t>(*npc);
        o.count = 1;
        if (!checkNpc(npcs, o.npc, t.get("npc"), what, p))
            return false;
        if (o.npc != q.turnIn || q.turnIn == q.giver)
        {
            p.add(t.get("npc"), std::format("{}: a talk objective's NPC must be the quest's turn_in NPC (and not its giver)", what));
            return false;
        }
        if (o.label.empty() && npcs)
            o.label = npcs->find(o.npc)->name;
        break;
    }
    }
    if (o.label.empty())
        o.label = std::string(objectiveKindName(o.kind)); // (no catalogs: tests)
    out = std::move(o);
    return true;
}

bool parseReward(const toml::table& t, const QuestDef& q, const ItemCatalog* items, QuestReward& out, Problems& p)
{
    const std::string what = std::format("[[quest]] {} reward", q.id);
    if (!onlyKeys(t, {"xp", "coins", "items", "class_items", "choice"}, what, p))
        return false;
    QuestReward r;
    const auto xp = getInt(t, "xp", what, p, 0, kMaxQuestXp, 0);
    const auto coins = getInt(t, "coins", what, p, 0, kMaxQuestCoins, 0);
    if (!xp || !coins)
        return false;
    r.xp = static_cast<std::uint32_t>(*xp);
    r.coins = static_cast<std::uint32_t>(*coins);
    if (const toml::node* n = t.get("items"))
    {
        const toml::array* a = n->as_array();
        if (!a || a->empty() || a->size() > 4)
        {
            p.add(n, std::format("{}: 'items' must be an array of 1..4 [item, count] pairs", what));
            return false;
        }
        for (const toml::node& e : *a)
        {
            const toml::array* pair = e.as_array();
            const auto* id = pair && pair->size() == 2 ? pair->get(0)->as_integer() : nullptr;
            const auto* count = pair && pair->size() == 2 ? pair->get(1)->as_integer() : nullptr;
            if (!id || !count || id->get() < 1 || id->get() > 65535 || count->get() < 1 || count->get() > 99)
            {
                p.add(&e, std::format("{}: 'items' entries are [item id, count 1..99]", what));
                return false;
            }
            QuestRewardItem ri{static_cast<ItemTemplateId>(id->get()), static_cast<std::uint16_t>(count->get())};
            const ItemTemplate* it = nullptr;
            if (!checkItem(items, ri.templateId, &e, what, p, &it))
                return false;
            if (it && ri.count > it->maxStack)
            {
                p.add(&e, std::format("{}: {} x item {} is more than one stack", what, ri.count, ri.templateId));
                return false;
            }
            for (const QuestRewardItem& other : r.items)
                if (other.templateId == ri.templateId)
                {
                    p.add(&e, std::format("{}: item {} twice in 'items'", what, ri.templateId));
                    return false;
                }
            r.items.push_back(ri);
        }
    }
    if (const toml::node* n = t.get("class_items"))
    {
        const toml::table* ct = n->as_table();
        if (!ct)
        {
            p.add(n, std::format("{}: 'class_items' must be a table {{ warrior = id, rogue = id, mage = id, priest = id }}", what));
            return false;
        }
        if (!onlyKeys(*ct, {"warrior", "rogue", "mage", "priest"}, what + " class_items", p))
            return false;
        for (std::uint8_t cls = 1; cls <= 4; ++cls)
        {
            const auto id = getInt(*ct, kClassKeys[cls], what + " class_items", p, 1, 65535);
            if (!id)
                return false;
            r.classItems[cls] = static_cast<ItemTemplateId>(*id);
            const ItemTemplate* it = nullptr;
            if (!checkItem(items, r.classItems[cls], ct->get(kClassKeys[cls]), what, p, &it))
                return false;
            if (it && (!it->isEquippable() || !it->allowsClass(cls)))
            {
                p.add(ct->get(kClassKeys[cls]),
                      std::format("{}: class item {} ({}) is not equipment a {} may wear", what, it->id, it->name, kClassKeys[cls]));
                return false;
            }
        }
    }
    if (const toml::node* n = t.get("choice"))
    {
        const auto ids = getIds(t, "choice", what, p, 2, 4);
        if (!ids)
            return false;
        for (const std::uint16_t id : *ids)
            if (!checkItem(items, id, n, what, p))
                return false;
        r.choice = *ids;
    }
    if (r.xp == 0 && r.coins == 0 && r.items.empty() && !r.hasClassItems() && r.choice.empty())
    {
        p.add(&t, std::format("{}: a quest must reward something", what));
        return false;
    }
    out = std::move(r);
    return true;
}

bool parseQuest(const toml::table& t, const QuestCatalog& sofar, const ItemCatalog* items, const NpcCatalog* npcs,
                const MonsterData* monsters, QuestDef& out, Problems& p)
{
    if (!onlyKeys(t, {"id", "name", "giver", "turn_in", "level", "race", "after", "repeatable", "cooldown_s", "story",
                      "progress_text", "done_text", "objective", "reward"},
                  "[[quest]]", p))
        return false;
    const auto id = getInt(t, "id", "[[quest]]", p, 1, kMaxQuestId);
    if (!id)
        return false;
    QuestDef q;
    q.id = static_cast<QuestId>(*id);
    const std::string where = std::format("[[quest]] {}", q.id);
    const auto name = getString(t, "name", where, p, 64);
    const auto giver = getInt(t, "giver", where, p, 1, kMaxNpcId);
    const auto level = getInt(t, "level", where, p, 1, kLevelCap, 1);
    const auto story = getString(t, "story", where, p, 900);
    const auto done = getString(t, "done_text", where, p, 500);
    if (!name || !giver || !level || !story || !done)
        return false;
    q.name = *name;
    q.giver = static_cast<std::uint16_t>(*giver);
    q.level = static_cast<std::uint16_t>(*level);
    q.story = *story;
    q.doneText = *done;
    if (const auto pt = getString(t, "progress_text", where, p, 500, false))
        q.progressText = *pt;
    const auto turnIn = getInt(t, "turn_in", where, p, 1, kMaxNpcId, q.giver);
    if (!turnIn || p.any())
        return false;
    q.turnIn = static_cast<std::uint16_t>(*turnIn);
    if (!checkNpc(npcs, q.giver, t.get("giver"), where, p) || !checkNpc(npcs, q.turnIn, t.get("turn_in"), where, p))
        return false;
    if (const auto race = getString(t, "race", where, p, 16, false))
    {
        if (*race == "aydor")
            q.race = static_cast<std::uint8_t>(Race::Aydor);
        else if (*race == "kargan")
            q.race = static_cast<std::uint8_t>(Race::Kargan);
        else
        {
            p.add(t.get("race"), std::format("{}: unknown race '{}' (\"aydor\", \"kargan\")", where, *race));
            return false;
        }
    }
    if (p.any())
        return false;
    const auto after = getInt(t, "after", where, p, 1, kMaxQuestId, 0);
    if (!after)
        return false;
    q.after = static_cast<QuestId>(*after);
    if (q.after != 0 && (q.after == q.id || !sofar.find(q.after)))
    {
        p.add(t.get("after"), std::format("{}: 'after' = {} must name a quest earlier in the file", where, q.after));
        return false;
    }
    if (const toml::node* r = t.get("repeatable"))
    {
        const auto* b = r->as_boolean();
        if (!b)
        {
            p.add(r, std::format("{}: 'repeatable' must be true or false", where));
            return false;
        }
        q.repeatable = b->get();
    }
    if (q.repeatable)
    {
        const auto cd = getInt(t, "cooldown_s", where, p, kMinQuestCooldownS, kMaxQuestCooldownS);
        if (!cd)
            return false;
        q.cooldownS = static_cast<std::uint32_t>(*cd);
    }
    else if (const toml::node* cd = t.get("cooldown_s"))
    {
        p.add(cd, std::format("{}: 'cooldown_s' is only for a repeatable quest", where));
        return false;
    }
    const toml::node* objNode = t.get("objective");
    const toml::array* objs = objNode ? objNode->as_array() : nullptr;
    if (!objNode)
    {
        p.add(&t, std::format("{}: no [[quest.objective]]", where));
        return false;
    }
    if (!objs || !objs->is_array_of_tables() || objs->empty() || objs->size() > kMaxQuestObjectives)
    {
        p.add(objNode, std::format("{}: 1..{} [[quest.objective]] tables", where, kMaxQuestObjectives));
        return false;
    }
    for (const toml::node& o : *objs)
    {
        QuestObjective obj;
        if (!parseObjective(*o.as_table(), q, items, npcs, monsters, obj, p))
            return false;
        q.objectives.push_back(std::move(obj));
    }
    if (q.turnIn != q.giver &&
        std::none_of(q.objectives.begin(), q.objectives.end(), [](const QuestObjective& o) { return o.kind == ObjectiveKind::Talk; }))
    {
        p.add(objNode, std::format("{}: handed in to another NPC: it needs a talk objective for that NPC", where));
        return false;
    }
    const toml::node* rewardNode = t.get("reward");
    const toml::table* reward = rewardNode ? rewardNode->as_table() : nullptr;
    if (!reward)
    {
        p.add(rewardNode ? rewardNode : &t, std::format("{}: needs a [quest.reward] table", where));
        return false;
    }
    if (!parseReward(*reward, q, items, q.reward, p))
        return false;
    out = std::move(q);
    return true;
}

struct Global
{
    std::mutex mutex;
    std::vector<std::shared_ptr<const QuestCatalog>> installed; // kept alive forever
    std::atomic<const QuestCatalog*> current{nullptr};
};

Global& global()
{
    static Global g;
    return g;
}

const QuestCatalog& defaultCatalog()
{
    static const QuestCatalog catalog = [] {
        QuestCatalog c;
        std::string error;
        ItemCatalog items;
        const ItemCatalog* itemsPtr = nullptr;
        if (const auto f = locateItemsFile(); f && loadItemCatalog(*f, items, error))
            itemsPtr = &items;
        MonsterData monsters;
        const MonsterData* monstersPtr = nullptr;
        if (const auto f = locateMonstersFile(); f && loadMonsterData(*f, monsters, error))
            monstersPtr = &monsters;
        error.clear();
        const NpcCatalog& npcCatalog = npcs();
        if (const auto file = locateQuestsFile())
        {
            if (!loadQuestCatalog(*file, itemsPtr, npcCatalog.empty() ? nullptr : &npcCatalog, monstersPtr, c, error))
                c = {};
        }
        else
            error = std::format("{} not found (searched upwards from the working directory and the executable directory)",
                                kQuestsFile);
        if (!error.empty())
            VO_LOG_ERROR("quests: {}", error);
        return c;
    }();
    return catalog;
}

} // namespace

bool QuestReward::hasClassItems() const noexcept
{
    return std::any_of(classItems.begin(), classItems.end(), [](ItemTemplateId id) { return id != 0; });
}

std::string_view objectiveKindName(ObjectiveKind k) noexcept
{
    switch (k)
    {
    case ObjectiveKind::Kill: return "kill";
    case ObjectiveKind::Boss: return "boss";
    case ObjectiveKind::Collect: return "collect";
    case ObjectiveKind::Talk: return "talk";
    }
    return "?";
}

std::string_view questMarkerName(QuestMarker m) noexcept
{
    switch (m)
    {
    case QuestMarker::None: return "none";
    case QuestMarker::InProgress: return "progress";
    case QuestMarker::Available: return "available";
    case QuestMarker::Ready: return "ready";
    }
    return "?";
}

// ---- Catalog ----

const QuestDef* QuestCatalog::find(QuestId id) const noexcept
{
    for (const QuestDef& q : m_quests)
        if (q.id == id)
            return &q;
    return nullptr;
}

std::vector<const QuestDef*> QuestCatalog::givenBy(std::uint16_t npcId) const
{
    std::vector<const QuestDef*> out;
    for (const QuestDef& q : m_quests)
        if (q.giver == npcId)
            out.push_back(&q);
    return out;
}

std::vector<const QuestDef*> QuestCatalog::handedInTo(std::uint16_t npcId) const
{
    std::vector<const QuestDef*> out;
    for (const QuestDef& q : m_quests)
        if (q.turnIn == npcId)
            out.push_back(&q);
    return out;
}

bool parseQuestCatalog(std::string_view tomlText, std::string_view sourceName, const ItemCatalog* items,
                       const NpcCatalog* npcs, const MonsterData* monsters, QuestCatalog& out, std::string& error)
{
    Problems p(sourceName);
    toml::table root;
    try
    {
        root = toml::parse(tomlText, sourceName);
    }
    catch (const toml::parse_error& e)
    {
        error = std::format("{}:{}: {}", sourceName, e.source().begin.line, e.description());
        return false;
    }
    QuestCatalog c;
    if (!onlyKeys(root, {"rules", "quest"}, "top level", p))
    {
        error = p.error();
        return false;
    }
    if (const toml::node* r = root.get("rules"))
    {
        const toml::table* rules = r->as_table();
        if (!rules)
            p.add(r, "'rules' must be a table");
        else if (onlyKeys(*rules, {"max_active"}, "[rules]", p))
        {
            if (const auto m = getInt(*rules, "max_active", "[rules]", p, 1, 25, static_cast<std::int64_t>(kDefaultMaxActiveQuests)))
                c.m_maxActive = static_cast<std::size_t>(*m);
        }
    }
    const toml::node* node = root.get("quest");
    const toml::array* list = node ? node->as_array() : nullptr;
    if (p.any())
    {
    }
    else if (!node)
        p.add(nullptr, "no [[quest]] entries");
    else if (!list || !list->is_array_of_tables())
        p.add(node, "'quest' must be an array of tables ([[quest]])");
    else if (list->empty())
        p.add(node, "no [[quest]] entries");
    else if (list->size() > kMaxQuests)
        p.add(node, std::format("more than {} quests", kMaxQuests));
    if (!p.any())
    {
        for (const toml::node& n : *list)
        {
            QuestDef def;
            if (!parseQuest(*n.as_table(), c, items, npcs, monsters, def, p))
                break;
            if (c.find(def.id))
            {
                p.add(&n, std::format("[[quest]]: id {} used twice", def.id));
                break;
            }
            c.m_quests.push_back(std::move(def));
        }
    }
    if (p.any())
    {
        error = p.error();
        return false;
    }
    out = std::move(c);
    return true;
}

bool loadQuestCatalog(const std::filesystem::path& file, const ItemCatalog* items, const NpcCatalog* npcs,
                      const MonsterData* monsters, QuestCatalog& out, std::string& error)
{
    std::ifstream in(file, std::ios::binary);
    if (!in)
    {
        error = std::format("cannot open {}", core::pathToUtf8(file));
        return false;
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    if (in.bad())
    {
        error = std::format("cannot read {}", core::pathToUtf8(file));
        return false;
    }
    return parseQuestCatalog(ss.str(), core::pathToUtf8(file), items, npcs, monsters, out, error);
}

std::optional<std::filesystem::path> locateQuestsFile()
{
    return core::locateDataFile(std::filesystem::path(kQuestsFile));
}

const QuestCatalog& quests() noexcept
{
    if (const QuestCatalog* c = global().current.load(std::memory_order_acquire))
        return *c;
    return defaultCatalog();
}

void installQuestCatalog(std::shared_ptr<const QuestCatalog> catalog)
{
    if (!catalog)
        return;
    Global& g = global();
    const std::scoped_lock lock(g.mutex);
    g.installed.push_back(catalog);
    g.current.store(catalog.get(), std::memory_order_release);
}

// ---- Log ----

const QuestRecord* QuestLog::find(QuestId id) const noexcept
{
    const auto it = std::lower_bound(m_records.begin(), m_records.end(), id,
                                     [](const QuestRecord& r, QuestId v) { return r.quest < v; });
    return it != m_records.end() && it->quest == id ? &*it : nullptr;
}

QuestRecord* QuestLog::find(QuestId id) noexcept
{
    return const_cast<QuestRecord*>(static_cast<const QuestLog*>(this)->find(id));
}

std::size_t QuestLog::activeCount() const noexcept
{
    return static_cast<std::size_t>(
        std::count_if(m_records.begin(), m_records.end(), [](const QuestRecord& r) { return r.state == QuestState::Active; }));
}

bool QuestLog::isDone(QuestId id) const noexcept
{
    const QuestRecord* r = find(id);
    return r != nullptr && (r->state == QuestState::Done || r->completions > 0);
}

void QuestLog::put(const QuestRecord& r)
{
    const auto it = std::lower_bound(m_records.begin(), m_records.end(), r.quest,
                                     [](const QuestRecord& a, QuestId v) { return a.quest < v; });
    if (it != m_records.end() && it->quest == r.quest)
        *it = r;
    else
        m_records.insert(it, r);
}

void QuestLog::erase(QuestId id)
{
    std::erase_if(m_records, [id](const QuestRecord& r) { return r.quest == id; });
}

// ---- Rules ----

std::uint32_t questCooldownLeft(const QuestDef& def, const QuestRecord* rec, std::int64_t now) noexcept
{
    if (!def.repeatable || rec == nullptr || rec->state != QuestState::Done)
        return 0;
    const std::int64_t ready = rec->completedAt + static_cast<std::int64_t>(def.cooldownS);
    return ready > now ? static_cast<std::uint32_t>(std::min<std::int64_t>(ready - now, def.cooldownS)) : 0u;
}

QuestResult checkQuestAccept(const QuestCatalog& catalog, const QuestDef& def, const QuestLog& log, const QuestUser& user,
                             std::int64_t now, std::size_t maxActive)
{
    (void)catalog;
    const QuestRecord* rec = log.find(def.id);
    if (rec != nullptr && rec->state == QuestState::Active)
        return QuestResult::AlreadyActive;
    if (rec != nullptr && rec->state == QuestState::Done)
    {
        if (!def.repeatable)
            return QuestResult::AlreadyDone;
        if (questCooldownLeft(def, rec, now) > 0)
            return QuestResult::OnCooldown;
    }
    if (def.race != 0 && def.race != user.race)
        return QuestResult::WrongRace;
    if (user.level < def.level)
        return QuestResult::LevelTooLow;
    if (def.after != 0 && !log.isDone(def.after))
        return QuestResult::NeedsQuest;
    if (log.activeCount() >= maxActive)
        return QuestResult::TooManyActive;
    return QuestResult::Ok;
}

std::uint32_t bagUnits(const Inventory& inv, ItemTemplateId templateId, const ItemCatalog* items) noexcept
{
    if (items != nullptr && items->find(templateId) == nullptr)
        return 0;
    std::uint32_t n = 0;
    for (std::uint8_t s = 0; s < kBagSlots; ++s)
    {
        const ItemStack& st = inv.at(ItemPlace::bag(s));
        if (!st.empty() && st.templateId == templateId)
            n += st.quantity;
    }
    return n;
}

std::uint16_t objectiveProgress(const QuestDef& def, const QuestRecord& rec, const Inventory& inv, const ItemCatalog* items,
                                std::size_t index) noexcept
{
    if (index >= def.objectives.size())
        return 0;
    const QuestObjective& o = def.objectives[index];
    switch (o.kind)
    {
    case ObjectiveKind::Kill:
    case ObjectiveKind::Boss: return std::min(rec.progress[index], o.count);
    case ObjectiveKind::Collect:
        return static_cast<std::uint16_t>(std::min<std::uint32_t>(bagUnits(inv, o.item, items), o.count));
    case ObjectiveKind::Talk: return rec.state == QuestState::Active ? 1 : 0;
    }
    return 0;
}

bool questReady(const QuestDef& def, const QuestRecord& rec, const Inventory& inv, const ItemCatalog* items) noexcept
{
    if (rec.state != QuestState::Active)
        return false;
    for (std::size_t i = 0; i < def.objectives.size(); ++i)
        if (objectiveProgress(def, rec, inv, items, i) < def.objectives[i].count)
            return false;
    return true;
}

std::vector<QuestId> noteQuestKill(const QuestCatalog& catalog, QuestLog& log, std::uint16_t templateId)
{
    std::vector<QuestId> changed;
    for (const QuestRecord& r : log.records())
    {
        if (r.state != QuestState::Active)
            continue;
        const QuestDef* def = catalog.find(r.quest);
        if (def == nullptr)
            continue;
        QuestRecord* rec = log.find(r.quest);
        bool any = false;
        for (std::size_t i = 0; i < def->objectives.size(); ++i)
        {
            const QuestObjective& o = def->objectives[i];
            if ((o.kind != ObjectiveKind::Kill && o.kind != ObjectiveKind::Boss) || rec->progress[i] >= o.count ||
                std::find(o.monsters.begin(), o.monsters.end(), templateId) == o.monsters.end())
                continue;
            ++rec->progress[i];
            any = true;
        }
        if (any)
            changed.push_back(r.quest);
    }
    return changed;
}

std::optional<std::vector<QuestRewardItem>> questRewardItems(const QuestDef& def, std::uint8_t charClass, std::uint8_t choice)
{
    std::vector<QuestRewardItem> out = def.reward.items;
    if (charClass < def.reward.classItems.size() && def.reward.classItems[charClass] != 0)
        out.push_back({def.reward.classItems[charClass], 1});
    if (def.reward.choice.empty())
    {
        if (choice != net::kQuestNoChoice)
            return std::nullopt;
    }
    else
    {
        if (choice >= def.reward.choice.size())
            return std::nullopt;
        out.push_back({def.reward.choice[choice], 1});
    }
    return out;
}

std::vector<ItemSlotChange> QuestHandInPlan::changes(const Inventory& before, const std::vector<ItemInstanceId>& createdIds) const
{
    Inventory after = before;
    std::vector<ItemSlotChange> out;
    const auto note = [&](ItemPlace place) {
        for (ItemSlotChange& c : out)
            if (c.place == place)
            {
                c.content = after.at(place);
                return;
            }
        out.push_back({place, after.at(place)});
    };
    for (const ItemQuantityChange& q : removed)
    {
        ItemStack& st = after.at(q.place);
        if (q.after == 0)
            st = {};
        else
            st.quantity = q.after;
        note(q.place);
    }
    for (const ItemSlotChange& c : add.changes(after, createdIds))
    {
        after.at(c.place) = c.content;
        note(c.place);
    }
    return out;
}

void QuestHandInPlan::apply(Inventory& inv, const std::vector<ItemInstanceId>& createdIds) const
{
    for (const ItemQuantityChange& q : removed)
    {
        ItemStack& st = inv.at(q.place);
        if (q.after == 0)
            st = {};
        else
            st.quantity = q.after;
    }
    applyItemAdd(inv, add, createdIds);
}

QuestHandInPlan planQuestHandIn(const ItemCatalog& items, const Inventory& inv, std::uint32_t coins, const QuestDef& def,
                                const QuestLog& log, const QuestUser& user, std::uint8_t choice)
{
    QuestHandInPlan plan;
    const QuestRecord* rec = log.find(def.id);
    if (rec == nullptr || rec->state != QuestState::Active)
    {
        plan.result = QuestResult::NotActive;
        return plan;
    }
    if (!questReady(def, *rec, inv, &items))
    {
        plan.result = QuestResult::NotReady;
        return plan;
    }
    const auto rewards = questRewardItems(def, user.charClass, choice);
    if (!rewards)
    {
        plan.result = QuestResult::BadChoice;
        return plan;
    }
    // The collected units go first (lowest bag slots), then the rewards are placed into what is left.
    Inventory after = inv;
    for (const QuestObjective& o : def.objectives)
    {
        if (o.kind != ObjectiveKind::Collect)
            continue;
        std::uint16_t left = o.count;
        for (std::uint8_t s = 0; s < kBagSlots && left > 0; ++s)
        {
            const ItemPlace place = ItemPlace::bag(s);
            ItemStack& st = after.at(place);
            if (st.empty() || st.templateId != o.item)
                continue;
            const std::uint16_t take = std::min(st.quantity, left);
            ItemQuantityChange* existing = nullptr;
            for (ItemQuantityChange& q : plan.removed)
                if (q.id == st.id)
                    existing = &q;
            const auto remaining = static_cast<std::uint16_t>(st.quantity - take);
            if (existing)
                existing->after = remaining;
            else
                plan.removed.push_back({st.id, place, st.quantity, remaining});
            left = static_cast<std::uint16_t>(left - take);
            if (remaining == 0)
                st = {};
            else
                st.quantity = remaining;
        }
    }
    std::vector<ItemAddRequest> requests;
    for (const QuestRewardItem& r : *rewards)
        requests.push_back({r.templateId, r.count});
    plan.add = planItemAdd(items, after, requests);
    for (std::size_t i = 0; i < requests.size(); ++i)
        if (i >= plan.add.placed.size() || plan.add.placed[i] < requests[i].count)
        {
            plan = {};
            plan.result = QuestResult::BagFull;
            return plan;
        }
    if (static_cast<std::uint64_t>(std::min(coins, net::kMaxCoins)) + def.reward.coins > net::kMaxCoins)
    {
        plan = {};
        plan.result = QuestResult::PurseFull;
        return plan;
    }
    plan.rewards = *rewards;
    plan.xp = def.reward.xp;
    plan.coins = def.reward.coins;
    plan.result = QuestResult::Ok;
    return plan;
}

QuestStatus questStatus(const QuestCatalog& catalog, const QuestDef& def, const QuestLog& log, const QuestUser& user,
                        const Inventory& inv, const ItemCatalog* items, std::int64_t now, std::size_t maxActive)
{
    const QuestRecord* rec = log.find(def.id);
    if (rec != nullptr && rec->state == QuestState::Active)
        return questReady(def, *rec, inv, items) ? QuestStatus::Ready : QuestStatus::Active;
    switch (checkQuestAccept(catalog, def, log, user, now, maxActive))
    {
    case QuestResult::Ok:
    case QuestResult::TooManyActive: return QuestStatus::Available; // (the dialog says why it cannot be taken)
    case QuestResult::AlreadyDone: return QuestStatus::Done;
    case QuestResult::OnCooldown: return QuestStatus::Cooldown;
    case QuestResult::LevelTooLow: return QuestStatus::Later;
    default: return QuestStatus::Hidden;
    }
}

QuestMarker questMarkerFor(const QuestCatalog& catalog, std::uint16_t npcId, const QuestLog& log, const QuestUser& user,
                           const Inventory& inv, const ItemCatalog* items, std::int64_t now, std::size_t maxActive)
{
    QuestMarker best = QuestMarker::None;
    for (const QuestDef& q : catalog.quests())
    {
        if (q.giver != npcId && q.turnIn != npcId)
            continue;
        const QuestStatus s = questStatus(catalog, q, log, user, inv, items, now, maxActive);
        QuestMarker m = QuestMarker::None;
        if (s == QuestStatus::Ready && q.turnIn == npcId)
            m = QuestMarker::Ready;
        else if (s == QuestStatus::Available && q.giver == npcId)
            m = QuestMarker::Available;
        else if (s == QuestStatus::Active || s == QuestStatus::Ready)
            m = QuestMarker::InProgress;
        best = std::max(best, m);
    }
    return best;
}

} // namespace vo::game
