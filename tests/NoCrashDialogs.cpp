// Linked into every test executable: a crash, abort() or CRT assert must fail the run with a message
// on stderr, never open a modal "Debug Error!" / Windows Error Reporting dialog that blocks the
// process until somebody clicks it. Runs during static initialisation, before Catch2's main().
#ifdef _WIN32
#include <crtdbg.h>
#include <cstdlib>
#include <windows.h>

namespace {

struct NoCrashDialogs
{
    NoCrashDialogs() noexcept
    {
        SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
        _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
        const int types[] = {_CRT_WARN, _CRT_ERROR, _CRT_ASSERT};
        for (const int type : types)
        {
            _CrtSetReportMode(type, _CRTDBG_MODE_FILE);
            _CrtSetReportFile(type, _CRTDBG_FILE_STDERR);
        }
    }
};

const NoCrashDialogs g_noCrashDialogs;

} // namespace
#endif
