#include "shared/net/Crypto.h"
#include "shared/net/Opcodes.h"

#include <catch2/catch_test_macros.hpp>

#include <sodium.h>

#include <process.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace vn = vo::net;

namespace {

struct TempDir
{
    std::filesystem::path path;
    TempDir()
    {
        static std::atomic<int> counter{0};
        path = std::filesystem::temp_directory_path() /
               ("vo_crypto_tests_" + std::to_string(::_getpid()) + "_" + std::to_string(++counter));
        std::filesystem::create_directories(path);
    }
    ~TempDir()
    {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;
};

struct Handshake
{
    vn::SessionKeys client;
    vn::SessionKeys server;
    vn::KxPublicKey clientPk{};
    vn::KxPublicKey serverPk{};
};

Handshake doHandshake()
{
    Handshake h;
    auto c = vn::generateKxKeyPair();
    auto s = vn::generateKxKeyPair();
    h.clientPk = c.publicKey;
    h.serverPk = s.publicKey;
    REQUIRE(vn::deriveClientSessionKeys(c, h.serverPk, h.client));
    REQUIRE(vn::deriveServerSessionKeys(s, h.clientPk, h.server));
    CHECK(c.secretKey.isZero()); // ephemeral secrets wiped after derivation
    CHECK(s.secretKey.isZero());
    return h;
}

bool equalKeys(const vn::SecretBytes<32>& a, const vn::SecretBytes<32>& b)
{
    return sodium_memcmp(a.data(), b.data(), 32) == 0;
}

std::vector<std::uint8_t> payloadOf(std::size_t n)
{
    std::vector<std::uint8_t> p(n);
    for (std::size_t i = 0; i < n; ++i)
        p[i] = static_cast<std::uint8_t>(i * 7);
    return p;
}

} // namespace

TEST_CASE("Client and server derive matching per-direction session keys", "[net][crypto]")
{
    REQUIRE(vn::initCrypto());
    auto h = doHandshake();
    CHECK(equalKeys(h.client.tx, h.server.rx));
    CHECK(equalKeys(h.client.rx, h.server.tx));
    CHECK_FALSE(equalKeys(h.client.tx, h.client.rx));
    CHECK_FALSE(h.client.tx.isZero());

    // Moving a secret wipes the source.
    vn::SecretBytes<32> moved = std::move(h.client.tx);
    CHECK(h.client.tx.isZero());
    CHECK_FALSE(moved.isZero());
}

TEST_CASE("Server rejects an all-zero (low order) client key", "[net][crypto]")
{
    REQUIRE(vn::initCrypto());
    auto s = vn::generateKxKeyPair();
    vn::SessionKeys keys;
    const vn::KxPublicKey zero{};
    CHECK_FALSE(vn::deriveServerSessionKeys(s, zero, keys));
    CHECK(keys.rx.isZero());
    CHECK(keys.tx.isZero());
}

TEST_CASE("FrameCipher matches the wire format of the spec", "[net][crypto]")
{
    REQUIRE(vn::initCrypto());
    auto h = doHandshake();
    std::array<std::uint8_t, 32> txKey{};
    std::copy(h.client.tx.data(), h.client.tx.data() + 32, txKey.begin());
    vn::FrameCipher sender(std::move(h.client));

    const auto payload = payloadOf(5);
    for (std::uint64_t counter = 0; counter < 3; ++counter)
    {
        std::vector<std::uint8_t> frame;
        REQUIRE(sender.seal(0x0110, payload, frame));
        REQUIRE(frame.size() == 2 + 2 + payload.size() + 16);
        const std::size_t ctLen = frame[0] | (frame[1] << 8);
        CHECK(ctLen == frame.size() - 2);

        // Independent decryption: nonce = 16 zero bytes + u64 LE counter, AD = the 2 length bytes.
        std::uint8_t nonce[24] = {};
        for (int i = 0; i < 8; ++i)
            nonce[16 + i] = static_cast<std::uint8_t>(counter >> (8 * i));
        std::vector<std::uint8_t> plain(ctLen - 16);
        unsigned long long plainLen = 0;
        REQUIRE(crypto_aead_xchacha20poly1305_ietf_decrypt(plain.data(), &plainLen, nullptr, frame.data() + 2, ctLen,
                                                           frame.data(), 2, nonce, txKey.data()) == 0);
        CHECK(plain[0] == 0x10);
        CHECK(plain[1] == 0x01);
        CHECK(std::vector<std::uint8_t>(plain.begin() + 2, plain.end()) == payload);
    }
    CHECK(sender.sendCounter() == 3);
    vn::secureZero(txKey.data(), txKey.size());
}

TEST_CASE("FrameCipher rejects tampered, truncated, replayed and reordered frames", "[net][crypto]")
{
    REQUIRE(vn::initCrypto());
    auto h = doHandshake();
    vn::FrameCipher client(std::move(h.client));
    vn::FrameCipher server(std::move(h.server));

    std::vector<std::uint8_t> f0, f1, f2;
    REQUIRE(client.seal(1, payloadOf(20), f0));
    REQUIRE(client.seal(2, payloadOf(20), f1));
    REQUIRE(client.seal(3, payloadOf(20), f2));
    const auto ct = [](const std::vector<std::uint8_t>& f) { return std::span<const std::uint8_t>(f).subspan(2); };
    std::vector<std::uint8_t> plain;

    SECTION("tampered ciphertext byte")
    {
        auto bad = f0;
        bad[10] ^= 0x01;
        CHECK_FALSE(server.open(ct(bad), plain));
        CHECK(server.receiveCounter() == 0);
        CHECK(server.open(ct(f0), plain)); // the genuine frame still opens
    }
    SECTION("tampered tag")
    {
        auto bad = f0;
        bad.back() ^= 0x80;
        CHECK_FALSE(server.open(ct(bad), plain));
    }
    SECTION("tampered length (frame cut short / AD mismatch)")
    {
        const auto whole = ct(f0);
        CHECK_FALSE(server.open(whole.first(whole.size() - 1), plain));
        // Ciphertext produced with a different AD (length bytes) than its real size.
        std::uint8_t nonce[24] = {};
        std::vector<std::uint8_t> forged(2 + 16 + 2);
        const std::uint8_t wrongAd[2] = {0xFF, 0x00};
        const std::uint8_t msg[2] = {1, 0};
        unsigned long long len = 0;
        std::array<std::uint8_t, 32> key{};
        // Keys are unknown to the attacker; even with the right key the AD binding must hold:
        vn::SessionKeys k2;
        auto c = vn::generateKxKeyPair();
        auto s = vn::generateKxKeyPair();
        const auto cpk = c.publicKey;
        vn::SessionKeys sKeys;
        REQUIRE(vn::deriveClientSessionKeys(c, s.publicKey, k2));
        REQUIRE(vn::deriveServerSessionKeys(s, cpk, sKeys));
        std::copy(k2.tx.data(), k2.tx.data() + 32, key.begin());
        REQUIRE(crypto_aead_xchacha20poly1305_ietf_encrypt(forged.data(), &len, msg, 2, wrongAd, 2, nullptr, nonce,
                                                           key.data()) == 0);
        forged.resize(static_cast<std::size_t>(len));
        vn::FrameCipher receiver(std::move(sKeys));
        CHECK_FALSE(receiver.open(forged, plain));
        vn::secureZero(key.data(), key.size());
    }
    SECTION("replayed frame")
    {
        REQUIRE(server.open(ct(f0), plain));
        CHECK_FALSE(server.open(ct(f0), plain));
        CHECK(server.open(ct(f1), plain));
    }
    SECTION("reordered frames")
    {
        CHECK_FALSE(server.open(ct(f1), plain));
        CHECK(server.open(ct(f0), plain));
        CHECK(server.open(ct(f1), plain));
        CHECK(server.open(ct(f2), plain));
        CHECK(server.receiveCounter() == 3);
    }
    SECTION("wrong direction key")
    {
        vn::FrameCipher& self = client; // client cannot open its own frames (tx != rx)
        CHECK_FALSE(self.open(ct(f0), plain));
    }
    SECTION("length out of range")
    {
        const std::vector<std::uint8_t> tiny(17, 0);
        CHECK_FALSE(server.open(tiny, plain));
    }
}

TEST_CASE("Handshake signature covers the exact transcript", "[net][crypto]")
{
    REQUIRE(vn::initCrypto());
    const auto id = vn::ServerIdentity::generate();
    REQUIRE(id.valid());
    vn::KxPublicKey c{}, s{};
    c.fill(0x01);
    s.fill(0x02);
    const auto sig = id.signHandshake(c, s, vn::kProtocolVersion);

    // Independent check of the signed message layout.
    std::vector<std::uint8_t> msg = {'V', 'O', '-', 'H', 'S', '1'};
    msg.insert(msg.end(), c.begin(), c.end());
    msg.insert(msg.end(), s.begin(), s.end());
    msg.push_back(1);
    msg.push_back(0);
    CHECK(vn::handshakeTranscript(c, s, 1) == msg);
    CHECK(crypto_sign_verify_detached(sig.data(), msg.data(), msg.size(), id.publicKey().data()) == 0);

    CHECK(vn::verifyHandshakeSignature(id.publicKey(), c, s, 1, sig));

    auto badSig = sig;
    badSig[5] ^= 0x10;
    CHECK_FALSE(vn::verifyHandshakeSignature(id.publicKey(), c, s, 1, badSig));
    CHECK_FALSE(vn::verifyHandshakeSignature(id.publicKey(), s, c, 1, sig)); // swapped keys
    CHECK_FALSE(vn::verifyHandshakeSignature(id.publicKey(), c, s, 2, sig)); // other version
    const auto other = vn::ServerIdentity::generate();
    CHECK_FALSE(vn::verifyHandshakeSignature(other.publicKey(), c, s, 1, sig)); // wrong pinned key
}

TEST_CASE("Server identity from seed is deterministic", "[net][crypto]")
{
    REQUIRE(vn::initCrypto());
    std::array<std::uint8_t, 32> seed{};
    seed.fill(7);
    const auto a = vn::ServerIdentity::fromSeed(seed);
    const auto b = vn::ServerIdentity::fromSeed(seed);
    CHECK(a.publicKey() == b.publicKey());
    CHECK(a.publicKeyHex().size() == 64);
}

TEST_CASE("Server key files: save, load, refuse overwrite, detect mismatch", "[net][crypto]")
{
    REQUIRE(vn::initCrypto());
    TempDir dir;
    const auto keyDir = dir.path / "keys";
    std::string err;

    vn::ServerIdentity missing;
    CHECK_FALSE(vn::ServerIdentity::load(keyDir, missing, err));
    CHECK(err.find("server_ed25519.key") != std::string::npos);

    const auto id = vn::ServerIdentity::generate();
    REQUIRE(id.save(keyDir, false, err));
    vn::ServerIdentity loaded;
    REQUIRE(vn::ServerIdentity::load(keyDir, loaded, err));
    CHECK(loaded.publicKey() == id.publicKey());

    std::ifstream pub(keyDir / "server_ed25519.pub");
    std::string pubHex;
    pub >> pubHex;
    CHECK(pubHex == id.publicKeyHex());
    pub.close();

    const auto other = vn::ServerIdentity::generate();
    err.clear();
    CHECK_FALSE(other.save(keyDir, false, err));
    CHECK(err.find("already exist") != std::string::npos);
    REQUIRE(vn::ServerIdentity::load(keyDir, loaded, err));
    CHECK(loaded.publicKey() == id.publicKey());

    REQUIRE(other.save(keyDir, true, err));
    REQUIRE(vn::ServerIdentity::load(keyDir, loaded, err));
    CHECK(loaded.publicKey() == other.publicKey());

    // A .pub that does not belong to the secret key is rejected.
    std::ofstream(keyDir / "server_ed25519.pub", std::ios::trunc) << id.publicKeyHex() << "\n";
    CHECK_FALSE(vn::ServerIdentity::load(keyDir, loaded, err));
    CHECK(err.find("does not match") != std::string::npos);

    // Malformed secret key file.
    std::ofstream(keyDir / "server_ed25519.key", std::ios::trunc) << "xyz\n";
    CHECK_FALSE(vn::ServerIdentity::load(keyDir, loaded, err));
}

TEST_CASE("Hex parsing", "[net][crypto]")
{
    REQUIRE(vn::initCrypto());
    const std::string hex(64, 'a');
    REQUIRE(vn::parsePublicKeyHex(hex).has_value());
    CHECK((*vn::parsePublicKeyHex("  " + hex + "\r\n"))[0] == 0xAA);
    CHECK_FALSE(vn::parsePublicKeyHex(hex.substr(1)).has_value());
    CHECK_FALSE(vn::parsePublicKeyHex(hex + "a").has_value());
    CHECK_FALSE(vn::parsePublicKeyHex(std::string(63, 'a') + "g").has_value());
    const std::vector<std::uint8_t> raw = {0x00, 0x0f, 0xf0, 0xff};
    CHECK(vn::toHex(raw) == "000ff0ff");
}

TEST_CASE("Argon2id password hashing", "[net][crypto][password]")
{
    REQUIRE(vn::initCrypto());
    const auto hash = vn::hashPassword("doğru-parola");
    REQUIRE(hash.has_value());
    CHECK(hash->rfind("$argon2id$", 0) == 0);
    CHECK(vn::verifyPassword(*hash, "doğru-parola"));
    CHECK_FALSE(vn::verifyPassword(*hash, "yanlış-parola"));
    CHECK_FALSE(vn::verifyPassword(*hash, ""));
    CHECK_FALSE(vn::verifyPassword("not a hash", "doğru-parola"));
    CHECK_FALSE(vn::verifyPassword(std::string(200, '$'), "x"));

    const auto again = vn::hashPassword("doğru-parola");
    REQUIRE(again.has_value());
    CHECK(*again != *hash); // random salt
}
