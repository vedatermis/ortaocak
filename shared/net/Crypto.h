// Protocol cryptography (docs/PROTOCOL.md §2.2, §3) on top of libsodium:
//   - ephemeral X25519 key exchange (crypto_kx) and session key derivation for both roles,
//   - Ed25519 signature of the handshake transcript with the server's long-term key,
//   - FrameCipher: XChaCha20-Poly1305 per-direction frame encryption with implicit counters,
//   - server long-term key files (generate / load / save, hex public key),
//   - Argon2id password hashing (crypto_pwhash_str, interactive limits).
//
// Call initCrypto() once before anything else (thread-safe, idempotent).
// sodium.h is deliberately not included here; sizes are checked against it in Crypto.cpp.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace vo::net {

inline constexpr std::size_t kKxPublicKeyBytes = 32;
inline constexpr std::size_t kKxSecretKeyBytes = 32;
inline constexpr std::size_t kSessionKeyBytes = 32;
inline constexpr std::size_t kSignPublicKeyBytes = 32;
inline constexpr std::size_t kSignSecretKeyBytes = 64;
inline constexpr std::size_t kSignSeedBytes = 32;
inline constexpr std::size_t kSignatureBytes = 64;
inline constexpr std::size_t kNonceBytes = 24;

inline constexpr std::string_view kServerSecretKeyFile = "server_ed25519.key"; // hex seed (64 chars)
inline constexpr std::string_view kServerPublicKeyFile = "server_ed25519.pub"; // hex public key

using KxPublicKey = std::array<std::uint8_t, kKxPublicKeyBytes>;
using SignPublicKey = std::array<std::uint8_t, kSignPublicKeyBytes>;
using Signature = std::array<std::uint8_t, kSignatureBytes>;

// Initialises libsodium. Returns false if it cannot be initialised (then nothing here works).
[[nodiscard]] bool initCrypto() noexcept;

// Overwrites memory so the compiler cannot elide it (sodium_memzero).
void secureZero(void* data, std::size_t size) noexcept;

// Fixed-size secret buffer, wiped on destruction and when moved from. Not copyable.
template <std::size_t N>
class SecretBytes
{
public:
    SecretBytes() noexcept = default;
    ~SecretBytes() { wipe(); }
    SecretBytes(const SecretBytes&) = delete;
    SecretBytes& operator=(const SecretBytes&) = delete;
    SecretBytes(SecretBytes&& other) noexcept : m_bytes(other.m_bytes) { other.wipe(); }
    SecretBytes& operator=(SecretBytes&& other) noexcept
    {
        if (this != &other)
        {
            m_bytes = other.m_bytes;
            other.wipe();
        }
        return *this;
    }

    void wipe() noexcept { secureZero(m_bytes.data(), N); }
    [[nodiscard]] std::uint8_t* data() noexcept { return m_bytes.data(); }
    [[nodiscard]] const std::uint8_t* data() const noexcept { return m_bytes.data(); }
    [[nodiscard]] static constexpr std::size_t size() noexcept { return N; }
    [[nodiscard]] std::span<const std::uint8_t, N> view() const noexcept { return std::span<const std::uint8_t, N>(m_bytes); }
    [[nodiscard]] bool isZero() const noexcept
    {
        std::uint8_t acc = 0;
        for (auto b : m_bytes)
            acc |= b;
        return acc == 0;
    }

private:
    std::array<std::uint8_t, N> m_bytes{};
};

// ---- Handshake ----

struct KxKeyPair
{
    KxPublicKey publicKey{};
    SecretBytes<kKxSecretKeyBytes> secretKey;
};

// Ephemeral key pair (crypto_kx_keypair).
[[nodiscard]] KxKeyPair generateKxKeyPair();

struct SessionKeys
{
    SecretBytes<kSessionKeyBytes> rx; // decrypts what we receive
    SecretBytes<kSessionKeyBytes> tx; // encrypts what we send
};

// crypto_kx_server_session_keys(rx, tx, server_pk, server_sk, client_pk). The server's ephemeral
// secret key is wiped afterwards (also on failure). False if the client key is unacceptable.
[[nodiscard]] bool deriveServerSessionKeys(KxKeyPair& server, const KxPublicKey& clientPk, SessionKeys& out);
// crypto_kx_client_session_keys(rx, tx, client_pk, client_sk, server_pk). Wipes the client secret.
[[nodiscard]] bool deriveClientSessionKeys(KxKeyPair& client, const KxPublicKey& serverPk, SessionKeys& out);

// "VO-HS1" || client_kx_pk || server_kx_pk || u16 LE protocol_version
[[nodiscard]] std::vector<std::uint8_t> handshakeTranscript(const KxPublicKey& clientPk, const KxPublicKey& serverPk,
                                                            std::uint16_t protocolVersion);

// Server long-term Ed25519 identity.
class ServerIdentity
{
public:
    ServerIdentity() = default;

    [[nodiscard]] static ServerIdentity generate();
    // Deterministic from a 32-byte seed (crypto_sign_seed_keypair).
    [[nodiscard]] static ServerIdentity fromSeed(std::span<const std::uint8_t, kSignSeedBytes> seed);

    [[nodiscard]] bool valid() const noexcept { return m_valid; }
    [[nodiscard]] const SignPublicKey& publicKey() const noexcept { return m_publicKey; }
    [[nodiscard]] std::string publicKeyHex() const;

    // crypto_sign_detached over handshakeTranscript(...).
    [[nodiscard]] Signature signHandshake(const KxPublicKey& clientPk, const KxPublicKey& serverPk,
                                          std::uint16_t protocolVersion) const;
    [[nodiscard]] Signature sign(std::span<const std::uint8_t> message) const;

    // Key files in `dir`: kServerSecretKeyFile (hex seed) and kServerPublicKeyFile (hex public key).
    // save() refuses to overwrite existing files unless `overwrite` is true.
    bool save(const std::filesystem::path& dir, bool overwrite, std::string& error) const;
    // Fails with a descriptive error if a file is missing or malformed, or if the .pub file does not
    // match the secret key.
    static bool load(const std::filesystem::path& dir, ServerIdentity& out, std::string& error);

private:
    SignPublicKey m_publicKey{};
    SecretBytes<kSignSeedBytes> m_seed;
    SecretBytes<kSignSecretKeyBytes> m_secretKey;
    bool m_valid = false;
};

[[nodiscard]] bool verifySignature(const SignPublicKey& publicKey, std::span<const std::uint8_t> message,
                                   const Signature& signature);
[[nodiscard]] bool verifyHandshakeSignature(const SignPublicKey& serverPublicKey, const KxPublicKey& clientPk,
                                            const KxPublicKey& serverPk, std::uint16_t protocolVersion,
                                            const Signature& signature);

// ---- Hex ----
[[nodiscard]] std::string toHex(std::span<const std::uint8_t> bytes);
// Decodes exactly out.size() bytes of hex (surrounding whitespace ignored). False on any mismatch.
[[nodiscard]] bool fromHex(std::string_view hex, std::span<std::uint8_t> out);
[[nodiscard]] std::optional<SignPublicKey> parsePublicKeyHex(std::string_view hex);

// ---- Frame cipher (§2.2) ----
//
// Nonce = 16 zero bytes || u64 LE counter; each direction counts its own frames from 0. AD = the two
// ct_length bytes as on the wire. The counter is advanced only by successful seal/open, so a replayed,
// reordered or tampered frame fails to open.
class FrameCipher
{
public:
    explicit FrameCipher(SessionKeys keys) noexcept : m_keys(std::move(keys)) {}

    // Encrypts `opcode || payload` and appends a complete wire frame (u16 ct_length || ciphertext) to
    // `outFrame`. False if the plaintext exceeds kMaxPlaintextPerFrame.
    [[nodiscard]] bool seal(std::uint16_t opcode, std::span<const std::uint8_t> payload,
                            std::vector<std::uint8_t>& outFrame);
    // Decrypts the ciphertext of one frame (the bytes after ct_length). On success `plaintext` holds
    // `opcode || payload`. False on a length out of range or authentication failure.
    [[nodiscard]] bool open(std::span<const std::uint8_t> ciphertext, std::vector<std::uint8_t>& plaintext);

    [[nodiscard]] std::uint64_t sendCounter() const noexcept { return m_txCounter; }
    [[nodiscard]] std::uint64_t receiveCounter() const noexcept { return m_rxCounter; }

private:
    SessionKeys m_keys;
    std::uint64_t m_txCounter = 0;
    std::uint64_t m_rxCounter = 0;
};

// ---- Passwords (Argon2id, interactive limits) ----
// Returns the encoded hash string ("$argon2id$...") or nullopt on failure (out of memory).
[[nodiscard]] std::optional<std::string> hashPassword(std::string_view password);
[[nodiscard]] bool verifyPassword(std::string_view encodedHash, std::string_view password);

} // namespace vo::net
