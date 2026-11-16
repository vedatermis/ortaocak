// In-process server harness for the tests: a real Server on an ephemeral 127.0.0.1 port (own
// listener thread, `ioThreads` network threads), a running World on the test terrain and either its
// own MemoryAccountService or an injected IAccountService (PostgreSQL tests). TestClient is a
// ClientSession driven synchronously from the test thread.
#pragma once

#include "server/auth/IAccountService.h"
#include "server/auth/MemoryAccountService.h"
#include "server/net/Server.h"
#include "server/world/World.h"
#include "shared/game/Items.h"
#include "shared/game/Monsters.h"
#include "shared/net/ClientSession.h"
#include "shared/net/Messages.h"
#include "tests/server/world/TestTerrain.h"

#include <catch2/catch_test_macros.hpp>

#include <asio/executor_work_guard.hpp>
#include <asio/io_context.hpp>
#include <asio/post.hpp>

#include <chrono>
#include <deque>
#include <functional>
#include <future>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace vo::tests {

// The race spawns of vo::tests::makeTestTerrain() (what vo_server takes from the zone).
inline server::auth::NewCharacterSpawns testTerrainSpawns()
{
    server::auth::NewCharacterSpawns s;
    s.aydor = {1, 100.0f, 0.0f, 100.0f, 0.0f};
    s.kargan = {1, 400.0f, 0.0f, 400.0f, 1.0f};
    return s;
}

// The repository's gamedata/items.toml, loaded once (REQUIREs that it is valid).
inline std::shared_ptr<const game::ItemCatalog> testItemCatalog()
{
    static const std::shared_ptr<const game::ItemCatalog> catalog = [] {
        auto c = std::make_shared<game::ItemCatalog>();
        const auto path = game::locateItemsFile();
        std::string error;
        if (!path || !game::loadItemCatalog(*path, *c, error))
            c.reset();
        return std::shared_ptr<const game::ItemCatalog>(c);
    }();
    REQUIRE(catalog);
    return catalog;
}

class TestServer
{
public:
    struct Options
    {
        bool autoRegister = true;
        server::auth::IAccountService* accounts = nullptr; // null: own MemoryAccountService
        server::world::WorldSettings world{};
        std::size_t ioThreads = 3; // sessions spread over several network threads (cross-thread kicks)
        std::shared_ptr<const game::MonsterData> monsters; // combat tests: monsters on the test terrain
        std::shared_ptr<const game::LootTables> loot;      // loot tests: what those monsters drop
    };

    explicit TestServer(server::net::ServerSettings settings, bool autoRegister = true)
        : TestServer(std::move(settings), Options{autoRegister})
    {
    }

    TestServer(server::net::ServerSettings settings, Options options)
        : m_memory({options.autoRegister, testTerrainSpawns()}),
          m_accounts(options.accounts != nullptr ? *options.accounts : m_memory),
          m_work(asio::make_work_guard(m_io)),
          m_world(options.world, [this](server::auth::AccountId a, server::auth::CharacterId c,
                                        server::auth::CharacterPosition p, server::auth::CharacterProgress g,
                                        std::function<void(bool)> done) {
              m_accounts.saveCharacterState(a, c, p, g, std::move(done));
          })
    {
        REQUIRE(net::initCrypto());
        std::string error;
        REQUIRE(m_world.createZone(1, makeTestTerrain(), error));
        if (options.monsters)
            REQUIRE(m_world.installMonsters(options.monsters, error));
        if (options.loot)
            m_world.installLoot(options.loot);
        settings.listenAddress = "127.0.0.1";
        settings.listenPort = 0;
        settings.ioThreads = options.ioThreads;
        if (!settings.items)
            settings.items = testItemCatalog();
        auto identity = net::ServerIdentity::generate();
        m_publicKey = identity.publicKey();
        m_server = std::make_unique<server::net::Server>(m_io, settings, std::move(identity), m_accounts, m_world);
        REQUIRE(m_server->start(error));
        m_world.start();
        m_thread = std::thread([this] { m_io.run(); });
    }

    ~TestServer() { stop(std::chrono::seconds(5)); }

    TestServer(const TestServer&) = delete;
    TestServer& operator=(const TestServer&) = delete;

    // The production shutdown order (server/main.cpp): kick everyone, wait at most `sessionWindow`
    // for the sessions, stop the world, the account service, then the network threads (handlers
    // still queued are dropped). Idempotent.
    void stop(std::chrono::milliseconds sessionWindow)
    {
        if (!m_server)
            return;
        std::promise<void> done;
        asio::post(m_io, [&] {
            m_server->shutdown();
            done.set_value();
        });
        done.get_future().wait();
        m_work.reset();
        m_thread.join();
        (void)m_server->waitForSessions(sessionWindow);
        m_world.stop();
        m_accounts.stop();
        m_server->stopNetwork(std::chrono::milliseconds(0));
        m_server.reset();
    }

    void shutdown()
    {
        asio::post(m_io, [this] { m_server->shutdown(); });
    }

    [[nodiscard]] std::size_t sessionCount()
    {
        std::promise<std::size_t> p;
        asio::post(m_io, [&] { p.set_value(m_server->sessionCount()); });
        return p.get_future().get();
    }

    // Waits until no session is left (true) or `timeout` passed.
    bool waitNoSessions(std::chrono::milliseconds timeout)
    {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (sessionCount() > 0)
        {
            if (std::chrono::steady_clock::now() >= deadline)
                return false;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        return true;
    }

    [[nodiscard]] net::ClientSettings clientSettings() const
    {
        net::ClientSettings s;
        s.host = "127.0.0.1";
        s.port = m_server->port();
        s.serverPublicKey = m_publicKey;
        s.handshakeTimeout = std::chrono::seconds(5);
        return s;
    }

    [[nodiscard]] std::uint16_t port() const { return m_server->port(); }
    server::auth::MemoryAccountService& accounts() { return m_memory; }
    server::world::World& world() { return m_world; }
    server::net::Server& server() { return *m_server; }

private:
    asio::io_context m_io;
    server::auth::MemoryAccountService m_memory;
    server::auth::IAccountService& m_accounts;
    asio::executor_work_guard<asio::io_context::executor_type> m_work;
    server::world::World m_world;
    std::unique_ptr<server::net::Server> m_server;
    net::SignPublicKey m_publicKey{};
    std::thread m_thread;
};

struct Msg
{
    std::uint16_t opcode = 0;
    std::vector<std::uint8_t> payload;
};

class TestClient
{
public:
    TestClient() : m_session(m_io)
    {
        m_session.setMessageHandler([this](std::uint16_t op, std::span<const std::uint8_t> p) {
            m_inbox.push_back({op, std::vector<std::uint8_t>(p.begin(), p.end())});
        });
        m_session.setClosedHandler([this](net::CloseReason) { m_closed = true; });
    }

    net::ConnectResult connect(const net::ClientSettings& s)
    {
        std::optional<net::ConnectResult> result;
        REQUIRE(m_session.connect(s, [&](net::ConnectResult r, const std::string&) { result = r; }));
        REQUIRE(pump([&] { return result.has_value(); }, std::chrono::seconds(10)));
        return *result;
    }

    template <class T>
    void send(const T& msg)
    {
        REQUIRE(m_session.sendMessage(msg));
    }

    void sendRawPayload(net::Opcode op, std::vector<std::uint8_t> payload)
    {
        REQUIRE(m_session.send(static_cast<std::uint16_t>(op), payload));
    }

    bool sendRawBytes(std::vector<std::uint8_t> bytes) { return m_session.sendRawForTesting(std::move(bytes)); }
    bool trySend(std::uint16_t opcode, std::span<const std::uint8_t> payload) { return m_session.send(opcode, payload); }

    std::optional<Msg> next(std::chrono::milliseconds timeout = std::chrono::seconds(10))
    {
        if (!pump([&] { return !m_inbox.empty() || m_closed; }, timeout) || m_inbox.empty())
            return std::nullopt;
        Msg m = std::move(m_inbox.front());
        m_inbox.pop_front();
        return m;
    }

    template <class T>
    T expect()
    {
        auto m = next();
        REQUIRE(m.has_value());
        INFO("got opcode " << net::opcodeName(static_cast<net::Opcode>(m->opcode)));
        REQUIRE(m->opcode == static_cast<std::uint16_t>(T::kOpcode));
        T out;
        REQUIRE(net::decodeMessage(m->payload, out));
        return out;
    }

    net::ErrorCode expectError() { return expect<net::Error>().errorCode; }

    static bool isWorldOpcode(std::uint16_t op)
    {
        return op == static_cast<std::uint16_t>(net::Opcode::EntitySpawn) ||
               op == static_cast<std::uint16_t>(net::Opcode::EntityDespawn) ||
               op == static_cast<std::uint16_t>(net::Opcode::EntityMove) ||
               op == static_cast<std::uint16_t>(net::Opcode::PositionCorrect) ||
               op == static_cast<std::uint16_t>(net::Opcode::EntityEquip);
    }

    // Like expect(), but skips world updates (ENTITY_*, POSITION_CORRECT) streamed meanwhile, and
    // the INVENTORY and COINS that follow an ENTER_WORLD_RES (unless T is one of them).
    template <class T>
    T expectSkippingWorld()
    {
        for (;;)
        {
            auto m = next();
            REQUIRE(m.has_value());
            if (isWorldOpcode(m->opcode) && !isWorldOpcode(static_cast<std::uint16_t>(T::kOpcode)))
                continue;
            if (m->opcode == static_cast<std::uint16_t>(net::Opcode::Coins) && T::kOpcode != net::Opcode::Coins)
                continue;
            if (m->opcode == static_cast<std::uint16_t>(net::Opcode::Inventory) && T::kOpcode != net::Opcode::Inventory)
                continue;
            INFO("got opcode " << net::opcodeName(static_cast<net::Opcode>(m->opcode)));
            REQUIRE(m->opcode == static_cast<std::uint16_t>(T::kOpcode));
            T out;
            REQUIRE(net::decodeMessage(m->payload, out));
            return out;
        }
    }

    // Waits for a message of type T satisfying `pred`, dropping everything else.
    template <class T, class P>
    std::optional<T> waitFor(P&& pred, std::chrono::milliseconds timeout = std::chrono::seconds(5))
    {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (std::chrono::steady_clock::now() < deadline)
        {
            auto m = next(std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()));
            if (!m)
                return std::nullopt;
            T out;
            if (m->opcode == static_cast<std::uint16_t>(T::kOpcode) && net::decodeMessage(m->payload, out) && pred(out))
                return out;
        }
        return std::nullopt;
    }

    // Login, create one character, enter the world.
    net::EnterWorldRes enterNew(const std::string& user, const std::string& name, std::uint8_t race = 1)
    {
        REQUIRE(login(user) == net::LoginResult::Ok);
        send(net::CharCreateReq{name, race, 1, 0, 0});
        const auto created = expect<net::CharCreateRes>();
        REQUIRE(created.result == net::CharCreateResult::Ok);
        m_lastCharId = created.character.charId;
        send(net::EnterWorldReq{created.character.charId});
        const auto enter = expect<net::EnterWorldRes>();
        REQUIRE(enter.result == net::EnterWorldResult::Ok);
        m_lastInventory = expect<net::Inventory>(); // PROTOCOL_ITEMS.md §2: right after ENTER_WORLD_RES
        m_lastCoins = expect<net::Coins>();         // PROTOCOL_LOOT.md §4.3: then the balance
        return enter;
    }

    // ENTER_WORLD_REQ for an existing character: ENTER_WORLD_RES (Ok) and the INVENTORY after it.
    net::EnterWorldRes enter(std::uint32_t charId)
    {
        send(net::EnterWorldReq{charId});
        const auto res = expect<net::EnterWorldRes>();
        REQUIRE(res.result == net::EnterWorldResult::Ok);
        m_lastInventory = expect<net::Inventory>();
        m_lastCoins = expect<net::Coins>();
        return res;
    }

    // Sends ITEM_MOVE_REQ and waits for its ITEM_MOVE_RES (world packets and ENTITY_EQUIP in between
    // are dropped; the ITEM_UPDATE before an Ok result is returned in `update`).
    net::ItemMoveResult moveItem(std::uint32_t seq, std::uint8_t fc, std::uint8_t fs, std::uint8_t tc, std::uint8_t ts,
                                 net::ItemUpdate* update = nullptr)
    {
        send(net::ItemMoveReq{seq, fc, fs, tc, ts});
        std::optional<net::ItemUpdate> lastUpdate;
        for (;;)
        {
            auto m = next();
            REQUIRE(m.has_value());
            if (m->opcode == static_cast<std::uint16_t>(net::Opcode::ItemUpdate))
            {
                net::ItemUpdate u;
                REQUIRE(net::decodeMessage(m->payload, u));
                lastUpdate = u;
                continue;
            }
            if (m->opcode != static_cast<std::uint16_t>(net::Opcode::ItemMoveRes))
                continue;
            net::ItemMoveRes res;
            REQUIRE(net::decodeMessage(m->payload, res));
            REQUIRE(res.seq == seq);
            // On success exactly one ITEM_UPDATE came first; on failure none.
            CHECK(lastUpdate.has_value() == (res.result == net::ItemMoveResult::Ok));
            if (update && lastUpdate)
                *update = *lastUpdate;
            return res.result;
        }
    }

    [[nodiscard]] const net::Inventory& lastInventory() const { return m_lastInventory; }
    [[nodiscard]] const net::Coins& lastCoins() const { return m_lastCoins; }

    bool waitClosed(std::chrono::milliseconds timeout = std::chrono::seconds(10))
    {
        return pump([&] { return m_closed; }, timeout);
    }

    net::LoginResult login(const std::string& user, const std::string& pw = "password1")
    {
        send(net::LoginReq{user, pw});
        return expect<net::LoginRes>().result;
    }

    // Runs the client's io_context for `d` (messages are collected meanwhile).
    void pumpFor(std::chrono::milliseconds d)
    {
        (void)pump([] { return false; }, d);
    }

    [[nodiscard]] bool closed() const { return m_closed; }
    [[nodiscard]] std::uint32_t lastCharId() const { return m_lastCharId; }
    void close() { m_session.close(); }

private:
    bool pump(const std::function<bool()>& done, std::chrono::milliseconds timeout)
    {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (!done())
        {
            const auto now = std::chrono::steady_clock::now();
            if (now >= deadline)
                return false;
            if (m_io.stopped())
                m_io.restart();
            if (m_io.run_one_for(std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now)) == 0)
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        return true;
    }

    asio::io_context m_io;
    net::ClientSession m_session;
    std::deque<Msg> m_inbox;
    bool m_closed = false;
    std::uint32_t m_lastCharId = 0;
    net::Inventory m_lastInventory;
    net::Coins m_lastCoins;
};

} // namespace vo::tests
