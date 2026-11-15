// Shared plumbing for the binary asset formats (docs/ASSET_FORMATS.md §1): common header, CRC-32,
// asset path validation, bounds-checked little-endian reading and a deterministic writer.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace vo::assets {

inline constexpr std::uint16_t kFormatVersion = 1;
inline constexpr std::size_t kHeaderSize = 16;
inline constexpr std::uint64_t kMaxFileSize = 1ull << 30; // 1 GiB hard cap for any asset file

// File magics, as the four bytes appear in the file.
inline constexpr char kMagicMesh[4] = {'V', 'M', 'S', 'H'};
inline constexpr char kMagicAnim[4] = {'V', 'A', 'N', 'M'};
inline constexpr char kMagicTerrain[4] = {'V', 'T', 'E', 'R'};

enum class FileKind : std::uint8_t
{
    Unknown,
    Mesh,
    Animation,
    Terrain,
};

// zlib-compatible CRC-32.
[[nodiscard]] std::uint32_t crc32(std::span<const std::byte> data, std::uint32_t crc = 0) noexcept;

// Kind from the magic (no other validation). Unknown for short or foreign data.
[[nodiscard]] FileKind detectFileKind(std::span<const std::byte> data) noexcept;

// Relative, forward slashes, no "..", no "\", no leading "/", no drive letter, no empty segment,
// no control characters, length 1..1024.
[[nodiscard]] bool isValidAssetPath(std::string_view path) noexcept;

// Reads a whole file (<= kMaxFileSize). Writes atomically (temp file + rename).
[[nodiscard]] bool readFileBytes(const std::filesystem::path& path, std::vector<std::byte>& out, std::string& error);
[[nodiscard]] bool writeFileBytes(const std::filesystem::path& path, std::span<const std::byte> data,
                                  std::string& error);

namespace detail {

// Bounds-checked reader over a whole file. All offsets are absolute; failures set `ok = false`
// and return zero values, so parsers check `ok` once per section.
class ByteReader
{
public:
    explicit ByteReader(std::span<const std::byte> data) noexcept : m_data(data) {}

    [[nodiscard]] std::size_t size() const noexcept { return m_data.size(); }
    // True if [offset, offset + count * elemSize) lies inside the data (64-bit math, no overflow).
    [[nodiscard]] bool hasRange(std::uint64_t offset, std::uint64_t count, std::uint64_t elemSize) const noexcept
    {
        if (elemSize != 0 && count > (kMaxFileSize / elemSize))
            return false;
        const std::uint64_t bytes = count * elemSize;
        return offset <= m_data.size() && bytes <= m_data.size() - offset;
    }

    template <class T>
    [[nodiscard]] T read(std::uint64_t offset) noexcept
    {
        T value{};
        if (!hasRange(offset, 1, sizeof(T)))
        {
            ok = false;
            return value;
        }
        std::memcpy(&value, m_data.data() + offset, sizeof(T));
        return value;
    }

    [[nodiscard]] std::span<const std::byte> bytes(std::uint64_t offset, std::uint64_t size) noexcept
    {
        if (!hasRange(offset, size, 1))
        {
            ok = false;
            return {};
        }
        return m_data.subspan(static_cast<std::size_t>(offset), static_cast<std::size_t>(size));
    }

    bool ok = true;

private:
    std::span<const std::byte> m_data;
};

// Validates the 16-byte header: magic, version, flags, fileSize == data.size(), CRC.
[[nodiscard]] bool checkHeader(std::span<const std::byte> data, const char (&magic)[4], std::string& error);

// Resolves a string reference into the string table; false if out of range.
[[nodiscard]] bool readStringRef(ByteReader& reader, std::uint64_t refOffset, std::span<const std::byte> table,
                                 std::string& out);

// Little-endian writer (the target platform is little-endian; static_assert in the .cpp).
class ByteWriter
{
public:
    [[nodiscard]] std::size_t size() const noexcept { return m_data.size(); }
    template <class T>
    void put(const T& value)
    {
        const auto* p = reinterpret_cast<const std::byte*>(&value);
        m_data.insert(m_data.end(), p, p + sizeof(T));
    }
    void putBytes(std::span<const std::byte> bytes) { m_data.insert(m_data.end(), bytes.begin(), bytes.end()); }
    void putZeros(std::size_t count) { m_data.insert(m_data.end(), count, std::byte{0}); }
    void align4()
    {
        while (m_data.size() % 4 != 0)
            m_data.push_back(std::byte{0});
    }
    template <class T>
    void patch(std::size_t offset, const T& value)
    {
        std::memcpy(m_data.data() + offset, &value, sizeof(T));
    }
    // Writes magic/version/fileSize/CRC into the first 16 bytes (reserved by beginFile).
    void beginFile(const char (&magic)[4]);
    [[nodiscard]] std::vector<std::byte> finishFile();

private:
    std::vector<std::byte> m_data;
};

// Collects strings in insertion order; refs are (offset, length) into the table.
class StringTableBuilder
{
public:
    struct Ref
    {
        std::uint32_t offset = 0;
        std::uint32_t length = 0;
    };
    Ref add(std::string_view s);
    [[nodiscard]] std::span<const std::byte> bytes() const noexcept { return m_bytes; }

private:
    std::vector<std::byte> m_bytes;
};

} // namespace detail
} // namespace vo::assets
