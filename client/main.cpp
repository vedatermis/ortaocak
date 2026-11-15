// vo_client entry point: Win32 window + D3D11 swap chain, 2D renderer, UI toolkit and scenes.
//
// Scene: --scene login (default: login -> character select/create -> world placeholder, against the
// server in config/client.toml [server] + server_public_key) | uidemo (every widget, for verification).
// Window: --size WxH (client pixels), --ui-scale <f> (multiplies the monitor DPI scale).
// User settings ("Beni hatırla"): %LOCALAPPDATA%\VedatOnline\client_settings.toml, --settings <file>.
// Automation:
//   --screenshot <file.png>   capture the back buffer of the last frame to PNG, then exit
//   --exit-after-frames <n>   exit after n presented frames (default with --screenshot: 1)
//   --script <file.voscript>  timed synthetic input (see client/automation/Script.h); while a
//                             script runs real mouse/keyboard input is ignored and the clipboard is
//                             an in-memory one
//   --screenshot-dir <dir>    base directory for relative script screenshot paths
// Other flags: --config <client.toml>, --log <file>, --warp, --no-debug-layer, --console, --help,
//   --protocol-version <n>    (testing) announce another protocol version in HELLO
//   --no-art                  (testing) ignore assets/ui images, draw the procedural fallbacks
//
// Exit codes: 0 ok, 1 bad arguments/script, 3 init failed, 4 runtime error, 5 screenshot failed,
// 6 script assertion failed (waitfor timeout, expect, clickon, expecttext).

#include "client/automation/ScriptPlayer.h"
#include "client/net/GameClient.h"
#include "client/platform/Clipboard.h"
#include "client/platform/FrameTimer.h"
#include "client/platform/Input.h"
#include "client/platform/Win32Util.h"
#include "client/platform/Window.h"
#include "client/render/Font.h"
#include "client/render/RenderDevice.h"
#include "client/render/RenderStates.h"
#include "client/render/Screenshot.h"
#include "client/render/SpriteBatch.h"
#include "client/scenes/SceneWidgets.h"
#include "client/scenes/Scenes.h"
#include "client/ui/ClipboardProvider.h"
#include "client/ui/Theme.h"
#include "client/ui/Ui.h"
#include "shared/core/Config.h"
#include "shared/core/Log.h"
#include "shared/core/Paths.h"
#include "shared/core/Version.h"
#include "shared/net/Crypto.h"

#include <Windows.h>
#include <KnownFolders.h>
#include <ShlObj.h>
#include <objbase.h>

#if defined(_DEBUG)
#include <crtdbg.h>
#endif

#include <array>
#include <atomic>
#include <algorithm>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace {

namespace platform = vo::client::platform;
namespace render = vo::client::render;
namespace scenes = vo::client::scenes;
namespace ui = vo::client::ui;
namespace automation = vo::client::automation;
namespace cnet = vo::client::net;

constexpr int kExitOk = 0;
constexpr int kExitUsage = 1;
constexpr int kExitInit = 3;
constexpr int kExitRuntime = 4;
constexpr int kExitScreenshot = 5;
constexpr int kExitScriptFailed = 6;

// ---- Debug-only heap allocation counter (verifies the steady state does not allocate) ----
#if defined(_DEBUG)
std::atomic<std::uint64_t> g_heapAllocations{0};

int __cdecl countingAllocHook(int allocType, void*, std::size_t, int, long, const unsigned char*, int)
{
    if (allocType == _HOOK_ALLOC || allocType == _HOOK_REALLOC)
        g_heapAllocations.fetch_add(1, std::memory_order_relaxed);
    return TRUE;
}
#endif

// Per-frame CRT heap allocation statistics (Debug builds; Release reports nothing).
class HeapStats
{
public:
    HeapStats()
    {
#if defined(_DEBUG)
        _CrtSetAllocHook(&countingAllocHook);
#endif
    }
    ~HeapStats()
    {
#if defined(_DEBUG)
        _CrtSetAllocHook(nullptr);
#endif
    }
    HeapStats(const HeapStats&) = delete;
    HeapStats& operator=(const HeapStats&) = delete;

    void beginFrame() noexcept { m_start = current(); }
    void endFrame() noexcept
    {
        const std::uint64_t n = current() - m_start;
        m_recent[m_frames % m_recent.size()] = n;
        ++m_frames;
    }
    void report() const
    {
#if defined(_DEBUG)
        const std::size_t count =
            m_frames < m_recent.size() ? static_cast<std::size_t>(m_frames) : m_recent.size();
        std::uint64_t total = 0;
        std::uint64_t worst = 0;
        std::size_t zero = 0;
        for (std::size_t i = 0; i < count; ++i)
        {
            total += m_recent[i];
            worst = m_recent[i] > worst ? m_recent[i] : worst;
            zero += m_recent[i] == 0 ? 1 : 0;
        }
        VO_LOG_INFO("heap: last {} frame(s): {} CRT allocation(s), worst frame {}, {} frame(s) with none",
                    count, total, worst, zero);
#endif
    }

private:
    static std::uint64_t current() noexcept
    {
#if defined(_DEBUG)
        return g_heapAllocations.load(std::memory_order_relaxed);
#else
        return 0;
#endif
    }

    std::array<std::uint64_t, 120> m_recent{};
    std::uint64_t m_frames = 0;
    std::uint64_t m_start = 0;
};

struct Options
{
    std::optional<std::filesystem::path> configPath;
    std::optional<std::filesystem::path> logPath;
    std::optional<std::filesystem::path> screenshotPath;
    std::optional<std::filesystem::path> scriptPath;
    std::optional<std::filesystem::path> screenshotDir;
    std::optional<std::filesystem::path> settingsPath;
    std::optional<std::uint64_t> exitAfterFrames;
    std::uint16_t protocolVersion = 0;
    std::optional<std::pair<int, int>> size;
    std::string scene = "login";
    float uiScale = 1.0f;
    bool forceWarp = false;
    bool noArt = false;
    bool noDebugLayer = false;
    bool console = false;
    bool help = false;
};

constexpr const char* kUsage =
    "usage: vo_client [--scene login|uidemo] [--size WxH] [--ui-scale <f>] [--config <client.toml>]\n"
    "                 [--log <file>] [--screenshot <file.png>] [--exit-after-frames <n>]\n"
    "                 [--script <file.voscript>] [--screenshot-dir <dir>] [--settings <file>]\n"
    "                 [--no-art] [--protocol-version <n>] [--warp] [--no-debug-layer] [--console] [--help]\n";

bool parseSize(const std::string& v, std::pair<int, int>& out)
{
    const std::size_t x = v.find_first_of("xX");
    if (x == std::string::npos)
        return false;
    int w = 0;
    int h = 0;
    const auto r1 = std::from_chars(v.data(), v.data() + x, w);
    const auto r2 = std::from_chars(v.data() + x + 1, v.data() + v.size(), h);
    if (r1.ec != std::errc{} || r1.ptr != v.data() + x || r2.ec != std::errc{} ||
        r2.ptr != v.data() + v.size())
        return false;
    if (w < 320 || h < 240 || w > 16384 || h > 16384)
        return false;
    out = {w, h};
    return true;
}

bool parseArgs(const std::vector<std::string>& args, Options& out, std::string& error)
{
    for (std::size_t i = 1; i < args.size(); ++i)
    {
        const std::string& arg = args[i];
        const bool hasValue = i + 1 < args.size();
        if (arg == "--help" || arg == "-h")
            out.help = true;
        else if (arg == "--no-art")
            out.noArt = true;
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
        else if (arg == "--script" && hasValue)
            out.scriptPath = vo::core::pathFromUtf8(args[++i]);
        else if (arg == "--screenshot-dir" && hasValue)
            out.screenshotDir = vo::core::pathFromUtf8(args[++i]);
        else if (arg == "--settings" && hasValue)
            out.settingsPath = vo::core::pathFromUtf8(args[++i]);
        else if (arg == "--protocol-version" && hasValue)
        {
            const std::string& v = args[++i];
            const auto [ptr, ec] = std::from_chars(v.data(), v.data() + v.size(), out.protocolVersion);
            if (ec != std::errc{} || ptr != v.data() + v.size() || out.protocolVersion == 0)
            {
                error = "invalid --protocol-version '" + v + "'";
                return false;
            }
        }
        else if (arg == "--scene" && hasValue)
        {
            out.scene = args[++i];
            if (out.scene != "login" && out.scene != "uidemo")
            {
                error = "unknown scene '" + out.scene + "' (login, uidemo)";
                return false;
            }
        }
        else if (arg == "--size" && hasValue)
        {
            std::pair<int, int> size;
            if (!parseSize(args[++i], size))
            {
                error = "invalid --size '" + args[i] + "' (expected WxH, 320x240 or larger)";
                return false;
            }
            out.size = size;
        }
        else if (arg == "--ui-scale" && hasValue)
        {
            const std::string& v = args[++i];
            float f = 0.0f;
            const auto [ptr, ec] = std::from_chars(v.data(), v.data() + v.size(), f);
            if (ec != std::errc{} || ptr != v.data() + v.size() || f < 0.5f || f > 4.0f)
            {
                error = "invalid --ui-scale '" + v + "' (0.5 .. 4)";
                return false;
            }
            out.uiScale = f;
        }
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

// System clipboard bound to the main window.
class SystemClipboard final : public ui::ClipboardProvider
{
public:
    explicit SystemClipboard(HWND hwnd) : m_hwnd(hwnd) {}
    [[nodiscard]] std::string getText() override
    {
        return platform::getClipboardText(m_hwnd).value_or(std::string{});
    }
    void setText(std::string_view utf8) override { platform::setClipboardText(m_hwnd, utf8); }

private:
    HWND m_hwnd;
};

bool captureScreenshot(render::RenderDevice& device, const std::filesystem::path& path)
{
    std::string error;
    render::CapturedImage image;
    if (device.captureBackBuffer(image, error) && render::writePng(path, image, error))
    {
        VO_LOG_INFO("screenshot {}x{} written to {}", image.width, image.height, vo::core::pathToUtf8(path));
        return true;
    }
    VO_LOG_ERROR("screenshot failed: {}", error);
    return false;
}

std::unique_ptr<scenes::Scene> makeScene(const std::string& name)
{
    if (name == "uidemo")
        return std::make_unique<scenes::UiDemoScene>();
    return std::make_unique<scenes::LoginScene>();
}

// %LOCALAPPDATA%\VedatOnline\client_settings.toml (falls back to the executable directory).
std::filesystem::path defaultSettingsPath()
{
    PWSTR dir = nullptr;
    std::filesystem::path out;
    if (SUCCEEDED(::SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_DEFAULT, nullptr, &dir)) && dir != nullptr)
        out = std::filesystem::path(dir) / L"VedatOnline" / L"client_settings.toml";
    ::CoTaskMemFree(dir);
    if (out.empty())
        out = vo::core::executableDir() / "client_settings.toml";
    return out;
}

cnet::GameClient::Settings makeNetSettings(const vo::core::Config& config, const Options& opts)
{
    cnet::GameClient::Settings s;
    s.host = config.getString("server.host", "127.0.0.1");
    const std::int64_t port = config.getInt("server.port", vo::net::kDefaultPort);
    if (port > 0 && port <= 65535)
        s.port = static_cast<std::uint16_t>(port);
    else
        VO_LOG_WARN("config: server.port {} out of range, using {}", port, vo::net::kDefaultPort);
    const std::string keyHex = config.getString("server_public_key", "");
    s.serverKey = vo::net::parsePublicKeyHex(keyHex);
    if (!s.serverKey)
        VO_LOG_WARN("config: server_public_key is {} - logging in will fail until it is set "
                    "(tools/dev/setup-dev.ps1 writes it to config/client.local.toml)",
                    keyHex.empty() ? "empty" : "not 64 hex characters");
    const auto clampMs = [](std::int64_t v, std::int64_t lo, std::int64_t hi)
    { return cnet::Millis(std::clamp<std::int64_t>(v, lo, hi)); };
    s.handshakeTimeout = clampMs(config.getInt("server.connect_timeout_ms", 10000), 500, 120000);
    s.requestTimeout = clampMs(config.getInt("server.request_timeout_ms", 15000), 1000, 300000);
    s.clientBuild = static_cast<std::uint32_t>(vo::version::kMajor * 10000 + vo::version::kMinor * 100 +
                                               vo::version::kPatch);
    s.protocolVersion = opts.protocolVersion;
    return s;
}

// Automation: what a named widget of the active scene looks like right now.
std::optional<automation::ScriptPlayer::WidgetInfo> describeWidget(scenes::Scene* scene, std::string_view name)
{
    ui::UiContext* uiContext = scene != nullptr ? scene->automationUi() : nullptr;
    if (uiContext == nullptr)
        return std::nullopt;
    ui::Widget* w = uiContext->findByName(name);
    if (w == nullptr)
        return std::nullopt;
    automation::ScriptPlayer::WidgetInfo info;
    info.rect = w->rect();
    info.visible = true;
    const ui::Widget* top = w;
    for (const ui::Widget* p = w; p != nullptr; p = p->parent())
    {
        info.visible = info.visible && p->isVisible();
        top = p;
    }
    // A modal layer blocks everything beneath it.
    info.enabled = w->isEnabled() && !(uiContext->hasModal() && top == &uiContext->root());
    if (const auto* label = dynamic_cast<const ui::Label*>(w))
        info.text = label->text();
    else if (const auto* button = dynamic_cast<const ui::Button*>(w))
        info.text = button->text();
    else if (const auto* box = dynamic_cast<const ui::TextBox*>(w))
        info.text = box->isPassword() ? std::string() : box->text();
    else if (const auto* panel = dynamic_cast<const ui::Panel*>(w))
        info.text = panel->title();
    else if (const auto* card = dynamic_cast<const scenes::CharacterSlotCard*>(w))
        info.text = card->displayText();
    else if (const auto* preview = dynamic_cast<const scenes::CharacterPreview*>(w))
        info.text = preview->look() ? preview->look()->name : std::string();
    return info;
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

    VO_LOG_INFO("{} client v{} ({})", vo::version::kProductName, vo::version::kString,
                vo::version::kBuildConfig);
    VO_LOG_INFO("config: {}", configStatus);

    int width = static_cast<int>(config.getInt("window.width", 1280));
    int height = static_cast<int>(config.getInt("window.height", 720));
    if (opts.size)
    {
        width = opts.size->first;
        height = opts.size->second;
    }
    const bool vsync = config.getBool("window.vsync", true);

    // ---- Network ----
    if (!vo::net::initCrypto())
    {
        VO_LOG_FATAL("libsodium initialisation failed");
        return kExitInit;
    }
    cnet::GameClient gameClient(makeNetSettings(config, opts));
    scenes::AppInfo appInfo;
    appInfo.settingsFile = opts.settingsPath ? *opts.settingsPath : defaultSettingsPath();
    appInfo.serverLabel = gameClient.settings().host + ":" + std::to_string(gameClient.settings().port);
    appInfo.versionLabel = std::string(vo::version::kProductName) + " v" + vo::version::kString + " (" +
                           vo::version::kBuildConfig + ") · protokol " + std::to_string(vo::net::kProtocolVersion);
    VO_LOG_INFO("server {}, user settings {}", appInfo.serverLabel, vo::core::pathToUtf8(appInfo.settingsFile));

    // ---- Automation script ----
    std::optional<automation::ScriptPlayer> script;
    if (opts.scriptPath)
    {
        script.emplace();
        if (!script->load(*opts.scriptPath, error))
        {
            VO_LOG_FATAL("{}", error);
            return kExitUsage;
        }
        if (opts.screenshotDir)
            script->setScreenshotDir(*opts.screenshotDir);
        VO_LOG_INFO("automation: script {} ({} commands)", vo::core::pathToUtf8(*opts.scriptPath),
                    script->commandCount());
    }

    // ---- Window + input ----
    platform::Input input;
    input.setRealInputEnabled(!script.has_value());
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
    window.setInput(&input);
    VO_LOG_INFO("window: client {}x{}, dpi {} (scale {:.2f}), ui scale factor {:.2f}", window.clientWidth(),
                window.clientHeight(), window.dpi(), window.dpiScale(), opts.uiScale);

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

    render::RenderStates states;
    render::SpriteBatch batch;
    render::FontLibrary fonts;
    if (!states.init(device.device(), error) ||
        !batch.init(device.device(), device.context(), states, error) || !fonts.init(device.device(), error))
    {
        VO_LOG_FATAL("2D renderer init failed: {}", error);
        fonts.shutdown();
        batch.shutdown();
        states.shutdown();
        device.shutdown();
        return kExitInit;
    }

    scenes::SceneResources sceneResources;
    if (opts.noArt)
        VO_LOG_INFO("art: --no-art, using the procedural fallbacks");
    else
        sceneResources.load(device);

    const ui::Theme theme = ui::makeDefaultTheme();
    SystemClipboard systemClipboard(window.handle());
    ui::MemoryClipboard memoryClipboard;
    ui::ClipboardProvider& clipboard = script ? static_cast<ui::ClipboardProvider&>(memoryClipboard)
                                              : static_cast<ui::ClipboardProvider&>(systemClipboard);

    float uiScaleFactor = opts.uiScale;
    scenes::SceneManager sceneManager;
    bool quitRequested = false;
    scenes::SceneContext sceneCtx{device,
                                  states,
                                  batch,
                                  fonts,
                                  theme,
                                  input,
                                  clipboard,
                                  sceneManager,
                                  gameClient,
                                  sceneResources,
                                  appInfo,
                                  [&quitRequested]() { quitRequested = true; }};
    sceneCtx.viewportWidth = static_cast<float>(device.width());
    sceneCtx.viewportHeight = static_cast<float>(device.height());
    sceneCtx.uiScale = window.dpiScale() * uiScaleFactor;
    sceneManager.requestChange(makeScene(opts.scene));

    std::optional<std::uint64_t> exitAfter = opts.exitAfterFrames;
    if (opts.screenshotPath && !exitAfter)
        exitAfter = 1;
    if (exitAfter)
        VO_LOG_INFO("automation: exit after {} frame(s){}", *exitAfter,
                    opts.screenshotPath ? ", screenshot " + vo::core::pathToUtf8(*opts.screenshotPath) : "");

    if (script)
    {
        automation::ScriptPlayer::Hooks hooks;
        auto tokens = std::make_shared<std::vector<std::string>>();
        hooks.hasState = [&sceneManager, tokens](std::string_view token)
        {
            tokens->clear();
            if (scenes::Scene* s = sceneManager.current())
                s->automationStates(*tokens);
            return std::find(tokens->begin(), tokens->end(), token) != tokens->end();
        };
        hooks.describeState = [&sceneManager, tokens]()
        {
            tokens->clear();
            if (scenes::Scene* s = sceneManager.current())
                s->automationStates(*tokens);
            std::string out;
            for (const std::string& t : *tokens)
                out += (out.empty() ? "" : "+") + t;
            return out.empty() ? std::string("no scene") : out;
        };
        hooks.findWidget = [&sceneManager](std::string_view name)
        { return describeWidget(sceneManager.current(), name); };
        script->setHooks(std::move(hooks));
    }

    // ---- Frame loop ----
    constexpr float kClearColor[4] = {0.03f, 0.04f, 0.05f, 1.0f};
    int exitCode = kExitOk;
    platform::FrameTimer timer;
    HeapStats heapStats;
    while (window.pumpMessages())
    {
        heapStats.beginFrame();
        automation::ScriptPlayer::FrameActions actions;
        if (script)
        {
            actions = script->step(input);
            if (actions.resize)
            {
                VO_LOG_INFO("script: resize to {}x{}", actions.resize->first, actions.resize->second);
                window.setClientSize(actions.resize->first, actions.resize->second);
            }
            if (actions.uiScale)
            {
                VO_LOG_INFO("script: ui scale factor {:.2f}", *actions.uiScale);
                uiScaleFactor = *actions.uiScale;
            }
            if (actions.failure)
            {
                VO_LOG_ERROR("script failed: {}", *actions.failure);
                exitCode = kExitScriptFailed;
                window.requestClose();
            }
        }
        gameClient.poll(); // network (and pings) also while minimised

        int newWidth = 0;
        int newHeight = 0;
        if (window.consumeResize(newWidth, newHeight) &&
            !device.resize(static_cast<std::uint32_t>(newWidth), static_cast<std::uint32_t>(newHeight),
                           error))
        {
            VO_LOG_ERROR("{}", error);
            exitCode = kExitRuntime;
            break;
        }
        if (window.consumeDpiChange())
            VO_LOG_INFO("dpi changed to {} (scale {:.2f})", window.dpi(), window.dpiScale());
        if (window.isMinimized())
        {
            input.endFrame();
            if (script)
                ::Sleep(16); // keep the script clock running
            else // nothing to draw: wait for a message, but wake up to keep the network serviced
                ::MsgWaitForMultipleObjects(0, nullptr, FALSE, 50, QS_ALLINPUT);
            continue;
        }

        const auto vw = static_cast<float>(device.width());
        const auto vh = static_cast<float>(device.height());
        const float uiScale = window.dpiScale() * uiScaleFactor;
        if (vw != sceneCtx.viewportWidth || vh != sceneCtx.viewportHeight || uiScale != sceneCtx.uiScale)
        {
            sceneCtx.viewportWidth = vw;
            sceneCtx.viewportHeight = vh;
            sceneCtx.uiScale = uiScale;
            VO_LOG_INFO("viewport {}x{}, ui scale {:.2f}", vw, vh, uiScale);
            sceneManager.resize(vw, vh, uiScale);
        }

        const double dt = timer.tick();
        const std::uint64_t frame = timer.frameCount();
        sceneCtx.frame = frame;
        sceneManager.applyPending(sceneCtx);
        sceneManager.update(dt);
        input.endFrame();

        device.beginFrame(kClearColor);
        sceneManager.render();
        sceneCtx.lastBatchStats = batch.takeStats();

        const bool lastFrame = exitAfter && frame >= *exitAfter;
        if (lastFrame && opts.screenshotPath && !captureScreenshot(device, *opts.screenshotPath))
            exitCode = kExitScreenshot;
        if (actions.screenshot && !captureScreenshot(device, *actions.screenshot))
            exitCode = kExitScreenshot;

        if (!device.present(error))
        {
            VO_LOG_ERROR("{}", error);
            exitCode = kExitRuntime;
            break;
        }
        if (timer.consumeFpsUpdate())
        {
            sceneCtx.fps = timer.fps();
            VO_LOG_DEBUG("fps {:.1f}, {} draw calls, {} quads", timer.fps(),
                         sceneCtx.lastBatchStats.drawCalls, sceneCtx.lastBatchStats.quads);
        }
        if (lastFrame || actions.quit || quitRequested)
        {
            if (actions.quit)
                VO_LOG_INFO("script finished");
            window.requestClose();
        }
        heapStats.endFrame();
    }

    VO_LOG_INFO("shutting down after {} frame(s)", timer.frameCount());
    heapStats.report();
    window.setInput(nullptr);
    sceneManager.shutdown();
    // Leave politely: LOGOUT lets the server save and close at once instead of timing out.
    gameClient.logout();
    gameClient.flush(cnet::Millis(500));
    sceneResources.reset();
    fonts.shutdown();
    batch.shutdown();
    states.shutdown();
    device.shutdown();
    window.destroy();
    if (device.liveObjectMessages() > 0 && exitCode == kExitOk)
        VO_LOG_WARN("live D3D objects reported at shutdown");
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
