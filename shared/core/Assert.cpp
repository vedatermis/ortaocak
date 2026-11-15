#include "shared/core/Assert.h"

#include "shared/core/Log.h"

#include <cstdlib>

#if defined(_WIN32)
#include <Windows.h>
#endif

namespace vo::core {

void assertFailed(const char* expression, const char* message, const char* file, int line) noexcept
{
    try
    {
        const std::string text = std::format("assertion failed: {}{}{}", expression ? expression : "?",
                                             message ? " - " : "", message ? message : "");
        vo::log::writeMessage(vo::log::Level::Fatal, text, file, line);
    }
    catch (...)
    {
    }
    vo::log::flush();
#if defined(_WIN32)
    if (::IsDebuggerPresent())
        __debugbreak();
#endif
    std::abort();
}

} // namespace vo::core
