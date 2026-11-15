#include "client/platform/Input.h"

#include "client/core/Utf8.h"

#include <windowsx.h>

namespace vo::client::platform {
namespace {

constexpr std::size_t kEventReserve = 256;

bool isTextControl(char32_t cp) noexcept
{
    return cp < 0x20 || cp == 0x7F;
}

} // namespace

Input::Input()
{
    m_events.reserve(kEventReserve);
}

std::uint8_t Input::modifiers() const noexcept
{
    std::uint8_t m = 0;
    if (m_keys[keys::kShift])
        m |= mods::kShift;
    if (m_keys[keys::kControl])
        m |= mods::kCtrl;
    if (m_keys[keys::kAlt])
        m |= mods::kAlt;
    return m;
}

void Input::push(const InputEvent& event)
{
    m_events.push_back(event);
}

void Input::apply(const InputEvent& e)
{
    switch (e.type)
    {
    case EventType::KeyDown:
        if (e.key < 256)
        {
            if (!m_keys[e.key])
                m_pressed[e.key] = true;
            m_keys[e.key] = true;
        }
        break;
    case EventType::KeyUp:
        if (e.key < 256)
        {
            if (m_keys[e.key])
                m_released[e.key] = true;
            m_keys[e.key] = false;
        }
        break;
    case EventType::MouseMove:
    case EventType::MouseWheel:
        m_mouseX = e.x;
        m_mouseY = e.y;
        break;
    case EventType::MouseDown:
        m_buttons[static_cast<std::size_t>(e.button)] = true;
        m_mouseX = e.x;
        m_mouseY = e.y;
        break;
    case EventType::MouseUp:
        m_buttons[static_cast<std::size_t>(e.button)] = false;
        m_mouseX = e.x;
        m_mouseY = e.y;
        break;
    case EventType::Char:
        break;
    case EventType::FocusLost:
        m_keys.fill(false);
        m_buttons.fill(false);
        break;
    }
}

void Input::inject(const InputEvent& event)
{
    InputEvent e = event;
    if (e.type == EventType::KeyDown || e.type == EventType::KeyUp)
    {
        // Modifier state is derived from held keys, like for real input (apply() first so a
        // modifier's own KeyDown already reports itself).
        apply(e);
        e.mods = modifiers();
        push(e);
        return;
    }
    if (e.type == EventType::MouseWheel)
    {
        e.x = m_mouseX;
        e.y = m_mouseY;
    }
    e.mods = modifiers();
    apply(e);
    push(e);
}

void Input::endFrame() noexcept
{
    m_events.clear();
    m_pressed.fill(false);
    m_released.fill(false);
}

void Input::clearState()
{
    InputEvent e;
    e.type = EventType::FocusLost;
    apply(e);
    push(e);
    m_pendingHighSurrogate = 0;
}

void Input::updateCapture(HWND hwnd)
{
    const bool anyDown = m_buttons[0] || m_buttons[1] || m_buttons[2];
    if (anyDown && !m_captured)
    {
        ::SetCapture(hwnd);
        m_captured = true;
    }
    else if (!anyDown && m_captured)
    {
        m_captured = false;
        ::ReleaseCapture();
    }
}

bool Input::handleMessage(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam, LRESULT& result)
{
    result = 0;
    auto mouseEvent = [&](EventType type, MouseButton button, std::uint8_t clicks)
    {
        InputEvent e;
        e.type = type;
        e.button = button;
        e.clickCount = clicks;
        e.x = GET_X_LPARAM(lParam);
        e.y = GET_Y_LPARAM(lParam);
        e.mods = modifiers();
        apply(e);
        push(e);
        updateCapture(hwnd);
    };

    switch (msg)
    {
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
    case WM_KEYUP:
    case WM_SYSKEYUP:
    {
        if (!m_realInputEnabled)
            return msg == WM_KEYDOWN || msg == WM_KEYUP; // let DefWindowProc see Alt+F4
        const bool down = msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN;
        InputEvent e;
        e.type = down ? EventType::KeyDown : EventType::KeyUp;
        e.key = static_cast<std::uint16_t>(wParam & 0xFF);
        e.repeat = down && (lParam & (1 << 30)) != 0;
        apply(e);
        e.mods = modifiers();
        push(e);
        // System keys (Alt combos, F10) still go to DefWindowProc so Alt+F4 keeps working.
        return msg == WM_KEYDOWN || msg == WM_KEYUP;
    }

    case WM_CHAR:
    {
        if (!m_realInputEnabled)
            return true;
        const auto unit = static_cast<char32_t>(wParam & 0xFFFF);
        char32_t cp = unit;
        if (utf8::isHighSurrogate(unit))
        {
            m_pendingHighSurrogate = unit;
            return true;
        }
        if (utf8::isLowSurrogate(unit))
        {
            cp = m_pendingHighSurrogate != 0 ? utf8::combineSurrogates(m_pendingHighSurrogate, unit)
                                             : utf8::kReplacement;
        }
        else if (m_pendingHighSurrogate != 0)
        {
            // Unpaired high surrogate followed by a normal unit: report the broken one too.
            InputEvent bad;
            bad.type = EventType::Char;
            bad.codePoint = utf8::kReplacement;
            bad.mods = modifiers();
            push(bad);
        }
        m_pendingHighSurrogate = 0;
        if (isTextControl(cp))
            return true;
        InputEvent e;
        e.type = EventType::Char;
        e.codePoint = cp;
        e.mods = modifiers();
        push(e);
        return true;
    }

    case WM_SYSCHAR:
        // Alt+key: no menu mnemonics in the game; swallowing avoids the default beep.
        return true;

    case WM_MOUSEMOVE:
    {
        if (!m_realInputEnabled)
            return true;
        if (!m_trackingLeave)
        {
            TRACKMOUSEEVENT tme{};
            tme.cbSize = sizeof(tme);
            tme.dwFlags = TME_LEAVE;
            tme.hwndTrack = hwnd;
            m_trackingLeave = ::TrackMouseEvent(&tme) != FALSE;
        }
        InputEvent e;
        e.type = EventType::MouseMove;
        e.x = GET_X_LPARAM(lParam);
        e.y = GET_Y_LPARAM(lParam);
        e.mods = modifiers();
        apply(e);
        push(e);
        return true;
    }

    case WM_MOUSELEAVE:
    {
        m_trackingLeave = false;
        if (!m_realInputEnabled || m_captured)
            return true;
        InputEvent e; // move off-screen so hover state clears
        e.type = EventType::MouseMove;
        e.x = -1;
        e.y = -1;
        apply(e);
        push(e);
        return true;
    }

    case WM_LBUTTONDOWN:
    case WM_LBUTTONDBLCLK:
    case WM_RBUTTONDOWN:
    case WM_RBUTTONDBLCLK:
    case WM_MBUTTONDOWN:
    case WM_MBUTTONDBLCLK:
    {
        if (!m_realInputEnabled)
            return true;
        const MouseButton b = (msg == WM_LBUTTONDOWN || msg == WM_LBUTTONDBLCLK)   ? MouseButton::Left
                              : (msg == WM_RBUTTONDOWN || msg == WM_RBUTTONDBLCLK) ? MouseButton::Right
                                                                                   : MouseButton::Middle;
        const bool dbl = msg == WM_LBUTTONDBLCLK || msg == WM_RBUTTONDBLCLK || msg == WM_MBUTTONDBLCLK;
        mouseEvent(EventType::MouseDown, b, static_cast<std::uint8_t>(dbl ? 2 : 1));
        return true;
    }

    case WM_LBUTTONUP:
    case WM_RBUTTONUP:
    case WM_MBUTTONUP:
    {
        if (!m_realInputEnabled)
            return true;
        const MouseButton b = msg == WM_LBUTTONUP   ? MouseButton::Left
                              : msg == WM_RBUTTONUP ? MouseButton::Right
                                                    : MouseButton::Middle;
        mouseEvent(EventType::MouseUp, b, 1);
        return true;
    }

    case WM_MOUSEWHEEL:
    {
        if (!m_realInputEnabled)
            return true;
        InputEvent e;
        e.type = EventType::MouseWheel;
        e.wheel = static_cast<float>(GET_WHEEL_DELTA_WPARAM(wParam)) / static_cast<float>(WHEEL_DELTA);
        POINT pt{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)}; // screen coordinates
        ::ScreenToClient(hwnd, &pt);
        e.x = pt.x;
        e.y = pt.y;
        e.mods = modifiers();
        apply(e);
        push(e);
        return true;
    }

    case WM_CAPTURECHANGED:
        if (m_captured && reinterpret_cast<HWND>(lParam) != hwnd)
        {
            // Someone else took the capture (e.g. a system dialog): release our buttons.
            m_captured = false;
            for (std::size_t i = 0; i < m_buttons.size(); ++i)
            {
                if (!m_buttons[i])
                    continue;
                InputEvent e;
                e.type = EventType::MouseUp;
                e.button = static_cast<MouseButton>(i);
                e.x = m_mouseX;
                e.y = m_mouseY;
                apply(e);
                push(e);
            }
        }
        return true;

    case WM_KILLFOCUS:
        if (m_realInputEnabled)
        {
            if (m_captured)
            {
                m_captured = false;
                ::ReleaseCapture();
            }
            clearState();
        }
        return false; // DefWindowProc too

    default:
        return false;
    }
}

} // namespace vo::client::platform
