#include "server/auth/MemoryAccountService.h"

#include "shared/core/Log.h"
#include "shared/game/Character.h"
#include "shared/game/Combat.h"
#include "shared/game/Names.h"
#include "shared/net/Crypto.h"

#include <algorithm>
#include <utility>

namespace vo::server::auth {

MemoryAccountService::MemoryAccountService(Options options) : m_options(options)
{
    m_worker = std::thread([this] { workerLoop(); });
}

MemoryAccountService::~MemoryAccountService()
{
    stop();
}

void MemoryAccountService::stop()
{
    {
        const std::scoped_lock lock(m_queueMutex);
        if (m_stopping && !m_worker.joinable())
            return;
        m_stopping = true;
    }
    m_queueCv.notify_all();
    if (m_worker.joinable())
        m_worker.join();
}

bool MemoryAccountService::enqueue(std::function<void()> task)
{
    {
        const std::scoped_lock lock(m_queueMutex);
        if (m_stopping)
            return false;
        m_queue.push_back(std::move(task));
    }
    m_queueCv.notify_one();
    return true;
}

void MemoryAccountService::workerLoop()
{
    for (;;)
    {
        std::function<void()> task;
        {
            std::unique_lock lock(m_queueMutex);
            m_queueCv.wait(lock, [this] { return m_stopping || !m_queue.empty(); });
            if (m_queue.empty())
                return; // stopping and drained
            task = std::move(m_queue.front());
            m_queue.pop_front();
        }
        task();
    }
}

// ---- Accounts ----

std::optional<AccountId> MemoryAccountService::insertAccountLocked(const std::string& key, std::string_view username,
                                                                   std::string hash)
{
    if (m_accountsByKey.contains(key))
        return std::nullopt;
    Account acc;
    acc.id = m_nextAccountId++;
    acc.username = std::string(username);
    acc.passwordHash = std::move(hash);
    const AccountId id = acc.id;
    m_accountKeyById[id] = key;
    m_accountsByKey.emplace(key, std::move(acc));
    return id;
}

std::optional<AccountId> MemoryAccountService::createAccount(std::string_view username, std::string_view password)
{
    if (!game::isValidUsername(username) || !game::isValidPassword(password))
        return std::nullopt;
    auto hash = net::hashPassword(password);
    if (!hash)
        return std::nullopt;
    const std::scoped_lock lock(m_dataMutex);
    return insertAccountLocked(game::usernameKey(username), username, std::move(*hash));
}

void MemoryAccountService::setBanned(AccountId account, bool banned)
{
    const std::scoped_lock lock(m_dataMutex);
    const auto keyIt = m_accountKeyById.find(account);
    if (keyIt == m_accountKeyById.end())
        return;
    m_accountsByKey[keyIt->second].banned = banned;
}

std::size_t MemoryAccountService::accountCount() const
{
    const std::scoped_lock lock(m_dataMutex);
    return m_accountsByKey.size();
}

std::vector<CharacterInfo> MemoryAccountService::charactersOf(std::string_view username) const
{
    const std::scoped_lock lock(m_dataMutex);
    std::vector<CharacterInfo> out;
    const auto it = m_accountsByKey.find(game::usernameKey(username));
    if (it == m_accountsByKey.end())
        return out;
    for (const auto& [id, c] : m_characters)
    {
        if (c.account == it->second.id && !c.deleted)
            out.push_back(withEquipment(c));
    }
    return out;
}

LoginOutcome MemoryAccountService::doLogin(const std::string& username, const std::string& password)
{
    const LoginOutcome invalid{net::LoginResult::InvalidCredentials, 0};
    if (!game::isValidUsername(username) || !game::isValidPassword(password))
        return invalid;

    const std::string key = game::usernameKey(username);
    std::optional<Account> account;
    {
        const std::scoped_lock lock(m_dataMutex);
        const auto it = m_accountsByKey.find(key);
        if (it != m_accountsByKey.end())
            account = it->second;
    }

    if (!account)
    {
        if (m_options.autoRegister)
        {
            auto hash = net::hashPassword(password);
            if (!hash)
                return {net::LoginResult::ServerError, 0};
            const std::scoped_lock lock(m_dataMutex);
            if (const auto id = insertAccountLocked(key, username, std::move(*hash)))
            {
                VO_LOG_INFO("auth: auto-registered account {} ('{}')", *id, username);
                return {net::LoginResult::Ok, *id};
            }
            return invalid; // registered concurrently by someone else
        }
        // Spend the same time as a real verification so unknown users are not distinguishable.
        if (m_dummyHash.empty())
            m_dummyHash = net::hashPassword("dummy-password-for-timing").value_or(std::string());
        if (!m_dummyHash.empty())
            (void)net::verifyPassword(m_dummyHash, password);
        return invalid;
    }

    if (!net::verifyPassword(account->passwordHash, password))
        return invalid;
    if (account->banned)
        return {net::LoginResult::Banned, 0};
    return {net::LoginResult::Ok, account->id};
}

void MemoryAccountService::login(std::string username, std::string password, std::string /*remoteAddress*/,
                                 std::function<void(LoginOutcome)> done)
{
    auto task = [this, username = std::move(username), password = std::move(password), done]() mutable {
        const LoginOutcome outcome = doLogin(username, password);
        net::secureZero(password.data(), password.size());
        done(outcome);
    };
    if (!enqueue(std::move(task)))
        done({net::LoginResult::ServerError, 0});
}

// ---- Characters ----

void MemoryAccountService::listCharacters(AccountId account, std::function<void(CharacterListOutcome)> done)
{
    auto task = [this, account, done] {
        CharacterListOutcome out;
        {
            const std::scoped_lock lock(m_dataMutex);
            for (const auto& [id, c] : m_characters)
            {
                if (c.account == account && !c.deleted)
                    out.characters.push_back(withEquipment(c));
            }
        }
        out.ok = out.characters.size() <= net::kMaxCharactersPerAccount;
        if (!out.ok)
            out.characters.clear();
        done(std::move(out));
    };
    if (!enqueue(std::move(task)))
        done({});
}

void MemoryAccountService::createCharacter(AccountId account, CreateCharacterRequest request,
                                           std::function<void(CreateCharacterOutcome)> done)
{
    auto task = [this, account, request = std::move(request), done] {
        using R = net::CharCreateResult;
        CreateCharacterOutcome out;
        if (!game::isValidCharacterName(request.name))
            out.result = R::NameInvalid;
        else if (!game::isAllowedRaceClass(request.race, request.charClass))
            out.result = R::InvalidRaceOrClass;
        else if (!game::isValidAppearance(request.face, request.hair))
            out.result = R::InvalidAppearance;
        else
        {
            const std::string key = game::characterNameKey(request.name);
            const std::scoped_lock lock(m_dataMutex);
            std::size_t used = 0;
            for (const auto& [id, c] : m_characters)
            {
                if (c.account == account && !c.deleted)
                    ++used;
            }
            if (!m_accountKeyById.contains(account))
                out.result = R::ServerError;
            else if (used >= net::kMaxCharactersPerAccount)
                out.result = R::NoFreeSlot;
            else if (m_reservedNames.contains(key))
                out.result = R::NameTaken;
            else
            {
                Character c;
                c.account = account;
                c.info.id = m_nextCharacterId++;
                c.info.name = request.name;
                c.info.race = request.race;
                c.info.charClass = request.charClass;
                c.info.face = request.face;
                c.info.hair = request.hair;
                c.info.level = game::kStartLevel;
                c.info.position = m_options.spawns.forRace(request.race);
                m_reservedNames.insert(key);
                out.result = R::Ok;
                out.character = c.info;
                m_characters.emplace(c.info.id, std::move(c));
            }
        }
        done(std::move(out));
    };
    if (!enqueue(std::move(task)))
        done({});
}

void MemoryAccountService::deleteCharacter(AccountId account, CharacterId character,
                                           std::function<void(DeleteCharacterOutcome)> done)
{
    auto task = [this, account, character, done] {
        DeleteCharacterOutcome out{net::CharDeleteResult::NotFound};
        {
            const std::scoped_lock lock(m_dataMutex);
            const auto it = m_characters.find(character);
            if (it != m_characters.end() && it->second.account == account && !it->second.deleted)
            {
                it->second.deleted = true; // soft delete: name stays in m_reservedNames
                out.result = net::CharDeleteResult::Ok;
            }
        }
        done(out);
    };
    if (!enqueue(std::move(task)))
        done({});
}

void MemoryAccountService::loadCharacterForWorld(AccountId account, CharacterId character,
                                                 std::function<void(LoadCharacterOutcome)> done)
{
    auto task = [this, account, character, done] {
        LoadCharacterOutcome out{net::EnterWorldResult::NotFound, {}};
        {
            const std::scoped_lock lock(m_dataMutex);
            const auto it = m_characters.find(character);
            if (it != m_characters.end() && it->second.account == account && !it->second.deleted)
            {
                out.result = net::EnterWorldResult::Ok;
                out.character = withEquipment(it->second);
            }
        }
        done(std::move(out));
    };
    if (!enqueue(std::move(task)))
        done({});
}

void MemoryAccountService::saveCharacterPosition(AccountId account, CharacterId character,
                                                 CharacterPosition position, std::function<void(bool ok)> done)
{
    auto task = [this, account, character, position, done] {
        bool ok = false;
        {
            const std::scoped_lock lock(m_dataMutex);
            const auto it = m_characters.find(character);
            if (it != m_characters.end() && it->second.account == account && !it->second.deleted)
            {
                it->second.info.position = position;
                ok = true;
            }
        }
        if (done)
            done(ok);
    };
    if (!enqueue(std::move(task)) && done)
        done(false);
}

void MemoryAccountService::saveCharacterState(AccountId account, CharacterId character, CharacterPosition position,
                                              CharacterProgress progress, std::function<void(bool ok)> done)
{
    auto task = [this, account, character, position, progress, done] {
        bool ok = false;
        {
            const std::scoped_lock lock(m_dataMutex);
            const auto it = m_characters.find(character);
            if (it != m_characters.end() && it->second.account == account && !it->second.deleted)
            {
                auto& info = it->second.info;
                info.position = position;
                info.level = std::max<std::uint16_t>(progress.level, 1);
                info.xp = progress.xp;
                info.hp = progress.hp;
                info.mp = progress.mp;
                ok = true;
            }
        }
        if (done)
            done(ok);
    };
    if (!enqueue(std::move(task)) && done)
        done(false);
}

bool MemoryAccountService::setLevel(CharacterId character, std::uint16_t level)
{
    const std::scoped_lock lock(m_dataMutex);
    const auto it = m_characters.find(character);
    if (it == m_characters.end() || it->second.deleted)
        return false;
    auto& info = it->second.info;
    info.level = std::clamp<std::uint16_t>(level, 1, game::kLevelCap);
    info.xp = 0;
    info.hp.reset();
    info.mp.reset();
    if (game::allocatedTotal(info.statPoints) > game::pointsForLevel(info.level))
        info.statPoints = {}; // PROTOCOL_STATS.md §6.2: no longer fits the lower level
    const game::SkillCatalog& catalog = game::skills();
    if (!game::skillRanksFitLevel(catalog, info.charClass, info.level, info.skillRanks))
    {
        info.skillRanks = {}; // PROTOCOL_SKILLS.md §6.2: as reset-skills
        info.skillBar.reset();
    }
    return true;
}

std::optional<CharacterInfo> MemoryAccountService::characterInfo(CharacterId character) const
{
    const std::scoped_lock lock(m_dataMutex);
    const auto it = m_characters.find(character);
    if (it == m_characters.end())
        return std::nullopt;
    return withEquipment(it->second);
}

// ---- Items ----

CharacterInfo MemoryAccountService::withEquipment(const Character& c)
{
    CharacterInfo info = c.info;
    info.mainHandTemplate = 0;
    info.offHandTemplate = 0;
    info.armorTemplates = {};
    for (const auto& item : c.items)
    {
        if (item.place.is(game::EquipSlot::MainHand))
            info.mainHandTemplate = item.templateId;
        else if (item.place.is(game::EquipSlot::OffHand))
            info.offHandTemplate = item.templateId;
        else
            for (std::size_t i = 0; i < game::kArmorSlots; ++i) // M17: the worn armour
                if (item.place.is(game::armorEquipSlot(i)))
                    info.armorTemplates[i] = item.templateId;
    }
    return info;
}

void MemoryAccountService::loadCharacterItems(AccountId account, CharacterId character,
                                              std::optional<game::StarterKit> kit,
                                              std::function<void(LoadItemsOutcome)> done)
{
    auto task = [this, account, character, kit = std::move(kit), done] {
        LoadItemsOutcome out;
        {
            const std::scoped_lock lock(m_dataMutex);
            const auto it = m_characters.find(character);
            if (it != m_characters.end() && it->second.account == account && !it->second.deleted)
            {
                Character& c = it->second;
                if (!c.info.starterKitGranted && kit)
                {
                    std::size_t dropped = 0;
                    for (const auto& n : game::starterKitItems(*kit, c.items, &dropped))
                        c.items.push_back({m_nextItemId++, n.place, n.templateId, n.quantity});
                    if (dropped > 0)
                        VO_LOG_WARN("items: {} starter kit item(s) of character {} did not fit", dropped, character);
                    c.info.starterKitGranted = true;
                    out.kitGranted = true;
                }
                out.ok = true;
                out.items = c.items;
            }
        }
        done(std::move(out));
    };
    if (!enqueue(std::move(task)))
        done({});
}

void MemoryAccountService::applyItemMove(AccountId account, CharacterId character,
                                         std::vector<ItemRelocation> relocations, std::function<void(bool ok)> done)
{
    auto task = [this, account, character, relocations = std::move(relocations), done] {
        bool ok = false;
        {
            const std::scoped_lock lock(m_dataMutex);
            const auto it = m_characters.find(character);
            if (!m_failItemMoves && it != m_characters.end() && it->second.account == account && !it->second.deleted)
            {
                // Work on a copy: all or nothing.
                std::vector<game::StoredItem> items = it->second.items;
                ok = true;
                for (const auto& r : relocations)
                {
                    auto row = std::find_if(items.begin(), items.end(),
                                            [&](const game::StoredItem& s) { return s.id == r.id; });
                    if (row == items.end() || !(row->place == r.from || row->place == r.to) ||
                        !game::isValidPlace(r.to))
                    {
                        ok = false;
                        break;
                    }
                    row->place = r.to;
                }
                // The deferred unique (char_id, container, slot) check at commit.
                for (std::size_t i = 0; ok && i < items.size(); ++i)
                    for (std::size_t j = i + 1; ok && j < items.size(); ++j)
                        ok = !(items[i].place == items[j].place);
                if (ok)
                    it->second.items = std::move(items);
            }
        }
        if (done)
            done(ok);
    };
    if (!enqueue(std::move(task)) && done)
        done(false);
}

void MemoryAccountService::applyItemWrite(AccountId account, CharacterId character, ItemWrite write,
                                          std::function<void(ItemWriteOutcome)> done)
{
    auto task = [this, account, character, write = std::move(write), done] {
        std::chrono::milliseconds delay{0};
        {
            const std::scoped_lock lock(m_dataMutex);
            delay = m_itemWriteDelay;
        }
        if (delay.count() > 0)
            std::this_thread::sleep_for(delay);
        ItemWriteOutcome out;
        {
            const std::scoped_lock lock(m_dataMutex);
            const auto it = m_characters.find(character);
            if (!m_failItemMoves && it != m_characters.end() && it->second.account == account && !it->second.deleted)
            {
                // Work on copies: all or nothing.
                std::vector<game::StoredItem> items = it->second.items;
                const std::int64_t coinsAfter = static_cast<std::int64_t>(it->second.info.coins) + write.coinsAdded -
                                                static_cast<std::int64_t>(write.coinsRemoved);
                bool ok = coinsAfter >= 0 && coinsAfter <= static_cast<std::int64_t>(net::kMaxCoins);
                for (const auto& q : write.quantities)
                {
                    if (!ok)
                        break;
                    auto row = std::find_if(items.begin(), items.end(),
                                            [&](const game::StoredItem& s) { return s.id == q.id; });
                    if (row == items.end() || row->quantity != q.before)
                    {
                        ok = false;
                        break;
                    }
                    if (q.after == 0)
                        items.erase(row);
                    else
                        row->quantity = q.after;
                }
                for (const auto& u : write.upgrades)
                {
                    if (!ok)
                        break;
                    auto row = std::find_if(items.begin(), items.end(),
                                            [&](const game::StoredItem& s) { return s.id == u.id; });
                    if (row == items.end() || row->upgrade != u.before || row->quantity != 1 ||
                        (!u.destroyed && u.after > game::kMaxUpgradeLevel))
                    {
                        ok = false;
                        break;
                    }
                    if (u.destroyed)
                        items.erase(row);
                    else
                        row->upgrade = u.after;
                }
                std::vector<game::ItemInstanceId> ids;
                game::ItemInstanceId next = m_nextItemId;
                for (const auto& n : write.inserts)
                {
                    if (!ok)
                        break;
                    if (!game::isValidPlace(n.place) || n.templateId == 0 || n.quantity == 0)
                    {
                        ok = false;
                        break;
                    }
                    ids.push_back(next);
                    items.push_back({next++, n.place, n.templateId, n.quantity});
                }
                for (std::size_t i = 0; ok && i < items.size(); ++i)
                    for (std::size_t j = i + 1; ok && j < items.size(); ++j)
                        ok = !(items[i].place == items[j].place);
                if (ok)
                {
                    it->second.items = std::move(items);
                    it->second.info.coins = static_cast<std::uint32_t>(coinsAfter);
                    m_nextItemId = next;
                    out.ok = true;
                    out.newIds = std::move(ids);
                    out.coins = it->second.info.coins;
                }
            }
        }
        done(std::move(out));
    };
    if (!enqueue(std::move(task)))
        done({});
}

void MemoryAccountService::allocateStats(AccountId account, CharacterId character, StatAllocation allocation,
                                         std::function<void(StatAllocationOutcome)> done)
{
    auto task = [this, account, character, allocation, done] {
        StatAllocationOutcome out;
        {
            const std::scoped_lock lock(m_dataMutex);
            const auto it = m_characters.find(character);
            if (!m_failStatWrites && it != m_characters.end() && it->second.account == account && !it->second.deleted)
            {
                game::Attributes after = it->second.info.statPoints;
                std::uint32_t sum = 0;
                bool ok = true;
                for (std::size_t i = 0; i < game::kAttributeCount; ++i)
                {
                    const std::uint32_t v = static_cast<std::uint32_t>(after[i]) + allocation.points[i];
                    if (v + allocation.classStart[i] > game::kAttributeCap)
                        ok = false;
                    after[i] = static_cast<std::uint16_t>(std::min<std::uint32_t>(v, game::kAttributeCap));
                    sum += v;
                }
                // The CHECK of 0005_stats.sql: never more than the stored level allows.
                const std::uint32_t storedBudget = game::pointsForLevel(std::max<std::uint16_t>(it->second.info.level, 1));
                if (ok && sum <= allocation.budget && sum <= storedBudget)
                {
                    it->second.info.statPoints = after;
                    out.ok = true;
                    out.allocated = after;
                }
            }
        }
        done(out);
    };
    if (!enqueue(std::move(task)))
        done({});
}

void MemoryAccountService::learnSkill(AccountId account, CharacterId character, SkillLearn learn,
                                      std::function<void(SkillLearnOutcome)> done)
{
    auto task = [this, account, character, learn, done] {
        SkillLearnOutcome out;
        {
            const std::scoped_lock lock(m_dataMutex);
            const auto it = m_characters.find(character);
            if (!m_failSkillWrites && it != m_characters.end() && it->second.account == account && !it->second.deleted)
            {
                auto& info = it->second.info;
                const game::SkillCatalog& catalog = game::skills();
                // The same rule as the Postgres job: the stored ranks, the lower of the live and the
                // stored level (PROTOCOL_SKILLS.md §6).
                const std::uint16_t level = std::min(learn.level, std::max<std::uint16_t>(info.level, 1));
                game::SkillRanks ranks = game::normalizeSkillRanks(catalog, info.charClass, info.skillRanks);
                out.result = info.charClass == learn.charClass
                                 ? game::checkSkillLearn(catalog, info.charClass, level, ranks, learn.skillId)
                                 : net::SkillLearnResult::UnknownSkill;
                if (out.result == net::SkillLearnResult::Ok)
                {
                    ++ranks[learn.skillId];
                    info.skillRanks[learn.skillId] = ranks[learn.skillId];
                }
                out.ranks = ranks;
            }
        }
        done(out);
    };
    if (!enqueue(std::move(task)))
        done({});
}

void MemoryAccountService::setSkillBar(AccountId account, CharacterId character, SkillBarWrite write,
                                       std::function<void(SkillBarOutcome)> done)
{
    auto task = [this, account, character, write, done] {
        SkillBarOutcome out;
        {
            const std::scoped_lock lock(m_dataMutex);
            const auto it = m_characters.find(character);
            if (!m_failSkillWrites && it != m_characters.end() && it->second.account == account && !it->second.deleted)
            {
                auto& info = it->second.info;
                const game::SkillCatalog& catalog = game::skills();
                out.ranks = game::normalizeSkillRanks(catalog, info.charClass, info.skillRanks);
                const game::SkillBar stored = info.skillBar.value_or(game::defaultSkillBar(catalog, info.charClass));
                if (info.charClass == write.charClass && game::validSkillBar(catalog, info.charClass, out.ranks, write.bar))
                {
                    info.skillBar = write.bar;
                    out.result = net::SkillBarResult::Ok;
                    out.bar = write.bar;
                }
                else
                {
                    out.result = net::SkillBarResult::NotLearned;
                    out.bar = game::sanitizeSkillBar(catalog, info.charClass, out.ranks, stored);
                }
            }
        }
        done(out);
    };
    if (!enqueue(std::move(task)))
        done({});
}

bool MemoryAccountService::resetSkills(CharacterId character)
{
    const std::scoped_lock lock(m_dataMutex);
    const auto it = m_characters.find(character);
    if (it == m_characters.end() || it->second.deleted)
        return false;
    it->second.info.skillRanks = {};
    it->second.info.skillBar.reset();
    return true;
}

void MemoryAccountService::setFailSkillWritesForTesting(bool fail)
{
    const std::scoped_lock lock(m_dataMutex);
    m_failSkillWrites = fail;
}

bool MemoryAccountService::resetStats(CharacterId character)
{
    const std::scoped_lock lock(m_dataMutex);
    const auto it = m_characters.find(character);
    if (it == m_characters.end() || it->second.deleted)
        return false;
    it->second.info.statPoints = {};
    return true;
}

void MemoryAccountService::setFailStatWritesForTesting(bool fail)
{
    const std::scoped_lock lock(m_dataMutex);
    m_failStatWrites = fail;
}

bool MemoryAccountService::setCoins(CharacterId character, std::uint32_t coins)
{
    const std::scoped_lock lock(m_dataMutex);
    const auto it = m_characters.find(character);
    if (it == m_characters.end() || it->second.deleted)
        return false;
    it->second.info.coins = std::min(coins, net::kMaxCoins);
    return true;
}

bool MemoryAccountService::setItemUpgrade(CharacterId character, game::ItemInstanceId item, std::uint8_t level)
{
    const std::scoped_lock lock(m_dataMutex);
    const auto it = m_characters.find(character);
    if (it == m_characters.end() || it->second.deleted || level > game::kMaxUpgradeLevel)
        return false;
    for (game::StoredItem& s : it->second.items)
    {
        if (s.id != item)
            continue;
        if (s.quantity != 1)
            return false;
        s.upgrade = level;
        return true;
    }
    return false;
}

void MemoryAccountService::setItemWriteDelayForTesting(std::chrono::milliseconds delay)
{
    const std::scoped_lock lock(m_dataMutex);
    m_itemWriteDelay = delay;
}

std::optional<game::ItemInstanceId> MemoryAccountService::giveItem(CharacterId character,
                                                                  game::ItemTemplateId templateId,
                                                                  std::uint16_t quantity)
{
    const std::scoped_lock lock(m_dataMutex);
    const auto it = m_characters.find(character);
    if (it == m_characters.end() || it->second.deleted || templateId == 0 || quantity == 0)
        return std::nullopt;
    auto& items = it->second.items;
    for (std::uint8_t slot = 0; slot < game::kBagSlots; ++slot)
    {
        const auto place = game::ItemPlace::bag(slot);
        if (std::none_of(items.begin(), items.end(), [&](const game::StoredItem& s) { return s.place == place; }))
        {
            const game::ItemInstanceId id = m_nextItemId++;
            items.push_back({id, place, templateId, quantity});
            return id;
        }
    }
    return std::nullopt;
}

std::vector<game::StoredItem> MemoryAccountService::itemsOf(CharacterId character) const
{
    const std::scoped_lock lock(m_dataMutex);
    const auto it = m_characters.find(character);
    return it == m_characters.end() ? std::vector<game::StoredItem>{} : it->second.items;
}

void MemoryAccountService::setFailItemMovesForTesting(bool fail)
{
    const std::scoped_lock lock(m_dataMutex);
    m_failItemMoves = fail;
}

} // namespace vo::server::auth
