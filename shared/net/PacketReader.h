// PacketReader: bounds-checked little-endian reader for protocol payloads (docs/PROTOCOL.md §1).
//
// Never throws and never reads out of bounds. The first malformed read (out of bytes, bool not 0/1,
// string not valid UTF-8) sets a sticky error flag; every later read returns a zero value.
//
//   PacketReader r(payload);
//   const auto id = r.readU32();
//   const auto name = r.readStr8();
//   if (!r.finished()) -> protocol error (malformed or trailing bytes)
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

namespace vo::net {

class PacketReader
{
public:
    PacketReader() = default;
    explicit PacketReader(std::span<const std::uint8_t> data) noexcept : m_data(data) {}

    [[nodiscard]] std::uint8_t readU8() noexcept;
    [[nodiscard]] std::uint16_t readU16() noexcept;
    [[nodiscard]] std::uint32_t readU32() noexcept;
    [[nodiscard]] std::uint64_t readU64() noexcept;
    [[nodiscard]] float readF32() noexcept;
    [[nodiscard]] bool readBool() noexcept;            // only 0 or 1 accepted
    [[nodiscard]] std::string readStr8();              // validated UTF-8
    [[nodiscard]] std::string readStr16();             // validated UTF-8
    // Copies exactly out.size() bytes; on failure `out` is zero-filled.
    bool readBytes(std::span<std::uint8_t> out) noexcept;

    // Marks the packet as malformed (e.g. a field value outside its allowed range).
    void fail() noexcept { m_ok = false; }

    [[nodiscard]] bool ok() const noexcept { return m_ok; }
    // True if no error occurred and every byte was consumed (no trailing bytes).
    [[nodiscard]] bool finished() const noexcept { return m_ok && m_pos == m_data.size(); }
    [[nodiscard]] std::size_t remaining() const noexcept { return m_ok ? m_data.size() - m_pos : 0; }
    [[nodiscard]] std::size_t position() const noexcept { return m_pos; }

private:
    // Returns a pointer to n readable bytes and advances, or nullptr (and sets the error flag).
    const std::uint8_t* take(std::size_t n) noexcept;

    std::span<const std::uint8_t> m_data;
    std::size_t m_pos = 0;
    bool m_ok = true;
};

} // namespace vo::net
