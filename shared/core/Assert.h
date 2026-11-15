// VO_ASSERT(cond)            - checked in Debug builds (or when VO_ENABLE_ASSERTS=1), no-op otherwise.
// VO_ASSERT_MSG(cond, msg)   - same, with a message (string literal / const char*).
// VO_VERIFY(cond)            - always checked, in every build. Use for invariants whose violation
//                              would corrupt state or memory.
// On failure the condition is logged at FATAL, the log is flushed, a debugger break is raised if a
// debugger is attached, and the process aborts.
#pragma once

namespace vo::core {

[[noreturn]] void assertFailed(const char* expression, const char* message, const char* file, int line) noexcept;

} // namespace vo::core

#if !defined(VO_ENABLE_ASSERTS)
#if defined(VO_DEBUG)
#define VO_ENABLE_ASSERTS 1
#else
#define VO_ENABLE_ASSERTS 0
#endif
#endif

#define VO_VERIFY_MSG(cond, msg)                                                                             \
    do                                                                                                       \
    {                                                                                                        \
        if (!(cond)) [[unlikely]]                                                                            \
            ::vo::core::assertFailed(#cond, (msg), __FILE__, __LINE__);                                      \
    } while (0)
#define VO_VERIFY(cond) VO_VERIFY_MSG(cond, nullptr)

#if VO_ENABLE_ASSERTS
#define VO_ASSERT_MSG(cond, msg) VO_VERIFY_MSG(cond, msg)
#else
#define VO_ASSERT_MSG(cond, msg)                                                                             \
    do                                                                                                       \
    {                                                                                                        \
        (void)sizeof(!(cond));                                                                               \
    } while (0)
#endif
#define VO_ASSERT(cond) VO_ASSERT_MSG(cond, nullptr)
