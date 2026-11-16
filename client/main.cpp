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
// World (M6): --debug-ghosts <n> synthetic remote players (+ F9 / `scene correct` POSITION_CORRECT),
//   --fixed-dt <s> fixed simulation step per frame (deterministic automation), --vsync on|off,
//   --no-shadows (sun shadow map off; blob shadows stay)
// M7: --msaa 1|2|4|8 overrides graphics.msaa (world pass multisampling, default 4).
// PBR: --graphics key=value overrides one [graphics] key (normal_maps, specular, rim_light,
//   shadow_cascades, shadow_filter, exposure, tonemap; repeatable); scripts: `scene graphics <key>
//   <value>` and `scene graphics log`. --debug-model <asset.vmesh> (repeatable) starts a server-less
//   model viewer (client/render/DebugModelViewer.h) with --debug-camera, --debug-light, --debug-yaw,
//   --debug-count, --debug-orbit, --debug-at x,z.
// Device loss (DXGI_ERROR_DEVICE_REMOVED/RESET at Present): the scene is closed, the session logged
// out, the device and every GPU resource (states, sprite batch, fonts, asset cache, world renderer,
// UI art) are destroyed and created again, and the login screen opens with "Ekran aygıtı
// sıfırlandı". Automation: `scene devicelost` simulates a loss at the next Present; `scene window
// minimize|restore|deactivate|activate` sends the real window messages (handled here, not by the
// scene).
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
#include "client/render/AssetCache.h"
#include "client/render/DebugModelViewer.h"
#include "client/render/Font.h"
#include "client/render/RenderDevice.h"
#include "client/render/RenderStates.h"
#include "client/render/Screenshot.h"
#include "client/render/SpriteBatch.h"
#include "client/render/WorldRenderer.h"
#include "client/scenes/SceneWidgets.h"
#include "client/scenes/Scenes.h"
#include "client/ui/ClipboardProvider.h"
#include "client/ui/Theme.h"
#include "client/ui/Ui.h"
#include "client/world/ItemData.h"
#include "shared/core/Config.h"
#include "shared/core/Log.h"
#include "shared/core/Paths.h"
#include "shared/core/Version.h"
#include "shared/net/Crypto.h"

#include <Windows.h>
#include <KnownFolders.h>
#include <ShlObj.h>
#include <objbase.h>

#include <crtdbg.h> // CRT report modes (also used without _DEBUG: the calls compile to no-ops there)
#include <cstdlib>

#include <array>
#include <atomic>
#include <algorithm>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <format>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace {

namespace platform = vo::client::platform;
namespace render = vo::client::render;
namespace scenes = vo::client::scenes;
namespace ui = vo::client::ui;
namespace automation = vo::client::automation;
namespace cnet = vo::client::net;
namespace cworld = vo::client::world;

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
    int debugGhosts = 0;
    std::optional<double> fixedDt;
    std::optional<bool> vsync;
    std::optional<int> msaa;
    bool noShadows = false;
    std::vector<std::pair<std::string, std::string>> graphics; // --graphics key=value
    render::DebugViewerOptions viewer;                         // --debug-model ...
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
    "                 [--no-art] [--protocol-version <n>] [--warp] [--no-debug-layer] [--console] [--help]\n"
    "                 [--debug-ghosts <n>] [--fixed-dt <seconds>] [--vsync on|off] [--no-shadows]\n"
    "                 [--msaa 1|2|4|8] [--graphics key=value]...\n"
    "                 [--debug-model <asset.vmesh>]... [--debug-camera front|threeq|side|back|face|armor|wide|y,p,d,h]\n"
    "                 [--debug-light world|preview-aydor|preview-kargan] [--debug-yaw <deg>] [--debug-facing <deg>] [--debug-count <n>]\n"
    "                 [--debug-orbit <deg/s>] [--debug-at <x>,<z>]\n";

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
        else if (arg == "--no-shadows")
            out.noShadows = true;
        else if (arg == "--debug-ghosts" && hasValue)
        {
            const std::string& v = args[++i];
            const auto [ptr, ec] = std::from_chars(v.data(), v.data() + v.size(), out.debugGhosts);
            if (ec != std::errc{} || ptr != v.data() + v.size() || out.debugGhosts < 0 || out.debugGhosts > 64)
            {
                error = "invalid --debug-ghosts '" + v + "' (0 .. 64)";
                return false;
            }
        }
        else if (arg == "--fixed-dt" && hasValue)
        {
            const std::string& v = args[++i];
            double dt = 0.0;
            const auto [ptr, ec] = std::from_chars(v.data(), v.data() + v.size(), dt);
            if (ec != std::errc{} || ptr != v.data() + v.size() || dt < 0.001 || dt > 0.25)
            {
                error = "invalid --fixed-dt '" + v + "' (0.001 .. 0.25 s)";
                return false;
            }
            out.fixedDt = dt;
        }
        else if (arg == "--vsync" && hasValue)
        {
            const std::string& v = args[++i];
            if (v != "on" && v != "off")
            {
                error = "invalid --vsync '" + v + "' (on, off)";
                return false;
            }
            out.vsync = v == "on";
        }
        else if (arg == "--msaa" && hasValue)
        {
            const std::string& v = args[++i];
            int n = 0;
            const auto [ptr, ec] = std::from_chars(v.data(), v.data() + v.size(), n);
            if (ec != std::errc{} || ptr != v.data() + v.size() || (n != 1 && n != 2 && n != 4 && n != 8))
            {
                error = "invalid --msaa '" + v + "' (1, 2, 4, 8)";
                return false;
            }
            out.msaa = n;
        }
        else if (arg == "--graphics" && hasValue)
        {
            const std::string& v = args[++i];
            const std::size_t eq = v.find('=');
            if (eq == std::string::npos || eq == 0)
            {
                error = "invalid --graphics '" + v + "' (key=value)";
                return false;
            }
            out.graphics.emplace_back(v.substr(0, eq), v.substr(eq + 1));
        }
        else if (arg == "--debug-model" && hasValue)
            out.viewer.models.push_back(args[++i]);
        else if (arg == "--debug-camera" && hasValue)
            out.viewer.camera = args[++i];
        else if (arg == "--debug-light" && hasValue)
            out.viewer.light = args[++i];
        else if ((arg == "--debug-yaw" || arg == "--debug-facing" || arg == "--debug-orbit" || arg == "--debug-count") && hasValue)
        {
            const std::string& v = args[++i];
            float f = 0.0f;
            const auto [ptr, ec] = std::from_chars(v.data(), v.data() + v.size(), f);
            if (ec != std::errc{} || ptr != v.data() + v.size() || (arg == "--debug-count" && (f < 1.0f || f > 256.0f)))
            {
                error = "invalid " + arg + " '" + v + "'";
                return false;
            }
            if (arg == "--debug-yaw")
                out.viewer.modelYawDeg = f;
            else if (arg == "--debug-facing")
                out.viewer.facingDeg = f;
            else if (arg == "--debug-orbit")
                out.viewer.orbitDegPerSecond = f;
            else
                out.viewer.count = static_cast<std::uint32_t>(f);
        }
        else if (arg == "--debug-at" && hasValue)
        {
            const std::string& v = args[++i];
            const std::size_t comma = v.find(',');
            float x = 0.0f, z = 0.0f;
            if (comma == std::string::npos ||
                std::from_chars(v.data(), v.data() + comma, x).ec != std::errc{} ||
                std::from_chars(v.data() + comma + 1, v.data() + v.size(), z).ec != std::errc{})
            {
                error = "invalid --debug-at '" + v + "' (x,z)";
                return false;
            }
            out.viewer.at = std::make_pair(x, z);
        }
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

// Camera mouse-look: hides the cursor and confines it to the client area while the right button
// drags the camera; warp() keeps the hidden cursor at the drag anchor so it never hits the edge.
class MouseLook
{
public:
    explicit MouseLook(HWND hwnd) : m_hwnd(hwnd) {}
    ~MouseLook() { setCaptured(false); }
    MouseLook(const MouseLook&) = delete;
    MouseLook& operator=(const MouseLook&) = delete;

    void setCaptured(bool captured)
    {
        if (captured == m_captured)
            return;
        m_captured = captured;
        if (captured)
        {
            ::ShowCursor(FALSE);
            RECT r{};
            ::GetClientRect(m_hwnd, &r);
            ::MapWindowPoints(m_hwnd, nullptr, reinterpret_cast<POINT*>(&r), 2);
            ::ClipCursor(&r);
        }
        else
        {
            ::ClipCursor(nullptr);
            ::ShowCursor(TRUE);
        }
    }
    void warp(int x, int y)
    {
        POINT p{x, y};
        ::ClientToScreen(m_hwnd, &p);
        ::SetCursorPos(p.x, p.y);
    }

private:
    HWND m_hwnd;
    bool m_captured = false;
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
    const bool vsync = opts.vsync.value_or(config.getBool("window.vsync", true));

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
        // Unattended runs (run-ui-flows.ps1): a crash, abort() or CRT assert must end the process with
        // a message on stderr / in the log, never wait for a click on a modal error dialog.
        SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
        _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
        const int reportTypes[] = {_CRT_WARN, _CRT_ERROR, _CRT_ASSERT};
        for (const int type : reportTypes)
        {
            _CrtSetReportMode(type, _CRTDBG_MODE_FILE);
            _CrtSetReportFile(type, _CRTDBG_FILE_STDERR);
        }
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
    const auto msaaSamples = static_cast<std::uint32_t>(
        std::clamp<std::int64_t>(opts.msaa.value_or(static_cast<int>(config.getInt("graphics.msaa", 4))), 1, 8));

    render::RenderStates states;
    render::SpriteBatch batch;
    render::FontLibrary fonts;
    render::AssetCache assetCache;
    render::WorldRenderer worldRenderer;
    scenes::SceneResources sceneResources;
    // Item templates and weapon attachments (M8): plain data, independent of the GPU.
    cworld::ItemData itemData;
    itemData.load();
    sceneResources.setItemData(&itemData);
    render::DebugModelViewer viewer;
    const bool viewerMode = !opts.viewer.models.empty();
    bool have3d = false;

    // [graphics] settings: config, then --graphics overrides (docs/CLIENT_RENDERING.md §7).
    render::GraphicsSettings graphics;
    bool toneMapClamp = false;
    {
        const auto fromConfig = [&](const char* key, std::string value) {
            std::string err;
            if (!render::applyGraphicsSetting(key, value, graphics, toneMapClamp, err))
                VO_LOG_ERROR("config: {}", err);
        };
        const auto boolKey = [&](const char* key) {
            const std::string full = std::string("graphics.") + key;
            if (config.has(full))
                fromConfig(key, config.getBool(full, true) ? "true" : "false");
        };
        boolKey("normal_maps");
        boolKey("specular");
        boolKey("rim_light");
        if (config.has("graphics.shadow_cascades"))
            fromConfig("shadow_cascades", std::to_string(config.getInt("graphics.shadow_cascades", 2)));
        if (config.has("graphics.shadow_filter"))
            fromConfig("shadow_filter", config.getString("graphics.shadow_filter", "pcf"));
        if (config.has("graphics.exposure"))
            fromConfig("exposure", std::format("{}", config.getDouble("graphics.exposure", 1.0)));
        if (config.has("graphics.tonemap"))
            fromConfig("tonemap", config.getString("graphics.tonemap", "aces"));
        for (const auto& [key, value] : opts.graphics)
        {
            std::string err;
            if (!render::applyGraphicsSetting(key, value, graphics, toneMapClamp, err))
            {
                VO_LOG_FATAL("--graphics: {}", err);
                return kExitUsage;
            }
        }
    }
    const bool compressedTextures = config.getBool("graphics.compressed_textures", true);

    // Everything that lives on the GPU, created and destroyed together (startup, device loss).
    const auto releaseGpu = [&]() {
        viewer.shutdown();
        sceneResources.reset();
        worldRenderer.shutdown();
        assetCache.shutdown();
        fonts.shutdown();
        batch.shutdown();
        states.shutdown();
        device.shutdown();
    };
    const auto initGpu = [&](std::string& err) -> bool {
        if (!device.init(deviceDesc, err))
        {
            err = "renderer init failed: " + err;
            return false;
        }
        if (!states.init(device.device(), err) || !batch.init(device.device(), device.context(), states, err) ||
            !fonts.init(device.device(), err))
        {
            err = "2D renderer init failed: " + err;
            return false;
        }
        device.post().toneMap = toneMapClamp ? render::ToneMap::Clamp : render::ToneMap::AcesFitted;
        std::string msaaError;
        if (!device.setMsaa(msaaSamples, msaaError))
            VO_LOG_ERROR("{} (world pass without MSAA)", msaaError);
        // 3D world renderer + asset cache (M6). Failure is not fatal: the 2D screens still work, the
        // world scene reports missing assets and the preview falls back to the 2D art.
        std::string error3d;
        assetCache.setPreferCompiledTextures(compressedTextures);
        have3d = assetCache.init(device.device(), device.context(), error3d);
        if (!have3d)
            VO_LOG_ERROR("asset cache: {}", error3d);
        worldRenderer.settings = graphics;
        if (have3d && !worldRenderer.init(device.device(), device.context(), states, error3d))
        {
            VO_LOG_ERROR("3D renderer init failed: {}", error3d);
            have3d = false;
        }
        if (!compressedTextures)
            VO_LOG_INFO("graphics: compressed_textures = false: .vtex files are ignored, PNGs are decoded");
        sceneResources.setPreviewServices(have3d ? scenes::PreviewServices{&device, &worldRenderer,
                                                                                           &assetCache}
                                                 : scenes::PreviewServices{});
        if (opts.noArt)
            VO_LOG_INFO("art: --no-art, using the procedural fallbacks");
        else
            sceneResources.load(device);
        return true;
    };
    if (!initGpu(error))
    {
        VO_LOG_FATAL("{}", error);
        releaseGpu();
        return kExitInit;
    }

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
    sceneCtx.worldRenderer = have3d ? &worldRenderer : nullptr;
    sceneCtx.assets = have3d ? &assetCache : nullptr;
    sceneCtx.debugGhosts = opts.debugGhosts;
    sceneCtx.shadows = !opts.noShadows && config.getBool("graphics.shadows", true);
    sceneCtx.scripted = script.has_value();
    MouseLook mouseLook(window.handle());
    sceneCtx.setMouseCaptured = [&mouseLook](bool captured) { mouseLook.setCaptured(captured); };
    sceneCtx.warpMouse = [&mouseLook](int x, int y) { mouseLook.warp(x, y); };
    if (opts.debugGhosts > 0)
        VO_LOG_INFO("debug: {} synthetic remote player(s) will be injected in the world", opts.debugGhosts);
    if (opts.fixedDt)
        VO_LOG_INFO("automation: fixed simulation step {:.4f} s per frame", *opts.fixedDt);
    if (viewerMode)
    {
        std::string viewerError;
        render::DebugViewerOptions viewerOptions = opts.viewer;
        viewerOptions.shadows = sceneCtx.shadows;
        if (!have3d || !viewer.init(device, assetCache, worldRenderer, viewerOptions, viewerError))
        {
            VO_LOG_FATAL("model viewer: {}", have3d ? viewerError : std::string("no 3D renderer / assets"));
            releaseGpu();
            return kExitInit;
        }
    }
    else
    {
        sceneManager.requestChange(makeScene(opts.scene));
    }

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
        hooks.sceneCommand = [&sceneManager, &device, &window, &worldRenderer](std::string_view command,
                                                                                std::string& err)
        {
            // Application-level commands (any scene).
            if (command.starts_with("graphics"))
            {
                // `graphics log` | `graphics <key> <value>` (docs/CLIENT_RENDERING.md §7).
                const std::string_view rest = command.size() > 9 ? command.substr(9) : std::string_view{};
                if (rest != "log")
                {
                    const std::size_t sp = rest.find(' ');
                    if (sp == std::string_view::npos)
                    {
                        err = "usage: graphics log | graphics <key> <value>";
                        return false;
                    }
                    bool clamp = device.post().toneMap == render::ToneMap::Clamp;
                    if (!render::applyGraphicsSetting(rest.substr(0, sp), rest.substr(sp + 1), worldRenderer.settings,
                                                      clamp, err))
                        return false;
                    device.post().toneMap = clamp ? render::ToneMap::Clamp : render::ToneMap::AcesFitted;
                }
                VO_LOG_INFO("automation: {}, msaa {}x, tonemap {}", worldRenderer.describeSettings(),
                            device.msaaSamples(), device.post().toneMap == render::ToneMap::Clamp ? "clamp" : "aces");
                return true;
            }
            if (command == "devicelost")
            {
                VO_LOG_INFO("automation: simulating a device loss at the next Present");
                device.simulateDeviceLoss();
                return true;
            }
            if (command.starts_with("window "))
            {
                // Real window messages, as the shell would send them.
                const HWND hwnd = window.handle();
                const std::string_view what = command.substr(7);
                if (what == "minimize")
                    ::PostMessageW(hwnd, WM_SYSCOMMAND, SC_MINIMIZE, 0);
                else if (what == "restore")
                    ::PostMessageW(hwnd, WM_SYSCOMMAND, SC_RESTORE, 0);
                else if (what == "deactivate") // what Alt+Tab to another window delivers
                {
                    ::SendMessageW(hwnd, WM_ACTIVATEAPP, FALSE, 0);
                    ::SendMessageW(hwnd, WM_ACTIVATE, WA_INACTIVE, 0);
                    ::SendMessageW(hwnd, WM_KILLFOCUS, 0, 0);
                }
                else if (what == "activate")
                {
                    ::SendMessageW(hwnd, WM_ACTIVATEAPP, TRUE, 0);
                    ::SendMessageW(hwnd, WM_ACTIVATE, WA_ACTIVE, 0);
                    ::SendMessageW(hwnd, WM_SETFOCUS, 0, 0);
                }
                else
                {
                    err = "usage: window minimize|restore|deactivate|activate";
                    return false;
                }
                VO_LOG_INFO("automation: window {}", what);
                return true;
            }
            scenes::Scene* s = sceneManager.current();
            if (s == nullptr)
            {
                err = "no active scene";
                return false;
            }
            return s->automationCommand(command, err);
        };
        script->setHooks(std::move(hooks));
    }

    // ---- Frame loop ----
    constexpr float kClearColor[4] = {0.03f, 0.04f, 0.05f, 1.0f};
    int exitCode = kExitOk;
    int deviceResets = 0;
    bool wasMinimized = false;
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
        if (window.isMinimized() != wasMinimized)
        {
            wasMinimized = window.isMinimized();
            VO_LOG_INFO("window {}", wasMinimized ? "minimised: rendering paused, network kept alive" : "restored");
        }
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

        const double realDt = timer.tick();
        const double dt = opts.fixedDt ? *opts.fixedDt : realDt;
        const std::uint64_t frame = timer.frameCount();
        sceneCtx.frame = frame;
        if (viewerMode)
        {
            viewer.update(dt, realDt);
        }
        else
        {
            sceneManager.applyPending(sceneCtx);
            sceneManager.update(dt);
        }
        input.endFrame();

        device.beginFrame(kClearColor);
        if (viewerMode)
            viewer.render();
        else
            sceneManager.render();
        sceneCtx.lastBatchStats = batch.takeStats();

        const bool lastFrame = exitAfter && frame >= *exitAfter;
        if (lastFrame && opts.screenshotPath && !captureScreenshot(device, *opts.screenshotPath))
            exitCode = kExitScreenshot;
        if (actions.screenshot && !captureScreenshot(device, *actions.screenshot))
            exitCode = kExitScreenshot;

        const auto presented = device.present(error);
        if (presented == render::RenderDevice::PresentResult::DeviceLost)
        {
            // Recreate the device and every GPU resource; the session cannot survive (the scenes and
            // the world state hang off GPU objects), so log out and start over at the login screen.
            ++deviceResets;
            if (viewerMode)
            {
                VO_LOG_ERROR("{}: the model viewer does not recover from a device loss", error);
                exitCode = kExitRuntime;
                break;
            }
            VO_LOG_WARN("{}: recreating the D3D11 device and all GPU resources (reset {})", error, deviceResets);
            mouseLook.setCaptured(false);
            sceneManager.shutdown();
            gameClient.logout();
            releaseGpu();
            deviceDesc.width = static_cast<std::uint32_t>(window.clientWidth());
            deviceDesc.height = static_cast<std::uint32_t>(window.clientHeight());
            if (!initGpu(error))
            {
                VO_LOG_FATAL("cannot recover from the device loss: {}", error);
                exitCode = kExitRuntime;
                break;
            }
            sceneCtx.worldRenderer = have3d ? &worldRenderer : nullptr;
            sceneCtx.assets = have3d ? &assetCache : nullptr;
            sceneCtx.viewportWidth = static_cast<float>(device.width());
            sceneCtx.viewportHeight = static_cast<float>(device.height());
            if (opts.scene == "uidemo")
                sceneManager.requestChange(makeScene(opts.scene));
            else
                sceneManager.requestChange(std::make_unique<scenes::LoginScene>(cnet::UserMessage{
                    "Ekran", "Ekran aygıtı sıfırlandı. Oturumunuz kapatıldı; lütfen tekrar giriş yapın."}));
            VO_LOG_INFO("device recovered: adapter '{}', {}x{}", device.adapterName(), device.width(), device.height());
        }
        else if (presented == render::RenderDevice::PresentResult::Failed)
        {
            VO_LOG_ERROR("{}", error);
            exitCode = kExitRuntime;
            break;
        }
        if (timer.consumeFpsUpdate())
        {
            sceneCtx.fps = timer.fps();
            const render::WorldRenderStats& ws = worldRenderer.stats();
            VO_LOG_DEBUG("fps {:.1f}, 2D {} draw calls / {} quads, 3D {} draw calls / {} triangles (+ shadow {} / {})",
                         timer.fps(), sceneCtx.lastBatchStats.drawCalls, sceneCtx.lastBatchStats.quads, ws.drawCalls,
                         ws.triangles, ws.shadowDrawCalls, ws.shadowTriangles);
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
    mouseLook.setCaptured(false);
    // Leave politely: LOGOUT lets the server save and close at once instead of timing out.
    gameClient.logout();
    gameClient.flush(cnet::Millis(500));
    releaseGpu();
    window.destroy();
    if (deviceResets > 0)
        VO_LOG_INFO("device resets during the run: {}", deviceResets);
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
