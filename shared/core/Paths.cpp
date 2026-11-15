#include "shared/core/Paths.h"

#include <system_error>
#include <vector>

#if defined(_WIN32)
#include <Windows.h>
#endif

namespace vo::core {

std::string pathToUtf8(const std::filesystem::path& path)
{
    const std::u8string u8 = path.u8string();
    return std::string(reinterpret_cast<const char*>(u8.data()), u8.size());
}

std::filesystem::path pathFromUtf8(std::string_view utf8)
{
    return std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(utf8.data()), utf8.size()));
}

std::filesystem::path executableDir()
{
#if defined(_WIN32)
    std::vector<wchar_t> buffer(MAX_PATH);
    for (;;)
    {
        const DWORD len = ::GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (len == 0)
            return std::filesystem::current_path();
        if (len < buffer.size())
            return std::filesystem::path(std::wstring(buffer.data(), len)).parent_path();
        buffer.resize(buffer.size() * 2);
    }
#else
    std::error_code ec;
    auto p = std::filesystem::read_symlink("/proc/self/exe", ec);
    return ec ? std::filesystem::current_path() : p.parent_path();
#endif
}

std::optional<std::filesystem::path> findUpwards(const std::filesystem::path& start,
                                                 const std::filesystem::path& relative)
{
    std::error_code ec;
    std::filesystem::path dir = std::filesystem::absolute(start, ec);
    if (ec)
        return std::nullopt;
    for (;;)
    {
        const auto candidate = dir / relative;
        if (std::filesystem::is_regular_file(candidate, ec))
            return candidate.lexically_normal();
        const auto parent = dir.parent_path();
        if (parent.empty() || parent == dir)
            return std::nullopt;
        dir = parent;
    }
}

std::optional<std::filesystem::path> locateDataFile(const std::filesystem::path& relative)
{
    std::error_code ec;
    if (auto p = findUpwards(std::filesystem::current_path(ec), relative))
        return p;
    return findUpwards(executableDir(), relative);
}

} // namespace vo::core
