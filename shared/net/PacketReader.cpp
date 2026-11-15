#include "shared/net/PacketReader.h"

#include "shared/net/Utf8.h"

#include <algorithm>
#include <bit>
#include <string_view>

namespace vo::net {

const std::uint8_t* PacketReader::take(std::size_t n) noexcept
{
    if (!m_ok || m_data.size() - m_pos < n)
    {
        m_ok = false;
        return nullptr;
    }
    const std::uint8_t* p = m_data.data() + m_pos;
    m_pos += n;
    return p;
}

std::uint8_t PacketReader::readU8() noexcept
{
    const std::uint8_t* p = take(1);
    return p ? p[0] : 0;
}

std::uint16_t PacketReader::readU16() noexcept
{
    const std::uint8_t* p = take(2);
    if (!p)
        return 0;
    return static_cast<std::uint16_t>(p[0] | (p[1] << 8));
}

std::uint32_t PacketReader::readU32() noexcept
{
    const std::uint8_t* p = take(4);
    if (!p)
        return 0;
    std::uint32_t v = 0;
    for (int i = 3; i >= 0; --i)
        v = (v << 8) | p[i];
    return v;
}

std::uint64_t PacketReader::readU64() noexcept
{
    const std::uint8_t* p = take(8);
    if (!p)
        return 0;
    std::uint64_t v = 0;
    for (int i = 7; i >= 0; --i)
        v = (v << 8) | p[i];
    return v;
}

float PacketReader::readF32() noexcept
{
    return std::bit_cast<float>(readU32());
}

bool PacketReader::readBool() noexcept
{
    const std::uint8_t v = readU8();
    if (v > 1)
    {
        m_ok = false;
        return false;
    }
    return v == 1;
}

std::string PacketReader::readStr8()
{
    const std::size_t len = readU8();
    const std::uint8_t* p = take(len);
    if (!p)
        return {};
    std::string s(reinterpret_cast<const char*>(p), len);
    if (!isValidUtf8(s))
    {
        m_ok = false;
        return {};
    }
    return s;
}

std::string PacketReader::readStr16()
{
    const std::size_t len = readU16();
    const std::uint8_t* p = take(len);
    if (!p)
        return {};
    std::string s(reinterpret_cast<const char*>(p), len);
    if (!isValidUtf8(s))
    {
        m_ok = false;
        return {};
    }
    return s;
}

bool PacketReader::readBytes(std::span<std::uint8_t> out) noexcept
{
    const std::uint8_t* p = take(out.size());
    if (!p)
    {
        std::fill(out.begin(), out.end(), std::uint8_t{0});
        return false;
    }
    std::copy_n(p, out.size(), out.begin());
    return true;
}

} // namespace vo::net
