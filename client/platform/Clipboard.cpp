#include "client/platform/Clipboard.h"

#include "client/platform/Win32Util.h"

#include <cstring>

namespace vo::client::platform {
namespace {

// RAII for OpenClipboard/CloseClipboard.
class ClipboardLock
{
public:
    explicit ClipboardLock(HWND owner) : m_open(::OpenClipboard(owner) != FALSE) {}
    ~ClipboardLock()
    {
        if (m_open)
            ::CloseClipboard();
    }
    ClipboardLock(const ClipboardLock&) = delete;
    ClipboardLock& operator=(const ClipboardLock&) = delete;
    [[nodiscard]] bool open() const noexcept { return m_open; }

private:
    bool m_open;
};

} // namespace

std::optional<std::string> getClipboardText(HWND owner)
{
    if (!::IsClipboardFormatAvailable(CF_UNICODETEXT))
        return std::nullopt;
    ClipboardLock lock(owner);
    if (!lock.open())
        return std::nullopt;
    HANDLE data = ::GetClipboardData(CF_UNICODETEXT);
    if (data == nullptr)
        return std::nullopt;
    const auto* wide = static_cast<const wchar_t*>(::GlobalLock(data));
    if (wide == nullptr)
        return std::nullopt;
    const std::size_t maxChars = ::GlobalSize(data) / sizeof(wchar_t);
    std::size_t len = 0;
    while (len < maxChars && wide[len] != L'\0')
        ++len;
    std::string text = toUtf8(std::wstring_view(wide, len));
    ::GlobalUnlock(data);
    return text;
}

bool setClipboardText(HWND owner, std::string_view utf8)
{
    const std::wstring wide = toWide(utf8);
    ClipboardLock lock(owner);
    if (!lock.open())
        return false;
    ::EmptyClipboard();
    const std::size_t bytes = (wide.size() + 1) * sizeof(wchar_t);
    HGLOBAL mem = ::GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (mem == nullptr)
        return false;
    void* dst = ::GlobalLock(mem);
    if (dst == nullptr)
    {
        ::GlobalFree(mem);
        return false;
    }
    std::memcpy(dst, wide.c_str(), bytes);
    ::GlobalUnlock(mem);
    if (::SetClipboardData(CF_UNICODETEXT, mem) == nullptr)
    {
        ::GlobalFree(mem);
        return false;
    }
    return true; // the clipboard owns `mem` now
}

} // namespace vo::client::platform
