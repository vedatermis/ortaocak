// Win32 top-level window. Owns the HWND; message pumping is driven by the caller's frame loop.
// The process is per-monitor-DPI-aware v2: the window follows WM_DPICHANGED and reports its DPI
// so the UI can scale. Keyboard/mouse messages are forwarded to an attached platform::Input.
#pragma once

#include <Windows.h>

#include <string>

namespace vo::client::platform {

class Input;

class Window
{
public:
    struct Desc
    {
        std::wstring title = L"VedatOnline";
        int clientWidth = 1280; // physical pixels
        int clientHeight = 720;
        bool resizable = true;
    };

    Window() = default;
    ~Window();
    Window(const Window&) = delete;
    Window& operator=(const Window&) = delete;

    bool create(const Desc& desc, std::string& error);
    void destroy() noexcept;

    // Dispatches all pending messages. Returns false once a close was requested (WM_CLOSE,
    // requestClose()) or WM_QUIT was received; the caller then shuts down and calls destroy().
    bool pumpMessages();

    void requestClose() noexcept { m_closeRequested = true; }
    [[nodiscard]] bool closeRequested() const noexcept { return m_closeRequested; }

    // Messages are forwarded to `input` (may be nullptr). The Input must outlive the window or be
    // detached first.
    void setInput(Input* input) noexcept { m_input = input; }

    [[nodiscard]] HWND handle() const noexcept { return m_hwnd; }
    [[nodiscard]] int clientWidth() const noexcept { return m_width; }
    [[nodiscard]] int clientHeight() const noexcept { return m_height; }
    [[nodiscard]] bool isMinimized() const noexcept { return m_minimized; }
    [[nodiscard]] unsigned dpi() const noexcept { return m_dpi; }
    [[nodiscard]] float dpiScale() const noexcept { return static_cast<float>(m_dpi) / 96.0f; }

    // Resizes the window so the client area is width x height physical pixels.
    void setClientSize(int width, int height);

    // Returns true (once) if the client area size changed since the last call.
    bool consumeResize(int& width, int& height) noexcept;
    // Returns true (once) if the DPI changed since the last call.
    bool consumeDpiChange() noexcept;

private:
    static LRESULT CALLBACK wndProcThunk(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    LRESULT wndProc(UINT msg, WPARAM wParam, LPARAM lParam);

    HWND m_hwnd = nullptr;
    Input* m_input = nullptr;
    int m_width = 0;
    int m_height = 0;
    unsigned m_dpi = 96;
    bool m_resized = false;
    bool m_dpiChanged = false;
    bool m_minimized = false;
    bool m_closeRequested = false;
};

} // namespace vo::client::platform
