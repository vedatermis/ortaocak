#include "shared/net/FrameCodec.h"

#include "shared/net/Crypto.h"
#include "shared/net/Opcodes.h"

namespace vo::net {

void FrameDecoder::feed(std::span<const std::uint8_t> bytes)
{
    if (m_failed)
        return;
    // Compact before growing so the buffer stays bounded by about one maximum frame plus one chunk.
    if (m_readPos > 0 && (m_readPos == m_buffer.size() || m_readPos >= 4096))
    {
        m_buffer.erase(m_buffer.begin(), m_buffer.begin() + static_cast<std::ptrdiff_t>(m_readPos));
        m_readPos = 0;
    }
    m_buffer.insert(m_buffer.end(), bytes.begin(), bytes.end());
}

FrameDecoder::Status FrameDecoder::next(Frame& out)
{
    if (m_failed)
        return Status::Error;
    const std::size_t avail = m_buffer.size() - m_readPos;
    if (avail < kFrameHeaderBytes)
        return Status::NeedMore;

    const std::size_t length =
        static_cast<std::size_t>(m_buffer[m_readPos]) | (static_cast<std::size_t>(m_buffer[m_readPos + 1]) << 8);
    const std::size_t minLen = m_mode == FrameMode::Plain ? kMinPlainFrameLength : kMinCipherFrameLength;
    const std::size_t maxLen = m_mode == FrameMode::Plain ? kMaxPlainFrameLength : kMaxCipherFrameLength;
    if (length < minLen || length > maxLen)
    {
        m_failed = true;
        m_buffer.clear();
        m_readPos = 0;
        return Status::Error;
    }
    if (avail < kFrameHeaderBytes + length)
        return Status::NeedMore;

    const auto begin = m_buffer.begin() + static_cast<std::ptrdiff_t>(m_readPos + kFrameHeaderBytes);
    out.body.assign(begin, begin + static_cast<std::ptrdiff_t>(length));
    m_readPos += kFrameHeaderBytes + length;
    if (m_readPos == m_buffer.size())
    {
        m_buffer.clear();
        m_readPos = 0;
    }
    return Status::Frame;
}

bool encodePlainFrame(std::uint16_t opcode, std::span<const std::uint8_t> payload, std::vector<std::uint8_t>& out)
{
    const std::size_t length = kOpcodeBytes + payload.size();
    if (length > kMaxPlainFrameLength)
        return false;
    out.push_back(static_cast<std::uint8_t>(length & 0xFF));
    out.push_back(static_cast<std::uint8_t>(length >> 8));
    out.push_back(static_cast<std::uint8_t>(opcode & 0xFF));
    out.push_back(static_cast<std::uint8_t>(opcode >> 8));
    out.insert(out.end(), payload.begin(), payload.end());
    return true;
}

bool encodeEncryptedFrame(FrameCipher& cipher, std::uint16_t opcode, std::span<const std::uint8_t> payload,
                          std::vector<std::uint8_t>& out)
{
    return cipher.seal(opcode, payload, out);
}

bool splitFrameBody(std::span<const std::uint8_t> body, std::uint16_t& opcode,
                    std::span<const std::uint8_t>& payload) noexcept
{
    if (body.size() < kOpcodeBytes)
        return false;
    opcode = static_cast<std::uint16_t>(body[0] | (body[1] << 8));
    payload = body.subspan(kOpcodeBytes);
    return true;
}

} // namespace vo::net
