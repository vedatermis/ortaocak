// PacketWriter: appends little-endian protocol fields (docs/PROTOCOL.md §1) to a byte buffer.
//
//   PacketWriter w;
//   w.writeU32(id);
//   w.writeStr8(name);
//   if (!w.ok()) { ... }   // a string was too long for its length prefix
//
// Errors are sticky: after the first failure ok() stays false and further writes are ignored.
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

namespace vo::net {

class PacketWriter
{
public:
    PacketWriter() = default;

    void writeU8(std::uint8_t v);
    void writeU16(std::uint16_t v);
    void writeU32(std::uint32_t v);
    void writeU64(std::uint64_t v);
    void writeF32(float v);
    void writeBool(bool v);
    // Length-prefixed UTF-8. Fails (sticky) if the byte length does not fit the prefix or the
    // text is not valid UTF-8.
    void writeStr8(std::string_view text);
    void writeStr16(std::string_view text);
    void writeBytes(std::span<const std::uint8_t> bytes);

    // Marks the packet as unencodable (e.g. a list longer than its protocol limit).
    void fail() noexcept { m_ok = false; }

    [[nodiscard]] bool ok() const noexcept { return m_ok; }
    [[nodiscard]] std::size_t size() const noexcept { return m_buffer.size(); }
    [[nodiscard]] std::span<const std::uint8_t> data() const noexcept { return m_buffer; }
    [[nodiscard]] std::vector<std::uint8_t>& buffer() noexcept { return m_buffer; }
    [[nodiscard]] std::vector<std::uint8_t> take() noexcept { return std::move(m_buffer); }

    void clear() noexcept
    {
        m_buffer.clear();
        m_ok = true;
    }

private:
    std::vector<std::uint8_t> m_buffer;
    bool m_ok = true;
};

} // namespace vo::net
