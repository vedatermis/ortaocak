// vo_client entry point (M0 skeleton): Win32 window + D3D11 swap chain cleared to dark blue.
//
// Automation flags:
//   --screenshot <file.png>   capture the back buffer of the last frame to PNG, then exit
//   --exit-after-frames <n>   exit after n presented frames (default with --screenshot: 1)
// Other flags: --config <client.toml>, --log <file>, --warp, --no-debug-layer, --console, --help

#include "client/platform/FrameTimer.h"
#include "client/platform/Win32Util.h"
#include "client/platform/Window.h"
#include "client/render/RenderDevice.h"
#include "client/render/Screenshot.h"
#include "shared/core/Config.h"
#include "shared/core/Log.h"
#include "shared/core/Paths.h"
#include "shared/core/Version.h"

#include <Windows.h>
#include <objbase.h>

#include <charconv>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace {

namespace platform = vo::client::platform;
namespace render = vo::client::render;

constexpr int kExitOk = 0;
constexpr int kExitUsage = 1;
constexpr int kExitInit = 3;
constexpr int kExitRuntime = 4;
constexpr int kExitScreenshot = 5;

constexpr float kClearColor[4] = {0.04f, 0.07f, 0.18f, 1.0f}; // dark blue

struct Options
{
    std::optional<std::filesystem::path> configPath;
    std::optional<std::filesystem::path> logPath;
    std::optional<std::filesystem::path> screenshotPath;
    std::optional<std::uint64_t> exitAfterFrames;
    bool forceWarp = false;
    bool noDebugLayer = false;
    bool console = false;
    bool help = false;
};

constexpr const char* kUsage =
    "usage: vo_client [--config <client.toml>] [--log <file>] [--screenshot <file.png>]\n"
    "                 [--exit-after-frames <n>] [--warp] [--no-debug-layer] [--console] [--help]\n";

bool parseArgs(const std::vector<std::string>& args, Options& out, std::string& error)
{
    for (std::size_t i = 1; i < args.size(); ++i)
    {
        const std::string& arg = args[i];
        const bool hasValue = i + 1 < args.size();
        if (arg == "--help" || arg == "-h")
            out.help = true;
        else if (arg == "--warp")
            out.forceWarp = true;
        else if (arg == "--no-debug-layer")
            out.noDebugLayer = true;
        else if (arg == "--console")
            out.console = true;
        else if (arg == "--config" && hasValue)
            out.configPath = vo::core::pathFromUtf8(args[++i]);
        else if (arg == "--log" && hasValue)
            out.logPath = vo::core::pathFromUtf8(args[++i]);
        else if (arg == "--screenshot" && hasValue)
            out.screenshotPath = vo::core::pathFromUtf8(args[++i]);
        else if (arg == "--exit-after-frames" && hasValue)
        {
            const std::string& v = args[++i];
            std::uint64_t n = 0;
            const auto [ptr, ec] = std::from_chars(v.data(), v.data() + v.size(), n);
            if (ec != std::errc{} || ptr != v.data() + v.size() || n == 0)
            {
                error = "invalid --exit-after-frames value '" + v + "'";
                return false;
            }
            out.exitAfterFrames = n;
        }
        else
        {
            error = "unknown or incomplete argument '" + arg + "'";
            return false;
        }
    }
    return true;
}

void setupLogging(const Options& opts, const vo::core::Config& config)
{
    vo::log::setDebuggerOutputEnabled(true);
    if (auto lvl = vo::log::parseLevel(config.getString("log.level", "info")))
        vo::log::setLevel(*lvl);

    std::filesystem::path logPath;
    if (opts.logPath)
        logPath = *opts.logPath;
    else if (auto fromConfig = config.getString("log.file", ""); !fromConfig.empty())
        logPath = vo::core::pathFromUtf8(fromConfig);
    if (logPath.empty())
        return;
    if (logPath.is_relative() && !opts.logPath)
        logPath = vo::core::executableDir() / logPath;
    vo::log::openFile(logPath, /*truncate=*/true);
}

int run(const Options& opts)
{
    // ---- Config (optional for the client: defaults are fine) ----
    vo::core::Config config;
    std::string error;
    std::optional<std::filesystem::path> configPath = opts.configPath;
    if (!configPath)
        configPath = vo::core::locateDataFile("config/client.toml");
    std::string configStatus = "defaults (config/client.toml not found)";
    if (configPath)
    {
        if (config.loadWithOverlay(*configPath, error))
            configStatus = vo::core::pathToUtf8(*configPath);
        else
            configStatus = "defaults (" + error + ")";
    }
    setupLogging(opts, config);

    VO_LOG_INFO("{} client v{} ({})", vo::version::kProductName, vo::version::kString, vo::version::kBuildConfig);
    VO_LOG_INFO("config: {}", configStatus);

    const auto width = static_cast<int>(config.getInt("window.width", 1280));
    const auto height = static_cast<int>(config.getInt("window.height", 720));
    const bool vsync = config.getBool("window.vsync", true);

    // ---- Window ----
    platform::Window window;
    platform::Window::Desc windowDesc;
    windowDesc.title = L"VedatOnline";
    windowDesc.clientWidth = width > 0 ? width : 1280;
    windowDesc.clientHeight = height > 0 ? height : 720;
    if (!window.create(windowDesc, error))
    {
        VO_LOG_FATAL("window creation failed: {}", error);
        return kExitInit;
    }

    // ---- Renderer ----
    render::RenderDevice device;
    render::RenderDevice::Desc deviceDesc;
    deviceDesc.hwnd = window.handle();
    deviceDesc.width = static_cast<std::uint32_t>(window.clientWidth());
    deviceDesc.height = static_cast<std::uint32_t>(window.clientHeight());
    deviceDesc.vsync = vsync;
#if defined(VO_DEBUG)
    deviceDesc.debugLayer = !opts.noDebugLayer;
#endif
    deviceDesc.forceWarp = opts.forceWarp;
    if (!device.init(deviceDesc, error))
    {
        VO_LOG_FATAL("renderer init failed: {}", error);
        return kExitInit;
    }

    std::optional<std::uint64_t> exitAfter = opts.exitAfterFrames;
    if (opts.screenshotPath && !exitAfter)
        exitAfter = 1;
    if (exitAfter)
        VO_LOG_INFO("automation: exit after {} frame(s){}", *exitAfter,
                    opts.screenshotPath ? ", screenshot " + vo::core::pathToUtf8(*opts.screenshotPath) : "");

    // ---- Frame loop ----
    int exitCode = kExitOk;
    platform::FrameTimer timer;
    while (window.pumpMessages())
    {
        int newWidth = 0;
        int newHeight = 0;
        if (window.consumeResize(newWidth, newHeight) &&
            !device.resize(static_cast<std::uint32_t>(newWidth), static_cast<std::uint32_t>(newHeight), error))
        {
            VO_LOG_ERROR("{}", error);
            exitCode = kExitRuntime;
            break;
        }
        if (window.isMinimized())
        {
            ::WaitMessage(); // nothing to draw; block until the window is restored
            continue;
        }

        timer.tick();
        const std::uint64_t frame = timer.frameCount();
        device.beginFrame(kClearColor);

        const bool lastFrame = exitAfter && frame >= *exitAfter;
        if (lastFrame && opts.screenshotPath)
        {
            render::CapturedImage image;
            if (device.captureBackBuffer(image, error) && render::writePng(*opts.screenshotPath, image, error))
            {
                VO_LOG_INFO("screenshot {}x{} written to {}", image.width, image.height,
                            vo::core::pathToUtf8(*opts.screenshotPath));
            }
            else
            {
                VO_LOG_ERROR("screenshot failed: {}", error);
                exitCode = kExitScreenshot;
            }
        }

        if (!device.present(error))
        {
            VO_LOG_ERROR("{}", error);
            exitCode = kExitRuntime;
            break;
        }
        if (timer.consumeFpsUpdate())
            VO_LOG_DEBUG("fps {:.1f}", timer.fps());
        if (lastFrame)
            window.requestClose();
    }

    VO_LOG_INFO("shutting down after {} frame(s)", timer.frameCount());
    device.shutdown();
    window.destroy();
    return exitCode;
}

} // namespace

int WINAPI wWinMain(_In_ HINSTANCE, _In_opt_ HINSTANCE, _In_ PWSTR, _In_ int)
{
    ::SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    Options opts;
    std::string argError;
    const bool argsOk = parseArgs(platform::commandLineArgsUtf8(), opts, argError);
    if (opts.console || !argsOk || opts.help)
        platform::attachParentConsole();
    if (!argsOk)
    {
        VO_LOG_ERROR("{}", argError);
        std::fputs(kUsage, stderr);
        return kExitUsage;
    }
    if (opts.help)
    {
        std::fputs(kUsage, stdout);
        return kExitOk;
    }

    const HRESULT comInit = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    int code = kExitRuntime;
    try
    {
        code = run(opts);
    }
    catch (const std::exception& e)
    {
        VO_LOG_FATAL("unhandled exception: {}", e.what());
    }
    if (SUCCEEDED(comInit))
        ::CoUninitialize();
    VO_LOG_INFO("exit code {}", code);
    vo::log::flush();
    vo::log::closeFile();
    return code;
}
