#include "shared/net/Crypto.h"

#include "shared/core/Paths.h"
#include "shared/net/Opcodes.h"

#include <sodium.h>

#include <algorithm>
#include <cstring>
#include <fstream>
#include <iterator>
#include <sstream>
#include <system_error>

namespace vo::net {

static_assert(kKxPublicKeyBytes == crypto_kx_PUBLICKEYBYTES);
static_assert(kKxSecretKeyBytes == crypto_kx_SECRETKEYBYTES);
static_assert(kSessionKeyBytes == crypto_kx_SESSIONKEYBYTES);
static_assert(kSessionKeyBytes == crypto_aead_xchacha20poly1305_ietf_KEYBYTES);
static_assert(kSignPublicKeyBytes == crypto_sign_PUBLICKEYBYTES);
static_assert(kSignSecretKeyBytes == crypto_sign_SECRETKEYBYTES);
static_assert(kSignSeedBytes == crypto_sign_SEEDBYTES);
static_assert(kSignatureBytes == crypto_sign_BYTES);
static_assert(kNonceBytes == crypto_aead_xchacha20poly1305_ietf_NPUBBYTES);
static_assert(kAeadTagBytes == crypto_aead_xchacha20poly1305_ietf_ABYTES);

bool initCrypto() noexcept
{
    return sodium_init() >= 0; // 0 = initialised now, 1 = already initialised
}

void secureZero(void* data, std::size_t size) noexcept
{
    sodium_memzero(data, size);
}

// ---- Handshake ----

KxKeyPair generateKxKeyPair()
{
    KxKeyPair kp;
    crypto_kx_keypair(kp.publicKey.data(), kp.secretKey.data());
    return kp;
}

bool deriveServerSessionKeys(KxKeyPair& server, const KxPublicKey& clientPk, SessionKeys& out)
{
    const int rc = crypto_kx_server_session_keys(out.rx.data(), out.tx.data(), server.publicKey.data(),
                                                 server.secretKey.data(), clientPk.data());
    server.secretKey.wipe();
    if (rc != 0)
    {
        out.rx.wipe();
        out.tx.wipe();
        return false;
    }
    return true;
}

bool deriveClientSessionKeys(KxKeyPair& client, const KxPublicKey& serverPk, SessionKeys& out)
{
    const int rc = crypto_kx_client_session_keys(out.rx.data(), out.tx.data(), client.publicKey.data(),
                                                 client.secretKey.data(), serverPk.data());
    client.secretKey.wipe();
    if (rc != 0)
    {
        out.rx.wipe();
        out.tx.wipe();
        return false;
    }
    return true;
}

std::vector<std::uint8_t> handshakeTranscript(const KxPublicKey& clientPk, const KxPublicKey& serverPk,
                                              std::uint16_t protocolVersion)
{
    std::vector<std::uint8_t> msg;
    msg.reserve(kHandshakeSignatureLabel.size() + 2 * kKxPublicKeyBytes + 2);
    msg.insert(msg.end(), kHandshakeSignatureLabel.begin(), kHandshakeSignatureLabel.end());
    msg.insert(msg.end(), clientPk.begin(), clientPk.end());
    msg.insert(msg.end(), serverPk.begin(), serverPk.end());
    msg.push_back(static_cast<std::uint8_t>(protocolVersion & 0xFF));
    msg.push_back(static_cast<std::uint8_t>(protocolVersion >> 8));
    return msg;
}

// ---- ServerIdentity ----

ServerIdentity ServerIdentity::generate()
{
    SecretBytes<kSignSeedBytes> seed;
    randombytes_buf(seed.data(), seed.size());
    return fromSeed(seed.view());
}

ServerIdentity ServerIdentity::fromSeed(std::span<const std::uint8_t, kSignSeedBytes> seed)
{
    ServerIdentity id;
    std::copy(seed.begin(), seed.end(), id.m_seed.data());
    crypto_sign_seed_keypair(id.m_publicKey.data(), id.m_secretKey.data(), id.m_seed.data());
    id.m_valid = true;
    return id;
}

std::string ServerIdentity::publicKeyHex() const
{
    return toHex(m_publicKey);
}

Signature ServerIdentity::sign(std::span<const std::uint8_t> message) const
{
    Signature sig{};
    crypto_sign_detached(sig.data(), nullptr, message.data(), message.size(), m_secretKey.data());
    return sig;
}

Signature ServerIdentity::signHandshake(const KxPublicKey& clientPk, const KxPublicKey& serverPk,
                                        std::uint16_t protocolVersion) const
{
    return sign(handshakeTranscript(clientPk, serverPk, protocolVersion));
}

namespace {

bool readTextFile(const std::filesystem::path& file, std::string& out, std::string& error)
{
    std::ifstream in(file, std::ios::binary);
    if (!in)
    {
        error = "cannot open '" + core::pathToUtf8(file) + "'";
        return false;
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    out = ss.str();
    return true;
}

bool writeTextFile(const std::filesystem::path& file, const std::string& text, std::string& error)
{
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    if (!out)
    {
        error = "cannot write '" + core::pathToUtf8(file) + "'";
        return false;
    }
    out << text;
    out.flush();
    if (!out)
    {
        error = "write failed for '" + core::pathToUtf8(file) + "'";
        return false;
    }
    return true;
}

} // namespace

bool ServerIdentity::save(const std::filesystem::path& dir, bool overwrite, std::string& error) const
{
    if (!m_valid)
    {
        error = "no key to save";
        return false;
    }
    const auto secretPath = dir / kServerSecretKeyFile;
    const auto publicPath = dir / kServerPublicKeyFile;
    std::error_code ec;
    if (!overwrite && (std::filesystem::exists(secretPath, ec) || std::filesystem::exists(publicPath, ec)))
    {
        error = "key files already exist in '" + core::pathToUtf8(dir) + "' (use --force to overwrite)";
        return false;
    }
    std::filesystem::create_directories(dir, ec);
    if (ec)
    {
        error = "cannot create '" + core::pathToUtf8(dir) + "': " + ec.message();
        return false;
    }
    std::string seedHex = toHex(m_seed.view());
    seedHex.push_back('\n');
    const bool ok = writeTextFile(secretPath, seedHex, error);
    secureZero(seedHex.data(), seedHex.size());
    if (!ok)
        return false;
    return writeTextFile(publicPath, publicKeyHex() + "\n", error);
}

bool ServerIdentity::load(const std::filesystem::path& dir, ServerIdentity& out, std::string& error)
{
    const auto secretPath = dir / kServerSecretKeyFile;
    const auto publicPath = dir / kServerPublicKeyFile;
    std::string seedHex;
    if (!readTextFile(secretPath, seedHex, error))
        return false;
    SecretBytes<kSignSeedBytes> seed;
    const bool parsed = fromHex(seedHex, std::span<std::uint8_t>(seed.data(), seed.size()));
    secureZero(seedHex.data(), seedHex.size());
    if (!parsed)
    {
        error = "'" + core::pathToUtf8(secretPath) + "' is not a 64-character hex Ed25519 seed";
        return false;
    }
    ServerIdentity id = fromSeed(seed.view());

    std::string pubHex;
    if (!readTextFile(publicPath, pubHex, error))
        return false;
    const auto pub = parsePublicKeyHex(pubHex);
    if (!pub)
    {
        error = "'" + core::pathToUtf8(publicPath) + "' is not a 64-character hex public key";
        return false;
    }
    if (*pub != id.publicKey())
    {
        error = "'" + core::pathToUtf8(publicPath) + "' does not match the secret key";
        return false;
    }
    out = std::move(id);
    return true;
}

bool verifySignature(const SignPublicKey& publicKey, std::span<const std::uint8_t> message, const Signature& signature)
{
    return crypto_sign_verify_detached(signature.data(), message.data(), message.size(), publicKey.data()) == 0;
}

bool verifyHandshakeSignature(const SignPublicKey& serverPublicKey, const KxPublicKey& clientPk,
                              const KxPublicKey& serverPk, std::uint16_t protocolVersion, const Signature& signature)
{
    return verifySignature(serverPublicKey, handshakeTranscript(clientPk, serverPk, protocolVersion), signature);
}

// ---- Hex ----

std::string toHex(std::span<const std::uint8_t> bytes)
{
    std::string out(bytes.size() * 2 + 1, '\0');
    sodium_bin2hex(out.data(), out.size(), bytes.data(), bytes.size());
    out.pop_back();
    return out;
}

bool fromHex(std::string_view hex, std::span<std::uint8_t> out)
{
    const auto isSpace = [](char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };
    while (!hex.empty() && isSpace(hex.front()))
        hex.remove_prefix(1);
    while (!hex.empty() && isSpace(hex.back()))
        hex.remove_suffix(1);
    if (hex.size() != out.size() * 2)
        return false;
    std::size_t binLen = 0;
    const char* end = nullptr;
    if (sodium_hex2bin(out.data(), out.size(), hex.data(), hex.size(), nullptr, &binLen, &end) != 0)
        return false;
    return binLen == out.size() && end == hex.data() + hex.size();
}

std::optional<SignPublicKey> parsePublicKeyHex(std::string_view hex)
{
    SignPublicKey key{};
    if (!fromHex(hex, key))
        return std::nullopt;
    return key;
}

// ---- FrameCipher ----

namespace {

void makeNonce(std::uint64_t counter, std::uint8_t (&nonce)[kNonceBytes]) noexcept
{
    std::fill(std::begin(nonce), std::end(nonce), std::uint8_t{0});
    for (int i = 0; i < 8; ++i)
        nonce[16 + i] = static_cast<std::uint8_t>((counter >> (8 * i)) & 0xFF);
}

} // namespace

bool FrameCipher::seal(std::uint16_t opcode, std::span<const std::uint8_t> payload, std::vector<std::uint8_t>& outFrame)
{
    const std::size_t plainLen = kOpcodeBytes + payload.size();
    if (plainLen > kMaxPlaintextPerFrame || m_txCounter == UINT64_MAX)
        return false;
    const std::size_t ctLen = plainLen + kAeadTagBytes;

    const std::uint8_t header[2] = {static_cast<std::uint8_t>(ctLen & 0xFF), static_cast<std::uint8_t>(ctLen >> 8)};
    std::uint8_t nonce[kNonceBytes];
    makeNonce(m_txCounter, nonce);

    // Encrypted in place (libsodium allows c == m): the plaintext is written where the ciphertext
    // goes, so sealing a frame needs no temporary buffer (no allocation per frame once outFrame
    // has capacity) and leaves no plaintext copy behind.
    const std::size_t base = outFrame.size();
    outFrame.resize(base + kFrameHeaderBytes + ctLen);
    std::uint8_t* frame = outFrame.data() + base;
    frame[0] = header[0];
    frame[1] = header[1];
    std::uint8_t* body = frame + kFrameHeaderBytes;
    body[0] = static_cast<std::uint8_t>(opcode & 0xFF);
    body[1] = static_cast<std::uint8_t>(opcode >> 8);
    if (!payload.empty())
        std::memcpy(body + kOpcodeBytes, payload.data(), payload.size());
    unsigned long long written = 0;
    const int rc = crypto_aead_xchacha20poly1305_ietf_encrypt(body, &written, body, plainLen, header, sizeof(header),
                                                              nullptr, nonce, m_keys.tx.data());
    if (rc != 0 || written != ctLen)
    {
        secureZero(frame, kFrameHeaderBytes + ctLen);
        outFrame.resize(base);
        return false;
    }
    ++m_txCounter;
    return true;
}

bool FrameCipher::open(std::span<const std::uint8_t> ciphertext, std::vector<std::uint8_t>& plaintext)
{
    if (ciphertext.size() < kMinCipherFrameLength || ciphertext.size() > kMaxCipherFrameLength ||
        m_rxCounter == UINT64_MAX)
        return false;
    const std::uint8_t header[2] = {static_cast<std::uint8_t>(ciphertext.size() & 0xFF),
                                    static_cast<std::uint8_t>(ciphertext.size() >> 8)};
    std::uint8_t nonce[kNonceBytes];
    makeNonce(m_rxCounter, nonce);

    plaintext.resize(ciphertext.size() - kAeadTagBytes);
    unsigned long long written = 0;
    const int rc = crypto_aead_xchacha20poly1305_ietf_decrypt(plaintext.data(), &written, nullptr, ciphertext.data(),
                                                              ciphertext.size(), header, sizeof(header), nonce,
                                                              m_keys.rx.data());
    if (rc != 0 || written != plaintext.size())
    {
        plaintext.clear();
        return false;
    }
    ++m_rxCounter;
    return true;
}

// ---- Passwords ----

std::optional<std::string> hashPassword(std::string_view password)
{
    char out[crypto_pwhash_STRBYTES];
    if (crypto_pwhash_str_alg(out, password.data(), password.size(), crypto_pwhash_OPSLIMIT_INTERACTIVE,
                              crypto_pwhash_MEMLIMIT_INTERACTIVE, crypto_pwhash_ALG_ARGON2ID13) != 0)
        return std::nullopt;
    return std::string(out);
}

bool verifyPassword(std::string_view encodedHash, std::string_view password)
{
    // crypto_pwhash_str_verify needs a NUL-terminated string of at most STRBYTES-1 characters.
    if (encodedHash.size() >= crypto_pwhash_STRBYTES)
        return false;
    char buf[crypto_pwhash_STRBYTES] = {};
    std::copy(encodedHash.begin(), encodedHash.end(), buf);
    return crypto_pwhash_str_verify(buf, password.data(), password.size()) == 0;
}

} // namespace vo::net
