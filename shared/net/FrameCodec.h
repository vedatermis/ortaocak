// Stream framing (docs/PROTOCOL.md §2): incremental decoder for plaintext and encrypted frames and
// encoders for both kinds.
//
//   FrameDecoder dec;                         // starts in Plain mode (handshake)
//   dec.feed(bytes);                          // any chunk size, even 1 byte
//   FrameDecoder::Frame f;
//   while (dec.next(f) == FrameDecoder::Status::Frame) { ... }
//   // Status::Error -> length out of range: close the connection
//
// Plain frame body   = u16 opcode + payload        (length 2..512)
// Encrypted body     = ciphertext                   (length 18..8208), decrypt with FrameCipher::open
// The mode can change between two next() calls (after HELLO / HELLO_ACK); bytes already buffered
// are interpreted in the new mode.
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace vo::net {

class FrameCipher;

enum class FrameMode : std::uint8_t
{
    Plain,
    Encrypted,
};

class FrameDecoder
{
public:
    enum class Status : std::uint8_t
    {
        NeedMore, // no complete frame buffered yet
        Frame,    // `out` holds one frame body
        Error,    // length field out of range; the decoder stays in this state
    };

    struct Frame
    {
        std::vector<std::uint8_t> body; // bytes after the u16 length field
    };

    explicit FrameDecoder(FrameMode mode = FrameMode::Plain) noexcept : m_mode(mode) {}

    void setMode(FrameMode mode) noexcept { m_mode = mode; }
    [[nodiscard]] FrameMode mode() const noexcept { return m_mode; }

    void feed(std::span<const std::uint8_t> bytes);
    [[nodiscard]] Status next(Frame& out);

    [[nodiscard]] bool failed() const noexcept { return m_failed; }
    [[nodiscard]] std::size_t buffered() const noexcept { return m_buffer.size() - m_readPos; }

private:
    std::vector<std::uint8_t> m_buffer;
    std::size_t m_readPos = 0;
    FrameMode m_mode;
    bool m_failed = false;
};

// Appends a plaintext frame (u16 length || u16 opcode || payload). False if the frame would exceed
// kMaxPlainFrameLength.
[[nodiscard]] bool encodePlainFrame(std::uint16_t opcode, std::span<const std::uint8_t> payload,
                                    std::vector<std::uint8_t>& out);

// Appends an encrypted frame (u16 ct_length || ciphertext), advancing the cipher's send counter.
// False if the plaintext would exceed kMaxPlaintextPerFrame.
[[nodiscard]] bool encodeEncryptedFrame(FrameCipher& cipher, std::uint16_t opcode,
                                        std::span<const std::uint8_t> payload, std::vector<std::uint8_t>& out);

// Splits a decrypted or plaintext frame body into opcode and payload. False if shorter than 2 bytes.
[[nodiscard]] bool splitFrameBody(std::span<const std::uint8_t> body, std::uint16_t& opcode,
                                  std::span<const std::uint8_t>& payload) noexcept;

} // namespace vo::net
