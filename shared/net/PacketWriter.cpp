#include "shared/net/PacketWriter.h"

#include "shared/net/Utf8.h"

#include <bit>
#include <limits>

namespace vo::net {

void PacketWriter::writeU8(std::uint8_t v)
{
    if (m_ok)
        m_buffer.push_back(v);
}

void PacketWriter::writeU16(std::uint16_t v)
{
    if (!m_ok)
        return;
    m_buffer.push_back(static_cast<std::uint8_t>(v & 0xFF));
    m_buffer.push_back(static_cast<std::uint8_t>(v >> 8));
}

void PacketWriter::writeU32(std::uint32_t v)
{
    if (!m_ok)
        return;
    for (int i = 0; i < 4; ++i)
        m_buffer.push_back(static_cast<std::uint8_t>((v >> (8 * i)) & 0xFF));
}

void PacketWriter::writeU64(std::uint64_t v)
{
    if (!m_ok)
        return;
    for (int i = 0; i < 8; ++i)
        m_buffer.push_back(static_cast<std::uint8_t>((v >> (8 * i)) & 0xFF));
}

void PacketWriter::writeF32(float v)
{
    writeU32(std::bit_cast<std::uint32_t>(v));
}

void PacketWriter::writeBool(bool v)
{
    writeU8(v ? 1 : 0);
}

void PacketWriter::writeStr8(std::string_view text)
{
    if (!m_ok)
        return;
    if (text.size() > std::numeric_limits<std::uint8_t>::max() || !isValidUtf8(text))
    {
        m_ok = false;
        return;
    }
    writeU8(static_cast<std::uint8_t>(text.size()));
    m_buffer.insert(m_buffer.end(), text.begin(), text.end());
}

void PacketWriter::writeStr16(std::string_view text)
{
    if (!m_ok)
        return;
    if (text.size() > std::numeric_limits<std::uint16_t>::max() || !isValidUtf8(text))
    {
        m_ok = false;
        return;
    }
    writeU16(static_cast<std::uint16_t>(text.size()));
    m_buffer.insert(m_buffer.end(), text.begin(), text.end());
}

void PacketWriter::writeBytes(std::span<const std::uint8_t> bytes)
{
    if (m_ok)
        m_buffer.insert(m_buffer.end(), bytes.begin(), bytes.end());
}

} // namespace vo::net
