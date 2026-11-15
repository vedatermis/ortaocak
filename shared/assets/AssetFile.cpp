#include "shared/assets/AssetFile.h"

#include "shared/core/Paths.h"

#include <array>
#include <bit>
#include <format>
#include <fstream>
#include <system_error>

namespace vo::assets {

static_assert(std::endian::native == std::endian::little, "asset formats assume a little-endian host");

namespace {

constexpr std::array<std::uint32_t, 256> makeCrcTable() noexcept
{
    std::array<std::uint32_t, 256> table{};
    for (std::uint32_t i = 0; i < 256; ++i)
    {
        std::uint32_t c = i;
        for (int k = 0; k < 8; ++k)
            c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        table[i] = c;
    }
    return table;
}

constexpr std::array<std::uint32_t, 256> kCrcTable = makeCrcTable();

} // namespace

std::uint32_t crc32(std::span<const std::byte> data, std::uint32_t crc) noexcept
{
    std::uint32_t c = crc ^ 0xFFFFFFFFu;
    for (const std::byte b : data)
        c = kCrcTable[(c ^ static_cast<std::uint32_t>(b)) & 0xFFu] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

FileKind detectFileKind(std::span<const std::byte> data) noexcept
{
    if (data.size() < 4)
        return FileKind::Unknown;
    const auto is = [&](const char (&magic)[4]) { return std::memcmp(data.data(), magic, 4) == 0; };
    if (is(kMagicMesh))
        return FileKind::Mesh;
    if (is(kMagicAnim))
        return FileKind::Animation;
    if (is(kMagicTerrain))
        return FileKind::Terrain;
    return FileKind::Unknown;
}

bool isValidAssetPath(std::string_view path) noexcept
{
    if (path.empty() || path.size() > 1024)
        return false;
    if (path.front() == '/')
        return false;
    std::size_t segStart = 0;
    for (std::size_t i = 0; i <= path.size(); ++i)
    {
        if (i == path.size() || path[i] == '/')
        {
            const std::string_view seg = path.substr(segStart, i - segStart);
            if (seg.empty() || seg == "." || seg == "..")
                return false;
            segStart = i + 1;
            continue;
        }
        const auto c = static_cast<unsigned char>(path[i]);
        if (c < 0x20 || c == 0x7F || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' ||
            c == '>' || c == '|')
            return false;
    }
    return true;
}

bool readFileBytes(const std::filesystem::path& path, std::vector<std::byte>& out, std::string& error)
{
    std::error_code ec;
    const auto size = std::filesystem::file_size(path, ec);
    if (ec)
    {
        error = std::format("cannot open '{}': {}", core::pathToUtf8(path), ec.message());
        return false;
    }
    if (size > kMaxFileSize)
    {
        error = std::format("'{}' is too large ({} bytes)", core::pathToUtf8(path), size);
        return false;
    }
    std::ifstream in(path, std::ios::binary);
    if (!in)
    {
        error = std::format("cannot open '{}'", core::pathToUtf8(path));
        return false;
    }
    out.resize(static_cast<std::size_t>(size));
    if (size > 0 && !in.read(reinterpret_cast<char*>(out.data()), static_cast<std::streamsize>(size)))
    {
        error = std::format("cannot read '{}'", core::pathToUtf8(path));
        return false;
    }
    return true;
}

bool writeFileBytes(const std::filesystem::path& path, std::span<const std::byte> data, std::string& error)
{
    std::error_code ec;
    if (path.has_parent_path())
        std::filesystem::create_directories(path.parent_path(), ec);
    std::filesystem::path tmp = path;
    tmp += ".tmp";
    {
        std::ofstream outFile(tmp, std::ios::binary | std::ios::trunc);
        if (!outFile)
        {
            error = std::format("cannot create '{}'", core::pathToUtf8(tmp));
            return false;
        }
        outFile.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
        if (!outFile)
        {
            error = std::format("cannot write '{}'", core::pathToUtf8(tmp));
            return false;
        }
    }
    std::filesystem::rename(tmp, path, ec);
    if (ec)
    {
        std::filesystem::remove(tmp, ec);
        error = std::format("cannot replace '{}'", core::pathToUtf8(path));
        return false;
    }
    return true;
}

namespace detail {

bool checkHeader(std::span<const std::byte> data, const char (&magic)[4], std::string& error)
{
    if (data.size() < kHeaderSize)
    {
        error = "file too small for header";
        return false;
    }
    if (std::memcmp(data.data(), magic, 4) != 0)
    {
        error = std::format("bad magic (expected {}{}{}{})", magic[0], magic[1], magic[2], magic[3]);
        return false;
    }
    ByteReader r(data);
    const auto version = r.read<std::uint16_t>(4);
    const auto flags = r.read<std::uint16_t>(6);
    const auto fileSize = r.read<std::uint32_t>(8);
    const auto crc = r.read<std::uint32_t>(12);
    if (version != kFormatVersion)
    {
        error = std::format("unsupported version {}", version);
        return false;
    }
    if (flags != 0)
    {
        error = "non-zero header flags";
        return false;
    }
    if (fileSize != data.size())
    {
        error = std::format("size mismatch: header says {} bytes, file has {}", fileSize, data.size());
        return false;
    }
    if (crc32(data.subspan(kHeaderSize)) != crc)
    {
        error = "CRC mismatch (file corrupt)";
        return false;
    }
    return true;
}

bool readStringRef(ByteReader& reader, std::uint64_t refOffset, std::span<const std::byte> table, std::string& out)
{
    const auto off = reader.read<std::uint32_t>(refOffset);
    const auto len = reader.read<std::uint32_t>(refOffset + 4);
    if (!reader.ok)
        return false;
    if (static_cast<std::uint64_t>(off) + len > table.size())
        return false;
    out.assign(reinterpret_cast<const char*>(table.data()) + off, len);
    return true;
}

void ByteWriter::beginFile(const char (&magic)[4])
{
    m_data.clear();
    putBytes(std::as_bytes(std::span<const char, 4>(magic, 4)));
    put<std::uint16_t>(kFormatVersion);
    put<std::uint16_t>(0);
    put<std::uint32_t>(0);
    put<std::uint32_t>(0);
}

std::vector<std::byte> ByteWriter::finishFile()
{
    patch<std::uint32_t>(8, static_cast<std::uint32_t>(m_data.size()));
    patch<std::uint32_t>(12, crc32(std::span<const std::byte>(m_data).subspan(kHeaderSize)));
    return std::move(m_data);
}

StringTableBuilder::Ref StringTableBuilder::add(std::string_view s)
{
    Ref ref{static_cast<std::uint32_t>(m_bytes.size()), static_cast<std::uint32_t>(s.size())};
    const auto* p = reinterpret_cast<const std::byte*>(s.data());
    m_bytes.insert(m_bytes.end(), p, p + s.size());
    return ref;
}

} // namespace detail
} // namespace vo::assets
