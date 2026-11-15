#include "client/platform/Window.h"

#include "client/platform/Input.h"
#include "client/platform/Win32Util.h"

namespace vo::client::platform {
namespace {

constexpr wchar_t kClassName[] = L"VedatOnlineWindowClass";
constexpr int kMinClientWidth = 320;
constexpr int kMinClientHeight = 240;

bool registerClassOnce(HINSTANCE instance, WNDPROC proc)
{
    WNDCLASSEXW existing{};
    existing.cbSize = sizeof(existing);
    if (::GetClassInfoExW(instance, kClassName, &existing))
        return true;

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
    wc.lpfnWndProc = proc;
    wc.hInstance = instance;
    wc.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = ::LoadIconW(nullptr, IDI_APPLICATION);
    wc.hbrBackground = nullptr; // the renderer paints every pixel
    wc.lpszClassName = kClassName;
    return ::RegisterClassExW(&wc) != 0;
}

RECT windowRectForClient(HWND hwnd, int width, int height, UINT dpi)
{
    RECT rect{0, 0, width, height};
    const auto style = static_cast<DWORD>(::GetWindowLongPtrW(hwnd, GWL_STYLE));
    const auto exStyle = static_cast<DWORD>(::GetWindowLongPtrW(hwnd, GWL_EXSTYLE));
    ::AdjustWindowRectExForDpi(&rect, style, FALSE, exStyle, dpi);
    return rect;
}

} // namespace

Window::~Window()
{
    destroy();
}

bool Window::create(const Desc& desc, std::string& error)
{
    HINSTANCE instance = ::GetModuleHandleW(nullptr);
    if (!registerClassOnce(instance, &Window::wndProcThunk))
    {
        error = "RegisterClassExW failed: " + lastErrorToString();
        return false;
    }

    const DWORD style =
        desc.resizable ? WS_OVERLAPPEDWINDOW : (WS_OVERLAPPEDWINDOW & ~(WS_THICKFRAME | WS_MAXIMIZEBOX));
    const DWORD exStyle = WS_EX_APPWINDOW;
    RECT rect{0, 0, desc.clientWidth, desc.clientHeight};
    ::AdjustWindowRectEx(&rect, style, FALSE, exStyle);

    m_width = desc.clientWidth;
    m_height = desc.clientHeight;
    m_hwnd =
        ::CreateWindowExW(exStyle, kClassName, desc.title.c_str(), style, CW_USEDEFAULT, CW_USEDEFAULT,
                          rect.right - rect.left, rect.bottom - rect.top, nullptr, nullptr, instance, this);
    if (m_hwnd == nullptr)
    {
        error = "CreateWindowExW failed: " + lastErrorToString();
        return false;
    }

    // The window may have been placed on a monitor with a different DPI than the one
    // AdjustWindowRectEx assumed: size the frame for the real DPI so the client area is exact.
    m_dpi = ::GetDpiForWindow(m_hwnd);
    if (m_dpi == 0)
        m_dpi = 96;
    const RECT exact = windowRectForClient(m_hwnd, desc.clientWidth, desc.clientHeight, m_dpi);
    ::SetWindowPos(m_hwnd, nullptr, 0, 0, exact.right - exact.left, exact.bottom - exact.top,
                   SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);

    ::ShowWindow(m_hwnd, SW_SHOW);
    ::UpdateWindow(m_hwnd);

    RECT client{};
    ::GetClientRect(m_hwnd, &client);
    m_width = client.right - client.left;
    m_height = client.bottom - client.top;
    m_resized = false;
    m_dpiChanged = false;
    return true;
}

void Window::destroy() noexcept
{
    if (m_hwnd != nullptr)
    {
        ::SetWindowLongPtrW(m_hwnd, GWLP_USERDATA, 0);
        ::DestroyWindow(m_hwnd);
        m_hwnd = nullptr;
    }
    m_input = nullptr;
}

bool Window::pumpMessages()
{
    MSG msg{};
    while (::PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
    {
        if (msg.message == WM_QUIT)
        {
            m_closeRequested = true;
            break;
        }
        ::TranslateMessage(&msg);
        ::DispatchMessageW(&msg);
    }
    return !m_closeRequested;
}

void Window::setClientSize(int width, int height)
{
    if (m_hwnd == nullptr || width <= 0 || height <= 0)
        return;
    if (::IsIconic(m_hwnd) || ::IsZoomed(m_hwnd))
        ::ShowWindow(m_hwnd, SW_RESTORE);
    const RECT rect = windowRectForClient(m_hwnd, width, height, m_dpi);
    ::SetWindowPos(m_hwnd, nullptr, 0, 0, rect.right - rect.left, rect.bottom - rect.top,
                   SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
}

bool Window::consumeResize(int& width, int& height) noexcept
{
    if (!m_resized)
        return false;
    m_resized = false;
    width = m_width;
    height = m_height;
    return true;
}

bool Window::consumeDpiChange() noexcept
{
    const bool changed = m_dpiChanged;
    m_dpiChanged = false;
    return changed;
}

LRESULT CALLBACK Window::wndProcThunk(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (msg == WM_NCCREATE)
    {
        const auto* cs = reinterpret_cast<const CREATESTRUCTW*>(lParam);
        auto* self = static_cast<Window*>(cs->lpCreateParams);
        ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        self->m_hwnd = hwnd;
    }
    auto* self = reinterpret_cast<Window*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (self != nullptr)
        return self->wndProc(msg, wParam, lParam);
    return ::DefWindowProcW(hwnd, msg, wParam, lParam);
}

LRESULT Window::wndProc(UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (m_input != nullptr)
    {
        LRESULT result = 0;
        if (m_input->handleMessage(m_hwnd, msg, wParam, lParam, result))
            return result;
    }

    switch (msg)
    {
    case WM_CLOSE:
        // Don't destroy here: the frame loop must release the swap chain before the HWND goes away.
        m_closeRequested = true;
        return 0;

    case WM_SYSCOMMAND:
        // Keep a bare Alt press from entering the (non-existent) menu loop and freezing the game.
        if ((wParam & 0xFFF0) == SC_KEYMENU)
            return 0;
        break;

    case WM_SIZE:
        if (wParam == SIZE_MINIMIZED)
        {
            m_minimized = true;
        }
        else
        {
            m_minimized = false;
            const int w = LOWORD(lParam);
            const int h = HIWORD(lParam);
            if (w > 0 && h > 0 && (w != m_width || h != m_height))
            {
                m_width = w;
                m_height = h;
                m_resized = true;
            }
        }
        return 0;

    case WM_DPICHANGED:
    {
        const UINT dpi = HIWORD(wParam);
        if (dpi != 0 && dpi != m_dpi)
        {
            m_dpi = dpi;
            m_dpiChanged = true;
        }
        const auto* suggested = reinterpret_cast<const RECT*>(lParam);
        ::SetWindowPos(m_hwnd, nullptr, suggested->left, suggested->top, suggested->right - suggested->left,
                       suggested->bottom - suggested->top, SWP_NOZORDER | SWP_NOACTIVATE);
        return 0;
    }

    case WM_GETMINMAXINFO:
    {
        auto* info = reinterpret_cast<MINMAXINFO*>(lParam);
        const RECT rect = windowRectForClient(m_hwnd, kMinClientWidth, kMinClientHeight, m_dpi);
        info->ptMinTrackSize.x = rect.right - rect.left;
        info->ptMinTrackSize.y = rect.bottom - rect.top;
        return 0;
    }

    case WM_ERASEBKGND:
        return 1; // avoid flicker; the renderer clears the client area

    case WM_DESTROY:
        return 0;

    default:
        break;
    }
    return ::DefWindowProcW(m_hwnd, msg, wParam, lParam);
}

} // namespace vo::client::platform
