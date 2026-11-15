#include "client/platform/Window.h"

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
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = proc;
    wc.hInstance = instance;
    wc.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = ::LoadIconW(nullptr, IDI_APPLICATION);
    wc.hbrBackground = nullptr; // the renderer paints every pixel
    wc.lpszClassName = kClassName;
    return ::RegisterClassExW(&wc) != 0;
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

    const DWORD style = desc.resizable ? WS_OVERLAPPEDWINDOW : (WS_OVERLAPPEDWINDOW & ~(WS_THICKFRAME | WS_MAXIMIZEBOX));
    const DWORD exStyle = WS_EX_APPWINDOW;
    RECT rect{0, 0, desc.clientWidth, desc.clientHeight};
    ::AdjustWindowRectEx(&rect, style, FALSE, exStyle);

    m_width = desc.clientWidth;
    m_height = desc.clientHeight;
    m_hwnd = ::CreateWindowExW(exStyle, kClassName, desc.title.c_str(), style, CW_USEDEFAULT, CW_USEDEFAULT,
                               rect.right - rect.left, rect.bottom - rect.top, nullptr, nullptr, instance, this);
    if (m_hwnd == nullptr)
    {
        error = "CreateWindowExW failed: " + lastErrorToString();
        return false;
    }

    ::ShowWindow(m_hwnd, SW_SHOW);
    ::UpdateWindow(m_hwnd);

    RECT client{};
    ::GetClientRect(m_hwnd, &client);
    m_width = client.right - client.left;
    m_height = client.bottom - client.top;
    m_resized = false;
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

bool Window::consumeResize(int& width, int& height) noexcept
{
    if (!m_resized)
        return false;
    m_resized = false;
    width = m_width;
    height = m_height;
    return true;
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
    switch (msg)
    {
    case WM_CLOSE:
        // Don't destroy here: the frame loop must release the swap chain before the HWND goes away.
        m_closeRequested = true;
        return 0;

    case WM_KEYDOWN:
        if (wParam == VK_ESCAPE)
        {
            m_closeRequested = true;
            return 0;
        }
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

    case WM_GETMINMAXINFO: {
        auto* info = reinterpret_cast<MINMAXINFO*>(lParam);
        RECT rect{0, 0, kMinClientWidth, kMinClientHeight};
        ::AdjustWindowRectEx(&rect, static_cast<DWORD>(::GetWindowLongPtrW(m_hwnd, GWL_STYLE)), FALSE,
                             static_cast<DWORD>(::GetWindowLongPtrW(m_hwnd, GWL_EXSTYLE)));
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
