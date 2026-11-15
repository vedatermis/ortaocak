// Win32 top-level window. Owns the HWND; message pumping is driven by the caller's frame loop.
#pragma once

#include <Windows.h>

#include <string>

namespace vo::client::platform {

class Window
{
public:
    struct Desc
    {
        std::wstring title = L"VedatOnline";
        int clientWidth = 1280;
        int clientHeight = 720;
        bool resizable = true;
    };

    Window() = default;
    ~Window();
    Window(const Window&) = delete;
    Window& operator=(const Window&) = delete;

    bool create(const Desc& desc, std::string& error);
    void destroy() noexcept;

    // Dispatches all pending messages. Returns false once a close was requested (WM_CLOSE, Esc,
    // requestClose()) or WM_QUIT was received; the caller then shuts down and calls destroy().
    bool pumpMessages();

    void requestClose() noexcept { m_closeRequested = true; }
    [[nodiscard]] bool closeRequested() const noexcept { return m_closeRequested; }

    [[nodiscard]] HWND handle() const noexcept { return m_hwnd; }
    [[nodiscard]] int clientWidth() const noexcept { return m_width; }
    [[nodiscard]] int clientHeight() const noexcept { return m_height; }
    [[nodiscard]] bool isMinimized() const noexcept { return m_minimized; }

    // Returns true (once) if the client area size changed since the last call.
    bool consumeResize(int& width, int& height) noexcept;

private:
    static LRESULT CALLBACK wndProcThunk(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    LRESULT wndProc(UINT msg, WPARAM wParam, LPARAM lParam);

    HWND m_hwnd = nullptr;
    int m_width = 0;
    int m_height = 0;
    bool m_resized = false;
    bool m_minimized = false;
    bool m_closeRequested = false;
};

} // namespace vo::client::platform
