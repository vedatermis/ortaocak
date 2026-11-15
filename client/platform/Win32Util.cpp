#include "client/platform/Win32Util.h"

#include <shellapi.h>

#include <cstdio>
#include <format>

namespace vo::client::platform {

std::string toUtf8(std::wstring_view wide)
{
    if (wide.empty())
        return {};
    const int len = ::WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), nullptr, 0,
                                          nullptr, nullptr);
    if (len <= 0)
        return {};
    std::string out(static_cast<std::size_t>(len), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), out.data(), len, nullptr,
                          nullptr);
    return out;
}

std::wstring toWide(std::string_view utf8)
{
    if (utf8.empty())
        return {};
    const int len = ::MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    if (len <= 0)
        return {};
    std::wstring out(static_cast<std::size_t>(len), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), out.data(), len);
    return out;
}

namespace {

std::string systemMessage(DWORD code)
{
    wchar_t* buffer = nullptr;
    const DWORD len = ::FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                                           FORMAT_MESSAGE_IGNORE_INSERTS,
                                       nullptr, code, 0, reinterpret_cast<wchar_t*>(&buffer), 0, nullptr);
    std::string text;
    if (len > 0 && buffer != nullptr)
    {
        text = toUtf8(std::wstring_view(buffer, len));
        while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == ' '))
            text.pop_back();
    }
    if (buffer != nullptr)
        ::LocalFree(buffer);
    return text;
}

} // namespace

std::string hresultToString(HRESULT hr)
{
    const std::string msg = systemMessage(static_cast<DWORD>(hr));
    if (msg.empty())
        return std::format("0x{:08X}", static_cast<unsigned long>(hr));
    return std::format("0x{:08X} ({})", static_cast<unsigned long>(hr), msg);
}

std::string lastErrorToString(DWORD error)
{
    const std::string msg = systemMessage(error);
    return msg.empty() ? std::format("error {}", error) : std::format("error {} ({})", error, msg);
}

std::vector<std::string> commandLineArgsUtf8()
{
    std::vector<std::string> args;
    int argc = 0;
    wchar_t** argv = ::CommandLineToArgvW(::GetCommandLineW(), &argc);
    if (argv == nullptr)
        return args;
    args.reserve(static_cast<std::size_t>(argc));
    for (int i = 0; i < argc; ++i)
        args.push_back(toUtf8(argv[i]));
    ::LocalFree(argv);
    return args;
}

bool attachParentConsole()
{
    if (!::AttachConsole(ATTACH_PARENT_PROCESS))
        return false;
    std::FILE* f = nullptr;
    freopen_s(&f, "CONOUT$", "w", stdout);
    freopen_s(&f, "CONOUT$", "w", stderr);
    return true;
}

} // namespace vo::client::platform
